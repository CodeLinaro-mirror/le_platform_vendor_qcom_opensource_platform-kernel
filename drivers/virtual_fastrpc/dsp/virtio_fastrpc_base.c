// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2023, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/debugfs.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/virtio_config.h>
#include <linux/uaccess.h>
#include <linux/of.h>
#include "virtio_fastrpc_core.h"
#include "virtio_fastrpc_mem.h"
#include "virtio_fastrpc_queue.h"
#include "virtio_fastrpc_trace.h"
#include "fastrpc_ioctl.h"

/* Virtio ID of FASTRPC : 0xC004 */
#define VIRTIO_ID_FASTRPC				49156
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
/* indicates signed PD control is available in config space */
#define VIRTIO_FASTRPC_F_SIGNED_PD_CONTROL		9


#define NUM_DEVICES			2 /* adsprpc-smd, adsprpc-smd-secure */

#define MAX_FASTRPC_BUF_SIZE		(1024*1024*4)
#define DEF_FASTRPC_BUF_SIZE		(128*1024)
#define DEBUGFS_SIZE			3072

/*
 * FE_MAJOR_VER is used for the FE and BE's version match check,
 * and it MUST be equal to BE_MAJOR_VER, otherwise virtual fastrpc
 * cannot work properly. It increases when fundamental protocol is
 * changed between FE and BE.
 */
#define FE_MAJOR_VER 0x6
/* FE_MINOR_VER is used to track patches in this driver. It does not
 * need to be matched with BE_MINOR_VER. And it will return to 0 when
 * FE_MAJOR_VER is increased.
 */
#define FE_MINOR_VER 0x7
#define FE_VERSION (FE_MAJOR_VER << 16 | FE_MINOR_VER)
#define BE_MAJOR_VER(ver) (((ver) >> 16) & 0xffff)

struct virtio_fastrpc_config {
	u32 version;
	u32 domain_num;
	u32 max_buf_size;
	u32 signed_pd_control;
} __packed;


static struct vfastrpc_apps gfa;

static ssize_t vfastrpc_debugfs_read(struct file *filp, char __user *buffer,
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
				"\n%s %d %s %d\n", "tgid_frpc =", fl->tgid_frpc,
				"sessionid =", fl->sessionid);
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
	.read = vfastrpc_debugfs_read,
};

static int vfastrpc_open(struct inode *inode, struct file *filp)
{
	int err = 0;
	struct vfastrpc_file *vfl = NULL;
	struct fastrpc_file *fl = NULL;
	struct vfastrpc_apps *me = &gfa;

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

	VERIFY(err, NULL != (vfl = vfastrpc_file_alloc(&vfrpc_ops)));
	if (err) {
		dev_err(me->dev, "Allocate vfastrpc_file failed %d\n", dev_minor);
		return err;
	}

	fl = to_fastrpc_file(vfl);
	fl->dev_minor = dev_minor;
	vfl->apps = me;

	filp->private_data = fl;
	return 0;
}

static int vfastrpc_release(struct inode *inode, struct file *file)
{
	struct fastrpc_file *fl = (struct fastrpc_file *)file->private_data;
	struct vfastrpc_file *vfl = to_vfastrpc_file(fl);

	if (vfl) {
		vfastrpc_file_free(vfl);
		file->private_data = NULL;
	}
	return 0;
}

static const struct file_operations fops = {
	.open = vfastrpc_open,
	.release = vfastrpc_release,
	.unlocked_ioctl = vfastrpc_ioctl,
	.compat_ioctl = compat_fastrpc_device_ioctl,
};

static int recv_single(struct virt_msg_hdr *rsp, unsigned int len)
{
	struct vfastrpc_apps *me = &gfa;
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

	if (msg->ctx && msg->ctx->asyncjob.isasyncjob)
		vfastrpc_queue_completed_async_job(msg->ctx);
	else
		complete(&msg->work);

	if (msg->ctx)
		trace_recv_single_end(msg->ctx);

	return 0;
}

