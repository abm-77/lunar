#include "../sys/log.h"
#include "../sys/window.h"
#include "gfx.h"
#include "gfx_handles.h"
#include "gfx_refl.h"
#include "gfx_resources.h"
#include "gfx_upload.h"
#include "umsh.h"

// max swapchain images. triple-buffered MAILBOX needs 3; leave headroom for
// drivers that add an extra image.
#define GFX_MAX_SWAPCHAIN_IMAGES 8u

// number of distinct VkDescriptorType values in the global set (set=0):
//   SAMPLED_IMAGE, SAMPLER, STORAGE_BUFFER — bindings 2+3+4 share one type.
#define GFX_POOL_TYPE_COUNT 3u
// number of STORAGE_BUFFER bindings in the global set (frame_arena,
// draw_packets, material_data).
#define GFX_SSBO_BINDING_COUNT 3u

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <alloca.h>
#include <assert.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//  defined in window.c
extern void *window_glfw_ptr(um_window_handle_t handle);

static const VkPresentModeKHR PM_MAP[] = {
    [GFX_PRESENT_MODE_FIFO] = VK_PRESENT_MODE_FIFO_KHR,
    [GFX_PRESENT_MODE_IMMEDIATE] = VK_PRESENT_MODE_IMMEDIATE_KHR,
    [GFX_PRESENT_MODE_MAILBOX] = VK_PRESENT_MODE_MAILBOX_KHR,
};

static inline uint32_t clamp_u32(uint32_t x, uint32_t lo, uint32_t hi) {
  return x < lo ? lo : x > hi ? hi : x;
}

typedef struct {
  VkInstance instance;
  VkDebugUtilsMessengerEXT
      debug_messenger; // VK_NULL_HANDLE when validation disabled
  VkPhysicalDevice physical_device;
  VkDevice device;
  VkQueue graphics_queue;
  uint32_t graphics_family_index;
  VkSurfaceKHR surface;
  VkSwapchainKHR swapchain;
  VkFormat swapchain_format;
  VkExtent2D swapchain_extent;

  VkImage swapchain_images[GFX_MAX_SWAPCHAIN_IMAGES];
  VkImageView swapchain_image_views[GFX_MAX_SWAPCHAIN_IMAGES];
  uint32_t swapchain_image_count;
  uint32_t current_image_index; // set by vkAcquireNextImageKHR each frame

  VkRenderPass render_pass;
  VkFramebuffer
      framebuffers[GFX_MAX_SWAPCHAIN_IMAGES]; // one per swapchain image

  // per-frame sync; indexed by current_frame % frames_in_flight
  VkSemaphore image_available[GFX_MAX_FRAMES_IN_FLIGHT];
  VkSemaphore render_done[GFX_MAX_FRAMES_IN_FLIGHT];
  VkFence in_flight_fences[GFX_MAX_FRAMES_IN_FLIGHT];
  VkCommandPool command_pool;
  VkCommandBuffer command_buffers[GFX_MAX_FRAMES_IN_FLIGHT];

  // global pipeline layout (all pipelines share this)
  VkDescriptorSetLayout global_set_layout;
  VkDescriptorPool descriptor_pool;
  VkDescriptorSet descriptor_sets[GFX_MAX_FRAMES_IN_FLIGHT];
  VkPipelineLayout pipeline_layout;

  // draw packets SSBO: one device-local buffer per frame slot, persistently
  // mapped
  VkBuffer draw_packets_buf[GFX_MAX_FRAMES_IN_FLIGHT];
  VkDeviceMemory draw_packets_mem[GFX_MAX_FRAMES_IN_FLIGHT];
  void *draw_packets_mapped[GFX_MAX_FRAMES_IN_FLIGHT];
  uint32_t draw_packets_used; // running count within a frame, reset at begin_frame

  VkPipeline pipeline_table[GFX_MAX_PIPELINES];
  gfx_slot_t pipeline_slots[GFX_MAX_PIPELINES];

  gfx_resource_table_t resources;
  gfx_frame_arena_t arena;

  rt_gfx_config_t cfg;
  um_window_handle_t window_handle; // stored for swapchain recreation on resize
  uint64_t
      min_ssbo_alignment; // VkPhysicalDeviceLimits.minStorageBufferOffsetAlignment

  VkImage depth_image;
  VkDeviceMemory depth_memory;
  VkImageView depth_image_view;
  bool depth_enabled;

  uint32_t
      current_frame; // monotonically increasing; mod frames_in_flight for slot
  bool frame_open;
} gfx_device_ctx_t;

// v0: single global device
static gfx_device_ctx_t g_dev;
static bool g_dev_alive = false;

static gfx_device_ctx_t *dev_from_handle(gfx_device_handle_t h) {
  (void)h;
  assert(g_dev_alive);
  return &g_dev;
}

static uint32_t find_memory_type(VkPhysicalDevice phys, uint32_t type_bits,
                                 VkMemoryPropertyFlags required) {
  VkPhysicalDeviceMemoryProperties props;
  vkGetPhysicalDeviceMemoryProperties(phys, &props);
  for (uint32_t j = 0; j < props.memoryTypeCount; j++) {
    if ((type_bits & (1u << j)) &&
        (props.memoryTypes[j].propertyFlags & required) == required)
      return j;
  }
  return UINT32_MAX;
}

static VkCommandBuffer begin_one_time_cmd(gfx_device_ctx_t *d) {
  VkCommandBufferAllocateInfo ai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = d->command_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
  };
  VkCommandBuffer cmd;
  vkAllocateCommandBuffers(d->device, &ai, &cmd);
  VkCommandBufferBeginInfo bi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  vkBeginCommandBuffer(cmd, &bi);
  return cmd;
}

static void end_one_time_cmd(gfx_device_ctx_t *d, VkCommandBuffer cmd) {
  vkEndCommandBuffer(cmd);
  VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &cmd,
  };
  VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence;
  vkCreateFence(d->device, &fi, NULL, &fence);
  vkQueueSubmit(d->graphics_queue, 1, &si, fence);
  vkWaitForFences(d->device, 1, &fence, VK_TRUE, UINT64_MAX);
  vkDestroyFence(d->device, fence, NULL);
  vkFreeCommandBuffers(d->device, d->command_pool, 1, &cmd);
}

static void vk_set_debug_name(VkDevice device, VkObjectType type,
                              uint64_t handle, const char *name) {
  if (!name) return;
  PFN_vkSetDebugUtilsObjectNameEXT fn =
      (PFN_vkSetDebugUtilsObjectNameEXT)vkGetDeviceProcAddr(
          device, "vkSetDebugUtilsObjectNameEXT");
  if (!fn) return;
  VkDebugUtilsObjectNameInfoEXT info = {
      .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
      .objectType = type,
      .objectHandle = handle,
      .pObjectName = name,
  };
  fn(device, &info);
}

static const char *VALIDATION_LAYERS[] = {"VK_LAYER_KHRONOS_validation"};
static const char *DEVICE_EXTENSIONS[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

static VKAPI_ATTR VkBool32 VKAPI_CALL
debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
               VkDebugUtilsMessageTypeFlagsEXT type,
               const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
  (void)type;
  (void)user;
  const char *prefix =
      (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "[vk error]"
      : (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
          ? "[vk warning]"
          : "[vk info]";
  fprintf(stderr, "%s %s\n", prefix, data->pMessage);
  return VK_FALSE;
}

static VkDebugUtilsMessengerCreateInfoEXT make_debug_ci(void) {
  VkDebugUtilsMessengerCreateInfoEXT ci = {
      .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
      .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
      .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
      .pfnUserCallback = debug_callback,
  };
  return ci;
}

static bool create_instance(gfx_device_ctx_t *d, bool validation) {
  if (validation) {
    uint32_t layer_count = 0;
    vkEnumerateInstanceLayerProperties(&layer_count, NULL);
    VkLayerProperties *layers = alloca(sizeof(VkLayerProperties) * layer_count);
    vkEnumerateInstanceLayerProperties(&layer_count, layers);
    bool found = false;
    for (uint32_t i = 0; i < layer_count; i++) {
      if (strcmp(layers[i].layerName, "VK_LAYER_KHRONOS_validation") == 0) {
        found = true;
        break;
      }
    }
    if (!found) {
      RT_LOG_WARN("gfx", "VK_LAYER_KHRONOS_validation not available; disabling validation");
      validation = false;
    }
  }

  VkApplicationInfo app_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "umbral",
      .applicationVersion = VK_MAKE_VERSION(0, 1, 0),
      .pEngineName = "umbral",
      .engineVersion = VK_MAKE_VERSION(0, 1, 0),
      .apiVersion = VK_API_VERSION_1_2,
  };

  uint32_t glfw_ext_count = 0;
  const char **glfw_exts = glfwGetRequiredInstanceExtensions(&glfw_ext_count);

  uint32_t ext_count = glfw_ext_count + (validation ? 1u : 0u);
  const char **exts = alloca(sizeof(char *) * ext_count);
  memcpy(exts, glfw_exts, sizeof(char *) * glfw_ext_count);
  if (validation) exts[glfw_ext_count] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;

  VkInstanceCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app_info,
      .enabledExtensionCount = ext_count,
      .ppEnabledExtensionNames = exts,
  };

  VkDebugUtilsMessengerCreateInfoEXT dbg_ci = make_debug_ci();
  if (validation) {
    ci.enabledLayerCount = 1;
    ci.ppEnabledLayerNames = VALIDATION_LAYERS;
    // chaining dbg_ci into pNext enables validation during vkCreateInstance /
    // vkDestroyInstance themselves (before the messenger object exists)
    ci.pNext = &dbg_ci;
  }

  if (vkCreateInstance(&ci, NULL, &d->instance) != VK_SUCCESS) {
    RT_LOG_ERROR("gfx", "vkCreateInstance failed");
    return false;
  }

  // create the persistent debug messenger now that the instance exists
  if (validation) {
    PFN_vkCreateDebugUtilsMessengerEXT fn =
        (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            d->instance, "vkCreateDebugUtilsMessengerEXT");
    if (fn) {
      fn(d->instance, &dbg_ci, NULL, &d->debug_messenger);
    } else {
      RT_LOG_WARN("gfx", "vkCreateDebugUtilsMessengerEXT not found; debug callback unavailable");
    }
  }

  return true;
}

