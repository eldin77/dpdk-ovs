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

#include <string.h>
#include <assert.h>

#include <rte_config.h>

#include <rte_mbuf.h>
#include <rte_ring.h>
#include <rte_malloc.h>
#include <rte_log.h>
#include <rte_cycles.h>

#include "rte_port_ivshm.h"

/*
 * The maximum number of buffers we will allocate/free at a time on
 * to the alloc ring and the free ring respectively
 */
#define ALLOC_BURST_SIZE         64
#define FREE_BURST_SIZE          256

/* Define prototypes for free functions so that they
 * can be used in other functions within this file
 */
static int rte_port_ivshm_writer_free(void *);
static int rte_port_ivshm_reader_free(void *);

/*
 * IVSHM Reader port struct
 *
 * An instance of rte_port_ivshm_reader receives traffic (i.e. mbufs) sent by
 * an IVSHM-based guest application.
 *
 * - rx_ring contains ingress data packets destined for the port
 * - free_ring contains mbufs no longer needed by an IVSHM-based guest
 *   application - these mbufs are freed by rte_port_ivshm_reader_rx.
 * - alloc_ring: contains mbufs for use by the IVSHM-based guest application.
 *   mbufs are allocated from host mempool 'mp', and enqueued to the guest via
 *   this ring to prevent corruption of the mbuf cache - a known issue when
 *   allocation takes place in the guest.
 */
struct rte_port_ivshm_reader {
	struct rte_ring *rx_ring;   /* Stores ingress mbufs */
	struct rte_ring *free_ring; /* Stores mbufs freed by IVSHM guest app */
	struct rte_ring *alloc_ring;/* Stores mbufs intended for use in guest */
	struct rte_mempool *mp;     /* Mempool from which mbufs are alloc'd */
};

/*
 * IVSHM Writer port struct
 *
 * An instance of rte_port_ivshm_writer transmits traffic (i.e. mbufs) destined
 * for an IVSHM-based guest application.
 *
 * - tx_ring: contains data packets destined for the guest app
 *
 * The number of mbufs written in a single burst is detemined by 'tx_burst_sz'.
 */
struct rte_port_ivshm_writer {
	struct rte_mbuf *tx_buf[RTE_PORT_IN_BURST_SIZE_MAX]; /* Local buffer */
	struct rte_ring *tx_ring;     /* Stores mbufs intended for Tx */
	uint32_t tx_burst_sz;         /* Transmit burst size */
	uint32_t tx_burst_retry_num;  /*How many retries to make*/
	uint32_t tx_burst_delay_time; /*How long to delay during retry*/
	uint32_t tx_buf_count;      /* Transmit buffer count */
};

/*============= IVSHM Reader Port functions ===============*/

/* IVSHM Reader port creation function.
 * Allocates memory for an instance of rte_port_ivshm_reader on 'socket_id.
 * Configures the port instance, using information contained in 'params':
 * - Memory address of Rx ring
 * - Memory address of Free ring
 * - Memory address of Alloc ring
 * - Memory address of Mempool */
static void *
rte_port_ivshm_reader_create(void *params, int socket_id)
{
	struct rte_port_ivshm_reader_params *conf =
	       (struct rte_port_ivshm_reader_params *) params;
	struct rte_port_ivshm_reader *port;

	/* Check input parameters */
	if (conf == NULL) {
		RTE_LOG(ERR, PORT, "%s: %d: Could not create IVSHM reader - "
			           "invalid configuration parameters - "
			           "conf struct\n",
			           __FUNCTION__, __LINE__);
		return NULL;
	}

	/* Memory allocation */
	port = rte_malloc_socket("PIPELINE",
	                         sizeof(*port),
	                         CACHE_LINE_SIZE,
	                         socket_id);
	if (port == NULL) {
		RTE_LOG(ERR, PORT, "%s: %d: Could not create IVSHM reader - "
			           "insuffcient memory available\n",
			           __FUNCTION__, __LINE__);
		return NULL;
	}

	memset(port, 0x0, sizeof(*port));

	/* Initialization */
	port->rx_ring = rte_ring_lookup(conf->rx_ring_name);

	if(port->rx_ring == NULL) {
		RTE_LOG(ERR, PORT,
		        "%s: Could not create IVSHM reader"
		        " - could not connect to rx ring %s\n",
		        __FUNCTION__,
		        conf->rx_ring_name);
		rte_port_ivshm_writer_free(port);
		return NULL;
	}

	port->free_ring = rte_ring_lookup(conf->free_ring_name);

	if(port->free_ring == NULL) {
		RTE_LOG(ERR, PORT,
		        "%s: Could not create IVSHM reader"
		        " - could not connect to free ring %s\n",
		        __FUNCTION__,
		        conf->free_ring_name);
		rte_port_ivshm_writer_free(port);
		return NULL;
	}

	port->alloc_ring = rte_ring_lookup(conf->alloc_ring_name);

	if(port->alloc_ring == NULL) {
		RTE_LOG(ERR, PORT,
		        "%s: Could not create IVSHM reader"
		        " - could not connect to alloc ring %s\n",
		        __FUNCTION__,
		        conf->alloc_ring_name);
		rte_port_ivshm_writer_free(port);
		return NULL;
	}

	port->mp = rte_mempool_lookup(conf->mp);

	if(port->mp == NULL) {
		RTE_LOG(ERR, PORT,
		        "%s: Could not create IVSHM writer"
		        " - could not connect to mempool %s\n",
		        __FUNCTION__,
		        conf->mp);
		rte_port_ivshm_writer_free(port);
		return NULL;
	}

	return port;
}

