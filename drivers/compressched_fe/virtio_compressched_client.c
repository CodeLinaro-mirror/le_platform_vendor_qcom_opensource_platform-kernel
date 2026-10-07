/* SPDX-License-Identifier: GPL-2.0-only
*
*  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
*/
#include "virtio_compressched_base.h"

/* Check and returns a valid index
 * if table full returns MAX_CLIENT
*/
/* Insert handle→idx mapping into the O(1) lookup table.
 * Must be called with vq_clientlock held or immediately after receiving
 * the handle from PVM while only one thread can be in this register path
 * for this idx (ensured by the per-idx completion). */
static void handle_ht_insert(compressched_handle handle, int idx)
{
    int slot = (int)((unsigned int)handle % (unsigned int)MAX_CLIENT);
    g_vdevcompressched->handle_ht[slot] = idx;
}

static int is_upid_exist(unsigned int *upid, unsigned int *new_upid)
{
  if(upid == NULL || new_upid == NULL)
  {
    LOG_COMPRESSCHEDFE(LEVEL_ERR, " is_upid_exist check failed due to Invalid Parameter \n");
    return EINVAL;
  }
  /* Outer loop: iterate through new UPIDs */
  for(int i = 0; i < MAX_NUM_OF_NSP; i++)
  {
    /* Stop if we hit the invalid marker */
    if (new_upid[i] == INVALID_UPID)
    {
        continue;
    }

    /* Inner loop: iterate through stored UPIDs */
    for (int j = 0; j < MAX_NUM_OF_NSP; j++)
    {
      /* Stop if we hit the invalid marker */
      if (upid[j] == INVALID_UPID)
      {
        continue;
      }

      /* Compare values */
      if(new_upid[i] == upid[j])
      {
        return EALREADY; /* Match found (Overlap) */
      }
    }
  }
  return EOK; /* No overlaps found */
}

static int is_upid_equal(unsigned int *upid, unsigned int *new_upid)
{
  if(upid == NULL || new_upid == NULL)
  {
    LOG_COMPRESSCHEDFE(LEVEL_ERR, " is_upid_equal failed due to Invalid Parameter \n");
 	return EINVAL;
  }
  if (memcmp(upid, new_upid, MAX_NUM_OF_NSP * sizeof(unsigned int)) != 0)
  {
    return EINVAL;
  }
  return EOK;
}

/* Clear one client slot. Caller must hold vq_clientlock. */
static void clear_client_slot_locked(int idx)
{
    compressched_handle h = g_vdevcompressched->client_list[idx].handle;
    if (h != 0) {
        int slot = (int)((unsigned int)h % (unsigned int)MAX_CLIENT);
        if (g_vdevcompressched->handle_ht[slot] == idx)
            g_vdevcompressched->handle_ht[slot] = MAX_CLIENT;
    }
    memset(&g_vdevcompressched->client_list[idx], 0,
           sizeof(struct compressched_client_table));
    memset(g_vdevcompressched->client_list[idx].upid, INVALID_UPID,
           MAX_NUM_OF_NSP * sizeof(unsigned int));
}

