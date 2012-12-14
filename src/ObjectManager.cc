/* Copyright (c) 2012 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "Buffer.h"
#include "Cycles.h"
#include "Dispatch.h"
#include "Enumeration.h"
#include "EnumerationIterator.h"
#include "LogEntryRelocator.h"
#include "ObjectManager.h"
#include "ShortMacros.h"
#include "RawMetrics.h"
#include "Tub.h"
#include "ProtoBuf.h"
#include "Segment.h"
#include "Transport.h"
#include "WallTime.h"

namespace RAMCloud {

/**
 * Construct an ObjectManager.
 *
 * \param context
 *      Overall information about the RAMCloud server or client.
 * \param serverId
 *      ServerId of the master server that is instantiating this object manager.
 * \param config
 *      Contains various parameters that configure the operation of this server.
 * \param tabletManager
 *      Pointer to the master's TabletManager instance. This defines which
 *      tablets are owned by the master and affects which objects can we read
 *      from this ObjectManager. For example, if an object is written and its
 *      tablet is deleted before the object is removed, reads on that object
 *      will fail because the tablet is no longer owned by the master.
 */
ObjectManager::ObjectManager(Context* context,
                             ServerId serverId,
                             const ServerConfig* config,
                             TabletManager* tabletManager)
    : context(context)
    , config(config)
    , tabletManager(tabletManager)
    , allocator(config)
    , replicaManager(context, serverId,
                     config->master.numReplicas,
                     config->master.useMinCopysets)
    , segmentManager(context, config, serverId,
                     allocator, replicaManager)
    , log(context, config, this, &segmentManager, &replicaManager)
    , objectMap(config->master.hashTableBytes / HashTable::bytesPerCacheLine())
    , anyWrites(false)
    , hashTableBucketLocks()
    , replaySegmentReturnCount(0)
    , tombstoneRemover()
{
    replicaManager.startFailureMonitor();

    if (!config->master.disableLogCleaner)
        log.enableCleaner();

    Dispatch::Lock lock(context->dispatch);
    tombstoneRemover.construct(this, &objectMap);
}

/**
 * The destructor does nothing particularly interesting right now.
 */
ObjectManager::~ObjectManager()
{
    replicaManager.haltFailureMonitor();
}

/**
 * Write an object to this ObjectManager, replacing a previous one if necessary.
 *
 * This method will do everything needed to store an object associated with
 * a particular key. This includes allocating or incrementing version numbers,
 * writing a tombstone if a previous version exists, storing to the log,
 * and adding or replacing an entry in the hash table.
 *
 * Note, however, that the write is not be guaranteed to have completed on
 * backups until the syncWrites() method is called. This allows callers to
 * issue multiple object writes and batch backup writes by syncing once per
 * batch, rather than for each object.
 *
 * \param key
 *      Key that will refer to the object being stored.
 * \param value
 *      The value portion of the key-value pair that a stored object represents.
 *      This is an uninterpreted sequence of bytes.
 * \param rejectRules
 *      Specifies conditions under which the write should be aborted with an
 *      error. May be NULL if no special reject conditions are desired.
 * \param outVersion
 *      If non-NULL, the version number of the new object is returned here. If
 *      the operation was successful this will be the new version for the
 *      object; if this object has ever existed previously the new version is
 *      guaranteed to be greater than any previous version of the object. If the
 *      operation failed then the version number returned is the current version
 *      of the object, or VERSION_NONEXISTENT if the object does not exist.
 * \return
 *      STATUS_OK if the object was written. Otherwise, for example,
 *      STATUS_UKNOWN_TABLE may be returned.
 */
