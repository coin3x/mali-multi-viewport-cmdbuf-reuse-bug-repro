#include "Renderer.h"

#include <android/asset_manager.h>
#include <android/log.h>
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include <game-activity/native_app_glue/android_native_app_glue.h>
#include <vulkan/vulkan_android.h>

#ifndef LOG_TAG
#define LOG_TAG "Renderer"
#endif

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static inline void VK_CHECK(VkResult result) {
    if (result != VK_SUCCESS) {
        LOGE("Vulkan call failed: %d", static_cast<int>(result));
        throw std::runtime_error("Vulkan call failed");
    }
}

static const char* ReproModeName(ReproMode mode) {
    switch (mode) {
    case ReproMode::ViewportIndex1Poison:
        return "ViewportIndex1Poison";
    case ReproMode::DoNothingControl:
        return "DoNothingControl";
    case ReproMode::ViewportIndex0Control:
        return "ViewportIndex0Control";
    }
    return "Unknown";
}

struct QueueFamilyIndices {
    uint32_t graphics_present = UINT32_MAX;
    bool Complete() const { return graphics_present != UINT32_MAX; }
};

static QueueFamilyIndices FindQueueFamily(VkPhysicalDevice gpu, VkSurfaceKHR surface) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, nullptr);
    std::vector<VkQueueFamilyProperties> props(count);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, props.data());

    QueueFamilyIndices indices{};
    for (uint32_t i = 0; i < count; i++) {
        if ((props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
            continue;
        }
        VkBool32 present_supported = VK_FALSE;
        VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, surface, &present_supported));
        if (present_supported) {
            indices.graphics_present = i;
            break;
        }
    }
    return indices;
}

static VkSurfaceFormatKHR ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) {
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_R8G8B8A8_UNORM ||
            f.format == VK_FORMAT_B8G8R8A8_UNORM ||
            f.format == VK_FORMAT_A8B8G8R8_UNORM_PACK32) {
            return f;
        }
    }
    return formats[0];
}

Renderer::~Renderer() {
    if (device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);
    }

    for (auto fence : frame_fences) {
        if (fence) vkDestroyFence(device, fence, nullptr);
    };
    for (auto sem : acquire_sems) {
        if (sem) vkDestroySemaphore(device, sem, nullptr);
    };
    for (auto sem : submit_sems) {
        if (sem) vkDestroySemaphore(device, sem, nullptr);
    }

    if (command_pool) vkDestroyCommandPool(device, command_pool, nullptr);

    if (dynamic_pipeline) vkDestroyPipeline(device, dynamic_pipeline, nullptr);
    if (pipeline_layout) vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
    if (vertex_shader) vkDestroyShaderModule(device, vertex_shader, nullptr);
    if (fragment_shader) vkDestroyShaderModule(device, fragment_shader, nullptr);

    DestroyOffscreenTarget();

    if (render_pass) vkDestroyRenderPass(device, render_pass, nullptr);

    for (VkImageView view : swapchain_image_views) {
        if (view) vkDestroyImageView(device, view, nullptr);
    }
    if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
    if (device) vkDestroyDevice(device, nullptr);
    if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
    if (instance) vkDestroyInstance(instance, nullptr);
}