static bool select_physical_device(gfx_device_ctx_t *d) {
  uint32_t count = 0;
  vkEnumeratePhysicalDevices(d->instance, &count, NULL);
  if (count == 0) {
    RT_LOG_ERROR("gfx", "no Vulkan-capable GPU found");
    return false;
  }

  VkPhysicalDevice *devs = alloca(sizeof(VkPhysicalDevice) * count);
  vkEnumeratePhysicalDevices(d->instance, &count, devs);

  // prefer discrete GPU; fall back to first available
  d->physical_device = VK_NULL_HANDLE;
  for (uint32_t i = 0; i < count; i++) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(devs[i], &props);
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
      d->physical_device = devs[i];
      break;
    }
  }
  if (d->physical_device == VK_NULL_HANDLE) d->physical_device = devs[0];

  // verify required descriptor indexing features for bindless arrays
  VkPhysicalDeviceDescriptorIndexingFeatures di = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES,
  };
  VkPhysicalDeviceFeatures2 f2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
      .pNext = &di,
  };
  vkGetPhysicalDeviceFeatures2(d->physical_device, &f2);

  if (!di.descriptorBindingPartiallyBound) {
    RT_LOG_ERROR("gfx", "GPU missing descriptorBindingPartiallyBound");
    return false;
  }
  if (!di.runtimeDescriptorArray) {
    RT_LOG_ERROR("gfx", "GPU missing runtimeDescriptorArray");
    return false;
  }
  if (!di.shaderSampledImageArrayNonUniformIndexing) {
    RT_LOG_ERROR("gfx", "GPU missing shaderSampledImageArrayNonUniformIndexing");
    return false;
  }

  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(d->physical_device, &props);
  d->min_ssbo_alignment = props.limits.minStorageBufferOffsetAlignment;

  RT_LOG_INFO("gfx", "selected GPU: %s", props.deviceName);
  return true;
}

static bool create_device(gfx_device_ctx_t *d) {
  // find a queue family that supports graphics (assume it also supports
  // present)
  uint32_t qfam_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(d->physical_device, &qfam_count,
                                           NULL);
  VkQueueFamilyProperties *qfams =
      alloca(sizeof(VkQueueFamilyProperties) * qfam_count);
  vkGetPhysicalDeviceQueueFamilyProperties(d->physical_device, &qfam_count,
                                           qfams);

  d->graphics_family_index = UINT32_MAX;
  for (uint32_t i = 0; i < qfam_count; i++) {
    if (qfams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      d->graphics_family_index = i;
      break;
    }
  }
  if (d->graphics_family_index == UINT32_MAX) {
    RT_LOG_ERROR("gfx", "no graphics queue family found");
    return false;
  }

  float prio = 1.0f;
  VkDeviceQueueCreateInfo queue_ci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = d->graphics_family_index,
      .queueCount = 1,
      .pQueuePriorities = &prio,
  };

  // Vulkan 1.1 features: shaderDrawParameters for gl_DrawID / gl_BaseInstance
  VkPhysicalDeviceVulkan11Features vk11_features = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
      .shaderDrawParameters = VK_TRUE,
  };

  // enable descriptor indexing features required for bindless arrays
  VkPhysicalDeviceDescriptorIndexingFeatures di_features = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES,
      .pNext = &vk11_features,
      .descriptorBindingPartiallyBound = VK_TRUE,
      .runtimeDescriptorArray = VK_TRUE,
      .shaderSampledImageArrayNonUniformIndexing = VK_TRUE,
      .descriptorBindingSampledImageUpdateAfterBind = VK_TRUE,
      .descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE,
  };

  VkPhysicalDeviceFeatures2 features2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
      .pNext = &di_features,
      .features =
          {
              .vertexPipelineStoresAndAtomics = VK_TRUE,
              .shaderInt64 = VK_TRUE,
              .shaderSampledImageArrayDynamicIndexing = VK_TRUE,
              .shaderStorageBufferArrayDynamicIndexing = VK_TRUE,
          },
  };

  VkDeviceCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = &features2,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_ci,
      .enabledExtensionCount = 1,
      .ppEnabledExtensionNames = DEVICE_EXTENSIONS,
      // pEnabledFeatures intentionally NULL; features are in pNext chain
  };

  if (d->cfg.enable_validation) {
    // device layers are deprecated in Vulkan 1.1+ but set for compatibility
    ci.enabledLayerCount = 1;
    ci.ppEnabledLayerNames = VALIDATION_LAYERS;
  }

  if (vkCreateDevice(d->physical_device, &ci, NULL, &d->device) != VK_SUCCESS) {
    RT_LOG_ERROR("gfx", "vkCreateDevice failed");
    return false;
  }

  vkGetDeviceQueue(d->device, d->graphics_family_index, 0, &d->graphics_queue);
  return true;
}

static bool create_swapchain(gfx_device_ctx_t *d,
                             um_window_handle_t window_handle) {
  GLFWwindow *window = window_glfw_ptr(window_handle);
  VkResult sr = glfwCreateWindowSurface(d->instance, window, NULL, &d->surface);
  if (sr != VK_SUCCESS) {
    const char *glfw_desc = NULL;
    int glfw_err = glfwGetError(&glfw_desc);
    RT_LOG_ERROR("gfx", "glfwCreateWindowSurface failed: VkResult=%d glfw_err=0x%x (%s)",
                 sr, glfw_err, glfw_desc ? glfw_desc : "none");
    return false;
  }

  VkSurfaceCapabilitiesKHR caps;
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(d->physical_device, d->surface,
                                            &caps);

  uint32_t fmt_count = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(d->physical_device, d->surface,
                                       &fmt_count, NULL);
  VkSurfaceFormatKHR fmts[fmt_count];
  vkGetPhysicalDeviceSurfaceFormatsKHR(d->physical_device, d->surface,
                                       &fmt_count, fmts);
  VkSurfaceFormatKHR chosen_fmt = fmts[0];
  for (int i = 0; i < fmt_count; ++i) {
    if (fmts[i].format == VK_FORMAT_B8G8R8A8_SRGB &&
        fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
      chosen_fmt = fmts[i];
  }

  uint32_t pm_count = 0;
  vkGetPhysicalDeviceSurfacePresentModesKHR(d->physical_device, d->surface,
                                            &pm_count, NULL);
  VkPresentModeKHR pms[pm_count];
  vkGetPhysicalDeviceSurfacePresentModesKHR(d->physical_device, d->surface,
                                            &pm_count, pms);

  bool desired_pm_present = false;
  VkPresentModeKHR chosen_pm = PM_MAP[d->cfg.present_mode];
  for (int i = 0; i < pm_count; ++i) {
    if (pms[i] == chosen_pm) {
      desired_pm_present = true;
      break;
    }
  }
  if (!desired_pm_present) chosen_pm = VK_PRESENT_MODE_FIFO_KHR;

  VkExtent2D extent;
  if (caps.currentExtent.width != UINT32_MAX) {
    extent = caps.currentExtent;
  } else {
    int w, h;
    glfwGetFramebufferSize(window, &w, &h);
    extent.width = clamp_u32((uint32_t)w, caps.minImageExtent.width,
                             caps.maxImageExtent.width);
    extent.height = clamp_u32((uint32_t)h, caps.minImageExtent.height,
                              caps.maxImageExtent.height);
  }

  uint32_t image_count = caps.minImageCount + 1;
  if (caps.maxImageCount > 0 && image_count > caps.maxImageCount)
    image_count = caps.maxImageCount;
  if (image_count > GFX_MAX_SWAPCHAIN_IMAGES)
    image_count = GFX_MAX_SWAPCHAIN_IMAGES;

  VkSwapchainCreateInfoKHR sci = {
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .surface = d->surface,
      .minImageCount = image_count,
      .imageFormat = chosen_fmt.format,
      .imageColorSpace = chosen_fmt.colorSpace,
      .imageExtent = extent,
      .imageArrayLayers = 1,
      .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .preTransform = caps.currentTransform,
      .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      .presentMode = chosen_pm,
      .clipped = VK_TRUE,
      .oldSwapchain = VK_NULL_HANDLE,
  };
  vkCreateSwapchainKHR(d->device, &sci, NULL, &d->swapchain);
  d->swapchain_format = chosen_fmt.format;
  d->swapchain_extent = extent;

  vkGetSwapchainImagesKHR(d->device, d->swapchain, &d->swapchain_image_count,
                          NULL);
  vkGetSwapchainImagesKHR(d->device, d->swapchain, &d->swapchain_image_count,
                          d->swapchain_images);
  for (int i = 0; i < d->swapchain_image_count; ++i) {
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = d->swapchain_images[i],
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = d->swapchain_format,
        .components = {VK_COMPONENT_SWIZZLE_IDENTITY,
                       VK_COMPONENT_SWIZZLE_IDENTITY,
                       VK_COMPONENT_SWIZZLE_IDENTITY,
                       VK_COMPONENT_SWIZZLE_IDENTITY},
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .baseMipLevel = 0,
                             .levelCount = 1,
                             .baseArrayLayer = 0,
                             .layerCount = 1},
    };
    vkCreateImageView(d->device, &vci, NULL, &d->swapchain_image_views[i]);
  };

  return true;
}

