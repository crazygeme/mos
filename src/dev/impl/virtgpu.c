/* VirtIO 1.0 PCI GPU transport and Linux-compatible VirGL/DRM interfaces. */
#include <dev/dev.h>
#include <errno.h>
#include <ext4_oflags.h>
#include <fs/fcntl.h>
#include <hw/pci.h>
#include <fs/sysfs.h>
#include <hw/time.h>
#include <hw/drm/virtgpu_drm.h>
#include <hw/drm/virtio_gpu.h>
#include <lib/klib.h>
#include <lib/lock.h>
#include <macro.h>
#include <mm/mmap.h>
#include <mm/mmu.h>
#include <mm/phymm.h>
#include <ps/ps.h>

#define GPU_MAJOR 226
#define GPU_RENDER_MINOR 128
#define GPU_QUEUE_SIZE 256
#define GPU_MAX_OBJECTS 4096
#define GPU_MAX_HANDLES 2048
#define GPU_MAX_FB 256
#define GPU_MAX_BYTES (64U * 1024 * 1024)
#define GPU_MAX_COMMAND (512U * 1024)
#define GPU_CRTC 1
#define GPU_ENCODER 2
#define GPU_CONNECTOR 3
#define GPU_MODE_WIDTH 1920
#define GPU_MODE_HEIGHT 1080

struct gpu_desc {
	uint64_t addr;
	uint32_t len;
	uint16_t flags, next;
};
struct gpu_avail {
	uint16_t flags, idx, ring[GPU_QUEUE_SIZE];
};
struct gpu_used_entry {
	uint32_t id, len;
};
struct gpu_used {
	uint16_t flags, idx;
	struct gpu_used_entry ring[GPU_QUEUE_SIZE];
};
struct gpu_common {
	uint32_t device_feature_select, device_feature;
	uint32_t driver_feature_select, driver_feature;
	uint16_t msix_config, num_queues;
	uint8_t device_status, config_generation;
	uint16_t queue_select, queue_size, queue_msix_vector, queue_enable;
	uint16_t queue_notify_off;
	uint64_t queue_desc, queue_driver, queue_device;
} __attribute__((packed));
struct gpu_bo {
	file *file;
	unsigned id, slot, size, pages, stride, width, height, named, dumb;
	unsigned submission_ctx;
	paddr_t *backing;
	unsigned host_created;
	uint64_t submission;
};
struct gpu_client {
	unsigned ctx, rdev, authenticated, busid;
	file *handles[GPU_MAX_HANDLES];
};
struct gpu_fb {
	unsigned id, owner, width, height, pitch, depth;
	file *buffer;
};
static volatile struct gpu_common *gpu_common;
static volatile uint16_t *gpu_notify;
static struct gpu_desc *gpu_desc;
static volatile struct gpu_avail *gpu_avail;
static volatile struct gpu_used *gpu_used;
static uint16_t gpu_index;
static unsigned gpu_pci = ~0U, gpu_ready, gpu_next_context = 1;
static unsigned gpu_next_resource = 1;
static uint64_t gpu_fence, gpu_submission, gpu_completed;
static void *gpu_dma_input, *gpu_dma_output;
static unsigned gpu_pending, gpu_pending_size, gpu_pending_fenced;
static uint64_t gpu_pending_fence, gpu_pending_submission, gpu_deadline;
static struct gpu_bo *gpu_objects[GPU_MAX_OBJECTS];
static struct gpu_fb gpu_fbs[GPU_MAX_FB];
static struct gpu_client *gpu_master;
static file *gpu_scanout;
static unsigned gpu_scanout_fb;
static rmutex_t gpu_lock;
static const file_operations gpu_buffer_fops;

/* User ranges are checked against VM regions before any ioctl data access. */
static int gpu_user_range(uint64_t pointer, unsigned size, unsigned write)
{
	vaddr_t pos, end;
	if (!size)
		return 1;
	if (!current || !current->user || pointer < PAGE_SIZE ||
	    pointer >= KERNEL_OFFSET || size > KERNEL_OFFSET - pointer)
		return 0;
	pos = pointer;
	end = pos + size;
	while (pos < end) {
		vm_region *region = vm_find_map(current->user->vm, pos);
		if (!region ||
		    !(region->prot & (write ? PROT_WRITE : PROT_READ)))
			return 0;
		pos = region->end < end ? region->end : end;
	}
	return 1;
}

static int gpu_out(uint64_t pointer, const void *data, unsigned size)
{
	if (!gpu_user_range(pointer, size, 1))
		return -EFAULT;
	memcpy((void *)(uintptr_t)pointer, data, size);
	return 0;
}

static void *gpu_pages(unsigned size)
{
	unsigned count = (size + PAGE_SIZE - 1) / PAGE_SIZE;
	vaddr_t memory = vm_alloc(count);
	if (memory)
		memset((void *)memory, 0, count * PAGE_SIZE);
	return (void *)memory;
}

static void gpu_free_pages(void *memory, unsigned size)
{
	if (memory)
		vm_free((vaddr_t)memory, (size + PAGE_SIZE - 1) / PAGE_SIZE);
}

static unsigned gpu_chain(void *data, unsigned length, unsigned first,
			  int write)
{
	unsigned n = first;
	while (length) {
		unsigned bytes =
			PAGE_SIZE - ((uintptr_t)data & (PAGE_SIZE - 1));
		if (bytes > length)
			bytes = length;
		if (n >= GPU_QUEUE_SIZE)
			return GPU_QUEUE_SIZE + 1;
		gpu_desc[n].addr = VIRT_TO_PHY(data);
		gpu_desc[n].len = bytes;
		gpu_desc[n].flags = 1 | (write ? 2 : 0);
		gpu_desc[n].next = n + 1;
		data = (char *)data + bytes;
		length -= bytes;
		n++;
	}
	return n;
}

/* The control queue reuses pinned DMA buffers. A pending fence owns the
 * buffers until completion, including across nonblocking resource queries. */
static int gpu_complete(int wait)
{
	struct virtio_gpu_ctrl_hdr *reply = gpu_dma_output;
	unsigned used_len;
	uint64_t yield_at = time_now_us() + 100;
	if (!gpu_pending)
		return 0;
	while (gpu_used->idx == gpu_index) {
		if (time_now_us() >= gpu_deadline) {
			gpu_common->device_status = 0;
			while (gpu_common->device_status)
				PAUSE();
			gpu_ready = gpu_pending = 0;
			klog("virtio_gpu: command timeout; device reset\n");
			return -ETIMEDOUT;
		}
		if (!wait)
			return -EBUSY;
		/* Keep input and other tasks runnable during host GPU waits. */
		if (ps_enabled() && sched_is_enabled() &&
		    time_now_us() >= yield_at) {
			task_sched();
			yield_at = time_now_us() + 100;
		} else {
			PAUSE();
		}
	}
	__sync_synchronize();
	used_len = gpu_used->ring[gpu_index % GPU_QUEUE_SIZE].len;
	gpu_index++;
	gpu_pending = 0;
	if (used_len < sizeof(*reply) || used_len > gpu_pending_size ||
	    reply->type < VIRTIO_GPU_RESP_OK_NODATA ||
	    reply->type >= VIRTIO_GPU_RESP_ERR_UNSPEC ||
	    (gpu_pending_fenced && (!(reply->flags & VIRTIO_GPU_FLAG_FENCE) ||
				    reply->fence_id != gpu_pending_fence)))
		return -EIO;
	if (gpu_pending_fenced)
		gpu_completed = gpu_pending_submission;
	return 0;
}