void Renderer::render() {
    updateRenderArea();
    auto frame_fence = frame_fences[current_frame];
    VK_CHECK(vkWaitForFences(device, 1, &frame_fence, VK_TRUE, UINT64_MAX));
    VK_CHECK(vkResetFences(device, 1, &frame_fence));

    uint32_t image_index = 0;
    VkResult acquire_result = vkAcquireNextImageKHR(
        device, swapchain, UINT64_MAX, acquire_sems[current_frame], VK_NULL_HANDLE, &image_index);
    if (acquire_result == VK_ERROR_OUT_OF_DATE_KHR || acquire_result == VK_SUBOPTIMAL_KHR) {
        LOGI("Swapchain outdated/suboptimal. Recreate not implemented in minimal repro.");
        return;
    }
    VK_CHECK(acquire_result);

    // Step 1: poison. Record only vkCmdSetViewport into the command buffer and submit it.
    // The command buffer handle is then reset and reused for Step 2 below.
    VK_CHECK(vkResetCommandBuffer(command_buffer, 0));
    RecordViewportSlotPoison(command_buffer);

    const VkSubmitInfo poison_submit{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = 0,
        .pWaitSemaphores = nullptr,
        .pWaitDstStageMask = nullptr,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer,
        .signalSemaphoreCount = 0,
        .pSignalSemaphores = nullptr,
    };
    VK_CHECK(vkQueueSubmit(queue, 1, &poison_submit, frame_fence));
    VK_CHECK(vkWaitForFences(device, 1, &frame_fence, VK_TRUE, UINT64_MAX));
    VK_CHECK(vkResetFences(device, 1, &frame_fence));

    // Step 2: victim. Reuse the same VkCommandBuffer handle.
    VK_CHECK(vkResetCommandBuffer(command_buffer, 0));
    RecordVictimFrame(command_buffer, image_index);

    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    const VkSubmitInfo victim_submit{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &acquire_sems[current_frame],
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &submit_sems[image_index],
    };
    VK_CHECK(vkQueueSubmit(queue, 1, &victim_submit, frame_fence));

    const VkPresentInfoKHR present_info{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = nullptr,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &submit_sems[image_index],
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = &image_index,
        .pResults = nullptr,
    };
    VkResult present_result = vkQueuePresentKHR(queue, &present_info);
    if (present_result == VK_ERROR_OUT_OF_DATE_KHR || present_result == VK_SUBOPTIMAL_KHR) {
        LOGI("Present outdated/suboptimal");
        return;
    }
    VK_CHECK(present_result);
}

