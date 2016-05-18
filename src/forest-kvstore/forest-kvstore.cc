/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2015 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "forest-kvstore/forest-kvstore.h"

#include "common.h"

#include <sys/stat.h>
#include <platform/dirutils.h>
#include <vbucket.h>
#include <JSON_checker.h>
#include <locks.h>

using namespace CouchbaseDirectoryUtilities;

extern "C" {
    static int recordChangesC(fdb_kvs_handle *handle, fdb_doc *doc, void *ctx)
    {
        return ForestKVStore::recordChanges(handle, doc, ctx);
    }
}

Mutex ForestKVStore::initLock;
int ForestKVStore::numGlobalFiles = 0;

void ForestKVStore::initForestDb() {
    LockHolder lh(initLock);
    if (numGlobalFiles == 0) {
        EventuallyPersistentEngine *epe = ObjectRegistry::onSwitchThread(NULL, true);
        fdb_status status = fdb_init(&fileConfig);
        if (status != FDB_RESULT_SUCCESS) {
            throw std::logic_error("ForestKVStore::initForestDb: failed "
                    "with status:" + std::to_string(status));
        }
        ObjectRegistry::onSwitchThread(epe);
    }
    ++numGlobalFiles;
}

void ForestKVStore::shutdownForestDb() {
   LockHolder lh(initLock);
   if (--numGlobalFiles == 0) {
       EventuallyPersistentEngine *epe = ObjectRegistry::onSwitchThread(NULL, true);
       fdb_status status = fdb_shutdown();
       if (status != FDB_RESULT_SUCCESS) {
           LOG(EXTENSION_LOG_WARNING,
               "Shutting down forestdb failed with error: %s",
               fdb_error_msg(status));
       }
       ObjectRegistry::onSwitchThread(epe);
   }
}

ForestKVStore::ForestKVStore(KVStoreConfig &config) :
    KVStore(config), intransaction(false),
    dbname(config.getDBName()), dbFileRevNum(1) {

    /* create the data directory */
    createDataDir(dbname);
    fdb_status status;

    uint16_t shardId = config.getShardId();
    uint16_t maxVbuckets = config.getMaxVBuckets();
    uint16_t maxShards = config.getMaxShards();

    std::stringstream dbFile;
    dbFile << dbname << "/" << shardId << ".fdb";

    std::stringstream prefix;
    prefix << shardId << ".fdb";

    const std::vector<std::string> files = findFilesContaining(dbname,
                                           prefix.str().c_str());

    for (auto& filename : files) {
        size_t secondDot = filename.rfind(".");
        std::string revNumStr = filename.substr(secondDot + 1);
        char *ptr = NULL;
        uint64_t revNum = strtoull(revNumStr.c_str(), &ptr, 10);
        if (revNum == 0) {
            LOG(EXTENSION_LOG_WARNING,
                "Invalid revision number obtained for database file");
            abort();
        }

        if (revNum > dbFileRevNum) {
            dbFileRevNum = revNum;
        }
    }

    dbFile << "." << dbFileRevNum;

    fileConfig = fdb_get_default_config();
    kvsConfig = fdb_get_default_kvs_config();

    /* Set the purge interval to the maximum possible value to ensure
     * that deleted items don't get removed immediately. The tombstone
     * items will be purged in the expiry callback.
     */
    fileConfig.purging_interval = std::numeric_limits<uint32_t>::max();

    /* Since DCP requires sequence based iteration, enable sequence tree
     * indexes in forestdb
     */
    fileConfig.seqtree_opt = FDB_SEQTREE_USE;

    initForestDb();

    status = fdb_open(&dbFileHandle, dbFile.str().c_str(), &fileConfig);
    if (status != FDB_RESULT_SUCCESS) {
        LOG(EXTENSION_LOG_WARNING,
            "Opening the database file instance failed with error: %s\n",
            fdb_error_msg(status));
        abort();
    }

    fdb_kvs_handle *kvsHandle = NULL;
    /* Initialize the reader and writer handle map for all the vbuckets in
     * the shard
     */
    for (uint16_t i = shardId; i < maxVbuckets; i += maxShards) {
        writeHandleMap.insert(std::make_pair(i, kvsHandle));
        readHandleMap.insert(std::make_pair(i, kvsHandle));
    }

    // Initialize the handle for the vbucket state information
    status = fdb_kvs_open_default(dbFileHandle, &vbStateHandle, &kvsConfig);

    if (status != FDB_RESULT_SUCCESS) {
        LOG(EXTENSION_LOG_WARNING,
            "Opening the vbucket state KV store instance failed "
            "with error: %s\n", fdb_error_msg(status));
        abort();
    }

    cachedVBStates.reserve(maxVbuckets);
    cachedValidVBCount = 0;
    for (uint16_t i = 0; i < maxVbuckets; ++i) {
        cachedVBStates.push_back((vbucket_state *)NULL);
        if (i == shardId) {
            if (files.size() != 0) {
                readVBState(i);
                if (cachedVBStates[i] && cachedVBStates[i]->state != vbucket_state_dead) {
                    cachedValidVBCount++;
                }
            }
            shardId += maxShards; // jump to next vbucket in shard
        }
    }

    cachedDocCount.assign(maxVbuckets, Couchbase::RelaxedAtomic<size_t>(-1));
    cachedDeleteCount.assign(maxVbuckets, Couchbase::RelaxedAtomic<size_t>(-1));
}

fdb_config ForestKVStore::getFileConfig() {
    return fileConfig;
}

fdb_kvs_config ForestKVStore::getKVConfig() {
    return kvsConfig;
}

ForestKVStore::ForestKVStore(const ForestKVStore &copyFrom) :
    KVStore(copyFrom), intransaction(false),
    dbname(copyFrom.dbname) {

    /* create the data directory */
    createDataDir(dbname);
    LockHolder lh(initLock);
    ++numGlobalFiles;
}

ForestKVStore::~ForestKVStore() {
   close();

   /* delete all the cached vbucket states */
   std::vector<vbucket_state *>::iterator stateItr;
   for (stateItr = cachedVBStates.begin(); stateItr != cachedVBStates.end();
        stateItr++) {
        vbucket_state *vbstate = *stateItr;
        if (vbstate) {
            delete vbstate;
            *stateItr = NULL;
        }
   }

   /* Close all the KV store instances */
   std::unordered_map<uint16_t, fdb_kvs_handle *>::iterator handleItr;
   for (handleItr = writeHandleMap.begin(); handleItr != writeHandleMap.end();
        handleItr++) {
       fdb_kvs_handle *kvsHandle = handleItr->second;
       fdb_kvs_close(kvsHandle);
   }

   writeHandleMap.clear();

   for (handleItr = readHandleMap.begin(); handleItr != readHandleMap.end();
        handleItr++) {
       fdb_kvs_handle *kvsHandle = handleItr->second;
       fdb_kvs_close(kvsHandle);
   }

   readHandleMap.clear();

   /* Close the database file instance */
   fdb_close(dbFileHandle);

   shutdownForestDb();
}