Status
ObjectManager::writeObject(Key& key,
                           Buffer& value,
                           RejectRules* rejectRules,
                           uint64_t* outVersion)
{
    if (!anyWrites) {
        // This is the first write; use this as a trigger to update the
        // cluster configuration information and open a session with each
        // backup, so it won't slow down recovery benchmarks.  This is a
        // temporary hack, and needs to be replaced with a more robust
        // approach to updating cluster configuration information.
        anyWrites = true;

        // Empty coordinator locator means we're in test mode, so skip this.
        if (!context->coordinatorSession->getLocation().empty()) {
            ProtoBuf::ServerList backups;
            CoordinatorClient::getBackupList(context, &backups);
            TransportManager& transportManager =
                *context->transportManager;
            foreach(auto& backup, backups.server())
                transportManager.getSession(backup.service_locator().c_str());
        }
    }

    HashTableBucketLock lock(*this, key);

    // If the tablet doesn't exist in the NORMAL state, we must plead ignorance.
    TabletManager::Tablet tablet;
    if (!tabletManager->getTablet(key, &tablet))
        return STATUS_UNKNOWN_TABLET;
    if (tablet.state != TabletManager::NORMAL)
        return STATUS_UNKNOWN_TABLET;

    LogEntryType currentType = LOG_ENTRY_TYPE_INVALID;
    Buffer currentBuffer;
    Log::Reference currentReference;
    uint64_t currentVersion = VERSION_NONEXISTENT;

    if (lookup(lock, key, currentType, currentBuffer, 0, &currentReference)) {
        if (currentType == LOG_ENTRY_TYPE_OBJTOMB) {
            removeIfTombstone(currentReference.toInteger(), this);
        } else {
            Object currentObject(currentBuffer);
            currentVersion = currentObject.getVersion();
        }
    }

    if (rejectRules != NULL) {
        Status status = rejectOperation(rejectRules, currentVersion);
        if (status != STATUS_OK) {
            if (outVersion != NULL)
                *outVersion = currentVersion;
            return status;
        }
    }

    // Existing objects get a bump in version, new objects start from
    // the next version allocated in the table.
    uint64_t newObjectVersion = (currentVersion == VERSION_NONEXISTENT) ?
            segmentManager.allocateVersion() : currentVersion + 1;

    Object newObject(key,
                     value,
                     newObjectVersion,
                     WallTime::secondsTimestamp());

    assert(currentVersion == VERSION_NONEXISTENT ||
           newObject.getVersion() > currentVersion);

    Tub<ObjectTombstone> tombstone;
    if (currentVersion != VERSION_NONEXISTENT &&
      currentType == LOG_ENTRY_TYPE_OBJ) {
        Object object(currentBuffer);
        tombstone.construct(object,
                            log.getSegmentId(currentReference),
                            WallTime::secondsTimestamp());
    }

    // Create a vector of appends in case we need to write a tombstone and
    // an object. This is necessary to ensure that both tombstone and object
    // are written atomically. The log makes no atomicity guarantees across
    // multiple append calls and we don't want a tombstone going to backups
    // before the new object, or the new object going out without a tombstone
    // for the old deleted version. Both cases lead to consistency problems.
    Log::AppendVector appends[2];

    newObject.serializeToBuffer(appends[0].buffer);
    appends[0].type = LOG_ENTRY_TYPE_OBJ;
    appends[0].timestamp = newObject.getTimestamp();

    if (tombstone) {
        tombstone->serializeToBuffer(appends[1].buffer);
        appends[1].type = LOG_ENTRY_TYPE_OBJTOMB;
        appends[1].timestamp = tombstone->getTimestamp();
    }

    if (!log.append(appends, tombstone ? 2 : 1)) {
        // The log is out of space. Tell the client to retry and hope
        // that either the cleaner makes space soon or we shift load
        // off of this server.
        return STATUS_RETRY;
    }

    replace(lock, key, appends[0].reference);
    if (tombstone)
        log.free(currentReference);
    if (outVersion != NULL)
        *outVersion = newObject.getVersion();

    tabletManager->incrementWriteCount(key);

    TEST_LOG("object: %u bytes, version %lu",
        appends[0].buffer.getTotalLength(), newObject.getVersion());
    if (tombstone) {
        TEST_LOG("tombstone: %u bytes, version %lu",
            appends[1].buffer.getTotalLength(), tombstone->getObjectVersion());
    }

    return STATUS_OK;
}

/**
 * Sync any previous writes. This operation is required after any writeObject()
 * calls to ensure that objects are on stable backup storage. Prior to invoking
 * this, no guarantees are made on the consistency of backup and master views of
 * the log since the previous sync operation.
 */
void
ObjectManager::syncWrites()
{
    log.sync();
}

/**
 * Read an object previously written to this ObjectManager.
 *
 * \param key
 *      Key of the object being read.
 * \param outBuffer
 *      Buffer to populate with the value of the object, if found.
 * \param rejectRules
 *      If non-NULL, use the specified rules to perform a conditional read. See
 *      the RejectRules class documentation for more details.
 * \param outVersion
 *      If non-NULL and the object is found, the version is returned here. If
 *      the reject rules failed the read, the current object's version is still
 *      returned.
 * \return
 *      Returns STATUS_OK if the lookup succeeded and the reject rules did not
 *      preclude this read. Other status values indicate different failures
 *      (object not found, tablet doesn't exist, reject rules applied, etc).
 */
Status
ObjectManager::readObject(Key& key,
                          Buffer* outBuffer,
                          RejectRules* rejectRules,
                          uint64_t* outVersion)
{
    HashTableBucketLock lock(*this, key);

    // If the tablet doesn't exist in the NORMAL state, we must plead ignorance.
    TabletManager::Tablet tablet;
    if (!tabletManager->getTablet(key, &tablet))
        return STATUS_UNKNOWN_TABLET;
    if (tablet.state != TabletManager::NORMAL)
        return STATUS_UNKNOWN_TABLET;

    Buffer buffer;
    LogEntryType type;
    uint64_t version;
    Log::Reference reference;
    bool found = lookup(lock, key, type, buffer, &version, &reference);
    if (!found || type != LOG_ENTRY_TYPE_OBJ)
        return STATUS_OBJECT_DOESNT_EXIST;

    if (outVersion != NULL)
        *outVersion = version;

    if (rejectRules != NULL) {
        Status status = rejectOperation(rejectRules, version);
        if (status != STATUS_OK)
            return status;
    }

    Object object(buffer);
    object.appendDataToBuffer(*outBuffer);

    tabletManager->incrementReadCount(key);

    return STATUS_OK;
}

/**
 * Remove an object previously written to this ObjectManager.
 *
 * \param key
 *      Key of the object to remove.
 * \param rejectRules
 *      If non-NULL, use the specified rules to perform a conditional remove.
 *      See the RejectRules class documentation for more details.
 * \param outVersion
 *      If non-NULL, the version of the current object version is returned here.
 *      Unless rejectRules prevented the operation, this object will have been
 *      deleted. If the rejectRules did prevent removal, the current object's
 *      version is still returned.
 * \return
 *      Returns STATUS_OK if the remove succeeded. Other status values indicate
 *      different failures (tablet doesn't exist, reject rules applied, etc).
 */
