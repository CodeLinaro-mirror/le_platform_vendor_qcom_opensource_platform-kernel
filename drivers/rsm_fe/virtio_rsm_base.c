/* SPDX-License-Identifier: GPL-2.0-only
*
*  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. 
*/
#include <linux/version.h>
#include "virtio_rsm_base.h"
#define VIRTIO_RSM_F_NSP_SHARING    7 /* Bit as defined in virtio vdev */
struct virtio_rsm_dev* g_vdevrsm = NULL;

/*#define RSM_FE_TEST*/
#ifdef RSM_FE_TEST
#define RSM_FE_EXTENDED_TEST 1
static void TestRSMFENormalFlow(bool newAPI, uint32_t nspID, const char *jobName);
#if RSM_FE_EXTENDED_TEST
static void TestRSMFEAcquireInvalid(uint32_t nspID);
static void TestRSMFEMaxRegisters(uint32_t nspID);
static void TestRSMFEDoubleRegisterFlow(uint32_t nspID);
static void TestRSMFEDoubleUnregisterFlow(uint32_t nspID);
#endif
#endif /*RSM_FE_TEST*/

static void * txbuf_get(void)
{
	unsigned int len = 0;
	void *ret = NULL;
	/*
	 * either pick the next unused tx buffer
	 */
	if (g_vdevrsm->txBufUsedCount < g_vdevrsm->num_buf)
    {
		ret = g_vdevrsm->txbufs[g_vdevrsm->txBufUsedCount++];
    }
	/* or recycle a used one */
	else
    {
		ret = virtqueue_get_buf(g_vdevrsm->vq_tx, &len);
    }
	return ret;
}

/**************send the tx buffer to PVM via tx virtqueue************/
int virt_rsm_txbuf(struct virtio_rsm_txbuf *send_buf)
{
    struct scatterlist sg[1];
    unsigned long flags;
    int err = NO_ERROR;
    /*for multiple request get a tx_buf from vring */
    spin_lock_irqsave(&g_vdevrsm->vqtx_lock, flags);
    struct virtio_rsm_txbuf *cpu_addr = txbuf_get();
    if(cpu_addr == NULL)
    {
        spin_unlock_irqrestore(&g_vdevrsm->vqtx_lock, flags);
        LOG_RSMFE(LEVEL_ERR, " rsm txbuf send failed. No free buffers \n");
        return -ENOSPC;
    }
    memcpy(cpu_addr,send_buf,sizeof(struct virtio_rsm_txbuf));

    sg_init_one(sg, cpu_addr, sizeof(struct virtio_rsm_txbuf));
    err = virtqueue_add_outbuf(g_vdevrsm->vq_tx, sg, 1, cpu_addr, GFP_KERNEL);
    if (err) {
        spin_unlock_irqrestore(&g_vdevrsm->vqtx_lock, flags);
        LOG_RSMFE(LEVEL_ERR, " rsm txbuf send failed \n");
        return err;
    }
    virtqueue_kick(g_vdevrsm->vq_tx);

    spin_unlock_irqrestore(&g_vdevrsm->vqtx_lock, flags);
    LOG_RSMFE(LEVEL_INFO, " rsm txbuf sent for msgid - %d! \n",send_buf->msg_id);
    return err;
}

/* add the buffer back to the remote processor's virtqueue */
static void rxbuf_emplace(struct virtio_rsm_rxbuf *rxBuf)
{
	struct scatterlist sg[1];
	int err = 0;

	sg_init_one(sg, rxBuf, sizeof(struct virtio_rsm_rxbuf));

	err = virtqueue_add_inbuf(g_vdevrsm->vq_rx, sg, 1, rxBuf, GFP_KERNEL);
	if (err)
    {
		LOG_RSMFE(LEVEL_ERR, " rsm RxBuf emplace failed: %d\n", err);
	}
    else
    {
	    virtqueue_kick(g_vdevrsm->vq_rx);
    }
}