void ForestKVStore::reset(uint16_t vbucketId) {
    vbucket_state *state = cachedVBStates[vbucketId];
    if (!state) {
        throw std::invalid_argument("ForestKVStore::reset::No entry "
                "in cached states for vbucket " +
                std::to_string(vbucketId));
    }

    state->reset();

    cachedDocCount[vbucketId] = 0;
    cachedDeleteCount[vbucketId] = 0;

    fdb_kvs_handle *kvsHandle = getKvsHandle(vbucketId, handleType::READER);
    fdb_status status;

    if (kvsHandle != nullptr) {
        status = fdb_kvs_close(kvsHandle);
        if (status != FDB_RESULT_SUCCESS) {
            LOG(EXTENSION_LOG_WARNING,
                "ForestKVStore::reset:fdb_kvs_close API call failed for "
                "vbucket %" PRIu16" with error: %s", vbucketId,
                fdb_error_msg(status));
        }
    }

    readHandleMap[vbucketId] = NULL;

    kvsHandle = getKvsHandle(vbucketId, handleType::WRITER);

    if (kvsHandle != nullptr) {
        status = fdb_kvs_close(kvsHandle);
        if (status != FDB_RESULT_SUCCESS) {
            LOG(EXTENSION_LOG_WARNING,
                "ForestKVStore::reset:fdb_kvs_close API call failed for "
                "vbucket %" PRIu16" with error: %s", vbucketId,
                fdb_error_msg(status));
        }
    }

    writeHandleMap[vbucketId] = NULL;

    char kvsName[20];
    sprintf(kvsName,"partition%" PRIu16, vbucketId);

    status = fdb_kvs_remove(dbFileHandle, kvsName);
    if (status != FDB_RESULT_SUCCESS) {
        LOG(EXTENSION_LOG_WARNING,
            "ForestKVStore::reset: ForestDB KV Store remove failed for "
            "vbucket :%" PRIu16" with error: %s", vbucketId,
            fdb_error_msg(status));
    }

    std::string stateStr = state->toJSON();

    if (!stateStr.empty()) {
        char keybuf[20];
        fdb_doc statDoc;
        memset(&statDoc, 0, sizeof(statDoc));
        sprintf(keybuf, "partition%d", vbucketId);
        statDoc.key = keybuf;
        statDoc.keylen = strlen(keybuf);
        statDoc.meta = NULL;
        statDoc.metalen = 0;
        statDoc.body = const_cast<char *>(stateStr.c_str());
        statDoc.bodylen = stateStr.length();
        fdb_status status = fdb_set(vbStateHandle, &statDoc);

        if (status != FDB_RESULT_SUCCESS) {
            LOG(EXTENSION_LOG_WARNING, "ForestKVStore::reset:Failed to save "
                "vbucket state for vbucket=%" PRIu16" error=%s", vbucketId,
                fdb_error_msg(status));
        }
    }

    updateFileInfo();
}

ForestRequest::ForestRequest(const Item &it, MutationRequestCallback &cb ,bool del)
    : IORequest(it.getVBucketId(), cb, del, it.getKey()),
      status(MUTATION_SUCCESS) { }

ForestRequest::~ForestRequest() { }

int ForestKVStore::recordChanges(fdb_kvs_handle *handle,
                                 fdb_doc *doc,
                                 void *ctx) {

}

void ForestKVStore::close() {
    intransaction = false;
}

ENGINE_ERROR_CODE ForestKVStore::readVBState(uint16_t vbId) {
    fdb_status status = FDB_RESULT_SUCCESS;
    vbucket_state_t state = vbucket_state_dead;
    uint64_t checkpointId = 0;
    uint64_t maxDeletedSeqno = 0;
    std::string failovers;
    int64_t highSeqno = 0;
    uint64_t lastSnapStart = 0;
    uint64_t lastSnapEnd = 0;
    uint64_t maxCas = 0;
    int64_t driftCounter = INITIAL_DRIFT;

    fdb_kvs_info kvsInfo;
    fdb_kvs_handle *kvsHandle = getKvsHandle(vbId, handleType::READER);
    if (kvsHandle != nullptr) {
        status = fdb_get_kvs_info(kvsHandle, &kvsInfo);
        //TODO: Update the purge sequence number
        if (status == FDB_RESULT_SUCCESS) {
            highSeqno = static_cast<int64_t>(kvsInfo.last_seqnum);
        } else {
            LOG(EXTENSION_LOG_WARNING, "ForestKVStore::readVBState: Failed to "
                "read KV Store info for vbucket: %d with error: %s", vbId,
                fdb_error_msg(status));
            return forestErr2EngineErr(status);
        }
    } else {
        LOG(EXTENSION_LOG_WARNING, "ForestKVStore::readVBState: Failed to "
            "get reader handle for vbucket: %d", vbId);
    }

    char keybuf[20];
    fdb_doc *statDoc;
    sprintf(keybuf, "partition%d", vbId);
    fdb_doc_create(&statDoc, (void *)keybuf, strlen(keybuf), NULL, 0, NULL, 0);
    status = fdb_get(vbStateHandle, statDoc);

    if (status != FDB_RESULT_SUCCESS) {
        LOG(EXTENSION_LOG_DEBUG,
            "ForestKVStore::readVBState: Failed to retrieve vbucket state for vBucket=%d "
            "with error=%s", vbId , fdb_error_msg(status));
    } else {
        cJSON *jsonObj = cJSON_Parse((char *)statDoc->body);

        if (!jsonObj) {
            fdb_doc_free(statDoc);
            LOG(EXTENSION_LOG_WARNING, "ForestKVStore::readVBState: Failed to "
                "parse the vbstat json doc for vbucket: %d: %s", vbId,
                (char *)statDoc->body);
            return forestErr2EngineErr(status);
        }

        const std::string vb_state = getJSONObjString(
                                 cJSON_GetObjectItem(jsonObj, "state"));

        const std::string checkpoint_id = getJSONObjString(
                                 cJSON_GetObjectItem(jsonObj,"checkpoint_id"));

        const std::string max_deleted_seqno = getJSONObjString(
                                 cJSON_GetObjectItem(jsonObj, "max_deleted_seqno"));

        const std::string snapStart = getJSONObjString(
                                 cJSON_GetObjectItem(jsonObj, "snap_start"));

        const std::string snapEnd = getJSONObjString(
                                 cJSON_GetObjectItem(jsonObj, "snap_end"));

        const std::string maxCasValue = getJSONObjString(
                                 cJSON_GetObjectItem(jsonObj, "max_cas"));

        const std::string driftCount = getJSONObjString(
                                 cJSON_GetObjectItem(jsonObj, "drift_counter"));

        cJSON *failover_json = cJSON_GetObjectItem(jsonObj, "failover_table");
        if (vb_state.compare("") == 0 || checkpoint_id.compare("") == 0
               || max_deleted_seqno.compare("") == 0) {
             LOG(EXTENSION_LOG_WARNING,
                 "ForestKVStore::readVBState: State JSON "
                 "doc for vbucket: %d is in the wrong format: %s,"
                 "vb state: %s, checkpoint id: %s and max deleted seqno: %s",
                 vbId, (char *)statDoc->body, vb_state.c_str(),
                 checkpoint_id.c_str(), max_deleted_seqno.c_str());
        } else {
            state = VBucket::fromString(vb_state.c_str());
            parseUint64(max_deleted_seqno.c_str(), &maxDeletedSeqno);
            parseUint64(checkpoint_id.c_str(), &checkpointId);

            if (snapStart.compare("")) {
                parseUint64(snapStart.c_str(), &lastSnapStart);
            }

            if (snapEnd.compare("")) {
                parseUint64(snapEnd.c_str(), &lastSnapEnd);
            }

            if (maxCasValue.compare("")) {
                parseUint64(maxCasValue.c_str(), &maxCas);
            }

            if (driftCount.compare("")) {
                parseInt64(driftCount.c_str(), &driftCounter);
            }

            if (failover_json) {
                char* json = cJSON_PrintUnformatted(failover_json);
                failovers.assign(json);
                free(json);
            }
        }

        cJSON_Delete(jsonObj);
    }

    if (failovers.empty()) {
        failovers.assign("[{\"id\":0,\"seq\":0}]");
    }

    delete cachedVBStates[vbId];
    cachedVBStates[vbId] = new vbucket_state(state, checkpointId,
                                             maxDeletedSeqno, highSeqno, 0,
                                             lastSnapStart, lastSnapEnd,
                                             maxCas, driftCounter,
                                             failovers);
    fdb_doc_free(statDoc);
    return forestErr2EngineErr(status);
}

void ForestKVStore::updateFileInfo(void) {
    fdb_file_info finfo;
    fdb_status status = fdb_get_file_info(dbFileHandle, &finfo);
    if (status == FDB_RESULT_SUCCESS) {
        cachedFileSize = finfo.file_size;
        cachedSpaceUsed = finfo.space_used;
    } else {
        LOG(EXTENSION_LOG_WARNING,
            "ForestKVStore::updateFileInfo: Getting file info"
            " failed with error: %s for shard id: %" PRIu16,
            fdb_error_msg(status), configuration.getShardId());
    }
}

