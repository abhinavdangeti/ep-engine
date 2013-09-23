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

#include <cassert>
#include <limits>
#include <string>

#include "stored-value.h"

#ifndef DEFAULT_HT_SIZE
#define DEFAULT_HT_SIZE 1531
#endif

size_t HashTable::defaultNumBuckets = DEFAULT_HT_SIZE;
size_t HashTable::defaultNumLocks = 193;
double StoredValue::mutation_mem_threshold = 0.9;
const int64_t StoredValue::state_cleared = -1;
const int64_t StoredValue::state_pending = -2;
const int64_t StoredValue::state_deleted_key = -3;
const int64_t StoredValue::state_non_existent_key = -4;
const int64_t StoredValue::state_temp_init = -5;

static ssize_t prime_size_table[] = {
    3, 7, 13, 23, 47, 97, 193, 383, 769, 1531, 3067, 6143, 12289, 24571, 49157,
    98299, 196613, 393209, 786433, 1572869, 3145721, 6291449, 12582917,
    25165813, 50331653, 100663291, 201326611, 402653189, 805306357,
    1610612741, -1
};

void StoredValue::referenced() {
    if (_nru > MIN_NRU_VALUE) {
        --_nru;
    }
}

void StoredValue::setNRUValue(uint8_t nru_val) {
    if (nru_val <= MAX_NRU_VALUE) {
        _nru = nru_val;
    }
}

uint8_t StoredValue::incrNRUValue() {
    uint8_t ret = MAX_NRU_VALUE;
    if (_nru < MAX_NRU_VALUE) {
        ret = ++_nru;
    }
    return ret;
}

uint8_t StoredValue::getNRUValue() {
    return _nru;
}

bool HashTable::unlocked_ejectItem(StoredValue*& vptr,
                                   item_eviction_policy_t policy) {
    assert(vptr);

    bool canEvict = vptr->eligibleForEviction(policy);
    if (canEvict && policy == VALUE_ONLY) {
        StoredValue::reduceCacheSize(*this, vptr->valuelen());
        vptr->resetValue(false);
        ++stats.numValueEjects;
        ++numNonResidentItems;
        ++numEjects;
        return true;
    } else if (canEvict && policy == METADATA_AND_VALUE) {
        // Note that even if an item is partially resident (i.e., only metadata is resident),
        // we evict the metadata from memory.
        StoredValue::reduceMetaDataSize(*this, stats, vptr->metaDataSize());
        StoredValue::reduceCacheSize(*this, vptr->size());

        StoredValue *new_ptr = NULL;
        int bucket_num = getBucketForHash(hash(vptr->getKey()));
        StoredValue *curr_ptr = values[bucket_num];
        if (curr_ptr == vptr) {
            StoredValue *tmp_ptr = NULL;
            std::memcpy(&tmp_ptr, vptr->bdata, sizeof(StoredValue *));
            // Replace a resident item with a non-resident item.
            new_ptr = valFact(vptr->getKey(), tmp_ptr, *this);
            values[bucket_num] = new_ptr;
        } else {
            StoredValue *next_ptr = NULL;
            std::memcpy(&next_ptr, curr_ptr->bdata, sizeof(StoredValue *));
            while (next_ptr) {
                if (next_ptr == vptr) {
                    StoredValue *tmp_ptr = NULL;
                    std::memcpy(&tmp_ptr, vptr->bdata, sizeof(StoredValue *));
                    // Replace a resident item with a non-resident item.
                    new_ptr = valFact(vptr->getKey(), tmp_ptr, *this);
                    std::memcpy(curr_ptr->bdata, &new_ptr, sizeof(StoredValue *));
                    break;
                } else {
                    curr_ptr = next_ptr;
                    std::memcpy(&next_ptr, next_ptr->bdata, sizeof(StoredValue *));
                }
            }
        }
        assert(new_ptr);

        if (vptr->isResident()) { // both metadata and value are resident.
            ++numNonResidentItems;
        }
        if (vptr->isValueResident()) {
            ++stats.numValueEjects;
        }
        delete vptr; // Free the old item.
        ++numEjects;

        vptr = new_ptr;
        return true;
    } else {
        ++stats.numFailedEjects;
        return false;
    }
}

