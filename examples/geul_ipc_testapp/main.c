/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2014 Intel Corporation
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <sys/queue.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>

#include <rte_memory.h>
#include <rte_launch.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_debug.h>
#include <rte_bus_vdev.h>
#include <rte_rawdev.h>
#include <rte_malloc.h>
#include <rte_cycles.h>

#include <rte_pmd_geul_ipc_rawdev.h>
#include <geul_ipc_api.h>
#include <geul_ipc_errorcodes.h>
#include <geul_ipc_um.h>
#include <gul_host_if.h>
#define UNUSED(x) void(x)

#define ipc_debug(...) printf(__VA_ARGS__)
//#define ipc_debug(...)

#define GEUL_DEVICE_ID "0"
#define GEUL_DEVICE_SEP "_"
/* Device name has to follow a certain naming pattern to be probed; this
 * includes having the driver name as the initial part; (or use device
 * alias - not implemented right now) - followed by ID
 */
#define GEUL_DEVICE_NAME GEUL_IPC_RAWDEV_NAME_PREFIX GEUL_DEVICE_SEP GEUL_DEVICE_ID

/* Only a single instance is supported */
#define GEUL_INSTANCE_ID 0

enum run_mode {
	VERIFICATION,
	PERFORMANCE,
	RUN_MODE_MAX
};

struct hugepage_info {
	void *vaddr;
	phys_addr_t paddr;
	size_t len;
};

/* Cycle Times
 * Default = 1; Single buffer send/recv */
int cycle_times = 1;
/* TestApp Mode
 * Default = VALIDATION; Only
 * buffer validation performed.
 */
uint8_t test_mode = VERIFICATION;

/* Driven by ipc_memelem_size, creating mempools which would be passed
 * as it it to the host_init call
 */
struct rte_mempool *pools[IPC_HOST_BUF_MAX_COUNT];
struct geulipc_channel *channels[CHANNELS_MAX];

/* Epoll Fd */
int32_t epoll_fd;
/* mask of event (interrupt) enabled channels */
int32_t int_enabled_ch_mask;
int32_t max_events = 0;

struct gul_stats *stats; /**< Stats for Host & modem (HIF) */

/* Signal control */
static uint8_t force_quit;

/*
 * From a given mempool, fetch the information about the physical backing
 * hugepage info. This function assumes that all mempool (or at least
 * mempool passed) is spreading across ONLY A SINGLE HUGEPAGE.
 */
static void
iter_for_each_chunk(struct rte_mempool *mp,
	    void *opaque __rte_unused,
	    struct rte_mempool_memhdr *memhdr,
	    unsigned mem_idx __rte_unused)
{
	struct hugepage_info *hp_info;
	struct rte_memseg *mseg;

	/* Just a safegaurd - in case this is called without opaque set
	 * to hugepage_info structure
	 */
	if (!opaque) {
		return;
	}

	hp_info = (struct hugepage_info *)opaque;
	mseg = rte_mem_virt2memseg(memhdr->addr, NULL);

	if (hp_info->vaddr && (memhdr->addr != hp_info->vaddr)) {
		printf("WARN: More than one memory segment observed."
		       " Not Supported (pool=%s)\n", mp->name);
	}

	/* Only update not already updated */
	if (!hp_info->vaddr) {
		hp_info->vaddr = mseg->addr;
		hp_info->paddr = rte_mem_virt2phy(mseg->addr);
		hp_info->len = mseg->len;
	}

	ipc_debug("hugepage info vaddr=%p, paddr=%lu, len=%lu\n",
	      hp_info->vaddr, hp_info->paddr, hp_info->len);

	return;
}

static struct hugepage_info *
get_hugepage_info(struct rte_mempool *mp)
{
	struct hugepage_info *hp_info;

	if (!mp)
		return NULL;

	hp_info = calloc(1, sizeof(struct hugepage_info));
	if (!hp_info) {
		printf("Unable to allocate on local heap\n");
		return NULL;
	}

	rte_mempool_mem_iter(mp, iter_for_each_chunk, hp_info);

	/* XXX Rather than iterator, can be replaced with single step below
	 * which extracts the memhdr in mempool and does the work. test!*/
#if 0
	struct rte_mempool_memhdr *mhdr;
	mhdr = STAILQ_FIRST(mp->mem_list);
	if (mhdr) {
		hp_info->vaddr = mhdr->addr;
		hp_info->iova = rte_mem_virt2phy(mhdr->addr);
		hp_info->len = mhdr->len;
	} else {
		return NULL;
	}
#endif
	return hp_info;
}


static void
fill_buffer(void *buffer, size_t len)
{
	uint32_t i, count;
	int *val = NULL;

	memset(buffer, 0, len);

	/* XXX Endianness is to be taken care of ? */
	val = (int *)buffer;
	count = len/sizeof(int);
	/* XXX Whatif len is not word aligned */

	for (i = 0; i < count; i++) {
		*val = POISON;
		val++;
	}
}

