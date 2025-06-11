// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2024, Qualcomm Innovation Center, Inc. All rights reserved.
 */
#include <linux/version.h>
#include <linux/vmalloc.h>
#include "virtio_fastrpc_mem.h"
#include "virtio_fastrpc_queue.h"

struct fastrpc_smmu_map {
	u32 attrs;
	u32 nents;
	u64 da;
	struct virt_fastrpc_sgl sgl[];
} __packed;

struct virt_smmu_map_msg {
	struct virt_msg_hdr hdr;		/* virtio fastrpc message header */
	u32 nents;				/* number of map entries */
	struct fastrpc_smmu_map smmu_map[];	/* smmu map list */
} __packed;

struct virt_smmu_unmap_msg {
	struct virt_msg_hdr hdr;		/* virtio fastrpc message header */
	u32 nents;				/* number of unmap entries */
	u64 da[];				/* smmu unmap da list */
} __packed;

#define MAX_CACHE_BUF_SIZE		(8*1024*1024)
/* Maximum buffers cached in cached buffer list */
#define MAX_CACHED_BUFS		32
#define MAX_BUF_SIZE	0x78000000

static inline void vfastrpc_free_pages(struct page **pages, unsigned int count)
{
	while (count--)
		__free_page(pages[count]);
	kvfree(pages);
}

static struct page **vfastrpc_alloc_pages(struct device *dev, unsigned int count, gfp_t gfp)
{
	struct page **pages;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
	unsigned long order_mask = (2U << NR_PAGE_ORDERS) - 1;
#else
	unsigned long order_mask = (2U << MAX_ORDER) - 1;
#endif
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
			vfastrpc_free_pages(pages, i);
			return NULL;
		}
		count -= order_size;
		while (order_size--)
			pages[i++] = page++;
	}
	return pages;
}

static struct page **vfastrpc_alloc_buffer(struct device *dev, struct vfastrpc_buf *buf,
		gfp_t gfp, pgprot_t prot)
{
	struct page **pages;
	unsigned int count = PAGE_ALIGN(buf->size) >> PAGE_SHIFT;

	pages = vfastrpc_alloc_pages(dev, count, gfp);
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
	vfastrpc_free_pages(pages, count);
	return NULL;
}

static inline void vfastrpc_free_buffer(struct vfastrpc_buf *buf)
{
	unsigned int count = PAGE_ALIGN(buf->size) >> PAGE_SHIFT;

	vunmap(buf->va);
	sg_free_table(&buf->sgt);
	vfastrpc_free_pages(buf->pages, count);
}

void vfastrpc_buf_free(struct vfastrpc_buf *buf, int cache)
{
	struct vfastrpc_file *vfl = buf == NULL ? NULL : buf->vfl;
	struct fastrpc_file *fl = vfl == NULL ? NULL : to_fastrpc_file(vfl);

	if (!vfl || !fl)
		return;

	if (cache && buf->size < MAX_CACHE_BUF_SIZE) {
		spin_lock(&fl->hlock);
		if (fl->num_cached_buf > MAX_CACHED_BUFS) {
			spin_unlock(&fl->hlock);
			dev_dbg(vfl->apps->dev, "num_cached_buf reaches upper limit\n");
			goto skip_buf_cache;
		}
		hlist_add_head(&buf->hn, &fl->cached_bufs);
		fl->num_cached_buf++;
		buf->type = -1;
		spin_unlock(&fl->hlock);
		return;
	}

skip_buf_cache:
	if (buf->type == VFASTRPC_BUF_TYPE_USERHEAP) {
		spin_lock(&fl->hlock);
		hlist_del_init(&buf->hn_rem);
		spin_unlock(&fl->hlock);
		buf->raddr = 0;
	}

	if (!IS_ERR_OR_NULL(buf->pages))
		vfastrpc_free_buffer(buf);
	kfree(buf);
}

