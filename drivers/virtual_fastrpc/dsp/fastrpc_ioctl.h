/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __VIRTIO_FASTRPC_IOCTL_H__
#define __VIRTIO_FASTRPC_IOCTL_H__

#include <linux/fs.h>

extern long vfastrpc_ioctl(struct file *file, unsigned int ioctl_num,
				unsigned long ioctl_param);
#endif /*__VIRTIO_FASTRPC_IOCTL_H__*/
