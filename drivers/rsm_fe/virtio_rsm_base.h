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
#include "virtio_rsm_client.h"
/* Virtio ID of RSM : 0xC00E */
#define VIRTIO_ID_RSM   0xC00E
//#define TEST_MODE     1

extern struct virtio_rsm_dev* g_vdevrsm;

#define LEVEL_DEBUG	1
#define LEVEL_INFO	2
#define LEVEL_ERR	3

static unsigned int log_level = LEVEL_INFO;

static char *prix[] = {"", "debug", "info", "error"};
static void log_rsmfe(int level, const char *fmt, ...)
{
    va_list args;

    if ((level) >= log_level) {
        va_start(args, fmt);
        vprintk(fmt, args);
        va_end(args);
    }
}
#define LOG_RSMFE(level, format, args...) \
log_rsmfe(level, "rsmfe: pid %.8x: %s: %s(%d) "format, \
current->pid, prix[0x3 & (level)], __func__, __LINE__, ## args)

#define MAX_RX_BUF_SIZE 1024
#define MAX_CLIENT 16
#define DEF_BUFF_SIZE MAX_CLIENT*1024
#define NO_ERROR 0 
#define ERROR -1
enum rsm_cmd{
        RSM_REGISTER = 0,
        RSM_ACQUIRE = 1,
        RSM_RELEASE = 2,
        RSM_UNREGISTER = 3,
        RSM_UNREGISTER_BATCH = 4,
};

typedef struct {
    unsigned int upid; //unique pid sent to dsp
    unsigned int tid; // thread id sent to dsp
}rsm_register_tx;

typedef struct {
    rsm_handle handle; // RSM handle
    char* job_name;
}rsm_acquire_tx;

typedef struct {
    rsm_handle handle;
    rsm_token token; // RSM handle
}rsm_release_tx;

typedef struct {
    rsm_handle handle; // RSM handle
}rsm_unregister_tx;

typedef struct {
    rsm_handle handle;
    unsigned int err; //handle is returned to client on successful register
}rsm_rx;

union rsm_txcmd_data{
    rsm_register_tx register_data;
    rsm_acquire_tx acquire_data;
    rsm_release_tx release_data;
    rsm_unregister_tx unregister_data;
};

struct virtio_rsm_txbuf {
    unsigned int msg_id;
    enum rsm_cmd cmd;
    union rsm_txcmd_data send_data;
};
struct virtio_rsm_rxbuf {
    unsigned int msg_id;
    enum rsm_cmd cmd;
    rsm_acquire_rsp_v2 acq_rsp;
    rsm_rx return_val;
};

struct rsm_client_table {
    unsigned int upid;
    unsigned int tid;
    rsm_handle handle;
    struct completion work;
    struct virtio_rsm_rxbuf rxbuf;
};
/* device private data (one per device) */
struct virtio_rsm_dev {
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
    struct rsm_client_table client_list[MAX_CLIENT]; 
    spinlock_t vq_clientlock;
    int txBufUsedCount;
};

int virt_rsm_txbuf(struct virtio_rsm_txbuf *send_buf);