void Renderer::initRenderer() {
    // Instance.
    {
        const std::array<const char*, 2> extensions{
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
        };
        const VkApplicationInfo app_info{
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pNext = nullptr,
            .pApplicationName = "repro",
            .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
            .pEngineName = "None",
            .engineVersion = VK_MAKE_VERSION(1, 0, 0),
            .apiVersion = VK_API_VERSION_1_1,
        };
        const VkInstanceCreateInfo create_info{
            .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .pApplicationInfo = &app_info,
            .enabledLayerCount = 0,
            .ppEnabledLayerNames = nullptr,
            .enabledExtensionCount = static_cast<uint32_t>(extensions.size()),
            .ppEnabledExtensionNames = extensions.data(),
        };
        VK_CHECK(vkCreateInstance(&create_info, nullptr, &instance));
    }

    // Surface.
    {
        const VkAndroidSurfaceCreateInfoKHR surface_ci{
            .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
            .pNext = nullptr,
            .flags = 0,
            .window = app_->window,
        };
        VK_CHECK(vkCreateAndroidSurfaceKHR(instance, &surface_ci, nullptr, &surface));
    }

    // Physical device.
    {
        uint32_t count = 0;
        VK_CHECK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        if (count == 0) {
            throw std::runtime_error("No Vulkan physical devices");
        }
        std::vector<VkPhysicalDevice> devices(count);
        VK_CHECK(vkEnumeratePhysicalDevices(instance, &count, devices.data()));

        bool found = false;
        for (VkPhysicalDevice gpu : devices) {
            QueueFamilyIndices indices = FindQueueFamily(gpu, surface);
            if (!indices.Complete()) {
                continue;
            }

            uint32_t ext_count = 0;
            VK_CHECK(vkEnumerateDeviceExtensionProperties(gpu, nullptr, &ext_count, nullptr));
            std::vector<VkExtensionProperties> exts(ext_count);
            VK_CHECK(vkEnumerateDeviceExtensionProperties(gpu, nullptr, &ext_count, exts.data()));
            bool has_swapchain = false;
            for (const auto& ext : exts) {
                if (std::strcmp(ext.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
                    has_swapchain = true;
                    break;
                }
            }
            if (!has_swapchain) {
                continue;
            }

            VkPhysicalDeviceFeatures supported{};
            vkGetPhysicalDeviceFeatures(gpu, &supported);
            if (!supported.multiViewport) {
                LOGI("Skipping GPU without multiViewport support");
                continue;
            }

            physical_device = gpu;
            queue_family = indices.graphics_present;
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(gpu, &props);
            LOGI("Selected GPU: %s vendor=0x%x device=0x%x driver=0x%x maxViewports=%u",
                 props.deviceName,
                 props.vendorID,
                 props.deviceID,
                 props.driverVersion,
                 props.limits.maxViewports);
            updateOverlay(props);
            found = true;
            break;
        }
        if (!found) {
            throw std::runtime_error("No suitable Vulkan physical device with multiViewport");
        }
    }

    // Device.
    {
        const float priority = 1.0f;
        const VkDeviceQueueCreateInfo queue_ci{
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .queueFamilyIndex = queue_family,
            .queueCount = 1,
            .pQueuePriorities = &priority,
        };
        const std::array<const char*, 1> extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkPhysicalDeviceFeatures features{};
        features.multiViewport = VK_TRUE;
        const VkDeviceCreateInfo device_ci{
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &queue_ci,
            .enabledLayerCount = 0,
            .ppEnabledLayerNames = nullptr,
            .enabledExtensionCount = static_cast<uint32_t>(extensions.size()),
            .ppEnabledExtensionNames = extensions.data(),
            .pEnabledFeatures = &features,
        };
        VK_CHECK(vkCreateDevice(physical_device, &device_ci, nullptr, &device));
        vkGetDeviceQueue(device, queue_family, 0, &queue);
    }

    // Swapchain.
    {
        VkSurfaceCapabilitiesKHR caps{};
        VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device, surface, &caps));
        if ((caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0) {
            throw std::runtime_error("Swapchain transfer dst unsupported");
        }

        uint32_t format_count = 0;
        VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &format_count, nullptr));
        std::vector<VkSurfaceFormatKHR> formats(format_count);
        VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &format_count, formats.data()));
        const VkSurfaceFormatKHR chosen_format = ChooseSurfaceFormat(formats);
        swapchain_format = chosen_format.format;

        if (caps.currentExtent.width == UINT32_MAX) {
            throw std::runtime_error("Android surface returned invalid currentExtent");
        }
        swapchain_extent = caps.currentExtent;

        uint32_t image_count = caps.minImageCount + 1;
        if (caps.maxImageCount > 0) {
            image_count = std::min(image_count, caps.maxImageCount);
        }

        const VkSwapchainCreateInfoKHR swapchain_ci{
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .pNext = nullptr,
            .flags = 0,
            .surface = surface,
            .minImageCount = image_count,
            .imageFormat = swapchain_format,
            .imageColorSpace = chosen_format.colorSpace,
            .imageExtent = swapchain_extent,
            .imageArrayLayers = 1,
            .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
            .preTransform = caps.currentTransform,
            .compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
            .presentMode = VK_PRESENT_MODE_FIFO_KHR,
            .clipped = VK_TRUE,
            .oldSwapchain = VK_NULL_HANDLE,
        };
        VK_CHECK(vkCreateSwapchainKHR(device, &swapchain_ci, nullptr, &swapchain));

        uint32_t actual_count = 0;
        VK_CHECK(vkGetSwapchainImagesKHR(device, swapchain, &actual_count, nullptr));
        swapchain_images.resize(actual_count);
        VK_CHECK(vkGetSwapchainImagesKHR(device, swapchain, &actual_count, swapchain_images.data()));

        swapchain_image_views.resize(swapchain_images.size());
        for (size_t i = 0; i < swapchain_images.size(); i++) {
            const VkImageViewCreateInfo view_ci{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .image = swapchain_images[i],
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = swapchain_format,
                .components = {
                    .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                    .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                    .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                    .a = VK_COMPONENT_SWIZZLE_IDENTITY,
                },
                .subresourceRange = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            };
            VK_CHECK(vkCreateImageView(device, &view_ci, nullptr, &swapchain_image_views[i]));
        }
    }

    // Render pass.
    {
        const VkAttachmentDescription color_attachment{
            .flags = 0,
            .format = swapchain_format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        const VkAttachmentReference color_ref{
            .attachment = 0,
            .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        };
        const VkSubpassDescription subpass{
            .flags = 0,
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .inputAttachmentCount = 0,
            .pInputAttachments = nullptr,
            .colorAttachmentCount = 1,
            .pColorAttachments = &color_ref,
            .pResolveAttachments = nullptr,
            .pDepthStencilAttachment = nullptr,
            .preserveAttachmentCount = 0,
            .pPreserveAttachments = nullptr,
        };
        const VkSubpassDependency dependency{
            .srcSubpass = VK_SUBPASS_EXTERNAL,
            .dstSubpass = 0,
            .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dependencyFlags = 0,
        };
        const VkRenderPassCreateInfo rp_ci{
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .attachmentCount = 1,
            .pAttachments = &color_attachment,
            .subpassCount = 1,
            .pSubpasses = &subpass,
            .dependencyCount = 1,
            .pDependencies = &dependency,
        };
        VK_CHECK(vkCreateRenderPass(device, &rp_ci, nullptr, &render_pass));
    }

    CreateOffscreenTarget();

    vertex_shader = CreateShaderModuleFromAsset("shaders/fullscreen.vert.spv");
    fragment_shader = CreateShaderModuleFromAsset("shaders/green.frag.spv");

    {
        const VkPipelineLayoutCreateInfo layout_ci{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .setLayoutCount = 0,
            .pSetLayouts = nullptr,
            .pushConstantRangeCount = 0,
            .pPushConstantRanges = nullptr,
        };
        VK_CHECK(vkCreatePipelineLayout(device, &layout_ci, nullptr, &pipeline_layout));
    }
    dynamic_pipeline = CreateFullscreenDynamicPipeline();

    // Single reusable command buffer.
    {
        const VkCommandPoolCreateInfo pool_ci{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = queue_family,
        };
        VK_CHECK(vkCreateCommandPool(device, &pool_ci, nullptr, &command_pool));

        const VkCommandBufferAllocateInfo alloc_info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .pNext = nullptr,
            .commandPool = command_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        VK_CHECK(vkAllocateCommandBuffers(device, &alloc_info, &command_buffer));
    }

    // Sync.
    {
        const VkSemaphoreCreateInfo sem_ci{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
        };
        acquire_sems.resize(MAX_IN_FLIGHT_FRAMES);
        submit_sems.resize(swapchain_images.size());
        for (auto & acquire_sem : acquire_sems) {
            VK_CHECK(vkCreateSemaphore(device, &sem_ci, nullptr, &acquire_sem));
        }
        for (auto & submit_sem : submit_sems) {
            VK_CHECK(vkCreateSemaphore(device, &sem_ci, nullptr, &submit_sem));
        }

        const VkFenceCreateInfo fence_ci{
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };
        frame_fences.resize(MAX_IN_FLIGHT_FRAMES);
        for (auto& fence:frame_fences) {
            VK_CHECK(vkCreateFence(device, &fence_ci, nullptr, &fence));
        }
    }

    LOGI("Initialized renderer! mode=%s extent=%ux%u format=%d cmd=%p",
         ReproModeName(repro_mode),
         swapchain_extent.width,
         swapchain_extent.height,
         static_cast<int>(swapchain_format),
         command_buffer);
}

