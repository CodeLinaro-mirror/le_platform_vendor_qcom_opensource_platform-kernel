/* SPDX-License-Identifier: GPL-2.0-only
*
*  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. 
*/
#include <linux/version.h>
#include "virtio_compressched_base.h"

/* Forward declarations for the character device init/exit functions
 * defined in compressched_cdev.c */
int  compressched_cdev_init(void);
void compressched_cdev_exit(void);
#define VIRTIO_COMPRESSCHED_F_NSP_SHARING    7 /* Bit as defined in virtio vdev */
struct virtio_compressched_dev* g_vdevcompressched = NULL;

static void * txbuf_get(void)
{
	unsigned int len = 0;
	void *ret = NULL;
	/*
	 * either pick the next unused tx buffer
	 */
	if (g_vdevcompressched->txBufUsedCount < g_vdevcompressched->num_buf)
	{
		ret = g_vdevcompressched->txbufs[g_vdevcompressched->txBufUsedCount++];
	}
	/* or recycle a used one */
	else
	{
	  ret = virtqueue_get_buf(g_vdevcompressched->vq_tx, &len);
	}
	return ret;
}

/**************send the tx buffer to PVM via tx virtqueue************/
int virt_compressched_txbuf(struct virtio_compressched_txbuf *send_buf)
{
    struct scatterlist sg[1];
    unsigned long flags;
    int err = NO_ERROR;
    /*for multiple request get a tx_buf from vring */
    spin_lock_irqsave(&g_vdevcompressched->vqtx_lock, flags);
    struct virtio_compressched_txbuf *cpu_addr = txbuf_get();
    if(cpu_addr == NULL)
    {
        spin_unlock_irqrestore(&g_vdevcompressched->vqtx_lock, flags);
        LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched txbuf send failed. No free buffers \n");
        return -ENOSPC;
    }
    memcpy(cpu_addr,send_buf,sizeof(struct virtio_compressched_txbuf));

    sg_init_one(sg, cpu_addr, sizeof(struct virtio_compressched_txbuf));
    err = virtqueue_add_outbuf(g_vdevcompressched->vq_tx, sg, 1, cpu_addr, GFP_KERNEL);
    if (err) {
        spin_unlock_irqrestore(&g_vdevcompressched->vqtx_lock, flags);
        LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched txbuf send failed \n");
        return err;
    }
    virtqueue_kick(g_vdevcompressched->vq_tx);

    spin_unlock_irqrestore(&g_vdevcompressched->vqtx_lock, flags);
    LOG_COMPRESSCHEDFE(LEVEL_INFO, " compressched txbuf sent for msgid - %d! \n",send_buf->msg_id);
    return err;
}

/* add the buffer back to the remote processor's virtqueue */
static void rxbuf_emplace(struct virtio_compressched_rxbuf *rxBuf)
{
	struct scatterlist sg[1];
	int err = 0;

	sg_init_one(sg, rxBuf, sizeof(struct virtio_compressched_rxbuf));

	err = virtqueue_add_inbuf(g_vdevcompressched->vq_rx, sg, 1, rxBuf, GFP_KERNEL);
	if (err)
  {
    LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched RxBuf emplace failed: %d\n", err);
	}
	else
  {
    virtqueue_kick(g_vdevcompressched->vq_rx);
  }
}

