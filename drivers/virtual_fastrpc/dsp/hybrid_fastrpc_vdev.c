// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023-2025, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/debugfs.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/pm_qos.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/virtio_config.h>
#include <linux/uaccess.h>
#include <linux/of.h>
#include <linux/remoteproc/qcom_rproc.h>
#include <linux/version.h>
#include "virtio_fastrpc_mem.h"
#include "virtio_fastrpc_queue.h"
#define CREATE_TRACE_POINTS
#include "virtio_fastrpc_trace.h"
#include "fastrpc_trace.h"
#include "fastrpc_ioctl.h"
#include "hybrid_fastrpc_core.h"

/* Virtio ID of FASTRPC : 0xC005 */
#define VIRTIO_ID_HYBRID_FASTRPC			49163
/* indicates remote invoke with buffer attributes is supported */
#define VIRTIO_FASTRPC_F_INVOKE_ATTR			1
/* indicates remote invoke with CRC is supported */
#define VIRTIO_FASTRPC_F_INVOKE_CRC			2
/* indicates remote mmap/munmap is supported */
#define VIRTIO_FASTRPC_F_MMAP				3
/* indicates QOS setting is supported */
#define VIRTIO_FASTRPC_F_CONTROL			4
/* indicates version is available in config space */
#define VIRTIO_FASTRPC_F_VERSION			5
/* indicates domain num is available in config space */
#define VIRTIO_FASTRPC_F_DOMAIN_NUM			6
#define VIRTIO_FASTRPC_F_VQUEUE_SETTING			7
/* indicates fastrpc_mmap/fastrpc_munmap is supported */
#define VIRTIO_FASTRPC_F_MEM_MAP			8
/* indicates fastrpc_mmap/fastrpc_munmap is supported */
#define VIRTIO_FASTRPC_F_HYBRID				9

#define MAX_FASTRPC_BUF_SIZE		(1024*1024*4)
#define DEF_FASTRPC_BUF_SIZE		(128*1024)
#define DEBUGFS_SIZE			3072

#define NUM_DEVICES			2

#define FASTRPC_CPUINFO_DEFAULT		0
#define FASTRPC_CPUINFO_EARLY_WAKEUP	1

#define SESSION_ID_INDEX		30
#define SESSION_ID_MASK			(1 << SESSION_ID_INDEX)
#define PROCESS_ID_MASK			((2^SESSION_ID_INDEX) - 1)

#define FASTRPC_CTX_MAGIC		0xbeeddeed
#define FASTRPC_NOTIF_CTX_RESERVED	0xABCDABCD
#define FASTRPC_RSP_VERSION2		2
#define FASTRPC_CTX_JOB_TYPE_POS (4)
#define FASTRPC_CTX_TABLE_IDX_POS (6)
#define FASTRPC_CTX_JOBID_POS (16)
#define FASTRPC_CTX_TABLE_IDX_MASK \
	((FASTRPC_CTX_MAX - 1) << FASTRPC_CTX_TABLE_IDX_POS)
#define FASTRPC_ASYNC_JOB_MASK   (1)

#define GET_TABLE_IDX_FROM_CTXID(ctxid) \
	((ctxid & FASTRPC_CTX_TABLE_IDX_MASK) >> FASTRPC_CTX_TABLE_IDX_POS)

/*
 * ctxid of every message is OR-ed with fl->pd (0/1/2) before
 * it is sent to DSP. So mask 2 LSBs to retrieve actual context
 */
#define CONTEXT_PD_CHECK (3)
#define GET_CTXID_FROM_RSP_CTX(rsp_ctx) (rsp_ctx & ~CONTEXT_PD_CHECK)

/* Convert the 19.2MHz clock count to micro-seconds */
#define CONVERT_CNT_TO_US(CNT) (CNT * 10ull / 192ull)

/*
 * FE_MAJOR_VER is used for the FE and BE's version match check,
 * and it MUST be equal to BE_MAJOR_VER, otherwise virtual fastrpc
 * cannot work properly. It increases when fundamental protocol is
 * changed between FE and BE.
 */
#define FE_MAJOR_VER 0x1
/* FE_MINOR_VER is used to track patches in this driver. It does not
 * need to be matched with BE_MINOR_VER. And it will return to 0 when
 * FE_MAJOR_VER is increased.
 */
#define FE_MINOR_VER 0x7
#define FE_VERSION (FE_MAJOR_VER << 16 | FE_MINOR_VER)
#define BE_MAJOR_VER(ver) (((ver) >> 16) & 0xffff)

struct hfastrpc_config {
	u32 version;
	u32 domain_num;
	u32 max_buf_size;
} __packed;

/* FastRPC remote subsystem state*/
enum fastrpc_remote_subsys_state {
	SUBSYSTEM_RESTARTING = 0,
	SUBSYSTEM_DOWN,
	SUBSYSTEM_UP,
};

static struct vfastrpc_apps vfa;
static struct fastrpc_apps fa;

static struct vfastrpc_channel_ctx gcinfo[NUM_CHANNELS] = {
	{
		.name = "adsprpc-smd",
		.subsys = "lpass",
		.cpuinfo_todsp = FASTRPC_CPUINFO_DEFAULT,
		.cpuinfo_status = false,
	},
	{
		.name = "mdsprpc-smd",
		.subsys = "mpss",
		.cpuinfo_todsp = FASTRPC_CPUINFO_DEFAULT,
		.cpuinfo_status = false,
	},
	{
		.name = "sdsprpc-smd",
		.subsys = "dsps",
		.cpuinfo_todsp = FASTRPC_CPUINFO_DEFAULT,
		.cpuinfo_status = false,
	},
	{
		.name = "cdsprpc-smd",
		.subsys = "cdsp",
		.cpuinfo_todsp = FASTRPC_CPUINFO_EARLY_WAKEUP,
		.cpuinfo_status = false,
	},
	{
		.name = "cdsprpc1-smd",
		.subsys = "cdsp1",
		.cpuinfo_todsp = FASTRPC_CPUINFO_EARLY_WAKEUP,
		.cpuinfo_status = false,
	},
};

