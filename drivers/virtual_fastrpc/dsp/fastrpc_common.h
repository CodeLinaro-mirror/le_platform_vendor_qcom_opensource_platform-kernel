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
#include <linux/kobject.h>
#include <linux/hashtable.h>
#include "fastrpc.h"
#if IS_ENABLED(CONFIG_HYBRID_FASTRPC_RSM)
#include "virtio_compressched_client.h"
#endif

#define ADSP_DOMAIN_ID			0
#define MDSP_DOMAIN_ID			1
#define SDSP_DOMAIN_ID			2
#define CDSP_DOMAIN_ID			3
#define CDSP1_DOMAIN_ID			4
#define NUM_LEGACY_ID_MAX		5 /* adsp, mdsp, slpi, cdsp, cdsp1 */
#define FASTRPC_DEV_MAX		    7 /* Maximum number of devices for Gen5 Nord as of now */
#define FASTRPC_MAX_SESSIONS		14
#define FASTRPC_MAX_SESSIONS_PER_PROCESS	4

/* Check if given session id is valid */
#define IS_VALID_SESSION_ID(sess) (sess < FASTRPC_MAX_SESSIONS_PER_PROCESS)

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
#define VIRTIO_FASTRPC_CMD_MDCTX_MANAGE		13

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

#define SMMU_1M				0x100000ULL
#define SMMU_2M				0x200000ULL
#define SMMU_1G				0x40000000ULL
/* Check if the given flag is used for extended UDMA mapping */
#define IS_EXTENDED_MAP_FLAG(flag) \
	(flag == FASTRPC_MAP_FD_EXTENDED || \
	 flag == FASTRPC_MAP_FD_DELAYED_EXTENDED)

/* set for cached mapping */
#define FASTRPC_MAP_ATTR_CACHED		1

/* set for multiple level SGT */
#define FASTRPC_MAP_ATTR_MULTI_LEVEL_SGT	(1U << 1) /* 1: Multiple level sglist, 0: One level sglist */
/* set for extended map */
#define FASTRPC_MAP_ATTR_EXTENDED		(1U << 2) /* 1: uDMA64 on extended CB, 0: regular CB */
/* Fastrpc attribute  for already mapped buffer */
#define FASTRPC_MAP_ATTR_BUFFER_MAPPED (128)

#define FASTRPC_DEVICE_NAME     "fastrpc"

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

/* Check if given domain id is valid */
#define IS_LEGACY_DOMAIN_ID(domain) (domain < NUM_LEGACY_ID_MAX)

/* Max length of domain name */
#define MAX_DOMAIN_NAMELEN 30

/* DSP status macros */
#define DSP_STATUS_UP true
#define DSP_STATUS_DOWN false

/*
 * Generates a physical ID for a DSP (Digital Signal Processor) device.
 *
 * The resulting physical ID is a composite value consisting of:
 *   Type identifier multiplied by 1000, plus the instance identifier
 *
 * @param type        : Type identifier for the DSP device
 * @param instance_id : Instance identifier for the DSP device
 *
 * @return The generated physical ID for the DSP device
 */
#define GENERATE_DSP_PHYSICAL_ID(type, instance_id) \
	((type * 1000) + instance_id)

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

enum fastrpc_rsm_node_type {
    FASTRPC_RSM_SIGNAL_CORE = 0,
    FASTRPC_RSM_MULTI_CORE,
    FASTRPC_RSM_TYPE_NUM
};

struct fastrpc_internal_dspsignal {
	u32 req;
	u32 signal_id;
	union {
		u32 flags;
		u32 timeout_usec;
	};
};