bool HashTable::unlocked_restoreItem(StoredValue*& vptr, Item *itm) {
    assert(vptr);

    if (vptr->isResident() || vptr->isDeleted()) {
        return false;
    } else  if (vptr->isMetaDataResident()) {
        uint64_t cas = vptr->getCas();
        if (cas > 0 && cas != itm->getCas()) {
            return false;
        }
    }

    StoredValue::reduceMetaDataSize(*this, stats, vptr->metaDataSize());
    StoredValue::reduceCacheSize(*this, vptr->size());

    StoredValue *new_ptr = NULL;
    int bucket_num = getBucketForHash(hash(vptr->getKey()));
    StoredValue *curr_ptr = values[bucket_num];
    if (curr_ptr == vptr) {
        StoredValue *tmp_ptr = NULL;
        std::memcpy(&tmp_ptr, vptr->bdata, sizeof(StoredValue *));
        // Replace a non-resident item with a resident item.
        new_ptr = valFact(*itm, tmp_ptr, *this, true, false);
        values[bucket_num] = new_ptr;
    } else {
        StoredValue *next_ptr = NULL;
        std::memcpy(&next_ptr, curr_ptr->bdata, sizeof(StoredValue *));
        while (next_ptr) {
            if (next_ptr == vptr) {
                StoredValue *tmp_ptr = NULL;
                std::memcpy(&tmp_ptr, vptr->bdata, sizeof(StoredValue *));
                // Replace a non-resident item with a resident item.
                new_ptr = valFact(*itm, tmp_ptr, *this, true, false);
                std::memcpy(curr_ptr->bdata, &new_ptr, sizeof(StoredValue *));
                break;
            } else {
                curr_ptr = next_ptr;
                std::memcpy(&next_ptr, next_ptr->bdata, sizeof(StoredValue *));
            }
        }
    }
    assert(new_ptr);
    --numNonResidentItems;
    delete vptr; // Free the non-resident item.
    vptr = new_ptr;
    return true;
}

bool HashTable::unlocked_restoreItemMeta(StoredValue*& vptr, const Item *itm,
                                         ENGINE_ERROR_CODE status) {
    assert(vptr);

    if (vptr->isMetaDataResident()) {
        if (!vptr->isTempItem()) {
            return true; // An regular item's metadata is already restored.
        } else if (!vptr->isTempInitialItem()) {
            return true;
        }
    }

    switch(status) {
    case ENGINE_SUCCESS:
        {
            if (vptr->isTempInitialItem()) {
                vptr->setCas(itm->getCas());
                vptr->setRevSeqno(itm->getRevSeqno());
                vptr->setBySeqno(itm->getBySeqno());
                vptr->setExptime(itm->getExptime());
                vptr->setFlags(itm->getFlags());
                vptr->setStoredValueState(StoredValue::state_deleted_key);
                return true;
            }

            StoredValue::reduceMetaDataSize(*this, stats, vptr->metaDataSize());
            StoredValue::reduceCacheSize(*this, vptr->size());

            StoredValue *new_ptr = NULL;
            int bucket_num = getBucketForHash(hash(vptr->getKey()));
            StoredValue *curr_ptr = values[bucket_num];
            if (curr_ptr == vptr) {
                StoredValue *tmp_ptr = NULL;
                std::memcpy(&tmp_ptr, vptr->bdata, sizeof(StoredValue *));
                // Replace a non-resident item with a non-resident item whose
                // meta data is only resident.
                new_ptr = valFact(*itm, tmp_ptr, *this, false, false);
                values[bucket_num] = new_ptr;
            } else {
                StoredValue *next_ptr = NULL;
                std::memcpy(&next_ptr, curr_ptr->bdata, sizeof(StoredValue *));
                while (next_ptr) {
                    if (next_ptr == vptr) {
                        StoredValue *tmp_ptr = NULL;
                        std::memcpy(&tmp_ptr, vptr->bdata, sizeof(StoredValue *));
                        // Replace a non-resident item with a non-resident item
                        // whose meta data is only resident.
                        new_ptr = valFact(*itm, tmp_ptr, *this, false, false);
                        std::memcpy(curr_ptr->bdata, &new_ptr, sizeof(StoredValue *));
                        break;
                    } else {
                        curr_ptr = next_ptr;
                        std::memcpy(&next_ptr, next_ptr->bdata, sizeof(StoredValue *));
                    }
                }
            }
            assert(new_ptr);
            delete vptr; // Free the non-resident item.
            vptr = new_ptr;
            return true;
        }
    case ENGINE_KEY_ENOENT:
        vptr->setStoredValueState(StoredValue::state_non_existent_key);
        return true;
    default:
        LOG(EXTENSION_LOG_WARNING,
            "The underlying storage returned error %d for get_meta\n", status);
        return false;
    }
}

