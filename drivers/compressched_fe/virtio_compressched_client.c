/* SPDX-License-Identifier: GPL-2.0-only
*
*  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. 
*/
#include "virtio_compressched_base.h"

/* Check and returns a valid index
 * If already registered returns MAX_CLIENT + 1
 * if table full returns MAX_CLIENT
*/
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

static int find_client_table_entry(compressched_handle handle)
{
    int idx = 0;
    spin_lock(&g_vdevcompressched->vq_clientlock);
    for (idx = 0; idx < MAX_CLIENT; idx ++)
    {
        if(g_vdevcompressched->client_list[idx].handle == handle)
        {
            break;
        }
    }
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
        memset(&g_vdevcompressched->client_list[idx], 0, sizeof(struct compressched_client_table));
        memset(g_vdevcompressched->client_list[idx].upid, INVALID_UPID, MAX_NUM_OF_NSP * sizeof(unsigned int));
        break;
      }
    }
    spin_unlock(&g_vdevcompressched->vq_clientlock);
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

int compressched_acquire(compressched_handle handle, char* job_name, compressched_acquire_rsp_v2 *response)
{
    struct virtio_compressched_txbuf *txbuf = NULL;
    int idx = 0;
    int err = NO_ERROR;
    if ((job_name == NULL) || (handle == 0))
    {
        LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched_acquire unsuccessful. invalid inputs \n");
        return -EINVAL;
    }
    if (g_vdevcompressched == NULL)
    {
        LOG_COMPRESSCHEDFE(LEVEL_ERR, "compressched_acquire unsuccessful. No device\n");
        return -ENODEV;
    }
    /*find client in client table*/
    idx = find_client_table_entry(handle);
    if(idx >= MAX_CLIENT)
    {
        /* No matching client table entries*/
        err = EBADF;
        LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched_acquire unsuccessful. invalid FE handle %x. err = %d \n", handle, err);
        return -err;
    }

    txbuf = kzalloc(sizeof(struct virtio_compressched_txbuf), GFP_KERNEL);
    if(NULL == txbuf)
    {
        /* Mem alloc error */
        err = ENOMEM;
        LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched_acquire unsuccessful. out of memory. handle %x err = %d \n", handle, err);
        return -err;
    }
    txbuf->cmd = COMPRESSCHED_ACQUIRE;
    txbuf->send_data.acquire_data.handle = handle;
    memcpy(txbuf->send_data.acquire_data.job_name , job_name , sizeof(txbuf->send_data.acquire_data.job_name)) ;
    txbuf->msg_id = idx;
    g_vdevcompressched->client_list[idx].rxbuf.return_val.err = 0;
    init_completion(&g_vdevcompressched->client_list[idx].work);
    err = virt_compressched_txbuf(txbuf);
    if(err != NO_ERROR)
    {
        LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched_acquire unsuccessful. TX Failed. handle %x err = %d \n", handle, err);
        kfree(txbuf);
        return err;
    }
    /*wait for rx callback */
    wait_for_completion(&g_vdevcompressched->client_list[idx].work);
    memcpy(response, &g_vdevcompressched->client_list[idx].rxbuf.acq_rsp ,sizeof(compressched_acquire_rsp_v2)) ;

    if(g_vdevcompressched->client_list[idx].rxbuf.return_val.err != 0)
    {
        LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched_acquire unsuccessful. handle %x err = %d \n", handle, g_vdevcompressched->client_list[idx].rxbuf.return_val.err);
    }
    else
    {
        LOG_COMPRESSCHEDFE(LEVEL_INFO, " compressched_acquire completed handle = %x \n", handle);
    }

    kfree(txbuf);
    return g_vdevcompressched->client_list[idx].rxbuf.return_val.err;
}

