/*
 *
 * NXP 2019
 *
 * ipc lib.c
 *
 */

/* Not sure how the PCI BAR does translation
 * assuming that translation is not done, so host_phys and modem_phys
 * exits else only host_phys is required
 */

#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>

#include <geul_ipc_um.h>
#include <geul_ipc.h>
#include <gul_host_if.h>
//#define PR(...) printf(__VA_ARGS__)
#define PR(...)

mem_range_t chvpaddr_arr[IPC_MAX_INSTANCE_COUNT][IPC_MAX_CHANNEL_COUNT];
#define TBD 0
#define DONOT_CHECK 1
#define UNUSED(x) (void)x;
#define MHIF_VADDR(A) \
	(void *)((unsigned long)(A) \
			- (ipc_priv->mhif_start.host_phys) \
			+  ipc_priv->mhif_start.vaddr)

#define IPC_CH_VADDR(A) \
	(void *)((unsigned long)(A) \
			- ipc_priv->ipc_start.host_phys \
			+ ipc_priv->ipc_start.host_vaddr)

#define MODEM_V2P(A) \
	((uint32_t) ((unsigned long) (A) \
	 		+ ipc_priv->hugepg_start.modem_phys \
			- (unsigned long )(ipc_priv->hugepg_start.host_vaddr)))
#define HOST_V2P(A) \
	((uint64_t) ((unsigned long) (A) \
	 		+ ipc_priv->hugepg_start.host_phys \
			- (unsigned long )(ipc_priv->hugepg_start.host_vaddr)))
#define MODEM_P2V(A) \
	((uint64_t) ((unsigned long) (A) \
			+ (unsigned long )(ipc_priv->peb_start.host_vaddr)))

#define HOST_RANGE_V(x) \
	(((uint64_t)(x) < (uint64_t)ipc_priv->hugepg_start.host_vaddr || \
	  (uint64_t)(x) > ((uint64_t)ipc_priv->hugepg_start.host_vaddr \
	  + ipc_priv->hugepg_start.size)) == 1 ? 1 : 0)

#define HUGEPG_OFFSET(A) \
		((uint64_t) ((unsigned long) (A) \
		- ((uint64_t)ipc_priv->hugepg_start.host_vaddr)))

#define SPLIT_VA32_H(A) ((uint32_t)((uint64_t)(A)>>32))

#define SPLIT_VA32_L(A) ((uint32_t)(uint64_t)(A))
#define JOIN_VA32_64(H,L) ( (uint64_t)( ((H)<<32) | (L)) )
static inline uint64_t join_va2_64(uint32_t h, uint32_t l)
{
	uint64_t high = 0x0;
	high = h;
	return JOIN_VA32_64(high, l);
}

#if TBD
static void *get_channel_vaddr(uint32_t channel_id, ipc_userspace_t *ipc_priv);
static void *__get_channel_vaddr(uint32_t channel_id,
		ipc_userspace_t *ipc_priv);
static unsigned long get_channel_paddr(uint32_t channel_id,
		ipc_userspace_t *ipc_priv);
static unsigned long __get_channel_paddr(uint32_t channel_id,
		ipc_userspace_t *ipc_priv);
#endif
#if 0 /* NOT needed */
static int get_ipc_inst(ipc_userspace_t *ipc_priv, uint32_t inst_id);
static int get_channels_info(ipc_userspace_t *ipc, uint32_t instance_id);
#endif
void signal_handler(int signo, siginfo_t *siginfo, void *data);

/*AK signal and PID to be sent kernel */

static inline void ipc_fill_errorcode(int *err, int code)
{
	if (err)
		*err = code;
}

static inline int ipc_is_bd_ring_empty(ipc_br_md_t *md)
{
	return ((md->cc == md->pc));
}

static inline int ipc_is_bd_ring_full(ipc_br_md_t *md)
{
	uint32_t cc, pc, ring_size;
	cc = md->cc;
	pc = md->pc;
	ring_size = md->ring_size;

	if (cc > pc)
		return (((UINT32_MAX - cc) + pc) == ring_size);

	return ((pc - cc) == ring_size);
}

static inline int ipc_is_bl_full(ipc_br_md_t *md)
{
	return (md->cc == md->pc);
}

static inline int ipc_is_bl_empty(ipc_br_md_t *md)
{
	uint32_t cc, pc, ring_size;
	cc = md->cc;
	pc = md->pc;
	ring_size = md->ring_size;

	if (pc > cc)
		return (((UINT32_MAX - pc) + cc) == ring_size);

	return ((cc - pc) == ring_size);
}

static inline void ipc_increment_pc(ipc_br_md_t *md)
{
	volatile uint32_t pc = md->pc;
	if (pc == UINT32_MAX)
		pc = 0;
	pc++;
	md->pc = pc;
}

static inline void ipc_increment_cc(ipc_br_md_t *md)
{
	volatile uint32_t cc = md->cc;
	if (cc == UINT32_MAX)
		cc = 0;
	cc++;
	md->cc = cc;
}

static inline int open_devmem(void)
{
        int dev_mem = open("/dev/mem", O_RDWR);
        if (dev_mem < 0) {
                printf("Error: Cannot open /dev/mem \n");
                return -1;
        }
        return dev_mem;
}

static inline int open_devipc(void)
{
        int devipc = open("/dev/gulipcgul0", O_RDWR);
        if (devipc  < 0) {
                printf("Error: Cannot open /dev/ipc_gul_x \n");
                return -1;
        }
        return devipc;
}

static inline void ipc_mark_channel_as_configured(uint32_t channel_id, ipc_instance_t *instance)
{
	/* Read mask */
	ipc_bitmask_t mask = instance->cfgmask[channel_id / bitcount(ipc_bitmask_t)];

	/* Set channel specific bit */
	mask |= 1 << (channel_id % bitcount(mask));

	/* Write mask */
	instance->cfgmask[channel_id / bitcount(ipc_bitmask_t)] = mask;
}