/**************receive callback on rx buffer from PVM via rx virtqueue************/
static void virtio_rsm_recv_cb(struct virtqueue *vq_rx)
{
    struct virtio_rsm_dev *dev = vq_rx->vdev->priv;
    struct virtio_rsm_rxbuf* buf;
    unsigned long flags;
    unsigned int len;

    LOG_RSMFE(LEVEL_INFO, "RSM virtq rx notification received\n");
    spin_lock_irqsave(&g_vdevrsm->vqrx_lock, flags);
    /* Receive all the messages */
    for(;;)
    {
        buf = virtqueue_get_buf(dev->vq_rx, &len);
        if (NULL == buf) {
            LOG_RSMFE(LEVEL_DEBUG, "RSM Rx: no more buffers \n");
            spin_unlock_irqrestore(&g_vdevrsm->vqrx_lock, flags);
            break;
        }

        if(buf->msg_id >= MAX_CLIENT || buf->msg_id < 0)
        {
            /* Invalid client handle received */
            LOG_RSMFE(LEVEL_ERR, "RSM Rx: client id %d not valid. \n", buf->msg_id);
            rxbuf_emplace(buf);
            continue;
        }

        memcpy(&g_vdevrsm->client_list[buf->msg_id].rxbuf,buf,sizeof(struct virtio_rsm_rxbuf));
        dev_info(&vq_rx->vdev->dev, "RSM Rx: Received rxbuf for msgid %d!!\n",buf->msg_id);   
        /* Unblock the client that is waiting */
        complete(&g_vdevrsm->client_list[buf->msg_id].work);
        rxbuf_emplace(buf);
    }
}

static int init_vqs(struct virtio_rsm_dev *vdev_rsm)
{
    int i, tearIter, err = 0;
	struct virtqueue *vqs[2];
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
    struct virtqueue_info vqs_info[] = {
        {"output", NULL },
        {"input", virtio_rsm_recv_cb },
    };

    err = virtio_find_vqs(vdev_rsm->vdev, 2, vqs, vqs_info, NULL);
#else
	static const char * const names[] = { "output", "input" };
	vq_callback_t *cbs[] = { NULL, virtio_rsm_recv_cb };
	err = virtio_find_vqs(vdev_rsm->vdev, 2, vqs, cbs, names, NULL);
#endif /* LINUX_VERSION_CODE */
	if (err != 0)
    {
        LOG_RSMFE(LEVEL_ERR, "Unable to setup vertio queues \n");
		return err;
    }

    vdev_rsm->vq_tx = vqs[0];
	vdev_rsm->vq_rx = vqs[1];

    vdev_rsm->num_buf = virtqueue_get_vring_size(vdev_rsm->vq_rx);
    dev_info(vdev_rsm->dev, "size of vring %d!!!\n",vdev_rsm->num_buf);
    vdev_rsm->rxbufs = kcalloc(vdev_rsm->num_buf, sizeof(void *), GFP_KERNEL);
	if (NULL == vdev_rsm->rxbufs)
    {
        err = -ENOMEM;
        LOG_RSMFE(LEVEL_ERR, "Unable to alloc rxbuf \n");
        return err;
	}
    vdev_rsm->txbufs = kcalloc(vdev_rsm->num_buf, sizeof(void *), GFP_KERNEL);
	if (NULL == vdev_rsm->txbufs)
    {
        err = -ENOMEM;
        LOG_RSMFE(LEVEL_ERR, "Unable to alloc txbuf \n");
        kfree(vdev_rsm->rxbufs);
        vdev_rsm->rxbufs = NULL;
        return err;
	}

    vdev_rsm->order = get_order(DEF_BUFF_SIZE);
	for (i = 0; i < vdev_rsm->num_buf; i++) {
		vdev_rsm->rxbufs[i] = (void *)__get_free_pages(GFP_KERNEL, vdev_rsm->order);
		if (!vdev_rsm->rxbufs[i])
        {
            err = -ENOMEM;
            LOG_RSMFE(LEVEL_ERR, "Unable to get free pages for rxbuf \n");
            for(tearIter=0; tearIter < i; tearIter++)
            {
                free_pages((unsigned long)vdev_rsm->rxbufs[tearIter], vdev_rsm->order);
            }
            kfree(vdev_rsm->rxbufs);
            kfree(vdev_rsm->txbufs);
            vdev_rsm->rxbufs = NULL;
            vdev_rsm->txbufs = NULL;
            return err;
		}
	}

	for (i = 0; i < vdev_rsm->num_buf; i++)
    {
        vdev_rsm->txbufs[i] = (void *)__get_free_pages(GFP_KERNEL, vdev_rsm->order);
        if (!vdev_rsm->txbufs[i]) {
            LOG_RSMFE(LEVEL_ERR, "Unable to get free pages for txbuf \n");
            for(tearIter=0; tearIter < i; tearIter++)
            {
                free_pages((unsigned long)vdev_rsm->txbufs[tearIter], vdev_rsm->order);
            }
            for(tearIter=0; tearIter < vdev_rsm->num_buf; tearIter++)
            {
                free_pages((unsigned long)vdev_rsm->rxbufs[tearIter], vdev_rsm->order);
            }
            kfree(vdev_rsm->rxbufs);
            kfree(vdev_rsm->txbufs);
            vdev_rsm->rxbufs = NULL;
            vdev_rsm->txbufs = NULL;
            err = -ENOMEM;
            return err;
        }
    }

    //Initialise used TX Buf count to 0
    vdev_rsm->txBufUsedCount = 0;
    spin_lock_init(&vdev_rsm->vqtx_lock);
    spin_lock_init(&vdev_rsm->vqrx_lock);

    dev_info(vdev_rsm->dev, "vring calloc successful \n");
    return err;
}

