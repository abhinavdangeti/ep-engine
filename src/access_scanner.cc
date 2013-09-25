/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2012 Couchbase, Inc
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

#include <iostream>

#include "access_scanner.h"
#include "ep_engine.h"
#include "iomanager/iomanager.h"

class ItemAccessVisitor : public VBucketVisitor {
public:
    ItemAccessVisitor(EventuallyPersistentStore &_store, EPStats &_stats,
                      bool *sfin) :
        store(_store), stats(_stats), startTime(ep_real_time()),
        stateFinalizer(sfin)
    {
        Configuration &conf = store.getEPEngine().getConfiguration();
        name = conf.getAlogPath();
        prev = name + ".old";
        next = name + ".next";

        log = new MutationLog(next, conf.getAlogBlockSize());
        assert(log != NULL);
        log->open();
        if (!log->isOpen()) {
            LOG(EXTENSION_LOG_WARNING, "FATAL: Failed to open access log: %s",
                next.c_str());
            delete log;
            log = NULL;
        }
    }

    void visit(StoredValue *v) {
        if (log != NULL && v->isResident()) {
            if (v->isExpired(startTime) || v->isDeleted()) {
                LOG(EXTENSION_LOG_INFO, "INFO: Skipping expired/deleted item: %s",
                    v->getKey().c_str());
            } else {
                log->newItem(currentBucket->getId(), v->getKey(),
                             v->getBySeqno());
            }
        }
    }

    bool visitBucket(RCPtr<VBucket> &vb) {
        if (log == NULL) {
            return false;
        }

        return VBucketVisitor::visitBucket(vb);
    }

    virtual void complete() {
        if (stateFinalizer) {
            *stateFinalizer = true;
        }

        if (log != NULL) {
            size_t num_items = log->itemsLogged[ML_NEW];
            log->commit1();
            log->commit2();
            delete log;
            log = NULL;
            ++stats.alogRuns;
            stats.alogRuntime.set(ep_real_time() - startTime);
            stats.alogNumItems.set(num_items);

            if (num_items == 0) {
                LOG(EXTENSION_LOG_INFO, "The new access log is empty. "
                    "Delete it without replacing the current access log...\n");
                remove(next.c_str());
                return;
            }

            if (access(prev.c_str(), F_OK) == 0 && remove(prev.c_str()) == -1) {
                LOG(EXTENSION_LOG_WARNING, "FATAL: Failed to remove '%s': %s",
                    prev.c_str(), strerror(errno));
                remove(next.c_str());
            } else if (access(name.c_str(), F_OK) == 0 && rename(name.c_str(), prev.c_str()) == -1) {
                LOG(EXTENSION_LOG_WARNING, "FATAL: Failed to rename '%s' to '%s': %s",
                    name.c_str(), prev.c_str(), strerror(errno));
                remove(next.c_str());
            } else if (rename(next.c_str(), name.c_str()) == -1) {
                LOG(EXTENSION_LOG_WARNING, "FATAL: Failed to rename '%s' to '%s': %s",
                    next.c_str(), name.c_str(), strerror(errno));
                remove(next.c_str());
            }
        }
    }

private:
    EventuallyPersistentStore &store;
    EPStats &stats;
    rel_time_t startTime;
    std::string prev;
    std::string next;
    std::string name;

    MutationLog *log;
    bool *stateFinalizer;
};

bool AccessScanner::run() {
    if (available) {
        available = false;
        shared_ptr<ItemAccessVisitor> pv(new ItemAccessVisitor(store, stats, &available));
        store.resetAccessScannerTasktime();
        shared_ptr<VBucketVisitor> vbv(pv);
        IOManager::get()->scheduleVBucketVisitor(&store, vbv, "Item Access Scanner",
                                                 sleepTime, true, true);
    }
    snooze(sleepTime, false);
    stats.alogTime.set(waketime.tv_sec);
    return true;
}

std::string AccessScanner::getDescription() {
    return std::string("Generating access log");
}

size_t AccessScanner::startTime() {
    Configuration &cfg = store.getEPEngine().getConfiguration();
    return cfg.getAlogTaskTime();
}