/* Validate if POISON is correctly filled or not */

static int
validate_buffer(void *buffer, size_t len)
{
//	ipc_debug("\n %s %d>>>>>>>>>\n%s\n",__func__, __LINE__, (char *)buffer);
	int ret = 0;
	uint32_t i, count;
	int *val = NULL;

	/* XXX Endianness is to be taken care of ? */
	val = (int *)buffer;
	count = len/sizeof(int);
	/* XXX Whatif len is not word aligned */
	for (i = 0; i < count; i++)
		if (*val != POISON) {
			ret = 1; /* Failed */
			break;
		} else
			val++;

	if (ret) {
		ipc_debug("### Validate buffer FAILED\n");
	}

	return ret;
}

static int
create_mempools(void)
{
	int i;
	char pool_name[32];
	uint32_t elem_size = 0, elem_count = 0;

	memset(pools, 0, IPC_HOST_BUF_MAX_COUNT * \
			 sizeof(struct rte_mempool *));
	memset(pool_name, 0, 32);

	/* A prefix of pool name to create unique names */
	#define POOL_NAME_PREFIX "geul_pool_"

	/* Pool element counts */
	#define POOL_2K_COUNT (102400 + 102400 + 1024)
	#define POOL_16K_COUNT 128
	#define POOL_128K_COUNT 256
	#define SH_POOL_COUNT 100

	/* Other pool values */
	#define PRIVATE_DATA_SIZE 256
	#define CACHE_SIZE 0

	/* For now skipping IPC_HOST_BUF_POOLSZ_R2 */
	for (i = 0; i < IPC_HOST_BUF_MAX_COUNT - 1; i++) {
		sprintf(pool_name, "%s%d", POOL_NAME_PREFIX, i);
		switch(i) {
			case IPC_HOST_BUF_POOLSZ_2K:
				elem_size = 2*1024;
				elem_count = POOL_2K_COUNT;
				break;
			case IPC_HOST_BUF_POOLSZ_16K:
				elem_size = 16*1024;
				elem_count = POOL_16K_COUNT;
				break;
			case IPC_HOST_BUF_POOLSZ_128K:
				elem_size = 128*1024;
				elem_count = POOL_128K_COUNT;
				break;
			case IPC_HOST_BUF_POOLSZ_SH_BUF:
				elem_size = sizeof(ipc_sh_buf_t);
				elem_count = SH_POOL_COUNT;
				break;
			default:
				printf("Invalid size of mempool specified:"
				       "(case %d)\n", i);
				goto cleanup;
		}

		/* Create a pool of (POOL_COUNT * elem_size), no contructor
		 * or destructor, no cache, no private_data and no flags
		 * and on socket_id = 0
		 */
		pools[i] = rte_mempool_create(pool_name, elem_count, elem_size,
					      CACHE_SIZE, PRIVATE_DATA_SIZE,
					      NULL, NULL, NULL, NULL, 0, 0);
		if (!pools[i]) {
			printf("Unable to allocate pool "
			       "(%i:enum ipc_mempool_size)\n", i);
			goto cleanup;
		}
		ipc_debug("Created pool %s with (%ux%u) dimensions\n",
			  pools[i]->name, elem_count, elem_size);
	}

	return 0;

cleanup:
	ipc_debug("Error creating pools: Cleanup initiated\n");
	for (i = 0; i < IPC_HOST_BUF_MAX_COUNT; i++)
		rte_mempool_free(pools[i]);
	return -1;
}

static void
cleanup_mempools(void)
{
	int i;

	for (i = 0; i < IPC_HOST_BUF_MAX_COUNT; i++) {
		rte_mempool_free(pools[i]);
		pools[i] = NULL;
	}
}

