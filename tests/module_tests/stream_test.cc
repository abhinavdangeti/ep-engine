/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2016 Couchbase, Inc
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

#include "assert.h"
#include "checkpoint.h"
#include "dcp-producer.h"
#include "dcp-stream.h"
#include "vbucket.h"

void takeover_send_test() {
    EPStats global_stats;
    CheckpointConfig checkpoint_config;
    EventuallyPersistentEngine *engine = new EventuallyPersistentEngine(NULL);
    RCPtr<VBucket> vb(new VBucket(0, vbucket_state_active, global_stats,
                                  checkpoint_config, NULL, 0, 0, 0, NULL));

    std::string name = "takeover_send_test";
    dcp_producer_t p(new DcpProducer(*engine, NULL, name, false));
    ActiveStream *as = new ActiveStream(NULL, p, name,
                                        0/*flags*/, 0/*opaque*/, 0/*vb*/,
                                        0/*st_seqno*/, 100/*en_seqno*/,
                                        0/*vb_uuid*/, 0/*snap_start*/,
                                        0/*snap_end*/, NULL);

    // Register checkpoint cursor for active stream
    vb->checkpointManager.registerTAPCursorBySeqno(name, 0);

    for (int i = 0; i < 2; ++i) {
        std::stringstream key;
        key << "key-" << i;
        queued_item qi(new Item(key.str(), vb->getId(), queue_op_set, 0, 0));
        vb->checkpointManager.queueDirty(vb, qi, true);
    }

    delete as;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    takeover_send_test();
    return 0;
}