static void init_client_table(void)
{
    memset(g_vdevrsm->client_list, 0, sizeof(g_vdevrsm->client_list));
}

static int virtio_rsm_probe(struct virtio_device *vdev)
{
    struct virtio_rsm_dev *vdev_rsm = NULL;
    int i,err;

	if (!virtio_has_feature(vdev, VIRTIO_F_VERSION_1))
    {
		dev_err(&vdev->dev,
			"RSM BE not loaded on the host\n");
		return -ENODEV;
    }
	if (!virtio_has_feature(vdev, VIRTIO_RSM_F_NSP_SHARING)) {
		dev_err(&vdev->dev,
			"NSP Sharing is not enabled on the host\n");
		return -ENODEV;
	}
    vdev_rsm = kzalloc(sizeof(struct virtio_rsm_dev), GFP_KERNEL);
  	if (!vdev_rsm) {
  		err = -ENOMEM;
        LOG_RSMFE(LEVEL_ERR, "Unable to alloc mem for device \n");
  		return err;
  	}

    g_vdevrsm = vdev_rsm;
    vdev->priv = vdev_rsm;
    vdev_rsm->vdev = vdev;
    vdev_rsm->dev = vdev->dev.parent;

 	err = init_vqs(vdev_rsm);
	if (err) 
    {
		dev_err(&vdev->dev, "failed to initialized virtqueue\n");
		return err;
	}  

    /* from this point on, the device can notify and get callbacks */
    virtio_device_ready(vdev);

    /* set up the receive buffers */
	for (i = 0; i < vdev_rsm->num_buf; i++) {
		struct scatterlist sg;
		void *cpu_addr = vdev_rsm->rxbufs[i];

		sg_init_one(&sg, cpu_addr, MAX_RX_BUF_SIZE);
		err = virtqueue_add_inbuf(vdev_rsm->vq_rx, &sg, 1, cpu_addr, GFP_KERNEL);
		WARN_ON(err); /* sanity check; this can't really happen */
	}

    spin_lock_init(&vdev_rsm->vq_clientlock);
    init_client_table();

    virtqueue_kick(vdev_rsm->vq_rx);

    /**********************testing APIs****************/
#ifdef RSM_FE_TEST
    TestRSMFENormalFlow(false, 0U, NULL);
    TestRSMFENormalFlow(true, 0U, NULL);
    //Calling with a job name not present in the job table. Should use the default "ANY_#" jobs.
    TestRSMFENormalFlow(true, 0U, "calc0");
    TestRSMFENormalFlow(true, 1U, "nsp1"); // This register will fail for Monaco and should pass for lemans assuming NSP1 is shared in device table
#if RSM_FE_EXTENDED_TEST
    TestRSMFEAcquireInvalid(0U);
    TestRSMFEAcquireInvalid(1U); //For Lemans
    TestRSMFEDoubleRegisterFlow(0U);
    TestRSMFEDoubleRegisterFlow(1U); //For Lemans
    TestRSMFEDoubleUnregisterFlow(0U);
    TestRSMFEDoubleUnregisterFlow(1U); //For Lemans
    TestRSMFEMaxRegisters(0U);
    TestRSMFEMaxRegisters(1U); //For Lemans
#endif /* RSM_FE_EXTENDED_TEST */
#endif /* RSM_FE_TEST */
    /*************************************************/
    //virt_rsm_init_txbuf(vdev_rsm);  such function can be used to do handshake before the comm starts
    dev_info(&vdev->dev, "RSM Virtio driver probe successful \n");
    return 0;
}

