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

#include <linux/virtio_net.h>
#include <linux/virtio_ring.h>

#include <rte_config.h>
#include <rte_string_fns.h>
#include <rte_malloc.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>

#include "rte_port_vhost.h"
#include "ovdk_virtio-net.h"

#define RTE_LOGTYPE_APP        RTE_LOGTYPE_USER1

#define PKT_BURST_SIZE 32

#define rpl_strnlen strnlen

/* The mbuf pool for packet rx */
struct rte_mempool *pf_pktmbuf_pool;

/* Number of descriptors per cacheline. */
#define DESC_PER_CACHELINE (CACHE_LINE_SIZE / sizeof(struct vring_desc))
#define MAX_PRINT_BUFF         6072  /* Size of buffers used for snprintfs for printing packets */
#define MAX_MRG_PKT_BURST      16    /* Max burst for merge buffers. This is used for legacy virtio. */
#define BURST_TX_WAIT_US       15    /* Defines how long we wait between retries on TX */
#define BURST_TX_RETRIES       4     /* Number of retries on TX. */

/* Specify timeout (in useconds) between retries on TX. */
uint32_t burst_tx_delay_time = BURST_TX_WAIT_US;
/* Specify the number of retries on TX. */
uint32_t burst_tx_retry_num = BURST_TX_RETRIES;

/*
 * Macro to print out packet contents. Wrapped in debug define so that the
 * data path is not effected when debug is disabled.
 */
#ifdef VHOST_DEBUG
#define PRINT_PACKET(device, addr, size, header) do {					\
	char *pkt_addr = (char*)(addr);										\
	unsigned int index;													\
	char packet[MAX_PRINT_BUFF];										\
																		\
	if ((header))														\
	snprintf(packet, MAX_PRINT_BUFF,								\
		"(%"PRIu64") Header size %d: ", (device->device_fh), (size));	\
	else																\
	snprintf(packet, MAX_PRINT_BUFF,								\
		"(%"PRIu64") Packet size %d: ", (device->device_fh), (size));	\
	for (index = 0; index < (size); index++) {							\
		snprintf(packet + strnlen(packet, MAX_PRINT_BUFF),			\
			MAX_PRINT_BUFF - strnlen(packet, MAX_PRINT_BUFF),			\
			"%02hhx ", pkt_addr[index]);								\
	}																	\
	snprintf(packet + strnlen(packet, MAX_PRINT_BUFF),				\
			MAX_PRINT_BUFF - strnlen(packet, MAX_PRINT_BUFF), "\n");	\
																		\
	LOG_DEBUG(APP, "%s", packet);						             	\
} while(0)
#else
#define PRINT_PACKET(device, addr, size, header) do{} while(0)
#endif

/*
 * Function to convert guest physical addresses to vhost virtual addresses. This
 * is used to convert virtio buffer addresses.
 */
static inline uint64_t __attribute__((always_inline))
gpa_to_vva(struct virtio_net *dev, uint64_t guest_pa)
{
	struct virtio_memory_regions *region;
	uint32_t regionidx;
	uint64_t vhost_va = 0;

	for (regionidx = 0; regionidx < dev->mem->nregions; regionidx++) {
		region = &dev->mem->regions[regionidx];
		if ((guest_pa >= region->guest_phys_address) &&
			(guest_pa <= region->guest_phys_address_end)) {
			vhost_va = region->address_offset + guest_pa;
			break;
		}
	}
	LOG_DEBUG(APP, "(%"PRIu64") GPA %p| VVA %p\n",
		dev->device_fh, (void*)(uintptr_t)guest_pa, (void*)(uintptr_t)vhost_va);

	return vhost_va;
}

/*
 * Enqueues packets to the guest virtio RX virtqueue for vhost devices.
 */