int compressched_release_v2(compressched_handle handle, compressched_token token)
{
    struct virtio_compressched_txbuf *txbuf = NULL;
    int idx = 0;
    int err = NO_ERROR;
    if ((handle == 0) || (token == 0))
    {
        LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched_release unsuccessful. invalid inputs \n");
        return -EINVAL;
    }
    if (g_vdevcompressched == NULL)
    {
        LOG_COMPRESSCHEDFE(LEVEL_ERR, "compressched_release unsuccessful. No device\n");
        return -ENODEV;
    }
    /*find client in client table*/
    idx = find_client_table_entry(handle);
    if(idx >= MAX_CLIENT)
    {
        /* No matching the client table entries*/
        err = EBADF;
        LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched_release unsuccessful. invalid FE handle %x. err = %d \n", handle, err);
        return -err;
    }

    txbuf = kzalloc(sizeof(struct virtio_compressched_txbuf), GFP_KERNEL);
    if(NULL == txbuf)
    {
        /* Mem alloc error */
        err = ENOMEM;
        LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched_release unsuccessful. out of memory. handle %x err = %d \n", handle, err);
        return -err;
    }
    txbuf->cmd = COMPRESSCHED_RELEASE;
    txbuf->send_data.release_data.handle = handle;
    txbuf->send_data.release_data.token = token;
    txbuf->msg_id = idx;
    g_vdevcompressched->client_list[idx].rxbuf.return_val.err = 0;
    init_completion(&g_vdevcompressched->client_list[idx].work);
    err = virt_compressched_txbuf(txbuf);
    if(err != NO_ERROR)
    {
        LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched_release unsuccessful. TX Failed. handle %x err = %d \n", handle, err);
        kfree(txbuf);
        return err;
    }

    /*wait for rx callback */
    wait_for_completion(&g_vdevcompressched->client_list[idx].work);
    if(g_vdevcompressched->client_list[idx].rxbuf.return_val.err != 0)
    {
        LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched_release unsuccessful. hanele %x err = %d \n", handle, g_vdevcompressched->client_list[idx].rxbuf.return_val.err);
    }
    else
    {
        LOG_COMPRESSCHEDFE(LEVEL_INFO, " compressched_release completed handle = %d \n",handle);
    }
    
    kfree(txbuf);
    return g_vdevcompressched->client_list[idx].rxbuf.return_val.err;
}

int compressched_unregister_v2(compressched_handle handle)
{
    struct virtio_compressched_txbuf *txbuf = NULL;
    int idx = 0;
    int err = NO_ERROR;
    if (handle == 0)
    {
        LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched_unregister unsuccessful. invalid inputs \n");
        return -EINVAL;
    }
    if (g_vdevcompressched == NULL)
    {
        LOG_COMPRESSCHEDFE(LEVEL_ERR, "compressched_unregister unsuccessful. No device\n");
        return -ENODEV;
    }

    /*find client in client table*/
    idx = find_client_table_entry(handle);
    if(idx >= MAX_CLIENT)
    {
        /* No matching the client table entries*/
        err = EBADF;
        LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched_unregister unsuccessful. invalid FE handle %x. err = %d \n", handle, err);
        return -err;
    }

    txbuf = kzalloc(sizeof(struct virtio_compressched_txbuf), GFP_KERNEL);
    if(NULL == txbuf)
    {
        /* Mem alloc error */
        err = ENOMEM;
        LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched_unregister unsuccessful. out of memory. handle %x err = %d \n", handle, err);
        return -err;
    }

    txbuf->cmd = COMPRESSCHED_UNREGISTER;
    txbuf->send_data.unregister_data.handle = handle;
    txbuf->msg_id = idx;
    g_vdevcompressched->client_list[idx].rxbuf.return_val.err = 0;
    init_completion(&g_vdevcompressched->client_list[idx].work);
    err = virt_compressched_txbuf(txbuf);
    if(err != NO_ERROR)
    {
        LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched_unregister unsuccessful. TX Failed. handle %x err = %d \n", handle, err);
        kfree(txbuf);
        return err;
    }
    /*wait for rx callback */
    wait_for_completion(&g_vdevcompressched->client_list[idx].work);
    if(g_vdevcompressched->client_list[idx].rxbuf.return_val.err != 0)
    {
        LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched_unregister unsuccessful. handle %x err = %d \n", handle, g_vdevcompressched->client_list[idx].rxbuf.return_val.err);
    }
    else
    {
        delete_client_table_entry(g_vdevcompressched->client_list[idx].upid,g_vdevcompressched->client_list[idx].target_id);
        LOG_COMPRESSCHEDFE(LEVEL_INFO, " compressched_unregister completed. handle %x \n", handle);
    }
    
    kfree(txbuf);
    return g_vdevcompressched->client_list[idx].rxbuf.return_val.err;
}