static bool create_render_pass(gfx_device_ctx_t *d) {
  VkAttachmentDescription color_att = {
      .format = d->swapchain_format,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
  };

  VkAttachmentReference color_ref = {
      .attachment = 0,
      .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
  };

  VkAttachmentDescription depth_att = {
      .format = VK_FORMAT_D32_SFLOAT,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
  };

  VkAttachmentReference depth_ref = {
      .attachment = 1,
      .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
  };

  VkSubpassDescription subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 1,
      .pColorAttachments = &color_ref,
      .pDepthStencilAttachment = d->depth_enabled ? &depth_ref : NULL,
  };

  // dependency ensures swapchain image is ready before we write to it and
  // that writes are visible before present reads them
  VkSubpassDependency dep = {
      .srcSubpass = VK_SUBPASS_EXTERNAL,
      .dstSubpass = 0,
      .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                      (d->depth_enabled
                           ? VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                           : 0),
      .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                      (d->depth_enabled
                           ? VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                           : 0),
      .srcAccessMask = 0,
      .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                       (d->depth_enabled
                            ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                            : 0),
  };

  VkAttachmentDescription attachments[2] = {color_att, depth_att};

  VkRenderPassCreateInfo rp_ci = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = d->depth_enabled ? 2u : 1u,
      .pAttachments = attachments,
      .subpassCount = 1,
      .pSubpasses = &subpass,
      .dependencyCount = 1,
      .pDependencies = &dep,
  };

  vkCreateRenderPass(d->device, &rp_ci, NULL, &d->render_pass);
  return true;
}

static bool create_framebuffers(gfx_device_ctx_t *d) {
  for (int i = 0; i < d->swapchain_image_count; ++i) {
    VkImageView attachments[] = {d->swapchain_image_views[i],
                                 d->depth_image_view};
    VkFramebufferCreateInfo fb_ci = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = d->render_pass,
        .attachmentCount = d->depth_enabled ? 2u : 1u,
        .pAttachments = attachments,
        .width = d->swapchain_extent.width,
        .height = d->swapchain_extent.height,
        .layers = 1,
    };
    vkCreateFramebuffer(d->device, &fb_ci, NULL, &d->framebuffers[i]);
  }
  return true;
}

static void write_arena_descriptors(gfx_device_ctx_t *d) {
  for (uint32_t i = 0; i < d->cfg.frames_in_flight; i++) {
    VkDescriptorBufferInfo buf_info = {
        .buffer = d->arena.buffer,
        .offset = (VkDeviceSize)i * d->cfg.frame_arena_bytes,
        .range = d->cfg.frame_arena_bytes,
    };
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = d->descriptor_sets[i],
        .dstBinding = 2,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &buf_info,
    };
    vkUpdateDescriptorSets(d->device, 1, &write, 0, NULL);
  }
}

static void destroy_framebuffers(gfx_device_ctx_t *d) {
  for (uint32_t i = 0; i < d->swapchain_image_count; i++)
    vkDestroyFramebuffer(d->device, d->framebuffers[i], NULL);
}

// destroys image views and the swapchain itself; does NOT destroy the surface
// (surface lifetime is tied to the instance, not the swapchain).
// render_pass is also left alive — it stays compatible across recreations as
// long as the format doesn't change.
static void destroy_swapchain(gfx_device_ctx_t *d) {
  for (uint32_t i = 0; i < d->swapchain_image_count; i++)
    vkDestroyImageView(d->device, d->swapchain_image_views[i], NULL);
  vkDestroySwapchainKHR(d->device, d->swapchain, NULL);
  d->swapchain = VK_NULL_HANDLE;
  d->swapchain_image_count = 0;
}

static bool create_depth_resources(gfx_device_ctx_t *d) {
  VkFormat depth_fmt = VK_FORMAT_D32_SFLOAT;
  VkImageCreateInfo img_ci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = depth_fmt,
      .extent = {d->swapchain_extent.width, d->swapchain_extent.height, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
  };
  vkCreateImage(d->device, &img_ci, NULL, &d->depth_image);

  VkMemoryRequirements mem_req;
  vkGetImageMemoryRequirements(d->device, d->depth_image, &mem_req);
  VkMemoryAllocateInfo alloc_ci = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = mem_req.size,
      .memoryTypeIndex = find_memory_type(d->physical_device,
                                          mem_req.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
  };
  vkAllocateMemory(d->device, &alloc_ci, NULL, &d->depth_memory);
  vkBindImageMemory(d->device, d->depth_image, d->depth_memory, 0);

  VkImageViewCreateInfo view_ci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = d->depth_image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = depth_fmt,
      .subresourceRange =
          {
              .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
              .levelCount = 1,
              .layerCount = 1,
          },
  };
  vkCreateImageView(d->device, &view_ci, NULL, &d->depth_image_view);
  return true;
}

static void destroy_depth_resources(gfx_device_ctx_t *d) {
  if (d->depth_image_view)
    vkDestroyImageView(d->device, d->depth_image_view, NULL);
  if (d->depth_image) vkDestroyImage(d->device, d->depth_image, NULL);
  if (d->depth_memory) vkFreeMemory(d->device, d->depth_memory, NULL);
  d->depth_image_view = VK_NULL_HANDLE;
  d->depth_image = VK_NULL_HANDLE;
  d->depth_memory = VK_NULL_HANDLE;
}

static bool create_sync_objects(gfx_device_ctx_t *d) {
  VkSemaphoreCreateInfo sem_ci = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
  };

  // pre-signal fence so first wait returns immediately
  VkFenceCreateInfo fence_ci = {
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
      .flags = VK_FENCE_CREATE_SIGNALED_BIT,
  };

  for (int i = 0; i < d->cfg.frames_in_flight; ++i) {
    vkCreateSemaphore(d->device, &sem_ci, NULL, &d->image_available[i]);
    vkCreateSemaphore(d->device, &sem_ci, NULL, &d->render_done[i]);
    vkCreateFence(d->device, &fence_ci, NULL, &d->in_flight_fences[i]);
  }

  return true;
}

static bool create_command_pool_and_buffers(gfx_device_ctx_t *d) {
  VkCommandPoolCreateInfo pool_ci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = d->graphics_family_index,
  };
  vkCreateCommandPool(d->device, &pool_ci, NULL, &d->command_pool);

  VkCommandBufferAllocateInfo alloc_ci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = d->command_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = d->cfg.frames_in_flight,
  };
  vkAllocateCommandBuffers(d->device, &alloc_ci, d->command_buffers);

  return true;
}

static bool create_global_descriptor_layout(gfx_device_ctx_t *d) {
  VkDescriptorSetLayoutBinding bindings[] = {
      // binding 0: bindless sampled images
      {
          .binding = 0,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .descriptorCount = d->cfg.max_textures,
          .stageFlags = VK_SHADER_STAGE_ALL,
      },
      // binding 1: bindless samplers
      {
          .binding = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
          .descriptorCount = d->cfg.max_samplers,
          .stageFlags = VK_SHADER_STAGE_ALL,
      },
      // binding 2: frame_arena_ssbo, transient per-frame upload
      {
          .binding = 2,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_ALL,
      },
      // binding 3: draw_packets_ssbo, indexed by gl_DrawID
      {
          .binding = 3,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_ALL,
      },
      // binding 4: material_data_ssbo, persistent, per-material
      {
          .binding = 4,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
      },
  };

  // per-binding flags for the bindless arrays (bindings 0 and 1):
  //   UPDATE_AFTER_BIND: allow descriptor writes while sets are bound
  //   PARTIALLY_BOUND:   unwritten descriptors may exist without error
  VkDescriptorBindingFlags bind_flags[] = {
      // binding 0
      VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
          VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
      // binding 1
      VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
          VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
      // binding 2
      VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
      // binding 3
      VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
      // binding 4
      VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
  };

  assert((sizeof(bindings) / sizeof(VkDescriptorSetLayoutBinding)) ==
         (sizeof(bind_flags) / sizeof(VkDescriptorBindingFlags)));
  const uint32_t n_bindings =
      sizeof(bind_flags) / sizeof(VkDescriptorBindingFlags);

  VkDescriptorSetLayoutBindingFlagsCreateInfo flags_ci = {
      .sType =
          VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
      .bindingCount = n_bindings,
      .pBindingFlags = bind_flags,
  };

  VkDescriptorSetLayoutCreateInfo layout_ci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .pNext = &flags_ci,
      .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
      .bindingCount = n_bindings,
      .pBindings = bindings,
  };
  vkCreateDescriptorSetLayout(d->device, &layout_ci, NULL,
                              &d->global_set_layout);

  VkPipelineLayoutCreateInfo pl_ci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &d->global_set_layout,
      .pushConstantRangeCount = 0, // TODO: add push constants
  };
  vkCreatePipelineLayout(d->device, &pl_ci, NULL, &d->pipeline_layout);

  return true;
}