void ForestKVStore::delVBucket(uint16_t vbucket) {
    fdb_kvs_handle *kvsHandle = getKvsHandle(vbucket, handleType::READER);
    fdb_status status;

    if (kvsHandle != nullptr) {
        status = fdb_kvs_close(kvsHandle);
        if (status != FDB_RESULT_SUCCESS) {
            LOG(EXTENSION_LOG_WARNING,
                "fdb_kvs_close API call failed for vbucket %d with error: %s "
                "in ForestKVStore::delVBucket\n", vbucket, fdb_error_msg(status));
        }
    }

    readHandleMap[vbucket] = NULL;

    kvsHandle = getKvsHandle(vbucket, handleType::WRITER);

    if (kvsHandle != nullptr) {
        status = fdb_kvs_close(kvsHandle);
        if (status != FDB_RESULT_SUCCESS) {
            LOG(EXTENSION_LOG_WARNING,
                "fdb_kvs_close API call failed for vbucket %d with error: %s "
                "in ForestKVStore::delVBucket\n", vbucket, fdb_error_msg(status));
        }
    }

    writeHandleMap[vbucket] = NULL;

    char kvsName[20];
    sprintf(kvsName,"partition%d", vbucket);

    status = fdb_kvs_remove(dbFileHandle, kvsName);
    if (status != FDB_RESULT_SUCCESS) {
        LOG(EXTENSION_LOG_WARNING,
            "ForestDB KV Store remove failed for vbucket "
            "%d with error: %s\n", vbucket,
            fdb_error_msg(status));
    }

    if (cachedVBStates[vbucket]) {
        delete cachedVBStates[vbucket];
    }

    std::string failovers("[{\"id\":0, \"seq\":0}]");
    cachedVBStates[vbucket] = new vbucket_state(vbucket_state_dead, 0, 0, 0, 0,
                                                0, 0, 0, INITIAL_DRIFT,
                                                failovers);

    vbucket_state *state = cachedVBStates[vbucket];
    std::string stateStr = state->toJSON();

    if (!stateStr.empty()) {
        fdb_doc statDoc;
        memset(&statDoc, 0, sizeof(statDoc));
        statDoc.key = kvsName;
        statDoc.keylen = strlen(kvsName);
        statDoc.meta = NULL;
        statDoc.metalen = 0;
        statDoc.body = const_cast<char *>(stateStr.c_str());
        statDoc.bodylen = stateStr.length();
        fdb_status status = fdb_set(vbStateHandle, &statDoc);

        if (status != FDB_RESULT_SUCCESS) {
            LOG(EXTENSION_LOG_WARNING, "ForestKVStore::delVBucket: Failed to "
                    "save vbucket state for vbucket=%" PRIu16" error=%s",
                    vbucket, fdb_error_msg(status));
        }
    }

    updateFileInfo();
    cachedValidVBCount--;
}

ENGINE_ERROR_CODE ForestKVStore::forestErr2EngineErr(fdb_status errCode) {
    switch (errCode) {
    case FDB_RESULT_SUCCESS:
        return ENGINE_SUCCESS;
    case FDB_RESULT_ALLOC_FAIL:
        return ENGINE_ENOMEM;
    case FDB_RESULT_KEY_NOT_FOUND:
        return ENGINE_KEY_ENOENT;
    case FDB_RESULT_NO_SUCH_FILE:
    case FDB_RESULT_NO_DB_HEADERS:
    default:
        // same as the general error return code of
        // EventuallyPersistentStore::getInternal
        return ENGINE_TMPFAIL;
    }
}

void ForestKVStore::getWithHeader(void *dbHandle, const std::string &key,
                                  uint16_t vb, Callback<GetValue> &cb,
                                  bool fetchDelete) {
    fdb_file_handle *dbFileHandle = (fdb_file_handle *)dbHandle;
    RememberingCallback<GetValue> *rc =
                       static_cast<RememberingCallback<GetValue> *>(&cb);
    bool getMetaOnly = rc && rc->val.isPartial();
    GetValue rv;
    fdb_kvs_handle *kvsHandle = NULL;
    fdb_status status;
    char kvsName[20];
    sprintf(kvsName, "partition%d", vb);

    std::unordered_map<uint16_t, fdb_kvs_handle*>::iterator found =
                                                       readHandleMap.find(vb);

    //There will always be a handle in the handle map for readers. It will be
    //initialized to NULL if one hasn't been initialized yet.
    if (found == readHandleMap.end()) {
        throw std::invalid_argument("ForestKVStore::getWithHeader: Failed "
                "to find vb (which is " + std::to_string(vb) +
                ") in readHandleMap");
    }
    if (!(found->second)) {
        status = fdb_kvs_open(dbFileHandle, &kvsHandle, kvsName, &kvsConfig);
        if (status != FDB_RESULT_SUCCESS) {
            LOG(EXTENSION_LOG_WARNING,
                "Opening the KV store instance for vbucket %d failed "
                "with error: %s", vb, fdb_error_msg(status));
            rv.setStatus(forestErr2EngineErr(status));
            cb.callback(rv);
            return;
        }
        found->second = kvsHandle;
    }
    else {
        kvsHandle = found->second;
    }

    fdb_doc rdoc;
    memset(&rdoc, 0, sizeof(rdoc));
    rdoc.key = const_cast<char *>(key.c_str());
    rdoc.keylen = key.length();

    if (!getMetaOnly) {
        status = fdb_get(kvsHandle, &rdoc);
    } else {
        status = fdb_get_metaonly(kvsHandle, &rdoc);
    }

    if (status != FDB_RESULT_SUCCESS) {
        if (!getMetaOnly) {
            LOG(EXTENSION_LOG_WARNING,
                "Failed to retrieve metadata from "
                "database, vbucketId:%d key:%s error:%s\n",
                vb, key.c_str(), fdb_error_msg(status));
        } else {
            LOG(EXTENSION_LOG_WARNING,
                "Failed to retrieve key value from database,"
                "vbucketId:%d key:%s error:%s deleted:%s", vb, key.c_str(),
                fdb_error_msg(status), rdoc.deleted ? "yes" : "no");
        }
    } else {
        rv = docToItem(kvsHandle, &rdoc, vb, getMetaOnly, fetchDelete);
    }

    rdoc.key = NULL;
    fdb_free_block(rdoc.meta);
    fdb_free_block(rdoc.body);

    rv.setStatus(forestErr2EngineErr(status));
    cb.callback(rv);
}

static ForestMetaData forestMetaDecode(const fdb_doc *rdoc) {
    char *metadata = static_cast<char *>(rdoc->meta);
    ForestMetaData forestMetaData;

    memcpy(&forestMetaData.cas, metadata + forestMetaOffset(cas),
           sizeof(forestMetaData.cas));
    memcpy(&forestMetaData.rev_seqno, metadata + forestMetaOffset(rev_seqno),
           sizeof(forestMetaData.rev_seqno));
    memcpy(&forestMetaData.exptime, metadata + forestMetaOffset(exptime),
           sizeof(forestMetaData.exptime));
    memcpy(&forestMetaData.texptime, metadata + forestMetaOffset(texptime),
           sizeof(forestMetaData.texptime));
    memcpy(&forestMetaData.flags, metadata + forestMetaOffset(flags),
           sizeof(forestMetaData.flags));
    memcpy(&forestMetaData.ext_meta, metadata + forestMetaOffset(ext_meta),
           EXT_META_LEN);
    memcpy(&forestMetaData.confresmode, metadata + forestMetaOffset(confresmode),
           CONFLICT_RES_META_LEN);

    forestMetaData.cas = ntohll(forestMetaData.cas);
    forestMetaData.rev_seqno = ntohll(forestMetaData.rev_seqno);
    forestMetaData.exptime = ntohl(forestMetaData.exptime);
    forestMetaData.texptime = ntohl(forestMetaData.texptime);

    return forestMetaData;
}

GetValue ForestKVStore::docToItem(fdb_kvs_handle *kvsHandle, fdb_doc *rdoc,
                                  uint16_t vbId, bool metaOnly, bool fetchDelete) {
    ForestMetaData forestMetaData;

    //TODO: handle metadata upgrade?
    forestMetaData = forestMetaDecode(rdoc);

    Item *it = NULL;
    if (metaOnly || (fetchDelete && rdoc->deleted)) {
        it = new Item((char *)rdoc->key, rdoc->keylen, forestMetaData.flags,
                      forestMetaData.exptime, NULL, 0, forestMetaData.ext_meta,
                      EXT_META_LEN, forestMetaData.cas,
                      (uint64_t)rdoc->seqnum, vbId);
        if (rdoc->deleted) {
            it->setDeleted();
        }
    } else {
        size_t valuelen = rdoc->bodylen;
        void *valuePtr = rdoc->body;
        uint8_t ext_meta[EXT_META_LEN];

        if (checkUTF8JSON((const unsigned char *)valuePtr, valuelen)) {
            ext_meta[0] = PROTOCOL_BINARY_DATATYPE_JSON;
        } else {
            ext_meta[0] = PROTOCOL_BINARY_RAW_BYTES;
        }

        it = new Item((char *)rdoc->key, rdoc->keylen, forestMetaData.flags,
                      forestMetaData.exptime, valuePtr, valuelen,
                      ext_meta, EXT_META_LEN, forestMetaData.cas,
                      (uint64_t)rdoc->seqnum, vbId);
    }

    it->setConflictResMode(
            static_cast<enum conflict_resolution_mode>(forestMetaData.confresmode));
    it->setRevSeqno(forestMetaData.rev_seqno);
    return GetValue(it);
}

