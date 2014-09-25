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

#ifndef SRC_BLOOMFILTER_H_
#define SRC_BLOOMFILTER_H_ 1

#include "config.h"
#include "common.h"
#include "locks.h"

/**
 * A bloom filter instance for a vbucket.
 * We are to maintain the vbucket-number of these instances.
 *
 * Each vbucket will hold one such object.
 */
class BloomFilter {
public:
    BloomFilter(uint32_t key_count, double false_positive_prob) {
        filterSize = estimateFilterSize(key_count, false_positive_prob);
        no_of_hashes = estimateNoOfHashes(filterSize, key_count);
        enabled = false;
        initializeFilter();
    }

    int estimateFilterSize(uint32_t key_count, double false_positive_prob) {
        return round(-(((double)(key_count) * log(false_positive_prob))
                                                        / (pow(log(2), 2))));
    }

    int estimateNoOfHashes(int size, uint32_t key_count) {
        return round(((double) filterSize / key_count) * (log (2)));
    }

    void initializeFilter() {
        for (int i = 0; i < filterSize; i++) {
            bitArray.push_back(0);
        }
    }

    void replaceFilter(int newKeyCount, double false_positive_prob);
    void enableFilter();
    bool isFilterEnabled();
    void addKeyToFilter(const char *key, uint32_t keylen);
    bool doesKeyExistInFilter(const char *key, uint32_t keylen);

private:
    Mutex mutex;
    int filterSize;
    int no_of_hashes;
    bool enabled;
    std::vector<bool> bitArray;
};

#endif // SRC_BLOOMFILTER_H_
