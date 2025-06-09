// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023-2025, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/delay.h>
#include <linux/sort.h>

#include "fastrpc_common.h"
#include "virtio_fastrpc_mem.h"
#include "virtio_fastrpc_queue.h"

#define DSPSIGNAL_TIMEOUT_NONE 0xffffffff
#define DSPSIGNAL_NUM_SIGNALS 1024

// Signal state and completions are stored in groups of DSPSIGNAL_GROUP_SIZE.
// Must be a power of two.
#define DSPSIGNAL_GROUP_SIZE			256

#define FASTRPC_STATIC_HANDLE_PROCESS_GROUP	1
#define FASTRPC_STATIC_HANDLE_DSP_UTILITIES	2
#define FASTRPC_STATIC_HANDLE_LISTENER		3
#define FASTRPC_STATIC_HANDLE_MAX		20

#define SESSION_ID_INDEX			30
#define SESSION_ID_MASK (1 << SESSION_ID_INDEX)
#define PROCESS_ID_MASK ((2^SESSION_ID_INDEX) - 1)

/* Convert the 19.2MHz clock count to micro-seconds */
#define CONVERT_CNT_TO_US(CNT) (CNT * 10ull / 192ull)

#define FASTRPC_POLL_TIME			4000
#define FASTRPC_POLL_TIME_MEM_UPDATE		500
#define FASTRPC_USER_EARLY_HINT_TIMEOUT		500
#define FASTRPC_EARLY_WAKEUP_POLL		0xabbccdde
#define FASTRPC_POLL_RESPONSE			0xdecaf
#define FASTRPC_RSP_VERSION2			2

#define MAX_PENDING_CTX_PER_SESSION		64
#define FASTRPC_CTX_JOB_TYPE_POS		4
#define FASTRPC_CTX_TABLE_IDX_POS		6
#define FASTRPC_CTX_JOBID_POS			16
#define FASTRPC_CTX_TABLE_IDX_MASK \
	((FASTRPC_CTX_MAX - 1) << FASTRPC_CTX_TABLE_IDX_POS)
#define FASTRPC_ASYNC_JOB_MASK			1

#define GET_TABLE_IDX_FROM_CTXID(ctxid) \
	((ctxid & FASTRPC_CTX_TABLE_IDX_MASK) >> FASTRPC_CTX_TABLE_IDX_POS)
#define FASTRPC_CTX_MAGIC			0xbeeddeed
/* Reserve few entries in context table for critical kernel and static RPC
 * calls to avoid user invocations from exhausting all entries.
 */
#define NUM_KERNEL_AND_STATIC_ONLY_CONTEXTS (70)

/* Max no. of persistent headers pre-allocated per process */
#define MAX_PERSISTENT_HEADERS    (25)

/* FastRPC remote subsystem state*/
enum fastrpc_remote_subsys_state {
	SUBSYSTEM_RESTARTING = 0,
	SUBSYSTEM_DOWN,
	SUBSYSTEM_UP,
};

struct virt_open_msg {
	struct virt_msg_hdr hdr;	/* virtio fastrpc message header */
	u32 domain;			/* DSP domain id */
	u32 pd;				/* DSP PD */
	u32 attrs;			/* DSP PD attributes */
	u32 upid;			/* unique pid sent to DSP */
} __packed;

static uint32_t kernel_capabilities[FASTRPC_MAX_ATTRIBUTES -
					FASTRPC_MAX_DSP_ATTRIBUTES] = {
	PERF_CAPABILITY_SUPPORT,
	/* PERF_LOGGING_V2_SUPPORT feature is supported, unsupported = 0 */
	KERNEL_ERROR_CODE_V1_SUPPORT,
	/* Fastrpc Driver error code changes present */
	0,
	/* Userspace allocation allowed for DSP memory request*/
	DSPSIGNAL_SUPPORT
	/* Lightweight driver-based signaling */
};

static int hfastrpc_internal_invoke(struct vfastrpc_file *vfl,
			uint32_t mode, uint32_t kernel,
			struct fastrpc_ioctl_invoke_async *inv);

static inline int64_t getnstimediff(struct timespec64 *start)
{
	int64_t ns;
	struct timespec64 ts, b;

	ktime_get_real_ts64(&ts);
	b = timespec64_sub(ts, *start);
	ns = timespec64_to_ns(&b);
	return ns;
}

static inline int64_t get_timestamp_in_ns(void)
{
	int64_t ns = 0;
	struct timespec64 ts;

	ktime_get_real_ts64(&ts);
	ns = timespec64_to_ns(&ts);
	return ns;
}

static inline uint64_t buf_page_start(uint64_t buf)
{
	uint64_t start = (uint64_t) buf & PAGE_MASK;
	return start;
}

static inline uint64_t buf_page_offset(uint64_t buf)
{
	uint64_t offset = (uint64_t) buf & (PAGE_SIZE - 1);
	return offset;
}

static inline uint64_t buf_num_pages(uint64_t buf, size_t len)
{
	uint64_t start = buf_page_start(buf) >> PAGE_SHIFT;
	uint64_t end = (((uint64_t) buf + len - 1) & PAGE_MASK) >> PAGE_SHIFT;
	uint64_t nPages = end - start + 1;
	return nPages;
}

static inline uint64_t buf_page_size(uint32_t size)
{
	uint64_t sz = (size + (PAGE_SIZE - 1)) & PAGE_MASK;

	return sz > PAGE_SIZE ? sz : PAGE_SIZE;
}

static inline void *uint64_to_ptr(uint64_t addr)
{
	void *ptr = (void *)((uintptr_t)addr);

	return ptr;
}

static inline uint64_t ptr_to_uint64(void *ptr)
{
	uint64_t addr = (uint64_t)((uintptr_t)ptr);

	return addr;
}

static int hfastrpc_set_process_info(struct vfastrpc_file *vfl, u32 domain)
{
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	int err = 0, buf_size = 0;
	char strpid[PID_SIZE];
	char cur_comm[TASK_COMM_LEN];

	memcpy(cur_comm, current->comm, TASK_COMM_LEN);
	cur_comm[TASK_COMM_LEN - 1] = '\0';
	fl->tgid = current->tgid;
	fl->tgid_frpc = get_unique_hlos_process_id(vfl);
	VERIFY(err, fl->tgid_frpc != -1);
	if (err) {
		ADSPRPC_ERR("too many fastrpc clients, max %u allowed\n", MAX_FRPC_TGID);
		return -EUSERS;
	}
	ADSPRPC_INFO("HLOS pid %d, domain %d is mapped to unique sessions pid %d\n",
			fl->tgid, domain, fl->tgid_frpc);

	/*
	 * Third-party apps don't have permission to open the fastrpc device, so
	 * it is opened on their behalf by DSP HAL. This is detected by
	 * comparing current PID with the one stored during device open.
	 */
	if (current->tgid != fl->tgid_open)
		fl->untrusted_process = true;
	scnprintf(strpid, PID_SIZE, "%d", current->pid);
	if (vfl->apps->debugfs_root) {
		buf_size = strlen(cur_comm) + strlen("_")
			+ strlen(strpid) + strlen("_")
			+ PID_SIZE + strlen("_")
			+ strlen(__TOSTR__(NUM_CHANNELS)) + 1;

		spin_lock(&fl->hlock);
		if (fl->debug_buf_alloced_attempted) {
			spin_unlock(&fl->hlock);
			return err;
		}
		fl->debug_buf_alloced_attempted = 1;
		spin_unlock(&fl->hlock);
		fl->debug_buf = kzalloc(buf_size, GFP_KERNEL);

		if (!fl->debug_buf) {
			err = -ENOMEM;
			spin_lock(&fl->hlock);
			fl->debug_buf_alloced_attempted = 0;
			spin_unlock(&fl->hlock);
			return err;
		}
		scnprintf(fl->debug_buf, buf_size, "%.10s%s%d%s%d%s%d",
			cur_comm, "_", current->pid, "_", fl->tgid_frpc, "_", domain);
		fl->debugfs_file = debugfs_create_file(fl->debug_buf, 0644,
			vfl->apps->debugfs_root, fl, vfl->apps->debugfs_fops);
		if (IS_ERR_OR_NULL(fl->debugfs_file)) {
			dev_warn(vfl->apps->dev, "%s: %s: failed to create debugfs file %s\n",
				cur_comm, __func__, fl->debug_buf);
			fl->debugfs_file = NULL;
			kfree(fl->debug_buf);
			fl->debug_buf = NULL;
			spin_lock(&fl->hlock);
			fl->debug_buf_alloced_attempted = 0;
			spin_unlock(&fl->hlock);
		}
	}
	return err;
}

static int hfastrpc_get_info(struct vfastrpc_file *vfl,
					uint32_t *info)
{
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	int err = 0;
	uint32_t domain;

	VERIFY(err, fl != NULL);
	if (err)
		goto bail;

	spin_lock(&fl->hlock);
	if (fl->set_session_info) {
		spin_unlock(&fl->hlock);
		ADSPRPC_ERR("Set session info invoked multiple times\n");
		err = -EBADR;
		goto bail;
	}
	// Set set_session_info to true
	fl->set_session_info = true;
	spin_unlock(&fl->hlock);

	domain = *info;
	VERIFY(err, domain < vfl->apps->num_channels);
	if (err)
		goto bail;

	err = hfastrpc_set_process_info(vfl, domain);
	if (err)
		goto bail;

	if (vfl->domain == -1) {
		struct vfastrpc_channel_ctx *chan = NULL;

		chan = &vfl->apps->channel[domain];
		/* Check to see if the device node is non-secure */
		if (fl->dev_minor == MINOR_NUM_DEV) {
			/*
			 * If an app is trying to offload to a secure remote
			 * channel by opening the non-secure device node, allow
			 * the access if the subsystem supports unsigned
			 * offload. Untrusted apps will be restricted from
			 * offloading to signed PD using DSP HAL.
			 */
			if (chan->secure == SECURE_CHANNEL
			&& !chan->unsigned_support) {
				dev_err(vfl->apps->dev,
				"cannot use domain %d with non-secure device\n", domain);
				err = -EACCES;
				goto bail;
			}
		}
		vfl->domain = domain;
		fl->cid = domain;
		fl->ssrcount = chan->ssrcount;
	}
	*info = 1;
bail:
	return err;
}

static int hfastrpc_channel_open(struct vfastrpc_file *vfl, uint32_t flags)
{
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	struct vfastrpc_channel_ctx *chan = NULL;
	int domain = -1, err = 0;

	VERIFY(err, vfl && vfl->domain >= 0 &&
			vfl->domain < vfl->apps->num_channels);
	if (err) {
		ADSPRPC_ERR("kernel session not initialized yet for %s\n",
			current->comm);
		err = -EBADR;
		return err;
	}
	domain = vfl->domain;

	err = fastrpc_wait_for_transport_interrupt(domain, flags);
	if (err)
		goto bail;

	err = verify_transport_device(domain, fl->tvm_remote_domain);
	if (err) {
		ADSPRPC_ERR("transport layer is not ready\n");
		err = -ECONNREFUSED;
		goto bail;
	}

	chan = &vfl->apps->channel[domain];
	mutex_lock(&chan->smd_mutex);
	if (chan->subsystemstate != SUBSYSTEM_UP) {
		err = -ECONNREFUSED;
		mutex_unlock(&chan->smd_mutex);
		goto bail;
	}
	fl->ssrcount = chan->ssrcount;

	chan->in_hib = 0;
	mutex_unlock(&chan->smd_mutex);

bail:
	return err;
}

static int hfastrpc_get_info_from_kernel(struct vfastrpc_file *vfl,
					struct fastrpc_ioctl_capability *cap)
{
	int err = 0;
	uint32_t domain = cap->domain, attribute_ID = cap->attribute_ID;
	struct fastrpc_dsp_capabilities *dsp_cap_ptr = NULL;

	VERIFY(err, domain < vfl->apps->num_channels);
	if (err) {
		err = -ECHRNG;
		goto bail;
	}

	/*
	 * Check if number of attribute IDs obtained from userspace
	 * is less than the number of attribute IDs supported by
	 * kernel
	 */
	if (attribute_ID >= FASTRPC_MAX_ATTRIBUTES) {
		err = -EOVERFLOW;
		goto bail;
	}

	dsp_cap_ptr = &vfl->apps->channel[domain].dsp_cap_kernel;

	if (attribute_ID >= FASTRPC_MAX_DSP_ATTRIBUTES) {
		/* Driver capability, pass it to user */
		memcpy(&cap->capability,
			&kernel_capabilities[attribute_ID -
			FASTRPC_MAX_DSP_ATTRIBUTES],
			sizeof(cap->capability));
	} else if (!dsp_cap_ptr->is_cached) {
		/*
		 * Information not on kernel, query device for information
		 * and cache on kernel
		 */
		err = virt_fastrpc_get_dsp_info(vfl,
				dsp_cap_ptr->dsp_attributes);
		if (err)
			goto bail;

		vfl->apps->channel[domain].unsigned_support =
			!!(dsp_cap_ptr->dsp_attributes[UNSIGNED_PD_SUPPORT]);

		memcpy(&cap->capability,
			&dsp_cap_ptr->dsp_attributes[attribute_ID],
			sizeof(cap->capability));

		dsp_cap_ptr->is_cached = 1;
	} else {
		/* Information on Kernel, pass it to user */
		memcpy(&cap->capability,
			&dsp_cap_ptr->dsp_attributes[attribute_ID],
			sizeof(cap->capability));
	}
bail:
	return err;
}

static int hfastrpc_control(struct vfastrpc_file *vfl,
					struct fastrpc_ioctl_control *cp)
{
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	int err = 0;
	unsigned long flags = 0;

	VERIFY(err, !IS_ERR_OR_NULL(fl) && !IS_ERR_OR_NULL(fl->apps));
	if (err)
		goto bail;
	VERIFY(err, !IS_ERR_OR_NULL(cp));
	if (err)
		goto bail;

	switch (cp->req) {
	case FASTRPC_CONTROL_LATENCY:
		err = 0;
		break;
	case FASTRPC_CONTROL_KALLOC:
		cp->kalloc.kalloc_support = 1;
		break;
	case FASTRPC_CONTROL_NOTIF_WAKE:
		fl->exit_notif = true;
		spin_lock_irqsave(&fl->proc_state_notif.nqlock, flags);
		atomic_add(1, &fl->proc_state_notif.notif_queue_count);
		wake_up_interruptible(&fl->proc_state_notif.notif_wait_queue);
		spin_unlock_irqrestore(&fl->proc_state_notif.nqlock, flags);
		break;
	default:
		err = -ENOTTY;
		break;
	}
bail:
	return err;
}