static void virtio_rsm_remove(struct virtio_device *vdev)
{
    struct virtio_rsm_dev *dev = vdev->priv;
    void *buf , *buf1;
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
    { VIRTIO_ID_RSM, VIRTIO_DEV_ANY_ID },
    { 0 },
};

static unsigned int features[] = {
	VIRTIO_RSM_F_NSP_SHARING,
};

static struct virtio_driver virtio_rsm_driver = {
    .feature_table		= features,
    .feature_table_size	= ARRAY_SIZE(features),
    .id_table =     id_table,
    .probe =        virtio_rsm_probe,
    .remove =       virtio_rsm_remove,
    .driver.name = KBUILD_MODNAME,
    .driver.owner = THIS_MODULE,
};

static int __init virtio_rsm_init(void)
{
    LOG_RSMFE(LEVEL_INFO, " DRIVER RSM FE Init\n");
	return register_virtio_driver(&virtio_rsm_driver);
}

static void __exit virtio_rsm_exit(void)
{
	unregister_virtio_driver(&virtio_rsm_driver);
}

module_init(virtio_rsm_init);
module_exit(virtio_rsm_exit);

EXPORT_SYMBOL_GPL(rsm_register);
EXPORT_SYMBOL_GPL(rsm_register_for_nsp);
EXPORT_SYMBOL_GPL(rsm_acquire);
EXPORT_SYMBOL_GPL(rsm_release_v2);
EXPORT_SYMBOL_GPL(rsm_unregister_v2);
EXPORT_SYMBOL_GPL(rsm_unregister_batch);

//module_virtio_driver(virtio_rsm_driver);
MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("RSM virtio driver");
MODULE_LICENSE("GPL v2");

#ifdef RSM_FE_TEST
static void TestRSMFENormalFlow(bool newAPI, uint32_t nspID, const char *jobName)
{
    int err;
    rsm_handle handle;
    rsm_acquire_rsp_v2 acq_response;
    char job_name[6] = "ANY_0"; /* Replace with a string available in the Job Table for the specific NSP */
    static uint32_t upid = 0x80000000U;
    uint32_t tid = 1;
    ++upid; //Increment the UPID for each test
    if(jobName == NULL)
    {
        jobName = job_name;
    }
    if(newAPI == false)
    {
        err = rsm_register(&handle,upid,tid);
    }
    else
    {
        RSMRegisterPDUType regData = {upid, tid, nspID};
        err = rsm_register_for_nsp(&handle, regData);
    }
    if(err != NO_ERROR)
    {
        LOG_RSMFE(LEVEL_ERR, " RSM FE Normal Test Register failed. Err %d\n", err);
        return;
    }
    err = rsm_acquire(handle, jobName, &acq_response);
    if(err != NO_ERROR)
    {
        LOG_RSMFE(LEVEL_ERR, " RSM FE Normal Test Acquire jobName '%s' failed. Err %d\n", jobName, err);
    }
    else
    {
        err = rsm_release_v2(handle, acq_response.token);
        if(err != NO_ERROR)
        {
            LOG_RSMFE(LEVEL_ERR, " RSM FE Normal Test Release failed. Err %d\n", err);
        }
    }
    err = rsm_unregister_v2(handle);
    if(err != NO_ERROR)
    {
        LOG_RSMFE(LEVEL_ERR, " RSM FE Normal Test Unregister failed. Err %d\n", err);
    }
}

