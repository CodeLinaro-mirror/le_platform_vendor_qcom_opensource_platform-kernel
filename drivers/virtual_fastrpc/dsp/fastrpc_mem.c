// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2025, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include "fastrpc_mem.h"
#include "fastrpc_vq.h"

struct fastrpc_smmu_map {
	u32 attrs;
	u32 nents;
	u64 da;
	struct virt_fastrpc_sgl sgl[0];
} __packed;

struct virt_smmu_map_msg {
	struct virt_msg_hdr hdr;		/* virtio fastrpc message header */
	u32 nents;				/* number of map entries */
	struct fastrpc_smmu_map smmu_map[0];	/* smmu map list */
} __packed;

struct virt_smmu_unmap_msg {
	struct virt_msg_hdr hdr;		/* virtio fastrpc message header */
	u32 nents;				/* number of unmap entries */
	u64 da[0];				/* smmu unmap da list */
} __packed;

#define FASTRPC_MAX_CACHED_BUFS (32)
#define FASTRPC_MAX_CACHE_BUF_SIZE (8*1024*1024)

static inline void fastrpc_free_pages(struct page **pages, unsigned int count)
{
	while (count--)
		__free_page(pages[count]);
	kvfree(pages);
}

static struct page **fastrpc_alloc_pages(struct device *dev, unsigned int count, gfp_t gfp)
{
	struct page **pages;
	unsigned long order_mask = (2U << MAX_ORDER) - 1;
	unsigned int i = 0, nid = dev_to_node(dev);

	pages = kvzalloc(count * sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return NULL;

	/* IOMMU can map any pages, so himem can also be used here */
	gfp |= __GFP_NOWARN | __GFP_HIGHMEM;
	gfp &= ~__GFP_COMP;

	while (count) {
		struct page *page = NULL;
		unsigned int order_size;

		/*
		 * Higher-order allocations are a convenience rather
		 * than a necessity, hence using __GFP_NORETRY until
		 * falling back to minimum-order allocations.
		 */
		for (order_mask &= (2U << __fls(count)) - 1;
		     order_mask; order_mask &= ~order_size) {
			unsigned int order = __fls(order_mask);
			gfp_t alloc_flags = gfp;

			order_size = 1U << order;
			if (order_mask > order_size)
				alloc_flags |= __GFP_NORETRY;

			page = alloc_pages_node(nid, alloc_flags, order);
			if (!page)
				continue;
			if (order)
				split_page(page, order);
			break;
		}
		if (!page) {
			fastrpc_free_pages(pages, i);
			return NULL;
		}
		count -= order_size;
		while (order_size--)
			pages[i++] = page++;
	}
	return pages;
}

static struct page **fastrpc_alloc_buffer(struct device *dev, struct fastrpc_buf *buf,
		gfp_t gfp, pgprot_t prot)
{
	struct page **pages;
	unsigned int count = PAGE_ALIGN(buf->size) >> PAGE_SHIFT;

	pages = fastrpc_alloc_pages(dev, count, gfp);
	if (!pages)
		return NULL;

	if (sg_alloc_table_from_pages(&buf->sgt, pages, count, 0,
				buf->size, GFP_KERNEL))
		goto out_free_pages;

	if (!(buf->dma_attr & DMA_ATTR_NO_KERNEL_MAPPING)) {
		buf->va = vmap(pages, count, VM_MAP, prot);
		if (!buf->va)
			goto out_free_sg;
	}
	return pages;

out_free_sg:
	sg_free_table(&buf->sgt);
out_free_pages:
	fastrpc_free_pages(pages, count);
	return NULL;
}

static inline void fastrpc_free_buffer(struct fastrpc_buf *buf)
{
	unsigned int count = PAGE_ALIGN(buf->size) >> PAGE_SHIFT;

	vunmap(buf->va);
	sg_free_table(&buf->sgt);
	fastrpc_free_pages(buf->pages, count);
}

static int virt_smmu_map(struct fastrpc_user *fl, u32 attr,
			struct scatterlist *table, u32 nents, uint64_t *da)
{
	struct fastrpc_channel_ctx *cctx = fl->cctx;
	struct fastrpc_common *gdriver = cctx->gdriver;
	struct virt_smmu_map_msg *vmsg, *rsp = NULL;
	struct virt_fastrpc_msg *msg = NULL;
	struct virt_fastrpc_sgl *sgbuf;
	int err, sgbuf_size, total_size;
	struct scatterlist *sgl = NULL;
	int sgl_index = 0;
	u32 lattr = attr;
	struct virt_fastrpc_sgtable *intmap = NULL;
	struct fastrpc_buf intbuf;

