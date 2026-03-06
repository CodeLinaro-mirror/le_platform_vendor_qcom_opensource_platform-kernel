/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
#ifndef __FASTRPC_RSM_H__
#define __FASTRPC_RSM_H__

#include "fastrpc_common.h"

/* Need corresponding update here if this definition changes in dspsignal.h */
#define DSPSIGNAL_DSPQUEUE_MIN 0

/* Need corresponding update here if the signal enum definition changes in dspsignal.h */
enum dspqueue_signal {
    DSPQUEUE_SIGNAL_REQ_PACKET = 0,
    DSPQUEUE_SIGNAL_REQ_SPACE,
    DSPQUEUE_SIGNAL_RESP_PACKET,
    DSPQUEUE_SIGNAL_RESP_SPACE,
    DSPQUEUE_NUM_SIGNALS
};

#define GET_SIGNAL_NO(signal_id) ((signal_id - DSPSIGNAL_DSPQUEUE_MIN) % DSPQUEUE_NUM_SIGNALS)

/* When it is rsm + dspqueue use case, this struct will be used */
typedef struct {
	/* protocol shared in both non-RSM and RSM scenario (63:32 for upid, 31:0 for signal_id) */
	uint64_t legacy_msg;

	/* new 64-bit extraly added in RSM scenario (the target_id field holds the upid) */
	uint64_t target_id;
} hfastrpc_rsm_dspsignal_msg;	/* protocol between hfastrpc and DSP fastrpc in RSM scenario */

void fastrpc_rsm_list_per_session_free(struct fastrpc_user *fl);
int fastrpc_rsm_acquire(struct fastrpc_user *fl, unsigned int target_id);
void fastrpc_rsm_release(struct fastrpc_user *fl, unsigned int target_id, enum fastrpc_rsm_node_type type);
bool fastrpc_multidomain_ctx_needs_rsm(struct fastrpc_user *fl, unsigned int ctx);
int fastrpc_multidomain_rsm_register(struct fastrpc_user *fl, struct fastrpc_mdctx_info *mdctx);
int fastrpc_multidomain_rsm_acquire(struct fastrpc_user *fl, unsigned int target_id);
void fastrpc_multidomain_rsm_release(struct fastrpc_user *fl, unsigned int target_id);

#endif /*__FASTRPC_RSM_H__*/
