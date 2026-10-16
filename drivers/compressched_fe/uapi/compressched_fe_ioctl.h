/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * compressched_fe_ioctl.h - UAPI ioctl interface for /dev/compressched_fe.
 *
 * This header is shared between the kernel driver (compressched_fe.ko) and
 * the userspace library (libcompressched).  Both sides include this file so
 * the ioctl magic byte, command numbers, and argument struct layouts are
 * guaranteed to be identical - no manual sync required.
 *
 * Kernel:    #include "uapi/compressched_fe_ioctl.h"
 * Userspace: #include <compressched_fe_ioctl.h>   (or via sysroot)
 */

#ifndef _UAPI_COMPRESSCHED_FE_IOCTL_H
#define _UAPI_COMPRESSCHED_FE_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

/*
 * COMPRESSCHED_IOC_MAGIC - vendor-specific ioctl magic byte.
 * Value 0xC5 is in the vendor-specific range (0x80-0xFF).
 */
#define COMPRESSCHED_IOC_MAGIC            0xC5

/* Maximum job name length including the null terminator */
#define COMPRESSCHED_FE_MAX_JOB_NAME_LEN  64

/* ------------------------------------------------------------------ */
/* Resource details sub-structures                                     */
/*                                                                     */
/* These mirror the compressched_resource_details and                  */
/* compressched_acquire_cmd types from the public API header           */
/* (compressched_fe.h) using fixed-width kernel types so the layout    */
/* is unambiguous on all architectures.                                */
/* ------------------------------------------------------------------ */

/**
 * struct compressched_ioctl_resource_details - resource type + sub-ID + domains
 * @req_type:            compressched_resource_request_type enum value
 * @sub_res_id:          compressched_sub_res_id enum value
 * @requested_domain_id: domain IDs for super-group requests (up to 4)
 */
struct compressched_ioctl_resource_details {
	__s32 req_type;
	__s32 sub_res_id;
	__s32 requested_domain_id[4];
};

/**
 * struct compressched_ioctl_acquire_cmd - resource acquire parameters
 * @res_details:  resource type, sub-resource ID, and domain IDs
 * @priority:     compressched_queue_priority enum value
 * @pad0:         explicit padding - keeps __s64 fields 8-byte aligned on all
 *                architectures without relying on implicit compiler padding;
 *                ABI-safe for both 32-bit and 64-bit userspace
 * @duration_us:  expected hold time in microseconds (0 = watchdog disabled)
 * @timeout_us:   acquire timeout in microseconds (0 = fail immediately)
 * @reserved:     must be zero
 * @job_name:     null-terminated job name string (max 63 chars + NUL);
 *                mirrors compressched_acquire_tx::job_name
 */
struct compressched_ioctl_acquire_cmd {
	struct compressched_ioctl_resource_details res_details; /* 24 bytes */
	__s32 priority;   /* 4 bytes - total 28 bytes */
	__s32 pad0;       /* 4 bytes explicit pad - aligns duration_us to 8 bytes */
	__s64 duration_us;
	__s64 timeout_us;
	__u64 reserved;
	//char  job_name[COMPRESSCHED_FE_MAX_JOB_NAME_LEN]; /* 64 bytes: 63 chars + NUL */
};

/* ------------------------------------------------------------------ */
/* ioctl argument structures                                           */
/* ------------------------------------------------------------------ */

/**
 * struct compressched_ioctl_register_args - COMPRESSCHED_IOCTL_REGISTER
 * @pid:    [in]  process ID of the registering client
 * @tid:    [in]  thread ID of the registering client
 * @handle: [out] client handle assigned by the PVM service
 */
struct compressched_ioctl_register_args {
	__u32 pid;
	__u32 tid;
	__u32 handle;
};

/**
 * struct compressched_ioctl_acquire_args - COMPRESSCHED_IOCTL_ACQUIRE
 * @handle:       [in]  registered client handle
 * @pad0:         explicit padding - cmd contains __s64 fields; pad ensures
 *                cmd starts at an 8-byte aligned offset (offset 8)
 * @cmd:          [in]  acquire parameters
 * @token:        [out] release token
 * @rsp_reserved: [out] reserved response word
 */
struct compressched_ioctl_acquire_args {
	__u32                             handle;
	__u32                             pad0;
	struct compressched_ioctl_acquire_cmd cmd;
	__u32                             token;
	__u32                             rsp_reserved;
};

/**
 * struct compressched_ioctl_release_args - COMPRESSCHED_IOCTL_RELEASE
 * @handle: [in] registered client handle
 * @token:  [in] token received from a prior acquire
 */
struct compressched_ioctl_release_args {
	__u32 handle;
	__u32 token;
};

/**
 * struct compressched_ioctl_unregister_args - COMPRESSCHED_IOCTL_UNREGISTER
 * @handle: [in] registered client handle
 */
struct compressched_ioctl_unregister_args {
	__u32 handle;
};