static bool create_descriptor_pool_and_sets(gfx_device_ctx_t *d) {

  uint32_t fif = d->cfg.frames_in_flight;
  VkDescriptorPoolSize pool_sizes[GFX_POOL_TYPE_COUNT] = {
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, d->cfg.max_textures * fif},
      {VK_DESCRIPTOR_TYPE_SAMPLER, d->cfg.max_samplers * fif},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, GFX_SSBO_BINDING_COUNT * fif},
  };
  VkDescriptorPoolCreateInfo pool_ci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
      .maxSets = fif,
      .poolSizeCount = GFX_POOL_TYPE_COUNT,
      .pPoolSizes = pool_sizes,
  };
  vkCreateDescriptorPool(d->device, &pool_ci, NULL, &d->descriptor_pool);

  VkDescriptorSetLayout layouts[GFX_MAX_FRAMES_IN_FLIGHT];
  for (int i = 0; i < fif; ++i) layouts[i] = d->global_set_layout;

  VkDescriptorSetAllocateInfo alloc_ci = {

      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = d->descriptor_pool,
      .descriptorSetCount = fif,
      .pSetLayouts = layouts,
  };
  vkAllocateDescriptorSets(d->device, &alloc_ci, d->descriptor_sets);

  return true;
}

static bool create_draw_packet_buffers(gfx_device_ctx_t *d) {
  uint64_t buf_size = sizeof(draw_packet_t) * d->cfg.draw_packets_max;
  for (uint32_t i = 0; i < d->cfg.frames_in_flight; i++) {
    VkBufferCreateInfo buf_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = buf_size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(d->device, &buf_ci, NULL, &d->draw_packets_buf[i]) !=
        VK_SUCCESS)
      return false;

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(d->device, d->draw_packets_buf[i], &req);
    uint32_t mt = find_memory_type(d->physical_device, req.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt == UINT32_MAX) return false;

    VkMemoryAllocateInfo alloc_ci = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = mt,
    };
    if (vkAllocateMemory(d->device, &alloc_ci, NULL, &d->draw_packets_mem[i]) !=
        VK_SUCCESS)
      return false;
    vkBindBufferMemory(d->device, d->draw_packets_buf[i],
                       d->draw_packets_mem[i], 0);
    vkMapMemory(d->device, d->draw_packets_mem[i], 0, buf_size, 0,
                &d->draw_packets_mapped[i]);

    VkDescriptorBufferInfo buf_info = {
        .buffer = d->draw_packets_buf[i],
        .offset = 0,
        .range = buf_size,
    };
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = d->descriptor_sets[i],
        .dstBinding = 3,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &buf_info,
    };
    vkUpdateDescriptorSets(d->device, 1, &write, 0, NULL);
  }
  return true;
}

gfx_device_handle_t rt_gfx_init(uint64_t window_handle,
                                uint32_t frames_in_flight,
                                uint32_t max_textures, uint32_t max_samplers,
                                uint64_t frame_arena_bytes,
                                uint32_t draw_packets_max,
                                bool enable_validation, uint32_t present_mode,
                                bool enable_depth) {
  rt_log_init();
  assert(!g_dev_alive && "rt_gfx_init called twice");
  memset(&g_dev, 0, sizeof(g_dev));

  g_dev.cfg.frames_in_flight = frames_in_flight;
  g_dev.cfg.max_textures = max_textures;
  g_dev.cfg.max_samplers = max_samplers;
  g_dev.cfg.frame_arena_bytes = frame_arena_bytes;
  g_dev.cfg.draw_packets_max = draw_packets_max;
  g_dev.cfg.enable_validation = enable_validation;
  g_dev.cfg.enable_depth = enable_depth;
  g_dev.cfg.present_mode = (gfx_present_mode_t)present_mode;
  g_dev.window_handle = window_handle;
  g_dev.depth_enabled = enable_depth;

  gfx_device_ctx_t *d = &g_dev;

  if (!create_instance(d, enable_validation)) goto fail;
  if (!select_physical_device(d)) goto fail;
  if (!create_device(d)) goto fail;

  if (!create_swapchain(d, window_handle)) goto fail;
  if (d->depth_enabled && !create_depth_resources(d)) goto fail;
  if (!create_render_pass(d)) goto fail;
  if (!create_framebuffers(d)) goto fail;

  if (!create_sync_objects(d)) goto fail;
  if (!create_command_pool_and_buffers(d)) goto fail;

  if (!create_global_descriptor_layout(d)) goto fail;
  if (!create_descriptor_pool_and_sets(d)) goto fail;

  gfx_arena_init(&d->arena, frame_arena_bytes, frames_in_flight, d->device,
                 d->physical_device);
  write_arena_descriptors(d);

  if (!create_draw_packet_buffers(d)) goto fail;

  g_dev_alive = true;
  return gfx_handle_make(1, 1);

fail:
  // partial cleanup: only resources that may have been created
  if (d->device != VK_NULL_HANDLE) vkDestroyDevice(d->device, NULL);
  if (d->debug_messenger != VK_NULL_HANDLE) {
    PFN_vkDestroyDebugUtilsMessengerEXT fn =
        (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            d->instance, "vkDestroyDebugUtilsMessengerEXT");
    if (fn) fn(d->instance, d->debug_messenger, NULL);
  }
  if (d->instance != VK_NULL_HANDLE) vkDestroyInstance(d->instance, NULL);
  memset(d, 0, sizeof(*d));
  return GFX_NULL_HANDLE;
}

void rt_gfx_shutdown(gfx_device_handle_t dev) {
  gfx_device_ctx_t *d = dev_from_handle(dev);

  vkDeviceWaitIdle(d->device);

  // flush deferred destroy queue immediately (no more frames will run)
  for (uint32_t i = 0; i < d->resources.deferred_count; i++) {
    uint32_t slot = (d->resources.deferred_head + i) % GFX_DEFERRED_QUEUE_MAX;
    d->resources.deferred[slot].frames_remaining = 0;
  }
  gfx_deferred_tick(&d->resources, d->device);

  gfx_arena_destroy(&d->arena, d->device);

  for (uint32_t i = 0; i < d->cfg.frames_in_flight; i++) {
    if (d->draw_packets_mapped[i])
      vkUnmapMemory(d->device, d->draw_packets_mem[i]);
    if (d->draw_packets_mem[i])
      vkFreeMemory(d->device, d->draw_packets_mem[i], NULL);
    if (d->draw_packets_buf[i])
      vkDestroyBuffer(d->device, d->draw_packets_buf[i], NULL);
  }

  // implicitly frees all descriptor sets allocated from it
  vkDestroyDescriptorPool(d->device, d->descriptor_pool, NULL);
  vkDestroyPipelineLayout(d->device, d->pipeline_layout, NULL);
  vkDestroyDescriptorSetLayout(d->device, d->global_set_layout, NULL);

  for (uint32_t i = 1; i < GFX_MAX_PIPELINES; i++) {
    if (d->pipeline_slots[i].allocated)
      vkDestroyPipeline(d->device, d->pipeline_table[i], NULL);
  }

  destroy_framebuffers(d);
  if (d->depth_enabled) destroy_depth_resources(d);
  vkDestroyRenderPass(d->device, d->render_pass, NULL);
  destroy_swapchain(d);
  vkDestroySurfaceKHR(d->instance, d->surface, NULL);

  for (uint32_t i = 0; i < d->cfg.frames_in_flight; i++) {
    vkDestroySemaphore(d->device, d->image_available[i], NULL);
    vkDestroySemaphore(d->device, d->render_done[i], NULL);
    vkDestroyFence(d->device, d->in_flight_fences[i], NULL);
  }

  // implicitly frees all VkCommandBuffers allocated from it
  vkDestroyCommandPool(d->device, d->command_pool, NULL);

  vkDestroyDevice(d->device, NULL);

  if (d->debug_messenger != VK_NULL_HANDLE) {
    PFN_vkDestroyDebugUtilsMessengerEXT fn =
        (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            d->instance, "vkDestroyDebugUtilsMessengerEXT");
    if (fn) fn(d->instance, d->debug_messenger, NULL);
  }

  vkDestroyInstance(d->instance, NULL);

  memset(d, 0, sizeof(*d));
  g_dev_alive = false;
}