static inline uint32_t __attribute__((always_inline))
pf_vhost_enqueue_burst(struct virtio_net *dev, struct rte_mbuf **pkts, unsigned count)
{
	struct vhost_virtqueue *vq;
	struct vring_desc *desc;
	struct rte_mbuf *buff;
	/* The virtio_hdr is initialised to 0. */
	struct virtio_net_hdr_mrg_rxbuf virtio_hdr = {{0,0,0,0,0,0},0};
	uint64_t buff_addr = 0;
	uint64_t buff_hdr_addr = 0;
	uint32_t head[PKT_BURST_SIZE], packet_len = 0;
	uint32_t head_idx, packet_success = 0;
	uint32_t mergeable, mrg_count = 0;
	uint32_t retry = 0;
	uint16_t avail_idx, res_cur_idx;
	uint16_t res_base_idx, res_end_idx;
	uint16_t free_entries;
	uint8_t success = 0;

	LOG_DEBUG(APP, "(%"PRIu64") virtio_dev_rx()\n", dev->device_fh);
	vq = dev->virtqueue[VIRTIO_RXQ];
	count = (count > PKT_BURST_SIZE) ? PKT_BURST_SIZE : count;

	/* As many data cores may want access to available buffers, they need to be reserved. */
	do {
		res_base_idx = vq->last_used_idx_res;
		avail_idx = *((volatile uint16_t *)&vq->avail->idx);

		free_entries = (avail_idx - res_base_idx);
		/* If retry is enabled and the queue is full then we wait and retry to avoid packet loss. */
		if (unlikely(count > free_entries)) {
			for (retry = 0; retry < burst_tx_retry_num; retry++) {
				rte_delay_us(burst_tx_delay_time);
				avail_idx =
					*((volatile uint16_t *)&vq->avail->idx);
				free_entries = (avail_idx - res_base_idx);
				if (count <= free_entries)
					break;
			}
		}
		/*check that we have enough buffers*/
		if (unlikely(count > free_entries))
			count = free_entries;

		if (count == 0) {
			return 0;
		}
		res_end_idx = res_base_idx + count;
		/* vq->last_used_idx_res is atomically updated. */
		success = rte_atomic16_cmpset(&vq->last_used_idx_res, res_base_idx,
									res_end_idx);
	} while (unlikely(success == 0));
	res_cur_idx = res_base_idx;
	LOG_DEBUG(APP, "(%"PRIu64") Current Index %d| End Index %d\n", dev->device_fh, res_cur_idx, res_end_idx);

	/* Prefetch available ring to retrieve indexes. */
	rte_prefetch0(&vq->avail->ring[res_cur_idx & (vq->size - 1)]);

	/* Check if the VIRTIO_NET_F_MRG_RXBUF feature is enabled. */
	mergeable = dev->features & (1 << VIRTIO_NET_F_MRG_RXBUF);


	/* Retrieve all of the head indexes first to avoid caching issues. */
	for (head_idx = 0; head_idx < count; head_idx++) {
		head[head_idx] = vq->avail->ring[(res_cur_idx + head_idx) & (vq->size - 1)];
	}
	/*Prefetch descriptor index. */
	rte_prefetch0(&vq->desc[head[packet_success]]);

	while (res_cur_idx != res_end_idx) {

		/* Get descriptor from available ring */
		desc = &vq->desc[head[packet_success]];

		buff = pkts[packet_success];

		/* Convert from gpa to vva (guest physical addr -> vhost virtual addr) */
		buff_addr = gpa_to_vva(dev, desc->addr);
		/* Prefetch buffer address. */
		rte_prefetch0((void*)(uintptr_t)buff_addr);

		if (mergeable && (mrg_count != 0)) {
			desc->len = packet_len = rte_pktmbuf_data_len(buff);
		} else {
			/* Copy virtio_hdr to packet and increment buffer address */
			buff_hdr_addr = buff_addr;
			packet_len = rte_pktmbuf_data_len(buff) + vq->vhost_hlen;

			/*
			 * If the descriptors are chained the header and data are placed in
			 * separate buffers.
			 */
			if (desc->flags & VRING_DESC_F_NEXT) {
				desc->len = vq->vhost_hlen;
				desc = &vq->desc[desc->next];
				/* Buffer address translation. */
				buff_addr = gpa_to_vva(dev, desc->addr);
				desc->len = rte_pktmbuf_data_len(buff);
			} else {
				buff_addr += vq->vhost_hlen;
				desc->len = packet_len;
			}
		}

		/* Update used ring with desc information */
		vq->used->ring[res_cur_idx & (vq->size - 1)].id = head[packet_success];
		vq->used->ring[res_cur_idx & (vq->size - 1)].len = packet_len;

		/* Copy mbuf data to buffer */
		rte_memcpy((void *)(uintptr_t)buff_addr, (const void*)buff->pkt.data, rte_pktmbuf_data_len(buff));

		PRINT_PACKET(dev, (uintptr_t)buff_addr, rte_pktmbuf_data_len(buff), 0);

		res_cur_idx++;
		packet_success++;

		/* If mergeable is disabled then a header is required per buffer. */
		if (!mergeable) {
			rte_memcpy((void *)(uintptr_t)buff_hdr_addr, (const void*)&virtio_hdr, vq->vhost_hlen);
			PRINT_PACKET(dev, (uintptr_t)buff_hdr_addr, vq->vhost_hlen, 1);
		} else {
			mrg_count++;
			/* Merge buffer can only handle so many buffers at a time. Tell the guest if this limit is reached. */
			if ((mrg_count == MAX_MRG_PKT_BURST) || (res_cur_idx == res_end_idx)) {
				virtio_hdr.num_buffers = mrg_count;
				RTE_LOG(INFO, USER1, "(%"PRIu64") RX: Num merge buffers %d\n", dev->device_fh, virtio_hdr.num_buffers);
				rte_memcpy((void *)(uintptr_t)buff_hdr_addr, (const void*)&virtio_hdr, vq->vhost_hlen);
				PRINT_PACKET(dev, (uintptr_t)buff_hdr_addr, vq->vhost_hlen, 1);
				mrg_count = 0;
			}
		}
		if (res_cur_idx < res_end_idx) {
			/* Prefetch descriptor index. */
			rte_prefetch0(&vq->desc[head[packet_success]]);
		}
	}

	rte_compiler_barrier();

	/* Wait until it's our turn to add our buffer to the used ring. */
	while (unlikely(vq->last_used_idx != res_base_idx))
		rte_pause();

	*(volatile uint16_t *) &vq->used->idx += count;
	vq->last_used_idx = res_end_idx;

	/* Kick the guest if necessary. */
	if (!(vq->avail->flags & VRING_AVAIL_F_NO_INTERRUPT))
		eventfd_write(vq->kickfd,1);

	return count;
}