static inline int64_t get_timestamp_in_ns(void)
{
	int64_t ns = 0;
	struct timespec64 ts;

	ktime_get_real_ts64(&ts);
	ns = timespec64_to_ns(&ts);
	return ns;
}

static ssize_t hfastrpc_debugfs_read(struct file *filp, char __user *buffer,
					 size_t count, loff_t *position)
{
	struct fastrpc_file *fl = filp->private_data;
	struct vfastrpc_file *vfl = to_vfastrpc_file(fl);
	struct vfastrpc_mmap *map = NULL;
	struct vfastrpc_buf *buf = NULL;
	struct hlist_node *n;
	struct vfastrpc_invoke_ctx *ictx = NULL;
	char *fileinfo = NULL;
	unsigned int len = 0;
	int err = 0;
	char title[] = "=========================";

	/* Only allow read once */
	if (*position != 0)
		goto bail;

	fileinfo = kzalloc(DEBUGFS_SIZE, GFP_KERNEL);
	if (!fileinfo) {
		err = -ENOMEM;
		goto bail;
	}
	if (fl && vfl) {
		len += scnprintf(fileinfo + len, DEBUGFS_SIZE - len,
				"\n%s %d %s %d\n", "channel =", vfl->domain,
				"proc_attr =", vfl->procattrs);

		len += scnprintf(fileinfo + len, DEBUGFS_SIZE - len,
			"\n========%s %s %s========\n", title,
			" SESSION INFO ", title);
		len += scnprintf(fileinfo + len, DEBUGFS_SIZE - len,
				"\n%s %d %s %d %s 0x%x\n", "tgid_frpc =",
				fl->tgid_frpc, "sessionid =", fl->sessionid,
				"upid =", vfl->upid);
		len += scnprintf(fileinfo + len, DEBUGFS_SIZE - len,
				"\n%s %d\n", "file_close =", fl->file_close);

		len += scnprintf(fileinfo + len, DEBUGFS_SIZE - len,
			"\n========%s %s %s========\n", title,
			" LIST OF BUFS ", title);
		spin_lock(&fl->hlock);
		len += scnprintf(fileinfo + len, DEBUGFS_SIZE - len,
			"%-19s|%-19s|%-19s\n\n",
			"virt", "phys", "size");
		hlist_for_each_entry_safe(buf, n, &fl->cached_bufs, hn) {
			len += scnprintf(fileinfo + len,
				DEBUGFS_SIZE - len,
				"0x%-17lX|0x%-17llX|%-19zu\n",
				(unsigned long)buf->va,
				(uint64_t)page_to_phys(buf->pages[0]),
				buf->size);
		}

		len += scnprintf(fileinfo + len, DEBUGFS_SIZE - len,
			"\n==%s %s %s==\n", title,
			" LIST OF PENDING CONTEXTS ", title);
		len += scnprintf(fileinfo + len, DEBUGFS_SIZE - len,
			"%-20s|%-10s|%-10s|%-10s|%-20s\n\n",
			"sc", "pid", "tgid", "size", "handle");
		hlist_for_each_entry_safe(ictx, n, &fl->clst.pending, hn) {
			len += scnprintf(fileinfo + len, DEBUGFS_SIZE - len,
				"0x%-18X|%-10d|%-10d|%-10zu|0x%-20X\n\n",
				ictx->sc, ictx->pid, ictx->tgid,
				ictx->size, ictx->handle);
		}

		len += scnprintf(fileinfo + len, DEBUGFS_SIZE - len,
			"\n%s %s %s\n", title,
			" LIST OF INTERRUPTED CONTEXTS ", title);
		len += scnprintf(fileinfo + len, DEBUGFS_SIZE - len,
			"%-20s|%-10s|%-10s|%-10s|%-20s\n\n",
			"sc", "pid", "tgid", "size", "handle");
		hlist_for_each_entry_safe(ictx, n, &fl->clst.interrupted, hn) {
			len += scnprintf(fileinfo + len, DEBUGFS_SIZE - len,
				"0x%-18X|%-10d|%-10d|%-10zu|0x%-20X\n\n",
				ictx->sc, ictx->pid, ictx->tgid,
				ictx->size, ictx->handle);
		}
		spin_unlock(&fl->hlock);

		len += scnprintf(fileinfo + len, DEBUGFS_SIZE - len,
			"\n========%s %s %s========\n", title,
			" LIST OF MAPS ", title);
		len += scnprintf(fileinfo + len, DEBUGFS_SIZE - len,
			"%-20s|%-20s|%-10s|%-10s|%-10s\n\n",
			"va", "phys", "size", "attr", "refs");
		mutex_lock(&fl->map_mutex);
		hlist_for_each_entry_safe(map, n, &fl->maps, hn) {
			len += scnprintf(fileinfo + len, DEBUGFS_SIZE - len,
				"0x%-18lX|0x%-18llX|%-10zu|0x%-10x|%-10d\n\n",
				map->va, map->phys, map->size,
				map->attr, map->refs);
		}
		mutex_unlock(&fl->map_mutex);
	}

	if (len > DEBUGFS_SIZE)
		len = DEBUGFS_SIZE;

	err = simple_read_from_buffer(buffer, count, position, fileinfo, len);
	kfree(fileinfo);
bail:
	return err;
}

