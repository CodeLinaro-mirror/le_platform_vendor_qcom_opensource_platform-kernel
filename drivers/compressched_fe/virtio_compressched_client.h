/* SPDX-License-Identifier: GPL-2.0-only
*
*  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. 
*/

#define MAX_NUM_OF_NSP  4
#define INVALID_UPID 0xFFFFFFFF
#define INVALID_LOGICAL_ID -1
#define EOK 0
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

int compressched_register(compressched_handle*handle, unsigned int upid, unsigned int tid);
int compressched_acquire(compressched_handle handle, char* job_name, compressched_acquire_rsp_v2 *response);
int compressched_release_v2(compressched_handle handle, compressched_token token);
int compressched_unregister_v2(compressched_handle handle);
int compressched_unregister_batch(unsigned int upid); 
int compressched_register_v2(compressched_handle* handle, compressched_register_msg *reg_msg);
