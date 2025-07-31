// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023-2025, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/debugfs.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/pm_qos.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/virtio_config.h>
#include <linux/uaccess.h>
#include <linux/of.h>
#include <linux/rpmsg.h>
#include <linux/version.h>
#include "fastrpc_common.h"

/* Virtio ID of FASTRPC : 0xC005 */
#define VIRTIO_ID_HYBRID_FASTRPC			49163
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
/* indicates fastrpc_mmap/fastrpc_munmap is supported */
#define VIRTIO_FASTRPC_F_HYBRID				9

#define VIRTIO_FASTRPC_F_DEVICE_DISCOVERY 11

#define MAX_FASTRPC_BUF_SIZE		(1024*1024*4)
#define DEF_FASTRPC_BUF_SIZE		(128*1024)
#define DEBUGFS_SIZE			3072

#define NUM_DEVICES			2

#define FASTRPC_CPUINFO_DEFAULT		0
#define FASTRPC_CPUINFO_EARLY_WAKEUP	1

#define SESSION_ID_INDEX		30
#define SESSION_ID_MASK			(1 << SESSION_ID_INDEX)
#define PROCESS_ID_MASK			((2^SESSION_ID_INDEX) - 1)

#define FASTRPC_CTX_MAGIC		0xbeeddeed
#define FASTRPC_NOTIF_CTX_RESERVED	0xABCDABCD
#define FASTRPC_RSP_VERSION2		2
#define FASTRPC_CTX_JOB_TYPE_POS (4)
#define FASTRPC_CTX_TABLE_IDX_POS (6)
#define FASTRPC_CTX_JOBID_POS (16)
#define FASTRPC_CTX_TABLE_IDX_MASK \
	((FASTRPC_CTX_MAX - 1) << FASTRPC_CTX_TABLE_IDX_POS)
#define FASTRPC_ASYNC_JOB_MASK   (1)

#define GET_TABLE_IDX_FROM_CTXID(ctxid) \
	((ctxid & FASTRPC_CTX_TABLE_IDX_MASK) >> FASTRPC_CTX_TABLE_IDX_POS)

/*
 * ctxid of every message is OR-ed with fl->pd (0/1/2) before
 * it is sent to DSP. So mask 2 LSBs to retrieve actual context
 */
#define CONTEXT_PD_CHECK (3)
#define GET_CTXID_FROM_RSP_CTX(rsp_ctx) (rsp_ctx & ~CONTEXT_PD_CHECK)

/* Convert the 19.2MHz clock count to micro-seconds */
#define CONVERT_CNT_TO_US(CNT) (CNT * 10ull / 192ull)

/*
 * FE_MAJOR_VER is used for the FE and BE's version match check,
 * and it MUST be equal to BE_MAJOR_VER, otherwise virtual fastrpc
 * cannot work properly. It increases when fundamental protocol is
 * changed between FE and BE.
 */
#define FE_MAJOR_VER 0x1
/* FE_MINOR_VER is used to track patches in this driver. It does not
 * need to be matched with BE_MINOR_VER. And it will return to 0 when
 * FE_MAJOR_VER is increased.
 */
#define FE_MINOR_VER 0x7
#define FE_VERSION (FE_MAJOR_VER << 16 | FE_MINOR_VER)
#define BE_MAJOR_VER(ver) (((ver) >> 16) & 0xffff)

struct hfastrpc_config {
	u32 version;
	u32 domain_num;
	u32 max_buf_size;
	u32 domain_info_offset;
} __packed;

struct fastrpc_domain_config {
	u32 domain_type;
	u32 instance_id;
	u32 logical_id;
	u32 reserved;
};

static struct fastrpc_common g_frpc;

static struct fastrpc_domain_config * g_domain_info = NULL;

static bool g_is_device_discovery_supported = false;

bool is_device_discovery_supported(void)
{
	return g_is_device_discovery_supported;
}

void fastrpc_update_gdriver(struct fastrpc_channel_ctx *cctx, int flag)
{
	if (flag == 1) {
		cctx->gdriver = &g_frpc;
		cctx->dev = cctx->gdriver->dev;
	} else {
		cctx->gdriver = NULL;
		cctx->dev = NULL;
	}
}

