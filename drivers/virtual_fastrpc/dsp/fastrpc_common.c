// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023-2025, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include "fastrpc_common.h"
#include "virtio_fastrpc_queue.h"
#include "virtio_fastrpc_mem.h"

/* Array to keep track unique tgid_frpc usage */
static bool frpc_tgid_usage_array[MAX_FRPC_TGID] = {0};

// Generate a unique process ID to DSP process
int get_unique_hlos_process_id(struct vfastrpc_file *vfl)
{
	int tgid_frpc = -1, tgid_index = 1;
	struct vfastrpc_apps *me = vfl->apps;
	unsigned long irq_flags = 0;

	spin_lock_irqsave(&me->hlock, irq_flags);
	for (tgid_index = 1; tgid_index < MAX_FRPC_TGID; tgid_index++) {
		if (!frpc_tgid_usage_array[tgid_index]) {
			tgid_frpc = tgid_index;
			/* Set the tgid usage to false */
			frpc_tgid_usage_array[tgid_index] = true;
			break;
		}
	}
	spin_unlock_irqrestore(&me->hlock, irq_flags);
	return tgid_frpc;
}

void put_unique_hlos_process_id(struct vfastrpc_file *vfl)
{
	struct vfastrpc_apps *me = vfl->apps;
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	unsigned long irq_flags = 0;

	spin_lock_irqsave(&me->hlock, irq_flags);
	if (fl->tgid_frpc != -1)
		frpc_tgid_usage_array[fl->tgid_frpc] = false;
	spin_unlock_irqrestore(&me->hlock, irq_flags);
}

int virt_fastrpc_close(struct vfastrpc_file *vfl)
{
	struct vfastrpc_apps *me = vfl->apps;
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	struct virt_msg_hdr *vmsg, *rsp = NULL;
	struct virt_fastrpc_msg *msg;
	int err;

	if (fl->cid < 0) {
		dev_err(me->dev, "close: channel id %d is invalid\n", fl->cid);
		return -EINVAL;
	}

	msg = virt_alloc_msg(vfl, sizeof(*vmsg));
	if (!msg) {
		dev_err(me->dev, "%s: no memory\n", __func__);
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

	spin_lock(&fl->hlock);
	fl->file_close = FASTRPC_PROCESS_DSP_EXIT_INIT;
	spin_unlock(&fl->hlock);

	err = vfastrpc_txbuf_send(vfl, vmsg, sizeof(*vmsg));
	if (err)
		goto bail;

	wait_for_completion(&msg->work);

	rsp = msg->rxbuf;
	if (!rsp)
		goto bail;

	err = rsp->result;
	if (!err) {
		spin_lock(&fl->hlock);
		fl->file_close = FASTRPC_PROCESS_DSP_EXIT_COMPLETE;
		spin_unlock(&fl->hlock);
	}
bail:
	if (rsp)
		vfastrpc_rxbuf_send(vfl, rsp, me->buf_size);

	if (err) {
		spin_lock(&fl->hlock);
		fl->file_close = FASTRPC_PROCESS_DSP_EXIT_ERROR;
		spin_unlock(&fl->hlock);
	}
	virt_free_msg(vfl, msg);

	return err;
}

void virt_free_msg(struct vfastrpc_file *vfl, struct virt_fastrpc_msg *msg)
{
	struct vfastrpc_apps *me = vfl->apps;
	unsigned long flags;

	spin_lock_irqsave(&me->msglock, flags);
	if (me->msgtable[msg->msgid] == msg)
		me->msgtable[msg->msgid] = NULL;
	else
		dev_err(me->dev, "can't find msg %d in table\n", msg->msgid);
	spin_unlock_irqrestore(&me->msglock, flags);

	kfree(msg);
}

struct virt_fastrpc_msg *virt_alloc_msg(struct vfastrpc_file *vfl, int size)
{
	struct vfastrpc_apps *me = vfl->apps;
	struct virt_fastrpc_msg *msg;
	void *buf;
	unsigned long flags;
	int i;

	if (size > me->buf_size) {
		dev_err(me->dev, "message is too big (%d)\n", size);
		return NULL;
	}

	msg = kzalloc(sizeof(*msg), GFP_KERNEL);
	if (!msg)
		return NULL;

	init_completion(&msg->work);
	spin_lock_irqsave(&me->msglock, flags);
	for (i = 0; i < FASTRPC_MSG_MAX; i++) {
		if (!me->msgtable[i]) {
			me->msgtable[i] = msg;
			msg->msgid = i;
			break;
		}
	}
	spin_unlock_irqrestore(&me->msglock, flags);

	if (i == FASTRPC_MSG_MAX) {
		dev_err(me->dev, "message queue is full\n");
		kfree(msg);
		return NULL;
	}

	buf = get_a_tx_buf(me);
	if (!buf) {
		dev_err(me->dev, "can't get tx buffer\n");
		virt_free_msg(vfl, msg);
		return NULL;
	}

	msg->txbuf = buf;
	return msg;
}

int virt_fastrpc_get_dsp_info(struct vfastrpc_file *vfl,
		u32 *dsp_attributes)
{
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	struct vfastrpc_apps *me = vfl->apps;
	struct virt_cap_msg *vmsg, *rsp = NULL;
	struct virt_fastrpc_msg *msg;
	int err;

	msg = virt_alloc_msg(vfl, sizeof(*vmsg));
	if (!msg) {
		dev_err(me->dev, "%s: no memory\n", __func__);
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
	vmsg->domain = vfl->domain;
	memset(vmsg->dsp_caps, 0, FASTRPC_MAX_DSP_ATTRIBUTES * (sizeof(u32)));

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
	memcpy(dsp_attributes, rsp->dsp_caps, FASTRPC_MAX_DSP_ATTRIBUTES * (sizeof(u32)));
bail:
	if (rsp)
		vfastrpc_rxbuf_send(vfl, rsp, me->buf_size);
	virt_free_msg(vfl, msg);

	return err;
}