/* IVSHM Reader port receive function.
 * Attempts to dequeue 'n_pkts' mbufs from 'pkts' and store them in 'port's Rx
 * ring.
 *
 * Also ensures that the enqueue ring is sufficiently full and that the free
 * ring is empty
 */
static int
rte_port_ivshm_reader_rx(void *port, struct rte_mbuf **pkts, uint32_t n_pkts)
{
	int i = 0;
	uint32_t num_alloced = 0;
	uint32_t ring_free_count = 0;
	uint32_t num_to_alloc = 0;
	struct rte_mbuf *alloc_buf[ALLOC_BURST_SIZE];
	int n_enq_pkts = 0;
	struct rte_mbuf *freebufs[FREE_BURST_SIZE];
	struct rte_port_ivshm_reader *p =
	       (struct rte_port_ivshm_reader *) port;

	assert(port !=NULL);

	/* Dequeue as many buffers to be freed as possible */
	n_enq_pkts = rte_ring_sc_dequeue_burst(p->free_ring,
	                                (void **) freebufs,
	                                FREE_BURST_SIZE);
	if (unlikely(n_enq_pkts > 0))
		RTE_LOG(INFO, PORT, "%s: Freeing %d packets from IVSHM free_ring %s\n",
		        __FUNCTION__, n_enq_pkts, p->free_ring->name);

	for(i = 0; i < n_enq_pkts; i++) {
		rte_pktmbuf_free(freebufs[i]);
	}

	/* Only allocate buffers if we need to */
	ring_free_count = rte_ring_free_count(p->alloc_ring);
	if (ring_free_count) {
		num_to_alloc = RTE_MIN(ALLOC_BURST_SIZE, ring_free_count);

		for(i = 0; i < num_to_alloc; i++)
			alloc_buf[i] = rte_pktmbuf_alloc(p->mp);

		num_alloced = rte_ring_sp_enqueue_burst(p->alloc_ring,
						      (void **) alloc_buf,
						      num_to_alloc);

		if (unlikely(num_alloced < num_to_alloc))
			RTE_LOG(INFO, PORT, "%s: unable to enqueue all buffers "
				"to IVSHM alloc ring %s; freeing %"PRIu32
				" buffers\n", __FUNCTION__,
				p->alloc_ring->name,
				num_to_alloc - num_alloced);
		for(i = num_alloced; i < num_to_alloc; i++)
			rte_pktmbuf_free(alloc_buf[i]);
	}

	return rte_ring_sc_dequeue_burst(p->rx_ring, (void **) pkts, n_pkts);
}

/* Drain 'ring' and free any residual mbufs */
static void
rte_port_ivshm_ring_drain(struct rte_ring *ring, uint32_t burst_size)
{
	uint32_t i = 0;
	uint32_t n_pkts = 0;
	struct rte_mbuf *bufs[burst_size];

	n_pkts = rte_ring_sc_dequeue_burst(ring, (void **)&bufs, burst_size);
	if (unlikely(n_pkts > 0))
		RTE_LOG(INFO, PORT, "%s: Freeing %d packets from IVSHM free "
		       "ring %s\n", __FUNCTION__, n_pkts, ring->name);

	for (i = 0; i < n_pkts; i++)
		rte_pktmbuf_free(bufs[i]);
}

/* Ensure that any residual mbufs in 'port's rings are handled */
static void
rte_port_ivshm_reader_flush(void * port)
{
	struct rte_port_ivshm_reader *p = (struct rte_port_ivshm_reader *)port;

	rte_port_ivshm_ring_drain(p->rx_ring, rte_ring_count(p->rx_ring));
	rte_port_ivshm_ring_drain(p->free_ring, rte_ring_count(p->free_ring));
	rte_port_ivshm_ring_drain(p->alloc_ring, rte_ring_count(p->alloc_ring));
}