static int gpu_command(void *request, unsigned request_size, void *response,
		       unsigned response_size, int fenced)
{
	struct virtio_gpu_ctrl_hdr *hdr = request;
	unsigned count;
	int result;
	if (!gpu_ready || request_size > GPU_MAX_COMMAND ||
	    response_size > GPU_MAX_COMMAND)
		return -ENODEV;
	result = gpu_complete(1);
	if (result)
		return result;
	if (fenced) {
		hdr->flags |= VIRTIO_GPU_FLAG_FENCE;
		hdr->fence_id = ++gpu_fence;
	}
	memcpy(gpu_dma_input, request, request_size);
	memset(gpu_dma_output, 0, response_size);
	count = gpu_chain(gpu_dma_input, request_size, 0, 0);
	count = gpu_chain(gpu_dma_output, response_size, count, 1);
	if (!count || count > GPU_QUEUE_SIZE)
		return -E2BIG;
	gpu_desc[count - 1].flags &= ~1U;
	gpu_pending = 1;
	gpu_pending_size = response_size;
	gpu_pending_fenced = fenced;
	gpu_pending_fence = hdr->fence_id;
	gpu_pending_submission = gpu_submission;
	gpu_deadline = time_now_us() + 5000000ULL;
	gpu_avail->ring[gpu_index % GPU_QUEUE_SIZE] = 0;
	__sync_synchronize();
	gpu_avail->idx = gpu_index + 1;
	__sync_synchronize();
	*gpu_notify = 0;
	result = gpu_complete(fenced != 2);
	if (!result)
		memcpy(response, gpu_dma_output, response_size);
	return result;
}

static int gpu_simple(void *request, unsigned size, int fenced)
{
	struct virtio_gpu_ctrl_hdr response;
	return gpu_command(request, size, &response, sizeof(response), fenced);
}

static int gpu_context_resource(unsigned type, unsigned ctx, unsigned id)
{
	struct virtio_gpu_ctx_resource req = { 0 };
	req.hdr.type = type;
	req.hdr.ctx_id = ctx;
	req.resource_id = id;
	return gpu_simple(&req, sizeof(req), 0);
}

static int gpu_idle_mode(unsigned ctx, int mode)
{
	struct virtio_gpu_cmd_submit req = { 0 };
	req.hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
	req.hdr.ctx_id = ctx;
	return gpu_simple(&req, sizeof(req), mode);
}

static int gpu_idle(unsigned ctx)
{
	return gpu_idle_mode(ctx, 1);
}

/* Each graphics page owns one allocator reference independently of VMAs. */
static void gpu_backing_free(struct gpu_bo *bo)
{
	unsigned i;
	for (i = 0; i < bo->pages; i++) {
		unsigned page = bo->backing[i] / PAGE_SIZE;
		if (!phymm_dereference_page(page))
			phymm_free_user(page);
	}
	free(bo->backing);
	bo->backing = NULL;
	bo->pages = 0;
}

static int gpu_backing_alloc(struct gpu_bo *bo)
{
	unsigned count = bo->size / PAGE_SIZE;
	bo->backing = zalloc(count * sizeof(*bo->backing));
	if (!bo->backing)
		return -ENOMEM;
	while (bo->pages < count) {
		unsigned page = phymm_alloc_user();
		paddr_t phys;
		if (page == PHYMM_INVALID) {
			phymm_reclaim_user_cache(32);
			page = phymm_alloc_user();
			if (page == PHYMM_INVALID)
				goto fail;
		}
		phys = page * PAGE_SIZE;
		phymm_reference_page(page);
		bo->backing[bo->pages++] = phys;
		if (mm_kmap_phys(phys) != 1)
			goto fail;
		memset((void *)PHY_TO_VIRT(phys), 0, PAGE_SIZE);
		mm_kunmap_phys(phys);
	}
	return 0;
fail:
	gpu_backing_free(bo);
	return -ENOMEM;
}

static int gpu_buffer_release(file *fp)
{
	struct gpu_bo *bo = fp->f_inode->i_private;
	struct virtio_gpu_resource_unref req = { 0 };
	rmutex_lock(&gpu_lock);
	req.hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
	req.resource_id = bo->id;
	if (bo->host_created && gpu_simple(&req, sizeof(req), 1)) {
		/* Retain DMA backing if host detachment cannot be confirmed. */
		klog("virtio_gpu: retaining resource %u after unref failure\n",
		     bo->id);
		rmutex_unlock(&gpu_lock);
		return 0;
	}
	gpu_objects[bo->slot] = NULL;
	gpu_backing_free(bo);
	free(bo);
	free(fp->f_inode);
	free(fp);
	rmutex_unlock(&gpu_lock);
	return 0;
}

static paddr_t gpu_buffer_page(file *fp, unsigned offset)
{
	struct gpu_bo *bo = fp->f_inode->i_private;
	if ((offset & (PAGE_SIZE - 1)) || offset >= bo->size)
		return 0;
	return bo->backing[offset / PAGE_SIZE];
}

static int gpu_buffer_map(file *fp, unsigned *offset, unsigned size,
			  unsigned prot, unsigned flags, file **backing)
{
	struct gpu_bo *bo = fp->f_inode->i_private;
	if (!size || (*offset & (PAGE_SIZE - 1)) || *offset >= bo->size ||
	    size > bo->size - *offset || (flags & MAP_TYPE) != MAP_SHARED ||
	    (prot & PROT_EXEC))
		return -EINVAL;
	fs_get_file(fp);
	*backing = fp;
	return 0;
}

static int gpu_buffer_stat(file *fp, struct stat *st)
{
	struct gpu_bo *bo = fp->f_inode->i_private;
	memset(st, 0, sizeof(*st));
	st->st_mode = S_IFREG | 0600;
	st->st_ino = bo->id;
	st->st_size = bo->size;
	st->st_blksize = PAGE_SIZE;
	st->st_nlink = 1;
	return 0;
}

static loff_t gpu_buffer_seek(file *fp, loff_t offset, int whence)
{
	struct gpu_bo *bo = fp->f_inode->i_private;
	if (offset || (whence != 0 && whence != 2))
		return -EINVAL;
	return whence == 2 ? bo->size : 0;
}

static const file_operations gpu_buffer_fops = {
	.release = gpu_buffer_release,
	.getattr = gpu_buffer_stat,
	.llseek = gpu_buffer_seek,
	.map_page = gpu_buffer_page,
	.mmap_file = gpu_buffer_map,
};

static struct gpu_bo *gpu_handle(struct gpu_client *client, unsigned handle)
{
	if (!handle || handle >= GPU_MAX_HANDLES || !client->handles[handle])
		return NULL;
	return client->handles[handle]->f_inode->i_private;
}

static int gpu_add_handle(struct gpu_client *client, file *fp)
{
	struct gpu_bo *bo = fp->f_inode->i_private;
	unsigned i;
	for (i = 1; i < GPU_MAX_HANDLES; i++)
		if (client->handles[i] == fp)
			return i;
	for (i = 1; i < GPU_MAX_HANDLES; i++)
		if (!client->handles[i]) {
			int result = gpu_context_resource(
				VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE, client->ctx,
				bo->id);
			if (result)
				return result;
			fs_get_file(fp);
			client->handles[i] = fp;
			return i;
		}
	return -EMFILE;
}

static int gpu_drop_handle(struct gpu_client *client, unsigned handle)
{
	struct gpu_bo *bo = gpu_handle(client, handle);
	file *fp;
	if (!bo)
		return -ENOENT;
	fp = client->handles[handle];
	client->handles[handle] = NULL;
	gpu_context_resource(VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE, client->ctx,
			     bo->id);
	fs_put_file(fp);
	return 0;
}

