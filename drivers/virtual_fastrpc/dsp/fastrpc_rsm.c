// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/cdev.h>
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/mutex.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/virtio.h>
#include <linux/slab.h>
#include <linux/scatterlist.h>
#include <linux/wait.h>
#include "fastrpc_rsm.h"


static void fastrpc_rsm_entry_free(struct kref *ref)
{
	struct vfastrpc_rsm_entry *rsm_entry = NULL;
	rsm_entry = container_of(ref, struct vfastrpc_rsm_entry, refcount);
	if (!rsm_entry)
		return;

	kfree(rsm_entry);
}

static void fastrpc_rsm_entry_put(struct vfastrpc_rsm_entry *rsm_entry)
{
	if (rsm_entry)
		kref_put(&rsm_entry->refcount, fastrpc_rsm_entry_free);
}

int fastrpc_rsm_entry_find(struct fastrpc_user *fl,
				struct vfastrpc_rsm_entry **pprsm_entry,
				unsigned int target_id)
{
	struct vfastrpc_rsm_entry *match = NULL, *rsm_entry = NULL;
	struct hlist_node *n;

	mutex_lock(&fl->rsm_list_mutex);
	hlist_for_each_entry_safe(rsm_entry, n, &fl->rsm_list_per_session, hn) {
		if (rsm_entry->target_id == target_id) {
			match = rsm_entry;
			break;
		}
	}
	mutex_unlock(&fl->rsm_list_mutex);

	if (match) {
		*pprsm_entry = match;
		return 0;
	}

	return -ENOTTY;
}

void fastrpc_rsm_entry_add(struct fastrpc_user *fl,
				struct vfastrpc_rsm_entry *rsm_entry)
{
	mutex_lock(&fl->rsm_list_mutex);
	hlist_add_head(&rsm_entry->hn, &fl->rsm_list_per_session);
	mutex_unlock(&fl->rsm_list_mutex);
}

void fastrpc_rsm_list_per_session_free(struct fastrpc_user *fl)
{
	struct vfastrpc_rsm_entry *rsm_entry, *rsm_entry_free;

	do {
		struct hlist_node *n;

		rsm_entry_free = NULL;
		mutex_lock(&fl->rsm_list_mutex);
		hlist_for_each_entry_safe(rsm_entry, n, &fl->rsm_list_per_session, hn) {
			hlist_del_init(&rsm_entry->hn);
			rsm_entry_free = rsm_entry;
			break;
		}
		mutex_unlock(&fl->rsm_list_mutex);
		/*
		 * Pd is closed. RSM can beunregistered safely.
		 * After the rsm_unregister_batch function is implemented,
		 * this rsm_unregister_v2 will be removed.
		 */
		if (rsm_entry_free)
		{
			compressched_unregister_v2(rsm_entry_free->handle);
			fastrpc_rsm_entry_put(rsm_entry_free);
		}
	} while (rsm_entry_free);
}

int fastrpc_rsm_entry_create(struct fastrpc_user *fl,
				struct vfastrpc_rsm_entry **pprsm_entry,
				unsigned int target_id)
{
	struct vfastrpc_rsm_entry *rsm_entry = NULL;
	int err = 0;
	compressched_handle handle = -1;

	if (!fastrpc_rsm_entry_find(fl, pprsm_entry, target_id))
		return 0;

	RPC_DBG("compressched_register start, target_id %u\n", target_id);
	err = compressched_register(&handle, fl->upid, target_id);
	if (err)
		goto bail;
	RPC_DBG("compressched_register end, target_id %u, handle %x\n",
					target_id, handle);

	rsm_entry = kzalloc(sizeof(*rsm_entry), GFP_KERNEL);
	if (!rsm_entry)
		goto bail;

	INIT_HLIST_NODE(&rsm_entry->hn);
	kref_init(&rsm_entry->refcount);
	rsm_entry->target_id = target_id;
	rsm_entry->handle = handle;
	rsm_entry->response.token = -1;

	fastrpc_rsm_entry_add(fl, rsm_entry);
	*pprsm_entry = rsm_entry;
	return 0;
bail:
	if (handle != -1)
		compressched_unregister_v2(handle);
	return err;
}

int fastrpc_rsm_acquire(struct fastrpc_user *fl, unsigned int target_id)
{
	int err = 0;
	struct vfastrpc_rsm_entry *rsm_entry = NULL;

	err = fastrpc_rsm_entry_create(fl, &rsm_entry, target_id);
	if (err)
		goto bail;

	RPC_DBG("compressched_acquire start, handle %x\n", rsm_entry->handle);
	err = compressched_acquire(rsm_entry->handle,
					current->comm, &rsm_entry->response);
	RPC_DBG("compressched_acquire end, handle %x, token %x\n",
					rsm_entry->handle, rsm_entry->response.token);
bail:
	return err;
}

void fastrpc_rsm_release(struct fastrpc_user *fl, unsigned int target_id)
{
	struct vfastrpc_rsm_entry *rsm_entry = NULL;
	if (!fastrpc_rsm_entry_find(fl, &rsm_entry, target_id)) {
		if (rsm_entry->handle != -1 && rsm_entry->response.token != -1) {
			RPC_DBG("compressched_release_v2 start, handle %x, token %x\n",
							rsm_entry->handle, rsm_entry->response.token);
			compressched_release_v2(rsm_entry->handle, rsm_entry->response.token);
			RPC_DBG("compressched_release_v2 end, handle %x\n", rsm_entry->handle);
			rsm_entry->response.token = -1;
		}
	} else {
		RPC_ERR("target_id %u didn't register rsm\n",
			target_id);
	}
}
