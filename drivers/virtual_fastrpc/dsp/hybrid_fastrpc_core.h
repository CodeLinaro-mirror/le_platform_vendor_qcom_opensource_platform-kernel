/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __HYBRID_FASTRPC_CORE_H__
#define __HYBRID_FASTRPC_CORE_H__

#include "fastrpc_common.h"

extern const struct vfastrpc_operations hfrpc_ops;
struct vfastrpc_file *hfastrpc_file_alloc(const struct vfastrpc_operations *ops);
int hfastrpc_file_free(struct vfastrpc_file *vfl);
#endif /*__HYBRID_FASTRPC_CORE_H__*/