int vfastrpc_buf_alloc(struct vfastrpc_file *vfl, size_t size,
				unsigned long dma_attr, uint32_t rflags,
				int buf_type, pgprot_t prot, struct vfastrpc_buf **obuf)
{
	struct vfastrpc_apps *me = vfl->apps;
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	struct vfastrpc_buf *buf = NULL, *fr = NULL;
	struct hlist_node *n;
	int err = 0;

	VERIFY(err, size > 0 && size < MAX_BUF_SIZE);
	if (err) {
		dev_err(me->dev, "%s: Invalid buffer size, 0x%zx\n",
				__func__, size);
		goto bail;
	}

	if (buf_type != VFASTRPC_BUF_TYPE_USERHEAP) {
		/* find the smallest buffer that fits in the cache */
		spin_lock(&fl->hlock);
		hlist_for_each_entry_safe(buf, n, &fl->cached_bufs, hn) {
			if (buf->size >= size && (!fr || fr->size > buf->size))
				fr = buf;
		}
		if (fr) {
			hlist_del_init(&fr->hn);
			fl->num_cached_buf--;
		}
		spin_unlock(&fl->hlock);
		if (fr) {
			*obuf = fr;
			return 0;
		}
	}

	VERIFY(err, NULL != (buf = kzalloc(sizeof(*buf), GFP_KERNEL)));
	if (err)
		goto bail;
	buf->vfl = vfl;
	buf->size = size;
	buf->va = NULL;
	buf->dma_attr = dma_attr;
	buf->map_attr = 0;
	buf->flags = rflags;
	buf->type = buf_type;
	buf->raddr = 0;
	buf->pages = vfastrpc_alloc_buffer(me->dev, buf, GFP_KERNEL, prot);
	if (IS_ERR_OR_NULL(buf->pages)) {
		err = -ENOMEM;
		dev_err(me->dev,
			"%s: %s: failed for size 0x%zx, returned %ld\n",
			current->comm, __func__, size, PTR_ERR(buf->pages));
		goto bail;
	}

	if (buf_type == VFASTRPC_BUF_TYPE_USERHEAP) {
		INIT_HLIST_NODE(&buf->hn_rem);
		spin_lock(&fl->hlock);
		hlist_add_head(&buf->hn_rem, &fl->remote_bufs);
		spin_unlock(&fl->hlock);
	}

	*obuf = buf;
 bail:
	if (err && buf)
		vfastrpc_buf_free(buf, 0);
	return err;
}

void vfastrpc_mmap_add(struct vfastrpc_file *vfl, struct vfastrpc_mmap *map)
{
	if (map->flags == ADSP_MMAP_HEAP_ADDR ||
				map->flags == ADSP_MMAP_REMOTE_HEAP_ADDR) {
		struct vfastrpc_apps *me = vfl->apps;

		dev_err(me->dev, "%s ADSP_MMAP_HEAP_ADDR is not supported\n",
				__func__);
	} else {
		struct vfastrpc_file *vfl = map->vfl;
		struct fastrpc_file *fl = to_fastrpc_file(vfl);

		hlist_add_head(&map->hn, &fl->maps);
	}
}

int vfastrpc_mmap_remove(struct vfastrpc_file *vfl, int fd,
		uintptr_t va, size_t len, struct vfastrpc_mmap **ppmap)
{
	struct vfastrpc_mmap *match = NULL, *map;
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	struct hlist_node *n;

	hlist_for_each_entry_safe(map, n, &fl->maps, hn) {
		if ((fd < 0 || map->fd == fd) && map->raddr == va &&
				map->raddr + map->len == va + len &&
				(map->refs == 1 ||
				 (map->refs == 2 &&
				  map->attr & FASTRPC_ATTR_KEEP_MAP)) &&
				/* Remove if only one reference map and no context map */
				!map->ctx_refs &&
				/* Remove map only if it isn't being used by DSP */
				!map->dma_handle_refs) {
			if (map->attr & FASTRPC_ATTR_KEEP_MAP)
				map->refs--;
			match = map;
			hlist_del_init(&map->hn);
			break;
		}
	}
	if (match) {
		*ppmap = match;
		return 0;
	}
	return -ETOOMANYREFS;
}

