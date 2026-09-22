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
	int err = 0;
	struct vfastrpc_rsm_entry *rsm_entry = NULL;
	rsm_entry = container_of(ref, struct vfastrpc_rsm_entry, refcount);
	if (!rsm_entry)
		return;

	err = rsm_unregister_v2(rsm_entry->handle);
	if (err) {
		ADSPRPC_ERR("rsm_unregister_v2 err %d, target_id %u, handle %x\n",
					err, rsm_entry->target_id, rsm_entry->handle);
	} else {
		ADSPRPC_DEBUG("rsm_unregister_v2 complete, target_id %u, handle %x\n",
					rsm_entry->target_id, rsm_entry->handle);
	}
	kfree(rsm_entry);
}

static int fastrpc_rsm_entry_get(struct vfastrpc_rsm_entry *rsm_entry)
{
	if (!rsm_entry)
		return -ENOENT;

	return kref_get_unless_zero(&rsm_entry->refcount) ? 0 : -ENOENT;
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
			if(fastrpc_rsm_entry_get(rsm_entry))
				continue;

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

static int fastrpc_rsm_entry_add(struct vfastrpc_file *vfl,
				struct vfastrpc_rsm_entry *rsm_entry)
{
	int err = 0;
	mutex_lock(&vfl->rsm_list_mutex);
	err = fastrpc_rsm_entry_get(rsm_entry);
	if (err)
		goto bail;

	hlist_add_head(&rsm_entry->hn, &vfl->rsm_list_per_session);
bail:
	mutex_unlock(&vfl->rsm_list_mutex);
	return err;
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
		if (rsm_entry_free)
			fastrpc_rsm_entry_put(rsm_entry_free);
	} while (rsm_entry_free);
}

int fastrpc_rsm_entry_create(struct vfastrpc_file *vfl,
				struct vfastrpc_rsm_entry **pprsm_entry,
				unsigned int target_id)
{
	struct vfastrpc_rsm_entry *rsm_entry = NULL;
	RSMRegisterPDUType reg_data = {0};
	int err = 0;
	rsm_handle handle = -1;

	if (!fastrpc_rsm_entry_find(vfl, pprsm_entry, target_id))
		return 0;

	rsm_entry = kzalloc(sizeof(*rsm_entry), GFP_KERNEL);
	if (!rsm_entry) {
		err = -ENOMEM;
		goto bail;
	}

	reg_data.upid = vfl->upid;
	reg_data.tid = target_id;
	reg_data.nspID = vfl->domain - 3; /* Maps the domain_id(CDSP0=3,CDSP1=4) to the corresponding nsp_id(CDSP0=0, CDSP1=1) */

	err = rsm_register_for_nsp(&handle, reg_data);
	if (err) {
		ADSPRPC_ERR("rsm_register_for_nsp err single core %d, target_id %u, handle %x, upid %u, logical_id %d\n",
						err, target_id, handle, vfl->upid, vfl->domain);
		goto bail;
	} else {
		ADSPRPC_DEBUG("rsm_register_for_nsp single core complete, target_id %u, handle %x, upid %u, logical_id %d\n",
						target_id, handle, vfl->upid, vfl->domain);
	}

	INIT_HLIST_NODE(&rsm_entry->hn);
	kref_init(&rsm_entry->refcount);
	rsm_entry->target_id = target_id;
	rsm_entry->handle = handle;
	rsm_entry->response.token = -1;

	err = fastrpc_rsm_entry_add(vfl, rsm_entry);
	if (err)
		goto bail;

	*pprsm_entry = rsm_entry;
bail:
	if (err)
		kfree(rsm_entry);
	return err;
}

int fastrpc_rsm_acquire(struct vfastrpc_file *vfl, unsigned int target_id)
{
	int err = 0;
	struct vfastrpc_rsm_entry *rsm_entry = NULL;
	err = fastrpc_rsm_entry_create(vfl, &rsm_entry, target_id);
	if (err)
		goto bail;

	err = rsm_acquire(rsm_entry->handle, current->comm, &rsm_entry->response);
	if (err) {
		ADSPRPC_ERR("rsm_acquire err %d, handle %x, token %x\n",
						err, rsm_entry->handle, rsm_entry->response.token);
	} else {
		ADSPRPC_DEBUG("rsm_acquire complete, handle %x, token %x\n",
						rsm_entry->handle, rsm_entry->response.token);
	}
bail:
	fastrpc_rsm_entry_put(rsm_entry);
	return err;
}

void fastrpc_rsm_release(struct vfastrpc_file *vfl, unsigned int target_id)
{
	int err = 0;
	struct vfastrpc_rsm_entry *rsm_entry = NULL;

	if (fastrpc_rsm_entry_find(vfl, &rsm_entry, target_id)) {
		ADSPRPC_ERR("target_id %u didn't register rsm\n", target_id);
		return;
	}

	if (rsm_entry->handle == -1 || rsm_entry->response.token == -1) {
		ADSPRPC_ERR("rsm_release_v2 skipped, invalid handle %x or token %x\n",
					rsm_entry->handle, rsm_entry->response.token);
		goto bail;
	}

	err = rsm_release_v2(rsm_entry->handle, rsm_entry->response.token);
	if (err) {
		ADSPRPC_ERR("rsm_release_v2 err %d, handle %x\n", err, rsm_entry->handle);
	} else {
		ADSPRPC_DEBUG("rsm_release_v2 complete, handle %x\n", rsm_entry->handle);
		rsm_entry->response.token = -1;
	}

bail:
	fastrpc_rsm_entry_put(rsm_entry);
}