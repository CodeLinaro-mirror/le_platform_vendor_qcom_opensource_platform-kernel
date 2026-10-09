// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * compressched_fe_cdev.c - Character device and ioctl interface for the
 * compressched_fe kernel driver.
 *
 * Creates /dev/compressched_fe and dispatches the five ioctl commands
 * (REGISTER, ACQUIRE, RELEASE, UNREGISTER, ACQUIRE_JOBNAME) to the
 * corresponding virtio client functions in virtio_compressched_client.c.
 *
 * ioctl definitions live in compressched_fe_ioctl.h which is shared
 * with the userspace library - no manual sync required.
 *
 * MODULE_LICENSE / MODULE_DESCRIPTION / MODULE_AUTHOR are declared in
 * virtio_compressched_base.c (the module entry point).
 */

#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/compat.h>
/*
 * virtio_compressched_base.h provides:
 *   - LOG_COMPRESSCHEDFE / LEVEL_ERR / LEVEL_INFO / LEVEL_DEBUG macros
 *   - virtio_compressched_client.h (transitively - kernel types + functions)
 */
#include "virtio_compressched_base.h"
/*
 * Shared UAPI header - ioctl magic, command numbers, and argument structs.
 * Identical to what the userspace library includes.
 */
#include "uapi/compressched_fe_ioctl.h"
#include "uapi/compressched_fe_dp_ioctl.h"

/* ------------------------------------------------------------------ */
/* Device name                                                         */
/* ------------------------------------------------------------------ */

#define COMPRESSCHED_FE_DEV_NAME  "compressched_fe"

/* ------------------------------------------------------------------ */
/* file_operations                                                     */
/* ------------------------------------------------------------------ */

static int compressched_fe_open(struct inode *inode, struct file *file)
{
	LOG_COMPRESSCHEDFE(LEVEL_DEBUG, "device opened by pid=%d\n", current->pid);
	return 0;
}

static int compressched_fe_release(struct inode *inode, struct file *file)
{
	unsigned int pid = (unsigned int)current->pid;
	compressched_handle handles[MAX_CLIENT];
	int count = 0;
	int i;

	LOG_COMPRESSCHEDFE(LEVEL_DEBUG, "device released by pid=%d\n", current->pid);

	if (!g_vdevcompressched)
		return 0;

	/*
	 * Collect all handles registered by this pid under the spinlock.
	 * We must not call compressched_unregister() under the spinlock because
	 * it calls wait_for_completion() which may sleep.
	 *
	 * For both the base (upid[0]=pid, target_id=tid) and _ex
	 * (upid[0]=pid, target_id=tid) registration paths, pid is always
	 * stored in upid[0].  A slot is in use when its handle != 0.
	 */
	spin_lock(&g_vdevcompressched->vq_clientlock);
	for (i = 0; i < MAX_CLIENT; i++) {
		if (g_vdevcompressched->client_list[i].handle != 0 &&
		    g_vdevcompressched->client_list[i].upid[0] == pid)
			handles[count++] = g_vdevcompressched->client_list[i].handle;
	}
	spin_unlock(&g_vdevcompressched->vq_clientlock);

	for (i = 0; i < count; i++) {
		LOG_COMPRESSCHEDFE(LEVEL_INFO,
			"fe_release: auto-unregistering handle=0x%x for pid=%d\n",
			handles[i], current->pid);
		compressched_unregister(handles[i]);
	}

	return 0;
}

