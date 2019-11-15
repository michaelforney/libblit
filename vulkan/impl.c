#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <pixman.h>
#include <vulkan/vulkan.h>
#include <blt.h>
#include "../priv.h"
#include "priv.h"

struct draw_context {
	VkCommandBuffer cmd;
	VkSemaphore semaphore;
	VkFramebuffer fb;
	VkFence fence;
	VkDeviceMemory vertex_memory;
	VkBuffer vertex_buffer;
	int32_t *vertex;
	size_t vertex_len, vertex_pos, vertex_cap;
};

struct image {
	struct blt_image base;
	VkImage vk;
	VkDeviceMemory memory;
	VkImageView view;
	VkImageLayout layout;
	struct draw_context *draw_ctx;
};

struct surface {
	struct blt_surface base;
	VkSurfaceKHR vk;
	VkSwapchainKHR swapchain;
	struct image *img;
	int *age;
	uint32_t img_len;
};

static const uint32_t vert_spv[] = {
#include "vert.vert.inc"
};

static const uint32_t fill_spv[] = {
#include "fill.frag.inc"
};

static const uint32_t copy_spv[] = {
#include "copy.frag.inc"
};

static void
image_destroy(struct blt_context *ctx_base, struct blt_image *img_base)
{
	struct image *img = (void *)img_base;

	free(img);
}

static const struct blt_image_impl image_impl = {
	.destroy = image_destroy,
};

static void
surface_destroy(struct blt_context *ctx_base, struct blt_surface *srf_base)
{
	struct surface *srf = (void *)srf_base;

	free(srf->img);
	free(srf->age);
	free(srf);
}

static struct blt_image *
acquire(struct blt_context *ctx_base, struct blt_surface *srf_base, int *age)
{
	struct context *ctx = (void *)ctx_base;
	struct surface *srf = (void *)srf_base;
	uint32_t idx;
	VkResult res;

	/* XXX: use semaphore */
	res = vkAcquireNextImageKHR(ctx->dev, srf->swapchain, -1, VK_NULL_HANDLE, VK_NULL_HANDLE, &idx);
	if (res != VK_SUCCESS)
		return NULL;
	if (age)
		*age = srf->age[idx];
	return &srf->img[idx].base;
}

static int
present(struct blt_context *ctx_base, struct blt_surface *srf_base, struct blt_image *img_base)
{
	struct context *ctx = (void *)ctx_base;
	struct surface *srf = (void *)srf_base;
	struct image *img = (void *)img_base;
	VkResult res;
	uint32_t i, idx;

	idx = img - srf->img;
	res = vkQueuePresentKHR(ctx->queue, &(VkPresentInfoKHR){
		.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		.swapchainCount = 1,
		.pSwapchains = &srf->swapchain,
		.pImageIndices = (uint32_t[]){idx},
	});
	if (res != VK_SUCCESS)
		return -1;
	for (i = 0; i < srf->img_len; ++i)
		++srf->age[i];
	srf->age[idx] = 0;
	return 0;
}

static const struct blt_surface_impl surface_impl = {
	.destroy = surface_destroy,
	.acquire = acquire,
	.present = present,
};

static uint32_t
find_memory_type(struct context *ctx, uint32_t bits, VkMemoryPropertyFlags flags)
{
	VkPhysicalDeviceMemoryProperties props;
	uint32_t i;

	vkGetPhysicalDeviceMemoryProperties(ctx->phys, &props);
	for (i = 0; i < props.memoryTypeCount; ++i) {
		if (bits & (1 << i) && (props.memoryTypes[i].propertyFlags & flags) == flags)
			return i;
	}
	return -1;
}

static int
alloc_buffer(struct context *ctx, size_t size, VkBufferUsageFlags usage, VkBuffer *buf, VkDeviceMemory *mem)
{
	VkResult res;
	VkMemoryRequirements reqs;
	VkMemoryPropertyFlags props = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	uint32_t mem_type;

	res = vkCreateBuffer(ctx->dev, &(VkBufferCreateInfo){
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	}, NULL, buf);
	if (res != VK_SUCCESS)
		goto error0;
	vkGetBufferMemoryRequirements(ctx->dev, *buf, &reqs);
	mem_type = find_memory_type(ctx, reqs.memoryTypeBits, props);
	if (mem_type == -1)
		goto error1;
	res = vkAllocateMemory(ctx->dev, &(VkMemoryAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = reqs.size,
		.memoryTypeIndex = mem_type,
	}, NULL, mem);
	if (res != VK_SUCCESS)
		goto error1;
	res = vkBindBufferMemory(ctx->dev, *buf, *mem, 0);
	if (res != VK_SUCCESS)
		goto error2;
	return VK_SUCCESS;

error2:
	vkFreeMemory(ctx->dev, *mem, NULL);
error1:
	vkDestroyBuffer(ctx->dev, *buf, NULL);
error0:
	return -1;
}

