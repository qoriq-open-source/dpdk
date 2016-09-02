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

#include <stddef.h>
#include <stdio.h>
#include <sys/queue.h>

#include <rte_log.h>
#include <rte_common.h>
#include <rte_devargs.h>
#include <rte_eal.h>
#include <rte_soc.h>

#include "eal_private.h"

/* Global SoC driver list */
struct soc_driver_list soc_driver_list =
	TAILQ_HEAD_INITIALIZER(soc_driver_list);
struct soc_device_list soc_device_list =
	TAILQ_HEAD_INITIALIZER(soc_device_list);

static struct rte_devargs *soc_devargs_lookup(struct rte_soc_device *dev)
{
	struct rte_devargs *devargs;

	TAILQ_FOREACH(devargs, &devargs_list, next) {
		if (devargs->type != RTE_DEVTYPE_BLACKLISTED_SOC &&
			devargs->type != RTE_DEVTYPE_WHITELISTED_SOC)
			continue;
		if (!rte_eal_compare_soc_addr(&dev->addr, &devargs->soc.addr))
			return devargs;
	}

	return NULL;
}

/**
 * Default function for matching the Soc driver with device. Each driver can either use this
 * function or define their own soc matching function.
 *
 * @return
 *      -  0 on success
 *      -  1 when no match found
 *      -  -1 when error encounterd in match - need to stop further matching
 */
int
rte_eal_soc_match(struct rte_soc_driver *drv, struct rte_soc_device *dev)
{
	int i, j;

	RTE_VERIFY(drv != NULL && drv->id_table != NULL);
	RTE_VERIFY(dev != NULL && dev->id != NULL);

	for (i = 0; drv->id_table[i].compatible; ++i) {
		const char *drv_compat = drv->id_table[i].compatible;

		for (j = 0; dev->id[j].compatible; ++j) {
			const char *dev_compat = dev->id[j].compatible;

			if (!strcmp(drv_compat, dev_compat))
				return 0;
		}
	}

	return 1;
}

static int
rte_eal_soc_probe_one_driver(struct rte_soc_driver *drv,
			     struct rte_soc_device *dev)
{
	int ret = 1;

	ret = drv->match_fn(drv, dev);
	if (ret) {
		RTE_LOG(DEBUG, EAL,
			" match function failed, skipping\n");
		return ret;
	}

	RTE_LOG(DEBUG, EAL, "SoC device %s on NUMA socket %d\n",
			dev->addr.name, dev->device.numa_node);
	RTE_LOG(DEBUG, EAL, "  probe driver %s\n", drv->driver.name);

	/* no initialization when blacklisted, return without error */
	if (dev->device.devargs != NULL
		&& dev->device.devargs->type == RTE_DEVTYPE_BLACKLISTED_SOC) {
		RTE_LOG(DEBUG, EAL,
			"  device is blacklisted, skipping\n");
		return ret;
	}

	dev->driver = drv;
	RTE_VERIFY(drv->devinit != NULL);
	return drv->devinit(drv, dev);
}

static int
soc_probe_all_drivers(struct rte_soc_device *dev)
{
	struct rte_soc_driver *drv = NULL;
	int rc = 0;

	if (dev == NULL)
		return -1;

	TAILQ_FOREACH(drv, &soc_driver_list, next) {
		rc = rte_eal_soc_probe_one_driver(drv, dev);
		if (rc < 0)
			/* negative value is an error */
			return -1;
		if (rc > 0)
			/* positive value means driver doesn't support it */
			continue;
		return 0;
	}
	return 1;
}

/* If the IDs match, call the devuninit() function of the driver. */
static int
rte_eal_soc_detach_dev(struct rte_soc_driver *drv,
		       struct rte_soc_device *dev)
{
	int ret;

	if ((drv == NULL) || (dev == NULL))
		return -EINVAL;

	ret = drv->match_fn(drv, dev);
	if (ret) {
		RTE_LOG(DEBUG, EAL,
			" match function failed, skipping\n");
		return ret;
	}

	RTE_LOG(DEBUG, EAL, "SoC device %s on NUMA socket %i\n",
			dev->addr.name, dev->device.numa_node);

	RTE_LOG(DEBUG, EAL, "  remove driver: %s\n", drv->driver.name);

	if (drv->devuninit && (drv->devuninit(dev) < 0))
		return -1;	/* negative value is an error */

	/* clear driver structure */
	dev->driver = NULL;

	return 0;
}

/*
 * Call the devuninit() function of all registered drivers for the given
 * device if their IDs match.
 *
 * @return
 *       0 when successful
 *      -1 if deinitialization fails
 *       1 if no driver is found for this device.
 */