int ipc_is_channel_configured(uint32_t channel_id, ipc_t instance)
{
	ipc_userspace_t *ipc_priv = instance;
	ipc_instance_t *ipc_instance = ipc_priv->instance;
	PR("here %s %d\n \n \n", __func__, __LINE__);

	/* Validate channel id */
	if (!ipc_instance || channel_id >= IPC_MAX_CHANNEL_COUNT)
		return IPC_CH_INVALID;
	PR("here %s %d\n \n \n", __func__, __LINE__);
	/* Read mask */
	ipc_bitmask_t mask = ipc_instance->cfgmask[channel_id / bitcount(ipc_bitmask_t)];
	PR("here %s %d\n \n \n", __func__, __LINE__);

	/* !! to return either 0 or 1 */
	return !!(mask & (1 << (channel_id % bitcount(mask))));
}

/* list array size must be IPC_BITMASK_ARRAY_SIZE */
int ipc_get_list_of_configured_channel(ipc_bitmask_t list[], ipc_t instance)
{
	uint32_t i;
	ipc_userspace_t *ipc_priv = instance;
	ipc_instance_t *ipc_instance = ipc_priv->instance;

	/* Validate instance*/
	if (!ipc_instance || !(ipc_instance->initialized))
		return IPC_INSTANCE_INVALID;

	/* Fill masks from metadata to the argument list */
	for (i = 0; i < IPC_BITMASK_ARRAY_SIZE; i++)
		list[i] = ipc_instance->cfgmask[i];

	return 0;
}

/*
 * Host should init free buffer list
 * So not implemented on modem as of now
 * Internal so done in configure channel
 */
int ipc_init_ptr_buf_list(uint32_t channel_id,
		uint32_t depth, uint32_t size, ipc_t instance)
{
	UNUSED(channel_id);
	UNUSED(instance);
	UNUSED(depth);
	UNUSED(size);

	return IPC_NOT_IMPLEMENTED;
}

/* MODEM ONLY */
ipc_sh_buf_t* ipc_get_buf(uint32_t channel_id, ipc_t instance, int *err)
{
	UNUSED(channel_id);
	UNUSED(instance);
	ipc_fill_errorcode(err, IPC_NOT_IMPLEMENTED);
	return NULL;
}

/*
 * As per current use case/design where PTR channel is used to transfer RX TB
 * from modem to host through shared buffer, This API will be called from host
 * side only to put back the received buffer to free buffer list.
 *
 * HOST only.
 */
int ipc_put_buf(uint32_t channel_id, ipc_sh_buf_t *buf_to_free, ipc_t instance)
{
	pr_debug("here %s %d\n \n \n", __func__, __LINE__);
	ipc_userspace_t *ipc_priv = instance;
	ipc_instance_t *ipc_instance = ipc_priv->instance;
	ipc_ch_t *ch;
	ipc_br_md_t *md;
	uint32_t ring_size, pi, ci, cc, pc;
	ipc_sh_buf_t *sh = buf_to_free;
	uint64_t range = 0;

	UNUSED(pc);
	UNUSED(cc);
	UNUSED(ci);

	if (!ipc_instance || !ipc_instance->initialized)
		return IPC_INSTANCE_INVALID;

	if (channel_id >= IPC_MAX_CHANNEL_COUNT)
		return IPC_CH_INVALID;

	range = join_va2_64(sh->host_virt_h, sh->host_virt_l);
	if (HOST_RANGE_V(range))
		return IPC_INPUT_INVALID;

	ch = &(ipc_instance->ch_list[channel_id]);
#if DONOT_CHECK
	if (!ipc_is_channel_configured(channel_id, ipc_priv) ||
			ch->ch_type != IPC_CH_PTR || !ch->bl_initialized)
		return IPC_CH_INVALID;
#endif
	md = &(ch->br_bl_desc.md);
	if (ipc_is_bl_full(md))
		return IPC_BL_FULL;

	ring_size = md->ring_size;

	pi = md->pi;
	cc = md->cc;
	pc = md->pc;
	ci = md->ci; /* Modification from consumer only */

	/* TO ipc_increment_cc(md);*/
	pr_debug("%s enter: pi: %u, ci: %u, pc: %u, cc: %u, ring size: %u\r\n", __func__,
			pi, ci, pc, cc, ring_size);

	pi = (pi + 1) % ring_size;
	md->pi = pi;
	ipc_increment_pc(md);
	pr_debug("%d %s sh->host_virt_h=%x sh->host_virt_l=%x sh->mod_phys=%x\n\n",__LINE__, __func__,
			sh->host_virt_h, sh->host_virt_l, sh->mod_phys);

	/* Copy back to ipc_sh_buf_t */
	memcpy((void *)sh, &ch->br_bl_desc.bd[pi], sizeof(ipc_sh_buf_t));
	pr_debug("%d %s ch->br_bl_desc.bd[pi].mod_phys=%x ch->br_bl_desc.bd[pi].host_virt_l=%x host_virt_h=%x\n\n",__LINE__, __func__,
			ch->br_bl_desc.bd[pi].mod_phys,
			ch->br_bl_desc.bd[pi].host_virt_l,
			ch->br_bl_desc.bd[pi].host_virt_h);
#if 0 /* REMOVE*/
	ch->br_bl_desc.bd[pi].mod_phys = HUGEPG_OFFSET(sh);
	ch->br_bl_desc.bd[pi].host_virt_l = SPLIT_VA32_L(sh);
	ch->br_bl_desc.bd[pi].host_virt_h = SPLIT_VA32_H(sh);
	//ch->br_bl_desc.bd[pi].host_phys = HOST_V2P(sh); /* Should be unused */
#endif

	pr_debug("%s exit: pi: %u, ci: %u, md->pi: %u, pc: %u, ring size: %u\r\n", __func__,
			pi, ci, md->pi, md->pc, ring_size);

	return IPC_SUCCESS;
}

