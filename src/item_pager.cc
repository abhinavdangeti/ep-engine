/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */

#include "config.h"
#include <iostream>
#include <cstdlib>
#include <utility>
#include <list>

#include "common.hh"
#include "item_pager.hh"
#include "ep.hh"
#include "ep_engine.h"

static const size_t MAX_PERSISTENCE_QUEUE_SIZE = 1000000;

/**
 * As part of the ItemPager, visit all of the objects in memory and
 * eject some within a constrained probability
 */
class PagingVisitor : public VBucketVisitor {
public:

    /**
     * Construct a PagingVisitor that will attempt to evict the given
     * percentage of objects.
     *
     * @param s the store that will handle the bulk removal
     * @param st the stats where we'll track what we've done
     * @param pcnt percentage of objects to attempt to evict (0-1)
     * @param sfin pointer to a bool to be set to true after run completes
     * @param pause flag indicating if PagingVisitor can pause between vbucket visits
     * @param bias active vbuckets eviction probability bias multiplier (0-1)
     * @param phase pointer to an item_pager_phase to be set
     */
    PagingVisitor(EventuallyPersistentStore &s, EPStats &st, double pcnt,
                  bool *sfin, bool pause = false,
                  double bias = 1, item_pager_phase *phase = NULL)
      : store(s), stats(st), percent(pcnt),
        activeBias(bias), ejected(0), totalEjected(0), totalEjectionAttempts(0),
        startTime(ep_real_time()), stateFinalizer(sfin), canPause(pause),
        completePhase(true), pager_phase(phase) {}

    void visit(StoredValue *v) {
        // Remember expired objects -- we're going to delete them.
        if ((v->isExpired(startTime) && !v->isDeleted()) || v->isTempItem()) {
            expired.push_back(std::make_pair(currentBucket->getId(), v->getKey()));
            return;
        }

        // return if not ItemPager, which uses valid eviction percentage
        if (percent <= 0 || !pager_phase) {
            return;
        }

        // always evict unreferenced items, or randomly evict referenced item
        double r = *pager_phase == PAGING_UNREFERENCED ?
            1 : static_cast<double>(std::rand()) / static_cast<double>(RAND_MAX);

        if (*pager_phase == PAGING_UNREFERENCED && v->getNRUValue() == MAX_NRU_VALUE) {
            doEviction(v);
        } else if (*pager_phase == PAGING_RANDOM && v->incrNRUValue() == MAX_NRU_VALUE &&
                   r <= percent) {
            doEviction(v);
        }
    }

    bool visitBucket(RCPtr<VBucket> &vb) {
        update();

        // fast path for expiry item pager
        if (percent <= 0 || !pager_phase) {
            return VBucketVisitor::visitBucket(vb);
        }

        // skip active vbuckets if active resident ratio is lower than replica
        double current = static_cast<double>(stats.getTotalMemoryUsed());
        double lower = static_cast<double>(stats.mem_low_wat);
        double high = static_cast<double>(stats.mem_high_wat);
        if (vb->getState() == vbucket_state_active && current < high &&
            store.cachedResidentRatio.activeRatio < store.cachedResidentRatio.replicaRatio)
        {
            return false;
        }

        if (current > lower) {
            double p = (current - static_cast<double>(lower)) / current;
            adjustPercent(p, vb->getState());
            return VBucketVisitor::visitBucket(vb);
        } else { // stop eviction whenever memory usage is below low watermark
            completePhase = false;
            return false;
        }
    }

    void update() {
        store.deleteExpiredItems(expired);

        if (numEjected() > 0) {
            LOG(EXTENSION_LOG_INFO, "Paged out %ld values", numEjected());
        }

        size_t num_expired = expired.size();
        if (num_expired > 0) {
            LOG(EXTENSION_LOG_INFO, "Purged %ld expired items", num_expired);
        }

        totalEjected += (ejected + num_expired);
        ejected = 0;
        expired.clear();
    }

    bool pauseVisitor() {
        size_t queueSize = stats.diskQueueSize.get();
        return canPause && queueSize >= MAX_PERSISTENCE_QUEUE_SIZE;
    }

