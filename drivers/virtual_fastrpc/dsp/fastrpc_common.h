/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2023-2025, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __FASTRPC_COMMON_H__
#define __FASTRPC_COMMON_H__

#include <linux/cdev.h>
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/idr.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/scatterlist.h>
#include <linux/virtio.h>
#include <linux/wait.h>
#include "fastrpc.h"

#define ADSP_DOMAIN_ID			0
#define MDSP_DOMAIN_ID			1
#define SDSP_DOMAIN_ID			2
#define CDSP_DOMAIN_ID			3
#define FASTRPC_DEV_MAX			4 /* adsp, mdsp, slpi, cdsp*/
#define FASTRPC_MAX_SESSIONS		14
#define FASTRPC_MAX_SESSIONS_PER_PROCESS	4

#define FASTRPC_GLINK_GUID		"fastrpcglink-apps-dsp"

#define FASTRPC_MSG_MAX			256

#define VIRTIO_FASTRPC_CMD_OPEN			1
#define VIRTIO_FASTRPC_CMD_CLOSE		2
#define VIRTIO_FASTRPC_CMD_INVOKE		3  /* used in legacy virtio fastrpc */
#define VIRTIO_FASTRPC_CMD_MMAP			4  /* used in legacy virtio fastrpc */
#define VIRTIO_FASTRPC_CMD_MUNMAP		5  /* used in legacy virtio fastrpc */
#define VIRTIO_FASTRPC_CMD_CONTROL		6  /* used in legacy virtio fastrpc */
#define VIRTIO_FASTRPC_CMD_GET_DSP_INFO		7
#define VIRTIO_FASTRPC_CMD_MUNMAP_FD		8  /* used in legacy virtio fastrpc */
#define VIRTIO_FASTRPC_CMD_MEM_MAP		9  /* used in legacy virtio fastrpc */
#define VIRTIO_FASTRPC_CMD_MEM_UNMAP		10 /* used in legacy virtio fastrpc */
#define VIRTIO_FASTRPC_CMD_SMMU_MAP		11
#define VIRTIO_FASTRPC_CMD_SMMU_UNMAP		12

#define FASTRPC_CPUINFO_DEFAULT		0
#define FASTRPC_CPUINFO_EARLY_WAKEUP	1

#define DSP_UNSUPPORTED_API		0x80000414
#define FASTRPC_MAX_DSP_ATTRIBUTES	256
#define FASTRPC_MAX_DSP_ATTRIBUTES_LEN	(sizeof(u32) * FASTRPC_MAX_DSP_ATTRIBUTES)

/* Add memory to static PD pool, protection thru XPU */
#define ADSP_MMAP_HEAP_ADDR		4
/* MAP static DMA buffer on DSP User PD */
#define ADSP_MMAP_DMA_BUFFER		6
/* Add memory to static PD pool protection thru hypervisor */
#define ADSP_MMAP_REMOTE_HEAP_ADDR	8
/* Add memory to userPD pool, for user heap */
#define ADSP_MMAP_ADD_PAGES		0x1000
/* Add memory to userPD pool, for LLC heap */
#define ADSP_MMAP_ADD_PAGES_LLC		0x3000
/* Map persistent header buffer on DSP */
#define ADSP_MMAP_PERSIST_HDR		0x4000

#define FASTRPC_DSPSIGNAL_TIMEOUT_NONE	0xffffffff
#define FASTRPC_DSPSIGNAL_NUM_SIGNALS	1024
#define FASTRPC_DSPSIGNAL_GROUP_SIZE	256

#define STATIC_PD			0
#define DYNAMIC_PD			1
#define GUEST_OS			2

#define DEFAULT_UNUSED			0
#define ROOT_PD				1
#define AUDIO_STATICPD			2
#define SENSORS_STATICPD		3
#define SECURE_STATICPD			4
#define OIS_STATICPD			5
#define CPZ_USERPD			6
#define USERPD				7
#define GUEST_OS_SHARED			8
#define USER_UNSIGNEDPD_POOL		9
#define MAX_PD_TYPE			10

/* set for cached mapping */
#define FASTRPC_MAP_ATTR_CACHED		1

/* set for multiple level SGT */
#define FASTRPC_MAP_ATTR_MULTI_LEVEL_SGT	(1U << 1) /* 1: Multiple level sglist, 0: One level sglist */

/* Fastrpc attribute  for already mapped buffer */
#define FASTRPC_MAP_ATTR_BUFFER_MAPPED (128)