/* IVSHM Reader port free function.
 * If 'port' is non-null, frees the port instance and  any residual mbufs in
 * the rx and free rings.
 */
static int
rte_port_ivshm_reader_free(void *port)
{
	if (port == NULL) {
		RTE_LOG(ERR, PORT, "%s: %d: invalid port argument\n",
			__FUNCTION__, __LINE__);
		return -1;
	}

	rte_port_ivshm_reader_flush(port);
	rte_free(port);

	return 0;
}

/*=========  IVSHM Writer port functions ===============*/

/* IVSHM port creation function.
 * Allocates memory for an instance of rte_port_ivshm_writer on 'socket_id'.
 * Configures the port instance using information contained in 'params':
 * - Address of port's Tx ring
 * - Tx burst size
 */
static void *
rte_port_ivshm_writer_create(void *params, int socket_id)
{
	struct rte_port_ivshm_writer_params *conf =
	       (struct rte_port_ivshm_writer_params *) params;
	struct rte_port_ivshm_writer *port;

	/* Check input parameters */
	if ((conf == NULL) ||
	    (conf->tx_burst_sz > RTE_PORT_IN_BURST_SIZE_MAX)) {
		RTE_LOG(ERR, PORT, "%s: %d: Could not create IVSHM writer - "
			           "invalid configuration parameters\n",
			           __FUNCTION__, __LINE__);
		return NULL;
	}


	/* Memory allocation */
	port = rte_malloc_socket("PIPELINE",
	                         sizeof(*port),
	                         CACHE_LINE_SIZE,
	                         socket_id);

	if (port == NULL) {
		RTE_LOG(ERR, PORT, "%s: %d: Could not create IVSHM writer - "
			           "port: insuffcient memory available\n",
			           __FUNCTION__, __LINE__);
		return NULL;
	}

	memset(port, 0x0, sizeof(*port));

	/* Initialization */

	port->tx_ring = rte_ring_lookup(conf->tx_ring_name);

	if(port->tx_ring == NULL) {
		RTE_LOG(ERR, PORT,
		        "%s: Could not create IVSHM writer"
		        " - could not connect to tx ring %s\n",
		        __FUNCTION__,
		        conf->tx_ring_name);
		rte_port_ivshm_writer_free(port);
		return NULL;
	}

	port->tx_burst_sz = conf->tx_burst_sz;
	port->tx_buf_count = 0;
	port->tx_burst_retry_num = 10;
	port->tx_burst_delay_time = 1;

	return port;
}

/* IVSHM Writer port burst-transmit function.
 * Attempts to enqueue 'tx_burst_sz' mbufs from 'p's internal tx buffer onto
 * its tx ring, freeing any packets unsuccessfully enqueued.
 */
static inline void
send_burst_tx(struct rte_port_ivshm_writer *p)
{
	uint32_t nb_tx = 0;
	uint32_t retry = 0;
	uint32_t free_entries = 0;

	free_entries = rte_ring_free_count(p->tx_ring);
	/* If retry is enabled and the queue is full then we wait and retry to avoid packet loss. */
	if (unlikely(p->tx_buf_count > free_entries)) {
		for (retry = 0; retry < p->tx_burst_retry_num; retry++) {
			rte_delay_us(p->tx_burst_delay_time);
			free_entries = rte_ring_free_count(p->tx_ring);
			if (p->tx_buf_count <= free_entries)
				break;
		}
	}

	if (unlikely(retry > 0 && retry == p->tx_burst_retry_num))
		RTE_LOG(INFO, PORT,
		        "%s: max number of retries exceeded (%"PRIu32"), but"
		        " still insufficient free entries in IVSHM port Tx ring"
		        " %s\nto accomodate %"PRIu32" outbound Tx buffers.\n"
		        "Attempting to enqueue buffers anyway\n",
		        __FUNCTION__,
		        p->tx_burst_retry_num,
		        p->tx_ring->name,
		        p->tx_buf_count);

       nb_tx = rte_ring_mp_enqueue_burst(p->tx_ring,
	                                  (void **)p->tx_buf,
	                                  p->tx_buf_count);

	if (unlikely(p->tx_buf_count > nb_tx))
		RTE_LOG(INFO, PORT, "%s: unable to enqueue all buffers to "
			"IVSHM port Tx ring %s; freeing %"PRIu32" buffers\n",
			__FUNCTION__, p->tx_ring->name,
			p->tx_buf_count - nb_tx);

	for ( ; nb_tx < p->tx_buf_count; nb_tx ++) {
		rte_pktmbuf_free(p->tx_buf[nb_tx]);
	}

	p->tx_buf_count = 0;
}