static int gpu_create(struct gpu_client *client,
		      struct drm_virtgpu_resource_create *arg, int dumb)
{
	struct virtio_gpu_resource_create_3d create = { 0 };
	struct virtio_gpu_resource_attach_backing *attach;
	struct virtio_gpu_mem_entry *entries;
	struct gpu_bo *bo;
	file *fp;
	unsigned id, slot, bytes, i;
	int result;
	if (arg->bo_handle || arg->size > GPU_MAX_BYTES || !arg->width ||
	    arg->flags & ~VIRTIO_GPU_RESOURCE_FLAG_Y_0_TOP)
		return -EINVAL;
	for (slot = 0; slot < GPU_MAX_OBJECTS && gpu_objects[slot]; slot++) {
	}
	if (slot == GPU_MAX_OBJECTS || !gpu_next_resource)
		return -ENOSPC;
	/* Host resource identifiers are not reused during a device lifetime. */
	id = gpu_next_resource++;
	fp = zalloc(sizeof(*fp));
	bo = zalloc(sizeof(*bo));
	if (!fp || !bo) {
		free(fp);
		free(bo);
		return -ENOMEM;
	}
	fp->f_inode = zalloc(sizeof(*fp->f_inode));
	if (!fp->f_inode) {
		free(fp);
		free(bo);
		return -ENOMEM;
	}
	bo->size = (arg->size + PAGE_SIZE - 1) & PAGE_SIZE_MASK;
	if (!bo->size)
		bo->size = PAGE_SIZE;
	if (gpu_backing_alloc(bo)) {
		free(fp->f_inode);
		free(fp);
		free(bo);
		return -ENOMEM;
	}
	bo->id = id;
	bo->slot = slot;
	bo->file = fp;
	bo->stride = arg->stride;
	bo->width = arg->width;
	bo->height = arg->height;
	bo->dumb = dumb;
	/* Mesa legacy fences use a resource created after the submitted batch. */
	bo->submission = gpu_submission;
	bo->submission_ctx = client->ctx;
	fp->f_inode->i_private = bo;
	fp->f_inode->i_mode = S_IFREG | 0600;
	fp->f_inode->i_size = bo->size;
	fp->f_fop = &gpu_buffer_fops;
	fp->f_count = 1;
	fp->f_mode = O_RDWR;
	gpu_objects[slot] = bo;
	create.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
	create.resource_id = id;
	create.target = arg->target;
	create.format = arg->format;
	create.bind = arg->bind;
	create.width = arg->width;
	create.height = arg->height;
	create.depth = arg->depth;
	create.array_size = arg->array_size;
	create.last_level = arg->last_level;
	create.nr_samples = arg->nr_samples;
	create.flags = arg->flags;
	result = gpu_simple(&create, sizeof(create), 0);
	if (result)
		goto fail;
	bo->host_created = 1;
	bytes = sizeof(*attach) + bo->pages * sizeof(*entries);
	attach = gpu_pages(bytes);
	if (!attach) {
		result = -ENOMEM;
		goto fail;
	}
	attach->hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
	attach->resource_id = id;
	attach->nr_entries = bo->pages;
	entries = (void *)(attach + 1);
	for (i = 0; i < bo->pages; i++) {
		entries[i].addr = bo->backing[i];
		entries[i].length = PAGE_SIZE;
	}
	result = gpu_simple(attach, bytes, 0);
	gpu_free_pages(attach, bytes);
	if (result)
		goto fail;
	result = gpu_add_handle(client, fp);
	if (result < 0)
		goto fail;
	arg->bo_handle = result;
	arg->res_handle = id;
	fs_put_file(fp);
	return 0;
fail:
	fs_put_file(fp);
	return result;
}

static int gpu_drm_map(file *fp, unsigned *offset, unsigned size, unsigned prot,
		       unsigned flags, file **backing)
{
	struct gpu_client *client = fp->f_inode->i_private;
	struct gpu_bo *bo;
	int result;
	rmutex_lock(&gpu_lock);
	bo = gpu_handle(client, *offset / PAGE_SIZE);
	if (!bo || (*offset & (PAGE_SIZE - 1)))
		result = -EINVAL;
	else {
		*offset = 0;
		result = gpu_buffer_map(bo->file, offset, size, prot, flags,
					backing);
	}
	rmutex_unlock(&gpu_lock);
	return result;
}

static const struct drm_mode_modeinfo gpu_display_mode = {
	.clock = 297000,
	.hdisplay = GPU_MODE_WIDTH,
	.hsync_start = 2008,
	.hsync_end = 2052,
	.htotal = 2200,
	.vdisplay = GPU_MODE_HEIGHT,
	.vsync_start = 1084,
	.vsync_end = 1089,
	.vtotal = 1125,
	.vrefresh = 120,
	.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC,
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
	.name = "1920x1080",
};

static int gpu_upload_dumb(struct gpu_fb *fb)
{
	struct gpu_bo *bo = fb->buffer->f_inode->i_private;
	struct virtio_gpu_transfer_host_3d req = { 0 };
	if (!bo->dumb)
		return 0;
	req.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D;
	req.hdr.ctx_id = fb->owner;
	req.box.w = bo->width;
	req.box.h = bo->height;
	req.box.d = 1;
	req.resource_id = bo->id;
	req.stride = bo->stride;
	return gpu_simple(&req, sizeof(req), 1);
}

static int gpu_flush(struct gpu_bo *bo)
{
	struct virtio_gpu_resource_flush req = { 0 };
	req.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
	req.resource_id = bo->id;
	req.r.width = bo->width;
	req.r.height = bo->height;
	/* Presentation does not require a synchronous resource fence. */
	return gpu_simple(&req, sizeof(req), 0);
}

static int gpu_set_scanout(struct gpu_fb *fb)
{
	struct virtio_gpu_set_scanout req = { 0 };
	struct gpu_bo *bo = fb ? fb->buffer->f_inode->i_private : NULL;
	int result;
	/* Closing a probe/master descriptor must not disable the VGA console
	 * when no KMS scanout has been installed. */
	if (!fb && !gpu_scanout)
		return 0;
	req.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
	if (bo) {
		result = gpu_upload_dumb(fb);
		if (result)
			return result;
		req.resource_id = bo->id;
		req.r.width = fb->width;
		req.r.height = fb->height;
	}
	result = gpu_simple(&req, sizeof(req), 1);
	if (result) {
		klog("virtio_gpu: set_scanout resource=%u size=%ux%u failed: %d\n",
		     req.resource_id, req.r.width, req.r.height, result);
		return result;
	}
	klog("virtio_gpu: scanout resource=%u size=%ux%u\n", req.resource_id,
	     req.r.width, req.r.height);
	if (fb)
		fs_get_file(fb->buffer);
	if (gpu_scanout)
		fs_put_file(gpu_scanout);
	gpu_scanout = fb ? fb->buffer : NULL;
	gpu_scanout_fb = fb ? fb->id : 0;
	return bo ? gpu_flush(bo) : 0;
}

static int gpu_add_fb(struct gpu_client *client, unsigned handle,
		      unsigned width, unsigned height, unsigned pitch,
		      unsigned depth, unsigned *id)
{
	struct gpu_bo *bo = gpu_handle(client, handle);
	unsigned i;
	if (!bo || width != bo->width || height != bo->height || !width ||
	    !height || width > 4096 || height > 4096 || pitch < width * 4 ||
	    (uint64_t)pitch * height > bo->size)
		return -EINVAL;
	for (i = 0; i < GPU_MAX_FB; i++)
		if (!gpu_fbs[i].buffer) {
			struct gpu_fb *fb = &gpu_fbs[i];
			fb->id = i + 16;
			fb->owner = client->ctx;
			fb->width = width;
			fb->height = height;
			fb->pitch = pitch;
			fb->depth = depth;
			fb->buffer = bo->file;
			fs_get_file(bo->file);
			*id = fb->id;
			return 0;
		}
	return -ENOSPC;
}