vbucket_state * ForestKVStore::getVBucketState(uint16_t vbucketId) {
    return cachedVBStates[vbucketId];
}

static void commitCallback(std::vector<ForestRequest *> &committedReqs) {
    size_t commitSize = committedReqs.size();

    for (size_t index = 0; index < commitSize; index++) {
        int rv = committedReqs[index]->getStatus();
        if (committedReqs[index]->isDelete()) {
            committedReqs[index]->getDelCallback()->callback(rv);
        } else {
            //TODO: For now, all mutations are passed in as insertions.
            //This needs to be revisited in order to update stats.
            mutation_result p(rv, true);
            committedReqs[index]->getSetCallback()->callback(p);
        }
    }
}

bool ForestKVStore::save2forestdb() {
    size_t pendingCommitCnt = pendingReqsQ.size();
    if (pendingCommitCnt == 0) {
        return true;
    }

    uint16_t maxVbuckets = configuration.getMaxVBuckets();
    uint16_t numShards = configuration.getMaxShards();
    uint16_t shardId = configuration.getShardId();

    for (uint16_t vbid = shardId; vbid < maxVbuckets; vbid += numShards) {
        vbucket_state *state = cachedVBStates[vbid];
        if (state != nullptr) {
            std::string stateStr = state->toJSON();
            if (!stateStr.empty()) {
                fdb_doc statDoc;
                memset(&statDoc, 0, sizeof(statDoc));
                char kvsName[20];
                statDoc.keylen = snprintf(kvsName, sizeof(kvsName),"partition%d", vbid);
                statDoc.key = kvsName;
                statDoc.meta = NULL;
                statDoc.metalen = 0;
                statDoc.body = const_cast<char *>(stateStr.c_str());
                statDoc.bodylen = stateStr.length();
                fdb_status status = fdb_set(vbStateHandle, &statDoc);

                if (status != FDB_RESULT_SUCCESS) {
                    throw std::runtime_error("ForestKVStore::save2forestdb: "
                        "Failed to save vbucket state for vbucket id: " +
                        std::to_string(vbid) + " with error: " +
                        std::string(fdb_error_msg(status)));
                }
            }
        }
    }

    fdb_status status = fdb_commit(dbFileHandle, FDB_COMMIT_NORMAL);
    if (status != FDB_RESULT_SUCCESS) {
        throw std::runtime_error("ForestKVStore::save2forestdb: "
            "fdb_commit failed for shard id: " + std::to_string(shardId) +
            "with error: " + std::string(fdb_error_msg(status)));
    }

    for (uint16_t vbId = shardId; vbId < maxVbuckets; vbId += numShards) {
        fdb_kvs_handle *kvsHandle = getKvsHandle(vbId, handleType::READER);
        fdb_kvs_info kvsInfo;
        fdb_status status = fdb_get_kvs_info(kvsHandle, &kvsInfo);

        if (status == FDB_RESULT_SUCCESS) {
            cachedDeleteCount[vbId] = kvsInfo.deleted_count;
            cachedDocCount[vbId] = kvsInfo.doc_count;
            vbucket_state *state = cachedVBStates[vbId];
            if (state) {
                state->highSeqno = kvsInfo.last_seqnum;
            }
        } else {
            throw std::runtime_error("ForestKVStore::save2forestdb: "
                "Failed to get KV store info for vbucket id: " +
                std::to_string(vbId) + " with error: " +
                std::string(fdb_error_msg(status)));
        }
    }

    commitCallback(pendingReqsQ);

    for (size_t i = 0; i < pendingCommitCnt; i++) {
        delete pendingReqsQ[i];
    }

    updateFileInfo();

    pendingReqsQ.clear();
    return true;
}

bool ForestKVStore::commit() {
    if (intransaction) {
        if (save2forestdb()) {
            intransaction = false;
        }
    }

    return !intransaction;
}

StorageProperties ForestKVStore::getStorageProperties(void) {
    StorageProperties rv(StorageProperties::EfficientVBDump::Yes,
                         StorageProperties::EfficientVBDeletion::Yes,
                         StorageProperties::PersistedDeletion::Yes,
                         StorageProperties::EfficientGet::Yes,
                         StorageProperties::ConcurrentWriteCompact::Yes);
    return rv;
}

fdb_kvs_handle* ForestKVStore::getKvsHandle(uint16_t vbucketId, handleType htype) {
    char kvsName[20];
    sprintf(kvsName, "partition%d", vbucketId);
    fdb_kvs_handle *kvsHandle = NULL;
    fdb_status status;
    std::unordered_map<uint16_t, fdb_kvs_handle*>::iterator found;

    switch (htype) {
        case handleType::WRITER:
            found = writeHandleMap.find(vbucketId);
            if (found == writeHandleMap.end()) {
                throw std::invalid_argument("ForestKVStore::getKvsHandle: Failed "
                    "to find vb (which is " + std::to_string(vbucketId) +
                    ") in writeHandleMap");
            }
            break;
        case handleType::READER:
            found = readHandleMap.find(vbucketId);
            if (found == readHandleMap.end()) {
                throw std::invalid_argument("ForestKVStore::getKvsHandle: Failed "
                    "to find vb (which is " + std::to_string(vbucketId) +
                    ") in readHandleMap");
            }
            break;
    }

    if (!(found->second)) {
        status = fdb_kvs_open(dbFileHandle, &kvsHandle, kvsName, &kvsConfig);
        if (status != FDB_RESULT_SUCCESS) {
            LOG(EXTENSION_LOG_WARNING,
                "Opening the KV store instance for vbucket %d failed "
                "with error: %s", vbucketId, fdb_error_msg(status));
            return NULL;
        }
        found->second = kvsHandle;
    } else {
        kvsHandle = found->second;
    }

    return kvsHandle;
}

static int8_t getMutationStatus(fdb_status errCode) {
    switch(errCode) {
        case FDB_RESULT_SUCCESS:
            return MUTATION_SUCCESS;
        case FDB_RESULT_NO_DB_HEADERS:
        case FDB_RESULT_NO_SUCH_FILE:
        case FDB_RESULT_KEY_NOT_FOUND:
            return DOC_NOT_FOUND;
        default:
            return MUTATION_FAILED;
    }
}

static void populateMetaData(const Item &itm, uint8_t *meta, bool deletion) {
    uint64_t cas = htonll(itm.getCas());
    uint64_t rev_seqno = htonll(itm.getRevSeqno());
    uint32_t flags = itm.getFlags();
    uint32_t exptime = itm.getExptime();
    uint32_t texptime = 0;
    uint8_t confresmode = static_cast<uint8_t>(itm.getConflictResMode());

    if (deletion) {
        texptime = ep_real_time();
    }

    exptime = htonl(exptime);
    texptime = htonl(texptime);

    memcpy(meta, &cas, sizeof(cas));
    memcpy(meta + forestMetaOffset(rev_seqno), &rev_seqno, sizeof(rev_seqno));
    memcpy(meta + forestMetaOffset(exptime), &exptime, sizeof(exptime));
    memcpy(meta + forestMetaOffset(texptime), &texptime, sizeof(texptime));
    memcpy(meta + forestMetaOffset(flags), &flags, sizeof(flags));

    *(meta + forestMetaOffset(flex_meta)) = FLEX_META_CODE;

    if (deletion) {
        *(meta + forestMetaOffset(ext_meta)) = PROTOCOL_BINARY_RAW_BYTES;
    } else {
        memcpy(meta + forestMetaOffset(ext_meta), itm.getExtMeta(),
               itm.getExtMetaLen());
    }

    memcpy(meta + forestMetaOffset(confresmode), &confresmode,
           CONFLICT_RES_META_LEN);
}

