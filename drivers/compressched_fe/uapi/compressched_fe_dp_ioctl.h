/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * compressched_fe_dp_ioctl.h - UAPI ioctl interface for the Dynamic Partition
 * (DP) "acquire available resource" API on /dev/compressched_fe.
 *
 * This header is shared between the kernel driver (compressched_fe.ko) and
 * the userspace library (libcompressched).  Both sides include this file so
 * the ioctl magic byte, command numbers, and argument struct layouts are
 * guaranteed to be identical — no manual sync required.
 *
 * Kernel:    #include "uapi/compressched_fe_dp_ioctl.h"
 * Userspace: #include <compressched_fe_dp_ioctl.h>   (or via sysroot)
 *
 * The DP structs defined here mirror the PVM-side types from
 * compressched_client_dp_min.h and the PVM vhost wire-format structs
 * (compressched_avail_res_acquire_rx / compressched_avail_res_release_rx in
 * vhu_compressched_msg.h) using fixed-width kernel types so the layout is
 * unambiguous on all architectures.
 *
 * Layout MUST remain byte-for-byte identical to the PVM-side structs.
 */

#ifndef _UAPI_COMPRESSCHED_FE_DP_IOCTL_H
#define _UAPI_COMPRESSCHED_FE_DP_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

/*
 * Re-use the same magic byte as the base ioctl header.
 * COMPRESSCHED_IOC_MAGIC is defined in compressched_fe_ioctl.h; include that
 * header before this one, or define it here if used standalone.
 */
#ifndef COMPRESSCHED_IOC_MAGIC
#define COMPRESSCHED_IOC_MAGIC  0xC5
#endif

/* ------------------------------------------------------------------ */
/* Constants (must match compressched_client_dp_min.h on the PVM side) */
/* ------------------------------------------------------------------ */

/** Maximum number of NSP DP group resources.
 *  Mirrors NUM_NSP_DP_GROUP_RESOURCE_MAX in compressched_client_dp_min.h. */
#define COMPRESSCHED_FE_DP_MAX_DOMAIN_IDS   4U

/** Trailing reserved[] size in every DP struct.
 *  Mirrors COMPRESSCHED_RESERVED_BYTES_DP in compressched_client_dp_min.h. */
#define COMPRESSCHED_FE_DP_RESERVED_BYTES   128U

/** Maximum job name length (mirrors MAX_JOB_NAME_SIZE in compressched_client.h). */
#define COMPRESSCHED_FE_DP_MAX_JOB_NAME_LEN 64U

/* ------------------------------------------------------------------ */
/* Resource family (mirrors compressched_avail_res_family)             */
/* ------------------------------------------------------------------ */
#define COMPRESSCHED_FE_DP_FAMILY_NSP   0
#define COMPRESSCHED_FE_DP_FAMILY_HPASS 1

/* ------------------------------------------------------------------ */
/* Candidate-pool selection (mirrors compressched_avail_res_selection) */
/* ------------------------------------------------------------------ */
#define COMPRESSCHED_FE_DP_SELECT_ANY  0
#define COMPRESSCHED_FE_DP_SELECT_LIST 1

/* ------------------------------------------------------------------ */
/* DP command blob                                                      */
/*                                                                      */
/* Flat, fixed-width representation of                                  */
/* compressched_acquire_available_resource_cmd (compressched_client.h). */
/*                                                                      */
/* Layout discipline: the base struct has an IMPLICIT 4-byte alignment  */
/* gap between resource_count (offset 20) and duration_us (offset 24). */
/* That gap is made EXPLICIT here as pad0 so the layout is unambiguous  */
/* on all architectures and the struct can be used directly in ioctl    */
/* copy_from/to_user without any field-by-field mapping.                */
/*                                                                      */
/* sizeof(struct compressched_ioctl_dp_cmd) MUST equal 248.             */
/* ------------------------------------------------------------------ */

/**
 * struct compressched_ioctl_dp_cmd - DP acquire command blob
 * @handle:              [in] optional client handle (takes precedence over
 *                            the API handle argument if valid)
 * @priority:            [in] compressched_queue_priority enum value
 * @select_mode:         [in] compressched_select_mode enum value
 *                            (NSP_SELECT_ANY=0, NSP_SELECT_LIST=1,
 *                             HPASS_SELECT_ANY=2, HPASS_SELECT_LIST=3)
 * @strict_mode:         [in] compressched_strict_mode enum value
 *                            (LESS_THAN_EQUAL_COUNT=0, EXACTLY_COUNT=1)
 * @resource_count:      [in] whole base resources requested (0 = default 1)
 * @pad0:                explicit padding — mirrors the implicit 4-byte gap
 *                       between resource_count (offset 20) and duration_us
 *                       (offset 24) in the base struct; MUST be zero
 * @duration_us:         [in] hold-duration watchdog in microseconds
 *                            (0 = watchdog disabled)
 * @timeout_us:          [in] acquire-wait timeout in microseconds
 *                            (0 = return immediately if nothing free)
 * @job_name:            [in] null-terminated job name for RSM job table lookup
 * @requested_domain_id: [in] candidate domain LIST for *_SELECT_LIST modes
 * @reserved:            must be zero; forward-compatible growth
 */
struct compressched_ioctl_dp_cmd {
	__u32 handle;                                          /* offset   0 */
	__s32 priority;                                        /* offset   4 */
	__s32 select_mode;                                     /* offset   8 */
	__s32 strict_mode;                                     /* offset  12 */
	__u32 resource_count;                                  /* offset  16 */
	__u32 pad0;   /* explicit pad — mirrors implicit gap   offset  20 */
	__s64 duration_us;                                     /* offset  24 */
	__s64 timeout_us;                                      /* offset  32 */
	char  job_name[COMPRESSCHED_FE_DP_MAX_JOB_NAME_LEN];  /* offset  40 */
	__s32 requested_domain_id[COMPRESSCHED_FE_DP_MAX_DOMAIN_IDS]; /* offset 104 */
	__u8  reserved[COMPRESSCHED_FE_DP_RESERVED_BYTES];    /* offset 120 */
};                                                         /* total: 248 */