void Renderer::updateOverlay(VkPhysicalDeviceProperties props) {
    JNIEnv* env = nullptr;
    auto vm = app_->activity->vm;
    auto res = vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    bool should_detach = false;
    if (res == JNI_EDETACHED) {
        if (vm->AttachCurrentThread(&env, nullptr) != 0) {
            throw std::runtime_error("Failed to attach");
        }
        should_detach = true;
    } else if (res == JNI_OK) {
        //
    } else if (res == JNI_EVERSION) {
        throw std::runtime_error("GetEnv: version not supported");
    }
    auto ins = static_cast<jobject>(app_->activity->javaGameActivity);
    auto activity_cls = env->GetObjectClass(ins);
    auto field = env->GetFieldID(activity_cls, "overlay", "Landroid/widget/TextView;");
    auto text_view = env->GetObjectField(ins, field);
    auto text_view_cls = env->GetObjectClass(text_view);
    auto text_view_cls_setText = env->GetMethodID(text_view_cls, "setText", "(Ljava/lang/CharSequence;)V");

    auto fmt = "Device: %s\nAPI version: 0x%X\nDriver version: 0x%X\nVendor ID: 0x%X\nDevice ID: 0x%X";
    int size = snprintf(nullptr, 0, fmt, props.deviceName, props.apiVersion, props.driverVersion, props.vendorID, props.deviceID);
    std::string s(size, 0);
    snprintf(s.data(), size, fmt, props.deviceName, props.apiVersion, props.driverVersion, props.vendorID, props.deviceID);
    auto str = env->NewStringUTF(s.data());
    env->CallVoidMethod(text_view, text_view_cls_setText, str);
    env->DeleteLocalRef(activity_cls);
    env->DeleteLocalRef(str);
    if (should_detach) {
        vm->DetachCurrentThread();
    }
}

