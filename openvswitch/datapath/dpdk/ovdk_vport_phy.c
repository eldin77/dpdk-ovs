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

#include <rte_config.h>
#include <rte_ethdev.h>
#include <rte_errno.h>

#include "ovdk_vport_phy.h"
#include "ovdk_mempools.h"
#include "ovdk_stats.h"
#include "ovdk_args.h"

#define PORT_RX_RING_SIZE       512
#define PORT_TX_RING_SIZE       512

/*
 * RX and TX Prefetch, Host, and Write-back threshold values should be
 * carefully set for optimal performance. Consult the network
 * controller's datasheet and supporting DPDK documentation for guidance
 * on how these parameters should be set.
 *
 * Default configuration for rx and tx thresholds etc.
 *
 * These default values are optimized for use with the Intel(R) 82599 10 GbE
 * Controller and the DPDK ixgbe PMD. Consider using other values for other
 * network controllers and/or network drivers.
 */
#define MP_DEFAULT_PTHRESH     36
#define MP_DEFAULT_RX_HTHRESH  8
#define MP_DEFAULT_TX_HTHRESH  0
#define MP_DEFAULT_WTHRESH     0
#define RX_FREE_THRESH         64
#define TX_FREE_THRESH         32
#define TX_RS_THRESH           32

static struct rte_eth_conf port_conf = {
	.rxmode = {
		.split_hdr_size = 0,
		.header_split   = 0, /* Header Split disabled */
		.hw_ip_checksum = 1, /* IP checksum offload enabled */
		.hw_vlan_filter = 0, /* VLAN filtering disabled */
		.jumbo_frame    = 0, /* Jumbo Frame Support disabled */
		.hw_strip_crc   = 0, /* CRC stripped by hardware */
	},
};

static struct rte_eth_rxconf rx_conf = {
	.rx_thresh = {
		.pthresh = MP_DEFAULT_PTHRESH,
		.hthresh = MP_DEFAULT_RX_HTHRESH,
		.wthresh = MP_DEFAULT_WTHRESH,
	},
	.rx_free_thresh = RX_FREE_THRESH,
	.rx_drop_en = 0,
};

static struct rte_eth_txconf tx_conf = {
	.tx_thresh = {
		.pthresh = MP_DEFAULT_PTHRESH,
		.hthresh = MP_DEFAULT_TX_HTHRESH,
		.wthresh = MP_DEFAULT_WTHRESH,
	},
	.tx_free_thresh = TX_FREE_THRESH,
	.tx_rs_thresh = TX_RS_THRESH,
};

/* Total number of active phy ports */
int total_ports = 0;

/*
 * Initialise all available phy ports.
 */
void
ovdk_vport_phy_init(void)
{
	int ret = 0;

	/* Scan PCI bus for recognised devices */
	ret = rte_eal_pci_probe();
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Could not probe PCI (%d)\n", ret);

	/* Get number of ports found in scan */
	total_ports = rte_eth_dev_count();
	if (total_ports == 0)
		rte_exit(EXIT_FAILURE, "No supported Ethernet devices found - "
		         "check that the IGB and/or IXGBE PMDs have been "
		         "enabled in the config file and Ethernet devices have "
		         "been bound to one of said drivers.\n");
}

/*
 * Return the total amount of phy ports on the system.
 *
 * This will always return 0 until the phy ports have been initialised, as
 * there will be no "active" phy ports.
 */
int
ovdk_vport_phy_get_max_available_phy_ports(void)
{
	return total_ports;
}

/*
 * Initialise a given phy port.
 */