int ipc_send_ptr(uint32_t channel_id,
		ipc_sh_buf_t *buf,
		ipc_t instance)
{
	UNUSED(channel_id);
	UNUSED(buf);
	UNUSED(instance);

	return IPC_NOT_IMPLEMENTED;
}

/*
 * Not to be implemented as of now.
 */
int ipc_get_prod_buf_ptr(uint32_t channel_id, void **buf_ptr, ipc_t instance)
{
	UNUSED(channel_id);
	UNUSED(buf_ptr);
	UNUSED(instance);

	return IPC_NOT_IMPLEMENTED;
}

int ipc_send_msg(uint32_t channel_id,
		void *src,
		uint32_t len,
		ipc_t instance)
{
	ipc_userspace_t *ipc_priv = instance;
	ipc_instance_t *ipc_instance = ipc_priv->instance;
	ipc_ch_t *ch = &(ipc_instance->ch_list[channel_id]);
	ipc_br_md_t *md = &(ch->br_msg_desc.md);
	uint32_t ring_size, pi, ci, cc, pc;
	ipc_bd_t *bdr, *bd;
	uint64_t virt;

	UNUSED(pc);
	UNUSED(cc);
	UNUSED(ci);

	PR("here %s %d\n \n \n", __func__, __LINE__);
	if (!src || !len)
		return IPC_INPUT_INVALID;
	PR("here %s %d\n \n \n", __func__, __LINE__);

	if (!ipc_instance || !ipc_instance->initialized)
		return IPC_INSTANCE_INVALID;
	PR("here %s %d\n \n \n", __func__, __LINE__);

	if (channel_id >= IPC_MAX_CHANNEL_COUNT)
		return IPC_CH_INVALID;
	PR("here %s %d\n \n \n", __func__, __LINE__);
#if DONOT_CHECK
	if (!ipc_is_channel_configured(channel_id, ipc_priv))
		return IPC_CH_INVALID;
	PR("here %s %d\n \n \n", __func__, __LINE__);
#endif
	if (len > md->msg_size)
		return IPC_INPUT_INVALID;
	PR("here %s %d\n \n \n", __func__, __LINE__);

	ring_size = md->ring_size;
	PR("here %s %d\n \n \n", __func__, __LINE__);


	ci = md->ci;
	pi = md->pi;
	cc = md->cc;
	pc = md->pc;
	pr_debug("%s before bd_ring_full: pi: %u, ci: %u, pc: %u, cc: %u, ring size: %u\r\n", __func__,
			pi, ci, pc, cc, ring_size);

	if (ipc_is_bd_ring_full(md))
		return IPC_CH_FULL;

	pr_debug("%s enter: pi: %u, ci: %u, pc: %u, cc: %u, ring size: %u\r\n", __func__,
			pi, ci, pc, cc, ring_size);

	bdr = ch->br_msg_desc.bd;
	bd = &bdr[pi];
	PR("here %s %d\n \n \n", __func__, __LINE__);
	//bd->host_virt = MODEM_P2V(bd->modem_ptr);
	virt = MODEM_P2V(bd->modem_ptr);
	memcpy((void *)(virt), src, len);
	PR("here %s %d\n \n \n", __func__, __LINE__);
	bd->len = len;

	pi = (pi + 1) % ring_size;
	PR("here %s %d\n \n \n", __func__, __LINE__);

	md->pi = pi;
	ipc_increment_pc(md);
	rte_mb();
	PR("here %s %d\n \n \n", __func__, __LINE__);

	pr_debug("%s exit: pi: %u, ci: %u, pc: %u, cc: %u, ring size: %u\r\n", __func__,
			pi, ci, md->pc, cc, ring_size);
	PR("here %s %d\n \n \n", __func__, __LINE__);

	return IPC_SUCCESS;
}

/*
 * PTR channel is used to transfer RX TB from modem to host.
 * So this API will only be used by host to receive RX TB.
 */
