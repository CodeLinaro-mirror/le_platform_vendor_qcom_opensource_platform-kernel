/* SPDX-License-Identifier: GPL-2.0-only
*
*  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. 
*/

#define MAX_NUM_OF_NSP  4
#define INVALID_UPID 0xFFFFFFFF
#define INVALID_LOGICAL_ID -1
#define EOK 0
#include <linux/types.h>

typedef unsigned int compressched_handle;
typedef unsigned int compressched_token;
typedef struct {
    compressched_token token;
    unsigned int reserved;
} compressched_acquire_rsp_v2;

typedef struct {
    unsigned char nsp_count;
    unsigned int target_id;
    unsigned int upid[MAX_NUM_OF_NSP];
    int logical_id[MAX_NUM_OF_NSP];
} compressched_register_msg;

typedef struct {
    unsigned int pid;
    unsigned int tid;
} compressched_register_ex_msg;
//#ifdef EN_NSP_SHARING
typedef struct {
    compressched_handle handle; // COMPRESSCHED handle
    char job_name[64];
}compressched_acquire_tx;
//#else

#define NUM_NSP_BASE_GROUP_RESOURCE_MAX   4U
/* Must match PVM's compressched_client.h NUM_BASE_GROUP_RESOURCE_MAX (7U).
 * compressched_platform_query is embedded in virtio_compressched_rxbuf; if
 * this value differs from the PVM the return_val field lands at a different
 * byte offset and the handle returned by registration is read as 0. */
#define NUM_BASE_GROUP_RESOURCE_MAX       7U

typedef enum {
    NSP_SUB_RES_ID0 = 0, //Base Group Request //VTCM:8 MB, HVX:4, HMX:1
    NSP_SUB_RES_ID1 = 1, //VTCM:4 MB, HVX:3, HMX:1
    NSP_SUB_RES_ID2 = 2, //VTCM:4 MB, HVX:1, HMX:0
    NSP_SUB_RES_ID3 = 3, //VTCM:4 MB, HVX:2, HMX:1
    NSP_SUB_RES_ID4 = 4, //VTCM:4 MB, HVX:2, HMX:0
    NSP_SUB_RES_ID5 = 5, //VTCM:4 MB, HVX:1, HMX:1
    NSP_SUB_RES_ID6 = 6  //VTCM:4 MB, HVX:3, HMX:0
} compressched_sub_res_id;

typedef enum {
    COMPRESSCHED_BASE_RESOURCE_REQUEST = 1,
    COMPRESSCHED_SUB_RESOURCE_REQUEST = 2,
    COMPRESSCHED_SUPER_RESOURCE_REQUEST = 3
} compressched_resource_request_type;

typedef struct  {
  compressched_resource_request_type req_type;
  compressched_sub_res_id sub_res_id;
  int requested_domain_id[NUM_NSP_BASE_GROUP_RESOURCE_MAX]; //This field is required for Super Group. Super Group feature is applicable only for NSP
}compressched_resource_details;

/**
  * Queue priority.
  */
typedef enum {
    QUEUE_PRIORITY_DEFAULT = 0,
    /* low priority Queue*/
    QUEUE_PRIORITY_LOW          =  QUEUE_PRIORITY_DEFAULT,
    /* Medium priority Queue*/
    QUEUE_PRIORITY_MEDIUM       = 1,
    /* High priority Queue*/
    QUEUE_PRIORITY_HIGH         = 2,
    /* Realtime priority Queue*/
    QUEUE_PRIORITY_REALTIME     = 3,
    /**< Maximum number of queue priorities. */
    NUM_QUEUE_PRIORITY_MAX
} compressched_queue_priority;

/**
  * Queue type.
  */
typedef enum {
    QUEUE_TYPE_PRIORITY = 0,
      /**< Priority queue type. */
    QUEUE_TYPE_MAX
      /**< Maximum number of queue types. */
} compressched_queue_type;

/**
  * Command for resource acquire.
  */
typedef struct {
    compressched_handle handle;
    /**< Registration handle from compressched_register(). */
    compressched_resource_details res_details;
    compressched_queue_priority priority;
    /**< Queue priority. */
    int64_t duration_us;
    /**< Duration of the resource usage time from acquire to
         release in microseconds.
         - 0 -- The duration watchdog is disabled. */
    int64_t timeout_us;
    /**< Timeout of the resource acquire request in microseconds.
         - 0 -- No timeout and request with unavailable resources
                immediately returns failure. */
    uint64_t reserved;
    /**< Reserved. */
    //char job_name[64];
    /**< Null-terminated job name string (max 63 chars + NUL). */
} compressched_acquire_legacy_tx;