mutation_type_t HashTable::insert(Item &itm, item_eviction_policy_t policy,
                                  bool eject, bool partial) {
    assert(isActive());
    if (!StoredValue::hasAvailableSpace(stats, itm)) {
        return NOMEM;
    }

    if (itm.getCas() == static_cast<uint64_t>(-1)) {
        if (partial) {
            itm.setCas(0);
        } else {
            itm.setCas(Item::nextCas());
        }
    }

    int bucket_num(0);
    LockHolder lh = getLockedBucket(itm.getKey(), &bucket_num);
    StoredValue *v = unlocked_find(itm.getKey(), bucket_num, true, false);

    if (v == NULL) {
        v = valFact(itm, values[bucket_num], *this, !partial, false);
        if (partial) {
            ++numNonResidentItems;
        }
        values[bucket_num] = v;
        ++numItems;
    } else {
        if (partial) {
            // We don't have a better error code ;)
            return INVALID_CAS;
        }

        // Verify that the CAS isn't changed
        if (v->getCas() != itm.getCas()) {
            if (v->getCas() == 0) {
                v->setCas(itm.getCas());
                v->setFlags(itm.getFlags());
                v->setExptime(itm.getExptime());
                v->setRevSeqno(itm.getRevSeqno());
            } else {
                return INVALID_CAS;
            }
        }
        if (!v->isResident() && !v->isDeleted()) {
            --numNonResidentItems;
        }
        v->setValue(const_cast<Item&>(itm), *this, true);
    }

    v->markClean();

    if (eject && !partial) {
        unlocked_ejectItem(v, policy);
    }

    return NOT_FOUND;

}

static inline size_t getDefault(size_t x, size_t d) {
    return x == 0 ? d : x;
}

size_t HashTable::getNumBuckets(size_t n) {
    return getDefault(n, defaultNumBuckets);
}

size_t HashTable::getNumLocks(size_t n) {
    return getDefault(n, defaultNumLocks);
}

/**
 * Set the default number of hashtable buckets.
 */
void HashTable::setDefaultNumBuckets(size_t to) {
    if (to != 0) {
        defaultNumBuckets = to;
    }
}

/**
 * Set the default number of hashtable locks.
 */
void HashTable::setDefaultNumLocks(size_t to) {
    if (to != 0) {
        defaultNumLocks = to;
    }
}

HashTableStatVisitor HashTable::clear(bool deactivate) {
    HashTableStatVisitor rv;

    if (!deactivate) {
        // If not deactivating, assert we're already active.
        assert(isActive());
    }
    MultiLockHolder mlh(mutexes, n_locks);
    if (deactivate) {
        setActiveState(false);
    }
    for (int i = 0; i < (int)size; i++) {
        while (values[i]) {
            StoredValue *v = values[i];
            rv.visit(v);
            std::memcpy(&values[i], v->bdata, sizeof(StoredValue *));
            delete v;
        }
    }

    stats.currentSize.decr(rv.memSize - rv.valSize);
    assert(stats.currentSize.get() < GIGANTOR);

    numItems.set(0);
    numTempItems.set(0);
    numNonResidentItems.set(0);
    memSize.set(0);
    cacheSize.set(0);

    return rv;
}

void HashTable::resize(size_t newSize) {
    assert(isActive());

    // Due to the way hashing works, we can't fit anything larger than
    // an int.
    if (newSize > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return;
    }

    // Don't resize to the same size, either.
    if (newSize == size) {
        return;
    }

    MultiLockHolder mlh(mutexes, n_locks);
    if (visitors.get() > 0) {
        // Do not allow a resize while any visitors are actually
        // processing.  The next attempt will have to pick it up.  New
        // visitors cannot start doing meaningful work (we own all
        // locks at this point).
        return;
    }

    // Get a place for the new items.
    StoredValue **newValues = static_cast<StoredValue**>(calloc(newSize,
                                                                sizeof(StoredValue*)));
    // If we can't allocate memory, don't move stuff around.
    if (!newValues) {
        return;
    }

    stats.memOverhead.decr(memorySize());
    ++numResizes;

    // Set the new size so all the hashy stuff works.
    size_t oldSize = size;
    size = newSize;
    ep_sync_synchronize();

    // Move existing records into the new space.
    for (size_t i = 0; i < oldSize; i++) {
        while (values[i]) {
            StoredValue *v = values[i];
            std::memcpy(&values[i], v->bdata, sizeof(StoredValue *));

            int newBucket = getBucketForHash(hash(v->getKeyBytes(), v->getKeyLen()));
            std::memcpy(v->bdata, &newValues[newBucket], sizeof(StoredValue *));
            newValues[newBucket] = v;
        }
    }

    // values still points to the old (now empty) table.
    free(values);
    values = newValues;

    stats.memOverhead.incr(memorySize());
    assert(stats.memOverhead.get() < GIGANTOR);
}