/*
 * Dequeues packets from the guest virtio TX virtqueue for vhost devices.
 */
static inline uint16_t __attribute__((always_inline))
pf_vhost_dequeue_burst(struct virtio_net *dev, struct rte_mbuf **pkts, unsigned count)
{
	struct rte_mbuf *mbuf;
	struct vhost_virtqueue *vq;
	struct vring_desc *desc;
	uint64_t buff_addr = 0;
	uint32_t head[PKT_BURST_SIZE];
	uint32_t used_idx, i;
	uint16_t free_entries, packet_success = 0;
	uint16_t avail_idx;

	vq = dev->virtqueue[VIRTIO_TXQ];
	avail_idx = *((volatile uint16_t *)&vq->avail->idx);

	/* If there are no available buffers then return. */
	if (vq->last_used_idx == avail_idx)
		return 0;

	LOG_DEBUG(APP, "(%"PRIu64") virtio_dev_tx()\n", dev->device_fh);

	/* Prefetch available ring to retrieve head indexes. */
	rte_prefetch0(&vq->avail->ring[vq->last_used_idx & (vq->size - 1)]);

	/*get the number of free entries in the ring*/
	free_entries = (avail_idx - vq->last_used_idx);

	/* Limit to PKT_BURST_SIZE. */
	if (free_entries > count)
		free_entries = count;

	/*
	 * Performance is better if cachelines containing descriptors are not accessed by multiple
	 * cores. We try to finish with a cacheline before passing it on.
	 */
	if (likely(free_entries > DESC_PER_CACHELINE))
		free_entries = free_entries - ((vq->last_used_idx + free_entries) % DESC_PER_CACHELINE);

	LOG_DEBUG(APP, "(%"PRIu64") Buffers available %d\n", dev->device_fh, free_entries);
	/* Retrieve all of the head indexes first to avoid caching issues. */
	for (i = 0; i < free_entries; i++)
		head[i] = vq->avail->ring[(vq->last_used_idx + i) & (vq->size - 1)];

	/* Prefetch descriptor index. */
	rte_prefetch0(&vq->desc[head[packet_success]]);
	rte_prefetch0(&vq->used->ring[vq->last_used_idx & (vq->size - 1)]);

	while (packet_success < free_entries) {
		desc = &vq->desc[head[packet_success]];

		/* Discard first buffer as it is the virtio header */
		desc = &vq->desc[desc->next];

		/* Buffer address translation. */
		buff_addr = gpa_to_vva(dev, desc->addr);
		/* Prefetch buffer address. */
		rte_prefetch0((void*)(uintptr_t)buff_addr);

		used_idx = vq->last_used_idx & (vq->size - 1);

		if (packet_success < (free_entries - 1)) {
			/* Prefetch descriptor index. */
			rte_prefetch0(&vq->desc[head[packet_success+1]]);
			rte_prefetch0(&vq->used->ring[(used_idx + 1) & (vq->size - 1)]);
		}

		/* Update used index buffer information. */
		vq->used->ring[used_idx].id = head[packet_success];
		vq->used->ring[used_idx].len = 0;

		/* Allocate an mbuf and populate the structure. */
		mbuf = rte_pktmbuf_alloc(pf_pktmbuf_pool);
		if (unlikely(mbuf == NULL)) {
			RTE_LOG(ERR, APP, "Failed to allocate memory for mbuf.\n");
			return packet_success;
		}

		/* Setup dummy mbuf. */
		mbuf->pkt.data_len = desc->len;
		mbuf->pkt.pkt_len = mbuf->pkt.data_len;

		rte_memcpy((void*) mbuf->pkt.data,
		           (const void*) buff_addr, mbuf->pkt.data_len);

		pkts[packet_success]=mbuf;

		PRINT_PACKET(dev, (uintptr_t)buff_addr, desc->len, 0);

		vq->last_used_idx++;
		packet_success++;
	}

	rte_compiler_barrier();
	vq->used->idx += packet_success;
	/* Kick guest if required. */
	if (!(vq->avail->flags & VRING_AVAIL_F_NO_INTERRUPT))
		eventfd_write(vq->kickfd,1);
	return packet_success;
}

