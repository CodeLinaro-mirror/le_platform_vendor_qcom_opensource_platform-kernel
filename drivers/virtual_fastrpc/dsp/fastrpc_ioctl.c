// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/spinlock.h>
#include <linux/types.h>
#include "fastrpc_common.h"

#define INIT_FILELEN_MAX		(2*1024*1024)
#define INIT_MEMLEN_MAX			(8*1024*1024)

static inline void get_fastrpc_ioctl_mmap_64(
			struct fastrpc_ioctl_mmap_64 *mmap64,
			struct fastrpc_ioctl_mmap *immap)
{
	immap->fd = mmap64->fd;
	immap->flags = mmap64->flags;
	immap->vaddrin = (uintptr_t)mmap64->vaddrin;
	immap->size = mmap64->size;
}

static inline void put_fastrpc_ioctl_mmap_64(
			struct fastrpc_ioctl_mmap_64 *mmap64,
			struct fastrpc_ioctl_mmap *immap)
{
	mmap64->vaddrout = (uint64_t)immap->vaddrout;
}

static inline void get_fastrpc_ioctl_munmap_64(
			struct fastrpc_ioctl_munmap_64 *munmap64,
			struct fastrpc_ioctl_munmap *imunmap)
{
	imunmap->vaddrout = (uintptr_t)munmap64->vaddrout;
	imunmap->size = munmap64->size;
}

static int vfastrpc_control_ioctl(struct fastrpc_ioctl_control *cp,
		void *param, struct vfastrpc_file *vfl)
{
	int err = 0;

	K_COPY_FROM_USER(err, 0, cp, param,
			sizeof(*cp));
	if (err)
		return err;

	VERIFY(err, 0 == (err = vfl->ops->control(vfl, cp)));
	if (err)
		return err;

	if (cp->req == FASTRPC_CONTROL_KALLOC)
		K_COPY_TO_USER(err, 0, param, cp, sizeof(*cp));

	return err;
}

static int vfastrpc_init_ioctl(struct fastrpc_ioctl_init_attrs *init,
		void *param, struct vfastrpc_file *vfl)
{
	int err = 0;

	VERIFY(err, init->init.filelen >= 0 &&
		init->init.filelen < INIT_FILELEN_MAX);
	if (err)
		return err;

	VERIFY(err, init->init.memlen >= 0 &&
		init->init.memlen < INIT_MEMLEN_MAX);
	if (err)
		return err;

	VERIFY(err, 0 == (err = vfl->ops->init_process(vfl, init)));
	return err;
}

static int vfastrpc_get_dsp_info(struct fastrpc_ioctl_capability *cap,
		void *param, struct vfastrpc_file *vfl)
{
	int err = 0;

	K_COPY_FROM_USER(err, 0, cap, param, sizeof(struct fastrpc_ioctl_capability));
	if (err)
		goto bail;

	VERIFY(err, cap->domain < vfl->apps->num_channels);
	if (err) {
		err = -ECHRNG;
		goto bail;
	}
	cap->capability = 0;

	err = vfl->ops->get_info_from_kernel(vfl, cap);
	if (err)
		goto bail;
	K_COPY_TO_USER(err, 0, &((struct fastrpc_ioctl_capability *)
				param)->capability, &cap->capability, sizeof(cap->capability));
bail:
	return err;
}

