/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2014 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <pthread.h>

#include <rte_config.h>
#include <rte_string_fns.h>
#include <rte_malloc.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_kni.h>

#include "rte_port_veth.h"

#define RTE_LOGTYPE_APP        RTE_LOGTYPE_USER1

#define VETH_RETRY_NUM         4
#define VETH_RETRY_DELAY_US    10

/*
 * vEth ports are implementations of "Host KNI", or Intel(R) DPDK KNI running
 * on the host.
 *
 * The code contained herein is not capable of setting up the KNI instances -
 * it merely wraps KNI such that it can be used as a packet framework port.
 */

/*
 * vEth reader port struct.
 */
struct rte_port_veth_reader {
	struct veth_dev **dev;
};

/*
 * vEth writer port struct.
 */
struct rte_port_veth_writer {
	struct veth_dev **dev;
	struct rte_mbuf *tx_buf[RTE_PORT_IN_BURST_SIZE_MAX];
	uint32_t tx_burst_sz;
	int32_t tx_burst_retry_num;    /* # of retries to make*/
	uint32_t tx_burst_delay_time;  /* time to delay during retry*/
	uint16_t tx_buf_count;         /* # of pkts in buffer */
};

/*
 * vEth reader create.
 *
 * Allocate memory on 'socket_id' for port instance. Configure same using
 * information contained in 'params'.
 *
 * Note that we don't allocate the KNI device here, as this code is executed
 * during 'pipeline_run' and must be fast. Instead, the KNI device is allocated
 * separately at some point prior to being used.
 */
static void *
rte_port_veth_reader_create(void *params, int socket_id)
{
	struct rte_port_veth_reader_params *conf;
	struct rte_port_veth_reader *port;

	/* Check input parameters */
	conf = (struct rte_port_veth_reader_params *) params;
	if (conf == NULL) {
		RTE_LOG(ERR, PORT, "Could not create vEth reader - invalid"
		        "config parameters - conf struct\n");
		return NULL;
	}

	/* Memory allocation */
	port = (struct rte_port_veth_reader *)rte_malloc_socket(
	        "PIPELINE", sizeof(*port), CACHE_LINE_SIZE, socket_id);
	if (port == NULL) {
		RTE_LOG(ERR, PORT, "Could not create vEth reader - failure"
		        "allocating memory");
		return NULL;
	}

	memset(port, 0x0, sizeof(*port));

	/* Initialization */
	port->dev = conf->dev;

	return (void *)port;
}

/*
 * vEth reader port free.
 *
 * Free the 'port' if not null.
 */
static int
rte_port_veth_reader_free(void *port)
{
	if (port == NULL)
		return -1;

	rte_free(port);

	return 0;
}

/*
 * vEth reader port receive.
 *
 * Attempt to dequeue 'n_pkts' from the KNI device 'p->dev' into 'pkts'. Once
 * dequeued, handle any requests on the KNI device and return.
 */
static int
rte_port_veth_reader_rx(void *port, struct rte_mbuf **pkts, uint32_t n_pkts)
{
	int rx_count = 0;
	struct rte_port_veth_reader *p = NULL;
	struct veth_dev *deq_dev = NULL;

	p = (struct rte_port_veth_reader *)port;
	if (unlikely(p == NULL)) {
		RTE_LOG(ERR, PORT, "vEth reader port is NULL, exiting rx"
		        " function\n");
		return 0;
	}

	deq_dev = *(p->dev);
	if (unlikely(deq_dev == NULL)) {
		RTE_LOG(ERR, PORT, "Attempting to dequeue packets from"
		        "an invalid vEth device\n");
		return 0;
	}

	/* There is a single-reader multi-writer design used for the datapath.
	 * It is not necessary to lock/unlock the mutex for reads as there is
	 * no contention. */

	/* receive and free */
	rx_count = rte_kni_rx_burst((*deq_dev).kni, pkts, n_pkts);

	/* handle callbacks (i.e. ifconfig) before we return */
	rte_kni_handle_request((*deq_dev).kni);

	return rx_count;
}

/*
 * vEth writer creation.
 *
 * Allocate memory on 'socket_id' for port instance. Configure same using
 * information contained in 'params'.
 *
 * Note that we don't allocate the KNI device here, as this code is executed
 * during 'pipeline_run' and must be fast. Instead, the KNI device is allocated
 * separately at some point prior to being used.
 */