/*
 * Vhost Reader port struct
 *
 * An instance of rte_port_vhost_reader receives traffic (i.e. mbufs) send by a
 * virtio-net device on a guest.
 */
struct rte_port_vhost_reader {
	struct virtio_net **dev; /* Virtio-net device which will be populated
	                          *  when the guest is brought up. */
};

/*============= Vhost Reader Port functions ===============*/

/* Vhost Reader port creation function.
 * Allocates memory for an instance of rte_port_vhost_reader on 'socket_id'
 * Configures the port instance, using information contained in 'params':
 * - Memory address of where the pointer to the virtio-net device will
 * eventually reside once the guest is brought up.
 * - Sets the packet mbuf-pool used for receival of mbufs on the host from a
 * guest virtio-net device. A lookup for this mempool is performed using the
 * name (string) provided in 'params'.
 */
static void *
rte_port_vhost_reader_create(void *params, int socket_id)
{
	struct rte_port_vhost_reader_params *conf =
			(struct rte_port_vhost_reader_params *)params;
	struct rte_port_vhost_reader *port;

	/* Check input parameters */
	if (conf == NULL)
		return NULL;

	/* Memory allocation */
	port = rte_malloc_socket("PIPELINE",
	                         sizeof(*port),
	                         CACHE_LINE_SIZE,
	                         socket_id);
	if (port == NULL)
		return NULL;

	memset(port, 0x0, sizeof(*port));

	/* Initialization */
	port->dev = conf->dev;
	pf_pktmbuf_pool = rte_mempool_lookup(conf->pool_name);

	return port;
}