static int
initialize_channels(ipc_t instance __rte_unused)
{
	int i, ret = 0;
	struct gul_hif *hif_start = NULL;
	geulipc_channel_t *ch = NULL;
	ipc_userspace_t *ipcu;
	uint8_t en_event = 0;
	struct epoll_event epoll_ev;

	if (!instance) {
		printf("Invalid instance handle\n");
		return -1;
	}

	ipcu = (ipc_userspace_t *)instance;
	hif_start = (struct gul_hif *)ipcu->mhif_start.host_vaddr;

	/* Point to the HIF stats */
	stats = &(hif_start->stats);
	memset(channels, 0, sizeof(struct geulipc_channel *) * CHANNELS_MAX);
	for (i = 0; i < CHANNELS_MAX; i++) {
		channels[i] = malloc(sizeof(geulipc_channel_t));
		if (!channels[i]) {
			printf("Unable to alloc channel mem (%d)\n", i);
			goto cleanup;
		}
		ch = channels[i];

		ipc_debug("Attempting initilaztion (%d)\n", i);

		switch(i) {
#define MSG_CHANNEL_DEPTH 4
		case L2_TO_L1_MSG_CH_1:
			/* 2K Channel */
			strcpy(ch->name, "L2_TO_L1_MSG_CH_1");
			ch->depth = MSG_CHANNEL_DEPTH;
			ch->type = IPC_CH_MSG;
			ch->mp = pools[IPC_HOST_BUF_POOLSZ_2K];
			ch->channel_id = i;
			/* Configure channels is not called for MSG Consumer */
			continue;
		case L2_TO_L1_MSG_CH_2:
			/* 2K Channel */
			strcpy(ch->name, "L2_TO_L1_MSG_CH_2");
			ch->depth = MSG_CHANNEL_DEPTH;
			ch->type = IPC_CH_MSG;
			ch->mp = pools[IPC_HOST_BUF_POOLSZ_2K];
			ch->channel_id = i;
			/* Configure channels is not called for MSG Consumer */
			continue;
		case L2_TO_L1_MSG_CH_3:
			/* 16K Channel */
			strcpy(ch->name, "L2_TO_L1_MSG_CH_3");
			ch->depth = MSG_CHANNEL_DEPTH;
			ch->type = IPC_CH_MSG;
			ch->mp = pools[IPC_HOST_BUF_POOLSZ_16K];
			ch->channel_id = i;
			/* Configure channels is not called for MSG Consumer */
			continue;
		case L1_TO_L2_MSG_CH_4:
			/* 2K Channel */
			strcpy(ch->name, "L1_TO_L2_MSG_CH_4");
			ch->depth = MSG_CHANNEL_DEPTH;
			ch->type = IPC_CH_MSG;
			ch->mp = pools[IPC_HOST_BUF_POOLSZ_2K];
			ch->channel_id = i;
			break;
		case L1_TO_L2_MSG_CH_5:
			/* 2K Channel */
			strcpy(ch->name, "L1_TO_L2_MSG_CH_5");
			ch->depth = MSG_CHANNEL_DEPTH;
			ch->type = IPC_CH_MSG;
			ch->mp = pools[IPC_HOST_BUF_POOLSZ_2K];
			ch->channel_id = i;
#ifndef GOLIVE
			break;
#else
			continue;
#endif
		case L1_TO_L2_PRT_CH_1:
#define PTR_CHANNEL_DEPTH 4
			/* 128K Channel */
			strcpy(ch->name, "L1_TO_L2_PRT_CH_1");
			ch->depth = PTR_CHANNEL_DEPTH;
			ch->type = IPC_CH_PTR;
			ch->mp = pools[IPC_HOST_BUF_POOLSZ_128K];
			ch->channel_id = i;
			break;
		case L1_TO_L2_PRT_CH_2:
			/* 128K Channel */
			strcpy(ch->name, "L1_TO_L2_PRT_CH_2");
			ch->depth = PTR_CHANNEL_DEPTH;
			ch->type = IPC_CH_PTR;
			ch->mp = pools[IPC_HOST_BUF_POOLSZ_128K];
			ch->channel_id = i;
#ifdef GOLIVE
			break;
#else
			continue;
#endif
		default:
			printf("Invalid channel number/type (%d)\n", i);
			goto cleanup;
		}

		en_event = (int_enabled_ch_mask & (1 << ch->channel_id));
		ipc_debug("Configuring channel (%d) with en_event %d\n",
						ch->channel_id, en_event);
		/* Call ipc_configure_channel */
		ret = ipc_configure_channel(ch->channel_id, ch->depth,
					    ch->type, ch->mp->elt_size,
					    en_event, instance);
		if (ret) {
			printf("Unable to configure channel (%d) (err=%d)\n",
			       i, ret);
			goto cleanup;
		}
		/* Store the Event FD if events are required */
		if (en_event) {
			ch->eventfd = ipc_get_eventfd(ch->channel_id, instance);
			/* Register the event */
			epoll_ev.events = EPOLLIN | EPOLLET;
			epoll_ev.data.ptr = (void *)ch;
			ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, ch->eventfd, &epoll_ev);
			if (ret < 0) {
				printf("epoll_ctl ADD failed for Channel ID %d\n",
								ch->channel_id);
				goto cleanup;
			}
			max_events++;
			ipc_debug("Got Event fd (%d)  max_events %d\n", ch->eventfd, max_events);
		} else
			ch->eventfd = -1;
	}
	return ret;

cleanup:
	for (; i > 0; i--) {
		if (channels[i]) {
			free(channels[i]);
			channels[i] = NULL;
		}
	}

	return -1;
}

