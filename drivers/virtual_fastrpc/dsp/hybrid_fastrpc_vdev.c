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
#include "fastrpc_common.h"

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
#define FE_MINOR_VER 0x6
#define FE_VERSION (FE_MAJOR_VER << 16 | FE_MINOR_VER)
#define BE_MAJOR_VER(ver) (((ver) >> 16) & 0xffff)

struct hfastrpc_config {
	u32 version;
	u32 domain_num;
	u32 max_buf_size;
} __packed;

static struct fastrpc_common g_frpc;

void fastrpc_update_gctx(struct fastrpc_channel_ctx *cctx, int flag)
{
	struct fastrpc_channel_ctx **ctx = &g_frpc.gctx[cctx->domain_id];

	if (flag == 1) {
		*ctx = cctx;
		cctx->gdriver = &g_frpc;
		cctx->dev = cctx->gdriver->dev;
	} else {
		*ctx = NULL;
		cctx->gdriver = NULL;
	}
}

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
	spin_unlock(&user->lock);
}

static int recv_single(struct virt_msg_hdr *rsp, unsigned int len)
{
	struct fastrpc_common *gdriver = &g_frpc;
	struct virt_fastrpc_msg *msg;

	if (len != rsp->len) {
		RPC_ERR("msg %u len mismatch,expected %u but %d found\n",
				rsp->cmd, rsp->len, len);
		return -EINVAL;
	}
	spin_lock(&gdriver->msglock);
	msg = gdriver->msgtable[rsp->msgid];
	spin_unlock(&gdriver->msglock);

	if (!msg) {
		RPC_ERR("msg %u already free in table[%u]\n",
				rsp->cmd, rsp->msgid);
		return -EINVAL;
	}
	msg->rxbuf = (void *)rsp;

	complete(&msg->work);

	return 0;
}

static void fastrpc_vq_callback(struct virtqueue *rvq)
{
	struct fastrpc_common *gdriver = &g_frpc;
	struct virt_msg_hdr *rsp;
	unsigned int len, msgs_received = 0;
	int err;
	unsigned long flags;

	spin_lock_irqsave(&gdriver->rvq.vq_lock, flags);
	rsp = virtqueue_get_buf(rvq, &len);
	if (!rsp) {
		spin_unlock_irqrestore(&gdriver->rvq.vq_lock, flags);
		RPC_ERR("incoming signal, but no used buffer\n");
		return;
	}
	spin_unlock_irqrestore(&gdriver->rvq.vq_lock, flags);

	while (rsp) {
		err = recv_single(rsp, len);
		if (err)
			break;

		msgs_received++;

		spin_lock_irqsave(&gdriver->rvq.vq_lock, flags);
		rsp = virtqueue_get_buf(rvq, &len);
		spin_unlock_irqrestore(&gdriver->rvq.vq_lock, flags);
	}
}

static void virt_init_vq(struct virt_fastrpc_vq *fastrpc_vq,
				struct virtqueue *vq)
{
	spin_lock_init(&fastrpc_vq->vq_lock);
	fastrpc_vq->vq = vq;
}

static int init_vqs(struct fastrpc_common *gdriver)
{
	struct virtqueue *vqs[2];
	static const char * const names[] = { "tx", "rx" };
	vq_callback_t *cbs[] = { NULL, fastrpc_vq_callback };
	int err, i;

	err = virtio_find_vqs(gdriver->vdev, 2, vqs, cbs, names, NULL);
	if (err)
		return err;

	virt_init_vq(&gdriver->svq, vqs[0]);
	virt_init_vq(&gdriver->rvq, vqs[1]);


	/* we expect symmetric tx/rx vrings */
	if (virtqueue_get_vring_size(gdriver->rvq.vq) !=
			virtqueue_get_vring_size(gdriver->svq.vq)) {
		RPC_ERR("tx/rx vring size does not match\n");
			err = -EINVAL;
		goto vqs_del;
	}

	gdriver->num_bufs = virtqueue_get_vring_size(gdriver->rvq.vq);
	gdriver->rbufs = kcalloc(gdriver->num_bufs,
				sizeof(void *), GFP_KERNEL);
	if (!gdriver->rbufs) {
		err = -ENOMEM;
		goto vqs_del;
	}
	gdriver->sbufs = kcalloc(gdriver->num_bufs,
				sizeof(void *), GFP_KERNEL);
	if (!gdriver->sbufs) {
		err = -ENOMEM;
		kfree(gdriver->rbufs);
		goto vqs_del;
	}

	gdriver->order = get_order(gdriver->buf_size);

	for (i = 0; i < gdriver->num_bufs; i++) {
		gdriver->rbufs[i] = (void *)__get_free_pages(GFP_KERNEL, gdriver->order);
		if (!gdriver->rbufs[i]) {
			err = -ENOMEM;
			goto rbuf_del;
		}
	}

	for (i = 0; i < gdriver->num_bufs; i++) {
		gdriver->sbufs[i] = (void *)__get_free_pages(GFP_KERNEL, gdriver->order);
		if (!gdriver->sbufs[i]) {
			err = -ENOMEM;
			goto sbuf_del;
		}
	}
	return 0;

sbuf_del:
	for (i = 0; i < gdriver->num_bufs; i++) {
		if (gdriver->sbufs[i])
			free_pages((unsigned long)gdriver->sbufs[i], gdriver->order);
	}

rbuf_del:
	for (i = 0; i < gdriver->num_bufs; i++) {
		if (gdriver->rbufs[i])
			free_pages((unsigned long)gdriver->rbufs[i], gdriver->order);
	}
	kfree(gdriver->sbufs);
	kfree(gdriver->rbufs);
vqs_del:
	gdriver->vdev->config->del_vqs(gdriver->vdev);
	return err;
}