	sgbuf_size = nents * sizeof(*sgbuf);
	total_size = sizeof(*vmsg) +
		sizeof(struct fastrpc_smmu_map) + sgbuf_size;

	if (total_size > gdriver->buf_size) {
		lattr |= FASTRPC_MAP_ATTR_MULTI_LEVEL_SGT;
		memset(&intbuf, 0, sizeof(struct fastrpc_buf));
		intbuf.size = PAGE_ALIGN(sizeof(*intmap) + sgbuf_size);
		intbuf.pages = fastrpc_alloc_buffer(cctx->dev, &intbuf,
				GFP_KERNEL, PAGE_KERNEL);
		if (!intbuf.pages) {
			RPC_ERR("fail to alloc buffer size %llx\n",
				intbuf.size);
			return -ENOMEM;
		}
		intmap = intbuf.va;
		intmap->nents = nents;
		sgbuf = intmap->sgl;

		for_each_sg(table, sgl, nents, sgl_index) {
			if (sg_dma_len(sgl)) {
				sgbuf[sgl_index].pv = sg_dma_address(sgl);
				sgbuf[sgl_index].len = sg_dma_len(sgl);
			} else {
				sgbuf[sgl_index].pv = page_to_phys(sg_page(sgl));
				sgbuf[sgl_index].len = sgl->length;
			}
		}
		sgbuf_size = intbuf.sgt.nents * sizeof(*sgbuf);
		total_size = sizeof(*vmsg) +
			sizeof(struct fastrpc_smmu_map) + sgbuf_size;
	}

	msg = virt_alloc_msg(fl, total_size);
	if (!msg) {
		err = -ENOMEM;
		goto bail;
	}

	vmsg = (struct virt_smmu_map_msg *)msg->txbuf;
	vmsg->hdr.pid = fl->tgid_frpc;
	vmsg->hdr.tid = current->pid;
	vmsg->hdr.cid = fl->cid;
	vmsg->hdr.cmd = VIRTIO_FASTRPC_CMD_SMMU_MAP;
	vmsg->hdr.len = total_size;
	vmsg->hdr.msgid = msg->msgid;
	vmsg->hdr.result = 0xffffffff;
	vmsg->nents = 1;
	vmsg->smmu_map[0].attrs = lattr;
	vmsg->smmu_map[0].nents = intmap ? intbuf.sgt.nents : nents;
	vmsg->smmu_map[0].da = 0;
	sgbuf = vmsg->smmu_map[0].sgl;

	if (intmap) {
		for_each_sg(intbuf.sgt.sgl, sgl, intbuf.sgt.nents, sgl_index) {
			sgbuf[sgl_index].pv = page_to_phys(sg_page(sgl));
			sgbuf[sgl_index].len = sgl->length;
		}
	} else {
		for_each_sg(table, sgl, nents, sgl_index) {
			if (sg_dma_len(sgl)) {
				sgbuf[sgl_index].pv = sg_dma_address(sgl);
				sgbuf[sgl_index].len = sg_dma_len(sgl);
			} else {
				sgbuf[sgl_index].pv = page_to_phys(sg_page(sgl));
				sgbuf[sgl_index].len = sgl->length;
			}
		}
	}

	err = fastrpc_txbuf_send(fl, vmsg, total_size);
	if (err)
		goto bail;

	wait_for_completion(&msg->work);

	rsp = msg->rxbuf;
	if (!rsp)
		goto bail;