struct fastrpc_internal_dspsignal_mc {
	u32 req;
	u32 signal_id;
	union {
		u32 flags;
		u32 timeout_usec;
	};
	u64 ctx;
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

struct fastrpc_domain;

/*
 * struct fastrpc_channel_ctx - Per-rpmsg-channel state.
 *
 * Allocated by fastrpc_rpmsg_probe() (kzalloc) with @refcount
 * initialised to 1 for the rpmsg driver. Each fastrpc_user takes
 * an extra ref via fastrpc_channel_ctx_get() at open and drops it
 * via fastrpc_channel_ctx_put() at release; fastrpc_rpmsg_remove()
 * drops the rpmsg driver's ref. Freed by fastrpc_channel_ctx_free()
 * (the kref release callback) when the last ref goes away.
 */
struct fastrpc_channel_ctx {
	struct fastrpc_common *gdriver;
	int domain_id;
	/* Cached domain->type, populated together with domain_id at bind
	 * time. Lets readers access the DSP type via fl->cctx directly
	 * so cctx->domain can be torn down at SSR without racing them.
	 */
	enum fastrpc_dsp_type domain_type;
/* Structure holding info on domain associated with channel */
	struct fastrpc_domain *domain;
	struct rpmsg_device *rpdev;
	struct device *dev;
	spinlock_t lock;
	struct idr ctx_idr;
	struct ida tgid_frpc_ida;
	struct list_head users;
	struct kref refcount;
	bool valid_attributes;
	u32 dsp_attributes[FASTRPC_MAX_DSP_ATTRIBUTES];
	/* Channel sysfs object */
	struct kobject kobj_sysfs;
	/* Flag to indicate if sysfs node has been created for channel */
	bool sys_fs_init;
	struct fastrpc_device_node *fdevice;
	/* Non secure device node using legacy device name */
	struct fastrpc_device_node *legacy_fdevice;
	/* Secure device node using legacy device name */
	struct fastrpc_device_node *legacy_secure_fdevice;
	bool secure;
	bool unsigned_support;
	u64 dma_mask;
	u64 cpuinfo_todsp;
	int max_sess_per_proc;
	atomic_t teardown;
	u64 jobid;
	atomic_t invoke_cnt;
};

/*
 * struct fastrpc_domain - Description of a DSP domain. No refcount.
 *
 * Non-discovery mode:
 *   Allocated in fastrpc_rpmsg_probe() (kzalloc) and owned by the
 *   bound cctx. Freed by fastrpc_rpmsg_remove().
 *
 * Discovery mode (is_device_discovery_supported() == true):
 *   Populated from device-tree into a global hash-table keyed by
 *   @phy_id; survives across SSR / rpmsg_remove / rpmsg_probe.
 *   fastrpc_rpmsg_remove() only clears the @cctx back-pointer.
 */
struct fastrpc_domain {
	/* Node for adding to global domains hash-table */
	struct hlist_node node;
	/* Logical domain ID returned to users */
	u32 id;
	/* Name of the dsp domain */
	char name[MAX_DOMAIN_NAMELEN];
	/* Flag to indicate domain up or down */
	bool status;
	/*
	 * Flag to indicate if configured as legacy node which is applicable
	 * for NSPs with instance id 0 and 1
	 */
	bool legacy;
	/* Instance ID configured in dtsi */
	u32 instance_id;
	/* Unique physical ID - the key for the kernel hash-table */
	u32 phy_id;
	/* Type of DSP */
	enum fastrpc_dsp_type type;
	/*
	 * Legacy name - This will be assigned to the dsp with the instance id '0'
	 * for types LPASS, SDSP
	 * for NSP, instance id '0' would be assigned legacy name 'cdsp'
	 *          instance id '1' would be assigned legacy name 'cdsp1'
	 * This will be used to handle all the rpc calls made by clients
	 * using old legacy domain names
	 */
	char *legacy_name;
	/*
	 * Legacy id - This will be assigned to the dsp with the instance id '0'
	 * for types LPASS, SDSP
	 * for NSP, instance id '0' would be assigned CDSP_DOMAIN_ID
	 *          instance id '1' would be assigned CDSP1_DOMAIN_ID
	 * This will be used to handle all the rpc calls made by clients
	 * using old legacy domain ids
	 */
	u32 legacy_id;
	/* Channel context for domain */
	struct fastrpc_channel_ctx *cctx;
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
	/*
	 * List of multidomain contexts created using this user,
	 * only the first session of a multi-domain context will
	 * book keep it.
	 */
	struct list_head mdctxs;

	struct fastrpc_channel_ctx *cctx;
	struct fastrpc_buf *pers_hdr_buf;
	struct fastrpc_buf *hdr_bufs;
#ifdef CONFIG_DEBUG_FS
	bool debugfs_file_create;
	struct dentry *debugfs_file;
	char *debugfs_buf;
#endif

#if IS_ENABLED(CONFIG_HYBRID_FASTRPC_RSM)
	/* mutex used to protect the following list */
	struct mutex rsm_list_mutex;
	/*
	 * A per session/pd list structure used in hybrid fastrpc to store resources
	 * associated with registered RSM/compressched handle instance.
	 */
	struct hlist_head rsm_list_per_session;
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

/*
 * Struct to describe a multi-domain context, it could be either
 * multi-core or multi-session.
 */
struct fastrpc_mdctx_info {
	/* Node to add to process multidomain context list */
	struct list_head node;
	/* List of logcal domain ids  on which context was created */
	uint32_t *domains;
	/* List of session ids on each domain */
	uint32_t *session_ids;
	/*
	 * List of channel id returned by virt_fastrpc_open,
	 * which is composed of logical id and client id.
	 */
	int32_t *cids;
	/* Number of domains */
	uint32_t num_domains;
	/* User-obj using which context was created */
	struct fastrpc_user *fl;
	/* User-objs of all domains in this multi-domain */
	struct fastrpc_user **fls;
	/* List of upids on each domain */
	uint32_t *upids;
	/* Kernel generated context id */
	uint64_t ctx;
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

#if IS_ENABLED(CONFIG_HYBRID_FASTRPC_RSM)
struct vfastrpc_rsm_entry {
	struct hlist_node hn;
	struct kref refcount;
	atomic_t dspqueue_req_cnt;
	atomic_t dspqueue_rsp_cnt;
	enum fastrpc_rsm_node_type type;
	/*
	 * thread id or unique fastrpc pid (upid)
	 * In normal invoke case, it will be thread id (gotten
	 * from current->pid)
	 * In dspqueue case, it will be unique fastrpc pid (used
	 * by DSP side to identify different PD/session)
	 */
	u32 target_id;
	/*
	 * In normal invoke case, the compressched handle registered for this thread
	 * and the registration occurs before before this thread starts offloading
	 * computation task to dsp through invoke.
	 * In dspqueue case, the compressched handle registered for this session
	 * registration occurs when HLOS side fastRPC begins signaling dsp to
	 * read the data written by fastrpc client through dspqueue APIs.
	 * In all cases, unregistration occurs in session/PD exit
	 */
	compressched_handle handle;
	/*
	 * response returned for resource acquire by calling compressched_acquire
	 * which will be used to call compressched_release_v2
	 */
	compressched_acquire_rsp_v2 response;
	compressched_register_msg reg_msg;
};
#endif

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
	/*
	 * use spin lock to protect global resources that are also accessed
	 * in interrupt context
	 */
	spinlock_t glock;