int vfastrpc_mmap_remove_fd(struct vfastrpc_file *vfl, int fd, u32 *entries)
{
	struct vfastrpc_mmap *match = NULL, *map = NULL;
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	struct hlist_node *n;
	int err = 0;

	*entries = 0;
	hlist_for_each_entry_safe(map, n, &fl->maps, hn) {
		if ((map->fd == fd) &&
				(map->attr & FASTRPC_ATTR_KEEP_MAP)) {
			(*entries)++;
			match = map;
			if (match->refs > 1 || match->ctx_refs) {
				dev_err(vfl->apps->dev,
						"%s map refs = %d or ctx_refs = %d is abnormal\n",
						__func__, match->refs, match->ctx_refs);
				err = -ETOOMANYREFS;
			}
			map->attr = map->attr & (~FASTRPC_ATTR_KEEP_MAP);
			vfastrpc_mmap_free(vfl, match, 0);
		}
	}
	return err;
}

void vfastrpc_mmap_free(struct vfastrpc_file *vfl,
		struct vfastrpc_mmap *map, uint32_t force_free)
{
	struct vfastrpc_apps *me = vfl->apps;

	if (!map)
		return;

	if (map->flags == ADSP_MMAP_HEAP_ADDR ||
				map->flags == ADSP_MMAP_REMOTE_HEAP_ADDR) {
		dev_err(me->dev, "%s ADSP_MMAP_HEAP_ADDR is not supported\n",
				__func__);
	} else {
		if (map->refs <= 0 || map->ctx_refs < 0 || map->dma_handle_refs < 0) {
			dev_warn(me->dev,
				"%s refs = %d ctx_refs = %d dma_handle_refs = %d is abnormal\n",
				__func__, map->refs, map->ctx_refs, map->dma_handle_refs);
			return;
		}

		map->refs--;
		if (force_free) {
			/*
			 * We only allow force_free happen for DMA-BUF with FASTRPC_ATTR_KEEP_MAP
			 * attribute set, because client could call remote_handle_close first,
			 * then call rpcmem_free.
			 */
			if (map->refs || map->ctx_refs || map->dma_handle_refs ||
					(map->refs == 0 && !(map->attr & FASTRPC_ATTR_KEEP_MAP)))
				dev_warn(me->dev,
					"force free, refs = %d ctx_refs = %d dma_handle_refs = %d attr = 0x%x\n",
					map->refs + 1, map->ctx_refs, map->dma_handle_refs,
					map->attr);
			map->refs = 0;
			map->ctx_refs = 0;
			map->dma_handle_refs = 0;
		}

		if (!map->refs && !map->ctx_refs && !map->dma_handle_refs) {
			hlist_del_init(&map->hn);
			if (!IS_ERR_OR_NULL(map->table)) {
				dma_buf_unmap_attachment(map->attach, map->table,
						DMA_BIDIRECTIONAL);
				map->table = NULL;
			}

			if (!IS_ERR_OR_NULL(map->attach)) {
				dma_buf_detach(map->buf, map->attach);
				map->attach = NULL;
			}

			if (!IS_ERR_OR_NULL(map->buf)) {
				dma_buf_put(map->buf);
				map->buf = NULL;
			}
			kfree(map);
		}
	}
}

int vfastrpc_mmap_find(struct vfastrpc_file *vfl, int fd,
		uintptr_t va, size_t len, int mflags, int refs,
		struct vfastrpc_mmap **ppmap)
{
	struct vfastrpc_apps *me = vfl->apps;
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	struct vfastrpc_mmap *match = NULL, *map = NULL;
	struct hlist_node *n;