static size_t distance(size_t a, size_t b) {
    return std::max(a, b) - std::min(a, b);
}

static size_t nearest(size_t n, size_t a, size_t b) {
    return (distance(n, a) < distance(b, n)) ? a : b;
}

static bool isCurrently(size_t size, ssize_t a, ssize_t b) {
    ssize_t current(static_cast<ssize_t>(size));
    return (current == a || current == b);
}

void HashTable::resize() {
    size_t ni = getNumItems();
    int i(0);
    size_t new_size(0);

    // Figure out where in the prime table we are.
    ssize_t target(static_cast<ssize_t>(ni));
    for (i = 0; prime_size_table[i] > 0 && prime_size_table[i] < target; ++i) {
        // Just looking...
    }

    if (prime_size_table[i] == -1) {
        // We're at the end, take the biggest
        new_size = prime_size_table[i-1];
    } else if (prime_size_table[i] < static_cast<ssize_t>(defaultNumBuckets)) {
        // Was going to be smaller than the configured ht_size.
        new_size = defaultNumBuckets;
    } else if (isCurrently(size, prime_size_table[i-1], prime_size_table[i])) {
        // If one of the candidate sizes is the current size, maintain
        // the current size in order to remain stable.
        new_size = size;
    } else {
        // Somewhere in the middle, use the one we're closer to.
        new_size = nearest(ni, prime_size_table[i-1], prime_size_table[i]);
    }

    resize(new_size);
}

void HashTable::visit(HashTableVisitor &visitor) {
    if ((numItems.get() + numTempItems.get()) == 0 || !isActive()) {
        return;
    }
    VisitorTracker vt(&visitors);
    bool aborted = !visitor.shouldContinue();
    size_t visited = 0;
    for (int l = 0; isActive() && !aborted && l < static_cast<int>(n_locks); l++) {
        LockHolder lh(mutexes[l]);
        for (int i = l; i < static_cast<int>(size); i+= n_locks) {
            assert(l == mutexForBucket(i));
            StoredValue *v = values[i];
            assert(v == NULL || i == getBucketForHash(hash(v->getKeyBytes(),
                                                           v->getKeyLen())));
            while (v) {
                visitor.visit(v);
                std::memcpy(&v, v->bdata, sizeof(StoredValue *));
            }
            ++visited;
        }
        lh.unlock();
        aborted = !visitor.shouldContinue();
    }
    assert(aborted || visited == size);
}

void HashTable::visitDepth(HashTableDepthVisitor &visitor) {
    if (numItems.get() == 0 || !isActive()) {
        return;
    }
    size_t visited = 0;
    VisitorTracker vt(&visitors);

    for (int l = 0; l < static_cast<int>(n_locks); l++) {
        LockHolder lh(mutexes[l]);
        for (int i = l; i < static_cast<int>(size); i+= n_locks) {
            size_t depth = 0;
            StoredValue *p = values[i];
            assert(p == NULL || i == getBucketForHash(hash(p->getKeyBytes(),
                                                           p->getKeyLen())));
            size_t mem(0);
            while (p) {
                depth++;
                mem += p->size();
                std::memcpy(&p, p->bdata, sizeof(StoredValue *));
            }
            visitor.visit(i, depth, mem);
            ++visited;
        }
    }

    assert(visited == size);
}

