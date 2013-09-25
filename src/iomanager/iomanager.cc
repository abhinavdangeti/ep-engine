/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2013 Couchbase, Inc.
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

#include <string>

#include "access_scanner.h"
#include "bgfetcher.h"
#include "ep_engine.h"
#include "flusher.h"
#include "iomanager/iomanager.h"
#include "tapconnection.h"
#include "warmup.h"

Mutex IOManager::initGuard;
IOManager *IOManager::instance = NULL;

IOManager *IOManager::get() {
    if (!instance) {
        LockHolder lh(initGuard);
        if (!instance) {
            Configuration &config =
                ObjectRegistry::getCurrentEngine()->getConfiguration();
            instance = new IOManager(config.getMaxIoThreads());
            (void)config;
        }
    }
    return instance;
}

size_t IOManager::scheduleFlusherTask(EventuallyPersistentEngine *engine,
                                      Flusher* flusher,
                                      const Priority &priority, uint16_t sid) {
    ExTask task = new FlusherTask(engine, flusher, priority, sid);
    flusher->setTaskId(task->getId());
    return schedule(task, WRITER_TASK_IDX);
}

size_t IOManager::scheduleVBSnapshot(EventuallyPersistentEngine *engine,
                                     const Priority &priority, uint16_t sid,
                                     int, bool isDaemon) {
    ExTask task = new VBSnapshotTask(engine, priority, sid, isDaemon);
    return schedule(task, WRITER_TASK_IDX);
}

size_t IOManager::scheduleVBDelete(EventuallyPersistentEngine *engine,
                                   const void* cookie, uint16_t vbucket,
                                   const Priority &priority, uint16_t sid,
                                   bool recreate, int sleeptime,
                                   bool isDaemon) {
    ExTask task = new VBDeleteTask(engine, vbucket, cookie, priority, sid,
                                   recreate, sleeptime, isDaemon);
    return schedule(task, WRITER_TASK_IDX);
}

size_t IOManager::scheduleStatsSnapshot(EventuallyPersistentEngine *engine,
                                        const Priority &priority, int sid,
                                        bool runOnce, int sleeptime,
                                        bool isDaemon, bool blockShutdown) {
    ExTask task = new StatSnap(engine, priority, runOnce, sleeptime,
                               isDaemon, blockShutdown);
    return schedule(task, WRITER_TASK_IDX);
}

size_t IOManager::scheduleMultiBGFetcher(EventuallyPersistentEngine *engine,
                                         BgFetcher *b, const Priority &priority,
                                         int sid, int sleeptime, bool isDaemon,
                                         bool blockShutdown) {
    ExTask task = new BgFetcherTask(engine, b, priority, sleeptime,
                                    isDaemon, blockShutdown);
    b->setTaskId(task->getId());
    return schedule(task, READER_TASK_IDX);
}

size_t IOManager::scheduleVKeyFetch(EventuallyPersistentEngine *engine,
                                    const std::string &key, uint16_t vbid,
                                    uint64_t seqNum, const void *cookie,
                                    const Priority &priority, int sid,
                                    int sleeptime, size_t delay, bool isDaemon,
                                    bool blockShutdown) {
    ExTask task = new VKeyStatBGFetchTask(engine, key, vbid, seqNum, cookie,
                                          priority, sleeptime, delay, isDaemon,
                                          blockShutdown);
    return schedule(task, READER_TASK_IDX);
}

size_t IOManager::scheduleBGFetch(EventuallyPersistentEngine *engine,
                                  const std::string &key, uint16_t vbid,
                                  uint64_t seqNum, const void *cookie,
                                  bool isMeta, const Priority &priority,
                                  int sid, int sleeptime, size_t delay,
                                  bool isDaemon, bool blockShutdown) {
    ExTask task = new BGFetchTask(engine, key, vbid, seqNum, cookie, isMeta,
                                  priority, sleeptime, delay, isDaemon,
                                  blockShutdown);
    return schedule(task, READER_TASK_IDX);
}

size_t IOManager::scheduleBackfillDiskLoad(EventuallyPersistentEngine *engine,
                                           const std::string &name,
                                           TapConnMap &tcm, KVStore *s,
                                           uint16_t vbid, backfill_t type,
                                           hrtime_t token, const Priority &p,
                                           double sleeptime, size_t delay,
                                           bool isDaemon, bool blockShutdown) {
    ExTask task = new BackfillDiskLoad(name, engine, tcm, s, vbid, type,
                                       token, p, sleeptime, delay,
                                       isDaemon, blockShutdown);
    return schedule(task, AUXIO_TASK_IDX);
}

size_t IOManager::scheduleAccessScanner(EventuallyPersistentStore &store,
                                        EPStats &st, const Priority &p,
                                        double sleeptime, size_t delay,
                                        bool isDaemon, bool blockShutdown) {
    ExTask task = new AccessScanner(store, st, p, sleeptime, delay,
                                    isDaemon, shutdown);
    return schedule(task, AUXIO_TASK_IDX);
}

size_t IOManager::scheduleVBucketVisitor(EventuallyPersistentStore *store,
                                         shared_ptr<VBucketVisitor> v,
                                         const char *l, double sleeptime,
                                         bool isDaemon, bool shutdown) {
    ExTask task = new VBucketVisitorTask(store, v, l, sleeptime,
                                         isDaemon, shutdown);
    return schedule(task, AUXIO_TASK_IDX);
}

size_t IOManager::scheduleTapBGFetchCallback(EventuallyPersistentEngine *engine,
                                             const std::string &name,
                                             const std::string &key,
                                             const Priority &p, uint16_t vbid,
                                             uint64_t rowid, hrtime_t token,
                                             double sleeptime, size_t delay,
                                             bool isDaemon, bool blockShutdown) {
    ExTask task = new TapBGFetchCallback(engine, name, key, vbid, rowid,
                                         token, p, sleeptime, delay,
                                         isDaemon, blockShutdown);
    return schedule(task, AUXIO_TASK_IDX);
}

size_t IOManager::scheduleWarmupStepper(EventuallyPersistentStore &store,
                                        Warmup *w, const Priority &p,
                                        double sleeptime, size_t delay,
                                        bool isDaemon, bool blockShutdown) {
    ExTask task = new WarmupStepper(store, w, p, sleeptime, delay,
                                    isDaemon, blockShutdown);
    return schedule(task, AUXIO_TASK_IDX);
}