static long compressched_fe_ioctl(struct file *file,
				   unsigned int cmd,
				   unsigned long arg)
{
	int ret = 0;

	LOG_COMPRESSCHEDFE(LEVEL_DEBUG, "ioctl cmd=0x%x pid=%d\n", cmd, current->pid);

	switch (cmd) {

	/* ---- REGISTER ------------------------------------------------ */
	case COMPRESSCHED_IOCTL_REGISTER_EX_LIB:
	{
		/*
		 * Use plain u32 for the local handle variable.
		 * compressched_handle is typedef'd as unsigned int in
		 * virtio_compressched_client.h; cast on the call boundary.
		 */
		struct compressched_ioctl_register_args args = {};
		u32 handle = 0;

		/*
		 * pid and tid are supplied by userspace in the input struct.
		 * Read them via copy_from_user before forwarding to the
		 * PVM backend.
		 */
		if (copy_from_user(&args, (void __user *)arg, sizeof(args))) {
			LOG_COMPRESSCHEDFE(LEVEL_ERR, "copy_from_user failed (REGISTER)\n");
			return -EFAULT;
		}

		ret = compressched_register_ex((compressched_handle *)&handle,
					    (unsigned int)args.pid,
					    (unsigned int)args.tid);
		if (ret) {
			LOG_COMPRESSCHEDFE(LEVEL_ERR, "register failed ret=%d\n", ret);
			return ret;
		}

		args.handle = handle;

		if (copy_to_user((void __user *)arg, &args, sizeof(args))) {
			LOG_COMPRESSCHEDFE(LEVEL_ERR, "copy_to_user failed (REGISTER)\n");
			return -EFAULT;
		}

		LOG_COMPRESSCHEDFE(LEVEL_INFO,
				   "register successful handle=0x%x pid=%u tid=%u\n",
				   args.handle, args.pid, args.tid);
		break;
	}
#ifdef REMOVE
	/* ---- ACQUIRE ------------------------------------------------- */
	case COMPRESSCHED_IOCTL_ACQUIRE:
	{
		struct compressched_ioctl_acquire_args args = {};
		/*
		 * compressched_acquire_tx is used for the NSP sharing
		 * acquire path (handle + job_name).
		 */
		compressched_acquire_tx acquire_cmd = {};
		compressched_acquire_rsp_v2 response = {};

		if (copy_from_user(&args, (void __user *)arg, sizeof(args))) {
			LOG_COMPRESSCHEDFE(LEVEL_ERR, "copy_from_user failed (ACQUIRE)\n");
			return -EFAULT;
		}

		acquire_cmd.handle   = (compressched_handle)args.handle;
		acquire_cmd.job_name = NULL;

		ret = compressched_acquire(acquire_cmd.handle,
					   acquire_cmd.job_name, &response);
		if (ret) {
			LOG_COMPRESSCHEDFE(LEVEL_ERR,
					   "acquire failed handle=0x%x ret=%d\n",
					   args.handle, ret);
			return ret;
		}

		args.token        = (__u32)response.token;
		args.rsp_reserved = (__u32)response.reserved;

		if (copy_to_user((void __user *)arg, &args, sizeof(args))) {
			LOG_COMPRESSCHEDFE(LEVEL_ERR, "copy_to_user failed (ACQUIRE)\n");
			return -EFAULT;
		}

		LOG_COMPRESSCHEDFE(LEVEL_INFO,
				   "acquire successful handle=0x%x token=0x%x\n",
				   args.handle, args.token);
		break;
	}
#endif 
	/* ---- RELEASE ------------------------------------------------- */
	case COMPRESSCHED_IOCTL_RELEASE_LIB:
	{
		struct compressched_ioctl_release_args args;

		if (copy_from_user(&args, (void __user *)arg, sizeof(args))) {
			LOG_COMPRESSCHEDFE(LEVEL_ERR, "copy_from_user failed (RELEASE)\n");
			return -EFAULT;
		}

		ret = compressched_release((compressched_handle)args.handle,
					   (compressched_token)args.token);
		if (ret) {
			LOG_COMPRESSCHEDFE(LEVEL_ERR,
					   "release failed handle=0x%x ret=%d\n",
					   args.handle, ret);
			return ret;
		}

		/* No output fields - copy_to_user not needed for RELEASE */
		LOG_COMPRESSCHEDFE(LEVEL_INFO,
				   "release successful handle=0x%x token=0x%x\n",
				   args.handle, args.token);
		break;
	}

	/* ---- UNREGISTER ---------------------------------------------- */
	case COMPRESSCHED_IOCTL_UNREGISTER_LIB:
	{
		struct compressched_ioctl_unregister_args args;

		if (copy_from_user(&args, (void __user *)arg, sizeof(args))) {
			LOG_COMPRESSCHEDFE(LEVEL_ERR, "copy_from_user failed (UNREGISTER)\n");
			return -EFAULT;
		}

		ret = compressched_unregister((compressched_handle)args.handle);
		if (ret) {
			LOG_COMPRESSCHEDFE(LEVEL_ERR,
					   "unregister failed handle=0x%x ret=%d\n",
					   args.handle, ret);
			return ret;
		}

		/* No output fields - copy_to_user not needed for UNREGISTER */
		LOG_COMPRESSCHEDFE(LEVEL_INFO,
				   "unregister successful handle=0x%x\n",
				   args.handle);
		break;
	}

	/* ---- ACQUIRE_JOBNAME ----------------------------------------- */
	case COMPRESSCHED_IOCTL_ACQUIRE_JOBNAME:
		/*
		 * Job-name acquire requires PVM backend virtio job_name support.
		 * This is an expected/planned code path - use DEBUG not ERR.
		 * Returns -EOPNOTSUPP; the userspace library maps this to
		 * -ENOTSUP for the caller.
		 */
		LOG_COMPRESSCHEDFE(LEVEL_DEBUG, "ACQUIRE_JOBNAME not yet supported\n");
		return -EOPNOTSUPP;

	/* ---- ACQUIRE_LEGACY ------------------------------------------ */
	case COMPRESSCHED_IOCTL_ACQUIRE_LIB:
	{
		struct compressched_ioctl_acquire_args args = {};
		/*
		 * compressched_acquire_legacy_tx and compressched_acquire_rsp_v2 are
		 * typedef'd anonymous structs in virtio_compressched_client.h.
		 * They cannot be forward-declared with 'struct' prefix.
		 */
		compressched_acquire_legacy_tx acquire_cmd = {};
		compressched_acquire_rsp_v2 response = {};

		if (copy_from_user(&args, (void __user *)arg, sizeof(args))) {
			LOG_COMPRESSCHEDFE(LEVEL_ERR, "copy_from_user failed (ACQUIRE_EX)\n");
			return -EFAULT;
		}

		/*
		 * Map uapi struct compressched_ioctl_acquire_cmd
		 * to kernel compressched_acquire_legacy_tx.
		 * Enum fields are stored as __s32 in the uapi struct; cast them
		 * back to the kernel enum types here.
		 */
		acquire_cmd.handle = (compressched_handle)args.handle;
		acquire_cmd.res_details.req_type =
			(compressched_resource_request_type)args.cmd.res_details.req_type;
		acquire_cmd.res_details.sub_res_id =
			(compressched_sub_res_id)args.cmd.res_details.sub_res_id;
		memcpy(acquire_cmd.res_details.requested_domain_id,
		       args.cmd.res_details.requested_domain_id,
		       sizeof(acquire_cmd.res_details.requested_domain_id));
		acquire_cmd.priority    = (compressched_queue_priority)args.cmd.priority;
		acquire_cmd.duration_us = args.cmd.duration_us;
		acquire_cmd.timeout_us  = args.cmd.timeout_us;
		acquire_cmd.reserved    = args.cmd.reserved;

		ret = compressched_acquire_legacy((compressched_handle)args.handle,
					      &acquire_cmd, &response);
		if (ret) {
			LOG_COMPRESSCHEDFE(LEVEL_ERR,
					   "acquire_ex failed handle=0x%x ret=%d\n",
					   args.handle, ret);
			return ret;
		}

		args.token        = (__u32)response.token;
		args.rsp_reserved = (__u32)response.reserved;

		if (copy_to_user((void __user *)arg, &args, sizeof(args))) {
			LOG_COMPRESSCHEDFE(LEVEL_ERR, "copy_to_user failed (ACQUIRE_EX)\n");
			return -EFAULT;
		}

		LOG_COMPRESSCHEDFE(LEVEL_INFO,
				   "acquire_ex successful handle=0x%x token=0x%x\n",
				   args.handle, args.token);
		break;
	}

	/* ---- QUERY --------------------------------------------------- */
	case COMPRESSCHED_IOCTL_QUERY_LIB:
	{
		struct compressched_ioctl_query_args args = {};
		compressched_platform_query query = {};
		int i, j;

		if (copy_from_user(&args, (void __user *)arg, sizeof(args))) {
			LOG_COMPRESSCHEDFE(LEVEL_ERR, "copy_from_user failed (QUERY)\n");
			return -EFAULT;
		}

		ret = compressched_query((compressched_handle)args.handle, &query);
		if (ret) {
			LOG_COMPRESSCHEDFE(LEVEL_ERR,
					   "query failed handle=0x%x ret=%d\n",
					   args.handle, ret);
			return ret;
		}

		/*
		 * Map kernel compressched_platform_query to the UAPI
		 * compressched_ioctl_platform_query.  The UAPI struct has an
		 * explicit pad0 after num_resources that the kernel struct
		 * lacks, so field-by-field assignment is required.
		 */
		args.query.num_resources = (__u32)query.num_resources;
		for (i = 0; i < COMPRESSCHED_FE_MAX_RESOURCES; i++) {
			args.query.resource_query[i].domain_id  =
				(__s32)query.resource_query[i].domain_id;
			args.query.resource_query[i].num_queues =
				(__u32)query.resource_query[i].num_queues;
			for (j = 0; j < COMPRESSCHED_FE_MAX_QUEUE_PRIORITIES; j++) {
				args.query.resource_query[i].queue_query[j].priority =
					(__s32)query.resource_query[i].queue_query[j].priority;
				args.query.resource_query[i].queue_query[j].queue_type =
					(__s32)query.resource_query[i].queue_query[j].queue_type;
				args.query.resource_query[i].queue_query[j].request_query.handle =
					(__u32)query.resource_query[i].queue_query[j].request_query.handle;
				args.query.resource_query[i].queue_query[j].request_query.token =
					(__u32)query.resource_query[i].queue_query[j].request_query.token;
				args.query.resource_query[i].queue_query[j].request_query.reserved =
					(__u64)query.resource_query[i].queue_query[j].request_query.reserved;
			}
		}

		if (copy_to_user((void __user *)arg, &args, sizeof(args))) {
			LOG_COMPRESSCHEDFE(LEVEL_ERR, "copy_to_user failed (QUERY)\n");
			return -EFAULT;
		}

		LOG_COMPRESSCHEDFE(LEVEL_INFO,
				   "query successful handle=0x%x num_resources=%u\n",
				   args.handle, args.query.num_resources);
		break;
	}

	/* ---- TRACING_START ------------------------------------------- */
	case COMPRESSCHED_IOCTL_TRACING_START_LIB:
	{
		struct compressched_ioctl_tracing_start_args args;

		if (copy_from_user(&args, (void __user *)arg, sizeof(args))) {
			LOG_COMPRESSCHEDFE(LEVEL_ERR,
				"copy_from_user failed (TRACING_START)\n");
			return -EFAULT;
		}

		ret = compressched_tracing_start((compressched_handle)args.handle,
						 (unsigned int)args.num_logs);
		if (ret) {
			LOG_COMPRESSCHEDFE(LEVEL_ERR,
				"tracing_start failed handle=0x%x ret=%d\n",
				args.handle, ret);
			return ret;
		}

		LOG_COMPRESSCHEDFE(LEVEL_INFO,
			"tracing_start successful handle=0x%x num_logs=%u\n",
			args.handle, args.num_logs);
		break;
	}

	/* ---- TRACING_STOP -------------------------------------------- */
	case COMPRESSCHED_IOCTL_TRACING_STOP_LIB:
	{
		struct compressched_ioctl_tracing_stop_args args;

		if (copy_from_user(&args, (void __user *)arg, sizeof(args))) {
			LOG_COMPRESSCHEDFE(LEVEL_ERR,
				"copy_from_user failed (TRACING_STOP)\n");
			return -EFAULT;
		}

		ret = compressched_tracing_stop((compressched_handle)args.handle);
		if (ret) {
			LOG_COMPRESSCHEDFE(LEVEL_ERR,
				"tracing_stop failed handle=0x%x ret=%d\n",
				args.handle, ret);
			return ret;
		}

		LOG_COMPRESSCHEDFE(LEVEL_INFO,
			"tracing_stop successful handle=0x%x\n", args.handle);
		break;
	}

	/* ---- DEBUG_CMD ----------------------------------------------- */
	case COMPRESSCHED_IOCTL_DEBUG_CMD_LIB:
	{
		struct compressched_ioctl_debug_cmd_args args;
		compressched_debug_cmd karg = {};

		if (copy_from_user(&args, (void __user *)arg, sizeof(args))) {
			LOG_COMPRESSCHEDFE(LEVEL_ERR,
				"copy_from_user failed (DEBUG_CMD)\n");
			return -EFAULT;
		}

		karg.handle                 = (compressched_handle)args.handle;
		karg.cmd_type               = args.cmd_type;
		karg.enable_latency_measure = args.enable_latency_measure;

		ret = compressched_debug_cmd_fn((compressched_handle)args.handle,
						&karg);
		if (ret) {
			LOG_COMPRESSCHEDFE(LEVEL_ERR,
				"debug_cmd failed handle=0x%x ret=%d\n",
				args.handle, ret);
			return ret;
		}

		LOG_COMPRESSCHEDFE(LEVEL_INFO,
			"debug_cmd successful handle=0x%x cmd_type=%u\n",
			args.handle, args.cmd_type);
		break;
	}

	/* ---- SUBSCRIBE_FAULTS ---------------------------------------- */
	case COMPRESSCHED_IOCTL_SUBSCRIBE_FAULTS_LIB:
	{
		struct compressched_ioctl_subscribe_faults_args args;
		compressched_subscribe_faults karg = {};
		int i;

		if (copy_from_user(&args, (void __user *)arg, sizeof(args))) {
			LOG_COMPRESSCHEDFE(LEVEL_ERR,
				"copy_from_user failed (SUBSCRIBE_FAULTS)\n");
			return -EFAULT;
		}

		karg.handle = (compressched_handle)args.handle;
		for (i = 0; i < COMPRESSCHED_FE_MAX_FAULT_DOMAINS; i++) {
			karg.mask[i].domain_id  = (int)args.domain_id[i];
			karg.mask[i].fault_mask = args.fault_mask[i];
		}

		ret = compressched_subscribe_faults_fn(
				(compressched_handle)args.handle, &karg);
		if (ret) {
			LOG_COMPRESSCHEDFE(LEVEL_ERR,
				"subscribe_faults failed handle=0x%x ret=%d\n",
				args.handle, ret);
			return ret;
		}

		LOG_COMPRESSCHEDFE(LEVEL_INFO,
			"subscribe_faults successful handle=0x%x\n", args.handle);
		break;
	}

	/* ---- UNSUBSCRIBE_FAULTS -------------------------------------- */
	case COMPRESSCHED_IOCTL_UNSUBSCRIBE_FAULTS_LIB:
	{
		struct compressched_ioctl_unsubscribe_faults_args args;

		if (copy_from_user(&args, (void __user *)arg, sizeof(args))) {
			LOG_COMPRESSCHEDFE(LEVEL_ERR,
				"copy_from_user failed (UNSUBSCRIBE_FAULTS)\n");
			return -EFAULT;
		}

		ret = compressched_unsubscribe_faults_fn(
				(compressched_handle)args.handle);
		if (ret) {
			LOG_COMPRESSCHEDFE(LEVEL_ERR,
				"unsubscribe_faults failed handle=0x%x ret=%d\n",
				args.handle, ret);
			return ret;
		}

		LOG_COMPRESSCHEDFE(LEVEL_INFO,
			"unsubscribe_faults successful handle=0x%x\n", args.handle);
		break;
	}

	/* ---- WAIT_FAULT ---------------------------------------------- */
	case COMPRESSCHED_IOCTL_WAIT_FAULT:
	{
		struct compressched_ioctl_wait_fault_args args;
		compressched_fault_notify_rx fault;
		unsigned long fl;

		if (copy_from_user(&args, (void __user *)arg, sizeof(args))) {
			LOG_COMPRESSCHEDFE(LEVEL_ERR,
				"copy_from_user failed (WAIT_FAULT)\n");
			return -EFAULT;
		}

		if (g_vdevcompressched == NULL)
			return -ENODEV;

		/*
		 * Block until the PVM sends a COMPRESSCHED_FAULT_NOTIFY_LIB message.
		 * virtio_compressched_recv_cb() calls complete(&fault_work) when
		 * such a message arrives (msg_id == MAX_CLIENT).
		 */
		ret = wait_for_completion_interruptible(
				&g_vdevcompressched->fault_work);
		if (ret) {
			/* Interrupted by a signal */
			return -EINTR;
		}

		/* Re-initialise so the next WAIT_FAULT call blocks again */
		reinit_completion(&g_vdevcompressched->fault_work);

		spin_lock_irqsave(&g_vdevcompressched->fault_lock, fl);
		fault = g_vdevcompressched->fault_pending;
		spin_unlock_irqrestore(&g_vdevcompressched->fault_lock, fl);

		args.fault_type = fault.fault_type;
		args.domain_id  = fault.domain_id;
		args.sub_res_id = fault.sub_res_id;
		args.fault_queue = fault.fault_queue;

		if (copy_to_user((void __user *)arg, &args, sizeof(args))) {
			LOG_COMPRESSCHEDFE(LEVEL_ERR,
				"copy_to_user failed (WAIT_FAULT)\n");
			return -EFAULT;
		}

		LOG_COMPRESSCHEDFE(LEVEL_INFO,
			"wait_fault returned handle=0x%x fault_type=%u domain=%d\n",
			args.handle, args.fault_type, args.domain_id);
		break;
	}

	/* ---- AVAIL_RES_ACQUIRE --------------------------------------- */
	case COMPRESSCHED_IOCTL_AVAIL_RES_ACQUIRE:
	{
		struct compressched_ioctl_avail_res_acquire_args args = {};
		compressched_dp_cmd_t   kblob  = {};
		compressched_dp_param_t kparam = {};
		compressched_dp_result_t kresult = {};
		const compressched_dp_param_t *param_ptr = NULL;

		if (copy_from_user(&args, (void __user *)arg, sizeof(args))) {
			LOG_COMPRESSCHEDFE(LEVEL_ERR,
				"copy_from_user failed (AVAIL_RES_ACQUIRE)\n");
			return -EFAULT;
		}

		/*
		 * Map the UAPI blob to the kernel compressched_dp_cmd_t.
		 * Both structs have identical layout (same field order, same
		 * explicit pad0) so a direct memcpy is safe.
		 */
		BUILD_BUG_ON(sizeof(kblob) != sizeof(args.cmd));
		memcpy(&kblob, &args.cmd, sizeof(kblob));

		if (args.has_param) {
			BUILD_BUG_ON(sizeof(kparam) != sizeof(args.param));
			memcpy(&kparam, &args.param, sizeof(kparam));
			param_ptr = &kparam;
		}

		ret = compressched_avail_res_acquire(
				(compressched_handle)args.cmd.handle,
				&kblob, param_ptr, &kresult);
		if (ret) {
			LOG_COMPRESSCHEDFE(LEVEL_ERR,
				"avail_res_acquire failed handle=0x%x ret=%d\n",
				args.cmd.handle, ret);
			return ret;
		}

		/* Copy the result back to userspace */
		BUILD_BUG_ON(sizeof(args.result) != sizeof(kresult));
		memcpy(&args.result, &kresult, sizeof(args.result));

		if (copy_to_user((void __user *)arg, &args, sizeof(args))) {
			LOG_COMPRESSCHEDFE(LEVEL_ERR,
				"copy_to_user failed (AVAIL_RES_ACQUIRE)\n");
			return -EFAULT;
		}

		LOG_COMPRESSCHEDFE(LEVEL_INFO,
			"avail_res_acquire successful handle=0x%x "
			"token=0x%x count=%u\n",
			args.cmd.handle, args.result.token,
			args.result.available_count);
		break;
	}

	/* ---- AVAIL_RES_RELEASE --------------------------------------- */
	case COMPRESSCHED_IOCTL_AVAIL_RES_RELEASE:
	{
		struct compressched_ioctl_avail_res_release_args args;
		compressched_dp_cmd_t kblob = {};

		if (copy_from_user(&args, (void __user *)arg, sizeof(args))) {
			LOG_COMPRESSCHEDFE(LEVEL_ERR,
				"copy_from_user failed (AVAIL_RES_RELEASE)\n");
			return -EFAULT;
		}

		BUILD_BUG_ON(sizeof(kblob) != sizeof(args.cmd));
		memcpy(&kblob, &args.cmd, sizeof(kblob));

		ret = compressched_avail_res_release(
				(compressched_handle)args.cmd.handle,
				&kblob,
				(__u32)args.token);
		if (ret) {
			LOG_COMPRESSCHEDFE(LEVEL_ERR,
				"avail_res_release failed handle=0x%x "
				"token=0x%x ret=%d\n",
				args.cmd.handle, args.token, ret);
			return ret;
		}

		/* No output fields — copy_to_user not needed for RELEASE */
		LOG_COMPRESSCHEDFE(LEVEL_INFO,
			"avail_res_release successful handle=0x%x token=0x%x\n",
			args.cmd.handle, args.token);
		break;
	}

	/* ---- Unknown ------------------------------------------------- */
	default:
		LOG_COMPRESSCHEDFE(LEVEL_ERR, "unknown ioctl cmd=0x%x\n", cmd);
		return -ENOTTY;
	}

	return ret;
}

