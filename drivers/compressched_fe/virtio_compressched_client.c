/* SPDX-License-Identifier: GPL-2.0-only
*
*  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. 
*/
#include "virtio_compressched_base.h"

/* Check and returns a valid index
 * If already registered returns MAX_CLIENT + 1
 * if table full returns MAX_CLIENT
*/
static int add_client_table_entry(unsigned int upid, unsigned int tid)
{
    int idx = 0;
    spin_lock(&g_vdevcompressched->vq_clientlock);
    while(idx < MAX_CLIENT)
    {
        if(g_vdevcompressched->client_list[idx].upid != 0)
        {
            if((g_vdevcompressched->client_list[idx].upid == upid)&&(g_vdevcompressched->client_list[idx].tid == tid))
            {
                idx = MAX_CLIENT + 1; //Already registered. Shall not add duplicate entry.
                break;
            }
            ++idx;
        }
        else
        {
            g_vdevcompressched->client_list[idx].upid = upid;
            g_vdevcompressched->client_list[idx].tid = tid;
            g_vdevcompressched->client_list[idx].rxbuf.return_val.err = 0; /* Reset the error */
            break;
        }
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

static void delete_client_table_entry(unsigned int upid, unsigned int tid)
{
    int idx = 0;
    spin_lock(&g_vdevcompressched->vq_clientlock);
    for (idx = 0; idx < MAX_CLIENT; idx ++)
    {
        if((g_vdevcompressched->client_list[idx].upid == upid)&&(g_vdevcompressched->client_list[idx].tid == tid))
        {
            memset(&g_vdevcompressched->client_list[idx], 0, sizeof(struct compressched_client_table));
            break;
        }
    }
    spin_unlock(&g_vdevcompressched->vq_clientlock);
}

int compressched_register(compressched_handle* handle, unsigned int upid, unsigned int tid)
{
    struct virtio_compressched_txbuf *txbuf = NULL;
    int idx = 0;
    int err = NO_ERROR;

    if ((handle == NULL) || (upid == 0))
    {
        LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched_register unsuccessful. Invalid Args \n");
        return -EINVAL;
    }
    if (g_vdevcompressched == NULL)
    {
        LOG_COMPRESSCHEDFE(LEVEL_ERR, "compressched_register unsuccessful. No device\n");
        return -ENODEV;
    }

    /*add client to client table*/
    idx = add_client_table_entry(upid, tid);
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
        delete_client_table_entry(upid,tid);
        return -err;
    }

    LOG_COMPRESSCHEDFE(LEVEL_INFO, " compressched_register start msgid = %d \n",idx);
    txbuf->cmd = COMPRESSCHED_REGISTER;
    txbuf->send_data.register_data.upid = upid;
    txbuf->send_data.register_data.tid = tid;
    txbuf->msg_id = idx;
    init_completion(&g_vdevcompressched->client_list[idx].work);
    err = virt_compressched_txbuf(txbuf);
    if(err != NO_ERROR)
    {
        LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched_register unsuccessful. TX Failed: %d \n", err);
        /* Send was unsuccessful so release the client index */
        delete_client_table_entry(upid,tid);
        kfree(txbuf);
        return err;
    }
    /*wait for rx callback */
    wait_for_completion(&g_vdevcompressched->client_list[idx].work);

    *handle = g_vdevcompressched->client_list[idx].rxbuf.return_val.handle;

    if((*handle == 0) || (g_vdevcompressched->client_list[idx].rxbuf.return_val.err != 0))
    {
        LOG_COMPRESSCHEDFE(LEVEL_ERR, " compressched_register unsuccessful err = %d \n",g_vdevcompressched->client_list[idx].rxbuf.return_val.err);
        delete_client_table_entry(upid,tid);
    }
    else
    {
        LOG_COMPRESSCHEDFE(LEVEL_INFO, " compressched_register completed handle = %x \n",*handle);
        g_vdevcompressched->client_list[idx].handle = *handle;
    }
    
    kfree(txbuf);
    return g_vdevcompressched->client_list[idx].rxbuf.return_val.err;
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
    //memcpy(txbuf->send_data.acquire_data.job_name , job_name , sizeof(*job_name)) ;
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
        delete_client_table_entry(g_vdevcompressched->client_list[idx].upid,g_vdevcompressched->client_list[idx].tid);
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
