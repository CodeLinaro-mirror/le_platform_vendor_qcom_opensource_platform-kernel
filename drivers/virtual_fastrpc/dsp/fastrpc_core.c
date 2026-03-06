// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2011-2018, The Linux Foundation. All rights reserved.
 * Copyright (c) 2018, Linaro Limited
 * Copyright (c) 2022-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#include <linux/delay.h>
#include <linux/sort.h>

#include "fastrpc_common.h"
#if IS_ENABLED(CONFIG_HYBRID_FASTRPC_RSM)
#include "fastrpc_rsm.h"
#endif
#include "fastrpc_core.h"
#include "fastrpc_mem.h"
#include "fastrpc_vq.h"

#define FASTRPC_INIT_HANDLE		1
#define FASTRPC_DSP_UTILITIES_HANDLE	2
#define FASTRPC_MAX_STATIC_HANDLE	20

#define FASTRPC_RMID_INIT_ATTACH	0
#define FASTRPC_RMID_INIT_RELEASE	1
#define FASTRPC_RMID_INIT_MMAP		4
#define FASTRPC_RMID_INIT_MUNMAP	5
#define FASTRPC_RMID_INIT_CREATE	6
#define FASTRPC_RMID_INIT_CREATE_ATTR	7
#define FASTRPC_RMID_INIT_CREATE_STATIC	8
#define FASTRPC_RMID_INIT_MEM_MAP	10
#define FASTRPC_RMID_INIT_MEM_UNMAP	11
#define FASTRPC_RMID_INIT_MAX		20

#define REMOTE_SCALARS_INBUFS(sc)	(((sc) >> 16) & 0x0ff)

#define REMOTE_SCALARS_OUTBUFS(sc)	(((sc) >> 8) & 0x0ff)

#define REMOTE_SCALARS_INHANDLES(sc)	(((sc) >> 4) & 0x0f)

#define REMOTE_SCALARS_OUTHANDLES(sc)	((sc) & 0x0f)

#define REMOTE_SCALARS_LENGTH(sc)	(REMOTE_SCALARS_INBUFS(sc) +   \
					 REMOTE_SCALARS_OUTBUFS(sc) +  \
					 REMOTE_SCALARS_INHANDLES(sc)+ \
					 REMOTE_SCALARS_OUTHANDLES(sc))
#define FASTRPC_BUILD_SCALARS(attr, method, in, out, oin, oout)  \
				(((attr & 0x07) << 29) |                \
				((method & 0x1f) << 24) |       \
				((in & 0xff) << 16) |           \
				((out & 0xff) <<  8) |          \
				((oin & 0x0f) <<  4) |          \
				(oout & 0x0f))

#define FASTRPC_SCALARS(method, in, out) \
		FASTRPC_BUILD_SCALARS(0, method, in, out, 0, 0)

#define MAX_FRPC_TGID			64
#define FASTRPC_UNIQUE_ID_CONST		1000

#define FIND_DIGITS(number) ({ \
		unsigned int count = 0, i= number; \
		while(i != 0) { \
			i /= 10; \
			count++; \
		} \
	count; \
	})
#define COUNT_OF(number) (number == 0 ? 1 : FIND_DIGITS(number))

#define miscdev_to_fdevice(d) container_of(d, struct fastrpc_device_node, miscdev)

#define PERF_END ((void)0)

#define PERF(enb, cnt, ff) \
	{\
		struct timespec64 startT = {0};\
		uint64_t *counter = cnt;\
		if (enb && counter) {\
			ktime_get_real_ts64(&startT);\
		} \
		ff ;\
		if (enb && counter) {\
			*counter += getnstimediff(&startT);\
		} \
	}

#define GET_COUNTER(perf_ptr, offset)  \
	(perf_ptr != NULL ?\
		(((offset >= 0) && (offset < PERF_KEY_MAX)) ?\
			(uint64_t *)(perf_ptr + offset)\
				: (uint64_t *)NULL) : (uint64_t *)NULL)

#define FASTRPC_ALIGN			128
#define FASTRPC_MAX_FDLIST		16
#define FASTRPC_MAX_CRCLIST		64
#define FASTRPC_KERNEL_PERF_LIST	(PERF_KEY_MAX)
#define FASTRPC_DSP_PERF_LIST		12

#define FASTRPC_RSP_VERSION2		2
/* Early wake up poll completion number received from remoteproc */
#define FASTRPC_EARLY_WAKEUP_POLL	0xabbccdde
/* Poll response number from remote processor for call completion */
#define FASTRPC_POLL_RESPONSE		0xdecaf
/* timeout in us for polling until memory barrier */
#define FASTRPC_POLL_TIME_MEM_UPDATE	500
/* timeout in us for busy polling after early response from remoteproc */
#define FASTRPC_POLL_TIME		4000

/* timeout in us for polling completion signal after user early hint */
#define FASTRPC_USER_EARLY_HINT_TIMEOUT	500
/* CPU feature information to DSP */
#define FASTRPC_CPUINFO_DEFAULT		0
#define FASTRPC_CPUINFO_EARLY_WAKEUP	1

/*
 * Fastrpc context ID bit-map:
 *
 * bits 0-3   : type of remote PD
 * bit  4     : type of job (sync/async)
 * bit  5     : reserved
 * bits 6-15  : IDR id
 * bits 16-63 : job id counter
 */
/* Starting position of idr in context id */
#define FASTRPC_CTXID_IDR_POS		6

/* Number of idr bits in context id */
#define FASTRPC_CTXID_IDR_BITS		10

/* Max idr value */
#define FASTRPC_CTX_MAX (1 << FASTRPC_CTXID_IDR_BITS)

/* Bit-mask for idr */
#define FASTRPC_CTXID_IDR_MASK (FASTRPC_CTX_MAX - 1)

/* Macro to pack idr into context id  */
#define FASTRPC_PACK_IDR_IN_CTXID(ctxid, idr) (ctxid | ((idr & \
	FASTRPC_CTXID_IDR_MASK) << FASTRPC_CTXID_IDR_POS))

/* Macro to extract idr from context id */
#define FASTRPC_GET_IDR_FROM_CTXID(ctxid) ((ctxid >> FASTRPC_CTXID_IDR_POS) & \
	FASTRPC_CTXID_IDR_MASK)

/* Number of pd bits in context id (starting pos 0) */
#define FASTRPC_CTXID_PD_BITS		4

/* Bit-mask for pd type */
#define FASTRPC_CTXID_PD_MASK ((1 << FASTRPC_CTXID_PD_BITS) - 1)

/* Macro to pack pd type into context id  */
#define FASTRPC_PACK_PD_IN_CTXID(ctxid, pd) (ctxid | (pd & \
	FASTRPC_CTXID_PD_MASK))

/* Starting position of job id counter in context id */
#define FASTRPC_CTXID_JOBID_POS		16

/* Macro to pack job id counter into context id  */
#define FASTRPC_PACK_JOBID_IN_CTXID(ctxid, jobid) (ctxid | \
	(jobid << FASTRPC_CTXID_JOBID_POS))

/* Macro to extract ctxid (mask pd type) from response context */
#define FASTRPC_GET_CTXID_FROM_RSP_CTX(rsp_ctx) (rsp_ctx & \
	~FASTRPC_CTXID_PD_MASK)

#define FASTRPC_NOTIF_CTX_RESERVED 0xABCDABCD

#define FASTRPC_MAX_PERSISTENT_HEADERS	8

enum fastrpc_internal_attributes {
	DMA_HANDLE_REVERSE_RPC_CAP = 129,
	ROOTPD_RPC_HEAP_SUPPORT = 132,
};

enum fastrpc_msg_type {
	USER_MSG = 0,
	KERNEL_MSG_WITH_ZERO_PID,
	KERNEL_MSG_WITH_NONZERO_PID,
};

struct fastrpc_internal_sessinfo {
	uint32_t domain_id;
	uint32_t session_id;
	uint32_t pd;
	uint32_t sharedcb;
};

struct fastrpc_internal_notif_rsp {
	u32 domain;
	u32 session;
	u32 status;
};

struct fastrpc_notif_rsp {
	struct list_head notifn;
	u32 domain;
	u32 session;
	enum fastrpc_status_flags status;
};

struct dsp_notif_rsp {
	u64 ctx;
	u32 type;
	int pid;
	u32 status;
};

struct fastrpc_ctrl_latency {
	u32 enable;	/* latency control enable */
	u32 latency;	/* latency request in us */
};

struct fastrpc_ctrl_smmu {
	u32 sharedcb;	/* Set to SMMU share context bank */
};

struct fastrpc_ctrl_wakelock {
	u32 enable;	/* wakelock control enable */
};

struct fastrpc_ctrl_pm {
	u32 timeout;	/* timeout(in ms) for PM to keep system awake */
};

struct fastrpc_internal_control {
	u32 req;
	union {
		struct fastrpc_ctrl_latency lp;
		struct fastrpc_ctrl_smmu smmu;
		struct fastrpc_ctrl_wakelock wp;
		struct fastrpc_ctrl_pm pm;
	};
};

struct fastrpc_mmap_req_msg {
	s32 pgid;
	u32 flags;
	u64 vaddr;
	s32 num;
};
struct fastrpc_mmap_rsp_msg {
	u64 vaddr;
};

struct fastrpc_munmap_req_msg {
	s32 pgid;
	u64 vaddr;
	u64 size;
};

struct fastrpc_mem_map_req_msg {
	s32 pgid;
	s32 fd;
	s32 offset;
	u32 flags;
	u64 vaddrin;
	s32 num;
	s32 data_len;
};

struct fastrpc_mem_unmap_req_msg {
	s32 pgid;
	s32 fd;
	u64 vaddrin;
	u64 len;
};

struct virt_open_msg {
	struct virt_msg_hdr hdr;	/* virtio fastrpc message header */
	u32 domain;			/* DSP domain id */
	u32 pd;				/* DSP PD */
	u32 attrs;			/* DSP PD attributes */
	u32 upid;			/* unique pid sent to DSP */
} __packed;

struct virt_mdctx_manage_msg {
	/* [in]: virtio fastrpc message header */
	struct virt_msg_hdr hdr;
	/* [in]: FASTRPC_MDCTX_SETUP/FASTRPC_MDCTX_REMOVE */
	u32 req;
	/* [in/out]: context id */
	u64 ctx;
	/* [in]: number of domain id */
	u32 num_domains;
	/* [in]: array of cid returned by virt_fastrpc_open */
	s32 cid[];
} __packed;

static int fastrpc_mem_map_to_dsp(struct fastrpc_user *fl, int fd, int offset,
					u32 flags, u32 va, u64 da,
					size_t size, uintptr_t *raddr);

static int fastrpc_multidomain_ctx_cleanup(struct fastrpc_user *fl,
	uint32_t req, uint64_t ctx);
/*
 * Checks if a given logical domain id is valid.
 *
 * @param domain_id Logical domain ID to check.
 *
 * @return true if the domain ID is valid, false otherwise.
 */
static bool fastrpc_is_valid_logical_domain_id(u32 domain_id)
{
	struct fastrpc_domain *domain = fastrpc_lookup_domain_in_table(domain_id,
		false);
	return domain ? true : false;
}

static inline int64_t getnstimediff(struct timespec64 *start)
{
	int64_t ns;
	struct timespec64 ts, b;

	ktime_get_real_ts64(&ts);
	b = timespec64_sub(ts, *start);
	ns = timespec64_to_ns(&b);

	return ns;
}

/*
 * Retrieves the fastrpc channel context for a given Logical domain ID.
 *
 * @param domain_id Logical domain id of channel context
 *
 * @return A pointer to the fastrpc channel context for the
 *         specified domain or NULL if the domain is not found.
 */
static inline struct fastrpc_channel_ctx
	*fastrpc_get_domain_channel_ctx(int domain_id)
{
	struct fastrpc_domain *domain = fastrpc_lookup_domain_in_table(domain_id,
		false);
	return domain ? domain->cctx : NULL;
}

static void fastrpc_channel_ctx_free(struct kref *ref)
{
	struct fastrpc_channel_ctx *cctx;

	cctx = container_of(ref, struct fastrpc_channel_ctx, refcount);

	ida_destroy(&cctx->tgid_frpc_ida);
	fastrpc_update_gdriver(cctx, 0);
	kfree(cctx);
}

void fastrpc_channel_ctx_get(struct fastrpc_channel_ctx *cctx)
{
	kref_get(&cctx->refcount);
}

void fastrpc_channel_ctx_put(struct fastrpc_channel_ctx *cctx)
{
	kref_put(&cctx->refcount, fastrpc_channel_ctx_free);
}

void fastrpc_channel_update_invoke_cnt(
		struct fastrpc_channel_ctx *cctx, bool incr)
{
	if (incr) {
		atomic_inc(&cctx->invoke_cnt);
	} else {
		atomic_dec(&cctx->invoke_cnt);
	}
}

static void fastrpc_context_free(struct kref *ref)
{
	struct fastrpc_invoke_ctx *ctx;
	struct fastrpc_channel_ctx *cctx;
	unsigned long flags;
	int i;

	ctx = container_of(ref, struct fastrpc_invoke_ctx, refcount);
	cctx = ctx->cctx;

	mutex_lock(&ctx->fl->map_mutex);
	for (i = 0; i < ctx->nbufs; i++)
		fastrpc_map_put(ctx->maps[i]);
	mutex_unlock(&ctx->fl->map_mutex);

	if (ctx->buf)
		fastrpc_buf_free(ctx->buf, true);

	if (ctx->fl->profile)
		kfree(ctx->perf);

	spin_lock_irqsave(&cctx->lock, flags);
	idr_remove(&cctx->ctx_idr, FASTRPC_GET_IDR_FROM_CTXID(ctx->ctxid));
	spin_unlock_irqrestore(&cctx->lock, flags);

	kfree(ctx->maps);
	kfree(ctx->olaps);
	kfree(ctx->args);
	kfree(ctx);

	fastrpc_channel_ctx_put(cctx);
}

static void fastrpc_context_put(struct fastrpc_invoke_ctx *ctx)
{
	kref_put(&ctx->refcount, fastrpc_context_free);
}

static void fastrpc_context_list_free(struct fastrpc_user *fl)
{
	struct fastrpc_invoke_ctx *ctx, *n;

	list_for_each_entry_safe(ctx, n, &fl->interrupted, node) {
		spin_lock(&fl->lock);
		list_del(&ctx->node);
		spin_unlock(&fl->lock);
		fastrpc_context_put(ctx);
	}

	list_for_each_entry_safe(ctx, n, &fl->pending, node) {
		spin_lock(&fl->lock);
		list_del(&ctx->node);
		spin_unlock(&fl->lock);
		fastrpc_context_put(ctx);
	}
}

static void fastrpc_buf_list_free(struct fastrpc_user *fl,
	struct list_head *buf_list, bool is_cached_buf)
{
	struct fastrpc_buf *buf = NULL, *n = NULL, *free = NULL;

	do {
		free = NULL;
		spin_lock(&fl->lock);
		list_for_each_entry_safe(buf, n, buf_list, node) {
			list_del(&buf->node);
			if (is_cached_buf)
				fl->num_cached_buf--;
			free = buf;
			break;
		}
		spin_unlock(&fl->lock);
		if (free)
			fastrpc_buf_free(free, false);
	} while (free);
}

void fastrpc_free_user(struct fastrpc_user *fl)
{
	struct fastrpc_map *map = NULL, *m = NULL;
	struct fastrpc_mdctx_info *mdctx = NULL, *n = NULL;

	fastrpc_context_list_free(fl);

	mutex_lock(&fl->remote_map_mutex);
	mutex_lock(&fl->map_mutex);
	// During process tear down free the map, even if refcount is non-zero
	list_for_each_entry_safe(map, m, &fl->maps, node)
		fastrpc_free_map(map);
	mutex_unlock(&fl->map_mutex);
	mutex_unlock(&fl->remote_map_mutex);

	fastrpc_buf_list_free(fl, &fl->mmaps, false);

	fastrpc_buf_list_free(fl, &fl->cached_bufs, true);

	/* Iterate thru all multidomain contexts of user and destroy each one */
	list_for_each_entry_safe(mdctx, n, &fl->mdctxs, node) {
		RPC_WARN("Unexpected mdctx 0x%llx exist!\n", mdctx->ctx);
		(void)fastrpc_multidomain_ctx_cleanup(fl,
			FASTRPC_MDCTX_REMOVE, mdctx->ctx);
	}

        return;
}

static int get_unique_hlos_process_id(struct fastrpc_channel_ctx *cctx)
{
	int tgid_frpc = -1;
	int ret = -1;

	/* allocate unique id between 1 and MAX_FRPC_TGID both inclusive */
	ret = ida_alloc_range(&cctx->tgid_frpc_ida, 1,
			MAX_FRPC_TGID, GFP_ATOMIC);
	if (ret < 0) {
		return -1;
	}
	tgid_frpc = ((cctx->domain_id) * FASTRPC_UNIQUE_ID_CONST) + ret;
	return tgid_frpc;
}

static void fastrpc_handle_signal_rpmsg(uint64_t msg,
					struct fastrpc_channel_ctx *cctx)
{
	u32 pid = msg >> 32;
	u32 signal_id = msg & 0xffffffff;
	struct fastrpc_user *fl ;
	unsigned long irq_flags = 0;
	bool process_found = false;

	DSPSIGNAL_VERBOSE("received queue signal %llx: PID %u, signal %u\n",
			msg, pid, signal_id);

	if (signal_id >=FASTRPC_DSPSIGNAL_NUM_SIGNALS)
		return;

	spin_lock_irqsave(&cctx->lock, irq_flags);
	list_for_each_entry(fl, &cctx->users, user) {
		if (fl->upid == pid && fl->state < DSP_EXIT_START) {
			process_found = true;
			break;
		}
	}
	spin_unlock_irqrestore(&cctx->lock, irq_flags);

	if (!process_found) {
		RPC_WARN("warning: no active processes found for pid %u, signal id %u",
				pid, signal_id);
		return;
	}