static ipc_t
setup_ipc(uint16_t devid)
{
	int ret;
	struct rte_rawdev_info rdev_conf = {0};
	struct hugepage_info *hp;
	geulipc_rawdev_config_t config = {0};
	mem_range_t mr = {0};
	ipc_t handle = NULL;

	ret = create_mempools();
	if (ret) {
		printf("Unable to create mempools. Not conitnuing\n");
		return NULL;
	}

	/* Get the hugepage info against it */
	hp = get_hugepage_info(pools[0]);
	if (!hp) {
		printf("Unable to get hugepage info\n");
		goto err_out;
	}

	mr.host_phys = hp->paddr;
	mr.host_vaddr = hp->vaddr;
	mr.size = hp->len;
	ipc_debug("%lx %p %x\n", mr.host_phys, mr.host_vaddr, mr.size);

	/* Call IPC host init */
#if 1
	handle = ipc_host_init(GEUL_INSTANCE_ID, pools, mr, &ret);
#else
	const char *a = "HANDLE";
	printf("Host vaddr=%p\n", mr.host_vaddr);
	handle = (ipc_t)&a;
#endif
	if (ret != IPC_SUCCESS) {
		printf("--->Error from HOST initialization (%d)\n", ret);
		goto err_out;
	}

	/* Create an epoll Fd */
	epoll_fd = epoll_create(1);
	if (epoll_fd < 0) {
		printf("--->Error in creating epoll fd\n");
		goto err_out;
	}
	/* Create the channels and get their IDs */
	ret = initialize_channels(handle);
	if (ret || !channels[0]) {
		printf("Unable to setup channels\n");
		goto err_out;
	}

	/* Send this info of handle to driver - just for future access */
	config.instance_handle = handle;
	config.device_id = GEUL_INSTANCE_ID;

	/* Wrap that into the rte_rawdev_info structure */
	rdev_conf.dev_private = &config;

	/* Configure the Geul device - includes host initialization */
	ret = rte_rawdev_configure(devid, &rdev_conf);
	if (ret < 0) {
		printf("Unable to configure device (%s): (%d)\n",
			   GEUL_DEVICE_NAME, ret);
		goto err_out;
	}

	return handle;

err_out:
	if (handle) {
		/* Ideally some host deinit should be done, but none exists */
		//ipc_host_deinit() //TODO in future, implement
		handle = NULL;
	}
	cleanup_mempools();

	return handle;
}

static int
parse_ch_mask(const char *ch_mask)
{
	char *end = NULL;
	unsigned long mask;

	/* parse hexadecimal string */
	mask = strtoul(ch_mask, &end, 16);
	if ((ch_mask[0] == '\0') || (end == NULL) || (*end != '\0'))
		return 0;

	return mask;
}

static void
usage(char *prgname)
{
	fprintf(stderr, "Usage: %s [EAL args] -- [-t TIMES] [-e CH_MASK]"
			"          [-m MODE]\n"
			"-t TIMES: number of a times the serialized test is"
			"          run (default 1)\n"
			"-e CH_MASK: Mask for Event enabled channels\n"
			"-m MODE : Mode of running:\n"
			"          0 - Verification (default)\n"
			"          1 - Performance (not implemented)\n",
			prgname);
}

static int
parse_args(int argc, char **argv)
{
	int opt;
	int mode;

	while ((opt = getopt(argc, argv, "t:e:m:h")) != -1) {
		switch (opt) {
		case 'h':
			usage(argv[0]);
			exit(0);
		case 't':
			if (!optarg) {
				printf("Arg parse error: Invalid TIMES count\n");
				return -1;
			}
			cycle_times = atoi(optarg);
			if (cycle_times < 0) {
				printf("Arg parse error: Invalid value for TIMES: (%d)\n", cycle_times);
				printf("Assuming default = 1\n");
				cycle_times = 1;
			}
			ipc_debug("Argument: Parsed TIMES = %d\n", cycle_times);
			break;
		case 'e':
			int_enabled_ch_mask = parse_ch_mask(optarg);
			if (int_enabled_ch_mask == -1) {
				printf("Invalid channel mask\n");
				return -1;
			}
			ipc_debug("Argument: int_enabled_ch_mask = 0x%X\n", int_enabled_ch_mask);
			break;
		case 'm':
			if (!optarg) {
				printf("Arg parse error: Invalid MODE value\n");
				return -1;
			}
			mode = atoi(optarg);
			if (mode < 0 || mode >= RUN_MODE_MAX) {
				printf("Arg parse error: Invalid MODE value (%d)\n", mode);
				return -1;
			}
			test_mode = mode;
			ipc_debug("Argument: Parsed Mode = %d\n", test_mode);
			break;
		default:
			usage(argv[0]);
		}
	}

	return 0;
}

