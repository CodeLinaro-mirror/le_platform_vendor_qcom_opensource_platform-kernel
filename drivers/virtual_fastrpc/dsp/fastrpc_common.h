/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2023-2024, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __FASTRPC_COMMON_H__
#define __FASTRPC_COMMON_H__

#include <linux/cdev.h>
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/mutex.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/virtio.h>
#include <linux/slab.h>
#include <linux/scatterlist.h>
#include <linux/wait.h>
#include "adsprpc_compat.h"
#include "adsprpc_shared.h"

#define NON_SECURE_CHANNEL		0
#define SECURE_CHANNEL			1

#define MINOR_NUM_DEV			0
#define MINOR_NUM_SECURE_DEV		1

#define PID_SIZE			10
#define FASTRPC_MSG_MAX			256

/* fastRPC DSP firmware capability */
#define UNSIGNED_PD_SUPPORT		1

/* fastRPC kernel driver capability */
#define PERF_CAPABILITY_SUPPORT		(1 << 1)
#define KERNEL_ERROR_CODE_V1_SUPPORT	1
#define USERSPACE_ALLOCATION_SUPPORT	1
#define DSPSIGNAL_SUPPORT		1

#define VIRTIO_FASTRPC_CMD_OPEN			1
#define VIRTIO_FASTRPC_CMD_CLOSE		2
#define VIRTIO_FASTRPC_CMD_INVOKE		3
#define VIRTIO_FASTRPC_CMD_MMAP			4
#define VIRTIO_FASTRPC_CMD_MUNMAP		5
#define VIRTIO_FASTRPC_CMD_CONTROL		6
#define VIRTIO_FASTRPC_CMD_GET_DSP_INFO		7
#define VIRTIO_FASTRPC_CMD_MUNMAP_FD		8
#define VIRTIO_FASTRPC_CMD_MEM_MAP		9
#define VIRTIO_FASTRPC_CMD_MEM_UNMAP		10
#define VIRTIO_FASTRPC_CMD_SMMU_MAP		11
#define VIRTIO_FASTRPC_CMD_SMMU_UNMAP		12

#define STATIC_PD			0
#define DYNAMIC_PD			1
#define GUEST_OS			2

#define ADSP_MMAP_HEAP_ADDR		4
#define ADSP_MMAP_REMOTE_HEAP_ADDR	8
#define ADSP_MMAP_ADD_PAGES		0x1000

#define RPC_TIMEOUT			(5 * HZ)
#define BALIGN				128
#define M_FDLIST			16
#define M_CRCLIST			64
#define M_KERNEL_PERF_LIST (PERF_KEY_MAX)
#define M_DSP_PERF_LIST			12

#define FASTRPC_DMAHANDLE_NOMAP	16

#define FASTRPC_STATIC_HANDLE_KERNEL	1
#define FASTRPC_STATIC_HANDLE_LISTENER	3
#define FASTRPC_STATIC_HANDLE_MAX	20

/* Max value of unique fastrpc tgid. The value range
 * is 1 - 255, so 255 PDs can be opened at the same
 * time as maximum if hardware resource is enough. */
#define MAX_FRPC_TGID 256

#define PERF_END ((void)0)

#define PERF(enb, cnt, ff) \
	{\
		struct timespec64 startT = {0};\
		uint64_t *counter = cnt;\
		if (enb && counter) {\
			ktime_get_real_ts64(&startT);\
		} \
		ff ;\
		if (enb && counter) {\
			*counter += getnstimediff(&startT);\
		} \
	}

#define GET_COUNTER(perf_ptr, offset)  \
	(perf_ptr != NULL ?\
		(((offset >= 0) && (offset < PERF_KEY_MAX)) ?\
			(uint64_t *)(perf_ptr + offset)\
				: (uint64_t *)NULL) : (uint64_t *)NULL)


/* set for cached mapping */
#define VFASTRPC_MAP_ATTR_CACHED	1

/* set for internal nested mapping */
#define VFASTRPC_MAP_ATTR_INTERNAL_MAP  (1U << 1) /* 1: nested sglist, 0: plain sglist */

/* Fastrpc attribute  for already mapped buffer */
#define VFASTRPC_MAP_ATTR_BUFFER_MAPPED (128)

/* Use the second definition to enable additional dspsignal debug logging */
#define DSPSIGNAL_VERBOSE(x, ...)
/*#define DSPSIGNAL_VERBOSE ADSPRPC_INFO*/

#define to_fastrpc_file(x) ((struct fastrpc_file *)&(x)->file)
#define to_vfastrpc_file(x) container_of(x, struct vfastrpc_file, file)

#define K_COPY_FROM_USER(err, kernel, dst, src, size) \
	do {\
		if (!(kernel))\
			VERIFY(err, 0 == copy_from_user((dst),\
			(void const __user *)(src),\
							(size)));\
		else\
			memmove((dst), (src), (size));\
	} while (0)

