/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2019 NXP
 */

#include <rte_dpaa_logs.h>

#include <vsp.h>

static uint8_t mac_id[] = {-1, 0, 1, 2, 3, 4, 5, 6, 7, 0, 1};
static t_Handle vsp[MAX_NUM_PORTS][VSP_MAX_NUM_PROFILES];
static int rel_idx = 0;

int num_profiles;

static t_Handle fman_handle;
struct rte_mempool *vsp_mempool[MAX_NUM_PORTS][VSP_MAX_NUM_PROFILES];

int vsp_init(struct fman_if *fm, struct dpaa_bp_info *bp_base)
{

	int ret = 0;
	int idx, i;
	uint32_t pool_id = 0;
	t_FmVspParams vsp_params;
	t_FmBufferPrefixContent buf_prefix_cont;
	char pool_name[RTE_MEMPOOL_NAMESIZE];
	struct fman_if_bpool *fm_if_bp;

	if (fm->num_profiles)
		num_profiles = fm->num_profiles;
	else
		goto out;

	fman_handle = FM_Open(fm->fman_idx);
	if (!fman_handle)
		return -1;

	idx = mac_id[fm->mac_idx];

	/* Extract the BPID associated with shared MAC. There is only one
	 * shared MAC so this loop is guaranteed to run only once.
	 */
	list_for_each_entry(fm_if_bp, &fm->bpool_list, node) {
		pool_id = fm_if_bp->bpid;
	}

	/* Create a pool with the BPID extracted - this would be pinned to
	 * kernel for its queues. This name is matched in mbuf_create_pool
	 * API of DPAA Mempool.
	 */
	snprintf(pool_name, sizeof(pool_name), "vsp_pool_%u", pool_id);
	vsp_mempool[rel_idx][0] =
		rte_pktmbuf_pool_create((const char *)pool_name,
					VSP_NUM_BUFS,
					VSP_CACHE_SIZE,
					0, VSP_DATA_ROOM_SIZE,
					SOCKET_ID_ANY);

	struct dpaa_bp_info *bp_info =
		DPAA_MEMPOOL_TO_POOL_INFO(vsp_mempool[rel_idx][0]);

	/* first initialize the base storage profile */
	memset(&vsp_params, 0, sizeof(vsp_params));

	vsp_params.h_Fm = fman_handle;
	vsp_params.relativeProfileId = VSP_BASE;
	vsp_params.portParams.portId = idx;
	vsp_params.portParams.portType = e_FM_PORT_TYPE_RX;
	vsp_params.extBufPools.numOfPoolsUsed = 1;
	/* First VSP profile contains the BPID extracted above */
	vsp_params.extBufPools.extBufPool[0].id = bp_info->bpid;
	vsp_params.extBufPools.extBufPool[0].size = bp_info->size;

	DPAA_PMD_DEBUG("Configured base profile: bpid %u size %u",
		       bp_info->bpid, bp_info->size);

	vsp[rel_idx][VSP_BASE] = FM_VSP_Config(&vsp_params);
	if (!vsp[rel_idx][VSP_BASE]) {
		DPAA_PMD_ERR("FM_VSP_Config");
		return -EINVAL;
	}

	/* configure the application buffer (structure, size and content) */

	memset(&buf_prefix_cont, 0, sizeof(buf_prefix_cont));

	buf_prefix_cont.privDataSize = 16;
	buf_prefix_cont.dataAlign = 64;
	buf_prefix_cont.passPrsResult = true;
	buf_prefix_cont.passTimeStamp = true;
	buf_prefix_cont.passHashResult = false;
	buf_prefix_cont.passAllOtherPCDInfo = false;

	ret = FM_VSP_ConfigBufferPrefixContent(vsp[rel_idx][VSP_BASE],
					       &buf_prefix_cont);
	if (ret != E_OK) {
		DPAA_PMD_ERR("FM_VSP_ConfigBufferPrefixContent error for vsp;"
			     " err: %d", ret);
		return ret;
	}

	/* initialize the FM VSP module */
	ret = FM_VSP_Init(vsp[rel_idx][VSP_BASE]);
	if (ret != E_OK) {
		error(0, ret, "FM_VSP_Init error: %d\n", ret);
	}

	/* create a descriptor for the FM VSP module */
	for (i = 1; i < num_profiles; i++) {
		memset(&vsp_params, 0, sizeof(vsp_params));

		vsp_params.h_Fm = fman_handle;
		vsp_params.relativeProfileId = i;
		vsp_params.portParams.portId = idx;
		vsp_params.portParams.portType = e_FM_PORT_TYPE_RX;
		vsp_params.extBufPools.numOfPoolsUsed = 1;
		vsp_params.extBufPools.extBufPool[0].id = bp_base->bpid;
		vsp_params.extBufPools.extBufPool[0].size = bp_base->size;

		DPAA_PMD_DEBUG("Configured profile %d: bpid %u size %u",
			       i, bp_base->bpid, bp_base->size);

		vsp[rel_idx][i] = FM_VSP_Config(&vsp_params);
		if (!vsp[rel_idx][i]) {
			DPAA_PMD_ERR("FM_VSP_Config error for profile %d", i);
			return -EINVAL;
		}

		/* configure the application buffer (structure, size and
		 * content)
		 */

		memset(&buf_prefix_cont, 0, sizeof(buf_prefix_cont));

		buf_prefix_cont.privDataSize = 16;
		buf_prefix_cont.dataAlign = 64;
		buf_prefix_cont.passPrsResult = true;
		buf_prefix_cont.passTimeStamp = true;
		buf_prefix_cont.passHashResult = false;
		buf_prefix_cont.passAllOtherPCDInfo = false;

		ret = FM_VSP_ConfigBufferPrefixContent(vsp[rel_idx][i],
						       &buf_prefix_cont);
		if (ret != E_OK) {
			DPAA_PMD_ERR("FM_VSP_ConfigBufferPrefixContent error"
				     "for profile %d err: %d", i, ret);
			return ret;
		}

		/* initialize the FM VSP module */

		ret = FM_VSP_Init(vsp[rel_idx][i]);
		if (ret != E_OK) {
			DPAA_PMD_ERR("FM_VSP_Init error for profile %d err:%d",
				     i, ret);
			return ret;
		}

	}
	/* for each port there is a number of storage profiles which
	 * saved in the vsp array. Relative index will keep the storage
	 * profiles per port
	 */
	rel_idx++;

out:

	return 0;
}

int vsp_clean(void)
{
	int ret = E_OK, i, j;

	if (!num_profiles)
		goto out;

	for (j = 0; j < rel_idx; j++) {
		for (i = 1; i < num_profiles; i++) {
			if (vsp[j][i]) {
				ret = FM_VSP_Free(vsp[j][i]);
				if (ret != E_OK) {
					DPAA_PMD_ERR(
						"Error FM_VSP_Free: "
						"%d rel_id %d profile id %d",
						ret, j, i);
					return ret;
				}
				rte_mempool_free(vsp_mempool[j][i]);
				DPAA_PMD_DEBUG("Cleaned storage profile:%d", i);
			}
		}
	}

	FM_Close(fman_handle);

out:
	return E_OK;
}