int ipc_recv_ptr(uint32_t channel_id, void *dst, ipc_t instance)
{
	PR("here %s %d\n \n \n", __func__, __LINE__);
	ipc_userspace_t *ipc_priv = instance;
	ipc_instance_t *ipc_instance = ipc_priv->instance;
	ipc_ch_t *ch;
	ipc_br_md_t *md;
	uint32_t cc, pc, ring_size, pi, ci, msg_len;
	ipc_bd_t *bdr, *bd;
	ipc_sh_buf_t sh;
	uint64_t vaddr2 =0x0;

	UNUSED(pc);
	UNUSED(cc);
	UNUSED(ci);
	UNUSED(pi);

	if (!ipc_instance || !ipc_instance->initialized) {
		return IPC_INSTANCE_INVALID;
	}

	if (channel_id >= IPC_MAX_CHANNEL_COUNT) {
		return IPC_CH_INVALID;
	}

	ch = &(ipc_instance->ch_list[channel_id]);
#if DONOT_CHECK
	if (!ipc_is_channel_configured(channel_id, ipc_priv)) {
		return IPC_CH_INVALID;
	}
#endif
	md = &(ch->br_msg_desc.md);
	if (ipc_is_bd_ring_empty(md))
		return IPC_CH_EMPTY;

	ci = md->ci;
	cc = md->cc;
	pc = md->pc;
	pi = md->pi; /* Modification from Producer only */
	ring_size = md->ring_size;

	pr_debug("\n %s enter: pi: %u, ci: %u, pc: %u, cc: %u, ring size: %u\r\n", __func__,
			pi, ci, pc, cc, ring_size);

	bdr = ch->br_msg_desc.bd;
	bd = &bdr[ci];

	PR("%d %s bd->host_virt_h=%x bd->host_virt_l=%x offset=%x\n\n",__LINE__, __func__,bd->host_virt_h, bd->host_virt_l, bd->modem_ptr);
	vaddr2 = join_va2_64(bd->host_virt_h, bd->host_virt_l);
#if 1
	msg_len = bd->len;
	if (msg_len > md->msg_size || msg_len == 0) {
		printf("%d %s ERROR size=%x\n\n",__LINE__, __func__, msg_len);
		//return IPC_CH_INVALID;
	}
	memcpy(dst, (void *)vaddr2, sizeof(ipc_sh_buf_t));
	memcpy((void *)&sh, (void *)vaddr2, sizeof(ipc_sh_buf_t));
#endif
	PR("%d %s size=%x\n\n",__LINE__, __func__, msg_len);
	PR("%d %s sh->host_virt_h=%x sh->host_virt_l=%x sh->mod_phys=%x\n\n",
	   __LINE__, __func__, sh.host_virt_h, sh.host_virt_l, sh.mod_phys);

	ci = (ci + 1) % ring_size;
	md->ci = ci;
	ipc_increment_cc(md);

	pr_debug("\n %s exit: pi: %u, ci: %u, pc: %u, cc: %u, ring size: %u\r\n", __func__,
			pi, ci, pc, md->cc, ring_size);
	pr_debug("%s %d %s\n\n",__func__, __LINE__, (char *)vaddr2);
	return IPC_SUCCESS;
}

int ipc_recv_msg(uint32_t channel_id, void *dst,
		uint32_t *len, ipc_t instance)
{
	ipc_userspace_t *ipc_priv = instance;
	ipc_instance_t *ipc_instance = ipc_priv->instance;
	ipc_ch_t *ch;
	ipc_br_md_t *md;
	uint32_t cc, pc, ring_size, pi, ci, msg_len;
	uint64_t vaddr2 = 0;
	ipc_bd_t *bdr, *bd;

	UNUSED(pc);
	UNUSED(cc);
	UNUSED(ci);
	UNUSED(pi);

	if (!dst || !len)
		return IPC_INPUT_INVALID;

	if (!ipc_instance || !ipc_instance->initialized)
		return IPC_INSTANCE_INVALID;

	if (channel_id >= IPC_MAX_CHANNEL_COUNT)
		return IPC_CH_INVALID;

	ch = &(ipc_instance->ch_list[channel_id]);
#if DONOT_CHECK
	if (!ipc_is_channel_configured(channel_id, ipc_priv))
		return IPC_CH_INVALID;
#endif
	md = &(ch->br_msg_desc.md);
	if (ipc_is_bd_ring_empty(md))
		return IPC_CH_EMPTY;

	pc = md->pc;
	cc = md->cc;
	pi = md->pi;
	ci = md->ci;
	ring_size = md->ring_size;

	pr_debug("%s enter: pi: %u, ci: %u, pc: %u, cc: %u, ring size: %u\r\n", __func__,
			pi, ci, pc, cc, ring_size);

	bdr = ch->br_msg_desc.bd;
	bd = &bdr[ci];

	msg_len = bd->len;
	if (msg_len > md->msg_size)
		return IPC_INPUT_INVALID;
	PR("%d %s\n\n",__LINE__, __func__);
	vaddr2 = join_va2_64(bd->host_virt_h, bd->host_virt_l);
	//memcpy(dst, (void *)(bd->host_virt), msg_len);
	memcpy(dst, (void *)(vaddr2), msg_len);
	PR("%d %s\n\n",__LINE__, __func__);
	*len = msg_len;

	ci = (ci + 1) % ring_size;
	md->ci = ci;
	ipc_increment_cc(md);

	pr_debug("%s exit: pi: %u, ci: %u, pc: %u, cc: %u, ring size: %u\r\n", __func__,
			pi, ci, pc, md->cc, ring_size);

	return 0;
}

int ipc_recv_msg_ptr(uint32_t channel_id, void **dst_buffer,
		uint32_t *len, ipc_t instance)
{
	ipc_userspace_t *ipc_priv = instance;
	ipc_instance_t *ipc_instance = ipc_priv->instance;
	ipc_ch_t *ch;
	ipc_br_md_t *md;
	uint32_t cc, pc, ring_size, pi, ci;
	ipc_bd_t *bdr, *bd;
	uint64_t vaddr2;

	UNUSED(pc);
	UNUSED(cc);
	UNUSED(ci);
	UNUSED(pi);
	UNUSED(ring_size);

	if (!dst_buffer || !(*dst_buffer) || !len)
		return IPC_INPUT_INVALID;

	if (!ipc_instance || !ipc_instance->initialized)
		return IPC_INSTANCE_INVALID;

	if (channel_id >= IPC_MAX_CHANNEL_COUNT)
		return IPC_CH_INVALID;

	ch = &(ipc_instance->ch_list[channel_id]);

	if (!ipc_is_channel_configured(channel_id, ipc_priv))
		return IPC_CH_INVALID;

	md = &(ch->br_msg_desc.md);
	if (ipc_is_bd_ring_empty(md))
		return IPC_CH_EMPTY;

	ci = md->ci;
	pi = md->pi;
	cc = md->cc;
	pc = md->pc;
	ring_size = md->ring_size;


	pr_debug("%s pi: %u, ci: %u, pc: %u, cc: %u, ring size: %u\r\n", __func__,
			pi, ci, pc, cc, ring_size);

	bdr = ch->br_msg_desc.bd;
	bd = &bdr[ci];
	/* host_phys and virt was done in configure*/
	PR("%d %s\n\n",__LINE__, __func__);
	vaddr2 = join_va2_64(bd->host_virt_h, bd->host_virt_l);
	//*dst_buffer = (void * )bd->host_virt;
	*dst_buffer = (void *)vaddr2;
	*len = bd->len;

	/* ipc_set_consumed_status needed to called by user*/
	/* as occupied and ci is not decremented */

	return IPC_SUCCESS;
}