static int add_client_table_entry(unsigned int *upid, unsigned int target_id)
{
  int idx = 0;
  int free_slot_idx = -1; /* FIX 1: Variable declared here */
  int has_valid_id = 0;

  if(upid == NULL || target_id == 0)
  {
    LOG_COMPRESSCHEDFE(LEVEL_ERR, " add_client_table_entry failed due to Invalid Parameter \n");
    return MAX_CLIENT;
  }

  for(int index=0; index<MAX_NUM_OF_NSP; index++) {
    if(upid[index] != INVALID_UPID) {
      has_valid_id = 1; //This will make sure that atleast one index has valid upid
    }
  }
  if(!has_valid_id)
  {
    LOG_COMPRESSCHEDFE(LEVEL_ERR, " Invalid UPIDs are provided \n");
    return MAX_CLIENT;
  }
  spin_lock(&g_vdevcompressched->vq_clientlock);
  for (idx = 0; idx < MAX_CLIENT; idx++)
  {
    if(g_vdevcompressched->client_list[idx].target_id == 0)
    {
      if (free_slot_idx == -1)
      {
          free_slot_idx = idx;
      }
      continue;
    }
    if ((g_vdevcompressched->client_list[idx].target_id == target_id) && (is_upid_exist(g_vdevcompressched->client_list[idx].upid, upid)!=EOK))
    {
      idx = MAX_CLIENT+1; //Duplicate entry found
      free_slot_idx = -1;
      LOG_COMPRESSCHEDFE(LEVEL_ERR, " Duplicate Entry found for target_id:%d \n",target_id);

      break;
    }
  }
  if (free_slot_idx != -1)
  {
    memcpy(g_vdevcompressched->client_list[free_slot_idx].upid, upid, MAX_NUM_OF_NSP * sizeof(unsigned int));
    g_vdevcompressched->client_list[free_slot_idx].target_id = target_id;
    idx = free_slot_idx;
  }
  else
  {
    idx = MAX_CLIENT;
    LOG_COMPRESSCHEDFE(LEVEL_ERR, " No Valid client slot found for target_id:%d \n",target_id);
  }
  spin_unlock(&g_vdevcompressched->vq_clientlock);
  return idx;
}

/*
 * add_client_table_entry_ex - allocate a free client slot for a
 * compressched_register_ex (pid/tid) registration.
 *
 * Unlike add_client_table_entry(), this function intentionally skips the
 * duplicate-upid check.  The PVM server is the authoritative source of
 * handle uniqueness; multiple handles from the same (pid, tid) are valid
 * on the PVM side and must be supported here too.  A single thread may
 * hold two independent handles concurrently (e.g. dp_multi_client_test).
 *
 * Returns: slot index on success, MAX_CLIENT if the table is full.
 */
static int add_client_table_entry_ex(unsigned int pid, unsigned int tid)
{
    int idx;
    int free_slot_idx = -1;

    if (pid == 0 || tid == 0) {
        LOG_COMPRESSCHEDFE(LEVEL_ERR,
            " add_client_table_entry_ex: invalid pid=%u tid=%u\n", pid, tid);
        return MAX_CLIENT;
    }

    spin_lock(&g_vdevcompressched->vq_clientlock);
    for (idx = 0; idx < MAX_CLIENT; idx++) {
        if (g_vdevcompressched->client_list[idx].target_id == 0) {
            if (free_slot_idx == -1)
                free_slot_idx = idx;
        }
    }
    if (free_slot_idx != -1) {
        /* Store pid in upid[0]; target_id holds tid — mirrors register_ex layout. */
        memset(g_vdevcompressched->client_list[free_slot_idx].upid,
               INVALID_UPID, MAX_NUM_OF_NSP * sizeof(unsigned int));
        g_vdevcompressched->client_list[free_slot_idx].upid[0] = pid;
        g_vdevcompressched->client_list[free_slot_idx].target_id = tid;
        idx = free_slot_idx;
    } else {
        idx = MAX_CLIENT;
        LOG_COMPRESSCHEDFE(LEVEL_ERR,
            " add_client_table_entry_ex: table full (pid=%u tid=%u)\n", pid, tid);
    }
    spin_unlock(&g_vdevcompressched->vq_clientlock);
    return idx;
}

/*
 * delete_client_table_entry_by_idx - free a client slot by its known index.
 *
 * Used by the compressched_register_ex / compressched_unregister paths where
 * the slot index is already known (from find_client_table_entry or from the
 * return value of add_client_table_entry_ex).  Searching by (upid, target_id)
 * is unsafe when multiple slots share the same pid+tid pair.
 */
static void delete_client_table_entry_by_idx(int idx)
{
    if (idx < 0 || idx >= MAX_CLIENT) {
        LOG_COMPRESSCHEDFE(LEVEL_ERR,
            " delete_client_table_entry_by_idx: bad idx=%d\n", idx);
        return;
    }
    spin_lock(&g_vdevcompressched->vq_clientlock);
    clear_client_slot_locked(idx);
    spin_unlock(&g_vdevcompressched->vq_clientlock);
}

