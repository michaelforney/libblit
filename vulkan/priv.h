struct pipeline {
	VkPipeline vk;
	VkPipelineLayout layout;
	VkDescriptorSet desc;
	VkDescriptorSetLayout desc_layout;
};

struct context {
	struct blt_context base;
	VkInstance instance;
	VkPhysicalDevice phys;
	VkDevice dev;
	VkQueue queue;
	uint32_t queue_index;
	VkRenderPass render_pass;
	VkDescriptorPool desc_pool;
	VkCommandPool cmd_pool;
	VkShaderModule vert_shader, fill_shader, copy_shader;
	struct pipeline fill_pipeline, copy_rgb_pipeline;
	VkSampler rgb_sampler;
};

enum {
	BLT_VULKAN_X11      = 1<<0,
	BLT_VULKAN_WAYLAND  = 1<<1,
};

struct blt_context *blt_vulkan_new(int flags);
struct blt_surface *blt_vulkan_new_surface(struct context *, VkSurfaceKHR, int, int);