static struct gpu_fb *gpu_fb(unsigned id)
{
	if (id < 16 || id >= GPU_MAX_FB + 16 || !gpu_fbs[id - 16].buffer)
		return NULL;
	return &gpu_fbs[id - 16];
}

static int gpu_kms(struct gpu_client *client, unsigned cmd, void *arg)
{
	unsigned value;
	const struct drm_mode_modeinfo *mode = &gpu_display_mode;
	switch (cmd) {
	case DRM_IOCTL_MODE_GETRESOURCES: {
		struct drm_mode_card_res *r = arg;
		unsigned count = 0, index;
		for (index = 0; index < GPU_MAX_FB; index++) {
			if (!gpu_fbs[index].buffer ||
			    gpu_fbs[index].owner != client->ctx)
				continue;
			if (count < r->count_fbs &&
			    gpu_out(r->fb_id_ptr + count * 4,
				    &gpu_fbs[index].id, 4))
				return -EFAULT;
			count++;
		}
		value = GPU_CRTC;
		if (r->count_crtcs && gpu_out(r->crtc_id_ptr, &value, 4))
			return -EFAULT;
		value = GPU_ENCODER;
		if (r->count_encoders && gpu_out(r->encoder_id_ptr, &value, 4))
			return -EFAULT;
		value = GPU_CONNECTOR;
		if (r->count_connectors &&
		    gpu_out(r->connector_id_ptr, &value, 4))
			return -EFAULT;
		r->count_fbs = count;
		r->count_crtcs = r->count_encoders = r->count_connectors = 1;
		r->min_width = r->min_height = 1;
		r->max_width = r->max_height = 4096;
		return 0;
	}
	case DRM_IOCTL_MODE_GETCONNECTOR: {
		struct drm_mode_get_connector *r = arg;
		if (r->connector_id != GPU_CONNECTOR)
			return -ENOENT;
		value = GPU_ENCODER;
		if (r->count_encoders && gpu_out(r->encoders_ptr, &value, 4))
			return -EFAULT;
		if (r->count_modes &&
		    gpu_out(r->modes_ptr, mode, sizeof(*mode)))
			return -EFAULT;
		r->count_modes = r->count_encoders = 1;
		r->count_props = 0;
		r->encoder_id = GPU_ENCODER;
		r->connector_type = DRM_MODE_CONNECTOR_VIRTUAL;
		r->connector_type_id = 1;
		r->connection = 1;
		r->mm_width = 508;
		r->mm_height = 286;
		r->subpixel = 1;
		return 0;
	}
	case DRM_IOCTL_MODE_GETENCODER: {
		struct drm_mode_get_encoder *r = arg;
		if (r->encoder_id != GPU_ENCODER)
			return -ENOENT;
		r->encoder_type = DRM_MODE_ENCODER_VIRTUAL;
		r->crtc_id = GPU_CRTC;
		r->possible_crtcs = 1;
		r->possible_clones = 0;
		return 0;
	}
	case DRM_IOCTL_MODE_GETCRTC: {
		struct drm_mode_crtc *r = arg;
		if (r->crtc_id != GPU_CRTC)
			return -ENOENT;
		r->fb_id = gpu_scanout_fb;
		r->x = r->y = r->gamma_size = 0;
		r->mode_valid = !!gpu_scanout;
		r->mode = *mode;
		return 0;
	}
	case DRM_IOCTL_MODE_SETCRTC: {
		struct drm_mode_crtc *r = arg;
		struct gpu_fb *fb = gpu_fb(r->fb_id);
		if (client != gpu_master)
			return -EACCES;
		if (r->crtc_id != GPU_CRTC || r->x || r->y)
			return -EINVAL;
		if (!r->mode_valid)
			return gpu_set_scanout(NULL);
		if (!fb || r->mode.hdisplay != fb->width ||
		    r->mode.vdisplay != fb->height ||
		    r->count_connectors != 1 ||
		    !gpu_user_range(r->set_connectors_ptr, 4, 0) ||
		    *(uint32_t *)(uintptr_t)r->set_connectors_ptr !=
			    GPU_CONNECTOR)
			return -EINVAL;
		return gpu_set_scanout(fb);
	}
	case DRM_IOCTL_MODE_GETFB: {
		struct drm_mode_fb_cmd *r = arg;
		struct gpu_fb *fb = gpu_fb(r->fb_id);
		int handle;
		if (!fb)
			return -ENOENT;
		r->width = fb->width;
		r->height = fb->height;
		r->pitch = fb->pitch;
		r->depth = fb->depth;
		r->bpp = 32;
		r->handle = 0;
		if (client == gpu_master) {
			handle = gpu_add_handle(client, fb->buffer);
			if (handle < 0)
				return handle;
			r->handle = handle;
		}
		return 0;
	}
	case DRM_IOCTL_MODE_ADDFB: {
		struct drm_mode_fb_cmd *r = arg;
		if (r->bpp != 32 || (r->depth != 24 && r->depth != 32))
			return -EINVAL;
		return gpu_add_fb(client, r->handle, r->width, r->height,
				  r->pitch, r->depth, &r->fb_id);
	}
	case DRM_IOCTL_MODE_ADDFB2: {
		struct drm_mode_fb_cmd2 *r = arg;
		if (r->flags || r->offsets[0] || r->modifier[0] ||
		    r->handles[1] || r->handles[2] || r->handles[3] ||
		    (r->pixel_format != 0x34325258U &&
		     r->pixel_format != 0x34325241U))
			return -EINVAL;
		return gpu_add_fb(client, r->handles[0], r->width, r->height,
				  r->pitches[0],
				  r->pixel_format == 0x34325258U ? 24 : 32,
				  &r->fb_id);
	}
	case DRM_IOCTL_MODE_RMFB: {
		struct gpu_fb *fb = gpu_fb(*(unsigned *)arg);
		if (!fb || fb->owner != client->ctx)
			return -ENOENT;
		if (gpu_scanout_fb == fb->id)
			gpu_set_scanout(NULL);
		fs_put_file(fb->buffer);
		memset(fb, 0, sizeof(*fb));
		return 0;
	}
	case DRM_IOCTL_MODE_DIRTYFB: {
		struct drm_mode_fb_dirty_cmd *r = arg;
		struct gpu_fb *fb = gpu_fb(r->fb_id);
		int result;
		if (!fb || fb->owner != client->ctx)
			return -ENOENT;
		result = gpu_upload_dumb(fb);
		return result ? result :
				gpu_flush(fb->buffer->f_inode->i_private);
	}
	case DRM_IOCTL_MODE_GETPLANERESOURCES:
		((struct drm_mode_get_plane_res *)arg)->count_planes = 0;
		return 0;
	case DRM_IOCTL_MODE_OBJ_GETPROPERTIES:
		((struct drm_mode_obj_get_properties *)arg)->count_props = 0;
		return 0;
	case DRM_IOCTL_MODE_CREATE_DUMB: {
		struct drm_mode_create_dumb *r = arg;
		struct drm_virtgpu_resource_create create = { 0 };
		int result;
		if (r->flags || r->bpp != 32 || !r->width || !r->height ||
		    r->width > 4096 || r->height > 4096)
			return -EINVAL;
		r->pitch = (r->width * 4 + 63) & ~63U;
		r->size = (uint64_t)r->pitch * r->height;
		create.target = 2;
		create.format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
		create.bind = (1U << 1) | (1U << 17) | (1U << 18);
		create.width = r->width;
		create.height = r->height;
		create.depth = create.array_size = 1;
		create.stride = r->pitch;
		create.size = r->size;
		result = gpu_create(client, &create, 1);
		r->handle = create.bo_handle;
		return result;
	}
	case DRM_IOCTL_MODE_MAP_DUMB: {
		struct drm_mode_map_dumb *r = arg;
		if (!gpu_handle(client, r->handle))
			return -ENOENT;
		r->offset = (uint64_t)r->handle * PAGE_SIZE;
		return 0;
	}
	case DRM_IOCTL_MODE_DESTROY_DUMB:
		return gpu_drop_handle(
			client, ((struct drm_mode_destroy_dumb *)arg)->handle);
	default:
		return -EOPNOTSUPP;
	}
}