gfx_cmd_handle_t rt_gfx_begin_frame(gfx_device_handle_t dev) {
  gfx_device_ctx_t *d = dev_from_handle(dev);
  assert(!d->frame_open);

  uint32_t slot = d->current_frame % d->cfg.frames_in_flight;

  vkWaitForFences(d->device, 1, &d->in_flight_fences[slot], VK_TRUE,
                  UINT64_MAX);
  gfx_deferred_tick(&d->resources, d->device);
  gfx_arena_next_frame(&d->arena);
  vkResetFences(d->device, 1, &d->in_flight_fences[slot]);
  VkResult r = vkAcquireNextImageKHR(d->device, d->swapchain, UINT64_MAX,
                                     d->image_available[slot], VK_NULL_HANDLE,
                                     &d->current_image_index);
  if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
    vkDeviceWaitIdle(d->device);
    destroy_framebuffers(d);
    if (d->depth_enabled) destroy_depth_resources(d);
    destroy_swapchain(d);
    create_swapchain(d, d->window_handle);
    if (d->depth_enabled) create_depth_resources(d);
    create_framebuffers(d);
    return GFX_NULL_HANDLE;
  }

  vkResetCommandBuffer(d->command_buffers[slot], 0);
  VkCommandBufferBeginInfo begin_ci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  vkBeginCommandBuffer(d->command_buffers[slot], &begin_ci);

  VkClearValue clear_vals[2] = {
      {.color = {.float32 = {0, 0, 0, 1}}},
      {.depthStencil = {.depth = 1.0f, .stencil = 0}},
  };
  VkRenderPassBeginInfo rp_bi = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = d->render_pass,
      .framebuffer = d->framebuffers[d->current_image_index],
      .renderArea = {.offset = {0, 0}, .extent = d->swapchain_extent},
      .clearValueCount = d->depth_enabled ? 2u : 1u,
      .pClearValues = clear_vals,
  };
  vkCmdBeginRenderPass(d->command_buffers[slot], &rp_bi,
                       VK_SUBPASS_CONTENTS_INLINE);

  VkViewport vp = {
      .x = 0,
      .y = 0,
      .width = (float)d->swapchain_extent.width,
      .height = (float)d->swapchain_extent.height,
      .minDepth = 0.0f,
      .maxDepth = 1.0f,
  };
  vkCmdSetViewport(d->command_buffers[slot], 0, 1, &vp);
  VkRect2D scissor = {.offset = {0, 0}, .extent = d->swapchain_extent};
  vkCmdSetScissor(d->command_buffers[slot], 0, 1, &scissor);

  gfx_resources_update_descriptors(&d->resources, d->device,
                                   d->descriptor_sets[slot], slot);
  vkCmdBindDescriptorSets(d->command_buffers[slot],
                          VK_PIPELINE_BIND_POINT_GRAPHICS, d->pipeline_layout,
                          0, 1, &d->descriptor_sets[slot], 0, NULL);

  d->frame_open = true;
  d->draw_packets_used = 0;
  return gfx_handle_make(slot + 1, 1);
}

void rt_gfx_end_frame(gfx_device_handle_t dev, gfx_cmd_handle_t cmd) {
  gfx_device_ctx_t *d = dev_from_handle(dev);
  assert(d->frame_open);
  uint32_t slot = d->current_frame % d->cfg.frames_in_flight;

  vkCmdEndRenderPass(d->command_buffers[slot]);
  vkEndCommandBuffer(d->command_buffers[slot]);

  VkPipelineStageFlags wait_stage =
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

  VkSubmitInfo submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &d->image_available[slot],
      .pWaitDstStageMask = &wait_stage,
      .commandBufferCount = 1,
      .pCommandBuffers = &d->command_buffers[slot],
      .signalSemaphoreCount = 1,
      .pSignalSemaphores = &d->render_done[slot],
  };
  vkQueueSubmit(d->graphics_queue, 1, &submit, d->in_flight_fences[slot]);

  VkPresentInfoKHR present = {
      .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &d->render_done[slot],
      .swapchainCount = 1,
      .pSwapchains = &d->swapchain,
      .pImageIndices = &d->current_image_index,
  };
  vkQueuePresentKHR(d->graphics_queue, &present);

  // one-shot framebuffer dump on frame 5 (debug/trace level only)
  if (d->current_frame == 5 && rt_log_level >= RT_LOG_LEVEL_TRACE) {
    vkDeviceWaitIdle(d->device);
    VkImage src = d->swapchain_images[d->current_image_index];
    uint32_t w = d->swapchain_extent.width;
    uint32_t h = d->swapchain_extent.height;

    // create a host-visible buffer
    VkBufferCreateInfo buf_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = (VkDeviceSize)w * h * 4,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    VkBuffer readback_buf;
    vkCreateBuffer(d->device, &buf_ci, NULL, &readback_buf);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(d->device, readback_buf, &req);
    uint32_t mt = find_memory_type(d->physical_device, req.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = mt,
    };
    VkDeviceMemory readback_mem;
    vkAllocateMemory(d->device, &ai, NULL, &readback_mem);
    vkBindBufferMemory(d->device, readback_buf, readback_mem, 0);

    // record copy commands
    VkCommandBuffer cb = d->command_buffers[slot];
    VkCommandBufferBeginInfo cbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkResetCommandBuffer(cb, 0);
    vkBeginCommandBuffer(cb, &cbi);

    // transition swapchain image to transfer src
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .image = src,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                         1, &barrier);

    VkBufferImageCopy region = {
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {w, h, 1},
    };
    vkCmdCopyImageToBuffer(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback_buf, 1, &region);

    // transition back
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                         NULL, 1, &barrier);
    vkEndCommandBuffer(cb);

    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb,
    };
    vkQueueSubmit(d->graphics_queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(d->graphics_queue);

    // read pixels
    void *mapped;
    vkMapMemory(d->device, readback_mem, 0, VK_WHOLE_SIZE, 0, &mapped);

    // write PPM (assumes BGRA8 swapchain — swap channels)
    FILE *ppm = fopen("/tmp/frame_dump.ppm", "wb");
    if (ppm) {
      fprintf(ppm, "P6\n%u %u\n255\n", w, h);
      const uint8_t *px = (const uint8_t *)mapped;
      for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
          uint32_t i = (y * w + x) * 4;
          // BGRA → RGB
          uint8_t rgb[3] = {px[i + 2], px[i + 1], px[i + 0]};
          fwrite(rgb, 1, 3, ppm);
        }
      fclose(ppm);
      RT_LOG_DEBUG("gfx", "frame dump: /tmp/frame_dump.ppm (%ux%u)", w, h);
    }

    vkUnmapMemory(d->device, readback_mem);
    vkFreeMemory(d->device, readback_mem, NULL);
    vkDestroyBuffer(d->device, readback_buf, NULL);
  }

  d->current_frame++;
  d->frame_open = false;
}

gfx_pipeline_handle_t
rt_gfx_pipeline_create(gfx_device_handle_t dev, const uint8_t *vs_spv,
                       uint64_t vs_len, const uint8_t *fs_spv, uint64_t fs_len,
                       const uint8_t *refl, uint64_t refl_len) {
  gfx_device_ctx_t *d = dev_from_handle(dev);

  umrf_parsed_t umrf = {0};
  if (!umrf_parse(refl, refl_len, &umrf)) {
    RT_LOG_ERROR("gfx", "pipeline_create: invalid UMRF blob");
    return GFX_NULL_HANDLE;
  }

  VkShaderModuleCreateInfo vs_ci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = (size_t)vs_len,
      .pCode = (const uint32_t *)vs_spv,
  };
  VkShaderModule vs_mod;
  if (vkCreateShaderModule(d->device, &vs_ci, NULL, &vs_mod) != VK_SUCCESS)
    return GFX_NULL_HANDLE;

  VkShaderModuleCreateInfo fs_ci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = (size_t)fs_len,
      .pCode = (const uint32_t *)fs_spv,
  };
  VkShaderModule fs_mod;
  if (vkCreateShaderModule(d->device, &fs_ci, NULL, &fs_mod) != VK_SUCCESS) {
    vkDestroyShaderModule(d->device, vs_mod, NULL);
    return GFX_NULL_HANDLE;
  }

  VkPipelineShaderStageCreateInfo stages[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_VERTEX_BIT,
       .module = vs_mod,
       .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
       .module = fs_mod,
       .pName = "main"},
  };

  VkVertexInputBindingDescription binding_desc = {
      .binding = 0,
      .stride = umrf.binding->stride,
      .inputRate = umrf.binding->input_rate == 0
                       ? VK_VERTEX_INPUT_RATE_VERTEX
                       : VK_VERTEX_INPUT_RATE_INSTANCE,
  };
  VkVertexInputAttributeDescription *attr_descs =
      alloca(umrf.attr_count * sizeof(VkVertexInputAttributeDescription));
  for (uint32_t i = 0; i < umrf.attr_count; i++) {
    attr_descs[i] = (VkVertexInputAttributeDescription){
        .location = umrf.attrs[i].location,
        .binding = 0,
        .format = (VkFormat)umrf.attrs[i].vk_format,
        .offset = umrf.attrs[i].offset,
    };
  }
  VkPipelineVertexInputStateCreateInfo vi = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 1,
      .pVertexBindingDescriptions = &binding_desc,
      .vertexAttributeDescriptionCount = umrf.attr_count,
      .pVertexAttributeDescriptions = attr_descs,
  };
  VkPipelineInputAssemblyStateCreateInfo ia = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
  };
  VkPipelineViewportStateCreateInfo vp_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1,
      .scissorCount = 1,
  };
  VkPipelineRasterizationStateCreateInfo rast = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_BACK_BIT,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .lineWidth = 1.0f,
  };
  VkPipelineMultisampleStateCreateInfo ms = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
  };
  VkPipelineColorBlendAttachmentState blend_att = {
      .blendEnable = VK_TRUE,
      .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
      .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      .colorBlendOp = VK_BLEND_OP_ADD,
      .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
      .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      .alphaBlendOp = VK_BLEND_OP_ADD,
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
  };
  VkPipelineColorBlendStateCreateInfo blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &blend_att,
  };
  VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT,
                                  VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dyn = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = 2,
      .pDynamicStates = dyn_states,
  };
  VkPipelineDepthStencilStateCreateInfo ds = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = VK_TRUE,
      .depthWriteEnable = VK_TRUE,
      .depthCompareOp = VK_COMPARE_OP_LESS,
  };
  VkGraphicsPipelineCreateInfo pipeline_ci = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = 2,
      .pStages = stages,
      .pVertexInputState = &vi,
      .pInputAssemblyState = &ia,
      .pViewportState = &vp_state,
      .pRasterizationState = &rast,
      .pMultisampleState = &ms,
      .pDepthStencilState = d->depth_enabled ? &ds : NULL,
      .pColorBlendState = &blend,
      .pDynamicState = &dyn,
      .layout = d->pipeline_layout,
      .renderPass = d->render_pass,
      .subpass = 0,
  };
  VkPipeline vk_pipe;
  VkResult pr = vkCreateGraphicsPipelines(d->device, VK_NULL_HANDLE, 1,
                                          &pipeline_ci, NULL, &vk_pipe);
  vkDestroyShaderModule(d->device, vs_mod, NULL);
  vkDestroyShaderModule(d->device, fs_mod, NULL);
  if (pr != VK_SUCCESS) return GFX_NULL_HANDLE;

  uint32_t slot = gfx_slot_alloc(d->pipeline_slots, GFX_MAX_PIPELINES);
  if (slot == 0) {
    vkDestroyPipeline(d->device, vk_pipe, NULL);
    return GFX_NULL_HANDLE;
  }
  d->pipeline_table[slot] = vk_pipe;
  return gfx_handle_make(slot, d->pipeline_slots[slot].gen);
}