VkPipeline Renderer::CreateFullscreenDynamicPipeline() {
    const VkPipelineShaderStageCreateInfo shader_stages[2]{
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vertex_shader,
            .pName = "main",
            .pSpecializationInfo = nullptr,
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fragment_shader,
            .pName = "main",
            .pSpecializationInfo = nullptr,
        },
    };

    const VkPipelineVertexInputStateCreateInfo vertex_input{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .vertexBindingDescriptionCount = 0,
        .pVertexBindingDescriptions = nullptr,
        .vertexAttributeDescriptionCount = 0,
        .pVertexAttributeDescriptions = nullptr,
    };
    const VkPipelineInputAssemblyStateCreateInfo input_assembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE,
    };

    const VkPipelineViewportStateCreateInfo viewport_state{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .viewportCount = 1,
        .pViewports = nullptr,
        .scissorCount = 1,
        .pScissors = nullptr,
    };
    const VkPipelineRasterizationStateCreateInfo rasterization{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable = VK_FALSE,
        .depthBiasConstantFactor = 0.0f,
        .depthBiasClamp = 0.0f,
        .depthBiasSlopeFactor = 0.0f,
        .lineWidth = 1.0f,
    };
    const VkPipelineMultisampleStateCreateInfo multisample{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        .sampleShadingEnable = VK_FALSE,
        .minSampleShading = 0.0f,
        .pSampleMask = nullptr,
        .alphaToCoverageEnable = VK_FALSE,
        .alphaToOneEnable = VK_FALSE,
    };
    const VkPipelineColorBlendAttachmentState color_blend_attachment{
        .blendEnable = VK_FALSE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                          VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT |
                          VK_COLOR_COMPONENT_A_BIT,
    };
    const VkPipelineColorBlendStateCreateInfo color_blend{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = 1,
        .pAttachments = &color_blend_attachment,
        .blendConstants = {0.0f, 0.0f, 0.0f, 0.0f},
    };
    const std::array<VkDynamicState, 2> dynamic_states{
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    const VkPipelineDynamicStateCreateInfo dynamic_state{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .dynamicStateCount = static_cast<uint32_t>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };
    const VkGraphicsPipelineCreateInfo pipeline_ci{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stageCount = 2,
        .pStages = shader_stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pTessellationState = nullptr,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &multisample,
        .pDepthStencilState = nullptr,
        .pColorBlendState = &color_blend,
        .pDynamicState = &dynamic_state,
        .layout = pipeline_layout,
        .renderPass = render_pass,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = 0,
    };
    VkPipeline result = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &result));
    return result;
}