static void *
rte_port_veth_writer_create(void *params, int socket_id)
{
	struct rte_port_veth_writer_params *conf = NULL;
	struct rte_port_veth_writer *port = NULL;

	/* Check input parameters */
	conf = (struct rte_port_veth_writer_params *)params;
	if (conf == NULL) {
		RTE_LOG(ERR, PORT, "Could not create vEth writer - invalid"
		        "config parameters - conf struct\n");
		return NULL;
	}

	/* Memory allocation */
	port = (struct rte_port_veth_writer *)rte_malloc_socket(
	        "PIPELINE", sizeof(*port), CACHE_LINE_SIZE, socket_id);
	if (port == NULL) {
		RTE_LOG(ERR, PORT, "Could not create vEth writer - failure"
		        "allocating memory");
		return NULL;
	}

	memset(port, 0x0, sizeof(*port));

	/* Initialization */
	port->dev = conf->dev;
	port->tx_burst_sz = conf->tx_burst_sz;
	port->tx_burst_retry_num = VETH_RETRY_NUM;
	port->tx_burst_delay_time = VETH_RETRY_DELAY_US;

	return (void *)port;
}

/*
 * vEth writer port free.
 *
 * Free the 'port' if not null.
 */
static int
rte_port_veth_writer_free(void *port)
{
	if (port == NULL)
		return -1;

	rte_free(port);

	return 0;
}

/*
 * vEth writer port burst transmit.
 *
 * Attempt to enqueue 'p->tx_buf_count' mbufs from 'p->tx_buf' onto the KNI
 * device 'p->dev'. Once enqueued, free said mbufs and reset the
 * 'p->tx_buf_count' to zero.
 */
static inline void
tx_burst(struct rte_port_veth_writer *p)
{
	int i = 0;
	int tx_count = 0;
	uint32_t retry = 0;
	uint32_t retry_pkts = 0;
	uint32_t count = 0;
	struct veth_dev *enq_dev = NULL;

	enq_dev = *(p->dev);
	if (likely(enq_dev != NULL)) {
		pthread_mutex_lock(&(*enq_dev).mutex);
		tx_count = rte_kni_tx_burst((*enq_dev).kni,
		                            (struct rte_mbuf**)p->tx_buf,
		                            p->tx_buf_count);
		pthread_mutex_unlock(&(*enq_dev).mutex);

		retry_pkts = p->tx_buf_count - tx_count;
		if (retry_pkts) {
			RTE_LOG(INFO, PORT, "Failed to transmit all packets on"
			        " vEth interface. Retrying...\n");

			for (; retry < p->tx_burst_retry_num; retry++) {
				rte_delay_us(p->tx_burst_delay_time);

				pthread_mutex_lock(&(*enq_dev).mutex);
				tx_count += count = rte_kni_tx_burst((*enq_dev).kni,
				        (struct rte_mbuf**)p->tx_buf,
				        retry_pkts);
				pthread_mutex_unlock(&(*enq_dev).mutex);

				retry_pkts -= count;
				if (!retry_pkts)
					break;
			}

			/* If we still have packets left to send, we need to
			 * notify the user */
			if (retry_pkts)
				RTE_LOG(ERR, PORT, "After '%d' attempts,"
				        " failed to transmit '%d' packets on"
				        " vEth interface (expected: '%d'"
				        " sent: '%d')\n",
				        p->tx_burst_retry_num, retry_pkts,
				        p->tx_buf_count, tx_count);
		}
	} else {
		RTE_LOG(ERR, PORT, "Attempting to enqueue packets onto an"
		        " invalid vEth device\n");
	}

	for (i = tx_count; i < p->tx_buf_count; i++)
		rte_pktmbuf_free_seg(p->tx_buf[i]);

	p->tx_buf_count = 0;
}

/*
 * vEth writer port single-packet transmit.
 *
 * Attempt to send 'pkt' to 'port', and return 0.
 */
static int
rte_port_veth_writer_tx(void *port, struct rte_mbuf *pkt)
{
	struct rte_port_veth_writer *p = (struct rte_port_veth_writer *)port;

	if (unlikely(p == NULL)) {
		RTE_LOG(ERR, PORT, "vEth writer port is NULL, exiting tx"
		        "function\n");
		return 0;
	}

	p->tx_buf[p->tx_buf_count++] = pkt;

	if (unlikely(p->tx_buf_count >= p->tx_burst_sz))
		tx_burst(p);

	return 0;
}