	spin_lock_irqsave(&fl->dspsignals_lock, irq_flags);
	if (fl->signal_groups[signal_id / FASTRPC_DSPSIGNAL_GROUP_SIZE]) {
		struct fastrpc_dspsignal *group =
			fl->signal_groups[signal_id / FASTRPC_DSPSIGNAL_GROUP_SIZE];
		struct fastrpc_dspsignal *sig =
			&group[signal_id % FASTRPC_DSPSIGNAL_GROUP_SIZE];
		if ((sig->state == DSPSIGNAL_STATE_PENDING) ||
				(sig->state == DSPSIGNAL_STATE_SIGNALED)) {
			complete(&sig->comp);
			sig->state = DSPSIGNAL_STATE_SIGNALED;
		} else if (sig->state == DSPSIGNAL_STATE_UNUSED) {
			RPC_ERR("received unknown signal %u for PID %u\n",
					signal_id, pid);
		}
	} else {
		RPC_ERR("received unknown signal %u for PID %u\n",
				signal_id, pid);
	}
	spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);

	return;
}

void fastrpc_queue_pd_status(struct fastrpc_user *fl, int domain,
				int status, int sessionid)
{
	struct fastrpc_notif_rsp *notif_rsp = NULL;
	unsigned long flags;

	notif_rsp = kzalloc(sizeof(*notif_rsp), GFP_ATOMIC);
	if (!notif_rsp) {
		RPC_ERR("allocation failed for notif\n");
		return;
	}

	notif_rsp->status = status;
	notif_rsp->domain = domain;
	notif_rsp->session = sessionid;

	spin_lock_irqsave(&fl->proc_state_notif.nqlock, flags);
	list_add_tail(&notif_rsp->notifn, &fl->notif_queue);
	atomic_add(1, &fl->proc_state_notif.notif_queue_count);
	wake_up_interruptible(&fl->proc_state_notif.notif_wait_queue);
	spin_unlock_irqrestore(&fl->proc_state_notif.nqlock, flags);
}

static void fastrpc_notif_find_process(int domain,
		struct fastrpc_channel_ctx *cctx, struct dsp_notif_rsp *notif)
{
	bool is_process_found = false;
	unsigned long irq_flags = 0;
	struct fastrpc_user *user;

	spin_lock_irqsave(&cctx->lock, irq_flags);
	list_for_each_entry(user, &cctx->users, user) {
		if (user->upid == notif->pid) {
			is_process_found = true;
			break;
		}
	}
	spin_unlock_irqrestore(&cctx->lock, irq_flags);

	if (!is_process_found) {
		RPC_DBG("failed to find notif for %d\n", notif->pid);
		return;
	}
	fastrpc_queue_pd_status(user, domain, notif->status, user->sessionid);
}

static void fastrpc_notify_user_ctx(struct fastrpc_invoke_ctx *ctx, int retval,
					u32 rsp_flags, u32 early_wake_time)
{
	ctx->retval = retval;
	ctx->rsp_flags = (enum fastrpc_response_flags)rsp_flags;
	switch (rsp_flags) {
	case NORMAL_RESPONSE:
	case COMPLETE_SIGNAL:
		ctx->is_work_done = true;
		complete(&ctx->work);
		break;
	case USER_EARLY_SIGNAL:
		ctx->early_wake_time = early_wake_time;
		break;
	case EARLY_RESPONSE:
		complete(&ctx->work);
		break;
	default:
		break;
	}
}

int fastrpc_handle_rpc_response(struct fastrpc_channel_ctx *cctx,
				void *data, int len)
{
	struct fastrpc_invoke_rsp *rsp = data;
	struct fastrpc_invoke_rspv2 *rspv2 = NULL;
	struct dsp_notif_rsp *notif = (struct dsp_notif_rsp *)data;
	struct fastrpc_invoke_ctx *ctx;
	unsigned long flags = 0, idr = 0;
	u64 ctxid = 0;
	u32 rsp_flags = 0, early_wake_time = 0, version = 0;

	if (len == sizeof(uint64_t)) {
		fastrpc_handle_signal_rpmsg(*((uint64_t *)data), cctx);
		return 0;
	}

	if (notif->ctx == FASTRPC_NOTIF_CTX_RESERVED) {
		if (notif->type == STATUS_RESPONSE && len >= sizeof(*notif)) {
			fastrpc_notif_find_process(cctx->domain_id, cctx, notif);
			return 0;
		} else {
			return -ENOENT;
		}
	}

	if (len < sizeof(*rsp))
		return -EINVAL;

	if (len >= sizeof(*rspv2)) {
		rspv2 = data;
		if (rspv2) {
			early_wake_time = rspv2->early_wake_time;
			rsp_flags = rspv2->flags;
			version = rspv2->version;
		}
	}

	idr = FASTRPC_GET_IDR_FROM_CTXID(rsp->ctx);
	ctxid = FASTRPC_GET_CTXID_FROM_RSP_CTX(rsp->ctx);

	spin_lock_irqsave(&cctx->lock, flags);
	ctx = idr_find(&cctx->ctx_idr, idr);

	if (!ctx) {
		spin_unlock_irqrestore(&cctx->lock, flags);
		return 0;
	}

	if (ctx->ctxid != ctxid) {
		spin_unlock_irqrestore(&cctx->lock, flags);
		RPC_WARN("rsp ctxid 0x%llx mismatch with local ctxid 0x%llx \
				(full rsp ctx 0x%llx)\n",
				ctxid, ctx->ctxid, rsp->ctx);
		return 0;
	}

	if (rspv2) {
		if (rspv2->version != FASTRPC_RSP_VERSION2) {
			RPC_ERR("incorrect response version %d\n",
					rspv2->version);
			spin_unlock_irqrestore(&cctx->lock, flags);
			return -EINVAL;
		}
	}
	fastrpc_notify_user_ctx(ctx, rsp->retval, rsp_flags, early_wake_time);
	spin_unlock_irqrestore(&cctx->lock, flags);

	return 0;
}

static int virt_fastrpc_get_dsp_info(struct fastrpc_user *fl,
					u32 *dsp_attributes)
{
	struct fastrpc_channel_ctx *cctx = fl->cctx;
	struct fastrpc_common *gdriver = cctx->gdriver;
	struct virt_cap_msg *vmsg, *rsp = NULL;
	struct virt_fastrpc_msg *msg;
	int err;

	msg = virt_alloc_msg(fl, sizeof(*vmsg));
	if (!msg) {
		RPC_ERR("out of memory\n");
		return -ENOMEM;
	}

	vmsg = (struct virt_cap_msg *)msg->txbuf;
	vmsg->hdr.pid = fl->tgid_frpc;
	vmsg->hdr.tid = current->pid;
	vmsg->hdr.cid = -1;
	vmsg->hdr.cmd = VIRTIO_FASTRPC_CMD_GET_DSP_INFO;
	vmsg->hdr.len = sizeof(*vmsg);
	vmsg->hdr.msgid = msg->msgid;
	vmsg->hdr.result = 0xffffffff;
	vmsg->domain = cctx->domain_id;
	memset(vmsg->dsp_caps, 0, FASTRPC_MAX_DSP_ATTRIBUTES * (sizeof(u32)));

	err = fastrpc_txbuf_send(fl, vmsg, sizeof(*vmsg));
	if (err)
		goto bail;
	wait_for_completion(&msg->work);

	rsp = msg->rxbuf;
	if (!rsp)
		goto bail;

	err = rsp->hdr.result;
	if (err)
		goto bail;
	memcpy(dsp_attributes, rsp->dsp_caps, FASTRPC_MAX_DSP_ATTRIBUTES * (sizeof(u32)));
bail:
	if (rsp)
		fastrpc_rxbuf_send(fl, rsp, gdriver->buf_size);
	virt_free_msg(fl, msg);

	return err;
}

static int fastrpc_get_info_from_kernel(struct fastrpc_ioctl_capability *cap,
					struct fastrpc_user *fl)
{
	struct fastrpc_channel_ctx *cctx = fl->cctx;
	uint32_t attribute_id = cap->attribute_id;
	uint32_t *dsp_attributes;
	unsigned long flags;
	int err;

	spin_lock_irqsave(&cctx->lock, flags);
	/* check if we already have queried dsp for attributes */
	if (cctx->valid_attributes) {
		spin_unlock_irqrestore(&cctx->lock, flags);
		goto done;
	}
	spin_unlock_irqrestore(&cctx->lock, flags);

	dsp_attributes = kzalloc(FASTRPC_MAX_DSP_ATTRIBUTES_LEN, GFP_KERNEL);
	if (!dsp_attributes)
		return -ENOMEM;

	err = virt_fastrpc_get_dsp_info(fl, dsp_attributes);
	if (err) {
		RPC_DBG("failed to get dsp information err: %d\n", err);
		kfree(dsp_attributes);
		return err;
	}

	spin_lock_irqsave(&cctx->lock, flags);
	memcpy(cctx->dsp_attributes, dsp_attributes,
			FASTRPC_MAX_DSP_ATTRIBUTES_LEN);
	cctx->valid_attributes = true;
	spin_unlock_irqrestore(&cctx->lock, flags);
	kfree(dsp_attributes);
done:
	cap->capability = cctx->dsp_attributes[attribute_id];

	return 0;
}

int fastrpc_get_dsp_info(struct fastrpc_user *fl, char __user *argp)
{
	struct fastrpc_ioctl_capability cap = {0};
	int err = 0;

	if (copy_from_user(&cap, argp, sizeof(cap)))
		return  -EFAULT;

	cap.capability = 0;

	/* Validate that domain passed is either a logical or legacy domain id */
	if (!IS_LEGACY_DOMAIN_ID(cap.domain) &&
		!fastrpc_is_valid_logical_domain_id(cap.domain)) {
		RPC_ERR("invalid domain id:%d\n", cap.domain);
		return -ECHRNG;
	}

	if (cap.attribute_id >= FASTRPC_MAX_DSP_ATTRIBUTES) {
		RPC_ERR("invalid attribute: %d\n", cap.attribute_id);
		return -EOVERFLOW;
	}

	err = fastrpc_get_info_from_kernel(&cap, fl);
	if (err)
		return err;

	if (copy_to_user(argp, &cap, sizeof(cap)))
		return -EFAULT;

	return 0;
}

static int fastrpc_create_persistent_headers(struct fastrpc_user *fl)
{
	int err = 0;
	int i = 0;
	u64 va_base = 0;
	struct fastrpc_buf *hdr_bufs, *buf, *pers_hdr_buf = NULL;
	u32 num_pers_hdrs = 0;
	size_t hdr_buf_alloc_len = 0;

	num_pers_hdrs = FASTRPC_MAX_PERSISTENT_HEADERS;
	hdr_buf_alloc_len = num_pers_hdrs * PAGE_SIZE;

	err = fastrpc_buf_alloc(fl, hdr_buf_alloc_len,
			METADATA_BUF, &pers_hdr_buf);
	if (err)
		return err;

	va_base = (u64) (uintptr_t)(pers_hdr_buf->va);
	err = fastrpc_mem_map_to_dsp(fl, -1, 0, ADSP_MMAP_PERSIST_HDR, 0,
			(u64)(uintptr_t)(pers_hdr_buf->da),
			pers_hdr_buf->size, &pers_hdr_buf->raddr);
	if (err)
		goto err_dsp_map;

	hdr_bufs = kcalloc(num_pers_hdrs, sizeof(struct fastrpc_buf),
				GFP_KERNEL);
	if (!hdr_bufs)
		return -ENOMEM;

	spin_lock(&fl->lock);
	fl->pers_hdr_buf = pers_hdr_buf;
	fl->num_pers_hdrs = num_pers_hdrs;
	fl->hdr_bufs = hdr_bufs;
	for (i = 0; i < num_pers_hdrs; i++) {
		buf = &fl->hdr_bufs[i];
		buf->fl = fl;
		buf->va = (void *)(va_base + (i * PAGE_SIZE));
		buf->da = pers_hdr_buf->da + (i * PAGE_SIZE);
		buf->size = PAGE_SIZE;
		buf->type = pers_hdr_buf->type;
		buf->in_use = false;
	}
	spin_unlock(&fl->lock);

	RPC_DBG("create %u num persistent headers\n", num_pers_hdrs);

	return 0;
err_dsp_map:
	RPC_ERR("failed to map len %zu, flags %d, num headers %u with err %d\n",
			hdr_buf_alloc_len, ADSP_MMAP_PERSIST_HDR,
			num_pers_hdrs, err);
	fastrpc_buf_free(pers_hdr_buf, 0);

	return err;
}

static int virt_fastrpc_open(struct fastrpc_user *fl, u32 attrs)
{
	struct fastrpc_channel_ctx *cctx = fl->cctx;
	struct fastrpc_common *gdriver = cctx->gdriver;
	struct virt_open_msg *vmsg, *rsp = NULL;
	struct virt_fastrpc_msg *msg;
	int err;

	msg = virt_alloc_msg(fl, sizeof(*vmsg));
	if (!msg) {
		RPC_ERR("out of memory\n");
		return -ENOMEM;
	}

	vmsg = (struct virt_open_msg *)msg->txbuf;
	vmsg->hdr.pid = fl->tgid_frpc;
	vmsg->hdr.tid = current->pid;
	vmsg->hdr.cid = -1;
	vmsg->hdr.cmd = VIRTIO_FASTRPC_CMD_OPEN;
	vmsg->hdr.len = sizeof(*vmsg);
	vmsg->hdr.msgid = msg->msgid;
	vmsg->hdr.result = 0xffffffff;
	vmsg->domain = cctx->domain_id;
	vmsg->pd = fl->pd_type;
	vmsg->attrs = attrs;

	err = fastrpc_txbuf_send(fl, vmsg, sizeof(*vmsg));
	if (err)
		goto bail;
	wait_for_completion(&msg->work);

	rsp = msg->rxbuf;
	if (!rsp)
		goto bail;
	err = rsp->hdr.result;
	if (err)
		goto bail;
	if (rsp->hdr.cid < 0) {
		RPC_ERR("channel id %d is invalid\n", rsp->hdr.cid);
		err = -EINVAL;
		goto bail;
	}
	fl->cid = rsp->hdr.cid;
	fl->upid = rsp->upid;
	RPC_DBG("host pid=%x\n", rsp->upid);
bail:
	if (rsp)
		fastrpc_rxbuf_send(fl, rsp, gdriver->buf_size);
	virt_free_msg(fl, msg);

	return err;
}

static bool is_session_rejected(struct fastrpc_user *fl, bool unsigned_pd_request)
{
	/* Check if the device node is non-secure and channel is secure */
	if (!fl->is_secure_dev && fl->cctx->secure) {
		/*
		 * Allow untrusted applications to offload only to Unsigned PD when
		 * channel is configured as secure and block untrusted apps on channel
		 * that does not support unsigned PD offload
		 */
		if (!fl->cctx->unsigned_support || !unsigned_pd_request)
			goto reject_session;
	}
	/* Check if untrusted process is trying to offload to signed PD */
	if (fl->untrusted_process && !unsigned_pd_request)
		goto reject_session;

	return false;
reject_session:
	RPC_ERR("untrusted application trying to offload to signed PD\n");
	return true;
}

#ifdef CONFIG_DEBUG_FS
void print_buf_info(struct seq_file *s_file, struct fastrpc_buf *buf)
{
	seq_printf(s_file, "\n %s %2s 0x%p", "va", ":", buf->va);
	seq_printf(s_file, "\n %s %2s 0x%lx", "raddr", ":", buf->raddr);
	seq_printf(s_file, "\n %s %2s 0x%x", "type", ":", buf->type);
	seq_printf(s_file, "\n %s %2s 0x%llx", "size", ":", buf->size);
	seq_printf(s_file, "\n %s %s %d", "in_use", ":", buf->in_use);
}

void print_ictx_info(struct seq_file *s_file, struct fastrpc_invoke_ctx *ictx)
{
	seq_printf(s_file, "\n %s %7s %d", "nscalars", ":", ictx->nscalars);
	seq_printf(s_file, "\n %s %10s %d", "nbufs", ":", ictx->nbufs);
	seq_printf(s_file, "\n %s %10s %d", "retval", ":", ictx->retval);
	seq_printf(s_file, "\n %s %12s %px", "crc", ":", ictx->crc);
	seq_printf(s_file, "\n %s %1s %d", "early_wake_time", ":", ictx->early_wake_time);
	seq_printf(s_file, "\n %s %5s %px", "perf_kernel", ":", ictx->perf_kernel);
	seq_printf(s_file, "\n %s %7s %px", "perf_dsp", ":", ictx->perf_dsp);
	seq_printf(s_file, "\n %s %12s %d", "pid", ":", ictx->pid);
	seq_printf(s_file, "\n %s %11s %d", "tgid", ":", ictx->tgid);
	seq_printf(s_file, "\n %s %13s 0x%x", "sc", ":", ictx->sc);
	seq_printf(s_file, "\n %s %10s %llu", "ctxid", ":", ictx->ctxid);
	seq_printf(s_file, "\n %s %3s %d", "is_work_done", ":", ictx->is_work_done);
	seq_printf(s_file, "\n %s %9s %llu", "msg_sz", ":", ictx->msg_sz);
	seq_printf(s_file, "\n %s %9s 0x%x", "handle", ":", ictx->handle);
}

void print_ctx_info(struct seq_file *s_file, struct fastrpc_channel_ctx *ctx)
{
	seq_printf(s_file, "%s %8s %d\n", "domain_id", ":", ctx->domain_id);
	seq_printf(s_file, "%s %s %d\n", "valid_attributes", ":", ctx->valid_attributes);
	seq_printf(s_file, "%s %11s %d\n", "secure", ":", ctx->secure);
	seq_printf(s_file, "%s %s %d\n", "unsigned_support", ":", ctx->unsigned_support);
}

void print_map_info(struct seq_file *s_file, struct fastrpc_map *map)
{
	seq_printf(s_file, "%s %4s %d\n", "fd", ":", map->fd);
	seq_printf(s_file, "%s %s 0x%llx\n", "da", ":", map->da);
	seq_printf(s_file, "%s %s 0x%llx\n", "size", ":", map->size);
	seq_printf(s_file, "%s %4s 0x%p\n", "va", ":", map->va);
	seq_printf(s_file, "%s %3s 0x%llx\n", "len", ":", map->len);
	seq_printf(s_file, "%s %2s 0x%llx\n", "raddr", ":", map->raddr);
	seq_printf(s_file, "%s %2s 0x%x\n", "attr", ":", map->attr);
	seq_printf(s_file, "%s %2s 0x%x\n", "flags", ":", map->flags);
}