void ForestKVStore::set(const Item &itm, Callback<mutation_result> &cb) {
    if (isReadOnly()) {
        throw std::logic_error("ForestKVStore::set: Not valid on a read-only "
                        "object.");
    }
    if (!intransaction) {
        throw std::invalid_argument("ForestKVStore::set: intransaction must be "
                        "true to perform a set operation.");
    }
    MutationRequestCallback requestcb;

    // each req will be de-allocated after commit
    requestcb.setCb = &cb;
    ForestRequest *req = new ForestRequest(itm, requestcb, false);
    fdb_doc setDoc;
    fdb_kvs_handle *kvsHandle = NULL;
    fdb_status status;
    uint8_t meta[FORESTDB_METADATA_SIZE];

    memset(meta, 0, sizeof(meta));
    populateMetaData(itm, meta, false);

    setDoc.key = const_cast<char *>(itm.getKey().c_str());
    setDoc.keylen = itm.getNKey();
    setDoc.meta = meta;
    setDoc.metalen = sizeof(meta);
    setDoc.body = const_cast<char *>(itm.getData());
    setDoc.bodylen = itm.getNBytes();
    setDoc.deleted = false;

    fdb_doc_set_seqnum(&setDoc, itm.getBySeqno());
    kvsHandle = getKvsHandle(req->getVBucketId(), handleType::WRITER);
    if (kvsHandle) {
        status = fdb_set(kvsHandle, &setDoc);
        if (status != FDB_RESULT_SUCCESS) {
            LOG(EXTENSION_LOG_WARNING, "fdb_set failed for key: %s and "
                "vbucketId: %d with error: %s", req->getKey().c_str(),
                req->getVBucketId(), fdb_error_msg(status));
            req->setStatus(getMutationStatus(status));
        }
        setDoc.body = NULL;
        setDoc.bodylen = 0;
    } else {
        LOG(EXTENSION_LOG_WARNING, "Failed to open KV store instance "
            "for key: %s and vbucketId: %d", req->getKey().c_str(),
            req->getVBucketId());
        req->setStatus(MUTATION_FAILED);
    }

    pendingReqsQ.push_back(req);
}

void ForestKVStore::get(const std::string &key, uint16_t vb,
                        Callback<GetValue> &cb, bool fetchDelete) {
    getWithHeader(dbFileHandle, key, vb, cb, fetchDelete);
}

ENGINE_ERROR_CODE
ForestKVStore::getAllKeys(uint16_t vbid, std::string &start_key, uint32_t count,
                         std::shared_ptr<Callback<uint16_t&, char*&> > cb) {

    fdb_kvs_handle *kvsHandle = getKvsHandle(vbid, handleType::READER);
    if (kvsHandle == nullptr) {
        throw std::invalid_argument("ForestKVStore::getAllKeys: Unable to "
                  "retrieve KV store handle for vbucket id:" +
                  std::to_string(vbid));
    }

    fdb_iterator *fdb_iter = NULL;
    fdb_status status = fdb_iterator_init(kvsHandle, &fdb_iter,
                                          start_key.c_str(), strlen(start_key.c_str()),
                                          NULL, 0, FDB_ITR_NO_DELETES);
    if (status != FDB_RESULT_SUCCESS) {
        throw std::runtime_error("ForestKVStore::getAllKeys: iterator "
                   "initalization failed for vbucket id " + std::to_string(vbid) +
                   " and start key:" + start_key.c_str());
    }

    fdb_doc *rdoc = NULL;
    status = fdb_doc_create(&rdoc, NULL, 0, NULL, 0, NULL, 0);
    if (status != FDB_RESULT_SUCCESS) {
       fdb_iterator_close(fdb_iter);
       throw std::runtime_error("ForestKVStore::getAllKeys: creating "
                  "the document failed for vbucket id:" +
                  std::to_string(vbid) + " with error:" +
                  fdb_error_msg(status));
    }

    rdoc->key = malloc(MAX_KEY_LENGTH);
    rdoc->meta = malloc(FORESTDB_METADATA_SIZE);

    for (uint32_t curr_count = 0; curr_count < count; curr_count++) {
        status = fdb_iterator_get_metaonly(fdb_iter, &rdoc);
        if (status != FDB_RESULT_SUCCESS) {
            fdb_doc_free(rdoc);
            fdb_iterator_close(fdb_iter);
            throw std::runtime_error("ForestKVStore::getAllKeys: iterator "
                       "get failed for vbucket id " + std::to_string(vbid) +
                       " and start key:" + start_key.c_str());
        }
        uint16_t keylen = static_cast<uint16_t>(rdoc->keylen);
        char *key = static_cast<char *>(rdoc->key);
        cb->callback(keylen, key);

        if (fdb_iterator_next(fdb_iter) != FDB_RESULT_SUCCESS) {
            break;
        }
    }

    fdb_doc_free(rdoc);
    fdb_iterator_close(fdb_iter);

    return ENGINE_SUCCESS;
}

void ForestKVStore::getMulti(uint16_t vb, vb_bgfetch_queue_t &itms) {
    bool meta_only = true;
    vb_bgfetch_queue_t::iterator itr = itms.begin();
    for (; itr != itms.end(); ++itr) {
        vb_bgfetch_item_ctx_t &bg_itm_ctx = (*itr).second;
        meta_only = bg_itm_ctx.isMetaOnly;

        RememberingCallback<GetValue> gcb;
        if (meta_only) {
            gcb.val.setPartial();
        }

        const std::string &key = (*itr).first;
        get(key, vb, gcb);
        ENGINE_ERROR_CODE status = gcb.val.getStatus();
        if (status != ENGINE_SUCCESS) {
            LOG(EXTENSION_LOG_WARNING, "Failed to retrieve key: %s",
                key.c_str());
        }

        std::list<VBucketBGFetchItem *> &fetches = bg_itm_ctx.bgfetched_list;
        std::list<VBucketBGFetchItem *>:: iterator fitr = fetches.begin();

        for (fitr = fetches.begin(); fitr != fetches.end(); ++fitr) {
            (*fitr)->value = gcb.val;
        }
    }
}

void ForestKVStore::del(const Item &itm, Callback<int> &cb) {
    if (isReadOnly()) {
        throw std::logic_error("ForestKVStore::del: Not valid on a read-only "
                        "object.");
    }
    if (!intransaction) {
        throw std::invalid_argument("ForestKVStore::del: intransaction must be "
                        "true to perform a delete operation.");
    }
    MutationRequestCallback requestcb;
    requestcb.delCb = &cb;
    ForestRequest *req = new ForestRequest(itm, requestcb, true);
    fdb_doc delDoc;
    fdb_kvs_handle *kvsHandle = NULL;
    fdb_status status;
    uint8_t meta[FORESTDB_METADATA_SIZE];

    memset(meta, 0, sizeof(meta));
    populateMetaData(itm, meta, true);

    delDoc.key = const_cast<char *>(itm.getKey().c_str());
    delDoc.keylen = itm.getNKey();
    delDoc.meta = meta;
    delDoc.metalen = sizeof(meta);
    delDoc.deleted = true;

    fdb_doc_set_seqnum(&delDoc, itm.getBySeqno());
    kvsHandle = getKvsHandle(req->getVBucketId(), handleType::WRITER);
    if (kvsHandle) {
        status = fdb_del(kvsHandle, &delDoc);
        if (status != FDB_RESULT_SUCCESS) {
            LOG(EXTENSION_LOG_WARNING, "fdb_del failed for key: %s and "
                "vbucketId: %d with error: %s",  req->getKey().c_str(),
                req->getVBucketId(), fdb_error_msg(status));
            req->setStatus(getMutationStatus(status));
        }
    } else {
        LOG(EXTENSION_LOG_WARNING, "Failure to open KV store instance "
            "for key: %s and vbucketId: %d", req->getKey().c_str(),
            req->getVBucketId());
        req->setStatus(MUTATION_FAILED);
    }

    pendingReqsQ.push_back(req);
}

std::vector<vbucket_state *> ForestKVStore::listPersistedVbuckets(void) {
    return cachedVBStates;
}

