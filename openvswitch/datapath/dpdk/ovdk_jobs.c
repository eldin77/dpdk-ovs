/*
 *   BSD LICENSE
 *
 *   Copyright(c) 2014 NEC Laboratories Europe Ltd. All rights reserved.
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
 *     * Neither the name of NEC Laboratories Europe Ltd. nor the names of
 *       its contributors may be used to endorse or promote products derived
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
#include <rte_common.h>
#include <rte_memory.h>
#include <rte_malloc.h>
#include <rte_lcore.h>
#include <rte_launch.h>
#include <rte_debug.h>

#include "ovdk_jobs.h"

#define JOBLIST_REFS_NAME "OVDK_joblist_refs"

#define ASSERT_IS_MASTER_LCORE(lcore_id) \
	do { \
		if ((lcore_id) != rte_get_master_lcore()) \
			rte_panic("Lcore %u is not master (%s:%u)\n", \
			          (lcore_id), __FUNCTION__, __LINE__); \
	} while(0)
#define ASSERT_IS_SLAVE_LCORE(lcore_id) \
	do { \
		if ((lcore_id) == rte_get_master_lcore()) \
			rte_panic("Lcore %u is not slave (%s:%u)\n", \
			          (lcore_id), __FUNCTION__, __LINE__); \
	} while(0)
#define ASSERT_SLAVE_LCORE_STOPPED(lcore_id) \
	do { \
		if ((lcore_id) != rte_get_master_lcore() && \
		    rte_eal_get_lcore_state((lcore_id)) == RUNNING) \
			rte_panic("Lcore %u in running state (%s:%u)\n", \
			          (lcore_id), __FUNCTION__, __LINE__); \
	} while(0)
#define ASSERT_JOBLIST_EXISTS(lcore_id) \
	do { \
		if (!ovdk_joblist_refs[(lcore_id)]) \
			rte_panic("No job list allocated for lcore %u\n", \
			          (lcore_id)); \
	} while(0)

struct ovdk_joblist **ovdk_joblist_refs;

/**
 * Initialization
 */
void
ovdk_jobs_init(void)
{
	unsigned i;

	ASSERT_IS_MASTER_LCORE(rte_lcore_id());

	ovdk_joblist_refs = rte_calloc(JOBLIST_REFS_NAME, RTE_MAX_LCORE,
	                          sizeof(struct joblist *), CACHE_LINE_SIZE);
	if (!ovdk_joblist_refs)
		rte_exit(EXIT_FAILURE, "Cannot allocate joblist reference "
		         "table\n");

	RTE_LCORE_FOREACH(i) {
		ovdk_joblist_refs[i] = rte_malloc_socket(NULL,
		                                    sizeof(struct ovdk_joblist),
		                                    CACHE_LINE_SIZE,
		                                    rte_lcore_to_socket_id(i));
		if (!ovdk_joblist_refs[i])
			rte_exit(EXIT_FAILURE,
			         "Cannot allocate joblist for lcore %u\n", i);
		ovdk_joblist_refs[i]->nb_jobs = 0;
		ovdk_joblist_refs[i]->online = 0;
	}
}

/**
 * Clears the job list of an lcore
 */
void
ovdk_jobs_clear_lcore(unsigned lcore_id)
{
	ASSERT_IS_MASTER_LCORE(rte_lcore_id());
	ASSERT_JOBLIST_EXISTS(lcore_id);
	ASSERT_SLAVE_LCORE_STOPPED(lcore_id);

	ovdk_joblist_refs[lcore_id]->nb_jobs = 0;
}

/**
 * Clears the job list of all lcores
 */
void
ovdk_jobs_clear_all(void)
{
	unsigned i;

	ASSERT_IS_MASTER_LCORE(rte_lcore_id());

	RTE_LCORE_FOREACH(i) {
		ovdk_jobs_clear_lcore(i);
	}
}

/**
 * Adds a job (represented by a function pointer) to
 * the job list of an lcore. On success 0 is returned.
 *
 * Note: Be sure that the depending lcore is in stopped
 * state (see ovdk_jobs_stop_lcore()) while using this function.
 */
