// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include "fastrpc_vq.h"

static void *fastrpc_txbuf_get(struct fastrpc_common *gdriver)
{
	unsigned int len;
	void *ret;
	unsigned long flags;

	/* support multiple concurrent senders */
	spin_lock_irqsave(&gdriver->svq.vq_lock, flags);
	/*
	 * either pick the next unused tx buffer
	 * (half of our buffers are used for sending gdriverssages)
	 */
	if (gdriver->last_sbuf < gdriver->num_bufs)
		ret = gdriver->sbufs[gdriver->last_sbuf++];
	/* or recycle a used one */
	else
		ret = virtqueue_get_buf(gdriver->svq.vq, &len);
	spin_unlock_irqrestore(&gdriver->svq.vq_lock, flags);
	return ret;
}

struct virt_fastrpc_msg *virt_alloc_msg(struct fastrpc_user *fl, int size)
{
	struct fastrpc_common *gdriver = fl->cctx->gdriver;
	struct virt_fastrpc_msg *msg;
	void *buf;
	unsigned long flags;
	int i;

	if (size > gdriver->buf_size) {
		RPC_ERR("message is too big 0x%x\n", size);
		return NULL;
	}

	msg = kzalloc(sizeof(*msg), GFP_KERNEL);
	if (!msg) {
		RPC_ERR("no memory\n");
		return NULL;
	}

	init_completion(&msg->work);
	spin_lock_irqsave(&gdriver->msglock, flags);
	for (i = 0; i < FASTRPC_MSG_MAX; i++) {
		if (!gdriver->msgtable[i]) {
			gdriver->msgtable[i] = msg;
			msg->msgid = i;
			break;
		}
	}
	spin_unlock_irqrestore(&gdriver->msglock, flags);

	if (i == FASTRPC_MSG_MAX) {
		RPC_ERR("message queue is full\n");
		kfree(msg);
		return NULL;
	}

	buf = fastrpc_txbuf_get(gdriver);
	if (!buf) {
		RPC_ERR("can't get tx buffer\n");
		virt_free_msg(fl, msg);
		return NULL;
	}

	msg->txbuf = buf;
	return msg;
}

void virt_free_msg(struct fastrpc_user *fl, struct virt_fastrpc_msg *msg)
{
	struct fastrpc_common *gdriver = fl->cctx->gdriver;
	unsigned long flags;

	spin_lock_irqsave(&gdriver->msglock, flags);
	if (gdriver->msgtable[msg->msgid] == msg)
		gdriver->msgtable[msg->msgid] = NULL;
	else
		RPC_ERR("can't find msg %d in table\n", msg->msgid);
	spin_unlock_irqrestore(&gdriver->msglock, flags);

	kfree(msg);
}

int fastrpc_txbuf_send(struct fastrpc_user *fl, void *data, unsigned int len)
{
	struct fastrpc_common *gdriver = fl->cctx->gdriver;
	struct scatterlist sg[1];
	unsigned long flags;
	int err = 0;

	sg_init_one(sg, data, len);

	spin_lock_irqsave(&gdriver->svq.vq_lock, flags);
	err = virtqueue_add_outbuf(gdriver->svq.vq, sg, 1, data, GFP_KERNEL);
	if (err) {
		RPC_ERR("fail to add output buffer\n");
		spin_unlock_irqrestore(&gdriver->svq.vq_lock, flags);
		goto bail;
	}
	virtqueue_kick(gdriver->svq.vq);
	spin_unlock_irqrestore(&gdriver->svq.vq_lock, flags);
bail:
	return err;
}

void fastrpc_rxbuf_send(struct fastrpc_user *fl, void *data, unsigned int len)
{
	struct fastrpc_common *gdriver = fl->cctx->gdriver;
	struct scatterlist sg[1];
	unsigned long flags;
	int err = 0;

	sg_init_one(sg, data, len);

	spin_lock_irqsave(&gdriver->rvq.vq_lock, flags);
	/* add the buffer back to the remote processor's virtqueue */
	err = virtqueue_add_inbuf(gdriver->rvq.vq, sg, 1, data, GFP_KERNEL);
	if (err) {
		RPC_ERR("fail to add input buffer\n");
		spin_unlock_irqrestore(&gdriver->rvq.vq_lock, flags);
		return;
	}
	virtqueue_kick(gdriver->rvq.vq);
	spin_unlock_irqrestore(&gdriver->rvq.vq_lock, flags);
}