static struct draw_context *
make_draw_context(struct context *ctx, struct image *img)
{
	struct draw_context *dc;
	VkResult res;
	void *data;

	dc = malloc(sizeof(*dc));
	if (!dc)
		goto error0;
	res = vkCreateFramebuffer(ctx->dev, &(VkFramebufferCreateInfo){
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = ctx->render_pass,
		.attachmentCount = 1,
		.pAttachments = (VkImageView[]){img->view},
		.width = img->base.width,
		.height = img->base.height,
		.layers = 1,
	}, NULL, &dc->fb);
	if (res != VK_SUCCESS)
		goto error1;
	dc->vertex_len = 0;
	dc->vertex_pos = 0;
	dc->vertex_cap = 0x400;
	res = vkAllocateCommandBuffers(ctx->dev, &(VkCommandBufferAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = ctx->cmd_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	}, &dc->cmd);
	if (res != VK_SUCCESS)
		goto error2;
	res = alloc_buffer(ctx, dc->vertex_cap * sizeof(dc->vertex[0]), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &dc->vertex_buffer, &dc->vertex_memory);
	if (res != VK_SUCCESS)
		goto error3;
	res = vkMapMemory(ctx->dev, dc->vertex_memory, 0, VK_WHOLE_SIZE, 0, &data);
	if (res != VK_SUCCESS)
		goto error4;
	dc->vertex = data;
	res = vkCreateSemaphore(ctx->dev, &(VkSemaphoreCreateInfo){
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
	}, NULL, &dc->semaphore);
	if (res != VK_SUCCESS)
		goto error5;
	return dc;

error5:
	vkUnmapMemory(ctx->dev, dc->vertex_memory);
error4:
	vkDestroyBuffer(ctx->dev, dc->vertex_buffer, NULL);
	vkFreeMemory(ctx->dev, dc->vertex_memory, NULL);
error3:
	vkFreeCommandBuffers(ctx->dev, ctx->cmd_pool, 1, (VkCommandBuffer[]){dc->cmd});
error2:
	vkDestroyFramebuffer(ctx->dev, dc->fb, NULL);
error1:
	free(dc);
error0:
	return NULL;
}

static int
init_image(struct context *ctx, struct image *img, VkFormat format, int flags)
{
	VkResult res;

	if (flags & (BLT_IMAGE_SRC|BLT_IMAGE_DST)) {
		res = vkCreateImageView(ctx->dev, &(VkImageViewCreateInfo){
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = img->vk,
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = format,
			.subresourceRange = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.levelCount = 1,
				.layerCount = 1,
			},
		}, NULL, &img->view);
		if (res != VK_SUCCESS)
			goto error0;
	} else {
		img->view = VK_NULL_HANDLE;
	}
	if (flags & BLT_IMAGE_DST) {
		img->draw_ctx = make_draw_context(ctx, img);
		if (!img->draw_ctx)
			goto error1;
	} else {
		img->draw_ctx = NULL;
	}
	return 0;

error1:
	if (img->view)
		vkDestroyImageView(ctx->dev, img->view, NULL);
error0:
	return -1;
}

static VkFormat
vulkan_format(uint32_t format)
{
	switch (format) {
	case BLT_FMT('X', 'R', '2', '4'):
	case BLT_FMT('A', 'R', '2', '4'):
		return VK_FORMAT_B8G8R8A8_UNORM;
	default:
		return VK_FORMAT_UNDEFINED;
	}
}