/**************receive callback on rx buffer from PVM via rx virtqueue************/
static void virtio_compressched_recv_cb(struct virtqueue *vq_rx)
{
    struct virtio_compressched_dev *dev = vq_rx->vdev->priv;
    struct virtio_compressched_rxbuf* buf;
    unsigned long flags;
    unsigned int len;

    LOG_COMPRESSCHEDFE(LEVEL_INFO, "COMPRESSCHED virtq rx notification received\n");
    spin_lock_irqsave(&g_vdevcompressched->vqrx_lock, flags);
    /* Receive all the messages */
    for(;;)
    {
        buf = virtqueue_get_buf(dev->vq_rx, &len);
        if (NULL == buf) {
            LOG_COMPRESSCHEDFE(LEVEL_DEBUG, "COMPRESSCHED Rx: no more buffers \n");
            spin_unlock_irqrestore(&g_vdevcompressched->vqrx_lock, flags);
            break;
        }

        /* msg_id == MAX_CLIENT signals an unsolicited fault notification */
        if (buf->msg_id == MAX_CLIENT) {
            if (buf->cmd == COMPRESSCHED_FAULT_NOTIFY_LIB) {
                unsigned long fl;
                spin_lock_irqsave(&g_vdevcompressched->fault_lock, fl);
                g_vdevcompressched->fault_pending = buf->fault_notify;
                spin_unlock_irqrestore(&g_vdevcompressched->fault_lock, fl);
                dev_info(&vq_rx->vdev->dev,
                         "COMPRESSCHED Rx: fault notify domain=%d type=%u\n",
                         buf->fault_notify.domain_id,
                         buf->fault_notify.fault_type);
                complete(&g_vdevcompressched->fault_work);
            } else {
                LOG_COMPRESSCHEDFE(LEVEL_ERR,
                    "COMPRESSCHED Rx: unexpected cmd %d for msg_id MAX_CLIENT\n",
                    buf->cmd);
            }
            rxbuf_emplace(buf);
            continue;
        }

        if(buf->msg_id >= MAX_CLIENT || buf->msg_id < 0)
        {
            /* Invalid client handle received */
            LOG_COMPRESSCHEDFE(LEVEL_ERR, "COMPRESSCHED Rx: client id %d not valid. \n", buf->msg_id);
            rxbuf_emplace(buf);
            continue;
        }

        memcpy(&g_vdevcompressched->client_list[buf->msg_id].rxbuf,buf,sizeof(struct virtio_compressched_rxbuf));
        dev_info(&vq_rx->vdev->dev, "COMPRESSCHED Rx: Received rxbuf for msgid %d!!\n",buf->msg_id);
        /* Unblock the client that is waiting */
        complete(&g_vdevcompressched->client_list[buf->msg_id].work);
        rxbuf_emplace(buf);
    }
}

static int init_vqs(struct virtio_compressched_dev *vdev_compressched)
{
    int i, tearIter, err = 0;
	struct virtqueue *vqs[2];
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
    struct virtqueue_info vqs_info[] = {
        {"output", NULL },
        {"input", virtio_compressched_recv_cb },
    };

    err = virtio_find_vqs(vdev_compressched->vdev, 2, vqs, vqs_info, NULL);
#else
	static const char * const names[] = { "output", "input" };
	vq_callback_t *cbs[] = { NULL, virtio_compressched_recv_cb };
	err = virtio_find_vqs(vdev_compressched->vdev, 2, vqs, cbs, names, NULL);
#endif /* LINUX_VERSION_CODE */
	if (err != 0)
    {
        LOG_COMPRESSCHEDFE(LEVEL_ERR, "Unable to setup vertio queues \n");
		return err;
    }

    vdev_compressched->vq_tx = vqs[0];
	vdev_compressched->vq_rx = vqs[1];

    vdev_compressched->num_buf = virtqueue_get_vring_size(vdev_compressched->vq_rx);
    dev_info(vdev_compressched->dev, "size of vring %d!!!\n",vdev_compressched->num_buf);
    vdev_compressched->rxbufs = kcalloc(vdev_compressched->num_buf, sizeof(void *), GFP_KERNEL);
	if (NULL == vdev_compressched->rxbufs)
    {
        err = -ENOMEM;
        LOG_COMPRESSCHEDFE(LEVEL_ERR, "Unable to alloc rxbuf \n");
        return err;
	}
    vdev_compressched->txbufs = kcalloc(vdev_compressched->num_buf, sizeof(void *), GFP_KERNEL);
	if (NULL == vdev_compressched->txbufs)
    {
        err = -ENOMEM;
        LOG_COMPRESSCHEDFE(LEVEL_ERR, "Unable to alloc txbuf \n");
        kfree(vdev_compressched->rxbufs);
        vdev_compressched->rxbufs = NULL;
        return err;
	}

    vdev_compressched->order = get_order(DEF_BUFF_SIZE);
	for (i = 0; i < vdev_compressched->num_buf; i++) {
		vdev_compressched->rxbufs[i] = (void *)__get_free_pages(GFP_KERNEL, vdev_compressched->order);
		if (!vdev_compressched->rxbufs[i])
        {
            err = -ENOMEM;
            LOG_COMPRESSCHEDFE(LEVEL_ERR, "Unable to get free pages for rxbuf \n");
            for(tearIter=0; tearIter < i; tearIter++)
            {
                free_pages((unsigned long)vdev_compressched->rxbufs[tearIter], vdev_compressched->order);
            }
            kfree(vdev_compressched->rxbufs);
            kfree(vdev_compressched->txbufs);
            vdev_compressched->rxbufs = NULL;
            vdev_compressched->txbufs = NULL;
            return err;
		}
	}

	for (i = 0; i < vdev_compressched->num_buf; i++)
    {
        vdev_compressched->txbufs[i] = (void *)__get_free_pages(GFP_KERNEL, vdev_compressched->order);
        if (!vdev_compressched->txbufs[i]) {
            LOG_COMPRESSCHEDFE(LEVEL_ERR, "Unable to get free pages for txbuf \n");
            for(tearIter=0; tearIter < i; tearIter++)
            {
                free_pages((unsigned long)vdev_compressched->txbufs[tearIter], vdev_compressched->order);
            }
            for(tearIter=0; tearIter < vdev_compressched->num_buf; tearIter++)
            {
                free_pages((unsigned long)vdev_compressched->rxbufs[tearIter], vdev_compressched->order);
            }
            kfree(vdev_compressched->rxbufs);
            kfree(vdev_compressched->txbufs);
            vdev_compressched->rxbufs = NULL;
            vdev_compressched->txbufs = NULL;
            err = -ENOMEM;
            return err;
        }
    }

    //Initialise used TX Buf count to 0
    vdev_compressched->txBufUsedCount = 0;
    spin_lock_init(&vdev_compressched->vqtx_lock);
    spin_lock_init(&vdev_compressched->vqrx_lock);

    dev_info(vdev_compressched->dev, "vring calloc successful \n");
    return err;
}

