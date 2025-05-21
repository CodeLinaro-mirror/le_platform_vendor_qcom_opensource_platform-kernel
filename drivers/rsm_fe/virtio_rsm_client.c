/* SPDX-License-Identifier: GPL-2.0-only
*
*  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. 
*/
#include "virtio_rsm_base.h"


static int find_free_client_table_idx(void)
{
    int idx = 0;
    spin_lock(&g_vdevrsm->vq_clientlock);
    while(g_vdevrsm->client_list[idx].upid != 0)
    {
        idx++;
    }
    spin_unlock(&g_vdevrsm->vq_clientlock);
    return idx;
}

static int find_client_table_entry(rsm_handle handle)
{
    int idx = 0;
    spin_lock(&g_vdevrsm->vq_clientlock);
    for (idx = 0; idx < MAX_CLIENT; idx ++)
    {
        if(g_vdevrsm->client_list[idx].handle == handle)
        {
            break;
        }
    }
    spin_unlock(&g_vdevrsm->vq_clientlock);
    return idx;
}

static void delete_client_table_entry(unsigned int upid, unsigned int tid)
{
    int idx = 0;
    spin_lock(&g_vdevrsm->vq_clientlock);
    for (idx = 0; idx < MAX_CLIENT; idx ++)
    {
        if((g_vdevrsm->client_list[idx].upid == upid)&&(g_vdevrsm->client_list[idx].tid == tid))
        {
            memset(&g_vdevrsm->client_list[idx], 0, sizeof(struct rsm_client_table));
            break;
        }
    }
    spin_unlock(&g_vdevrsm->vq_clientlock);
}
int rsm_register(rsm_handle* handle, unsigned int upid, unsigned int tid)
{
    struct virtio_rsm_txbuf *txbuf = NULL;
    int idx = 0;
    int err = NO_ERROR;
    if (handle != NULL)
    {
        txbuf = kzalloc(sizeof(struct virtio_rsm_txbuf), GFP_KERNEL);
        txbuf->cmd = RSM_REGISTER;
        txbuf->send_data.register_data.upid = upid;
        txbuf->send_data.register_data.tid = tid;
        /*add client to client table*/
        idx = find_free_client_table_idx();
        LOG_RSMFE(LEVEL_INFO, " rsm_register start msgid = %d \n",idx);
        g_vdevrsm->client_list[idx].upid = upid;
        g_vdevrsm->client_list[idx].tid = tid;

        txbuf->msg_id = idx;
        init_completion(&g_vdevrsm->client_list[idx].work);
        err = virt_rsm_txbuf(txbuf);
        if(err != NO_ERROR)
        {
            kfree(txbuf);
            return err;
        }
        /*wait for rx callback */
        wait_for_completion(&g_vdevrsm->client_list[idx].work);

        *handle = g_vdevrsm->client_list[idx].rxbuf.return_val.handle;

        if((*handle == 0)&&(g_vdevrsm->client_list[idx].rxbuf.return_val.err != 0))
        {
            delete_client_table_entry(upid,tid);
            LOG_RSMFE(LEVEL_ERR, " rsm_register unsuccessful err = %d \n",g_vdevrsm->client_list[idx].rxbuf.return_val.err);
        }
        else
        {
            LOG_RSMFE(LEVEL_INFO, " rsm_register completed handle = %d \n",*handle);
            g_vdevrsm->client_list[idx].handle = *handle;
        }
        
        kfree(txbuf);
        return g_vdevrsm->client_list[idx].rxbuf.return_val.err;
    }
    return ERROR;
}

