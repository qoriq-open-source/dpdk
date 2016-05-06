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

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/queue.h>

#include <rte_soc.h>
#include <rte_devargs.h>
#include <rte_debug.h>

#include "test.h"

static char *safe_strdup(const char *s)
{
	char *c = strdup(s);

	if (c == NULL)
		rte_panic("failed to strdup '%s'\n", s);

	return c;
}

static int test_compare_addr(void)
{
	struct rte_soc_addr a0;
	struct rte_soc_addr a1;
	struct rte_soc_addr a2;

	a0.name = safe_strdup("ethernet0");
	a0.fdt_path = NULL;

	a1.name = safe_strdup("ethernet0");
	a1.fdt_path = NULL;

	a2.name = safe_strdup("ethernet1");
	a2.fdt_path = NULL;

	TEST_ASSERT(!rte_eal_compare_soc_addr(&a0, &a1),
		    "Failed to compare two soc addresses that equal");
	TEST_ASSERT(rte_eal_compare_soc_addr(&a0, &a2),
		    "Failed to compare two soc addresses that differs");

	free(a2.name);
	free(a1.name);
	free(a0.name);
	return 0;
}

static int
test_soc(void)
{
	if (test_compare_addr())
		return -1;

	return 0;
}

REGISTER_TEST_COMMAND(soc_autotest, test_soc);