	/* Mutex to protect access of global domains hash tables */
	struct mutex hmut;

	/*
	 * Declare a hash table to store fastrpc domains.
	 * The hash table is used to efficiently manage and look up fastrpc domains.
	 */
	DECLARE_HASHTABLE(fastrpc_domains_table, FASTRPC_DEV_MAX);

	/*
	 * use mutex to protect global resources that will never be accessed
	 * in interrupt context
	 */
	struct mutex gmut;

#ifdef CONFIG_DEBUG_FS
	struct dentry *debugfs_root;
	struct dentry *debugfs_global_file;
#endif
};

/* Legacy domain names of DSP */
static const char *legacy_domains[NUM_LEGACY_ID_MAX] =
{
	"adsp",
	"mdsp",
	"sdsp",
	"cdsp",
	"cdsp1"
};

/* DSP labels defined in device tree, we support only nsp and hpass in Auto */
static const char *fastrpc_dsp_type_labels[FASTRPC_MAX_DSP_TYPE] =
{
	NULL,
	"nsp",
	"lpass",
	"sdsp",
	"mdsp",
	"hpass"
};

int fastrpc_transport_send(struct fastrpc_channel_ctx *cctx,
		void *rpc_msg, uint32_t rpc_msg_size);
int fastrpc_transport_init(void);
void fastrpc_transport_deinit(void);
int fastrpc_handle_rpc_response(struct fastrpc_channel_ctx *cctx,
		void *data, int len);
struct fastrpc_channel_ctx* get_current_channel_ctx(struct device *dev);
void fastrpc_update_gdriver(struct fastrpc_channel_ctx *cctx, int flag);
void fastrpc_notify_users(struct fastrpc_user *user);
long fastrpc_device_ioctl(struct file *file, unsigned int cmd,
		unsigned long arg);
int fastrpc_convert_legacy_id_to_logical_id(u32 legacy_id,
		u32 *logical_id);
bool is_device_discovery_supported(void);
bool fastrpc_domain_needs_rsm(u32 logical_id);

/*
 * Creates a sysfs interface for the given fastrpc channel context.
 *
 * @param cctx The fastrpc channel context to create the sysfs interface for.
 *
 * @return 0 on success, a negative error code on failure.
 */
int fastrpc_sysfs_domain_create(struct fastrpc_channel_ctx *cctx);

/*
 * Removes sysfs directory of a channel.
 *
 * This function is responsible for deleting the sysfs directory
 * associated with a specific channel context.
 * It takes a pointer to the channel context as an argument.
 *
 * @param cctx Pointer to the channel context to remove sysfs directory
 */
void fastrpc_sysfs_domain_remove(struct fastrpc_channel_ctx *cctx);

/*
 * fastrpc_lookup_domain_in_table() -
 * Looks up a domain in the in the fastrpc domains hash-table using either
 * physical id or logical domain id based on the flag.
 *
 * @param key          : physical id / logical domain id to lookup in table
 * @param use_phy_id   : Flag to indicate whether to lookup using phy id
 *                       or logical id.
 *
 * @return Pointer to the matching domain structure, or NULL if not found.
 */
struct fastrpc_domain *fastrpc_lookup_domain_in_table(u32 key,
	bool use_phy_id);

/*
 * Populate fastrpc_domain from device tree node.
 *
 * @param rdev   Device structure to extract info from.
 * @param domain Pointer to fastrpc_domain pointer to be populated.
 *
 * @return 0 on success, negative error code on failure.
 */
int fastrpc_populate_domain_from_dt(struct device *rdev,
	struct fastrpc_domain **domain);

/*
 * fastrpc_sysfs_register_kset - Register the fastrpc kset
 *
 * Creates a kset to create a parent directory "fastrpc" under /sys/kernel.
 *
 * Return: 0 on success, -ENOMEM on failure
 */
int fastrpc_sysfs_register_kset(void);

/*
 * fastrpc_sysfs_deregister_kset - Deregister the fastrpc kset from sysfs
 *
 * This function deregisters the fastrpc kset from the sysfs file system.
 *
 * @return: None
 */
void fastrpc_sysfs_deregister_kset(void);

#endif /*__FASTRPC_COMMON_H__*/