static int hfastrpc_probe(struct virtio_device *vdev)
{
	struct hfastrpc_config config;
	int err, i;
	struct fastrpc_common *gdriver = &g_frpc;
#ifdef CONFIG_DEBUG_FS
        struct dentry *debugfs_root = NULL;
#endif

	if (!virtio_has_feature(vdev, VIRTIO_F_VERSION_1))
		return -ENODEV;

	if (!virtio_has_feature(vdev, VIRTIO_FASTRPC_F_HYBRID)) {
		RPC_ERR("hybrid fastrpc can't work with legacy virtio fastrpc\n");
		return -ENODEV;
	}

	memset(&config, 0x0, sizeof(config));
	if (virtio_has_feature(vdev, VIRTIO_FASTRPC_F_VERSION)) {
		virtio_cread(vdev, struct hfastrpc_config, version, &config.version);
		if (BE_MAJOR_VER(config.version) != FE_MAJOR_VER) {
			RPC_ERR("vdev major version does not match 0x%x:0x%x\n",
					FE_VERSION, config.version);
			return -ENODEV;
		}
	}
	RPC_INFO("hybrid fastrpc version 0x%x:0x%x\n",
			FE_VERSION, config.version);

	memset(gdriver, 0, sizeof(*gdriver));
	spin_lock_init(&gdriver->msglock);
	spin_lock_init(&gdriver->glock);

	vdev->priv = gdriver;
	gdriver->vdev = vdev;
	gdriver->dev = vdev->dev.parent;

	if (virtio_has_feature(vdev, VIRTIO_FASTRPC_F_VQUEUE_SETTING)) {
		virtio_cread(vdev, struct hfastrpc_config, max_buf_size,
				&config.max_buf_size);
		if (config.max_buf_size > MAX_FASTRPC_BUF_SIZE) {
			RPC_ERR("buffer size 0x%x is exceed to maximum limit 0x%x\n",
					config.max_buf_size, MAX_FASTRPC_BUF_SIZE);
			return -EINVAL;
		}

		gdriver->buf_size = config.max_buf_size;
		RPC_INFO("set buf_size to 0x%x\n", gdriver->buf_size);
	} else {
		gdriver->buf_size = DEF_FASTRPC_BUF_SIZE;
		RPC_INFO("set buf_size to default value 0x%x\n", gdriver->buf_size);
	}

	if (virtio_has_feature(vdev, VIRTIO_FASTRPC_F_DOMAIN_NUM)) {
		virtio_cread(vdev, struct hfastrpc_config, domain_num,
				&config.domain_num);
		RPC_INFO("get domain_num %d from config space\n",
				config.domain_num);
		if (config.domain_num < FASTRPC_DEV_MAX)
			gdriver->num_channels = config.domain_num;
		else
			gdriver->num_channels = FASTRPC_DEV_MAX;
	} else {
		RPC_INFO("set domain_num to default value %d\n", FASTRPC_DEV_MAX);
		gdriver->num_channels = FASTRPC_DEV_MAX;
	}

	err = init_vqs(gdriver);
	if (err) {
		RPC_ERR("failed to initialized virtqueue\n");
		return err;
	}

	err = fastrpc_transport_init();
	if (err) {
		RPC_ERR("fastrpc: failed to register rpmsg driver\n");
		goto bail;
	}

#ifdef CONFIG_DEBUG_FS
	debugfs_root = debugfs_create_dir("fastrpc", NULL);
	if (IS_ERR_OR_NULL(debugfs_root)) {
		RPC_WARN("failed to create debugfs root dir\n");
		debugfs_root = NULL;
	}

	gdriver->debugfs_root = debugfs_root;
#endif

	virtio_device_ready(vdev);

	/* set up the receive buffers */
	for (i = 0; i < gdriver->num_bufs; i++) {
		struct scatterlist sg;
		void *cpu_addr = gdriver->rbufs[i];

		sg_init_one(&sg, cpu_addr, gdriver->buf_size);
		err = virtqueue_add_inbuf(gdriver->rvq.vq, &sg, 1, cpu_addr,
				GFP_KERNEL);
		WARN_ON(err); /* sanity check; this can't really happen */
	}

	/* suppress "tx-complete" interrupts */
	virtqueue_disable_cb(gdriver->svq.vq);

	virtqueue_enable_cb(gdriver->rvq.vq);
	virtqueue_kick(gdriver->rvq.vq);

	RPC_INFO("Registered hybrid fastrpc device\n");
	return 0;
bail:
	vdev->config->del_vqs(vdev);
	return err;
}

static void hfastrpc_remove(struct virtio_device *vdev)
{
	struct fastrpc_common *gdriver = &g_frpc;
	int i;
#ifdef CONFIG_DEBUG_FS
	debugfs_remove_recursive(gdriver->debugfs_root);
#endif
	fastrpc_transport_deinit();
	vdev->config->reset(vdev);
	vdev->config->del_vqs(vdev);

	for (i = 0; i < gdriver->num_bufs; i++)
		free_pages((unsigned long)gdriver->rbufs[i], gdriver->order);
	for (i = 0; i < gdriver->num_bufs; i++)
		free_pages((unsigned long)gdriver->sbufs[i], gdriver->order);

	kfree(gdriver->sbufs);
	kfree(gdriver->rbufs);
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