static struct blt_image *
new_image(struct blt_context *ctx_base, int width, int height, uint32_t format, int flags)
{
	struct context *ctx = (void *)ctx;
	struct image *img;
	VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.extent = {width, height, 1},
		.mipLevels = 1,
		.arrayLayers = 1,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.queueFamilyIndexCount = 1,
		.pQueueFamilyIndices = &ctx->queue_index,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	VkMemoryPropertyFlags memory_flags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	VkMemoryRequirements reqs;
	VkResult res;

	info.format = vulkan_format(format);
	if (info.format == VK_FORMAT_UNDEFINED)
		return NULL;

	if (flags & BLT_IMAGE_DST)
		info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	if (flags & BLT_IMAGE_SRC)
		info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;

	img = malloc(sizeof(*img));
	if (!img)
		goto error0;
	img->base = (struct blt_image){
		.impl = &image_impl,
		.width = width,
		.height = height,
		.format = format,
	};
	res = vkCreateImage(ctx->dev, &info, NULL, &img->vk);
	if (res != VK_SUCCESS)
		goto error1;
	vkGetImageMemoryRequirements(ctx->dev, img->vk, &reqs);
	res = vkAllocateMemory(ctx->dev, &(VkMemoryAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.pNext = &(VkMemoryDedicatedAllocateInfo){
			.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
			.image = img->vk,
		},
		.allocationSize = reqs.size,
		.memoryTypeIndex = find_memory_type(ctx, reqs.memoryTypeBits, memory_flags),
	}, NULL, &img->memory);
	if (res != VK_SUCCESS)
		goto error2;
	res = vkBindImageMemory(ctx->dev, img->vk, img->memory, 0);
	if (res != VK_SUCCESS)
		goto error3;
	if (init_image(ctx, img, info.format, flags) < 0)
		goto error3;

	return &img->base;

error3:
	vkFreeMemory(ctx->dev, img->memory, NULL);
error2:
	vkDestroyImage(ctx->dev, img->vk, NULL);
error1:
	free(img);
error0:
	return NULL;
}

struct blt_surface *
blt_vulkan_new_surface(struct context *ctx, VkSurfaceKHR vk, int width, int height, uint32_t format)
{
	struct surface *srf;
	VkSurfaceCapabilitiesKHR caps;
	VkImage *vkimg;
	VkResult res;
	VkSurfaceFormatKHR *formats;
	uint32_t formats_len;
	VkSwapchainCreateInfoKHR info = {
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
		.surface = vk,
		.imageArrayLayers = 1,
		.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
		.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
		.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
		.presentMode = VK_PRESENT_MODE_FIFO_KHR,
		.clipped = VK_TRUE,
	};
	int i;
	VkBool32 supported;

	res = vkGetPhysicalDeviceSurfaceSupportKHR(ctx->phys, ctx->queue_index, vk, &supported);
	if (res != VK_SUCCESS || !supported)
		goto error0;
	info.imageFormat = vulkan_format(format);
	if (info.imageFormat == VK_FORMAT_UNDEFINED)
		goto error0;
	res = vkGetPhysicalDeviceSurfaceFormatsKHR(ctx->phys, vk, &formats_len, NULL);
	if (res != VK_SUCCESS)
		goto error0;
	formats = blt_reallocarray(NULL, formats_len, sizeof(formats[0]));
	if (!formats)
		goto error0;
	res = vkGetPhysicalDeviceSurfaceFormatsKHR(ctx->phys, vk, &formats_len, formats);
	if (res != VK_SUCCESS) {
		free(formats);
		goto error0;
	}
	for (i = 0; i < formats_len; ++i) {
		if (formats[i].format == info.imageFormat) {
			info.imageColorSpace = formats[i].colorSpace;
			break;
		}
	}
	free(formats);
	if (i == formats_len)
		goto error0;
	/* XXX: check for surface formats */
	srf = malloc(sizeof(*srf));
	if (!srf)
		goto error0;
	srf->base = (struct blt_surface){.impl = &surface_impl};
	srf->vk = vk;
	res = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx->phys, vk, &caps);
	if (res != VK_SUCCESS)
		goto error1;
	if (width >= 0) {
		if (caps.currentExtent.width != 0xffffffff)
			goto error1;
		caps.currentExtent.width = width;
	}
	if (height >= 0) {
		if (caps.currentExtent.height != 0xffffffff)
			goto error1;
		caps.currentExtent.height = height;
	}
	info.imageExtent = caps.currentExtent;
	info.minImageCount = caps.minImageCount;
	res = vkCreateSwapchainKHR(ctx->dev, &info, NULL, &srf->swapchain);
	if (res != VK_SUCCESS)
		goto error1;
	res = vkGetSwapchainImagesKHR(ctx->dev, srf->swapchain, &srf->img_len, NULL);
	if (res != VK_SUCCESS)
		goto error2;
	srf->img = blt_reallocarray(NULL, srf->img_len, sizeof(srf->img[0]));
	if (!srf->img)
		goto error2;
	srf->age = blt_reallocarray(NULL, srf->img_len, sizeof(srf->age[0]));
	if (!srf->age)
		goto error3;
	vkimg = blt_reallocarray(NULL, srf->img_len, sizeof(vkimg[0]));
	if (!vkimg)
		goto error4;
	res = vkGetSwapchainImagesKHR(ctx->dev, srf->swapchain, &srf->img_len, vkimg);
	if (res != VK_SUCCESS)
		goto error5;
	for (i = 0; i < srf->img_len; ++i) {
		srf->age[i] = INT_MAX;
		srf->img[i].base = (struct blt_image){
			.impl = &image_impl,
			.width = caps.currentExtent.width,
			.height = caps.currentExtent.height,
			.format = BLT_FMT('X', 'R', '2', '4'),
		};
		srf->img[i].vk = vkimg[i];
		if (init_image(ctx, &srf->img[i], VK_FORMAT_B8G8R8A8_UNORM, BLT_IMAGE_DST) < 0)
			goto error6;
	}
	free(vkimg);
	return &srf->base;