static int find_client_table_entry(compressched_handle handle)
{
    int slot, idx;
    if (handle == 0)
        return MAX_CLIENT;
    slot = (int)((unsigned int)handle % (unsigned int)MAX_CLIENT);
    spin_lock(&g_vdevcompressched->vq_clientlock);
    idx = g_vdevcompressched->handle_ht[slot];
    /* Verify the stored slot still maps to this handle (no stale entry). */
    if (idx < MAX_CLIENT && g_vdevcompressched->client_list[idx].handle != handle)
        idx = MAX_CLIENT;
    spin_unlock(&g_vdevcompressched->vq_clientlock);
    return idx;
}

static void delete_client_table_entry(unsigned int *upid, unsigned int target_id)
{
    int idx = 0;
    if(upid == NULL || target_id == 0)
    {
      LOG_COMPRESSCHEDFE(LEVEL_ERR, " Invalid parameter passed for delete_client_table_entry");
    }
    spin_lock(&g_vdevcompressched->vq_clientlock);
    for (idx = 0; idx < MAX_CLIENT; idx ++)
    {
      if((g_vdevcompressched->client_list[idx].target_id == target_id) && (is_upid_equal(g_vdevcompressched->client_list[idx].upid,upid) == EOK))
      {
        clear_client_slot_locked(idx);
        break;
      }
    }
    spin_unlock(&g_vdevcompressched->vq_clientlock);
}

/*
 * compressched_lookup - validate handle and return its client table index.
 *
 * Checks that the device is present, the handle is non-zero, and the handle
 * maps to a live client table slot.  Returns the slot index on success or a
 * negative errno on failure.
 */
static int compressched_lookup(compressched_handle handle, const char *op)
{
    int idx;

    if (handle == 0) {
        LOG_COMPRESSCHEDFE(LEVEL_ERR, " %s: invalid handle\n", op);
        return -EINVAL;
    }
    if (g_vdevcompressched == NULL) {
        LOG_COMPRESSCHEDFE(LEVEL_ERR, "%s: no device\n", op);
        return -ENODEV;
    }
    idx = find_client_table_entry(handle);
    if (idx >= MAX_CLIENT) {
        LOG_COMPRESSCHEDFE(LEVEL_ERR, " %s: invalid FE handle %x\n", op, handle);
        return -EINVAL;
    }
    return idx;
}

/*
 * compressched_send_wait - transmit txbuf, wait for PVM completion, return status.
 *
 * Initialises the per-slot completion, fires the TX, blocks until the RX
 * callback fires, logs success/failure, and returns the negated PVM error code
 * (or 0 on success).
 */
static int compressched_send_wait(struct virtio_compressched_txbuf *txbuf,
                                  int idx, compressched_handle handle,
                                  const char *op)
{
    int err;

    g_vdevcompressched->client_list[idx].rxbuf.return_val.err = 0;
    init_completion(&g_vdevcompressched->client_list[idx].work);

    err = virt_compressched_txbuf(txbuf);
    if (err != NO_ERROR) {
        LOG_COMPRESSCHEDFE(LEVEL_ERR, " %s TX failed handle %x err=%d\n",
                           op, handle, err);
        return err;
    }

    wait_for_completion(&g_vdevcompressched->client_list[idx].work);

    err = (int)g_vdevcompressched->client_list[idx].rxbuf.return_val.err;
    if (err != 0)
        LOG_COMPRESSCHEDFE(LEVEL_ERR, " %s unsuccessful handle %x err=%d\n",
                           op, handle, err);
    else
        LOG_COMPRESSCHEDFE(LEVEL_INFO, " %s completed handle=%x\n", op, handle);

    return err ? -err : 0;
}

int compressched_register(compressched_handle* handle, unsigned int upid, unsigned int tid)
{
  compressched_register_msg reg_msg;
  int ret = EOK;
  reg_msg.nsp_count = 1;
  reg_msg.target_id = tid;
  reg_msg.upid[0] = upid;
  reg_msg.logical_id[0] = 16;
  ret = compressched_register_v2(handle,&reg_msg);
  return ret;
}