static int
is_modem_ready(ipc_t handle __rte_unused)
{
	ipc_debug("%d %s\n", __LINE__, __func__);
	usleep(5000);
	ipc_debug("\n \n \n");
	struct gul_hif *hif_start = NULL;
	ipc_userspace_t *ipcu;
	int ready = 1;

	if (!handle) {
		printf("Invalid handle for modem ready check\n");
		return -1;
	}

	ipcu = (ipc_userspace_t *)handle;
	ipc_debug("%d %s %p\n", __LINE__, __func__, handle);
	hif_start = (struct gul_hif *)ipcu->mhif_start.host_vaddr;
	ipc_debug("%d %s %p\n", __LINE__, __func__, hif_start);

	/* Set Host Read bit */
	ipc_debug("%d %s\n", __LINE__, __func__);
	SET_HIF_HOST_RDY(hif_start, HIF_HOST_READY_IPC_APP);
	ipc_debug("%d %s\n", __LINE__, __func__);

	/* Now wait for modem ready bit */
	while (ready && !force_quit) {
		ready = !CHK_HIF_MOD_RDY(hif_start, HIF_MOD_READY_IPC_APP);
	}

	if (force_quit)
		ready = 1;

	return ready;
}

static int
_send(struct rte_mempool *mp, uint32_t channel_id, ipc_t instance)
{
	int ret, jj=0;
	void *buffer;

	ret = rte_mempool_get(mp, &buffer);
	if (ret) {
		printf("Unable to get pool\n");
		return -1;
	}

	fill_buffer(buffer, mp->elt_size);

repeat:
	ret = ipc_send_msg(channel_id, buffer, mp->elt_size, instance);
	/* XXX clarify what is IPC_BL_* */
	if (ret == IPC_CH_FULL && !force_quit) {
		/* Loop - right now infinitely */
		if ((++jj % 10000 ) == 0) {
			ipc_debug("#");
			jj = 0;
			fflush(stdout);
		}
		//ipc_debug("send_msg returned = %d, repeating\n", ret);
		goto repeat;
	}

	rte_mempool_put(mp, buffer);

	return ret;
}

static int
_recv(struct rte_mempool *mp, uint32_t channel_id, ipc_t instance)
{
	int ret, jj =0;
	uint32_t len;
	void *buffer;

	ret = rte_mempool_get(mp, &buffer);
	if (ret) {
		printf("Unable to get pool\n");
		return -1;
	}

repeat:
	ret = ipc_recv_msg(channel_id, buffer, &len, instance);
	if (ret == IPC_CH_EMPTY && !force_quit) {
		if ((++jj % 10000) == 0) {
			ipc_debug(".");
			jj = 0;
			fflush(stdout);
		}
		goto repeat;
	} else if (ret) {
		printf("Error from ipc_recv_msg %d\n", ret);
		goto out;
	} else if (!ret && len > 0 && len <= mp->elt_size) {
		ret = validate_buffer(buffer, len);
		if (ret) {
			printf("Validation of buffer failed\n");
			goto out;
		}
	} else if (len == 0)
		printf("Invalid length of received buffer. recvd:%u\n", len);
out:
	rte_mempool_put(mp, buffer);
	return ret;
}

static int
_recv_ptr(struct rte_mempool *mp __rte_unused, uint32_t channel_id,
	  ipc_t instance)
{
	int ret, err, jj = 0;
	//void *buffer2;
	ipc_sh_buf_t buffer;
	uint64_t validate_buf = 0;
#ifdef GOLIVE
	ret = rte_mempool_get(mp, &buffer2);
	if (ret) {
		printf("Unable to get pool\n");
		return -1;
	}
#endif
repeat:
	err = ipc_recv_ptr(channel_id, (void *)&buffer, instance);
	if (err == IPC_CH_EMPTY && !force_quit) {
		if ((++jj % 10000) == 0) {
			ipc_debug("*");
			fflush(stdout);
			jj = 0;
		}
		goto repeat;
	} else if (err != IPC_SUCCESS) {
		goto out;
	} else {
		validate_buf = buffer.host_virt_h;
		validate_buf = JOIN_VA32_64_APP(validate_buf, buffer.host_virt_l);
		if (!validate_buf || err) {
			printf("Invalid response from recv_ptr. (%lu)\n", validate_buf);
			goto out;
		}

		/* Buffer is valid, and no error */
		ret = validate_buffer((void *)validate_buf,
				      buffer.data_size);
		if (!buffer.data_size) {
			printf("WARN: %s : Received %d len buffer\n",
						__func__, buffer.data_size);
		}
		ipc_put_buf(channel_id, &buffer, instance);
		if (ret) {
			printf("Invalid buffer in recv_ptr (ret=%d)\n", ret);
			/* XXX Increase stats */
			err = ret;
			goto out;
		}
	}

	err = 0;
out:
#ifdef GOLIVE
	if (ret == 0 && !force_quit)
		ipc_put_buf(channel_id, &buffer, instance);
#endif
	return err;
}
/*
 * Create 3 Senders, each with Non-RT priority.
 * Sender 1: Send 2K;
 * Sender 2: Send 2K
 * Sender 3: Send 16K
 */