error6:
	/* XXX: destroy images */
error5:
	free(vkimg);
error4:
	free(srf->age);
error3:
	free(srf->img);
error2:
	vkDestroySwapchainKHR(ctx->dev, srf->swapchain, NULL);
error1:
	free(srf);
error0:
	return NULL;
}

static void
flush(struct context *ctx)
{
	struct image *dst = (void *)ctx->base.dst;
	struct draw_context *dc = dst->draw_ctx;

	if (dc->vertex_pos == dc->vertex_len)
		return;
	/*
	All pipeline layouts we use are compatible for push
	constants, so we can just choose an arbitrary one here.
	*/
	vkCmdPushConstants(dc->cmd, ctx->fill_pipeline.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 24, (float[]){
		ctx->base.dst_x,
		ctx->base.dst_y,
		ctx->base.src_x,
		ctx->base.src_y,
		2./ctx->base.dst->width,
		2./ctx->base.dst->height,
	});
	vkCmdDraw(dc->cmd, (dc->vertex_len - dc->vertex_pos) / 2, 1, dc->vertex_pos / 2, 0);
	dc->vertex_pos = dc->vertex_len;
}

static int
submit(struct context *ctx)
{
	struct image *dst = (void *)ctx->base.dst;
	struct draw_context *dc = dst->draw_ctx;
	VkSubmitInfo info = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.pWaitDstStageMask = (VkPipelineStageFlags[]){VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT},
		.commandBufferCount = 1,
		.pCommandBuffers = (VkCommandBuffer[]){dc->cmd},
		.signalSemaphoreCount = 1,
		.pSignalSemaphores = (VkSemaphore[]){dc->semaphore},
	};
	VkResult res;

	flush(ctx);
	vkCmdEndRenderPass(dc->cmd);
	res = vkEndCommandBuffer(dc->cmd);
	if (res != VK_SUCCESS)
		return -1;
	res = vkQueueSubmit(ctx->queue, 1, &info, VK_NULL_HANDLE);
	if (res != VK_SUCCESS)
		return -1;
	dc->vertex_len = 0;
	dc->vertex_pos = 0;
	return 0;
}

