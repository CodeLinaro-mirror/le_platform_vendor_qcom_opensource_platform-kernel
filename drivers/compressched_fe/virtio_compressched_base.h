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
};

typedef struct {
    unsigned int upid; //unique pid sent to dsp
    unsigned int tid; // thread id sent to dsp
}compressched_register_tx;

typedef struct {
    compressched_handle handle; // COMPRESSCHED handle
    char* job_name;
}compressched_acquire_tx;

typedef struct {
    compressched_handle handle;
    compressched_token token; // COMPRESSCHED handle
}compressched_release_tx;

typedef struct {
    compressched_handle handle; // COMPRESSCHED handle
}compressched_unregister_tx;

typedef struct {
    compressched_handle handle;
    unsigned int err; //handle is returned to client on successful register
}compressched_rx;

union compressched_txcmd_data{
    compressched_register_tx register_data;
    compressched_acquire_tx acquire_data;
    compressched_release_tx release_data;
    compressched_unregister_tx unregister_data;
};

struct virtio_compressched_txbuf {
    unsigned int msg_id;
    enum compressched_cmd cmd;
    union compressched_txcmd_data send_data;
};
struct virtio_compressched_rxbuf {
    unsigned int msg_id;
    enum compressched_cmd cmd;
    compressched_acquire_rsp_v2 acq_rsp;
    compressched_rx return_val;
};

struct compressched_client_table {
    unsigned int upid;
    unsigned int tid;
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
};

int virt_compressched_txbuf(struct virtio_compressched_txbuf *send_buf);