static int
non_rt_sender(void *arg)
{
	int ret = 1, i;
	ipc_t instance;

	if (!arg) {
		printf("Invalid call to NON RT thread without args\n");
		return -1;
	}
	instance  = (ipc_t)arg;

	printf(" --> Starting NON RT Sender (lcore_id=%u)\n", rte_lcore_id());

	for (i = 0; i < cycle_times; i++) {
		/* For the L2_TO_L1_MSG_CH_1 */
		ret = _send(channels[L2_TO_L1_MSG_CH_1]->mp,
			    channels[L2_TO_L1_MSG_CH_1]->channel_id,
			    instance);
		if (ret) {
			printf("Unable to send msg on L2_TO_L1_MSG_CH_1 (%d)\n", ret);
			/* XXX For performance, writing stats for each run is
			* bad but, if stats require error collection, it has
			* to be done per send/recv call*/
			return ret;
		}

		/* For the L2_TO_L1_MSG_CH_2 */
		ret = _send(channels[L2_TO_L1_MSG_CH_2]->mp,
			    channels[L2_TO_L1_MSG_CH_2]->channel_id,
			    instance);
		if (ret) {
			printf("Unable to send msg on L2_TO_L1_MSG_CH_2 (%d)\n", ret);
			return ret;
		}

		if (force_quit)
			break;
	}

	ipc_debug("Quiting NON RT Sender thread\n");

	return ret;
}