//#ifdef EN_NSP_SHARING
int compressched_acquire(compressched_handle handle, char* job_name, compressched_acquire_rsp_v2 *response)
{
    struct virtio_compressched_txbuf txbuf = {};
    int idx;

    if (job_name == NULL) {
        LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched_acquire: invalid inputs\n");
        return -EINVAL;
    }

    idx = compressched_lookup(handle, "compressched_acquire");
    if (idx < 0)
        return idx;

    txbuf.cmd = COMPRESSCHED_ACQUIRE;
    txbuf.send_data.acquire_nsp_share_data.handle = handle;
    strscpy(txbuf.send_data.acquire_nsp_share_data.job_name, job_name,
            sizeof(txbuf.send_data.acquire_nsp_share_data.job_name));
    txbuf.msg_id = idx;

    {
        int ret = compressched_send_wait(&txbuf, idx, handle, "compressched_acquire");
        if (ret == 0)
            memcpy(response, &g_vdevcompressched->client_list[idx].rxbuf.acq_rsp,
                   sizeof(compressched_acquire_rsp_v2));
        return ret;
    }
}
//#else

int compressched_acquire_legacy(compressched_handle handle, compressched_acquire_legacy_tx* cmd, compressched_acquire_rsp_v2 *response)
{
    struct virtio_compressched_txbuf txbuf = {};
    int idx;

    if (cmd == NULL) {
        LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched_acquire_legacy: invalid inputs\n");
        return -EINVAL;
    }

    idx = compressched_lookup(handle, "compressched_acquire_legacy");
    if (idx < 0)
        return idx;

    txbuf.cmd = COMPRESSCHED_ACQUIRE_LIB;
    txbuf.send_data.acquire_data_lagacy.handle      = handle;
    txbuf.send_data.acquire_data_lagacy.res_details = cmd->res_details;
    txbuf.send_data.acquire_data_lagacy.priority    = cmd->priority;
    txbuf.send_data.acquire_data_lagacy.duration_us = cmd->duration_us;
    txbuf.send_data.acquire_data_lagacy.timeout_us  = cmd->timeout_us;
    txbuf.msg_id = idx;

    {
        int ret = compressched_send_wait(&txbuf, idx, handle, "compressched_acquire_legacy");
        if (ret == 0)
            memcpy(response, &g_vdevcompressched->client_list[idx].rxbuf.acq_rsp,
                   sizeof(compressched_acquire_rsp_v2));
        return ret;
    }
}

//#endif


int compressched_release_v2(compressched_handle handle, compressched_token token)
{
    struct virtio_compressched_txbuf txbuf = {};
    int idx;

    if (token == 0) {
        LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched_release_v2: invalid inputs\n");
        return -EINVAL;
    }

    idx = compressched_lookup(handle, "compressched_release_v2");
    if (idx < 0)
        return idx;

    txbuf.cmd = COMPRESSCHED_RELEASE;
    txbuf.send_data.release_data.handle = handle;
    txbuf.send_data.release_data.token  = token;
    txbuf.msg_id = idx;

    return compressched_send_wait(&txbuf, idx, handle, "compressched_release_v2");
}

int compressched_unregister_v2(compressched_handle handle)
{
    struct virtio_compressched_txbuf txbuf = {};
    int idx;
    int ret;

    idx = compressched_lookup(handle, "compressched_unregister_v2");
    if (idx < 0)
        return idx;

    txbuf.cmd = COMPRESSCHED_UNREGISTER;
    txbuf.send_data.unregister_data.handle = handle;
    txbuf.msg_id = idx;

    ret = compressched_send_wait(&txbuf, idx, handle, "compressched_unregister_v2");

    /* Always free the local client table slot regardless of PVM response,
     * so the slot is available for future registrations.
     * Use the index-based delete to correctly handle register_ex handles
     * where multiple slots may share the same (pid, tid). */
    delete_client_table_entry_by_idx(idx);
    return ret;
}