	if ((va + len) < va)
		return -EOVERFLOW;
	if (mflags == ADSP_MMAP_HEAP_ADDR ||
				 mflags == ADSP_MMAP_REMOTE_HEAP_ADDR) {
		dev_err(me->dev, "%s ADSP_MMAP_HEAP_ADDR is not supported\n",
				__func__);
	} else {
		hlist_for_each_entry_safe(map, n, &fl->maps, hn) {
			if (va >= map->va &&
				va + len <= map->va + map->len &&
				map->fd == fd) {
				if (refs) {
					if (map->refs + 1 == INT_MAX)
						return -ETOOMANYREFS;
					map->refs++;
				}
				match = map;
				break;
			}
		}
	}
	if (match) {
		*ppmap = match;
		return 0;
	}
	return -ENOTTY;
}

int vfastrpc_mmap_create(struct vfastrpc_file *vfl, int fd,
	unsigned int attr, uintptr_t va, size_t len, int mflags,
	struct vfastrpc_mmap **ppmap)
{
	struct vfastrpc_apps *me = vfl->apps;
	struct vfastrpc_mmap *map = NULL;
	int err = 0, sgl_index = 0;
	struct scatterlist *sgl = NULL;

	ADSP_LOG("fd=%d,va=%lx,len=%ld\n", fd, va, len);
	if (!vfastrpc_mmap_find(vfl, fd, va, len, mflags, 1, ppmap))
		return 0;

	map = kzalloc(sizeof(*map), GFP_KERNEL);
	VERIFY(err, !IS_ERR_OR_NULL(map));
	if (err)
		goto bail;

	INIT_HLIST_NODE(&map->hn);
	map->flags = mflags;
	map->refs = 1;
	map->vfl = vfl;
	map->fd = fd;
	map->attr = attr;
	map->ctx_refs = 0;
	if (mflags == ADSP_MMAP_HEAP_ADDR ||
			mflags == ADSP_MMAP_REMOTE_HEAP_ADDR) {
		dev_err(me->dev, "%s ADSP_MMAP_HEAP_ADDR is not supported\n",
				__func__);
		err = -EINVAL;
		goto bail;
	} else {
		if (map->attr && (map->attr & FASTRPC_ATTR_KEEP_MAP)) {
			map->refs = 2;
			dev_dbg(me->dev, "KEE_MAP is set for fd = %d\n", map->fd);
		}

		VERIFY(err, !IS_ERR_OR_NULL(map->buf = dma_buf_get(fd)));
		if (err) {
			dev_err(me->dev, "can't get dma buf fd %d\n", fd);
			goto bail;
		}

		VERIFY(err, !IS_ERR_OR_NULL(map->attach =
					dma_buf_attach(map->buf, me->dev)));
		if (err) {
			dev_err(me->dev, "can't attach dma buf\n");
			goto bail;
		}

		/*
		 * no need to sync cache even for cached buffers, depending on
		 * IO coherency
		 */
		map->attach->dma_map_attrs |= DMA_ATTR_SKIP_CPU_SYNC;
		VERIFY(err, !IS_ERR_OR_NULL(map->table =
					dma_buf_map_attachment(map->attach,
					DMA_BIDIRECTIONAL)));
		if (err) {
			dev_err(me->dev, "can't get sg table of dma buf\n");
			goto bail;
		}
		map->phys = sg_dma_address(map->table->sgl);
		for_each_sg(map->table->sgl, sgl, map->table->nents, sgl_index)
			map->size += sg_dma_len(sgl);
		map->va = va;
	}

	map->len = len;
	vfastrpc_mmap_add(vfl, map);
	*ppmap = map;
bail:
	if (err && map)
		vfastrpc_mmap_free(vfl, map, 0);
	return err;
}

static int virt_smmu_map(struct vfastrpc_file *vfl, u32 attr,
			struct scatterlist *table, u32 nents, uint64_t *da)
{
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	struct vfastrpc_apps *me = vfl->apps;
	struct virt_smmu_map_msg *vmsg, *rsp = NULL;
	struct virt_fastrpc_msg *msg = NULL;
	struct virt_fastrpc_sgl *sgbuf;
	int err, sgbuf_size, total_size;
	struct scatterlist *sgl = NULL;
	int sgl_index = 0;
	u32 lattr = attr;
	struct virt_fastrpc_sgtable *intmap = NULL;
	struct vfastrpc_buf intbuf;