int ipc_set_produced_status(uint32_t channel_id, ipc_t instance)
{
	UNUSED(channel_id);
	UNUSED(instance);

	return IPC_NOT_IMPLEMENTED;
}

int ipc_set_consumed_status(uint32_t channel_id, ipc_t instance)
{
	ipc_userspace_t *ipc_priv = instance;
	ipc_instance_t *ipc_instance = ipc_priv->instance;
	ipc_ch_t *ch;
	ipc_br_md_t *md;
	uint32_t cc, pc, ring_size, pi, ci;

	UNUSED(pc);
	UNUSED(cc);
	UNUSED(ci);
	UNUSED(pi);

	if (!ipc_instance || !ipc_instance->initialized)
		return IPC_INSTANCE_INVALID;

	if (channel_id >= IPC_MAX_CHANNEL_COUNT)
		return IPC_CH_INVALID;

	ch = &(ipc_instance->ch_list[channel_id]);
#if DONOT_CHECK
	if (!ipc_is_channel_configured(channel_id, ipc_priv))
		return IPC_CH_INVALID;
#endif
	md = &(ch->br_msg_desc.md);
	if (ipc_is_bd_ring_empty(md))
		return IPC_CH_EMPTY;

	ci = md->ci;
	pi = md->pi;
	cc = md->cc;
	pc = md->pc;
	ring_size = md->ring_size;

	pr_debug("%s enter: pi: %u, ci: %u cc: %u pc: %u, ring size: %u\r\n",
			__func__, pi, ci, cc, pc, ring_size);

	ci = (ci + 1) % ring_size;
	md->ci = ci;
	ipc_increment_cc(md);

	pr_debug("%s exit: pi: %u, ci: %u, cc: %u, ring size: %u\r\n",
			__func__, pi, ci, md->cc, ring_size);

	return IPC_SUCCESS;
}

/* TODO: Implement below API for Geul */
int ipc_chk_recv_status(uint64_t *bmask, ipc_t instance)
{
	UNUSED(bmask);
	UNUSED(instance);

	return IPC_NOT_IMPLEMENTED;
}

int ipc_shutdown(ipc_t ipc)
{
	int i;
	ipc_userspace_t *ipc_priv = (ipc_userspace_t *)ipc;

	/* close dev/geulipc */
	close(ipc_priv->dev_ipc);
	close(ipc_priv->dev_mem);

	/* free memory */
	for (i = 0; i < IPC_MAX_CHANNEL_COUNT; i++)
		free(ipc_priv->channels[i]);

	/* free ipc */
	free(ipc_priv);
	return IPC_SUCCESS;
}