Status
ObjectManager::removeObject(Key& key,
                            RejectRules* rejectRules,
                            uint64_t* outVersion)
{
    HashTableBucketLock lock(*this, key);

    // If the tablet doesn't exist in the NORMAL state, we must plead ignorance.
    TabletManager::Tablet tablet;
    if (!tabletManager->getTablet(key, &tablet))
        return STATUS_UNKNOWN_TABLET;
    if (tablet.state != TabletManager::NORMAL)
        return STATUS_UNKNOWN_TABLET;

    LogEntryType type;
    Buffer buffer;
    Log::Reference reference;
    if (!lookup(lock, key, type, buffer, NULL, &reference) ||
      type != LOG_ENTRY_TYPE_OBJ) {
        static RejectRules defaultRejectRules;
        if (rejectRules == NULL)
            rejectRules = &defaultRejectRules;
        return rejectOperation(rejectRules, VERSION_NONEXISTENT);

    }

    Object object(buffer);
    if (outVersion != NULL)
        *outVersion = object.getVersion();

    // Abort if we're trying to delete the wrong version.
    if (rejectRules != NULL) {
        Status status = rejectOperation(rejectRules, object.getVersion());
        if (status != STATUS_OK)
            return status;
    }

    ObjectTombstone tombstone(object,
                              log.getSegmentId(reference),
                              WallTime::secondsTimestamp());
    Buffer tombstoneBuffer;
    tombstone.serializeToBuffer(tombstoneBuffer);

    // Write the tombstone into the Log, increment the tablet version
    // number, and remove from the hash table.
    if (!log.append(LOG_ENTRY_TYPE_OBJTOMB,
                    tombstone.getTimestamp(),
                    tombstoneBuffer)) {
        // The log is out of space. Tell the client to retry and hope
        // that either the cleaner makes space soon or we shift load
        // off of this server.
        return STATUS_RETRY;
    }
    log.sync();

    segmentManager.raiseSafeVersion(object.getVersion() + 1);
    log.free(reference);
    remove(lock, key);
    return STATUS_OK;
}

/**
 * This method is used by replaySegment() to prefetch the hash table bucket
 * corresponding to the next entry to be replayed. Doing so avoids a cache
 * miss for subsequent hash table lookups and significantly speeds up replay.
 *
 * \param it
 *      SegmentIterator to use for prefetching. Whatever is currently pointed
 *      to by this iterator will be used to prefetch, if possible. Some entries
 *      do not contain keys; they are safely ignored.
 */
inline void
ObjectManager::prefetchHashTableBucket(SegmentIterator* it)
{
    if (expect_false(it->isDone()))
        return;

    if (expect_true(it->getType() == LOG_ENTRY_TYPE_OBJ)) {
        const Object::SerializedForm* obj =
            it->getContiguous<Object::SerializedForm>(NULL, 0);
        Key key(obj->tableId, obj->keyAndData, obj->keyLength);
        objectMap.prefetchBucket(key);
    } else if (it->getType() == LOG_ENTRY_TYPE_OBJTOMB) {
        const ObjectTombstone::SerializedForm* tomb =
            it->getContiguous<ObjectTombstone::SerializedForm>(NULL, 0);
        Key key(tomb->tableId, tomb->key, tomb->keyLength);
        objectMap.prefetchBucket(key);
    }
}

/**
 * This class is used by replaySegment to increment the number of times that
 * that method returns, regardless of the return path. That counter is used
 * by the RemoveTombstonePoller class to know when it need not bother scanning
 * the hash table.
 */
template<typename T>
class DelayedIncrementer {
  public:
    /**
     * \param incrementee
     *      Pointer to the object to increment when the destructor is called.
     */
    explicit DelayedIncrementer(T* incrementee)
        : incrementee(incrementee)
    {
    }

    /**
     * Destroy this object and increment the incrementee.
     */
    ~DelayedIncrementer()
    {
        (*incrementee)++;
    }

  PRIVATE:
    /// Pointer to the object that will be incremented in the destructor.
    T* incrementee;

    DISALLOW_COPY_AND_ASSIGN(DelayedIncrementer);
};

/**
 * Replay the entries within a segment and store the appropriate objects. 
 * This method is used during recovery to replay a portion of a failed
 * master's log. It is also used during tablet migration to receive objects
 * from another master.
 *
 * To support out-of-order replay (necessary for performance), ObjectManager
 * will keep track of tombstones during replay and remove any older objects
 * encountered to maintain delete consistency.
 *
 * Objects being replayed should belong to existing tablets in the RECOVERING
 * state. ObjectManager uses the state of the tablets to determine when it is
 * safe to prune tombstones created during replaySegment calls. In particular,
 * tombstones referring to unknown tablets or to tablets not in the RECOVERING
 * state will be pruned. The caller should ensure that when replaying objects
 * for a particular tablet, the tablet already exists in the RECOVERING state
 * before the first invocation of replaySegment() and that the state is changed
 * (or the tablet is dropped) after the last call.
 *
 * \param sideLog
 *      Pointer to the SideLog in which replayed data will be stored.
 * \param it
 *       SegmentIterator which is pointing to the start of the recovery segment
 *       to be replayed into the log.
 */