static void init_client_table(void)
{
    int i;
    memset(g_vdevcompressched->client_list, 0, sizeof(g_vdevcompressched->client_list));
    for (i = 0; i < MAX_CLIENT; i++)
        g_vdevcompressched->handle_ht[i] = MAX_CLIENT;
}

static void init_client_upid(struct virtio_compressched_dev *vdev_compressched)
{
  if(vdev_compressched == NULL)
  {
    LOG_COMPRESSCHEDFE(LEVEL_ERR, "Invalid Args passed for init_client_upid function \n");
    return;
  }
  for(int i = 0; i < MAX_CLIENT; i++)
  {
    memset(vdev_compressched->client_list[i].upid, 0xFF, MAX_NUM_OF_NSP * sizeof(unsigned int));
  }
}

static int virtio_compressched_probe(struct virtio_device *vdev)
{
    struct virtio_compressched_dev *vdev_compressched = NULL;
    int i,err;

	if (!virtio_has_feature(vdev, VIRTIO_F_VERSION_1))
    {
		dev_err(&vdev->dev,
			"COMPRESSCHED BE not loaded on the host\n");
		return -ENODEV;
    }

    //#ifdef EN_NSP_SHARING
	if (!virtio_has_feature(vdev, VIRTIO_COMPRESSCHED_F_NSP_SHARING)) {
		dev_err(&vdev->dev,
			"NSP Sharing is not enabled on the host\n");
		//return -ENODEV;
	}
    //#endif
    vdev_compressched = kzalloc(sizeof(struct virtio_compressched_dev), GFP_KERNEL);
    if (!vdev_compressched) {
    err = -ENOMEM;
      LOG_COMPRESSCHEDFE(LEVEL_ERR, "Unable to alloc mem for device \n");
    return err;
    }

    g_vdevcompressched = vdev_compressched;
    vdev->priv = vdev_compressched;
    vdev_compressched->vdev = vdev;
    vdev_compressched->dev = vdev->dev.parent;
    init_client_table();

	err = init_vqs(vdev_compressched);
	if (err) 
    {
		dev_err(&vdev->dev, "failed to initialized virtqueue\n");
		return err;
	}  

    /* set up the receive buffers */
	for (i = 0; i < vdev_compressched->num_buf; i++) {
		struct scatterlist sg;
		void *cpu_addr = vdev_compressched->rxbufs[i];

		sg_init_one(&sg, cpu_addr, MAX_RX_BUF_SIZE);
		err = virtqueue_add_inbuf(vdev_compressched->vq_rx, &sg, 1, cpu_addr, GFP_KERNEL);
		WARN_ON(err); /* sanity check; this can't really happen */
	}

    spin_lock_init(&vdev_compressched->vq_clientlock);
    spin_lock_init(&vdev_compressched->fault_lock);
    init_completion(&vdev_compressched->fault_work);
    memset(&vdev_compressched->fault_pending, 0, sizeof(vdev_compressched->fault_pending));
    init_client_upid(g_vdevcompressched);

    /* from this point on, the device can notify and get callbacks */
    virtio_device_ready(vdev);

    virtqueue_kick(vdev_compressched->vq_rx);

    /**********************testing APIs****************/
//#define TEST_MODE
#ifdef TEST_MODE
    compressched_handle handle;
    compressched_acquire_rsp_v2 acq_response;
    char job_name[32] = "GVM_BASE_RES_REQ";
    unsigned int upid = 1234;
    unsigned int target_id = 1;
    dev_info(&vdev->dev, "TEST_MODE: calling compressched_register upid=%u target_id=%u\n", upid, target_id);
    err = compressched_register(&handle,upid,target_id);
    dev_info(&vdev->dev, "TEST_MODE: compressched_register returned err=%d handle=%d\n", err, handle);
    dev_info(&vdev->dev, "TEST_MODE: calling compressched_acquire job_name=%s\n", job_name);
    err = compressched_acquire(handle, job_name, &acq_response);
    dev_info(&vdev->dev, "TEST_MODE: compressched_acquire returned err=%d token=%u\n", err, acq_response.token);
    dev_info(&vdev->dev, "TEST_MODE: calling compressched_release_v2 handle=%d token=%u\n", handle, acq_response.token);
    err = compressched_release_v2(handle, acq_response.token);
    dev_info(&vdev->dev, "TEST_MODE: compressched_release_v2 returned err=%d\n", err);
    dev_info(&vdev->dev, "TEST_MODE: calling compressched_unregister_v2 handle=%d\n", handle);
    err = compressched_unregister_v2(handle);
    dev_info(&vdev->dev, "TEST_MODE: compressched_unregister_v2 returned err=%d\n", err);


    strscpy(job_name, "GVM_SUP_RES_REQ", sizeof(job_name));
    job_name[sizeof(job_name) - 1] = '\0';
    dev_info(&vdev->dev, "TEST_MODE: job_name set to %s\n", job_name);

    compressched_register_msg super_res_reg;
    super_res_reg.nsp_count = 2;
    super_res_reg.upid[0] = 1234;
    super_res_reg.upid[1] = 5678;
    super_res_reg.logical_id[0] = 16;
    super_res_reg.logical_id[1] = 17;    
    super_res_reg.target_id = 2;
    dev_info(&vdev->dev, "TEST_MODE: super_res_reg nsp_count=%u upid[0]=%u upid[1]=%u logical_id[0]=%u logical_id[1]=%u target_id=%u\n",
             super_res_reg.nsp_count, super_res_reg.upid[0], super_res_reg.upid[1],
             super_res_reg.logical_id[0], super_res_reg.logical_id[1], super_res_reg.target_id);
    dev_info(&vdev->dev, "TEST_MODE: calling compressched_register_v2\n");
    err = compressched_register_v2(&handle,&super_res_reg);
    dev_info(&vdev->dev, "TEST_MODE: compressched_register_v2 returned err=%d handle=%d\n", err, handle);
    dev_info(&vdev->dev, "TEST_MODE: calling compressched_acquire job_name=%s\n", job_name);
    err = compressched_acquire(handle, job_name, &acq_response);
    dev_info(&vdev->dev, "TEST_MODE: compressched_acquire returned err=%d token=%u\n", err, acq_response.token);
    dev_info(&vdev->dev, "TEST_MODE: calling compressched_release_v2 handle=%d token=%u\n", handle, acq_response.token);
    err = compressched_release_v2(handle, acq_response.token);
    dev_info(&vdev->dev, "TEST_MODE: compressched_release_v2 returned err=%d\n", err);
    dev_info(&vdev->dev, "TEST_MODE: calling compressched_unregister_v2 handle=%d\n", handle);
    err = compressched_unregister_v2(handle);
    dev_info(&vdev->dev, "TEST_MODE: compressched_unregister_v2 returned err=%d\n", err);

#endif
    /*************************************************/
    //virt_compressched_init_txbuf(vdev_compressched);  such function can be used to do handshake before the comm starts

    /* Register /dev/compressched_fe so the userspace library can open it */
    err = compressched_cdev_init();
    if (err) {
        dev_err(&vdev->dev, "COMPRESSCHED cdev init failed err=%d\n", err);
        return err;
    }

    dev_info(&vdev->dev, "COMPRESSCHED Virtio driver probe successful \n");
    return 0;
}

