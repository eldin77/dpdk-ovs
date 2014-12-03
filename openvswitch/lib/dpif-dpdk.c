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

/* Implementation of the DPIF interface for Intel DPDK vSwitch. */

#include <config.h>

#include <rte_config.h>
#include <rte_byteorder.h>
#include <rte_common.h>
#include <rte_branch_prediction.h>
#include <rte_string_fns.h>

#include <inttypes.h>
#include <stdint.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <net/if.h>

#include <unistd.h>
#include <string.h>
#include <errno.h>

#include "datapath/dpdk/ovdk_datapath_messages.h"
#include "datapath/dpdk/ovdk_flow_types.h"
#include "datapath/dpdk/ovdk_action_types.h"
#include "datapath/dpdk/ovdk_vport_types.h"
#include "datapath/dpdk/ovdk_stats_types.h"
#include "datapath/dpdk/ovdk_config.h"

#include "dpif-dpdk.h"
#include "dpif-dpdk-vport-table.h"
#include "dpdk-link.h"
#include "dpif-provider.h"
#include "flow.h"
#include "netlink.h"
#include "netdev-provider.h"
#include "odp-util.h"
#include "poll-loop.h"
#include "vlog.h"
#include "netdev-dpdk.h"
#include "dpif-dpdk-flow-table.h"

#define VLAN_CFI                   0x1000
#define VLAN_ID_MASK               0xFFF
#define VLAN_PRIO_SHIFT            13

#define DPDK_DEBUG() VLOG_DBG_RL(&dpmsg_rl, "%s: %s Line %d\n", __FILE__, __FUNCTION__, __LINE__);
#define DPIF_SOCKNAME "\0dpif-dpdk"

#define SHM_DIR "/dev/shm"
#define MEMNIC_SHM_NAME_FMT (SHM_DIR "/ovs_dpdk_not_used_%u")

#define SIGNAL_HANDLED(sock_fd, sock_msg)                                    \
    do {                                                                     \
        recvfrom(sock_fd, &sock_msg, sizeof(sock_msg), 0, NULL, NULL);       \
    } while (0);
#define TEST_AND_SET(initial_error, error)                                   \
    do {                                                                     \
        if (!(*initial_error)) {                                             \
            *initial_error = error;                                          \
        }                                                                    \
    } while (0);

VLOG_DEFINE_THIS_MODULE(dpif_dpdk);

/* This is used by the state machine found in the DPIF's 'dump' command. */
struct dpif_dpdk_port_state {
    uint32_t vportid;
};

struct dpif_dpdk_flow_state {
    struct ovdk_flow_message flow;
    struct dpif_flow_stats stats;
    struct ofpbuf actions_buf;
    struct ofpbuf key_buf;
    uint16_t flow_table_index;
};

/* Bitmask of enabled cores determined by call to dpdk_link_init */
static uint64_t pipeline_bitmask = 0;
/* Dedicated state variables for pipeline load-balancing; dynamically
 * initialised to correct values in dpif_dpdk_init.
 */
/* Last pipeline that a port was added to */
static unsigned last_used_add_pipeline = 0;
/* Last pipeline that dpif_dpdk_recv was invoked on */
static unsigned last_used_recv_pipeline = 0;

/*
 * Pipelines are contained in a bitmask.  For efficiency the min and
 * max enabled pipeline is calculated during init.
 */
static unsigned max_pipeline_id = 0;
static unsigned min_pipeline_id = 0;

static int dpdk_sock = -1;

static struct vlog_rate_limit dpmsg_rl = VLOG_RATE_LIMIT_INIT(600, 600);

static void dpif_dpdk_vport_msg_init(struct ovdk_vport_message *);
static int dpif_dpdk_vport_transact(struct ovdk_vport_message *request,
                                    unsigned pipeline_id,
                                    struct ovdk_vport_message *reply);

static void dpif_dpdk_flow_init(struct ovdk_flow_message *);
static int dpif_dpdk_flow_transact(struct ovdk_flow_message *request,
                                   unsigned pipeline_id,
                                   struct ovdk_flow_message *reply);
static void dpif_dpdk_flow_get_stats(const struct ovdk_flow_message *,
                                     struct dpif_flow_stats *);
static void dpif_dpdk_flow_key_from_flow(struct ovdk_flow_key *,
                                         const struct flow *);
static void dpif_dpdk_flow_key_to_flow(const struct ovdk_flow_key *,
                                       struct flow *);
static void dpif_dpdk_flow_actions_to_actions(const struct ovdk_action *,
                                              struct ofpbuf *,
                                              uint8_t num_actions);

static int dpif_dpdk_init(void);

static int flow_message_get_create(const struct dpif *dpif_ OVS_UNUSED,
                                    const struct nlattr *key, size_t key_len,
                                    struct ovdk_flow_message *request);
static void flow_message_put_create(struct dpif *dpif OVS_UNUSED,
                                    enum dpif_flow_put_flags flags,
                                    const struct nlattr *key, size_t key_len,
                                    const struct nlattr *actions,
                                    size_t actions_len OVS_UNUSED,
                                    struct ovdk_flow_message *request);
static int flow_message_del_create(struct ovdk_flow_message *request,
                                    const struct nlattr *key, size_t key_len);
static void create_action_set_datapath(struct ovdk_action *dpif_actions,
                           const struct nlattr *actions, const int actions_index);
static int memnic_rename_shm_object(uint32_t vportid, const char *port_name);

static inline bool is_valid_pipeline(unsigned pipeline_id);
static int next_available_pipeline(unsigned *last_used);
static int peek_next_pipeline(unsigned *last_used);
static unsigned max_available_pipeline_id(void);

static inline bool
is_valid_pipeline(unsigned pipeline_id) {
    return (1 & (pipeline_bitmask >> pipeline_id));
}

/* Determine the next pipeline following 'last_used'.
 * Set 'last_used' to the pipeline found.
 */
static int
next_available_pipeline(unsigned *last_used) {
    unsigned pipeline = 0;

    pipeline = peek_next_pipeline(last_used);
    if (pipeline != -1) {
        *last_used = pipeline;
        return 0;
    }

    return -1;
}

/* Determine the next pipeline, but don't modify pipeline state variables */
static int
peek_next_pipeline(unsigned *last_used)
{
    unsigned i = 0;
    unsigned start_search_index = 0;

    /* Check for wrap around case */
    if (unlikely(*last_used == max_pipeline_id)) {
        /* Wrap around case - next pipeline is the min_pipeline_id */
        return min_pipeline_id;
    } else {
        start_search_index = *last_used + 1;
    }

    /* Search from last use up to max pipeline */
    for (i = start_search_index; i <= max_pipeline_id; i++) {
        if (1 & (pipeline_bitmask >> i)) {
            return i;
        }
    }
    /* Wrap around case - next pipeline is the min_pipeline_id */
    return min_pipeline_id;
}

static unsigned
max_available_pipeline_id(void)
{
    unsigned max_pipeline_id = 0;

    /* Count the number of leading zeroes in the mask to determine the index
     * of the max pipeline.
     */
    max_pipeline_id = (RTE_MAX_LCORE - 1) - __builtin_clzll(pipeline_bitmask);

    return max_pipeline_id;
}

static unsigned
min_available_pipeline_id(void)
{
    /*
     * Count the number of trailing zeroes in the mask to determine the index
     * of the min pipeline.
     */
    return __builtin_ctzll(pipeline_bitmask);
}

/* Delete a port in two steps:
 * - first, delete the IN_PORT from the pipeline that the port was
 *   initially added to
 * - secondly, delete the port's OUT_PORT component from each of the other
 *   pipelines
 */
