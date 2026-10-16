/* SPDX-License-Identifier: GPL-2.0-only
*
*  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. 
*/
#include <linux/virtio.h>
#include <linux/virtio_ids.h>
#include <linux/virtio_config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/debugfs.h>
#include <linux/string.h>
#include "virtio_compressched_client.h"
/* Virtio ID of COMPRESSCHED : 0xC00F */
#define VIRTIO_ID_COMPRESSCHED   0xC00F  //This should match with the VIRTIO_DT_COMPRESSCHED defined at PVM BE
//#define TEST_MODE     1
#define JOB_NAME_LENGTH 64
extern struct virtio_compressched_dev* g_vdevcompressched;

#define LEVEL_DEBUG	1
#define LEVEL_INFO	2
#define LEVEL_ERR	3

static unsigned int log_level = LEVEL_INFO;

static char *prix[] = {"", "debug", "info", "error"};
static void log_compresschedfe(int level, const char *fmt, ...)
{
    va_list args;

    if ((level) >= log_level) {
        va_start(args, fmt);
        vprintk(fmt, args);
        va_end(args);
    }
}
#define LOG_COMPRESSCHEDFE(level, format, args...) \
log_compresschedfe(level, "compresschedfe: pid %.8x: %s: %s(%d) "format, \
current->pid, prix[0x3 & (level)], __func__, __LINE__, ## args)

#define MAX_RX_BUF_SIZE 1024
#define MAX_CLIENT 16
#define DEF_BUFF_SIZE MAX_CLIENT*1024
#define NO_ERROR 0 
#define ERROR -1
enum compressched_cmd{
	COMPRESSCHED_REGISTER = 0,
	COMPRESSCHED_ACQUIRE = 1,
	COMPRESSCHED_RELEASE = 2,
	COMPRESSCHED_UNREGISTER = 3,
	COMPRESSCHED_UNREGISTER_BATCH = 4,
	COMPRESSCHED_REGISTER_EX_LIB = 5,        /* formerly COMPRESSCHED_REGISTER_EX */
	COMPRESSCHED_ACQUIRE_LIB = 6,             /* formerly COMPRESSCHED_ACQUIRE_LEGACY */
	COMPRESSCHED_RELEASE_LIB = 7,
	COMPRESSCHED_UNREGISTER_LIB = 8,
	COMPRESSCHED_QUERY_LIB = 9,               /* formerly COMPRESSCHED_QUERY */
	COMPRESSCHED_TRACING_START_LIB = 10,      /* formerly COMPRESSCHED_TRACING_START */
	COMPRESSCHED_TRACING_STOP_LIB = 11,       /* formerly COMPRESSCHED_TRACING_STOP */
	COMPRESSCHED_DEBUG_CMD_LIB = 12,          /* formerly COMPRESSCHED_DEBUG_CMD */
	COMPRESSCHED_SUBSCRIBE_FAULTS_LIB = 13,   /* formerly COMPRESSCHED_SUBSCRIBE_FAULTS */
	COMPRESSCHED_UNSUBSCRIBE_FAULTS_LIB = 14, /* formerly COMPRESSCHED_UNSUBSCRIBE_FAULTS */
	/* PVM → GVM unsolicited fault notification */
	COMPRESSCHED_FAULT_NOTIFY_LIB = 15,       /* formerly COMPRESSCHED_FAULT_NOTIFY */
	/* DP minimal API – avail-resource acquire/release */
	COMPRESSCHED_AVAIL_RES_ACQUIRE_LIB = 16,
	COMPRESSCHED_AVAIL_RES_RELEASE_LIB = 17,
	COMPRESSCHED_INVALID_CMD = 18,
};

typedef struct {
  compressched_register_msg register_msg;
}compressched_register_tx;

typedef struct {
  compressched_register_ex_msg register_ex_msg;
}compressched_register_ex_tx;

typedef struct {
    compressched_handle handle;
    compressched_token token; // COMPRESSCHED handle
}compressched_release_tx;

typedef struct {
    compressched_handle handle; // COMPRESSCHED handle
}compressched_unregister_tx;

typedef struct {
    compressched_handle handle; /* client handle for the query */
}compressched_query_tx;

typedef struct {
    compressched_handle handle;
    unsigned int err; //handle is returned to client on successful register
}compressched_rx;

/* ---- New TX payloads (GVM → PVM) --------------------------------- */

typedef struct {
    compressched_handle handle;
    unsigned int        num_logs; /* 0 = unlimited */
} compressched_tracing_start_tx;

typedef struct {
    compressched_handle handle;
} compressched_tracing_stop_tx;

/* compressched_debug_cmd, compressched_subscribe_faults,
 * compressched_domain_mask, and compressched_unsubscribe_faults_tx are
 * defined in virtio_compressched_client.h (included above) to break the
 * circular-include dependency. */

/* ---- Fault notification RX payload (PVM → GVM) ------------------- */

typedef struct {
    unsigned char fault_type; /* compressched_faults enum value */
    unsigned char pad[3];
    int           domain_id;
    int           sub_res_id;
    int           fault_queue; /* compressched_queue_priority */
} compressched_fault_notify_rx;

/* ====================================================================
 * DP (Dynamic Partition) wire-format types
 *
 * These types define the GVM↔PVM virtio wire format for the
 * COMPRESSCHED_AVAIL_RES_ACQUIRE_LIB and COMPRESSCHED_AVAIL_RES_RELEASE_LIB
 * commands.  Their layouts MUST remain byte-for-byte identical to the
 * PVM-side structs in vhu_compressched_msg.h:
 *   - compressched_avail_res_acquire_rx  (GVM TX / PVM RX)
 *   - compressched_avail_res_release_rx  (GVM TX / PVM RX)
 *   - compressched_avail_res_result_t    (PVM TX / GVM RX)
 * ==================================================================== */

/*
 * compressched_dp_cmd_t
 *
 * Flat kernel representation of compressched_acquire_available_resource_cmd
 * (compressched_client.h, 248 bytes).
 *
 * The base struct has an IMPLICIT 4-byte alignment gap between
 * resource_count (offset 20) and duration_us (offset 24).  That gap is
 * made EXPLICIT here as pad0 so the layout is unambiguous on all
 * architectures.
 *
 * sizeof(compressched_dp_cmd_t) MUST equal 248.
 */
typedef struct {
	unsigned int  handle;                  /* offset   0 */
	int           priority;                /* offset   4 */
	int           select_mode;             /* offset   8 */
	int           strict_mode;             /* offset  12 */
	unsigned int  resource_count;          /* offset  16 */
	unsigned int  pad0;  /* explicit pad — mirrors implicit gap at offset 20 */
	long long     duration_us;             /* offset  24 */
	long long     timeout_us;              /* offset  32 */
	char          job_name[64];            /* offset  40 */
	int           requested_domain_id[4];  /* offset 104 */
	unsigned char reserved[128];           /* offset 120 */
} compressched_dp_cmd_t;                   /* total: 248 */

/*
 * compressched_dp_param_t
 *
 * Flat kernel representation of compressched_avail_res_param_t
 * (compressched_client_dp_min.h, 156 bytes).
 *
 * sizeof(compressched_dp_param_t) MUST equal 156.
 */
typedef struct {
	int           family;                  /* offset  0 */
	int           selection;               /* offset  4 */
	unsigned int  resource_count;          /* offset  8 */
	int           requested_domain_id[4];  /* offset 12 */
	unsigned char reserved[128];           /* offset 28 */
} compressched_dp_param_t;                 /* total: 156 */

/*
 * compressched_dp_result_t
 *
 * Flat kernel representation of compressched_avail_res_result_t
 * (compressched_client_dp_min.h, 152 bytes).
 *
 * sizeof(compressched_dp_result_t) MUST equal 152.
 */
typedef struct {
	unsigned int  token;                   /* offset  0 */
	unsigned int  available_count;         /* offset  4 */
	int           available_domain_id[4];  /* offset  8 */
	unsigned char reserved[128];           /* offset 24 */
} compressched_dp_result_t;                /* total: 152 */

/*
 * compressched_avail_res_acquire_tx  (GVM → PVM)
 *
 * Layout MUST match compressched_avail_res_acquire_rx in
 * vhu_compressched_msg.h (PVM side).
 */
typedef struct {
	compressched_dp_cmd_t   cmd;       /* the blob (248 bytes) */
	compressched_dp_param_t param;     /* optional override (156 bytes) */
	unsigned char           has_param; /* 1 = param valid, 0 = NULL */
	unsigned char           pad[3];
} compressched_avail_res_acquire_tx;   /* total: 408 bytes */

/*
 * compressched_avail_res_release_tx  (GVM → PVM)
 *
 * Layout MUST match compressched_avail_res_release_rx in
 * vhu_compressched_msg.h (PVM side).
 */
typedef struct {
	compressched_dp_cmd_t cmd;   /* blob (handle extraction only, 248 bytes) */
	unsigned int          token; /* token from prior acquire */
} compressched_avail_res_release_tx;   /* total: 252 bytes */

/* ---- TX command data union --------------------------------------- */

union compressched_txcmd_data{
    compressched_register_tx             register_data;
    compressched_register_ex_tx          register_ex_data;
    compressched_acquire_tx              acquire_nsp_share_data;
    compressched_release_tx              release_data;
    compressched_unregister_tx           unregister_data;
    compressched_acquire_legacy_tx       acquire_data_lagacy;
    compressched_query_tx                query_data;
    compressched_tracing_start_tx        tracing_start_data;
    compressched_tracing_stop_tx         tracing_stop_data;
    compressched_debug_cmd               debug_cmd_data;
    compressched_subscribe_faults        subscribe_faults_data;
    compressched_unsubscribe_faults_tx   unsubscribe_faults_data;
    /* DP minimal API */
    compressched_avail_res_acquire_tx    avail_res_acquire_data;
    compressched_avail_res_release_tx    avail_res_release_data;
};

struct virtio_compressched_txbuf {
    unsigned int msg_id;
    enum compressched_cmd cmd;
    union compressched_txcmd_data send_data;
};

struct virtio_compressched_rxbuf {
    unsigned int msg_id;
    enum compressched_cmd cmd;
    //union {
        compressched_acquire_rsp_v2  acq_rsp;
        compressched_platform_query  query_rsp;
    //};
    compressched_rx              return_val;
    /* Fault notification payload — valid only when cmd == COMPRESSCHED_FAULT_NOTIFY_LIB */
    compressched_fault_notify_rx fault_notify;
    /* DP acquire result — valid only when cmd == COMPRESSCHED_AVAIL_RES_ACQUIRE_LIB */
    compressched_dp_result_t     avail_res_result;
};

struct compressched_client_table {
    unsigned int upid[MAX_NUM_OF_NSP];
    unsigned int target_id;
    compressched_handle handle;
    struct completion work;
    struct virtio_compressched_rxbuf rxbuf;
};
/* device private data (one per device) */
struct virtio_compressched_dev {
    struct virtio_device *vdev;
    struct device *dev;
    struct virtqueue *vq_tx;
    spinlock_t vqtx_lock;
    struct virtqueue *vq_rx;
    spinlock_t vqrx_lock;
    void **txbufs;
    void **rxbufs;
    int num_buf;
    unsigned int order;
    struct compressched_client_table client_list[MAX_CLIENT];
    spinlock_t vq_clientlock;
    int txBufUsedCount;
    /*
     * O(1) handle → client_list index map.
     * Slot handle_ht[handle % MAX_CLIENT] stores the client_list index
     * (0..MAX_CLIENT-1) for that handle, or MAX_CLIENT when empty.
     * Protected by vq_clientlock.
     */
    int handle_ht[MAX_CLIENT];
    /* Fault notification slot — written by virtio_compressched_recv_cb when
     * msg_id == MAX_CLIENT (COMPRESSCHED_FAULT_NOTIFY), read by the
     * COMPRESSCHED_IOCTL_WAIT_FAULT ioctl handler. */
    compressched_fault_notify_rx fault_pending;
    struct completion            fault_work;
    spinlock_t                   fault_lock;
};

int virt_compressched_txbuf(struct virtio_compressched_txbuf *send_buf);

/* DP minimal API — acquire/release available resource (GVM → PVM). */
int compressched_avail_res_acquire(compressched_handle handle,
				   const compressched_dp_cmd_t *cmd,
				   const compressched_dp_param_t *param,
				   compressched_dp_result_t *result);
int compressched_avail_res_release(compressched_handle handle,
				   const compressched_dp_cmd_t *cmd,
				   unsigned int token);