void
ObjectManager::replaySegment(SideLog* sideLog, SegmentIterator& it)
{
    uint64_t startReplicationTicks = metrics->master.replicaManagerTicks;
    uint64_t startReplicationPostingWriteRpcTicks =
        metrics->master.replicationPostingWriteRpcTicks;
    CycleCounter<RawMetric> _(&metrics->master.recoverSegmentTicks);

    // Metrics can be very expense (they're atomic operations), so we aggregate
    // as much as we can in local variables and update the counters once at the
    // end of this method.
    uint64_t verifyChecksumTicks = 0;
    uint64_t segmentAppendTicks = 0;
    uint64_t recoverySegmentEntryCount = 0;
    uint64_t recoverySegmentEntryBytes = 0;
    uint64_t objectAppendCount = 0;
    uint64_t tombstoneAppendCount = 0;
    uint64_t liveObjectCount = 0;
    uint64_t liveObjectBytes = 0;
    uint64_t objectDiscardCount = 0;
    uint64_t tombstoneDiscardCount = 0;
    uint64_t safeVersionRecoveryCount = 0;
    uint64_t safeVersionNonRecoveryCount = 0;

    // Keep track of the number of times this method returns (or throws). See
    // RemoveTombstonePoller for how this count is used.
    DelayedIncrementer<std::atomic<uint64_t>>
        returnCountIncrementer(&replaySegmentReturnCount);

    SegmentIterator prefetcher = it;
    prefetcher.next();

    uint64_t bytesIterated = 0;
    while (expect_true(!it.isDone())) {
        prefetchHashTableBucket(&prefetcher);
        prefetcher.next();

        LogEntryType type = it.getType();

        if (bytesIterated > 50000) {
            bytesIterated = 0;
            replicaManager.proceed();
        }
        bytesIterated += it.getLength();

        recoverySegmentEntryCount++;
        recoverySegmentEntryBytes += it.getLength();

        if (expect_true(type == LOG_ENTRY_TYPE_OBJ)) {
            // The recovery segment is guaranteed to be contiguous, so we need
            // not provide a copyout buffer.
            const Object::SerializedForm* recoveryObj =
                it.getContiguous<Object::SerializedForm>(NULL, 0);
            Key key(recoveryObj->tableId,
                    recoveryObj->keyAndData,
                    recoveryObj->keyLength);

            bool checksumIsValid = ({
                CycleCounter<uint64_t> c(&verifyChecksumTicks);
                Object::computeChecksum(recoveryObj, it.getLength()) ==
                    recoveryObj->checksum;
            });
            if (expect_false(!checksumIsValid)) {
                LOG(WARNING, "bad object checksum! key: %s, version: %lu",
                    key.toString().c_str(), recoveryObj->version);
                // TODO(Stutsman): Should throw and try another segment replica?
            }

            HashTableBucketLock lock(*this, key);

            uint64_t minSuccessor = 0;
            bool freeCurrentEntry = false;

            LogEntryType currentType;
            Buffer currentBuffer;
            Log::Reference currentReference;
            if (lookup(lock, key, currentType, currentBuffer, 0,
                                                        &currentReference)) {
                uint64_t currentVersion;

                if (currentType == LOG_ENTRY_TYPE_OBJTOMB) {
                    ObjectTombstone currentTombstone(currentBuffer);
                    currentVersion = currentTombstone.getObjectVersion();
                } else {
                    Object currentObject(currentBuffer);
                    currentVersion = currentObject.getVersion();
                    freeCurrentEntry = true;
                }

                minSuccessor = currentVersion + 1;
            }

            if (recoveryObj->version >= minSuccessor) {
                // write to log (with lazy backup flush) & update hash table
                Log::Reference newObjReference;
                {
                    CycleCounter<uint64_t> _(&segmentAppendTicks);
                    sideLog->append(LOG_ENTRY_TYPE_OBJ,
                                    recoveryObj->timestamp,
                                    recoveryObj,
                                    it.getLength(),
                                    &newObjReference);
                }

                // TODO(steve/ryan): what happens if the log is full? won't an
                //      exception here just cause the master to try another
                //      backup?

                objectAppendCount++;
                liveObjectBytes += it.getLength();

                replace(lock, key, newObjReference);

                // nuke the old object, if it existed
                // TODO(steve): put tombstones in the HT and have this free them
                //              as well
                if (freeCurrentEntry) {
                    liveObjectBytes -= currentBuffer.getTotalLength();
                    sideLog->free(currentReference);
                } else {
                    liveObjectCount++;
                }
            } else {
                objectDiscardCount++;
            }
        } else if (type == LOG_ENTRY_TYPE_OBJTOMB) {
            Buffer buffer;
            it.appendToBuffer(buffer);
            Key key(type, buffer);

            ObjectTombstone recoverTomb(buffer);
            bool checksumIsValid = ({
                CycleCounter<uint64_t> c(&verifyChecksumTicks);
                recoverTomb.checkIntegrity();
            });
            if (expect_false(!checksumIsValid)) {
                LOG(WARNING, "bad tombstone checksum! key: %s, version: %lu",
                    key.toString().c_str(), recoverTomb.getObjectVersion());
                // TODO(Stutsman): Should throw and try another segment replica?
            }

            HashTableBucketLock lock(*this, key);

            uint64_t minSuccessor = 0;
            bool freeCurrentEntry = false;

            LogEntryType currentType;
            Buffer currentBuffer;
            Log::Reference currentReference;
            if (lookup(lock, key, currentType, currentBuffer, 0,
                                                        &currentReference)) {
                if (currentType == LOG_ENTRY_TYPE_OBJTOMB) {
                    ObjectTombstone currentTombstone(currentBuffer);
                    minSuccessor = currentTombstone.getObjectVersion() + 1;
                } else {
                    Object currentObject(currentBuffer);
                    minSuccessor = currentObject.getVersion();
                    freeCurrentEntry = true;
                }
            }

            if (recoverTomb.getObjectVersion() >= minSuccessor) {
                tombstoneAppendCount++;
                Log::Reference newTombReference;
                {
                    CycleCounter<uint64_t> _(&segmentAppendTicks);
                    sideLog->append(LOG_ENTRY_TYPE_OBJTOMB,
                                    recoverTomb.getTimestamp(),
                                    buffer,
                                    &newTombReference);
                }

                // TODO(steve/ryan): append could fail here!

                replace(lock, key, newTombReference);

                // nuke the object, if it existed
                if (freeCurrentEntry) {
                    liveObjectCount++;
                    liveObjectBytes -= currentBuffer.getTotalLength();
                    sideLog->free(currentReference);
                }
            } else {
                tombstoneDiscardCount++;
            }
        } else if (type == LOG_ENTRY_TYPE_SAFEVERSION) {
            // LOG_ENTRY_TYPE_SAFEVERSION is duplicated to all the
            // partitions in BackupService::buildRecoverySegments()
            Buffer buffer;
            it.appendToBuffer(buffer);

            ObjectSafeVersion recoverSafeVer(buffer);
            uint64_t safeVersion = recoverSafeVer.getSafeVersion();

            bool checksumIsValid = ({
                CycleCounter<uint64_t> _(&verifyChecksumTicks);
                recoverSafeVer.checkIntegrity();
            });
            if (expect_false(!checksumIsValid)) {
                LOG(WARNING, "bad objectSafeVer checksum! version: %lu",
                    safeVersion);
                // TODO(Stutsman): Should throw and try another segment replica?
            }

            // Copy SafeVerObject to the recovery segment.
            // Sync can be delayed, because recovery can be replayed
            // with the same backup data when the recovery crashes on the way.
            {
                CycleCounter<uint64_t> _(&segmentAppendTicks);
                sideLog->append(LOG_ENTRY_TYPE_SAFEVERSION, 0, buffer);
            }

            // recover segmentManager.safeVersion (Master safeVersion)
            if (segmentManager.raiseSafeVersion(safeVersion)) {
                // true if log.safeVersion is revised.
                safeVersionRecoveryCount++;
                LOG(DEBUG, "SAFEVERSION %lu recovered", safeVersion);
            } else {
                safeVersionNonRecoveryCount++;
                LOG(DEBUG, "SAFEVERSION %lu discarded", safeVersion);
            }
        }

        it.next();
    }

    metrics->master.backupInRecoverTicks +=
        metrics->master.replicaManagerTicks - startReplicationTicks;
    metrics->master.recoverSegmentPostingWriteRpcTicks +=
        metrics->master.replicationPostingWriteRpcTicks -
        startReplicationPostingWriteRpcTicks;
    metrics->master.verifyChecksumTicks += verifyChecksumTicks;
    metrics->master.segmentAppendTicks += segmentAppendTicks;
    metrics->master.recoverySegmentEntryCount += recoverySegmentEntryCount;
    metrics->master.recoverySegmentEntryBytes += recoverySegmentEntryBytes;
    metrics->master.objectAppendCount += objectAppendCount;
    metrics->master.tombstoneAppendCount += tombstoneAppendCount;
    metrics->master.liveObjectCount += liveObjectCount;
    metrics->master.liveObjectBytes += liveObjectBytes;
    metrics->master.objectDiscardCount += objectDiscardCount;
    metrics->master.tombstoneDiscardCount += tombstoneDiscardCount;
    metrics->master.safeVersionRecoveryCount += safeVersionRecoveryCount;
    metrics->master.safeVersionNonRecoveryCount += safeVersionNonRecoveryCount;
}