	err = rsp->hdr.result;
	if (err)
		goto bail;
	if (!rsp->smmu_map[0].da) {
		RPC_ERR("invalid smmu da 0x%lx\n", rsp->smmu_map[0].da);
		err = -EFAULT;
		goto bail;
	}
	*da = rsp->smmu_map[0].da;
bail:
	if (rsp)
		fastrpc_rxbuf_send(fl, rsp, gdriver->buf_size);
	if (msg)
		virt_free_msg(fl, msg);
	if (intmap)
		fastrpc_free_buffer(&intbuf);
	return err;
}

static int virt_smmu_unmap(struct fastrpc_user *fl, uint64_t da)
{
	struct fastrpc_common *gdriver = fl->cctx->gdriver;
	struct virt_smmu_unmap_msg *vmsg, *rsp = NULL;
	struct virt_fastrpc_msg *msg;
	int err, total_size;

	RPC_DBG("smmu unmap da = 0x%lx\n", da);
	spin_lock(&fl->lock);
	if (fl->state >= DSP_EXIT_START) {
		spin_unlock(&fl->lock);
		return -ESHUTDOWN;
	}
	spin_unlock(&fl->lock);

	total_size = sizeof(*vmsg) + sizeof(uint64_t);

	msg = virt_alloc_msg(fl, total_size);
	if (!msg)
		return -ENOMEM;

	vmsg = (struct virt_smmu_unmap_msg *)msg->txbuf;
	vmsg->hdr.pid = fl->tgid_frpc;
	vmsg->hdr.tid = current->pid;
	vmsg->hdr.cid = fl->cid;
	vmsg->hdr.cmd = VIRTIO_FASTRPC_CMD_SMMU_UNMAP;
	vmsg->hdr.len = total_size;
	vmsg->hdr.msgid = msg->msgid;
	vmsg->hdr.result = 0xffffffff;
	vmsg->nents = 1;
	vmsg->da[0] = da;

	err = fastrpc_txbuf_send(fl, vmsg, total_size);
	if (err)
		goto bail;

	wait_for_completion(&msg->work);

	rsp = msg->rxbuf;
	if (!rsp)
		goto bail;

	err = rsp->hdr.result;
	if (err)
		goto bail;
bail:
	if (err)
		RPC_ERR("failed to unmap smmu da = 0x%lx\n", da);
	if (rsp)
		fastrpc_rxbuf_send(fl, rsp, gdriver->buf_size);
	virt_free_msg(fl, msg);
	return err;
}

int fastrpc_map_lookup(struct fastrpc_user *fl, int fd,
		u64 va, u64 len, int mflags,
		struct fastrpc_map **ppmap, bool take_ref)
{
	struct fastrpc_map *map = NULL;
	int ret = -ENOENT;

	spin_lock(&fl->lock);
	if (mflags == ADSP_MMAP_DMA_BUFFER ||
			mflags == ADSP_MMAP_HEAP_ADDR ||
			mflags == ADSP_MMAP_REMOTE_HEAP_ADDR) {
		RPC_ERR("buffer type is not supported mflags=%d\n", mflags);
		return -EINVAL;
	}

	list_for_each_entry(map, &fl->maps, node) {
		if (map->fd == fd && va >= (u64)map->va &&
				va + len >= va &&
				va + len <= (u64)map->va + map->size)
			goto map_found;
	}

	spin_unlock(&fl->lock);
	return ret;

map_found:
	if (take_ref) {
		ret = fastrpc_map_get(map);
		if (ret) {
			RPC_ERR("failed to get map fd=%d ret=%d\n", fd, ret);
			spin_unlock(&fl->lock);
			goto error;
		}
	}
	spin_unlock(&fl->lock);

	*ppmap = map;
	ret = 0;
error:
	return ret;
}

int fastrpc_map_create(struct fastrpc_user *fl, int fd,
		u64 va, u64 len, u32 attr, int mflags,
		struct fastrpc_map **ppmap,bool take_ref)
{
	struct fastrpc_map *map = NULL;
	int err = 0, sgl_index = 0;
	struct scatterlist *sgl = NULL;

	RPC_DBG("fd=%d,va=%lx,len=0x%lx\n", fd, va, len);
	if (!fastrpc_map_lookup(fl, fd, va, len, mflags, ppmap, take_ref)) {
		if (!(*ppmap)->da)
			RPC_ERR("find invalid map, fd=%d,va=%lx,len=0x%lx\n",
					fd, va, len);
		return 0;
	}

	map = kzalloc(sizeof(*map), GFP_KERNEL);
	if (!map)
		return -ENOMEM;

	INIT_LIST_HEAD(&map->node);
	kref_init(&map->refcount);

	map->fl = fl;
	map->fd = fd;
	map->flags = mflags;
	map->attr = attr;
	map->len = len;