size_t ForestKVStore::getNumPersistedDeletes(uint16_t vbid) {
    size_t delCount = cachedDeleteCount[vbid];
    if (delCount != (size_t) -1) {
        return delCount;
    }

    fdb_kvs_handle *kvsHandle = getKvsHandle(vbid, handleType::READER);
    if (!kvsHandle) {
        std::string err("ForestKVStore::getNumPersistedDeletes: Failed to "
                        "get reader KV store handle for vbucket:" +
                        std::to_string(static_cast<int>(vbid)));
        throw std::invalid_argument(err);
    }

    fdb_kvs_info kvsInfo;
    fdb_status status = fdb_get_kvs_info(kvsHandle, &kvsInfo);
    if (status != FDB_RESULT_SUCCESS) {
        std::string err("ForestKVStore::getNumPersistedDeletes:Failed to "
            "retrieve KV store info with error:" +
            std::string(fdb_error_msg(status)) + " for vbucket id:" +
            std::to_string(static_cast<int>(vbid)));
        throw std::runtime_error(err);
    }

    return kvsInfo.deleted_count;
}

DBFileInfo ForestKVStore::getDbFileInfo(uint16_t vbId) {
    DBFileInfo dbInfo;
    fdb_file_info fileInfo;
    fdb_kvs_info kvsInfo;
    fdb_kvs_handle *kvsHandle = NULL;
    fdb_status status;

    status = fdb_get_file_info(dbFileHandle, &fileInfo);
    if (status != FDB_RESULT_SUCCESS) {
        std::string err("ForestKVStore::getDbFileInfo:Failed to retrieve "
            "file info with error:" + std::string(fdb_error_msg(status)) +
            " for vbucket id:" + std::to_string(static_cast<int>(vbId)));
        throw std::runtime_error(err);
    }

    kvsHandle = getKvsHandle(vbId, handleType::READER);
    if (!kvsHandle) {
        std::string err("ForestKVStore::getDbFileInfo:Failed to get reader "
            "KV store handle for vbucket:" +
            std::to_string(static_cast<int>(vbId)));
        throw std::invalid_argument(err);
    }

    status = fdb_get_kvs_info(kvsHandle, &kvsInfo);
    if (status != FDB_RESULT_SUCCESS) {
        std::string err("ForestKVStore::getDbFileInfo:Failed to retrieve "
            "KV store info with error:" + std::string(fdb_error_msg(status)) +
            " vbucket id:" + std::to_string(static_cast<int>(vbId)));
        throw std::runtime_error(err);
    }

    dbInfo.fileSize =  fileInfo.file_size/fileInfo.num_kv_stores;
    dbInfo.spaceUsed = fileInfo.space_used/fileInfo.num_kv_stores;

    return dbInfo;
}

DBFileInfo ForestKVStore::getAggrDbFileInfo() {
    DBFileInfo dbInfo;

    dbInfo.fileSize = cachedFileSize.load();
    dbInfo.spaceUsed = cachedSpaceUsed.load();

    return dbInfo;
}

bool ForestKVStore::snapshotVBucket(uint16_t vbucketId, vbucket_state &vbstate,
                                    VBStatePersist options) {
    if (updateCachedVBState(vbucketId, vbstate) &&
         (options == VBStatePersist::VBSTATE_PERSIST_WITHOUT_COMMIT ||
          options == VBStatePersist::VBSTATE_PERSIST_WITH_COMMIT)) {
        std::string stateStr = cachedVBStates[vbucketId]->toJSON();
        char keybuf[20];
        fdb_doc statDoc;
        memset(&statDoc, 0, sizeof(statDoc));
        sprintf(keybuf, "partition%d", vbucketId);
        statDoc.key = keybuf;
        statDoc.keylen = strlen(keybuf);
        statDoc.meta = NULL;
        statDoc.metalen = 0;
        statDoc.body = const_cast<char *>(stateStr.c_str());
        statDoc.bodylen = stateStr.length();
        fdb_status status = fdb_set(vbStateHandle, &statDoc);

        if (status != FDB_RESULT_SUCCESS) {
            LOG(EXTENSION_LOG_WARNING, "ForestKVStore::snapshotVBucket:Failed to "
                "save vbucket state for vbucket=%d error=%s", vbucketId,
                fdb_error_msg(status));
            return false;
        }

        if (options == VBStatePersist::VBSTATE_PERSIST_WITH_COMMIT) {
            status = fdb_commit(dbFileHandle, FDB_COMMIT_NORMAL);

            if (status != FDB_RESULT_SUCCESS) {
                LOG(EXTENSION_LOG_WARNING, "ForestKVStore::snapshotVBucket::Failed "
                    "to commit vbucket state for vbucket=%d error=%s", vbucketId,
                    fdb_error_msg(status));
                return false;
            }
        }
    }

    updateFileInfo();

    return true;
}

ScanContext* ForestKVStore::initScanContext(std::shared_ptr<Callback<GetValue> > cb,
                                           std::shared_ptr<Callback<CacheLookup> > cl,
                                           uint16_t vbid, uint64_t startSeqno,
                                           DocumentFilter options,
                                           ValueFilter valOptions) {
    fdb_kvs_handle *kvsHandle = NULL;

    kvsHandle = getKvsHandle(vbid, handleType::READER);
    if (!kvsHandle) {
        std::string err("ForestKVStore::initScanContext: Failed to get reader "
                        "KV store handle for vbucket:" +
                        std::to_string(static_cast<int>(vbid)));
        throw std::invalid_argument(err);
    }

    fdb_kvs_info kvsInfo;
    fdb_status status = fdb_get_kvs_info(kvsHandle, &kvsInfo);
    if (status != FDB_RESULT_SUCCESS) {
        std::string err("ForestKVStore::initScanContext:Failed to retrieve "
            "KV store info with error:" + std::string(fdb_error_msg(status)) +
            "vbucket id:" + std::to_string(static_cast<int>(vbid)));
        throw std::runtime_error(err);
    }

    size_t backfillId = backfillCounter++;
    size_t count = getNumItems(kvsHandle,
                               startSeqno,
                               std::numeric_limits<uint64_t>::max());

    LockHolder lh(backfillLock);
    backfills[backfillId] = kvsHandle;

    return new ScanContext(cb, cl, vbid, backfillId, startSeqno,
                           (uint64_t)kvsInfo.last_seqnum, options,
                           valOptions, count);
}

