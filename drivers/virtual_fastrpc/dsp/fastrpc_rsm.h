/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __FASTRPC_RSM_H__
#define __FASTRPC_RSM_H__

#include "fastrpc_common.h"

#define DSPSIGNAL_DSPQUEUE_MIN 0

/* Signals IDs used with driver signaling. Update the signal allocations in dspsignal.h
   if this changes. */
enum dspqueue_signal {
    DSPQUEUE_SIGNAL_REQ_PACKET = 0,
    DSPQUEUE_SIGNAL_REQ_SPACE,
    DSPQUEUE_SIGNAL_RESP_PACKET,
    DSPQUEUE_SIGNAL_RESP_SPACE,
    DSPQUEUE_NUM_SIGNALS
};

typedef struct {
	/* protocol in non-RSM scenario (63:32 for upid, 31:0 for signal_id) */
	uint64_t legacy_msg;
	/* new 64-bit added in RSM scenario (the target_id field holds the upid) */
	uint64_t target_id;
} hfastrpc_rsm_dspsignal_msg;	/* protocol in RSM scenario */

void fastrpc_rsm_list_per_session_free(struct vfastrpc_file *vfl);
int fastrpc_rsm_acquire(struct vfastrpc_file *vfl, unsigned int target_id);
void fastrpc_rsm_release(struct vfastrpc_file *vfl, unsigned int target_id);

#endif /*__FASTRPC_RSM_H__*/