static int
setup(struct blt_context *ctx_base, int op, struct blt_image *dst_base, struct blt_image *src_base, struct blt_image *mask)
{
	struct context *ctx = (void *)ctx_base;
	struct image *dst;
	struct draw_context *dc;
	struct pipeline *pipeline = NULL;
	VkResult res;

	if (ctx->base.dst && dst_base != ctx->base.dst)
		submit(ctx);
	if (!dst_base)
		return 0;
	if (dst_base->impl != &image_impl)
		return -1;
	if (mask)
		return -1;
	dst = (void *)dst_base;
	dc = dst->draw_ctx;
	if (!dc)
		return -1;

	if (&dst->base != ctx->base.dst) {
		ctx->base.src = NULL;

		res = vkBeginCommandBuffer(dc->cmd, &(VkCommandBufferBeginInfo){
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		});
		if (res != VK_SUCCESS)
			return -1;
		vkCmdBeginRenderPass(dc->cmd, &(VkRenderPassBeginInfo){
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = ctx->render_pass,
			.framebuffer = dc->fb,
			.renderArea.extent = {dst->base.width, dst->base.height},
		}, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindVertexBuffers(dc->cmd, 0, 1, (VkBuffer[]){dc->vertex_buffer}, (VkDeviceSize[]){0});
		vkCmdSetViewport(dc->cmd, 0, 1, &(VkViewport){
			.width = dst->base.width,
			.height = dst->base.height,
		});
		vkCmdSetScissor(dc->cmd, 0, 1, &(VkRect2D){
			.extent = {dst->base.width, dst->base.height},
		});
	}
	if (src_base != ctx->base.src) {
		if (ctx->base.dst)
			flush(ctx);
		if (src_base->impl == &image_impl) {
			struct image *src = (void *)src_base;

			switch (src_base->format) {
			case BLT_FMT('X', 'R', '2', '4'):
			case BLT_FMT('A', 'R', '2', '4'):
				pipeline = &ctx->copy_rgb_pipeline;
				break;
			default:
				return -1;
			}
			vkCmdBindPipeline(dc->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->vk);
			vkCmdBindDescriptorSets(dc->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->layout, 0, 1, &pipeline->desc, 0, NULL);
			vkUpdateDescriptorSets(ctx->dev, 1, (VkWriteDescriptorSet[]){
				{
					.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
					.dstSet = pipeline->desc,
					.dstBinding = 0,
					.descriptorCount = 1,
					.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
					.pImageInfo = &(VkDescriptorImageInfo){
						.imageView = src->view,
						.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
					},
				},
			}, 0, NULL);
		} else if (src_base->impl == &blt_solid_image_impl) {
			struct blt_solid *src = (void *)src_base;

			pipeline = &ctx->fill_pipeline;
			vkCmdBindPipeline(dc->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->vk);
			vkCmdPushConstants(dc->cmd, pipeline->layout, VK_SHADER_STAGE_FRAGMENT_BIT, 32, 16, (float[]){
				(float)src->color.red / UINT16_MAX,
				(float)src->color.green / UINT16_MAX,
				(float)src->color.blue / UINT16_MAX,
				(float)src->color.alpha / UINT16_MAX,
			});
		} else {
			return -1;
		}
	}
	return 0;
}

static int
rect(struct blt_context *ctx_base, size_t len, const struct pixman_box32 *rect)
{
	struct context *ctx = (void *)ctx_base;
	struct image *img = (void *)ctx->base.dst;
	struct draw_context *dc = img->draw_ctx;

	if (dc->vertex_cap - dc->vertex_len < 12)
		flush(ctx);
	dc->vertex[dc->vertex_len++] = rect->x1;
	dc->vertex[dc->vertex_len++] = rect->y1;
	dc->vertex[dc->vertex_len++] = rect->x2;
	dc->vertex[dc->vertex_len++] = rect->y1;
	dc->vertex[dc->vertex_len++] = rect->x2;
	dc->vertex[dc->vertex_len++] = rect->y2;
	dc->vertex[dc->vertex_len++] = rect->x2;
	dc->vertex[dc->vertex_len++] = rect->y2;
	dc->vertex[dc->vertex_len++] = rect->x1;
	dc->vertex[dc->vertex_len++] = rect->y2;
	dc->vertex[dc->vertex_len++] = rect->x1;
	dc->vertex[dc->vertex_len++] = rect->y1;
	return 0;
}

static const struct blt_context_impl impl = {
	.new_image = new_image,
	.new_solid = blt_new_solid_image,
	.setup = setup,
	.rect = rect,
};

static bool
has_extension(VkExtensionProperties *prop, uint32_t prop_len, const char *name)
{
	for (; prop_len; ++prop, --prop_len) {
		if (strcmp(prop->extensionName, name) == 0)
			return true;
	}
	return false;
}

static int
make_pipeline(struct context *ctx)
{
	VkResult res;
	VkGraphicsPipelineCreateInfo info[2];
	VkPipeline pipeline[2];
	VkDescriptorSet desc[2];
	VkPushConstantRange push[] = {
		{
			/*
			layout(offset = 0) vec2 dst_origin;
			layout(offset = 8) vec2 src_origin;
			layout(offset = 16) vec2 dst_size;
			*/
			.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
			.offset = 0,
			.size = 24,
		},
		{
			/*
			layout(offset = 16) vec4 color;
			*/
			.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
			.offset = 32,
			.size = 16,
		},
	};

	res = vkCreateDescriptorSetLayout(ctx->dev, &(VkDescriptorSetLayoutCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1,
		.pBindings = (VkDescriptorSetLayoutBinding[]){
			{
				.binding = 0,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
				.pImmutableSamplers = (VkSampler[]){ctx->rgb_sampler},
			},
		},
	}, NULL, &ctx->copy_rgb_pipeline.desc_layout);
	if (res != VK_SUCCESS)
		goto error0;
	res = vkCreateDescriptorPool(ctx->dev, &(VkDescriptorPoolCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 2,
		.poolSizeCount = 1,
		.pPoolSizes = (VkDescriptorPoolSize[]){
			{
				.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.descriptorCount = 2,
			},
		},
	}, NULL, &ctx->desc_pool);
	if (res != VK_SUCCESS)
		goto error1;
	res = vkAllocateDescriptorSets(ctx->dev, &(VkDescriptorSetAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = ctx->desc_pool,
		.descriptorSetCount = 1,
		.pSetLayouts = (VkDescriptorSetLayout[]){
			ctx->copy_rgb_pipeline.desc_layout,
		},
	}, desc);
	ctx->copy_rgb_pipeline.desc = desc[0];
	if (res != VK_SUCCESS)
		goto error2;
	res = vkCreatePipelineLayout(ctx->dev, &(VkPipelineLayoutCreateInfo){
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.pushConstantRangeCount = LEN(push),
		.pPushConstantRanges = push,
	}, NULL, &ctx->fill_pipeline.layout);
	if (res != VK_SUCCESS)
		goto error3;
	res = vkCreatePipelineLayout(ctx->dev, &(VkPipelineLayoutCreateInfo){
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &ctx->copy_rgb_pipeline.desc_layout,
		.pushConstantRangeCount = LEN(push),
		.pPushConstantRanges = push,
	}, NULL, &ctx->copy_rgb_pipeline.layout);
	if (res != VK_SUCCESS)
		goto error4;
	info[0] = (VkGraphicsPipelineCreateInfo){
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = 2,
		.pStages = (VkPipelineShaderStageCreateInfo[]){
			{
				.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				.stage = VK_SHADER_STAGE_VERTEX_BIT,
				.module = ctx->vert_shader,
				.pName = "main",
			},
			{
				.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
				.module = ctx->fill_shader,
				.pName = "main",
			},
		},
		.pVertexInputState = &(VkPipelineVertexInputStateCreateInfo){
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
			.vertexBindingDescriptionCount = 1,
			.pVertexBindingDescriptions = &(VkVertexInputBindingDescription){
				.binding = 0,
				.stride = 8,
				.inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
			},
			.vertexAttributeDescriptionCount = 1,
			.pVertexAttributeDescriptions = (VkVertexInputAttributeDescription[]){
				{
					.binding = 0,
					.location = 0,
					.format = VK_FORMAT_R32G32_SINT,
					.offset = 0,
				},
			},
		},
		.pInputAssemblyState = &(VkPipelineInputAssemblyStateCreateInfo){
			.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
			.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
		},
		.pViewportState = &(VkPipelineViewportStateCreateInfo){
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
			.viewportCount = 1,
			.scissorCount = 1,
		},
		.pRasterizationState = &(VkPipelineRasterizationStateCreateInfo){
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
			.polygonMode = VK_POLYGON_MODE_FILL,
			.lineWidth = 1,
		},
		.pMultisampleState = &(VkPipelineMultisampleStateCreateInfo){
			.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
			.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
		},
		.pColorBlendState = &(VkPipelineColorBlendStateCreateInfo){
			.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
			.logicOpEnable = VK_FALSE,
			.logicOp = VK_LOGIC_OP_COPY,
			.attachmentCount = 1,
			.pAttachments = &(VkPipelineColorBlendAttachmentState){
				.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
			},
		},
		.pDynamicState = &(VkPipelineDynamicStateCreateInfo){
			.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
			.dynamicStateCount = 2,
			.pDynamicStates = (VkDynamicState[]){
				VK_DYNAMIC_STATE_VIEWPORT,
				VK_DYNAMIC_STATE_SCISSOR,
			},
		},
		.layout = ctx->fill_pipeline.layout,
		.renderPass = ctx->render_pass,
		.subpass = 0,
	};
	info[1] = info[0];
	info[1].pStages = (VkPipelineShaderStageCreateInfo[]){
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT,
			.module = ctx->vert_shader,
			.pName = "main",
		},
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
			.module = ctx->copy_shader,
			.pName = "main",
		},
	};
	info[1].layout = ctx->copy_rgb_pipeline.layout;
	res = vkCreateGraphicsPipelines(ctx->dev, VK_NULL_HANDLE, LEN(pipeline), info, NULL, pipeline);
	if (res != VK_SUCCESS)
		goto error5;
	ctx->fill_pipeline.vk = pipeline[0];
	ctx->copy_rgb_pipeline.vk = pipeline[1];
	return 0;