/**
 * Removes an object from the hash table and frees it from the log if
 * it belongs to a tablet that doesn't exist in the master's TabletManager.
 * Used by deleteOrphanedObjects().
 *
 * \param reference
 *      Reference into the log for an object as returned from the master's
 *      objectMap->lookup() or on callback from objectMap->forEachInBucket().
 *      This object is removed from the objectMap and freed from the log if it
 *      doesn't belong to any tablet the master lists among its tablets.
 * \param cookie
 *      Pointer to the MasterService where this object is currently
 *      stored.
 */
void
ObjectManager::removeIfOrphanedObject(uint64_t reference, void *cookie)
{
    CleanupParameters* params = reinterpret_cast<CleanupParameters*>(cookie);
    ObjectManager* objectManager = params->objectManager;
    LogEntryType type;
    Buffer buffer;

    type = objectManager->log.getEntry(Log::Reference(reference), buffer);
    if (type != LOG_ENTRY_TYPE_OBJ)
        return;

    Key key(type, buffer);
    if (!objectManager->tabletManager->getTablet(key)) {
        TEST_LOG("removing orphaned object at ref %lu", reference);
        bool r = objectManager->remove(*params->lock, key);
        assert(r);
        objectManager->log.free(Log::Reference(reference));
    }
}

/**
 * Scan the hashtable and remove all objects that do not belong to a
 * tablet currently owned by this master. Used to clean up any objects
 * created as part of an aborted recovery.
 */