static int virt_fastrpc_open(struct vfastrpc_file *vfl,
		struct fastrpc_ioctl_init_attrs *uproc)
{
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	struct vfastrpc_apps *me = vfl->apps;
	struct virt_open_msg *vmsg, *rsp = NULL;
	struct virt_fastrpc_msg *msg;
	int err;

	msg = virt_alloc_msg(vfl, sizeof(*vmsg));
	if (!msg) {
		dev_err(me->dev, "%s: no memory\n", __func__);
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
	vmsg->domain = vfl->domain;
	vmsg->pd = fl->pd;
	vmsg->attrs = uproc->attrs;

	err = vfastrpc_txbuf_send(vfl, vmsg, sizeof(*vmsg));
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
		dev_err(me->dev, "open: channel id %d is invalid\n", rsp->hdr.cid);
		err = -EINVAL;
		goto bail;
	}
	fl->cid = rsp->hdr.cid;
	vfl->upid = rsp->upid;
	ADSP_LOG("host pid=%x\n", rsp->upid);
bail:
	if (rsp)
		vfastrpc_rxbuf_send(vfl, rsp, me->buf_size);
	virt_free_msg(vfl, msg);

	return err;
}

static int hfastrpc_init_process(struct vfastrpc_file *vfl,
				struct fastrpc_ioctl_init_attrs *uproc)
{
	int err = 0;
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	struct fastrpc_file *fl_curr = NULL;
	struct vfastrpc_apps *me = vfl->apps;
	struct fastrpc_ioctl_init *init = &uproc->init;
	int domain = vfl->domain;
	struct hlist_node *n = NULL;
	unsigned long irq_flags = 0;
	struct vfastrpc_channel_ctx *chan = NULL;

	if (domain < 0 || domain >= vfl->apps->num_channels) {
		err = -ECHRNG;
		goto bail;
	}

	chan = &vfl->apps->channel[domain];

	if (chan->unsigned_support && fl->dev_minor == MINOR_NUM_DEV) {
		/*
		 * Make sure third party applications
		 * can spawn only unsigned PD when
		 * channel configured as secure.
		 */
		if (chan->secure && !(uproc->attrs & FASTRPC_MODE_UNSIGNED_MODULE)) {
			err = -ECONNREFUSED;
			goto bail;
		}
	} else if (!(chan->unsigned_support) && (uproc->attrs & FASTRPC_MODE_UNSIGNED_MODULE)) {
		err = -ECONNREFUSED;
		goto bail;
	}

	err = hfastrpc_channel_open(vfl, init->flags);
	if (err)
		goto bail;

	fl->proc_flags = init->flags;
	switch (init->flags) {
	case FASTRPC_INIT_CREATE:
		fl->pd = DYNAMIC_PD;
		/* Untrusted apps are not allowed to offload to signedPD on DSP. */
		if (fl->untrusted_process) {
			VERIFY(err, uproc->attrs & FASTRPC_MODE_UNSIGNED_MODULE);
			if (err) {
				err = -ECONNREFUSED;
				dev_err(vfl->apps->dev,
					"untrusted app trying to offload to signed remote process\n");
				goto bail;
			}
		}
		if (uproc->attrs & FASTRPC_MODE_UNSIGNED_MODULE)
			fl->is_unsigned_pd = true;
		/* Validate that any existing sessions of process are of same pd type */
		spin_lock_irqsave(&me->hlock, irq_flags);
		hlist_for_each_entry_safe(fl_curr, n, &me->drivers, hn) {
			if ((fl != fl_curr) && (fl->tgid == fl_curr->tgid) &&
					(fl->cid == fl_curr->cid) &&
					fl->is_unsigned_pd != fl_curr->is_unsigned_pd) {
					err = -ECONNREFUSED;
					break;
			}
		}
		spin_unlock_irqrestore(&me->hlock, irq_flags);
		if (err) {
			ADSPRPC_ERR("existing sessions PD type are not aligned\n");
			goto bail;
		}
		vfl->procattrs = uproc->attrs;
		break;
	case FASTRPC_INIT_CREATE_STATIC:
	case FASTRPC_INIT_ATTACH:
	case FASTRPC_INIT_ATTACH_SENSORS:
		err = -ECONNREFUSED;
		goto bail;
	default:
		return -ENOTTY;
	}
	err = virt_fastrpc_open(vfl, uproc);
	if (err)
		goto bail;
	fl->dsp_proc_init = 1;
bail:
	return err;
}

static int hfastrpc_mmap_on_dsp(struct vfastrpc_file *vfl, uint32_t flags,
					uintptr_t va, uint64_t da,
					size_t size, int refs, uintptr_t *raddr)
{
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	struct fastrpc_ioctl_invoke_async ioctl;
	struct smq_phy_page page;
	int num = 1;
	remote_arg_t ra[3];
	int err = 0;
	struct {
		int pid;
		uint32_t flags;
		uintptr_t vaddrin;
		int num;
	} inargs;
	struct {
		uintptr_t vaddrout;
	} routargs;

	if (!fl) {
		err = -EBADF;
		goto bail;
	}
	inargs.pid = vfl->upid;
	inargs.vaddrin = (uintptr_t)va;
	inargs.flags = flags;
	inargs.num = fl->apps->compat ? num * sizeof(page) : num;
	ra[0].buf.pv = (void *)&inargs;
	ra[0].buf.len = sizeof(inargs);
	page.addr = da;
	page.size = size;
	ra[1].buf.pv = (void *)&page;
	ra[1].buf.len = num * sizeof(page);

	routargs.vaddrout = 0;
	ra[2].buf.pv = (void *)&routargs;
	ra[2].buf.len = sizeof(routargs);

	ioctl.inv.handle = FASTRPC_STATIC_HANDLE_PROCESS_GROUP;
	if (fl->apps->compat)
		ioctl.inv.sc = REMOTE_SCALARS_MAKE(4, 2, 1);
	else
		ioctl.inv.sc = REMOTE_SCALARS_MAKE(2, 2, 1);
	ioctl.inv.pra = ra;
	ioctl.fds = NULL;
	ioctl.attrs = NULL;
	ioctl.crc = NULL;
	ioctl.perf_kernel = NULL;
	ioctl.perf_dsp = NULL;
	ioctl.job = NULL;
	VERIFY(err, 0 == (err = hfastrpc_internal_invoke(vfl,
		FASTRPC_MODE_PARALLEL, KERNEL_MSG_WITH_NONZERO_PID, &ioctl)));
	*raddr = (uintptr_t)routargs.vaddrout;
	if (err)
		goto bail;
bail:
	return err;
}

static int hfastrpc_munmap_on_dsp(struct vfastrpc_file *vfl,
		uintptr_t raddr, size_t size)
{
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	struct fastrpc_ioctl_invoke_async ioctl;
	remote_arg_t ra[1] = {};
	int err = 0;
	struct {
		int pid;
		uintptr_t vaddrout;
		size_t size;
	} inargs;

	inargs.pid = vfl->upid;
	inargs.size = size;
	inargs.vaddrout = raddr;
	ra[0].buf.pv = (void *)&inargs;
	ra[0].buf.len = sizeof(inargs);

	ioctl.inv.handle = FASTRPC_STATIC_HANDLE_PROCESS_GROUP;
	if (fl->apps->compat)
		ioctl.inv.sc = REMOTE_SCALARS_MAKE(5, 1, 0);
	else
		ioctl.inv.sc = REMOTE_SCALARS_MAKE(3, 1, 0);
	ioctl.inv.pra = ra;
	ioctl.fds = NULL;
	ioctl.attrs = NULL;
	ioctl.crc = NULL;
	ioctl.perf_kernel = NULL;
	ioctl.perf_dsp = NULL;
	ioctl.job = NULL;
	VERIFY(err, 0 == (err = hfastrpc_internal_invoke(vfl,
		FASTRPC_MODE_PARALLEL, KERNEL_MSG_WITH_NONZERO_PID, &ioctl)));
	if (err)
		goto bail;
bail:
	return err;
}

static int hfastrpc_mmap(struct vfastrpc_file *vfl,
				 struct fastrpc_ioctl_mmap *ud)
{
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	struct vfastrpc_apps *me = vfl->apps;
	struct vfastrpc_mmap *map = NULL;
	struct vfastrpc_buf *rbuf = NULL;
	unsigned long dma_attr = 0;
	uintptr_t raddr = 0;
	int err = 0;

	ADSP_LOG("fd=%d,size=%ld,flags=%x\n",
			ud->fd, ud->size, ud->flags);
	VERIFY(err, fl->dsp_proc_init == 1);
	if (err) {
		dev_err(me->dev, "%s: user application %s trying to map without initialization\n",
			__func__, current->comm);
		err = -EBADR;
		return err;
	}
	mutex_lock(&fl->internal_map_mutex);
	if (ud->flags == ADSP_MMAP_ADD_PAGES) {
		if (ud->vaddrin) {
			err = -EINVAL;
			dev_err(me->dev, "%s: %s: ERROR: adding user allocated pages is not supported\n",
					current->comm, __func__);
			goto bail;
		}
		dma_attr = DMA_ATTR_NO_KERNEL_MAPPING;
		err = hfastrpc_buf_alloc(vfl, ud->size, dma_attr, ud->flags,
						VFASTRPC_BUF_TYPE_USERHEAP,
						PAGE_KERNEL, &rbuf);
		if (err)
			goto bail;

		err = hfastrpc_mmap_on_dsp(vfl, ud->flags, 0,
				rbuf->da, rbuf->size, 0, &raddr);
		if (err)
			goto bail;

		rbuf->raddr = raddr;
	} else {
		uintptr_t va_to_dsp;

		mutex_lock(&fl->map_mutex);
		VERIFY(err, !hfastrpc_mmap_create(vfl, ud->fd, 0,
				(uintptr_t)ud->vaddrin, ud->size,
				 ud->flags, &map));
		mutex_unlock(&fl->map_mutex);
		if (err)
			goto bail;

		if (ud->flags == ADSP_MMAP_HEAP_ADDR ||
			ud->flags == ADSP_MMAP_REMOTE_HEAP_ADDR)
			va_to_dsp = 0;
		else
			va_to_dsp = (uintptr_t)map->va;

		VERIFY(err, 0 == (err = hfastrpc_mmap_on_dsp(vfl, ud->flags,
			va_to_dsp, map->da, map->size, map->refs, &raddr)));
		if (err)
			goto bail;
		map->raddr = raddr;
	}
	ud->vaddrout = raddr;
	ADSP_LOG("map vaddr=%lx\n", raddr);
 bail:
	if (err) {
		if (map) {
			mutex_lock(&fl->map_mutex);
			hfastrpc_mmap_free(vfl, map, 0);
			mutex_unlock(&fl->map_mutex);
		}
		if (!IS_ERR_OR_NULL(rbuf))
			hfastrpc_buf_free(rbuf, 0);
	}
	mutex_unlock(&fl->internal_map_mutex);
	return err;
}

static int hfastrpc_munmap(struct vfastrpc_file *vfl,
				struct fastrpc_ioctl_munmap *ud)
{
	int err = 0;
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	struct vfastrpc_mmap *map = NULL;
	struct vfastrpc_buf *rbuf = NULL, *free = NULL;
	struct hlist_node *n;

	ADSP_LOG("vaddr=%lx,size=%ld\n", ud->vaddrout, ud->size);
	VERIFY(err, fl->dsp_proc_init == 1);
	if (err) {
		ADSPRPC_ERR(
			"user application %s trying to unmap without initialization\n",
			current->comm);
		err = -EHOSTDOWN;
		return err;
	}
	mutex_lock(&fl->internal_map_mutex);

	spin_lock(&fl->hlock);
	hlist_for_each_entry_safe(rbuf, n, &fl->remote_bufs, hn_rem) {
		if (rbuf->raddr && ((rbuf->flags == ADSP_MMAP_ADD_PAGES) ||
				    (rbuf->flags == ADSP_MMAP_ADD_PAGES_LLC))) {
			if ((rbuf->raddr == ud->vaddrout) &&
				(rbuf->size == ud->size)) {
				free = rbuf;
				break;
			}
		}
	}
	spin_unlock(&fl->hlock);

	if (free) {
		VERIFY(err, !(err = hfastrpc_munmap_on_dsp(vfl, free->raddr,
							free->size)));
		if (err)
			goto bail;
		hfastrpc_buf_free(rbuf, 0);
		mutex_unlock(&fl->internal_map_mutex);
		return err;
	}

	mutex_lock(&fl->map_mutex);
	VERIFY(err, !(err = vfastrpc_mmap_remove(vfl, -1, ud->vaddrout,
		ud->size, &map)));
	mutex_unlock(&fl->map_mutex);
	if (err)
		goto bail;
	VERIFY(err, map != NULL);
	if (err) {
		err = -EINVAL;
		goto bail;
	}
	if (!map->is_persistent) {
		VERIFY(err, !(err = hfastrpc_munmap_on_dsp(vfl, map->raddr,
								map->size)));
		if (err)
			goto bail;
	}
	mutex_lock(&fl->map_mutex);
	hfastrpc_mmap_free(vfl, map, 0);
	mutex_unlock(&fl->map_mutex);
bail:
	if (err && map) {
		mutex_lock(&fl->map_mutex);
		vfastrpc_mmap_add(vfl, map);
		mutex_unlock(&fl->map_mutex);
	}
	mutex_unlock(&fl->internal_map_mutex);
	return err;
}

static int hfastrpc_munmap_fd(struct vfastrpc_file *vfl,
				struct fastrpc_ioctl_munmap_fd *ud)
{
	int err = 0;
	struct fastrpc_file *fl = to_fastrpc_file(vfl);