/**
 * struct compressched_ioctl_acquire_jobname_args - COMPRESSCHED_IOCTL_ACQUIRE_JOBNAME
 * @handle:       [in]  registered client handle
 * @job_name:     [in]  null-terminated job name string (max 63 chars + NUL)
 * @token:        [out] release token
 * @rsp_reserved: [out] reserved response word
 *
 * Requires PVM backend virtio job_name support.  Returns -EOPNOTSUPP
 * until that support is present.
 */
struct compressched_ioctl_acquire_jobname_args {
	__u32 handle;
	char  job_name[COMPRESSCHED_FE_MAX_JOB_NAME_LEN];
	__u32 token;
	__u32 rsp_reserved;
};

/* ------------------------------------------------------------------ */
/* ioctl command numbers                                               */
/*                                                                     */
/* _IOWR is used for REGISTER (even though there is no user input)    */
/* to allow future ABI extension without breaking the ioctl number.   */
/* ------------------------------------------------------------------ */

/** Register a new client; returns a handle. */
#define COMPRESSCHED_IOCTL_REGISTER_EX_LIB \
	_IOWR(COMPRESSCHED_IOC_MAGIC, 1, struct compressched_ioctl_register_args)

/** Acquire a compute resource; returns a release token. */
#define COMPRESSCHED_IOCTL_ACQUIRE \
	_IOWR(COMPRESSCHED_IOC_MAGIC, 2, struct compressched_ioctl_acquire_args)

/** Release a previously acquired resource. */
#define COMPRESSCHED_IOCTL_RELEASE_LIB \
	_IOW(COMPRESSCHED_IOC_MAGIC, 3, struct compressched_ioctl_release_args)

/** Unregister a client. */
#define COMPRESSCHED_IOCTL_UNREGISTER_LIB \
	_IOW(COMPRESSCHED_IOC_MAGIC, 4, struct compressched_ioctl_unregister_args)

/** Acquire by job name (requires PVM backend support). */
#define COMPRESSCHED_IOCTL_ACQUIRE_JOBNAME \
	_IOWR(COMPRESSCHED_IOC_MAGIC, 5, struct compressched_ioctl_acquire_jobname_args)

/** Acquire a compute resource using the extended API (compressched_acquire_legacy_tx); returns a release token. */
#define COMPRESSCHED_IOCTL_ACQUIRE_LIB \
	_IOWR(COMPRESSCHED_IOC_MAGIC, 6, struct compressched_ioctl_acquire_args)

/** Query platform resource information; returns compressched_platform_query. */
#define COMPRESSCHED_IOCTL_QUERY_LIB \
	_IOWR(COMPRESSCHED_IOC_MAGIC, 7, struct compressched_ioctl_query_args)

/** Start compressched tracing/logging. */
#define COMPRESSCHED_IOCTL_TRACING_START_LIB \
	_IOW(COMPRESSCHED_IOC_MAGIC, 8, struct compressched_ioctl_tracing_start_args)

/** Stop compressched tracing/logging. */
#define COMPRESSCHED_IOCTL_TRACING_STOP_LIB \
	_IOW(COMPRESSCHED_IOC_MAGIC, 9, struct compressched_ioctl_tracing_stop_args)

/** Send a debug command to the compressched service. */
#define COMPRESSCHED_IOCTL_DEBUG_CMD_LIB \
	_IOW(COMPRESSCHED_IOC_MAGIC, 10, struct compressched_ioctl_debug_cmd_args)

/** Subscribe to fault notifications. */
#define COMPRESSCHED_IOCTL_SUBSCRIBE_FAULTS_LIB \
	_IOW(COMPRESSCHED_IOC_MAGIC, 11, struct compressched_ioctl_subscribe_faults_args)

/** Unsubscribe from fault notifications. */
#define COMPRESSCHED_IOCTL_UNSUBSCRIBE_FAULTS_LIB \
	_IOW(COMPRESSCHED_IOC_MAGIC, 12, struct compressched_ioctl_unsubscribe_faults_args)

/**
 * Block until a fault notification arrives (or the handle is unregistered).
 * Used by the userspace library's fault-polling thread.
 */
#define COMPRESSCHED_IOCTL_WAIT_FAULT \
	_IOWR(COMPRESSCHED_IOC_MAGIC, 13, struct compressched_ioctl_wait_fault_args)

/* ------------------------------------------------------------------ */
/* Query structures                                                    */
/*                                                                     */
/* These mirror the compressched_platform_query hierarchy from         */
/* virtio_compressched_client.h using fixed-width kernel types.        */
/* ------------------------------------------------------------------ */

/* Maximum number of resources for platform query (= NUM_BASE_GROUP_RESOURCE_MAX) */
#define COMPRESSCHED_FE_MAX_RESOURCES        4
/* Maximum number of queue priorities for query (= NUM_QUEUE_PRIORITY_MAX) */
#define COMPRESSCHED_FE_MAX_QUEUE_PRIORITIES 4

/**
 * struct compressched_ioctl_request_query - per-request query info
 * @handle:   [out] client handle used to acquire the resource
 * @token:    [out] token associated with the resource acquire
 * @reserved: must be zero
 */
struct compressched_ioctl_request_query {
	__u32 handle;
	__u32 token;
	__u64 reserved;
};

