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
#include "adsprpc_compat.h"
#include "adsprpc_shared.h"
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

int fastrpc_rsm_entry_find(struct vfastrpc_file *vfl,
				struct vfastrpc_rsm_entry **pprsm_entry,
				unsigned int target_id)
{
	struct vfastrpc_rsm_entry *match = NULL, *rsm_entry = NULL;
	struct hlist_node *n;

	mutex_lock(&vfl->rsm_list_mutex);
	hlist_for_each_entry_safe(rsm_entry, n, &vfl->rsm_list_per_session, hn) {
		if (rsm_entry->target_id == target_id) {
			match = rsm_entry;
			break;
		}
	}
	mutex_unlock(&vfl->rsm_list_mutex);

	if (match) {
		*pprsm_entry = match;
		return 0;
	}

	return -ENOTTY;
}

void fastrpc_rsm_entry_add(struct vfastrpc_file *vfl,
				struct vfastrpc_rsm_entry *rsm_entry)
{
	mutex_lock(&vfl->rsm_list_mutex);
	hlist_add_head(&rsm_entry->hn, &vfl->rsm_list_per_session);
	mutex_unlock(&vfl->rsm_list_mutex);
}

void fastrpc_rsm_list_per_session_free(struct vfastrpc_file *vfl)
{
	struct vfastrpc_rsm_entry *rsm_entry, *rsm_entry_free;

	do {
		struct hlist_node *n;

		rsm_entry_free = NULL;
		mutex_lock(&vfl->rsm_list_mutex);
		hlist_for_each_entry_safe(rsm_entry, n, &vfl->rsm_list_per_session, hn) {
			hlist_del_init(&rsm_entry->hn);
			rsm_entry_free = rsm_entry;
			break;
		}
		mutex_unlock(&vfl->rsm_list_mutex);
		/*
		 * Pd is closed. RSM can beunregistered safely.
		 * After the rsm_unregister_batch function is implemented,
		 * this rsm_unregister_v2 will be removed.
		 */
		if (rsm_entry_free)
		{
			rsm_unregister_v2(rsm_entry_free->handle);
			fastrpc_rsm_entry_put(rsm_entry_free);
		}
	} while (rsm_entry_free);
}

int fastrpc_rsm_entry_create(struct vfastrpc_file *vfl,
				struct vfastrpc_rsm_entry **pprsm_entry,
				unsigned int target_id)
{
	struct vfastrpc_rsm_entry *rsm_entry = NULL;
	int err = 0;
	rsm_handle handle = -1;

	if (!fastrpc_rsm_entry_find(vfl, pprsm_entry, target_id))
		return 0;

	VERIFY(err, 0 == (err = rsm_register(&handle, vfl->upid, target_id)));
	if (err)
		goto bail;

	VERIFY(err, NULL != (rsm_entry = kzalloc(sizeof(*rsm_entry), GFP_KERNEL)));
	if (err)
		goto bail;

	INIT_HLIST_NODE(&rsm_entry->hn);
	kref_init(&rsm_entry->refcount);
	rsm_entry->target_id = target_id;
	rsm_entry->handle = handle;
	rsm_entry->response.token = -1;

	fastrpc_rsm_entry_add(vfl, rsm_entry);
	*pprsm_entry = rsm_entry;
	return 0;
bail:
	if (handle != -1)
		rsm_unregister_v2(handle);
	return err;
}

int fastrpc_rsm_acquire(struct vfastrpc_file *vfl, unsigned int target_id)
{
	int err = 0;
	struct vfastrpc_rsm_entry *rsm_entry = NULL;

	VERIFY(err, 0 == (err = fastrpc_rsm_entry_create(vfl, &rsm_entry, target_id)));
	if (err)
		goto bail;

	VERIFY(err, 0 == (err = rsm_acquire(rsm_entry->handle,
					current->comm, &rsm_entry->response)));
bail:
	return err;
}

void fastrpc_rsm_release(struct vfastrpc_file *vfl, unsigned int target_id)
{
	struct vfastrpc_rsm_entry *rsm_entry = NULL;
	if (!fastrpc_rsm_entry_find(vfl, &rsm_entry, target_id)) {
		if (rsm_entry->handle != -1 && rsm_entry->response.token != -1) {
			rsm_release_v2(rsm_entry->handle, rsm_entry->response.token);
			rsm_entry->response.token = -1;
		}
	} else {
		ADSPRPC_ERR("target_id %u didn't register rsm\n",
			target_id);
	}
}