	ADSP_LOG("fd=%d,size=%ld,vaddr=%lx\n", ud->fd, ud->len, ud->va);
	VERIFY(err, (fl && ud));
	if (err) {
		err = -EINVAL;
		return err;
	}
	VERIFY(err, fl->dsp_proc_init == 1);
	if (err) {
		ADSPRPC_ERR(
			"user application %s trying to unmap without initialization\n",
			current->comm);
		err = -EHOSTDOWN;
		return err;
	}
	mutex_lock(&fl->internal_map_mutex);
	mutex_lock(&fl->map_mutex);
	err = hfastrpc_mmap_remove_fd(vfl, ud->fd);
	mutex_unlock(&fl->map_mutex);
	mutex_unlock(&fl->internal_map_mutex);
	return err;
}

static int hfastrpc_mem_map_to_dsp(struct vfastrpc_file *vfl, int fd, int offset,
				uint32_t flags, uintptr_t va, uint64_t da,
				size_t size, uintptr_t *raddr)
{
	struct fastrpc_ioctl_invoke_async ioctl;
	struct smq_phy_page page;
	remote_arg_t ra[4];
	int err = 0;
	struct {
		int pid;
		int fd;
		int offset;
		uint32_t flags;
		uint64_t vaddrin;
		int num;
		int data_len;
	} inargs;
	struct {
		uint64_t vaddrout;
	} routargs;

	inargs.pid = vfl->upid;
	inargs.fd = fd;
	inargs.offset = offset;
	inargs.vaddrin = (uintptr_t)va;
	inargs.flags = flags;
	inargs.num = sizeof(page);
	inargs.data_len = 0;
	ra[0].buf.pv = (void *)&inargs;
	ra[0].buf.len = sizeof(inargs);
	page.addr = da;
	page.size = size;
	ra[1].buf.pv = (void *)&page;
	ra[1].buf.len = sizeof(page);
	ra[2].buf.pv = (void *)&page;
	ra[2].buf.len = 0;
	routargs.vaddrout = 0;
	ra[3].buf.pv = (void *)&routargs;
	ra[3].buf.len = sizeof(routargs);

	ioctl.inv.handle = FASTRPC_STATIC_HANDLE_PROCESS_GROUP;
	ioctl.inv.sc = REMOTE_SCALARS_MAKE(10, 3, 1);
	ioctl.inv.pra = ra;
	ioctl.fds = NULL;
	ioctl.attrs = NULL;
	ioctl.crc = NULL;
	ioctl.perf_kernel = NULL;
	ioctl.perf_dsp = NULL;
	ioctl.job = NULL;
	VERIFY(err, 0 == (err = hfastrpc_internal_invoke(vfl,
		FASTRPC_MODE_PARALLEL, KERNEL_MSG_WITH_NONZERO_PID, &ioctl)));
	if (err)
		goto bail;
	if (raddr)
		*raddr = (uintptr_t)routargs.vaddrout;
bail:
	return err;
}

static int hfastrpc_mem_unmap_to_dsp(struct vfastrpc_file *vfl, int fd,
				uint32_t flags,	uintptr_t va,
				size_t size)
{
	struct fastrpc_ioctl_invoke_async ioctl;
	remote_arg_t ra[1];
	int err = 0;
	struct {
		int pid;
		int fd;
		uint64_t vaddrin;
		uint64_t len;
	} inargs;

	inargs.pid = vfl->upid;
	inargs.fd = fd;
	inargs.vaddrin = (uint64_t)va;
	inargs.len = (uint64_t)size;
	ra[0].buf.pv = (void *)&inargs;
	ra[0].buf.len = sizeof(inargs);

	ioctl.inv.handle = FASTRPC_STATIC_HANDLE_PROCESS_GROUP;
	ioctl.inv.sc = REMOTE_SCALARS_MAKE(11, 1, 0);
	ioctl.inv.pra = ra;
	ioctl.fds = NULL;
	ioctl.attrs = NULL;
	ioctl.crc = NULL;
	ioctl.perf_kernel = NULL;
	ioctl.perf_dsp = NULL;
	ioctl.job = NULL;
	VERIFY(err, 0 == (err = hfastrpc_internal_invoke(vfl,
		FASTRPC_MODE_PARALLEL, KERNEL_MSG_WITH_NONZERO_PID, &ioctl)));
	if (err)
		goto bail;
bail:
	return err;
}

static int hfastrpc_mem_map(struct vfastrpc_file *vfl,
				struct fastrpc_ioctl_mem_map *ud)
{
	int err = 0;
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	struct vfastrpc_mmap *map = NULL;

	ADSP_LOG("fd=%d,size=%ld,flags=%x,attr=%x\n",
			ud->m.fd, ud->m.length, ud->m.flags, ud->m.attrs);
	VERIFY(err, fl->dsp_proc_init == 1);
	if (err) {
		pr_err("adsprpc: ERROR: %s: user application %s trying to map without initialization\n",
			__func__, current->comm);
		err = EBADR;
		goto bail;
	}

	/* create SMMU mapping */
	mutex_lock(&fl->internal_map_mutex);
	mutex_lock(&fl->map_mutex);
	VERIFY(err, !(err = hfastrpc_mmap_create(vfl, ud->m.fd, ud->m.attrs,
						ud->m.vaddrin, ud->m.length,
						ud->m.flags, &map)));

	mutex_unlock(&fl->map_mutex);
	if (err)
		goto bail;

	if (map->raddr) {
		err = -EEXIST;
		goto bail;
	}

	/* create DSP mapping */
	VERIFY(err, !(err = hfastrpc_mem_map_to_dsp(vfl, ud->m.fd, ud->m.offset,
		ud->m.flags, map->va, map->da, map->size, &map->raddr)));
	if (err)
		goto bail;
	ud->m.vaddrout = map->raddr;
	ADSP_LOG("map vaddr=%lx\n", map->raddr);
bail:
	if (err) {
		ADSPRPC_ERR("failed to map fd %d, len 0x%zx, flags %d, map %pK, err %d\n",
			ud->m.fd, ud->m.length, ud->m.flags, map, err);
		if (map) {
			mutex_lock(&fl->map_mutex);
			hfastrpc_mmap_free(vfl, map, 0);
			mutex_unlock(&fl->map_mutex);
		}
	}
	mutex_unlock(&fl->internal_map_mutex);
	return err;
}

static int hfastrpc_mem_unmap(struct vfastrpc_file *vfl,
				struct fastrpc_ioctl_mem_unmap *ud)
{
	int err = 0;
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	struct vfastrpc_mmap *map = NULL;
	size_t map_size = 0;

	ADSP_LOG("fd=%d,size=%ld,vaddr=%x\n",
			ud->um.fd, ud->um.length, ud->um.vaddr);
	VERIFY(err, fl->dsp_proc_init == 1);
	if (err) {
		pr_err("adsprpc: ERROR: %s: user application %s trying to map without initialization\n",
			__func__, current->comm);
		err = EBADR;
		goto bail;
	}

	mutex_lock(&fl->internal_map_mutex);
	mutex_lock(&fl->map_mutex);
	VERIFY(err, !(err = vfastrpc_mmap_remove(vfl, ud->um.fd,
			(uintptr_t)ud->um.vaddr, ud->um.length, &map)));
	mutex_unlock(&fl->map_mutex);
	if (err)
		goto bail;

	VERIFY(err, map->flags == FASTRPC_MAP_FD ||
		map->flags == FASTRPC_MAP_FD_DELAYED ||
		map->flags == FASTRPC_MAP_STATIC);
	if (err) {
		err = -EBADMSG;
		goto bail;
	}
	map_size = map->size;
	/* remove mapping on DSP */
	VERIFY(err, !(err = hfastrpc_mem_unmap_to_dsp(vfl, map->fd, map->flags,
						map->raddr, map->size)));
	if (err)
		goto bail;

	/* remove SMMU mapping */
	mutex_lock(&fl->map_mutex);
	hfastrpc_mmap_free(vfl, map, 0);
	mutex_unlock(&fl->map_mutex);
	map = NULL;
bail:
	if (err) {
		ADSPRPC_ERR(
			"failed to unmap fd %d addr 0x%llx length %zu map size %zu err 0x%x\n",
			ud->um.fd, ud->um.vaddr, ud->um.length, map_size, err);
		/* Add back to map list in case of error to unmap on DSP */
		if (map) {
			mutex_lock(&fl->map_mutex);
			if (map->attr & FASTRPC_ATTR_KEEP_MAP)
				map->refs++;
			vfastrpc_mmap_add(vfl, map);
			mutex_unlock(&fl->map_mutex);
		}
	}
	mutex_unlock(&fl->internal_map_mutex);
	return err;
}

static int hfastrpc_setmode(struct vfastrpc_file *vfl,
					unsigned long ioctl_param)
{
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	int err = 0;

	switch ((uint32_t)ioctl_param) {
	case FASTRPC_MODE_PARALLEL:
	case FASTRPC_MODE_SERIAL:
		fl->mode = (uint32_t)ioctl_param;
		break;
	case FASTRPC_MODE_SESSION:
		if (!fl->multi_session_support)
			fl->sessionid = 1;
		break;
	case FASTRPC_MODE_PROFILE:
		fl->profile = (uint32_t)ioctl_param;
		break;
	default:
		err = -ENOTTY;
		break;
	}
	return err;
}

static void hfastrpc_update_invoke_count(uint32_t handle, uint64_t *perf_counter,
					struct timespec64 *invoket)
{
	/* update invoke count for dynamic handles */
	if (handle != FASTRPC_STATIC_HANDLE_LISTENER) {
		uint64_t *count = GET_COUNTER(perf_counter, PERF_INVOKE);

		if (count)
			*count += getnstimediff(invoket);
	}
	if (handle > FASTRPC_STATIC_HANDLE_MAX) {
		uint64_t *count = GET_COUNTER(perf_counter, PERF_COUNT);

		if (count)
			*count += 1;
	}
}

#define CMP(aa, bb) ((aa) == (bb) ? 0 : (aa) < (bb) ? -1 : 1)

static int overlap_ptr_cmp(const void *a, const void *b)
{
	struct overlap *pa = *((struct overlap **)a);
	struct overlap *pb = *((struct overlap **)b);
	/* sort with lowest starting buffer first */
	int st = CMP(pa->start, pb->start);
	/* sort with highest ending buffer first */
	int ed = CMP(pb->end, pa->end);
	return st == 0 ? ed : st;
}

static int context_build_overlap(struct vfastrpc_invoke_ctx *ctx)
{
	int i, err = 0;
	remote_arg_t *lpra = ctx->lpra;
	int inbufs = REMOTE_SCALARS_INBUFS(ctx->sc);
	int outbufs = REMOTE_SCALARS_OUTBUFS(ctx->sc);
	int nbufs = inbufs + outbufs;
	struct overlap max;

	for (i = 0; i < nbufs; ++i) {
		ctx->overs[i].start = (uintptr_t)lpra[i].buf.pv;
		ctx->overs[i].end = ctx->overs[i].start + lpra[i].buf.len;
		if (lpra[i].buf.len) {
			VERIFY(err, ctx->overs[i].end > ctx->overs[i].start);
			if (err) {
				err = -EFAULT;
				ADSPRPC_ERR(
					"Invalid address 0x%lx and size %zu\n",
					(uintptr_t)lpra[i].buf.pv,
					lpra[i].buf.len);
				goto bail;
			}
		}
		ctx->overs[i].raix = i;
		ctx->overps[i] = &ctx->overs[i];
	}
	sort(ctx->overps, nbufs, sizeof(*ctx->overps), overlap_ptr_cmp, NULL);
	max.start = 0;
	max.end = 0;
	for (i = 0; i < nbufs; ++i) {
		if (ctx->overps[i]->start < max.end) {
			ctx->overps[i]->mstart = max.end;
			ctx->overps[i]->mend = ctx->overps[i]->end;
			ctx->overps[i]->offset = max.end -
				ctx->overps[i]->start;
			if (ctx->overps[i]->end > max.end) {
				max.end = ctx->overps[i]->end;
			} else {
				if ((max.raix < inbufs &&
					ctx->overps[i]->raix + 1 > inbufs) ||
					(ctx->overps[i]->raix < inbufs &&
					max.raix + 1 > inbufs))
					ctx->overps[i]->do_cmo = 1;
				ctx->overps[i]->mend = 0;
				ctx->overps[i]->mstart = 0;
			}
		} else  {
			ctx->overps[i]->mend = ctx->overps[i]->end;
			ctx->overps[i]->mstart = ctx->overps[i]->start;
			ctx->overps[i]->offset = 0;
			max = *ctx->overps[i];
		}
	}
bail:
	return err;
}

static void context_free(struct vfastrpc_invoke_ctx *ctx);

static int context_alloc(struct vfastrpc_file *vfl, uint32_t kernel,
			 struct fastrpc_ioctl_invoke_async *invokefd,
			 struct vfastrpc_invoke_ctx **po)
{
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	struct fastrpc_apps *me = fl->apps;
	int err = 0, bufs, ii, size = 0, domain = vfl->domain;
	struct vfastrpc_invoke_ctx *ctx = NULL;
	struct fastrpc_ctx_lst *clst = &fl->clst;
	struct fastrpc_ioctl_invoke *invoke = &invokefd->inv;
	struct vfastrpc_channel_ctx *chan = NULL;
	unsigned long irq_flags = 0;
	uint32_t kernel_msg = ((kernel == COMPAT_MSG) ? USER_MSG : kernel);

	spin_lock(&fl->hlock);
	if (fl->clst.num_active_ctxs > MAX_PENDING_CTX_PER_SESSION &&
		!(kernel_msg || invoke->handle < FASTRPC_STATIC_HANDLE_MAX)) {
		err = -EDQUOT;
		spin_unlock(&fl->hlock);
		goto bail;
	}
	spin_unlock(&fl->hlock);
	bufs = REMOTE_SCALARS_LENGTH(invoke->sc);
	size = bufs * sizeof(*ctx->lpra) + bufs * sizeof(*ctx->maps) +
		sizeof(*ctx->fds) * (bufs) +
		sizeof(*ctx->attrs) * (bufs) +
		sizeof(*ctx->overs) * (bufs) +
		sizeof(*ctx->overps) * (bufs);

	VERIFY(err, NULL != (ctx = kzalloc(sizeof(*ctx) + size, GFP_KERNEL)));
	if (err) {
		err = -ENOMEM;
		goto bail;
	}