add_type_t HashTable::unlocked_add(int &bucket_num,
                                   const Item &val,
                                   item_eviction_policy_t policy,
                                   bool isDirty,
                                   bool storeVal) {
    StoredValue *v = unlocked_find(val.getKey(), bucket_num,
                                   true, false);

    if (v && !v->isMetaDataResident()) {
        // Need a bg metadata fetch to check if an item is expired or not.
        return ADD_NEED_META_FETCH;
    }

    add_type_t rv = ADD_SUCCESS;
    if (v && !v->isDeleted() && !v->isExpired(ep_real_time())) {
        rv = ADD_EXISTS;
    } else {
        Item &itm = const_cast<Item&>(val);
        itm.setCas();
        if (!StoredValue::hasAvailableSpace(stats, itm)) {
            return ADD_NOMEM;
        }
        if (v) {
            rv = (v->isDeleted() || v->isExpired(ep_real_time())) ? ADD_UNDEL : ADD_SUCCESS;
            v->setValue(itm, *this, false);
            if (isDirty) {
                v->markDirty();
            } else {
                v->markClean();
            }
        } else {
            v = valFact(itm, values[bucket_num], *this, true, isDirty);
            values[bucket_num] = v;

            if (v->isTempItem()) {
                ++numTempItems;
            } else {
                ++numItems;
            }

            /**
             * Possibly, this item is being recreated. Conservatively assign
             * it a seqno that is greater than the greatest seqno of all
             * deleted items seen so far.
             */
            uint64_t seqno = getMaxDeletedRevSeqno() + 1;
            v->setRevSeqno(seqno);
            itm.setRevSeqno(seqno);
        }
        if (!storeVal) {
            unlocked_ejectItem(v, policy);
        }
        if (v->isTempItem()) {
            v->resetValue();
            v->setNRUValue(MAX_NRU_VALUE);
        }
    }

    return rv;
}

add_type_t HashTable::unlocked_addTempDeletedItem(int &bucket_num,
                                                  const std::string &key,
                                                  item_eviction_policy_t policy) {

    assert(isActive());
    Item itm(key.c_str(), key.length(), (size_t)0, (uint32_t)0, (time_t)0,
             0, StoredValue::state_temp_init);

    // if a temp item for a possibly deleted, set it non-resident by resetting
    // the value cuz normally a new item added is considered resident which does
    // not apply for temp item.

    return unlocked_add(bucket_num, itm, policy,
                        false,  // isDirty
                        true);   // storeVal
}

void StoredValue::setMutationMemoryThreshold(double memThreshold) {
    if (memThreshold > 0.0 && memThreshold <= 1.0) {
        mutation_mem_threshold = memThreshold;
    }
}

void StoredValue::increaseCacheSize(HashTable &ht, size_t by) {
    ht.cacheSize.incr(by);
    assert(ht.cacheSize.get() < GIGANTOR);
    ht.memSize.incr(by);
    assert(ht.memSize.get() < GIGANTOR);
}

void StoredValue::reduceCacheSize(HashTable &ht, size_t by) {
    ht.cacheSize.decr(by);
    assert(ht.cacheSize.get() < GIGANTOR);
    ht.memSize.decr(by);
    assert(ht.memSize.get() < GIGANTOR);
}

void StoredValue::increaseMetaDataSize(HashTable &ht, EPStats &st, size_t by) {
    ht.metaDataMemory.incr(by);
    assert(ht.metaDataMemory.get() < GIGANTOR);
    st.currentSize.incr(by);
    assert(st.currentSize.get() < GIGANTOR);
}

void StoredValue::reduceMetaDataSize(HashTable &ht, EPStats &st, size_t by) {
    ht.metaDataMemory.decr(by);
    assert(ht.metaDataMemory.get() < GIGANTOR);
    st.currentSize.decr(by);
    assert(st.currentSize.get() < GIGANTOR);
}

/**
 * Is there enough space for this thing?
 */
bool StoredValue::hasAvailableSpace(EPStats &st, const Item &itm) {
    // Minimum memory overhead for a non-resident item
    size_t min_mem = sizeof(StoredValue) - sizeof(uint8_t) + sizeof(StoredValue *) +
        sizeof(uint8_t) + itm.getNKey();
    double newSize = static_cast<double>(st.getTotalMemoryUsed() + min_mem);
    double maxSize=  static_cast<double>(st.getMaxDataSize()) * mutation_mem_threshold;
    return newSize <= maxSize;
}

Item* StoredValue::toItem(bool lck, uint16_t vbucket) const {
    return new Item(getKey(), getFlags(), getExptime(),
                    getValue(),
                    lck ? static_cast<uint64_t>(-1) : getCas(),
                    getBySeqno(), vbucket, getRevSeqno());
}
