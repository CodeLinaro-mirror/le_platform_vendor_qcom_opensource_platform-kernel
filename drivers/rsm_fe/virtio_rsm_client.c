/* SPDX-License-Identifier: GPL-2.0-only
*
*  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. 
*/
#include "virtio_rsm_base.h"
#define RSMFE_MAX_NSP_COUNT 2U /* TODO: This can probably be retrieved from the vdev during probe and populated */

/* Check and returns a valid index
 * If already registered returns MAX_CLIENT + 1
 * if table full returns MAX_CLIENT
*/
static int add_client_table_entry(unsigned int upid, unsigned int tid)
{
    int idx = 0;
    spin_lock(&g_vdevrsm->vq_clientlock);
    while(idx < MAX_CLIENT)
    {
        if(g_vdevrsm->client_list[idx].upid != 0)
        {
            if((g_vdevrsm->client_list[idx].upid == upid)&&(g_vdevrsm->client_list[idx].tid == tid))
            {
                idx = MAX_CLIENT + 1; //Already registered. Shall not add duplicate entry.
                break;
            }
            ++idx;
        }
        else
        {
            g_vdevrsm->client_list[idx].upid = upid;
            g_vdevrsm->client_list[idx].tid = tid;
            g_vdevrsm->client_list[idx].rxbuf.return_val.err = 0; /* Reset the error */
            break;
        }
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
    /* Default the existing API to NSP 0 */
    RSMRegisterPDUType rsmRegData = {upid, tid, 0U};
    return rsm_register_for_nsp(handle, rsmRegData);
}

int rsm_register_for_nsp(rsm_handle* handle, RSMRegisterPDUType rsmRegData)
{
    struct virtio_rsm_txbuf *txbuf = NULL;
    int idx = 0;
    int err = NO_ERROR;

    if ((handle == NULL) || (rsmRegData.upid == 0U) || (rsmRegData.nspID >= RSMFE_MAX_NSP_COUNT))
    {
        LOG_RSMFE(LEVEL_ERR, " rsm_register unsuccessful. Invalid Args \n");
        return -EINVAL;
    }
    if (g_vdevrsm == NULL)
    {
        LOG_RSMFE(LEVEL_ERR, "rsm_register unsuccessful. No device\n");
        return -ENODEV;
    }

    /*add client to client table*/
    idx = add_client_table_entry(rsmRegData.upid, rsmRegData.tid);
    if(idx >= MAX_CLIENT)
    {
        if(idx > MAX_CLIENT)
        {
            err = EEXIST;
            LOG_RSMFE(LEVEL_ERR, " rsm_register unsuccessful. Already registered. err = %d \n",err);
            return -err;
        }
        /* No available client table entries*/
        err = ENOSPC;
        LOG_RSMFE(LEVEL_ERR, " rsm_register unsuccessful. reached max GVM Clients. err = %d \n",err);
        return -err;
    }

    txbuf = kzalloc(sizeof(struct virtio_rsm_txbuf), GFP_KERNEL);
    if(NULL == txbuf)
    {
        /* Mem alloc error */
        err = ENOMEM;
        LOG_RSMFE(LEVEL_ERR, " rsm_register unsuccessful. out of memory. err = %d \n",err);
        /* release the client index */
        delete_client_table_entry(rsmRegData.upid, rsmRegData.tid);
        return -err;
    }

    LOG_RSMFE(LEVEL_INFO, " rsm_register start msgid = %d \n",idx);
    txbuf->cmd = RSM_REGISTER;
    txbuf->send_data.register_data.upid = rsmRegData.upid;
    txbuf->send_data.register_data.tid = rsmRegData.tid;
    txbuf->send_data.register_data.nspID = rsmRegData.nspID;
    txbuf->msg_id = idx;
    init_completion(&g_vdevrsm->client_list[idx].work);
    err = virt_rsm_txbuf(txbuf);
    if(err != NO_ERROR)
    {
        LOG_RSMFE(LEVEL_ERR, " rsm_register unsuccessful. TX Failed: %d \n", err);
        /* Send was unsuccessful so release the client index */
        delete_client_table_entry(rsmRegData.upid, rsmRegData.tid);
        kfree(txbuf);
        return err;
    }
    /*wait for rx callback */
    wait_for_completion(&g_vdevrsm->client_list[idx].work);

    *handle = g_vdevrsm->client_list[idx].rxbuf.return_val.handle;

    if((*handle == 0) || (g_vdevrsm->client_list[idx].rxbuf.return_val.err != 0))
    {
        LOG_RSMFE(LEVEL_ERR, " rsm_register unsuccessful err = %d \n",g_vdevrsm->client_list[idx].rxbuf.return_val.err);
        err = g_vdevrsm->client_list[idx].rxbuf.return_val.err;
        delete_client_table_entry(rsmRegData.upid, rsmRegData.tid);
    }
    else
    {
        LOG_RSMFE(LEVEL_INFO, " rsm_register completed handle = %x \n",*handle);
        g_vdevrsm->client_list[idx].handle = *handle;
    }

    kfree(txbuf);
    return -err;
}

int rsm_acquire(rsm_handle handle, const char *job_name, rsm_acquire_rsp_v2 *response)
{
    struct virtio_rsm_txbuf *txbuf = NULL;
    int idx = 0;
    int err = NO_ERROR;
    ssize_t strCpyRet = 0, kerCpyRet = 0;
    if ((job_name == NULL) || (handle == 0))
    {
        LOG_RSMFE(LEVEL_ERR, " rsm_acquire unsuccessful. invalid inputs \n");
        return -EINVAL;
    }
    if (g_vdevrsm == NULL)
    {
        LOG_RSMFE(LEVEL_ERR, "rsm_acquire unsuccessful. No device\n");
        return -ENODEV;
    }
    /*find client in client table*/
    idx = find_client_table_entry(handle);
    if(idx >= MAX_CLIENT)
    {
        /* No matching client table entries*/
        err = EBADF;
        LOG_RSMFE(LEVEL_ERR, " rsm_acquire unsuccessful. invalid FE handle %x. err = %d \n", handle, err);
        return -err;
    }

    txbuf = kzalloc(sizeof(struct virtio_rsm_txbuf), GFP_KERNEL);
    if(NULL == txbuf)
    {
        /* Mem alloc error */
        err = ENOMEM;
        LOG_RSMFE(LEVEL_ERR, " rsm_acquire unsuccessful. out of memory. handle %x err = %d \n", handle, err);
        return -err;
    }
    txbuf->cmd = RSM_ACQUIRE;
    txbuf->send_data.acquire_data.handle = handle;

    //Use this if one we confirm the job_name is a kernel space pointer
    //strscpy(txbuf->send_data.acquire_data.job_name , job_name , sizeof(txbuf->send_data.acquire_data.job_name));
    strCpyRet = strncpy_from_user(txbuf->send_data.acquire_data.job_name, (const char __user *)job_name, sizeof(txbuf->send_data.acquire_data.job_name));
    if (strCpyRet < 0)
    {
        if((strCpyRet == -EFAULT) &&
            // Failed as userspace, try as kernel pointer
            // Check if it looks like a kernel address
            ((unsigned long)job_name >= TASK_SIZE))
        {
            // Likely a kernel pointer, use kernel copy
            kerCpyRet = strscpy(txbuf->send_data.acquire_data.job_name, job_name, sizeof(txbuf->send_data.acquire_data.job_name));
            if(kerCpyRet < 0)
            {
                LOG_RSMFE(LEVEL_ERR, " rsm_acquire unsuccessful. Invalid Jobname Kernel Pointer. handle %x err = %d \n", handle, -kerCpyRet);
                kfree(txbuf);
                return kerCpyRet;
            }
            // strscpy returns length, make it consistent with strncpy_from_user
            strCpyRet = strlen(job_name);
        }
        else
        {
            LOG_RSMFE(LEVEL_ERR, " rsm_acquire unsuccessful. Invalid Jobname Pointer. handle %x err = %d \n", handle, -strCpyRet);
            kfree(txbuf);
            return strCpyRet;
        }
    }
    if (strCpyRet >= sizeof(txbuf->send_data.acquire_data.job_name))
    {
        // String was truncated but is still null-terminated
        err = E2BIG;
        LOG_RSMFE(LEVEL_ERR, " rsm_acquire unsuccessful. Jobname too long (%d). handle %x err = %d \n", strCpyRet, handle, err);
        kfree(txbuf);
        return -err;
    }

    txbuf->msg_id = idx;
    g_vdevrsm->client_list[idx].rxbuf.return_val.err = 0;
    init_completion(&g_vdevrsm->client_list[idx].work);
    err = virt_rsm_txbuf(txbuf);
    if(err != NO_ERROR)
    {
        LOG_RSMFE(LEVEL_ERR, " rsm_acquire unsuccessful. TX Failed. handle %x err = %d \n", handle, err);
        kfree(txbuf);
        return err;
    }
    /*wait for rx callback */
    wait_for_completion(&g_vdevrsm->client_list[idx].work);
    memcpy(response, &g_vdevrsm->client_list[idx].rxbuf.acq_rsp ,sizeof(rsm_acquire_rsp_v2)) ;

    if(g_vdevrsm->client_list[idx].rxbuf.return_val.err != 0)
    {
        LOG_RSMFE(LEVEL_ERR, " rsm_acquire unsuccessful. handle %x err = %d \n", handle, g_vdevrsm->client_list[idx].rxbuf.return_val.err);
    }
    else
    {
        LOG_RSMFE(LEVEL_INFO, " rsm_acquire completed handle = %x \n", handle);
    }

    kfree(txbuf);
    return g_vdevrsm->client_list[idx].rxbuf.return_val.err;
}

int rsm_release_v2(rsm_handle handle, rsm_token token)
{
    struct virtio_rsm_txbuf *txbuf = NULL;
    int idx = 0;
    int err = NO_ERROR;
    if ((handle == 0) || (token == 0))
    {
        LOG_RSMFE(LEVEL_ERR, " rsm_release unsuccessful. invalid inputs \n");
        return -EINVAL;
    }
    if (g_vdevrsm == NULL)
    {
        LOG_RSMFE(LEVEL_ERR, "rsm_release unsuccessful. No device\n");
        return -ENODEV;
    }
    /*find client in client table*/
    idx = find_client_table_entry(handle);
    if(idx >= MAX_CLIENT)
    {
        /* No matching the client table entries*/
        err = EBADF;
        LOG_RSMFE(LEVEL_ERR, " rsm_release unsuccessful. invalid FE handle %x. err = %d \n", handle, err);
        return -err;
    }

    txbuf = kzalloc(sizeof(struct virtio_rsm_txbuf), GFP_KERNEL);
    if(NULL == txbuf)
    {
        /* Mem alloc error */
        err = ENOMEM;
        LOG_RSMFE(LEVEL_ERR, " rsm_release unsuccessful. out of memory. handle %x err = %d \n", handle, err);
        return -err;
    }
    txbuf->cmd = RSM_RELEASE;
    txbuf->send_data.release_data.handle = handle;
    txbuf->send_data.release_data.token = token;
    txbuf->msg_id = idx;
    g_vdevrsm->client_list[idx].rxbuf.return_val.err = 0;
    init_completion(&g_vdevrsm->client_list[idx].work);
    err = virt_rsm_txbuf(txbuf);
    if(err != NO_ERROR)
    {
        LOG_RSMFE(LEVEL_ERR, " rsm_release unsuccessful. TX Failed. handle %x err = %d \n", handle, err);
        kfree(txbuf);
        return err;
    }

    /*wait for rx callback */
    wait_for_completion(&g_vdevrsm->client_list[idx].work);
    if(g_vdevrsm->client_list[idx].rxbuf.return_val.err != 0)
    {
        LOG_RSMFE(LEVEL_ERR, " rsm_release unsuccessful. hanele %x err = %d \n", handle, g_vdevrsm->client_list[idx].rxbuf.return_val.err);
    }
    else
    {
        LOG_RSMFE(LEVEL_INFO, " rsm_release completed handle = %d \n",handle);
    }
    
    kfree(txbuf);
    return g_vdevrsm->client_list[idx].rxbuf.return_val.err;
}

int rsm_unregister_v2(rsm_handle handle)
{
    struct virtio_rsm_txbuf *txbuf = NULL;
    int idx = 0;
    int err = NO_ERROR;
    if (handle == 0)
    {
        LOG_RSMFE(LEVEL_ERR, " rsm_unregister unsuccessful. invalid inputs \n");
        return -EINVAL;
    }
    if (g_vdevrsm == NULL)
    {
        LOG_RSMFE(LEVEL_ERR, "rsm_unregister unsuccessful. No device\n");
        return -ENODEV;
    }

    /*find client in client table*/
    idx = find_client_table_entry(handle);
    if(idx >= MAX_CLIENT)
    {
        /* No matching the client table entries*/
        err = EBADF;
        LOG_RSMFE(LEVEL_ERR, " rsm_unregister unsuccessful. invalid FE handle %x. err = %d \n", handle, err);
        return -err;
    }

    txbuf = kzalloc(sizeof(struct virtio_rsm_txbuf), GFP_KERNEL);
    if(NULL == txbuf)
    {
        /* Mem alloc error */
        err = ENOMEM;
        LOG_RSMFE(LEVEL_ERR, " rsm_unregister unsuccessful. out of memory. handle %x err = %d \n", handle, err);
        return -err;
    }

    txbuf->cmd = RSM_UNREGISTER;
    txbuf->send_data.unregister_data.handle = handle;
    txbuf->msg_id = idx;
    g_vdevrsm->client_list[idx].rxbuf.return_val.err = 0;
    init_completion(&g_vdevrsm->client_list[idx].work);
    err = virt_rsm_txbuf(txbuf);
    if(err != NO_ERROR)
    {
        LOG_RSMFE(LEVEL_ERR, " rsm_unregister unsuccessful. TX Failed. handle %x err = %d \n", handle, err);
        kfree(txbuf);
        return err;
    }
    /*wait for rx callback */
    wait_for_completion(&g_vdevrsm->client_list[idx].work);
    if(g_vdevrsm->client_list[idx].rxbuf.return_val.err != 0)
    {
        LOG_RSMFE(LEVEL_ERR, " rsm_unregister unsuccessful. handle %x err = %d \n", handle, g_vdevrsm->client_list[idx].rxbuf.return_val.err);
        err = g_vdevrsm->client_list[idx].rxbuf.return_val.err;
    }
    else
    {
        delete_client_table_entry(g_vdevrsm->client_list[idx].upid,g_vdevrsm->client_list[idx].tid);
        LOG_RSMFE(LEVEL_INFO, " rsm_unregister completed. handle %x \n", handle);
    }

    kfree(txbuf);
    return -err;
}

int rsm_unregister_batch(unsigned int upid)
{
    /********to be implemented********/
    LOG_RSMFE(LEVEL_ERR, " rsm_unregister_batch unsuccessful. Not implemented \n");
    return EINVAL;
}