static void virtio_compressched_remove(struct virtio_device *vdev)
{
    struct virtio_compressched_dev *dev = vdev->priv;
    void *buf , *buf1;

    /* Deregister /dev/compressched_fe before tearing down virtio */
    compressched_cdev_exit();

    /*
        * disable vq interrupts: equivalent to
        * vdev->config->reset(vdev)
        */
    virtio_reset_device(vdev);

    /* detach unused buffers */
    while ((buf = virtqueue_detach_unused_buf(dev->vq_tx)) != NULL) {
        kfree(buf);
    }
    while ((buf1 = virtqueue_detach_unused_buf(dev->vq_rx)) != NULL) {
        kfree(buf1);
    }
    /* remove virtqueues */
    vdev->config->del_vqs(vdev);
    kfree(dev);
}

static struct virtio_device_id id_table[] = {
    { VIRTIO_ID_COMPRESSCHED, VIRTIO_DEV_ANY_ID },
    { 0 },
};

static unsigned int features[] = {
	VIRTIO_COMPRESSCHED_F_NSP_SHARING,
};

static struct virtio_driver virtio_compressched_driver = {
    .feature_table		= features,
    .feature_table_size	= ARRAY_SIZE(features),
    .id_table =     id_table,
    .probe =        virtio_compressched_probe,
    .remove =       virtio_compressched_remove,
    .driver.name = KBUILD_MODNAME,
    .driver.owner = THIS_MODULE,
};

