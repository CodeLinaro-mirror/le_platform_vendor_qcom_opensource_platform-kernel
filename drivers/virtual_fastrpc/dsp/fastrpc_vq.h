/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2025, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __FASTRPC_VQ_H__
#define __FASTRPC_VQ_H__

#include "fastrpc_common.h"

struct virt_fastrpc_msg *virt_alloc_msg(struct fastrpc_user *fl, int size);
void virt_free_msg(struct fastrpc_user *fl, struct virt_fastrpc_msg *msg);
int fastrpc_txbuf_send(struct fastrpc_user *fl, void *data, unsigned int len);
void fastrpc_rxbuf_send(struct fastrpc_user *fl, void *data, unsigned int len);
#endif /*__FASTRPC_VQ_H__*/