void fastrpc_notify_users(struct fastrpc_user *user)
{
	struct fastrpc_invoke_ctx *ctx;
	struct fastrpc_user *fl;

	spin_lock(&user->lock);
	list_for_each_entry(ctx, &user->pending, node) {
		fl = ctx->fl;
		ctx->retval = -EPIPE;
		ctx->is_work_done = true;
		complete(&ctx->work);
	}
	list_for_each_entry(ctx, &user->interrupted, node) {
		ctx->retval = -EPIPE;
		ctx->is_work_done = true;
		complete(&ctx->work);
	}
	spin_unlock(&user->lock);
}

static int recv_single(struct virt_msg_hdr *rsp, unsigned int len)
{
	struct fastrpc_common *gdriver = &g_frpc;
	struct virt_fastrpc_msg *msg;

	if (len != rsp->len) {
		RPC_ERR("msg %u len mismatch,expected %u but %d found\n",
				rsp->cmd, rsp->len, len);
		return -EINVAL;
	}
	spin_lock(&gdriver->msglock);
	msg = gdriver->msgtable[rsp->msgid];
	spin_unlock(&gdriver->msglock);

	if (!msg) {
		RPC_ERR("msg %u already free in table[%u]\n",
				rsp->cmd, rsp->msgid);
		return -EINVAL;
	}
	msg->rxbuf = (void *)rsp;

	complete(&msg->work);

	return 0;
}

static void fastrpc_vq_callback(struct virtqueue *rvq)
{
	struct fastrpc_common *gdriver = &g_frpc;
	struct virt_msg_hdr *rsp;
	unsigned int len, msgs_received = 0;
	int err;
	unsigned long flags;

	spin_lock_irqsave(&gdriver->rvq.vq_lock, flags);
	rsp = virtqueue_get_buf(rvq, &len);
	if (!rsp) {
		spin_unlock_irqrestore(&gdriver->rvq.vq_lock, flags);
		RPC_ERR("incoming signal, but no used buffer\n");
		return;
	}
	spin_unlock_irqrestore(&gdriver->rvq.vq_lock, flags);

	while (rsp) {
		err = recv_single(rsp, len);
		if (err)
			break;

		msgs_received++;

		spin_lock_irqsave(&gdriver->rvq.vq_lock, flags);
		rsp = virtqueue_get_buf(rvq, &len);
		spin_unlock_irqrestore(&gdriver->rvq.vq_lock, flags);
	}
}
/*
 * Add entry for domain in hash-table or update status of existing entry.
 *
 * @param domain  Pointer to the fastrpc domain structure to be added.
 * @param type    Type of the domain.
 * @param label   Label of the domain.
 * @param instance_id  Instance ID of the domain.
 *
 * @return 0 on success, negative error code on failure.
 */
static int fastrpc_add_domain_to_table(struct fastrpc_domain **domain,
				u32 type, const char* label, u32 instance_id)
{
	struct fastrpc_domain *entry = NULL;
	struct mutex *hmut = &g_frpc.hmut;
	u32 phy_id = 0;
	u32 logical_id = 0;
	int i, err = 0;

	phy_id = GENERATE_DSP_PHYSICAL_ID(type, instance_id);

	for (i = 0; i < g_frpc.num_channels; i++) {
		if ((type == g_domain_info[i].domain_type) &&
			(instance_id == g_domain_info[i].instance_id)) {
			logical_id = g_domain_info[i].logical_id;
		}
	}

	if (!logical_id) {
		err = -ENODEV;
		RPC_ERR("Error %d: (phy id %u) not provided by PVM",
			err, phy_id);
		return err;
	}

	/* Validate if there is an exisitng entry for phy_id */
	entry = fastrpc_lookup_domain_in_table(phy_id, true);
	if (!entry) {
		/*
		 * If the domain is not found in the table, create a new
		 * entry and populate all the attributes
		 * phy_id, instance_id, type, logical_id, name
		 */
		entry = kzalloc(sizeof(*entry), GFP_KERNEL);
		if (!entry)
			return -ENOMEM;
		entry->phy_id = phy_id;
		entry->instance_id = instance_id;
		entry->type = type;

		/* Channel name will be generated as <dsp-type-name><physical-id> */
		err = snprintf(entry->name, sizeof(entry->name), "%s%d", label, phy_id);
		if (err < 0 || err >= sizeof(entry->name)) {
			err = -EFAULT;
			RPC_ERR("Error %d: failed to generate name for label %s phy_id %u",
				err, label, phy_id);
			return err;
		}

		mutex_lock(hmut);
		hash_add(g_frpc.fastrpc_domains_table, &entry->node, phy_id);

		if ((type != FASTRPC_HPASS && instance_id == 0) ||
				(type == FASTRPC_NSP && instance_id == 1))  {
			/*
			 * For LPASS, SDSP types only the dsp with instance_id 0 is
			 *                 assigned as legacy adsp, slpi domains
			 * For NSP types, DSP with instance id '0' and '1' are marked as legacy
			 *                to handle legacy cdsp and cdsp1 domains
			*/
			entry->legacy = true;
		}

		entry->id = logical_id;

		mutex_unlock(hmut);
	} else {
		if (entry->status != DSP_STATUS_DOWN) {
			/*
			* If entry for channel is already present in hash-table, it means
			* the channel has gone through ssr. In that case, its status has to be
			* DOWN. If not, system is in bad-state.
			*/
			err = EINVAL;
			RPC_ERR("Error %d: %s (phy id %u) already in table with bad status %d",
					err, entry->name, entry->phy_id, entry->status);
			return err;
		}
	}
	*domain = entry;
	return 0;
}