	sgbuf_size = nents * sizeof(*sgbuf);
	total_size = sizeof(*vmsg) +
		sizeof(struct fastrpc_smmu_map) + sgbuf_size;

	if (total_size > me->buf_size) {
		lattr |= VFASTRPC_MAP_ATTR_INTERNAL_MAP;
		memset(&intbuf, 0, sizeof(struct vfastrpc_buf));
		intbuf.size = PAGE_ALIGN(sizeof(*intmap) + sgbuf_size);
		intbuf.pages = vfastrpc_alloc_buffer(me->dev, &intbuf,
				GFP_KERNEL, PAGE_KERNEL);
		if (!intbuf.pages) {
			dev_err(me->dev, "%s: fail to alloc buffer size %zx\n",
					__func__, intbuf.size);
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

	msg = virt_alloc_msg(vfl, total_size);
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

	err = vfastrpc_txbuf_send(vfl, vmsg, total_size);
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
		dev_err(me->dev, "invalid smmu da %llu\n",
				rsp->smmu_map[0].da);
		err = -EFAULT;
		goto bail;
	}
	*da = rsp->smmu_map[0].da;
bail:
	if (rsp)
		vfastrpc_rxbuf_send(vfl, rsp, me->buf_size);
	if (msg)
		virt_free_msg(vfl, msg);
	if (intmap)
		vfastrpc_free_buffer(&intbuf);
	return err;
}

static int virt_smmu_unmap(struct vfastrpc_file *vfl, uint64_t da)
{
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	struct vfastrpc_apps *me = vfl->apps;
	struct virt_smmu_unmap_msg *vmsg, *rsp = NULL;
	struct virt_fastrpc_msg *msg;
	int err, total_size;

	spin_lock(&fl->hlock);
	if (fl->file_close >= FASTRPC_PROCESS_EXIT_START) {
		spin_unlock(&fl->hlock);
		return -ESHUTDOWN;
	}
	spin_unlock(&fl->hlock);

	total_size = sizeof(*vmsg) + sizeof(uint64_t);

	msg = virt_alloc_msg(vfl, total_size);
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

	err = vfastrpc_txbuf_send(vfl, vmsg, total_size);
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
		ADSPRPC_ERR("failed to unmap smmu da = %llx\n", da);
	if (rsp)
		vfastrpc_rxbuf_send(vfl, rsp, me->buf_size);
	virt_free_msg(vfl, msg);
	return err;
}

int hfastrpc_mmap_create(struct vfastrpc_file *vfl, int fd,
	unsigned int attr, uintptr_t va, size_t len, int mflags,
	struct vfastrpc_mmap **ppmap)
{
	struct vfastrpc_apps *me = vfl->apps;
	struct vfastrpc_mmap *map = NULL;
	int err = 0, sgl_index = 0;
	struct scatterlist *sgl = NULL;

	ADSP_LOG("fd=%d,va=%lx,len=%ld\n", fd, va, len);
	if (!vfastrpc_mmap_find(vfl, fd, va, len, mflags, 1, ppmap)) {
		if (!(*ppmap)->da)
			dev_err(me->dev, "find invalid map, fd=%d,va=%lx,len=%ld\n",
					fd, va, len);
		return 0;
	}

	map = kzalloc(sizeof(*map), GFP_KERNEL);
	VERIFY(err, !IS_ERR_OR_NULL(map));
	if (err)
		goto bail;

