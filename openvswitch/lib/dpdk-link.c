/*
 * Copyright 2012-2014 Intel Corporation All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Interface layer to communicate with Intel DPDK vSwitch. */

#include <unistd.h>
#include <assert.h>
#include <sys/syscall.h>
#include <stdbool.h>
#include <config.h>

#include <rte_config.h>
#include <rte_mbuf.h>
#include <rte_memcpy.h>
#include <rte_lcore.h>
#include <rte_string_fns.h>

#include "datapath/dpdk/ovdk_datapath_messages.h"
#include "dpdk-link.h"
#include "vlog.h"

VLOG_DEFINE_THIS_MODULE(dpdk_link);

#define PKT_BURST_SIZE 256

#ifdef PG_DEBUG
#define DPDK_DEBUG() printf("DPDK-LINK.c %s Line %d\n", __FUNCTION__, __LINE__);
#else
#define DPDK_DEBUG()
#endif

#define enqueue_packet_mbufs_to_be_freed(mbufs, num_mbufs, pipeline_id) { \
    while (rte_ring_mp_enqueue_bulk(packet_free_ring[pipeline_id], mbufs, num_mbufs) == -ENOBUFS); \
}

#define enqueue_packet_mbuf_to_be_freed(mbuf, pipeline_id) {                \
    while (rte_ring_mp_enqueue(packet_free_ring[pipeline_id], mbuf) == -ENOBUFS);  \
}

#define enqueue_control_mbufs_to_be_freed(mbufs, num_mbufs, pipeline_id) {  \
    while (rte_ring_mp_enqueue_bulk(control_free_ring[pipeline_id], mbufs,  \
           num_mbufs) == -ENOBUFS); \
}

#define enqueue_control_mbuf_to_be_freed(mbuf, pipeline_id) {               \
    while (rte_ring_mp_enqueue(control_free_ring[pipeline_id], mbuf) == -ENOBUFS); \
}

#define alloc_packet_mbufs(packet_mbufs, num_mbufs, pipeline_id) {          \
    while (rte_ring_mc_dequeue_bulk(packet_alloc_ring[pipeline_id],         \
           packet_mbufs, num_mbufs) != 0); \
}

#define alloc_control_mbufs(control_mbufs, num_mbufs, pipeline_id) {        \
    while (rte_ring_mc_dequeue_bulk(control_alloc_ring[pipeline_id],        \
           control_mbufs, num_mbufs) != 0); \
}

static struct rte_ring *
ring_lookup(const char *template, unsigned lcore_id);

static struct rte_ring *request_ring[RTE_MAX_LCORE] = {0};
static struct rte_ring *reply_ring[RTE_MAX_LCORE] = {0};
static struct rte_ring *exception_ring[RTE_MAX_LCORE] = {0};
static struct rte_ring *packet_ring[RTE_MAX_LCORE] = {0};
static struct rte_ring *packet_free_ring[RTE_MAX_LCORE] = {0};
static struct rte_ring *control_free_ring[RTE_MAX_LCORE] = {0};
static struct rte_ring *packet_alloc_ring[RTE_MAX_LCORE] = {0};
static struct rte_ring *control_alloc_ring[RTE_MAX_LCORE] = {0};


/* Sends 'packet' and 'request' data to datapath. */
int
dpdk_link_send(struct ovdk_message *request,
               const struct ofpbuf *const packet, unsigned pipeline_id)
{
    return dpdk_link_send_bulk(request, &packet, 1, pipeline_id);
}

/* Sends 'num_pkts' 'packets' and 'request' data to datapath. */
int
dpdk_link_send_bulk(struct ovdk_message *requests,
                    const struct ofpbuf *const *packets,
                    size_t num_pkts,
                    unsigned pipeline_id)

