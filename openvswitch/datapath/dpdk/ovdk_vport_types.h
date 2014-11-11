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

#ifndef __OVDK_VPORT_TYPES_H_
#define __OVDK_VPORT_TYPES_H_

#include "ovdk_config.h"

#define OVDK_MZ_VPORT_INFO       "OVDK_vport_info"
#define OVDK_MAX_VPORT_NAMESIZE  (32)

enum ovdk_vport_type {
	/*
	 * Order is important here. PHY Ports must start at 0 as they
	 * correspond to the DPDK NIC port numbers
	 */
	OVDK_VPORT_TYPE_PHY = 0,
	OVDK_VPORT_TYPE_CLIENT = OVDK_VPORT_TYPE_PHY + OVDK_MAX_PHYPORTS,
	OVDK_VPORT_TYPE_VHOST = OVDK_VPORT_TYPE_CLIENT + OVDK_MAX_CLIENTS,
	OVDK_VPORT_TYPE_KNI = OVDK_VPORT_TYPE_VHOST + OVDK_MAX_VHOSTS,
	OVDK_VPORT_TYPE_BRIDGE = OVDK_VPORT_TYPE_KNI + OVDK_MAX_KNIS,
	OVDK_VPORT_TYPE_VETH = OVDK_VPORT_TYPE_BRIDGE + OVDK_MAX_BRIDGES,
	OVDK_VPORT_TYPE_MEMNIC = OVDK_VPORT_TYPE_VETH + OVDK_MAX_VETHS,
	OVDK_VPORT_TYPE_VSWITCHD,
	OVDK_VPORT_TYPE_DISABLED,
	OVDK_VPORT_TYPE_MAX,
};

#endif /* __OVDK_VPORT_TYPES_H_ */