static int
del_port(odp_port_t port_no, unsigned max_pipeline)
{
    struct ovdk_vport_message request;
    int error = 0;
    int initial_error = 0;
    unsigned i = 0;
    unsigned lcore_id = 0;
    enum ovdk_vport_type vport_type = OVDK_VPORT_TYPE_DISABLED;

    dpif_dpdk_vport_msg_init(&request);
    request.cmd = OVS_VPORT_CMD_DEL;
    request.vportid = port_no;

    /* Check that the port type is not a bridge, a bridge port only
     * consists of an OUT component. We should not attempt to remove
     * an IN component for a bridge port.
     */
    error = dpif_dpdk_vport_table_entry_get_type(port_no, &vport_type);
    if (error) {
        return -error;
    }
    if(vport_type != OVDK_VPORT_TYPE_BRIDGE) {
        /* Delete the port's IN port component */
        request.flags = VPORT_FLAG_IN_PORT;
        error = dpif_dpdk_vport_table_entry_get_lcore_id(port_no, &lcore_id);
        if (error) {
            VLOG_ERR("IN port '%"PRIu32"' is not associated with a datapath core",
                     port_no);
            initial_error = error;
        }
        error = dpif_dpdk_vport_transact(&request, lcore_id, NULL);
        if (error) {
            /* Flag the error, but don't return the error code yet */
            VLOG_ERR("Failed to remove IN port '%"PRIu32"' from pipeline %u.",
                      port_no, lcore_id);
            TEST_AND_SET(&initial_error, error);
        }
    }

    /* Delete the port's OUT port component */
    request.flags = VPORT_FLAG_OUT_PORT;
    for (i = min_pipeline_id; i <= max_pipeline; i++) {
        if (is_valid_pipeline(i)) {
            error = dpif_dpdk_vport_transact(&request, i, NULL);
            if (error) {
                /* Flag the error, but don't return the error code yet */
                VLOG_ERR("Failed to remove OUT port '%"PRIu32"' from pipeline "
                         "'%u', error '%d'", port_no, i, error);
                TEST_AND_SET(&initial_error, error);
            }
        }
    }

    /* NOTE: if an error is encountered during the transaction with a datapath
     * pipeline, the 'deleted' port could still potentially be present on
     * that pipeline. Delete the entry from the vport table anyway. The
     * alternative approach is to leave the entry in the table, but the port
     * would not actually be present in any of the pipelines from which it was
     * successfully deleted.
     */
    error = dpif_dpdk_vport_table_entry_reset(port_no);
    if (error) {
        VLOG_ERR("Failed to remove port '%"PRIu32"' from vport table, error "
                 "'%d'", port_no, error);
    }

    return initial_error || error;
}