	INIT_HLIST_NODE(&ctx->hn);
	INIT_HLIST_NODE(&ctx->asyncn);
	hlist_add_fake(&ctx->hn);
	hlist_add_fake(&ctx->asyncn);
	ctx->vfl = vfl;
	ctx->maps = (struct vfastrpc_mmap **)(&ctx[1]);
	ctx->lpra = (remote_arg_t *)(&ctx->maps[bufs]);
	ctx->fds = (int *)(&ctx->lpra[bufs]);
	ctx->attrs = (unsigned int *)(&ctx->fds[bufs]);
	ctx->overs = (struct overlap *)(&ctx->attrs[bufs]);
	ctx->overps = (struct overlap **)(&ctx->overs[bufs]);

	K_COPY_FROM_USER(err, kernel, (void *)ctx->lpra, invoke->pra,
							bufs * sizeof(*ctx->lpra));
	if (err) {
		ADSPRPC_ERR(
			"copy from user failed with %d for remote arguments list\n",
			err);
		err = -EFAULT;
		goto bail;
	}

	if (invokefd->fds) {
		K_COPY_FROM_USER(err, kernel_msg, ctx->fds, invokefd->fds,
						bufs * sizeof(*ctx->fds));
		if (err) {
			ADSPRPC_ERR(
				"copy from user failed with %d for fd list\n",
				err);
			err = -EFAULT;
			goto bail;
		}
	} else {
		ctx->fds = NULL;
	}
	if (invokefd->attrs) {
		K_COPY_FROM_USER(err, kernel_msg, ctx->attrs, invokefd->attrs,
						bufs * sizeof(*ctx->attrs));
		if (err) {
			ADSPRPC_ERR(
				"copy from user failed with %d for attribute list\n",
				err);
			err = -EFAULT;
			goto bail;
		}
	}
	ctx->crc = (uint32_t *)invokefd->crc;
	ctx->perf_dsp = (uint64_t *)invokefd->perf_dsp;
	ctx->perf_kernel = (uint64_t *)invokefd->perf_kernel;
	ctx->handle = invoke->handle;
	ctx->sc = invoke->sc;
	if (bufs) {
		VERIFY(err, 0 == (err = context_build_overlap(ctx)));
		if (err)
			goto bail;
	}
	ctx->retval = -1;
	ctx->pid = current->pid;
	ctx->tgid = fl->tgid;
	init_completion(&ctx->work);
	ctx->magic = FASTRPC_CTX_MAGIC;
	ctx->rsp_flags = NORMAL_RESPONSE;
	ctx->is_work_done = false;
	ctx->copybuf = NULL;
	ctx->is_early_wakeup = false;

	if (fl->profile) {
		ctx->perf = kzalloc(sizeof(*(ctx->perf)), GFP_KERNEL);
		VERIFY(err, !IS_ERR_OR_NULL(ctx->perf));
		if (err) {
			kfree(ctx->perf);
			err = -ENOMEM;
			goto bail;
		}
		memset(ctx->perf, 0, sizeof(*(ctx->perf)));
		ctx->perf->tid = fl->tgid;
	}
	if (invokefd->job) {
		K_COPY_FROM_USER(err, kernel_msg, &ctx->asyncjob, invokefd->job,
						sizeof(ctx->asyncjob));
		if (err)
			goto bail;
	}
	if (domain < 0 || domain >= vfl->apps->num_channels) {
		err = -ECHRNG;
		goto bail;
	}
	chan = &vfl->apps->channel[domain];

	spin_lock_irqsave(&chan->ctxlock, irq_flags);
	me->jobid[domain]++;

	/*
	 * To prevent user invocations from exhausting all entries in context
	 * table, it is necessary to reserve a few context table entries for
	 * critical kernel and static RPC calls. The index will begin at 0 for
	 * static handles, while user handles start from
	 * NUM_KERNEL_AND_STATIC_ONLY_CONTEXTS.
	 */
	for (ii = ((kernel_msg || ctx->handle < FASTRPC_STATIC_HANDLE_MAX)
				? 0 : NUM_KERNEL_AND_STATIC_ONLY_CONTEXTS);
				ii < FASTRPC_CTX_MAX; ii++) {
		if (!chan->ctxtable[ii]) {
			chan->ctxtable[ii] = ctx;
			ctx->ctxid = (me->jobid[domain] << FASTRPC_CTX_JOBID_POS)
			  | (ii << FASTRPC_CTX_TABLE_IDX_POS)
			  | ((ctx->asyncjob.isasyncjob &&
			  FASTRPC_ASYNC_JOB_MASK) << FASTRPC_CTX_JOB_TYPE_POS);
			break;
		}
	}
	spin_unlock_irqrestore(&chan->ctxlock, irq_flags);
	VERIFY(err, ii < FASTRPC_CTX_MAX);
	if (err) {
		ADSPRPC_ERR(
			"adsprpc: out of context table entries for handle 0x%x, sc 0x%x\n",
			ctx->handle, ctx->sc);
		err = -ENOKEY;
		goto bail;
	}
	spin_lock(&fl->hlock);
	hlist_add_head(&ctx->hn, &clst->pending);
	clst->num_active_ctxs++;
	spin_unlock(&fl->hlock);

	*po = ctx;
bail:
	if (ctx && err)
		context_free(ctx);
	return err;
}

static void context_save_interrupted(struct vfastrpc_invoke_ctx *ctx)
{
	struct vfastrpc_file *vfl = ctx->vfl;
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	struct fastrpc_ctx_lst *clst = &fl->clst;

	spin_lock(&fl->hlock);
	hlist_del_init(&ctx->hn);
	hlist_add_head(&ctx->hn, &clst->interrupted);
	spin_unlock(&fl->hlock);
}

static void context_list_ctor(struct fastrpc_ctx_lst *me)
{
	INIT_HLIST_HEAD(&me->interrupted);
	INIT_HLIST_HEAD(&me->pending);
	me->num_active_ctxs = 0;
	INIT_HLIST_HEAD(&me->async_queue);
	INIT_LIST_HEAD(&me->notif_queue);
}

static void context_free(struct vfastrpc_invoke_ctx *ctx)
{
	uint32_t i = 0;
	struct vfastrpc_file *vfl = ctx->vfl;
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	int nbufs = REMOTE_SCALARS_INBUFS(ctx->sc) +
		    REMOTE_SCALARS_OUTBUFS(ctx->sc);
	int domain = ctx->vfl->domain;
	struct vfastrpc_channel_ctx *chan = NULL;
	unsigned long irq_flags = 0;

	if (domain < 0 || domain >= vfl->apps->num_channels) {
		ADSPRPC_ERR(
			"invalid channel 0x%x set for session\n",
								domain);
		return;
	}
	chan = &vfl->apps->channel[domain];
	i = (uint32_t)GET_TABLE_IDX_FROM_CTXID(ctx->ctxid);

	spin_lock_irqsave(&chan->ctxlock, irq_flags);
	if (i < FASTRPC_CTX_MAX && chan->ctxtable[i] == ctx) {
		chan->ctxtable[i] = NULL;
	} else {
		for (i = 0; i < FASTRPC_CTX_MAX; i++) {
			if (chan->ctxtable[i] == ctx) {
				chan->ctxtable[i] = NULL;
				break;
			}
		}
	}
	spin_unlock_irqrestore(&chan->ctxlock, irq_flags);

	spin_lock(&fl->hlock);
	if (!hlist_unhashed(&ctx->hn)) {
		hlist_del_init(&ctx->hn);
		fl->clst.num_active_ctxs--;
	}
	spin_unlock(&fl->hlock);

	mutex_lock(&fl->map_mutex);
	for (i = 0; i < nbufs; ++i) {
		if (ctx->maps[i] && ctx->maps[i]->ctx_refs)
			ctx->maps[i]->ctx_refs--;
		hfastrpc_mmap_free(vfl, ctx->maps[i], 0);
	}
	mutex_unlock(&fl->map_mutex);

	hfastrpc_buf_free(ctx->buf, 1);
	if (ctx->copybuf != ctx->buf)
		hfastrpc_buf_free(ctx->copybuf, 1);
	kfree(ctx->lrpra);
	ctx->lrpra = NULL;
	ctx->magic = 0;
	ctx->ctxid = 0;
	if (fl->profile)
		kfree(ctx->perf);

	kfree(ctx);
}

static void hfastrpc_context_list_dtor(struct vfastrpc_file *vfl)
{
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	struct fastrpc_ctx_lst *clst = &fl->clst;
	struct vfastrpc_invoke_ctx *ictx = NULL, *ctxfree;
	struct hlist_node *n;

	do {
		ctxfree = NULL;
		spin_lock(&fl->hlock);
		hlist_for_each_entry_safe(ictx, n, &clst->interrupted, hn) {
			hlist_del_init(&ictx->hn);
			ctxfree = ictx;
			break;
		}
		spin_unlock(&fl->hlock);
		if (ctxfree)
			context_free(ctxfree);
	} while (ctxfree);
	do {
		ctxfree = NULL;
		spin_lock(&fl->hlock);
		hlist_for_each_entry_safe(ictx, n, &clst->pending, hn) {
			hlist_del_init(&ictx->hn);
			ctxfree = ictx;
			break;
		}
		spin_unlock(&fl->hlock);
		if (ctxfree)
			context_free(ctxfree);
	} while (ctxfree);
}

static void hfastrpc_remote_buf_list_free(struct vfastrpc_file *vfl)
{
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	struct vfastrpc_buf *buf, *free;

	do {
		struct hlist_node *n;

		free = NULL;
		spin_lock(&fl->hlock);
		hlist_for_each_entry_safe(buf, n, &fl->remote_bufs, hn_rem) {
			free = buf;
			break;
		}
		spin_unlock(&fl->hlock);
		if (free)
			hfastrpc_buf_free(free, 0);
	} while (free);
}

static void hfastrpc_cached_buf_list_free(struct vfastrpc_file *vfl)
{
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	struct vfastrpc_buf *buf, *free;

	do {
		struct hlist_node *n;

		free = NULL;
		spin_lock(&fl->hlock);
		hlist_for_each_entry_safe(buf, n, &fl->cached_bufs, hn) {
			hlist_del_init(&buf->hn);
			fl->num_cached_buf--;
			free = buf;
			break;
		}
		spin_unlock(&fl->hlock);
		if (free)
			hfastrpc_buf_free(free, 0);
	} while (free);
}

struct vfastrpc_file *hfastrpc_file_alloc(const struct vfastrpc_operations *ops)
{
	int err = 0;
	struct vfastrpc_file *vfl = NULL;
	struct fastrpc_file *fl = NULL;

	VERIFY(err, NULL != (vfl = kzalloc(sizeof(*vfl), GFP_KERNEL)));
	if (err)
		return NULL;
	fl = to_fastrpc_file(vfl);

	context_list_ctor(&fl->clst);
	spin_lock_init(&fl->hlock);
	spin_lock_init(&fl->aqlock);
	spin_lock_init(&fl->proc_state_notif.nqlock);
	INIT_HLIST_HEAD(&fl->maps);
	INIT_HLIST_HEAD(&fl->cached_bufs);
	fl->num_cached_buf = 0;
	INIT_HLIST_HEAD(&fl->remote_bufs);
	INIT_HLIST_HEAD(&vfl->interrupted_cmds);
	init_waitqueue_head(&fl->async_wait_queue);
	init_waitqueue_head(&fl->proc_state_notif.notif_wait_queue);
	INIT_HLIST_NODE(&fl->hn);
	fl->sessionid = 0;
	fl->tgid_open = current->tgid;
	fl->mode = FASTRPC_MODE_SERIAL;
	vfl->domain = -1;
	fl->cid = -1;
	fl->tgid_frpc = -1;
	fl->tvm_remote_domain = -1;
	fl->init_mem = NULL;
	fl->qos_request = 0;
	fl->dsp_proc_init = 0;
	fl->dsp_process_state = PROCESS_CREATE_DEFAULT;
	fl->is_unsigned_pd = false;
	fl->exit_notif = false;
	fl->exit_async = false;
	fl->set_session_info = false;
	fl->multi_session_support = false;
	init_completion(&fl->dma_invoke);
	fl->file_close = FASTRPC_PROCESS_DEFAULT_STATE;
	mutex_init(&fl->internal_map_mutex);
	mutex_init(&fl->map_mutex);
	spin_lock_init(&fl->dspsignals_lock);
	mutex_init(&fl->signal_create_mutex);
	init_completion(&fl->shutdown);

	vfl->ops = ops;

	return vfl;
}

int hfastrpc_file_free(struct vfastrpc_file *vfl)
{
	struct fastrpc_file *fl = vfl ? to_fastrpc_file(vfl) : NULL;
	struct vfastrpc_mmap *map = NULL, *lmap = NULL;
	unsigned long flags;
	int i;

	if (!vfl || !fl)
		return 0;

	spin_lock(&fl->hlock);
	fl->file_close = FASTRPC_PROCESS_EXIT_START;
	spin_unlock(&fl->hlock);

	/* This cmd is only required when PD is opened on DSP */
	if (fl->dsp_proc_init == 1)
		virt_fastrpc_close(vfl);

	debugfs_remove(fl->debugfs_file);
	kfree(fl->debug_buf);

	/* Dummy wake up to exit Async worker thread */
	spin_lock_irqsave(&fl->aqlock, flags);
	atomic_add(1, &fl->async_queue_job_count);
	wake_up_interruptible(&fl->async_wait_queue);
	spin_unlock_irqrestore(&fl->aqlock, flags);

	// Dummy wake up to exit notification worker thread
	spin_lock_irqsave(&fl->proc_state_notif.nqlock, flags);
	atomic_add(1, &fl->proc_state_notif.notif_queue_count);
	wake_up_interruptible(&fl->proc_state_notif.notif_wait_queue);
	spin_unlock_irqrestore(&fl->proc_state_notif.nqlock, flags);

	hfastrpc_context_list_dtor(vfl);
	hfastrpc_cached_buf_list_free(vfl);
	hfastrpc_remote_buf_list_free(vfl);
	if (!IS_ERR_OR_NULL(vfl->hdr_bufs))
		kfree(vfl->hdr_bufs);
	if (!IS_ERR_OR_NULL(vfl->pers_hdr_buf))
		hfastrpc_buf_free(vfl->pers_hdr_buf, 0);

	mutex_lock(&fl->map_mutex);
	do {
		struct hlist_node *n = NULL;

		lmap = NULL;
		hlist_for_each_entry_safe(map, n, &fl->maps, hn) {
			lmap = map;
			break;
		}
		hfastrpc_mmap_free(vfl, lmap, 1);
	} while (lmap);
	mutex_unlock(&fl->map_mutex);

	put_unique_hlos_process_id(vfl);
	spin_lock_irqsave(&vfl->apps->hlock, flags);
	hlist_del_init(&fl->hn);
	spin_unlock_irqrestore(&vfl->apps->hlock, flags);

	for (i = 0; i < (DSPSIGNAL_NUM_SIGNALS / DSPSIGNAL_GROUP_SIZE); i++)
		kfree(fl->signal_groups[i]);
	mutex_destroy(&fl->signal_create_mutex);

	mutex_destroy(&fl->map_mutex);
	mutex_destroy(&fl->internal_map_mutex);
	kfree(vfl);
	return 0;
}

