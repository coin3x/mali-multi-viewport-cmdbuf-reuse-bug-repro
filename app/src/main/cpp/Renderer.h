#pragma once

#include <array>
#include <cstdint>
#include <vector>
#include <vulkan/vulkan.h>

struct android_app;

enum class ReproMode : uint32_t {
    // Expected bad on the affected device/driver: poison records vkCmdSetViewport(firstViewport=1, viewportCount=1).
    ViewportIndex1Poison = 0,
    // Expected control: do nothing to the command buffer before recording the actual draw.
    DoNothingControl = 1,
    // Expected control: touches only viewport slot 0.
    ViewportIndex0Control = 2,
};

class Renderer {
public:
    explicit Renderer(android_app* app) : app_(app) {
        initRenderer();
    }
    ~Renderer();

    void handleInput();
    void render();

private:
    void initRenderer();
    void updateRenderArea();

    uint32_t FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags flags);
    VkShaderModule CreateShaderModuleFromAsset(const char* path);
    VkPipeline CreateFullscreenDynamicPipeline();

    void CreateOffscreenTarget();
    void DestroyOffscreenTarget();

    void RecordViewportSlotPoison(VkCommandBuffer cmd);
    void RecordVictimFrame(VkCommandBuffer cmd, uint32_t swapchain_image_index);

    void ImageBarrier(VkCommandBuffer cmd,
                      VkImage image,
                      VkPipelineStageFlags src_stage,
                      VkPipelineStageFlags dst_stage,
                      VkAccessFlags src_access,
                      VkAccessFlags dst_access,
                      VkImageLayout old_layout,
                      VkImageLayout new_layout);

private:
    android_app* app_{};

    // Change this from ViewportIndex1Poison to DoNothingControl/ViewportIndex0Control
    // to see the correct rendering on problematic devices.
    ReproMode repro_mode = ReproMode::ViewportIndex1Poison;

    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = UINT32_MAX;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapchain_format = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchain_extent{};
    std::vector<VkImage> swapchain_images;
    std::vector<VkImageView> swapchain_image_views;

    VkImage offscreen_image = VK_NULL_HANDLE;
    VkDeviceMemory offscreen_memory = VK_NULL_HANDLE;
    VkImageView offscreen_view = VK_NULL_HANDLE;
    VkFramebuffer offscreen_framebuffer = VK_NULL_HANDLE;

    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline dynamic_pipeline = VK_NULL_HANDLE;
    VkShaderModule vertex_shader = VK_NULL_HANDLE;
    VkShaderModule fragment_shader = VK_NULL_HANDLE;

    // The repro requires reusing the same handle.
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;

    static constexpr size_t MAX_IN_FLIGHT_FRAMES = 2;
    std::vector<VkSemaphore> acquire_sems;
    std::vector<VkSemaphore> submit_sems;
    std::vector<VkFence> frame_fences;
    size_t current_frame = 0;

    void updateOverlay(VkPhysicalDeviceProperties props);
};