#define RPC_ERR(format, args...) \
	pr_err("fastrpc (%d): %s: " format, current->pid,\
	__func__, ##args)
#define RPC_INFO(format, args...) \
	pr_info("fastrpc (%d): %s: " format, current->pid,\
	__func__, ##args)
#define RPC_WARN(format, args...) \
	pr_warn("fastrpc (%d): %s: " format, current->pid,\
	__func__, ##args)
#define RPC_DBG(format, args...) \
	pr_debug("fastrpc (%d): %s: " format, current->pid,\
	__func__, ##args)
#define DSPSIGNAL_VERBOSE(format, args...)

enum fastrpc_process_state {
	/* Default state */
	DEFAULT_PROC_STATE = 0,
	/*
	 * Process create on DSP initiated.
	 * This state not being used at present.
	 */
	DSP_CREATE_START,
	/* Process create on DSP complete */
	DSP_CREATE_COMPLETE,
	/* Process exit on DSP initiated */
	DSP_EXIT_START,
	/* Process exit on DSP complete */
	DSP_EXIT_COMPLETE,
};

enum fastrpc_response_flags {
	NORMAL_RESPONSE = 0,
	EARLY_RESPONSE = 1,
	USER_EARLY_SIGNAL = 2,
	COMPLETE_SIGNAL = 3,
	STATUS_RESPONSE = 4,
	POLL_MODE = 5,
};

enum fastrpc_dspsignal_state {
	DSPSIGNAL_STATE_UNUSED = 0,
	DSPSIGNAL_STATE_PENDING,
	DSPSIGNAL_STATE_SIGNALED,
	DSPSIGNAL_STATE_CANCELED,
};

struct fastrpc_internal_dspsignal {
	u32 req;
	u32 signal_id;
	union {
		u32 flags;
		u32 timeout_usec;
	};
};

struct fastrpc_dspsignal {
	struct completion comp;
	int state;
};

struct fastrpc_phy_page {
	u64 addr;		/* physical address */
	u64 size;		/* size of contiguous region */
};

struct fastrpc_invoke_buf {
	u32 num;		/* number of contiguous regions */
	u32 pgidx;		/* index to start of contiguous region */
};

struct fastrpc_remote_dmahandle {
	s32 fd;		/* dma handle fd */
	u32 offset;	/* dma handle offset */
	u32 len;	/* dma handle length */
};

struct fastrpc_remote_buf {
	u64 pv;		/* buffer pointer */
	u64 len;	/* length of buffer */
};

union fastrpc_remote_arg {
	struct fastrpc_remote_buf buf;
	struct fastrpc_remote_dmahandle dma;
};

struct fastrpc_notif_queue {
	/* Number of pending status notifications in queue */
	atomic_t notif_queue_count;
	/* Wait queue to synchronize notifier thread and response */
	wait_queue_head_t notif_wait_queue;
	/* IRQ safe spin lock for protecting notif queue */
	spinlock_t nqlock;
};

struct fastrpc_msg {
	int pid;	/* process group id */
	int tid;	/* thread id */
	u64 ctx;	/* invoke caller context */
	u32 handle;	/* handle to invoke */
	u32 sc;		/* scalars structure describing the data */
	u64 addr;	/* physical address */
	u64 size;	/* size of contiguous region */
};

struct fastrpc_invoke_rsp {
	u64 ctx;	/* invoke caller context */
	int retval;	/* invoke return value */
};

struct fastrpc_invoke_rspv2 {
	u64 ctx;		/* invoke caller context */
	int retval;		/* invoke return value */
	u32 flags;		/* early response flags */
	u32 early_wake_time;	/* user hint in us */
	u32 version;		/* version number */
};

struct fastrpc_buf_overlap {
	u64 start;
	u64 end;
	int raix;
	u64 mstart;
	u64 mend;
	u64 offset;
};

struct fastrpc_perf {
	u64 count;
	u64 flush;
	u64 map;
	u64 copy;
	u64 link;
	u64 getargs;
	u64 putargs;
	u64 invargs;
	u64 invoke;
	u64 tid;
};

struct fastrpc_invoke_ctx {
	struct list_head node;
	int nscalars;
	int nbufs;
	int retval;
	int pid;
	int tgid;
	u32 sc;
	u32 handle;
	u32 *crc;
	u32 early_wake_time;
	u64 *perf_kernel;
	u64 *perf_dsp;
	u64 ctxid;
	u64 msg_sz;
	bool is_work_done;
	enum fastrpc_response_flags rsp_flags;
	struct kref refcount;
	struct completion work;
        // struct work_struct put_work;
	struct fastrpc_msg msg;
	struct fastrpc_user *fl;
        union fastrpc_remote_arg *rpra;
	struct fastrpc_map **maps;
	struct fastrpc_buf *buf;
	struct fastrpc_invoke_args *args;
	struct fastrpc_buf_overlap *olaps;
	struct fastrpc_channel_ctx *cctx;
	struct fastrpc_perf *perf;
};

struct fastrpc_channel_ctx {
	struct fastrpc_common *gdriver;
	int domain_id;
	struct rpmsg_device *rpdev;
	struct device *dev;
	spinlock_t lock;
	struct idr ctx_idr;
	struct ida tgid_frpc_ida;
	struct list_head users;
	struct kref refcount;
	bool valid_attributes;
	u32 dsp_attributes[FASTRPC_MAX_DSP_ATTRIBUTES];
	struct fastrpc_device_node *secure_fdevice;
	struct fastrpc_device_node *fdevice;
	bool secure;
	bool unsigned_support;
	u64 dma_mask;
	u64 cpuinfo_todsp;
	int max_sess_per_proc;
	atomic_t teardown;
	u64 jobid;
	atomic_t invoke_cnt;
};

struct fastrpc_device_node {
	struct fastrpc_channel_ctx *cctx;
	struct miscdevice miscdev;
	bool secure;
};

struct fastrpc_user {
	struct list_head user;
	struct list_head maps;
	struct list_head pending;
	struct list_head interrupted;
	struct list_head mmaps;
	struct list_head cached_bufs;

	struct fastrpc_channel_ctx *cctx;
	struct fastrpc_buf *pers_hdr_buf;
	struct fastrpc_buf *hdr_bufs;
#ifdef CONFIG_DEBUG_FS
	bool debugfs_file_create;
	struct dentry *debugfs_file;
	char *debugfs_buf;
#endif
	int tgid;
	int tgid_frpc;
	u32 pd_type;
	int upid;
	int cid;
	u32 num_cached_buf;
	u32 num_pers_hdrs;
	u32 profile;
	int sessionid;
	u32 poll_timeout;
	bool is_secure_dev;
	bool poll_mode;
	bool is_unsigned_pd;
	spinlock_t lock;
	spinlock_t dspsignals_lock;
	struct mutex signal_create_mutex;
	struct fastrpc_dspsignal *signal_groups[FASTRPC_DSPSIGNAL_NUM_SIGNALS / FASTRPC_DSPSIGNAL_GROUP_SIZE];
	struct mutex remote_map_mutex;
	struct mutex map_mutex;
	struct fastrpc_notif_queue proc_state_notif;
	struct list_head notif_queue;
	bool multi_session_support;
	bool untrusted_process;
	bool set_session_info;
	enum fastrpc_process_state state;
};

struct virt_fastrpc_msg {
	struct completion work;
	struct fastrpc_invoke_ctx *ctx;
	u16 msgid;
	void *txbuf;
	void *rxbuf;
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

struct virt_fastrpc_vq {
	/* protects vq */
	spinlock_t vq_lock;
	struct virtqueue *vq;
};


/* Struct to hold globally used variables */
struct fastrpc_common {
	struct virtio_device *vdev;
	struct device *dev;
	struct virt_fastrpc_vq rvq;
	struct virt_fastrpc_vq svq;
	void **rbufs;
	void **sbufs;
	unsigned int num_bufs;
	unsigned int order;
	unsigned int buf_size;
	unsigned int num_channels;
	int last_sbuf;

	spinlock_t msglock;
	struct virt_fastrpc_msg *msgtable[FASTRPC_MSG_MAX];

	/* global lock  to access channel context */
	spinlock_t glock;

	/* global copy of channel contexts */
	struct fastrpc_channel_ctx *gctx[FASTRPC_DEV_MAX];

#ifdef CONFIG_DEBUG_FS
	struct dentry *debugfs_root;
	struct dentry *debugfs_global_file;
#endif
};

static const char *domains[FASTRPC_DEV_MAX] = { "adsp", "mdsp",
						"sdsp", "cdsp"};

int fastrpc_transport_send(struct fastrpc_channel_ctx *cctx,
		void *rpc_msg, uint32_t rpc_msg_size);
int fastrpc_transport_init(void);
void fastrpc_transport_deinit(void);
int fastrpc_handle_rpc_response(struct fastrpc_channel_ctx *cctx,
		void *data, int len);
struct fastrpc_channel_ctx* get_current_channel_ctx(struct device *dev);
void fastrpc_update_gctx(struct fastrpc_channel_ctx *cctx, int flag);
void fastrpc_notify_users(struct fastrpc_user *user);
long fastrpc_device_ioctl(struct file *file, unsigned int cmd,
		unsigned long arg);
#endif /*__FASTRPC_COMMON_H__*/