void
ObjectManager::removeOrphanedObjects()
{
    for (uint64_t i = 0; i < objectMap.getNumBuckets(); i++) {
        HashTableBucketLock lock(*this, i);
        CleanupParameters params = { this , &lock };
        objectMap.forEachInBucket(removeIfOrphanedObject, &params, i);
    }
}

/**
 * Check a set of RejectRules against the current state of an object
 * to decide whether an operation is allowed.
 *
 * \param rejectRules
 *      Specifies conditions under which the operation should fail.
 * \param version
 *      The current version of an object, or VERSION_NONEXISTENT
 *      if the object does not currently exist (used to test rejectRules)
 *
 * \return
 *      The return value is STATUS_OK if none of the reject rules
 *      indicate that the operation should be rejected. Otherwise
 *      the return value indicates the reason for the rejection.
 */
Status
ObjectManager::rejectOperation(const RejectRules* rejectRules, uint64_t version)
{
    if (version == VERSION_NONEXISTENT) {
        if (rejectRules->doesntExist)
            return STATUS_OBJECT_DOESNT_EXIST;
        return STATUS_OK;
    }
    if (rejectRules->exists)
        return STATUS_OBJECT_EXISTS;
    if (rejectRules->versionLeGiven && version <= rejectRules->givenVersion)
        return STATUS_WRONG_VERSION;
    if (rejectRules->versionNeGiven && version != rejectRules->givenVersion)
        return STATUS_WRONG_VERSION;
    return STATUS_OK;
}

/**
 * Extract the timestamp from an entry written into the log. Used by the log
 * code do more efficient cleaning.
 *
 * \param type
 *      Type of the object being queried.
 * \param buffer
 *      Buffer pointing to the object in the log being queried.
 */
uint32_t
ObjectManager::getTimestamp(LogEntryType type, Buffer& buffer)
{
    if (type == LOG_ENTRY_TYPE_OBJ)
        return getObjectTimestamp(buffer);
    else if (type == LOG_ENTRY_TYPE_OBJTOMB)
        return getTombstoneTimestamp(buffer);
    else
        return 0;
}

/**
 * Relocate and update metadata for an object or tombstone that is being
 * cleaned. The cleaner invokes this method for every entry it comes across
 * when processing a segment. If the entry is no longer needed, nothing needs
 * to be done. If it is needed, the provided relocator should be used to copy
 * it to a new location and any metadata pointing to the old entry must be
 * updated before returning.
 *
 * \param type
 *      Type of the entry being cleaned.
 * \param oldBuffer
 *      Buffer pointing to the entry in the log being cleaned. This is the
 *      location that will soon be invalid due to garbage collection.
 * \param relocator
 *      The relocator is used to copy a live entry to a new location in the
 *      log and get a reference to that new location. If the entry is not
 *      needed, the relocator should not be used.
 */
void
ObjectManager::relocate(LogEntryType type,
                        Buffer& oldBuffer,
                        LogEntryRelocator& relocator)
{
    if (type == LOG_ENTRY_TYPE_OBJ)
        relocateObject(oldBuffer, relocator);
    else if (type == LOG_ENTRY_TYPE_OBJTOMB)
        relocateTombstone(oldBuffer, relocator);
}

/**
 * Callback used by the LogCleaner when it's cleaning a Segment and comes
 * across an Object.
 *
 * This callback will decide if the object is still alive. If it is, it must
 * use the relocator to move it to a new location and atomically update the
 * hash table.
 *
 * \param oldBuffer
 *      Buffer pointing to the object's current location, which will soon be
 *      invalidated.
 * \param relocator
 *      The relocator may be used to store the object in a new location if it
 *      is still alive. It also provides a reference to the new location and
 *      keeps track of whether this call wanted the object anymore or not.
 *
 *      It is possible that relocation may fail (because more memory needs to
 *      be allocated). In this case, the callback should just return. The
 *      cleaner will note the failure, allocate more memory, and try again.
 */
void
ObjectManager::relocateObject(Buffer& oldBuffer,
                              LogEntryRelocator& relocator)
{
    Key key(LOG_ENTRY_TYPE_OBJ, oldBuffer);
    HashTableBucketLock lock(*this, key);

    TabletManager::Tablet tablet;
    if (!tabletManager->getTablet(key, &tablet)) {
        // This tablet doesn't exist on the server anymore.
        // Just remove the hash table entry, if it exists.
        remove(lock, key);
        return;
    }

    bool keepNewObject = false;

    LogEntryType currentType;
    Buffer currentBuffer;
    if (lookup(lock, key, currentType, currentBuffer)) {
        assert(currentType == LOG_ENTRY_TYPE_OBJ);

        keepNewObject = (currentBuffer.getStart<uint8_t>() ==
                         oldBuffer.getStart<uint8_t>());
        if (keepNewObject) {
            // Try to relocate it. If it fails, just return. The cleaner will
            // allocate more memory and retry.
            uint32_t timestamp = getObjectTimestamp(oldBuffer);
            if (!relocator.append(LOG_ENTRY_TYPE_OBJ, oldBuffer, timestamp))
                return;
            replace(lock, key, relocator.getNewReference());
        }
    }
}

