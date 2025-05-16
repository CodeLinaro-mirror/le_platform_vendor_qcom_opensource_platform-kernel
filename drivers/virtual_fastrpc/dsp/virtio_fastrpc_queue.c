// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2025, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/list.h>
#include "virtio_fastrpc_queue.h"


int put_a_tx_buf(struct vfastrpc_apps *me, void *buf)
{
	struct vfastrpc_vqbuf *vtxbuf;

	vtxbuf = kzalloc(sizeof(*vtxbuf), GFP_KERNEL);
	if (!vtxbuf)
		return -ENOMEM;

	vtxbuf->buf = buf;
	INIT_HLIST_NODE(&vtxbuf->hn);
	spin_lock(&me->svq.vq_lock);
	hlist_add_head(&vtxbuf->hn, &me->unused_tx_bufs);
	spin_unlock(&me->svq.vq_lock);
	return 0;
}

void *get_a_tx_buf(struct vfastrpc_apps *me)
{
	struct vfastrpc_vqbuf *vtxbuf = NULL;
	unsigned int len;
	void *ret = NULL;
	unsigned long flags;

	/* support multiple concurrent senders */
	spin_lock_irqsave(&me->svq.vq_lock, flags);
	/*
	 * either pick the next unused tx buffer
	 * (half of our buffers are used for sending messages)
	 */
	if (!hlist_empty(&me->unused_tx_bufs)) {
		struct hlist_node *n;
		hlist_for_each_entry_safe(vtxbuf, n, &me->unused_tx_bufs, hn) {
			ret = vtxbuf->buf;
			hlist_del_init(&vtxbuf->hn);
			kfree(vtxbuf);
			break;
		}
	/* or recycle a used one */
	} else {
		ret = virtqueue_get_buf(me->svq.vq, &len);
	}
	spin_unlock_irqrestore(&me->svq.vq_lock, flags);
	return ret;
}

int vfastrpc_txbuf_send(struct vfastrpc_file *vfl, void *data, unsigned int len)
{
	struct vfastrpc_apps *me = vfl->apps;
	struct scatterlist sg[1];
	unsigned long flags;
	int err = 0;

	sg_init_one(sg, data, len);

	spin_lock_irqsave(&me->svq.vq_lock, flags);
	err = virtqueue_add_outbuf(me->svq.vq, sg, 1, data, GFP_KERNEL);
	if (err) {
		dev_err(me->dev, "%s: fail to add output buffer\n", __func__);
		spin_unlock_irqrestore(&me->svq.vq_lock, flags);
		goto bail;
	}
	virtqueue_kick(me->svq.vq);
	spin_unlock_irqrestore(&me->svq.vq_lock, flags);
bail:
	return err;
}

void vfastrpc_rxbuf_send(struct vfastrpc_file *vfl, void *data, unsigned int len)
{
	struct vfastrpc_apps *me = vfl->apps;
	struct scatterlist sg[1];
	unsigned long flags;
	int err = 0;

	sg_init_one(sg, data, len);

	spin_lock_irqsave(&me->rvq.vq_lock, flags);
	/* add the buffer back to the remote processor's virtqueue */
	err = virtqueue_add_inbuf(me->rvq.vq, sg, 1, data, GFP_KERNEL);
	if (err) {
		dev_err(me->dev,
			"%s: fail to add input buffer\n", __func__);
		spin_unlock_irqrestore(&me->rvq.vq_lock, flags);
		return;
	}
	virtqueue_kick(me->rvq.vq);
	spin_unlock_irqrestore(&me->rvq.vq_lock, flags);
}