static const struct file_operations debugfs_fops = {
	.open = simple_open,
	.read = hfastrpc_debugfs_read,
};

static int hfastrpc_open(struct inode *inode, struct file *filp)
{
	int err = 0;
	struct vfastrpc_file *vfl = NULL;
	struct fastrpc_file *fl = NULL;
	struct vfastrpc_apps *me = &vfa;
	unsigned long irq_flags = 0;

	/*
	 * Indicates the device node opened
	 * MINOR_NUM_DEV or MINOR_NUM_SECURE_DEV
	 */
	int dev_minor = MINOR(inode->i_rdev);

	VERIFY(err, ((dev_minor == MINOR_NUM_DEV) ||
			(dev_minor == MINOR_NUM_SECURE_DEV)));
	if (err) {
		dev_err(me->dev, "Invalid dev minor num %d\n", dev_minor);
		return err;
	}

	VERIFY(err, NULL != (vfl = hfastrpc_file_alloc(&hfrpc_ops)));
	if (err) {
		dev_err(me->dev, "Allocate vfastrpc_file failed %d\n", dev_minor);
		return err;
	}

	fl = to_fastrpc_file(vfl);
	fl->dev_minor = dev_minor;
	vfl->apps = me;
	fl->apps = &fa;

	spin_lock_irqsave(&me->hlock, irq_flags);
	hlist_add_head(&fl->hn, &me->drivers);
	spin_unlock_irqrestore(&me->hlock, irq_flags);

	filp->private_data = fl;
	return 0;
}

static int hfastrpc_release(struct inode *inode, struct file *file)
{
	struct fastrpc_file *fl = (struct fastrpc_file *)file->private_data;
	struct vfastrpc_file *vfl = to_vfastrpc_file(fl);

	if (vfl) {
		hfastrpc_file_free(vfl);
		file->private_data = NULL;
	}
	return 0;
}

static const struct file_operations fops = {
	.open = hfastrpc_open,
	.release = hfastrpc_release,
	.unlocked_ioctl = vfastrpc_ioctl,
	.compat_ioctl = compat_fastrpc_device_ioctl,
};

static void handle_remote_signal(uint64_t msg, int domain)
{
	struct vfastrpc_apps *me = &vfa;
	uint32_t pid = msg >> 32;
	uint32_t signal_id = msg & 0xffffffff;
	struct fastrpc_file *fl = NULL;
	struct vfastrpc_file *vfl = NULL;
	struct hlist_node *n = NULL;
	unsigned long irq_flags = 0;

	DSPSIGNAL_VERBOSE("Received queue signal %llx: PID %u, signal %u\n", msg, pid, signal_id);

	if (signal_id >= DSPSIGNAL_NUM_SIGNALS) {
		ADSPRPC_ERR("Received bad signal %u for PID %u\n", signal_id, pid);
		return;
	}

	spin_lock_irqsave(&me->hlock, irq_flags);
	hlist_for_each_entry_safe(fl, n, &me->drivers, hn) {
		vfl = to_vfastrpc_file(fl);
		if ((vfl->upid == pid) && (vfl->domain == domain)) {
			unsigned long fflags = 0;

			spin_lock_irqsave(&fl->dspsignals_lock, fflags);
			if (fl->signal_groups[signal_id / DSPSIGNAL_GROUP_SIZE]) {
				struct fastrpc_dspsignal *group =
					fl->signal_groups[signal_id / DSPSIGNAL_GROUP_SIZE];
				struct fastrpc_dspsignal *sig =
					&group[signal_id % DSPSIGNAL_GROUP_SIZE];

				if ((sig->state == DSPSIGNAL_STATE_PENDING) ||
				    (sig->state == DSPSIGNAL_STATE_SIGNALED)) {
					DSPSIGNAL_VERBOSE("Signaling signal %u for PID %u\n",
							  signal_id, pid);
					complete(&sig->comp);
					sig->state = DSPSIGNAL_STATE_SIGNALED;
				} else if (sig->state == DSPSIGNAL_STATE_UNUSED) {
					ADSPRPC_ERR("Received unknown signal %u for PID %u\n",
						    signal_id, pid);
				}
			} else {
				ADSPRPC_ERR("Received unknown signal %u for PID %u\n",
					    signal_id, pid);
			}
			spin_unlock_irqrestore(&fl->dspsignals_lock, fflags);
			break;
		}
	}
	spin_unlock_irqrestore(&me->hlock, irq_flags);
}

static void fastrpc_queue_pd_status(struct fastrpc_file *fl, int domain, int status, int sessionid)
{
	struct smq_notif_rsp *notif_rsp = NULL;
	unsigned long flags;
	int err = 0;

	VERIFY(err, NULL != (notif_rsp = kzalloc(sizeof(*notif_rsp), GFP_ATOMIC)));
	if (err) {
		ADSPRPC_ERR(
			"allocation failed for size 0x%zx\n",
								sizeof(*notif_rsp));
		return;
	}
	notif_rsp->status = status;
	notif_rsp->domain = domain;
	notif_rsp->session = sessionid;

	spin_lock_irqsave(&fl->proc_state_notif.nqlock, flags);
	list_add_tail(&notif_rsp->notifn, &fl->clst.notif_queue);
	atomic_add(1, &fl->proc_state_notif.notif_queue_count);
	wake_up_interruptible(&fl->proc_state_notif.notif_wait_queue);
	spin_unlock_irqrestore(&fl->proc_state_notif.nqlock, flags);
}