/* Vhost Reader port receive function.
 * Attempts to dequeue 'n_pkts' from the guest virtio-net device specified by
 * 'port'. Once dequeued, these mbufs are freed.
 */
static int
rte_port_vhost_reader_rx(void *port, struct rte_mbuf **pkts, uint32_t n_pkts)
{
	int nb_rx = 0;

	struct rte_port_vhost_reader *p = (struct rte_port_vhost_reader *)port;

	struct virtio_net *deq_dev = *(p->dev);

	if (unlikely(deq_dev == NULL)) {
		return 0;
	} else {
		nb_rx = pf_vhost_dequeue_burst(deq_dev, pkts, n_pkts);

		return nb_rx;
	}
}

/* Vhost Reader port free function.
 * If 'port' is non-null, frees the port instance.
 */
static int
rte_port_vhost_reader_free(void *port)
{
	if (port == NULL)
		return -1;

	rte_free(port);

	return 0;
}

/*
 * Vhost Writer port struct
 *
 * An instance of rte_port_vhost_writer transmits traffic (i.e. mbufs) destined
 * for a virtio-net device on a guest.
 */

struct rte_port_vhost_writer {
	struct virtio_net **dev;
	struct rte_mbuf *tx_buf[RTE_PORT_IN_BURST_SIZE_MAX];
	uint32_t tx_burst_sz;
	uint16_t tx_buf_count;
};

/* Vhost writer port creation function.
 * Allocates memory for an instance of rte_port_vhost_writer on 'socket_id'.
 * Configures the port instance using information contained in 'params':
 * - Memory address of where the pointer to the virtio-net device will
 * eventually reside once the guest is brought up.
 * - Tx burst size.
 */
static void *
rte_port_vhost_writer_create(void *params, int socket_id)
{
	struct rte_port_vhost_writer_params *conf =
			(struct rte_port_vhost_writer_params *)params;
	struct rte_port_vhost_writer *port;

	/* Check input parameters */
	if (conf == NULL)
		return NULL;

	/* Memory allocation */
	port = rte_malloc_socket("PIPELINE",
	                         sizeof(*port),
	                         CACHE_LINE_SIZE,
	                         socket_id);
	if (port == NULL)
		return NULL;

	memset(port, 0x0, sizeof(*port));

	/* Initialization */
	port->dev = conf->dev;
	port->tx_burst_sz = conf->tx_burst_sz;

	return port;
}

/* Vhost Writer port burst-transmit function.
 * Attempts to enqueue 'tx_buf_count' mbufs from 'p''s internal tx buffer onto
 * the virtio-net device specified by 'p'. Once enqueued, these mbufs are freed
 * and the count is reset to zero.
 */
static inline void
send_burst(struct rte_port_vhost_writer *p)
{
	int i = 0;
	uint32_t num_enqueued = 0;
	struct virtio_net *enq_dev = *(p->dev);

	if (unlikely(enq_dev != NULL)) {
		num_enqueued = pf_vhost_enqueue_burst(enq_dev,
		                                   (struct rte_mbuf**)p->tx_buf,
		                                   p->tx_buf_count);

		/* This log _should_ never actually be executed. mbufs are added
		 * to the port's local tx buffer sequentially and individually;
		 * 'send_burst' is only invoked once the port's tx_buf_count
		 * reaches its tx_burst_sz (with the exception of the flush
		 * function).
		 *
		 * Thus, assuming p->tx_buf_count <= PKT_BURST_SIZE,
		 * num_enqueued should never be smaller than the tx_buf_count.
		 * Nonetheless, this log is introduced as a sanity check.
		 */
		if (unlikely(num_enqueued < p->tx_buf_count))
			RTE_LOG(INFO, APP, "%s: Attempted to enqueue %"
			        PRIu32" mbufs to vhost port %s, but only %"PRIu32
			        " succeeded\n", __FUNCTION__, p->tx_buf_count,
			        enq_dev->port_name, num_enqueued);


	}

	/* Free all mbufs even if virtio device is down (i.e. enq_dev == NULL),
	 * otherwise tx_buf_count is going to overflow.
	 */
	for (i = 0; i < p->tx_buf_count; i++)
		rte_pktmbuf_free_seg(p->tx_buf[i]);

	p->tx_buf_count = 0;
}