int compressched_release(compressched_handle handle, compressched_token token)
{
    struct virtio_compressched_txbuf txbuf = {};
    int idx;

    if (token == 0) {
        LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched_release: invalid inputs\n");
        return -EINVAL;
    }

    idx = compressched_lookup(handle, "compressched_release");
    if (idx < 0)
        return idx;

    txbuf.cmd = COMPRESSCHED_RELEASE_LIB;
    txbuf.send_data.release_data.handle = handle;
    txbuf.send_data.release_data.token  = token;
    txbuf.msg_id = idx;

    return compressched_send_wait(&txbuf, idx, handle, "compressched_release");
}

int compressched_unregister(compressched_handle handle)
{
    struct virtio_compressched_txbuf txbuf = {};
    int idx;
    int ret;

    idx = compressched_lookup(handle, "compressched_unregister");
    if (idx < 0)
        return idx;

    txbuf.cmd = COMPRESSCHED_UNREGISTER_LIB;
    txbuf.send_data.unregister_data.handle = handle;
    txbuf.msg_id = idx;

    ret = compressched_send_wait(&txbuf, idx, handle, "compressched_unregister");

    /* Always free the local client table slot regardless of PVM response,
     * so the slot is available for future registrations.
     * Use the index-based delete to correctly handle register_ex handles
     * where multiple slots may share the same (pid, tid). */
    delete_client_table_entry_by_idx(idx);
    return ret;
}

int compressched_unregister_batch(unsigned int upid)
{
    /********to be implemented********/
    LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched_unregister_batch unsuccessful. Not implemented \n");
    return EINVAL;
}

int compressched_query(compressched_handle handle, compressched_platform_query *query)
{
    struct virtio_compressched_txbuf txbuf = {};
    int idx;
    int ret;

    if (query == NULL) {
        LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched_query: invalid inputs\n");
        return -EINVAL;
    }

    idx = compressched_lookup(handle, "compressched_query");
    if (idx < 0)
        return idx;

    txbuf.cmd = COMPRESSCHED_QUERY_LIB;
    txbuf.send_data.query_data.handle = handle;
    txbuf.msg_id = idx;

    ret = compressched_send_wait(&txbuf, idx, handle, "compressched_query");
    if (ret == 0)
        memcpy(query, &g_vdevcompressched->client_list[idx].rxbuf.query_rsp,
               sizeof(compressched_platform_query));
    return ret;
}


/* ================================================================
 * Tracing / debug / fault-subscription virtio client functions
 * ================================================================ */

/**
 * compressched_tracing_start() - Send COMPRESSCHED_TRACING_START to PVM.
 *
 * Uses the first available client slot (idx 0) as the msg_id because
 * tracing is a device-wide operation, not per-client.  The caller must
 * hold a valid registered handle so the PVM can associate the request.
 */
int compressched_tracing_start(compressched_handle handle, unsigned int num_logs)
{
    struct virtio_compressched_txbuf txbuf = {};
    int idx;

    idx = compressched_lookup(handle, "compressched_tracing_start");
    if (idx < 0)
        return idx;

    txbuf.cmd = COMPRESSCHED_TRACING_START_LIB;
    txbuf.msg_id = idx;
    txbuf.send_data.tracing_start_data.handle   = handle;
    txbuf.send_data.tracing_start_data.num_logs = num_logs;

    return compressched_send_wait(&txbuf, idx, handle, "compressched_tracing_start");
}

/**
 * compressched_tracing_stop() - Send COMPRESSCHED_TRACING_STOP to PVM.
 */
int compressched_tracing_stop(compressched_handle handle)
{
    struct virtio_compressched_txbuf txbuf = {};
    int idx;

    idx = compressched_lookup(handle, "compressched_tracing_stop");
    if (idx < 0)
        return idx;

    txbuf.cmd = COMPRESSCHED_TRACING_STOP_LIB;
    txbuf.msg_id = idx;
    txbuf.send_data.tracing_stop_data.handle = handle;

    return compressched_send_wait(&txbuf, idx, handle, "compressched_tracing_stop");
}

/**
 * compressched_debug_cmd_fn() - Send COMPRESSCHED_DEBUG_CMD to PVM.
 */
