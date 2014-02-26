/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2010 Couchbase, Inc
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

#include "config.h"

#include <map>
#include <string>

#include "common.h"
#include "couch-kvstore/couch-kvstore.h"
#include "ep_engine.h"
#include "kvstore.h"
#include "stats.h"
#include "warmup.h"


KVStore *KVStoreFactory::create(EPStats &stats, Configuration &config,
                                bool read_only) {

    KVStore *ret = NULL;
    std::string backend = config.getBackend();
    if (backend.compare("couchdb") == 0) {
        ret = new CouchKVStore(stats, config, read_only);
    } else {
        LOG(EXTENSION_LOG_WARNING, "Unknown backend: [%s]", backend.c_str());
    }

    return ret;
}

size_t KVStore::getEstimatedItemCount(std::vector<uint16_t> &vbs) {
    // Not supported
    return 0;
}

void RollbackCB::callback(GetValue &val) {
    assert(val.getValue());
    assert(dbHandle);
    Item *itm = val.getValue();
    RCPtr<VBucket> vb = engine_.getVBucket(itm->getVBucketId());
    int bucket_num(0);
    RememberingCallback<GetValue> gcb;
    engine_.getEpStore()->getROUnderlying(itm->getVBucketId())->
                                          getWithHeader(dbHandle,
                                                        itm->getKey(),
                                                        itm->getVBucketId(),
                                                        gcb);
    gcb.waitForValue();
    assert(gcb.fired);
    if (gcb.val.getStatus() == ENGINE_SUCCESS) {
        Item *it = gcb.val.getValue();
        if (it->isDeleted()) {
            LockHolder lh = vb->ht.getLockedBucket(it->getKey(),
                    &bucket_num);
            bool ret = vb->ht.unlocked_del(it->getKey(), bucket_num);
            if(!ret) {
                setStatus(ENGINE_KEY_ENOENT);
            } else {
                setStatus(ENGINE_SUCCESS);
            }
        } else {
            mutation_type_t mtype = vb->ht.set(*it, it->getCas(),
                                               true, true,
                                               engine_.getEpStore()->
                                                    getItemEvictionPolicy(),
                                               INITIAL_NRU_VALUE);
            if (mtype == NOMEM) {
                setStatus(ENGINE_ENOMEM);
            }
        }
        delete it;
    } else if (gcb.val.getStatus() == ENGINE_KEY_ENOENT) {
        LockHolder lh = vb->ht.getLockedBucket(itm->getKey(), &bucket_num);
        bool ret = vb->ht.unlocked_del(itm->getKey(), bucket_num);
        if (!ret) {
            setStatus(ENGINE_KEY_ENOENT);
        } else {
            setStatus(ENGINE_SUCCESS);
        }
    } else {
        LOG(EXTENSION_LOG_WARNING, "Unexpected Error Status: %d",
                gcb.val.getStatus());
    }
    delete itm;
}

void AllKeysCB::incrAllKeysBuffer() {
    //If buffer length is hit, increase buffer length by twice
    allKeys.buffersize *= 2;
    char *temp = (char *) malloc (allKeys.buffersize);
    memcpy (temp, allKeys.data, allKeys.len);
    free (allKeys.data);
    allKeys.data = temp;
}

void AllKeysCB::appendAllKeys(uint8_t len, char *buf) {
    if (allKeys.len + len > allKeys.buffersize) {
        incrAllKeysBuffer();
    }
    memcpy (allKeys.data + allKeys.len, &len, 2);
    memcpy (allKeys.data + allKeys.len + 2, buf, len);
    allKeys.len += (len + 2);
}

void AllKeysCB::prependAllKeys(uint8_t len, char *buf) {
    if (allKeys.len + len > allKeys.buffersize) {
        incrAllKeysBuffer();
    }
    char *temp = (char *) malloc(allKeys.len);
    memcpy (temp, allKeys.data, allKeys.len);
    memcpy (allKeys.data, &len, 2);
    memcpy (allKeys.data + 2, buf, len);
    memcpy (allKeys.data + len + 2, temp, allKeys.len);
    allKeys.len += (len + 2);
    free (temp);
}