	INIT_HLIST_NODE(&map->hn);
	map->flags = mflags;
	map->refs = 1;
	map->vfl = vfl;
	map->fd = fd;
	map->attr = attr;
	map->ctx_refs = 0;
	if (mflags == ADSP_MMAP_HEAP_ADDR ||
			mflags == ADSP_MMAP_REMOTE_HEAP_ADDR) {
		dev_err(me->dev, "%s ADSP_MMAP_HEAP_ADDR is not supported\n",
				__func__);
		err = -EINVAL;
		goto bail;
	} else {
		if (map->attr && (map->attr & FASTRPC_ATTR_KEEP_MAP))
			map->refs = 2;

		VERIFY(err, !IS_ERR_OR_NULL(map->buf = dma_buf_get(fd)));
		if (err) {
			dev_err(me->dev, "can't get dma buf fd %d\n", fd);
			goto bail;
		}

		VERIFY(err, !IS_ERR_OR_NULL(map->attach =
					dma_buf_attach(map->buf, me->dev)));
		if (err) {
			dev_err(me->dev, "can't attach dma buf\n");
			goto bail;
		}

		/*
		 * no need to sync cache even for cached buffers, depending on
		 * IO coherency
		 */
		map->attach->dma_map_attrs |= DMA_ATTR_SKIP_CPU_SYNC;
		VERIFY(err, !IS_ERR_OR_NULL(map->table =
					dma_buf_map_attachment(map->attach,
					DMA_BIDIRECTIONAL)));
		if (err) {
			dev_err(me->dev, "can't get sg table of dma buf\n");
			goto bail;
		}
		map->phys = sg_dma_address(map->table->sgl);
		for_each_sg(map->table->sgl, sgl, map->table->nents, sgl_index)
			map->size += sg_dma_len(sgl);
		map->va = va;
	}
	map->len = len;

	err = virt_smmu_map(vfl, VFASTRPC_MAP_ATTR_CACHED,
					map->table->sgl,
					map->table->nents,
					&map->da);
	if (err) {
		dev_err(me->dev, "can't get map da\n");
		goto bail;
	}

	vfastrpc_mmap_add(vfl, map);
	*ppmap = map;
	ADSP_LOG("Create new map %lx, refs=%d, attr=%x\n",
			map, map->refs, map->attr);
bail:
	if (err && map)
		hfastrpc_mmap_free(vfl, map, 0);
	return err;
}

void hfastrpc_mmap_free(struct vfastrpc_file *vfl,
		struct vfastrpc_mmap *map, uint32_t force_free)
{
	struct vfastrpc_apps *me = vfl->apps;

	if (!map)
		return;