int compressched_debug_cmd_fn(compressched_handle handle,
                               compressched_debug_cmd *cmd)
{
    struct virtio_compressched_txbuf txbuf = {};
    int idx;

    if (cmd == NULL) {
        LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched_debug_cmd_fn: invalid args\n");
        return -EINVAL;
    }

    idx = compressched_lookup(handle, "compressched_debug_cmd_fn");
    if (idx < 0)
        return idx;

    txbuf.cmd = COMPRESSCHED_DEBUG_CMD_LIB;
    txbuf.msg_id = idx;
    txbuf.send_data.debug_cmd_data         = *cmd;
    txbuf.send_data.debug_cmd_data.handle  = handle;

    return compressched_send_wait(&txbuf, idx, handle, "compressched_debug_cmd_fn");
}

/**
 * compressched_subscribe_faults_fn() - Send COMPRESSCHED_SUBSCRIBE_FAULTS to PVM.
 */
int compressched_subscribe_faults_fn(compressched_handle handle,
                                      compressched_subscribe_faults *filter)
{
    struct virtio_compressched_txbuf txbuf = {};
    int idx;

    if (filter == NULL) {
        LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched_subscribe_faults_fn: invalid args\n");
        return -EINVAL;
    }

    idx = compressched_lookup(handle, "compressched_subscribe_faults_fn");
    if (idx < 0)
        return idx;

    txbuf.cmd = COMPRESSCHED_SUBSCRIBE_FAULTS_LIB;
    txbuf.msg_id = idx;
    txbuf.send_data.subscribe_faults_data         = *filter;
    txbuf.send_data.subscribe_faults_data.handle  = handle;

    return compressched_send_wait(&txbuf, idx, handle, "compressched_subscribe_faults_fn");
}

/**
 * compressched_unsubscribe_faults_fn() - Send COMPRESSCHED_UNSUBSCRIBE_FAULTS to PVM.
 */
int compressched_unsubscribe_faults_fn(compressched_handle handle)
{
    struct virtio_compressched_txbuf txbuf = {};
    int idx;

    idx = compressched_lookup(handle, "compressched_unsubscribe_faults_fn");
    if (idx < 0)
        return idx;

    txbuf.cmd = COMPRESSCHED_UNSUBSCRIBE_FAULTS_LIB;
    txbuf.msg_id = idx;
    txbuf.send_data.unsubscribe_faults_data.handle = handle;

    return compressched_send_wait(&txbuf, idx, handle, "compressched_unsubscribe_faults_fn");
}

int compressched_register_v2(compressched_handle* handle, compressched_register_msg *reg_msg)
{
  struct virtio_compressched_txbuf txbuf = {};
  int idx = 0;
  int err = NO_ERROR;
  unsigned char index = 0;
  if (handle == NULL || reg_msg == NULL)
  {
      LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched_register unsuccessful. Invalid Args \n");
      return -EINVAL;
  }
  if (g_vdevcompressched == NULL)
  {
      LOG_COMPRESSCHEDFE(LEVEL_ERR, "compressched_register unsuccessful. No device\n");
      return -ENODEV;
  }

  if(reg_msg->nsp_count > MAX_NUM_OF_NSP)
  {
    return -EINVAL;
  }

  /*add client to client table*/
  for(index = reg_msg->nsp_count; index < MAX_NUM_OF_NSP; index++)
  {
    reg_msg->upid[index] = INVALID_UPID;
    reg_msg->logical_id[index] = INVALID_LOGICAL_ID;
  }
  idx = add_client_table_entry(reg_msg->upid, reg_msg->target_id);
  if(idx >= MAX_CLIENT)
  {
    /* No available client table entries*/
    err = ENOSPC;
    LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched_register unsuccessful. reached max GVM Clients. err = %d \n",err);
    return -err;
  }

  LOG_COMPRESSCHEDFE(LEVEL_INFO, " compressched_register start msgid = %d \n",idx);
  txbuf.msg_id = idx;
  txbuf.cmd = COMPRESSCHED_REGISTER;
  txbuf.send_data.register_data.register_msg.nsp_count = reg_msg->nsp_count;
  txbuf.send_data.register_data.register_msg.target_id = reg_msg->target_id;

  for(index = 0; index < MAX_NUM_OF_NSP;index++)
  {
    txbuf.send_data.register_data.register_msg.upid[index] = reg_msg->upid[index];
    txbuf.send_data.register_data.register_msg.logical_id[index] = reg_msg->logical_id[index];
  }
  init_completion(&g_vdevcompressched->client_list[idx].work);
  err = virt_compressched_txbuf(&txbuf);
  if(err != NO_ERROR)
  {
      LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched_register unsuccessful. TX Failed: %d \n", err);
      /* Send was unsuccessful so release the client index */
      delete_client_table_entry(reg_msg->upid,reg_msg->target_id);
      return err;
  }
    /*wait for rx callback */
  wait_for_completion(&g_vdevcompressched->client_list[idx].work);

  *handle = g_vdevcompressched->client_list[idx].rxbuf.return_val.handle;

  if((*handle == 0) || (g_vdevcompressched->client_list[idx].rxbuf.return_val.err != 0))
  {
      LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched_register unsuccessful err = %d \n",g_vdevcompressched->client_list[idx].rxbuf.return_val.err);
      delete_client_table_entry(reg_msg->upid,reg_msg->target_id);
  }
  else
  {
      LOG_COMPRESSCHEDFE(LEVEL_INFO, " compressched_register completed handle = %x \n",*handle);
      g_vdevcompressched->client_list[idx].handle = *handle;
      handle_ht_insert(*handle, idx);
  }

  {
    int _err = (int)g_vdevcompressched->client_list[idx].rxbuf.return_val.err;
    return _err ? -_err : 0;
  }
}