ipc_t ipc_host_init(uint32_t instance_id,
		struct rte_mempool *rtemempool[MAX_MEM_POOL_COUNT],
		mem_range_t hugepgstart, int *err)
{
	PR("Amit:1");
	ipc_userspace_t *ipc_priv;
	ipc_channel_us_t *ipc_priv_ch;
	int ret, dev_ipc, dev_mem, i;
	ipc_metadata_t *ipc_md;
	struct gul_hif *mhif;
	uint32_t phy_align = 0;

	ipc_priv = malloc(sizeof(ipc_userspace_t));
	if (ipc_priv == NULL) {
		ipc_fill_errorcode(err, IPC_MEM_INVALID);
		return NULL;
	}
	memset(ipc_priv, 0, sizeof(ipc_userspace_t));

	for (i = 0; i < IPC_MAX_CHANNEL_COUNT; i++) {
		ipc_priv_ch = malloc(sizeof(ipc_channel_us_t));
		if (ipc_priv_ch == NULL) {
			ipc_fill_errorcode(err, IPC_MALLOC_FAIL);
			return NULL;
		}
		ipc_priv->channels[i] = ipc_priv_ch;
	}
	dev_mem = open_devmem();
	if (dev_mem < 0) {
		ipc_fill_errorcode(err, IPC_OPEN_FAIL);
		return NULL;
	}

	dev_ipc = open_devipc();
	if (dev_ipc < 0) {
		ipc_fill_errorcode(err, IPC_OPEN_FAIL);
		//return NULL;
	}

	ipc_priv->instance_id = instance_id;
	ipc_priv->dev_ipc = dev_ipc;
	ipc_priv->dev_mem = dev_mem;

	PR("hugepg input %lx %p %x\n", hugepgstart.host_phys , hugepgstart.host_vaddr, hugepgstart.size);

	ipc_priv->sys_map.hugepg_start.host_phys = hugepgstart.host_phys;
	ipc_priv->sys_map.hugepg_start.size = hugepgstart.size;
	/* Send IOCTL to get system map */
	/* Send IOCTL to put hugepg_start map */
	ret = ioctl(dev_ipc, IOCTL_GUL_IPC_GET_SYS_MAP, &ipc_priv->sys_map);
	 if (ret) {
		ipc_fill_errorcode(err, IPC_IOCTL_FAIL);
		return NULL;

	}
	phy_align = (ipc_priv->sys_map.mhif_start.host_phys % 0x1000);
	ipc_priv->mhif_start.host_vaddr =
		mmap(0, ipc_priv->sys_map.mhif_start.size, (PROT_READ | \
			PROT_WRITE), MAP_SHARED, dev_mem, \
			(ipc_priv->sys_map.mhif_start.host_phys - phy_align));
	if (ipc_priv->mhif_start.host_vaddr == MAP_FAILED) {
		 perror("MAP failed:");
		ipc_fill_errorcode(err, IPC_MMAP_FAIL);
		return NULL;
	} else
		ipc_priv->mhif_start.host_vaddr = (void *)
			((uint64_t)(ipc_priv->mhif_start.host_vaddr) + phy_align);

	phy_align = (ipc_priv->sys_map.peb_start.host_phys % 0x1000);
	ipc_priv->peb_start.host_vaddr =
		mmap(0, ipc_priv->sys_map.peb_start.size, (PROT_READ | \
			PROT_WRITE), MAP_SHARED, dev_mem, \
			(ipc_priv->sys_map.peb_start.host_phys - phy_align));
	if (ipc_priv->peb_start.host_vaddr == MAP_FAILED) {
		perror("MAP failed:");
		ipc_fill_errorcode(err, IPC_MMAP_FAIL);
		return NULL;
	} else
		ipc_priv->peb_start.host_vaddr = (void *)
			((uint64_t)(ipc_priv->peb_start.host_vaddr) + phy_align);

	ipc_priv->hugepg_start.host_phys = hugepgstart.host_phys;
	ipc_priv->hugepg_start.host_vaddr = hugepgstart.host_vaddr;
	ipc_priv->hugepg_start.size = ipc_priv->sys_map.hugepg_start.size;
	ipc_priv->hugepg_start.modem_phys = ipc_priv->sys_map.hugepg_start.modem_phys;

	ipc_priv->mhif_start.host_phys = ipc_priv->sys_map.mhif_start.host_phys;
	ipc_priv->mhif_start.size = ipc_priv->sys_map.mhif_start.size;
	ipc_priv->peb_start.host_phys = ipc_priv->sys_map.peb_start.host_phys;
	ipc_priv->peb_start.size = ipc_priv->sys_map.peb_start.size;

	/*These handle to be used create dpdk pool of 2K 16k and 128k */
	ipc_priv->rtemempool[IPC_HOST_BUF_POOLSZ_2K] = rtemempool[0];
	ipc_priv->rtemempool[IPC_HOST_BUF_POOLSZ_16K] = rtemempool[1];
	ipc_priv->rtemempool[IPC_HOST_BUF_POOLSZ_128K] = rtemempool[2];
	ipc_priv->rtemempool[IPC_HOST_BUF_POOLSZ_SH_BUF] = rtemempool[3];
	ipc_priv->rtemempool[IPC_HOST_BUF_POOLSZ_R2] = rtemempool[4];

	PR("peb %lx %p %x\n", ipc_priv->peb_start.host_phys , ipc_priv->peb_start.host_vaddr, ipc_priv->peb_start.size);
	PR("hugepg %lx %p %x\n", ipc_priv->hugepg_start.host_phys , ipc_priv->hugepg_start.host_vaddr, ipc_priv->hugepg_start.size);
	PR("mhif %lx %p %x\n", ipc_priv->mhif_start.host_phys , ipc_priv->mhif_start.host_vaddr, ipc_priv->mhif_start.size);
	mhif = (struct gul_hif *)ipc_priv->mhif_start.host_vaddr;
	/* offset is from start of PEB */
	ipc_md = (ipc_metadata_t *)((uint64_t)ipc_priv->peb_start.host_vaddr + mhif->ipc_regs.ipc_mdata_offset);

	if (sizeof(ipc_metadata_t) !=
			mhif->ipc_regs.ipc_mdata_size) {
		ipc_fill_errorcode(err, IPC_MD_SZ_MISS_MATCH);
		PR("\n ipc_metadata_t =%lx, mhif->ipc_regs.ipc_mdata_size=%x\n", sizeof(ipc_metadata_t), mhif->ipc_regs.ipc_mdata_size);
		PR("--> mhif->ipc_regs.ipc_mdata_offset= %x\n", mhif->ipc_regs.ipc_mdata_offset);
		PR("gul_hif size=%lx, \n", sizeof(struct gul_hif));

		//return NULL;
	}

	ipc_priv->instance = (ipc_instance_t *)(&ipc_md->instance_list[instance_id]);
#if 0
	ret = get_channels_info(ipc_priv, instance_id);
	if (ret) {
		if(!err)
			*err = ERROR_IOCTL;
		return NULL;
	}
#endif
	PR("finish host init\n");
	return ipc_priv;
}

