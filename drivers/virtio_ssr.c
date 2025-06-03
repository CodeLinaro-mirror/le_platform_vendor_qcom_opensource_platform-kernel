// SPDX-License-Identifier: GPL-2.0-only
/*
 * Virtio SSR Driver
 * Copyright (c) 2019-2020, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
#include <linux/completion.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/notifier.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/of_platform.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include "qcom_common.h"

#define SUBSYSTEM_NAME_SIZE 16
/**
 * struct virtio_ssr_evt - the virtio SSR event structure
 * @ss_name: the subsystem name of the virtio SSR message
 * @ssr_event: the ssr event of the virtio SSR message
 */
struct virtio_ssr_evt {
	char ss_name[SUBSYSTEM_NAME_SIZE];
	__le16 ssr_event;
};

#define VIRTIO_ID_SSR		0xc00e /* virtio ssr */

/**
 * struct virtio_ssr - virtio SSR data
 * @vdev: virtio device for this controller
 * @vq: the virtio virtqueue for communication
 * @work: the work thread to run client's SSR callback
 * @lock: the virtio virtqueue spinlock for avoiding conflict
 */
struct virtio_ssr {
	struct virtio_device *vdev;
	struct virtqueue *vq;
	struct work_struct work;
	struct virtio_ssr_evt event[32];
	spinlock_t lock;
};

static struct workqueue_struct *ssr_wq;

static void virtio_ssr_queue_evtbuf(struct virtio_ssr *vssr,
				   struct virtio_ssr_evt *event)
{
	struct scatterlist sg[1];

	sg_init_one(sg, event, sizeof(*event));
	virtqueue_add_inbuf(vssr->vq, sg, 1, event, GFP_ATOMIC);
}

static void virtio_ssr_fill_evt(struct virtio_ssr *vssr)
{
	unsigned long flags;
	int i, size;

	spin_lock_irqsave(&vssr->lock, flags);
	size = virtqueue_get_vring_size(vssr->vq);
	if (size > ARRAY_SIZE(vssr->event))
		size = ARRAY_SIZE(vssr->event);
	for (i = 0; i < size; i++)
		virtio_ssr_queue_evtbuf(vssr, &vssr->event[i]);
	virtqueue_kick(vssr->vq);
	spin_unlock_irqrestore(&vssr->lock, flags);
}

static void virtio_ssr_msg_coming(struct virtqueue *vq)
{
	struct virtio_ssr *vssr;

	if (!vq)
		return;

	vssr = vq->vdev->priv;
	queue_work(ssr_wq, &vssr->work);

	return;
}

static void virtio_ssr_receive(struct work_struct *work)
{
	struct virtio_ssr *vssr =
		container_of(work, struct virtio_ssr, work);
	struct virtio_ssr_evt *evt;
	unsigned int len;
	void *subsystem_handle = NULL;
	unsigned long flags = 0;

	spin_lock_irqsave(&vssr->lock, flags);
	while ((evt = virtqueue_get_buf(vssr->vq, &len)) != NULL) {
		spin_unlock_irqrestore(&vssr->lock, flags);
		pr_info("%s: receive SSR event: %d from ss-name: %s\n", __func__, evt->ssr_event, evt->ss_name);

		subsystem_handle = qcom_ssr_get_subsys(evt->ss_name);
		if (subsystem_handle) {
			qcom_notify_ssr_clients(subsystem_handle, evt->ssr_event, NULL);
		}
		spin_lock_irqsave(&vssr->lock, flags);
		virtio_ssr_queue_evtbuf(vssr, evt);
	}
	virtqueue_kick(vssr->vq);

	spin_unlock_irqrestore(&vssr->lock, flags);

	return;
}

static void virtio_ssr_del_vqs(struct virtio_device *vdev)
{
	virtio_reset_device(vdev);
	vdev->config->del_vqs(vdev);
}

static int virtio_ssr_setup_vqs(struct virtio_ssr *vssr)
{
	struct virtio_device *vdev = vssr->vdev;

	vssr->vq = virtio_find_single_vq(vdev, virtio_ssr_msg_coming, "eventq");
	return PTR_ERR_OR_ZERO(vssr->vq);
}

static int virtio_ssr_probe(struct virtio_device *vdev)
{
	struct virtio_ssr *vssr;
	int ret;

	vssr = devm_kzalloc(&vdev->dev, sizeof(*vssr), GFP_KERNEL);
	if (!vssr)
		return -ENOMEM;

	vdev->priv = vssr;
	vssr->vdev = vdev;

	ret = virtio_ssr_setup_vqs(vssr);
	if (ret) {
		kfree(vssr);
		vssr = NULL;
		return ret;
	}

	ssr_wq = create_singlethread_workqueue("ssr_wq");
	if (!ssr_wq) {
		dev_err(&vdev->dev, "virtio-SSR Workqueue creation failed\n");
		kfree(vssr);
		vssr = NULL;
		return -EIO;
	}

	INIT_WORK(&vssr->work, virtio_ssr_receive);

	virtio_device_ready(vdev);

	virtio_ssr_fill_evt(vssr);

	dev_info(&vdev->dev, "virtio-SSR probe done!!!\n");

	return ret;
}

static void virtio_ssr_remove(struct virtio_device *vdev)
{
	struct virtio_ssr *vssr = vdev->priv;
	void *buf;

	virtio_reset_device(vdev);
	while ((buf = virtqueue_detach_unused_buf(vssr->vq)) != NULL)
		kfree(buf);

	vdev->config->del_vqs(vdev);

	kfree(vssr);
	vssr = NULL;
}

static struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_SSR, VIRTIO_DEV_ANY_ID },
	{}
};
MODULE_DEVICE_TABLE(virtio, id_table);

#ifdef CONFIG_PM_SLEEP
static int virtio_ssr_freeze(struct virtio_device *vdev)
{
	virtio_ssr_del_vqs(vdev);
	return 0;
}

static int virtio_ssr_restore(struct virtio_device *vdev)
{
	struct virtio_ssr *vssr = vdev->priv;
	int err;

	err = virtio_ssr_setup_vqs(vssr);
	if (err)
		return err;

	virtio_ssr_fill_evt(vssr);
	return 0;
}
#endif


static struct virtio_driver virtio_ssr_driver = {
	.id_table		= id_table,
	.probe			= virtio_ssr_probe,
	.remove			= virtio_ssr_remove,
	.driver			= {
		.name	= "virtio_ssr",
	},
#ifdef CONFIG_PM_SLEEP
	.freeze = virtio_ssr_freeze,
	.restore = virtio_ssr_restore,
#endif
};
module_virtio_driver(virtio_ssr_driver);

MODULE_DESCRIPTION("Virtio SSR driver");
MODULE_LICENSE("GPL");