/* IVSHM Writer Port single-packet transmit function.
 * Attempts to send 'pkt' out through 'port'.
 */
static int
rte_port_ivshm_writer_tx(void *port, struct rte_mbuf *pkt)
{
	struct rte_port_ivshm_writer *p = (struct rte_port_ivshm_writer *) port;

	assert(port != NULL);
	assert(pkt != NULL);

	p->tx_buf[p->tx_buf_count ++] = pkt;

	if (p->tx_buf_count >= p->tx_burst_sz)
		send_burst_tx(p);

	return 0;
}

/* IVSHM Writer port bulk-transmit function.
 * Attempts to send 'tx_burst_sz' mbufs contained in 'pkts' to 'port'.
 * If only a subset of the mbufs contained with 'pkts' is intended for 'port',
 * uses 'pkts_mask' to determine the specific mbufs that should be sent.
 */
static int
rte_port_ivshm_writer_tx_bulk(void *port,
                              struct rte_mbuf **pkts,
                              uint64_t pkts_mask)
{
	struct rte_port_ivshm_writer *p = (struct rte_port_ivshm_writer *) port;

	assert(port !=NULL);
	assert(pkts !=NULL);

	/* All packets in 'pkts' should be sent to 'port'. */
	if ((pkts_mask & (pkts_mask + 1)) == 0) {
		/* Count the number of set bits in 'pkts_mask' - this indicates
		 * the total number of mbufs to send */
		uint64_t n_pkts = __builtin_popcountll(pkts_mask);
		uint32_t i;

		for (i = 0; i < n_pkts; i ++) {
			struct rte_mbuf *pkt = pkts[i];

			p->tx_buf[p->tx_buf_count ++] = pkt;

			if (p->tx_buf_count >= p->tx_burst_sz)
				send_burst_tx(p);

		}
	/* Only send a subset of 'pkts' - use 'pkts_mask' to determine exactly
	 * which mbufs within 'pkts' should be sent */
	} else {
		for ( ; pkts_mask; ) {
			/* Count the number of trailing zeroes in 'pkt_mask'
			 * to determine the index of the first Tx packet for
			 * the port.
			 */
			uint32_t pkt_index = __builtin_ctzll(pkts_mask);
			/* Create a mask which will be used to unmark
			 * packets that have been processed within 'pkts_mask'.
			 */
			uint64_t pkt_mask = 1LLU << pkt_index;
			struct rte_mbuf *pkt = pkts[pkt_index];

			p->tx_buf[p->tx_buf_count ++] = pkt;

			if (p->tx_buf_count >= p->tx_burst_sz)
				send_burst_tx(p);

			/* Unmark processed packets within 'pkts_mask'. */
			pkts_mask &= ~pkt_mask;
		}
	}

	return 0;
}

/* IVSHM Writer port flush function.
 * Clears out 'port's Tx ring by invoking 'send_burst_tx'.
 */
static int
rte_port_ivshm_writer_flush(void *port)
{
	struct rte_port_ivshm_writer *p = NULL;

	if(port == NULL)
		rte_exit(ENODEV, "IVSHM port could not flush writer,"
		                 " invalid port.\n");

	p = (struct rte_port_ivshm_writer *) port;

	if (p->tx_buf_count > 0)
		send_burst_tx(p);

	return 0;
}

/* IVSHM Writer port free function.
 * If 'port' is non-null, drains the Tx queue, sending residual mbufs before
 * freeing the port instance.
 */
static int
rte_port_ivshm_writer_free(void *port)
{
	if (port == NULL) {
		RTE_LOG(ERR, PORT, "%s: %d: invalid port argument\n",
			__FUNCTION__, __LINE__);
		return -1;
	}

	rte_port_ivshm_writer_flush(port);

	rte_free(port);

	return 0;
}

/*
 * Summary of port operations
 */
struct rte_port_in_ops rte_port_ivshm_reader_ops = {
	.f_create = rte_port_ivshm_reader_create,
	.f_free = rte_port_ivshm_reader_free,
	.f_rx = rte_port_ivshm_reader_rx,
};

struct rte_port_out_ops rte_port_ivshm_writer_ops = {
	.f_create = rte_port_ivshm_writer_create,
	.f_free = rte_port_ivshm_writer_free,
	.f_tx = rte_port_ivshm_writer_tx,
	.f_tx_bulk = rte_port_ivshm_writer_tx_bulk,
	.f_flush = rte_port_ivshm_writer_flush,
};