static int __init virtio_compressched_init(void)
{
    LOG_COMPRESSCHEDFE(LEVEL_INFO, " DRIVER COMPRESSCHED FE Init\n");
	return register_virtio_driver(&virtio_compressched_driver);
}

static void __exit virtio_compressched_exit(void)
{
	unregister_virtio_driver(&virtio_compressched_driver);
}

module_init(virtio_compressched_init);
module_exit(virtio_compressched_exit);

EXPORT_SYMBOL_GPL(compressched_register);
EXPORT_SYMBOL_GPL(compressched_register_v2);
EXPORT_SYMBOL_GPL(compressched_acquire);
EXPORT_SYMBOL_GPL(compressched_acquire_legacy);
EXPORT_SYMBOL_GPL(compressched_release_v2);
EXPORT_SYMBOL_GPL(compressched_unregister_v2);
EXPORT_SYMBOL_GPL(compressched_unregister_batch);
EXPORT_SYMBOL_GPL(compressched_query);
EXPORT_SYMBOL_GPL(compressched_tracing_start);
EXPORT_SYMBOL_GPL(compressched_tracing_stop);
EXPORT_SYMBOL_GPL(compressched_debug_cmd_fn);
EXPORT_SYMBOL_GPL(compressched_subscribe_faults_fn);
EXPORT_SYMBOL_GPL(compressched_unsubscribe_faults_fn);
EXPORT_SYMBOL_GPL(compressched_avail_res_acquire);
EXPORT_SYMBOL_GPL(compressched_avail_res_release);

//module_virtio_driver(virtio_compressched_driver);
MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("COMPRESSCHED virtio driver");
MODULE_LICENSE("GPL v2");