static void recv_done(struct virtqueue *rvq)
{

	struct vfastrpc_apps *me = &gfa;
	struct virt_msg_hdr *rsp;
	unsigned int len, msgs_received = 0;
	int err;
	unsigned long flags;

	trace_recv_done_start(0);
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

static int init_vqs(struct vfastrpc_apps *me)
{
	struct virtqueue *vqs[2];
	static const char * const names[] = { "output", "input" };
	vq_callback_t *cbs[] = { NULL, recv_done };
	int err, i;

	err = virtio_find_vqs(me->vdev, 2, vqs, cbs, names, NULL);
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
	return 0;

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

static int vfastrpc_channel_init(struct vfastrpc_apps *me)
{
	int i;

	me->channel = kcalloc(me->num_channels,
			sizeof(struct vfastrpc_channel_ctx), GFP_KERNEL);
	me->max_sess_per_proc = DEFAULT_MAX_SESS_PER_PROC;
	if (!me->channel)
		return -ENOMEM;
	for (i = 0; i < me->num_channels; i++) {
		/* All channels are secure by default except CDSP and CDSP1*/
		if (i == CDSP_DOMAIN_ID || i == CDSP1_DOMAIN_ID) {
			me->channel[i].secure = false;
			me->channel[i].unsigned_support = true;
		} else {
			me->channel[i].secure = true;
			me->channel[i].unsigned_support = false;
		}
		me->channel[i].sesscount = 0;
	}
	return 0;
}

static void vfastrpc_channel_deinit(struct vfastrpc_apps *me)
{
	kfree(me->channel);
	me->channel = NULL;
}

static int virt_fastrpc_probe(struct virtio_device *vdev)
{
	struct vfastrpc_apps *me = &gfa;
	struct device *dev = NULL;
	struct device *secure_dev = NULL;
	struct virtio_fastrpc_config config;
	int err, i;

	if (!virtio_has_feature(vdev, VIRTIO_F_VERSION_1))
		return -ENODEV;

	memset(&config, 0x0, sizeof(config));
	if (virtio_has_feature(vdev, VIRTIO_FASTRPC_F_VERSION)) {
		virtio_cread(vdev, struct virtio_fastrpc_config, version, &config.version);
		if (BE_MAJOR_VER(config.version) != FE_MAJOR_VER) {
			dev_err(&vdev->dev, "vdev major version does not match 0x%x:0x%x\n",
					FE_VERSION, config.version);
			return -ENODEV;
		}
	}
	dev_info(&vdev->dev, "virtio fastrpc version 0x%x:0x%x\n",
			FE_VERSION, config.version);

	memset(me, 0, sizeof(*me));
	spin_lock_init(&me->msglock);
	spin_lock_init(&me->hlock);

	if (virtio_has_feature(vdev, VIRTIO_FASTRPC_F_INVOKE_ATTR))
		me->has_invoke_attr = true;

	if (virtio_has_feature(vdev, VIRTIO_FASTRPC_F_INVOKE_CRC))
		me->has_invoke_crc = true;

	if (virtio_has_feature(vdev, VIRTIO_FASTRPC_F_MMAP))
		me->has_mmap = true;

	if (virtio_has_feature(vdev, VIRTIO_FASTRPC_F_CONTROL))
		me->has_control = true;

	if (virtio_has_feature(vdev, VIRTIO_FASTRPC_F_MEM_MAP))
		me->has_mem_map = true;

	if (virtio_has_feature(vdev, VIRTIO_FASTRPC_F_SIGNED_PD_CONTROL)) {
		virtio_cread(vdev, struct virtio_fastrpc_config, signed_pd_control,
				&config.signed_pd_control);
		me->signed_pd_control = config.signed_pd_control;
	} else {
		me->signed_pd_control = 0;
	}

	vdev->priv = me;
	me->vdev = vdev;
	me->dev = vdev->dev.parent;

	if (virtio_has_feature(vdev, VIRTIO_FASTRPC_F_VQUEUE_SETTING)) {
		virtio_cread(vdev, struct virtio_fastrpc_config, max_buf_size,
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
		virtio_cread(vdev, struct virtio_fastrpc_config, domain_num,
				&config.domain_num);
		dev_info(&vdev->dev, "get domain_num %d from config space\n",
				config.domain_num);
		me->num_channels = config.domain_num;
	} else if (of_get_property(me->dev->of_node, "qcom,domain_num", NULL) != NULL) {
		err = of_property_read_u32(me->dev->of_node, "qcom,domain_num",
					&me->num_channels);
		if (err) {
			dev_err(&vdev->dev, "failed to read domain_num %d\n", err);
			goto alloc_channel_bail;
		}
	} else {
		dev_dbg(&vdev->dev, "set domain_num to default value\n");
		me->num_channels = NUM_CHANNELS;
	}

	err = vfastrpc_channel_init(me);
	if (err) {
		dev_err(&vdev->dev, "failed to init channel context %d\n", err);
		goto alloc_channel_bail;
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

	me->class = class_create(THIS_MODULE, "fastrpc");
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

	dev_info(&vdev->dev, "Registered virtio fastrpc device\n");
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
	vfastrpc_channel_deinit(me);
alloc_channel_bail:
	vdev->config->del_vqs(vdev);
	return err;
}

static void virt_fastrpc_remove(struct virtio_device *vdev)
{
	struct vfastrpc_apps *me = &gfa;
	int i;

	device_destroy(me->class, MKDEV(MAJOR(me->dev_no), MINOR_NUM_DEV));
	device_destroy(me->class, MKDEV(MAJOR(me->dev_no),
					MINOR_NUM_SECURE_DEV));
	class_destroy(me->class);
	cdev_del(&me->cdev);
	unregister_chrdev_region(me->dev_no, me->num_channels);
	debugfs_remove_recursive(me->debugfs_root);

	vfastrpc_channel_deinit(me);
	vdev->config->reset(vdev);
	vdev->config->del_vqs(vdev);

	for (i = 0; i < me->num_bufs; i++)
		free_pages((unsigned long)me->rbufs[i], me->order);
	for (i = 0; i < me->num_bufs; i++)
		free_pages((unsigned long)me->sbufs[i], me->order);

	kfree(me->sbufs);
	kfree(me->rbufs);
}

static struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_FASTRPC, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static unsigned int features[] = {
	VIRTIO_FASTRPC_F_INVOKE_ATTR,
	VIRTIO_FASTRPC_F_INVOKE_CRC,
	VIRTIO_FASTRPC_F_MMAP,
	VIRTIO_FASTRPC_F_CONTROL,
	VIRTIO_FASTRPC_F_VERSION,
	VIRTIO_FASTRPC_F_DOMAIN_NUM,
	VIRTIO_FASTRPC_F_VQUEUE_SETTING,
	VIRTIO_FASTRPC_F_MEM_MAP,
	VIRTIO_FASTRPC_F_SIGNED_PD_CONTROL,
};

static struct virtio_driver virtio_fastrpc_driver = {
	.feature_table		= features,
	.feature_table_size	= ARRAY_SIZE(features),
	.driver.name		= KBUILD_MODNAME,
	.driver.owner		= THIS_MODULE,
	.id_table		= id_table,
	.probe			= virt_fastrpc_probe,
	.remove			= virt_fastrpc_remove,
};

static int __init virtio_fastrpc_init(void)
{
	return register_virtio_driver(&virtio_fastrpc_driver);
}

static void __exit virtio_fastrpc_exit(void)
{
	unregister_virtio_driver(&virtio_fastrpc_driver);
}
module_init(virtio_fastrpc_init);
module_exit(virtio_fastrpc_exit);

MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("Virtio fastrpc driver");
MODULE_LICENSE("GPL v2");
MODULE_IMPORT_NS(DMA_BUF);