/**
 * compressched_avail_res_acquire() - DP acquire available resource (GVM → PVM).
 *
 * Sends a COMPRESSCHED_AVAIL_RES_ACQUIRE_LIB virtio message to the PVM.
 * The PVM calls compressched_avail_res_acquire() from the DP minimal API
 * and returns the result in the RX buffer.
 *
 * @handle:    registered client handle (must be non-zero)
 * @cmd:       pointer to the DP command blob (compressched_dp_cmd_t)
 * @param:     optional override parameters; NULL means use the blob's own fields
 * @result:    output: filled with token, available_count, available_domain_id[]
 *             on success
 *
 * Returns 0 on success, negative errno on failure.
 */
int compressched_avail_res_acquire(compressched_handle handle,
				   const compressched_dp_cmd_t *cmd,
				   const compressched_dp_param_t *param,
				   compressched_dp_result_t *result)
{
	struct virtio_compressched_txbuf txbuf = {};
	int idx;
	int ret;

	if (cmd == NULL || result == NULL) {
		LOG_COMPRESSCHEDFE(LEVEL_ERR,
			" compressched_avail_res_acquire: invalid args\n");
		return -EINVAL;
	}

	idx = compressched_lookup(handle, "compressched_avail_res_acquire");
	if (idx < 0)
		return idx;

	txbuf.cmd    = COMPRESSCHED_AVAIL_RES_ACQUIRE_LIB;
	txbuf.msg_id = idx;

	/* Copy the blob into the TX payload */
	txbuf.send_data.avail_res_acquire_data.cmd = *cmd;

	/* Copy the optional override param */
	if (param != NULL) {
		txbuf.send_data.avail_res_acquire_data.param     = *param;
		txbuf.send_data.avail_res_acquire_data.has_param = 1;
	} else {
		txbuf.send_data.avail_res_acquire_data.has_param = 0;
	}

	ret = compressched_send_wait(&txbuf, idx, handle, "compressched_avail_res_acquire");
	if (ret == 0) {
		*result = g_vdevcompressched->client_list[idx].rxbuf.avail_res_result;
		LOG_COMPRESSCHEDFE(LEVEL_INFO,
			" compressched_avail_res_acquire token=0x%x count=%u\n",
			result->token, result->available_count);
	}
	return ret;
}