gfx_pipeline_handle_t rt_gfx_pipeline_create_from_umsh(gfx_device_handle_t dev,
                                                       um_slice_u8_t umsh,
                                                       uint32_t variant_key) {
  (void)variant_key;

  const umsh_header_t *hdr = NULL;
  uint32_t section_count = 0;
  if (!umsh_parse_header(umsh.ptr, umsh.len, &hdr, &section_count)) {
    RT_LOG_ERROR("gfx", "pipeline_create_from_umsh: invalid .umsh header");
    return GFX_NULL_HANDLE;
  }

  umsh_section_view_t stages_view = {0};
  if (!umsh_find_section(umsh.ptr, umsh.len, section_count, UMSH_SECTION_STAGES,
                         &stages_view)) {
    RT_LOG_ERROR("gfx", "pipeline_create_from_umsh: STAGES section not found");
    return GFX_NULL_HANDLE;
  }

  const uint32_t *vs_words = NULL;
  uint32_t vs_word_count = 0;
  const uint32_t *fs_words = NULL;
  uint32_t fs_word_count = 0;
  if (!umsh_find_stage(stages_view, UMSH_STAGE_VERTEX, &vs_words,
                       &vs_word_count) ||
      !umsh_find_stage(stages_view, UMSH_STAGE_FRAGMENT, &fs_words,
                       &fs_word_count)) {
    fprintf(
        stderr,
        "rt_gfx_pipeline_create_from_umsh: missing vertex or fragment stage\n");
    return GFX_NULL_HANDLE;
  }

  // build a minimal UMRF blob from the optional REFL section so we can reuse
  // rt_gfx_pipeline_create's existing pipeline creation logic.
  // if no REFL section, synthesize an empty UMRF (stride=0, no attrs).
  umsh_section_view_t refl_view = {0};
  bool has_refl = umsh_find_section(umsh.ptr, umsh.len, section_count,
                                    UMSH_SECTION_REFL, &refl_view);

  uint32_t stride = 0, input_rate = 0, attr_count = 0;
  if (has_refl && refl_view.size >= 12) {
    memcpy(&stride, refl_view.data + 0, 4);
    memcpy(&input_rate, refl_view.data + 4, 4);
    memcpy(&attr_count, refl_view.data + 8, 4);
    if (refl_view.size < 12 + (uint64_t)attr_count * 12) {
      RT_LOG_ERROR("gfx", "pipeline_create_from_umsh: truncated REFL section");
      return GFX_NULL_HANDLE;
    }
  }

  // synthesize an inline UMRF blob on the stack (header + binding + attrs)
  uint32_t umrf_total = 16 + 8 + 4 + attr_count * 12;
  uint8_t *umrf_buf = (uint8_t *)alloca(umrf_total);
  // umrf_header_t
  uint32_t magic_val = UMRF_MAGIC;
  uint16_t ver = UMRF_VERSION, endian = UMRF_ENDIAN_LE;
  uint32_t umrf_rsv = 0;
  memcpy(umrf_buf + 0, &magic_val, 4);
  memcpy(umrf_buf + 4, &ver, 2);
  memcpy(umrf_buf + 6, &endian, 2);
  memcpy(umrf_buf + 8, &umrf_total, 4);
  memcpy(umrf_buf + 12, &umrf_rsv, 4);
  // umrf_vertex_binding_t
  memcpy(umrf_buf + 16, &stride, 4);
  memcpy(umrf_buf + 20, &input_rate, 4);
  // attr_count + attrs
  memcpy(umrf_buf + 24, &attr_count, 4);
  if (attr_count > 0)
    memcpy(umrf_buf + 28, refl_view.data + 12, (size_t)attr_count * 12);

  gfx_pipeline_handle_t ph = rt_gfx_pipeline_create(
      dev, (const uint8_t *)vs_words, (uint64_t)vs_word_count * 4,
      (const uint8_t *)fs_words, (uint64_t)fs_word_count * 4, umrf_buf,
      (uint64_t)umrf_total);
  RT_LOG_DEBUG("gfx", "pipeline_create_from_umsh: VS=%u words, FS=%u words, handle=0x%lx",
               vs_word_count, fs_word_count, (unsigned long)ph);
  return ph;
}

void rt_gfx_pipeline_destroy(gfx_device_handle_t dev,
                             gfx_pipeline_handle_t pipe) {
  gfx_device_ctx_t *d = dev_from_handle(dev);
  if (!gfx_handle_valid(pipe)) return;
  uint32_t slot = gfx_handle_index(pipe);
  if (slot == 0 || slot >= GFX_MAX_PIPELINES ||
      !d->pipeline_slots[slot].allocated ||
      d->pipeline_slots[slot].gen != gfx_handle_gen(pipe))
    return;
  // wait for all in-flight frames to finish before destroying
  vkDeviceWaitIdle(d->device);
  vkDestroyPipeline(d->device, d->pipeline_table[slot], NULL);
  d->pipeline_table[slot] = VK_NULL_HANDLE;
  gfx_slot_free(d->pipeline_slots, slot);
}

gfx_texture_handle_t rt_gfx_texture2d_create_rgba8(gfx_device_handle_t dev,
                                                   uint32_t w, uint32_t h,
                                                   const uint8_t *rgba,
                                                   uint64_t rgba_len,
                                                   um_slice_u8_t debug_name) {
  gfx_device_ctx_t *d = dev_from_handle(dev);
  RT_LOG_DEBUG("gfx", "texture2d_create: %ux%u, %lu bytes, data=%p",
               w, h, (unsigned long)rgba_len, (void*)rgba);
  assert(rgba != NULL);
  assert(rgba_len == (uint64_t)w * h * 4);

  uint32_t slot = gfx_tex_alloc_slot(&d->resources);
  RT_LOG_TRACE("gfx", "  tex slot=%u", slot);
  if (slot == 0) return GFX_NULL_HANDLE;

  VkImageCreateInfo img_ci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = {.width = w, .height = h, .depth = 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };
  VkImage image;
  if (vkCreateImage(d->device, &img_ci, NULL, &image) != VK_SUCCESS) {
    gfx_tex_free_slot(&d->resources, slot);
    return GFX_NULL_HANDLE;
  }

  VkMemoryRequirements img_req;
  vkGetImageMemoryRequirements(d->device, image, &img_req);
  uint32_t img_mt = find_memory_type(d->physical_device, img_req.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VkMemoryAllocateInfo img_alloc = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = img_req.size,
      .memoryTypeIndex = img_mt,
  };
  VkDeviceMemory image_mem;
  vkAllocateMemory(d->device, &img_alloc, NULL, &image_mem);
  vkBindImageMemory(d->device, image, image_mem, 0);

  VkImageViewCreateInfo view_ci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .baseMipLevel = 0,
                           .levelCount = 1,
                           .baseArrayLayer = 0,
                           .layerCount = 1},
  };
  VkImageView image_view;
  vkCreateImageView(d->device, &view_ci, NULL, &image_view);

  VkBufferCreateInfo stg_ci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = rgba_len,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  VkBuffer stg_buf;
  vkCreateBuffer(d->device, &stg_ci, NULL, &stg_buf);
  VkMemoryRequirements stg_req;
  vkGetBufferMemoryRequirements(d->device, stg_buf, &stg_req);
  uint32_t stg_mt = find_memory_type(d->physical_device, stg_req.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VkMemoryAllocateInfo stg_alloc = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = stg_req.size,
      .memoryTypeIndex = stg_mt,
  };
  VkDeviceMemory stg_mem;
  vkAllocateMemory(d->device, &stg_alloc, NULL, &stg_mem);
  vkBindBufferMemory(d->device, stg_buf, stg_mem, 0);
  void *stg_ptr;
  vkMapMemory(d->device, stg_mem, 0, rgba_len, 0, &stg_ptr);
  memcpy(stg_ptr, rgba, (size_t)rgba_len);
  vkUnmapMemory(d->device, stg_mem);

  VkCommandBuffer tmp = begin_one_time_cmd(d);

  VkImageMemoryBarrier to_transfer = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
      .srcAccessMask = 0,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
  };
  vkCmdPipelineBarrier(tmp, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                       &to_transfer);

  VkBufferImageCopy copy = {
      .bufferOffset = 0,
      .bufferRowLength = 0,
      .bufferImageHeight = 0,
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .imageOffset = {0, 0, 0},
      .imageExtent = {w, h, 1},
  };
  vkCmdCopyBufferToImage(tmp, stg_buf, image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

  VkImageMemoryBarrier to_read = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
  };
  vkCmdPipelineBarrier(tmp, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                       NULL, 1, &to_read);

  end_one_time_cmd(d, tmp);

  vkFreeMemory(d->device, stg_mem, NULL);
  vkDestroyBuffer(d->device, stg_buf, NULL);

  d->resources.textures[slot] = (gfx_texture_entry_t){
      .image = image,
      .memory = image_mem,
      .view = image_view,
      .width = w,
      .height = h,
      .vk_format = VK_FORMAT_R8G8B8A8_UNORM,
  };
  for (uint32_t i = 0; i < d->cfg.frames_in_flight; i++)
    gfx_resources_update_descriptors(&d->resources, d->device,
                                     d->descriptor_sets[i], i);

  const char *dbg = debug_name.len ? (const char *)debug_name.ptr : NULL;
  vk_set_debug_name(d->device, VK_OBJECT_TYPE_IMAGE, (uint64_t)image, dbg);
  vk_set_debug_name(d->device, VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t)image_view,
                    dbg);

  gfx_texture_handle_t ret = gfx_handle_make(slot, d->resources.tex_slots[slot].gen);
  RT_LOG_TRACE("gfx", "  tex handle=0x%lx (lower32=%u)", (unsigned long)ret, (uint32_t)ret);
  return ret;
}