int rsm_acquire(rsm_handle handle, char* job_name, rsm_acquire_rsp_v2 *response)
{
    struct virtio_rsm_txbuf *txbuf = NULL;
    int idx = 0;
    int err = NO_ERROR;
    if ((job_name != NULL) && (handle != 0))
    {
        txbuf = kzalloc(sizeof(struct virtio_rsm_txbuf), GFP_KERNEL);
        txbuf->cmd = RSM_ACQUIRE;
        txbuf->send_data.acquire_data.handle = handle;
       // memcpy(txbuf->send_data.acquire_data.job_name , job_name , sizeof(*job_name)) ;
        /*find client in client table*/
        idx = find_client_table_entry(handle);

        txbuf->msg_id = idx;
        init_completion(&g_vdevrsm->client_list[idx].work);
        err = virt_rsm_txbuf(txbuf);
        if(err != NO_ERROR)
        {
            kfree(txbuf);
            return err;
        }
        /*wait for rx callback */
        wait_for_completion(&g_vdevrsm->client_list[idx].work);

        memcpy(response, &g_vdevrsm->client_list[idx].rxbuf.acq_rsp ,sizeof(rsm_acquire_rsp_v2)) ;

        if((g_vdevrsm->client_list[idx].rxbuf.return_val.err != 0))
        {
            LOG_RSMFE(LEVEL_ERR, " rsm_acquire unsuccessful err = %d \n",g_vdevrsm->client_list[idx].rxbuf.return_val.err);
        }
        else
        {
            LOG_RSMFE(LEVEL_INFO, " rsm_acquire completed handle = %d \n",handle);
        }
        
        kfree(txbuf);
        return g_vdevrsm->client_list[idx].rxbuf.return_val.err;
    }
    return ERROR;
}

int rsm_release_v2(rsm_handle handle, rsm_token token)
{
    struct virtio_rsm_txbuf *txbuf = NULL;
    int idx = 0;
    int err = NO_ERROR;
    if (handle != 0)
    {
        txbuf = kzalloc(sizeof(struct virtio_rsm_txbuf), GFP_KERNEL);
        txbuf->cmd = RSM_RELEASE;
        txbuf->send_data.release_data.handle = handle;
        txbuf->send_data.release_data.token = token;
        /*find client in client table*/
        idx = find_client_table_entry(handle);

        txbuf->msg_id = idx;
        init_completion(&g_vdevrsm->client_list[idx].work);
        err = virt_rsm_txbuf(txbuf);
        if(err != NO_ERROR)
        {
            kfree(txbuf);
            return err;
        }
        /*wait for rx callback */
        wait_for_completion(&g_vdevrsm->client_list[idx].work);

        if((g_vdevrsm->client_list[idx].rxbuf.return_val.err != 0))
        {
            LOG_RSMFE(LEVEL_ERR, " rsm_release unsuccessful err = %d \n",g_vdevrsm->client_list[idx].rxbuf.return_val.err);
        }
        else
        {
            LOG_RSMFE(LEVEL_INFO, " rsm_release completed handle = %d \n",handle);
        }
        
        kfree(txbuf);
        return g_vdevrsm->client_list[idx].rxbuf.return_val.err;
    }
    return ERROR;
}

int rsm_unregister_v2(rsm_handle handle)
{
    struct virtio_rsm_txbuf *txbuf = NULL;
    int idx = 0;
    int err = NO_ERROR;
    if (handle != 0)
    {
        txbuf = kzalloc(sizeof(struct virtio_rsm_txbuf), GFP_KERNEL);
        txbuf->cmd = RSM_UNREGISTER;
        txbuf->send_data.unregister_data.handle = handle;
        /*find client in client table*/
        idx = find_client_table_entry(handle);

        txbuf->msg_id = idx;
        init_completion(&g_vdevrsm->client_list[idx].work);
        err = virt_rsm_txbuf(txbuf);
        if(err != NO_ERROR)
        {
            kfree(txbuf);
            return err;
        }
        /*wait for rx callback */
        wait_for_completion(&g_vdevrsm->client_list[idx].work);

        if(g_vdevrsm->client_list[idx].rxbuf.return_val.err != 0)
        {
            LOG_RSMFE(LEVEL_ERR, " rsm_unregister unsuccessful err = %d \n",g_vdevrsm->client_list[idx].rxbuf.return_val.err);
        }
        else
        {
            delete_client_table_entry(g_vdevrsm->client_list[idx].upid,g_vdevrsm->client_list[idx].tid);
            LOG_RSMFE(LEVEL_INFO, " rsm_unregister completed handle = %d \n",handle);
        }
        
        kfree(txbuf);
        return g_vdevrsm->client_list[idx].rxbuf.return_val.err;
    }
    return ERROR;
}
int rsm_unregister_batch(unsigned int upid)
{
    /********to be implemented********/
    return 0;
}