/**
 * Callback used by the Log to determine the modification timestamp of an
 * Object. Timestamps are stored in the Object itself, rather than in the
 * Log, since not all Log entries need timestamps and other parts of the
 * system (or clients) may care about Object modification times.
 *
 * \param buffer
 *      Buffer pointing to the object the timestamp is to be extracted from.
 * \return
 *      The Object's modification timestamp.
 */
uint32_t
ObjectManager::getObjectTimestamp(Buffer& buffer)
{
    Object object(buffer);
    return object.getTimestamp();
}

/**
 * Callback used by the LogCleaner when it's cleaning a Segment and comes
 * across a Tombstone.
 *
 * This callback will decide if the tombstone is still alive. If it is, it must
 * use the relocator to move it to a new location and atomically update the
 * hash table.
 *
 * \param oldBuffer
 *      Buffer pointing to the tombstone's current location, which will soon be
 *      invalidated.
 * \param relocator
 *      The relocator may be used to store the tombstone in a new location if it
 *      is still alive. It also provides a reference to the new location and
 *      keeps track of whether this call wanted the tombstone anymore or not.
 *
 *      It is possible that relocation may fail (because more memory needs to
 *      be allocated). In this case, the callback should just return. The
 *      cleaner will note the failure, allocate more memory, and try again.
 */
void
ObjectManager::relocateTombstone(Buffer& oldBuffer,
                                 LogEntryRelocator& relocator)
{
    ObjectTombstone tomb(oldBuffer);

    // See if the object this tombstone refers to is still in the log.
    bool keepNewTomb = log.segmentExists(tomb.getSegmentId());

    if (keepNewTomb) {
        // Try to relocate it. If it fails, just return. The cleaner will
        // allocate more memory and retry.
        uint32_t timestamp = getTombstoneTimestamp(oldBuffer);
        if (!relocator.append(LOG_ENTRY_TYPE_OBJTOMB, oldBuffer, timestamp))
            return;
    }
}

/**
 * Callback used by the Log to determine the age of Tombstone.
 *
 * \param buffer
 *      Buffer pointing to the tombstone the timestamp is to be extracted from.
 * \return
 *      The tombstone's creation timestamp.
 */
uint32_t
ObjectManager::getTombstoneTimestamp(Buffer& buffer)
{
    ObjectTombstone tomb(buffer);
    return tomb.getTimestamp();
}

/**
 * Look up an object in the hash table, then extract the entry from the
 * log. Since tombstones are stored in the hash table during recovery,
 * this method may return either an object or a tombstone.
 *
 * \param lock
 *      This method must be invoked with the appropriate hash table bucket
 *      lock already held. This parameter exists to help ensure correct
 *      caller behaviour.
 * \param key
 *      Key of the object being looked up.
 * \param[out] outType
 *      The type of the log entry is returned here.
 * \param[out] buffer
 *      The entry, if found, is appended to this buffer. Note that the data
 *      pointed to by this buffer will be exactly the data in the log. The
 *      cleaner uses this fact to check whether an object in a segment is
 *      alive by comparing the pointer in the hash table (see #relocateObject).
 * \param[out] outVersion
 *      The version of the object or tombstone, when one is found, stored in
 *      this optional patameter.
 * \param[out] outReference
 *      The log reference to the entry, if found, is stored in this optional
 *      parameter.
 * \return
 *      True if an entry is found matching the given key, otherwise false.
 */
bool
ObjectManager::lookup(HashTableBucketLock& lock,
                      Key& key,
                      LogEntryType& outType,
                      Buffer& buffer,
                      uint64_t* outVersion,
                      Log::Reference* outReference)
{
    HashTable::Candidates candidates = objectMap.lookup(key);
    while (!candidates.isDone()) {
        Buffer candidateBuffer;
        Log::Reference candidateRef(candidates.getReference());
        LogEntryType type = log.getEntry(candidateRef, candidateBuffer);

        Key candidateKey(type, candidateBuffer);
        if (key == candidateKey) {
            outType = type;
            buffer.append(&candidateBuffer);
            if (outVersion != NULL) {
                if (type == LOG_ENTRY_TYPE_OBJ) {
                    Object o(candidateBuffer);
                    *outVersion = o.getVersion();
                } else {
                    ObjectTombstone o(candidateBuffer);
                    *outVersion = o.getObjectVersion();
                }
            }
            if (outReference != NULL)
                *outReference = candidateRef;
            return true;
        }

        candidates.next();
    }

    return false;
}

/**
 * Remove an object from the hash table, if it exists in it. Return whether or
 * not it was found and removed.
 *
 * \param lock
 *      This method must be invoked with the appropriate hash table bucket
 *      lock already held. This parameter exists to help ensure correct
 *      caller behaviour.
 * \param key
 *      Key of the object being removed.
 * \return
 *      True if the key was found and the object removed. False if it was not
 *      in the hash table.
 */
bool
ObjectManager::remove(HashTableBucketLock& lock, Key& key)
{
    HashTable::Candidates candidates = objectMap.lookup(key);
    while (!candidates.isDone()) {
        Buffer buffer;
        Log::Reference candidateRef(candidates.getReference());
        LogEntryType type = log.getEntry(candidateRef, buffer);
        Key candidateKey(type, buffer);
        if (key == candidateKey) {
            candidates.remove();
            return true;
        }
        candidates.next();
    }
    return false;
}