scan_error_t ForestKVStore::scan(ScanContext *ctx) {
     if (!ctx) {
         return scan_failed;
     }

     if (ctx->lastReadSeqno == ctx->maxSeqno) {
         return scan_success;
     }

     LockHolder lh(backfillLock);
     auto itr = backfills.find(ctx->scanId);
     if (itr == backfills.end()) {
         return scan_failed;
     }

     fdb_kvs_handle *kvsHandle = itr->second;
     lh.unlock();

     fdb_iterator_opt_t options;
     fdb_iterator *fdb_iter = NULL;
     fdb_status status;
     fdb_doc *rdoc;

     std::shared_ptr<Callback<GetValue> > cb = ctx->callback;
     std::shared_ptr<Callback<CacheLookup> > cl = ctx->lookup;

     bool validFilter = false;
     switch (ctx->docFilter) {
          case DocumentFilter::NO_DELETES:
              options = FDB_ITR_NO_DELETES;
              validFilter = true;
              break;
          case DocumentFilter::ALL_ITEMS:
              options = FDB_ITR_NONE;
              validFilter = true;
              break;
          case DocumentFilter::ONLY_DELETES:
              break;
     }

     if (!validFilter) {
         std::string err("ForestKVStore::scan: Illegal document filter!" +
                         std::to_string(static_cast<int>(ctx->docFilter)));
         throw std::logic_error(err);
     }

     fdb_seqnum_t start;
     if (ctx->lastReadSeqno != 0) {
         start = static_cast<fdb_seqnum_t>(ctx->lastReadSeqno) + 1;
     } else {
         start = static_cast<fdb_seqnum_t>(ctx->startSeqno);
     }

     status = fdb_iterator_sequence_init(kvsHandle, &fdb_iter, start,
                                         0, options);
     if (status != FDB_RESULT_SUCCESS) {
         LOG(EXTENSION_LOG_WARNING,
             "ForestDB iterator initialization failed with error "
             "message: %s", fdb_error_msg(status));
         return scan_failed;
     }

     status = fdb_doc_create(&rdoc, NULL, 0, NULL, 0, NULL, 0);
     if (status != FDB_RESULT_SUCCESS) {
         LOG(EXTENSION_LOG_WARNING,
             "ForestDB doc creation failed with error: %s",
             fdb_error_msg(status));
         fdb_iterator_close(fdb_iter);
         return scan_failed;
     }
     // Pre-allocate key and meta data as their max sizes are known.
     rdoc->key = malloc(MAX_KEY_LENGTH);
     rdoc->meta = malloc(FORESTDB_METADATA_SIZE);
     // Document body will be allocated by fdb_iterator_get API below.
     rdoc->body = NULL;
     do {
         void *valuePtr = NULL;
         size_t valueLen = 0;

         if (ctx->valFilter != ValueFilter::KEYS_ONLY) {
             status = fdb_iterator_get(fdb_iter, &rdoc);
         } else {
             status = fdb_iterator_get_metaonly(fdb_iter, &rdoc);
         }
         if (status != FDB_RESULT_SUCCESS) {
             LOG(EXTENSION_LOG_WARNING,
                 "fdb_iterator_get failed with error: %s",
                 fdb_error_msg(status));
             fdb_doc_free(rdoc);
             fdb_iterator_close(fdb_iter);
             return scan_failed;
         }
         std::string docKey((char *)rdoc->key, rdoc->keylen);

         CacheLookup lookup(docKey, (uint64_t) rdoc->seqnum, ctx->vbid);
         cl->callback(lookup);
         if (cl->getStatus() == ENGINE_KEY_EEXISTS) {
             ctx->lastReadSeqno = static_cast<uint64_t>(rdoc->seqnum);
             free(rdoc->body);
             rdoc->body = NULL;
             continue;
         } else if (cl->getStatus() == ENGINE_ENOMEM) {
             fdb_doc_free(rdoc);
             fdb_iterator_close(fdb_iter);
             return scan_again;
         }

         ForestMetaData forestMetaData;
         forestMetaData = forestMetaDecode(rdoc);
         uint64_t bySeqno = static_cast<uint64_t>(rdoc->seqnum);

         if (ctx->valFilter != ValueFilter::KEYS_ONLY && !rdoc->deleted) {
             valuePtr = rdoc->body;
             valueLen = rdoc->bodylen;
         }

         Item *it = new Item(rdoc->key, rdoc->keylen, forestMetaData.flags,
                             forestMetaData.exptime,valuePtr, valueLen,
                             forestMetaData.ext_meta, EXT_META_LEN,
                             forestMetaData.cas, bySeqno, ctx->vbid,
                             forestMetaData.rev_seqno);

         if (rdoc->deleted) {
             it->setDeleted();
         }

         it->setConflictResMode(
               static_cast<enum conflict_resolution_mode>(forestMetaData.confresmode));

         bool onlyKeys = (ctx->valFilter == ValueFilter::KEYS_ONLY) ? true : false;
         GetValue rv(it, ENGINE_SUCCESS, -1, onlyKeys);
         cb->callback(rv);

         free(rdoc->body);
         rdoc->body = NULL;

         if (cb->getStatus() == ENGINE_ENOMEM) {
             fdb_doc_free(rdoc);
             fdb_iterator_close(fdb_iter);
             return scan_again;
         }

         ctx->lastReadSeqno = bySeqno;
     } while (fdb_iterator_next(fdb_iter) == FDB_RESULT_SUCCESS);

     fdb_doc_free(rdoc);
     rdoc = NULL;
     fdb_iterator_close(fdb_iter);

     return scan_success;
}

void ForestKVStore::destroyScanContext(ScanContext* ctx) {
    if (!ctx) {
        return;
    }

    LockHolder lh(backfillLock);
    auto itr = backfills.find(ctx->scanId);
    if (itr != backfills.end()) {
        backfills.erase(itr);
    }

    delete ctx;
}

extern "C" {
    static fdb_compact_decision compaction_cb_c(fdb_file_handle *fhandle,
                            fdb_compaction_status status, const char *kv_name,
                            fdb_doc *doc, uint64_t old_offset,
                            uint64_t new_offset, void *ctx) {
        return ForestKVStore::compaction_cb(fhandle, status, kv_name, doc,
                                            old_offset, new_offset, ctx);
    }
}

fdb_compact_decision ForestKVStore::compaction_cb(fdb_file_handle *fhandle,
                                                  fdb_compaction_status comp_status,
                                                  const char *kv_name,
                                                  const fdb_doc *doc, uint64_t old_offset,
                                                  uint64_t new_offset, void *ctx) {
    /**
     * The default KV store holds the vbucket state information for all the vbuckets
     * in the shard.
     */
    if (strcmp(kv_name, "default") == 0) {
        return FDB_CS_KEEP_DOC;
    }

    compaction_ctx *comp_ctx = reinterpret_cast<compaction_ctx *>(ctx);
    ForestKVStore *store = reinterpret_cast<ForestKVStore *>(comp_ctx->store);

    /* Every KV store name for ep-engine has the format "partition<vbucket id>."
     * Extract the vbucket id from the kvstore name */
    std::string kvNameStr(kv_name);
    kvNameStr.erase(kvNameStr.begin(), kvNameStr.begin() + strlen("partition"));

    uint16_t vbid;
    try {
        vbid = std::stoi(kvNameStr);
    } catch (...) {
        LOG(EXTENSION_LOG_WARNING,
            "ForestKVStore::compaction_cb: Failed to convert string to integer "
            "for KV store name: %s", kv_name);
        return FDB_CS_KEEP_DOC;
    }

    fdb_kvs_handle *kvsHandle = store->getKvsHandle(vbid, handleType::READER);
    if (!kvsHandle) {
        LOG(EXTENSION_LOG_WARNING,
            "ForestKVStore::compaction_cb: Failed to retrieve reader handle for "
            "vbucket %" PRIu16, vbid);
        return FDB_CS_KEEP_DOC;
    }

    fdb_kvs_info kvsInfo;
    fdb_status status = fdb_get_kvs_info(kvsHandle, &kvsInfo);
    if (status != FDB_RESULT_SUCCESS) {
        LOG(EXTENSION_LOG_WARNING,
            "ForestKVStore::compaction_cb: Failed to retrieve KV store information for "
            "vbucket %" PRIu16 " with error: %s", vbid, fdb_error_msg(status));
        return FDB_CS_KEEP_DOC;
    }

    ForestMetaData forestMetaData = forestMetaDecode(doc);

    std::string key((char *)doc->key, doc->keylen);
    if (doc->deleted) {
        uint64_t max_purge_seq = 0;
        auto it = comp_ctx->max_purged_seq.find(vbid);

        if (it == comp_ctx->max_purged_seq.end()) {
            comp_ctx->max_purged_seq[vbid] = 0;
        } else {
            max_purge_seq = it->second;
        }

        if (doc->seqnum != kvsInfo.last_seqnum) {
            if (comp_ctx->drop_deletes) {
                if (max_purge_seq < doc->seqnum) {
                    comp_ctx->max_purged_seq[vbid] = doc->seqnum;
                }

                return FDB_CS_DROP_DOC;
            }

            if (forestMetaData.texptime < comp_ctx->purge_before_ts &&
                    (!comp_ctx->purge_before_seq ||
                     doc->seqnum <= comp_ctx->purge_before_seq)) {
                if (max_purge_seq < doc->seqnum) {
                    comp_ctx->max_purged_seq[vbid] = doc->seqnum;
                }

                return FDB_CS_DROP_DOC;
            }
        }
    } else if (forestMetaData.exptime) {
        std::vector<vbucket_state *> vbStates = store->listPersistedVbuckets();
        vbucket_state *vbState = vbStates[vbid];
        if (vbState && vbState->state == vbucket_state_active) {
            time_t curr_time = ep_real_time();
            if (forestMetaData.exptime < curr_time) {
                comp_ctx->expiryCallback->callback(vbid, key,
                                                   forestMetaData.rev_seqno,
                                                   curr_time);
            }
        }
    }

    if (comp_ctx->bloomFilterCallback) {
        bool deleted = doc->deleted;
        comp_ctx->bloomFilterCallback->callback(vbid, key, deleted);
    }

    return FDB_CS_KEEP_DOC;
}