/**
 * struct compressched_ioctl_queue_query - per-queue query info
 * @priority:      [out] compressched_queue_priority enum value
 * @queue_type:    [out] compressched_queue_type enum value
 * @request_query: [out] per-request query info
 */
struct compressched_ioctl_queue_query {
	__s32 priority;
	__s32 queue_type;
	struct compressched_ioctl_request_query request_query;
};

/**
 * struct compressched_ioctl_resource_query - per-resource query info
 * @domain_id:   [out] logical id for NSP domain
 * @num_queues:  [out] number of available queues
 * @queue_query: [out] list of queue queries (one per priority level)
 */
struct compressched_ioctl_resource_query {
	__s32 domain_id;
	__u32 num_queues;
	struct compressched_ioctl_queue_query
		queue_query[COMPRESSCHED_FE_MAX_QUEUE_PRIORITIES];
};

/**
 * struct compressched_ioctl_platform_query - platform-level query result
 * @num_resources:  [out] number of available resources
 * @pad0:           explicit padding - resource_query contains nested __u64
 *                  fields; pad ensures resource_query starts at an 8-byte
 *                  aligned offset (offset 8)
 * @resource_query: [out] list of resource queries
 */
struct compressched_ioctl_platform_query {
	__u32 num_resources;
	__u32 pad0;
	struct compressched_ioctl_resource_query
		resource_query[COMPRESSCHED_FE_MAX_RESOURCES];
};

/**
 * struct compressched_ioctl_query_args - COMPRESSCHED_IOCTL_QUERY
 * @handle: [in]  registered client handle
 * @pad0:   explicit padding - query contains nested __u64 fields; pad ensures
 *          query starts at an 8-byte aligned offset (offset 8)
 * @query:  [out] platform query result
 */
struct compressched_ioctl_query_args {
	__u32 handle;
	__u32 pad0;
	struct compressched_ioctl_platform_query query;
};

/* ------------------------------------------------------------------ */
/* Tracing / debug / fault-subscription argument structures           */
/* ------------------------------------------------------------------ */

/**
 * struct compressched_ioctl_tracing_start_args - COMPRESSCHED_IOCTL_TRACING_START
 * @handle:   [in] registered client handle
 * @num_logs: [in] maximum number of log entries to capture (0 = unlimited)
 */
struct compressched_ioctl_tracing_start_args {
	__u32 handle;
	__u32 num_logs;
};

/**
 * struct compressched_ioctl_tracing_stop_args - COMPRESSCHED_IOCTL_TRACING_STOP
 * @handle: [in] registered client handle
 */
struct compressched_ioctl_tracing_stop_args {
	__u32 handle;
};

/**
 * struct compressched_ioctl_debug_cmd_args - COMPRESSCHED_IOCTL_DEBUG_CMD
 * @handle:                 [in] registered client handle
 * @cmd_type:               [in] compressched_debug_cmdtype enum value
 * @enable_latency_measure: [in] 1 = enable latency measurement, 0 = disable
 * @pad:                    explicit padding to 4-byte boundary
 */
struct compressched_ioctl_debug_cmd_args {
	__u32 handle;
	__u32 cmd_type;
	__u8  enable_latency_measure;
	__u8  pad[3];
};

/* Maximum number of fault-filter entries (= COMPRESSCHED_FE_MAX_RESOURCES) */
#define COMPRESSCHED_FE_MAX_FAULT_DOMAINS  COMPRESSCHED_FE_MAX_RESOURCES

/**
 * struct compressched_ioctl_subscribe_faults_args - COMPRESSCHED_IOCTL_SUBSCRIBE_FAULTS
 * @handle:     [in] registered client handle
 * @domain_id:  [in] domain IDs to filter (INVALID_DOMAIN_ID = -1 means all)
 * @fault_mask: [in] bitmask of compressched_faults values per domain
 * @pad:        explicit padding to 4-byte boundary
 */
struct compressched_ioctl_subscribe_faults_args {
	__u32 handle;
	__s32 domain_id[COMPRESSCHED_FE_MAX_FAULT_DOMAINS];
	__u8  fault_mask[COMPRESSCHED_FE_MAX_FAULT_DOMAINS];
	__u8  pad[4];
};

/**
 * struct compressched_ioctl_unsubscribe_faults_args - COMPRESSCHED_IOCTL_UNSUBSCRIBE_FAULTS
 * @handle: [in] registered client handle
 */
struct compressched_ioctl_unsubscribe_faults_args {
	__u32 handle;
};

/**
 * struct compressched_ioctl_wait_fault_args - COMPRESSCHED_IOCTL_WAIT_FAULT
 * @handle:      [in]  registered client handle
 * @fault_type:  [out] compressched_faults enum value
 * @pad:         explicit padding
 * @domain_id:   [out] domain that triggered the fault
 * @sub_res_id:  [out] sub-resource ID
 * @fault_queue: [out] compressched_queue_priority of the faulting queue
 */
struct compressched_ioctl_wait_fault_args {
	__u32 handle;
	__u8  fault_type;
	__u8  pad[3];
	__s32 domain_id;
	__s32 sub_res_id;
	__s32 fault_queue;
};

#endif /* _UAPI_COMPRESSCHED_FE_IOCTL_H */