void rt_gfx_texture_destroy(gfx_device_handle_t dev, gfx_texture_handle_t tex) {
  gfx_device_ctx_t *d = dev_from_handle(dev);
  if (!gfx_handle_valid(tex)) return;
  // TODO: validate against d->resources.tex_slots
  gfx_deferred_push(&d->resources, tex, GFX_DEFERRED_TEXTURE,
                    (uint8_t)d->cfg.frames_in_flight);
}

gfx_sampler_handle_t rt_gfx_sampler_create_linear(gfx_device_handle_t dev,
                                                  um_slice_u8_t debug_name) {
  gfx_device_ctx_t *d = dev_from_handle(dev);

  uint32_t slot = gfx_samp_alloc_slot(&d->resources);
  if (slot == 0) return GFX_NULL_HANDLE;

  VkSamplerCreateInfo samp_ci = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR,
      .minFilter = VK_FILTER_LINEAR,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .anisotropyEnable = VK_FALSE,
      .compareEnable = VK_FALSE,
      .minLod = 0.0f,
      .maxLod = VK_LOD_CLAMP_NONE,
      .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
      .unnormalizedCoordinates = VK_FALSE,
  };
  VkSampler sampler;
  if (vkCreateSampler(d->device, &samp_ci, NULL, &sampler) != VK_SUCCESS) {
    gfx_samp_free_slot(&d->resources, slot);
    return GFX_NULL_HANDLE;
  }

  d->resources.samplers[slot] = (gfx_sampler_entry_t){.sampler = sampler};
  for (uint32_t i = 0; i < d->cfg.frames_in_flight; i++)
    gfx_resources_update_descriptors(&d->resources, d->device,
                                     d->descriptor_sets[i], i);

  const char *sdbg = debug_name.len ? (const char *)debug_name.ptr : NULL;
  vk_set_debug_name(d->device, VK_OBJECT_TYPE_SAMPLER, (uint64_t)sampler, sdbg);

  gfx_sampler_handle_t ret = gfx_handle_make(slot, d->resources.samp_slots[slot].gen);
  RT_LOG_DEBUG("gfx", "sampler_create: slot=%u handle=0x%lx (lower32=%u)",
               slot, (unsigned long)ret, (uint32_t)ret);
  return ret;
}

gfx_sampler_handle_t rt_gfx_sampler_create_nearest(gfx_device_handle_t dev,
                                                   um_slice_u8_t debug_name) {
  gfx_device_ctx_t *d = dev_from_handle(dev);

  uint32_t slot = gfx_samp_alloc_slot(&d->resources);
  if (slot == 0) return GFX_NULL_HANDLE;

  VkSamplerCreateInfo samp_ci = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_NEAREST,
      .minFilter = VK_FILTER_NEAREST,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .anisotropyEnable = VK_FALSE,
      .compareEnable = VK_FALSE,
      .minLod = 0.0f,
      .maxLod = VK_LOD_CLAMP_NONE,
      .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
      .unnormalizedCoordinates = VK_FALSE,
  };
  VkSampler sampler;
  if (vkCreateSampler(d->device, &samp_ci, NULL, &sampler) != VK_SUCCESS) {
    gfx_samp_free_slot(&d->resources, slot);
    return GFX_NULL_HANDLE;
  }

  d->resources.samplers[slot] = (gfx_sampler_entry_t){.sampler = sampler};
  for (uint32_t i = 0; i < d->cfg.frames_in_flight; i++)
    gfx_resources_update_descriptors(&d->resources, d->device,
                                     d->descriptor_sets[i], i);

  const char *sdbg = debug_name.len ? (const char *)debug_name.ptr : NULL;
  vk_set_debug_name(d->device, VK_OBJECT_TYPE_SAMPLER, (uint64_t)sampler, sdbg);

  return gfx_handle_make(slot, d->resources.samp_slots[slot].gen);
}

void rt_gfx_sampler_destroy(gfx_device_handle_t dev,
                            gfx_sampler_handle_t samp) {
  gfx_device_ctx_t *d = dev_from_handle(dev);
  if (!gfx_handle_valid(samp)) return;
  gfx_deferred_push(&d->resources, samp, GFX_DEFERRED_SAMPLER,
                    (uint8_t)d->cfg.frames_in_flight);
}

void *rt_gfx_frame_alloc(gfx_device_handle_t dev, uint64_t size, uint64_t align,
                         uint32_t *out_offset) {
  gfx_device_ctx_t *d = dev_from_handle(dev);
  assert(d->frame_open && "rt_gfx_frame_alloc called outside begin/end_frame");
  return gfx_arena_alloc(&d->arena, size, align, out_offset);
}

void rt_gfx_submit_draw_packets(gfx_cmd_handle_t cmd,
                                gfx_pipeline_handle_t pipe,
                                const draw_packet_t *packets,
                                uint32_t packet_count) {
  assert(g_dev_alive && "submit called but device not alive");
  assert(g_dev.frame_open && "submit called outside begin_frame/end_frame");
  assert(packets != NULL && "null packet array");
  assert(packet_count > 0 && "zero packets");
  gfx_device_ctx_t *d = &g_dev;
  uint32_t slot = d->current_frame % d->cfg.frames_in_flight;
  (void)cmd;

  assert(d->draw_packets_used + packet_count <= d->cfg.draw_packets_max &&
         "too many packets");
  // append after previously submitted packets so multiple submits per frame
  // don't clobber each other in the SSBO
  char *dst = (char *)d->draw_packets_mapped[slot] +
              (size_t)d->draw_packets_used * sizeof(draw_packet_t);
  memcpy(dst, packets, sizeof(draw_packet_t) * packet_count);

  uint32_t pipe_slot = gfx_handle_index(pipe);
  assert(pipe_slot > 0 && pipe_slot < GFX_MAX_PIPELINES && "bad pipeline slot");
  assert(d->pipeline_slots[pipe_slot].allocated && "pipeline slot not allocated");
  assert(d->pipeline_table[pipe_slot] != VK_NULL_HANDLE && "pipeline is null");

  RT_LOG_TRACE("gfx", "submit_draw: pipe_slot=%u packets=%u", pipe_slot, packet_count);
  for (uint32_t i = 0; i < packet_count; i++) {
    const draw_packet_t *p_dbg = &packets[i];
    RT_LOG_TRACE("gfx", "  pkt[%u]: verts=%u inst=%u flags=%u tex=%u samp=%u",
                 i, p_dbg->vertex_count, p_dbg->instance_count, p_dbg->flags,
                 p_dbg->tex2d_index, p_dbg->sampler_index);
  }

  vkCmdBindPipeline(d->command_buffers[slot], VK_PIPELINE_BIND_POINT_GRAPHICS,
                    d->pipeline_table[pipe_slot]);

  uint32_t base = d->draw_packets_used;
  for (uint32_t i = 0; i < packet_count; i++) {
    const draw_packet_t *p = &packets[i];
    assert(p->vertex_count > 0 && "zero vertex count");
    assert(p->instance_count > 0 && "zero instance count");
    // firstInstance = base + i so that InstanceIndex (mapped from @draw_id())
    // gives the correct index into the draw_packets SSBO.
    if (p->flags & GFX_DRAW_FLAG_INDEXED) {
      vkCmdDrawIndexed(d->command_buffers[slot], p->index_count,
                       p->instance_count, p->first_index, 0, base + i);
    } else {
      vkCmdDraw(d->command_buffers[slot], p->vertex_count, p->instance_count, 0,
                base + i);
    }
  }
  d->draw_packets_used += packet_count;
}