bool ForestKVStore::compactDB(compaction_ctx *ctx) {
    uint16_t shardId = ctx->db_file_id;
    uint64_t prevRevNum = dbFileRevNum;

    dbFileRevNum++;

    std::string dbFileBase = dbname + "/" + std::to_string(shardId) + ".fdb.";

    std::string prevDbFile = dbFileBase + std::to_string(prevRevNum);

    std::string newDbFile = dbFileBase + std::to_string(dbFileRevNum);

    fileConfig.compaction_cb = compaction_cb_c;
    fileConfig.compaction_cb_ctx = ctx;
    fileConfig.compaction_cb_mask = FDB_CS_MOVE_DOC;

    ctx->store = this;

    fdb_file_handle *compactFileHandle;
    fdb_status status = fdb_open(&compactFileHandle, prevDbFile.c_str(),
                                 &fileConfig);

    if (status != FDB_RESULT_SUCCESS) {
        LOG(EXTENSION_LOG_WARNING,
            "ForestKVStore::compactDB: Failed to open database file: %s"
            " with error: %s", prevDbFile.c_str(), fdb_error_msg(status));
        return false;
    }

    status = fdb_compact(compactFileHandle, newDbFile.c_str());

    if (status != FDB_RESULT_SUCCESS) {
        LOG(EXTENSION_LOG_WARNING,
            "ForestKVStore::compactDB: Failed to compact from database file: %s"
            " to database file: %s with error: %s", prevDbFile.c_str(), newDbFile.c_str(),
            fdb_error_msg(status));
        fdb_close(compactFileHandle);
        return false;
    }

    fdb_close(compactFileHandle);

    return true;
}

size_t ForestKVStore::getNumItems(uint16_t vbid, uint64_t min_seq,
                                  uint64_t max_seq) {
    fdb_kvs_handle *kvsHandle = NULL;
    kvsHandle = getKvsHandle(vbid, handleType::READER);
    return getNumItems(kvsHandle, min_seq, max_seq);
}

size_t ForestKVStore::getNumItems(fdb_kvs_handle* kvsHandle,
                                  uint64_t min_seq,
                                  uint64_t max_seq) {
    size_t totalCount = 0;
    fdb_iterator* fdb_iter = nullptr;
    fdb_status status = fdb_iterator_sequence_init(kvsHandle, &fdb_iter, min_seq,
                                                   max_seq, FDB_ITR_NONE);
    if (status != FDB_RESULT_SUCCESS) {
        std::string err("ForestKVStore::getNumItems: ForestDB iterator "
            "initialization failed with error: " +
             std::string(fdb_error_msg(status)));
        throw std::runtime_error(err);
    }

    do {
        totalCount++;
    } while (fdb_iterator_next(fdb_iter) == FDB_RESULT_SUCCESS);

    fdb_iterator_close(fdb_iter);

    return totalCount;
}

size_t ForestKVStore::getItemCount(uint16_t vbid) {
        return cachedDocCount.at(vbid);
}

RollbackResult ForestKVStore::rollback(uint16_t vbid, uint64_t rollbackSeqno,
                                       std::shared_ptr<RollbackCB> cb) {
   fdb_kvs_handle *kvsHandle = NULL;
   fdb_kvs_info kvsInfo;

   kvsHandle = getKvsHandle(vbid, handleType::WRITER);
   if (!kvsHandle) {
       LOG(EXTENSION_LOG_WARNING,
           "Failed to get reader KV store handle for vbucket "
           "id :%d and rollback sequence number: %" PRIu64, vbid,
           rollbackSeqno);
       return RollbackResult(false, 0, 0, 0);
   }

   fdb_status status = fdb_get_kvs_info(kvsHandle, &kvsInfo);
   if (status != FDB_RESULT_SUCCESS) {
       LOG(EXTENSION_LOG_WARNING,
           "Failed to retrieve KV store info with error: %s for "
           "vbucket id: %d and rollback sequence number: %" PRIu64,
           fdb_error_msg(status), vbid, rollbackSeqno);
       return RollbackResult(false, 0, 0 ,0);
   }

   fdb_iterator *fdb_iter;
   fdb_iterator_opt_t options;
   options = FDB_ITR_NONE;

   uint64_t totalCount = kvsInfo.doc_count;

   uint64_t currentSeqno = fdb_get_available_rollback_seq(kvsHandle,
                                                          rollbackSeqno);
   if (currentSeqno == 0) {
       LOG(EXTENSION_LOG_WARNING,
           "Unable to find an available rollback sequence number "
           "for vbucket id: %d with rollback request sequence number: "
           "%" PRIu64, vbid, rollbackSeqno);
       return RollbackResult(false, 0, 0, 0);
   }

   uint64_t lastSeqno = kvsInfo.last_seqnum;
   status = fdb_iterator_sequence_init(kvsHandle, &fdb_iter, currentSeqno,
                                       lastSeqno, options);
   if (status != FDB_RESULT_SUCCESS) {
       LOG(EXTENSION_LOG_WARNING,
           "ForestDB iterator initialization failed with error message: %s "
           "for vbucket id: %d and rollback sequence number: %" PRIu64,
           fdb_error_msg(status), vbid, rollbackSeqno);
       return RollbackResult(false, 0, 0, 0);
   }

   uint64_t rollbackCount = 0;
   std::vector<std::string> rollbackKeys;
   fdb_doc *rdoc = NULL;
   fdb_doc_create(&rdoc, NULL, 0, NULL, 0, NULL, 0);
   rdoc->key = malloc(MAX_KEY_LENGTH);
   rdoc->meta = malloc(FORESTDB_METADATA_SIZE);
   do {
       status = fdb_iterator_get_metaonly(fdb_iter, &rdoc);
       if (status != FDB_RESULT_SUCCESS) {
           LOG(EXTENSION_LOG_WARNING,
               "ForestDB iterator get meta failed with error: %s "
               "for vbucket id: %d and rollback sequence number: %" PRIu64,
               fdb_error_msg(status), vbid, rollbackSeqno);
           fdb_doc_free(rdoc);
           fdb_iterator_close(fdb_iter);
           return RollbackResult(false, 0, 0, 0);
       }
       rollbackCount++;
       std::string str((char *)rdoc->key, rdoc->keylen);
       rollbackKeys.push_back(str);
   } while (fdb_iterator_next(fdb_iter) == FDB_RESULT_SUCCESS);

   fdb_doc_free(rdoc);
   fdb_iterator_close(fdb_iter);

   if ((totalCount/2) <= rollbackCount) {
       return RollbackResult(false, 0, 0, 0);
   }

   cb->setDbHeader(kvsHandle);

   status = fdb_rollback(&kvsHandle, currentSeqno);
   if (status != FDB_RESULT_SUCCESS) {
       LOG(EXTENSION_LOG_WARNING,
           "ForestDB rollback failed on vbucket: %d and rollback "
           "sequence number: %" PRIu64 "with error: %s",
           vbid, currentSeqno, fdb_error_msg(status));
       return RollbackResult(false, 0, 0, 0);
   }

   writeHandleMap[vbid] = kvsHandle;

   /* This is currently a workaround to update the rolled back keys
    * in memory. ForestDB doesn't provide a way to retrieve the
    * rolled back keys. So, we try to retrieve the rolled back keys
    * which are cached in rollbackKeys. If the key isn't available,
    * it is deleted in memory, otherwise the the original value is
    * restored. */

   for (auto it = rollbackKeys.begin(); it != rollbackKeys.end(); ++it) {
       RememberingCallback<GetValue> gcb;
       get(*it, vbid, gcb);
       if (gcb.val.getStatus() == ENGINE_KEY_ENOENT) {
           Item *itm = new Item(*it, vbid, queue_op_del, 0, 0);
           gcb.val.setValue(itm);
       }
       cb->callback(gcb.val);
   }

   status = fdb_get_kvs_info(kvsHandle, &kvsInfo);
   if (status != FDB_RESULT_SUCCESS) {
       LOG(EXTENSION_LOG_WARNING,
           "Failed to retrieve KV store info after rollback with error: %s "
           "for vbucket id: %d and rollback sequence number: %" PRIu64,
           fdb_error_msg(status), vbid, rollbackSeqno);
       return RollbackResult(false, 0, 0 ,0);
   }

   readVBState(vbid);

   cachedDocCount[vbid] = kvsInfo.doc_count;
   cachedDeleteCount[vbid] = kvsInfo.deleted_count;

   vbucket_state *vb_state = cachedVBStates[vbid];
   return RollbackResult(true, vb_state->highSeqno,
                         vb_state->lastSnapStart, vb_state->lastSnapEnd);
}