/**
 * Insert an object reference into the hash table, or replace the object
 * reference currently associated with the key if one already exists in the
 * table.
 *
 * \param lock
 *      This method must be invoked with the appropriate hash table bucket
 *      lock already held. This parameter exists to help ensure correct
 *      caller behaviour.
 * \param key
 *      The key to add to update a reference for.
 * \param reference
 *      The reference to store in the hash table under the given key.
 * \return
 *      Returns true if the key already existed in the hash table and the
 *      reference was updated. False indicates that the key did not already
 *      exist. In either case, the hash table will refer to the given reference.
 */
bool
ObjectManager::replace(HashTableBucketLock& lock,
                       Key& key,
                       Log::Reference reference)
{
    HashTable::Candidates candidates = objectMap.lookup(key);
    while (!candidates.isDone()) {
        Buffer buffer;
        Log::Reference candidateRef(candidates.getReference());
        LogEntryType type = log.getEntry(candidateRef, buffer);
        Key candidateKey(type, buffer);
        if (key == candidateKey) {
            candidates.setReference(reference.toInteger());
            return true;
        }
        candidates.next();
    }

    objectMap.insert(key, reference.toInteger());
    return false;
}

/**
 * This function is a callback used to purge the tombstones from the hash
 * table after a recovery has taken place. It is invoked by HashTable::
 * forEach via the RemoveTombstonePoller class that runs in the dispatch
 * thread.
 *
 * This function must be called with the appropriate HashTableBucketLock
 * held.
 */
void
ObjectManager::removeIfTombstone(uint64_t maybeTomb, void *cookie)
{
    CleanupParameters* params = reinterpret_cast<CleanupParameters*>(cookie);
    ObjectManager* objectManager = params->objectManager;
    LogEntryType type;
    Buffer buffer;

    type = objectManager->log.getEntry(Log::Reference(maybeTomb), buffer);
    if (type == LOG_ENTRY_TYPE_OBJTOMB) {
        Key key(type, buffer);

        // We can remove tombstones so long as they meet one of the two
        // following criteria:
        //  1) Tablet is not assigned to us (not in TabletManager, so we don't
        //     care about it).
        //  2) Tablet is not in the RECOVERING state (replaySegment won't be
        //     called for objects in that tablet anymore).
        bool discard = false;

        TabletManager::Tablet tablet;
        if (!objectManager->tabletManager->getTablet(key, &tablet) ||
          tablet.state != TabletManager::RECOVERING) {
            discard = true;
        }

        if (discard) {
            TEST_LOG("discarding");
            bool r = objectManager->remove(*params->lock, key);
            assert(r);
        }

        // Tombstones are not explicitly freed in the log. The cleaner will
        // figure out that they're dead.
    }
}

/**
 * Synchronously remove leftover tombstones in the hash table added during
 * replaySegment calls (for example, as caused by a recovery). This private
 * method exists for testing purposes only, since asynchronous removal raises
 * hell in unit tests.
 */
void
ObjectManager::removeTombstones()
{
    for (uint64_t i = 0; i < objectMap.getNumBuckets(); i++) {
        HashTableBucketLock lock(*this, i);
        CleanupParameters params = { this , &lock };
        objectMap.forEachInBucket(removeIfTombstone, &params, i);
    }
}

/**
 * Clean tombstones from #objectMap lazily and in the background.
 *
 * Instances of this class must be allocated with new since they
 * delete themselves when the #objectMap scan is completed which
 * automatically deregisters it from Dispatch.
 *
 * \param objectManager 
 *      The instance of ObjectManager which owns the #objectMap.
 * \param objectMap
 *      The HashTable which will be purged of tombstones.
 */
ObjectManager::RemoveTombstonePoller::RemoveTombstonePoller(
                                        ObjectManager* objectManager,
                                        HashTable* objectMap)
    : Dispatch::Poller(*objectManager->context->dispatch, "TombstoneRemover")
    , currentBucket(0)
    , passes(0)
    , lastReplaySegmentCount(0)
    , objectManager(objectManager)
    , objectMap(objectMap)
{
    LOG(DEBUG, "Starting cleanup of tombstones in background");
}

/**
 * Remove tombstones from a single bucket and yield to other work
 * in the system.
 */
void
ObjectManager::RemoveTombstonePoller::poll()
{
    if (lastReplaySegmentCount == objectManager->replaySegmentReturnCount &&
      currentBucket == 0) {
        return;
    }

    // At the start of a new pass, record the number of replaySegment()
    // calls that have completed by this point. We will then keep doing
    // passes until this number remains constant at the beginning and
    // end of a pass.
    //
    // A recovery is likely to issue many replaySegment calls, but
    // should complete much faster than one pass here, so at worst we
    // should hopefully only traverse the hash table an extra time per
    // recovery.
    if (currentBucket == 0)
        lastReplaySegmentCount = objectManager->replaySegmentReturnCount;

    HashTableBucketLock lock(*objectManager, currentBucket);
    CleanupParameters params = { objectManager, &lock };
    objectMap->forEachInBucket(removeIfTombstone, &params, currentBucket);

    ++currentBucket;
    if (currentBucket == objectMap->getNumBuckets()) {
        LOG(DEBUG, "Cleanup of tombstones completed pass %lu", passes);
        currentBucket = 0;
        passes++;
    }
}

} //enamespace RAMCloud
