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

/*
 * Retrieves legacy information for a given fastrpc_domain.
 *
 * This function maps the domain's type to its corresponding legacy name
 * and ID, based on the following table:
 *
 *   Domain Type       | Legacy Name              | Legacy ID
 *   ------------------|--------------------------|---------------
 *   SDSP              | domains[SDSP_DOMAIN_ID]  | SDSP_DOMAIN_ID
 *   LPASS             | domains[ADSP_DOMAIN_ID]  | ADSP_DOMAIN_ID
 *   NSP(instance 0)   | domains[CDSP_DOMAIN_ID]  | CDSP_DOMAIN_ID
 *   NSP(instance 1)   | domains[CDSP1_DOMAIN_ID] | CDSP1_DOMAIN_ID
 *
 * @param domain Pointer to the fastrpc_domain structure to retrieve
 * legacy info
 *
 * @return 0 on success, or a negative error code on failure
 *
 * Error codes:
 *   -EINVAL: Invalid domain type
 */
static int fastrpc_retrieve_legacy_info(struct fastrpc_domain *domain){
	int err = 0;

	switch (domain->type) {
	case FASTRPC_NSP:
		if (domain->instance_id == 0) {
			domain->legacy_name = (char *)legacy_domains[CDSP_DOMAIN_ID];
			domain->legacy_id = CDSP_DOMAIN_ID;
		} else if (domain->instance_id == 1) {
			domain->legacy_name = (char *)legacy_domains[CDSP1_DOMAIN_ID];
			domain->legacy_id = CDSP1_DOMAIN_ID;
		}
		break;
	default:
		err = -EINVAL;
		break;
	}
	return err;
}

/*
 * Configures device nodes for a given fastrpc channel context and device.
 *
 * This function registers device nodes for the specified channel
 *
 * For NSP domains:
 *   - Registers a single device node with domain name
 *   - If the domain has a legacy set to true,
 *     registers two additional device nodes with the legacy name
 *     one for secure and non-secure.
 *
 * For non-NSP domains:
 *   - Registers a single device node with the domain name
 *   - If the domain has a legacy name,
 *     registers an additional secure device node with the
 *     legacy name
 *
 * @param data Pointer to the fastrpc channel context structure
 * @param rdev Pointer to the device structure
 *
 * @return 0 on success, or a negative error code on failure
 */
static int fastrpc_configure_device_nodes(struct fastrpc_channel_ctx *data,
		struct device *rdev)
{
	struct fastrpc_domain *domain = data->domain;
	int err = 0;

	if (is_device_discovery_supported()) {
		err = fastrpc_device_register(rdev, data, true, false,
					domain->name);
		if (err)
			return err;
	}

	if (domain->legacy) {
		err = fastrpc_retrieve_legacy_info(domain);
		if (err)
			return err;

		/* Register a secure device with legacy name */
		err = fastrpc_device_register(rdev, data, true, true,
				domain->legacy_name);
		if (err)
			return err;

		/* For NSP register a non secure device with legacy name*/
		if (domain->type == FASTRPC_NSP) {
			err = fastrpc_device_register(rdev, data, false, true,
					domain->legacy_name);
			if (err)
				return err;
		}
	}

	return 0;
}

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

static int fastrpc_rpmsg_probe(struct rpmsg_device *rpdev)
{
	struct device *rdev = &rpdev->dev;
	struct fastrpc_channel_ctx *data;
	struct fastrpc_domain *domain = NULL;
	int i, err;
	const char * name;

	RPC_INFO("started\n");

	if (is_device_discovery_supported()) {
		err = fastrpc_populate_domain_from_dt(rdev, &domain);
		if (err)
			return err;
	} else {
		err = of_property_read_string(rpdev->dev.parent->of_node,
			"label", &name);

		if (err) {
			RPC_INFO("domain not specified in DT\n");
			return err;
		}

		domain = kzalloc(sizeof(struct fastrpc_domain), GFP_KERNEL);
		if (!domain)
			return -ENOMEM;

		domain->id = -1;
		for (i = 0; i <= CDSP1_DOMAIN_ID; i++) {
			if (!strcmp(legacy_domains[i], name)) {
				strlcpy(domain->name, legacy_domains[i], sizeof(domain->name));
				domain->id = i;
				break;
			}
		}

		if (domain->id < 0) {
			RPC_INFO("invalid domain name %s\n", domain);
			return -ENOMEM;
		}

		domain->legacy = true;

		if (domain->id == CDSP_DOMAIN_ID) {
			domain->type = FASTRPC_NSP;
			domain->instance_id = 0;
		} else if (domain->id == CDSP1_DOMAIN_ID) {
			domain->type = FASTRPC_NSP;
			domain->instance_id = 1;
		}
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
	data->domain_id = domain->id;
	data->max_sess_per_proc = FASTRPC_MAX_SESSIONS_PER_PROCESS;
	data->rpdev = rpdev;
	data->domain = domain;
	data->unsigned_support = false;
	data->cpuinfo_todsp = FASTRPC_CPUINFO_DEFAULT;

	if (domain->type == FASTRPC_NSP) {
		data->unsigned_support = true;
		data->cpuinfo_todsp = FASTRPC_CPUINFO_EARLY_WAKEUP;
	}

	/* Configure device nodes for DSP */
	err = fastrpc_configure_device_nodes(data, rdev);
	if (err)
		goto bail;

	/* Create sysfs directory for channel */
	if (is_device_discovery_supported()) {
		err = fastrpc_sysfs_domain_create(data);
		if (err)
			goto bail;
	}

	/* Update domain status and global ctx */
	domain->status = DSP_STATUS_UP;
	domain->cctx = data;

	fastrpc_update_gdriver(data, 1);
	RPC_INFO("opened rpmsg channel for %s", domain->name);
	return 0;
bail:
	fastrpc_remove_device_nodes(data);
	kfree(data);

	return err;
}

static void fastrpc_rpmsg_remove(struct rpmsg_device *rpdev)
{
	struct fastrpc_channel_ctx *cctx = dev_get_drvdata(&rpdev->dev);
	struct fastrpc_domain *domain = cctx->domain;
	struct fastrpc_user *user;
	unsigned long flags;

	RPC_INFO("started\n");

	if (is_device_discovery_supported())
		fastrpc_sysfs_domain_remove(cctx);

	spin_lock_irqsave(&cctx->lock, flags);
	atomic_set(&cctx->teardown, 1);
	domain->status = DSP_STATUS_DOWN;
	domain->cctx = NULL;
	list_for_each_entry(user, &cctx->users, user) {
		fastrpc_notify_users(user);
	}
	spin_unlock_irqrestore(&cctx->lock, flags);
	fastrpc_remove_device_nodes(cctx);

	list_for_each_entry(user, &cctx->users, user) {
		fastrpc_free_user(user);
	}
	RPC_INFO("closing rpmsg channel for %s", cctx->domain->name);
	cctx->dev = NULL;
	cctx->rpdev = NULL;
	cctx->domain = NULL;

	if (!is_device_discovery_supported())
		kfree(domain);

	fastrpc_update_gdriver(cctx, 0);
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