static int fastrpc_debugfs_show(struct seq_file *s_file, void *data)
{
	struct fastrpc_user *fl = s_file->private;
	struct fastrpc_map *map;
	struct fastrpc_channel_ctx *ctx;
	struct fastrpc_invoke_ctx *ictx, *m;
	struct fastrpc_buf *buf, *n;
	int i;
	unsigned long irq_flags = 0;

	if (fl != NULL) {
		seq_printf(s_file, "%s %12s %d\n", "tgid", ":", fl->tgid);
		seq_printf(s_file, "%s %7s %d\n", "tgid_frpc", ":", fl->tgid_frpc);
		seq_printf(s_file, "%s %3s %d\n", "is_secure_dev", ":", fl->is_secure_dev);
		seq_printf(s_file, "%s %2s %d\n",  "is_unsigned_pd", ":", fl->is_unsigned_pd);
		seq_printf(s_file, "%s %7s %d\n",  "sessionid", ":", fl->sessionid);
		seq_printf(s_file, "%s %9s %d\n", "pd_type", ":", fl->pd_type);
		seq_printf(s_file, "%s %9s %d\n",  "profile", ":", fl->profile);

		if(fl->cctx) {
			seq_printf(s_file, "\n=============== Channel Context ===============\n");
			ctx = fl->cctx;
			print_ctx_info(s_file, ctx);
		}
		seq_printf(s_file, "\n=============== DSP Signal Status ===============\n");
		spin_lock_irqsave(&fl->dspsignals_lock, irq_flags);
		for (i = 0; i < FASTRPC_DSPSIGNAL_NUM_SIGNALS / FASTRPC_DSPSIGNAL_GROUP_SIZE; i++) {
			if (fl->signal_groups[i] != NULL)
				seq_printf(s_file, "%d : %d ", i, fl->signal_groups[i]->state);
		}
		spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);

		seq_printf(s_file, "\n=============== User space maps ===============\n");
		spin_lock(&fl->lock);
		list_for_each_entry(map, &fl->maps, node) {
			if (map)
				print_map_info(s_file, map);
		}
		seq_printf(s_file, "\n=============== Kernel maps ===============\n");
		list_for_each_entry(map, &fl->mmaps, node) {
			if (map)
				print_map_info(s_file, map);
		}
		seq_printf(s_file, "\n=============== Cached Bufs ===============\n");
		list_for_each_entry_safe(buf, n, &fl->cached_bufs, node) {
			if(buf)
				print_buf_info(s_file, buf);
		}
		seq_printf(s_file, "\n=============== Pending contexts ===============\n");
		list_for_each_entry_safe(ictx, m, &fl->pending, node) {
			if (ictx)
				print_ictx_info(s_file, ictx);
		}
		seq_printf(s_file, "\n=============== Interrupted contexts ===============\n");
		list_for_each_entry_safe(ictx, m, &fl->interrupted, node) {
			if (ictx)
				print_ictx_info(s_file, ictx);
		}
		spin_unlock(&fl->lock);
	}
	return 0;
}

DEFINE_SHOW_ATTRIBUTE(fastrpc_debugfs);

static int fastrpc_create_session_debugfs(struct fastrpc_user *fl)
{
	struct fastrpc_channel_ctx *cctx = fl->cctx;
	struct fastrpc_common *gdriver = cctx->gdriver;
        char cur_comm[TASK_COMM_LEN];
        int domain_id = -1, size = 0;
        struct dentry *debugfs_root = gdriver->debugfs_root;

        memcpy(cur_comm, current->comm, TASK_COMM_LEN);
        cur_comm[TASK_COMM_LEN-1] = '\0';
        if (debugfs_root != NULL) {
                domain_id = fl->cctx->domain_id;
                if (!(fl->debugfs_file_create)) {
                        size = strlen(cur_comm) + strlen("_")
                                + COUNT_OF(current->pid) + strlen("_")
                                + COUNT_OF(domain_id)
                                + 1;

                        fl->debugfs_buf = kzalloc(size, GFP_KERNEL);
                        if (fl->debugfs_buf == NULL) {
                                return -ENOMEM;
                        }
                        /*
                         * Use HLOS process name, HLOS PID, unique fastrpc PID
                         * domain_id in debugfs filename to create unique file name
                         */
                        snprintf(fl->debugfs_buf, size, "%.10s%s%d%s%d%s%d",
                                cur_comm, "_", current->pid, "_",
                                fl->tgid_frpc, "_", domain_id);
                        fl->debugfs_file = debugfs_create_file(fl->debugfs_buf, 0644,
                                        debugfs_root, fl, &fastrpc_debugfs_fops);
                        if (IS_ERR_OR_NULL(fl->debugfs_file)) {
                                pr_warn("Error: %s: %s: failed to create debugfs file %s\n",
                                                cur_comm, __func__, fl->debugfs_buf);
                                fl->debugfs_file = NULL;
}
                        kfree(fl->debugfs_buf);
                        fl->debugfs_file_create = true;
                }
        }
return 0;
}
#endif

int fastrpc_init_create_process(struct fastrpc_user *fl, char __user *argp)
{
	struct fastrpc_init_create init;
	int err = 0;

	if (copy_from_user(&init, argp, sizeof(init)))
		return -EFAULT;

	if (current->tgid != fl->tgid)
		fl->untrusted_process = true;

	if (init.attrs & FASTRPC_MODE_UNSIGNED_MODULE)
		fl->is_unsigned_pd = true;

	if (is_session_rejected(fl, fl->is_unsigned_pd))
		return -EACCES;
#if 0
	if (!fl->untrusted_process && fl->is_unsigned_pd)
		init.attrs |= FASTRPC_MODE_SYSTEM_UNSIGNED_PD;
#endif
	fl->pd_type = DYNAMIC_PD;

	err = virt_fastrpc_open(fl, init.attrs);
	if (err)
		return err;

	if (fl->cctx->domain->type == FASTRPC_NSP ||
			fl->cctx->domain->type == FASTRPC_HPASS)
		fastrpc_create_persistent_headers(fl);
#ifdef CONFIG_DEBUG_FS
	fastrpc_create_session_debugfs(fl);
#endif

	return 0;
}

#define CMP(aa, bb) ((aa) == (bb) ? 0 : (aa) < (bb) ? -1 : 1)

static int olaps_cmp(const void *a, const void *b)
{
	struct fastrpc_buf_overlap *pa = (struct fastrpc_buf_overlap *)a;
	struct fastrpc_buf_overlap *pb = (struct fastrpc_buf_overlap *)b;
	/* sort with lowest starting buffer first */
	int st = CMP(pa->start, pb->start);
	/* sort with highest ending buffer first */
	int ed = CMP(pb->end, pa->end);

	return st == 0 ? ed : st;
}

static void fastrpc_get_buff_overlaps(struct fastrpc_invoke_ctx *ctx)
{
	u64 max_end = 0;
	int i;

	for (i = 0; i < ctx->nbufs; ++i) {
		ctx->olaps[i].start = ctx->args[i].ptr;
		ctx->olaps[i].end = ctx->olaps[i].start + ctx->args[i].length;
		ctx->olaps[i].raix = i;
	}

	sort(ctx->olaps, ctx->nbufs, sizeof(*ctx->olaps), olaps_cmp, NULL);

	for (i = 0; i < ctx->nbufs; ++i) {
		if (ctx->olaps[i].start < max_end) {
			ctx->olaps[i].mstart = max_end;
			ctx->olaps[i].mend = ctx->olaps[i].end;
			ctx->olaps[i].offset = max_end - ctx->olaps[i].start;

			if (ctx->olaps[i].end > max_end) {
				max_end = ctx->olaps[i].end;
			} else {
				ctx->olaps[i].mend = 0;
				ctx->olaps[i].mstart = 0;
			}

		} else  {
			ctx->olaps[i].mend = ctx->olaps[i].end;
			ctx->olaps[i].mstart = ctx->olaps[i].start;
			ctx->olaps[i].offset = 0;
			max_end = ctx->olaps[i].end;
		}
	}
}

static struct fastrpc_invoke_ctx *fastrpc_context_alloc(struct fastrpc_user *fl,
		u32 kernel, u32 sc, struct fastrpc_enhanced_invoke *invoke)
{
	struct fastrpc_channel_ctx *cctx = fl->cctx;
	struct fastrpc_invoke_ctx *ctx = NULL;
	unsigned long flags;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&ctx->node);
	ctx->fl = fl;
	ctx->nscalars = REMOTE_SCALARS_LENGTH(sc);
	ctx->nbufs = REMOTE_SCALARS_INBUFS(sc) +
			REMOTE_SCALARS_OUTBUFS(sc);

	if (ctx->nscalars) {
		ctx->maps = kcalloc(ctx->nscalars,
				sizeof(*ctx->maps), GFP_KERNEL);
		if (!ctx->maps) {
			ret = -ENOMEM;
			goto err_alloc;
		}
		ctx->olaps = kcalloc(ctx->nscalars,
				sizeof(*ctx->olaps), GFP_KERNEL);
		if (!ctx->olaps) {
			ret = -ENOMEM;
			goto err_alloc;
		}
		ctx->args = kcalloc(ctx->nscalars,
				sizeof(*ctx->args), GFP_KERNEL);
		if (!ctx->args) {
			ret = -ENOMEM;
			goto err_alloc;
		}
		if (!kernel) {
			if (copy_from_user((void *)ctx->args,
				(void __user *)(uintptr_t)invoke->inv.args,
				ctx->nscalars * sizeof(*ctx->args))) {
				ret = -EFAULT;
				goto err_alloc;
			}
		} else {
			memcpy((void *)ctx->args,
					(void *)(uintptr_t)invoke->inv.args,
					ctx->nscalars * sizeof(*ctx->args));
		}
		invoke->inv.args = (__u64)ctx->args;
		fastrpc_get_buff_overlaps(ctx);
	}

	fastrpc_channel_ctx_get(cctx);

	ctx->crc = (u32 *)(uintptr_t)invoke->crc;
	ctx->perf_dsp = (u64 *)(uintptr_t)invoke->perf_dsp;
	ctx->perf_kernel = (u64 *)(uintptr_t)invoke->perf_kernel;
	if (ctx->fl->profile) {
		ctx->perf = kzalloc(sizeof(*(ctx->perf)), GFP_KERNEL);
		if (!ctx->perf) {
			ret = -ENOMEM;
			goto err_perf_alloc;
		}
		ctx->perf->tid = ctx->fl->tgid;
	}
	ctx->handle = invoke->inv.handle;
	ctx->sc = sc;
	ctx->retval = -1;
	ctx->pid = current->pid;
	ctx->tgid = fl->tgid;
	ctx->cctx = cctx;
	ctx->rsp_flags = NORMAL_RESPONSE;
	ctx->is_work_done = false;
	init_completion(&ctx->work);

	spin_lock(&fl->lock);
	list_add_tail(&ctx->node, &fl->pending);
	spin_unlock(&fl->lock);

	spin_lock_irqsave(&cctx->lock, flags);
	ret = idr_alloc_cyclic(&cctx->ctx_idr, ctx, 1,
				FASTRPC_CTX_MAX, GFP_ATOMIC);
	if (ret < 0) {
		spin_unlock_irqrestore(&cctx->lock, flags);
		goto err_idr;
	}
	cctx->jobid++;
	ctx->ctxid = FASTRPC_PACK_JOBID_IN_CTXID(ctx->ctxid, cctx->jobid);
	ctx->ctxid = FASTRPC_PACK_IDR_IN_CTXID(ctx->ctxid, ret);
	spin_unlock_irqrestore(&cctx->lock, flags);

 	kref_init(&ctx->refcount);

	return ctx;
err_idr:
	spin_lock(&fl->lock);
	list_del(&ctx->node);
	spin_unlock(&fl->lock);
err_perf_alloc:
	fastrpc_channel_ctx_put(cctx);
err_alloc:
	kfree(ctx->maps);
	kfree(ctx->olaps);
	kfree(ctx->args);
	kfree(ctx);

	return ERR_PTR(ret);
}

static struct fastrpc_invoke_ctx *fastrpc_context_restore_interrupted(struct fastrpc_user *fl,
		struct fastrpc_invoke *inv)
{
	struct fastrpc_invoke_ctx *ctx = NULL, *ictx = NULL, *n;

	spin_lock(&fl->lock);
	list_for_each_entry_safe(ictx, n, &fl->interrupted, node) {
		if (ictx->pid == current->pid) {
			if (inv->sc != ictx->sc || ictx->fl != fl) {
				RPC_ERR("interrupted sc (0x%x) or fl (%pK) \
					does not match with invoke sc (0x%x) or fl (%pK)\n",
					ictx->sc, ictx->fl, inv->sc, fl);
				spin_unlock(&fl->lock);
				return ERR_PTR(-EINVAL);
			} else {
 				ctx = ictx;
				list_del(&ctx->node);
				list_add_tail(&ctx->node, &fl->pending);
			}
			break;
		}
	}
	spin_unlock(&fl->lock);

	return ctx;
}

static void fastrpc_context_save_interrupted(struct fastrpc_invoke_ctx *ctx)
{
	spin_lock(&ctx->fl->lock);
	list_del(&ctx->node);
	list_add_tail(&ctx->node, &ctx->fl->interrupted);
	spin_unlock(&ctx->fl->lock);
}

static int fastrpc_get_meta_size(struct fastrpc_invoke_ctx *ctx)
{
	int size = 0;

	size = (sizeof(struct fastrpc_remote_buf) +
		sizeof(struct fastrpc_invoke_buf) +
		sizeof(struct fastrpc_phy_page)) * ctx->nscalars +
		sizeof(u64) * FASTRPC_MAX_FDLIST +
		sizeof(u32) * FASTRPC_MAX_CRCLIST +
		sizeof(u32) + sizeof(u64) * FASTRPC_DSP_PERF_LIST;

	return size;
}

static u64 fastrpc_get_payload_size(struct fastrpc_invoke_ctx *ctx, int metalen)
{
	u64 size = 0;
	int oix;

	size = ALIGN(metalen, FASTRPC_ALIGN);
	for (oix = 0; oix < ctx->nbufs; oix++) {
		int i = ctx->olaps[oix].raix;

		if (ctx->args[i].fd == 0 || ctx->args[i].fd == -1) {
			if (ctx->olaps[oix].offset == 0)
				size = ALIGN(size, FASTRPC_ALIGN);

			size += (ctx->olaps[oix].mend - ctx->olaps[oix].mstart);
		}
	}

	return size;
}

static int fastrpc_create_maps(struct fastrpc_invoke_ctx *ctx)
{
	struct fastrpc_channel_ctx *cctx = ctx->fl->cctx;
	int i, err;

	for (i = 0; i < ctx->nscalars; ++i) {
		bool take_ref = true;

		if (ctx->args[i].fd == 0 || ctx->args[i].fd == -1 ||
			(i >= ctx->nbufs && cctx->dsp_attributes[DMA_HANDLE_REVERSE_RPC_CAP]) ||
			ctx->args[i].length == 0)
			continue;

		if (i >= ctx->nbufs)
			take_ref = false;
		mutex_lock(&ctx->fl->map_mutex);
		err = fastrpc_map_create(ctx->fl, ctx->args[i].fd,
				(u64)ctx->args[i].ptr, ctx->args[i].length,
				ctx->args[i].attr, 0,
				&ctx->maps[i], take_ref);
		mutex_unlock(&ctx->fl->map_mutex);
		if (err) {
			RPC_ERR("failed to create map %d\n", err);
			return -EINVAL;
		}
	}

	return 0;
}

static struct fastrpc_invoke_buf *fastrpc_invoke_buf_start(union fastrpc_remote_arg *pra, int len)
{
	return (struct fastrpc_invoke_buf *)(&pra[len]);
}

static struct fastrpc_phy_page *fastrpc_phy_page_start(struct fastrpc_invoke_buf *buf, int len)
{
	return (struct fastrpc_phy_page *)(&buf[len]);
}