/* Vhost Writer port single-packet transmit function.
 * Attempts to send 'pkt' to 'port'.
 */
static int
rte_port_vhost_writer_tx(void *port, struct rte_mbuf *pkt)
{
	struct rte_port_vhost_writer *p = (struct rte_port_vhost_writer *)port;

	p->tx_buf[p->tx_buf_count++] = pkt;
	if (p->tx_buf_count >= p->tx_burst_sz)
		send_burst(p);


	return 0;
}

/* Vhost Writer port bulk-transmit function.
 * Attempts to send the port's 'tx_buf_count' mbufs contained in 'pkts' to
 * 'port'. If only a subset of the mbufs contained in 'pkts' is intended for
 * 'port', uses 'pkts_mask' to determine the specific mbufs to be sent.
 */
static int
rte_port_vhost_writer_tx_bulk(void *port, struct rte_mbuf **pkts, uint64_t pkts_mask)
{
	struct rte_port_vhost_writer *p = (struct rte_port_vhost_writer *)port;

	if ((pkts_mask & (pkts_mask + 1)) == 0) {
		uint64_t n_pkts = __builtin_popcountll(pkts_mask);
		uint32_t i;

		for (i = 0; i < n_pkts; i ++) {
			struct rte_mbuf *pkt = pkts[i];
			p->tx_buf[p->tx_buf_count ++] = pkt;
			if (p->tx_buf_count >= p->tx_burst_sz) {
				send_burst(p);
			}
		}
	} else {
		for ( ; pkts_mask; ) {
			uint32_t pkt_index = __builtin_ctzll(pkts_mask);
			uint64_t pkt_mask = 1LLU << pkt_index;
			struct rte_mbuf *pkt = pkts[pkt_index];

			p->tx_buf[p->tx_buf_count ++] = pkt;
			if (p->tx_buf_count >= p->tx_burst_sz) {
				send_burst(p);
			}
			pkts_mask &= ~pkt_mask;
		}
	}

	return 0;
}

/* Vhost Writer port flush function.
 * Transmits any packets within 'ports''s internal transmit buffer by invoking
 * 'send_burst'.
 */
static int
rte_port_vhost_writer_flush(void *port)
{
	struct rte_port_vhost_writer *p = (struct rte_port_vhost_writer *)port;

	send_burst(p);
	return 0;
}

/* Vhost Writer port free function
 * If 'port' is non-null, frees the port instance.
 */
static int
rte_port_vhost_writer_free(void *port)
{
	if (port == NULL)
		return -1;

	rte_free(port);

	return 0;
}

/*
 * Summary of port operations
 */
struct rte_port_in_ops rte_port_vhost_reader_ops = {
	.f_create = rte_port_vhost_reader_create,
	.f_free = rte_port_vhost_reader_free,
	.f_rx = rte_port_vhost_reader_rx,
};

struct rte_port_out_ops rte_port_vhost_writer_ops = {
	.f_create = rte_port_vhost_writer_create,
	.f_free = rte_port_vhost_writer_free,
	.f_tx = rte_port_vhost_writer_tx,
	.f_tx_bulk = rte_port_vhost_writer_tx_bulk,
	.f_flush = rte_port_vhost_writer_flush,
};