uint32_t gfx_slot_alloc(gfx_slot_t *table, uint32_t table_len) {
  // TODO: replace linear scan with intrusive free-list
  for (uint32_t i = 1; i < table_len; i++) {
    if (!table[i].allocated) {
      table[i].allocated = true;
      if (table[i].gen == 0) table[i].gen = 1;
      return i;
    }
  }
  return 0;
}

void gfx_slot_free(gfx_slot_t *table, uint32_t index) {
  assert(index != 0 && table[index].allocated);
  table[index].allocated = false;
  if (++table[index].gen == 0)
    table[index].gen = 1; // skip 0 to keep handles non-null
}

bool umrf_parse(const uint8_t *blob, uint64_t blob_len, umrf_parsed_t *out) {
  uint64_t min_size =
      sizeof(umrf_header_t) + sizeof(umrf_vertex_binding_t) + 4u;
  if (!blob || blob_len < min_size) return false;

  const umrf_header_t *h = (const umrf_header_t *)blob;
  if (h->magic != UMRF_MAGIC) return false;
  if (h->endian != UMRF_ENDIAN_LE) return false;
  if (h->version > UMRF_VERSION) return false;

  const umrf_vertex_binding_t *b =
      (const umrf_vertex_binding_t *)(blob + sizeof(umrf_header_t));

  uint32_t attr_count = 0;
  memcpy(&attr_count,
         blob + sizeof(umrf_header_t) + sizeof(umrf_vertex_binding_t), 4);

  uint64_t required =
      min_size + (uint64_t)attr_count * sizeof(umrf_vertex_attr_t);
  if (blob_len < required) return false;

  out->header = h;
  out->binding = b;
  out->attr_count = attr_count;
  out->attrs = (const umrf_vertex_attr_t *)(blob + min_size);
  return true;
}

void gfx_deferred_push(gfx_resource_table_t *rt, uint64_t handle, uint8_t kind,
                       uint8_t frames_remaining) {
  assert(rt->deferred_count < GFX_DEFERRED_QUEUE_MAX);
  uint32_t slot =
      (rt->deferred_head + rt->deferred_count) % GFX_DEFERRED_QUEUE_MAX;
  rt->deferred[slot] = (gfx_deferred_entry_t){
      .handle = handle,
      .kind = kind,
      .frames_remaining = frames_remaining,
  };
  rt->deferred_count++;
}

void gfx_deferred_tick(gfx_resource_table_t *rt, void *vk_device) {
  uint32_t i = 0;
  while (i < rt->deferred_count) {
    uint32_t s = (rt->deferred_head + i) % GFX_DEFERRED_QUEUE_MAX;
    gfx_deferred_entry_t *e = &rt->deferred[s];
    if (e->frames_remaining == 0) {
      VkDevice dev = (VkDevice)vk_device;
      uint32_t idx = gfx_handle_index(e->handle);
      if (e->kind == GFX_DEFERRED_TEXTURE) {
        vkDestroyImageView(dev, (VkImageView)rt->textures[idx].view, NULL);
        vkDestroyImage(dev, (VkImage)rt->textures[idx].image, NULL);
        vkFreeMemory(dev, (VkDeviceMemory)rt->textures[idx].memory, NULL);
        rt->textures[idx] = (gfx_texture_entry_t){0};
        gfx_tex_free_slot(rt, idx);
      } else if (e->kind == GFX_DEFERRED_SAMPLER) {
        vkDestroySampler(dev, (VkSampler)rt->samplers[idx].sampler, NULL);
        rt->samplers[idx] = (gfx_sampler_entry_t){0};
        gfx_samp_free_slot(rt, idx);
      }
      uint32_t last =
          (rt->deferred_head + rt->deferred_count - 1) % GFX_DEFERRED_QUEUE_MAX;
      if (s != last) rt->deferred[s] = rt->deferred[last];
      rt->deferred_count--;
    } else {
      e->frames_remaining--;
      i++;
    }
  }
}

uint32_t gfx_tex_alloc_slot(gfx_resource_table_t *rt) {
  return gfx_slot_alloc(rt->tex_slots, GFX_MAX_TEXTURES);
}
void gfx_tex_free_slot(gfx_resource_table_t *rt, uint32_t slot) {
  gfx_slot_free(rt->tex_slots, slot);
}
uint32_t gfx_samp_alloc_slot(gfx_resource_table_t *rt) {
  return gfx_slot_alloc(rt->samp_slots, GFX_MAX_SAMPLERS);
}
void gfx_samp_free_slot(gfx_resource_table_t *rt, uint32_t slot) {
  gfx_slot_free(rt->samp_slots, slot);
}

void gfx_resources_update_descriptors(gfx_resource_table_t *rt, void *vk_device,
                                      void *vk_descriptor_set,
                                      uint32_t frame_index) {
  (void)frame_index;
  VkDevice dev = (VkDevice)vk_device;
  VkDescriptorSet set = (VkDescriptorSet)vk_descriptor_set;

  // write each allocated texture/sampler at its correct array element (slot index)
  VkWriteDescriptorSet writes[GFX_MAX_TEXTURES + GFX_MAX_SAMPLERS];
  VkDescriptorImageInfo infos[GFX_MAX_TEXTURES + GFX_MAX_SAMPLERS];
  uint32_t write_count = 0;

  for (uint32_t i = 1; i < GFX_MAX_TEXTURES; i++) {
    if (!rt->tex_slots[i].allocated) continue;
    infos[write_count] = (VkDescriptorImageInfo){
        .sampler = VK_NULL_HANDLE,
        .imageView = (VkImageView)rt->textures[i].view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    writes[write_count] = (VkWriteDescriptorSet){
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set,
        .dstBinding = 0,
        .dstArrayElement = i,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        .pImageInfo = &infos[write_count],
    };
    write_count++;
  }
  for (uint32_t i = 1; i < GFX_MAX_SAMPLERS; i++) {
    if (!rt->samp_slots[i].allocated) continue;
    infos[write_count] = (VkDescriptorImageInfo){
        .sampler = (VkSampler)rt->samplers[i].sampler,
        .imageView = VK_NULL_HANDLE,
        .imageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    writes[write_count] = (VkWriteDescriptorSet){
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set,
        .dstBinding = 1,
        .dstArrayElement = i,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
        .pImageInfo = &infos[write_count],
    };
    write_count++;
  }
  if (write_count > 0)
    vkUpdateDescriptorSets(dev, write_count, writes, 0, NULL);
}

void gfx_arena_init(gfx_frame_arena_t *arena, uint64_t frame_arena_bytes,
                    uint32_t frames_in_flight, void *vk_device,
                    void *physical_device) {
  memset(arena, 0, sizeof(*arena));
  arena->frame_size = frame_arena_bytes;
  arena->capacity = frame_arena_bytes * frames_in_flight;
  arena->frames_in_flight = frames_in_flight;
  VkDevice dev = (VkDevice)vk_device;

  VkBufferCreateInfo buf_ci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = arena->capacity,
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  vkCreateBuffer(dev, &buf_ci, NULL, &arena->buffer);

  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(dev, arena->buffer, &req);

  uint32_t chosen_type =
      find_memory_type((VkPhysicalDevice)physical_device, req.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

  VkMemoryAllocateInfo alloc_ci = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex = chosen_type,
  };

  vkAllocateMemory(dev, &alloc_ci, NULL, &arena->memory);
  vkBindBufferMemory(dev, arena->buffer, arena->memory, 0);
  vkMapMemory(dev, arena->memory, 0, arena->capacity, 0, &arena->mapped_ptr);
}

void gfx_arena_next_frame(gfx_frame_arena_t *arena) {
  arena->frame_index = (arena->frame_index + 1) % arena->frames_in_flight;
  arena->frame_base_offset = (uint64_t)arena->frame_index * arena->frame_size;
  arena->head = 0;
}

void *gfx_arena_alloc(gfx_frame_arena_t *arena, uint64_t size, uint64_t align,
                      uint32_t *out_offset) {
  uint64_t mask = align - 1;
  uint64_t aligned = (arena->head + mask) & ~mask;
  if (aligned + size > arena->frame_size) {
    RT_LOG_ERROR("gfx", "frame_alloc: frame arena exhausted");
    *out_offset = 0;
    return NULL;
  }
  arena->head = aligned + size;
  uint64_t abs = arena->frame_base_offset + aligned;
  // offset relative to current frame's slice (descriptor set already binds at frame_base)
  *out_offset = (uint32_t)aligned;
  if (!arena->mapped_ptr) return NULL;
  return (uint8_t *)arena->mapped_ptr + abs;
}

void gfx_arena_destroy(gfx_frame_arena_t *arena, void *vk_device) {
  VkDevice dev = (VkDevice)vk_device;
  if (arena->mapped_ptr) vkUnmapMemory(dev, arena->memory);
  if (arena->memory) vkFreeMemory(dev, arena->memory, NULL);
  if (arena->buffer) vkDestroyBuffer(dev, arena->buffer, NULL);
  memset(arena, 0, sizeof(*arena));
}