static int fastrpc_get_args(u32 kernel, struct fastrpc_invoke_ctx *ctx)
{
	union fastrpc_remote_arg *rpra;
	struct fastrpc_invoke_buf *list;
	struct fastrpc_phy_page *pages;
	int inbufs, i, oix, err = 0;
	u64 len, rlen, pkt_size;
	u64 pg_start, pg_end;
	u64 *perf_counter = NULL;
	uintptr_t args;
	int metalen;

	if (ctx->fl->profile)
		perf_counter = (u64 *)ctx->perf + PERF_COUNT;

	inbufs = REMOTE_SCALARS_INBUFS(ctx->sc);
	metalen = fastrpc_get_meta_size(ctx);
	pkt_size = fastrpc_get_payload_size(ctx, metalen);

	PERF(ctx->fl->profile, GET_COUNTER(perf_counter, PERF_MAP),
	err = fastrpc_create_maps(ctx);
	if (err)
		return err;
	PERF_END);

	ctx->msg_sz = metalen;

	err = fastrpc_buf_alloc(ctx->fl, pkt_size, METADATA_BUF, &ctx->buf);
	if (err)
		return err;

	memset(ctx->buf->va, 0, pkt_size);
	rpra = ctx->buf->va;
	list = fastrpc_invoke_buf_start(rpra, ctx->nscalars);
	pages = fastrpc_phy_page_start(list, ctx->nscalars);
	args = (uintptr_t)ctx->buf->va + metalen;
	rlen = pkt_size - metalen;
	ctx->rpra = rpra;

	for (oix = 0; oix < ctx->nbufs; ++oix) {
		u64 mlen;
		u64 offset = 0;

		i = ctx->olaps[oix].raix;
		len = ctx->args[i].length;

		rpra[i].buf.pv = 0;
		rpra[i].buf.len = len;
		list[i].num = len ? 1 : 0;
		list[i].pgidx = i;

		if (!len)
			continue;

		if (ctx->maps[i]) {
			struct vm_area_struct *vma = NULL;
			u64 addr = (u64)ctx->args[i].ptr & PAGE_MASK,
			    vm_start = 0, vm_end = 0;

			PERF(ctx->fl->profile, GET_COUNTER(perf_counter, PERF_MAP),

			rpra[i].buf.pv = (u64) ctx->args[i].ptr;
			pages[i].addr = ctx->maps[i]->da;

			if (!(ctx->maps[i]->attr & FASTRPC_ATTR_NOVA)) {
				mmap_read_lock(current->mm);
				vma = find_vma(current->mm, ctx->args[i].ptr);
				if (vma) {
					vm_start = vma->vm_start;
					vm_end = vma->vm_end;
				}
				mmap_read_unlock(current->mm);
				if (addr < vm_start || addr + len > vm_end ||
					(addr - vm_start) + len > ctx->maps[i]->size) {
					err = -EFAULT;
					RPC_ERR("invalid buffer addr 0x%llx len 0x%llx \
						vm start 0x%llx vm end 0x%llx \
						da 0x%llx size 0x%llx",
						ctx->args[i].ptr, len, vm_start, vm_end,
						ctx->maps[i]->da, ctx->maps[i]->size);
					goto bail;
				} else {
					offset = addr - vm_start;
				}
				pages[i].addr += offset;
			}

			pg_start = addr >> PAGE_SHIFT;
			pg_end = ((ctx->args[i].ptr + len - 1) & PAGE_MASK) >>
					PAGE_SHIFT;
			pages[i].size = (pg_end - pg_start + 1) * PAGE_SIZE;
			PERF_END);
		} else {
			PERF(ctx->fl->profile, GET_COUNTER(perf_counter, PERF_COPY),
			if (ctx->olaps[oix].offset == 0) {
				rlen -= ALIGN(args, FASTRPC_ALIGN) - args;
				args = ALIGN(args, FASTRPC_ALIGN);
			}

			mlen = ctx->olaps[oix].mend - ctx->olaps[oix].mstart;

			if (mlen > LONG_MAX) {
				RPC_ERR("invalid payload size 0x%llx\n", mlen);
				return -EFAULT;
			}

			if (rlen < mlen)
				goto bail;

			rpra[i].buf.pv = args - ctx->olaps[oix].offset;
			pages[i].addr = ctx->buf->da -
					ctx->olaps[oix].offset +
					(pkt_size - rlen);
			pages[i].addr = pages[i].addr & PAGE_MASK;

			pg_start = (rpra[i].buf.pv & PAGE_MASK) >> PAGE_SHIFT;
			pg_end = ((rpra[i].buf.pv + len - 1) & PAGE_MASK) >> PAGE_SHIFT;
			pages[i].size = (pg_end - pg_start + 1) * PAGE_SIZE;
			args = args + mlen;
			rlen -= mlen;
			PERF_END);
		}

		if (i < inbufs && !ctx->maps[i]) {
			void *dst = (void *)(uintptr_t)rpra[i].buf.pv;
			void *src = (void *)(uintptr_t)ctx->args[i].ptr;
			PERF(ctx->fl->profile, GET_COUNTER(perf_counter, PERF_COPY),

			if (!kernel) {
				if (copy_from_user(dst, (void __user *)src,
							len)) {
					RPC_ERR("invalid buffer length 0x%llx", len);
					err = -EFAULT;
					goto bail;
				}
			} else {
				memcpy(dst, src, len);
			}
			PERF_END);
		}
	}

	for (i = ctx->nbufs; i < ctx->nscalars; ++i) {
		list[i].num = ctx->args[i].length ? 1 : 0;
		list[i].pgidx = i;
		if (ctx->maps[i]) {
			pages[i].addr = ctx->maps[i]->da;
			pages[i].size = ctx->maps[i]->size;
		}
		rpra[i].dma.fd = ctx->args[i].fd;
		rpra[i].dma.len = ctx->args[i].length;
		rpra[i].dma.offset = (u64) ctx->args[i].ptr;
	}

bail:
	if (err)
		RPC_ERR("get invoke args failed:%d\n", err);

	return err;
}

static int fastrpc_put_args(struct fastrpc_invoke_ctx *ctx, u32 kernel)
{
	union fastrpc_remote_arg *rpra = ctx->rpra;
	struct fastrpc_user *fl = ctx->fl;
	struct fastrpc_map *mmap = NULL;
	struct fastrpc_invoke_buf *list;
	struct fastrpc_phy_page *pages;
	u64 *fdlist, *perf_dsp_list;
	u32 *crclist, *poll;
	int i, inbufs, outbufs, handles, perferr;

	inbufs = REMOTE_SCALARS_INBUFS(ctx->sc);
	outbufs = REMOTE_SCALARS_OUTBUFS(ctx->sc);
	handles = REMOTE_SCALARS_INHANDLES(ctx->sc) +
			REMOTE_SCALARS_OUTHANDLES(ctx->sc);
	list = fastrpc_invoke_buf_start(rpra, ctx->nscalars);
	pages = fastrpc_phy_page_start(list, ctx->nscalars);
	fdlist = (u64 *)(pages + inbufs + outbufs + handles);
	crclist = (u32 *)(fdlist + FASTRPC_MAX_FDLIST);
	poll = (u32 *)(crclist + FASTRPC_MAX_CRCLIST);
	perf_dsp_list = (u64 *)(poll + 1);

	for (i = inbufs; i < ctx->nbufs; ++i) {
		if (!ctx->maps[i]) {
			void *src = (void *)(uintptr_t)rpra[i].buf.pv;
			void *dst = (void *)(uintptr_t)ctx->args[i].ptr;
			u64 len = rpra[i].buf.len;

			if (!kernel) {
				if (copy_to_user((void __user *)dst, src, len))
					return -EFAULT;
			} else {
				memcpy(dst, src, len);
			}
		}
	}

	for (i = 0; i < FASTRPC_MAX_FDLIST; i++) {
		if (!fdlist[i])
			break;
		mutex_lock(&fl->map_mutex);
		if (!fastrpc_map_lookup(fl, (int)fdlist[i], 0, 0,
					0, &mmap, false))
			fastrpc_map_put(mmap);
		mutex_unlock(&fl->map_mutex);
	}
	if (ctx->crc && crclist && rpra) {
		if (copy_to_user((void __user *)ctx->crc, crclist,
					FASTRPC_MAX_CRCLIST * sizeof(u32)))
			return -EFAULT;
	}
	if (ctx->perf_dsp && perf_dsp_list) {
		if (0 != (perferr = copy_to_user((void __user *)ctx->perf_dsp,
				perf_dsp_list,
				FASTRPC_DSP_PERF_LIST * sizeof(u64)))) {
			RPC_ERR("failed to copy perf data %d\n", perferr);
		}
	}

	return 0;
}

static int fastrpc_invoke_send(struct fastrpc_invoke_ctx *ctx,
				u32 kernel, uint32_t handle)
{
	struct fastrpc_channel_ctx *cctx;
	struct fastrpc_user *fl = ctx->fl;
	struct fastrpc_msg *msg = &ctx->msg;
	int ret;

	cctx = fl->cctx;
	msg->pid = fl->upid;
	msg->tid = current->pid;

	if (kernel == KERNEL_MSG_WITH_ZERO_PID)
		msg->pid = 0;

	/* Last 2 ctx ID bits, to route glink msg to appropriate PD type on DSP */
	msg->ctx = FASTRPC_PACK_PD_IN_CTXID(ctx->ctxid, fl->pd_type);
	msg->handle = handle;
	msg->sc = ctx->sc;
	msg->addr = ctx->buf ? ctx->buf->da : 0;
	msg->size = roundup(ctx->msg_sz, PAGE_SIZE);

	ret = fastrpc_transport_send(cctx, (void *)msg, sizeof(*msg));

	return ret;
}

static void fastrpc_update_invoke_count(u32 handle, u64 *perf_counter,
					struct timespec64 *invoket)
{
	u64 *invcount, *count;

	invcount = GET_COUNTER(perf_counter, PERF_INVOKE);
	if (invcount)
		*invcount += getnstimediff(invoket);

	count = GET_COUNTER(perf_counter, PERF_COUNT);
	if (count)
		*count += 1;
}

static int poll_for_remote_response(struct fastrpc_invoke_ctx *ctx, u32 timeout)
{
	int err = -EIO, ii = 0, jj = 0;
	u32 sc = ctx->sc;
	struct fastrpc_invoke_buf *list;
	struct fastrpc_phy_page *pages;
	u64 *fdlist = NULL;
	u32 *crclist = NULL, *poll = NULL;
	unsigned int inbufs, outbufs, handles;

	inbufs = REMOTE_SCALARS_INBUFS(sc);
	outbufs = REMOTE_SCALARS_OUTBUFS(sc);
	handles = REMOTE_SCALARS_INHANDLES(sc) + REMOTE_SCALARS_OUTHANDLES(sc);
	list = fastrpc_invoke_buf_start(ctx->rpra, ctx->nscalars);
	pages = fastrpc_phy_page_start(list, ctx->nscalars);
	fdlist = (u64 *)(pages + inbufs + outbufs + handles);
	crclist = (u32 *)(fdlist + FASTRPC_MAX_FDLIST);
	poll = (u32 *)(crclist + FASTRPC_MAX_CRCLIST);

	for (ii = 0, jj = 0; ii < timeout; ii++, jj++) {
		if (*poll == FASTRPC_EARLY_WAKEUP_POLL) {
			/* Remote processor sent early response */
			err = 0;
			break;
		} else if (*poll == FASTRPC_POLL_RESPONSE) {
			err = 0;
			ctx->is_work_done = true;
			ctx->retval = 0;
			break;
		}
		if (jj == FASTRPC_POLL_TIME_MEM_UPDATE) {
			/* Wait for DSP to finish updating poll memory */
			rmb();
			jj = 0;
		}
		udelay(1);
	}

	return err;
}

static inline int fastrpc_wait_for_response(struct fastrpc_invoke_ctx *ctx,
						u32 kernel)
{
	int interrupted = 0;

	if (kernel)
		wait_for_completion(&ctx->work);
	else
		interrupted = wait_for_completion_interruptible(&ctx->work);

	return interrupted;
}

static void fastrpc_wait_for_completion(struct fastrpc_invoke_ctx *ctx,
					int *ptr_interrupted, u32 kernel)
{
	int err = 0, jj = 0;
	bool wait_resp = false;
	u32 wTimeout = FASTRPC_USER_EARLY_HINT_TIMEOUT;
	u32 wakeTime = ctx->early_wake_time;

	do {
		switch (ctx->rsp_flags) {
		/* try polling on completion with timeout */
		case USER_EARLY_SIGNAL:
			preempt_disable();
			jj = 0;
			wait_resp = false;
			for (; wakeTime < wTimeout && jj < wTimeout; jj++) {
				wait_resp = try_wait_for_completion(&ctx->work);
				if (wait_resp)
					break;
				udelay(1);
			}
			preempt_enable();
			if (!wait_resp) {
				*ptr_interrupted = fastrpc_wait_for_response(ctx, kernel);
				if (*ptr_interrupted || ctx->is_work_done)
					return;
			}
			break;
		case EARLY_RESPONSE:
			err = poll_for_remote_response(ctx, FASTRPC_POLL_TIME);
			if (!err) {
				ctx->is_work_done = true;
				return;
			}
			if (!ctx->is_work_done) {
				*ptr_interrupted = fastrpc_wait_for_response(ctx, kernel);
				if (*ptr_interrupted || ctx->is_work_done)
					return;
			}
			break;
		case COMPLETE_SIGNAL:
		case NORMAL_RESPONSE:
			*ptr_interrupted = fastrpc_wait_for_response(ctx, kernel);
			if (*ptr_interrupted || ctx->is_work_done)
				return;
			break;
		case POLL_MODE:
			err = poll_for_remote_response(ctx, ctx->fl->poll_timeout);
			if (err)
				ctx->rsp_flags = NORMAL_RESPONSE;
			else
				*ptr_interrupted = 0;
			break;
		default:
			*ptr_interrupted = -EBADR;
			RPC_ERR("unsupported response type:0x%x\n", ctx->rsp_flags);
			break;
		}
	} while (!ctx->is_work_done);
}

static int fastrpc_internal_invoke(struct fastrpc_user *fl, u32 kernel,
					struct fastrpc_enhanced_invoke *invoke)
{
	struct fastrpc_invoke_ctx *ctx = NULL;
	struct fastrpc_invoke *inv = &invoke->inv;
	u32 handle, sc;
	int err = 0, perferr = 0, interrupted = 0;
	u64 *perf_counter = NULL;
	struct timespec64 invoket = {0};
#if IS_ENABLED(CONFIG_HYBRID_FASTRPC_RSM)
	bool need_rsm = false;
#endif

	RPC_DBG("start pid=%d,tid=%d,sc=0x%x,hdl=0x%x\n",
			fl->tgid, current->pid, inv->sc, inv->handle);
	if (atomic_read(&fl->cctx->teardown))
		return -EPIPE;

	if (fl->profile)
		ktime_get_real_ts64(&invoket);

	handle = inv->handle;
	sc = inv->sc;
	if (handle == FASTRPC_INIT_HANDLE && !kernel) {
		RPC_ERR("user app trying to send a kernel RPC message (%d)\n",
				handle);
		return -EPERM;
	}

	if (!kernel) {
		ctx = fastrpc_context_restore_interrupted(fl, inv);
		if (IS_ERR(ctx))
			return PTR_ERR(ctx);
		if (ctx)
			goto wait;
	}

	ctx = fastrpc_context_alloc(fl, kernel, sc, invoke);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);
	if (fl->profile)
		perf_counter = (u64 *)ctx->perf + PERF_COUNT;
	PERF(fl->profile, GET_COUNTER(perf_counter, PERF_GETARGS),
	err = fastrpc_get_args(kernel, ctx);
	if (err)
		goto bail;
	PERF_END);

	/* make sure that all CPU memory writes are seen by DSP */
	dma_wmb();

#if IS_ENABLED(CONFIG_HYBRID_FASTRPC_RSM)
	need_rsm = fastrpc_domain_needs_rsm(fl->cctx->domain->id);
	/*
	 * static handles are directly used by fastRPC itself rather than its client,
	 * and also it will not use those special DSP resource (e.g., VTCM) we need to
	 * use RSM to control their sharing w/ host side
	 */
	if (need_rsm && handle > FASTRPC_MAX_STATIC_HANDLE) {
		/* need to acquire resource from rsm/compresssched before accessing DSP */
		err = fastrpc_rsm_acquire(fl, current->pid);
		if (err) {
			RPC_ERR("fastrpc_rsm_acquire failed, pid %d\n", current->pid);
			goto bail;
		}
	}
#endif

	PERF(fl->profile, GET_COUNTER(perf_counter, PERF_LINK),
	err = fastrpc_invoke_send(ctx, kernel, handle);
	if (err)
		goto bail;
	PERF_END);

wait:
	if (fl->poll_mode &&
		handle > FASTRPC_MAX_STATIC_HANDLE &&
		fl->cctx->domain->type == FASTRPC_NSP &&
		fl->pd_type == DYNAMIC_PD)
		ctx->rsp_flags = POLL_MODE;

	fastrpc_wait_for_completion(ctx, &interrupted, kernel);
	if (interrupted != 0) {
		err = interrupted;
		goto bail;
	}

	if (!ctx->is_work_done) {
		err = -ETIMEDOUT;
		RPC_ERR("invalid workdone state for handle 0x%x, sc 0x%x\n",
				handle, sc);
		goto bail;
	}

	/* make sure that all memory writes by DSP are seen by CPU */
	dma_rmb();

	PERF(fl->profile, GET_COUNTER(perf_counter, PERF_PUTARGS),
	err = fastrpc_put_args(ctx, kernel);
	if (err)
		goto bail;
	PERF_END);

	err = ctx->retval;
	if (err)
		goto bail;
bail:
	if (ctx && interrupted == -ERESTARTSYS) {
		fastrpc_context_save_interrupted(ctx);
	} else if (ctx) {
		if (fl->profile && !interrupted)
			fastrpc_update_invoke_count(handle, perf_counter, &invoket);
		if (fl->profile && ctx->perf && handle > FASTRPC_RMID_INIT_MAX) {
			if (ctx->perf_kernel)
				if (0 != (perferr = copy_to_user((void __user *)ctx->perf_kernel,
						ctx->perf, FASTRPC_KERNEL_PERF_LIST * sizeof(u64))))
					RPC_WARN("failed to copy perf data err 0x%x\n", perferr);
		}
		spin_lock(&fl->lock);
		list_del(&ctx->node);
		spin_unlock(&fl->lock);
		fastrpc_context_put(ctx);
	}

	RPC_DBG("end err=%d,pid=%d,tid=%d,sc=0x%x,hdl=0x%x\n",
			err, fl->tgid, current->pid,
			inv->sc, inv->handle);

#if IS_ENABLED(CONFIG_HYBRID_FASTRPC_RSM)
	if (need_rsm && handle > FASTRPC_MAX_STATIC_HANDLE)
		fastrpc_rsm_release(fl, current->pid, FASTRPC_RSM_SIGNAL_CORE);
#endif
	return err;
}

int fastrpc_invoke(struct fastrpc_user *fl, char __user *argp)
{
	struct fastrpc_enhanced_invoke ioctl;
	struct fastrpc_invoke inv;
	int err;

	if (copy_from_user(&inv, argp, sizeof(inv)))
		return -EFAULT;

	ioctl.inv = inv;

	err = fastrpc_internal_invoke(fl, USER_MSG, &ioctl);

	return err;
}

static int fastrpc_internal_control(struct fastrpc_user *fl,
		struct fastrpc_internal_control *cp)
{
	int err = 0;

	switch (cp->req) {
	case FASTRPC_CONTROL_LATENCY:
		err = 0;
		break;
	default:
		err = -EBADRQC;
		break;
	}

	return err;
}

#if IS_ENABLED(CONFIG_HYBRID_FASTRPC_RSM)
static bool fastrpc_multidomain_needs_rsm(struct fastrpc_mdctx_info *mdctx)
{
	bool need_rsm = false;
	int ii = 0;

	for (ii = 0; ii < mdctx->num_domains; ii++)
		need_rsm |= fastrpc_domain_needs_rsm(mdctx->domains[ii]);

	return need_rsm;
}
#endif