    void complete() {
        update();
        if (stateFinalizer) {
            *stateFinalizer = true;
        }

        if (pager_phase && completePhase) {
            if (*pager_phase == PAGING_UNREFERENCED) {
                *pager_phase = PAGING_RANDOM;
            } else {
                *pager_phase = PAGING_UNREFERENCED;
            }
        }
    }

    /**
     * Get the number of items ejected during the visit.
     */
    size_t numEjected() { return ejected; }

    /**
     * Get the total number of items whose values are ejected or removed due to
     * the expiry time.
     */
    size_t getTotalEjected() { return totalEjected; }

    /**
     * Get the total number of ejection attempts.
     */
    size_t getTotalEjectionAttempts() { return totalEjectionAttempts; }

private:
    void adjustPercent(double prob, vbucket_state_t state) {
        if (state == vbucket_state_replica ||
            state == vbucket_state_dead)
        {
            // replica items should have higher eviction probability
            double p = prob*(2 - activeBias);
            percent = p < 0.9 ? p : 0.9;
        } else {
            // active items have lower eviction probability
            percent = prob*activeBias;
        }
    }

    void doEviction(StoredValue *v) {
        ++totalEjectionAttempts;
        if (!v->eligibleForEviction()) {
            ++stats.numFailedEjects;
            return;
        }
        // Check if the key was already visited by all the cursors.
        bool can_evict = currentBucket->checkpointManager.eligibleForEviction(v->getKey());
        if (can_evict && v->ejectValue(stats, currentBucket->ht)) {
            ++ejected;
        }
    }

    std::list<std::pair<uint16_t, std::string> > expired;

    EventuallyPersistentStore &store;
    EPStats &stats;
    double percent;
    double activeBias;
    size_t ejected;
    size_t totalEjected;
    size_t totalEjectionAttempts;
    time_t startTime;
    bool *stateFinalizer;
    bool canPause;
    bool completePhase;
    item_pager_phase *pager_phase;
};

bool ItemPager::callback(Dispatcher &d, TaskId &t) {
    double current = static_cast<double>(stats.getTotalMemoryUsed());
    double upper = static_cast<double>(stats.mem_high_wat);
    double lower = static_cast<double>(stats.mem_low_wat);
    double sleepTime = 5;
    if (available && current > upper) {
        ++stats.pagerRuns;

        double toKill = (current - static_cast<double>(lower)) / current;

        std::stringstream ss;
        ss << "Using " << stats.getTotalMemoryUsed()
           << " bytes of memory, paging out %0f%% of items." << std::endl;
        LOG(EXTENSION_LOG_INFO, ss.str().c_str(), (toKill*100.0));

        // compute active vbuckets evicition bias factor
        Configuration &cfg = store.getEPEngine().getConfiguration();
        size_t activeEvictPerc = cfg.getPagerActiveVbPcnt();
        double bias = static_cast<double>(activeEvictPerc) / 50;

        available = false;
        shared_ptr<PagingVisitor> pv(new PagingVisitor(store, stats, toKill,
                                                       &available,
                                                       false, bias, &phase));
        store.visit(pv, "Item pager", &d, Priority::ItemPagerPriority);
    }

    d.snooze(t, sleepTime);
    return true;
}

bool ExpiredItemPager::callback(Dispatcher &d, TaskId &t) {
    if (available) {
        ++stats.expiryPagerRuns;

        available = false;
        shared_ptr<PagingVisitor> pv(new PagingVisitor(store, stats, -1,
                                                       &available,
                                                       true, 1, NULL));
        store.visit(pv, "Expired item remover", &d, Priority::ItemPagerPriority,
                    true, 10);
    }
    d.snooze(t, sleepTime);
    return true;
}

bool DiskUsageTracker::callback(Dispatcher &d, TaskId &t) {
    Configuration &cfg = store.getEPEngine().getConfiguration();
    stats.diskUsage = getDiskUsage(cfg.getDbname().c_str());

    // dispatcher snoozes for 10 seconds
    d.snooze(t, 10);
    return true;
}