int
ovdk_jobs_add_to_lcore(lcore_job_t *f, void *arg, unsigned lcore_id)
{
	struct ovdk_joblist *joblist;
	unsigned i;

	ASSERT_IS_MASTER_LCORE(rte_lcore_id());
	ASSERT_JOBLIST_EXISTS(lcore_id);
	ASSERT_SLAVE_LCORE_STOPPED(lcore_id);
	if (f == NULL)
		rte_panic("Tried to add a null pointer as job reference\n");

	joblist = ovdk_joblist_refs[lcore_id];
	if (joblist->nb_jobs >= MAXJOBS_PER_LCORE)
		return -ENOBUFS;

	i = joblist->nb_jobs;
	joblist->jobs[i].func = f;
	joblist->jobs[i].arg  = arg;
	++joblist->nb_jobs;
	return 0;
}

static int _slave_lcore_loop(void *arg);

/**
 * Launches a slave lcore thread for job processing
 * This function will return 0 on success.
 */
int
ovdk_jobs_launch_slave_lcore(unsigned lcore_id)
{
	enum rte_lcore_state_t state;

	ASSERT_IS_MASTER_LCORE(rte_lcore_id());
	ASSERT_JOBLIST_EXISTS(lcore_id);
	ASSERT_IS_SLAVE_LCORE(lcore_id);

	state = rte_eal_get_lcore_state(lcore_id);
	if (state == RUNNING)
		return 0; /* lcore already launched */

	ovdk_joblist_refs[lcore_id]->online = 1; /* enable processing flag */
	return rte_eal_remote_launch(_slave_lcore_loop, ovdk_joblist_refs[lcore_id],
	                             lcore_id);
}

void
ovdk_jobs_launch_slaves_all(void)
{
	unsigned i;

	ASSERT_IS_MASTER_LCORE(rte_lcore_id());

	RTE_LCORE_FOREACH_SLAVE(i) {
		if (ovdk_jobs_launch_slave_lcore(i) < 0)
			rte_panic("Could not launch lcore %u\n", i);
	}
}

/**
 * Stops a slave lcore thread
 * This core will actually put into blocked state by DPDK
 * until it get started up again (see: ovdk_jobs_launch_lcore()).
 *
 * This function will return when the according lcore thread
 * stopped.
 */
int
ovdk_jobs_stop_slave_lcore(unsigned lcore_id)
{
	enum rte_lcore_state_t state;

	ASSERT_IS_MASTER_LCORE(rte_lcore_id());
	ASSERT_JOBLIST_EXISTS(lcore_id);
	ASSERT_IS_SLAVE_LCORE(lcore_id);

	ovdk_joblist_refs[lcore_id]->online = 0; /* request lcore exit */
	state = rte_eal_get_lcore_state(lcore_id);
	if (state == RUNNING || state == FINISHED)
		return rte_eal_wait_lcore(lcore_id); /* wait for lcore exit */
	return 0;
}

void
ovdk_jobs_stop_slaves_all(void)
{
	unsigned i;

	ASSERT_IS_MASTER_LCORE(rte_lcore_id());

	RTE_LCORE_FOREACH_SLAVE(i) {
		if (ovdk_jobs_stop_slave_lcore(i) < 0)
			rte_panic("Could not stop lcore %u\n", i);
	}
}


/**
 * Job execution loop on slave lcore
 * This function gets assigned to a slave lcore via ovdk_jobs_launch_slave_lcore().
 * It executes the assigned job functions until exit is requested
 * (see ovdk_jobs_stop_lcore()).
 */
static int
_slave_lcore_loop(void *arg)
{
	struct ovdk_joblist *joblist = arg;
	unsigned i;

	rte_prefetch0(joblist);
	switch(joblist->nb_jobs) {
	case 0:
		/* no jobs -> clear online flag and exit */
		joblist->online = 0;
		break;
	case 1:
		/* only a single job is on the list */
		while (likely(joblist->online)) {
			joblist->jobs[0].func(joblist->jobs[0].arg);
		}
		break;
	default:
		/* multiple jobs are on the list */
		while (likely(joblist->online)) {
			for (i = 0; i < joblist->nb_jobs; ++i)
				joblist->jobs[i].func(joblist->jobs[i].arg);
		}
		break;
	}

	/* put this lcore into sleeping state */
	return 0;
}