static int vfastrpc_mmap_ioctl(struct vfastrpc_file *vfl,
		unsigned int ioctl_num, union fastrpc_ioctl_param *p,
		void *param)
{
	union {
		struct fastrpc_ioctl_mmap mmap;
		struct fastrpc_ioctl_munmap munmap;
	} i;
	struct vfastrpc_apps *me = vfl->apps;
	int err = 0;

	switch (ioctl_num) {
	case FASTRPC_IOCTL_MEM_MAP:
		if (!me->has_hybrid && !me->has_mem_map) {
			dev_err(me->dev, "mem_map is not supported\n");
			return -ENOTTY;
		}
		K_COPY_FROM_USER(err, 0, &p->mem_map, param,
						sizeof(p->mem_map));
		if (err)
			return err;

		VERIFY(err, 0 == (err = vfl->ops->mem_map(vfl,
						&p->mem_map)));
		if (err)
			return err;

		K_COPY_TO_USER(err, 0, param, &p->mem_map, sizeof(p->mem_map));
		if (err)
			return err;
		break;
	case FASTRPC_IOCTL_MEM_UNMAP:
		if (!me->has_hybrid && !me->has_mem_map) {
			dev_err(me->dev, "mem_unmap is not supported\n");
			return -ENOTTY;
		}
		K_COPY_FROM_USER(err, 0, &p->mem_unmap, param,
						sizeof(p->mem_unmap));
		if (err)
			return err;

		VERIFY(err, 0 == (err = vfl->ops->mem_unmap(vfl,
						&p->mem_unmap)));
		if (err)
			return err;
		break;
	case FASTRPC_IOCTL_MMAP:
		if (!me->has_hybrid && !me->has_mmap) {
			dev_err(me->dev, "mmap is not supported\n");
			return -ENOTTY;
		}

		K_COPY_FROM_USER(err, 0, &p->mmap, param, sizeof(p->mmap));
		if (err)
			return err;

		VERIFY(err, 0 == (err = vfl->ops->mmap(vfl, &p->mmap)));
		if (err)
			return err;

		K_COPY_TO_USER(err, 0, param, &p->mmap, sizeof(p->mmap));
		break;
	case FASTRPC_IOCTL_MUNMAP:
		if (!me->has_hybrid && !me->has_mmap) {
			dev_err(me->dev, "munmap is not supported\n");
			return -ENOTTY;
		}

		K_COPY_FROM_USER(err, 0, &p->munmap, param, sizeof(p->munmap));
		if (err)
			return err;

		VERIFY(err, 0 == (err = vfl->ops->munmap(vfl, &p->munmap)));
		break;
	case FASTRPC_IOCTL_MMAP_64:
		if (!me->has_hybrid && !me->has_mmap) {
			dev_err(me->dev, "mmap is not supported\n");
			return -ENOTTY;
		}

		K_COPY_FROM_USER(err, 0, &p->mmap64, param, sizeof(p->mmap64));
		if (err)
			return err;

		get_fastrpc_ioctl_mmap_64(&p->mmap64, &i.mmap);
		VERIFY(err, 0 == (err = vfl->ops->mmap(vfl, &i.mmap)));
		if (err)
			return err;

		put_fastrpc_ioctl_mmap_64(&p->mmap64, &i.mmap);
		K_COPY_TO_USER(err, 0, param, &p->mmap64, sizeof(p->mmap64));
		break;
	case FASTRPC_IOCTL_MUNMAP_64:
		if (!me->has_hybrid && !me->has_mmap) {
			dev_err(me->dev, "munmap is not supported\n");
			return -ENOTTY;
		}

		K_COPY_FROM_USER(err, 0, &p->munmap64, param,
						sizeof(p->munmap64));
		if (err)
			return err;

		get_fastrpc_ioctl_munmap_64(&p->munmap64, &i.munmap);
		VERIFY(err, 0 == (err = vfl->ops->munmap(vfl, &i.munmap)));
		break;
	case FASTRPC_IOCTL_MUNMAP_FD:
		K_COPY_FROM_USER(err, 0, &p->munmap_fd, param, sizeof(p->munmap_fd));
		if (err)
			return err;

		VERIFY(err, 0 == (err = vfl->ops->munmap_fd(vfl, &p->munmap_fd)));
		break;
	default:
		err = -ENOTTY;
		dev_err(me->dev, "bad ioctl: 0x%x\n", ioctl_num);
		break;
	}
	return err;
}

static int check_invoke_supported(struct vfastrpc_file *vfl,
		struct fastrpc_ioctl_invoke_async *inv)
{
	int err = 0;
	struct vfastrpc_apps *me = vfl->apps;