static int context_restore_interrupted(struct vfastrpc_file *vfl,
				struct fastrpc_ioctl_invoke_async *inv,
				struct vfastrpc_invoke_ctx **po)
{
	int err = 0;
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	struct vfastrpc_invoke_ctx *ctx = NULL, *ictx = NULL;
	struct hlist_node *n;
	struct fastrpc_ioctl_invoke *invoke = &inv->inv;

	spin_lock(&fl->hlock);
	hlist_for_each_entry_safe(ictx, n, &fl->clst.interrupted, hn) {
		if (ictx->pid == current->pid) {
			if (invoke->sc != ictx->sc || ictx->vfl != vfl) {
				err = -EINVAL;
				ADSPRPC_ERR(
					"interrupted sc (0x%x) or fl (%pK) does not match with invoke sc (0x%x) or fl (%pK)\n",
					ictx->sc, ictx->vfl, invoke->sc, vfl);
			} else {
				ctx = ictx;
				hlist_del_init(&ctx->hn);
				hlist_add_head(&ctx->hn, &fl->clst.pending);
			}
			break;
		}
	}
	spin_unlock(&fl->hlock);
	if (ctx)
		*po = ctx;
	return err;
}
static int get_args(uint32_t kernel, struct vfastrpc_invoke_ctx *ctx)
{
	struct vfastrpc_file *vfl = ctx->vfl;
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	remote_arg64_t *rpra, *lrpra;
	remote_arg_t *lpra = ctx->lpra;
	struct smq_invoke_buf *list;
	struct smq_phy_page *pages, *ipage;
	uint32_t sc = ctx->sc;
	int inbufs = REMOTE_SCALARS_INBUFS(sc);
	int outbufs = REMOTE_SCALARS_OUTBUFS(sc);
	int handles, bufs = inbufs + outbufs;
	uintptr_t args = 0;
	size_t rlen = 0, copylen = 0, metalen = 0, lrpralen = 0, templen = 0;
	size_t totallen = 0; //header and non ion copy buf len
	int i, oix;
	int err = 0, j = 0, domain = vfl->domain;
	int mflags = 0;
	uint64_t *fdlist = NULL;
	uint32_t *crclist = NULL;
	uint32_t early_hint;
	uint64_t *perf_counter = NULL;
	struct fastrpc_dsp_capabilities *dsp_cap_ptr = NULL;

	if (fl->profile)
		perf_counter = (uint64_t *)ctx->perf + PERF_COUNT;

	/* calculate size of the metadata */
	rpra = NULL;
	lrpra = NULL;
	list = smq_invoke_buf_start(rpra, sc);
	pages = smq_phy_page_start(sc, list);
	ipage = pages;

	PERF(fl->profile, GET_COUNTER(perf_counter, PERF_MAP),
	for (i = 0; i < bufs; ++i) {
		uintptr_t buf = (uintptr_t)lpra[i].buf.pv;
		size_t len = lpra[i].buf.len;

		mutex_lock(&fl->map_mutex);
		if (ctx->fds && (ctx->fds[i] != -1))
			err = hfastrpc_mmap_create(vfl, ctx->fds[i],
					ctx->attrs[i], buf, len,
					mflags, &ctx->maps[i]);
		if (ctx->maps[i])
			ctx->maps[i]->ctx_refs++;
		mutex_unlock(&fl->map_mutex);
		if (err)
			goto bail;
		ipage += 1;
	}
	PERF_END);
	handles = REMOTE_SCALARS_INHANDLES(sc) + REMOTE_SCALARS_OUTHANDLES(sc);
	mutex_lock(&fl->map_mutex);
	for (i = bufs; i < bufs + handles; i++) {
		int dmaflags = 0;

		if (ctx->attrs && (ctx->attrs[i] & FASTRPC_ATTR_NOMAP))
			dmaflags = FASTRPC_MAP_FD_NOMAP;
		if (domain < 0 || domain >= vfl->apps->num_channels) {
			err = -ECHRNG;
			mutex_unlock(&fl->map_mutex);
			goto bail;
		}
		dsp_cap_ptr = &vfl->apps->channel[domain].dsp_cap_kernel;
		// Skip cpu mapping if DMA_HANDLE_REVERSE_RPC_CAP is true.
		if (!dsp_cap_ptr->dsp_attributes[DMA_HANDLE_REVERSE_RPC_CAP] &&
					ctx->fds && (ctx->fds[i] != -1))
			err = hfastrpc_mmap_create(vfl, ctx->fds[i],
					FASTRPC_ATTR_NOVA, 0, 0, dmaflags,
					&ctx->maps[i]);
		if (err) {
			for (j = bufs; j < i; j++) {
				if (ctx->maps[j] && ctx->maps[j]->dma_handle_refs) {
					ctx->maps[j]->dma_handle_refs--;
					hfastrpc_mmap_free(vfl, ctx->maps[j], 0);
				}
			}
			mutex_unlock(&fl->map_mutex);
			goto bail;
		} else if (ctx->maps[i]) {
			ctx->maps[i]->dma_handle_refs++;
		}
		ipage += 1;
	}
	mutex_unlock(&fl->map_mutex);

	/* metalen includes meta data, fds, crc, dsp perf and early wakeup hint */
	metalen = totallen = (size_t)&ipage[0] + (sizeof(uint64_t) * M_FDLIST) +
			(sizeof(uint32_t) * M_CRCLIST) + (sizeof(uint64_t) * M_DSP_PERF_LIST) +
			sizeof(early_hint);

	if (metalen) {
		err = hfastrpc_buf_alloc(vfl, metalen, 0, 0,
				VFASTRPC_BUF_TYPE_METADATA,
				PAGE_KERNEL, &ctx->buf);
		if (err)
			goto bail;
		VERIFY(err, !IS_ERR_OR_NULL(ctx->buf->va));
		if (err)
			goto bail;
		memset(ctx->buf->va, 0, metalen);
	}
	ctx->used = metalen;

	/* allocate new local rpra buffer */
	lrpralen = (size_t)&list[0];
	if (lrpralen) {
		lrpra = kzalloc(lrpralen, GFP_KERNEL);
		VERIFY(err, !IS_ERR_OR_NULL(lrpra));
		if (err) {
			err = -ENOMEM;
			goto bail;
		}
	}
	ctx->lrpra = lrpra;

	/* calculate len required for copying */
	for (oix = 0; oix < inbufs + outbufs; ++oix) {
		int i = ctx->overps[oix]->raix;
		uintptr_t mstart, mend;
		size_t len = lpra[i].buf.len;

		if (!len)
			continue;
		if (ctx->maps[i])
			continue;
		if (ctx->overps[oix]->offset == 0)
			copylen = ALIGN(copylen, BALIGN);
		mstart = ctx->overps[oix]->mstart;
		mend = ctx->overps[oix]->mend;
		templen = mend - mstart;
		VERIFY(err, ((templen <= LONG_MAX) && (copylen <= (LONG_MAX - templen))));
		if (err) {
			err = -EFAULT;
			goto bail;
		}
		copylen += templen;
	}
	totallen = ALIGN(totallen, BALIGN) + copylen;

	/* allocate non -ion copy buffer */
	/* Checking if copylen can be accomodated in metalen*/
	/*if not allocating new buffer */
	if (totallen <= (size_t)buf_page_size(metalen)) {
		args = (uintptr_t)ctx->buf->va + metalen;
		ctx->copybuf = ctx->buf;
		rlen = totallen - metalen;
	} else if (copylen) {
		err = hfastrpc_buf_alloc(vfl, copylen, 0, 0,
				VFASTRPC_BUF_TYPE_COPYDATA,
				PAGE_KERNEL,
				&ctx->copybuf);
		if (err)
			goto bail;

		memset(ctx->copybuf->va, 0, copylen);
		args = (uintptr_t)ctx->copybuf->va;
		rlen = copylen;
		totallen = copylen;
	}

	/* copy metadata */
	rpra = ctx->buf->va;
	ctx->rpra = rpra;
	list = smq_invoke_buf_start(rpra, sc);
	pages = smq_phy_page_start(sc, list);
	ipage = pages;
	for (i = 0; i < bufs + handles; ++i) {
		if (lpra[i].buf.len)
			list[i].num = 1;
		else
			list[i].num = 0;
		list[i].pgidx = ipage - pages;
		ipage++;
	}

	/* map ion buffers */
	PERF(fl->profile, GET_COUNTER(perf_counter, PERF_MAP),
	for (i = 0; rpra && i < inbufs + outbufs; ++i) {
		struct vfastrpc_mmap *map = ctx->maps[i];
		uint64_t buf = ptr_to_uint64(lpra[i].buf.pv);
		size_t len = lpra[i].buf.len;

		rpra[i].buf.pv = 0;
		rpra[i].buf.len = len;
		if (!len)
			continue;
		if (map) {
			struct vm_area_struct *vma;
			uintptr_t offset;
			uint64_t num = buf_num_pages(buf, len);
			int idx = list[i].pgidx;

			if (map->attr & FASTRPC_ATTR_NOVA) {
				offset = 0;
			} else {
				down_read(&current->mm->mmap_lock);
				VERIFY(err, NULL != (vma = find_vma(current->mm,
								map->va)));
				if (err) {
					up_read(&current->mm->mmap_lock);
					goto bail;
				}
				offset = buf_page_start(buf) - vma->vm_start;
				up_read(&current->mm->mmap_lock);
				VERIFY(err, offset + len <= (uintptr_t)map->size);
				if (err) {
					ADSPRPC_ERR(
						"buffer address is invalid for the fd passed for %d address 0x%lx and size %zu\n",
						i, (uintptr_t)lpra[i].buf.pv,
						lpra[i].buf.len);
					err = -EFAULT;
					goto bail;
				}
			}
			pages[idx].addr = map->da + offset;
			pages[idx].size = num << PAGE_SHIFT;
		}
		rpra[i].buf.pv = buf;
	}
	PERF_END);
	/* Since we are not holidng map_mutex during get args whole time
	 * it is possible that dma handle map may be removed by some invalid
	 * fd passed by DSP. Inside the lock check if the map present or not
	 */
	mutex_lock(&fl->map_mutex);
	for (i = bufs; i < bufs + handles; ++i) {
		struct vfastrpc_mmap *mmap = NULL;
		/* check if map  was created */
		if (ctx->maps[i] && ctx->fds) {
			/* check if map still exist */
			if (!vfastrpc_mmap_find(ctx->vfl, ctx->fds[i], 0, 0,
				0, 0, &mmap)) {
				if (mmap) {
					pages[i].addr = mmap->da;
					pages[i].size = mmap->size;
				}
			} else {
				/* map already freed by some other call */
				mutex_unlock(&fl->map_mutex);
				ADSPRPC_ERR("could not find map associated with dma hadle fd %d \n",
					ctx->fds[i]);
				goto bail;
			}
		}
	}
	mutex_unlock(&fl->map_mutex);
	fdlist = (uint64_t *)&pages[bufs + handles];
	crclist = (uint32_t *)&fdlist[M_FDLIST];
	/* reset fds, crc and early wakeup hint memory */
	/* remote process updates these values before responding */
	memset(fdlist, 0, sizeof(uint64_t)*M_FDLIST + sizeof(uint32_t)*M_CRCLIST +
			(sizeof(uint64_t) * M_DSP_PERF_LIST) + sizeof(early_hint));

	/* copy non ion buffers */
	PERF(fl->profile, GET_COUNTER(perf_counter, PERF_COPY),
	for (oix = 0; rpra && oix < inbufs + outbufs; ++oix) {
		int i = ctx->overps[oix]->raix;
		struct vfastrpc_mmap *map = ctx->maps[i];
		size_t mlen;
		uint64_t buf;
		size_t len = lpra[i].buf.len;

		if (!len)
			continue;
		if (map)
			continue;
		if (ctx->overps[oix]->offset == 0) {
			rlen -= ALIGN(args, BALIGN) - args;
			args = ALIGN(args, BALIGN);
		}
		mlen = ctx->overps[oix]->mend - ctx->overps[oix]->mstart;
		VERIFY(err, rlen >= mlen);
		if (err) {
			err = -EFAULT;
			goto bail;
		}
		rpra[i].buf.pv =
			 (args - ctx->overps[oix]->offset);
		pages[list[i].pgidx].addr = ctx->copybuf->da -
					    ctx->overps[oix]->offset +
					    (totallen - rlen);
		pages[list[i].pgidx].addr =
			buf_page_start(pages[list[i].pgidx].addr);
		buf = rpra[i].buf.pv;
		pages[list[i].pgidx].size = buf_num_pages(buf, len) * PAGE_SIZE;
		if (i < inbufs) {
			K_COPY_FROM_USER(err, kernel, uint64_to_ptr(buf),
					lpra[i].buf.pv, len);
			if (err) {
				ADSPRPC_ERR(
					"copy from user failed with %d for dst 0x%llx, src %pK, size 0x%zx, arg %d\n",
					err, buf, lpra[i].buf.pv, len, i+1);
				err = -EFAULT;
				goto bail;
			}
		}
		if (len > DEBUG_PRINT_SIZE_LIMIT)
			ADSPRPC_DEBUG(
				"copied non ion buffer sc 0x%x pv 0x%llx, mend 0x%lx mstart 0x%lx, len %zu\n",
				sc, rpra[i].buf.pv,
				ctx->overps[oix]->mend,
				ctx->overps[oix]->mstart, len);
		args = args + mlen;
		rlen -= mlen;
	}
	PERF_END);

	for (i = bufs; ctx->fds && rpra && i < bufs + handles; i++) {
		rpra[i].dma.fd = ctx->fds[i];
		rpra[i].dma.len = (uint32_t)lpra[i].buf.len;
		rpra[i].dma.offset =
				(uint32_t)(uintptr_t)lpra[i].buf.pv;
	}

	/* Copy rpra to local buffer */
	if (ctx->lrpra && rpra && lrpralen > 0)
		memcpy(ctx->lrpra, rpra, lrpralen);
 bail:
	return err;
}