int
ovdk_vport_phy_port_init(struct vport_info *vport_info,
                         unsigned port_id)
{
	struct rte_pipeline_port_in_params *port_in_params = NULL;
	struct rte_pipeline_port_out_params *port_out_params = NULL;
	struct rte_port_ethdev_writer_params *port_writer_params = NULL;
	struct rte_port_ethdev_reader_params *port_reader_params = NULL;
	struct rte_mempool *mp = NULL;
	unsigned tx_q_id, lcore_count = rte_lcore_count();
	unsigned lcore_id = 0;
	int ret = 0;

	RTE_LOG(INFO, USER1, "Initializing NIC port %u ...\n", port_id);

	mp = rte_mempool_lookup(PKTMBUF_POOL_NAME);
	if (mp == NULL)
		rte_panic("Cannot find mempool '%s' (%s)\n", PKTMBUF_POOL_NAME,
		          rte_strerror(rte_errno));

	/* Check for null type before use*/
	if(vport_info == NULL)
		rte_panic("Cannot init NIC port '%d', invalid vport info\n",
		          port_id);

	vport_info->type = OVDK_VPORT_TYPE_PHY;

	if (ovdk_args_get_max_frame_size() > OVDK_DEFAULT_MAX_FRAME_SIZE) {
		port_conf.rxmode.max_rx_pkt_len = ovdk_args_get_max_frame_size();
		port_conf.rxmode.jumbo_frame = 1;
	}

	/* Init port */
	ret = rte_eth_dev_configure(
	        port_id,
	        1,             /* Currently only one rx queue is supported */
	        lcore_count,   /* Output queue for enabled lcore */
	        &port_conf);
	if (ret < 0)
		rte_panic("Cannot init NIC port '%u' (%s)\n", port_id,
		          rte_strerror(rte_errno));

	rte_eth_promiscuous_enable(port_id);

	/* Init RX queues */
	ret = rte_eth_rx_queue_setup(
	        port_id,
	        0,
	        PORT_RX_RING_SIZE,
	        rte_eth_dev_socket_id(port_id),
	        &rx_conf,
	        mp);
	if (ret < 0)
		rte_panic("Cannot init RX for port '%u' (%s)\n", port_id,
		          rte_strerror(rte_errno));

	for (tx_q_id = 0; tx_q_id < lcore_count; tx_q_id++) {
		/* Init TX queues - One for every lcore which is enabled */
		ret = rte_eth_tx_queue_setup(
		        port_id,
		        tx_q_id,
		        PORT_TX_RING_SIZE,
		        rte_eth_dev_socket_id(port_id),
		        &tx_conf);
		if (ret < 0)
			rte_panic("Cannot init TX for port %d (%s)\n", port_id,
			          rte_strerror(rte_errno));
	}

	/* Start port */
	ret = rte_eth_dev_start(port_id);
	if (ret < 0)
		rte_panic("Cannot start port %d (%s)\n", port_id,
		          rte_strerror(rte_errno));

	port_reader_params = &vport_info->phy.port_reader_ethdev_params;
	port_reader_params->port_id = port_id;
	port_reader_params->queue_id = 0;

	port_in_params = &vport_info->port_in_params;
	port_in_params->ops = &rte_port_ethdev_reader_ops;
	port_in_params->arg_create = port_reader_params;
	port_in_params->f_action = NULL;
	port_in_params->arg_ah = &vport_info->vportid;
	port_in_params->burst_size = 32;

	for (tx_q_id = 0, lcore_id = 0; lcore_id < RTE_MAX_LCORE; lcore_id++) {
		/* Assign TX queue only to enbaled lcore */
		if (!rte_lcore_is_enabled(lcore_id))
			continue;
		port_writer_params = &vport_info->phy.port_writer_ethdev_params[lcore_id];
		port_writer_params->port_id = port_id;
		port_writer_params->queue_id = tx_q_id;
		port_writer_params->tx_burst_sz = 32;

		port_out_params = &vport_info->port_out_params[lcore_id];
		port_out_params->ops = &rte_port_ethdev_writer_ops;
		port_out_params->arg_create = port_writer_params;
		port_out_params->f_action = ovdk_stats_port_out_update;
		port_out_params->f_action_bulk = ovdk_stats_port_out_update_bulk;
		port_out_params->arg_ah = &vport_info->vportid;

		tx_q_id++;
	}

	return 0;
}