static int
rt_sender(void *arg)
{
	int ret = 1, i;
	ipc_t instance;
	pid_t tid;
#define COMMAND_LEN 256
	char command[COMMAND_LEN];

	if (!arg) {
		printf("Invalid call to RT thread without args\n");
		return -1;
	}
	instance  = (ipc_t)arg;

	printf(" --> Starting RT Sender (lcore_id=%u)\n", rte_lcore_id());

	/* Call chrt */
	tid = syscall(SYS_gettid);
	snprintf(command, COMMAND_LEN, "chrt -p 90 %d", tid);
	ret = system(command);
	if (ret < 0)
		printf("Unable to set RT priority\n");
	else
		printf("RT Priority set for Send on Core %u\n", rte_lcore_id());

	/* XXX Loop on cycle_times */
	for (i = 0; i < cycle_times; i++) {
		/* For the L2_TO_L1_MSG_CH_3 */
		ret = _send(channels[L2_TO_L1_MSG_CH_3]->mp,
			    channels[L2_TO_L1_MSG_CH_3]->channel_id,
			    instance);
		if (ret) {
			printf("Unable to send msg on L2_TO_L1_MSG_CH_3 (%d)\n", ret);
		}

		if (force_quit)
			break;
	}

	ipc_debug("Quiting RT Sender thread\n");

	return ret;
}
static int
receiver_poll(void *arg __rte_unused)
{
	int ret = 1, i;
	ipc_t instance;

	instance  = (ipc_t)arg;

	printf(" --> Starting Receiver (Poll Mode) (lcore_id=%u)\n", rte_lcore_id());

	/* XXX Loop on cycle_times */
	for (i = 0; i < cycle_times; i++) {
		/* For the L1_TO_L2_MSG_CH_4 */
		if (channels[L1_TO_L2_MSG_CH_4]->eventfd < 0) {
		ipc_debug("loop for _recv L1_TO_L2_MSG_CH_4\n");

		ret = _recv(channels[L1_TO_L2_MSG_CH_4]->mp,
			    channels[L1_TO_L2_MSG_CH_4]->channel_id,
			    instance);
		if (ret)
			printf("Unable to recv msg on L1_TO_L2_MSG_CH_4 (%d)\n", ret);
		}

		if (channels[L1_TO_L2_MSG_CH_5]->eventfd < 0) {
		ipc_debug("loop for _recv L1_TO_L2_MSG_CH_5\n");
		/* For the L1_TO_L2_MSG_CH_5 */
		ret = _recv(channels[L1_TO_L2_MSG_CH_5]->mp,
			    channels[L1_TO_L2_MSG_CH_5]->channel_id,
			    instance);
		if (ret)
			printf("Unable to recv msg on L1_TO_L2_MSG_CH_5 (%d)\n", ret);
		}

		if (channels[L1_TO_L2_PRT_CH_1]->eventfd < 0) {
		ipc_debug("loop for _recv L1_TO_L2_PRT_CH_1\n");
		/* For the L1_TO_L2_PRT_CH_1 */
		ret = _recv_ptr(channels[L1_TO_L2_PRT_CH_1]->mp,
				channels[L1_TO_L2_PRT_CH_1]->channel_id,
				instance);
		if (ret)
			printf("Unable to recv_ptr on L1_TO_L2_MSG_CH_5 (%d)\n", ret);
		}

#if 0	/* PTR Channel 2 is not supported in this release */
		if (channels[L1_TO_L2_PRT_CH_2]->eventfd < 0) {
		/* For the L1_TO_L2_PRT_CH_2 */
		ret = _recv_ptr(channels[L1_TO_L2_PRT_CH_2]->mp,
				channels[L1_TO_L2_PRT_CH_2]->channel_id,
				instance);
		if (ret) {
			printf("Unable to recv msg on L1_TO_L2_PRT_CH_2 (%d)\n", ret);
			return ret;
		}
#endif
		if (force_quit)
			break;
	}

	printf(" --------- Quiting receiver Poll Mode\n");

	return ret;
}


static int
receiver_event(void *arg __rte_unused)
{
	int ret = 0, i, nfds;
	ipc_t instance;
	struct epoll_event events[CHANNELS_MAX];
	geulipc_channel_t *ch = NULL;
	uint64_t timeout_ms = 1000 * 2; /* 2 Sec */

	instance  = (ipc_t)arg;
	printf(" --> Starting Receiver (Event Mode) (lcore_id=%u)\n", rte_lcore_id());

	for (;;) {
		nfds = epoll_wait(epoll_fd, events, max_events, timeout_ms);
		if (nfds < 0) {
			if (errno == EINTR)
				continue;
			else if (errno == EINVAL) /* No FDs left */
				goto done;
			ipc_debug("epoll_wait return fail %d \n", errno);
			return -1;
		} else if (0 == nfds) {
			/*ipc_debug("epoll wait timeout...\n");*/
			if (force_quit) {
				ipc_debug("%s: Quiting.....\n",__func__);
				break;
			} else
				continue;
		}
		for (i = 0; i < nfds; i++) {
			ch = (geulipc_channel_t *) events[i].data.ptr;
			ipc_debug("Got event for Channel Id %d\n", ch->channel_id);

			if (ch->channel_id < L1_TO_L2_PRT_CH_1)
				ret = _recv(ch->mp, ch->channel_id, instance);
			else
				ret = _recv_ptr(ch->mp, ch->channel_id, instance);
			if (ret) {
				ipc_debug("Unable to recv msg on channel Id %d ret %d\n", ch->channel_id, ret);
			}

			if (stats->h_ipc_stats.ipc_ch_stats[ch->channel_id].num_of_msg_recved
					== (uint32_t)cycle_times) {
				epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ch->eventfd, NULL);
				max_events--;
				if (0 == max_events)
					goto done;
			}
		}

		if (force_quit)
			break;
	}
done:
	printf(" -------- Quiting receiver Event Mode\n");

	return ret;
}
static int
receiver(void *arg __rte_unused)
{
	int ret = 1;

	if (!arg) {
		ipc_debug("Invalid call to Receive thread without args\n");
		return -1;
	}
	/* first Check if any channel/s need Polling then process them first */
	ret = receiver_poll (arg);
	if (ret  < 0)
		goto err;
	/* Now move to Event Mode */
	ret = receiver_event (arg);
	if (ret  < 0)
		goto err;

	return ret;

err:
	ipc_debug("Receiver thread Failed...Quiting\n");
	return -1;
}

static void
_dump_stats_per_channel(struct gul_ipc_ch_stats *stats)
{
	printf("recvd = %8u  sent = %8u\n",
	       stats->num_of_msg_recved,
	       stats->num_of_msg_sent);
	printf("total_message_len = %u\n",
	       stats->total_msg_length);
	printf("\tinput_invalid: %u\n", stats->err_input_invalid);
	printf("\tchannel_invalid: %u\n", stats->err_channel_invalid);
	printf("\tinvalid_memory: %u\n", stats->err_mem_invalid);
	printf("\tchannel_full: %u\n", stats->err_channel_full);
	printf("\tchannel_empty: %u\n", stats->err_channel_empty);
	printf("\tbuf_full: %u\n", stats->err_buf_list_full);
	printf("\tbuf_empty: %u\n", stats->err_buf_list_empty);
	printf("\tBuf_alloc_failed: %u\n", stats->err_host_buf_alloc_fail);
	printf("\tioctl_failed: %u\n", stats->err_ioctl_fail);
	printf("\teventfd_reg_failed: %u\n", stats->err_efd_reg_fail);
}

