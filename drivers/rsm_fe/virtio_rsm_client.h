/* SPDX-License-Identifier: GPL-2.0-only
*
*  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. 
*/
typedef uint32_t rsm_handle;
typedef uint32_t rsm_token;
typedef struct {
    rsm_token token;
    uint32_t reserved;
} rsm_acquire_rsp_v2;

typedef struct
{
  uint32_t upid;
  uint32_t tid;
  uint32_t nspID;
} RSMRegisterPDUType;

int rsm_register(rsm_handle*handle, unsigned int upid, unsigned int tid);
int rsm_register_for_nsp(rsm_handle* handle, RSMRegisterPDUType rsmRegData);
int rsm_acquire(rsm_handle handle, const char *job_name, rsm_acquire_rsp_v2 *response);
int rsm_release_v2(rsm_handle handle, rsm_token token);
int rsm_unregister_v2(rsm_handle handle);
int rsm_unregister_batch(unsigned int upid); 