static int fastrpc_dspsignal_signal(struct fastrpc_user *fl,
					struct fastrpc_internal_dspsignal *fsig)
{
	int err = 0;
	struct fastrpc_channel_ctx *cctx = NULL;
	u64 msg = 0;
	u32 signal_id = fsig->signal_id;
#if IS_ENABLED(CONFIG_HYBRID_FASTRPC_RSM)
	bool need_rsm = false;
	hfastrpc_rsm_dspsignal_msg rsm_dspsignal_msg;
#endif

	DSPSIGNAL_VERBOSE("send signal PID %u, unique fastrpc pid %u signal %u\n",
			fl->tgid, fl->upid, signal_id);
	cctx = fl->cctx;
	if (!(signal_id < FASTRPC_DSPSIGNAL_NUM_SIGNALS)) {
		RPC_ERR("sending bad signal %u for PID %u",
				signal_id, fl->tgid);
		return -EINVAL;
	}

	msg = (((uint64_t)fl->upid) << 32) | ((uint64_t)fsig->signal_id);
#if IS_ENABLED(CONFIG_HYBRID_FASTRPC_RSM)
	need_rsm = fastrpc_domain_needs_rsm(fl->cctx->domain->id);
	/**
	 * @brief Expected Task dispatch & completion workflow via dspqueue and signals for NSP
	 * shared between GVM and host/PVM through compute resource manager/RSM
	 *
	 * Sequence:
	 * 1) HLOS enqueues a compute task into dspqueue, then sends DSPQUEUE_SIGNAL_REQ_PACKET
	 *    to notify the DSP that new work is available.
	 * 2) DSP receives DSPQUEUE_SIGNAL_REQ_PACKET, dequeues the task from dspqueue,
	 *    and executes the computation.
	 * 3) After finishing, DSP writes the result back into dspqueue, then sends
	 *    DSPQUEUE_SIGNAL_RESP_PACKET to notify HLOS that the result is ready.
	 * 4) HLOS receives DSPQUEUE_SIGNAL_RESP_PACKET, gets the result from dspqueue.
	 *    This completes one full round trip.
	 * 5) HLOS continues by enqueuing the next compute task into dspqueue and repeats.
	 */
	if (need_rsm) {
		/*
		 * Only sending DSPQUEUE_SIGNAL_REQ_PACKET requires calling
		 * compressched_acquire(). No other signals should be sent in
		 * this scenario (e.g., DSPQUEUE_SIGNAL_RESP_SPACE).
		 * DSPQUEUE_SIGNAL_RESP_SPACE is used only when DSP previously
		 * ran out of space due to continuous writes and DSP is blocked
		 * by waiting for DSPQUEUE_SIGNAL_RESP_SPACE; after HLOS reads
		 * and frees space, HLOS would send DSPQUEUE_SIGNAL_RESP_SPACE to
		 * unblock DSP from above waiting and continue writing. This flow
		 * is not valid for NSP shared between GVM and host/PVM through
		 * compute resource manager/RSM.
		 */
		if (GET_SIGNAL_NO(signal_id) != DSPQUEUE_SIGNAL_REQ_PACKET) {
			RPC_ERR("unexpected signal %u to be sent to this dsp shared through compute resource manager for PID %u",
				GET_SIGNAL_NO(signal_id), fl->tgid);
			return -EINVAL;
		}
		err = fastrpc_rsm_acquire(fl, fl->upid);
		if (err)
			return err;
		rsm_dspsignal_msg.legacy_msg = msg;
		rsm_dspsignal_msg.target_id = fl->upid;
		/* RSM case */
		RPC_DBG("rsm_dspsignal_msg sent, target_id %llu, upid %d, signal_id %u",
						rsm_dspsignal_msg.target_id, fl->upid, fsig->signal_id);
		err = fastrpc_transport_send(cctx, (void *)&rsm_dspsignal_msg,
						sizeof(hfastrpc_rsm_dspsignal_msg));
	} else {
		/* non-RSM case */
		RPC_DBG("dspsignal msg sent, upid %d, signal_id %u\n",
						fl->upid, fsig->signal_id);
		err = fastrpc_transport_send(cctx, (void *)&msg, sizeof(msg));
	}
#else
	err = fastrpc_transport_send(cctx, (void *)&msg, sizeof(msg));
#endif
	return err;
}

#if IS_ENABLED(CONFIG_HYBRID_FASTRPC_RSM)
static int fastrpc_dspsignal_signal_mc(struct fastrpc_user *fl,
					struct fastrpc_internal_dspsignal_mc *fmcsig)
{
	int err = 0;
	struct fastrpc_channel_ctx *cctx = NULL;
	u64 msg = 0;
	u32 signal_id = fmcsig->signal_id;
	unsigned int target_id = fmcsig->ctx;
	hfastrpc_rsm_dspsignal_msg rsm_dspsignal_msg;

	DSPSIGNAL_VERBOSE("send signal PID %u, unique fastrpc pid %u signal %u\n",
			fl->tgid, fl->upid, signal_id);
	cctx = fl->cctx;
	if (!(signal_id < FASTRPC_DSPSIGNAL_NUM_SIGNALS)) {
		RPC_ERR("sending bad signal %u for PID %u",
				signal_id, fl->tgid);
		return -EINVAL;
	}

	msg = (((uint64_t)fl->upid) << 32) | ((uint64_t)fmcsig->signal_id);
	if (GET_SIGNAL_NO(signal_id) != DSPQUEUE_SIGNAL_REQ_PACKET) {
		RPC_ERR("unexpected signal %u to be sent to this dsp shared through compute resource manager for PID %u",
			GET_SIGNAL_NO(signal_id), fl->tgid);
		return -EINVAL;
	}
	err = fastrpc_multidomain_rsm_acquire(fl, target_id);
	if (err)
		return err;
	rsm_dspsignal_msg.legacy_msg = msg;
	rsm_dspsignal_msg.target_id = target_id;
	/* RSM case */
	RPC_DBG("rsm_dspsignal_msg sent, target_id %llu, upid %d, signal_id %u",
					rsm_dspsignal_msg.target_id, fl->upid, fmcsig->signal_id);
	err = fastrpc_transport_send(cctx, (void *)&rsm_dspsignal_msg,
					sizeof(hfastrpc_rsm_dspsignal_msg));

	return err;
}
#endif

static int fastrpc_dspsignal_wait(struct fastrpc_user *fl,
					struct fastrpc_internal_dspsignal *fsig)
{
	int err = 0;
	uint32_t timeout_usec = fsig->timeout_usec;
	unsigned long timeout = usecs_to_jiffies(fsig->timeout_usec);
	u32 signal_id = fsig->signal_id;
	struct fastrpc_dspsignal *s = NULL;
	long ret = 0;
	unsigned long irq_flags = 0;
#if IS_ENABLED(CONFIG_HYBRID_FASTRPC_RSM)
	bool need_rsm = false;
#endif

	DSPSIGNAL_VERBOSE("wait for signal %u\n", signal_id);
	if (!(signal_id < FASTRPC_DSPSIGNAL_NUM_SIGNALS)) {
		RPC_ERR("waiting on bad signal %u\n", signal_id);
		return -EINVAL;
	}

	spin_lock_irqsave(&fl->dspsignals_lock, irq_flags);
	if (fl->signal_groups[signal_id / FASTRPC_DSPSIGNAL_GROUP_SIZE] != NULL) {
		struct fastrpc_dspsignal *group =
			fl->signal_groups[signal_id / FASTRPC_DSPSIGNAL_GROUP_SIZE];

		s = &group[signal_id % FASTRPC_DSPSIGNAL_GROUP_SIZE];
	}
	if ((s == NULL) || (s->state == DSPSIGNAL_STATE_UNUSED)) {
		spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);
		RPC_ERR("unknown signal id %u\n", signal_id);
		return -ENOENT;
	}
	if (s->state != DSPSIGNAL_STATE_PENDING) {
		if ((s->state == DSPSIGNAL_STATE_CANCELED) ||
				(s->state == DSPSIGNAL_STATE_UNUSED))
			err = -EINTR;
		spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);
		DSPSIGNAL_VERBOSE("signal %u in state %u, complete wait immediately",
				signal_id, s->state);
		return err;
	}
	spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);
	if (timeout_usec != 0xffffffff)
		ret = wait_for_completion_interruptible_timeout(&s->comp, timeout);
	else
		ret = wait_for_completion_interruptible(&s->comp);

	if (timeout_usec != 0xffffffff && ret == 0) {
		DSPSIGNAL_VERBOSE("wait for signal %u timed out\n", signal_id);
		return -ETIMEDOUT;
	} else if (ret < 0) {
		RPC_ERR("wait for signal %u failed %d\n", signal_id, (int)ret);
		return ret;
	}

	spin_lock_irqsave(&fl->dspsignals_lock, irq_flags);
	if (s->state == DSPSIGNAL_STATE_SIGNALED) {
		s->state = DSPSIGNAL_STATE_PENDING;
		DSPSIGNAL_VERBOSE("signal %u completed\n", signal_id);
	} else if ((s->state == DSPSIGNAL_STATE_CANCELED) ||
			(s->state == DSPSIGNAL_STATE_UNUSED)) {
		DSPSIGNAL_VERBOSE("signal %u cancelled or destroyed\n", signal_id);
		err = -EINTR;
	}
	spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);
#if IS_ENABLED(CONFIG_HYBRID_FASTRPC_RSM)
	need_rsm = fastrpc_domain_needs_rsm(fl->cctx->domain->id);
	/* refer to above workflow */
	if (need_rsm) {
		/*
		 * Only recieving DSPQUEUE_SIGNAL_RESP_PACKET requires calling
		 * compressched_release(). No other signals should be sent in
		 * this scenario (e.g., DSPQUEUE_SIGNAL_REQ_SPACE).
		 * DSPQUEUE_SIGNAL_REQ_SPACE is used only when HLOS previously
		 * ran out of space due to continuous writes and HLOS is blocked
		 * by waiting for DSPQUEUE_SIGNAL_REQ_SPACE; after DSP reads and
		 * frees space, DSP would send DSPQUEUE_SIGNAL_REQ_SPACE to unblock
		 * HLOS from above waiting and continue writing. This flow is not
		 * valid for NSP shared between GVM and host/PVM through compute
		 * resource manager/RSM.
		 */
		if(GET_SIGNAL_NO(signal_id) != DSPQUEUE_SIGNAL_RESP_PACKET) {
			RPC_ERR("unexpected signal %u received from this dsp shared through compute resource manager for PID %u",
				GET_SIGNAL_NO(signal_id), fl->tgid);
			return -EINVAL;
		}
		fastrpc_rsm_release(fl, fl->upid, FASTRPC_RSM_SIGNAL_CORE);
	}
#endif
	return err;
}

#if IS_ENABLED(CONFIG_HYBRID_FASTRPC_RSM)
static int fastrpc_dspsignal_wait_mc(struct fastrpc_user *fl,
					struct fastrpc_internal_dspsignal_mc *fmcsig)
{
	int err = 0;
	uint32_t timeout_usec = fmcsig->timeout_usec;
	unsigned long timeout = usecs_to_jiffies(fmcsig->timeout_usec);
	u32 signal_id = fmcsig->signal_id;
	struct fastrpc_dspsignal *s = NULL;
	long ret = 0;
	unsigned long irq_flags = 0;
	unsigned int target_id = fmcsig->ctx;


	DSPSIGNAL_VERBOSE("wait for signal %u\n", signal_id);
	if (!(signal_id < FASTRPC_DSPSIGNAL_NUM_SIGNALS)) {
		RPC_ERR("waiting on bad signal %u\n", signal_id);
		return -EINVAL;
	}

	spin_lock_irqsave(&fl->dspsignals_lock, irq_flags);
	if (fl->signal_groups[signal_id / FASTRPC_DSPSIGNAL_GROUP_SIZE] != NULL) {
		struct fastrpc_dspsignal *group =
			fl->signal_groups[signal_id / FASTRPC_DSPSIGNAL_GROUP_SIZE];

		s = &group[signal_id % FASTRPC_DSPSIGNAL_GROUP_SIZE];
	}
	if ((s == NULL) || (s->state == DSPSIGNAL_STATE_UNUSED)) {
		spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);
		RPC_ERR("unknown signal id %u\n", signal_id);
		return -ENOENT;
	}
	if (s->state != DSPSIGNAL_STATE_PENDING) {
		if ((s->state == DSPSIGNAL_STATE_CANCELED) ||
				(s->state == DSPSIGNAL_STATE_UNUSED))
			err = -EINTR;
		spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);
		DSPSIGNAL_VERBOSE("signal %u in state %u, complete wait immediately",
				signal_id, s->state);
		return err;
	}
	spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);
	if (timeout_usec != 0xffffffff)
		ret = wait_for_completion_interruptible_timeout(&s->comp, timeout);
	else
		ret = wait_for_completion_interruptible(&s->comp);

	if (timeout_usec != 0xffffffff && ret == 0) {
		DSPSIGNAL_VERBOSE("wait for signal %u timed out\n", signal_id);
		return -ETIMEDOUT;
	} else if (ret < 0) {
		RPC_ERR("wait for signal %u failed %d\n", signal_id, (int)ret);
		return ret;
	}

	spin_lock_irqsave(&fl->dspsignals_lock, irq_flags);
	if (s->state == DSPSIGNAL_STATE_SIGNALED) {
		s->state = DSPSIGNAL_STATE_PENDING;
		DSPSIGNAL_VERBOSE("signal %u completed\n", signal_id);
	} else if ((s->state == DSPSIGNAL_STATE_CANCELED) ||
			(s->state == DSPSIGNAL_STATE_UNUSED)) {
		DSPSIGNAL_VERBOSE("signal %u cancelled or destroyed\n", signal_id);
		err = -EINTR;
	}
	spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);
	if(GET_SIGNAL_NO(signal_id) != DSPQUEUE_SIGNAL_RESP_PACKET) {
		RPC_ERR("unexpected signal %u received from this dsp shared through compute resource manager for PID %u",
			GET_SIGNAL_NO(signal_id), fl->tgid);
		return -EINVAL;
	}
	fastrpc_multidomain_rsm_release(fl, target_id);

	return err;
}
#endif

static int fastrpc_dspsignal_create(struct fastrpc_user *fl,
					struct fastrpc_internal_dspsignal *fsig)
{
	int err = 0;
	u32 signal_id = fsig->signal_id;
	struct fastrpc_dspsignal *group, *sig;
	unsigned long irq_flags = 0;

	if (!(signal_id <FASTRPC_DSPSIGNAL_NUM_SIGNALS))
		return -EINVAL;

	mutex_lock(&fl->signal_create_mutex);
	spin_lock_irqsave(&fl->dspsignals_lock, irq_flags);

	group = fl->signal_groups[signal_id / FASTRPC_DSPSIGNAL_GROUP_SIZE];
	if (group == NULL) {
		int i;

		spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);
		group = kzalloc(FASTRPC_DSPSIGNAL_GROUP_SIZE * sizeof(*group),
				GFP_KERNEL);
		if (group == NULL) {
			RPC_ERR("unable to allocate signal group\n");
			mutex_unlock(&fl->signal_create_mutex);
			return -ENOMEM;
		}

		for (i = 0; i < FASTRPC_DSPSIGNAL_GROUP_SIZE; i++) {
			sig = &group[i];
			init_completion(&sig->comp);
			sig->state = DSPSIGNAL_STATE_UNUSED;
		}
		spin_lock_irqsave(&fl->dspsignals_lock, irq_flags);
		fl->signal_groups[signal_id / FASTRPC_DSPSIGNAL_GROUP_SIZE] = group;
	}

	sig = &group[signal_id % FASTRPC_DSPSIGNAL_GROUP_SIZE];
	if (sig->state != DSPSIGNAL_STATE_UNUSED) {
		spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);
		mutex_unlock(&fl->signal_create_mutex);
		RPC_ERR("attempting to create signal %u already in use (state %u)\n",
				signal_id, sig->state);
		return -EBUSY;
	}

	sig->state = DSPSIGNAL_STATE_PENDING;
	reinit_completion(&sig->comp);

	spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);
	mutex_unlock(&fl->signal_create_mutex);
	DSPSIGNAL_VERBOSE("signal %u created\n", signal_id);

	return err;
}

static int fastrpc_dspsignal_destroy(struct fastrpc_user *fl,
					struct fastrpc_internal_dspsignal *fsig)
{
	u32 signal_id = fsig->signal_id;
	struct fastrpc_dspsignal *s = NULL;
	unsigned long irq_flags = 0;

	DSPSIGNAL_VERBOSE("destroy signal %u\n", signal_id);
	if (!(signal_id < FASTRPC_DSPSIGNAL_NUM_SIGNALS))
		return -EINVAL;

	spin_lock_irqsave(&fl->dspsignals_lock, irq_flags);

	if (fl->signal_groups[signal_id / FASTRPC_DSPSIGNAL_GROUP_SIZE] != NULL) {
		struct fastrpc_dspsignal *group =
			fl->signal_groups[signal_id / FASTRPC_DSPSIGNAL_GROUP_SIZE];

		s = &group[signal_id % FASTRPC_DSPSIGNAL_GROUP_SIZE];
	}
	if ((s == NULL) || (s->state == DSPSIGNAL_STATE_UNUSED)) {
		spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);
		RPC_ERR("attempting to destroy unused signal %u\n", signal_id);
		return -ENOENT;
	}

	s->state = DSPSIGNAL_STATE_UNUSED;
	complete_all(&s->comp);

	spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);
	DSPSIGNAL_VERBOSE("signal %u destroyed\n", signal_id);

	return 0;
}

static int fastrpc_dspsignal_cancel_wait(struct fastrpc_user *fl,
					struct fastrpc_internal_dspsignal *fsig)
{
	u32 signal_id = fsig->signal_id;
	struct fastrpc_dspsignal *s = NULL;
	unsigned long irq_flags = 0;

	DSPSIGNAL_VERBOSE("cancel wait for signal %u\n", signal_id);
	if (!(signal_id < FASTRPC_DSPSIGNAL_NUM_SIGNALS))
		return -EINVAL;

	spin_lock_irqsave(&fl->dspsignals_lock, irq_flags);

