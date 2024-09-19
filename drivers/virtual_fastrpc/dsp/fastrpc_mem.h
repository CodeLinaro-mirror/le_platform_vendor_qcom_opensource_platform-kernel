/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2025, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __FASTRPC_MEM_H__
#define __FASTRPC_MEM_H__

#include <linux/dma-buf.h>
#include <linux/types.h>
#include "fastrpc_common.h"

struct fastrpc_map {
        struct list_head node;
        struct fastrpc_user *fl;
        int fd;
        struct dma_buf *buf;
        struct sg_table *table;
        struct dma_buf_attachment *attach;
        u64 phys;
	u64 da;
        u64 size;
        void *va;
        u64 len;
        u64 raddr;
        u32 attr;
        u32 flags;
        struct kref refcount;
        int secure;
};

struct fastrpc_buf {
	struct list_head node;
	struct fastrpc_user *fl;
	struct dma_buf *dmabuf;
	struct device *dev;
	u32 type;
	u64 size;
	void *va;
	uint64_t da;
	struct sg_table sgt;
	struct page **pages;
	unsigned long dma_attr;
	u32 map_attr;
	struct mutex lock;
	struct list_head attachments;
	uintptr_t raddr;
	bool in_use;
	u32 domain_id;
};

enum fastrpc_buf_type {
	METADATA_BUF,
	COPYDATA_BUF,
	INITMEM_BUF,
	USER_BUF,
	REMOTEHEAP_BUF,
	ROOTHEAP_BUF,
	INTERNAL_BUF,
};

int fastrpc_map_create(struct fastrpc_user *fl, int fd,
		u64 va, u64 len, u32 attr, int mflags,
		struct fastrpc_map **ppmap,bool take_ref);
void fastrpc_free_map(struct fastrpc_map *map);
int fastrpc_map_get(struct fastrpc_map *map);
void fastrpc_map_put(struct fastrpc_map *map);
int fastrpc_map_lookup(struct fastrpc_user *fl, int fd,
			u64 va, u64 len, int mflags,
			struct fastrpc_map **ppmap, bool take_ref);
int fastrpc_buf_alloc(struct fastrpc_user *fl, u64 size,
				u32 buf_type, struct fastrpc_buf **obuf);
void fastrpc_buf_free(struct fastrpc_buf *buf, bool cache);
#endif /*__FASTRPC_MEM_H__*/