{
    struct rte_mbuf *packet_mbufs[PKT_BURST_SIZE] = {0};
    struct rte_mbuf *control_mbufs[PKT_BURST_SIZE] = {0};
    unsigned num_control_pkts = 0;
    unsigned num_packet_pkts = 0;
    uint8_t *mbuf_data = NULL;
    int i = 0;
    int control_ret = 0;
    int packet_ret = 0;
    uint32_t tid = 0;

    DPDK_DEBUG()

    if (num_pkts > PKT_BURST_SIZE) {
        VLOG_ERR("%s: %zd exceeds the permitted packet burst size of"
                 "%d\n", __FUNCTION__, num_pkts, PKT_BURST_SIZE);
        return EINVAL;
    }

    for (i = 0; i < num_pkts; i++) {
        if (requests[i].type == OVDK_PACKET_CMD_FAMILY)
            num_packet_pkts++;
    }

    num_control_pkts = num_pkts - num_packet_pkts;

    if (num_packet_pkts) {
        alloc_packet_mbufs((void **)packet_mbufs, num_packet_pkts, pipeline_id);
        VLOG_DBG("%s: allocated %u 'packet' mbufs from ring %s\n",
                __FUNCTION__, num_packet_pkts,
                packet_alloc_ring[pipeline_id]->name);
    }
    if (num_control_pkts) {
        alloc_control_mbufs((void **)control_mbufs, num_control_pkts, pipeline_id);
        VLOG_DBG("%s: allocated %u 'control' mbufs from ring %s\n",
                __FUNCTION__, num_control_pkts,
                control_alloc_ring[pipeline_id]->name);
    }

    /* Get thread id to ensure reply is handled by the same thread */
    tid = (uint32_t)syscall(SYS_gettid);

    num_packet_pkts = 0;
    num_control_pkts = 0;
    for (i = 0; i < num_pkts; i++) {
        if (requests[i].type == OVDK_FLOW_CMD_FAMILY) {
            requests[i].flow_msg.thread_id = tid;
            control_mbufs[num_control_pkts]->pkt.nb_segs = 1;
            mbuf_data = rte_ctrlmbuf_data(control_mbufs[num_control_pkts]);
            rte_memcpy(mbuf_data, &requests[i], sizeof(requests[i]));
            num_control_pkts++;
        } else if (requests[i].type == OVDK_VPORT_CMD_FAMILY) {
            requests[i].vport_msg.thread_id = tid;
            control_mbufs[num_control_pkts]->pkt.nb_segs = 1;
            mbuf_data = rte_ctrlmbuf_data(control_mbufs[num_control_pkts]);
            rte_memcpy(mbuf_data, &requests[i], sizeof(requests[i]));
            num_control_pkts++;
        } else if (requests[i].type == OVDK_PACKET_CMD_FAMILY) {
            packet_mbufs[num_packet_pkts]->pkt.nb_segs = 1;
            mbuf_data = rte_pktmbuf_mtod(packet_mbufs[num_packet_pkts], uint8_t *);
            rte_memcpy(mbuf_data, &requests[i], sizeof(requests[i]));
            mbuf_data = mbuf_data + sizeof(requests[i]);

            if (likely(packets[i]->size <= (packet_mbufs[num_packet_pkts]->buf_len - sizeof(requests[i])))) {
                rte_memcpy(mbuf_data, packets[i]->data, packets[i]->size);
                rte_pktmbuf_data_len(packet_mbufs[num_packet_pkts]) = sizeof(requests[i])
                        + packets[i]->size;
                rte_pktmbuf_pkt_len(packet_mbufs[num_packet_pkts]) = rte_pktmbuf_data_len(packet_mbufs[num_packet_pkts]);
            } else {
                VLOG_ERR("%s, %d: %s", __FUNCTION__, __LINE__,
                         "memcpy prevented: packet size exceeds available mbuf space");
                enqueue_packet_mbufs_to_be_freed((void * const *)packet_mbufs, num_pkts, pipeline_id);
                return ENOMEM;
            }
            num_packet_pkts++;
        }
    }

    if (num_packet_pkts) {
        packet_ret = rte_ring_mp_enqueue_bulk(packet_ring[pipeline_id], (void * const *)packet_mbufs, num_packet_pkts);
        if (packet_ret == -ENOBUFS) {
            VLOG_DBG("%s: Could not enqueue %u 'packet' mbufs to ring %s;\n"
                     "enqueuing these packets to ring %s to be freed\n",
                     __FUNCTION__, num_packet_pkts,
                     packet_ring[pipeline_id]->name,
                     packet_free_ring[pipeline_id]->name);
            enqueue_packet_mbufs_to_be_freed((void * const *)packet_mbufs, num_packet_pkts, pipeline_id);
            packet_ret = ENOBUFS;
        } else if (unlikely(packet_ret == -EDQUOT)) {
            /* do not return this error code to the caller */
            packet_ret = 0;
        }
    }

    if (num_control_pkts) {
        control_ret = rte_ring_mp_enqueue_bulk(request_ring[pipeline_id], (void * const *)control_mbufs, num_control_pkts);
        if (control_ret == -ENOBUFS) {
            VLOG_DBG("%s: Could not enqueue %u 'control' mbufs to ring %s;\n"
                     "enqueuing these packets to ring %s to be freed\n",
                     __FUNCTION__, num_control_pkts,
                     request_ring[pipeline_id]->name,
                     control_free_ring[pipeline_id]->name);
            enqueue_control_mbufs_to_be_freed((void * const *)control_mbufs, num_control_pkts, pipeline_id);
            control_ret = ENOBUFS;
        } else if (unlikely(control_ret == -EDQUOT)) {
            /* do not return this error code to the caller */
            control_ret = 0;
        }
    }

    if (packet_ret != 0)
        return packet_ret;

    if (control_ret != 0)
        return control_ret;

    return 0;
}