int compressched_unregister_batch(unsigned int upid)
{
    /********to be implemented********/
    LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched_unregister_batch unsuccessful. Not implemented \n");
    return EINVAL;
}


int compressched_register_v2(compressched_handle* handle, compressched_register_msg *reg_msg)
{
  struct virtio_compressched_txbuf *txbuf = NULL;
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
    if(idx > MAX_CLIENT)
    {
        err = EEXIST;
        LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched_register unsuccessful. Already registered. err = %d \n",err);
        return -err;
    }
    /* No available client table entries*/
    err = ENOSPC;
    LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched_register unsuccessful. reached max GVM Clients. err = %d \n",err);
    return -err;
  }
  txbuf = kzalloc(sizeof(struct virtio_compressched_txbuf), GFP_KERNEL);
  if(NULL == txbuf)
  {
      /* Mem alloc error */
      err = ENOMEM;
      LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched_register unsuccessful. out of memory. err = %d \n",err);
      /* release the client index */
      delete_client_table_entry(reg_msg->upid,reg_msg->target_id);
      return -err;
  }

  LOG_COMPRESSCHEDFE(LEVEL_INFO, " compressched_register start msgid = %d \n",idx);
  txbuf->msg_id = idx;
  txbuf->cmd = COMPRESSCHED_REGISTER;
  txbuf->send_data.register_data.register_msg.nsp_count = reg_msg->nsp_count;
  txbuf->send_data.register_data.register_msg.target_id = reg_msg->target_id;
      LOG_COMPRESSCHEDFE(LEVEL_ERR, " MARK3:%d,%d \n", txbuf->send_data.register_data.register_msg.nsp_count,reg_msg->nsp_count);
    LOG_COMPRESSCHEDFE(LEVEL_ERR, " MARK4:%d,%d \n", txbuf->send_data.register_data.register_msg.target_id,reg_msg->target_id);

  for(index = 0; index < MAX_NUM_OF_NSP;index++)
  {
    txbuf->send_data.register_data.register_msg.upid[index] = reg_msg->upid[index];
    txbuf->send_data.register_data.register_msg.logical_id[index] = reg_msg->logical_id[index];
    LOG_COMPRESSCHEDFE(LEVEL_ERR, " MARK1:%d,%d \n", txbuf->send_data.register_data.register_msg.upid[index],reg_msg->upid[index]);
    LOG_COMPRESSCHEDFE(LEVEL_ERR, " MARK2:%d,%d \n", txbuf->send_data.register_data.register_msg.logical_id[index],reg_msg->logical_id[index]);

  }
  init_completion(&g_vdevcompressched->client_list[idx].work);
  err = virt_compressched_txbuf(txbuf);
  if(err != NO_ERROR)
  {
      LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched_register unsuccessful. TX Failed: %d \n", err);
      /* Send was unsuccessful so release the client index */
      delete_client_table_entry(reg_msg->upid,reg_msg->target_id);
      kfree(txbuf);
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
  }
    
  kfree(txbuf);
  return g_vdevcompressched->client_list[idx].rxbuf.return_val.err;
}