#if RSM_FE_EXTENDED_TEST
static void TestRSMFEAcquireInvalid(uint32_t nspID)
{
    int err;
    rsm_handle handle;
    rsm_acquire_rsp_v2 acq_response;
    char jobNameTooLong[] = "ThisStringCannotBeAJobNameAsItIsLongerThanSixtyThreeCharacters64";
    static uint32_t upid = 0x90000000U;
    uint32_t tid = 1;
    ++upid; //Increment the UPID for each test
    RSMRegisterPDUType regData = {upid, tid, nspID};
    err = rsm_register_for_nsp(&handle, regData);
if(err != NO_ERROR)
    {
        LOG_RSMFE(LEVEL_ERR, " RSM FE Invalid Acquire Test, Register failed. Err %d\n", err);
        return;
    }

    /* Acquire with NULL job name */
    err = rsm_acquire(handle, NULL, &acq_response);
    if(err != NO_ERROR)
    {
        LOG_RSMFE(LEVEL_ERR, " RSM FE Invalid Acquire Test, Acquire NULL Job failed. Err %d. PASS\n", err);
    }
    else
    {
        LOG_RSMFE(LEVEL_ERR, " RSM FE Invalid Acquire Test, Acquire NULL Job got no error. FAIL\n", err);
        err = rsm_release_v2(handle, acq_response.token);
        if(err != NO_ERROR)
        {
            LOG_RSMFE(LEVEL_ERR, " RSM FE Invalid Acquire Test, Release failed. Err %d\n", err);
        }
    }

    /* Acquire with long job name */
    err = rsm_acquire(handle, jobNameTooLong, &acq_response);
    if(err != NO_ERROR)
    {
        LOG_RSMFE(LEVEL_ERR, " RSM FE Invalid Acquire Test, Acquire LONG Job Name failed. Err %d. PASS\n", err);
    }
    else
    {
        LOG_RSMFE(LEVEL_ERR, " RSM FE Invalid Acquire Test, Acquire LONG Job Name got no error. FAIL\n", err);
        err = rsm_release_v2(handle, acq_response.token);
        if(err != NO_ERROR)
        {
            LOG_RSMFE(LEVEL_ERR, " RSM FE Invalid Acquire Test, Release failed. Err %d\n", err);
        }
    }

    err = rsm_unregister_v2(handle);
    if(err != NO_ERROR)
    {
        LOG_RSMFE(LEVEL_ERR, " RSM FE Invalid Acquire Test, Unregister failed. Err %d\n", err);
    }
}

