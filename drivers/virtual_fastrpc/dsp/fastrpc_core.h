/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __FASTRPC_CORE_H__
#define __FASTRPC_CORE_H__

#include "fastrpc_common.h"
int fastrpc_device_register(struct device *dev, struct fastrpc_channel_ctx *cctx,
		bool is_secured, bool legacy, const char *domain);
void fastrpc_channel_ctx_get(struct fastrpc_channel_ctx *cctx);
void fastrpc_channel_ctx_put(struct fastrpc_channel_ctx *cctx);
void fastrpc_channel_update_invoke_cnt(
		struct fastrpc_channel_ctx *cctx, bool incr);
void fastrpc_free_user(struct fastrpc_user *fl);
int fastrpc_get_dsp_info(struct fastrpc_user *fl, char __user *argp);
int fastrpc_init_create_process(struct fastrpc_user *fl, char __user *argp);
int fastrpc_invoke(struct fastrpc_user *fl, char __user *argp);
int fastrpc_multimode_invoke(struct fastrpc_user *fl, char __user *argp);
int fastrpc_req_mmap(struct fastrpc_user *fl, char __user *argp);
int fastrpc_req_munmap(struct fastrpc_user *fl, char __user *argp);
int fastrpc_req_mem_map(struct fastrpc_user *fl, char __user *argp);
int fastrpc_req_mem_unmap(struct fastrpc_user *fl, char __user *argp);
void fastrpc_queue_pd_status(struct fastrpc_user *fl, int domain, int status, int sessionid);
void fastrpc_notify_user_ctx(struct fastrpc_invoke_ctx *ctx, int retval,
				u32 rsp_flags, u32 early_wake_time);
#endif /*__FASTRPC_CORE_H__*/