static void
dump_stats(void)
{
	int i;
	struct gul_ipc_stats *m_ipc_stats;
	struct gul_ipc_stats *h_ipc_stats;

	h_ipc_stats = &(stats->h_ipc_stats);
	m_ipc_stats = &(stats->m_ipc_stats);

	printf("##### HOST common stats  ######\n");
	printf("Invalid IPC Instance = %u\n", h_ipc_stats->err_instance_invalid);
	printf("IPC Metadata Size mismatch = %u\n", h_ipc_stats->err_md_sz_mismatch);
	printf("##### MODEM common stats  ######\n");
	printf("Invalid IPC Instance = %u\n", m_ipc_stats->err_instance_invalid);
	printf("IPC Metadata Size mismatch = %u\n", m_ipc_stats->err_md_sz_mismatch);

	printf("-------------------------------------------\n");
	printf("------- Per Channel Stats -----------------\n");
	printf("-------------------------------------------\n");
	for (i = 0; i < CHANNELS_MAX; i++) {
		printf("---- For Channel %s --- \n", channels[i]->name);
		printf("##### HOST Stats ######\n");
		_dump_stats_per_channel(&h_ipc_stats->ipc_ch_stats[i]);
		printf("\n");
		printf("##### MODEM Stats ######\n");
		_dump_stats_per_channel(&m_ipc_stats->ipc_ch_stats[i]);
		printf("-------------------------\n");
	}

	return;
}

static void
signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM) {
		printf("\n\n Signal %d recieved. Preparing to exit\n", signum);
		force_quit = 1;
	}

	/* else, ignore */
}

int
main(int argc, char **argv)
{
	int ret;
	uint16_t devid; /* Geul device ID */
	ipc_t instance_handle = NULL;

	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_panic("[%s] Cannot init EAL\n", argv[0]);

	if (rte_lcore_count() < 4)
		rte_panic("[%s] Cannot Run application as designed to run with minimium of 4 Cores\n", argv[0]);
	argc -= ret;
	argv += ret;

	force_quit = 0;
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* Set Event based rcv mode as default */
	int_enabled_ch_mask = 0x78;

	ret = parse_args(argc, argv);
	if (ret < 0)
		rte_panic("[%s] Unable to parse args.\n", argv[0]);

	/* Create a vdev device; Name of device contains Instance ID
	 * for the Geul IPC device instance
	 */
	ipc_debug("Creating VDEV with name=%s\n", GEUL_DEVICE_NAME);
	ret = rte_vdev_init(GEUL_DEVICE_NAME, "");
	if (ret) {
		printf("Unable to create Geul device (%s)\n", GEUL_DEVICE_NAME);
		goto err_cleanup;
	}

	ret = rte_rawdev_get_dev_id(GEUL_DEVICE_NAME);
	if (ret < 0) {
		printf("Unable to get Geul device ID\n");
		goto cleanup_vdev;
	}
	devid = ret;

	instance_handle = setup_ipc(devid);
	if (!instance_handle) {
		printf("IPC Setup failed\n");
		goto cleanup_vdev;
	}

	/* Synchronize with Modem */
	ret = is_modem_ready(instance_handle);
	if (ret) {
		printf("Modem not ready in stripulated time\n");
		goto cleanup_vdev;
	}

	/* If modem is ready, start the test cycle */

	/* Run Test Case
	 * 1. A Non-RT Sender - for sending on L2_TO_L1_MSG_CH_1,
	 *    L2_TO_L1_MSG_CH_2 and L2_TO_L1_MSG_CH_3
	 * 2. A RT Sender - for sending on L2_TO_L1_MSG_CH_3
	 * 3. A receiver - for receiving on L1_TO_L2_PRT_CH_1 and
	 *    L1_TO_L2_PRT_CH_2
	 * 4. A Stats Dump - *probably* within each thread rather than
	 *    independent thread
	 */

	printf("=-=-=-=-=--=-==-=-=-=-==-=-=-=-=-=-=-=-=-=-=-=-\n");
	printf(" \tPrint # : Non-RT Sender is waiting\n");
	printf(" \tPrint . : RT Sender is waiting\n");
	printf(" \tPrint * : Receiver is waiting\n");
	printf("=-=-=-=-=--=-==-=-=-=-==-=-=-=-=-=-=-=-=-=-=-=-\n");

#define NON_RT_CORE	1
#define RT_CORE		2
#define RECV_CORE	3
//#define STATS_CORE   4

	rte_eal_remote_launch(non_rt_sender, instance_handle, NON_RT_CORE);
	rte_eal_remote_launch(rt_sender, instance_handle, RT_CORE);
	rte_eal_remote_launch(receiver, instance_handle, RECV_CORE);

	rte_eal_mp_wait_lcore();

	dump_stats();

	return 0;

cleanup_vdev:
	rte_vdev_uninit(GEUL_DEVICE_NAME);
	/* Ignoring any errors from rte_vdev_uninit*/

err_cleanup:
	if (epoll_fd >= 0)
		close(epoll_fd);

	return ret;
}