/**
  * Payload for request query
  */
typedef struct {
    compressched_handle handle;
    /**< The compressched handle used to acquire the resource. */
    compressched_token token;
    /**< The identifier associated with resource acquire. */
    uint64_t reserved;
    /**< Reserved. */
} compressched_request_query;

/**
  * Payload for queue query
  */
typedef struct {
    compressched_queue_priority priority;
    /**< Queue priority. */
    compressched_queue_type queue_type;
    /**< Queue type. */
    compressched_request_query request_query;
} compressched_queue_query; // add req query for handling multiple req. -RP

/**
  * Payload for resource query
  */
typedef struct {
    int domain_id;
    /**< logical id for NSP domain */
    uint32_t num_queues;
    /**< Number of available queues. */
    compressched_queue_query queue_query[NUM_QUEUE_PRIORITY_MAX];
    /**< list of queue queries. */
} compressched_resource_query;

/**
  * Payload for platform query
  */
typedef struct {
    uint32_t num_resources;
    /**< Number of available resources. */
    compressched_resource_query resource_query[NUM_BASE_GROUP_RESOURCE_MAX];
    /**< List of resource queries. */
} compressched_platform_query;



//#endif

int compressched_register(compressched_handle*handle, unsigned int upid, unsigned int tid);
int compressched_register_ex(compressched_handle*handle, unsigned int pid, unsigned int tid);
//#ifdef EN_NSP_SHARING
int compressched_acquire(compressched_handle handle, char* job_name, compressched_acquire_rsp_v2 *response);
//#else
int compressched_acquire_legacy(compressched_handle handle, compressched_acquire_legacy_tx* cmd, compressched_acquire_rsp_v2 *response);
//#endif
int compressched_release_v2(compressched_handle handle, compressched_token token);
int compressched_unregister_v2(compressched_handle handle);
int compressched_release(compressched_handle handle, compressched_token token);
int compressched_unregister(compressched_handle handle);
int compressched_unregister_batch(unsigned int upid);
int compressched_register_v2(compressched_handle* handle, compressched_register_msg *reg_msg);
int compressched_query(compressched_handle handle, compressched_platform_query *query);

/* ---- New functions added for tracing / debug / fault subscription -- */

/**
 * compressched_tracing_start() - Start compressched tracing.
 * @handle:   registered client handle
 * @num_logs: max log entries (0 = unlimited)
 */
int compressched_tracing_start(compressched_handle handle, unsigned int num_logs);

/**
 * compressched_tracing_stop() - Stop compressched tracing.
 * @handle: registered client handle
 */
int compressched_tracing_stop(compressched_handle handle);

/* ---- Wire-format structs for debug-cmd and fault-subscription ---- *
 * These are defined here (not in virtio_compressched_base.h) so that  *
 * the function declarations below can reference them without a        *
 * circular-include problem.                                           */

/** Debug-command TX payload (GVM → PVM). */
typedef struct {
    compressched_handle handle;
    unsigned int        cmd_type;               /**< compressched_debug_cmdtype */
    unsigned char       enable_latency_measure; /**< 1 = enable, 0 = disable   */
    unsigned char       pad[3];
} compressched_debug_cmd;

/** Per-domain fault-filter entry. */
typedef struct {
    int           domain_id;  /**< -1 = all domains */
    unsigned char fault_mask; /**< bitmask of compressched_faults */
    unsigned char pad[3];
} compressched_domain_mask;

/** Subscribe-faults TX payload (GVM → PVM). */
typedef struct {
    compressched_handle    handle;
    compressched_domain_mask mask[NUM_BASE_GROUP_RESOURCE_MAX];
} compressched_subscribe_faults;

/** Unsubscribe-faults TX payload (GVM → PVM). */
typedef struct {
    compressched_handle handle;
} compressched_unsubscribe_faults_tx;

/**
 * compressched_debug_cmd_fn() - Send a debug command to the PVM service.
 * @handle: registered client handle
 * @cmd:    pointer to the debug command payload
 */
int compressched_debug_cmd_fn(compressched_handle handle,
                               compressched_debug_cmd *cmd);

/**
 * compressched_subscribe_faults_fn() - Subscribe to fault notifications.
 * @handle: registered client handle
 * @filter: pointer to the fault-filter payload
 */
int compressched_subscribe_faults_fn(compressched_handle handle,
                                      compressched_subscribe_faults *filter);

/**
 * compressched_unsubscribe_faults_fn() - Unsubscribe from fault notifications.
 * @handle: registered client handle
 */
int compressched_unsubscribe_faults_fn(compressched_handle handle);