static int
soc_detach_all_drivers(struct rte_soc_device *dev)
{
	struct rte_soc_driver *dr = NULL;
	int rc = 0;

	if (dev == NULL)
		return -1;

	TAILQ_FOREACH(dr, &soc_driver_list, next) {
		rc = rte_eal_soc_detach_dev(dr, dev);
		if (rc < 0)
			/* negative value is an error */
			return -1;
		if (rc > 0)
			/* positive value means driver doesn't support it */
			continue;
		return 0;
	}
	return 1;
}

/*
 * Detach device specified by its SoC address.
 */
int
rte_eal_soc_detach(const struct rte_soc_addr *addr)
{
	struct rte_soc_device *dev = NULL;
	int ret = 0;

	if (addr == NULL)
		return -1;

	TAILQ_FOREACH(dev, &soc_device_list, next) {
		if (rte_eal_compare_soc_addr(&dev->addr, addr))
			continue;

		ret = soc_detach_all_drivers(dev);
		if (ret < 0)
			goto err_return;

		TAILQ_REMOVE(&soc_device_list, dev, next);
		return 0;
	}
	return -1;

err_return:
	RTE_LOG(WARNING, EAL, "Requested device %s cannot be used\n",
		dev->addr.name);
	return -1;
}

int
rte_eal_soc_probe_one(const struct rte_soc_addr *addr)
{
	struct rte_soc_device *dev = NULL;
	int ret = 0;

	if (addr == NULL)
		return -1;

	/* unlike pci, in case of soc, it the responsibility of the soc driver
	 * to check during init whether device has been updated since last add.
	 */

	TAILQ_FOREACH(dev, &soc_device_list, next) {
		if (rte_eal_compare_soc_addr(&dev->addr, addr))
			continue;

		ret = soc_probe_all_drivers(dev);
		if (ret < 0)
			goto err_return;
		return 0;
	}
	return -1;

err_return:
	RTE_LOG(WARNING, EAL,
		"Requested device %s cannot be used\n", addr->name);
	return -1;
}

/*
 * Scan the SoC devices and call the devinit() function for all registered
 * drivers that have a matching entry in its id_table for discovered devices.
 */
int
rte_eal_soc_probe(void)
{
	struct rte_soc_device *dev = NULL;
	struct rte_devargs *devargs = NULL;
	int ret = 0;
	int probe_all = 0;

	if (rte_eal_devargs_type_count(RTE_DEVTYPE_WHITELISTED_SOC) == 0)
		probe_all = 1;

	TAILQ_FOREACH(dev, &soc_device_list, next) {

		/* set devargs in SoC structure */
		devargs = soc_devargs_lookup(dev);
		if (devargs != NULL)
			dev->device.devargs = devargs;

		/* probe all or only whitelisted devices */
		if (probe_all)
			ret = soc_probe_all_drivers(dev);
		else if (devargs != NULL &&
			devargs->type == RTE_DEVTYPE_WHITELISTED_SOC)
			ret = soc_probe_all_drivers(dev);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "Requested device %s "
				 "cannot be used\n", dev->addr.name);
	}

	return 0;
}

/* dump one device */
static int
soc_dump_one_device(FILE *f, struct rte_soc_device *dev)
{
	int i;

	fprintf(f, "%s", dev->addr.name);
	fprintf(f, " - fdt_path: %s\n",
			dev->addr.fdt_path ? dev->addr.fdt_path : "(none)");

	for (i = 0; dev->id && dev->id[i].compatible; ++i)
		fprintf(f, "   %s\n", dev->id[i].compatible);

	return 0;
}

/* dump devices on the bus to an output stream */
void
rte_eal_soc_dump(FILE *f)
{
	struct rte_soc_device *dev = NULL;

	if (!f)
		return;

	TAILQ_FOREACH(dev, &soc_device_list, next) {
		soc_dump_one_device(f, dev);
	}
}

/* register a driver */
void
rte_eal_soc_register(struct rte_soc_driver *driver)
{
	/* For a valid soc driver, match and scan function
	 * should be provided.
	 */
	RTE_VERIFY(driver != NULL);
	RTE_VERIFY(driver->match_fn != NULL);
	RTE_VERIFY(driver->scan_fn != NULL);
	TAILQ_INSERT_TAIL(&soc_driver_list, driver, next);
}

/* unregister a driver */
void
rte_eal_soc_unregister(struct rte_soc_driver *driver)
{
	TAILQ_REMOVE(&soc_driver_list, driver, next);
}