#define GPU_MAX_CLIENTS 128
static struct gpu_client *gpu_clients[GPU_MAX_CLIENTS];

static int gpu_string(char *dst, size_t *length, const char *src)
{
	unsigned count = strlen(src), copy = *length;
	if (copy > count)
		copy = count;
	if (copy && gpu_out((uintptr_t)dst, src, copy))
		return -EFAULT;
	*length = count;
	return 0;
}

static int gpu_ioctl_locked(struct gpu_client *client, unsigned cmd, void *arg)
{
	unsigned i;
	switch (cmd) {
	case DRM_IOCTL_VERSION: {
		struct drm_version *r = arg;
		r->version_major = 0;
		r->version_minor = 0;
		r->version_patchlevel = 1;
		if (gpu_string(r->name, &r->name_len, "virtio_gpu") ||
		    gpu_string(r->date, &r->date_len, "20260928") ||
		    gpu_string(r->desc, &r->desc_len, "MOS VirtIO GPU"))
			return -EFAULT;
		return 0;
	}
	case DRM_IOCTL_GET_UNIQUE: {
		struct drm_unique *r = arg;
		char *busid = name_get();
		int result;
		if (!busid)
			return -ENOMEM;
		busid[0] = 0;
		if (client->busid)
			sprintf(busid, "pci:0000:%02x:%02x.%u",
				pci_extract_bus(gpu_pci),
				pci_extract_slot(gpu_pci),
				pci_extract_func(gpu_pci));
		result = gpu_string(r->unique, &r->unique_len, busid);
		name_put(busid);
		return result;
	}
	case DRM_IOCTL_SET_VERSION: {
		struct drm_set_version *r = arg;
		client->busid = 1;
		r->drm_di_major = 1;
		r->drm_di_minor = 4;
		r->drm_dd_major = 0;
		r->drm_dd_minor = 0;
		return 0;
	}
	case DRM_IOCTL_GET_MAGIC:
		if (MINOR(client->rdev))
			return -EACCES;
		((struct drm_auth *)arg)->magic = client->ctx;
		return 0;
	case DRM_IOCTL_AUTH_MAGIC:
		if (gpu_master != client)
			return -EACCES;
		for (i = 0; i < GPU_MAX_CLIENTS; i++)
			if (gpu_clients[i] &&
			    gpu_clients[i]->ctx ==
				    ((struct drm_auth *)arg)->magic) {
				gpu_clients[i]->authenticated = 1;
				return 0;
			}
		return -EINVAL;
	case DRM_IOCTL_SET_MASTER:
		if (MINOR(client->rdev) != 0 || current->user->euid)
			return -EACCES;
		if (gpu_master && gpu_master != client)
			return -EBUSY;
		gpu_master = client;
		client->authenticated = 1;
		return 0;
	case DRM_IOCTL_DROP_MASTER:
		if (gpu_master != client)
			return -EACCES;
		gpu_set_scanout(NULL);
		gpu_master = NULL;
		return 0;
	case DRM_IOCTL_GET_CAP: {
		struct drm_get_cap *r = arg;
		r->value = 0;
		switch (r->capability) {
		case DRM_CAP_DUMB_BUFFER:
			r->value = 1;
			break;
		case DRM_CAP_DUMB_PREFERRED_DEPTH:
			r->value = 24;
			break;
		case DRM_CAP_DUMB_PREFER_SHADOW:
			break;
		case DRM_CAP_PRIME:
			r->value = DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT;
			break;
		case DRM_CAP_TIMESTAMP_MONOTONIC:
			r->value = 1;
			break;
		case DRM_CAP_CURSOR_WIDTH:
		case DRM_CAP_CURSOR_HEIGHT:
			r->value = 64;
			break;
		case DRM_CAP_ASYNC_PAGE_FLIP:
		case DRM_CAP_ADDFB2_MODIFIERS:
			break;
		default:
			return -EINVAL;
		}
		return 0;
	}
	case DRM_IOCTL_SET_CLIENT_CAP: {
		struct drm_set_client_cap *r = arg;
		if (r->capability == DRM_CLIENT_CAP_UNIVERSAL_PLANES &&
		    r->value <= 1)
			return 0;
		return -EOPNOTSUPP;
	}
	default:
		break;
	}
	if (!client->authenticated)
		return -EACCES;
	if (((cmd >> 8) & 255) != 'd')
		return -ENOTTY;
	if ((cmd & 255) >= 0xa0) {
		if (MINOR(client->rdev))
			return -EACCES;
		return gpu_kms(client, cmd, arg);
	}
	switch (cmd) {
	case DRM_IOCTL_GEM_CLOSE:
		return gpu_drop_handle(client,
				       ((struct drm_gem_close *)arg)->handle);
	case DRM_IOCTL_GEM_FLINK: {
		struct drm_gem_flink *r = arg;
		struct gpu_bo *bo = gpu_handle(client, r->handle);
		if (MINOR(client->rdev))
			return -EACCES;
		if (!bo)
			return -ENOENT;
		bo->named = 1;
		r->name = bo->id;
		return 0;
	}
	case DRM_IOCTL_GEM_OPEN: {
		struct drm_gem_open *r = arg;
		struct gpu_bo *bo;
		int result;
		if (MINOR(client->rdev))
			return -EACCES;
		if (!r->name)
			return -ENOENT;
		bo = NULL;
		for (i = 0; i < GPU_MAX_OBJECTS; i++) {
			if (gpu_objects[i] && gpu_objects[i]->id == r->name) {
				bo = gpu_objects[i];
				break;
			}
		}
		if (!bo || !bo->named)
			return -ENOENT;
		result = gpu_add_handle(client, bo->file);
		if (result < 0)
			return result;
		r->handle = result;
		r->size = bo->size;
		return 0;
	}
	case DRM_IOCTL_PRIME_HANDLE_TO_FD: {
		struct drm_prime_handle *r = arg;
		struct gpu_bo *bo = gpu_handle(client, r->handle);
		int fd;
		if (!bo)
			return -ENOENT;
		if (r->flags & ~(DRM_CLOEXEC | DRM_RDWR))
			return -EINVAL;
		fs_get_file(bo->file);
		/* fs_ioctl holds the descriptor-table lock. */
		fd = fs_install_fd_unsafe(
			bo->file, (r->flags & DRM_CLOEXEC) ? O_CLOEXEC : 0);
		if (fd < 0) {
			fs_put_file(bo->file);
			return fd;
		}
		r->fd = fd;
		return 0;
	}
	case DRM_IOCTL_PRIME_FD_TO_HANDLE: {
		struct drm_prime_handle *r = arg;
		file *fp;
		int result;
		if (r->flags || r->fd < 0 || r->fd >= MAX_FD)
			return -EINVAL;
		fp = current->fds[r->fd];
		if (!fp || fp->f_fop != &gpu_buffer_fops)
			return -EINVAL;
		result = gpu_add_handle(client, fp);
		if (result < 0)
			return result;
		r->handle = result;
		return 0;
	}
	case DRM_IOCTL_VIRTGPU_GETPARAM: {
		struct drm_virtgpu_getparam *r = arg;
		uint64_t value = 0;
		if (r->param == VIRTGPU_PARAM_3D_FEATURES ||
		    r->param == VIRTGPU_PARAM_CAPSET_QUERY_FIX)
			value = 1;
		return gpu_out(r->value, &value, sizeof(value));
	}
	case DRM_IOCTL_VIRTGPU_RESOURCE_CREATE:
		return gpu_create(client, arg, 0);
	case DRM_IOCTL_VIRTGPU_RESOURCE_INFO: {
		struct drm_virtgpu_resource_info *r = arg;
		struct gpu_bo *bo = gpu_handle(client, r->bo_handle);
		if (!bo)
			return -ENOENT;
		r->res_handle = bo->id;
		r->size = bo->size;
		r->blob_mem = 0;
		return 0;
	}
	case DRM_IOCTL_VIRTGPU_MAP: {
		struct drm_virtgpu_map *r = arg;
		if (!gpu_handle(client, r->handle))
			return -ENOENT;
		r->offset = (uint64_t)r->handle * PAGE_SIZE;
		return 0;
	}
	case DRM_IOCTL_VIRTGPU_GET_CAPS: {
		struct drm_virtgpu_get_caps *r = arg;
		struct virtio_gpu_get_capset req = { 0 };
		void *response;
		unsigned size;
		int result;
		if ((r->cap_set_id != VIRTIO_GPU_CAPSET_VIRGL &&
		     r->cap_set_id != VIRTIO_GPU_CAPSET_VIRGL2) ||
		    !r->size || r->size > 65536 ||
		    !gpu_user_range(r->addr, r->size, 1))
			return -EINVAL;
		req.hdr.type = VIRTIO_GPU_CMD_GET_CAPSET;
		req.capset_id = r->cap_set_id;
		req.capset_version = r->cap_set_ver;
		size = sizeof(struct virtio_gpu_ctrl_hdr) + r->size;
		response = gpu_pages(size);
		if (!response)
			return -ENOMEM;
		result = gpu_command(&req, sizeof(req), response, size, 0);
		if (!result)
			result = gpu_out(
				r->addr,
				(char *)response +
					sizeof(struct virtio_gpu_ctrl_hdr),
				r->size);
		gpu_free_pages(response, size);
		return result;
	}
	case DRM_IOCTL_VIRTGPU_EXECBUFFER: {
		struct drm_virtgpu_execbuffer *r = arg;
		struct virtio_gpu_cmd_submit *req;
		uint32_t *handles;
		unsigned size;
		int result;
		if (r->flags || r->num_in_syncobjs || r->num_out_syncobjs ||
		    (r->size & 3) || r->size > GPU_MAX_COMMAND - sizeof(*req) ||
		    r->num_bo_handles >= GPU_MAX_HANDLES ||
		    !gpu_user_range(r->command, r->size, 0) ||
		    !gpu_user_range(r->bo_handles, r->num_bo_handles * 4, 0))
			return -EINVAL;
		size = sizeof(*req) + r->size;
		/* Snapshot the handle list before commands can yield to another task. */
		handles = zalloc(r->num_bo_handles ? r->num_bo_handles * 4 : 1);
		if (!handles)
			return -ENOMEM;
		memcpy(handles, (void *)(uintptr_t)r->bo_handles,
		       r->num_bo_handles * 4);
		for (i = 0; i < r->num_bo_handles; i++) {
			if (!gpu_handle(client, handles[i])) {
				free(handles);
				return -ENOENT;
			}
		}
		req = gpu_pages(size);
		if (!req) {
			free(handles);
			return -ENOMEM;
		}
		req->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
		req->hdr.ctx_id = client->ctx;
		req->size = r->size;
		memcpy(req + 1, (void *)(uintptr_t)r->command, r->size);
		/* Shared resources must complete their producer's work before another
		 * VirGL context consumes them. Same-context submissions stay ordered
		 * on the host command stream without a CPU wait. */
		result = 0;
		for (i = 0; i < r->num_bo_handles; i++) {
			struct gpu_bo *bo = gpu_handle(client, handles[i]);
			if (bo->submission_ctx != client->ctx &&
			    bo->submission > gpu_completed) {
				result = gpu_idle(bo->submission_ctx);
				if (result)
					break;
			}
		}
		if (!result) {
			++gpu_submission;
			for (i = 0; i < r->num_bo_handles; i++) {
				struct gpu_bo *bo =
					gpu_handle(client, handles[i]);
				bo->submission = gpu_submission;
				bo->submission_ctx = client->ctx;
			}
			result = gpu_simple(req, size, 0);
		}
		gpu_free_pages(req, size);
		free(handles);
		return result;
	}
	case DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST:
	case DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST: {
		struct drm_virtgpu_3d_transfer_to_host *r = arg;
		struct gpu_bo *bo = gpu_handle(client, r->bo_handle);
		struct virtio_gpu_transfer_host_3d req = { 0 };
		if (!bo || r->offset >= bo->size)
			return -EINVAL;
		req.hdr.type = cmd == DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST ?
				       VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D :
				       VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D;
		req.hdr.ctx_id = client->ctx;
		memcpy(&req.box, &r->box, sizeof(req.box));
		req.offset = r->offset;
		req.resource_id = bo->id;
		req.level = r->level;
		req.stride = r->stride;
		req.layer_stride = r->layer_stride;
		return gpu_simple(&req, sizeof(req), 1);
	}
	case DRM_IOCTL_VIRTGPU_WAIT: {
		struct drm_virtgpu_3d_wait *r = arg;
		struct gpu_bo *bo = gpu_handle(client, r->handle);
		int result;
		if (!bo || (r->flags & ~VIRTGPU_WAIT_NOWAIT))
			return -EINVAL;
		result = gpu_complete(!(r->flags & VIRTGPU_WAIT_NOWAIT));
		if (result)
			return result;
		if (bo->submission <= gpu_completed)
			return 0;
		return gpu_idle_mode(client->ctx,
				     (r->flags & VIRTGPU_WAIT_NOWAIT) ? 2 : 1);
	}
	default:
		return -ENOTTY;
	}
}

