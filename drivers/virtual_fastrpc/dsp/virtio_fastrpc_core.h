/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2023, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __VIRTIO_FASTRPC_CORE_H__
#define __VIRTIO_FASTRPC_CORE_H__

#include "fastrpc_common.h"

extern const struct vfastrpc_operations vfrpc_ops;
struct vfastrpc_file *vfastrpc_file_alloc(const struct vfastrpc_operations *ops);
int vfastrpc_file_free(struct vfastrpc_file *vfl);
#endif /*__VIRTIO_FASTRPC_CORE_H__*/
