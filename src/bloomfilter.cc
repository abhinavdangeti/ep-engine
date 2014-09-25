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

void BloomFilter::replaceFilter(int newKeyCount,
                                double false_positive_prob) {
    LockHolder lh(mutex);
    enabled = false;
    bitArray.clear();
    filterSize = estimateFilterSize(newKeyCount, false_positive_prob);
    no_of_hashes = estimateNoOfHashes(filterSize, newKeyCount);
    initializeFilter();
}

void BloomFilter::enableFilter() {
    LockHolder lh(mutex);
    enabled = true;
}

bool BloomFilter::isFilterEnabled() {
    LockHolder lh(mutex);
    return enabled;
}

void BloomFilter::addKeyToFilter(const char *key, uint32_t keylen) {
    LockHolder lh(mutex);
    uint32_t i;
    uint64_t result;
    for (i = 0; i < no_of_hashes; i++) {
        MURMURHASH_3(key, keylen, i, &result);
        bitArray[result % filterSize] = 1;
    }
}

bool BloomFilter::doesKeyExistInFilter(const char *key, uint32_t keylen) {
    LockHolder lh(mutex);
    uint32_t i;
    uint64_t result;
    for (i = 0; i < no_of_hashes; i++) {
        MURMURHASH_3(key, keylen, i, &result);
        if (bitArray[result % filterSize] == 0) {
            // The key does NOT exist.
            return false;
        }
    }
    // The key may exist.
    return true;
}