static int put_args(uint32_t kernel, struct vfastrpc_invoke_ctx *ctx,
		    remote_arg_t *upra)
{
	struct vfastrpc_file *vfl = ctx->vfl;
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	uint32_t sc = ctx->sc;
	struct smq_invoke_buf *list;
	struct smq_phy_page *pages;
	struct vfastrpc_mmap *mmap;
	uint64_t *fdlist;
	uint32_t *crclist = NULL, *poll = NULL;
	uint64_t *perf_dsp_list = NULL;

	remote_arg64_t *rpra = ctx->lrpra;
	int i, inbufs, outbufs, handles;
	int err = 0, perfErr = 0;

	inbufs = REMOTE_SCALARS_INBUFS(sc);
	outbufs = REMOTE_SCALARS_OUTBUFS(sc);
	handles = REMOTE_SCALARS_INHANDLES(sc) + REMOTE_SCALARS_OUTHANDLES(sc);
	list = smq_invoke_buf_start(ctx->rpra, sc);
	pages = smq_phy_page_start(sc, list);
	fdlist = (uint64_t *)(pages + inbufs + outbufs + handles);
	crclist = (uint32_t *)(fdlist + M_FDLIST);
	poll = (uint32_t *)(crclist + M_CRCLIST);
	perf_dsp_list = (uint64_t *)(poll + 1);

	for (i = inbufs; i < inbufs + outbufs; ++i) {
		if (!ctx->maps[i]) {
			K_COPY_TO_USER(err, kernel,
				ctx->lpra[i].buf.pv,
				uint64_to_ptr(rpra[i].buf.pv),
				rpra[i].buf.len);
			if (err) {
				ADSPRPC_ERR(
					"Invalid size 0x%llx for output argument %d ret %d\n",
					rpra[i].buf.len, i+1, err);
				err = -EFAULT;
				goto bail;
			}
		} else {
			mutex_lock(&fl->map_mutex);
			if (ctx->maps[i]->ctx_refs)
				ctx->maps[i]->ctx_refs--;
			hfastrpc_mmap_free(vfl, ctx->maps[i], 0);
			mutex_unlock(&fl->map_mutex);
			ctx->maps[i] = NULL;
		}
	}
	mutex_lock(&fl->map_mutex);
	for (i = 0; i < M_FDLIST; i++) {
		if (!fdlist[i])
			break;
		if (!vfastrpc_mmap_find(vfl, (int)fdlist[i], 0, 0,
					0, 0, &mmap)) {
			if (mmap && mmap->dma_handle_refs) {
				mmap->dma_handle_refs = 0;
				hfastrpc_mmap_free(vfl, mmap, 0);
			}
		}
	}
	mutex_unlock(&fl->map_mutex);
	if (ctx->crc && crclist && rpra)
		K_COPY_TO_USER(err, kernel, ctx->crc,
			crclist, M_CRCLIST*sizeof(uint32_t));
	if (ctx->perf_dsp && perf_dsp_list) {
		K_COPY_TO_USER(perfErr, kernel, ctx->perf_dsp,
			perf_dsp_list, M_DSP_PERF_LIST*sizeof(uint64_t));
		if (perfErr)
			ADSPRPC_WARN("failed to copy perf data err %d\n", perfErr);
	}

 bail:
	return err;
}

static inline void hfastrpc_update_txmsg_buf(struct vfastrpc_channel_ctx *chan,
	struct smq_msg *msg, int transport_send_err, int64_t ns, uint64_t xo_time_in_us)
{
	unsigned long flags = 0;
	unsigned int tx_index = 0;
	struct fastrpc_tx_msg *tx_msg = NULL;

	spin_lock_irqsave(&chan->gmsg_log.lock, flags);

	tx_index = chan->gmsg_log.tx_index;
	tx_msg = &chan->gmsg_log.tx_msgs[tx_index];

	memcpy(&tx_msg->msg, msg, sizeof(struct smq_msg));
	tx_msg->transport_send_err = transport_send_err;
	tx_msg->ns = ns;
	tx_msg->xo_time_in_us = xo_time_in_us;

	tx_index++;
	chan->gmsg_log.tx_index =
		(tx_index > (GLINK_MSG_HISTORY_LEN - 1)) ? 0 : tx_index;

	spin_unlock_irqrestore(&chan->gmsg_log.lock, flags);
}

static inline void hfastrpc_update_rxmsg_buf(struct vfastrpc_channel_ctx *chan,
	uint64_t ctx, int retval, uint32_t rsp_flags,
	uint32_t early_wake_time, uint32_t ver, int64_t ns, uint64_t xo_time_in_us)
{
	unsigned long flags = 0;
	unsigned int rx_index = 0;
	struct fastrpc_rx_msg *rx_msg = NULL;
	struct smq_invoke_rspv2 *rsp = NULL;

	spin_lock_irqsave(&chan->gmsg_log.lock, flags);

	rx_index = chan->gmsg_log.rx_index;
	rx_msg = &chan->gmsg_log.rx_msgs[rx_index];
	rsp = &rx_msg->rsp;

	rsp->ctx = ctx;
	rsp->retval = retval;
	rsp->flags = rsp_flags;
	rsp->early_wake_time = early_wake_time;
	rsp->version = ver;
	rx_msg->ns = ns;
	rx_msg->xo_time_in_us = xo_time_in_us;

	rx_index++;
	chan->gmsg_log.rx_index =
		(rx_index > (GLINK_MSG_HISTORY_LEN - 1)) ? 0 : rx_index;

	spin_unlock_irqrestore(&chan->gmsg_log.lock, flags);
}

static int hfastrpc_invoke_send(struct vfastrpc_invoke_ctx *ctx,
			       uint32_t kernel, uint32_t handle)
{
	struct smq_msg *msg = &ctx->smsg;
	struct smq_msg msg_temp;
	struct vfastrpc_file *vfl = ctx->vfl;
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	struct vfastrpc_channel_ctx *channel_ctx = NULL;
	int err = 0, domain = -1;
	uint32_t sc = ctx->sc;
	int64_t ns = 0;
	uint64_t xo_time_in_us = 0;
	int isasync = (ctx->asyncjob.isasyncjob ? true : false);

	if (!fl) {
		err = -EBADF;
		goto bail;
	}
	domain = vfl->domain;
	if (domain < 0 || domain >= vfl->apps->num_channels) {
		err = -ECHRNG;
		goto bail;
	}

	msg->pid = vfl->upid;
	msg->tid = current->pid;
	if (fl->sessionid)
		msg->tid |= SESSION_ID_MASK;
	if (kernel == KERNEL_MSG_WITH_ZERO_PID)
		msg->pid = 0;
	msg->invoke.header.ctx = ctx->ctxid | fl->pd;
	msg->invoke.header.handle = handle;
	msg->invoke.header.sc = sc;
	msg->invoke.page.addr = ctx->buf ? ctx->buf->da : 0;
	msg->invoke.page.size = buf_page_size(ctx->used);

	channel_ctx = &vfl->apps->channel[domain];
	mutex_lock(&channel_ctx->smd_mutex);

	if (fl->ssrcount != channel_ctx->ssrcount) {
		err = -ECONNRESET;
		mutex_unlock(&channel_ctx->smd_mutex);
		goto bail;
	}
	mutex_unlock(&channel_ctx->smd_mutex);

	xo_time_in_us = CONVERT_CNT_TO_US(__arch_counter_get_cntvct());
	if (isasync) {
		/*
		 * After message is sent to DSP, async response thread could immediately
		 * get the response and free context, which will result in a use-after-free
		 * in this function. So use a local variable for message.
		 */
		memcpy(&msg_temp, msg, sizeof(struct smq_msg));
		msg = &msg_temp;
	}
	err = fastrpc_transport_send(domain, (void *)msg, sizeof(*msg), fl->tvm_remote_domain);
	ns = get_timestamp_in_ns();
	hfastrpc_update_txmsg_buf(channel_ctx, msg, err, ns, xo_time_in_us);
 bail:
	return err;
}

static inline int hfastrpc_wait_for_response(struct vfastrpc_invoke_ctx *ctx,
						uint32_t kernel)
{
	int interrupted = 0;

	if (kernel)
		wait_for_completion(&ctx->work);
	else
		interrupted = wait_for_completion_interruptible(&ctx->work);

	return interrupted;
}