#define K_COPY_TO_USER(err, kernel, dst, src, size) \
	do {\
		if (!(kernel))\
			VERIFY(err, 0 == copy_to_user((void __user *)(dst),\
						(src), (size)));\
		else\
			memmove((dst), (src), (size));\
	} while (0)

#define K_COPY_TO_USER_WITHOUT_ERR(kernel, dst, src, size) \
	do {\
		if (!(kernel))\
			(void)copy_to_user((void __user *)(dst),\
			(src), (size));\
		else\
			memmove((dst), (src), (size));\
	} while (0)

#ifdef ADSP_DEBUG
#define ADSP_LOG(format, args...) \
	pr_info("adsprpc (%d): %s: " format, current->pid,\
	__func__, ##args)
#else
#define ADSP_LOG(format, ...) ((void)0)
#endif

enum fastrpc_proc_attr {
	/* Macro for Debug attr */
	FASTRPC_MODE_DEBUG	= 1 << 0,
	/* Macro for Ptrace */
	FASTRPC_MODE_PTRACE	= 1 << 1,
	/* Macro for CRC Check */
	FASTRPC_MODE_CRC	= 1 << 2,
	/* Macro for Unsigned PD */
	FASTRPC_MODE_UNSIGNED_MODULE	= 1 << 3,
	/* Macro for Adaptive QoS */
	FASTRPC_MODE_ADAPTIVE_QOS	= 1 << 4,
	/* Macro for System Process */
	FASTRPC_MODE_SYSTEM_PROCESS	= 1 << 5,
	/* Macro for Prvileged Process */
	FASTRPC_MODE_PRIVILEGED	= (1 << 6),
};

struct virt_fastrpc_msg {
	struct completion work;
	struct vfastrpc_invoke_ctx *ctx;
	u16 msgid;
	void *txbuf;
	void *rxbuf;
};

struct vfastrpc_file;

struct vfastrpc_operations {
	int (*get_info)(struct vfastrpc_file *vfl, uint32_t *info);
	int (*get_info_from_kernel)(struct vfastrpc_file *vfl,
			struct fastrpc_ioctl_capability *cap);
	int (*control)(struct vfastrpc_file *vfl,
			struct fastrpc_ioctl_control *cp);
	int (*init_process)(struct vfastrpc_file *vfl,
			struct fastrpc_ioctl_init_attrs *uproc);
	int (*mmap)(struct vfastrpc_file *vfl,
			struct fastrpc_ioctl_mmap *ud);
	int (*munmap)(struct vfastrpc_file *vfl,
			struct fastrpc_ioctl_munmap *ud);
	int (*munmap_fd)(struct vfastrpc_file *vfl,
			struct fastrpc_ioctl_munmap_fd *ud);
	int (*mem_map)(struct vfastrpc_file *vfl,
			struct fastrpc_ioctl_mem_map *ud);
	int (*mem_unmap)(struct vfastrpc_file *vfl,
			struct fastrpc_ioctl_mem_unmap *ud);
	int (*setmode)(struct vfastrpc_file *vfl, unsigned long mode);
	int (*invoke)(struct vfastrpc_file *vfl, uint32_t mode,
			struct fastrpc_ioctl_invoke_async *inv, uint32_t msg_type);
	int (*invoke2)(struct vfastrpc_file *vfl,
			struct fastrpc_ioctl_invoke2 *inv2, bool is_compat);
	int (*dspsignal_cancel_wait)(struct vfastrpc_file *vfl,
			struct fastrpc_ioctl_dspsignal_cancel_wait *cancel);
	int (*dspsignal_wait)(struct vfastrpc_file *vfl,
			struct fastrpc_ioctl_dspsignal_wait *wait);
	int (*dspsignal_signal)(struct vfastrpc_file *vfl,
			struct fastrpc_ioctl_dspsignal_signal *sig);
	int (*dspsignal_create)(struct vfastrpc_file *vfl,
			struct fastrpc_ioctl_dspsignal_create *create);
	int (*dspsignal_destroy)(struct vfastrpc_file *vfl,
			struct fastrpc_ioctl_dspsignal_destroy *destroy);
};

struct vfastrpc_file {
	struct fastrpc_file file;
	struct vfastrpc_apps *apps;
	const struct vfastrpc_operations *ops;
	int domain;
	int procattrs;
	int upid;
	/*
	 * List to store virtio fastrpc cmds interrupted by signal while waiting
	 * for completion.
	 */
	struct hlist_head interrupted_cmds;
	/* Unique sequence num inside a process to identify the invoke msg. */
	atomic64_t seq_num;
	/* No. of persistent headers */
	unsigned int num_pers_hdrs;
	/* Pre-allocated header buffer */
	struct vfastrpc_buf *pers_hdr_buf;
	/* Pre-allocated buffer divided into N chunks */
	struct vfastrpc_buf *hdr_bufs;
};

struct vfastrpc_invoke_ctx {
	struct virt_fastrpc_msg *msg;
	struct completion work;
	int retval;
	size_t size;
	struct vfastrpc_buf_desc *desc;
	struct hlist_node hn;
	struct hlist_node asyncn;
	struct vfastrpc_mmap **maps;
	remote_arg_t *lpra;
	remote_arg64_t *rpra;
	remote_arg64_t *lrpra;
	int *fds;
	unsigned int outbufs_offset;
	unsigned int *attrs;
	struct vfastrpc_buf *buf;
	struct vfastrpc_buf *copybuf;
	size_t used;
	struct vfastrpc_file *vfl;
	int pid;
	int tgid;
	uint32_t sc;
	uint32_t handle;
	struct overlap *overs;
	struct overlap **overps;
	struct smq_msg smsg;
	uint32_t *crc;
	struct fastrpc_perf *perf;
	uint64_t *perf_kernel;
	uint64_t *perf_dsp;
	unsigned int magic;
	uint64_t ctxid;
	enum fastrpc_response_flags rsp_flags;
	uint32_t early_wake_time;
	bool is_work_done;
	bool is_early_wakeup;
	struct fastrpc_async_job asyncjob;
	/* Unique sequence num inside a process to identify the invoke msg. */
	s64 seq_num;
};

struct virt_msg_hdr {
	u32 pid;	/* GVM pid */
	u32 tid;	/* GVM tid */
	s32 cid;	/* channel id connected to DSP */
	u32 cmd;	/* command type */
	u32 len;	/* command length */
	u16 msgid;	/* unique message id */
	u32 result;	/* message return value */
} __packed;

struct virt_fastrpc_sgl {
	u64 pv;		/* buffer physical address */
	u64 len;	/* buffer length */
};

struct virt_fastrpc_sgtable {
	u32 nents;
	struct virt_fastrpc_sgl sgl[0];
} __packed;


struct virt_cap_msg {
	struct virt_msg_hdr hdr;	/* virtio fastrpc message header */
	u32 domain;		/* DSP domain id */
	u32 dsp_caps[FASTRPC_MAX_DSP_ATTRIBUTES];	/* DSP capability */
} __packed;

struct vfastrpc_channel_ctx {
	char *name;
	char *subsys;
	struct device *dev;
	struct mutex smd_mutex;
	uint64_t sesscount;
	uint64_t ssrcount;
	int in_hib;
	void *handle;
	struct notifier_block nb;
	int subsystemstate;

	int secure;
	bool unsigned_support;
	struct fastrpc_dsp_capabilities dsp_cap_kernel;
	uint64_t cpuinfo_todsp;
	bool cpuinfo_status;
	struct vfastrpc_invoke_ctx *ctxtable[FASTRPC_CTX_MAX];
	spinlock_t ctxlock;
	struct fastrpc_transport_log gmsg_log;
};

struct virt_fastrpc_vq {
	/* protects vq */
	spinlock_t vq_lock;
	struct virtqueue *vq;
};

struct virt_fastrpc_msg;

struct vfastrpc_apps {
	struct virtio_device *vdev;
	struct virt_fastrpc_vq rvq;
	struct virt_fastrpc_vq svq;
	void **rbufs;
	void **sbufs;
	unsigned int num_bufs;
	unsigned int order;
	unsigned int buf_size;
	unsigned int num_channels;
	int last_sbuf;

	bool has_invoke_attr;
	bool has_invoke_crc;
	bool has_mmap;
	bool has_control;
	bool has_mem_map;
	u32 signed_pd_control;
	bool has_hybrid;

	struct device *dev;
	struct cdev cdev;
	struct class *class;
	dev_t dev_no;

	struct vfastrpc_channel_ctx *channel;
	struct dentry *debugfs_root;
	const struct file_operations *debugfs_fops;
	spinlock_t msglock;
	struct virt_fastrpc_msg *msgtable[FASTRPC_MSG_MAX];
	uint32_t max_sess_per_proc;
	spinlock_t hlock;
	struct hlist_head drivers;
	uint32_t duplicate_rsp_err_cnt;
};

int get_unique_hlos_process_id(struct vfastrpc_file *vfl);
void put_unique_hlos_process_id(struct vfastrpc_file *vfl);
int virt_fastrpc_close(struct vfastrpc_file *vfl);
void vfastrpc_queue_completed_async_job(struct vfastrpc_invoke_ctx *ctx);
void virt_free_msg(struct vfastrpc_file *vfl, struct virt_fastrpc_msg *msg);
struct virt_fastrpc_msg *virt_alloc_msg(struct vfastrpc_file *vfl, int size);
int virt_fastrpc_get_dsp_info(struct vfastrpc_file *vfl,
		u32 *dsp_attributes);

static inline unsigned long long msm_hr_timer_get_sclk_ticks(void)
{
	return 0;
}
#endif /*__FASTRPC_COMMON_H__*/