static int gpu_ioctl(file *fp, unsigned cmd, void *arg)
{
	unsigned size = (cmd >> 16) & 0x3fff;
	unsigned direction = cmd >> 30;
	void *local;
	int result;
	if (size > 256 || ((cmd >> 8) & 255) != 'd')
		return -ENOTTY;
	if ((direction & 1) && !gpu_user_range((uintptr_t)arg, size, 0))
		return -EFAULT;
	if ((direction & 2) && !gpu_user_range((uintptr_t)arg, size, 1))
		return -EFAULT;
	local = zalloc(size ? size : 1);
	if (!local)
		return -ENOMEM;
	if (direction & 1)
		memcpy(local, arg, size);
	rmutex_lock(&gpu_lock);
	result = gpu_ready ?
			 gpu_ioctl_locked(fp->f_inode->i_private, cmd, local) :
			 -ENODEV;
	rmutex_unlock(&gpu_lock);
	if (!result && (direction & 2))
		memcpy(arg, local, size);
	free(local);
	return result;
}

static int gpu_stat(file *fp, struct stat *st)
{
	struct gpu_client *client = fp->f_inode->i_private;
	memset(st, 0, sizeof(*st));
	st->st_mode = S_IFCHR | 0666;
	st->st_rdev = client->rdev;
	st->st_ino = client->rdev;
	st->st_blksize = PAGE_SIZE;
	st->st_nlink = 1;
	return 0;
}