/* ------------------------------------------------------------------ */
/* Override parameters                                                  */
/*                                                                      */
/* Flat, fixed-width representation of compressched_avail_res_param_t. */
/* sizeof(struct compressched_ioctl_dp_param) MUST equal 156.          */
/* ------------------------------------------------------------------ */

/**
 * struct compressched_ioctl_dp_param - optional DP acquire override
 * @family:              [in] COMPRESSCHED_FE_DP_FAMILY_NSP or _HPASS
 * @selection:           [in] COMPRESSCHED_FE_DP_SELECT_ANY or _LIST
 * @resource_count:      [in] whole base resources requested (0 = default 1)
 * @requested_domain_id: [in] candidate domain LIST for SELECT_LIST modes;
 *                            ignored for SELECT_ANY; zero-terminated
 * @reserved:            must be zero; forward-compatible growth
 */
struct compressched_ioctl_dp_param {
	__s32 family;                                                  /* offset  0 */
	__s32 selection;                                               /* offset  4 */
	__u32 resource_count;                                          /* offset  8 */
	__s32 requested_domain_id[COMPRESSCHED_FE_DP_MAX_DOMAIN_IDS]; /* offset 12 */
	__u8  reserved[COMPRESSCHED_FE_DP_RESERVED_BYTES];            /* offset 28 */
};                                                                 /* total: 156 */

/* ------------------------------------------------------------------ */
/* Acquire result                                                       */
/*                                                                      */
/* Flat, fixed-width representation of compressched_avail_res_result_t.*/
/* sizeof(struct compressched_ioctl_dp_result) MUST equal 152.         */
/* ------------------------------------------------------------------ */

/**
 * struct compressched_ioctl_dp_result - DP acquire result
 * @token:               [out] reservation token; 0 on failure
 * @available_count:     [out] number of base resources actually reserved
 * @available_domain_id: [out] logical domain IDs of the reserved resources;
 *                             first available_count entries are valid
 * @reserved:            reserved; forward-compatible growth
 */
struct compressched_ioctl_dp_result {
	__u32 token;                                                   /* offset  0 */
	__u32 available_count;                                         /* offset  4 */
	__s32 available_domain_id[COMPRESSCHED_FE_DP_MAX_DOMAIN_IDS]; /* offset  8 */
	__u8  reserved[COMPRESSCHED_FE_DP_RESERVED_BYTES];            /* offset 24 */
};                                                                 /* total: 152 */

/* ------------------------------------------------------------------ */
/* ioctl argument structures                                           */
/* ------------------------------------------------------------------ */

/**
 * struct compressched_ioctl_avail_res_acquire_args
 *   - COMPRESSCHED_IOCTL_AVAIL_RES_ACQUIRE
 *
 * @cmd:       [in]  DP acquire command blob (see struct compressched_ioctl_dp_cmd).
 *                   The client handle embedded in cmd.handle is used by the PVM.
 * @param:     [in]  Optional override parameters.  Ignored when has_param == 0.
 * @has_param: [in]  1 = param is valid and should be applied; 0 = use blob's
 *                   own fields (param is ignored).
 * @pad:       explicit padding to 4-byte boundary; must be zero
 * @result:    [out] filled on success with token, available_count, and
 *                   available_domain_id[].
 */
struct compressched_ioctl_avail_res_acquire_args {
	struct compressched_ioctl_dp_cmd    cmd;       /* [in]  248 bytes */
	struct compressched_ioctl_dp_param  param;     /* [in]  156 bytes */
	__u8                                has_param; /* [in]    1 byte  */
	__u8                                pad[3];    /*         3 bytes */
	struct compressched_ioctl_dp_result result;    /* [out] 152 bytes */
};                                                 /* total: 560 bytes */

/**
 * struct compressched_ioctl_avail_res_release_args
 *   - COMPRESSCHED_IOCTL_AVAIL_RES_RELEASE
 *
 * @cmd:   [in] DP command blob; used ONLY to extract the client handle.
 *              The same blob that was passed to the matching acquire call.
 * @token: [in] reservation token from the prior acquire result.
 */
struct compressched_ioctl_avail_res_release_args {
	struct compressched_ioctl_dp_cmd cmd;   /* [in] 248 bytes */
	__u32                            token; /* [in]   4 bytes */
};                                          /* total: 252 bytes */

/* ------------------------------------------------------------------ */
/* ioctl command numbers                                               */
/*                                                                      */
/* Numbers 14 and 15 are the next available slots after the base       */
/* ioctl header (compressched_fe_ioctl.h) which uses 1–13.             */
/* ------------------------------------------------------------------ */

/** Acquire available resources via the DP minimal API. */
#define COMPRESSCHED_IOCTL_AVAIL_RES_ACQUIRE \
	_IOWR(COMPRESSCHED_IOC_MAGIC, 14, \
	      struct compressched_ioctl_avail_res_acquire_args)

/** Release a DP reservation. */
#define COMPRESSCHED_IOCTL_AVAIL_RES_RELEASE \
	_IOW(COMPRESSCHED_IOC_MAGIC, 15, \
	     struct compressched_ioctl_avail_res_release_args)

#endif /* _UAPI_COMPRESSCHED_FE_DP_IOCTL_H */