static int
dpif_dpdk_open(const struct dpif_class *dpif_classp, const char *name,
               bool create, struct dpif **dpifp)
{
    struct sockaddr_un addr = {0};
    int error = 0;
    int status = 1;

    DPDK_DEBUG()

    if (dpif_classp == NULL) {
        return EINVAL;
    }

    error = dpif_dpdk_init();
    if (error) {
        return error;
    }

    if (create && (dpdk_sock == -1)) {
        dpdk_sock = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (dpdk_sock == -1) {
            return errno;
        }
        if (ioctl(dpdk_sock, FIONBIO, &status) < 0) {
            close(dpdk_sock);
            dpdk_sock = -1;
            return errno;
        }
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        memcpy(addr.sun_path, DPIF_SOCKNAME, sizeof(DPIF_SOCKNAME));
        if (bind(dpdk_sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
            close(dpdk_sock);
            dpdk_sock = -1;
            return errno;
        }
    }

    if (create) {
        /* cleanup after last run */
        dpif_dpdk_vport_table_destroy();

        error = dpif_dpdk_vport_table_construct();
        if (error) {
            return -error;
        }

        error = dpif_dpdk_flow_table_construct();
        if (error) {
            return -error;
        }
    } else {
        error = dpif_dpdk_vport_table_exists();
        if (error) {
            return -error;
        }

        error = dpif_dpdk_vport_table_open();
        if (error) {
            return -error;
        }

        error = dpif_dpdk_flow_table_open();
        if (error) {
            return -error;
        }
    }

    *dpifp = xzalloc(sizeof(**dpifp));
    dpif_init(*dpifp, dpif_classp, name, 0, 0);

    return error;
}

static void
dpif_dpdk_close(struct dpif *dpif_)
{
    int ret = 0;

    DPDK_DEBUG()
    dpif_assert_class(dpif_, &dpif_dpdk_class);

    /* don't care about the error codes here - just ignore them */
    ret = dpif_dpdk_flow_table_close();
    if (ret) {
        VLOG_ERR("Failed to close flow table, error '%d'", ret);
    }

    ret = dpif_dpdk_vport_table_close();
    if (ret) {
        VLOG_ERR("Failed to close vport table, error '%d'", ret);
    }

    free(dpif_);
}

static int
dpif_dpdk_destroy(struct dpif *dpif_ OVS_UNUSED)
{
    int ret = 0;

    DPDK_DEBUG()

    ret = dpif_dpdk_flow_table_destroy();
    if (ret) {
        VLOG_ERR("Failed to destroy flow table, error '%d'", ret);
    }

    return dpif_dpdk_vport_table_destroy();
}

static int
dpif_dpdk_get_stats(const struct dpif *dpif_ OVS_UNUSED,
                    struct dpif_dp_stats *stats)
{
    struct flow key;
    uint64_t handle = 0;
    uint16_t index = UINT16_MAX;

    DPDK_DEBUG()

    if (stats == NULL){
        return EINVAL;
    }

    stats->n_hit = 0;
    stats->n_missed = 0;
    stats->n_lost = 0;
    stats->n_flows = 0;

    /* Calculate number of flows */
    while (dpif_dpdk_flow_table_entry_next(&key, &handle, &index) == -EAGAIN) {
        stats->n_flows++;
    }

    return 0;
}

/* Converts an OF (daemon) port type to an OD (datapath) port type */
static enum ovdk_vport_type
dpif_dpdk_odport_type(const char *type)
{
    enum ovdk_vport_type vport_type = OVDK_VPORT_TYPE_DISABLED;

    if (type == NULL) {
        return OVDK_VPORT_TYPE_DISABLED;
    }

    if (!strncmp(type, "internal", OVDK_MAX_VPORT_NAMESIZE)) {
        vport_type = OVDK_VPORT_TYPE_BRIDGE;
    } else if (!strncmp(type, "dpdkclient", OVDK_MAX_VPORT_NAMESIZE)) {
        vport_type = OVDK_VPORT_TYPE_CLIENT;
    } else if (!strncmp(type, "dpdkkni", OVDK_MAX_VPORT_NAMESIZE)) {
        vport_type = OVDK_VPORT_TYPE_KNI;
    } else if (!strncmp(type, "dpdkphy", OVDK_MAX_VPORT_NAMESIZE)) {
        vport_type = OVDK_VPORT_TYPE_PHY;
    } else if (!strncmp(type, "dpdkveth", OVDK_MAX_VPORT_NAMESIZE)) {
        vport_type = OVDK_VPORT_TYPE_VETH;
    } else if (!strncmp(type, "dpdkvhost", OVDK_MAX_VPORT_NAMESIZE)) {
        vport_type = OVDK_VPORT_TYPE_VHOST;
    } else if (!strncmp(type, "dpdkmemnic", OVDK_MAX_VPORT_NAMESIZE)) {
        vport_type = OVDK_VPORT_TYPE_MEMNIC;
    } else {
        VLOG_ERR("Failed to get ODP type from OFP type '%s'", type);
    }

    return vport_type;
}

/* Converts an OD (datapath) port type to an OF (daemon) port type */
static int
dpif_dpdk_ofport_type(enum ovdk_vport_type type, char *vport_type)
{
    if (vport_type == NULL) {
        return EINVAL;
    }

    switch(type) {
    case OVDK_VPORT_TYPE_BRIDGE:
        strncpy(vport_type, "internal", OVDK_MAX_VPORT_NAMESIZE);
        break;
    case OVDK_VPORT_TYPE_CLIENT:
        strncpy(vport_type, "dpdkclient", OVDK_MAX_VPORT_NAMESIZE);
        break;
    case OVDK_VPORT_TYPE_KNI:
        strncpy(vport_type, "dpdkkni", OVDK_MAX_VPORT_NAMESIZE);
        break;
    case OVDK_VPORT_TYPE_PHY:
        strncpy(vport_type, "dpdkphy", OVDK_MAX_VPORT_NAMESIZE);
        break;
    case OVDK_VPORT_TYPE_VETH:
        strncpy(vport_type, "dpdkveth", OVDK_MAX_VPORT_NAMESIZE);
        break;
    case OVDK_VPORT_TYPE_VHOST:
        strncpy(vport_type, "dpdkvhost", OVDK_MAX_VPORT_NAMESIZE);
        break;
    case OVDK_VPORT_TYPE_MEMNIC:
        strncpy(vport_type, "dpdkmemnic", OVDK_MAX_VPORT_NAMESIZE);
        break;
    case OVDK_VPORT_TYPE_DISABLED:
    case OVDK_VPORT_TYPE_MAX:
    case OVDK_VPORT_TYPE_VSWITCHD:
    default:
        VLOG_ERR("Failed to get OFP type from ODP type '%d'", type);
        return EINVAL;
        break;
    }

    return 0;
};

static int
memnic_rename_shm_object(uint32_t vportid, const char *port_name)
{
    char old_name[PATH_MAX];
    char new_name[PATH_MAX];

    /* Remove any old shm object with the same port name. Ignore ENOENT error
     * (No such file or directory) meaning that the shm object didn't exist
     * in the first place */
    if (shm_unlink(port_name) < 0 && errno != ENOENT) {
        VLOG_ERR("Could not unlink previous shm object '%s'\n", port_name);
        return errno;
    }

    /* Calculate names of old shm (created in datapath) and new shm name
     * from the port name */
    snprintf(old_name, sizeof(old_name), MEMNIC_SHM_NAME_FMT, vportid);
    snprintf(new_name, sizeof(new_name), "%s/%s", SHM_DIR, port_name);

    /* Do the shm object renaming */
    if (rename(old_name, new_name) < 0) {
        VLOG_ERR("Could not rename shm object '%s' to '%s'\n",
                old_name, new_name);
        return errno;
    }

    return 0;
}

static int
dpif_dpdk_port_add(struct dpif *dpif_, struct netdev *netdev,
                   odp_port_t *port_no)
{
    struct ovdk_vport_message request = {0};
    struct ovdk_vport_message reply = {0};
    uint32_t vportid = OVDK_MAX_VPORTS;
    struct netdev_dpdk_phyport *netdev_dpdk = NETDEV_DPDK_PHYPORT_CAST(netdev);
    unsigned pipeline_id = 0;
    unsigned max_out_port = 0;
    int error = 0;
    unsigned i = 0;

    dpif_assert_class(dpif_, &dpif_dpdk_class);

    DPDK_DEBUG()

    if ((netdev == NULL) || (port_no == NULL)) {
        VLOG_ERR("Invalid params");
        return EINVAL;
    }

    /* For vhost ports the port name is directly linked to the
     * tap device name, so the length of the port name must not
     * be greater than IFNAMSIZ
     */
    if (dpif_dpdk_odport_type(netdev_get_type(netdev)) == OVDK_VPORT_TYPE_VHOST
                        && strlen(netdev_get_name(netdev)) > IFNAMSIZ) {
        VLOG_ERR("Invalid VHOST name length");
        return EINVAL;
    }

    error = next_available_pipeline(&last_used_add_pipeline);
    if (error) {
        VLOG_ERR("Cannot find next available pipeline, error '%d'", error);
        return ENODEV;
    }

    pipeline_id = last_used_add_pipeline;

    /* Populate 'request' to send to datapath */
    dpif_dpdk_vport_msg_init(&request);
    request.cmd = OVS_VPORT_CMD_NEW;
    strncpy(request.port_name, netdev_get_name(netdev),
            sizeof(request.port_name) - 1);
    request.type = dpif_dpdk_odport_type(netdev_get_type(netdev));

    if (request.type == OVDK_VPORT_TYPE_DISABLED) {
        VLOG_ERR("Requested port type 'disabled'");
        return ENODEV;
    }

    if (request.type == OVDK_VPORT_TYPE_PHY) {
        /*
         * The vportid for phy ports starts at 0 so there is a 1:1 mapping
         * between phy port ids and the equivalent vportid. As we want to
         * request a specific physical port, we need to check if it is already
         * added.
         */
        vportid = netdev_dpdk->port_id;
    }

    /*
     * Attempt to add to vport table. If the physical port is not available
     * then we fail as it is already in use.
     */
    error = dpif_dpdk_vport_table_entry_add(request.type,
                                            &pipeline_id,
                                            netdev_get_name(netdev),
                                            &vportid);
    if (error) {
        VLOG_ERR("Unable to add port to vport table, error '%d'", error);
        if (error == -EBUSY) {
            return -error;
        }
        return ENODEV;
    }

    request.vportid = vportid;

    /* Currently bridge ports are only out ports, do not request an in port
     * for a type bridge.
     */
    if (request.type != OVDK_VPORT_TYPE_BRIDGE) {
        request.flags = VPORT_FLAG_IN_PORT;
    }

    /* Add port to the datapath. A port consists of two parts: In port
     * component for handling inbound traffic, and Out port which handles
     * outbound traffic. The underlying datapath is comprised of one or more
     * pipelines (i.e. cores) - consequently, the Out port must be added to
     * each of these pipelines to support Output actions, while the In port
     * only needs to be added to the pipeline assigned to handle inbound
     * traffic for that port.
     */
    /* In port, check that in port flag is set before transacting */
    if (request.flags == VPORT_FLAG_IN_PORT) {
        error = dpif_dpdk_vport_transact(&request, pipeline_id, &reply);
        if (error) {
            /* Reset table entry here if datapath fails to add port */
            dpif_dpdk_vport_table_entry_reset(vportid);
            VLOG_ERR("Unable to successfully add IN port to datapath, "
                     "error '%d'", error);
            return error;
        }
    }

    VLOG_DBG("Added vportid '%d' as IN port to pipeline '%d'", vportid, pipeline_id);

    /* Modify message and add output ports to available datapath pipelines.
     * Adding a bridge port is handled here.
     */
    request.flags = VPORT_FLAG_OUT_PORT;
    for (i = min_pipeline_id; i <= max_pipeline_id; i++) {
        if (is_valid_pipeline(i)) {
            /* If an error is encountered, delete all previously-added
             * instances of this port from the appropriate datapath
             * pipelines.
             */
            error = dpif_dpdk_vport_transact(&request, i, &reply);
            if (unlikely(error)) {
                del_port(vportid, max_out_port);
                VLOG_ERR("Unable to successfully add out port to datapath "
                         "on pipeline '%u', error '%d'", i, error);
                return error;
            }
            max_out_port = i;
            VLOG_DBG("Added vportid '%d' as OUT port to pipeline '%d'", vportid, i);
        }
    }

    /* Only set *port_no if port has been successfully added */
    *port_no = vportid;

    if (request.type == OVDK_VPORT_TYPE_MEMNIC) {
        error = memnic_rename_shm_object(reply.vportid,
                                         netdev_get_name(netdev));
    }

    if (unlikely(error)) {
        VLOG_ERR("Unable to add port '%u' to "
                 "in port pipeline '%u', error '%d'", vportid, pipeline_id, error);
    } else {
        VLOG_DBG("Added port '%u', '%s', in port pipeline '%u'",
                  vportid, request.port_name, pipeline_id);
    }

    return error;
}

static int
dpif_dpdk_port_del(struct dpif *dpif_ OVS_UNUSED, odp_port_t port_no)
{
    int error = 0;
    unsigned pipeline_id = 0;

    dpif_assert_class(dpif_, &dpif_dpdk_class);

    DPDK_DEBUG()

    /* Ensure that 'port_no' is present in the dpif_dpdk_vport_table */
    error = dpif_dpdk_vport_table_entry_get_lcore_id(port_no, &pipeline_id);
    if (error) {
        VLOG_ERR("Unable to get port '%u' pipeline, error '%d'", port_no, error);
        return -error;
    }

    error = del_port(port_no, max_pipeline_id);
    if (unlikely(error)) {
        VLOG_ERR("Unable to delete port '%u', error '%d'", port_no, error);
    } else {
        VLOG_DBG("Deleted port '%u'", port_no);
    }

    return error;
}

static int
dpif_dpdk_port_query_by_number(const struct dpif *dpif,
                               odp_port_t port_no,
                               struct dpif_port *dpif_port)
{
    char type[OVDK_MAX_VPORT_NAMESIZE] = {0};
    enum ovdk_vport_type vport_type = OVDK_VPORT_TYPE_DISABLED;
    char name[OVDK_MAX_VPORT_NAMESIZE] = {0};
    int error  = 0;

    DPDK_DEBUG()

    if (dpif == NULL){
        return EINVAL;
    }

    error = dpif_dpdk_vport_table_entry_get_type(port_no, &vport_type);
    if (error) {
        return -error;
    }

    error = dpif_dpdk_vport_table_entry_get_name(port_no, &name[0]);
    if (error) {
        return -error;
    }

    error = dpif_dpdk_ofport_type(vport_type, type);
    if (error) {
        return error; /* Invalid port type */
    }

    /* dpif_port may be NULL - only poplulate the structure
     * if non-NULL parameter specified
     */
    if (dpif_port) {
        dpif_port->name = xstrdup(name);
        dpif_port->type = xstrdup(type);
        dpif_port->port_no = port_no;
    }

    return 0;
}

static int
dpif_dpdk_port_query_by_name(const struct dpif *dpif,
                             const char *devname, struct dpif_port *dpif_port)
{
    char type[OVDK_MAX_VPORT_NAMESIZE] = {0};
    enum ovdk_vport_type vport_type = OVDK_VPORT_TYPE_DISABLED;
    int error = 0;
    uint32_t vportid = 0;

    DPDK_DEBUG()

    if (dpif == NULL || devname == NULL) {
        return EINVAL;
    }

    error = dpif_dpdk_vport_table_entry_get_vportid(devname, &vportid);
    if (error) {
        return -error;
    }

    error = dpif_dpdk_vport_table_entry_get_type(vportid, &vport_type);
    if (error) {
        return -error;
    }

    error = dpif_dpdk_ofport_type(vport_type, type);
    if (error) {
        return error; /* Invalid port type */
    }

    /* dpif_port may be NULL - only poplulate the structure
     * if non-NULL parameter specified
     */
    if (dpif_port) {
        dpif_port->name = xstrdup(devname);
        dpif_port->type = xstrdup(type);
        dpif_port->port_no = vportid;
    }

    return 0;
}

static uint32_t
dpif_dpdk_get_max_ports(const struct dpif *dpif OVS_UNUSED)
{
    DPDK_DEBUG()

    return OVDK_MAX_VPORTS;
}

static int
dpif_dpdk_port_dump_start(const struct dpif *dpif_,
                          void **statep)
{
    struct dpif_dpdk_port_state *state;

    DPDK_DEBUG()

    if (dpif_ == NULL || statep == NULL) {
        return EINVAL;
    }

    *statep = state = xmalloc(sizeof(*state));

    state->vportid = 0;

    return 0;
}

static int
dpif_dpdk_port_dump_next(const struct dpif *dpif,
                         void *state_,
                         struct dpif_port *dpif_port)
{
    struct dpif_dpdk_port_state *state = state_;
    char name[OVDK_MAX_VPORT_NAMESIZE];
    char type[OVDK_MAX_VPORT_NAMESIZE];
    int error = 0;
    uint32_t vportid = 0;
    enum ovdk_vport_type vport_type = OVDK_VPORT_TYPE_DISABLED;

    DPDK_DEBUG()

    if (dpif == NULL || state_ == NULL || dpif_port == NULL) {
        return EINVAL;
    }

    vportid = state->vportid;

    error = dpif_dpdk_vport_table_entry_get_next_inuse(&vportid);
    if (error) {
        if (error == -ENOENT){
            return EOF;
        }
        return -error;
    }

    error = dpif_dpdk_vport_table_entry_get_name(vportid, &name[0]);
    if (error) {
        return -error;
    }

    error = dpif_dpdk_vport_table_entry_get_type(vportid, &vport_type);
    if (error) {
        return -error;
    }

    error = dpif_dpdk_ofport_type(vport_type, type);
    if (error) {
        return error; /* Invalid port type */
    }

    dpif_port->type = xstrdup(type);
    dpif_port->name = xstrdup(name);
    dpif_port->port_no = vportid;

    /* start looking from next 'vportid' during the next call to this function
     * by saving vportid in 'state' */
    state->vportid = vportid + 1;

    return 0;
}

static int
dpif_dpdk_port_dump_done(const struct dpif *dpif_ OVS_UNUSED,
                         void *state_)
{
    struct dpif_dpdk_vport_state *state = state_;

    DPDK_DEBUG()

    free(state);

    return 0;
}

static int
dpif_dpdk_port_poll(const struct dpif *dpif_ OVS_UNUSED,
                    char **devnamep OVS_UNUSED)
{
    DPDK_DEBUG()

    return EAGAIN;
}

static void
dpif_dpdk_port_poll_wait(const struct dpif *dpif_ OVS_UNUSED)
{
    DPDK_DEBUG()
}

/* Return statistics for vport associated with 'name'.
 * Invoked by netdev_dpdk_get_stats()
 */
int
dpif_dpdk_port_get_stats(const char *name, struct ovdk_port_stats *stats)
{
    struct ovdk_vport_message request, reply;
    unsigned pipeline_id = 0;
    uint32_t vportid = 0;
    int error = 0;

    DPDK_DEBUG()

    if (name == NULL || stats == NULL) {
        return EINVAL;
    }

    error = dpif_dpdk_vport_table_entry_get_vportid(name, &vportid);
    if (error) {
        return -error;
    }

    error = dpif_dpdk_vport_table_entry_get_lcore_id(vportid, &pipeline_id);
    if (error) {
        return -error;
    }

    request.cmd = OVS_VPORT_CMD_GET;
    request.flags = 0;
    request.vportid = vportid;

    error = dpif_dpdk_vport_transact(&request, pipeline_id, &reply);
    if (error) {
        VLOG_ERR("Failed to retrieve stats for port '%"PRIu32"' from pipeline"
                 " '%u', error '%d'", vportid, pipeline_id, error);
        return error;
    }

   *stats = reply.stats;

    return error;
}

/*
 * This function will initialize an ovdk_flow_message for get.
 */
static int
flow_message_get_create(const struct dpif *dpif_ OVS_UNUSED,
                        const struct nlattr *key, size_t key_len,
                        struct ovdk_flow_message *request)
{
    int ret = 0;
    struct flow flow;
    uint64_t flow_handle = 0;
    enum odp_key_fitness fitness_error = ODP_FIT_ERROR;

    dpif_dpdk_flow_init(request);
    request->cmd = OVS_FLOW_CMD_GET;

    fitness_error = odp_flow_key_to_flow(key, key_len, &flow);
    if (fitness_error == ODP_FIT_ERROR)
        return EINVAL;

    dpif_dpdk_flow_key_from_flow(&request->key, &flow);

    ret = dpif_dpdk_flow_table_entry_find(&flow, &flow_handle);
    if (ret) {
        return -ret;
    }
    request->flow_handle = flow_handle;

    return ret;
}

static int
dpif_dpdk_flow_get(const struct dpif *dpif_,
                   const struct nlattr *key, size_t key_len,
                   struct ofpbuf **actionsp, struct dpif_flow_stats *stats)
{
    struct ovdk_flow_message request = {0};
    struct ovdk_flow_message reply = {0};
    int error = 0;
    unsigned pipeline_id = 1;

    DPDK_DEBUG()

    if (dpif_ != NULL) {
        dpif_assert_class(dpif_, &dpif_dpdk_class);
    }

    if (key == NULL) {
        return EINVAL;
    }

    error = flow_message_get_create(dpif_, key, key_len, &request);
    if (error) {
        return error;
    }

    error = dpif_dpdk_vport_table_entry_get_lcore_id(request.key.in_port,
                                                     &pipeline_id);
    if (error) {
        return error;
    }

    error = dpif_dpdk_flow_transact(&request, pipeline_id, &reply);
    if (!error) {
        if (stats) {
            dpif_dpdk_flow_get_stats(&reply, stats);
        }
        if (actionsp) {
            *actionsp = ofpbuf_new(1024);
            dpif_dpdk_flow_actions_to_actions(reply.actions,
                                              *actionsp,
                                              reply.num_actions);
        }
    }

    return error;
}

static uint8_t
dpif_dpdk_create_actions(struct ovdk_action *dpif_actions,
                         const struct nlattr *actions, size_t actions_len)
{
    const struct nlattr *a;
    struct ovs_action_push_vlan *vlan = NULL;
    size_t len = 0;
    int i = 0;

    if (actions_len == 0) {
        dpif_actions[i].type = OVDK_ACTION_DROP;
        return 1;
    }

    if (likely(actions != NULL)) {
        NL_ATTR_FOR_EACH_UNSAFE(a, len, actions, actions_len) {
            switch (nl_attr_type(a)) {
            case OVS_ACTION_ATTR_OUTPUT:
                dpif_actions[i].type = OVDK_ACTION_OUTPUT;
                dpif_actions[i].data.output.port = nl_attr_get_u32(a);
                ++i;
                break;
            case OVS_ACTION_ATTR_POP_VLAN:
                dpif_actions[i].type = OVDK_ACTION_POP_VLAN;
                ++i;
                break;
            case OVS_ACTION_ATTR_PUSH_VLAN:
                dpif_actions[i].type = OVDK_ACTION_PUSH_VLAN;
                vlan = (struct ovs_action_push_vlan *)
                    nl_attr_get_unspec(a, sizeof(struct ovs_action_push_vlan));
                dpif_actions[i].data.vlan.tpid = vlan->vlan_tpid;
                dpif_actions[i].data.vlan.tci = vlan->vlan_tci;
                ++i;
                break;
            case OVS_ACTION_ATTR_SET:
                create_action_set_datapath(dpif_actions, nl_attr_get(a), i);
                ++i;
                break;
            case OVS_ACTION_ATTR_USERSPACE:
                dpif_actions[i].type = OVDK_ACTION_VSWITCHD;
                dpif_actions[i].data.vswitchd.pid = nl_attr_get_u32(nl_attr_get(a));
                ++i;
            default:
                /* unsupported action */
                break;
            }
        }
    }

    return i;
}

static void
create_action_set_datapath(struct ovdk_action *dpif_actions,
                           const struct nlattr *actions, const int action_index)
{
    const int i = action_index;
    enum ovs_key_attr type = nl_attr_type(actions);

    switch (type) {
    case OVS_KEY_ATTR_PRIORITY:
    case OVS_KEY_ATTR_IPV6:
        /* not implemented */
        break;
    case OVS_KEY_ATTR_ETHERNET:
        dpif_actions[i].type = OVDK_ACTION_SET_ETHERNET;
        dpif_actions[i].data.ethernet = *(struct ovs_key_ethernet *)(
            nl_attr_get_unspec(actions, sizeof(struct ovs_key_ethernet)));
        break;
    case OVS_KEY_ATTR_IPV4:
        dpif_actions[i].type = OVDK_ACTION_SET_IPV4;
        dpif_actions[i].data.ipv4 = *(struct ovs_key_ipv4 *)(
            nl_attr_get_unspec(actions, sizeof(struct ovs_key_ipv4)));
        break;
    case OVS_KEY_ATTR_TCP:
        dpif_actions[i].type = OVDK_ACTION_SET_TCP;
        dpif_actions[i].data.tcp = *(struct ovs_key_tcp *)(
            nl_attr_get_unspec(actions, sizeof(struct ovs_key_tcp)));
        break;

     case OVS_KEY_ATTR_UDP:
        dpif_actions[i].type = OVDK_ACTION_SET_UDP;
        dpif_actions[i].data.udp = *(struct ovs_key_udp *)(
            nl_attr_get_unspec(actions, sizeof(struct ovs_key_udp)));
        break;
     case OVS_KEY_ATTR_UNSPEC:
     case OVS_KEY_ATTR_ENCAP:
     case OVS_KEY_ATTR_ETHERTYPE:
     case OVS_KEY_ATTR_IN_PORT:
     case OVS_KEY_ATTR_VLAN:
     case OVS_KEY_ATTR_ICMP:
     case OVS_KEY_ATTR_ICMPV6:
     case OVS_KEY_ATTR_ARP:
     case OVS_KEY_ATTR_ND:
     case OVS_KEY_ATTR_SKB_MARK:
     case OVS_KEY_ATTR_TUNNEL:
     case OVS_KEY_ATTR_SCTP:
     case OVS_KEY_ATTR_MPLS:
     case OVS_KEY_ATTR_TCP_FLAGS:
     case __OVS_KEY_ATTR_MAX:
     default:
        OVS_NOT_REACHED();
    }
}

/*
 * Initialise an 'ovdk_flow_message' for 'flow_put'.
 */
static void
flow_message_put_create(struct dpif *dpif OVS_UNUSED,
                        enum dpif_flow_put_flags flags,
                        const struct nlattr *key, size_t key_len,
                        const struct nlattr *actions,
                        size_t actions_len,
                        struct ovdk_flow_message *request)
{
    struct flow flow;

    DPDK_DEBUG()

    dpif_dpdk_flow_init(request);
    request->cmd = OVS_FLOW_CMD_NEW;

    odp_flow_key_to_flow(key, key_len, &flow);
    dpif_dpdk_flow_key_from_flow(&request->key, &flow);

    request->num_actions = dpif_dpdk_create_actions(request->actions,
                            actions, actions_len);

    if (flags & DPIF_FP_ZERO_STATS) {
        request->clear = true;
    }

    request->flags = 0;
    if (flags & DPIF_FP_MODIFY) {
        request->flags |= NLM_F_REPLACE;
    }
    if (flags & DPIF_FP_CREATE) {
        request->flags |= NLM_F_CREATE;
    }
}

static int
dpif_dpdk_flow_put(struct dpif *dpif_, const struct dpif_flow_put *put)
{
    struct ovdk_flow_message request = {0};
    struct ovdk_flow_message reply = {0};
    struct flow flow;
    struct ovdk_flow_key key = {0};
    int error = 0;
    unsigned pipeline_id = 1;
    uint64_t flow_handle = 0;
    enum odp_key_fitness fitness_error = ODP_FIT_ERROR;
    bool in_use = false;

    DPDK_DEBUG()

    if (dpif_ != NULL) {
        dpif_assert_class(dpif_, &dpif_dpdk_class);
    }

    if (put == NULL || put->key == NULL) {
        VLOG_ERR("Invalid put command");
        return EINVAL;
    }

    fitness_error = odp_flow_key_to_flow(put->key, put->key_len, &flow);
    if (fitness_error == ODP_FIT_ERROR) {
        VLOG_ERR("Key failed fitness test");
        return EINVAL;
    }

    dpif_dpdk_flow_key_from_flow(&key, &flow);

    /*
     * We can only put a flow for a port that is inuse
     */
    error = dpif_dpdk_vport_table_entry_get_inuse(key.in_port, &in_use);
    if (error) {
        VLOG_ERR("Unable to get port in-use status for flow, error '%d'",
                 error);
        return EINVAL;
    } else if (!in_use) {
        VLOG_ERR("Invalid port - port is not in use.");
        return EINVAL;
    }

    error = dpif_dpdk_vport_table_entry_get_lcore_id(key.in_port, &pipeline_id);
    if (error) {
        VLOG_ERR("Unable to get port lcore id for flow, error '%d'", error);
        return EINVAL;
    }

    flow_message_put_create(dpif_, put->flags, put->key,
                            put->key_len, put->actions,
                            put->actions_len, &request);

    /* We must send datapath at least one action */
    if (request.num_actions == 0) {
        VLOG_ERR("No actions to be sent to datapath");
        return EINVAL;
    }

    error = dpif_dpdk_flow_transact(&request, pipeline_id, &reply);
    if (error) {
        VLOG_ERR("Transact to datapath failed, error '%d'", error);
        return -error;
    }

    if (put->stats) {
        dpif_dpdk_flow_get_stats(&reply, put->stats);
    }

    if (request.flags & NLM_F_REPLACE) {
        error = dpif_dpdk_flow_table_entry_del(&flow);
        if (error == -ENOENT) {
            /* Key not found during attempted delete */
            if ((request.flags & NLM_F_CREATE) == 0) {
                /*
                 * The flag indicates that we should not create the flow if
                 * the one it is intended to replace is not found
                 */
                VLOG_ERR("Unable to find existing flow in flow table, error "
                         "'%d'", error);
                return error;
            }
        }
    }

    flow_handle = reply.flow_handle;
    error = dpif_dpdk_flow_table_entry_add(&flow, &flow_handle);
    if (unlikely(error)) {
        VLOG_ERR("Unable to add flow to flow table, error '%d'", error);
    } else {
        VLOG_DBG("Added flow to pipeline '%d'", pipeline_id);
    }


    return -error;
}

/*
 * This function will initialize an ovdk_flow_message for del.
 */
static int
flow_message_del_create(struct ovdk_flow_message *request,
                        const struct nlattr *key, size_t key_len)
{
    int ret = 0;
    struct flow flow;
    uint64_t flow_handle = 0;

    DPDK_DEBUG()

    dpif_dpdk_flow_init(request);
    request->cmd = OVS_FLOW_CMD_DEL;

    odp_flow_key_to_flow(key, key_len, &flow);
    dpif_dpdk_flow_key_from_flow(&request->key, &flow);

    ret = dpif_dpdk_flow_table_entry_find(&flow, &flow_handle);
    if (ret) {
        return ret;
    }

    request->flow_handle = flow_handle;

    return ret;
}

static int
dpif_dpdk_flow_del(struct dpif *dpif_ ,
                   const struct dpif_flow_del *del)
{
    struct ovdk_flow_message request = {0};
    struct ovdk_flow_message reply = {0};
    struct flow flow;
    struct ovdk_flow_key key = {0};
    int error = 0;
    enum odp_key_fitness fitness_error = ODP_FIT_ERROR;
    unsigned pipeline_id = 1;

    DPDK_DEBUG()

    if (dpif_ != NULL) {
        dpif_assert_class(dpif_, &dpif_dpdk_class);
    }

    if (del == NULL || del->key == NULL) {
        VLOG_ERR("Invalid del command");
        return EINVAL;
    }

    fitness_error = odp_flow_key_to_flow(del->key, del->key_len, &flow);
    if (fitness_error == ODP_FIT_ERROR) {
        VLOG_ERR("Key failed fitness test");
        return EINVAL;
    }

    dpif_dpdk_flow_key_from_flow(&key, &flow);
    error = dpif_dpdk_vport_table_entry_get_lcore_id(key.in_port, &pipeline_id);
    if (error) {
        VLOG_ERR("Unable to get port lcore id for flow, error '%d'", error);
        return EINVAL;
    }

    error = flow_message_del_create(&request, del->key, del->key_len);
    if (error) {
        VLOG_ERR("Unable to create del message, error '%d'", error);
        return -error;
    }

    error = dpif_dpdk_flow_transact(&request, pipeline_id, &reply);
    if (!error) {
        if (del->stats) {
            dpif_dpdk_flow_get_stats(&reply, del->stats);
        }
    } else {
        VLOG_ERR("Transact to datapath failed, error '%d'", error);
    }

    error = dpif_dpdk_flow_table_entry_del(&flow);
    if (unlikely(error)) {
        VLOG_ERR("Unable to delete flow from flow table, error '%d'", error);
    } else {
        VLOG_DBG("Deleted flow from pipeline '%d'", pipeline_id);
    }

    return error;
}


static int
dpif_dpdk_flow_flush(struct dpif *dpif_)
{
    int ret = -EAGAIN;
    struct flow flow;
    uint64_t flow_handle;
    uint16_t flow_index = UINT16_MAX;

    struct ovdk_flow_key key = {0};
    unsigned pipeline_id = 1;

    struct ovdk_flow_message request = {0};
    int error = 0;

    DPDK_DEBUG()

    if (dpif_ != NULL) {
        dpif_assert_class(dpif_, &dpif_dpdk_class);
    }

    /* Loop through each entry until no more entries in flow table */
    while (1) {
        /* Find the ovs flow and flow_handle for the next flow entry index */
        ret = dpif_dpdk_flow_table_entry_next(&flow,
                                              &flow_handle,
                                              &(flow_index));
        if (ret != -EAGAIN) {
            return -ret;
        }

        /* Get the lcoreid */
        dpif_dpdk_flow_key_from_flow(&key, &flow);
        error = dpif_dpdk_vport_table_entry_get_lcore_id(key.in_port,
                                                         &pipeline_id);
        if (error) {
            VLOG_ERR("Unable to get flow pipeline, error '%d'", error);
            return -error;
        }

        /* Create the del message */
        dpif_dpdk_flow_init(&request);
        request.cmd = OVS_FLOW_CMD_DEL;
        request.flow_handle = flow_handle;
        dpif_dpdk_flow_key_from_flow(&request.key, &flow);

        /* Send the del message */
        error = dpif_dpdk_flow_transact(&request, pipeline_id, NULL);
        if (error) {
            VLOG_ERR("Transact to datapath failed, error '%d'", error);
            return error;
        }

        /* Delete entry from flow table */
        error = dpif_dpdk_flow_table_entry_del(&flow);
        if (error) {
            VLOG_ERR("Unable to delete flow from flow table, "
                     "error '%d'", error);
            return -error;
        } else {
            VLOG_DBG("Deleted flow from pipeline '%d'", pipeline_id);
        }
    }

    return error;
}

static int
dpif_dpdk_flow_dump_start(const struct dpif *dpif_, void **statep)
{

    struct dpif_dpdk_flow_state *state = NULL;

    DPDK_DEBUG()

    if (dpif_ != NULL) {
        dpif_assert_class(dpif_, &dpif_dpdk_class);
    }

    /* Maintains state between iterations of flow dump. */
    *statep = state = xmalloc(sizeof(*state));
    if (state == NULL) {
        return ENOMEM;
    }

    dpif_dpdk_flow_init(&state->flow);
    state->flow.cmd = OVS_FLOW_CMD_GET;

    state->flow_table_index = UINT16_MAX;

    memset(&state->stats, 0, sizeof(state->stats));

    /* Initially set ofpbuf size to zero. */
    ofpbuf_init(&state->actions_buf, 0);
    ofpbuf_init(&state->key_buf, 0);

    return 0;
}

static int
dpif_dpdk_flow_dump_next(const struct dpif *dpif_ , void *state_,
                         const struct nlattr **key, size_t *key_len,
                         const struct nlattr **mask,
                         size_t *mask_len,
                         const struct nlattr **actions, size_t *actions_len,
                         const struct dpif_flow_stats **stats)
{
    struct dpif_dpdk_flow_state *state = NULL;
    struct ovdk_flow_message reply = {0};
    struct flow flow;
    int error = 0;
    unsigned pipeline_id = 1;
    int ret =0;
    uint64_t flow_handle;

    DPDK_DEBUG()

    if (dpif_ != NULL) {
        dpif_assert_class(dpif_, &dpif_dpdk_class);
    }

    if (state_ == NULL) {
        return EINVAL;
    }
    state = state_; /* Get state from prev iteration */

    /*Find the ovs flow and flow_handle for the next flow entry index*/
    ret = dpif_dpdk_flow_table_entry_next(&flow,
                                          &flow_handle,
                                          &(state->flow_table_index));
    if (ret != -EAGAIN) {
        if (ret == 0) {
            /* We have not found any entries. Return EOF to indicate that
             * dpif_dpdk_flow_dump_next() should not be called again. */
            return EOF;
        } else {
            /* We have encountered an error. Return a positive errno to
             * indicate this. */
            return -ret;
        }
    }

    /* Store the pf flow_handle in the flow message */
    state->flow.flow_handle = flow_handle;

    /*Get the ovdk key from the ovs flow*/
    dpif_dpdk_flow_key_from_flow(&state->flow.key, &flow);

    /*Get the lcore Id for this OVDK in_port*/
    error = dpif_dpdk_vport_table_entry_get_lcore_id(state->flow.key.in_port,
                                                        &pipeline_id);
    if (error) {
        return -error;
    }

    /*Send the message and get the reply*/
    error = dpif_dpdk_flow_transact(&state->flow, pipeline_id, &reply);
    if (error) {
        return -error;
    }

    /* If actions, key or stats are not null, retrieve from state. */
    if (actions) {
        ofpbuf_reinit(&state->actions_buf, 0); /* zero buf again */
        dpif_dpdk_flow_actions_to_actions(reply.actions,
                                          &state->actions_buf,
                                          reply.num_actions);
        *actions = state->actions_buf.data;
        *actions_len = state->actions_buf.size;
    }
    if (key) {
        ofpbuf_reinit(&state->key_buf, 0); /* zero buf again */
        odp_flow_key_from_flow(&state->key_buf, &flow, flow.in_port.odp_port);
        *key = state->key_buf.data;
        *key_len = state->key_buf.size;
    }
    if (stats) {
        dpif_dpdk_flow_get_stats(&reply, &state->stats);
        *stats = &state->stats;
    }

    /*
     * Must explicitly set mask to null here otherwise key attributes are not
     * handled by other functions as they are incorrectly masked out.
     */
    if (mask) {
        *mask = NULL;
        *mask_len = 0;
    }

    return error;
}

static int
dpif_dpdk_flow_dump_done(const struct dpif *dpif , void *state_)
{
    struct dpif_dpdk_flow_state *state = NULL;

    DPDK_DEBUG()

    if (dpif != NULL)
        dpif_assert_class(dpif, &dpif_dpdk_class);

    if (state_ == NULL) {
        return EINVAL;
    }

    state = state_;

    ofpbuf_uninit(&state->actions_buf);
    ofpbuf_uninit(&state->key_buf);
    free(state);

    return 0;
}

static int
dpif_dpdk_execute(struct dpif *dpif_ OVS_UNUSED,
                  const struct dpif_execute *execute)
{
    struct ovdk_message request = {0};
    int error = 0;
    unsigned pipeline_id = 0;
    struct flow flow;
    struct ovdk_flow_key key = {0};
    enum odp_key_fitness fitness_error = ODP_FIT_ERROR;

    DPDK_DEBUG()

    if (execute == NULL || execute->packet == NULL) {
        return EINVAL;
    }

    fitness_error = odp_flow_key_to_flow(execute->key, execute->key_len, &flow);
    if (fitness_error == ODP_FIT_ERROR) {
        return EINVAL;
    }

    dpif_dpdk_flow_key_from_flow(&key, &flow);
    error = dpif_dpdk_vport_table_entry_get_lcore_id(key.in_port, &pipeline_id);
    if (error) {
        return -error;
    }

    request.type = OVDK_PACKET_CMD_FAMILY;
    request.packet_msg.num_actions = dpif_dpdk_create_actions(
            request.packet_msg.actions,
            execute->actions,
            execute->actions_len);

    error = dpdk_link_send(&request, execute->packet, pipeline_id);

    return error;
}

static int
dpif_dpdk_recv_set(struct dpif *dpif_ OVS_UNUSED,
                   bool enable OVS_UNUSED)
{
    DPDK_DEBUG()

    return 0;
}

static int
dpif_dpdk_queue_to_priority(const struct dpif *dpif OVS_UNUSED,
                            uint32_t queue_id OVS_UNUSED,
                            uint32_t *priority OVS_UNUSED)
{
    DPDK_DEBUG()

    return 0;
}

static int
dpif_dpdk_recv(struct dpif *dpif_ OVS_UNUSED,
               struct dpif_upcall *upcall,
               struct ofpbuf *buf)
{
    struct ofpbuf key = {0};
    struct flow flow;
    struct ovdk_upcall info = {0};
    int error = 0;
    int sock_msg = 0;
    unsigned pipeline_id = 0;
    unsigned initial_pipeline = UINT32_MAX;
    union user_action_cookie cookie = {0};
    size_t userdata_len = 0;

    DPDK_DEBUG()

    if (upcall == NULL || dpif_ == NULL || buf == NULL) {
        return EINVAL;
    }

    do {
        error = next_available_pipeline(&last_used_recv_pipeline);
        if (error) {
            return ENODEV;
        }
        pipeline_id = last_used_recv_pipeline;

        /* Set the initial pipeline on the first loop */
        if (initial_pipeline == UINT32_MAX) {
            initial_pipeline = pipeline_id;
        }

        error = dpdk_link_recv_packet(buf, &info, pipeline_id);
        if (unlikely(error == EAGAIN)) {
            if (peek_next_pipeline(&last_used_recv_pipeline) == initial_pipeline) {
                /* As there is nothing left in any of the available pipeline's
                 * exception rings, we assert that the signal we received from
                 * the datapath has been handled. This means that there are no
                 * more packets to be handled
                 */
                SIGNAL_HANDLED(dpdk_sock, sock_msg);
                return EAGAIN;
            } else {
                /* Check next pipeline for exception packets */
                continue;
            }
        }

        memset(upcall, 0, sizeof(*upcall));

        switch (info.cmd) {
        case OVS_PACKET_CMD_MISS:
            upcall->type = DPIF_UC_MISS;
            upcall->userdata = 0;
            userdata_len = 0;
            break;
        case OVS_PACKET_CMD_ACTION:
            upcall->type = DPIF_UC_ACTION;
            cookie.type = USER_ACTION_COOKIE_SLOW_PATH;
            nl_msg_put_unspec(buf, 0, &cookie, sizeof(cookie.slow_path));
            userdata_len = sizeof(cookie.slow_path) + NLA_HDRLEN;
            break;
        default:
            return EINVAL;
        }

        dpif_dpdk_flow_key_to_flow(&info.key, &flow);
        ofpbuf_init(&key, 0);
        odp_flow_key_from_flow(&key, &flow, flow.in_port.odp_port);
        ofpbuf_put(buf, key.data, key.size);
        upcall->key_len = key.size;

        buf->size -= key.size;
        upcall->key = ofpbuf_tail(buf);

        buf->size -= userdata_len;
        upcall->userdata = ofpbuf_tail(buf);

        ofpbuf_use_stub(&upcall->packet, buf->data, buf->size);
        upcall->packet.size = buf->size;

        /* free memory allocated in ofpbuf key */
        ofpbuf_uninit(&key);

        break;

    } while (peek_next_pipeline(&last_used_recv_pipeline) != initial_pipeline);

    return 0;
}

static void
dpif_dpdk_recv_wait(struct dpif *dpif_ OVS_UNUSED)
{
    DPDK_DEBUG()

    /*
     * Register the calling function to listen on the dpdk_sock for
     * POLLIN signal which will be triggered by the dpdk datapath
     * when a packet is availiable for reading by dpif_dpdk_recv.
     */
    poll_fd_wait(dpdk_sock, POLLIN);
}

static void
dpif_dpdk_recv_purge(struct dpif *dpif_ OVS_UNUSED)
{
    DPDK_DEBUG()
}

const struct dpif_class dpif_dpdk_class =
{
    "dpdk",
    NULL,                       /* enumerate */
    NULL,                       /* port open type */
    dpif_dpdk_open,
    dpif_dpdk_close,
    dpif_dpdk_destroy,
    NULL,                       /* run */
    NULL,                       /* wait */
    dpif_dpdk_get_stats,
    dpif_dpdk_port_add,
    dpif_dpdk_port_del,
    dpif_dpdk_port_query_by_number,
    dpif_dpdk_port_query_by_name,
    dpif_dpdk_get_max_ports,
    NULL,                       /* port_get_pid */
    dpif_dpdk_port_dump_start,
    dpif_dpdk_port_dump_next,
    dpif_dpdk_port_dump_done,
    dpif_dpdk_port_poll,
    dpif_dpdk_port_poll_wait,
    dpif_dpdk_flow_get,
    dpif_dpdk_flow_put,
    dpif_dpdk_flow_del,
    dpif_dpdk_flow_flush,
    dpif_dpdk_flow_dump_start,
    dpif_dpdk_flow_dump_next,
    dpif_dpdk_flow_dump_done,
    dpif_dpdk_execute,
    NULL,                       /* operate */
    dpif_dpdk_recv_set,
    dpif_dpdk_queue_to_priority,
    dpif_dpdk_recv,
    dpif_dpdk_recv_wait,
    dpif_dpdk_recv_purge
};

static int
dpif_dpdk_init(void)
{
    int error = 0;
    static int init = 0;

    DPDK_DEBUG()

    /* Check if already initialized. */
    if (init) {
        return 0;
    }

    error = dpdk_link_init(&pipeline_bitmask);
    if (!error) {
        init = 1;
        max_pipeline_id = max_available_pipeline_id();
        min_pipeline_id = min_available_pipeline_id();
        last_used_recv_pipeline = last_used_add_pipeline = max_pipeline_id;
    }

    return error;
}

/* Clear 'vport_msg' to empty values. */
static void
dpif_dpdk_vport_msg_init(struct ovdk_vport_message *vport_msg)
{
    DPDK_DEBUG()

    memset(vport_msg, 0, sizeof(*vport_msg));
    vport_msg->vportid = OVDK_MAX_VPORTS;
}

/*
 * Carry out a transaction with the datapath specified in 'request'.
 * If there is an error this function returns a positive errno value.
 * If the reply to this request is null, this function returns 0.
 * If the reply is not null, this functions stores the reply in '*reply'.
 * The type of this reply is expected to be a vport.
 */
static int
dpif_dpdk_vport_transact(struct ovdk_vport_message *request,
                         unsigned pipeline_id,
                         struct ovdk_vport_message *reply)
{
    struct ovdk_message request_buf = {0};
    int error = 0;

    DPDK_DEBUG()

    if (!is_valid_pipeline(pipeline_id)) {
        return EINVAL;
    }

    request_buf.type = OVDK_VPORT_CMD_FAMILY;
    request_buf.vport_msg = *request;

    error = dpdk_link_send(&request_buf, NULL, pipeline_id);
    if (error) {
        return error;
    }

    error = dpdk_link_recv_reply(&request_buf, pipeline_id);
    if (error) {
        return error;
    }

    if (reply) {
        *reply = request_buf.vport_msg;
    }

    /* Datapath returns -ve error values, but upper layers expect +ve values */
    return -request_buf.error;
}

/* Clears 'flow' to "empty" values. */
static void
dpif_dpdk_flow_init(struct ovdk_flow_message *flow_msg)
{
    DPDK_DEBUG()

    memset(flow_msg, 0, sizeof(*flow_msg));
}

/*
 * Carries out a transaction with the datapath specified with request.
 * If there is an error this function returns a positive errno value.
 * If the reply to this request is null, this function returns 0.
 * If the reply is not null, this functions stores the reply in '*reply'.
 * The type of this reply is the return type of this function.
 */
static int
dpif_dpdk_flow_transact(struct ovdk_flow_message *request,
                        unsigned pipeline_id,
                        struct ovdk_flow_message *reply)
{
    struct ovdk_message request_buf = {0};
    int error = 0;

    DPDK_DEBUG()

    request_buf.type = OVDK_FLOW_CMD_FAMILY;
    request_buf.flow_msg = *request;

    error = dpdk_link_send(&request_buf, NULL, pipeline_id);
    if (error) {
        return error;
    }

    error = dpdk_link_recv_reply(&request_buf, pipeline_id);
    if (error) {
        return error;
    }

    if (reply) {
        *reply = request_buf.flow_msg;
    }

    return request_buf.error;
}

/*
 * Parse ovdk_flow_message to get stats and return to caller as
 * dpif_flow_stats.
 */
static void
dpif_dpdk_flow_get_stats(const struct ovdk_flow_message *flow_msg,
                         struct dpif_flow_stats *stats)
{
    DPDK_DEBUG()

    stats->n_packets = flow_msg->stats.packet_count;
    stats->n_bytes = flow_msg->stats.byte_count;
    stats->used = flow_msg->stats.used;
    stats->tcp_flags = flow_msg->stats.tcp_flags;
}

/*
 * Convert flow key type from struct flow to struct ovdk_flow_key.
 */
static void
dpif_dpdk_flow_key_from_flow(struct ovdk_flow_key *key,
                             const struct flow *flow)
{
    uint16_t vlan_tci = 0;

    memset(key, 0, sizeof(*key));
    key->in_port = flow->in_port.odp_port;
    memcpy(key->ether_dst.addr_bytes, flow->dl_dst, ETHER_ADDR_LEN);
    memcpy(key->ether_src.addr_bytes, flow->dl_src, ETHER_ADDR_LEN);
    key->ether_type = flow->dl_type;
    vlan_tci = rte_be_to_cpu_16(flow->vlan_tci);
    key->vlan_id = vlan_tci & VLAN_ID_MASK;
    key->vlan_prio = vlan_tci >> VLAN_PRIO_SHIFT;
    key->ip_src = flow->nw_src;
    key->ip_dst = flow->nw_dst;
    key->ip_proto = flow->nw_proto;
    key->ip_frag = flow->nw_frag == 0 ? OVS_FRAG_TYPE_NONE
                 : flow->nw_frag == FLOW_NW_FRAG_ANY ? OVS_FRAG_TYPE_FIRST
                 : OVS_FRAG_TYPE_LATER;
    key->tran_src_port = flow->tp_src;
    key->tran_dst_port = flow->tp_dst;
}

/*
 * Convert flow key type from struct ovdk_flow_key to struct flow.
 */
static void
dpif_dpdk_flow_key_to_flow(const struct ovdk_flow_key *key,
                           struct flow *flow)
{
    memset(flow, 0, sizeof(*flow));
    flow->in_port.odp_port = key->in_port;
    memcpy(flow->dl_dst, key->ether_dst.addr_bytes, ETHER_ADDR_LEN);
    memcpy(flow->dl_src, key->ether_src.addr_bytes, ETHER_ADDR_LEN);
    flow->dl_type = key->ether_type;
    if (key->vlan_id != 0) {
        flow->vlan_tci = rte_cpu_to_be_16(key->vlan_prio << VLAN_PRIO_SHIFT | key->vlan_id | VLAN_CFI);
    }
    flow->nw_src = key->ip_src;
    flow->nw_dst = key->ip_dst;
    flow->nw_proto = key->ip_proto;
    flow->nw_frag = 0;
    if (key->ip_frag != OVS_FRAG_TYPE_NONE) {
        flow->nw_frag |= FLOW_NW_FRAG_ANY;
        if (key->ip_frag == OVS_FRAG_TYPE_LATER) {
            flow->nw_frag |= FLOW_NW_FRAG_LATER;
        }
    }
    flow->tp_src = key->tran_src_port;
    flow->tp_dst = key->tran_dst_port;
}

/*
 * Convert from ovdk_actions to ofpbuf actions
 */
static void
dpif_dpdk_flow_actions_to_actions(const struct ovdk_action *actions,
                                  struct ofpbuf *actionsp,
                                  uint8_t num_actions)
{
    int i = 0;
    size_t offset = 0;

    for (i = 0; i < num_actions; i++) {
        switch (actions[i].type) {
        case OVDK_ACTION_OUTPUT:
            nl_msg_put_u32(actionsp, OVS_ACTION_ATTR_OUTPUT,
                           actions[i].data.output.port);
            break;
        case OVDK_ACTION_POP_VLAN:
            nl_msg_put_flag(actionsp, OVS_ACTION_ATTR_POP_VLAN);
            break;
        case OVDK_ACTION_PUSH_VLAN:
            nl_msg_put_unspec(actionsp, OVS_ACTION_ATTR_PUSH_VLAN,
                              &actions[i].data.vlan,
                              sizeof(struct ovdk_action_push_vlan));
            break;
        case OVDK_ACTION_SET_ETHERNET:
            offset = nl_msg_start_nested(actionsp, OVS_ACTION_ATTR_SET);
            nl_msg_put_unspec(actionsp, OVS_KEY_ATTR_ETHERNET,
                              &actions[i].data.ethernet,
                              sizeof(struct ovs_key_ethernet));
            nl_msg_end_nested(actionsp, offset);
            break;
        case OVDK_ACTION_SET_IPV4:
            offset = nl_msg_start_nested(actionsp, OVS_ACTION_ATTR_SET);
            nl_msg_put_unspec(actionsp, OVS_KEY_ATTR_IPV4,
                              &actions[i].data.ipv4,
                              sizeof(struct ovs_key_ipv4));
            nl_msg_end_nested(actionsp, offset);
            break;
        case OVDK_ACTION_SET_TCP:
            offset = nl_msg_start_nested(actionsp, OVS_ACTION_ATTR_SET);
            nl_msg_put_unspec(actionsp, OVS_KEY_ATTR_TCP,
                              &actions[i].data.tcp,
                              sizeof(struct ovs_key_tcp));
            nl_msg_end_nested(actionsp, offset);
            break;
        case OVDK_ACTION_SET_UDP:
            offset = nl_msg_start_nested(actionsp, OVS_ACTION_ATTR_SET);
            nl_msg_put_unspec(actionsp, OVS_KEY_ATTR_UDP,
                              &actions[i].data.udp,
                              sizeof(struct ovs_key_udp));
            nl_msg_end_nested(actionsp, offset);
            break;
        case OVDK_ACTION_VSWITCHD:
            offset = nl_msg_start_nested(actionsp, OVS_ACTION_ATTR_USERSPACE);
            nl_msg_put_u32(actionsp, OVS_USERSPACE_ATTR_PID,
                           actions[i].data.vswitchd.pid);
            nl_msg_end_nested(actionsp, offset);
            break;
        case OVDK_ACTION_NULL:
        case OVDK_ACTION_DROP:
        case OVDK_ACTION_MAX:
            break;
        }
    }
}
