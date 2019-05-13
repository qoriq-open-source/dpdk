/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2019 NXP
 *
 */

#ifndef __VSP_H
#define __VSP_H
#include <fman.h>
#include <fm_port_ext.h>
#include <fm_pcd_ext.h>
#include <fm_vsp_ext.h>
#include <dpaa_mempool.h>

#define VSP_NUM_BUFS 1025
#define VSP_CACHE_SIZE 250
#define VSP_DATA_ROOM_SIZE 9000
#define VSP_MAX_NUM_PROFILES 8
#define VSP_BASE 0
#define MAX_NUM_PORTS 10

extern int num_profiles;
int vsp_init(struct fman_if *fm, struct dpaa_bp_info *bp_base);
int vsp_clean(void);



#endif