void Renderer::RecordViewportSlotPoison(VkCommandBuffer cmd) {
    const VkCommandBufferBeginInfo begin_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    VK_CHECK(vkBeginCommandBuffer(cmd, &begin_info));

    const VkViewport viewport{
        .x = 0.0f,
        .y = 0.0f,
        .width = 1024.0f,
        .height = 1024.0f,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };

    if (repro_mode == ReproMode::ViewportIndex1Poison) {
        vkCmdSetViewport(cmd, 1, 1, &viewport);
    } else if (repro_mode == ReproMode::ViewportIndex0Control) {
        vkCmdSetViewport(cmd, 0, 1, &viewport);
    }
    VK_CHECK(vkEndCommandBuffer(cmd));
}

void Renderer::RecordVictimFrame(VkCommandBuffer cmd, uint32_t swapchain_image_index) {
    const VkCommandBufferBeginInfo begin_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    VK_CHECK(vkBeginCommandBuffer(cmd, &begin_info));

    const VkRenderPassBeginInfo rp_begin{
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .pNext = nullptr,
        .renderPass = render_pass,
        .framebuffer = offscreen_framebuffer,
        .renderArea = {.offset = {0, 0}, .extent = swapchain_extent},
        .clearValueCount = 0,
        .pClearValues = nullptr,
    };

    const VkClearAttachment magenta_clear{
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .colorAttachment = 0,
        .clearValue = {.color = {.float32 = {1.0f, 0.0f, 1.0f, 1.0f}}},
    };
    const VkClearAttachment green_clear{
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .colorAttachment = 0,
        .clearValue = {.color = {.float32 = {0.0f, 1.0f, 0.0f, 1.0f}}},
    };
    const VkClearRect full_rect{
        .rect = {.offset = {0, 0}, .extent = swapchain_extent},
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
    const VkClearRect small_rect{
        .rect = {.offset = {0, 0}, .extent = {.width = 128, .height = 128}},
        .baseArrayLayer = 0,
        .layerCount = 1,
    };

    vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdClearAttachments(cmd, 1, &magenta_clear, 1, &full_rect);

    const VkViewport viewport{
        .x = 0.0f,
        .y = 0.0f,
        .width = static_cast<float>(swapchain_extent.width),
        .height = static_cast<float>(swapchain_extent.height),
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    const VkRect2D scissor{
        .offset = {0, 0},
        .extent = swapchain_extent,
    };

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, dynamic_pipeline);
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    // If the draw is invisible/bad, the frame remains magenta except this small green marker.
    vkCmdClearAttachments(cmd, 1, &green_clear, 1, &small_rect);
    vkCmdEndRenderPass(cmd);

    ImageBarrier(cmd,
                 offscreen_image,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_ACCESS_TRANSFER_READ_BIT,
                 VK_IMAGE_LAYOUT_GENERAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    ImageBarrier(cmd,
                 swapchain_images[swapchain_image_index],
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    const VkImageBlit blit{
        .srcSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .srcOffsets = {
            {0, 0, 0},
            {static_cast<int32_t>(swapchain_extent.width), static_cast<int32_t>(swapchain_extent.height), 1},
        },
        .dstSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .dstOffsets = {
            {0, 0, 0},
            {static_cast<int32_t>(swapchain_extent.width), static_cast<int32_t>(swapchain_extent.height), 1},
        },
    };
    vkCmdBlitImage(cmd,
                   offscreen_image,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   swapchain_images[swapchain_image_index],
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1,
                   &blit,
                   VK_FILTER_NEAREST);

    ImageBarrier(cmd,
                 swapchain_images[swapchain_image_index],
                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                 VK_ACCESS_TRANSFER_WRITE_BIT,
                 0,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    VK_CHECK(vkEndCommandBuffer(cmd));
}

void Renderer::CreateOffscreenTarget() {
    const VkImageCreateInfo image_ci{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = swapchain_format,
        .extent = {.width = swapchain_extent.width, .height = swapchain_extent.height, .depth = 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VK_CHECK(vkCreateImage(device, &image_ci, nullptr, &offscreen_image));

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device, offscreen_image, &req);
    const VkMemoryAllocateInfo alloc_info{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = nullptr,
        .allocationSize = req.size,
        .memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    VK_CHECK(vkAllocateMemory(device, &alloc_info, nullptr, &offscreen_memory));
    VK_CHECK(vkBindImageMemory(device, offscreen_image, offscreen_memory, 0));

    const VkImageViewCreateInfo view_ci{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = offscreen_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = swapchain_format,
        .components = {
            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
            .a = VK_COMPONENT_SWIZZLE_IDENTITY,
        },
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    VK_CHECK(vkCreateImageView(device, &view_ci, nullptr, &offscreen_view));

    const VkImageView attachment = offscreen_view;
    const VkFramebufferCreateInfo fb_ci{
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .renderPass = render_pass,
        .attachmentCount = 1,
        .pAttachments = &attachment,
        .width = swapchain_extent.width,
        .height = swapchain_extent.height,
        .layers = 1,
    };
    VK_CHECK(vkCreateFramebuffer(device, &fb_ci, nullptr, &offscreen_framebuffer));
}

void Renderer::DestroyOffscreenTarget() {
    if (offscreen_framebuffer) {
        vkDestroyFramebuffer(device, offscreen_framebuffer, nullptr);
        offscreen_framebuffer = VK_NULL_HANDLE;
    }
    if (offscreen_view) {
        vkDestroyImageView(device, offscreen_view, nullptr);
        offscreen_view = VK_NULL_HANDLE;
    }
    if (offscreen_image) {
        vkDestroyImage(device, offscreen_image, nullptr);
        offscreen_image = VK_NULL_HANDLE;
    }
    if (offscreen_memory) {
        vkFreeMemory(device, offscreen_memory, nullptr);
        offscreen_memory = VK_NULL_HANDLE;
    }
}

void Renderer::ImageBarrier(VkCommandBuffer cmd,
                            VkImage image,
                            VkPipelineStageFlags src_stage,
                            VkPipelineStageFlags dst_stage,
                            VkAccessFlags src_access,
                            VkAccessFlags dst_access,
                            VkImageLayout old_layout,
                            VkImageLayout new_layout) {
    const VkImageMemoryBarrier barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void Renderer::updateRenderArea() {}

void Renderer::handleInput() {
    auto* input_buffer = android_app_swap_input_buffers(app_);
    if (!input_buffer) {
        return;
    }
    android_app_clear_motion_events(input_buffer);
    android_app_clear_key_events(input_buffer);
}

uint32_t Renderer::FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties mem_props{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        const bool type_ok = (type_bits & (1u << i)) != 0;
        const bool flags_ok = (mem_props.memoryTypes[i].propertyFlags & flags) == flags;
        if (type_ok && flags_ok) {
            return i;
        }
    }
    throw std::runtime_error("No suitable memory type");
}

VkShaderModule Renderer::CreateShaderModuleFromAsset(const char* path) {
    AAsset* file = AAssetManager_open(app_->activity->assetManager, path, AASSET_MODE_BUFFER);
    if (!file) {
        throw std::runtime_error("Failed to open shader asset");
    }

    const size_t spv_len = AAsset_getLength(file);
    if (spv_len == 0 || (spv_len % 4) != 0) {
        AAsset_close(file);
        throw std::runtime_error("Invalid SPIR-V size");
    }

    std::vector<uint32_t> spv(spv_len / sizeof(uint32_t));
    const int read_bytes = AAsset_read(file, spv.data(), spv_len);
    AAsset_close(file);
    if (read_bytes != static_cast<int>(spv_len)) {
        throw std::runtime_error("Failed to read full SPIR-V asset");
    }

    const VkShaderModuleCreateInfo shader_ci{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .codeSize = spv_len,
        .pCode = spv.data(),
    };
    VkShaderModule module = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(device, &shader_ci, nullptr, &module));
    return module;
}
