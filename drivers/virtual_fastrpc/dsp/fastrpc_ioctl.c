// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023-2025, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/spinlock.h>
#include <linux/types.h>
#include "fastrpc_common.h"
#include "fastrpc_core.h"

long fastrpc_device_ioctl(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	struct fastrpc_user *fl = (struct fastrpc_user *)file->private_data;
	struct fastrpc_channel_ctx *cctx = fl->cctx;
	char __user *argp = (char __user *)arg;
	int err;
	int process_init = 0;
	unsigned long flags = 0;

	RPC_DBG("ioctl: 0x%x\n", cmd);
	fastrpc_channel_ctx_get(cctx);
	spin_lock_irqsave(&cctx->lock, flags);
	if (atomic_read(&cctx->teardown) || fl->state >= DSP_EXIT_START) {
		spin_unlock_irqrestore(&cctx->lock, flags);
		fastrpc_channel_ctx_put(cctx);
		RPC_DBG("ioctl refused: teardown=%d,state=%d\n",
				atomic_read(&cctx->teardown),
				fl->state);
		return -EPIPE;
	}

	fastrpc_channel_update_invoke_cnt(cctx, true);
	spin_unlock_irqrestore(&cctx->lock, flags);

	switch (cmd) {
	case FASTRPC_IOCTL_INVOKE:
		err = fastrpc_invoke(fl, argp);
		break;
	case FASTRPC_IOCTL_MULTIMODE_INVOKE:
		err = fastrpc_multimode_invoke(fl, argp);
		break;
	case FASTRPC_IOCTL_INIT_CREATE:
		err = fastrpc_init_create_process(fl, argp);
		process_init = 1;
		break;
	case FASTRPC_IOCTL_MMAP:
		mutex_lock(&fl->remote_map_mutex);
		err = fastrpc_req_mmap(fl, argp);
		mutex_unlock(&fl->remote_map_mutex);
		break;
	case FASTRPC_IOCTL_MUNMAP:
		mutex_lock(&fl->remote_map_mutex);
		err = fastrpc_req_munmap(fl, argp);
		mutex_unlock(&fl->remote_map_mutex);
		break;
	case FASTRPC_IOCTL_MEM_MAP:
		err = fastrpc_req_mem_map(fl, argp);
		break;
	case FASTRPC_IOCTL_MEM_UNMAP:
		err = fastrpc_req_mem_unmap(fl, argp);
		break;
	case FASTRPC_IOCTL_GET_DSP_INFO:
		err = fastrpc_get_dsp_info(fl, argp);
		break;
	default:
		err = -ENOTTY;
		RPC_DBG("unsupported ioctl: 0x%x\n", cmd);
		break;
	}

	if (process_init && !err) {
		spin_lock_irqsave(&fl->lock, flags);
		fl->state = DSP_CREATE_COMPLETE;
		spin_unlock_irqrestore(&fl->lock, flags);
	}

	fastrpc_channel_update_invoke_cnt(cctx, false);

	fastrpc_channel_ctx_put(fl->cctx);

	return err;
}