static inline int poll_for_remote_response(struct vfastrpc_invoke_ctx *ctx, uint32_t timeout)
{
	struct vfastrpc_file *vfl = ctx->vfl;
	struct vfastrpc_channel_ctx *chan = &vfl->apps->channel[vfl->domain];
	int err = -EIO;
	uint32_t sc = ctx->sc, ii = 0, jj = 0;
	struct smq_invoke_buf *list;
	struct smq_phy_page *pages;
	uint64_t *fdlist = NULL;
	uint32_t *crclist = NULL, *poll = NULL;
	unsigned int inbufs, outbufs, handles;

	/* calculate poll memory location */
	inbufs = REMOTE_SCALARS_INBUFS(sc);
	outbufs = REMOTE_SCALARS_OUTBUFS(sc);
	handles = REMOTE_SCALARS_INHANDLES(sc) + REMOTE_SCALARS_OUTHANDLES(sc);
	list = smq_invoke_buf_start(ctx->rpra, sc);
	pages = smq_phy_page_start(sc, list);
	fdlist = (uint64_t *)(pages + inbufs + outbufs + handles);
	crclist = (uint32_t *)(fdlist + M_FDLIST);
	poll = (uint32_t *)(crclist + M_CRCLIST);

	/* poll on memory for DSP response. Return failure on timeout */
	for (ii = 0, jj = 0; ii < timeout; ii++, jj++) {
		if (*poll == FASTRPC_EARLY_WAKEUP_POLL) {
			/* Remote processor sent early response */
			err = 0;
			break;
		} else if (*poll == FASTRPC_POLL_RESPONSE) {
			/* Remote processor sent poll response to complete the call */
			err = 0;
			ctx->is_work_done = true;
			ctx->retval = 0;
			/* Update DSP response history */
			hfastrpc_update_rxmsg_buf(chan,
				ctx->smsg.invoke.header.ctx, 0, POLL_MODE, 0,
				FASTRPC_RSP_VERSION2, get_timestamp_in_ns(),
				CONVERT_CNT_TO_US(__arch_counter_get_cntvct()));
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

static void hfastrpc_wait_for_completion(struct vfastrpc_invoke_ctx *ctx,
			int *ptr_interrupted, uint32_t kernel, uint32_t async,
			bool *ptr_isworkdone)
{
	struct vfastrpc_file *vfl = NULL;
	struct fastrpc_file *fl = NULL;
	int interrupted = 0, err = 0;
	int jj;
	bool wait_resp;
	uint32_t wTimeout = FASTRPC_USER_EARLY_HINT_TIMEOUT;
	uint32_t wakeTime = 0;
	unsigned long flags;

	if (!ctx) {
		/* This failure is not expected */
		err = *ptr_interrupted = EFAULT;
		*ptr_isworkdone = false;
		ADSPRPC_ERR("ctx is NULL, cannot wait for response err %d\n",
					err);
		return;
	}
	vfl = ctx->vfl;
	fl = to_fastrpc_file(vfl);
	wakeTime = ctx->early_wake_time;

	do {
		switch (ctx->rsp_flags) {
		/* try polling on completion with timeout */
		case USER_EARLY_SIGNAL:
			/* try wait if completion time is less than timeout */
			/* disable preempt to avoid context switch latency */
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
			if (async) {
				spin_lock_irqsave(&fl->aqlock, flags);
				if (!ctx->is_work_done) {
					ctx->is_early_wakeup = false;
					*ptr_isworkdone = false;
				} else
					*ptr_isworkdone = true;
				spin_unlock_irqrestore(&fl->aqlock, flags);
				goto bail;
			} else if (!wait_resp) {
				interrupted = hfastrpc_wait_for_response(ctx,
									kernel);
				*ptr_interrupted = interrupted;
				if (interrupted || ctx->is_work_done)
					goto bail;
			}
			break;

		/* busy poll on memory for actual job done */
		case EARLY_RESPONSE:
			err = poll_for_remote_response(ctx, FASTRPC_POLL_TIME);

			/* Mark job done if poll on memory successful */
			/* Wait for completion if poll on memory timoeut */
			if (!err) {
				ctx->is_work_done = true;
				*ptr_isworkdone = true;
				goto bail;
			}
			ADSPRPC_INFO("early rsp poll timeout (%u us) for handle 0x%x, sc 0x%x\n",
				FASTRPC_POLL_TIME, ctx->handle, ctx->sc);
			if (async) {
				spin_lock_irqsave(&fl->aqlock, flags);
				if (!ctx->is_work_done) {
					ctx->is_early_wakeup = false;
					*ptr_isworkdone = false;
				} else
					*ptr_isworkdone = true;
				spin_unlock_irqrestore(&fl->aqlock, flags);
				goto bail;
			} else if (!ctx->is_work_done) {
				interrupted = hfastrpc_wait_for_response(ctx,
									kernel);
				*ptr_interrupted = interrupted;
				if (interrupted || ctx->is_work_done)
					goto bail;
			}
			break;

		case COMPLETE_SIGNAL:
		case NORMAL_RESPONSE:
			if (!async) {
				interrupted = hfastrpc_wait_for_response(ctx,
								kernel);
				*ptr_interrupted = interrupted;
				if (interrupted || ctx->is_work_done)
					goto bail;
			} else {
				spin_lock_irqsave(&fl->aqlock, flags);
				if (!ctx->is_work_done) {
					ctx->is_early_wakeup = false;
					*ptr_isworkdone = false;
				} else
					*ptr_isworkdone = true;
				spin_unlock_irqrestore(&fl->aqlock, flags);
				goto bail;
			}
			break;
		case POLL_MODE:
			err = poll_for_remote_response(ctx, fl->poll_timeout);

			/* If polling timed out, move to normal response state */
			if (err) {
				ADSPRPC_INFO("poll mode timeout (%u us) for handle 0x%x, sc 0x%x\n",
					fl->poll_timeout, ctx->handle, ctx->sc);
				ctx->rsp_flags = NORMAL_RESPONSE;
			} else {
				*ptr_interrupted = 0;
				*ptr_isworkdone = true;
			}
			break;
		default:
			*ptr_interrupted = EBADR;
			*ptr_isworkdone = false;
			ADSPRPC_ERR(
				"unsupported response flags 0x%x for handle 0x%x, sc 0x%x\n",
				ctx->rsp_flags, ctx->handle, ctx->sc);
			goto bail;
		} /* end of switch */
	} while (!ctx->is_work_done);
bail:
	return;
}

int hfastrpc_internal_invoke(struct vfastrpc_file *vfl, uint32_t mode,
				uint32_t msg_type,
				struct fastrpc_ioctl_invoke_async *inv)
{
	struct vfastrpc_invoke_ctx *ctx = NULL;
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	struct fastrpc_ioctl_invoke *invoke = &inv->inv;
	int err = 0, interrupted = 0, domain = -1, perfErr = 0;
	struct timespec64 invoket = {0};
	uint64_t *perf_counter = NULL;
	bool isasyncinvoke = false, isworkdone = false;
	uint32_t kernel = (msg_type == COMPAT_MSG) ? USER_MSG : msg_type;

	ADSP_LOG("start pid=%d,tid=%d,sc=%x,h=%x\n",
			fl->tgid, current->pid, inv->inv.sc, inv->inv.handle);
	domain = vfl->domain;
	if (domain < 0 || domain >= vfl->apps->num_channels) {
		ADSPRPC_ERR("invalid channel 0x%x set for session\n",
			domain);
		err = -EBADR;
		goto bail;
	}

	if (fl->profile)
		ktime_get_real_ts64(&invoket);

	if (!kernel) {
		VERIFY(err, invoke->handle !=
			FASTRPC_STATIC_HANDLE_PROCESS_GROUP);
		VERIFY(err, invoke->handle !=
			FASTRPC_STATIC_HANDLE_DSP_UTILITIES);
		if (err) {
			err = -EINVAL;
			ADSPRPC_ERR(
				"user application trying to send a kernel RPC message to channel %d, handle 0x%x\n",
				domain, invoke->handle);
			goto bail;
		}
		VERIFY(err, 0 == (err = context_restore_interrupted(vfl,
		inv, &ctx)));
		if (err)
			goto bail;
		if (ctx)
			goto wait;
	}

	VERIFY(err, 0 == (err = context_alloc(vfl, msg_type, inv, &ctx)));
	if (err)
		goto bail;
	isasyncinvoke = (ctx->asyncjob.isasyncjob ? true : false);
	if (fl->profile)
		perf_counter = (uint64_t *)ctx->perf + PERF_COUNT;
	PERF(fl->profile, GET_COUNTER(perf_counter, PERF_GETARGS),
	VERIFY(err, 0 == (err = get_args(kernel, ctx)));
	PERF_END);
	if (err)
		goto bail;

	PERF(fl->profile, GET_COUNTER(perf_counter, PERF_LINK),
	VERIFY(err, 0 == (err = hfastrpc_invoke_send(ctx,
		kernel, invoke->handle)));
	PERF_END);

	if (err)
		goto bail;
	if (isasyncinvoke)
		goto invoke_end;
 wait:
	/* Poll mode allowed only for non-static handle calls to dynamic CDSP process */
	if (fl->poll_mode && (invoke->handle > FASTRPC_STATIC_HANDLE_MAX)
		&& (domain == CDSP_DOMAIN_ID)
		&& (fl->proc_flags == FASTRPC_INIT_CREATE))
		ctx->rsp_flags = POLL_MODE;

	hfastrpc_wait_for_completion(ctx, &interrupted, kernel, 0, &isworkdone);
	VERIFY(err, 0 == (err = interrupted));
	if (err)
		goto bail;

	if (!ctx->is_work_done) {
		err = -ETIMEDOUT;
		ADSPRPC_ERR(
			"WorkDone state is invalid for handle 0x%x, sc 0x%x\n",
			invoke->handle, ctx->sc);
		goto bail;
	}

	PERF(fl->profile, GET_COUNTER(perf_counter, PERF_PUTARGS),
	VERIFY(err, 0 == (err = put_args(kernel, ctx, invoke->pra)));
	PERF_END);
	if (err)
		goto bail;

	VERIFY(err, 0 == (err = ctx->retval));
	if (err)
		goto bail;
 bail:
	if (ctx && interrupted == -ERESTARTSYS) {
		context_save_interrupted(ctx);
	} else if (ctx) {
		if (fl->profile && !interrupted)
			hfastrpc_update_invoke_count(invoke->handle,
				perf_counter, &invoket);
		if (fl->profile && ctx->perf && ctx->handle > FASTRPC_STATIC_HANDLE_MAX) {
			if (ctx->perf_kernel) {
				K_COPY_TO_USER(perfErr, kernel, ctx->perf_kernel,
				ctx->perf, M_KERNEL_PERF_LIST*sizeof(uint64_t));
				if (perfErr)
					ADSPRPC_WARN("failed to copy perf data err %d\n", perfErr);
			}
		}
		context_free(ctx);
		if (fl->profile)
			perf_counter = NULL;
	}
	if (domain >= 0 && domain < vfl->apps->num_channels) {
		mutex_lock(&(vfl->apps->channel[domain].smd_mutex));
		if (fl->ssrcount != vfl->apps->channel[domain].ssrcount)
			err = -ECONNRESET;
		mutex_unlock(&(vfl->apps->channel[domain].smd_mutex));
	}

invoke_end:
	if (fl->profile && !interrupted && isasyncinvoke)
		hfastrpc_update_invoke_count(invoke->handle, perf_counter,
						&invoket);
	ADSP_LOG("end err=%d, pid=%d,tid=%d,sc=%x,h=%x\n",
			err, fl->tgid, current->pid,
			inv->inv.sc, inv->inv.handle);
	return err;
}

static int hfastrpc_invoke(struct vfastrpc_file *vfl,
			uint32_t mode, struct fastrpc_ioctl_invoke_async *inv, uint32_t msg_type)
{
	return hfastrpc_internal_invoke(vfl, mode, msg_type, inv);
}

static int hfastrpc_get_async_response(
		struct fastrpc_ioctl_async_response *async_res,
			void *param, struct vfastrpc_file *vfl)
{
	return 0;
}

static int hfastrpc_set_session_info(
		struct fastrpc_proc_sess_info *sess_info,
			void *param, struct vfastrpc_file *vfl)
{
	int err = 0;
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	struct vfastrpc_apps *me = vfl->apps;

	/*
	 * Third-party apps don't have permission to open the fastrpc device, so
	 * it is opened on their behalf by DSP HAL. This is detected by
	 * comparing current PID with the one stored during device open.
	 */
	if (current->tgid != fl->tgid_open)
		fl->untrusted_process = true;
	VERIFY(err, sess_info->pd_type > DEFAULT_UNUSED &&
				sess_info->pd_type < MAX_PD_TYPE);
	if (err) {
		ADSPRPC_ERR(
		"Session PD type %u is invalid for the process\n",
							sess_info->pd_type);
		err = -EBADR;
		goto bail;
	}
	if (fl->untrusted_process && sess_info->pd_type != USERPD) {
		ADSPRPC_ERR(
		"Session PD type %u not allowed for untrusted process\n",
						sess_info->pd_type);
		err = -EBADR;
		goto bail;
	}
	if (sess_info->session_id >= me->max_sess_per_proc) {
		ADSPRPC_ERR(
		"Session ID %u cannot be beyond %u\n",
				sess_info->session_id, me->max_sess_per_proc);
		err = -EBADR;
		goto bail;
	}
	fl->sessionid = sess_info->session_id;
	// Set multi_session_support, to disable old way of setting session_id
	fl->multi_session_support = true;
	VERIFY(err, 0 == (err = hfastrpc_get_info(vfl, &(sess_info->domain_id))));
	if (err)
		goto bail;
	K_COPY_TO_USER(err, 0, param, sess_info,
			sizeof(struct fastrpc_proc_sess_info));
bail:
	return err;
}

static int hfastrpc_wait_on_notif_queue(
			struct fastrpc_ioctl_notif_rsp *notif_rsp,
			struct vfastrpc_file *vfl)
{
	int err = 0, interrupted = 0;
	unsigned long flags;
	struct fastrpc_file *fl = NULL;
	struct smq_notif_rsp  *notif = NULL, *inotif = NULL, *n = NULL;

read_notif_status:
        if (!vfl) {
                err = -EBADF;
                goto bail;
        }
	fl = to_fastrpc_file(vfl);
	interrupted = wait_event_interruptible(fl->proc_state_notif.notif_wait_queue,
				atomic_read(&fl->proc_state_notif.notif_queue_count));
	if (fl->exit_notif) {
		err = -EFAULT;
		goto bail;
	}
	VERIFY(err, 0 == (err = interrupted));
	if (err)
		goto bail;

	spin_lock_irqsave(&fl->proc_state_notif.nqlock, flags);
	list_for_each_entry_safe(inotif, n, &fl->clst.notif_queue, notifn) {
		list_del_init(&inotif->notifn);
		atomic_sub(1, &fl->proc_state_notif.notif_queue_count);
		notif = inotif;
		break;
	}
	spin_unlock_irqrestore(&fl->proc_state_notif.nqlock, flags);

	if (notif) {
		notif_rsp->status = notif->status;
		notif_rsp->domain = notif->domain;
		notif_rsp->session = notif->session;
	} else {// Go back to wait if ctx is invalid
		ADSPRPC_ERR("Invalid status notification response\n");
		goto read_notif_status;
	}
bail:
	kfree(notif);
	return err;
}

static int hfastrpc_get_notif_response(
		struct fastrpc_ioctl_notif_rsp *notif,
			void *param, struct vfastrpc_file *vfl)
{
	int err = 0;

	err = hfastrpc_wait_on_notif_queue(notif, vfl);
	if (err)
		goto bail;
	K_COPY_TO_USER(err, 0, param, notif,
			sizeof(struct fastrpc_ioctl_notif_rsp));
bail:
	return err;
}

static int hfastrpc_create_persistent_headers(struct vfastrpc_file *vfl,
			uint32_t user_concurrency)
{
	struct fastrpc_file* fl = to_fastrpc_file(vfl);
	int err = 0, i = 0;
	uint64_t va_base = 0;
	struct vfastrpc_buf *pers_hdr_buf = NULL, *hdr_bufs = NULL, *buf = NULL;
	unsigned int num_pers_hdrs = 0;
	size_t hdr_buf_alloc_len = 0;

	if (vfl->pers_hdr_buf || !user_concurrency)
		goto bail;

	/*
	 * Pre-allocate memory for persistent header buffers based
	 * on concurrency info passed by user. Upper limit enforced.
	 */
	num_pers_hdrs = (user_concurrency > MAX_PERSISTENT_HEADERS) ?
		MAX_PERSISTENT_HEADERS : user_concurrency;
	hdr_buf_alloc_len = num_pers_hdrs * PAGE_SIZE;
	err = hfastrpc_buf_alloc(vfl, hdr_buf_alloc_len, 0, 0,
			VFASTRPC_BUF_TYPE_METADATA, PAGE_KERNEL, &pers_hdr_buf);
	if (err)
		goto bail;
	va_base = ptr_to_uint64(pers_hdr_buf->va);

	/* Map entire buffer on remote subsystem in single RPC call */
	err = hfastrpc_mem_map_to_dsp(vfl, -1, 0, ADSP_MMAP_PERSIST_HDR, 0,
			pers_hdr_buf->da, pers_hdr_buf->size,
			&pers_hdr_buf->raddr);
	if (err)
		goto bail;

	/* Divide and store as N chunks, each of 1 page size */
	hdr_bufs = kcalloc(num_pers_hdrs, sizeof(struct vfastrpc_buf),
				GFP_KERNEL);
	if (!hdr_bufs) {
		err = -ENOMEM;
		goto bail;
	}
	spin_lock(&fl->hlock);
	vfl->pers_hdr_buf = pers_hdr_buf;
	vfl->num_pers_hdrs = num_pers_hdrs;
	vfl->hdr_bufs = hdr_bufs;
	for (i = 0; i < num_pers_hdrs; i++) {
		buf = &vfl->hdr_bufs[i];
		buf->vfl = vfl;
		buf->va = uint64_to_ptr(va_base + (i * PAGE_SIZE));
		buf->da = pers_hdr_buf->da + (i * PAGE_SIZE);
		buf->size = PAGE_SIZE;
		buf->dma_attr = pers_hdr_buf->dma_attr;
		buf->flags = pers_hdr_buf->flags;
		buf->type = pers_hdr_buf->type;
		buf->pers_hdr_in_use = false;
	}
	spin_unlock(&fl->hlock);
bail:
	if (err) {
		ADSPRPC_ERR(
			"failed to map len %zu, flags %d, user concurrency %u, num headers %u with err %d\n",
			hdr_buf_alloc_len, ADSP_MMAP_PERSIST_HDR,
			user_concurrency, num_pers_hdrs, err);
		vfl->pers_hdr_buf = NULL;
		vfl->hdr_bufs = NULL;
		vfl->num_pers_hdrs = 0;
		if (!IS_ERR_OR_NULL(pers_hdr_buf))
			hfastrpc_buf_free(pers_hdr_buf, 0);
		if (!IS_ERR_OR_NULL(hdr_bufs))
			kfree(hdr_bufs);
	}
	return err;
}

static int hfastrpc_invoke2(struct vfastrpc_file *vfl,
				struct fastrpc_ioctl_invoke2 *inv2, bool is_compat)
{
	union {
		struct fastrpc_ioctl_invoke_async inv;
		struct fastrpc_ioctl_async_response async_res;
		struct fastrpc_proc_sess_info sess_info;
		struct fastrpc_ioctl_notif_rsp notif;
		uint32_t user_concurrency;
	} p;
	struct fastrpc_dsp_capabilities *dsp_cap_ptr = NULL;
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	uint32_t size = 0, msg_type = 0;
	int err = 0, domain = vfl->domain;

	if (inv2->req == FASTRPC_INVOKE2_ASYNC ||
		inv2->req == FASTRPC_INVOKE2_ASYNC_RESPONSE) {
		VERIFY(err, domain == CDSP_DOMAIN_ID || domain == CDSP1_DOMAIN_ID);
		if (err)
			goto bail;

		dsp_cap_ptr = &vfl->apps->channel[domain].dsp_cap_kernel;
		VERIFY(err, dsp_cap_ptr->dsp_attributes[ASYNC_FASTRPC_CAP] == 1);
		if (err) {
			err = -EPROTONOSUPPORT;
			goto bail;
		}
	}
	switch (inv2->req) {
	case FASTRPC_INVOKE2_ASYNC:
		size = sizeof(struct fastrpc_ioctl_invoke_async);
		VERIFY(err, size >= inv2->size);
		if (err) {
			err = -EBADE;
			goto bail;
		}

		K_COPY_FROM_USER(err, is_compat, &p.inv, (void *)inv2->invparam, size);
		if (err)
			goto bail;

		msg_type = (is_compat) ? COMPAT_MSG : USER_MSG;
		VERIFY(err, 0 == (err = hfastrpc_internal_invoke(vfl, fl->mode,
					msg_type, &p.inv)));
		if (err)
			goto bail;
		break;
	case FASTRPC_INVOKE2_ASYNC_RESPONSE:
		VERIFY(err,
		sizeof(struct fastrpc_ioctl_async_response) >= inv2->size);
		if (err) {
			err = -EBADE;
			goto bail;
		}
		err = hfastrpc_get_async_response(&p.async_res,
						(void *)inv2->invparam, vfl);
		break;
	case FASTRPC_INVOKE2_KERNEL_OPTIMIZATIONS:
		size = sizeof(uint32_t);
		if (inv2->size != size) {
			err = -EBADE;
			goto bail;
		}
		K_COPY_FROM_USER(err, 0, &p.user_concurrency,
				(void *)inv2->invparam, size);
		if (err)
			goto bail;
		err = hfastrpc_create_persistent_headers(vfl,
				p.user_concurrency);
		break;
	case FASTRPC_INVOKE2_STATUS_NOTIF:
		VERIFY(err,
		sizeof(struct fastrpc_ioctl_notif_rsp) >= inv2->size);
		if (err) {
			err = -EBADE;
			goto bail;
		}
		err = hfastrpc_get_notif_response(&p.notif,
						(void *)inv2->invparam, vfl);
		break;
	case FASTRPC_INVOKE2_SESS_INFO:
		VERIFY(err,
		sizeof(struct fastrpc_proc_sess_info) >= inv2->size);
		if (err) {
			err = -EBADE;
			goto bail;
		}
		K_COPY_FROM_USER(err, is_compat, &p.sess_info,
					 (void *)inv2->invparam, inv2->size);
		if (err)
			goto bail;
		err = hfastrpc_set_session_info(&p.sess_info,
						(void *)inv2->invparam, vfl);
		break;
	default:
		err = -ENOTTY;
		break;
	}
bail:
	return err;
}

static int hfastrpc_dspsignal_signal(struct vfastrpc_file *vfl,
			     struct fastrpc_ioctl_dspsignal_signal *sig)
{
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	int err = 0, domain = -1;
	struct vfastrpc_channel_ctx *channel_ctx = NULL;
	uint64_t msg = 0;

	// We don't check if the signal has even been allocated since we don't
	// track outgoing signals in the driver. The userspace library does a
	// basic sanity check and any security validation needs to be done by
	// the recipient.
	DSPSIGNAL_VERBOSE("Send signal PID %d, UPID %d, signal %u\n",
			  fl->tgid, vfl->upid, sig->signal_id);
	VERIFY(err, sig->signal_id < DSPSIGNAL_NUM_SIGNALS);
	if (err) {
		ADSPRPC_ERR("Sending bad signal %u for PID %d, UPID %d\n",
			    sig->signal_id, fl->tgid, vfl->upid);
		err = -EBADR;
		goto bail;
	}

	domain = vfl->domain;

	channel_ctx = &vfl->apps->channel[domain];
	mutex_lock(&channel_ctx->smd_mutex);
	if (fl->ssrcount != channel_ctx->ssrcount) {
		err = -ECONNRESET;
		mutex_unlock(&channel_ctx->smd_mutex);
		goto bail;
	}
	mutex_unlock(&channel_ctx->smd_mutex);

	msg = (((uint64_t)vfl->upid) << 32) | ((uint64_t)sig->signal_id);
	err = fastrpc_transport_send(domain, (void *)&msg, sizeof(msg), fl->tvm_remote_domain);

bail:
	return err;
}

static int hfastrpc_dspsignal_wait(struct vfastrpc_file *vfl,
			   struct fastrpc_ioctl_dspsignal_wait *wait)
{
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	int err = 0;
	uint32_t timeout_usec = wait->timeout_usec;
	unsigned long timeout = usecs_to_jiffies(wait->timeout_usec);
	uint32_t signal_id = wait->signal_id;
	struct fastrpc_dspsignal *s = NULL;
	long ret = 0;
	unsigned long irq_flags = 0;

	DSPSIGNAL_VERBOSE("Wait for signal %u\n", signal_id);
	VERIFY(err, signal_id < DSPSIGNAL_NUM_SIGNALS);
	if (err) {
		ADSPRPC_ERR("Waiting on bad signal %u", signal_id);
		err = -EINVAL;
		goto bail;
	}

	spin_lock_irqsave(&fl->dspsignals_lock, irq_flags);
	if (fl->signal_groups[signal_id / DSPSIGNAL_GROUP_SIZE] != NULL) {
		struct fastrpc_dspsignal *group =
			fl->signal_groups[signal_id / DSPSIGNAL_GROUP_SIZE];

		s = &group[signal_id % DSPSIGNAL_GROUP_SIZE];
	}
	if ((s == NULL) || (s->state == DSPSIGNAL_STATE_UNUSED)) {
		spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);
		ADSPRPC_ERR("Unknown signal id %u\n", signal_id);
		err = -ENOENT;
		goto bail;
	}
	if (s->state != DSPSIGNAL_STATE_PENDING) {
		if ((s->state == DSPSIGNAL_STATE_CANCELED) || (s->state == DSPSIGNAL_STATE_UNUSED))
			err = -EINTR;
		spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);
		DSPSIGNAL_VERBOSE("Signal %u in state %u, complete wait immediately",
				  signal_id, s->state);
		goto bail;
	}
	spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);

	if (timeout_usec != 0xffffffff)
		ret = wait_for_completion_interruptible_timeout(&s->comp, timeout);
	else
		ret = wait_for_completion_interruptible(&s->comp);

	if (timeout_usec != 0xffffffff && ret == 0) {
		DSPSIGNAL_VERBOSE("Wait for signal %u timed out %ld us\n",
				signal_id, timeout_usec);
		err = -ETIMEDOUT;
		goto bail;
	} else if (ret < 0) {
		ADSPRPC_ERR("Wait for signal %u failed %d\n", signal_id, (int)ret);
		err = ret;
		goto bail;
	}

	spin_lock_irqsave(&fl->dspsignals_lock, irq_flags);
	if (s->state == DSPSIGNAL_STATE_SIGNALED) {
		s->state = DSPSIGNAL_STATE_PENDING;
		DSPSIGNAL_VERBOSE("Signal %u completed\n", signal_id);
	} else if ((s->state == DSPSIGNAL_STATE_CANCELED) || (s->state == DSPSIGNAL_STATE_UNUSED)) {
		DSPSIGNAL_VERBOSE("Signal %u cancelled or destroyed\n", signal_id);
		err = -EINTR;
	}
	spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);