static unsigned gpu_poll(file *fp, unsigned events, poll_table *pt)
{
	(void)fp;
	(void)pt;
	return gpu_ready ? (events & FS_POLL_WRITE) : FS_POLL_ERR;
}

static int gpu_release(file *fp)
{
	struct gpu_client *client = fp->f_inode->i_private;
	struct virtio_gpu_ctx_destroy req = { 0 };
	unsigned i;
	if (!client->ctx) {
		free(client);
		free(fp->f_inode);
		free(fp);
		return 0;
	}
	rmutex_lock(&gpu_lock);
	if (gpu_master == client) {
		gpu_set_scanout(NULL);
		gpu_master = NULL;
	}
	gpu_idle(client->ctx);
	for (i = 0; i < GPU_MAX_FB; i++)
		if (gpu_fbs[i].buffer && gpu_fbs[i].owner == client->ctx) {
			if (gpu_scanout_fb == gpu_fbs[i].id)
				gpu_set_scanout(NULL);
			fs_put_file(gpu_fbs[i].buffer);
			memset(&gpu_fbs[i], 0, sizeof(gpu_fbs[i]));
		}
	for (i = 1; i < GPU_MAX_HANDLES; i++)
		if (client->handles[i])
			gpu_drop_handle(client, i);
	req.hdr.type = VIRTIO_GPU_CMD_CTX_DESTROY;
	req.hdr.ctx_id = client->ctx;
	gpu_simple(&req, sizeof(req), 1);
	for (i = 0; i < GPU_MAX_CLIENTS; i++)
		if (gpu_clients[i] == client)
			gpu_clients[i] = NULL;
	free(client);
	free(fp->f_inode);
	free(fp);
	rmutex_unlock(&gpu_lock);
	return 0;
}

static const file_operations gpu_fops = {
	.getattr = gpu_stat,
	.ioctl = gpu_ioctl,
	.poll = gpu_poll,
	.release = gpu_release,
	.mmap_file = gpu_drm_map,
};

static file *gpu_open(super_block *sb, unsigned rdev, int flags)
{
	struct virtio_gpu_ctx_create req = { 0 };
	struct gpu_client *client = NULL;
	file *fp = NULL;
	unsigned slot;
	(void)sb;
	rmutex_lock(&gpu_lock);
	for (slot = 0; slot < GPU_MAX_CLIENTS && gpu_clients[slot]; slot++) {
	}
	if (!gpu_ready || slot == GPU_MAX_CLIENTS || gpu_next_context == 0)
		goto fail;
	client = zalloc(sizeof(*client));
	fp = zalloc(sizeof(*fp));
	if (!client || !fp)
		goto fail;
	fp->f_inode = zalloc(sizeof(*fp->f_inode));
	if (!fp->f_inode)
		goto fail;
	client->ctx = (flags & O_PATH) ? 0 : gpu_next_context++;
	client->rdev = rdev;
	client->authenticated = MINOR(rdev) == GPU_RENDER_MINOR ||
				current->user->euid == 0;
	fp->f_inode->i_mode = S_IFCHR | 0666;
	fp->f_inode->i_private = client;
	fp->f_fop = &gpu_fops;
	fp->f_count = 1;
	fp->f_mode = flags & O_ACCMODE;
	fp->f_flag = flags;
	if (flags & O_PATH) {
		rmutex_unlock(&gpu_lock);
		return fp;
	}
	req.hdr.type = VIRTIO_GPU_CMD_CTX_CREATE;
	req.hdr.ctx_id = client->ctx;
	req.nlen = 3;
	memcpy(req.debug_name, "MOS", 3);
	if (gpu_simple(&req, sizeof(req), 0))
		goto fail;
	gpu_clients[slot] = client;
	if (!MINOR(rdev) && !gpu_master && !current->user->euid)
		gpu_master = client;
	rmutex_unlock(&gpu_lock);
	return fp;
fail:
	if (fp)
		free(fp->f_inode);
	free(fp);
	free(client);
	rmutex_unlock(&gpu_lock);
	return NULL;
}

static void *gpu_pci_map(const pci_resource *resources, unsigned bar,
			 unsigned offset, unsigned length)
{
	uint64_t start, end, page;
	if (bar >= 6 || (resources[bar].flags & 1) || !length ||
	    (uint64_t)offset + length > resources[bar].size)
		return NULL;
	start = resources[bar].start + offset;
	end = start + length;
	if (start < KERNEL_IO_BEGIN || end > KERNEL_IO_END || end <= start)
		return NULL;
	for (page = start & PAGE_SIZE_MASK; page < end; page += PAGE_SIZE) {
		if (mm_map_io(page) != 1)
			return NULL;
		mm_set_map_flag(page, PAGE_ENTRY_KERNEL_DATA | PAGE_ENTRY_CD |
					      PAGE_ENTRY_WT);
		arch_mm_invalidate(page);
	}
	return (void *)(uintptr_t)start;
}

static void gpu_find(unsigned pci, uint16_t vendor, uint16_t device, void *arg)
{
	(void)arg;
	if (gpu_pci == ~0U && vendor == 0x1af4 && device == 0x1050)
		gpu_pci = pci;
}

static int gpu_dir_stat(file *fp, struct stat *st)
{
	memset(st, 0, sizeof(*st));
	st->st_mode = S_IFDIR | 0555;
	st->st_ino = GPU_MAJOR;
	st->st_size = fp->f_inode->i_size;
	st->st_blksize = PAGE_SIZE;
	st->st_nlink = 2;
	return 0;
}

static ssize_t gpu_dir_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	unsigned length = fp->f_inode->i_size;
	if (*pos < 0)
		return -EINVAL;
	if ((uint64_t)*pos >= length)
		return 0;
	if (size > length - *pos)
		size = length - *pos;
	memcpy(buf, (char *)fp->f_inode->i_private + *pos, size);
	*pos += size;
	return size;
}

static int gpu_dir_release(file *fp)
{
	free(fp->f_inode->i_private);
	free(fp->f_inode);
	free(fp);
	return 0;
}

static const file_operations gpu_dir_fops = {
	.read = gpu_dir_read,
	.getattr = gpu_dir_stat,
	.release = gpu_dir_release,
};

static file *gpu_dir_open(super_block *sb, int flags)
{
	file *fp = zalloc(sizeof(*fp));
	char *p, *begin;
	struct linux_dirent *dirp;
	(void)sb;
	(void)flags;
	if (!fp)
		return NULL;
	fp->f_inode = zalloc(sizeof(*fp->f_inode));
	if (!fp->f_inode) {
		free(fp);
		return NULL;
	}
	begin = p = zalloc(256);
	if (!p) {
		free(fp->f_inode);
		free(fp);
		return NULL;
	}
	FILL_ENTRY(".", GPU_MAJOR);
	FILL_ENTRY("..", DEV_INODE);
	FILL_ENTRY("card0", MKDEV(GPU_MAJOR, 0));
	FILL_ENTRY("renderD128", MKDEV(GPU_MAJOR, GPU_RENDER_MINOR));
	fp->f_inode->i_private = begin;
	fp->f_inode->i_size = p - begin;
	fp->f_inode->i_mode = S_IFDIR | 0555;
	fp->f_fop = &gpu_dir_fops;
	fp->f_count = 1;
	return fp;
}

static const super_operations gpu_dir_sops = { .open_root = gpu_dir_open };