/* Blocking function that waits for 'reply' from datapath. */
int
dpdk_link_recv_reply(struct ovdk_message *reply, unsigned pipeline_id)
{
    struct rte_mbuf *mbuf = NULL;
    void *ctrlmbuf_data = NULL;
    uint32_t tid = 0;
    int error = 0;

    bool loop = true;

    DPDK_DEBUG()

    /* Get thread id to ensure reply is received by sending thread */
    tid = (uint32_t)syscall(SYS_gettid);

    while(loop) {
        while (rte_ring_mc_dequeue(reply_ring[pipeline_id], (void **)&mbuf) != 0)
        ;
        ctrlmbuf_data = rte_ctrlmbuf_data(mbuf);

        switch(((struct ovdk_message *)ctrlmbuf_data)->type) {
        case OVDK_VPORT_CMD_FAMILY:
            /* Allow multi-threading */
            if (((struct ovdk_message *)ctrlmbuf_data)->vport_msg.thread_id != tid ){
                /* Don't touch other processes' packets - re-enqueue back onto
                 * reply ring. If reply ring is full, keep trying until packet
                 * has been succssfully enqueued
                 */
                while (rte_ring_mp_enqueue(reply_ring[pipeline_id], (void *)mbuf) == -ENOBUFS)
                    ;
            } else {
                loop = false;
            }
            break;
        case OVDK_FLOW_CMD_FAMILY:
            /* Allow multi-threading */
            if (((struct ovdk_message *)ctrlmbuf_data)->flow_msg.thread_id != tid ){
                /* Don't touch other processes' packets - re-enqueue */
                while (rte_ring_mp_enqueue(reply_ring[pipeline_id], (void *)mbuf) == -ENOBUFS)
                    ;
            } else {
               loop = false;
            }
            break;
        default:
            VLOG_WARN("invalid reply type\n");
            loop = false;
            error = EPERM;
            break;
        }
    }

    rte_memcpy(reply, ctrlmbuf_data, sizeof(struct ovdk_message));

    enqueue_control_mbuf_to_be_freed(mbuf, pipeline_id);

    return error;
}

