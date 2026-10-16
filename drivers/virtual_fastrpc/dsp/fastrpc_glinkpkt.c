/* SPDX-License-Identifier: GPL-2.0
 * Copyright (c) 2011-2018, The Linux Foundation. All rights reserved.
 * Copyright (c) 2018, Linaro Limited
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
#include <linux/device.h>

#include "fastrpc_common.h"
#include "fastrpc_core.h"
#include "fastrpc_vq.h"

/* TO DO: more cctx for device discovery */
static struct fastrpc_channel_ctx *vcctx = NULL;

static void fastrpc_remove_device_nodes(struct fastrpc_channel_ctx *cctx)
{
	if (cctx->fdevice)
		misc_deregister(&cctx->fdevice->miscdev);

	if(cctx->legacy_fdevice)
		misc_deregister(&cctx->legacy_fdevice->miscdev);

	if(cctx->legacy_secure_fdevice)
		misc_deregister(&cctx->legacy_secure_fdevice->miscdev);

	return;
}

int fastrpc_transport_glinkpkt_send(struct fastrpc_channel_ctx *cctx,
				void *pkt_msg, uint32_t data_size)
{
	int err = 0;
	struct glink_pkt_msg *glink_pkt = pkt_msg;
	struct fastrpc_user *fl = glink_pkt->fl;
	struct virt_fastrpc_msg *msg;
	struct virt_glink_pkt_msg *vmsg, *rsp = NULL;

	if (atomic_read(&cctx->teardown))
		return -EPIPE;

	msg = virt_alloc_msg(fl, sizeof(*vmsg) + data_size);
	if (!msg) {
		RPC_ERR("out of memory\n");
		return -ENOMEM;
	}

	vmsg = (struct virt_glink_pkt_msg *)msg->txbuf;
	vmsg->hdr.pid = fl->tgid_frpc;
	vmsg->hdr.tid = current->pid;
	vmsg->hdr.cid = fl->cid;
	vmsg->hdr.cmd = VIRTIO_FASTRPC_CMD_SEND_GLINK_PKT;
	vmsg->hdr.len = sizeof(*vmsg) + data_size;
	vmsg->hdr.msgid = msg->msgid;
	vmsg->hdr.result = 0xffffffff;
	vmsg->seq_num = 0;
	memcpy(vmsg->data, glink_pkt->data, data_size);

	err = fastrpc_txbuf_send(fl, vmsg, sizeof(*vmsg) + data_size);
	if (err)
		goto bail;
	wait_for_completion(&msg->work);

	rsp = msg->rxbuf;
	if (!rsp)
		goto bail;
	fastrpc_notify_user_ctx(glink_pkt->ctx, rsp->hdr.result,
				NORMAL_RESPONSE, 0);
bail:
	if (rsp)
		fastrpc_rxbuf_send(fl, rsp, cctx->gdriver->buf_size);
	virt_free_msg(fl, msg);

	return err;
}

int fastrpc_transport_glinkpkt_init(void)
{
	int err = 0;
	struct fastrpc_domain *domain = NULL;

	if (vcctx) {
		RPC_ERR("channel ctx is not NULL\n");
		return -EBUSY;
	}
	vcctx = kzalloc(sizeof(*vcctx), GFP_KERNEL);
	if (!vcctx) {
		RPC_ERR("failed to alloc channel ctx\n");
		return -ENOMEM;
	}

	domain = kzalloc(sizeof(*domain), GFP_KERNEL);
	if (!domain) {
		RPC_ERR("failed to alloc domain\n");
		kfree(vcctx);
		vcctx = NULL;
		return -ENOMEM;
	}

	/* we only support CDSP domain */
	domain->id = CDSP_DOMAIN_ID;
	domain->type = FASTRPC_NSP;
	domain->instance_id = 0;
	domain->legacy_name = (char *)legacy_domains[CDSP_DOMAIN_ID];
	domain->legacy_id = CDSP_DOMAIN_ID;

	atomic_set(&vcctx->teardown, 0);
	kref_init(&vcctx->refcount);
	INIT_LIST_HEAD(&vcctx->users);
	spin_lock_init(&vcctx->lock);
	idr_init(&vcctx->ctx_idr);
	ida_init(&vcctx->tgid_frpc_ida);
	vcctx->domain_id = domain->id;
	vcctx->max_sess_per_proc = FASTRPC_MAX_SESSIONS_PER_PROCESS;
	vcctx->domain = domain;
	vcctx->unsigned_support = true;
	vcctx->cpuinfo_todsp = FASTRPC_CPUINFO_EARLY_WAKEUP;

	fastrpc_update_gdriver(vcctx, 1);
	err = fastrpc_device_register(vcctx->dev, vcctx, false, true, domain->legacy_name);
	if (err) {
		RPC_ERR("failed to register device node, err %d\n", err);
		kfree(domain);
		fastrpc_channel_ctx_put(vcctx);
		vcctx = NULL;
		return err;
	}

	domain->status = DSP_STATUS_UP;
	domain->cctx = vcctx;
	RPC_INFO("opened glink pkt channel for cdsp\n");

	return 0;
}

void fastrpc_transport_glinkpkt_deinit(void)
{
	struct fastrpc_domain *domain;
	struct fastrpc_user *user;
	unsigned long flags;

	if (!vcctx) {
		RPC_ERR("channel ctx is NULL\n");
		return;
	}
	domain = vcctx->domain;

	spin_lock_irqsave(&vcctx->lock, flags);
	atomic_set(&vcctx->teardown, 1);
	domain->status = DSP_STATUS_DOWN;
	domain->cctx = NULL;
	list_for_each_entry(user, &vcctx->users, user) {
		fastrpc_queue_pd_status(user, vcctx->domain_id, FASTRPC_DSP_SSR,
			user->sessionid);
		fastrpc_notify_users(user);
	}
	spin_unlock_irqrestore(&vcctx->lock, flags);
	fastrpc_remove_device_nodes(vcctx);

	RPC_INFO("closing glink pkt channel for %s", domain->name);
	vcctx->domain = NULL;

	kfree(domain);

	fastrpc_channel_ctx_put(vcctx);
	vcctx = NULL;
}