static void TestRSMFEDoubleRegisterFlow(uint32_t nspID)
{
    int err;
    rsm_handle handle = 0U, handle2 = 0U;
    static uint32_t upid = 0xA0000000U;
    uint32_t tid = 1;
    ++upid; //Increment the UPID for each test
    RSMRegisterPDUType regData = {upid, tid, nspID};
    err = rsm_register_for_nsp(&handle, regData);
    if(err != NO_ERROR)
    {
        LOG_RSMFE(LEVEL_ERR, " RSM FE Double Register Test Register 1 failed. Err %d\n", err);
        return;
    }

    err = rsm_register_for_nsp(&handle, regData);
    if(err != NO_ERROR)
    {
        LOG_RSMFE(LEVEL_ERR, " RSM FE Double Register Test Register 2 returned Err %d. PASS\n", err);
    }
    else
    {
        LOG_RSMFE(LEVEL_ERR, " RSM FE Double Register Test Register 2 got no error. FAIL\n");
        err = rsm_unregister_v2(handle2);
        if(err != NO_ERROR)
        {
            LOG_RSMFE(LEVEL_ERR, " RSM FE Double Register Test Unregister 2 returned. Err %d.\n", err);
        }
    }
    err = rsm_unregister_v2(handle);
    if(err != NO_ERROR)
    {
        LOG_RSMFE(LEVEL_ERR, " RSM FE Double Register Test Unregister 1 returned. Err %d.\n", err);
    }
}

static void TestRSMFEDoubleUnregisterFlow(uint32_t nspID)
{
    int err;
    rsm_handle handle = 0U;
    static uint32_t upid = 0xB0000000U;
    uint32_t tid = 1;
    ++upid; //Increment the UPID for each test
    RSMRegisterPDUType regData = {upid, tid, nspID};
    err = rsm_register_for_nsp(&handle, regData);
    if(err != NO_ERROR)
    {
        LOG_RSMFE(LEVEL_ERR, " RSM FE Double Unregister Test: Register failed. Err %d\n", err);
        return;
    }
    err = rsm_unregister_v2(handle);
    if(err != NO_ERROR)
    {
        LOG_RSMFE(LEVEL_ERR, " RSM FE Double Unregister Test Unregister 1 Failed. Err %d.\n", err);
    }
    err = rsm_unregister_v2(handle);
    if(err != NO_ERROR)
    {
        LOG_RSMFE(LEVEL_ERR, " RSM FE Double Unregister Test Unregister 2 returned Err %d. PASS\n", err);
    }
    else
    {
        LOG_RSMFE(LEVEL_ERR, " RSM FE Double Unregister Test Unregister 2 got no error. FAIL\n");
    }
}

static void TestRSMFEMaxRegisters(uint32_t nspID)
{
    int err;
    rsm_handle handle[MAX_CLIENT+1];
    static uint32_t upid = 0xC0000000U;
    uint32_t tid = 1;
    ++upid; //Increment the UPID for each test
    uint32_t handleIter = 0;
    for(; handleIter<(MAX_CLIENT+1U); handleIter++)
    {
        RSMRegisterPDUType regData = {upid, tid+handleIter, nspID};
        err = rsm_register_for_nsp(&handle[handleIter], regData);
        if(err != NO_ERROR)
        {
            LOG_RSMFE(LEVEL_ERR, " RSM FE Max Register Test: Register %d failed. Err %d. %s\n", handleIter+1U, err, (handleIter==MAX_CLIENT)?"PASS":"FAIL");
        }
        else
        {
            LOG_RSMFE(LEVEL_ERR, " RSM FE Max Register Test: Register %d succeeded. %s\n", handleIter+1U, (handleIter!=MAX_CLIENT)?"PASS":"FAIL");
        }
    }

    for(handleIter = 0; handleIter<MAX_CLIENT; handleIter++)
    {
        err = rsm_unregister_v2(handle[handleIter]);
        if(err != NO_ERROR)
        {
            LOG_RSMFE(LEVEL_ERR, " RSM FE Max Register Test: Unregister %d failed. Err %d. %s\n", handleIter+1U, err, (handleIter==MAX_CLIENT)?"PASS":"FAIL");
        }
        else
        {
            LOG_RSMFE(LEVEL_ERR, " RSM FE Max Register Test: Unregister %d succeeded. %s\n", handleIter+1U, (handleIter!=MAX_CLIENT)?"PASS":"FAIL");
        }
    }
}
#endif /* RSM_FE_EXTENDED_TEST */
#endif /*RSM_FE_TEST*/