	ADSP_LOG("Free map %lx, refs=%d, attr=%x\n",
			map, map->refs, map->attr);
	if (map->flags == ADSP_MMAP_HEAP_ADDR ||
				map->flags == ADSP_MMAP_REMOTE_HEAP_ADDR) {
		dev_err(me->dev, "%s ADSP_MMAP_HEAP_ADDR is not supported\n",
				__func__);
	} else {
		if (map->refs <= 0 || map->ctx_refs < 0 || map->dma_handle_refs < 0) {
			dev_warn(me->dev,
				"%s refs = %d ctx_refs = %d dma_handle_refs = %d is abnormal\n",
				__func__, map->refs, map->ctx_refs, map->dma_handle_refs);
			return;
		}

		map->refs--;
		if (force_free) {
			/*
			 * We only allow force_free happen for DMA-BUF with FASTRPC_ATTR_KEEP_MAP
			 * attribute set, because client could call remote_handle_close first,
			 * then call rpcmem_free.
			 */
			if (map->refs || map->ctx_refs || map->dma_handle_refs ||
					(map->refs == 0 && !(map->attr & FASTRPC_ATTR_KEEP_MAP)))
				dev_warn(me->dev,
					"force free, refs = %d ctx_refs = %d dma_handle_refs = %d attr = 0x%x\n",
					map->refs + 1, map->ctx_refs, map->dma_handle_refs,
					map->attr);
			map->refs = 0;
			map->ctx_refs = 0;
			map->dma_handle_refs = 0;
		}

		if (!map->refs && !map->ctx_refs && !map->dma_handle_refs) {
			if (map->da)
				virt_smmu_unmap(vfl, map->da);

			hlist_del_init(&map->hn);
			if (!IS_ERR_OR_NULL(map->table)) {
				dma_buf_unmap_attachment(map->attach, map->table,
						DMA_BIDIRECTIONAL);
				map->table = NULL;
			}

			if (!IS_ERR_OR_NULL(map->attach)) {
				dma_buf_detach(map->buf, map->attach);
				map->attach = NULL;
			}

			if (!IS_ERR_OR_NULL(map->buf)) {
				dma_buf_put(map->buf);
				map->buf = NULL;
			}
			kfree(map);
		}
	}
}

int hfastrpc_mmap_remove_fd(struct vfastrpc_file *vfl, int fd)
{
	struct vfastrpc_mmap *match = NULL, *map = NULL;
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	struct hlist_node *n;
	int err = 0;

	hlist_for_each_entry_safe(map, n, &fl->maps, hn) {
		if ((map->fd == fd) &&
				(map->attr & FASTRPC_ATTR_KEEP_MAP)) {
			match = map;
			if (match->refs > 1 || match->ctx_refs) {
				dev_err(vfl->apps->dev,
						"%s map refs = %d or ctx_refs = %d is abnormal\n",
						__func__, match->refs, match->ctx_refs);
				err = -ETOOMANYREFS;
			}
			map->attr = map->attr & (~FASTRPC_ATTR_KEEP_MAP);
			hfastrpc_mmap_free(vfl, match, 0);
		}
	}
	return err;
}

void hfastrpc_buf_free(struct vfastrpc_buf *buf, int cache)
{
	struct vfastrpc_file *vfl = buf == NULL ? NULL : buf->vfl;
	struct fastrpc_file *fl = vfl == NULL ? NULL : to_fastrpc_file(vfl);

	if (!vfl || !fl)
		return;

	if (buf->pers_hdr_in_use) {
		/* Don't free persistent header buf. Just mark as available */
		spin_lock(&fl->hlock);
		buf->pers_hdr_in_use = false;
		spin_unlock(&fl->hlock);
		return;
	}

	if (cache && buf->size < MAX_CACHE_BUF_SIZE) {
		spin_lock(&fl->hlock);
		if (fl->num_cached_buf > MAX_CACHED_BUFS) {
			spin_unlock(&fl->hlock);
			dev_dbg(vfl->apps->dev, "num_cached_buf reaches upper limit\n");
			goto skip_buf_cache;
		}
		hlist_add_head(&buf->hn, &fl->cached_bufs);
		fl->num_cached_buf++;
		buf->type = -1;
		spin_unlock(&fl->hlock);
		return;
	}

skip_buf_cache:
	if (buf->type == VFASTRPC_BUF_TYPE_USERHEAP) {
		spin_lock(&fl->hlock);
		hlist_del_init(&buf->hn_rem);
		spin_unlock(&fl->hlock);
		buf->raddr = 0;
	}

	if (buf->da)
		virt_smmu_unmap(vfl, buf->da);
	if (!IS_ERR_OR_NULL(buf->pages))
		vfastrpc_free_buffer(buf);
	kfree(buf);
}

static inline bool hfastrpc_get_cached_buf(struct vfastrpc_file *vfl,
		size_t size, int buf_type, struct vfastrpc_buf **obuf)
{
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	bool found = false;
	struct vfastrpc_buf *buf = NULL, *fr = NULL;
	struct hlist_node *n = NULL;

	if (buf_type == VFASTRPC_BUF_TYPE_USERHEAP)
		goto bail;

	/* find the smallest buffer that fits in the cache */
	spin_lock(&fl->hlock);
	hlist_for_each_entry_safe(buf, n, &fl->cached_bufs, hn) {
		if (buf->size >= size && (!fr || fr->size > buf->size))
			fr = buf;
	}
	if (fr) {
		hlist_del_init(&fr->hn);
		fl->num_cached_buf--;
	}
	spin_unlock(&fl->hlock);
	if (fr) {
		fr->type = buf_type;
		*obuf = fr;
		found = true;
	}
bail:
	return found;
}

static inline bool hfastrpc_get_persistent_buf(struct vfastrpc_file *vfl,
		size_t size, int buf_type, struct vfastrpc_buf **obuf)
{
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	unsigned int i = 0;
	bool found = false;
	struct vfastrpc_buf *buf = NULL;

	spin_lock(&fl->hlock);
	if (!vfl->num_pers_hdrs)
		goto bail;

	/*
	 * Persistent header buffer can be used only if
	 * metadata length is no more than 1 page size.
	 */
	if (buf_type != VFASTRPC_BUF_TYPE_METADATA || size > PAGE_SIZE)
		goto bail;

	for (i = 0; i < vfl->num_pers_hdrs; i++) {
		buf = &vfl->hdr_bufs[i];
		/* If buffer not in use, then assign it for requested alloc */
		if (!buf->pers_hdr_in_use) {
			buf->pers_hdr_in_use = true;
			*obuf = buf;
			found = true;
			break;
		}
	}
bail:
	spin_unlock(&fl->hlock);
	return found;
}

int hfastrpc_buf_alloc(struct vfastrpc_file *vfl, size_t size,
				unsigned long dma_attr, uint32_t rflags,
				int buf_type, pgprot_t prot, struct vfastrpc_buf **obuf)
{
	struct vfastrpc_apps *me = vfl->apps;
	struct fastrpc_file *fl = to_fastrpc_file(vfl);
	struct vfastrpc_buf *buf = NULL;
	int err = 0;

	VERIFY(err, size > 0 && size < MAX_BUF_SIZE);
	if (err) {
		ADSPRPC_ERR("Invalid buffer size, 0x%zx\n", size);
		goto bail;
	}

	if (hfastrpc_get_persistent_buf(vfl, size, buf_type, obuf))
		goto bail;

	if (hfastrpc_get_cached_buf(vfl, size, buf_type, obuf))
		goto bail;

	VERIFY(err, NULL != (buf = kzalloc(sizeof(*buf), GFP_KERNEL)));
	if (err)
		goto bail;
	buf->vfl = vfl;
	/*
	 * For buf_type that could be cached, we save the page-aligned size,
	 * because the buffer allocation is page-aligned underline and the
	 * entire buffer is reusable.
	 */
	buf->size = (buf_type == VFASTRPC_BUF_TYPE_USERHEAP) ? size : PAGE_ALIGN(size);
	buf->va = NULL;
	buf->dma_attr = dma_attr;
	buf->map_attr = 0;
	buf->flags = rflags;
	buf->type = buf_type;
	buf->raddr = 0;
	buf->pages = vfastrpc_alloc_buffer(me->dev, buf, GFP_KERNEL, prot);
	if (IS_ERR_OR_NULL(buf->pages)) {
		err = -ENOMEM;
		dev_err(me->dev,
			"%s: %s: fastrpc_alloc_buffer failed for size 0x%zx, returned %ld\n",
			current->comm, __func__, size, PTR_ERR(buf->pages));
		goto bail;
	}

	err = virt_smmu_map(vfl, VFASTRPC_MAP_ATTR_CACHED,
					buf->sgt.sgl,
					buf->sgt.nents, &buf->da);
	if (err) {
		dev_err(me->dev, "can't get buf da\n");
		goto bail;
	}

	if (buf_type == VFASTRPC_BUF_TYPE_USERHEAP) {
		INIT_HLIST_NODE(&buf->hn_rem);
		spin_lock(&fl->hlock);
		hlist_add_head(&buf->hn_rem, &fl->remote_bufs);
		spin_unlock(&fl->hlock);
	}

	*obuf = buf;
 bail:
	if (err && buf)
		hfastrpc_buf_free(buf, 0);
	return err;
}