	if (mflags == ADSP_MMAP_DMA_BUFFER ||
			mflags == ADSP_MMAP_HEAP_ADDR ||
			mflags == ADSP_MMAP_REMOTE_HEAP_ADDR) {
		RPC_ERR("buffer type is not supported mflags=%d\n", mflags);
		err = -EINVAL;
		goto get_err;
	} else {
		map->buf = dma_buf_get(fd);
		if (IS_ERR(map->buf)) {
			RPC_ERR("failed to get dma buf fd %d\n", fd);
			err = PTR_ERR(map->buf);
			goto get_err;
		}
		map->attach = dma_buf_attach(map->buf, fl->cctx->dev);
		if (IS_ERR(map->attach)) {
			RPC_ERR("failed to attach dmabuf\n");
			err = PTR_ERR(map->attach);
			goto attach_err;
		}

		map->attach->dma_map_attrs |= DMA_ATTR_SKIP_CPU_SYNC;
		map->table = dma_buf_map_attachment(map->attach,
							DMA_BIDIRECTIONAL);
		if (IS_ERR(map->table)) {
			RPC_ERR("failed to get sg table of dma buf\n");
			err = PTR_ERR(map->table);
			goto map_err;
		}

		map->phys = sg_dma_address(map->table->sgl);
		for_each_sg(map->table->sgl, sgl, map->table->nents, sgl_index)
			map->size += sg_dma_len(sgl);
		map->va = (void *) (uintptr_t) va;

		err = virt_smmu_map(fl, FASTRPC_MAP_ATTR_CACHED,
					map->table->sgl,
					map->table->nents,
					&map->da);
		if (err) {
			RPC_ERR("failed to get map da\n");
			goto smmu_map_err;
		}

		spin_lock(&fl->lock);
		list_add_tail(&map->node, &fl->maps);
		spin_unlock(&fl->lock);
		*ppmap = map;
		RPC_DBG("Create new map 0x%lx,flags=0x%x,attr=0x%x,da=0x%lx\n",
				map, map->flags, map->attr, map->da);
	}

	return 0;
smmu_map_err:
	dma_buf_unmap_attachment(map->attach, map->table,
			DMA_BIDIRECTIONAL);
map_err:
	dma_buf_detach(map->buf, map->attach);
attach_err:
	dma_buf_put(map->buf);
get_err:
	kfree(map);