int ipc_configure_channel(uint32_t channel_id, uint32_t depth, ipc_ch_type_t channel_type,
		uint32_t msg_size, uint8_t en_event, ipc_t instance)
{

	ipc_userspace_t *ipc_priv = (ipc_userspace_t *)instance;
	ipc_instance_t *ipc_instance = ipc_priv->instance;
	ipc_ch_t *ch;
	void *vaddr;
	uint32_t i = 0;
	int ret, event_fd;

	PR("%x %p\n", ipc_instance->initialized, ipc_priv->instance);
	pr_debug("%s: channel: %u, depth: %u, type: %d, msg size: %u\r\n",
			__func__, channel_id, depth, channel_type, msg_size);
	if (!ipc_priv->instance || !ipc_instance->initialized)
		return IPC_INSTANCE_INVALID;

	if (channel_id >= IPC_MAX_CHANNEL_COUNT)
		return IPC_CH_INVALID;

	if (depth > IPC_MAX_DEPTH)
		return IPC_CH_INVALID;

	ch = &(ipc_instance->ch_list[channel_id]);

	PR("here %s %d\n \n \n", __func__, __LINE__);
#if 1
	if (ipc_is_channel_configured(channel_id, ipc_priv)) {
		printf("WARNING: [%s]: Channel already configured\n NOT configuring again\n",__func__);
		return IPC_SUCCESS;
	}
#endif
	pr_debug("%s: channel: %u, depth: %u, type: %d, msg size: %u\r\n",
			__func__, channel_id, depth, channel_type, msg_size);

	/* Start init of channel */
	ch->ch_type = channel_type;
	ch->ch_id = channel_id; /* May not be required since modem does this */
#if 0
	if (ch->bl_initialized == 1) {
		printf("WARNING: [%s]: Channel already configured\n NOT configuring again\n",__func__);
		return IPC_SUCCESS;
	}
#endif

	if (channel_type == IPC_CH_MSG) {
		ch->br_msg_desc.md.ring_size = depth;
		ch->br_msg_desc.md.cc = 0;
		ch->br_msg_desc.md.pi = 0;
		ch->br_msg_desc.md.ci = 0;
		ch->br_msg_desc.md.msg_size = msg_size;
	//	ch->br_msg_desc.bd[i].len = 0; /* not sure use of this len */
		for (i = 0; i < depth; i++) {
			if (msg_size == SIZE_2K) {
				ret = rte_mempool_get(ipc_priv->rtemempool[IPC_HOST_BUF_POOLSZ_2K], &vaddr);
				if (ret < 0)
					return IPC_HOST_BUF_ALLOC_FAIL;
			} else if (msg_size == SIZE_16K) {
				ret = rte_mempool_get(ipc_priv->rtemempool[IPC_HOST_BUF_POOLSZ_16K], &vaddr);
				if (ret < 0)
					return IPC_HOST_BUF_ALLOC_FAIL;
			}
	PR("---> %d %s V1=%p V2=%p i=%d\n \n \n",__LINE__, __func__, vaddr, ipc_priv->hugepg_start.host_vaddr, i);
			ch->br_msg_desc.bd[i].modem_ptr = HUGEPG_OFFSET(vaddr); /* Only offset now */
		//	ch->br_msg_desc.bd[i].modem_ptr = 0xdeadbeef;
		//	ch->br_msg_desc.bd[i].host_phy_l = 0xdeafbee1;
		//	ch->br_msg_desc.bd[i].host_phy_h = 0xdeafbee2;
	PR("%d %s vaddr %p\n \n \n",__LINE__, __func__, vaddr);
			ch->br_msg_desc.bd[i].host_virt_l = SPLIT_VA32_L(vaddr);
			ch->br_msg_desc.bd[i].host_virt_h = SPLIT_VA32_H(vaddr);
			ch->br_msg_desc.bd[i].len = 0; /* not sure use of this len may be for CRC*/
		}
		ch->bl_initialized = 1;
	}

	if (channel_type == IPC_CH_PTR) {
	PR("%d %s\n \n \n",__LINE__, __func__);
		/* do_dpdk_alloc using rtemempool;
		and fill in ipc_sh_buf_t[];
		translate using hugepgstart and hugepgtart.modem
		*/
		/* Fill msg */
		ch->br_msg_desc.md.ring_size = depth;
		ch->br_msg_desc.md.cc = 0;
		ch->br_msg_desc.md.pi = 0;
		ch->br_msg_desc.md.ci = 0;
		ch->br_msg_desc.md.msg_size = sizeof(ipc_sh_buf_t);
		ch->br_msg_desc.bd[i].len = 0; /* should be same as ipc_sh_buf_t always*/

		/* Fill bl */
		ch->br_bl_desc.md.ring_size = depth;
		ch->br_bl_desc.md.cc = 0; /* think again*/
		ch->br_bl_desc.md.pi = 0;
		ch->br_bl_desc.md.ci = 0;
		ch->br_bl_desc.md.msg_size = msg_size; /*  128K */
		for (i = 0; i < depth; i++) {
			/* Fill bl ring */
			ret = rte_mempool_get(ipc_priv->rtemempool[IPC_HOST_BUF_POOLSZ_128K], &vaddr);
			if (ret < 0)
				return IPC_HOST_BUF_ALLOC_FAIL;

			ch->br_bl_desc.bd[i].mod_phys = HUGEPG_OFFSET(vaddr);
			ch->br_bl_desc.bd[i].host_virt_l = SPLIT_VA32_L(vaddr);
			ch->br_bl_desc.bd[i].host_virt_h = SPLIT_VA32_H(vaddr);
			//ch->br_bl_desc.bd[i].host_phys = HOST_V2P(vaddr); /* Should be unused */
			/* ch->br_bl_desc.bd[i].buf_size should be unused */
			/* ch->br_bl_desc.bd[i].data_size to be filled by producer */
			/* ch->br_bl_desc.bd[i].cookie = 0; */ /*unused as of now*/

			/* Fill msg ring */
			ret = rte_mempool_get(ipc_priv->rtemempool[IPC_HOST_BUF_POOLSZ_SH_BUF], &vaddr);
			if (ret < 0)
				return IPC_HOST_BUF_ALLOC_FAIL;

			ch->br_msg_desc.bd[i].modem_ptr = HUGEPG_OFFSET(vaddr);
			ch->br_msg_desc.bd[i].host_virt_l = SPLIT_VA32_L(vaddr);
			ch->br_msg_desc.bd[i].host_virt_h = SPLIT_VA32_H(vaddr);
			/* ch->br_msg_desc.bd[i].host_phy = HOST_V2P(vaddr); DO not access now bus error. Should be used to fetch ipc_sh_buf_t*/
		}
		ch->bl_initialized = 1;
	}

	if (en_event) {
		ipc_eventfd_t efd_args;

		/* Open an Event FD to get events from kernel.*/
		event_fd = eventfd(0, EFD_NONBLOCK);
		if (event_fd < 0) {
			perror("Eventfd allocation Failed: ");
			return IPC_EVENTFD_FAIL;
		}
		printf("Eventfd %d for Channel ID %d\n", event_fd, channel_id);
		ipc_priv->channels[channel_id]->eventfd = event_fd;

		/* Send IOCTL to register this event_fd with kernel*/
		efd_args.efd = event_fd;
		efd_args.ipc_channel_num = channel_id;
		ret = ioctl(ipc_priv->dev_ipc, IOCTL_GUL_IPC_CHANNEL_REGISTER, &efd_args);
		if (ret) {
			printf("IPC_CHANNEL_REGISTER failed for Channel ID %d\n", channel_id);
			return IPC_IOCTL_FAIL;
		}
		/* Store the received MSI Value */
		ch->msi_value = efd_args.msi_value;
		ch->msi_valid = 1;
		printf("got MSI %d for Channel ID %d\n", efd_args.msi_value, channel_id);

	} else {
		ipc_priv->channels[channel_id]->eventfd = -1;
		ch->msi_valid = 0;
	}
	ipc_mark_channel_as_configured(channel_id, ipc_priv->instance);
	PR("finish configure\n");
	return IPC_SUCCESS;

}

