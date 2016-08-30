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

#include "eal_private.h"

/* Global SoC driver list */
struct soc_driver_list soc_driver_list =
	TAILQ_HEAD_INITIALIZER(soc_driver_list);
struct soc_device_list soc_device_list =
	TAILQ_HEAD_INITIALIZER(soc_device_list);

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
	TAILQ_INSERT_TAIL(&soc_driver_list, driver, next);
}

/* unregister a driver */
void
rte_eal_soc_unregister(struct rte_soc_driver *driver)
{
	TAILQ_REMOVE(&soc_driver_list, driver, next);
}