/* Blocking function that waits for a packet from datapath. 'pkt' will get
 * populated with packet data. */
int
dpdk_link_recv_packet(struct ofpbuf *pkt, struct ovdk_upcall *info, unsigned pipeline_id)
{
    struct rte_mbuf *mbuf = NULL;
    uint16_t pktmbuf_len = 0;
    void *pktmbuf_data = NULL;

    DPDK_DEBUG()

    if (rte_ring_mc_dequeue(exception_ring[pipeline_id], (void **)&mbuf) != 0) {
        return EAGAIN;
    }

    pktmbuf_data = rte_pktmbuf_mtod(mbuf, void *);
    pktmbuf_len = rte_pktmbuf_data_len(mbuf);
    rte_memcpy(info, pktmbuf_data, sizeof(*info));
    pktmbuf_data = (uint8_t *)pktmbuf_data + sizeof(*info);
    ofpbuf_put(pkt, pktmbuf_data, pktmbuf_len - sizeof(*info));

    enqueue_packet_mbuf_to_be_freed(mbuf, pipeline_id);

    return 0;
}

/* Initialize DPDK link layer.
 *
 * No need to free any memory on shutdown as memory is owned by datapath.
 */
int
dpdk_link_init(uint64_t *mask)
{
    DPDK_DEBUG()
    int i = 0;
    int error = 0;

    /* Loop through every possible core and check if the eight expected rings
     * exist. If any of these ring lookups fail, we do not enable that core
     * in 'mask'. Otherwise, we enable it.
     */

    for(i = 0; i < RTE_MAX_LCORE; i++) {
        reply_ring[i] = ring_lookup(VSWITCHD_REPLY_RING_NAME, i);
        if (reply_ring[i] == NULL) {
            continue;
        }

        request_ring[i] = ring_lookup(VSWITCHD_REQUEST_RING_NAME, i);
        if (request_ring[i] == NULL) {
            continue;
        }

        exception_ring[i] = ring_lookup(VSWITCHD_EXCEPTION_RING_NAME, i);
        if (exception_ring[i] == NULL) {
            continue;
        }

        packet_ring[i] = ring_lookup(VSWITCHD_PACKET_RING_NAME, i);
        if (packet_ring[i] == NULL) {
            continue;
        }

        packet_free_ring[i] = ring_lookup(VSWITCHD_PACKET_FREE_RING_NAME, i);
        if (packet_free_ring[i] == NULL) {
            continue;
        }

        packet_alloc_ring[i] = ring_lookup(VSWITCHD_PACKET_ALLOC_RING_NAME, i);
        if (packet_alloc_ring[i] == NULL) {
            continue;
        }

        control_free_ring[i] = ring_lookup(VSWITCHD_CONTROL_FREE_RING_NAME, i);
        if (control_free_ring[i] == NULL) {
            continue;
        }

        control_alloc_ring[i] = ring_lookup(VSWITCHD_CONTROL_ALLOC_RING_NAME, i);
        if (control_alloc_ring[i] == NULL) {
            continue;
        }

        /* If all rings are non-NULL, this particular core is enabled in the
         * bitmask
         */
        *mask |= 1LLU << i;
    }

    /* If no rings have been found, we have encountered an error */
    if (*mask == 0) {
        error = 1;
    }

    return error;
}

/* lookup an rte_ring using a known template */
static struct rte_ring *
ring_lookup(const char *template, unsigned lcore_id)
{
    struct rte_ring *ring = NULL;
    char ring_name[OVDK_MAX_NAME_SIZE] = {0};

    snprintf(ring_name, sizeof(ring_name), template, lcore_id);
    ring = rte_ring_lookup(ring_name);

    /* Return ring even if NULL. The NULL case is checked after all calls to
     * this function in dpdk_link_init. We use this information to deduce if
     * the core is enabled or not.
     */
    if (ring != NULL) {
        VLOG_INFO("Found %s\n", ring_name);
    }

    return ring;
}