bail:
	return err;
}

static int hfastrpc_dspsignal_create(struct vfastrpc_file *vfl,
			     struct fastrpc_ioctl_dspsignal_create *create)
{
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	int err = 0;
	uint32_t signal_id = create->signal_id;
	struct fastrpc_dspsignal *group, *sig;
	unsigned long irq_flags = 0;

	VERIFY(err, signal_id < DSPSIGNAL_NUM_SIGNALS);
	if (err) {
		err = -EINVAL;
		goto bail;
	}

	// Use a separate mutex for creating signals. This avoids holding on
	// to a spinlock if we need to allocate a whole group of signals. The
	// mutex ensures nobody else will allocate the same group.
	mutex_lock(&fl->signal_create_mutex);
	spin_lock_irqsave(&fl->dspsignals_lock, irq_flags);

	group = fl->signal_groups[signal_id / DSPSIGNAL_GROUP_SIZE];
	if (group == NULL) {
		int i;
		// Release the spinlock while we allocate a new group but take
		// it back before taking the group into use. No other code
		// allocates groups so the mutex is sufficient.
		spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);
		VERIFY(err, (group = kzalloc(DSPSIGNAL_GROUP_SIZE * sizeof(*group),
					     GFP_KERNEL)) != NULL);
		if (err) {
			ADSPRPC_ERR("Unable to allocate signal group\n");
			err = -ENOMEM;
			mutex_unlock(&fl->signal_create_mutex);
			goto bail;
		}

		for (i = 0; i < DSPSIGNAL_GROUP_SIZE; i++) {
			sig = &group[i];
			init_completion(&sig->comp);
			sig->state = DSPSIGNAL_STATE_UNUSED;
		}
		spin_lock_irqsave(&fl->dspsignals_lock, irq_flags);
		fl->signal_groups[signal_id / DSPSIGNAL_GROUP_SIZE] = group;
	}

	sig = &group[signal_id % DSPSIGNAL_GROUP_SIZE];
	if (sig->state != DSPSIGNAL_STATE_UNUSED) {
		err = -EBUSY;
		spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);
		mutex_unlock(&fl->signal_create_mutex);
		ADSPRPC_ERR("Attempting to create signal %u already in use (state %u)\n",
			    signal_id, sig->state);
		goto bail;
	}

	sig->state = DSPSIGNAL_STATE_PENDING;
	reinit_completion(&sig->comp);

	spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);
	mutex_unlock(&fl->signal_create_mutex);

	DSPSIGNAL_VERBOSE("Signal %u created\n", signal_id);

bail:
	return err;
}

static int hfastrpc_dspsignal_destroy(struct vfastrpc_file *vfl,
			      struct fastrpc_ioctl_dspsignal_destroy *destroy)
{
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	int err = 0;
	uint32_t signal_id = destroy->signal_id;
	struct fastrpc_dspsignal *s = NULL;
	unsigned long irq_flags = 0;

	DSPSIGNAL_VERBOSE("Destroy signal %u\n", signal_id);

	VERIFY(err, signal_id < DSPSIGNAL_NUM_SIGNALS);
	if (err) {
		err = -EINVAL;
		goto bail;
	}

	spin_lock_irqsave(&fl->dspsignals_lock, irq_flags);

	if (fl->signal_groups[signal_id / DSPSIGNAL_GROUP_SIZE] != NULL) {
		struct fastrpc_dspsignal *group =
			fl->signal_groups[signal_id / DSPSIGNAL_GROUP_SIZE];

		s = &group[signal_id % DSPSIGNAL_GROUP_SIZE];
	}
	if ((s == NULL) || (s->state == DSPSIGNAL_STATE_UNUSED)) {
		spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);
		ADSPRPC_ERR("Attempting to destroy unused signal %u\n", signal_id);
		err = -ENOENT;
		goto bail;
	}

	s->state = DSPSIGNAL_STATE_UNUSED;
	complete_all(&s->comp);

	spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);
	DSPSIGNAL_VERBOSE("Signal %u destroyed\n", signal_id);

bail:
	return err;
}

static int hfastrpc_dspsignal_cancel_wait(struct vfastrpc_file *vfl,
				  struct fastrpc_ioctl_dspsignal_cancel_wait *cancel)
{
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	int err = 0;
	uint32_t signal_id = cancel->signal_id;
	struct fastrpc_dspsignal *s = NULL;
	unsigned long irq_flags = 0;

	DSPSIGNAL_VERBOSE("Cancel wait for signal %u\n", signal_id);

	VERIFY(err, signal_id < DSPSIGNAL_NUM_SIGNALS);
	if (err) {
		err = -EINVAL;
		goto bail;
	}

	spin_lock_irqsave(&fl->dspsignals_lock, irq_flags);

	if (fl->signal_groups[signal_id / DSPSIGNAL_GROUP_SIZE] != NULL) {
		struct fastrpc_dspsignal *group =
			fl->signal_groups[signal_id / DSPSIGNAL_GROUP_SIZE];

		s = &group[signal_id % DSPSIGNAL_GROUP_SIZE];
	}
	if ((s == NULL) || (s->state == DSPSIGNAL_STATE_UNUSED)) {
		spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);
		ADSPRPC_ERR("Attempting to cancel unused signal %u\n", signal_id);
		err = -ENOENT;
		goto bail;
	}

	if (s->state != DSPSIGNAL_STATE_CANCELED) {
		s->state = DSPSIGNAL_STATE_CANCELED;
		complete_all(&s->comp);
	}

	spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);

	DSPSIGNAL_VERBOSE("Signal %u cancelled\n", signal_id);

bail:
	return err;
}

const struct vfastrpc_operations hfrpc_ops = {
	.get_info = hfastrpc_get_info,
	.get_info_from_kernel = hfastrpc_get_info_from_kernel,
	.control = hfastrpc_control,
	.init_process = hfastrpc_init_process,
	.mmap = hfastrpc_mmap,
	.munmap = hfastrpc_munmap,
	.munmap_fd = hfastrpc_munmap_fd,
	.mem_map = hfastrpc_mem_map,
	.mem_unmap = hfastrpc_mem_unmap,
	.setmode = hfastrpc_setmode,
	.invoke = hfastrpc_invoke,
	.invoke2 = hfastrpc_invoke2,
	.dspsignal_cancel_wait = hfastrpc_dspsignal_cancel_wait,
	.dspsignal_wait = hfastrpc_dspsignal_wait,
	.dspsignal_signal = hfastrpc_dspsignal_signal,
	.dspsignal_create = hfastrpc_dspsignal_create,
	.dspsignal_destroy = hfastrpc_dspsignal_destroy,
};