	return err;
}

void fastrpc_free_map(struct fastrpc_map *map)
{
	struct fastrpc_user *fl = NULL;

	if (!map)
		return;

	RPC_DBG("free map 0x%lx, attr=%x\n",
			map, map->attr);
	fl = map->fl;
	if (fl) {
		spin_lock(&map->fl->lock);
		list_del(&map->node);
		spin_unlock(&map->fl->lock);
	}

	if (map->da)
		virt_smmu_unmap(fl, map->da);

	if (map->table) {
		dma_buf_unmap_attachment(map->attach, map->table,
						DMA_BIDIRECTIONAL);
		dma_buf_detach(map->buf, map->attach);
		dma_buf_put(map->buf);
	}
	kfree(map);
}

static void __fastrpc_free_map(struct kref *ref)
{
	struct fastrpc_map *map = NULL;

	map = container_of(ref, struct fastrpc_map, refcount);
	fastrpc_free_map(map);
}

void fastrpc_map_put(struct fastrpc_map *map)
{
	if (map)
		kref_put(&map->refcount, __fastrpc_free_map);
}

int fastrpc_map_get(struct fastrpc_map *map)
{
	if (!map)
		return -ENOENT;

	return kref_get_unless_zero(&map->refcount) ? 0 : -ENOENT;
}

static void __fastrpc_buf_free(struct fastrpc_buf *buf)
{
	struct fastrpc_user *fl = buf->fl;

	if (buf->da)
		virt_smmu_unmap(fl, buf->da);
	if (!IS_ERR_OR_NULL(buf->pages))
		fastrpc_free_buffer(buf);
	kfree(buf);
}

static void fastrpc_cached_buf_list_add(struct fastrpc_buf *buf)
{
	struct fastrpc_user *fl = buf->fl;

	if (buf->size < FASTRPC_MAX_CACHE_BUF_SIZE) {
		spin_lock(&fl->lock);
		if (fl->num_cached_buf > FASTRPC_MAX_CACHED_BUFS) {
			spin_unlock(&fl->lock);
			goto skip_buf_cache;
		}

		list_add_tail(&buf->node, &fl->cached_bufs);
		fl->num_cached_buf++;
		buf->type = -1;
		spin_unlock(&fl->lock);
		return;
	}

skip_buf_cache:
	__fastrpc_buf_free(buf);
	return;
}

void fastrpc_buf_free(struct fastrpc_buf *buf, bool cache)
{
	struct fastrpc_user *fl = buf->fl;

	if (buf->in_use) {
		/* Don't free persistent header buf. Just mark as available */
		spin_lock(&fl->lock);
		buf->in_use = false;
		spin_unlock(&fl->lock);
		return;
	}
	if (cache)
		fastrpc_cached_buf_list_add(buf);
	else
		__fastrpc_buf_free(buf);
}

static bool fastrpc_get_persistent_buf(struct fastrpc_user *fl,
		size_t size, int buf_type, struct fastrpc_buf **obuf)
{
	u32 i = 0;
	bool found = false;
	struct fastrpc_buf *buf = NULL;

	spin_lock(&fl->lock);
 	if (!fl->num_pers_hdrs || buf_type != METADATA_BUF ||
			size > PAGE_SIZE) {
		spin_unlock(&fl->lock);
		return found;
	}

	for (i = 0; i < fl->num_pers_hdrs; i++) {
		buf = &fl->hdr_bufs[i];
		if (!buf->in_use) {
			buf->in_use = true;
			*obuf = buf;
			found = true;
			break;
		}
	}
	spin_unlock(&fl->lock);

	return found;
}

static inline bool fastrpc_get_cached_buf(struct fastrpc_user *fl,
			u64 size, u32 buf_type, struct fastrpc_buf **obuf)
{
	bool found = false;
	struct fastrpc_buf *buf, *n, *cbuf = NULL;

	if (buf_type == USER_BUF || buf_type == REMOTEHEAP_BUF)
		return found;

	/* find the smallest buffer that fits in the cache */
	spin_lock(&fl->lock);
	list_for_each_entry_safe(buf, n, &fl->cached_bufs, node) {
		if (buf->size >= size && (!cbuf || cbuf->size > buf->size))
			cbuf = buf;
	}
	if (cbuf) {
		list_del_init(&cbuf->node);
		fl->num_cached_buf--;
	}
	spin_unlock(&fl->lock);
	if (cbuf) {
		cbuf->type = buf_type;
		*obuf = cbuf;
		found = true;
	}

	return found;
}

static int __fastrpc_buf_alloc(struct fastrpc_user *fl, u32 domain_id,
			u64 size, struct fastrpc_buf **obuf, u32 buf_type)
{
	struct fastrpc_buf *buf;
	int err;

	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	INIT_LIST_HEAD(&buf->attachments);
	INIT_LIST_HEAD(&buf->node);
	mutex_init(&buf->lock);

	buf->fl = fl;
	buf->va = NULL;
	buf->size = size;
	buf->raddr = 0;
	buf->type = buf_type;
	buf->domain_id = domain_id;
	buf->pages = fastrpc_alloc_buffer(fl->cctx->dev, buf, GFP_KERNEL, PAGE_KERNEL);
	if (IS_ERR_OR_NULL(buf->pages)) {
		RPC_ERR("fastrpc_alloc_buffer failed for size 0x%lx, returned %ld\n",
			size, PTR_ERR(buf->pages));
		goto bail;
	}

	err = virt_smmu_map(fl, FASTRPC_MAP_ATTR_CACHED,
					buf->sgt.sgl,
					buf->sgt.nents, &buf->da);
	if (err) {
		RPC_ERR("failed to get buf da\n");
		goto bail;
	}

	*obuf = buf;

	return 0;
bail:
	if (!IS_ERR_OR_NULL(buf->pages))
		fastrpc_free_buffer(buf);
	mutex_destroy(&buf->lock);
	kfree(buf);
	return -ENOMEM;
}

void fastrpc_buf_list_free(struct fastrpc_user *fl,
		struct list_head *buf_list, bool is_cached_buf)
{
	struct fastrpc_buf *buf = NULL, *n = NULL, *free = NULL;

	do {
		free = NULL;
		spin_lock(&fl->lock);
		list_for_each_entry_safe(buf, n, buf_list, node) {
			list_del(&buf->node);
			if (is_cached_buf)
				fl->num_cached_buf--;
			free = buf;
			break;
		}
		spin_unlock(&fl->lock);
		if (free)
			fastrpc_buf_free(free, false);
	} while (free);
}

int fastrpc_buf_alloc(struct fastrpc_user *fl, u64 size,
				u32 buf_type, struct fastrpc_buf **obuf)
{
	int ret;

	if (fastrpc_get_persistent_buf(fl, size, buf_type, obuf))
		return 0;

	if (fastrpc_get_cached_buf(fl, size, buf_type, obuf))
		return 0;

	ret = __fastrpc_buf_alloc(fl, fl->cctx->domain_id,
					size, obuf, buf_type);
	if (ret == -ENOMEM) {
		fastrpc_buf_list_free(fl, &fl->cached_bufs, true);
		ret = __fastrpc_buf_alloc(fl, fl->cctx->domain_id,
					size, obuf, buf_type);
	}

	return ret;
}
