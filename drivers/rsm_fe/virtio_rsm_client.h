/* SPDX-License-Identifier: GPL-2.0-only
*
*  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. 
*/
typedef unsigned int rsm_handle;
typedef unsigned int rsm_token;
typedef struct {
    rsm_token token;
    unsigned int reserved;
} rsm_acquire_rsp_v2;

int rsm_register(rsm_handle*handle, unsigned int upid, unsigned int tid);
int rsm_acquire(rsm_handle handle, char* job_name, rsm_acquire_rsp_v2 *response);
int rsm_release_v2(rsm_handle handle, rsm_token token);
int rsm_unregister_v2(rsm_handle handle);
int rsm_unregister_batch(unsigned int upid); 
