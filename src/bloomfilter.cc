/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2014 Couchbase, Inc.
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

#include "bloomfilter.h"
#include "murmurhash3.h"

#if __x86_64__ || __ppc64__
#define MURMURHASH_3 MurmurHash3_x64_128
#else
#define MURMURHASH_3 MurmurHash3_x86_128
#endif

BloomFilter::BloomFilter(size_t key_count, double false_positive_prob,
                         bfilter_status_t new_status) :
             filterSize(0), noOfHashes(0) {

    replace(key_count, false_positive_prob, new_status);
}

BloomFilter::~BloomFilter() {
    LockHolder lh(mutex);
    status = BFILTER_DISABLED;
    bitArray.clear();
}

size_t BloomFilter::estimateFilterSize(size_t key_count,
                                       double false_positive_prob) {
    return round(-(((double)(key_count) * log(false_positive_prob))
                                                    / (pow(log(2), 2))));
}

size_t BloomFilter::estimateNoOfHashes(size_t key_count) {
    return round(((double) filterSize / key_count) * (log (2)));
}

void BloomFilter::initialize() {
    bitArray.assign(filterSize, false);
}

void BloomFilter::replace(size_t new_key_count, double false_positive_prob,
                          bfilter_status_t new_status) {
    LockHolder lh(mutex);
    status = new_status;
    filterSize = estimateFilterSize(new_key_count, false_positive_prob);
    noOfHashes = estimateNoOfHashes(new_key_count);
    initialize();
}

void BloomFilter::setStatus(bool to) {
    LockHolder lh(mutex);
    if (to) {
        if (status == BFILTER_DISABLED) {
            status = BFILTER_PENDING;
        }
    } else {
        status = BFILTER_DISABLED;
        bitArray.clear();
    }
}

bfilter_status_t BloomFilter::getStatus() {
    LockHolder lh(mutex);
    return status;
}

std::string BloomFilter::getStatusString() {
    LockHolder lh(mutex);
    if (status == BFILTER_DISABLED) {
        return "DISABLED";
    } else if (status == BFILTER_PENDING) {
        return "PENDING (ENABLED)";
    } else if (status == BFILTER_COMPACTING) {
        return "COMPACTING";
    } else {
        return "ENABLED";
    }
}

/**
 * Only called from compactVBucket.
 */
void BloomFilter::enable() {
    LockHolder lh(mutex);
    cb_assert(status == BFILTER_COMPACTING);
    status = BFILTER_ENABLED;
}

void BloomFilter::addKey(const char *key, size_t keylen) {
    LockHolder lh(mutex);
    if (status == BFILTER_COMPACTING || status == BFILTER_ENABLED) {
        uint32_t i;
        uint64_t result;
        for (i = 0; i < noOfHashes; i++) {
            MURMURHASH_3(key, keylen, i, &result);
            bitArray[result % filterSize] = 1;
        }
    }
}

bool BloomFilter::maybeKeyExists(const char *key, uint32_t keylen) {
    LockHolder lh(mutex);
    if (status == BFILTER_ENABLED) {
        uint32_t i;
        uint64_t result;
        for (i = 0; i < noOfHashes; i++) {
            MURMURHASH_3(key, keylen, i, &result);
            if (bitArray[result % filterSize] == 0) {
                // The key does NOT exist.
                return false;
            }
        }
    }
    // The key may exist.
    return true;
}