/*
 * fastrpc_lookup_domain_in_table() -
 * Looks up a domain in the in the fastrpc domains hash-table using either
 * physical id or logical domain id based on the flag.
 *
 * @param key          : physical id / logical domain id to lookup in table
 * @param use_phy_id   : Flag to indicate whether to lookup using phy id
 *                       or logical id.
 *
 * @return Pointer to the matching domain structure, or NULL if not found.
 */
struct fastrpc_domain *fastrpc_lookup_domain_in_table(
	u32 key, bool use_phy_id)
{
	struct fastrpc_domain *domain = NULL, *match = NULL;
	struct mutex *hmut = &g_frpc.hmut;
	int i = 0;

	mutex_lock(hmut);
	hash_for_each(g_frpc.fastrpc_domains_table, i, domain, node) {
		/*
		 * Based on flag, lookup domain based on 32-bit physical id,
		 * logical id
		 */
		if (use_phy_id) {
			if (domain->phy_id == key) {
				match = domain;
				break;
			}
		} else {
			if (domain->id == key) {
				match = domain;
				break;
			}
		}
	}
	mutex_unlock(hmut);
	return match;
}

/*
 * Deletes all entries from the fastrpc domains hash-table.
 */
static void fastrpc_delete_domains_table(void)
{
	struct fastrpc_domain *domain = NULL;
	struct mutex *hmut = &g_frpc.hmut;
	int i = 0;

	mutex_lock(hmut);
	hash_for_each(g_frpc.fastrpc_domains_table, i, domain, node) {
		hash_del(&domain->node);
		kfree(domain);
	}
	mutex_unlock(hmut);
}


/*
 * Convert legacy ID to logical domain ID
 *
 * This function takes a legacy ID as input and returns the corresponding
 * logical ID.
 *
 * @param id: Legacy ID to convert
 * @param logical_id :   Pointer to logical id
 *
 * @return 0 on success
 *         EINAL if logical id is not found.
 */
int fastrpc_convert_legacy_id_to_logical_id(u32 legacy_id,
					u32 *logical_id)
{
	struct fastrpc_domain *domain = NULL;
	struct mutex *hmut = &g_frpc.hmut;
	int i = 0, err = -EINVAL;

	mutex_lock(hmut);
	hash_for_each(g_frpc.fastrpc_domains_table, i, domain, node) {
		if (domain->legacy_id == legacy_id) {
			*logical_id = domain->id;
			err = 0;
			break;
		}
	}
	mutex_unlock(hmut);
	return err;
}

/*
 * Populate fastrpc_domain from device tree node.
 *
 * @param rdev   Device structure to extract info from.
 * @param domain Pointer to fastrpc_domain pointer to be populated.
 *
 * @return 0 on success, negative error code on failure.
 */
int fastrpc_populate_domain_from_dt(struct device *rdev,
				struct fastrpc_domain **domain)
{
	const char *label = NULL;
	u32 type = 0, instance_id = U32_MAX;
	int err = 0;
	bool valid_label = false;
	struct device_node *fnode = NULL;

	fnode = of_get_child_by_name(rdev->parent->of_node, "qcom,fastrpc");
	if (!fnode) {
		pr_err("Child node not found\n");
		return -ENODEV;
	}