/*
 * vEth writer port burst transmit.
 *
 * Attempt to enqueue 'port->tx_buf_count' mbufs from 'pkts' onto the KNI
 * device specified by 'p'. If only a subset of the mbufs contained in 'pkts'
 * is intended for 'port', use 'pkts_mask' to determine the specific mbufs
 * to be sent.
 */
static int
rte_port_veth_writer_tx_bulk(void *port, struct rte_mbuf **pkts, uint64_t pkts_mask)
{
	struct rte_port_veth_writer *p = (struct rte_port_veth_writer *)port;
	struct rte_mbuf *pkt = NULL;

	if (unlikely(p == NULL)) {
		RTE_LOG(ERR, PORT, "vEth writer port is NULL, exiting tx"
		        "function\n");
		return 0;
	}

	/* All packets in 'pkts' should be sent to 'port'. */
	if ((pkts_mask & (pkts_mask + 1)) == 0) {
		/* Count the number of set bits in 'pkts_mask' - this indicates
		 * the total number of mbufs to send */
		uint64_t n_pkts = __builtin_popcountll(pkts_mask);
		uint32_t i;

		for (i = 0; i < n_pkts; i ++) {
			pkt = pkts[i];

			p->tx_buf[p->tx_buf_count++] = pkt;
			if (p->tx_buf_count > RTE_PORT_IN_BURST_SIZE_MAX)
				RTE_LOG(WARNING, PORT, "Overflowed the vEth"
				        " tx_buf (expected: '%d' received: '%d'"
				        ")\n", RTE_PORT_IN_BURST_SIZE_MAX,
				        p->tx_buf_count);
			if (p->tx_buf_count >= p->tx_burst_sz)
				tx_burst(p);
		}
	/* Only send a subset of 'pkts' - use 'pkts_mask' to determine exactly
	 * which mbufs within 'pkts' should be sent */
	} else {
		uint32_t pkt_index;
		uint64_t pkt_mask;

		for ( ; pkts_mask; ) {
			/* Count the number of trailing zeroes in 'pkt_mask'
			 * to determine the index of the first Tx packet for
			 * the port.
			 */
			pkt_index = __builtin_ctzll(pkts_mask);
			/* Create a mask which will be used to unmark
			 * packets that have been processed within 'pkts_mask'.
			 */
			pkt_mask = 1LLU << pkt_index;
			pkt = pkts[pkt_index];

			p->tx_buf[p->tx_buf_count ++] = pkt;
			if (p->tx_buf_count > RTE_PORT_IN_BURST_SIZE_MAX)
				RTE_LOG(WARNING, PORT, "Overflowed the vEth"
				        " tx_buf (expected: '%d' received: '%d'"
				        ")\n", RTE_PORT_IN_BURST_SIZE_MAX,
				        p->tx_buf_count);
			if (p->tx_buf_count >= p->tx_burst_sz)
				tx_burst(p);

			/* Unmark processed packets within 'pkts_mask'. */
			pkts_mask &= ~pkt_mask;
		}
	}

	return 0;
}

/*
 * vEth writer port flush.
 *
 * Transmit any packets within 'port's internal transmit buffer.
 */
static int
rte_port_veth_writer_flush(void *port)
{
	struct rte_port_veth_writer *p = (struct rte_port_veth_writer *)port;

	tx_burst(p);

	return 0;
}

/*
 * vEth reader port operations struct.
 */
struct rte_port_in_ops rte_port_veth_reader_ops = {
	.f_create = rte_port_veth_reader_create,
	.f_free = rte_port_veth_reader_free,
	.f_rx = rte_port_veth_reader_rx,
};

/*
 * vEth writer port operations struct.
 */
struct rte_port_out_ops rte_port_veth_writer_ops = {
	.f_create = rte_port_veth_writer_create,
	.f_free = rte_port_veth_writer_free,
	.f_tx = rte_port_veth_writer_tx,
	.f_tx_bulk = rte_port_veth_writer_tx_bulk,
	.f_flush = rte_port_veth_writer_flush,
};
