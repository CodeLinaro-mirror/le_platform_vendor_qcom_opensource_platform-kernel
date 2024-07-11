// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include "fastrpc_common.h"

int fastrpc_get_info(struct fastrpc_file *fl, uint32_t *info)
{
	struct vfastrpc_file *vfl = to_vfastrpc_file(fl);

	return vfl->ops->get_info(vfl, info);
}

int fastrpc_get_info_from_kernel(struct fastrpc_ioctl_capability *cap,
					struct fastrpc_file *fl)
{
	struct vfastrpc_file *vfl = to_vfastrpc_file(fl);

	return vfl->ops->get_info_from_kernel(vfl, cap);
}

int fastrpc_internal_control(struct fastrpc_file *fl,
				struct fastrpc_ioctl_control *cp)
{
	struct vfastrpc_file *vfl = to_vfastrpc_file(fl);

	return vfl->ops->control(vfl, cp);
}

int fastrpc_init_process(struct fastrpc_file *fl,
				struct fastrpc_ioctl_init_attrs *uproc)
{
	struct vfastrpc_file *vfl = to_vfastrpc_file(fl);

	return vfl->ops->init_process(vfl, uproc);
}

int fastrpc_internal_mmap(struct fastrpc_file *fl,
				struct fastrpc_ioctl_mmap *ud)
{
	struct vfastrpc_file *vfl = to_vfastrpc_file(fl);

	return vfl->ops->mmap(vfl, ud);
}

int fastrpc_internal_munmap(struct fastrpc_file *fl,
				struct fastrpc_ioctl_munmap *ud)
{
	struct vfastrpc_file *vfl = to_vfastrpc_file(fl);

	return vfl->ops->munmap(vfl, ud);
}

int fastrpc_internal_munmap_fd(struct fastrpc_file *fl,
				struct fastrpc_ioctl_munmap_fd *ud)
{
	struct vfastrpc_file *vfl = to_vfastrpc_file(fl);

	return vfl->ops->munmap_fd(vfl, ud);
}

int fastrpc_internal_mem_map(struct fastrpc_file *fl,
				struct fastrpc_ioctl_mem_map *ud)
{
	struct vfastrpc_file *vfl = to_vfastrpc_file(fl);

	return vfl->ops->mem_map(vfl, ud);
}

int fastrpc_internal_mem_unmap(struct fastrpc_file *fl,
				struct fastrpc_ioctl_mem_unmap *ud)
{
	struct vfastrpc_file *vfl = to_vfastrpc_file(fl);

	return vfl->ops->mem_unmap(vfl, ud);
}

int fastrpc_setmode(unsigned long ioctl_param, struct fastrpc_file *fl)
{
	struct vfastrpc_file *vfl = to_vfastrpc_file(fl);

	return vfl->ops->setmode(vfl, ioctl_param);
}

int fastrpc_internal_invoke(struct fastrpc_file *fl, uint32_t mode,
				uint32_t kernel,
				struct fastrpc_ioctl_invoke_async *inv)
{
	struct vfastrpc_file *vfl = to_vfastrpc_file(fl);

	return vfl->ops->invoke(vfl, mode, inv, kernel);
}

int fastrpc_internal_invoke2(struct fastrpc_file *fl,
				struct fastrpc_ioctl_invoke2 *inv2, bool is_compat)
{
	struct vfastrpc_file *vfl = to_vfastrpc_file(fl);

	return vfl->ops->invoke2(vfl, inv2, is_compat);
}

int fastrpc_dspsignal_cancel_wait(struct fastrpc_file *fl,
			struct fastrpc_ioctl_dspsignal_cancel_wait *cancel)
{
	struct vfastrpc_file *vfl = to_vfastrpc_file(fl);

	return vfl->ops->dspsignal_cancel_wait(vfl, cancel);
}

int fastrpc_dspsignal_wait(struct fastrpc_file *fl,
			struct fastrpc_ioctl_dspsignal_wait *wait)
{
	struct vfastrpc_file *vfl = to_vfastrpc_file(fl);

	return vfl->ops->dspsignal_wait(vfl, wait);
}

int fastrpc_dspsignal_signal(struct fastrpc_file *fl,
			struct fastrpc_ioctl_dspsignal_signal *sig)
{
	struct vfastrpc_file *vfl = to_vfastrpc_file(fl);

	return vfl->ops->dspsignal_signal(vfl, sig);
}

int fastrpc_dspsignal_create(struct fastrpc_file *fl,
			struct fastrpc_ioctl_dspsignal_create *create)
{
	struct vfastrpc_file *vfl = to_vfastrpc_file(fl);

	return vfl->ops->dspsignal_create(vfl, create);
}

int fastrpc_dspsignal_destroy(struct fastrpc_file *fl,
			struct fastrpc_ioctl_dspsignal_destroy *destroy)
{
	struct vfastrpc_file *vfl = to_vfastrpc_file(fl);

	return vfl->ops->dspsignal_destroy(vfl, destroy);
}