error5:
	vkDestroyPipelineLayout(ctx->dev, ctx->copy_rgb_pipeline.layout, NULL);
error4:
	vkDestroyPipelineLayout(ctx->dev, ctx->fill_pipeline.layout, NULL);
error3:
	vkFreeDescriptorSets(ctx->dev, ctx->desc_pool, 1, desc);
error2:
	vkDestroyDescriptorPool(ctx->dev, ctx->desc_pool, NULL);
error1:
	vkDestroyDescriptorSetLayout(ctx->dev, ctx->copy_rgb_pipeline.desc_layout, NULL);
error0:
	return -1;
}

struct blt_context *
blt_vulkan_new(int flags)
{
	struct context *ctx;
	VkResult res;
	const char *ext[4];
	VkPhysicalDevice *phys;
	VkExtensionProperties *ext_prop = NULL;
	VkQueueFamilyProperties *family;
	uint32_t i, j, ext_len, phys_len, ext_prop_len, family_len;

	ctx = malloc(sizeof(*ctx));
	if (!ctx)
		goto error0;
	ctx->base = (struct blt_context){.impl = &impl};

	ext_len = 0;
	if (flags & (BLT_VULKAN_WAYLAND|BLT_VULKAN_X11))
		ext[ext_len++] = VK_KHR_SURFACE_EXTENSION_NAME;
	if (flags & BLT_VULKAN_WAYLAND)
		ext[ext_len++] = "VK_KHR_wayland_surface";
	if (flags & BLT_VULKAN_X11)
		ext[ext_len++] = "VK_KHR_xcb_surface";

	res = vkCreateInstance(&(VkInstanceCreateInfo){
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &(VkApplicationInfo){
			.apiVersion = VK_MAKE_VERSION(1, 1, 0),
		},
		.enabledExtensionCount = ext_len,
		.ppEnabledExtensionNames = ext,
	}, NULL, &ctx->instance);
	if (res != VK_SUCCESS)
		goto error1;

	ext_len = 0;
	if (flags & (BLT_VULKAN_WAYLAND|BLT_VULKAN_X11))
		ext[ext_len++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;

	res = vkEnumeratePhysicalDevices(ctx->instance, &phys_len, NULL);
	if (res != VK_SUCCESS)
		goto error1;
	phys = blt_reallocarray(NULL, phys_len, sizeof(phys[0]));
	if (!phys)
		goto error2;
	res = vkEnumeratePhysicalDevices(ctx->instance, &phys_len, phys);
	if (res != VK_SUCCESS)
		goto error3;

	for (i = 0; i < phys_len; ++i) {
		res = vkEnumerateDeviceExtensionProperties(phys[i], NULL, &ext_prop_len, NULL);
		if (res != VK_SUCCESS)
			goto error4;
		ext_prop = blt_reallocarray(ext_prop, ext_prop_len, sizeof(ext_prop[0]));
		if (!ext_prop)
			goto error4;
		res = vkEnumerateDeviceExtensionProperties(phys[i], NULL, &ext_prop_len, ext_prop);
		if (res != VK_SUCCESS)
			goto error4;
		for (j = 0; j < ext_len; ++j) {
			if (has_extension(ext_prop, ext_prop_len, ext[i])) {
				ctx->phys = phys[i];
				goto found;
			}
		}
	}
	if (ctx->phys == VK_NULL_HANDLE)
		goto error4;
found:
	vkGetPhysicalDeviceQueueFamilyProperties(ctx->phys, &family_len, NULL);
	family = blt_reallocarray(NULL, family_len, sizeof(family[0]));
	if (!family)
		goto error4;
	vkGetPhysicalDeviceQueueFamilyProperties(ctx->phys, &family_len, family);
	for (i = 0; i < family_len; ++i) {
		if (family[i].queueCount > 0 && family[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			ctx->queue_index = i;
			break;
		}
	}
	if (i == family_len)
		goto error5;

	res = vkCreateDevice(ctx->phys, &(VkDeviceCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
			.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
			.queueFamilyIndex = ctx->queue_index,
			.queueCount = 1,
			.pQueuePriorities = (float[]){1},
		},
		.enabledExtensionCount = ext_len,
		.ppEnabledExtensionNames = ext,
	}, NULL, &ctx->dev);
	if (res != VK_SUCCESS)
		goto error5;

	vkGetDeviceQueue(ctx->dev, ctx->queue_index, 0, &ctx->queue);
	res = vkCreateShaderModule(ctx->dev, &(VkShaderModuleCreateInfo){
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = sizeof(vert_spv),
		.pCode = vert_spv,
	}, NULL, &ctx->vert_shader);
	if (res != VK_SUCCESS)
		goto error6;
	res = vkCreateShaderModule(ctx->dev, &(VkShaderModuleCreateInfo){
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = sizeof(fill_spv),
		.pCode = fill_spv,
	}, NULL, &ctx->fill_shader);
	if (res != VK_SUCCESS)
		goto error7;
	res = vkCreateShaderModule(ctx->dev, &(VkShaderModuleCreateInfo){
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = sizeof(copy_spv),
		.pCode = copy_spv,
	}, NULL, &ctx->copy_shader);
	if (res != VK_SUCCESS)
		goto error8;
	res = vkCreateRenderPass(ctx->dev, &(VkRenderPassCreateInfo){
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &(VkAttachmentDescription){
			.format = VK_FORMAT_B8G8R8A8_UNORM,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		},
		.subpassCount = 1,
		.pSubpasses = &(VkSubpassDescription){
			.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
			.colorAttachmentCount = 1,
			.pColorAttachments = &(VkAttachmentReference){
				.attachment = 0,
				.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			},
		},
	}, NULL, &ctx->render_pass);
	if (res != VK_SUCCESS)
		goto error9;
	res = vkCreateSampler(ctx->dev, &(VkSamplerCreateInfo){
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_NEAREST,
		.minFilter = VK_FILTER_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
		.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
		.unnormalizedCoordinates = VK_TRUE,
	}, NULL, &ctx->rgb_sampler);
	if (res != VK_SUCCESS)
		goto error10;
	res = make_pipeline(ctx);
	if (res != VK_SUCCESS)
		goto error11;
	res = vkCreateCommandPool(ctx->dev, &(VkCommandPoolCreateInfo){
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.queueFamilyIndex = ctx->queue_index,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	}, NULL, &ctx->cmd_pool);
	if (res != VK_SUCCESS)
		goto error12;

	free(family);
	free(ext_prop);
	free(phys);

	return &ctx->base;

error12:
	vkDestroyPipeline(ctx->dev, ctx->fill_pipeline.vk, NULL);
	vkDestroyPipeline(ctx->dev, ctx->copy_rgb_pipeline.vk, NULL);
error11:
	vkDestroyPipelineLayout(ctx->dev, ctx->fill_pipeline.layout, NULL);
error10:
	vkDestroyRenderPass(ctx->dev, ctx->render_pass, NULL);
error9:
	vkDestroyShaderModule(ctx->dev, ctx->copy_shader, NULL);
error8:
	vkDestroyShaderModule(ctx->dev, ctx->fill_shader, NULL);
error7:
	vkDestroyShaderModule(ctx->dev, ctx->vert_shader, NULL);
error6:
	vkDestroyDevice(ctx->dev, NULL);
error5:
	free(family);
error4:
	free(ext_prop);
error3:
	free(phys);
error2:
	vkDestroyInstance(ctx->instance, NULL);
error1:
	free(ctx);
error0:
	return NULL;
}