static int gpu_sysfs(sysfs_tree *tree, void *data)
{
	static const struct {
		const char *name;
		unsigned minor;
	} nodes[] = {
		{ "card0", 0 },
		{ "renderD128", GPU_RENDER_MINOR },
	};
	sysfs_node *device, *devices, *class, *characters;
	char *text;
	unsigned i;
	(void)data;
	if (!gpu_ready)
		return 0;
	device = sysfs_pci_device(tree, gpu_pci);
	if (!device)
		return -ENODEV;
	devices = sysfs_directory(device, "drm");
	class = sysfs_directory(sysfs_directory(sysfs_root(tree), "class"),
				"drm");
	characters = sysfs_directory(sysfs_directory(sysfs_root(tree), "dev"),
				     "char");
	if (!devices || !class || !characters)
		return -ENOMEM;
	text = name_get();
	if (!text)
		return -ENOMEM;
	for (i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
		sysfs_node *node = sysfs_directory(devices, nodes[i].name);
		if (!node || !sysfs_link(class, nodes[i].name, node) ||
		    !sysfs_link(node, "device", device) ||
		    !sysfs_link(node, "subsystem", class))
			goto fail;
		sprintf(text, "%u:%u", GPU_MAJOR, nodes[i].minor);
		if (!sysfs_link(characters, text, node))
			goto fail;
		sprintf(text, "%u:%u\n", GPU_MAJOR, nodes[i].minor);
		if (!sysfs_text(node, "dev", text))
			goto fail;
		sprintf(text, "MAJOR=%u\nMINOR=%u\nDEVNAME=dri/%s\n", GPU_MAJOR,
			nodes[i].minor, nodes[i].name);
		if (!sysfs_text(node, "uevent", text))
			goto fail;
	}
	name_put(text);
	return 0;
fail:
	name_put(text);
	return -ENOMEM;
}

static void gpu_register(super_block *sb)
{
	unsigned cap, seen = 0, bar, offset, length, type,
		      notify_multiplier = 0;
	volatile uint8_t *notify_base = NULL;
	unsigned notify_length = 0;
	unsigned low_features, high_features;
	pci_resource *resources = NULL;
	void *desc = NULL, *avail = NULL, *used = NULL;
	rmutex_init(&gpu_lock);
	pci_scan(gpu_find, PCI_SCAN_ALL, NULL);
	if (gpu_pci == ~0U)
		return;
	resources = zalloc(7 * sizeof(*resources));
	if (!resources)
		goto fail;
	pci_get_resources(gpu_pci, resources);
	pci_write_field(gpu_pci, PCI_COMMAND, 2,
			pci_read_field(gpu_pci, PCI_COMMAND, 2) | 6 |
				(1U << 10));
	cap = pci_read_field(gpu_pci, 0x34, 1) & ~3U;
	while (cap >= 0x40 && cap <= 0xfc && seen++ < 48) {
		if (pci_read_field(gpu_pci, cap, 1) == 9 &&
		    pci_read_field(gpu_pci, cap + 2, 1) >= 16) {
			type = pci_read_field(gpu_pci, cap + 3, 1);
			bar = pci_read_field(gpu_pci, cap + 4, 1);
			offset = pci_read_field(gpu_pci, cap + 8, 4);
			length = pci_read_field(gpu_pci, cap + 12, 4);
			if (type == 1 && length >= sizeof(*gpu_common))
				gpu_common = gpu_pci_map(resources, bar, offset,
							 sizeof(*gpu_common));
			if (type == 2 &&
			    pci_read_field(gpu_pci, cap + 2, 1) >= 20) {
				notify_base = gpu_pci_map(resources, bar,
							  offset, length);
				notify_multiplier =
					pci_read_field(gpu_pci, cap + 16, 4);
				notify_length = length;
			}
		}
		cap = pci_read_field(gpu_pci, cap + 1, 1) & ~3U;
	}
	if (!gpu_common || !notify_base)
		goto fail;
	gpu_common->device_status = 0;
	while (gpu_common->device_status)
		PAUSE();
	gpu_common->device_status = 1 | 2;
	gpu_common->device_feature_select = 0;
	low_features = gpu_common->device_feature;
	gpu_common->device_feature_select = 1;
	high_features = gpu_common->device_feature;
	if (!(low_features & (1U << VIRTIO_GPU_F_VIRGL)) ||
	    !(high_features & 1))
		goto fail;
	gpu_common->driver_feature_select = 0;
	gpu_common->driver_feature = 1U << VIRTIO_GPU_F_VIRGL;
	gpu_common->driver_feature_select = 1;
	gpu_common->driver_feature = 1;
	gpu_common->device_status = 1 | 2 | 8;
	if (!(gpu_common->device_status & 8))
		goto fail;
	gpu_common->msix_config = 0xffff;
	gpu_common->queue_select = 0;
	if (gpu_common->queue_size < GPU_QUEUE_SIZE)
		goto fail;
	gpu_common->queue_size = GPU_QUEUE_SIZE;
	gpu_common->queue_msix_vector = 0xffff;
	if ((uint64_t)gpu_common->queue_notify_off * notify_multiplier + 2 >
	    notify_length)
		goto fail;
	gpu_notify = (void *)(notify_base +
			      gpu_common->queue_notify_off * notify_multiplier);
	desc = gpu_pages(PAGE_SIZE);
	avail = gpu_pages(PAGE_SIZE);
	used = gpu_pages(PAGE_SIZE);
	gpu_dma_input = gpu_pages(GPU_MAX_COMMAND);
	gpu_dma_output = gpu_pages(GPU_MAX_COMMAND);
	if (!desc || !avail || !used || !gpu_dma_input || !gpu_dma_output)
		goto fail;
	gpu_desc = desc;
	gpu_avail = avail;
	gpu_used = used;
	gpu_avail->flags = 1;
	gpu_common->queue_desc = VIRT_TO_PHY(desc);
	gpu_common->queue_driver = VIRT_TO_PHY(avail);
	gpu_common->queue_device = VIRT_TO_PHY(used);
	__sync_synchronize();
	gpu_common->queue_enable = 1;
	gpu_common->device_status = 1 | 2 | 8 | 4;
	gpu_ready = 1;
	if (sysfs_register_provider(gpu_sysfs, NULL))
		goto fail;
	cdev_register_named(S_IFCHR, GPU_MAJOR, 0, 1, "drm", gpu_open);
	cdev_register_named(S_IFCHR, GPU_MAJOR, GPU_RENDER_MINOR, 1, "drm",
			    gpu_open);
	{
		super_block *directory = sget(&gpu_dir_sops);
		vfs_mount(sb, "/dri", directory);
		vfs_mknod(directory, "/card0", S_IFCHR | 0666,
			  MKDEV(GPU_MAJOR, 0));
		vfs_mknod(directory, "/renderD128", S_IFCHR | 0666,
			  MKDEV(GPU_MAJOR, GPU_RENDER_MINOR));
	}
	free(resources);
	printk("virtio_gpu: VirGL enabled; /dev/dri/card0 and renderD128\n");
	return;
fail:
	if (gpu_common) {
		gpu_common->device_status = 0;
		while (gpu_common->device_status)
			PAUSE();
	}
	free(resources);
	gpu_free_pages(gpu_dma_input, GPU_MAX_COMMAND);
	gpu_free_pages(gpu_dma_output, GPU_MAX_COMMAND);
	gpu_dma_input = gpu_dma_output = NULL;
	gpu_free_pages(desc, PAGE_SIZE);
	gpu_free_pages(avail, PAGE_SIZE);
	gpu_free_pages(used, PAGE_SIZE);
	gpu_ready = 0;
	printk("virtio_gpu: hardware 3D initialization failed\n");
}

DEV_INIT(gpu_register);