	/* Retrieve the label of DSP from DT */
	err = of_property_read_string(fnode, "label", &label);
	if (err < 0) {
		dev_err(rdev, "Error %d: %s: FastRPC DSP label not specified in DT\n",
			err, __func__);
		return err;
	}
	/* Validate the label retrieved from DT */
	for (int i = 1; i < FASTRPC_MAX_DSP_TYPE; i++) {
		if (strcmp(label, fastrpc_dsp_type_labels[i]) == 0) {
			valid_label = true;
			break;
		}
	}

	/*
	 * Fail the device probe if it has invalid label. This driver assumes that
	 * the DTSI file is always updated to contain the new DT properties.
	 */
	if (!valid_label) {
		err = -EINVAL;
		dev_err(rdev, "Error %d: %s: DSP label %s specified in DT is invalid\n",
				err, __func__, label);
		return err;
	}

	/*
	 * Retrieve and validate the type of DSP from DT
	 *
	 * Fail the call if either dsp-type is not present in DT,
	 * or invalid DSP type is specified in DT
	 */
	err = of_property_read_u32(fnode, "dsp-type", &type);
	if (err < 0) {
		dev_err(rdev, "Error %d: %s: dsp-type not specified for %s",
				err, __func__, label);
		return -EINVAL;
	} else if (type >= FASTRPC_MAX_DSP_TYPE || type == 0) {
		err = -EINVAL;
		dev_err(rdev, "Error %d: %s: DSP type %u specified in DT is invalid\n",
				err, __func__, type);
		return err;
	}


	/* Retrieve the instance id of the DSP, fail the call if not specified */
	err = of_property_read_u32(fnode, "instance-id", &instance_id);
	if (err < 0) {
		dev_info(rdev, "Error %d: %s: instance-id not specified for %s\n",
				err, __func__, label);
		return -EINVAL;
	}

	/* Add the info to the domain table */
	err = fastrpc_add_domain_to_table(domain, type, label, instance_id);
	if (err < 0) {
		dev_err(rdev, "Error %d: %s: failed to add domain %s to table (type %u, instance id %u)",
				err, __func__, label, type, instance_id);
		return err;
	}
	return err;
}


static void virt_init_vq(struct virt_fastrpc_vq *fastrpc_vq,
				struct virtqueue *vq)
{
	spin_lock_init(&fastrpc_vq->vq_lock);
	fastrpc_vq->vq = vq;
}

static int init_vqs(struct fastrpc_common *gdriver)
{
	struct virtqueue *vqs[2];
	int err, i;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
	struct virtqueue_info vqs_info[] = {
		{"tx", NULL },
		{"rx", fastrpc_vq_callback },
	};

	err = virtio_find_vqs(gdriver->vdev, 2, vqs, vqs_info, NULL);
#else
	static const char * const names[] = { "tx", "rx" };
	vq_callback_t *cbs[] = { NULL, fastrpc_vq_callback };

	err = virtio_find_vqs(gdriver->vdev, 2, vqs, cbs, names, NULL);
#endif
	if (err)
		return err;

	virt_init_vq(&gdriver->svq, vqs[0]);
	virt_init_vq(&gdriver->rvq, vqs[1]);


	/* we expect symmetric tx/rx vrings */
	if (virtqueue_get_vring_size(gdriver->rvq.vq) !=
			virtqueue_get_vring_size(gdriver->svq.vq)) {
		RPC_ERR("tx/rx vring size does not match\n");
			err = -EINVAL;
		goto vqs_del;
	}

	gdriver->num_bufs = virtqueue_get_vring_size(gdriver->rvq.vq);
	gdriver->rbufs = kcalloc(gdriver->num_bufs,
				sizeof(void *), GFP_KERNEL);
	if (!gdriver->rbufs) {
		err = -ENOMEM;
		goto vqs_del;
	}
	gdriver->sbufs = kcalloc(gdriver->num_bufs,
				sizeof(void *), GFP_KERNEL);
	if (!gdriver->sbufs) {
		err = -ENOMEM;
		kfree(gdriver->rbufs);
		goto vqs_del;
	}

	gdriver->order = get_order(gdriver->buf_size);

	for (i = 0; i < gdriver->num_bufs; i++) {
		gdriver->rbufs[i] = (void *)__get_free_pages(GFP_KERNEL, gdriver->order);
		if (!gdriver->rbufs[i]) {
			err = -ENOMEM;
			goto rbuf_del;
		}
	}

	for (i = 0; i < gdriver->num_bufs; i++) {
		gdriver->sbufs[i] = (void *)__get_free_pages(GFP_KERNEL, gdriver->order);
		if (!gdriver->sbufs[i]) {
			err = -ENOMEM;
			goto sbuf_del;
		}
	}
	return 0;

sbuf_del:
	for (i = 0; i < gdriver->num_bufs; i++) {
		if (gdriver->sbufs[i])
			free_pages((unsigned long)gdriver->sbufs[i], gdriver->order);
	}

rbuf_del:
	for (i = 0; i < gdriver->num_bufs; i++) {
		if (gdriver->rbufs[i])
			free_pages((unsigned long)gdriver->rbufs[i], gdriver->order);
	}
	kfree(gdriver->sbufs);
	kfree(gdriver->rbufs);
vqs_del:
	gdriver->vdev->config->del_vqs(gdriver->vdev);
	return err;
}