static void fastrpc_notif_find_process(int domain, struct smq_notif_rspv3 *notif)
{
	struct vfastrpc_apps *me = &vfa;
	struct fastrpc_file *fl = NULL;
	struct vfastrpc_file *vfl = NULL;
	struct hlist_node *n;
	bool is_process_found = false;
	unsigned long irq_flags = 0;

	ADSPRPC_DEBUG("Received PD status %d for UPID %d\n", notif->status, notif->pid);
	spin_lock_irqsave(&me->hlock, irq_flags);
	hlist_for_each_entry_safe(fl, n, &me->drivers, hn) {
		vfl = to_vfastrpc_file(fl);
		if (vfl->upid == notif->pid) {
			is_process_found = true;
			break;
		}
	}
	spin_unlock_irqrestore(&me->hlock, irq_flags);

	if (!is_process_found)
		return;
	fastrpc_queue_pd_status(fl, domain, notif->status, fl->sessionid);
}

static inline void fastrpc_update_rxmsg_buf(struct vfastrpc_channel_ctx *chan,
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

static void fastrpc_queue_completed_async_job(struct vfastrpc_invoke_ctx *ctx)
{
	struct vfastrpc_file *vfl = ctx->vfl;
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	unsigned long flags;

	spin_lock_irqsave(&fl->aqlock, flags);
	if (ctx->is_early_wakeup)
		goto bail;
	if (!hlist_unhashed(&ctx->asyncn)) {
		hlist_add_head(&ctx->asyncn, &fl->clst.async_queue);
		atomic_add(1, &fl->async_queue_job_count);
		ctx->is_early_wakeup = true;
		wake_up_interruptible(&fl->async_wait_queue);
	}
bail:
	spin_unlock_irqrestore(&fl->aqlock, flags);
}

static void context_notify_user(struct vfastrpc_invoke_ctx *ctx,
		int retval, uint32_t rsp_flags, uint32_t early_wake_time)
{
	ctx->retval = retval;
	ctx->rsp_flags = (enum fastrpc_response_flags)rsp_flags;
	switch (rsp_flags) {
	case NORMAL_RESPONSE:
		fallthrough;
	case COMPLETE_SIGNAL:
		/* normal and complete response with return value */
		ctx->is_work_done = true;
		if (ctx->asyncjob.isasyncjob)
			fastrpc_queue_completed_async_job(ctx);
		complete(&ctx->work);
		break;
	case USER_EARLY_SIGNAL:
		/* user hint of approximate time of completion */
		ctx->early_wake_time = early_wake_time;
		if (ctx->asyncjob.isasyncjob)
			break;
		fallthrough;
	case EARLY_RESPONSE:
		/* rpc framework early response with return value */
		if (ctx->asyncjob.isasyncjob)
			fastrpc_queue_completed_async_job(ctx);
		else {
			complete(&ctx->work);
		}
		break;
	default:
		break;
	}
}

int fastrpc_handle_rpc_response(void *data, int len, int domain)
{
	struct smq_invoke_rsp *rsp = (struct smq_invoke_rsp *)data;
	struct smq_notif_rspv3 *notif = (struct smq_notif_rspv3 *)data;
	struct smq_invoke_rspv2 *rspv2 = NULL;
	struct vfastrpc_invoke_ctx *ctx = NULL;
	struct vfastrpc_apps *me = &vfa;
	uint32_t index, rsp_flags = 0, early_wake_time = 0, ver = 0;
	int err = 0, ignore_rsp_err = 0;
	struct vfastrpc_channel_ctx *chan = NULL;
	unsigned long irq_flags = 0;
	int64_t ns = 0;
	uint64_t xo_time_in_us = 0;

	ADSPRPC_DEBUG("Received RSP from domain %d, len %d\n",
			domain, len);
	xo_time_in_us = CONVERT_CNT_TO_US(__arch_counter_get_cntvct());

	if (len == sizeof(uint64_t)) {
		/*
		 * dspsignal message from the DSP
		 */
		handle_remote_signal(*((uint64_t *)data), domain);
		goto bail;
	}

	chan = &vfa.channel[domain];
	VERIFY(err, (rsp && len >= sizeof(*rsp)));
	if (err) {
		err = -EINVAL;
		goto bail;
	}

	if (notif->ctx == FASTRPC_NOTIF_CTX_RESERVED) {
		VERIFY(err, (notif->type == STATUS_RESPONSE &&
					 len >= sizeof(*notif)));
		if (err)
			goto bail;
		fastrpc_notif_find_process(domain, notif);
		goto bail;
	}

	if (len >= sizeof(struct smq_invoke_rspv2))
		rspv2 = (struct smq_invoke_rspv2 *)data;

	if (rspv2) {
		early_wake_time = rspv2->early_wake_time;
		rsp_flags = rspv2->flags;
		ver = rspv2->version;
	}
	ns = get_timestamp_in_ns();
	fastrpc_update_rxmsg_buf(chan, rsp->ctx, rsp->retval,
		rsp_flags, early_wake_time, ver, ns, xo_time_in_us);

	index = (uint32_t)GET_TABLE_IDX_FROM_CTXID(rsp->ctx);
	VERIFY(err, index < FASTRPC_CTX_MAX);
	if (err)
		goto bail;

	spin_lock_irqsave(&chan->ctxlock, irq_flags);
	ctx = chan->ctxtable[index];
	VERIFY(err, !IS_ERR_OR_NULL(ctx) &&
		(ctx->ctxid == GET_CTXID_FROM_RSP_CTX(rsp->ctx)) &&
		ctx->magic == FASTRPC_CTX_MAGIC);
	if (err) {
		/*
		 * Received an anticipatory COMPLETE_SIGNAL from DSP for a
		 * context after CPU successfully polling on memory and
		 * completed processing of context. Ignore the message.
		 * Also ignore response for a call which was already
		 * completed by update of poll memory and the context was
		 * removed from the table and possibly reused for another call.
		 */
		ignore_rsp_err = ((rsp_flags == COMPLETE_SIGNAL) || !ctx ||
			(ctx && (ctx->ctxid != GET_CTXID_FROM_RSP_CTX(rsp->ctx)))) ? 1 : 0;
		goto bail_unlock;
	}

	if (rspv2) {
		VERIFY(err, rspv2->version == FASTRPC_RSP_VERSION2);
		if (err)
			goto bail_unlock;
	}
	context_notify_user(ctx, rsp->retval, rsp_flags, early_wake_time);
bail_unlock:
	spin_unlock_irqrestore(&chan->ctxlock, irq_flags);
bail:
	if (err) {
		err = -ENOKEY;
		if (!ignore_rsp_err)
			ADSPRPC_ERR(
				"invalid response data %pK, len %d from remote subsystem err %d\n",
				data, len, err);
		else {
			err = 0;
			me->duplicate_rsp_err_cnt++;
		}
	}

	return err;
}

static int recv_single(struct virt_msg_hdr *rsp, unsigned int len)
{
	struct vfastrpc_apps *me = &vfa;
	struct virt_fastrpc_msg *msg;

	if (len != rsp->len) {
		dev_err(me->dev, "msg %u len mismatch,expected %u but %d found\n",
				rsp->cmd, rsp->len, len);
		return -EINVAL;
	}
	spin_lock(&me->msglock);
	msg = me->msgtable[rsp->msgid];
	spin_unlock(&me->msglock);

	if (!msg) {
		dev_err(me->dev, "msg %u already free in table[%u]\n",
				rsp->cmd, rsp->msgid);
		return -EINVAL;
	}
	msg->rxbuf = (void *)rsp;

	complete(&msg->work);

	return 0;
}

static void fastrpc_vq_callback(struct virtqueue *rvq)
{

	struct vfastrpc_apps *me = &vfa;
	struct virt_msg_hdr *rsp;
	unsigned int len, msgs_received = 0;
	int err;
	unsigned long flags;

	spin_lock_irqsave(&me->rvq.vq_lock, flags);
	rsp = virtqueue_get_buf(rvq, &len);
	if (!rsp) {
		spin_unlock_irqrestore(&me->rvq.vq_lock, flags);
		dev_err(me->dev, "incoming signal, but no used buffer\n");
		return;
	}
	spin_unlock_irqrestore(&me->rvq.vq_lock, flags);

	while (rsp) {
		err = recv_single(rsp, len);
		if (err)
			break;

		msgs_received++;

		spin_lock_irqsave(&me->rvq.vq_lock, flags);
		rsp = virtqueue_get_buf(rvq, &len);
		spin_unlock_irqrestore(&me->rvq.vq_lock, flags);
	}
}

static void virt_init_vq(struct virt_fastrpc_vq *fastrpc_vq,
				struct virtqueue *vq)
{
	spin_lock_init(&fastrpc_vq->vq_lock);
	fastrpc_vq->vq = vq;
}

static void hfastrpc_unused_tx_bufs_list_free(struct vfastrpc_apps *me)
{
	struct vfastrpc_vqbuf *vtxbuf, *free;
	struct hlist_node *n;

	if (hlist_empty(&me->unused_tx_bufs))
		return;
	do {
		free = NULL;
		hlist_for_each_entry_safe(vtxbuf, n, &me->unused_tx_bufs, hn) {
			free = vtxbuf;
			hlist_del_init(&vtxbuf->hn);
			break;
		}
		if (free)
			kfree(free);
	} while(free);
}

static int init_vqs(struct vfastrpc_apps *me)
{
	struct virtqueue *vqs[2];
	int err, i;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
	struct virtqueue_info vqs_info[] = {
		{"tx", NULL },
		{"rx", fastrpc_vq_callback },
	};

	err = virtio_find_vqs(me->vdev, 2, vqs, vqs_info, NULL);
#else
	static const char * const names[] = { "tx", "rx" };
	vq_callback_t *cbs[] = { NULL, fastrpc_vq_callback };

	err = virtio_find_vqs(me->vdev, 2, vqs, cbs, names, NULL);
#endif
	if (err)
		return err;

	virt_init_vq(&me->svq, vqs[0]);
	virt_init_vq(&me->rvq, vqs[1]);


	/* we expect symmetric tx/rx vrings */
	if (virtqueue_get_vring_size(me->rvq.vq) !=
			virtqueue_get_vring_size(me->svq.vq)) {
		dev_err(&me->vdev->dev, "tx/rx vring size does not match\n");
			err = -EINVAL;
		goto vqs_del;
	}

	me->num_bufs = virtqueue_get_vring_size(me->rvq.vq);
	me->rbufs = kcalloc(me->num_bufs, sizeof(void *), GFP_KERNEL);
	if (!me->rbufs) {
		err = -ENOMEM;
		goto vqs_del;
	}
	me->sbufs = kcalloc(me->num_bufs, sizeof(void *), GFP_KERNEL);
	if (!me->sbufs) {
		err = -ENOMEM;
		kfree(me->rbufs);
		goto vqs_del;
	}

	me->order = get_order(me->buf_size);

	for (i = 0; i < me->num_bufs; i++) {
		me->rbufs[i] = (void *)__get_free_pages(GFP_KERNEL, me->order);
		if (!me->rbufs[i]) {
			err = -ENOMEM;
			goto rbuf_del;
		}
	}

	for (i = 0; i < me->num_bufs; i++) {
		me->sbufs[i] = (void *)__get_free_pages(GFP_KERNEL, me->order);
		if (!me->sbufs[i]) {
			err = -ENOMEM;
			goto sbuf_del;
		}
	}

	INIT_HLIST_HEAD(&me->unused_tx_bufs);
	for (i = 0; i < me->num_bufs; i++) {
		err = put_a_tx_buf(me, me->sbufs[i]);
		if (err) {
			goto list_del;
		}
	}
	return 0;
list_del:
	hfastrpc_unused_tx_bufs_list_free(me);
sbuf_del:
	for (i = 0; i < me->num_bufs; i++) {
		if (me->sbufs[i])
			free_pages((unsigned long)me->sbufs[i], me->order);
	}

rbuf_del:
	for (i = 0; i < me->num_bufs; i++) {
		if (me->rbufs[i])
			free_pages((unsigned long)me->rbufs[i], me->order);
	}
	kfree(me->sbufs);
	kfree(me->rbufs);
vqs_del:
	me->vdev->config->del_vqs(me->vdev);
	return err;
}

static void fastrpc_notify_users(struct fastrpc_file *fl)
{
	struct vfastrpc_invoke_ctx *ictx;
	struct hlist_node *n;
	unsigned long irq_flags = 0;

	spin_lock_irqsave(&fl->hlock, irq_flags);
	hlist_for_each_entry_safe(ictx, n, &fl->clst.pending, hn) {
		ictx->is_work_done = true;
		ictx->retval = -ECONNRESET;
		complete(&ictx->work);
	}
	hlist_for_each_entry_safe(ictx, n, &fl->clst.interrupted, hn) {
		ictx->is_work_done = true;
		ictx->retval = -ECONNRESET;
		complete(&ictx->work);
	}
	spin_unlock_irqrestore(&fl->hlock, irq_flags);
}

static void fastrpc_notify_drivers(struct vfastrpc_apps *vme, int domain)
{
	struct fastrpc_file *fl;
	struct hlist_node *n;
	unsigned long irq_flags = 0;

	spin_lock_irqsave(&vme->hlock, irq_flags);
	hlist_for_each_entry_safe(fl, n, &vme->drivers, hn) {
		if (to_vfastrpc_file(fl)->domain == domain) {
			fastrpc_queue_pd_status(fl, domain, FASTRPC_DSP_SSR, fl->sessionid);
			fastrpc_notify_users(fl);
		}
	}
	spin_unlock_irqrestore(&vme->hlock, irq_flags);
}

static int fastrpc_restart_notifier_cb(struct notifier_block *nb,
					unsigned long code,
					void *data)
{
	struct vfastrpc_apps *vme = &vfa;
	struct vfastrpc_channel_ctx *ctx;
	int domain = -1;

	ctx = container_of(nb, struct vfastrpc_channel_ctx, nb);
	domain = ctx - &vme->channel[0];
	switch (code) {
	case QCOM_SSR_BEFORE_SHUTDOWN:
		ADSPRPC_INFO("subsystem %s is restarting\n", gcinfo[domain].subsys);
		mutex_lock(&vme->channel[domain].smd_mutex);
		ctx->ssrcount++;
		ctx->subsystemstate = SUBSYSTEM_RESTARTING;
		mutex_unlock(&vme->channel[domain].smd_mutex);
		break;
	case QCOM_SSR_AFTER_SHUTDOWN:
		ADSPRPC_INFO("subsystem %s is down\n", gcinfo[domain].subsys);
		mutex_lock(&vme->channel[domain].smd_mutex);
		ctx->subsystemstate = SUBSYSTEM_DOWN;
		mutex_unlock(&vme->channel[domain].smd_mutex);
		break;
	case QCOM_SSR_BEFORE_POWERUP:
		ADSPRPC_INFO("subsystem %s is about to start\n", gcinfo[domain].subsys);
		fastrpc_notify_drivers(vme, domain);
		break;
	case QCOM_SSR_AFTER_POWERUP:
		ADSPRPC_INFO("subsystem %s is up\n", gcinfo[domain].subsys);
		mutex_lock(&vme->channel[domain].smd_mutex);
		ctx->subsystemstate = SUBSYSTEM_UP;
		mutex_unlock(&vme->channel[domain].smd_mutex);
		break;
	default:
		break;
	}
	return NOTIFY_DONE;
}

static int hfastrpc_init(void)
{
	int i, err = 0;
	struct fastrpc_apps *me = &fa;
	struct vfastrpc_channel_ctx *chan = &gcinfo[0];

	memset(me, 0, sizeof(*me));
	INIT_HLIST_HEAD(&me->drivers);
	INIT_HLIST_HEAD(&me->maps);
	spin_lock_init(&me->hlock);
	me->compat = 1;
	vfa.channel = chan;
	mutex_init(&me->mut_uid);
	for (i = 0; i < NUM_CHANNELS; i++) {
		me->jobid[i] = 1;
		/* This mutex has to been initialized first because
		 * it will be used in SSR callback. */
		mutex_init(&chan[i].smd_mutex);
		chan[i].ssrcount = 0;
		chan[i].in_hib = 0;
		chan[i].sesscount = 0;
		chan[i].subsystemstate = SUBSYSTEM_UP;
		chan[i].nb.notifier_call = fastrpc_restart_notifier_cb;
		chan[i].handle = qcom_register_ssr_notifier(
				gcinfo[i].subsys, &chan[i].nb);
		if (IS_ERR_OR_NULL(chan[i].handle))
			ADSPRPC_WARN("SSR notifier register failed for %s with err %ld\n",
				gcinfo[i].subsys, PTR_ERR(chan[i].handle));
		else
			ADSPRPC_INFO("SSR notifier registered for %s\n",
				gcinfo[i].subsys);
		/* All channels are secure by default except CDSP */
		if (i == CDSP_DOMAIN_ID || i == CDSP1_DOMAIN_ID) {
			chan[i].secure = NON_SECURE_CHANNEL;
			chan[i].unsigned_support = true;
		} else {
			chan[i].secure = SECURE_CHANNEL;
			chan[i].unsigned_support = false;
		}
		fastrpc_transport_session_init(i, chan[i].subsys);
		spin_lock_init(&chan[i].ctxlock);
		spin_lock_init(&chan[i].gmsg_log.lock);
	}
	err = fastrpc_transport_init();
	if (err)
		return err;
	me->transport_initialized = 1;

	return 0;
}

static void hfastrpc_deinit(void)
{
	struct vfastrpc_channel_ctx *chan = gcinfo;
	struct fastrpc_apps *me = &fa;
	int i;

	for (i = 0; i < NUM_CHANNELS; i++, chan++) {
		qcom_unregister_ssr_notifier(chan->handle, &chan->nb);
		fastrpc_transport_session_deinit(i);
		mutex_destroy(&chan->smd_mutex);
	}
	if (me->transport_initialized)
		fastrpc_transport_deinit();
	me->transport_initialized = 0;
	mutex_destroy(&me->mut_uid);
}

static int hfastrpc_probe(struct virtio_device *vdev)
{
	struct vfastrpc_apps *me = &vfa;
	struct device *dev = NULL;
	struct device *secure_dev = NULL;
	struct hfastrpc_config config;
	int err, i;

	if (!virtio_has_feature(vdev, VIRTIO_F_VERSION_1))
		return -ENODEV;

	if (!virtio_has_feature(vdev, VIRTIO_FASTRPC_F_HYBRID)) {
		dev_err(&vdev->dev,
			"hybrid fastrpc can't work with legacy virtio fastrpc\n");
		return -ENODEV;
	}

	memset(&config, 0x0, sizeof(config));
	if (virtio_has_feature(vdev, VIRTIO_FASTRPC_F_VERSION)) {
		virtio_cread(vdev, struct hfastrpc_config, version, &config.version);
		if (BE_MAJOR_VER(config.version) != FE_MAJOR_VER) {
			dev_err(&vdev->dev, "vdev major version does not match 0x%x:0x%x\n",
					FE_VERSION, config.version);
			return -ENODEV;
		}
	}
	dev_info(&vdev->dev, "hybrid fastrpc version 0x%x:0x%x\n",
			FE_VERSION, config.version);

	memset(me, 0, sizeof(*me));
	spin_lock_init(&me->msglock);
	spin_lock_init(&me->hlock);
	INIT_HLIST_HEAD(&me->drivers);
	me->max_sess_per_proc = DEFAULT_MAX_SESS_PER_PROC;

	vdev->priv = me;
	me->vdev = vdev;
	me->dev = vdev->dev.parent;

	if (virtio_has_feature(vdev, VIRTIO_FASTRPC_F_HYBRID))
		me->has_hybrid = true;

	if (virtio_has_feature(vdev, VIRTIO_FASTRPC_F_VQUEUE_SETTING)) {
		virtio_cread(vdev, struct hfastrpc_config, max_buf_size,
				&config.max_buf_size);
		if (config.max_buf_size > MAX_FASTRPC_BUF_SIZE) {
			dev_err(&vdev->dev, "buffer size 0x%x is exceed to maximum limit 0x%x\n",
					config.max_buf_size, MAX_FASTRPC_BUF_SIZE);
			return -EINVAL;
		}

		me->buf_size = config.max_buf_size;
		dev_info(&vdev->dev, "set buf_size to 0x%x\n", me->buf_size);
	} else {
		dev_info(&vdev->dev, "set buf_size to default value\n");
		me->buf_size = DEF_FASTRPC_BUF_SIZE;
	}

	err = init_vqs(me);
	if (err) {
		dev_err(&vdev->dev, "failed to initialized virtqueue\n");
		return err;
	}

	if (virtio_has_feature(vdev, VIRTIO_FASTRPC_F_DOMAIN_NUM)) {
		virtio_cread(vdev, struct hfastrpc_config, domain_num,
				&config.domain_num);
		dev_info(&vdev->dev, "get domain_num %d from config space\n",
				config.domain_num);
		if (config.domain_num < NUM_CHANNELS)
			me->num_channels = config.domain_num;
		else
			me->num_channels = NUM_CHANNELS;
	} else {
		dev_dbg(&vdev->dev, "set domain_num to default value\n");
		me->num_channels = NUM_CHANNELS;
	}

	err = hfastrpc_init();
	if (err) {
		dev_err(&vdev->dev, "failed to init fastrpc %d\n", err);
		goto init_fastrpc_bail;
	}

	me->debugfs_root = debugfs_create_dir("adsprpc", NULL);
	if (IS_ERR_OR_NULL(me->debugfs_root)) {
		dev_warn(&vdev->dev, "%s: %s: failed to create debugfs root dir\n",
			current->comm, __func__);
		me->debugfs_root = NULL;
	}

	me->debugfs_fops = &debugfs_fops;
	err = alloc_chrdev_region(&me->dev_no, 0, me->num_channels, DEVICE_NAME);
	if (err)
		goto alloc_chrdev_bail;

	cdev_init(&me->cdev, &fops);
	me->cdev.owner = THIS_MODULE;
	err = cdev_add(&me->cdev, MKDEV(MAJOR(me->dev_no), 0), NUM_DEVICES);
	if (err)
		goto cdev_init_bail;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
	me->class = class_create("fastrpc");
#else
	me->class = class_create(THIS_MODULE, "fastrpc");
#endif
	if (IS_ERR(me->class))
		goto class_create_bail;

	/*
	 * Create devices and register with sysfs
	 * Create first device with minor number 0
	 */
	dev = device_create(me->class, NULL,
				MKDEV(MAJOR(me->dev_no), MINOR_NUM_DEV),
				NULL, DEVICE_NAME);
	if (IS_ERR_OR_NULL(dev))
		goto device_create_bail;

	/* Create secure device with minor number for secure device */
	secure_dev = device_create(me->class, NULL,
				MKDEV(MAJOR(me->dev_no), MINOR_NUM_SECURE_DEV),
				NULL, DEVICE_NAME_SECURE);
	if (IS_ERR_OR_NULL(secure_dev))
		goto device_create_bail;

#ifdef CONFIG_VIRTIO_MMIO_SWIOTLB
	/* MMIO SWIOTLB replaced dma_map_ops of virtio platfrom device
	 * So use char device of fastrpc as a WR
	 */
	me->dev = dev;
	err = dma_coerce_mask_and_coherent(me->dev, DMA_BIT_MASK(64));
	if (err)
		ADSP_LOG("set DMA mask failed\n");
#endif

	virtio_device_ready(vdev);

	/* set up the receive buffers */
	for (i = 0; i < me->num_bufs; i++) {
		struct scatterlist sg;
		void *cpu_addr = me->rbufs[i];

		sg_init_one(&sg, cpu_addr, me->buf_size);
		err = virtqueue_add_inbuf(me->rvq.vq, &sg, 1, cpu_addr,
				GFP_KERNEL);
		WARN_ON(err); /* sanity check; this can't really happen */
	}

	/* suppress "tx-complete" interrupts */
	virtqueue_disable_cb(me->svq.vq);

	virtqueue_enable_cb(me->rvq.vq);
	virtqueue_kick(me->rvq.vq);

	dev_info(&vdev->dev, "Registered hybrid fastrpc device\n");
	return 0;
device_create_bail:
	if (!IS_ERR_OR_NULL(dev))
		device_destroy(me->class, MKDEV(MAJOR(me->dev_no),
						MINOR_NUM_DEV));
	if (!IS_ERR_OR_NULL(secure_dev))
		device_destroy(me->class, MKDEV(MAJOR(me->dev_no),
						MINOR_NUM_SECURE_DEV));
	class_destroy(me->class);
class_create_bail:
	cdev_del(&me->cdev);
cdev_init_bail:
	unregister_chrdev_region(me->dev_no, me->num_channels);
alloc_chrdev_bail:
	debugfs_remove_recursive(me->debugfs_root);
	hfastrpc_deinit();
init_fastrpc_bail:
	vdev->config->del_vqs(vdev);
	return err;
}

static void hfastrpc_remove(struct virtio_device *vdev)
{
	struct vfastrpc_apps *me = &vfa;
	int i;

	device_destroy(me->class, MKDEV(MAJOR(me->dev_no), MINOR_NUM_DEV));
	device_destroy(me->class, MKDEV(MAJOR(me->dev_no),
					MINOR_NUM_SECURE_DEV));
	class_destroy(me->class);
	cdev_del(&me->cdev);
	unregister_chrdev_region(me->dev_no, me->num_channels);
	debugfs_remove_recursive(me->debugfs_root);

	hfastrpc_deinit();
	vdev->config->reset(vdev);
	vdev->config->del_vqs(vdev);

	hfastrpc_unused_tx_bufs_list_free(me);
	for (i = 0; i < me->num_bufs; i++)
		free_pages((unsigned long)me->rbufs[i], me->order);
	for (i = 0; i < me->num_bufs; i++)
		free_pages((unsigned long)me->sbufs[i], me->order);

	kfree(me->sbufs);
	kfree(me->rbufs);
}

static struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_HYBRID_FASTRPC, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static unsigned int features[] = {
	VIRTIO_FASTRPC_F_VERSION,
	VIRTIO_FASTRPC_F_DOMAIN_NUM,
	VIRTIO_FASTRPC_F_VQUEUE_SETTING,
	VIRTIO_FASTRPC_F_HYBRID,
};

static struct virtio_driver hybrid_fastrpc_driver = {
	.feature_table		= features,
	.feature_table_size	= ARRAY_SIZE(features),
	.driver.name		= KBUILD_MODNAME,
	.driver.owner		= THIS_MODULE,
	.id_table		= id_table,
	.probe			= hfastrpc_probe,
	.remove			= hfastrpc_remove,
};

static int __init hybrid_fastrpc_init(void)
{
	return register_virtio_driver(&hybrid_fastrpc_driver);
}

static void __exit hybrid_fastrpc_exit(void)
{
	unregister_virtio_driver(&hybrid_fastrpc_driver);
}
module_init(hybrid_fastrpc_init);
module_exit(hybrid_fastrpc_exit);

MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("Hybrid FastRPC Driver");
MODULE_IMPORT_NS(DMA_BUF);
MODULE_LICENSE("GPL v2");
