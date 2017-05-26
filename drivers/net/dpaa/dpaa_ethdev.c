/*-
 *   BSD LICENSE
 *
 *   Copyright 2016 Freescale Semiconductor, Inc. All rights reserved.
 *   Copyright 2017 NXP. All rights reserved.
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
 *     * Neither the name of  Freescale Semiconductor, Inc nor the names of its
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
 */
/* System headers */
#include <stdio.h>
#include <inttypes.h>
#include <unistd.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/syscall.h>

#include <rte_config.h>
#include <rte_byteorder.h>
#include <rte_common.h>
#include <rte_interrupts.h>
#include <rte_log.h>
#include <rte_debug.h>
#include <rte_pci.h>
#include <rte_atomic.h>
#include <rte_branch_prediction.h>
#include <rte_memory.h>
#include <rte_memzone.h>
#include <rte_tailq.h>
#include <rte_eal.h>
#include <rte_alarm.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_malloc.h>
#include <rte_ring.h>

#include <rte_dpaa_bus.h>
#include <rte_dpaa_logs.h>

#include <dpaa_ethdev.h>

/* Keep track of whether QMAN and BMAN have been globally initialized */
static int is_global_init;

static int
dpaa_eth_dev_configure(struct rte_eth_dev *dev __rte_unused)
{
	PMD_INIT_FUNC_TRACE();

	return 0;
}


static int dpaa_eth_dev_start(struct rte_eth_dev *dev)
{
	PMD_INIT_FUNC_TRACE();

	/* Change tx callback to the real one */
	dev->tx_pkt_burst = NULL;

	return 0;
}

static void dpaa_eth_dev_stop(struct rte_eth_dev *dev)
{
	dev->tx_pkt_burst = NULL;
}

static void dpaa_eth_dev_close(struct rte_eth_dev *dev __rte_unused)
{
	PMD_INIT_FUNC_TRACE();
}

static struct eth_dev_ops dpaa_devops = {
	.dev_configure		  = dpaa_eth_dev_configure,
	.dev_start		  = dpaa_eth_dev_start,
	.dev_stop		  = dpaa_eth_dev_stop,
	.dev_close		  = dpaa_eth_dev_close,
};

/* Initialise a network interface */
static int dpaa_eth_dev_init(struct rte_eth_dev *eth_dev __rte_unused)
{
	int dev_id;
	struct rte_dpaa_device *dpaa_device;
	struct dpaa_if *dpaa_intf;

	PMD_INIT_FUNC_TRACE();

	dpaa_device = DEV_TO_DPAA_DEVICE(eth_dev->device);
	dev_id = dpaa_device->id.dev_id;
	dpaa_intf = eth_dev->data->dev_private;

	dpaa_intf->name = dpaa_device->name;

	dpaa_intf->ifid = dev_id;

	eth_dev->dev_ops = &dpaa_devops;

	return -1;
}

static int
rte_dpaa_probe(struct rte_dpaa_driver *dpaa_drv __rte_unused,
			   struct rte_dpaa_device *dpaa_dev)
{
	int diag;
	int ret;
	struct rte_eth_dev *eth_dev;
	char ethdev_name[RTE_ETH_NAME_MAX_LEN];

	PMD_INIT_FUNC_TRACE();

	if (!is_global_init) {
		/* One time load of Qman/Bman drivers */
		ret = qman_global_init();
		if (ret) {
			PMD_DRV_LOG(ERR, "QMAN initialization failed: %d",
				    ret);
			return ret;
		}
		ret = bman_global_init();
		if (ret) {
			PMD_DRV_LOG(ERR, "BMAN initialization failed: %d",
				    ret);
			return ret;
		}

		is_global_init = 1;
	}

	sprintf(ethdev_name, "%s", dpaa_dev->name);

	ret = rte_dpaa_portal_init((void *)1);
	if (ret) {
		PMD_DRV_LOG(ERR, "Unable to initialize portal");
		return ret;
	}

	eth_dev = rte_eth_dev_allocate(ethdev_name);
	if (eth_dev == NULL)
		return -ENOMEM;

	if (rte_eal_process_type() == RTE_PROC_PRIMARY) {
		eth_dev->data->dev_private = rte_zmalloc(
						"ethdev private structure",
						sizeof(struct dpaa_if),
						RTE_CACHE_LINE_SIZE);
		if (!eth_dev->data->dev_private) {
			PMD_INIT_LOG(CRIT, "Cannot allocate memzone for"
				     " private port data\n");
			rte_eth_dev_release_port(eth_dev);
			return -ENOMEM;
		}
	}

	eth_dev->device = &dpaa_dev->device;
	dpaa_dev->eth_dev = eth_dev;
	eth_dev->data->rx_mbuf_alloc_failed = 0;

	/* Invoke PMD device initialization function */
	diag = dpaa_eth_dev_init(eth_dev);
	if (diag) {
		PMD_DRV_LOG(ERR, "Eth dev initialization failed: %d", ret);
		return diag;
	}

	PMD_DRV_LOG(DEBUG, "Eth dev initialized: %d\n", diag);

	return 0;
}

static int
rte_dpaa_remove(struct rte_dpaa_device *dpaa_dev)
{
	struct rte_eth_dev *eth_dev;

	PMD_INIT_FUNC_TRACE();

	eth_dev = dpaa_dev->eth_dev;

	if (rte_eal_process_type() == RTE_PROC_PRIMARY)
		rte_free(eth_dev->data->dev_private);

	rte_eth_dev_release_port(eth_dev);

	return 0;
}

static struct rte_dpaa_driver rte_dpaa_pmd = {
	.driver_type = FSL_DPAA_ETH,
	.probe = rte_dpaa_probe,
	.remove = rte_dpaa_remove,
};

RTE_PMD_REGISTER_DPAA(net_dpaa, rte_dpaa_pmd);