static int hfastrpc_probe(struct virtio_device *vdev)
{
	struct hfastrpc_config config;
	int err, i;
	struct fastrpc_common *gdriver = &g_frpc;
#ifdef CONFIG_DEBUG_FS
        struct dentry *debugfs_root = NULL;
#endif

	if (!virtio_has_feature(vdev, VIRTIO_F_VERSION_1))
		return -ENODEV;

	if (!virtio_has_feature(vdev, VIRTIO_FASTRPC_F_HYBRID)) {
		RPC_ERR("hybrid fastrpc can't work with legacy virtio fastrpc\n");
		return -ENODEV;
	}

	memset(&config, 0x0, sizeof(config));
	if (virtio_has_feature(vdev, VIRTIO_FASTRPC_F_VERSION)) {
		virtio_cread(vdev, struct hfastrpc_config, version, &config.version);
		if (BE_MAJOR_VER(config.version) != FE_MAJOR_VER) {
			RPC_ERR("vdev major version does not match 0x%x:0x%x\n",
					FE_VERSION, config.version);
			return -ENODEV;
		}
	}
	RPC_INFO("hybrid fastrpc version 0x%x:0x%x\n",
			FE_VERSION, config.version);

	memset(gdriver, 0, sizeof(*gdriver));
	spin_lock_init(&gdriver->msglock);
	spin_lock_init(&gdriver->glock);
	mutex_init(&gdriver->gmut);

	vdev->priv = gdriver;
	gdriver->vdev = vdev;
	gdriver->dev = vdev->dev.parent;

	if (virtio_has_feature(vdev, VIRTIO_FASTRPC_F_VQUEUE_SETTING)) {
		virtio_cread(vdev, struct hfastrpc_config, max_buf_size,
				&config.max_buf_size);
		if (config.max_buf_size > MAX_FASTRPC_BUF_SIZE) {
			RPC_ERR("buffer size 0x%x is exceed to maximum limit 0x%x\n",
					config.max_buf_size, MAX_FASTRPC_BUF_SIZE);
			return -EINVAL;
		}

		gdriver->buf_size = config.max_buf_size;
		RPC_INFO("set buf_size to 0x%x\n", gdriver->buf_size);
	} else {
		gdriver->buf_size = DEF_FASTRPC_BUF_SIZE;
		RPC_INFO("set buf_size to default value 0x%x\n", gdriver->buf_size);
	}

	if (virtio_has_feature(vdev, VIRTIO_FASTRPC_F_DOMAIN_NUM)) {
		virtio_cread(vdev, struct hfastrpc_config, domain_num,
				&config.domain_num);
		RPC_INFO("get domain_num %d from config space\n",
				config.domain_num);
		if (config.domain_num < FASTRPC_DEV_MAX)
			gdriver->num_channels = config.domain_num;
		else
			gdriver->num_channels = FASTRPC_DEV_MAX;

		if (virtio_has_feature(vdev, VIRTIO_FASTRPC_F_DEVICE_DISCOVERY)) {
			RPC_INFO("Device discovery is supported\n");
			virtio_cread(vdev, struct hfastrpc_config, domain_info_offset,
					&config.domain_info_offset);

			g_domain_info = kzalloc(sizeof(struct fastrpc_domain_config) *
									gdriver->num_channels, GFP_KERNEL);

			if (!g_domain_info)
				return -ENOMEM;

			virtio_cread_bytes(vdev, config.domain_info_offset, &g_domain_info[0],
							   sizeof(struct fastrpc_domain_config) * gdriver->num_channels);
			g_is_device_discovery_supported = true;

			mutex_init(&g_frpc.hmut);
			hash_init(g_frpc.fastrpc_domains_table);
			fastrpc_sysfs_register_kset();
		} else {
			RPC_INFO("Device discovery is not supported\n");
			g_is_device_discovery_supported = false;
		}
	} else {
		RPC_INFO("set domain_num to default value %d\n", FASTRPC_DEV_MAX);
		gdriver->num_channels = FASTRPC_DEV_MAX;
	}

	err = init_vqs(gdriver);
	if (err) {
		RPC_ERR("failed to initialized virtqueue\n");
		return err;
	}

	err = fastrpc_transport_init();
	if (err) {
		RPC_ERR("fastrpc: failed to register rpmsg driver\n");
		goto bail;
	}

#ifdef CONFIG_DEBUG_FS
	debugfs_root = debugfs_create_dir("fastrpc", NULL);
	if (IS_ERR_OR_NULL(debugfs_root)) {
		RPC_WARN("failed to create debugfs root dir\n");
		debugfs_root = NULL;
	}
	gdriver->debugfs_root = debugfs_root;
#endif

	virtio_device_ready(vdev);

	/* set up the receive buffers */
	for (i = 0; i < gdriver->num_bufs; i++) {
		struct scatterlist sg;
		void *cpu_addr = gdriver->rbufs[i];

		sg_init_one(&sg, cpu_addr, gdriver->buf_size);
		err = virtqueue_add_inbuf(gdriver->rvq.vq, &sg, 1, cpu_addr,
				GFP_KERNEL);
		WARN_ON(err); /* sanity check; this can't really happen */
	}

	/* suppress "tx-complete" interrupts */
	virtqueue_disable_cb(gdriver->svq.vq);

	virtqueue_enable_cb(gdriver->rvq.vq);
	virtqueue_kick(gdriver->rvq.vq);

	RPC_INFO("Registered hybrid fastrpc device\n");
	return 0;
bail:
	vdev->config->del_vqs(vdev);
	return err;
}