	if (me->has_hybrid)
		return 0;

	if (inv->attrs && !(me->has_invoke_attr)) {
		dev_err(me->dev, "invoke attr is not supported\n");
		return -ENOTTY;
	}

	if (inv->crc && !(me->has_invoke_crc)) {
		dev_err(me->dev, "invoke crc is not supported\n");
		err = -ENOTTY;
	}
	return err;
}

long vfastrpc_ioctl(struct file *file, unsigned int ioctl_num,
			unsigned long ioctl_param)
{
	union fastrpc_ioctl_param p;
	void *param = (char *)ioctl_param;
	struct fastrpc_file *fl = (struct fastrpc_file *)file->private_data;
	struct vfastrpc_file *vfl = to_vfastrpc_file(fl);
	struct vfastrpc_apps *me = vfl->apps;
	int size = 0, err = 0;
	uint32_t info;

	p.inv.fds = NULL;
	p.inv.attrs = NULL;
	p.inv.crc = NULL;
	p.inv.perf_kernel = NULL;
	p.inv.perf_dsp = NULL;
	p.inv.job = NULL;

	ADSP_LOG("ioctl: 0x%x\n", ioctl_num);
	spin_lock(&fl->hlock);
	if (fl->file_close >= FASTRPC_PROCESS_EXIT_START) {
		err = -ESHUTDOWN;
		dev_warn(me->dev, "fastrpc_device_release is happening, So not sending any new requests to DSP\n");
		spin_unlock(&fl->hlock);
		goto bail;
	}
	spin_unlock(&fl->hlock);

	switch (ioctl_num) {
	case FASTRPC_IOCTL_INVOKE:
		size = sizeof(struct fastrpc_ioctl_invoke);
		fallthrough;
	case FASTRPC_IOCTL_INVOKE_FD:
		if (!size)
			size = sizeof(struct fastrpc_ioctl_invoke_fd);
		fallthrough;
	case FASTRPC_IOCTL_INVOKE_ATTRS:
		if (!size)
			size = sizeof(struct fastrpc_ioctl_invoke_attrs);
		fallthrough;
	case FASTRPC_IOCTL_INVOKE_CRC:
		if (!size)
			size = sizeof(struct fastrpc_ioctl_invoke_crc);
		fallthrough;
	case FASTRPC_IOCTL_INVOKE_PERF:
		if (!size)
			size = sizeof(struct fastrpc_ioctl_invoke_perf);
		K_COPY_FROM_USER(err, 0, &p.inv, param, size);
		if (err) {
			err = -EFAULT;
			goto bail;
		}
		err = check_invoke_supported(vfl, &p.inv);
		if (err)
			goto bail;

		err = vfl->ops->invoke(vfl, fl->mode, &p.inv, USER_MSG);

		if (err)
			goto bail;
		break;
	case FASTRPC_IOCTL_INVOKE2:
		K_COPY_FROM_USER(err, 0, &p.inv2, param,
					sizeof(struct fastrpc_ioctl_invoke2));
		if (err) {
			err = -EFAULT;
			goto bail;
		}
		err = vfl->ops->invoke2(vfl, &p.inv2, false);
		if (err)
			goto bail;
		break;
	case FASTRPC_IOCTL_SETMODE:
		err = vfl->ops->setmode(vfl, ioctl_param);
		break;
	case FASTRPC_IOCTL_CONTROL:
		err = vfastrpc_control_ioctl(&p.cp, param, vfl);
		break;
	case FASTRPC_IOCTL_GETINFO:
		K_COPY_FROM_USER(err, 0, &info, param, sizeof(info));
		if (err) {
			err = -EFAULT;
			goto bail;
		}
		VERIFY(err, 0 == (err = vfl->ops->get_info(vfl, &info)));
		if (err)
			goto bail;
		K_COPY_TO_USER(err, 0, param, &info, sizeof(info));
		if (err) {
			err = -EFAULT;
			goto bail;
		}
		break;
	case FASTRPC_IOCTL_INIT:
		p.init.attrs = 0;
		p.init.siglen = 0;
		size = sizeof(struct fastrpc_ioctl_init);
		fallthrough;
	case FASTRPC_IOCTL_INIT_ATTRS:
		if (!size)
			size = sizeof(struct fastrpc_ioctl_init_attrs);
		K_COPY_FROM_USER(err, 0, &p.init, param, size);
		if (err) {
			err = -EFAULT;
			goto bail;
		}
		VERIFY(err, 0 == (err = vfastrpc_init_ioctl(&p.init, param, vfl)));
		if (err)
			goto bail;
		break;
	case FASTRPC_IOCTL_GET_DSP_INFO:
		err = vfastrpc_get_dsp_info(&p.cap, param, vfl);
		break;
	case FASTRPC_IOCTL_MEM_MAP:
		fallthrough;
	case FASTRPC_IOCTL_MEM_UNMAP:
		fallthrough;
	case FASTRPC_IOCTL_MMAP:
		fallthrough;
	case FASTRPC_IOCTL_MUNMAP:
		fallthrough;
	case FASTRPC_IOCTL_MMAP_64:
		fallthrough;
	case FASTRPC_IOCTL_MUNMAP_64:
		fallthrough;
	case FASTRPC_IOCTL_MUNMAP_FD:
		err = vfastrpc_mmap_ioctl(vfl, ioctl_num, &p, param);
		break;

	case FASTRPC_IOCTL_DSPSIGNAL_SIGNAL:
		K_COPY_FROM_USER(err, 0, &p.sig, param,
					sizeof(struct fastrpc_ioctl_dspsignal_signal));
		if (err) {
			err = -EFAULT;
			goto bail;
		}
		VERIFY(err, 0 == (err = vfl->ops->dspsignal_signal(vfl, &p.sig)));
		if (err)
			goto bail;
		break;
	case FASTRPC_IOCTL_DSPSIGNAL_WAIT:
		K_COPY_FROM_USER(err, 0, &p.wait, param,
					sizeof(struct fastrpc_ioctl_dspsignal_wait));
		if (err) {
			err = -EFAULT;
			goto bail;
		}
		VERIFY(err, 0 == (err = vfl->ops->dspsignal_wait(vfl, &p.wait)));
		if (err)
			goto bail;
		break;
	case FASTRPC_IOCTL_DSPSIGNAL_CREATE:
		K_COPY_FROM_USER(err, 0, &p.cre, param,
					sizeof(struct fastrpc_ioctl_dspsignal_create));
		if (err) {
			err = -EFAULT;
			goto bail;
		}
		VERIFY(err, 0 == (err = vfl->ops->dspsignal_create(vfl, &p.cre)));
		if (err)
			goto bail;
		break;
	case FASTRPC_IOCTL_DSPSIGNAL_DESTROY:
		K_COPY_FROM_USER(err, 0, &p.des, param,
					sizeof(struct fastrpc_ioctl_dspsignal_destroy));
		if (err) {
			err = -EFAULT;
			goto bail;
		}
		VERIFY(err, 0 == (err = vfl->ops->dspsignal_destroy(vfl, &p.des)));
		if (err)
			goto bail;
		break;
	case FASTRPC_IOCTL_DSPSIGNAL_CANCEL_WAIT:
		K_COPY_FROM_USER(err, 0, &p.canc, param,
					sizeof(struct fastrpc_ioctl_dspsignal_cancel_wait));
		if (err) {
			err = -EFAULT;
			goto bail;
		}
		VERIFY(err, 0 == (err = vfl->ops->dspsignal_cancel_wait(vfl, &p.canc)));
		if (err)
			goto bail;
		break;
	default:
		err = -ENOTTY;
		dev_err(me->dev, "bad ioctl: 0x%x\n", ioctl_num);
		break;
	}
bail:
	return err;
}