	if (fl->signal_groups[signal_id / FASTRPC_DSPSIGNAL_GROUP_SIZE] != NULL) {
		struct fastrpc_dspsignal *group =
			fl->signal_groups[signal_id / FASTRPC_DSPSIGNAL_GROUP_SIZE];

		s = &group[signal_id % FASTRPC_DSPSIGNAL_GROUP_SIZE];
	}
	if ((s == NULL) || (s->state == DSPSIGNAL_STATE_UNUSED)) {
		spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);
		RPC_ERR("attempting to cancel unused signal %u\n", signal_id);
		return -ENOENT;
	}

	if (s->state != DSPSIGNAL_STATE_CANCELED) {
		s->state = DSPSIGNAL_STATE_CANCELED;
		complete_all(&s->comp);
	}

	spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);
	DSPSIGNAL_VERBOSE("signal %u cancelled\n", signal_id);

	return 0;
}

/*
 * Unblock all dspsignals in pending state. It is only used by
 * SSR use case, because driver won't handle ioctl from user to
 * cancel pending dspsignal wait, we need to cancel them by driver
 * itself.
 * */
static int fastrpc_dspsignal_cancel_all(struct fastrpc_user *fl)
{
	u32 i = 0, j = 0;
	struct fastrpc_dspsignal *s = NULL;
	struct fastrpc_dspsignal *group = NULL;
	unsigned long irq_flags = 0;

	RPC_DBG("Cancel all signals for pid %d\n", fl->tgid);

	spin_lock_irqsave(&fl->dspsignals_lock, irq_flags);
	for (i = 0; i < (FASTRPC_DSPSIGNAL_NUM_SIGNALS
		/ FASTRPC_DSPSIGNAL_GROUP_SIZE); i++) {
		group = fl->signal_groups[i];
		if (!group)
			continue;

		for (j = 0; j < FASTRPC_DSPSIGNAL_GROUP_SIZE; j++) {
			s = &group[j];
			if (s->state == DSPSIGNAL_STATE_PENDING) {
				s->state = DSPSIGNAL_STATE_CANCELED;
				complete_all(&s->comp);
			}
		}
	}
	spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);

	RPC_DBG("All signals canceled for pid %d\n", fl->tgid);
	return 0;
}

static int fastrpc_invoke_dspsignal(struct fastrpc_user *fl,
					struct fastrpc_internal_dspsignal *fsig)
{
	int err = 0;

	switch(fsig->req) {
	case FASTRPC_DSPSIGNAL_SIGNAL:
		err = fastrpc_dspsignal_signal(fl,fsig);
		break;
	case FASTRPC_DSPSIGNAL_WAIT :
		err = fastrpc_dspsignal_wait(fl,fsig);
		break;
	case FASTRPC_DSPSIGNAL_CREATE :
		err = fastrpc_dspsignal_create(fl,fsig);
		break;
	case FASTRPC_DSPSIGNAL_DESTROY :
		err = fastrpc_dspsignal_destroy(fl,fsig);
		break;
	case FASTRPC_DSPSIGNAL_CANCEL_WAIT :
		err = fastrpc_dspsignal_cancel_wait(fl,fsig);
		break;
	}

	return err;
}

#if IS_ENABLED(CONFIG_HYBRID_FASTRPC_RSM)
static int fastrpc_invoke_dspsignal_mc(struct fastrpc_user *fl,
					struct fastrpc_internal_dspsignal_mc *fmcsig)
{
	int err = 0;
	bool need_rsm = false;
	struct fastrpc_internal_dspsignal *fsig = NULL;
	unsigned int ctx;

	fsig = kzalloc(sizeof(*fsig), GFP_KERNEL);
	if (!fsig)
		return -ENOMEM;

	fsig->req = fmcsig->req;
	fsig->signal_id = fmcsig->signal_id;
	fsig->flags = fmcsig->flags;
	fsig->timeout_usec = fmcsig->timeout_usec;
	ctx = fmcsig->ctx;
	need_rsm = fastrpc_multidomain_ctx_needs_rsm(fl, ctx);

	switch(fmcsig->req) {

	case FASTRPC_DSPSIGNAL_SIGNAL_MC:
		if (need_rsm)
			err = fastrpc_dspsignal_signal_mc(fl, fmcsig);
		else
			err = fastrpc_dspsignal_signal(fl, fsig);

		break;
	case FASTRPC_DSPSIGNAL_WAIT_MC :
		if (need_rsm)
			err = fastrpc_dspsignal_wait_mc(fl, fmcsig);
		else
			err = fastrpc_dspsignal_wait(fl, fsig);
		break;
	}

	kfree(fsig);
	return err;
}
#endif

void fastrpc_notify_users(struct fastrpc_user *user)
{
	struct fastrpc_invoke_ctx *ctx;
	struct fastrpc_user *fl;

	spin_lock(&user->lock);
	list_for_each_entry(ctx, &user->pending, node) {
		fl = ctx->fl;
		ctx->retval = -EPIPE;
		ctx->is_work_done = true;
		complete(&ctx->work);
	}
	list_for_each_entry(ctx, &user->interrupted, node) {
		ctx->retval = -EPIPE;
		ctx->is_work_done = true;
		complete(&ctx->work);
	}
	fastrpc_dspsignal_cancel_all(user);
	spin_unlock(&user->lock);
}

static int fastrpc_wait_on_notif_queue(struct fastrpc_internal_notif_rsp *notif_rsp,
					struct fastrpc_user *fl)
{
	int err = 0;
	unsigned long flags;
	struct fastrpc_notif_rsp *notif = NULL, *inotif = NULL, *n = NULL;

read_notif_status:
	err = wait_event_interruptible(fl->proc_state_notif.notif_wait_queue,
			atomic_read(&fl->proc_state_notif.notif_queue_count));
	if (err)
		return err;

	spin_lock_irqsave(&fl->proc_state_notif.nqlock, flags);
	list_for_each_entry_safe(inotif, n, &fl->notif_queue, notifn) {
		list_del(&inotif->notifn);
		atomic_sub(1, &fl->proc_state_notif.notif_queue_count);
		notif = inotif;
		break;
	}
	spin_unlock_irqrestore(&fl->proc_state_notif.nqlock, flags);

	if (notif) {
		notif_rsp->status = notif->status;
		notif_rsp->domain = notif->domain;
		notif_rsp->session = notif->session;
	} else {
		RPC_ERR("invalid status notification response\n");
		goto read_notif_status;
	}

	kfree(notif);

	return err;
}

static int fastrpc_get_notif_response(struct fastrpc_internal_notif_rsp *notif,
					void *param, struct fastrpc_user *fl, bool legacy_domains)
{
	int err = 0;
	struct fastrpc_domain *domain = NULL;

	err = fastrpc_wait_on_notif_queue(notif, fl);
	if (err)
		return err;

	/*
	 * If user is using legacy domain ids, send the legacy id back to
	 * client in process status notification.
	 */
	if (legacy_domains) {
		if (is_device_discovery_supported()) {
			domain = fastrpc_lookup_domain_in_table(notif->domain, false);
			if (domain && domain->legacy)
				notif->domain = domain->legacy_id;
		}
	}

	if (copy_to_user((void __user *)param, notif,
			sizeof(struct fastrpc_internal_notif_rsp)))
		return -EFAULT;

	return 0;
}

static int fastrpc_set_session_info(struct fastrpc_user *fl,
				struct fastrpc_internal_sessinfo *sessinfo)
{
	spin_lock(&fl->lock);
	if (fl->set_session_info) {
		spin_unlock(&fl->lock);
		RPC_ERR("set session info invoked multiple times\n");
		return -EBADR;
	}
	fl->set_session_info = true;
	spin_unlock(&fl->lock);

	if(sessinfo->pd <= DEFAULT_UNUSED ||
			sessinfo->pd >= MAX_PD_TYPE) {
		RPC_ERR("invalid PD type %d, range is %d - %d\n",
				sessinfo->pd, DEFAULT_UNUSED + 1, MAX_PD_TYPE - 1);
		return -EBADR;
	}

	if (sessinfo->session_id >= fl->cctx->max_sess_per_proc) {
		RPC_ERR("session ID %u cannot be beyond %u\n",
				sessinfo->session_id,
				fl->cctx->max_sess_per_proc);
		return -EBADR;
	}

	fl->sessionid = sessinfo->session_id;
	fl->multi_session_support = true;

	return 0;
}

/* Get fastrpc cid of given session on given domain */
static int fastrpc_get_frpc_cid_upid_fl(uint32_t domain, uint32_t session,
	int32_t *cid, uint32_t *upid, struct fastrpc_user **fl)
{
	int err = 0;
	bool found = false;
	unsigned long flags = 0;
	struct fastrpc_channel_ctx *cctx = NULL;
	struct fastrpc_user *user = NULL;

	cctx = fastrpc_get_domain_channel_ctx(domain);
	if (!cctx) {
		/* Channel is going thru ssr */
		err = -EPIPE;
		return err;
	}
	fastrpc_channel_ctx_get(cctx);
	if (atomic_read(&cctx->teardown)) {
		/* If subsystem already going thru SSR, fail immediately */
		err = -EPIPE;
		goto bail;
	}
	spin_lock_irqsave(&cctx->lock, flags);
	fastrpc_channel_update_invoke_cnt(cctx, true);
	/*
	 * Search for user objects of current process on remote channel
	 * corresponding to given domain & find object of given session
	 */
	list_for_each_entry(user, &cctx->users, user) {
		if (user->tgid == current->tgid && user->sessionid == session) {
			*cid = user->cid;
			*upid = user->upid;
			*fl = user;
			found = true;
			break;
		}
	}
	/*
	 * In multicore usecase, this is ensured by fastrpc lib that
	 * sessions are opened before mdctx creation.
	 * If no user-object is found for given remote session in the
	 * current channel context's list gotten from fastrpc_domain
	 * struct with logical domain id, it means the specific DSP
	 * has gone thru SSR and the user-object was present in the
	 * legacy channel context's list.
	 * But if fastrpc lib is not working as expected, this error
	 * code might not be accurate, that's a trade-off for simplicity
	 * of design without any side effect.
	 * */
	if (!found)
		err = -EPIPE;
	fastrpc_channel_update_invoke_cnt(cctx, false);
	spin_unlock_irqrestore(&cctx->lock, flags);
bail:
	fastrpc_channel_ctx_put(cctx);
	return err;
}

/* Helper function to get frpc tgid, upids and fls of each session of context */
static int fastrpc_multidomain_ctx_get_cids_upids_fls(struct device *dev,
	struct fastrpc_mdctx_info *mdctx)
{
	int err = 0, ii = 0;
	uint32_t logical_domain_id = 0, domain = 0 , session = 0;
	uint32_t num_domains = mdctx->num_domains;

	for (ii = 0; ii < num_domains; ii++) {
		domain = mdctx->domains[ii];
		session = mdctx->session_ids[ii];

		/* Validate domain id passed by user */
		if (fastrpc_is_valid_logical_domain_id(domain)) {
			logical_domain_id = domain;
		} else {
			if (IS_LEGACY_DOMAIN_ID(domain)) {
				/* If its a valid legacy id, get the corresponding logical id */
				err = fastrpc_convert_legacy_id_to_logical_id(domain, &logical_domain_id);
				if (err != 0) {
					dev_err(dev, "Error %d: %s: [%u of %u]: no domain found for legacy domain id %u",
						err, __func__, ii, num_domains, domain);
					break;
				}
			} else {
				/*
				 * If domain id is neither a valid logical id nor a legacy id,
				 * return error.
				 */
				err = -EINVAL;
				dev_err(dev, "Error %d: %s: [%u of %u]: %u is not a valid logical domain id",
					err, __func__, ii, num_domains, domain);
				break;
			}
		}

		if (!IS_VALID_SESSION_ID(session)) {
			err = -EINVAL;
			dev_err(dev, "Error %d: %s: [%u of %u]: session %u is invalid",
					err, __func__, ii, num_domains, session);
			break;
		}

		err = fastrpc_get_frpc_cid_upid_fl(logical_domain_id, session,
					&mdctx->cids[ii], &mdctx->upids[ii], &mdctx->fls[ii]);
		if (err) {
			dev_err(dev, "Error %d: %s: [%d of %d]: unable to get frpc tgid, upid, fl for domain %u, session %u",
							err, __func__, ii, num_domains, logical_domain_id, session);
			break;
		}
	}
	return err;
}

/* Helper function to initialize multidomain context object */
static int fastrpc_multidomain_ctx_obj_init(struct fastrpc_user *fl,
	struct fastrpc_ioctl_mdctx_manage *ctxm,
	struct fastrpc_mdctx_info **o_mdctx)
{
	int err = 0, ii = 0;
	uint32_t rsvd = 0, num_domains = ctxm->num_domains,
		max_domains = fl->cctx->gdriver->num_channels *
						FASTRPC_MAX_SESSIONS_PER_PROCESS;
	struct device *dev = fl->cctx->dev;
	size_t size = 0;
	uint32_t *domains = NULL, *session_ids = NULL, *upids = NULL;
	int32_t *cid = NULL;
	struct fastrpc_user **fls = NULL;
	struct fastrpc_mdctx_info *mdctx = NULL;

	/* Validate that reserved fields are all zero */
	for (ii = 0; ii < FASTRPC_MDCTX_IOCTL_RSVD; ii++) {
		rsvd = ctxm->reserved[ii];
		if (rsvd) {
			err = -EINVAL;
			dev_err(dev, "Error %d: %s: rsvd[%d] %u expected to be 0",
				err, __func__, ii, rsvd);
			goto bail;
		}
	}

	/* Validate number of domains passed by user */
	if (num_domains >= max_domains) {
		err = -EINVAL;
		dev_err(dev, "Error %d: %s: num domains %u more than max domains %u",
			err, __func__, num_domains, max_domains);
		goto bail;
	}

	mdctx = kzalloc(sizeof(*mdctx), GFP_KERNEL);
	if (!mdctx) {
		err = -ENOMEM;
		dev_err(dev, "Error %d: %s: failed to alloc mdctx obj",
			err, __func__);
		goto bail;
	}
	size = sizeof(*domains) * num_domains;

	/* Allocate local domains array to send to dsp */
	domains = kzalloc(size, GFP_KERNEL);
	if (!domains) {
		err = -ENOMEM;
		dev_err(dev, "Error %d: %s: failed to alloc domains array of size %zu",
			err, __func__, size);
		goto bail;
	}

	/* Copy list of domains passed by user */
	err = copy_from_user((void *)domains,
			(void __user *)(uintptr_t)ctxm->domain_ids, size);
	if (err) {
		dev_err(dev, "Error %d: %s: failed to copy domain ids from user (size %zu)",
			err, __func__, size);
		err = -EFAULT;
		goto bail;
	}

	/* Allocate local sessions array */
	session_ids = kzalloc(size, GFP_KERNEL);
	if (!session_ids) {
		err = -ENOMEM;
		dev_err(dev, "Error %d: %s: failed to alloc sessions array of size %zu",
			err, __func__, size);
		goto bail;
	}

	/* Copy list of session ids passed by user */
	err = copy_from_user((void *)session_ids,
			(void __user *)(uintptr_t)ctxm->session_ids, size);
	if (err) {
		dev_err(dev, "Error %d: %s: failed to copy session ids from user (size %zu)",
			err, __func__, size);
		err = -EFAULT;
		goto bail;
	}

	/* Allocate tgids array to send to dsp */
	size = sizeof(*cid) * num_domains;
	cid = kzalloc(size, GFP_KERNEL);
	if (!cid) {
		err = -ENOMEM;
		dev_err(dev, "Error %d: %s: failed to alloc tgids array of size %zu",
			err, __func__, size);
		goto bail;
	}

	size = sizeof(*upids) * num_domains;
	upids = kzalloc(size, GFP_KERNEL);
	if (!upids) {
		err = -ENOMEM;
		dev_err(dev, "Error %d: %s: failed to alloc pids array of size %zu",
			err, __func__, size);
		goto bail;
	}

	size = sizeof(*fls) * num_domains;
	fls = kzalloc(size, GFP_KERNEL);
	if (!fls) {
		err = -ENOMEM;
		dev_err(dev, "Error %d: %s: failed to alloc fls array of size %zu",
			err, __func__, size);
		goto bail;
	}

	mdctx->num_domains = num_domains;
	mdctx->domains = domains;
	mdctx->session_ids = session_ids;
	mdctx->cids = cid;
	mdctx->upids = upids;
	mdctx->fls = fls;
	INIT_LIST_HEAD(&mdctx->node);

	err = fastrpc_multidomain_ctx_get_cids_upids_fls(dev, mdctx);
	if (err)
		goto bail;

	*o_mdctx = mdctx;
bail:
	if (err) {
		kfree(fls);
		kfree(upids);
		kfree(cid);
		kfree(session_ids);
		kfree(domains);
		kfree(mdctx);
	}
	return err;
}

static int virt_fastrpc_mdctx_setup(struct fastrpc_user *fl,
		struct fastrpc_mdctx_info *mdctx, u64* ctx)
{
	struct fastrpc_channel_ctx *cctx = fl->cctx;
	struct fastrpc_common *gdriver = cctx->gdriver;
	struct virt_mdctx_manage_msg *vmsg, *rsp = NULL;
	struct virt_fastrpc_msg *msg;
	int err, size, i;

	size = sizeof(*vmsg) + sizeof(s32) * mdctx->num_domains;
	msg = virt_alloc_msg(fl, size);
	if (!msg) {
		RPC_ERR("out of memory\n");
		return -ENOMEM;
	}

	vmsg = (struct virt_mdctx_manage_msg *)msg->txbuf;
	vmsg->hdr.pid = fl->tgid_frpc;
	vmsg->hdr.tid = current->pid;
	vmsg->hdr.cid = fl->cid;
	vmsg->hdr.cmd = VIRTIO_FASTRPC_CMD_MDCTX_MANAGE;
	/* This len filled in header means the length of valid data. */
	vmsg->hdr.len = size;
	vmsg->hdr.msgid = msg->msgid;
	vmsg->hdr.result = 0xffffffff;
	vmsg->req = (u32)FASTRPC_MDCTX_SETUP;
	vmsg->ctx = 0;
	vmsg->num_domains = mdctx->num_domains;
	for (i = 0; i < mdctx->num_domains; i++)
		vmsg->cid[i] = mdctx->cids[i];