static void hfastrpc_remove(struct virtio_device *vdev)
{
	struct fastrpc_common *gdriver = &g_frpc;
	int i;
#ifdef CONFIG_DEBUG_FS
	debugfs_remove_recursive(gdriver->debugfs_root);
#endif

	if (g_is_device_discovery_supported == true) {
		fastrpc_sysfs_deregister_kset();
		fastrpc_delete_domains_table();
		mutex_destroy(&g_frpc.hmut);
		kfree(g_domain_info);
	}

	mutex_destroy(&gdriver->gmut);
	fastrpc_transport_deinit();
	vdev->config->reset(vdev);
	vdev->config->del_vqs(vdev);

	for (i = 0; i < gdriver->num_bufs; i++)
		free_pages((unsigned long)gdriver->rbufs[i], gdriver->order);
	for (i = 0; i < gdriver->num_bufs; i++)
		free_pages((unsigned long)gdriver->sbufs[i], gdriver->order);

	kfree(gdriver->sbufs);
	kfree(gdriver->rbufs);
}

static struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_HYBRID_FASTRPC, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static unsigned int features[] = {
	VIRTIO_FASTRPC_F_VERSION,
	VIRTIO_FASTRPC_F_DOMAIN_NUM,
	VIRTIO_FASTRPC_F_VQUEUE_SETTING,
	VIRTIO_FASTRPC_F_HYBRID,
	VIRTIO_FASTRPC_F_DEVICE_DISCOVERY,
};

static struct virtio_driver hybrid_fastrpc_driver = {
	.feature_table		= features,
	.feature_table_size	= ARRAY_SIZE(features),
	.driver.name		= KBUILD_MODNAME,
	.driver.owner		= THIS_MODULE,
	.id_table		= id_table,
	.probe			= hfastrpc_probe,
	.remove			= hfastrpc_remove,
};

static int __init hybrid_fastrpc_init(void)
{
	return register_virtio_driver(&hybrid_fastrpc_driver);
}

static void __exit hybrid_fastrpc_exit(void)
{
	unregister_virtio_driver(&hybrid_fastrpc_driver);
}
module_init(hybrid_fastrpc_init);
module_exit(hybrid_fastrpc_exit);

MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("Hybrid FastRPC Driver");
MODULE_IMPORT_NS(DMA_BUF);
MODULE_LICENSE("GPL v2");
