/* SPDX-License-Identifier: GPL-2.0-only
*
*  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. 
*/
typedef unsigned int compressched_handle;
typedef unsigned int compressched_token;
typedef struct {
    compressched_token token;
    unsigned int reserved;
} compressched_acquire_rsp_v2;

int compressched_register(compressched_handle*handle, unsigned int upid, unsigned int tid);
int compressched_acquire(compressched_handle handle, char* job_name, compressched_acquire_rsp_v2 *response);
int compressched_release_v2(compressched_handle handle, compressched_token token);
int compressched_unregister_v2(compressched_handle handle);
int compressched_unregister_batch(unsigned int upid); 
