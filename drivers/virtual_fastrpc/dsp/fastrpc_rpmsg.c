// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2011-2018, The Linux Foundation. All rights reserved.
 * Copyright (c) 2018, Linaro Limited
 * Copyright (c) 2022-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/of_platform.h>
#include <linux/rpmsg.h>

#include "fastrpc_common.h"
#include "fastrpc_core.h"

struct fastrpc_channel_ctx* get_current_channel_ctx(struct device *dev)
{
	return dev_get_drvdata(dev->parent);
}

static int fastrpc_rpmsg_probe(struct rpmsg_device *rpdev)
{
	struct device *rdev = &rpdev->dev;
        struct fastrpc_channel_ctx *data;
	const char *domain;
	int i, err, domain_id = -1;

	RPC_INFO("started\n");

	err = of_property_read_string(rpdev->dev.parent->of_node,
					"label", &domain);
	if (err) {
		RPC_INFO("domain not specified in DT\n");
		return err;
	}

	for (i = 0; i <= CDSP_DOMAIN_ID; i++) {
		if (!strcmp(domains[i], domain)) {
			domain_id = i;
			break;
		}
	}

	if (domain_id < 0) {
		RPC_INFO("invalid domain name %s\n", domain);
		return -EINVAL;
	}

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	atomic_set(&data->teardown, 0);
	kref_init(&data->refcount);
	dev_set_drvdata(&rpdev->dev, data);
	rdev->dma_mask = &data->dma_mask;
	INIT_LIST_HEAD(&data->users);
	dma_set_mask_and_coherent(rdev, DMA_BIT_MASK(32));
	spin_lock_init(&data->lock);
	idr_init(&data->ctx_idr);
	ida_init(&data->tgid_frpc_ida);
	data->domain_id = domain_id;
	data->max_sess_per_proc = FASTRPC_MAX_SESSIONS_PER_PROCESS;
	data->rpdev = rpdev;

	if (domain_id == CDSP_DOMAIN_ID) {
		data->unsigned_support = true;
		/* create both device nodes so that we can allow both Signed and Unsigned PD */
		err = fastrpc_device_register(rdev, data, true, domains[domain_id]);
		if (err)
			goto bail;
		err = fastrpc_device_register(rdev, data, false, domains[domain_id]);
		if (err)
			goto bail;
		data->cpuinfo_todsp = FASTRPC_CPUINFO_EARLY_WAKEUP;
	} else {
		err = -EINVAL;
		goto bail;
	}

	fastrpc_update_gctx(data, 1);
	RPC_INFO("opened rpmsg channel for %s", domain);
	return 0;
bail:
	if (data->fdevice)
		misc_deregister(&data->fdevice->miscdev);
	if (data->secure_fdevice)
		misc_deregister(&data->secure_fdevice->miscdev);
	kfree(data);

	return err;
}

static void fastrpc_rpmsg_remove(struct rpmsg_device *rpdev)
{
	struct fastrpc_channel_ctx *cctx = dev_get_drvdata(&rpdev->dev);
	struct fastrpc_user *user;
	unsigned long flags;

	RPC_INFO("started\n");

	spin_lock_irqsave(&cctx->lock, flags);
	atomic_set(&cctx->teardown, 1);
	list_for_each_entry(user, &cctx->users, user) {
		fastrpc_notify_users(user);
	}
	spin_unlock_irqrestore(&cctx->lock, flags);

	if (cctx->fdevice)
		misc_deregister(&cctx->fdevice->miscdev);

	if (cctx->secure_fdevice)
		misc_deregister(&cctx->secure_fdevice->miscdev);

	list_for_each_entry(user, &cctx->users, user) {
		fastrpc_free_user(user);
	}
	RPC_INFO("closing rpmsg channel for %s", domains[cctx->domain_id]);
	cctx->dev = NULL;
	cctx->rpdev = NULL;
	fastrpc_update_gctx(cctx, 0);
	fastrpc_channel_ctx_put(cctx);
}

static int fastrpc_rpmsg_callback(struct rpmsg_device *rpdev, void *data,
					int len, void *priv, u32 addr)
{
	struct fastrpc_channel_ctx *cctx = dev_get_drvdata(&rpdev->dev);

	return fastrpc_handle_rpc_response(cctx, data, len);
}

static const struct rpmsg_device_id fastrpc_rpmsg_match[] = {
	{ FASTRPC_GLINK_GUID },
	{ },
};

static const struct of_device_id fastrpc_rpmsg_of_match[] = {
	{ .compatible = "qcom,fastrpc" },
	{ },
};

MODULE_DEVICE_TABLE(of, fastrpc_rpmsg_of_match);

static struct rpmsg_driver fastrpc_driver = {
	.id_table = fastrpc_rpmsg_match,
	.probe = fastrpc_rpmsg_probe,
	.remove = fastrpc_rpmsg_remove,
	.callback = fastrpc_rpmsg_callback,
	.drv = {
		.name = "qcom,fastrpc",
		.of_match_table = fastrpc_rpmsg_of_match,
	},
};

int fastrpc_transport_send(struct fastrpc_channel_ctx *cctx,
				void *rpc_msg, uint32_t rpc_msg_size)
{
	int err = 0;

	if (atomic_read(&cctx->teardown))
		return -EPIPE;

	err = rpmsg_send(cctx->rpdev->ept, rpc_msg, rpc_msg_size);
	return err;
}

int fastrpc_transport_init(void)
{
	int ret;

	ret = register_rpmsg_driver(&fastrpc_driver);
	if (ret < 0) {
		RPC_ERR("fastrpc: failed to register rpmsg driver\n");
		return ret;
	}

	return 0;
}

void fastrpc_transport_deinit(void)
{
	unregister_rpmsg_driver(&fastrpc_driver);
}