	/* This size provided to virtio_mmio also means the length of valid data. */
	err = fastrpc_txbuf_send(fl, vmsg, size);
	if (err)
		goto bail;
	wait_for_completion(&msg->work);

	rsp = msg->rxbuf;
	if (!rsp)
		goto bail;
	err = rsp->hdr.result;
	if (err)
		goto bail;
	if (!rsp->ctx) {
		RPC_ERR("multidomain context id is invalid\n");
		err = -EINVAL;
		goto bail;
	}
	*ctx = rsp->ctx;
	RPC_DBG("multidomain context id = %lld\n", rsp->ctx);
bail:
	if (rsp)
		fastrpc_rxbuf_send(fl, rsp, gdriver->buf_size);
	virt_free_msg(fl, msg);

	return err;
}

/*
 * Setup multidomain context in kernel
 *
 * For a multidomain context created in userspace, generate a unique
 * context id in kernel.
 *
 * Also share the list of domains on which context was created to rootpd
 * on dsp.
 */
static int fastrpc_multidomain_ctx_setup(struct fastrpc_user *fl,
	struct fastrpc_ioctl_mdctx_manage *ctxm)
{
	int err = 0;
	uint64_t ctx = 0;
	struct fastrpc_common *gdriver = fl->cctx->gdriver;
	struct mutex *gmut = &gdriver->gmut;
	struct device *dev = fl->cctx->dev;
	struct fastrpc_mdctx_info *mdctx = NULL;
#if IS_ENABLED(CONFIG_HYBRID_FASTRPC_RSM)
	bool need_rsm = false;
#endif
	err = fastrpc_multidomain_ctx_obj_init(fl, ctxm, &mdctx);
	if (err)
		return err;

	/* Call to BE to generate kernel context id and send context
	 * with peer info (i.e. domains list) to all dsps */
	mutex_lock(gmut);
	err = virt_fastrpc_mdctx_setup(fl, mdctx, &ctx);
	if (err)
		goto bail;

	/* Copy context back to user */
	err = copy_to_user((void __user *)ctxm->ctx, &ctx, sizeof(ctx));
	if (err) {
		dev_err(dev, "Error %d: %s: failed to copy ctx 0x%llx to user",
			err, __func__, ctx);
		err = -EFAULT;
		goto bail;
	}
	mdctx->ctx = ctx;
	mdctx->fl = fl;

#if IS_ENABLED(CONFIG_HYBRID_FASTRPC_RSM)
	need_rsm = fastrpc_multidomain_needs_rsm(mdctx);
	if (need_rsm) {
		err = fastrpc_multidomain_rsm_register(fl, mdctx);
		if (err)
			goto bail;
	}
#endif
	/* Add node to user's multidomain context list */
	spin_lock(&fl->lock);
	list_add_tail(&mdctx->node, &fl->mdctxs);
	spin_unlock(&fl->lock);
bail:
	if (err) {
		kfree(mdctx->cids);
		kfree(mdctx->session_ids);
		kfree(mdctx->domains);
		kfree(mdctx);
	}
	mutex_unlock(gmut);
	return err;
}

static int virt_fastrpc_mdctx_remove(struct fastrpc_user *fl, u64 ctx)
{
	struct fastrpc_channel_ctx *cctx = fl->cctx;
	struct fastrpc_common *gdriver = cctx->gdriver;
	struct virt_mdctx_manage_msg *vmsg, *rsp = NULL;
	struct virt_fastrpc_msg *msg;
	int err, size;

	/*
	 * Stop sending virtio cmd to BE as the clean up has been done
	 * during virt_fastrpc_close, and the client has been destroyed.
	 */
	spin_lock(&fl->lock);
	if (fl->state >= DSP_EXIT_START) {
		spin_unlock(&fl->lock);
		return 0;
	}
	spin_unlock(&fl->lock);

	size = sizeof(*vmsg);
	msg = virt_alloc_msg(fl, size);
	if (!msg) {
		RPC_ERR("out of memory\n");
		return -ENOMEM;
	}

	vmsg = (struct virt_mdctx_manage_msg *)msg->txbuf;
	vmsg->hdr.pid = fl->tgid_frpc;
	vmsg->hdr.tid = current->pid;
	vmsg->hdr.cid = fl->cid;
	vmsg->hdr.cmd = VIRTIO_FASTRPC_CMD_MDCTX_MANAGE;
	vmsg->hdr.len = size;
	vmsg->hdr.msgid = msg->msgid;
	vmsg->hdr.result = 0xffffffff;
	vmsg->req = (u32)FASTRPC_MDCTX_REMOVE;
	vmsg->ctx = ctx;
	vmsg->num_domains = 0;

	err = fastrpc_txbuf_send(fl, vmsg, sizeof(*vmsg));
	if (err)
		goto bail;
	wait_for_completion(&msg->work);

	rsp = msg->rxbuf;
	if (!rsp)
		goto bail;

	err = rsp->hdr.result;
bail:
	if (rsp)
		fastrpc_rxbuf_send(fl, rsp, gdriver->buf_size);
	virt_free_msg(fl, msg);

	return err;
}

/* Clean-up multidomain context resources in kernel and dsp */
static int fastrpc_multidomain_ctx_cleanup(struct fastrpc_user *fl,
	uint32_t req, uint64_t ctx)
{
	int err = 0;
	struct device *dev = fl->cctx->dev;
	struct fastrpc_common *gdriver = fl->cctx->gdriver;
	struct mutex *gmut = &gdriver->gmut;
	struct fastrpc_mdctx_info *mdctx = NULL, *imdctx, *n;

	/* Release the context - if it was allocated to same client */
	mutex_lock(gmut);
	list_for_each_entry_safe(imdctx, n, &fl->mdctxs, node) {
		if (imdctx->ctx == ctx) {
			mdctx = imdctx;
			break;
		}
	}

	if (mdctx) {
		err = virt_fastrpc_mdctx_remove(fl, ctx);
		if (err) {
			dev_err(dev, "Error %d: %s: BE failed to deregister mdctx %llu\n",
				err, __func__, ctx);
			goto bail;
		}
	} else {
		err = -ENOENT;
		dev_err(dev, "Error %d: %s: don't find matched mdctx %llu\n",
			err, __func__, ctx);
		goto bail;
	}
	/* Remove node from user's multidomain context list */
	spin_lock(&fl->lock);
	list_del(&mdctx->node);
	spin_unlock(&fl->lock);

	kfree(mdctx->cids);
	kfree(mdctx->session_ids);
	kfree(mdctx->domains);
	kfree(mdctx);
bail:
	mutex_unlock(gmut);
	return err;
}

/*
 * Release a multidomain context in kernel
 *
 * Also, send msg to dsp to release the same context
 */
static int fastrpc_multidomain_ctx_remove(struct fastrpc_user *fl,
	struct fastrpc_ioctl_mdctx_manage *ctxm)
{
	int err = 0, ii = 0;
	uint32_t rsvd = 0;

	/* Validate that reserved fields are all zero */
	for (ii = 0; ii < FASTRPC_MDCTX_IOCTL_RSVD; ii++) {
		rsvd = ctxm->reserved[ii];
		if (rsvd) {
			err = -EINVAL;
			dev_err(fl->cctx->dev, "Error %d: %s: rsvd[%d] %u expected to be 0",
				err, __func__, ii, rsvd);
			return err;
		}
	}
	return fastrpc_multidomain_ctx_cleanup(fl, ctxm->req, ctxm->ctx);
}

/* Manage multi-domain context in kernel (register / remove) */
static int fastrpc_multidomain_ctx_manage(struct fastrpc_user *fl,
	struct fastrpc_ioctl_mdctx_manage *ctxm)
{
	int err = 0;

	switch (ctxm->req) {
	case FASTRPC_MDCTX_SETUP:
		err = fastrpc_multidomain_ctx_setup(fl, ctxm);
		break;
	case FASTRPC_MDCTX_REMOVE:
		err = fastrpc_multidomain_ctx_remove(fl, ctxm);
		break;
	default:
		err = -EBADRQC;
		break;
	}
	return err;
}

int fastrpc_multimode_invoke(struct fastrpc_user *fl, char __user *argp)
{
	struct fastrpc_enhanced_invoke inv2 ;
	struct fastrpc_ioctl_multimode_invoke invoke;
	struct fastrpc_internal_control cp = {0};
	struct fastrpc_internal_dspsignal *fsig = NULL;
	struct fastrpc_internal_dspsignal_mc *fmcsig = NULL;
	struct fastrpc_internal_notif_rsp notif;
	struct fastrpc_internal_sessinfo sessinfo;
	struct fastrpc_ioctl_mdctx_manage ctxm = {0};
	u32 multisession;
	u64 *perf_kernel;
	int err = 0;
	bool legacy_domains = true;

	if (copy_from_user(&invoke, argp, sizeof(invoke)))
		return -EFAULT;
	switch (invoke.req) {
	case FASTRPC_INVOKE:
	case FASTRPC_INVOKE_ENHANCED:
		if (copy_from_user(&inv2,
				(void __user *)(uintptr_t)invoke.invparam,
				invoke.size))
			return -EFAULT;
		perf_kernel = (u64 *)(uintptr_t)inv2.perf_kernel;
		if (perf_kernel)
			fl->profile = true;
		err = fastrpc_internal_invoke(fl, USER_MSG, &inv2);
		break;
	case FASTRPC_INVOKE_CONTROL:
		if (copy_from_user(&cp,
				(void __user *)(uintptr_t)invoke.invparam,
				sizeof(cp)))
			return  -EFAULT;

		err = fastrpc_internal_control(fl, &cp);
		break;
	case FASTRPC_INVOKE_DSPSIGNAL:
		if (invoke.size > sizeof(*fsig))
			return -EINVAL;
		fsig = kzalloc(sizeof(*fsig), GFP_KERNEL);
		if (!fsig)
			return -ENOMEM;
		if (copy_from_user(fsig,
				(void __user *)(uintptr_t)invoke.invparam,
				invoke.size)) {
			kfree(fsig);
			return -EFAULT;
		}
		err = fastrpc_invoke_dspsignal(fl, fsig);
		kfree(fsig);
		break;
#if IS_ENABLED(CONFIG_HYBRID_FASTRPC_RSM)
	case FASTRPC_INVOKE_DSPSIGNAL_MC:
		if (invoke.size > sizeof(*fmcsig))
			return -EINVAL;
		fmcsig = kzalloc(sizeof(*fmcsig), GFP_KERNEL);
		if (!fmcsig)
			return -ENOMEM;
		if (copy_from_user(fmcsig,
				(void __user *)(uintptr_t)invoke.invparam,
				invoke.size)) {
			kfree(fmcsig);
			return -EFAULT;
		}
		err = fastrpc_invoke_dspsignal_mc(fl, fmcsig);
		kfree(fmcsig);
		break;
#endif
	case FASTRPC_INVOKE_NOTIF:
		if (invoke.dynamic_domains)
			legacy_domains = false;
		err = fastrpc_get_notif_response(&notif,
				(void *)invoke.invparam, fl, legacy_domains);
		break;
	case FASTRPC_INVOKE_MULTISESSION:
		if (copy_from_user(&multisession,
				(void __user *)(uintptr_t)invoke.invparam,
				sizeof(multisession)))
			return -EFAULT;
		if (!fl->multi_session_support)
			fl->sessionid = 1;
		break;
	case FASTRPC_INVOKE_SESSIONINFO:
		if (copy_from_user(&sessinfo,
				(void __user *)(uintptr_t)invoke.invparam,
				sizeof(struct fastrpc_internal_sessinfo)))
			return -EFAULT;
		err = fastrpc_set_session_info(fl, &sessinfo);
		break;
	case FASTRPC_INVOKE_MDCTX_MANAGE:
		if (copy_from_user(&ctxm, (void __user *)(uintptr_t)invoke.invparam,
			sizeof(ctxm)))
			return -EFAULT;
		err = fastrpc_multidomain_ctx_manage(fl, &ctxm);
		break;
	default:
		err = -ENOTTY;
		break;
	}

	return err;
}

static int fastrpc_req_munmap_dsp(struct fastrpc_user *fl,
					uintptr_t raddr, u64 size)
{
	struct fastrpc_invoke_args args[1] = { [0] = { 0 } };
	struct fastrpc_enhanced_invoke ioctl;
	struct fastrpc_munmap_req_msg req_msg;
	int err = 0;

	req_msg.pgid = fl->upid;
	req_msg.size = size;
	req_msg.vaddr = raddr;

	args[0].ptr = (u64)(uintptr_t)&req_msg;
	args[0].length = sizeof(req_msg);

	ioctl.inv.handle = FASTRPC_INIT_HANDLE;
	ioctl.inv.sc = FASTRPC_SCALARS(FASTRPC_RMID_INIT_MUNMAP, 1, 0);
	ioctl.inv.args = (__u64)args;

	/* GVM fastrpc can't send command to ROOT PD so we can't use zero pid here */
	err = fastrpc_internal_invoke(fl, KERNEL_MSG_WITH_NONZERO_PID, &ioctl);

	return err;
}

int fastrpc_req_munmap(struct fastrpc_user *fl, char __user *argp)
{
	struct fastrpc_buf *buf = NULL, *iter, *b;
	struct fastrpc_req_munmap req;
	struct fastrpc_map *map = NULL, *iterm, *m;
	int err = 0;

	if (fl->state != DSP_CREATE_COMPLETE) {
		RPC_ERR("trying to unmap buf before creating remote session\n");
		return -EHOSTDOWN;
	}
	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	RPC_DBG("raddr=0x%llx,size=0x%llx\n",
			req.vaddrout, req.size);
	spin_lock(&fl->lock);
	list_for_each_entry_safe(iter, b, &fl->mmaps, node) {
		if ((iter->raddr == req.vaddrout) && (iter->size == req.size)) {
			buf = iter;
			list_del(&buf->node);
			break;
		}
	}
	spin_unlock(&fl->lock);

	if (buf) {
		err = fastrpc_req_munmap_dsp(fl, buf->raddr, buf->size);
		if(!err) {
			fastrpc_buf_free(buf, false);
		} else {
			RPC_ERR("unmmap dsp error, raddr = 0x%lx\n",
					buf->raddr);
			spin_lock(&fl->lock);
			list_add_tail(&buf->node, &fl->mmaps);
			spin_unlock(&fl->lock);
		}
		return err;
	}

	spin_lock(&fl->lock);
	list_for_each_entry_safe(iterm, m, &fl->maps, node) {
		if (iterm->raddr == req.vaddrout) {
			map = iterm;
			break;
		}
	}
	spin_unlock(&fl->lock);
	if (!map) {
		RPC_ERR("buffer not in buf or map list\n");
		return -EINVAL;
	}

	err = fastrpc_req_munmap_dsp(fl, map->raddr, map->size);
	if (err) {
		RPC_ERR("unmmap dsp error, fd = %d, raddr = 0x%llx\n",
				map->fd, map->raddr);
	} else {
		mutex_lock(&fl->map_mutex);
		fastrpc_map_put(map);
		mutex_unlock(&fl->map_mutex);
	}

	return err;
}

int fastrpc_req_mmap(struct fastrpc_user *fl, char __user *argp)
{
	struct fastrpc_req_mmap req;
	struct fastrpc_mmap_req_msg req_msg;
	struct fastrpc_mmap_rsp_msg rsp_msg;
	struct fastrpc_phy_page pages;
	struct fastrpc_enhanced_invoke ioctl;
	struct fastrpc_invoke_args args[3] = { [0 ... 2] = { 0 } };
	struct fastrpc_buf *buf = NULL;
	struct fastrpc_map *map = NULL;
	int err;

	if (fl->state != DSP_CREATE_COMPLETE) {
		RPC_ERR("trying to map buf before creating remote session\n");
		return -EHOSTDOWN;
	}
	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	if (req.flags == ADSP_MMAP_REMOTE_HEAP_ADDR) {
		RPC_ERR("remote heap is not supported\n");
		return -EINVAL;
	}

	RPC_DBG("fd=%d,size=0x%llx,flags=0x%x\n",
			req.fd, req.size, req.flags);
	if (req.flags == ADSP_MMAP_ADD_PAGES && !fl->is_unsigned_pd) {
		if (req.vaddrin) {
			RPC_ERR("adding user allocated pages is only supported for unsigned PD\n");
			return -EINVAL;
		}
		err = fastrpc_buf_alloc(fl, req.size, USER_BUF, &buf);
		if (err) {
			RPC_ERR("failed to allocate buffer\n");
			return err;
		}

		req_msg.pgid = fl->upid;
		req_msg.flags = req.flags;
		req_msg.vaddr = req.vaddrin;
		req_msg.num = sizeof(pages);

		args[0].ptr = (u64)(uintptr_t)&req_msg;
		args[0].length = sizeof(req_msg);

		pages.addr = buf->da;
		pages.size = buf->size;

		args[1].ptr = (u64)(uintptr_t)&pages;
		args[1].length = sizeof(pages);

		args[2].ptr = (u64)(uintptr_t)&rsp_msg;
		args[2].length = sizeof(rsp_msg);

		ioctl.inv.handle = FASTRPC_INIT_HANDLE;
		ioctl.inv.sc = FASTRPC_SCALARS(FASTRPC_RMID_INIT_MMAP, 2, 1);
		ioctl.inv.args = (__u64)args;

		/* GVM fastrpc can't send command to ROOT PD so we can't use zero pid here */
		err = fastrpc_internal_invoke(fl, KERNEL_MSG_WITH_NONZERO_PID, &ioctl);
		if (err) {
			RPC_ERR("mmap error da=0x%llx,size=0x%llx\n",
					buf->da, buf->size);
			goto err_invoke;
		}

		/* update the buffer to be able to deallocate the memory on the DSP */
		buf->raddr = (uintptr_t)rsp_msg.vaddr;

		req.vaddrout = rsp_msg.vaddr;
		if (copy_to_user((void __user *)argp, &req, sizeof(req))) {
			err = -EFAULT;
			goto err_copy;
		}
		spin_lock(&fl->lock);
		list_add_tail(&buf->node, &fl->mmaps);
		spin_unlock(&fl->lock);
	} else {
		mutex_lock(&fl->map_mutex);
		err = fastrpc_map_create(fl, req.fd, req.vaddrin,
				req.size, 0, 0, &map, true);
		mutex_unlock(&fl->map_mutex);
		if (err) {
			RPC_ERR("failed to map buffer, fd = %d\n", req.fd);
			return err;
		}

		req_msg.pgid = fl->upid;
		req_msg.flags = req.flags;
		req_msg.vaddr = req.vaddrin;
		req_msg.num = sizeof(pages);

		args[0].ptr = (u64)(uintptr_t)&req_msg;
		args[0].length = sizeof(req_msg);

		pages.addr = map->da;
		pages.size = map->size;

		args[1].ptr = (u64)(uintptr_t)&pages;
		args[1].length = sizeof(pages);

		args[2].ptr = (u64)(uintptr_t)&rsp_msg;
		args[2].length = sizeof(rsp_msg);

		ioctl.inv.handle = FASTRPC_INIT_HANDLE;
		ioctl.inv.sc = FASTRPC_SCALARS(FASTRPC_RMID_INIT_MMAP, 2, 1);
		ioctl.inv.args = (__u64)args;
		/* GVM fastrpc can't send command to ROOT PD so we can't use zero pid here */
		err = fastrpc_internal_invoke(fl,
				KERNEL_MSG_WITH_NONZERO_PID, &ioctl);
		if (err) {
			RPC_ERR("mmap error (len 0x%08llx)\n", map->size);
			goto err_invoke;
		}

		/* update the buffer to be able to deallocate the memory on the DSP */
		map->raddr = (uintptr_t)rsp_msg.vaddr;

		/* let the client know the address to use */
		req.vaddrout = rsp_msg.vaddr;

		if (copy_to_user((void __user *)argp, &req, sizeof(req))) {
			err = -EFAULT;
			goto err_copy;
		}
	}

	RPC_DBG("map raddr = 0x%llx\n", rsp_msg.vaddr);
	return 0;
err_copy:
	if (req.flags != ADSP_MMAP_ADD_PAGES) {
		err = fastrpc_req_munmap_dsp(fl, map->raddr, map->size);
		if (err) {
			RPC_ERR("unmmap dsp error, fd = %d, raddr = 0x%llx\n",
					map->fd, map->raddr);
			map = NULL;
		}
	} else if(buf) {
		err = fastrpc_req_munmap_dsp(fl, buf->raddr, buf->size);
		if (err) {
			RPC_ERR("unmmap dsp error, raddr = 0x%lx\n",
					buf->raddr);
			spin_lock(&fl->lock);
			list_add_tail(&buf->node, &fl->mmaps);
			spin_unlock(&fl->lock);
			buf = NULL;
		}
	}
err_invoke:
	if (map) {
		mutex_lock(&fl->map_mutex);
		fastrpc_map_put(map);
		mutex_unlock(&fl->map_mutex);
	}
	if (buf)
		fastrpc_buf_free(buf, false);

	return err;
}

