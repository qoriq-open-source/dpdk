/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2016 RehiveTech. All rights reserved.
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
 *     * Neither the name of RehiveTech nor the names of its
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

#ifndef _RTE_SOC_H_
#define _RTE_SOC_H_

/**
 * @file
 *
 * RTE SoC Interface
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>

#include <rte_dev.h>
#include <rte_debug.h>

struct rte_soc_id {
	const char *compatible; /**< OF compatible specification */
	uint64_t priv_data;     /**< SoC Driver specific data */
};

struct rte_soc_addr {
	char *name;     /**< name used in sysfs */
	char *fdt_path; /**< path to the associated node in FDT */
};

/**
 * A structure describing a SoC device.
 */
struct rte_soc_device {
	TAILQ_ENTRY(rte_soc_device) next;   /**< Next probed SoC device */
	struct rte_device device;           /**< Inherit code device */
	struct rte_soc_addr addr;           /**< SoC device Location */
	struct rte_soc_id *id;              /**< SoC device ID list */
	struct rte_soc_driver *driver;      /**< Associated driver */
};

struct rte_soc_driver;

/**
 * Initialization function for the driver called during SoC probing.
 */
typedef int (soc_devinit_t)(struct rte_soc_driver *, struct rte_soc_device *);

/**
 * Uninitialization function for the driver called during hotplugging.
 */
typedef int (soc_devuninit_t)(struct rte_soc_device *);

/**
 * A structure describing a SoC driver.
 */
struct rte_soc_driver {
	TAILQ_ENTRY(rte_soc_driver) next;  /**< Next in list */
	struct rte_driver driver;          /**< Inherit core driver. */
	soc_devinit_t *devinit;            /**< Device initialization */
	soc_devuninit_t *devuninit;        /**< Device uninitialization */
	const struct rte_soc_id *id_table; /**< ID table, NULL terminated */
};

/**
 * Utility function to write a SoC device name, this device name can later be
 * used to retrieve the corresponding rte_soc_addr using above functions.
 *
 * @param addr
 *	The SoC address
 * @param output
 *	The output buffer string
 * @param size
 *	The output buffer size
 * @return
 *  0 on success, negative on error.
 */
static inline void
rte_eal_soc_device_name(const struct rte_soc_addr *addr,
			char *output, size_t size)
{
	int ret;

	RTE_VERIFY(addr != NULL);
	RTE_VERIFY(size >= strlen(addr->name));
	ret = snprintf(output, size, "%s", addr->name);
	RTE_VERIFY(ret >= 0);
}

static inline int
rte_eal_compare_soc_addr(const struct rte_soc_addr *a0,
			 const struct rte_soc_addr *a1)
{
	if (a0 == NULL || a1 == NULL)
		return -1;

	RTE_VERIFY(a0->name != NULL);
	RTE_VERIFY(a1->name != NULL);

	return strcmp(a0->name, a1->name);
}

#endif