/**
 * compressched_avail_res_release() - DP release a reservation (GVM → PVM).
 *
 * Sends a COMPRESSCHED_AVAIL_RES_RELEASE_LIB virtio message to the PVM.
 * The PVM calls compressched_avail_res_release() from the DP minimal API.
 *
 * @handle:  registered client handle (must be non-zero)
 * @cmd:     pointer to the same DP command blob used for the matching acquire;
 *           the PVM reads only the embedded handle from it
 * @token:   reservation token from the prior acquire result
 *
 * Returns 0 on success, negative errno on failure.
 */
int compressched_avail_res_release(compressched_handle handle,
				   const compressched_dp_cmd_t *cmd,
				   unsigned int token)
{
	struct virtio_compressched_txbuf txbuf = {};
	int idx;

	if (cmd == NULL || token == 0) {
		LOG_COMPRESSCHEDFE(LEVEL_ERR,
			" compressched_avail_res_release: invalid args\n");
		return -EINVAL;
	}

	idx = compressched_lookup(handle, "compressched_avail_res_release");
	if (idx < 0)
		return idx;

	txbuf.cmd    = COMPRESSCHED_AVAIL_RES_RELEASE_LIB;
	txbuf.msg_id = idx;
	txbuf.send_data.avail_res_release_data.cmd   = *cmd;
	txbuf.send_data.avail_res_release_data.token = token;

	return compressched_send_wait(&txbuf, idx, handle, "compressched_avail_res_release");
}

int compressched_register_ex(compressched_handle* handle, unsigned int pid, unsigned int tid)
{
  struct virtio_compressched_txbuf txbuf = {};
  int idx = 0;
  int err = NO_ERROR;
  compressched_register_ex_msg reg_msg_ex;

  if (handle == NULL)
  {
      LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched_register_ex unsuccessful. Invalid Args \n");
      return -EINVAL;
  }
  if (g_vdevcompressched == NULL)
  {
      LOG_COMPRESSCHEDFE(LEVEL_ERR, "compressched_register_ex unsuccessful. No device\n");
      return -ENODEV;
  }

  reg_msg_ex.pid = pid;
  reg_msg_ex.tid = tid;

  idx = add_client_table_entry_ex(reg_msg_ex.pid, reg_msg_ex.tid);
  if(idx >= MAX_CLIENT)
  {
    /* No available client table entries */
    err = ENOSPC;
    LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched_register_ex unsuccessful. reached max GVM Clients. err = %d \n", err);
    return -err;
  }

  LOG_COMPRESSCHEDFE(LEVEL_INFO, " compressched_register_ex start msgid = %d pid = %u tid = %u\n", idx, reg_msg_ex.pid, reg_msg_ex.tid);
  txbuf.msg_id = idx;
  txbuf.cmd = COMPRESSCHED_REGISTER_EX_LIB;
  txbuf.send_data.register_ex_data.register_ex_msg.pid = reg_msg_ex.pid;
  txbuf.send_data.register_ex_data.register_ex_msg.tid = reg_msg_ex.tid;

  init_completion(&g_vdevcompressched->client_list[idx].work);
  err = virt_compressched_txbuf(&txbuf);
  if(err != NO_ERROR)
  {
      LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched_register_ex unsuccessful. TX Failed: %d \n", err);
      delete_client_table_entry_by_idx(idx);
      return err;
  }

  /* wait for rx callback */
  wait_for_completion(&g_vdevcompressched->client_list[idx].work);

  *handle = g_vdevcompressched->client_list[idx].rxbuf.return_val.handle;

  if((*handle == 0) || (g_vdevcompressched->client_list[idx].rxbuf.return_val.err != 0))
  {
      LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched_register_ex unsuccessful err = %d \n", g_vdevcompressched->client_list[idx].rxbuf.return_val.err);
      delete_client_table_entry_by_idx(idx);
  }
  else
  {
      LOG_COMPRESSCHEDFE(LEVEL_INFO, " compressched_register_ex completed handle = %x \n", *handle);
      g_vdevcompressched->client_list[idx].handle = *handle;
      handle_ht_insert(*handle, idx);
  }

  {
    int _err = (int)g_vdevcompressched->client_list[idx].rxbuf.return_val.err;
    return _err ? -_err : 0;
  }
}