static int fastrpc_mem_map_to_dsp(struct fastrpc_user *fl, int fd, int offset,
					u32 flags, u32 va, u64 da,
					size_t size, uintptr_t *raddr)
{
	struct fastrpc_invoke_args args[4] = { [0 ... 3] = { 0 } };
	struct fastrpc_enhanced_invoke ioctl;
	struct fastrpc_mem_map_req_msg req_msg = { 0 };
	struct fastrpc_mmap_rsp_msg rsp_msg = { 0 };
	struct fastrpc_phy_page pages = { 0 };
	int err = 0;

	req_msg.pgid = fl->upid;
	req_msg.fd = fd;
	req_msg.offset = offset;
	req_msg.vaddrin = va;
	req_msg.flags = flags;
	req_msg.num = sizeof(pages);
	req_msg.data_len = 0;

	args[0].ptr = (u64)(uintptr_t)&req_msg;
	args[0].length = sizeof(req_msg);

	pages.addr = da;
	pages.size = size;

	args[1].ptr = (u64)(uintptr_t)&pages;
	args[1].length = sizeof(pages);

	args[2].ptr = (u64)(uintptr_t)&pages;
	args[2].length = 0;

	args[3].ptr = (u64)(uintptr_t)&rsp_msg;
	args[3].length = sizeof(rsp_msg);

	ioctl.inv.handle = FASTRPC_INIT_HANDLE;
	ioctl.inv.sc = FASTRPC_SCALARS(FASTRPC_RMID_INIT_MEM_MAP, 3, 1);
	ioctl.inv.args = (__u64)args;
	/* GVM fastrpc can't send command to ROOT PD so we can't use zero pid here */
	err = fastrpc_internal_invoke(fl, KERNEL_MSG_WITH_NONZERO_PID, &ioctl);
	if (err) {
		RPC_ERR("mem mmap error, fd %d, vaddr %x, size %lx, err 0x%x\n",
				fd, va, size, err);
		return err;
	}
	*raddr = rsp_msg.vaddr;

	return 0;
}

static int fastrpc_req_mem_unmap_dsp(struct fastrpc_user *fl,
					int fd, uintptr_t raddr)
{
	struct fastrpc_invoke_args args[1] = { [0] = { 0 } };
	struct fastrpc_enhanced_invoke ioctl;
	struct fastrpc_map *map = NULL, *iter, *m;
	struct fastrpc_mem_unmap_req_msg req_msg = { 0 };
	int err = 0;

	spin_lock(&fl->lock);
	list_for_each_entry_safe(iter, m, &fl->maps, node) {
		if ((fd < 0 || iter->fd == fd) && (iter->raddr == raddr)) {
			map = iter;
			break;
		}
	}

	spin_unlock(&fl->lock);

	if (!map) {
		RPC_ERR("map not in list\n");
		return -EINVAL;
	}

	req_msg.pgid = fl->upid;
	req_msg.len = map->len;
	req_msg.vaddrin = map->raddr;
	req_msg.fd = map->fd;

	args[0].ptr = (u64)(uintptr_t)&req_msg;
	args[0].length = sizeof(req_msg);
	ioctl.inv.handle = FASTRPC_INIT_HANDLE;
	ioctl.inv.sc = FASTRPC_SCALARS(FASTRPC_RMID_INIT_MEM_UNMAP, 1, 0);
	ioctl.inv.args = (__u64)args;

	/* GVM fastrpc can't send command to ROOT PD so we can't use zero pid here */
	err = fastrpc_internal_invoke(fl, KERNEL_MSG_WITH_NONZERO_PID, &ioctl);
	if (err) {
		RPC_ERR("Unmap on DSP failed for fd:%d, addr:0x%09llx\n",
				map->fd, map->raddr);
		return err;
	}
	mutex_lock(&fl->map_mutex);
	fastrpc_map_put(map);
	mutex_unlock(&fl->map_mutex);

	return 0;
}

int fastrpc_req_mem_unmap(struct fastrpc_user *fl, char __user *argp)
{
	struct fastrpc_mem_unmap req;

	if (fl->state != DSP_CREATE_COMPLETE) {
		RPC_ERR("trying to unmap buf before creating remote session\n");
		return -EHOSTDOWN;
	}
	
	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	RPC_DBG("fd=%d,raddr=0x%llx,size=0x%llx\n",
			req.fd, req.vaddr, req.length);

	return fastrpc_req_mem_unmap_dsp(fl, req.fd, req.vaddr);
}

int fastrpc_req_mem_map(struct fastrpc_user *fl, char __user *argp)
{
	struct fastrpc_mem_unmap req_unmap = { 0 };
	struct fastrpc_mem_map req = {0};
	struct fastrpc_map *map = NULL;
	int err;

	if (fl->state != DSP_CREATE_COMPLETE) {
		RPC_ERR("trying to map buf before creating remote session\n");
		return -EHOSTDOWN;
	}
	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	RPC_DBG("fd=%d,size=0x%llx,flags=0x%x,attrs=0x%x\n",
			req.fd, req.length, req.flags, req.attrs);

	mutex_lock(&fl->map_mutex);
	err = fastrpc_map_create(fl, req.fd, req.vaddrin,
			req.length, req.attrs, req.flags, &map, true);
	mutex_unlock(&fl->map_mutex);
	if (err) {
		RPC_ERR("failed to map buffer, fd = %d\n", req.fd);
		return err;
	}

	map->va = (void *)(uintptr_t)req.vaddrin;
	err = fastrpc_mem_map_to_dsp(fl, map->fd, req.offset,
			req.flags, req.vaddrin, map->da,
			map->size, (uintptr_t *)&req.vaddrout);
	if (err) {
		RPC_ERR("failed to map buffer on dsp, fd = %d\n",
					map->fd);
		mutex_lock(&fl->map_mutex);
		fastrpc_map_put(map);
		mutex_unlock(&fl->map_mutex);
		return err;
	}

	map->raddr = req.vaddrout;

	if (copy_to_user((void __user *)argp, &req, sizeof(req))) {
		req_unmap.vaddr = (uintptr_t)req.vaddrout;
		req_unmap.length = map->size;
		fastrpc_req_mem_unmap_dsp(fl, map->fd, map->raddr);
		return -EFAULT;
	}

	RPC_DBG("map raddr = 0x%llx\n", map->raddr);

	return 0;
}

static int fastrpc_device_open(struct inode *inode, struct file *filp)
{
	struct fastrpc_channel_ctx *cctx;
	struct fastrpc_device_node *fdevice;
	struct fastrpc_user *fl = NULL;
	unsigned long flags;
	int err;

	fdevice = miscdev_to_fdevice(filp->private_data);
	cctx = fdevice->cctx;

	if (atomic_read(&cctx->teardown))
		return -EPIPE;

	fl = kzalloc(sizeof(*fl), GFP_KERNEL);
	if (!fl)
		return -ENOMEM;

	fastrpc_channel_ctx_get(cctx);

	filp->private_data = fl;
	spin_lock_init(&fl->lock);
	mutex_init(&fl->remote_map_mutex);
	mutex_init(&fl->map_mutex);
	spin_lock_init(&fl->dspsignals_lock);
	mutex_init(&fl->signal_create_mutex);
	INIT_LIST_HEAD(&fl->pending);
	INIT_LIST_HEAD(&fl->interrupted);
	INIT_LIST_HEAD(&fl->maps);
	INIT_LIST_HEAD(&fl->mmaps);
	INIT_LIST_HEAD(&fl->user);
	INIT_LIST_HEAD(&fl->cached_bufs);
	INIT_LIST_HEAD(&fl->notif_queue);
	INIT_LIST_HEAD(&fl->mdctxs);
	init_waitqueue_head(&fl->proc_state_notif.notif_wait_queue);
	spin_lock_init(&fl->proc_state_notif.nqlock);
#if IS_ENABLED(CONFIG_HYBRID_FASTRPC_RSM)
	INIT_HLIST_HEAD(&fl->rsm_list_per_session);
	mutex_init(&fl->rsm_list_mutex);
#endif

	fl->cctx = cctx;
	fl->tgid = current->tgid;
	fl->tgid_frpc = get_unique_hlos_process_id(cctx);
	fl->state = DEFAULT_PROC_STATE;

	if (fl->tgid_frpc == -1) {
		RPC_ERR("too many fastrpc clients, max %u allowed\n", MAX_FRPC_TGID);
		err = -EUSERS;
		goto error;
	}
	RPC_DBG("HLOS pid %d, domain %d is mapped to unique sessions pid %d",
			fl->tgid, fl->cctx->domain_id, fl->tgid_frpc);
	fl->is_secure_dev = fdevice->secure;
	fl->sessionid = 0;
	fl->multi_session_support = false;
	fl->set_session_info = false;

	spin_lock_irqsave(&cctx->lock, flags);
	list_add_tail(&fl->user, &cctx->users);
	spin_unlock_irqrestore(&cctx->lock, flags);

	return 0;
error:
	mutex_destroy(&fl->remote_map_mutex);
	mutex_destroy(&fl->map_mutex);
	mutex_destroy(&fl->signal_create_mutex);
	kfree(fl);
	fastrpc_channel_ctx_put(cctx);

        return err;
}

static int virt_fastrpc_close(struct fastrpc_user *fl)
{
	struct fastrpc_channel_ctx *cctx = fl->cctx;
	struct fastrpc_common *gdriver = cctx->gdriver;
	struct virt_msg_hdr *vmsg, *rsp = NULL;
	struct virt_fastrpc_msg *msg;
	int err;

	if (fl->cid < 0) {
		RPC_ERR("channel id %d is invalid\n", fl->cid);
		return -EINVAL;
	}

	msg = virt_alloc_msg(fl, sizeof(*vmsg));
	if (!msg) {
		RPC_ERR("out of memory\n");
		return -ENOMEM;
	}

	vmsg = (struct virt_msg_hdr *)msg->txbuf;
	vmsg->pid = fl->tgid_frpc;
	vmsg->tid = current->pid;
	vmsg->cid = fl->cid;
	vmsg->cmd = VIRTIO_FASTRPC_CMD_CLOSE;
	vmsg->len = sizeof(*vmsg);
	vmsg->msgid = msg->msgid;
	vmsg->result = 0xffffffff;

	err = fastrpc_txbuf_send(fl, vmsg, sizeof(*vmsg));
	if (err)
		goto bail;

	wait_for_completion(&msg->work);

	rsp = msg->rxbuf;
	if (!rsp)
		goto bail;

	err = rsp->result;
bail:
	if (rsp)
		fastrpc_rxbuf_send(fl, rsp, gdriver->buf_size);

	virt_free_msg(fl, msg);

	return err;
}

static int fastrpc_device_release(struct inode *inode, struct file *file)
{
	struct fastrpc_user *fl = (struct fastrpc_user *)file->private_data;
	struct fastrpc_channel_ctx *cctx = fl->cctx;
	bool proc_init = false;
	unsigned long flags;
	int i;

	spin_lock_irqsave(&fl->lock, flags);
	if (fl->state == DSP_CREATE_COMPLETE)
		proc_init = true;
	fl->state = DSP_EXIT_START;
	spin_unlock_irqrestore(&fl->lock, flags);

	if (proc_init == true)
		virt_fastrpc_close(fl);

	spin_lock_irqsave(&fl->lock, flags);
	fl->state = DSP_EXIT_COMPLETE;
	spin_unlock_irqrestore(&fl->lock, flags);

	spin_lock_irqsave(&cctx->lock, flags);
	list_del(&fl->user);
	spin_unlock_irqrestore(&cctx->lock, flags);

	spin_lock_irqsave(&fl->proc_state_notif.nqlock, flags);
	atomic_add(1, &fl->proc_state_notif.notif_queue_count);
	wake_up_interruptible(&fl->proc_state_notif.notif_wait_queue);
	spin_unlock_irqrestore(&fl->proc_state_notif.nqlock, flags);

#if IS_ENABLED(CONFIG_HYBRID_FASTRPC_RSM)
	/*
	 * unregister all the jobs corresponds to the same UPID
	 * rsm_unregister_batch(fl->upid) is to be implemented by rsmfe
	 */
	fastrpc_rsm_list_per_session_free(fl);
	mutex_destroy(&fl->rsm_list_mutex);
#endif

	if (fl->tgid_frpc != -1)
		ida_free(&cctx->tgid_frpc_ida,
			fl->tgid_frpc - (cctx->domain_id * FASTRPC_UNIQUE_ID_CONST));
        /*
	 * fl->signal_groups is a static pointer array allocated during device open.
	 * The memory it points to is allocated during dspqueue_create, but during
	 * dspqueue_close, the memory is not freed but marked as unused. So, that needs
	 * to be freed here. And kfree will do sanity check on NULL pointer.
	 * */
	spin_lock_irqsave(&fl->dspsignals_lock, flags);
	for (i = 0; i < (FASTRPC_DSPSIGNAL_NUM_SIGNALS / FASTRPC_DSPSIGNAL_GROUP_SIZE); i++)
		kfree(fl->signal_groups[i]);
	spin_unlock_irqrestore(&fl->dspsignals_lock, flags);

	fastrpc_free_user(fl);
#ifdef CONFIG_DEBUG_FS
	debugfs_remove(fl->debugfs_file);
#endif
	mutex_destroy(&fl->signal_create_mutex);
	mutex_destroy(&fl->remote_map_mutex);
	mutex_destroy(&fl->map_mutex);
	kfree(fl);

	fastrpc_channel_update_invoke_cnt(cctx, false);

	fastrpc_channel_ctx_put(cctx);
	file->private_data = NULL;

	return 0;
}

static const struct file_operations fastrpc_fops = {
	.open = fastrpc_device_open,
	.release = fastrpc_device_release,
	.unlocked_ioctl = fastrpc_device_ioctl,
	.compat_ioctl = fastrpc_device_ioctl,
};

int fastrpc_device_register(struct device *dev, struct fastrpc_channel_ctx *cctx,
				bool is_secured, bool legacy, const char *domain)
{
	struct fastrpc_device_node *fdev;
	int err;

	fdev = devm_kzalloc(dev, sizeof(*fdev), GFP_KERNEL);
	if (!fdev)
		return -ENOMEM;

	fdev->secure = is_secured;
	fdev->cctx = cctx;
	cctx->dev = dev;
	fdev->miscdev.minor = MISC_DYNAMIC_MINOR;
	fdev->miscdev.fops = &fastrpc_fops;
	if (legacy)
		fdev->miscdev.name = devm_kasprintf(dev, GFP_KERNEL, "fastrpc-%s%s",
							domain, is_secured ? "-secure" : "");
	else
		fdev->miscdev.name = devm_kasprintf(dev, GFP_KERNEL, "fastrpc-%s",
							domain);
	if (!fdev->miscdev.name)
		return -ENOMEM;

	err = misc_register(&fdev->miscdev);
	if (!err) {
		/*
		 * Device nodes are created based on following criteria:
		 *   - For all channels, create a single device node with the
		 *     new domain name
		 *   - For channels that are marked as the legacy dsp of that type,
		 *      (for backward compatibility), also create the secure (and
		 *      non-secure, if applicable) device nodes using the legacy name
		 *      of the channel (eg: using CDSP name for the first NSP)
		 */
		if (legacy) {
			if (is_secured)
					cctx->legacy_secure_fdevice = fdev;
			else
					cctx->legacy_fdevice = fdev;
		} else {
			cctx->fdevice = fdev;
		}
	}

	return err;
}