int32_t ipc_get_eventfd(uint32_t channel_id, ipc_t instance)
{
	ipc_userspace_t *ipc_priv = (ipc_userspace_t *)instance;
	return ipc_priv->channels[channel_id]->eventfd;
}

#if TBD
/**************** Internal API ************************/
#if 0 /*AK not needed */
/*
 * @get_channels_info
 *
 * Read number of channels and max msg size from sh_ctrl_area
 *
 * Type: Internal function
 */

int get_ipc_inst(ipc_userspace_t *ipc_priv, uint32_t inst_id)
{
	int ret = IPC_SUCCESS;
	ENTER();

	os_het_control_t *sh_ctrl =  ipc_priv->sh_ctrl_area.vaddr;
	os_het_ipc_t *ipc = IPC_CH_VADDR(sh_ctrl->ipc)
				+ sizeof(os_het_ipc_t)*inst_id;
	if (!ipc) {
		ret = -1;
		goto end;
	}
	if (ipc->num_ipc_channels > MAX_IPC_CHANNELS) {
		ret = -1;
		goto end;
	}

	/* ipc_channels is 64 bits but, area of hugetlb/DDR will always
	* less than 4GB(B4),for 913x it is only 2GB, so the value is
	* always in 32 bits, that is why bitwise and with 0xFFFFFFFF
	*/
	if ((ipc->ipc_channels & 0xFFFFFFFF) == 0) {
		ret = -ERR_INCORRECT_RAT_MODE;
		goto end;
	}

	ipc_priv->max_channels = ipc->num_ipc_channels;
	ipc_priv->max_depth = ipc->ipc_max_bd_size;
	ipc_priv->ipc_inst = ipc;
end:
	EXIT(ret);
	return ret;
}

int get_channels_info(ipc_userspace_t *ipc_priv, uint32_t inst_id)
{
	return get_ipc_inst(ipc_priv, inst_id);
}
#endif

/*
 * @get_channel_paddr
 *
 * Returns the phyical address of the channel data structure in the
 * share control area.
 *
 * Type: Internal function
 */
static unsigned long __get_channel_paddr(uint32_t channel_id,
		ipc_userspace_t *ipc_priv)
{
	unsigned long		phys_addr;
	ipc_instance_t *ipc = (ipc_instance_t *)ipc_priv->instance;
	ipc_ch_t *ch = &(ipc->ch_list[channel_id]);

	if (!ipc || !(ipc->initialized))
		return IPC_INSTANCE_INVALID;

	if (channel_id >= IPC_MAX_CHANNEL_COUNT)
		return IPC_CH_INVALID;

#if TBD /* AK not complete */
	phys_addr = (unsigned long)ipc->ipc_channels +
		sizeof(os_het_ipc_channel_t)*channel_id;
	EXIT(phys_addr);
	return phys_addr;
#endif
}
/*
 * @get_channel_vaddr
 *
 * Returns the virtual address of the channel data structure in the
 * share control area.
 *
 * Type: Internal function
 */
static void *__get_channel_vaddr(uint32_t channel_id,
		ipc_userspace_t *ipc_priv)
{
#if 0
	void *vaddr;
#endif
	ipc_userspace_t *ipc_priv = (ipc_userspace_t *)instance;
	ipc_instance_t *ipc = ipc_priv->instance;
	ipc_ch_t *ch = &(ipc->ch_list[channel_id]);

	if (!ipc || !(ipc->initialized))
		return IPC_INSTANCE_INVALID;

	if (channel_id >= IPC_MAX_CHANNEL_COUNT)
	return IPC_CH_INVALID;

	ch = &(ipc->ch_list[channel_id]);

	if (!ipc || !(ipc->initialized))
		return IPC_INSTANCE_INVALID;


//	vaddr = IPC_CH_VADDR(get_channel_paddr(channel_id, ipc_priv));

	return ch;
}
/*
 * @get_channel_paddr
 *
 * Returns the phyical address of the channel data structure in the
 * share control area.
 *
 * Type: Internal function
 */
static unsigned long get_channel_paddr(uint32_t channel_id,
		ipc_userspace_t *ipc_priv)
{
	return chvpaddr_arr[ipc_priv->instance_id][channel_id].host_phys;
}

/*
 * @get_channel_vaddr
 *
 * Returns the virtual address of the channel data structure in the
 * share control area.
 *
 * Type: Internal function
 */
static void *get_channel_vaddr(uint32_t channel_id, ipc_userspace_t *ipc_priv)
{
	return chvpaddr_arr[ipc_priv->instance_id][channel_id].vaddr;
}
#endif