/* ------------------------------------------------------------------ */
/* misc device registration                                            */
/* ------------------------------------------------------------------ */

static const struct file_operations compressched_fe_fops = {
	.owner          = THIS_MODULE,
	.open           = compressched_fe_open,
	.release        = compressched_fe_release,
	.unlocked_ioctl = compressched_fe_ioctl,
#ifdef CONFIG_COMPAT
	/*
	 * All ioctl structs are flat (no pointer members) so compat_ptr_ioctl
	 * handles 32-bit userspace correctly without a custom compat handler.
	 */
	.compat_ioctl   = compat_ptr_ioctl,
#endif
};

static struct miscdevice compressched_fe_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = COMPRESSCHED_FE_DEV_NAME,   /* creates /dev/compressched_fe */
	.fops  = &compressched_fe_fops,
};

/**
 * compressched_cdev_init() - Register /dev/compressched_fe.
 *
 * Called from virtio_compressched_probe() after the virtio queues and
 * client table are fully initialised.
 *
 * Return: 0 on success, negative errno on failure.
 */
int compressched_cdev_init(void)
{
	int ret;

	ret = misc_register(&compressched_fe_misc);
	if (ret) {
		LOG_COMPRESSCHEDFE(LEVEL_ERR, "misc_register failed ret=%d\n", ret);
		return ret;
	}
	LOG_COMPRESSCHEDFE(LEVEL_INFO, "misc_register OK - /dev/%s ready\n",
			   COMPRESSCHED_FE_DEV_NAME);
	return 0;
}

/**
 * compressched_cdev_exit() - Deregister /dev/compressched_fe.
 *
 * Called from virtio_compressched_remove() during driver teardown.
 */
void compressched_cdev_exit(void)
{
	misc_deregister(&compressched_fe_misc);
	LOG_COMPRESSCHEDFE(LEVEL_INFO, "/dev/%s deregistered\n",
			   COMPRESSCHED_FE_DEV_NAME);
}