#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES

#include "preview_vulkan_renderer.h"
#include "preview_vulkan_shaders.generated.h"

#include <Windows.h>
#include <DirectXMath.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace native_mc {
namespace {

using namespace DirectX;

constexpr float kPi = 3.14159265358979323846f;
constexpr std::uint32_t kMaximumFramesInFlight = 3;

struct RenderVertex {
    float position[3];
    float normal[3];
    std::uint8_t color[4];
};

static_assert(sizeof(RenderVertex) == 28, "Unexpected Vulkan model vertex packing");

struct ColorVertex {
    float position[3];
    std::uint8_t color[4];
};

static_assert(sizeof(ColorVertex) == 16, "Unexpected Vulkan color vertex packing");

struct PushConstants {
    XMFLOAT4X4 worldViewProjection;
    XMFLOAT4 normalColumn0;
    XMFLOAT4 normalColumn1;
    XMFLOAT4 normalColumn2;
    XMFLOAT4 lightDirectionAndAmbient;
};

static_assert(sizeof(PushConstants) == 128, "Vulkan push constants must fit the core 128-byte minimum");

void SetError(std::string* output, const std::string& message) {
    if (output) *output = message;
}

const char* VkResultName(VkResult result) {
    switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_EVENT_SET: return "VK_EVENT_SET";
    case VK_EVENT_RESET: return "VK_EVENT_RESET";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    default: return "unknown VkResult";
    }
}

std::string VkError(const char* operation, VkResult result) {
    char buffer[160]{};
    std::snprintf(buffer, sizeof(buffer), "%s failed (%s, %ld)", operation,
                  VkResultName(result), static_cast<long>(result));
    return buffer;
}

struct VulkanDispatch {
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
    PFN_vkCreateInstance CreateInstance = nullptr;
    PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties = nullptr;

    PFN_vkDestroyInstance DestroyInstance = nullptr;
    PFN_vkCreateWin32SurfaceKHR CreateWin32SurfaceKHR = nullptr;
    PFN_vkDestroySurfaceKHR DestroySurfaceKHR = nullptr;
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceFeatures GetPhysicalDeviceFeatures = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR GetPhysicalDeviceSurfaceSupportKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR GetPhysicalDeviceSurfaceFormatsKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR GetPhysicalDeviceSurfacePresentModesKHR = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties = nullptr;
    PFN_vkCreateDevice CreateDevice = nullptr;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;

    PFN_vkDestroyDevice DestroyDevice = nullptr;
    PFN_vkGetDeviceQueue GetDeviceQueue = nullptr;
    PFN_vkDeviceWaitIdle DeviceWaitIdle = nullptr;
    PFN_vkQueueWaitIdle QueueWaitIdle = nullptr;
    PFN_vkQueueSubmit QueueSubmit = nullptr;
    PFN_vkQueuePresentKHR QueuePresentKHR = nullptr;
    PFN_vkCreateSwapchainKHR CreateSwapchainKHR = nullptr;
    PFN_vkDestroySwapchainKHR DestroySwapchainKHR = nullptr;
    PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR = nullptr;
    PFN_vkAcquireNextImageKHR AcquireNextImageKHR = nullptr;
    PFN_vkCreateImageView CreateImageView = nullptr;
    PFN_vkDestroyImageView DestroyImageView = nullptr;
    PFN_vkCreateRenderPass CreateRenderPass = nullptr;
    PFN_vkDestroyRenderPass DestroyRenderPass = nullptr;
    PFN_vkCreateFramebuffer CreateFramebuffer = nullptr;
    PFN_vkDestroyFramebuffer DestroyFramebuffer = nullptr;
    PFN_vkCreateShaderModule CreateShaderModule = nullptr;
    PFN_vkDestroyShaderModule DestroyShaderModule = nullptr;
    PFN_vkCreatePipelineLayout CreatePipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout DestroyPipelineLayout = nullptr;
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines = nullptr;
    PFN_vkDestroyPipeline DestroyPipeline = nullptr;
    PFN_vkCreateCommandPool CreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool DestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
    PFN_vkFreeCommandBuffers FreeCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;
    PFN_vkResetCommandBuffer ResetCommandBuffer = nullptr;
    PFN_vkCmdBeginRenderPass CmdBeginRenderPass = nullptr;
    PFN_vkCmdEndRenderPass CmdEndRenderPass = nullptr;
    PFN_vkCmdSetViewport CmdSetViewport = nullptr;
    PFN_vkCmdSetScissor CmdSetScissor = nullptr;
    PFN_vkCmdBindPipeline CmdBindPipeline = nullptr;
    PFN_vkCmdPushConstants CmdPushConstants = nullptr;
    PFN_vkCmdBindVertexBuffers CmdBindVertexBuffers = nullptr;
    PFN_vkCmdBindIndexBuffer CmdBindIndexBuffer = nullptr;
    PFN_vkCmdDraw CmdDraw = nullptr;
    PFN_vkCmdDrawIndexed CmdDrawIndexed = nullptr;
    PFN_vkCmdCopyBuffer CmdCopyBuffer = nullptr;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
    PFN_vkCreateBuffer CreateBuffer = nullptr;
    PFN_vkDestroyBuffer DestroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;
    PFN_vkAllocateMemory AllocateMemory = nullptr;
    PFN_vkFreeMemory FreeMemory = nullptr;
    PFN_vkBindBufferMemory BindBufferMemory = nullptr;
    PFN_vkMapMemory MapMemory = nullptr;
    PFN_vkUnmapMemory UnmapMemory = nullptr;
    PFN_vkCreateImage CreateImage = nullptr;
    PFN_vkDestroyImage DestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
    PFN_vkBindImageMemory BindImageMemory = nullptr;
    PFN_vkCreateSemaphore CreateSemaphore = nullptr;
    PFN_vkDestroySemaphore DestroySemaphore = nullptr;
    PFN_vkCreateFence CreateFence = nullptr;
    PFN_vkDestroyFence DestroyFence = nullptr;
    PFN_vkWaitForFences WaitForFences = nullptr;
    PFN_vkResetFences ResetFences = nullptr;

    template <typename T>
    bool LoadGlobal(T* output, const char* name) const {
        *output = reinterpret_cast<T>(GetInstanceProcAddr(VK_NULL_HANDLE, name));
        return *output != nullptr;
    }

    template <typename T>
    bool LoadInstance(VkInstance instance, T* output, const char* name) const {
        *output = reinterpret_cast<T>(GetInstanceProcAddr(instance, name));
        return *output != nullptr;
    }

    template <typename T>
    bool LoadDevice(VkDevice device, T* output, const char* name) const {
        *output = reinterpret_cast<T>(GetDeviceProcAddr(device, name));
        return *output != nullptr;
    }

    bool LoadGlobals(std::string* errorText) {
        if (!GetInstanceProcAddr ||
            !LoadGlobal(&CreateInstance, "vkCreateInstance") ||
            !LoadGlobal(&EnumerateInstanceExtensionProperties, "vkEnumerateInstanceExtensionProperties")) {
            SetError(errorText, "Vulkan loader is missing required global entry points");
            return false;
        }
        return true;
    }

    bool LoadInstanceFunctions(VkInstance instance, std::string* errorText) {
#define LOAD_INSTANCE(name) LoadInstance(instance, &name, "vk" #name)
        const bool ok =
            LOAD_INSTANCE(DestroyInstance) &&
            LOAD_INSTANCE(CreateWin32SurfaceKHR) &&
            LOAD_INSTANCE(DestroySurfaceKHR) &&
            LOAD_INSTANCE(EnumeratePhysicalDevices) &&
            LOAD_INSTANCE(GetPhysicalDeviceProperties) &&
            LOAD_INSTANCE(GetPhysicalDeviceFeatures) &&
            LOAD_INSTANCE(GetPhysicalDeviceQueueFamilyProperties) &&
            LOAD_INSTANCE(GetPhysicalDeviceMemoryProperties) &&
            LOAD_INSTANCE(GetPhysicalDeviceFormatProperties) &&
            LOAD_INSTANCE(GetPhysicalDeviceSurfaceSupportKHR) &&
            LOAD_INSTANCE(GetPhysicalDeviceSurfaceCapabilitiesKHR) &&
            LOAD_INSTANCE(GetPhysicalDeviceSurfaceFormatsKHR) &&
            LOAD_INSTANCE(GetPhysicalDeviceSurfacePresentModesKHR) &&
            LOAD_INSTANCE(EnumerateDeviceExtensionProperties) &&
            LOAD_INSTANCE(CreateDevice) &&
            LOAD_INSTANCE(GetDeviceProcAddr);
#undef LOAD_INSTANCE
        if (!ok) SetError(errorText, "Vulkan instance is missing required Win32 or device entry points");
        return ok;
    }

    bool LoadDeviceFunctions(VkDevice device, std::string* errorText) {
#define LOAD_DEVICE(name) LoadDevice(device, &name, "vk" #name)
        const bool ok =
            LOAD_DEVICE(DestroyDevice) && LOAD_DEVICE(GetDeviceQueue) &&
            LOAD_DEVICE(DeviceWaitIdle) && LOAD_DEVICE(QueueWaitIdle) &&
            LOAD_DEVICE(QueueSubmit) && LOAD_DEVICE(QueuePresentKHR) &&
            LOAD_DEVICE(CreateSwapchainKHR) && LOAD_DEVICE(DestroySwapchainKHR) &&
            LOAD_DEVICE(GetSwapchainImagesKHR) && LOAD_DEVICE(AcquireNextImageKHR) &&
            LOAD_DEVICE(CreateImageView) && LOAD_DEVICE(DestroyImageView) &&
            LOAD_DEVICE(CreateRenderPass) && LOAD_DEVICE(DestroyRenderPass) &&
            LOAD_DEVICE(CreateFramebuffer) && LOAD_DEVICE(DestroyFramebuffer) &&
            LOAD_DEVICE(CreateShaderModule) && LOAD_DEVICE(DestroyShaderModule) &&
            LOAD_DEVICE(CreatePipelineLayout) && LOAD_DEVICE(DestroyPipelineLayout) &&
            LOAD_DEVICE(CreateGraphicsPipelines) && LOAD_DEVICE(DestroyPipeline) &&
            LOAD_DEVICE(CreateCommandPool) && LOAD_DEVICE(DestroyCommandPool) &&
            LOAD_DEVICE(AllocateCommandBuffers) && LOAD_DEVICE(FreeCommandBuffers) &&
            LOAD_DEVICE(BeginCommandBuffer) && LOAD_DEVICE(EndCommandBuffer) &&
            LOAD_DEVICE(ResetCommandBuffer) && LOAD_DEVICE(CmdBeginRenderPass) &&
            LOAD_DEVICE(CmdEndRenderPass) && LOAD_DEVICE(CmdSetViewport) &&
            LOAD_DEVICE(CmdSetScissor) && LOAD_DEVICE(CmdBindPipeline) &&
            LOAD_DEVICE(CmdPushConstants) && LOAD_DEVICE(CmdBindVertexBuffers) &&
            LOAD_DEVICE(CmdBindIndexBuffer) && LOAD_DEVICE(CmdDraw) &&
            LOAD_DEVICE(CmdDrawIndexed) && LOAD_DEVICE(CmdCopyBuffer) &&
            LOAD_DEVICE(CmdPipelineBarrier) &&
            LOAD_DEVICE(CreateBuffer) && LOAD_DEVICE(DestroyBuffer) &&
            LOAD_DEVICE(GetBufferMemoryRequirements) && LOAD_DEVICE(AllocateMemory) &&
            LOAD_DEVICE(FreeMemory) && LOAD_DEVICE(BindBufferMemory) &&
            LOAD_DEVICE(MapMemory) && LOAD_DEVICE(UnmapMemory) &&
            LOAD_DEVICE(CreateImage) && LOAD_DEVICE(DestroyImage) &&
            LOAD_DEVICE(GetImageMemoryRequirements) && LOAD_DEVICE(BindImageMemory) &&
            LOAD_DEVICE(CreateSemaphore) && LOAD_DEVICE(DestroySemaphore) &&
            LOAD_DEVICE(CreateFence) && LOAD_DEVICE(DestroyFence) &&
            LOAD_DEVICE(WaitForFences) && LOAD_DEVICE(ResetFences);
#undef LOAD_DEVICE
        if (!ok) SetError(errorText, "Vulkan device is missing required rendering entry points");
        return ok;
    }
};

struct GpuBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
};

struct DepthTarget {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

struct FrameSync {
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
};

struct QueueFamilySelection {
    std::uint32_t graphics = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t present = std::numeric_limits<std::uint32_t>::max();
};

struct PendingUploadBatch {
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    std::vector<GpuBuffer> stagingBuffers;
    std::vector<GpuBuffer> destinationBuffers;
};

struct DeviceBufferUpload {
    const void* data = nullptr;
    std::size_t byteCount = 0;
    VkBufferUsageFlags finalUsage = 0;
    GpuBuffer* output = nullptr;
};

XMMATRIX BuildViewProjection(const PreviewVulkanCamera& camera, float aspect) {
    const float yaw = camera.yawDegrees * kPi / 180.0f;
    const float pitch = std::clamp(camera.pitchDegrees, -89.0f, 89.0f) * kPi / 180.0f;
    const float distance = std::max(0.01f, camera.distance);
    const XMVECTOR target = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
    const float horizontal = std::cos(pitch) * distance;
    const XMVECTOR offset = XMVectorSet(std::sin(yaw) * horizontal,
                                        std::sin(pitch) * distance,
                                        -std::cos(yaw) * horizontal, 0.0f);
    const XMVECTOR eye = XMVectorAdd(target, offset);
    const XMMATRIX view = XMMatrixLookAtRH(eye, target, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
    const XMMATRIX pan = XMMatrixTranslation(camera.panX, camera.panY, 0.0f);
    const float nearPlane = std::max(0.001f, camera.nearPlane);
    const float farPlane = std::max(nearPlane + 1.0f, camera.farPlane);
    const float fov = std::clamp(camera.fieldOfViewDegrees, 5.0f, 175.0f) * kPi / 180.0f;
    return XMMatrixMultiply(XMMatrixMultiply(view, pan), XMMatrixPerspectiveFovRH(
        fov, std::max(0.01f, aspect), nearPlane, farPlane));
}

XMMATRIX BuildModelMatrix(const PreviewVulkanModelTransform& transform) {
    XMVECTOR sourceQuaternion = XMVectorSet(transform.sourceQuaternion[1], transform.sourceQuaternion[2],
                                            transform.sourceQuaternion[3], transform.sourceQuaternion[0]);
    const float sourceLengthSquared = XMVectorGetX(XMVector4LengthSq(sourceQuaternion));
    sourceQuaternion = sourceLengthSquared > 1.0e-12f
        ? XMQuaternionNormalize(sourceQuaternion) : XMQuaternionIdentity();
    XMVECTOR quaternion = XMVectorSet(transform.quaternion[1], transform.quaternion[2],
                                      transform.quaternion[3], transform.quaternion[0]);
    const float lengthSquared = XMVectorGetX(XMVector4LengthSq(quaternion));
    quaternion = lengthSquared > 1.0e-12f ? XMQuaternionNormalize(quaternion) : XMQuaternionIdentity();
    const XMMATRIX center = XMMatrixTranslation(-transform.center[0], -transform.center[1], -transform.center[2]);
    const XMMATRIX sourceRotation = XMMatrixRotationQuaternion(sourceQuaternion);
    const XMMATRIX scale = XMMatrixScaling(transform.scale[0], transform.scale[1], transform.scale[2]);
    const XMMATRIX rotation = XMMatrixRotationQuaternion(quaternion);
    const XMMATRIX translation = XMMatrixTranslation(transform.position[0], transform.position[1],
                                                       transform.position[2]);
    return XMMatrixMultiply(
        XMMatrixMultiply(XMMatrixMultiply(XMMatrixMultiply(center, sourceRotation), scale), rotation),
        translation);
}

PushConstants MakePushConstants(const XMMATRIX& world, const XMMATRIX& viewProjection) {
    PushConstants constants{};
    XMVECTOR determinant{};
    const XMMATRIX inverseWorld = XMMatrixInverse(&determinant, world);
    const XMMATRIX normalWorld = std::abs(XMVectorGetX(determinant)) > 1.0e-12f
        ? XMMatrixTranspose(inverseWorld) : world;
    XMFLOAT4X4 normalValues{};
    XMStoreFloat4x4(&normalValues, normalWorld);
    XMStoreFloat4x4(&constants.worldViewProjection, XMMatrixMultiply(world, viewProjection));
    constants.normalColumn0 = XMFLOAT4(normalValues._11, normalValues._21, normalValues._31, 0.0f);
    constants.normalColumn1 = XMFLOAT4(normalValues._12, normalValues._22, normalValues._32, 0.0f);
    constants.normalColumn2 = XMFLOAT4(normalValues._13, normalValues._23, normalValues._33, 0.0f);
    constants.lightDirectionAndAmbient = XMFLOAT4(0.4f, 1.0f, 0.6f, 0.22f);
    return constants;
}

}  // namespace

struct PreviewVulkanRenderer::Impl {
    HWND window = nullptr;
    PreviewVulkanOptions options{};
    HMODULE loader = nullptr;
    VulkanDispatch vk{};
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties physicalProperties{};
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    VkPhysicalDeviceFeatures physicalFeatures{};
    VkDevice device = VK_NULL_HANDLE;
    std::uint32_t graphicsQueueFamily = 0;
    std::uint32_t presentQueueFamily = 0;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue presentQueue = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    bool hardwareDevice = false;
    bool wireframeSupported = false;
    bool largePointsSupported = false;
    bool operational = false;
    std::string adapterName;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainImageViews;
    std::vector<DepthTarget> depthTargets;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkFence> imagesInFlight;
    std::vector<VkSemaphore> renderFinishedSemaphores;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline modelSolidPipeline = VK_NULL_HANDLE;
    VkPipeline modelWirePipeline = VK_NULL_HANDLE;
    VkPipeline colorTrianglePipeline = VK_NULL_HANDLE;
    VkPipeline colorPointPipeline = VK_NULL_HANDLE;
    VkPipeline colorLinePipeline = VK_NULL_HANDLE;
    VkPipeline colorOverlayLinePipeline = VK_NULL_HANDLE;

    std::vector<FrameSync> frames;
    std::size_t currentFrame = 0;
    std::uint64_t frameNumber = 0;

    GpuBuffer modelVertexBuffer;
    std::array<GpuBuffer, 4> modelIndexBuffers{};
    std::array<bool, 4> modelIndexBufferOwned{};
    std::array<std::uint32_t, 4> modelIndexCounts{};
    GpuBuffer worldVertexBuffer;
    GpuBuffer worldIndexBuffer;
    std::uint32_t worldVertexCount = 0;
    std::uint32_t worldIndexCount = 0;
    PreviewVulkanWorldPrimitive worldPrimitive = PreviewVulkanWorldPrimitive::Quads;
    std::vector<GpuBuffer> overlayVertexBuffers;
    std::vector<std::size_t> overlayVertexCapacities;
    std::optional<PendingUploadBatch> pendingUpload;

    bool LoadVulkan(std::string* errorText) {
        wchar_t systemDirectory[MAX_PATH]{};
        const UINT length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) {
            SetError(errorText, "Could not resolve the Windows system directory for Vulkan");
            return false;
        }
        std::wstring loaderPath(systemDirectory, length);
        loaderPath += L"\\vulkan-1.dll";
        loader = LoadLibraryExW(loaderPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!loader) loader = LoadLibraryW(loaderPath.c_str());
        if (!loader) {
            SetError(errorText, "Vulkan loader was not found in the Windows system directory");
            return false;
        }
        vk.GetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            GetProcAddress(loader, "vkGetInstanceProcAddr"));
        return vk.LoadGlobals(errorText);
    }

    bool HasInstanceExtension(const char* required, std::string* errorText) {
        std::uint32_t count = 0;
        VkResult result = vk.EnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
        if (result != VK_SUCCESS) {
            SetError(errorText, VkError("vkEnumerateInstanceExtensionProperties", result));
            return false;
        }
        std::vector<VkExtensionProperties> properties(count);
        result = vk.EnumerateInstanceExtensionProperties(nullptr, &count, properties.data());
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
            SetError(errorText, VkError("vkEnumerateInstanceExtensionProperties", result));
            return false;
        }
        return std::any_of(properties.begin(), properties.begin() + count,
                           [required](const VkExtensionProperties& property) {
                               return std::strcmp(property.extensionName, required) == 0;
                           });
    }

    bool CreateVulkanInstance(std::string* errorText) {
        if (!HasInstanceExtension(VK_KHR_SURFACE_EXTENSION_NAME, errorText) ||
            !HasInstanceExtension(VK_KHR_WIN32_SURFACE_EXTENSION_NAME, errorText)) {
            if (!errorText || errorText->empty()) {
                SetError(errorText, "Vulkan loader does not expose the required Win32 surface extensions");
            }
            return false;
        }
        constexpr std::array<const char*, 2> extensions = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
        };
        VkApplicationInfo application{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
        application.pApplicationName = "3dmodel-to-minecraft preview";
        application.applicationVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
        application.pEngineName = "native_mc preview";
        application.engineVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
        application.apiVersion = VK_API_VERSION_1_0;

        VkInstanceCreateInfo createInfo{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        createInfo.pApplicationInfo = &application;
        createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();
        const VkResult result = vk.CreateInstance(&createInfo, nullptr, &instance);
        if (result != VK_SUCCESS) {
            SetError(errorText, VkError("vkCreateInstance", result));
            return false;
        }
        return vk.LoadInstanceFunctions(instance, errorText);
    }

    bool CreateSurface(std::string* errorText) {
        VkWin32SurfaceCreateInfoKHR createInfo{ VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
        createInfo.hinstance = GetModuleHandleW(nullptr);
        createInfo.hwnd = window;
        const VkResult result = vk.CreateWin32SurfaceKHR(instance, &createInfo, nullptr, &surface);
        if (result != VK_SUCCESS) {
            SetError(errorText, VkError("vkCreateWin32SurfaceKHR", result));
            return false;
        }
        return true;
    }

    bool HasDeviceExtension(VkPhysicalDevice candidate, const char* required) const {
        std::uint32_t count = 0;
        if (vk.EnumerateDeviceExtensionProperties(candidate, nullptr, &count, nullptr) != VK_SUCCESS) return false;
        std::vector<VkExtensionProperties> properties(count);
        const VkResult result = vk.EnumerateDeviceExtensionProperties(
            candidate, nullptr, &count, properties.data());
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) return false;
        return std::any_of(properties.begin(), properties.begin() + count,
                           [required](const VkExtensionProperties& property) {
                               return std::strcmp(property.extensionName, required) == 0;
                           });
    }

    std::optional<QueueFamilySelection> FindGraphicsPresentQueues(
        VkPhysicalDevice candidate) const {
        std::uint32_t count = 0;
        vk.GetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
        std::vector<VkQueueFamilyProperties> properties(count);
        vk.GetPhysicalDeviceQueueFamilyProperties(candidate, &count, properties.data());
        std::optional<std::uint32_t> firstGraphics;
        std::optional<std::uint32_t> firstPresent;
        for (std::uint32_t index = 0; index < count; ++index) {
            if (properties[index].queueCount == 0) continue;
            const bool graphics = (properties[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
            if (graphics && !firstGraphics) firstGraphics = index;
            VkBool32 present = VK_FALSE;
            if (vk.GetPhysicalDeviceSurfaceSupportKHR(candidate, index, surface, &present) == VK_SUCCESS &&
                present == VK_TRUE) {
                if (!firstPresent) firstPresent = index;
                if (graphics) return QueueFamilySelection{ index, index };
            }
        }
        if (firstGraphics && firstPresent) {
            return QueueFamilySelection{ *firstGraphics, *firstPresent };
        }
        return std::nullopt;
    }

    bool HasUsableSwapchain(VkPhysicalDevice candidate) const {
        std::uint32_t formatCount = 0;
        std::uint32_t presentModeCount = 0;
        return vk.GetPhysicalDeviceSurfaceFormatsKHR(candidate, surface, &formatCount, nullptr) == VK_SUCCESS &&
            formatCount > 0 &&
            vk.GetPhysicalDeviceSurfacePresentModesKHR(candidate, surface, &presentModeCount, nullptr) == VK_SUCCESS &&
            presentModeCount > 0;
    }

    bool PickPhysicalDevice(std::string* errorText) {
        std::uint32_t count = 0;
        VkResult result = vk.EnumeratePhysicalDevices(instance, &count, nullptr);
        if (result != VK_SUCCESS || count == 0) {
            SetError(errorText, result == VK_SUCCESS
                ? "Vulkan did not enumerate a physical device"
                : VkError("vkEnumeratePhysicalDevices", result));
            return false;
        }
        std::vector<VkPhysicalDevice> devices(count);
        result = vk.EnumeratePhysicalDevices(instance, &count, devices.data());
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
            SetError(errorText, VkError("vkEnumeratePhysicalDevices", result));
            return false;
        }

        int bestScore = std::numeric_limits<int>::min();
        std::optional<QueueFamilySelection> bestQueues;
        for (std::uint32_t index = 0; index < count; ++index) {
            const VkPhysicalDevice candidate = devices[index];
            if (!HasDeviceExtension(candidate, VK_KHR_SWAPCHAIN_EXTENSION_NAME) ||
                !HasUsableSwapchain(candidate)) continue;
            const auto queues = FindGraphicsPresentQueues(candidate);
            if (!queues) continue;
            VkPhysicalDeviceProperties properties{};
            vk.GetPhysicalDeviceProperties(candidate, &properties);
            int score = 100;
            if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                score += options.preferDiscreteGpu ? 1000 : 300;
            } else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
                score += options.preferDiscreteGpu ? 500 : 800;
            } else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU) {
                score += 100;
            }
            score += static_cast<int>(properties.limits.maxImageDimension2D / 1024);
            if (score > bestScore) {
                bestScore = score;
                physicalDevice = candidate;
                physicalProperties = properties;
                bestQueues = queues;
            }
        }
        if (physicalDevice == VK_NULL_HANDLE || !bestQueues) {
            SetError(errorText, "No Vulkan device supports graphics, Win32 presentation, and VK_KHR_swapchain");
            return false;
        }
        graphicsQueueFamily = bestQueues->graphics;
        presentQueueFamily = bestQueues->present;
        vk.GetPhysicalDeviceFeatures(physicalDevice, &physicalFeatures);
        vk.GetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
        adapterName = physicalProperties.deviceName;
        hardwareDevice = physicalProperties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU;
        wireframeSupported = physicalFeatures.fillModeNonSolid == VK_TRUE;
        largePointsSupported = physicalFeatures.largePoints == VK_TRUE;
        return true;
    }

    bool CreateLogicalDevice(std::string* errorText) {
        const float queuePriority = 1.0f;
        std::array<VkDeviceQueueCreateInfo, 2> queueInfos{};
        queueInfos[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfos[0].queueFamilyIndex = graphicsQueueFamily;
        queueInfos[0].queueCount = 1;
        queueInfos[0].pQueuePriorities = &queuePriority;
        std::uint32_t queueInfoCount = 1;
        if (presentQueueFamily != graphicsQueueFamily) {
            queueInfos[1].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueInfos[1].queueFamilyIndex = presentQueueFamily;
            queueInfos[1].queueCount = 1;
            queueInfos[1].pQueuePriorities = &queuePriority;
            queueInfoCount = 2;
        }

        VkPhysicalDeviceFeatures enabledFeatures{};
        enabledFeatures.fillModeNonSolid = wireframeSupported ? VK_TRUE : VK_FALSE;
        enabledFeatures.largePoints = largePointsSupported ? VK_TRUE : VK_FALSE;
        constexpr const char* extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        VkDeviceCreateInfo createInfo{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        createInfo.queueCreateInfoCount = queueInfoCount;
        createInfo.pQueueCreateInfos = queueInfos.data();
        createInfo.enabledExtensionCount = 1;
        createInfo.ppEnabledExtensionNames = extensions;
        createInfo.pEnabledFeatures = &enabledFeatures;
        const VkResult result = vk.CreateDevice(physicalDevice, &createInfo, nullptr, &device);
        if (result != VK_SUCCESS) {
            SetError(errorText, VkError("vkCreateDevice", result));
            return false;
        }
        if (!vk.LoadDeviceFunctions(device, errorText)) return false;
        vk.GetDeviceQueue(device, graphicsQueueFamily, 0, &graphicsQueue);
        vk.GetDeviceQueue(device, presentQueueFamily, 0, &presentQueue);
        if (graphicsQueue == VK_NULL_HANDLE || presentQueue == VK_NULL_HANDLE) {
            SetError(errorText, "Vulkan did not return the requested graphics and presentation queues");
            return false;
        }
        return true;
    }

    bool CreateCommandResources(std::string* errorText) {
        VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
                         VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = graphicsQueueFamily;
        VkResult result = vk.CreateCommandPool(device, &poolInfo, nullptr, &commandPool);
        if (result != VK_SUCCESS) {
            SetError(errorText, VkError("vkCreateCommandPool", result));
            return false;
        }

        const std::uint32_t frameCount = std::clamp(
            options.framesInFlight, 1u, kMaximumFramesInFlight);
        frames.resize(frameCount);
        overlayVertexBuffers.resize(frameCount);
        overlayVertexCapacities.assign(frameCount, 0);
        std::vector<VkCommandBuffer> commandBuffers(frameCount);
        VkCommandBufferAllocateInfo allocateInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        allocateInfo.commandPool = commandPool;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = frameCount;
        result = vk.AllocateCommandBuffers(device, &allocateInfo, commandBuffers.data());
        if (result != VK_SUCCESS) {
            SetError(errorText, VkError("vkAllocateCommandBuffers", result));
            return false;
        }
        for (std::size_t index = 0; index < frames.size(); ++index) {
            frames[index].commandBuffer = commandBuffers[index];
            VkSemaphoreCreateInfo semaphoreInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            result = vk.CreateSemaphore(device, &semaphoreInfo, nullptr, &frames[index].imageAvailable);
            if (result != VK_SUCCESS) {
                SetError(errorText, VkError("vkCreateSemaphore(image available)", result));
                return false;
            }
            VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            result = vk.CreateFence(device, &fenceInfo, nullptr, &frames[index].fence);
            if (result != VK_SUCCESS) {
                SetError(errorText, VkError("vkCreateFence", result));
                return false;
            }
        }
        return true;
    }

    void DestroyBuffer(GpuBuffer* buffer) {
        if (!buffer || device == VK_NULL_HANDLE) return;
        if (buffer->buffer != VK_NULL_HANDLE) vk.DestroyBuffer(device, buffer->buffer, nullptr);
        if (buffer->memory != VK_NULL_HANDLE) vk.FreeMemory(device, buffer->memory, nullptr);
        *buffer = {};
    }

    std::optional<std::uint32_t> FindMemoryType(
        std::uint32_t typeBits, VkMemoryPropertyFlags required) const {
        for (std::uint32_t index = 0; index < memoryProperties.memoryTypeCount; ++index) {
            if ((typeBits & (1u << index)) != 0 &&
                (memoryProperties.memoryTypes[index].propertyFlags & required) == required) {
                return index;
            }
        }
        return std::nullopt;
    }

    bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags memoryFlags, GpuBuffer* output,
                      std::string* errorText) {
        if (!output || size == 0) {
            SetError(errorText, "Vulkan buffer size must be non-zero");
            return false;
        }
        GpuBuffer created{};
        created.size = size;
        VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufferInfo.size = size;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkResult result = vk.CreateBuffer(device, &bufferInfo, nullptr, &created.buffer);
        if (result != VK_SUCCESS) {
            SetError(errorText, VkError("vkCreateBuffer", result));
            return false;
        }
        VkMemoryRequirements requirements{};
        vk.GetBufferMemoryRequirements(device, created.buffer, &requirements);
        const auto memoryType = FindMemoryType(requirements.memoryTypeBits, memoryFlags);
        if (!memoryType) {
            vk.DestroyBuffer(device, created.buffer, nullptr);
            SetError(errorText, "No Vulkan memory type satisfies the requested buffer properties");
            return false;
        }
        VkMemoryAllocateInfo allocateInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocateInfo.allocationSize = requirements.size;
        allocateInfo.memoryTypeIndex = *memoryType;
        result = vk.AllocateMemory(device, &allocateInfo, nullptr, &created.memory);
        if (result != VK_SUCCESS) {
            vk.DestroyBuffer(device, created.buffer, nullptr);
            SetError(errorText, VkError("vkAllocateMemory(buffer)", result));
            return false;
        }
        result = vk.BindBufferMemory(device, created.buffer, created.memory, 0);
        if (result != VK_SUCCESS) {
            vk.FreeMemory(device, created.memory, nullptr);
            vk.DestroyBuffer(device, created.buffer, nullptr);
            SetError(errorText, VkError("vkBindBufferMemory", result));
            return false;
        }
        *output = created;
        return true;
    }

    bool WriteHostBuffer(const GpuBuffer& buffer, const void* data, std::size_t byteCount,
                         std::string* errorText) {
        if (!data || byteCount == 0 || byteCount > buffer.size) {
            SetError(errorText, "Vulkan host buffer update exceeds the allocation");
            return false;
        }
        void* mapped = nullptr;
        const VkResult result = vk.MapMemory(device, buffer.memory, 0,
                                              static_cast<VkDeviceSize>(byteCount), 0, &mapped);
        if (result != VK_SUCCESS) {
            SetError(errorText, VkError("vkMapMemory", result));
            return false;
        }
        std::memcpy(mapped, data, byteCount);
        vk.UnmapMemory(device, buffer.memory);
        return true;
    }

    bool CreateDeviceBuffers(const std::vector<DeviceBufferUpload>& uploads,
                             std::string* errorText) {
        if (uploads.empty() || pendingUpload) {
            SetError(errorText, pendingUpload
                ? "A previous Vulkan upload is still pending"
                : "Vulkan upload batch is empty");
            return false;
        }
        std::vector<GpuBuffer> stagingBuffers(uploads.size());
        std::vector<GpuBuffer> destinationBuffers(uploads.size());
        auto destroyBuffers = [&]() {
            for (GpuBuffer& buffer : stagingBuffers) DestroyBuffer(&buffer);
            for (GpuBuffer& buffer : destinationBuffers) DestroyBuffer(&buffer);
        };
        for (std::size_t index = 0; index < uploads.size(); ++index) {
            const DeviceBufferUpload& upload = uploads[index];
            if (!upload.data || upload.byteCount == 0 || !upload.output) {
                destroyBuffers();
                SetError(errorText, "Vulkan upload data is empty");
                return false;
            }
            const VkDeviceSize size = static_cast<VkDeviceSize>(upload.byteCount);
            if (!CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              &stagingBuffers[index], errorText) ||
                !WriteHostBuffer(stagingBuffers[index], upload.data,
                                 upload.byteCount, errorText) ||
                !CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT | upload.finalUsage,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                              &destinationBuffers[index], errorText)) {
                destroyBuffers();
                return false;
            }
        }

        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo allocateInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        allocateInfo.commandPool = commandPool;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = 1;
        VkResult result = vk.AllocateCommandBuffers(device, &allocateInfo, &commandBuffer);
        if (result != VK_SUCCESS) {
            destroyBuffers();
            SetError(errorText, VkError("vkAllocateCommandBuffers(upload)", result));
            return false;
        }
        VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vk.BeginCommandBuffer(commandBuffer, &beginInfo);
        if (result == VK_SUCCESS) {
            for (std::size_t index = 0; index < uploads.size(); ++index) {
                const VkDeviceSize size = static_cast<VkDeviceSize>(uploads[index].byteCount);
                const VkBufferCopy region{ 0, 0, size };
                vk.CmdCopyBuffer(commandBuffer, stagingBuffers[index].buffer,
                                 destinationBuffers[index].buffer, 1, &region);
                VkBufferMemoryBarrier barrier{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                barrier.dstAccessMask =
                    (uploads[index].finalUsage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) != 0
                        ? VK_ACCESS_INDEX_READ_BIT : VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.buffer = destinationBuffers[index].buffer;
                barrier.offset = 0;
                barrier.size = size;
                vk.CmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                      VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0,
                                      0, nullptr, 1, &barrier, 0, nullptr);
            }
            result = vk.EndCommandBuffer(commandBuffer);
        }
        VkFence fence = VK_NULL_HANDLE;
        if (result == VK_SUCCESS) {
            VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            result = vk.CreateFence(device, &fenceInfo, nullptr, &fence);
        }
        bool submissionAttempted = false;
        bool submitted = false;
        if (result == VK_SUCCESS) {
            VkSubmitInfo submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &commandBuffer;
            submissionAttempted = true;
            result = vk.QueueSubmit(graphicsQueue, 1, &submitInfo, fence);
            submitted = result == VK_SUCCESS;
        }
        if (submitted) {
            result = vk.WaitForFences(device, 1, &fence, VK_TRUE,
                                      std::numeric_limits<std::uint64_t>::max());
        }
        if (result != VK_SUCCESS) {
            if (submissionAttempted) {
                operational = false;
                PendingUploadBatch abandoned{};
                abandoned.commandBuffer = commandBuffer;
                abandoned.fence = fence;
                abandoned.stagingBuffers = std::move(stagingBuffers);
                abandoned.destinationBuffers = std::move(destinationBuffers);
                pendingUpload.emplace(std::move(abandoned));
            } else {
                if (fence != VK_NULL_HANDLE) vk.DestroyFence(device, fence, nullptr);
                vk.FreeCommandBuffers(device, commandPool, 1, &commandBuffer);
                destroyBuffers();
            }
            SetError(errorText, VkError(
                submitted ? "vkWaitForFences(upload)" :
                submissionAttempted ? "vkQueueSubmit(upload)" :
                "Vulkan staging buffer preparation", result));
            return false;
        }
        vk.DestroyFence(device, fence, nullptr);
        vk.FreeCommandBuffers(device, commandPool, 1, &commandBuffer);
        for (GpuBuffer& staging : stagingBuffers) DestroyBuffer(&staging);
        for (std::size_t index = 0; index < uploads.size(); ++index) {
            *uploads[index].output = destinationBuffers[index];
            destinationBuffers[index] = {};
        }
        return true;
    }

    VkFormat FindDepthFormat() const {
        constexpr std::array<VkFormat, 3> candidates = {
            VK_FORMAT_D32_SFLOAT,
            VK_FORMAT_D24_UNORM_S8_UINT,
            VK_FORMAT_D16_UNORM,
        };
        for (const VkFormat candidate : candidates) {
            VkFormatProperties properties{};
            vk.GetPhysicalDeviceFormatProperties(physicalDevice, candidate, &properties);
            if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
                return candidate;
            }
        }
        return VK_FORMAT_UNDEFINED;
    }

    bool CreateDepthTarget(DepthTarget* output, std::string* errorText) {
        DepthTarget target{};
        VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = { extent.width, extent.height, 1 };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = depthFormat;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkResult result = vk.CreateImage(device, &imageInfo, nullptr, &target.image);
        if (result != VK_SUCCESS) {
            SetError(errorText, VkError("vkCreateImage(depth)", result));
            return false;
        }
        VkMemoryRequirements requirements{};
        vk.GetImageMemoryRequirements(device, target.image, &requirements);
        const auto memoryType = FindMemoryType(requirements.memoryTypeBits,
                                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!memoryType) {
            vk.DestroyImage(device, target.image, nullptr);
            SetError(errorText, "No device-local Vulkan memory type is available for the depth target");
            return false;
        }
        VkMemoryAllocateInfo allocateInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocateInfo.allocationSize = requirements.size;
        allocateInfo.memoryTypeIndex = *memoryType;
        result = vk.AllocateMemory(device, &allocateInfo, nullptr, &target.memory);
        if (result == VK_SUCCESS) result = vk.BindImageMemory(device, target.image, target.memory, 0);
        if (result != VK_SUCCESS) {
            if (target.memory) vk.FreeMemory(device, target.memory, nullptr);
            vk.DestroyImage(device, target.image, nullptr);
            SetError(errorText, VkError("Vulkan depth memory allocation", result));
            return false;
        }
        VkImageViewCreateInfo viewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        viewInfo.image = target.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = depthFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        result = vk.CreateImageView(device, &viewInfo, nullptr, &target.view);
        if (result != VK_SUCCESS) {
            vk.DestroyImage(device, target.image, nullptr);
            vk.FreeMemory(device, target.memory, nullptr);
            SetError(errorText, VkError("vkCreateImageView(depth)", result));
            return false;
        }
        *output = target;
        return true;
    }

    void DestroyDepthTarget(DepthTarget* target) {
        if (!target || device == VK_NULL_HANDLE) return;
        if (target->view) vk.DestroyImageView(device, target->view, nullptr);
        if (target->image) vk.DestroyImage(device, target->image, nullptr);
        if (target->memory) vk.FreeMemory(device, target->memory, nullptr);
        *target = {};
    }

    bool CreateRenderPass(std::string* errorText) {
        VkAttachmentDescription color{};
        color.format = swapchainFormat;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentDescription depth{};
        depth.format = depthFormat;
        depth.samples = VK_SAMPLE_COUNT_1_BIT;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        const std::array<VkAttachmentDescription, 2> attachments = { color, depth };
        VkAttachmentReference colorReference{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkAttachmentReference depthReference{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorReference;
        subpass.pDepthStencilAttachment = &depthReference;

        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstStageMask = dependency.srcStageMask;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo createInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        createInfo.attachmentCount = static_cast<std::uint32_t>(attachments.size());
        createInfo.pAttachments = attachments.data();
        createInfo.subpassCount = 1;
        createInfo.pSubpasses = &subpass;
        createInfo.dependencyCount = 1;
        createInfo.pDependencies = &dependency;
        const VkResult result = vk.CreateRenderPass(device, &createInfo, nullptr, &renderPass);
        if (result != VK_SUCCESS) {
            SetError(errorText, VkError("vkCreateRenderPass", result));
            return false;
        }
        return true;
    }

    bool CreateShaderModule(const std::uint32_t* code, std::size_t byteCount,
                            VkShaderModule* output, std::string* errorText) {
        VkShaderModuleCreateInfo createInfo{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        createInfo.codeSize = byteCount;
        createInfo.pCode = code;
        const VkResult result = vk.CreateShaderModule(device, &createInfo, nullptr, output);
        if (result != VK_SUCCESS) {
            SetError(errorText, VkError("vkCreateShaderModule", result));
            return false;
        }
        return true;
    }

    bool CreateGraphicsPipeline(bool modelVertices, VkPrimitiveTopology topology,
                                VkPolygonMode polygonMode, bool depthEnabled,
                                VkShaderModule vertexModule, const char* vertexEntry,
                                VkShaderModule pixelModule, const char* pixelEntry,
                                VkPipeline* output, std::string* errorText) {
        const std::array<VkPipelineShaderStageCreateInfo, 2> stages = {{
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
              VK_SHADER_STAGE_VERTEX_BIT, vertexModule, vertexEntry, nullptr },
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
              VK_SHADER_STAGE_FRAGMENT_BIT, pixelModule, pixelEntry, nullptr },
        }};

        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = modelVertices ? sizeof(RenderVertex) : sizeof(ColorVertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        std::array<VkVertexInputAttributeDescription, 3> attributes{};
        attributes[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 };
        std::uint32_t attributeCount = 0;
        if (modelVertices) {
            attributes[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12 };
            attributes[2] = { 2, 0, VK_FORMAT_R8G8B8A8_UNORM, 24 };
            attributeCount = 3;
        } else {
            attributes[1] = { 1, 0, VK_FORMAT_R8G8B8A8_UNORM, 12 };
            attributeCount = 2;
        }
        VkPipelineVertexInputStateCreateInfo vertexInput{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
        };
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = attributeCount;
        vertexInput.pVertexAttributeDescriptions = attributes.data();

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO
        };
        inputAssembly.topology = topology;
        inputAssembly.primitiveRestartEnable = VK_FALSE;

        VkPipelineViewportStateCreateInfo viewport{
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO
        };
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterization{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO
        };
        rasterization.depthClampEnable = VK_FALSE;
        rasterization.rasterizerDiscardEnable = VK_FALSE;
        rasterization.polygonMode = polygonMode;
        rasterization.cullMode = VK_CULL_MODE_NONE;
        rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterization.depthBiasEnable = polygonMode == VK_POLYGON_MODE_LINE ? VK_TRUE : VK_FALSE;
        rasterization.depthBiasConstantFactor = polygonMode == VK_POLYGON_MODE_LINE ? -1.0f : 0.0f;
        rasterization.depthBiasSlopeFactor = polygonMode == VK_POLYGON_MODE_LINE ? -0.5f : 0.0f;
        rasterization.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO
        };
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depth{
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO
        };
        depth.depthTestEnable = depthEnabled ? VK_TRUE : VK_FALSE;
        depth.depthWriteEnable = depthEnabled ? VK_TRUE : VK_FALSE;
        depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.blendEnable = VK_TRUE;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO
        };
        blend.attachmentCount = 1;
        blend.pAttachments = &blendAttachment;

        constexpr std::array<VkDynamicState, 2> dynamicStates = {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR,
        };
        VkPipelineDynamicStateCreateInfo dynamic{
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO
        };
        dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
        dynamic.pDynamicStates = dynamicStates.data();

        VkGraphicsPipelineCreateInfo createInfo{
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO
        };
        createInfo.stageCount = static_cast<std::uint32_t>(stages.size());
        createInfo.pStages = stages.data();
        createInfo.pVertexInputState = &vertexInput;
        createInfo.pInputAssemblyState = &inputAssembly;
        createInfo.pViewportState = &viewport;
        createInfo.pRasterizationState = &rasterization;
        createInfo.pMultisampleState = &multisample;
        createInfo.pDepthStencilState = &depth;
        createInfo.pColorBlendState = &blend;
        createInfo.pDynamicState = &dynamic;
        createInfo.layout = pipelineLayout;
        createInfo.renderPass = renderPass;
        createInfo.subpass = 0;
        const VkResult result = vk.CreateGraphicsPipelines(
            device, VK_NULL_HANDLE, 1, &createInfo, nullptr, output);
        if (result != VK_SUCCESS) {
            SetError(errorText, VkError("vkCreateGraphicsPipelines", result));
            return false;
        }
        return true;
    }

    bool CreatePipelines(std::string* errorText) {
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.offset = 0;
        pushRange.size = sizeof(PushConstants);
        VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
        VkResult result = vk.CreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout);
        if (result != VK_SUCCESS) {
            SetError(errorText, VkError("vkCreatePipelineLayout", result));
            return false;
        }

        VkShaderModule modelVertex = VK_NULL_HANDLE;
        VkShaderModule colorVertex = VK_NULL_HANDLE;
        VkShaderModule pointVertex = VK_NULL_HANDLE;
        VkShaderModule pointFallbackVertex = VK_NULL_HANDLE;
        VkShaderModule litPixel = VK_NULL_HANDLE;
        VkShaderModule colorPixel = VK_NULL_HANDLE;
        auto destroyModules = [&]() {
            if (modelVertex) vk.DestroyShaderModule(device, modelVertex, nullptr);
            if (colorVertex) vk.DestroyShaderModule(device, colorVertex, nullptr);
            if (pointVertex) vk.DestroyShaderModule(device, pointVertex, nullptr);
            if (pointFallbackVertex) vk.DestroyShaderModule(device, pointFallbackVertex, nullptr);
            if (litPixel) vk.DestroyShaderModule(device, litPixel, nullptr);
            if (colorPixel) vk.DestroyShaderModule(device, colorPixel, nullptr);
        };
        if (!CreateShaderModule(vulkan_shaders::kModelVertexShader,
                                sizeof(vulkan_shaders::kModelVertexShader), &modelVertex, errorText) ||
            !CreateShaderModule(vulkan_shaders::kColorVertexShader,
                                sizeof(vulkan_shaders::kColorVertexShader), &colorVertex, errorText) ||
            !CreateShaderModule(vulkan_shaders::kPointVertexShader,
                                sizeof(vulkan_shaders::kPointVertexShader), &pointVertex, errorText) ||
            !CreateShaderModule(vulkan_shaders::kPointFallbackVertexShader,
                                sizeof(vulkan_shaders::kPointFallbackVertexShader),
                                &pointFallbackVertex, errorText) ||
            !CreateShaderModule(vulkan_shaders::kLitPixelShader,
                                sizeof(vulkan_shaders::kLitPixelShader), &litPixel, errorText) ||
            !CreateShaderModule(vulkan_shaders::kColorPixelShader,
                                sizeof(vulkan_shaders::kColorPixelShader), &colorPixel, errorText)) {
            destroyModules();
            return false;
        }

        bool ok = CreateGraphicsPipeline(
            true, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, true,
            modelVertex, "ModelVS", litPixel, "LitPS", &modelSolidPipeline, errorText);
        if (ok && wireframeSupported) {
            ok = CreateGraphicsPipeline(
                true, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_LINE, true,
                modelVertex, "ModelVS", colorPixel, "ColorPS", &modelWirePipeline, errorText);
        }
        if (ok) {
            ok = CreateGraphicsPipeline(
                false, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, true,
                colorVertex, "ColorVS", colorPixel, "ColorPS", &colorTrianglePipeline, errorText);
        }
        if (ok) {
            ok = CreateGraphicsPipeline(
                false, VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_POLYGON_MODE_FILL, true,
                largePointsSupported ? pointVertex : pointFallbackVertex,
                largePointsSupported ? "PointVS" : "PointFallbackVS",
                colorPixel, "ColorPS", &colorPointPipeline, errorText);
        }
        if (ok) {
            ok = CreateGraphicsPipeline(
                false, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_POLYGON_MODE_FILL, true,
                colorVertex, "ColorVS", colorPixel, "ColorPS", &colorLinePipeline, errorText);
        }
        if (ok) {
            ok = CreateGraphicsPipeline(
                false, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_POLYGON_MODE_FILL, false,
                colorVertex, "ColorVS", colorPixel, "ColorPS", &colorOverlayLinePipeline, errorText);
        }
        destroyModules();
        return ok;
    }

    void DestroySwapchainResources() {
        if (device == VK_NULL_HANDLE) return;
        for (VkFramebuffer framebuffer : framebuffers) {
            if (framebuffer) vk.DestroyFramebuffer(device, framebuffer, nullptr);
        }
        framebuffers.clear();
        for (DepthTarget& target : depthTargets) DestroyDepthTarget(&target);
        depthTargets.clear();
        if (modelSolidPipeline) vk.DestroyPipeline(device, modelSolidPipeline, nullptr);
        if (modelWirePipeline) vk.DestroyPipeline(device, modelWirePipeline, nullptr);
        if (colorTrianglePipeline) vk.DestroyPipeline(device, colorTrianglePipeline, nullptr);
        if (colorPointPipeline) vk.DestroyPipeline(device, colorPointPipeline, nullptr);
        if (colorLinePipeline) vk.DestroyPipeline(device, colorLinePipeline, nullptr);
        if (colorOverlayLinePipeline) vk.DestroyPipeline(device, colorOverlayLinePipeline, nullptr);
        modelSolidPipeline = VK_NULL_HANDLE;
        modelWirePipeline = VK_NULL_HANDLE;
        colorTrianglePipeline = VK_NULL_HANDLE;
        colorPointPipeline = VK_NULL_HANDLE;
        colorLinePipeline = VK_NULL_HANDLE;
        colorOverlayLinePipeline = VK_NULL_HANDLE;
        if (pipelineLayout) vk.DestroyPipelineLayout(device, pipelineLayout, nullptr);
        pipelineLayout = VK_NULL_HANDLE;
        if (renderPass) vk.DestroyRenderPass(device, renderPass, nullptr);
        renderPass = VK_NULL_HANDLE;
        for (VkImageView view : swapchainImageViews) {
            if (view) vk.DestroyImageView(device, view, nullptr);
        }
        swapchainImageViews.clear();
        swapchainImages.clear();
        imagesInFlight.clear();
        for (VkSemaphore semaphore : renderFinishedSemaphores) {
            if (semaphore) vk.DestroySemaphore(device, semaphore, nullptr);
        }
        renderFinishedSemaphores.clear();
        if (swapchain) vk.DestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
        swapchainFormat = VK_FORMAT_UNDEFINED;
        extent = {};
    }

    bool CreateSwapchain(std::uint32_t requestedWidth, std::uint32_t requestedHeight,
                         std::string* errorText) {
        struct FailureCleanup {
            Impl* owner;
            bool committed = false;
            ~FailureCleanup() {
                if (!committed) owner->DestroySwapchainResources();
            }
        } cleanup{ this };
        VkSurfaceCapabilitiesKHR capabilities{};
        VkResult result = vk.GetPhysicalDeviceSurfaceCapabilitiesKHR(
            physicalDevice, surface, &capabilities);
        if (result != VK_SUCCESS) {
            SetError(errorText, VkError("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", result));
            return false;
        }
        std::uint32_t formatCount = 0;
        result = vk.GetPhysicalDeviceSurfaceFormatsKHR(
            physicalDevice, surface, &formatCount, nullptr);
        if (result != VK_SUCCESS || formatCount == 0) {
            SetError(errorText, result == VK_SUCCESS
                ? "Vulkan surface did not report a color format"
                : VkError("vkGetPhysicalDeviceSurfaceFormatsKHR", result));
            return false;
        }
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        result = vk.GetPhysicalDeviceSurfaceFormatsKHR(
            physicalDevice, surface, &formatCount, formats.data());
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
            SetError(errorText, VkError("vkGetPhysicalDeviceSurfaceFormatsKHR", result));
            return false;
        }
        VkSurfaceFormatKHR selectedFormat = formats.front();
        if (formats.size() == 1 && formats[0].format == VK_FORMAT_UNDEFINED) {
            selectedFormat = { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
        } else {
            const auto preferred = std::find_if(formats.begin(), formats.begin() + formatCount,
                [](const VkSurfaceFormatKHR& format) {
                    return (format.format == VK_FORMAT_B8G8R8A8_UNORM ||
                            format.format == VK_FORMAT_R8G8B8A8_UNORM) &&
                           format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
                });
            if (preferred != formats.begin() + formatCount) selectedFormat = *preferred;
        }

        std::uint32_t presentModeCount = 0;
        result = vk.GetPhysicalDeviceSurfacePresentModesKHR(
            physicalDevice, surface, &presentModeCount, nullptr);
        if (result != VK_SUCCESS || presentModeCount == 0) {
            SetError(errorText, result == VK_SUCCESS
                ? "Vulkan surface did not report a presentation mode"
                : VkError("vkGetPhysicalDeviceSurfacePresentModesKHR", result));
            return false;
        }
        std::vector<VkPresentModeKHR> presentModes(presentModeCount);
        result = vk.GetPhysicalDeviceSurfacePresentModesKHR(
            physicalDevice, surface, &presentModeCount, presentModes.data());
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
            SetError(errorText, VkError("vkGetPhysicalDeviceSurfacePresentModesKHR", result));
            return false;
        }
        VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
        if (!options.verticalSync) {
            if (std::find(presentModes.begin(), presentModes.begin() + presentModeCount,
                          VK_PRESENT_MODE_MAILBOX_KHR) != presentModes.begin() + presentModeCount) {
                presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
            } else if (std::find(presentModes.begin(), presentModes.begin() + presentModeCount,
                                 VK_PRESENT_MODE_IMMEDIATE_KHR) != presentModes.begin() + presentModeCount) {
                presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
            }
        }

        VkExtent2D selectedExtent{};
        if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
            selectedExtent = capabilities.currentExtent;
        } else {
            selectedExtent.width = std::clamp(
                std::max(1u, requestedWidth), capabilities.minImageExtent.width,
                capabilities.maxImageExtent.width);
            selectedExtent.height = std::clamp(
                std::max(1u, requestedHeight), capabilities.minImageExtent.height,
                capabilities.maxImageExtent.height);
        }
        std::uint32_t imageCount = std::max(capabilities.minImageCount + 1, 2u);
        if (capabilities.maxImageCount > 0) imageCount = std::min(imageCount, capabilities.maxImageCount);

        VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        constexpr std::array<VkCompositeAlphaFlagBitsKHR, 4> alphaModes = {
            VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        };
        for (const auto mode : alphaModes) {
            if ((capabilities.supportedCompositeAlpha & mode) != 0) {
                compositeAlpha = mode;
                break;
            }
        }

        VkSwapchainCreateInfoKHR createInfo{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
        createInfo.surface = surface;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = selectedFormat.format;
        createInfo.imageColorSpace = selectedFormat.colorSpace;
        createInfo.imageExtent = selectedExtent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        const std::array<std::uint32_t, 2> queueFamilies = {
            graphicsQueueFamily, presentQueueFamily
        };
        if (graphicsQueueFamily != presentQueueFamily) {
            createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            createInfo.queueFamilyIndexCount = static_cast<std::uint32_t>(queueFamilies.size());
            createInfo.pQueueFamilyIndices = queueFamilies.data();
        } else {
            createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }
        createInfo.preTransform = capabilities.currentTransform;
        createInfo.compositeAlpha = compositeAlpha;
        createInfo.presentMode = presentMode;
        createInfo.clipped = VK_TRUE;
        result = vk.CreateSwapchainKHR(device, &createInfo, nullptr, &swapchain);
        if (result != VK_SUCCESS) {
            SetError(errorText, VkError("vkCreateSwapchainKHR", result));
            return false;
        }
        swapchainFormat = selectedFormat.format;
        extent = selectedExtent;

        std::uint32_t actualImageCount = 0;
        result = vk.GetSwapchainImagesKHR(device, swapchain, &actualImageCount, nullptr);
        if (result != VK_SUCCESS || actualImageCount == 0) {
            SetError(errorText, result == VK_SUCCESS
                ? "Vulkan swapchain did not expose an image"
                : VkError("vkGetSwapchainImagesKHR", result));
            return false;
        }
        swapchainImages.resize(actualImageCount);
        result = vk.GetSwapchainImagesKHR(device, swapchain, &actualImageCount, swapchainImages.data());
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
            SetError(errorText, VkError("vkGetSwapchainImagesKHR", result));
            return false;
        }
        swapchainImages.resize(actualImageCount);
        renderFinishedSemaphores.resize(actualImageCount);
        for (VkSemaphore& semaphore : renderFinishedSemaphores) {
            VkSemaphoreCreateInfo semaphoreInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            result = vk.CreateSemaphore(device, &semaphoreInfo, nullptr, &semaphore);
            if (result != VK_SUCCESS) {
                SetError(errorText, VkError("vkCreateSemaphore(present)", result));
                return false;
            }
        }
        swapchainImageViews.resize(actualImageCount);
        for (std::size_t index = 0; index < swapchainImages.size(); ++index) {
            VkImageViewCreateInfo viewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            viewInfo.image = swapchainImages[index];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = swapchainFormat;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.layerCount = 1;
            result = vk.CreateImageView(device, &viewInfo, nullptr, &swapchainImageViews[index]);
            if (result != VK_SUCCESS) {
                SetError(errorText, VkError("vkCreateImageView(swapchain)", result));
                return false;
            }
        }

        depthFormat = FindDepthFormat();
        if (depthFormat == VK_FORMAT_UNDEFINED) {
            SetError(errorText, "Vulkan device has no supported depth attachment format");
            return false;
        }
        if (!CreateRenderPass(errorText) || !CreatePipelines(errorText)) return false;
        depthTargets.resize(actualImageCount);
        framebuffers.resize(actualImageCount);
        for (std::size_t index = 0; index < actualImageCount; ++index) {
            if (!CreateDepthTarget(&depthTargets[index], errorText)) return false;
            const std::array<VkImageView, 2> attachments = {
                swapchainImageViews[index], depthTargets[index].view
            };
            VkFramebufferCreateInfo framebufferInfo{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            framebufferInfo.renderPass = renderPass;
            framebufferInfo.attachmentCount = static_cast<std::uint32_t>(attachments.size());
            framebufferInfo.pAttachments = attachments.data();
            framebufferInfo.width = extent.width;
            framebufferInfo.height = extent.height;
            framebufferInfo.layers = 1;
            result = vk.CreateFramebuffer(device, &framebufferInfo, nullptr, &framebuffers[index]);
            if (result != VK_SUCCESS) {
                SetError(errorText, VkError("vkCreateFramebuffer", result));
                return false;
            }
        }
        imagesInFlight.assign(actualImageCount, VK_NULL_HANDLE);
        cleanup.committed = true;
        return true;
    }

    bool RecreateSwapchain(std::uint32_t width, std::uint32_t height,
                           std::string* errorText) {
        if (width == 0 || height == 0) return true;
        const VkResult idleResult = vk.DeviceWaitIdle(device);
        if (idleResult != VK_SUCCESS) {
            operational = false;
            SetError(errorText, VkError("vkDeviceWaitIdle(resize)", idleResult));
            return false;
        }
        DestroySwapchainResources();
        currentFrame = 0;
        if (!CreateSwapchain(width, height, errorText)) {
            operational = false;
            return false;
        }
        return true;
    }

    bool EnsureOverlayCapacity(std::size_t frameIndex, std::size_t vertexCount,
                               std::string* errorText) {
        if (vertexCount == 0) return true;
        if (frameIndex >= overlayVertexBuffers.size()) {
            SetError(errorText, "Vulkan overlay frame index is out of range");
            return false;
        }
        if (overlayVertexBuffers[frameIndex].buffer != VK_NULL_HANDLE &&
            vertexCount <= overlayVertexCapacities[frameIndex]) return true;
        const std::size_t capacity = std::max<std::size_t>(256, vertexCount + vertexCount / 2);
        if (capacity > std::numeric_limits<std::size_t>::max() / sizeof(ColorVertex)) {
            SetError(errorText, "Vulkan overlay vertex allocation overflows size_t");
            return false;
        }
        GpuBuffer replacement{};
        if (!CreateBuffer(static_cast<VkDeviceSize>(capacity * sizeof(ColorVertex)),
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &replacement, errorText)) return false;
        DestroyBuffer(&overlayVertexBuffers[frameIndex]);
        overlayVertexBuffers[frameIndex] = replacement;
        overlayVertexCapacities[frameIndex] = capacity;
        return true;
    }

    void PushFrameConstants(VkCommandBuffer commandBuffer, const XMMATRIX& world,
                            const XMMATRIX& viewProjection) const {
        const PushConstants constants = MakePushConstants(world, viewProjection);
        vk.CmdPushConstants(commandBuffer, pipelineLayout,
                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                             0, sizeof(constants), &constants);
    }

    bool WaitForFrameSubmissions(const char* operation, std::string* errorText) {
        std::vector<VkFence> fences;
        fences.reserve(frames.size());
        for (const FrameSync& frame : frames) {
            if (frame.fence != VK_NULL_HANDLE) fences.push_back(frame.fence);
        }
        if (fences.empty()) return true;
        const VkResult result = vk.WaitForFences(
            device, static_cast<std::uint32_t>(fences.size()), fences.data(), VK_TRUE,
            std::numeric_limits<std::uint64_t>::max());
        if (result == VK_SUCCESS) return true;
        operational = false;
        SetError(errorText, VkError(operation, result));
        return false;
    }

    bool RecoverFrameFenceAfterSubmitFailure(FrameSync* frame) {
        if (!frame || device == VK_NULL_HANDLE) return false;
        const VkFence failedFence = frame->fence;
        for (VkFence& imageFence : imagesInFlight) {
            if (imageFence == failedFence) imageFence = VK_NULL_HANDLE;
        }
        if (failedFence && vk.DestroyFence) vk.DestroyFence(device, failedFence, nullptr);
        frame->fence = VK_NULL_HANDLE;
        VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        return vk.CreateFence(device, &fenceInfo, nullptr, &frame->fence) == VK_SUCCESS;
    }

    void ClearModelBuffers() {
        DestroyBuffer(&modelVertexBuffer);
        for (std::size_t index = 0; index < modelIndexBuffers.size(); ++index) {
            if (modelIndexBufferOwned[index]) DestroyBuffer(&modelIndexBuffers[index]);
            modelIndexBuffers[index] = {};
        }
        modelIndexBufferOwned.fill(false);
        modelIndexCounts.fill(0);
    }

    void ClearWorldBuffers() {
        DestroyBuffer(&worldVertexBuffer);
        DestroyBuffer(&worldIndexBuffer);
        worldVertexCount = 0;
        worldIndexCount = 0;
    }

    void ShutdownResources() {
        if (device != VK_NULL_HANDLE && vk.DeviceWaitIdle) {
            const VkResult idleResult = vk.DeviceWaitIdle(device);
            if (idleResult != VK_SUCCESS && idleResult != VK_ERROR_DEVICE_LOST) {
                // Completion is unknown after OOM-style wait failures. Abandon the
                // driver objects instead of destroying resources that may still be in use.
                operational = false;
                device = VK_NULL_HANDLE;
                surface = VK_NULL_HANDLE;
                instance = VK_NULL_HANDLE;
                loader = nullptr;
                return;
            }
        }
        if (device != VK_NULL_HANDLE && vk.DestroyBuffer && vk.FreeMemory) {
            if (pendingUpload) {
                for (GpuBuffer& buffer : pendingUpload->stagingBuffers) DestroyBuffer(&buffer);
                for (GpuBuffer& buffer : pendingUpload->destinationBuffers) DestroyBuffer(&buffer);
                if (pendingUpload->fence != VK_NULL_HANDLE && vk.DestroyFence) {
                    vk.DestroyFence(device, pendingUpload->fence, nullptr);
                }
                if (pendingUpload->commandBuffer != VK_NULL_HANDLE && commandPool != VK_NULL_HANDLE &&
                    vk.FreeCommandBuffers) {
                    vk.FreeCommandBuffers(device, commandPool, 1, &pendingUpload->commandBuffer);
                }
                pendingUpload.reset();
            }
            ClearModelBuffers();
            ClearWorldBuffers();
            for (GpuBuffer& buffer : overlayVertexBuffers) DestroyBuffer(&buffer);
            overlayVertexBuffers.clear();
            overlayVertexCapacities.clear();
        }
        if (device != VK_NULL_HANDLE && vk.DestroyFramebuffer) DestroySwapchainResources();
        if (device != VK_NULL_HANDLE) {
            for (FrameSync& frame : frames) {
                if (frame.imageAvailable && vk.DestroySemaphore) {
                    vk.DestroySemaphore(device, frame.imageAvailable, nullptr);
                }
                if (frame.fence && vk.DestroyFence) vk.DestroyFence(device, frame.fence, nullptr);
            }
            frames.clear();
            if (commandPool && vk.DestroyCommandPool) {
                vk.DestroyCommandPool(device, commandPool, nullptr);
            }
            commandPool = VK_NULL_HANDLE;
            if (vk.DestroyDevice) vk.DestroyDevice(device, nullptr);
            device = VK_NULL_HANDLE;
        }
        if (surface != VK_NULL_HANDLE && instance != VK_NULL_HANDLE && vk.DestroySurfaceKHR) {
            vk.DestroySurfaceKHR(instance, surface, nullptr);
            surface = VK_NULL_HANDLE;
        }
        if (instance != VK_NULL_HANDLE && vk.DestroyInstance) {
            vk.DestroyInstance(instance, nullptr);
            instance = VK_NULL_HANDLE;
        }
        if (loader) {
            FreeLibrary(loader);
            loader = nullptr;
        }
    }
};

PreviewVulkanRenderer::PreviewVulkanRenderer() : impl_(std::make_unique<Impl>()) {}

PreviewVulkanRenderer::~PreviewVulkanRenderer() {
    Shutdown();
}

PreviewVulkanRenderer::PreviewVulkanRenderer(PreviewVulkanRenderer&& other) noexcept
    : impl_(std::move(other.impl_)) {}

PreviewVulkanRenderer& PreviewVulkanRenderer::operator=(PreviewVulkanRenderer&& other) noexcept {
    if (this == &other) return *this;
    Shutdown();
    impl_ = std::move(other.impl_);
    return *this;
}

bool PreviewVulkanRenderer::Initialize(PreviewVulkanNativeWindow window,
                                       const PreviewVulkanOptions& options,
                                       std::string* errorText) {
    Shutdown();
    impl_ = std::make_unique<Impl>();
    impl_->window = static_cast<HWND>(window);
    impl_->options = options;
    if (!impl_->window || !IsWindow(impl_->window)) {
        SetError(errorText, "PreviewVulkanRenderer requires a valid HWND");
        return false;
    }
    RECT bounds{};
    GetClientRect(impl_->window, &bounds);
    const std::uint32_t width = static_cast<std::uint32_t>(
        std::max<LONG>(1, bounds.right - bounds.left));
    const std::uint32_t height = static_cast<std::uint32_t>(
        std::max<LONG>(1, bounds.bottom - bounds.top));
    if (!impl_->LoadVulkan(errorText) ||
        !impl_->CreateVulkanInstance(errorText) ||
        !impl_->CreateSurface(errorText) ||
        !impl_->PickPhysicalDevice(errorText) ||
        !impl_->CreateLogicalDevice(errorText) ||
        !impl_->CreateCommandResources(errorText) ||
        !impl_->CreateSwapchain(width, height, errorText)) {
        impl_->ShutdownResources();
        impl_.reset();
        return false;
    }
    impl_->operational = true;
    return true;
}

bool PreviewVulkanRenderer::Resize(std::uint32_t width, std::uint32_t height,
                                   std::string* errorText) {
    if (!IsReady()) {
        SetError(errorText, "Vulkan renderer is not initialized");
        return false;
    }
    if (width == 0 || height == 0) return true;
    if (impl_->extent.width == width && impl_->extent.height == height) return true;
    return impl_->RecreateSwapchain(width, height, errorText);
}

void PreviewVulkanRenderer::Shutdown() {
    if (!impl_) return;
    impl_->ShutdownResources();
    impl_.reset();
}

bool PreviewVulkanRenderer::UploadMesh(const PreviewMesh& mesh, std::string* errorText) {
    if (!IsReady()) {
        SetError(errorText, "Vulkan renderer is not initialized");
        return false;
    }
    const std::size_t vertexCount = mesh.positions.size() / 3;
    if (vertexCount == 0 || mesh.positions.size() != vertexCount * 3 || mesh.indices.empty()) {
        SetError(errorText, "Preview mesh has no valid vertices or indices");
        return false;
    }
    if (vertexCount > std::numeric_limits<std::uint32_t>::max()) {
        SetError(errorText, "Preview mesh has more vertices than Vulkan 32-bit indices can address");
        return false;
    }

    std::vector<RenderVertex> vertices(vertexCount);
    const bool hasNormals = mesh.normals.size() == vertexCount * 3;
    const bool hasColors = mesh.colors.size() == vertexCount * 4;
    for (std::size_t index = 0; index < vertexCount; ++index) {
        std::copy_n(mesh.positions.data() + index * 3, 3, vertices[index].position);
        if (hasNormals) {
            std::copy_n(mesh.normals.data() + index * 3, 3, vertices[index].normal);
        } else {
            vertices[index].normal[0] = 0.0f;
            vertices[index].normal[1] = 1.0f;
            vertices[index].normal[2] = 0.0f;
        }
        if (hasColors) {
            std::copy_n(mesh.colors.data() + index * 4, 4, vertices[index].color);
        } else {
            vertices[index].color[0] = 205;
            vertices[index].color[1] = 210;
            vertices[index].color[2] = 215;
            vertices[index].color[3] = 255;
        }
    }

    GpuBuffer vertexBuffer{};
    std::array<GpuBuffer, 4> indexBuffers{};
    std::array<bool, 4> indexBufferOwned{};
    std::array<std::uint32_t, 4> indexCounts{};
    auto destroyTemporary = [&]() {
        impl_->DestroyBuffer(&vertexBuffer);
        for (std::size_t level = 0; level < indexBuffers.size(); ++level) {
            if (indexBufferOwned[level]) impl_->DestroyBuffer(&indexBuffers[level]);
        }
    };
    std::vector<DeviceBufferUpload> uploads;
    uploads.reserve(1 + indexBuffers.size());
    uploads.push_back(DeviceBufferUpload{
        vertices.data(), vertices.size() * sizeof(RenderVertex),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vertexBuffer
    });
    for (std::size_t level = 0; level < indexBuffers.size(); ++level) {
        if (level > 0 && mesh.lodIndices[level].empty()) {
            indexCounts[level] = static_cast<std::uint32_t>(mesh.indices.size());
            continue;
        }
        const auto& indices = level == 0 ? mesh.indices : mesh.lodIndices[level];
        if (indices.empty() || indices.size() > std::numeric_limits<std::uint32_t>::max()) {
            destroyTemporary();
            SetError(errorText, "Preview mesh LOD index buffer is empty or too large");
            return false;
        }
        if (std::any_of(indices.begin(), indices.end(),
                        [vertexCount](std::uint32_t index) { return index >= vertexCount; })) {
            destroyTemporary();
            SetError(errorText, "Preview mesh LOD contains an out-of-range vertex index");
            return false;
        }
        indexBufferOwned[level] = true;
        indexCounts[level] = static_cast<std::uint32_t>(indices.size());
        uploads.push_back(DeviceBufferUpload{
            indices.data(), indices.size() * sizeof(std::uint32_t),
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &indexBuffers[level]
        });
    }
    if (!impl_->CreateDeviceBuffers(uploads, errorText)) {
        destroyTemporary();
        return false;
    }
    if (!impl_->WaitForFrameSubmissions("vkWaitForFences(model replacement)", errorText)) {
        destroyTemporary();
        return false;
    }
    for (std::size_t level = 1; level < indexBuffers.size(); ++level) {
        if (!indexBufferOwned[level]) indexBuffers[level] = indexBuffers[0];
    }
    impl_->ClearModelBuffers();
    impl_->modelVertexBuffer = vertexBuffer;
    impl_->modelIndexBuffers = indexBuffers;
    impl_->modelIndexBufferOwned = indexBufferOwned;
    impl_->modelIndexCounts = indexCounts;
    return true;
}

bool PreviewVulkanRenderer::UploadWorldMesh(const PreviewVulkanWorldMeshView& mesh,
                                            std::string* errorText) {
    if (!IsReady()) {
        SetError(errorText, "Vulkan renderer is not initialized");
        return false;
    }
    if (!mesh.positions || !mesh.colors || mesh.positionFloatCount == 0 ||
        mesh.positionFloatCount % 3 != 0 ||
        (mesh.colorChannels != 3 && mesh.colorChannels != 4)) {
        SetError(errorText, "World preview mesh has invalid position or color data");
        return false;
    }
    const std::size_t vertexCount = mesh.positionFloatCount / 3;
    if (mesh.colorByteCount < vertexCount * mesh.colorChannels ||
        vertexCount > std::numeric_limits<std::uint32_t>::max()) {
        SetError(errorText, "World preview mesh color data is short or the vertex count is too large");
        return false;
    }
    if (mesh.primitive == PreviewVulkanWorldPrimitive::Quads && vertexCount % 4 != 0) {
        SetError(errorText, "World preview quad vertex count must be divisible by four");
        return false;
    }
    if (mesh.primitive == PreviewVulkanWorldPrimitive::Triangles && vertexCount % 3 != 0) {
        SetError(errorText, "World preview triangle vertex count must be divisible by three");
        return false;
    }

    std::vector<ColorVertex> vertices(vertexCount);
    for (std::size_t index = 0; index < vertexCount; ++index) {
        std::copy_n(mesh.positions + index * 3, 3, vertices[index].position);
        const std::size_t colorOffset = index * mesh.colorChannels;
        vertices[index].color[0] = mesh.colors[colorOffset];
        vertices[index].color[1] = mesh.colors[colorOffset + 1];
        vertices[index].color[2] = mesh.colors[colorOffset + 2];
        vertices[index].color[3] = mesh.colorChannels == 4 ? mesh.colors[colorOffset + 3] : 255;
    }
    GpuBuffer vertexBuffer{};
    GpuBuffer indexBuffer{};
    std::uint32_t indexCount = 0;
    std::vector<std::uint32_t> indices;
    if (mesh.primitive == PreviewVulkanWorldPrimitive::Quads) {
        const std::size_t quadCount = vertexCount / 4;
        if (quadCount > std::numeric_limits<std::uint32_t>::max() / 6u) {
            SetError(errorText, "World preview quad index count is too large");
            return false;
        }
        indices.reserve(quadCount * 6);
        for (std::size_t quad = 0; quad < quadCount; ++quad) {
            const std::uint32_t base = static_cast<std::uint32_t>(quad * 4);
            indices.insert(indices.end(), { base, base + 1, base + 2,
                                            base, base + 2, base + 3 });
        }
        indexCount = static_cast<std::uint32_t>(indices.size());
    }
    std::vector<DeviceBufferUpload> uploads;
    uploads.reserve(indices.empty() ? 1 : 2);
    uploads.push_back(DeviceBufferUpload{
        vertices.data(), vertices.size() * sizeof(ColorVertex),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vertexBuffer
    });
    if (!indices.empty()) {
        uploads.push_back(DeviceBufferUpload{
            indices.data(), indices.size() * sizeof(std::uint32_t),
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &indexBuffer
        });
    }
    if (!impl_->CreateDeviceBuffers(uploads, errorText)) {
        impl_->DestroyBuffer(&vertexBuffer);
        impl_->DestroyBuffer(&indexBuffer);
        return false;
    }
    if (!impl_->WaitForFrameSubmissions("vkWaitForFences(world replacement)", errorText)) {
        impl_->DestroyBuffer(&vertexBuffer);
        impl_->DestroyBuffer(&indexBuffer);
        return false;
    }
    impl_->ClearWorldBuffers();
    impl_->worldVertexBuffer = vertexBuffer;
    impl_->worldIndexBuffer = indexBuffer;
    impl_->worldVertexCount = static_cast<std::uint32_t>(vertexCount);
    impl_->worldIndexCount = indexCount;
    impl_->worldPrimitive = mesh.primitive;
    return true;
}

void PreviewVulkanRenderer::ClearMesh() {
    if (!impl_ || impl_->device == VK_NULL_HANDLE || !impl_->operational) return;
    if (!impl_->WaitForFrameSubmissions("vkWaitForFences(clear mesh)", nullptr)) return;
    impl_->ClearModelBuffers();
}

void PreviewVulkanRenderer::ClearWorldMesh() {
    if (!impl_ || impl_->device == VK_NULL_HANDLE || !impl_->operational) return;
    if (!impl_->WaitForFrameSubmissions("vkWaitForFences(clear world mesh)", nullptr)) return;
    impl_->ClearWorldBuffers();
}

bool PreviewVulkanRenderer::Render(const PreviewVulkanFrame& frame,
                                   PreviewVulkanRenderStats* stats,
                                   std::string* errorText) {
    if (!IsReady()) {
        SetError(errorText, "Vulkan renderer is not initialized");
        return false;
    }
    RECT bounds{};
    GetClientRect(impl_->window, &bounds);
    const std::uint32_t clientWidth = static_cast<std::uint32_t>(
        std::max<LONG>(0, bounds.right - bounds.left));
    const std::uint32_t clientHeight = static_cast<std::uint32_t>(
        std::max<LONG>(0, bounds.bottom - bounds.top));
    if (clientWidth == 0 || clientHeight == 0) return true;
    if ((impl_->extent.width != clientWidth || impl_->extent.height != clientHeight) &&
        !impl_->RecreateSwapchain(clientWidth, clientHeight, errorText)) return false;

    const auto started = std::chrono::steady_clock::now();
    PreviewVulkanRenderStats localStats{};
    localStats.frameNumber = ++impl_->frameNumber;
    const XMMATRIX viewProjection = BuildViewProjection(
        frame.camera, static_cast<float>(impl_->extent.width) /
                      static_cast<float>(impl_->extent.height));

    std::vector<ColorVertex> overlayVertices;
    auto addLine = [&overlayVertices](const std::array<float, 3>& a,
                                      const std::array<float, 3>& b,
                                      const std::array<std::uint8_t, 4>& color) {
        ColorVertex first{};
        ColorVertex second{};
        std::copy(a.begin(), a.end(), first.position);
        std::copy(b.begin(), b.end(), second.position);
        std::copy(color.begin(), color.end(), first.color);
        std::copy(color.begin(), color.end(), second.color);
        overlayVertices.push_back(first);
        overlayVertices.push_back(second);
    };
    if (frame.overlay.showGrid) {
        const float step = std::max(0.001f, frame.overlay.gridStep);
        const std::uint32_t count = std::min<std::uint32_t>(
            frame.overlay.gridLineCount, 4096);
        const float extent = step * static_cast<float>(count);
        for (std::int32_t index = -static_cast<std::int32_t>(count);
             index <= static_cast<std::int32_t>(count); ++index) {
            const float line = static_cast<float>(index) * step;
            addLine({ line, 0.0f, -extent }, { line, 0.0f, extent }, { 46, 49, 54, 255 });
            addLine({ -extent, 0.0f, line }, { extent, 0.0f, line }, { 46, 49, 54, 255 });
        }
    }
    if (frame.overlay.showAxes) {
        addLine({ 0.0f, 0.0f, 0.0f }, { 4.0f, 0.0f, 0.0f }, { 255, 82, 82, 255 });
        addLine({ 0.0f, 0.0f, 0.0f }, { 0.0f, 4.0f, 0.0f }, { 118, 255, 3, 255 });
        addLine({ 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 4.0f }, { 64, 156, 255, 255 });
    }
    const std::uint32_t sceneLineVertexCount = static_cast<std::uint32_t>(overlayVertices.size());
    const std::uint32_t gizmoFirstVertex = sceneLineVertexCount;
    if (frame.overlay.gizmoLength > 0.0f) {
        const auto& position = frame.overlay.gizmoPosition;
        const float length = frame.overlay.gizmoLength;
        addLine(position, { position[0] + length, position[1], position[2] }, { 255, 92, 92, 255 });
        addLine(position, { position[0], position[1] + length, position[2] }, { 118, 255, 3, 255 });
        addLine(position, { position[0], position[1], position[2] + length }, { 64, 156, 255, 255 });
    }
    const std::uint32_t gizmoVertexCount = static_cast<std::uint32_t>(
        overlayVertices.size() - gizmoFirstVertex);

    FrameSync& sync = impl_->frames[impl_->currentFrame];
    VkResult result = impl_->vk.WaitForFences(
        impl_->device, 1, &sync.fence, VK_TRUE, std::numeric_limits<std::uint64_t>::max());
    if (result != VK_SUCCESS) {
        impl_->operational = false;
        SetError(errorText, VkError("vkWaitForFences(frame)", result));
        return false;
    }

    if (!overlayVertices.empty()) {
        if (!impl_->EnsureOverlayCapacity(impl_->currentFrame, overlayVertices.size(), errorText) ||
            !impl_->WriteHostBuffer(impl_->overlayVertexBuffers[impl_->currentFrame],
                                    overlayVertices.data(),
                                    overlayVertices.size() * sizeof(ColorVertex), errorText)) {
            return false;
        }
    }

    std::uint32_t imageIndex = 0;
    result = impl_->vk.AcquireNextImageKHR(
        impl_->device, impl_->swapchain, std::numeric_limits<std::uint64_t>::max(),
        sync.imageAvailable, VK_NULL_HANDLE, &imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        return impl_->RecreateSwapchain(clientWidth, clientHeight, errorText);
    }
    const bool acquireSuboptimal = result == VK_SUBOPTIMAL_KHR;
    if (result != VK_SUCCESS && !acquireSuboptimal) {
        impl_->operational = false;
        SetError(errorText, VkError("vkAcquireNextImageKHR", result));
        return false;
    }
    if (imageIndex >= impl_->framebuffers.size()) {
        impl_->operational = false;
        SetError(errorText, "Vulkan swapchain returned an out-of-range image index");
        return false;
    }
    if (impl_->imagesInFlight[imageIndex] != VK_NULL_HANDLE) {
        result = impl_->vk.WaitForFences(
            impl_->device, 1, &impl_->imagesInFlight[imageIndex], VK_TRUE,
            std::numeric_limits<std::uint64_t>::max());
        if (result != VK_SUCCESS) {
            impl_->operational = false;
            SetError(errorText, VkError("vkWaitForFences(swapchain image)", result));
            return false;
        }
    }

    result = impl_->vk.ResetCommandBuffer(sync.commandBuffer, 0);
    if (result != VK_SUCCESS) {
        impl_->operational = false;
        SetError(errorText, VkError("vkResetCommandBuffer", result));
        return false;
    }
    VkCommandBufferBeginInfo commandBegin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    commandBegin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = impl_->vk.BeginCommandBuffer(sync.commandBuffer, &commandBegin);
    if (result != VK_SUCCESS) {
        impl_->operational = false;
        SetError(errorText, VkError("vkBeginCommandBuffer", result));
        return false;
    }

    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color.float32[0] = frame.clearColor[0];
    clearValues[0].color.float32[1] = frame.clearColor[1];
    clearValues[0].color.float32[2] = frame.clearColor[2];
    clearValues[0].color.float32[3] = frame.clearColor[3];
    clearValues[1].depthStencil = { 1.0f, 0 };
    VkRenderPassBeginInfo renderPassBegin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    renderPassBegin.renderPass = impl_->renderPass;
    renderPassBegin.framebuffer = impl_->framebuffers[imageIndex];
    renderPassBegin.renderArea.extent = impl_->extent;
    renderPassBegin.clearValueCount = static_cast<std::uint32_t>(clearValues.size());
    renderPassBegin.pClearValues = clearValues.data();
    impl_->vk.CmdBeginRenderPass(sync.commandBuffer, &renderPassBegin, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport{};
    viewport.width = static_cast<float>(impl_->extent.width);
    viewport.height = static_cast<float>(impl_->extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    impl_->vk.CmdSetViewport(sync.commandBuffer, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = impl_->extent;
    impl_->vk.CmdSetScissor(sync.commandBuffer, 0, 1, &scissor);

    const VkDeviceSize vertexOffset = 0;
    if (sceneLineVertexCount > 0) {
        const VkBuffer overlayBuffer = impl_->overlayVertexBuffers[impl_->currentFrame].buffer;
        impl_->PushFrameConstants(sync.commandBuffer, XMMatrixIdentity(), viewProjection);
        impl_->vk.CmdBindPipeline(sync.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  impl_->colorLinePipeline);
        impl_->vk.CmdBindVertexBuffers(sync.commandBuffer, 0, 1, &overlayBuffer, &vertexOffset);
        impl_->vk.CmdDraw(sync.commandBuffer, sceneLineVertexCount, 1, 0, 0);
        ++localStats.drawCalls;
    }

    if (frame.showWorld && impl_->worldVertexBuffer.buffer != VK_NULL_HANDLE) {
        const XMMATRIX world = XMMatrixTranslation(
            frame.worldOffset[0], frame.worldOffset[1], frame.worldOffset[2]);
        impl_->PushFrameConstants(sync.commandBuffer, world, viewProjection);
        const VkBuffer worldVertexBuffer = impl_->worldVertexBuffer.buffer;
        impl_->vk.CmdBindVertexBuffers(sync.commandBuffer, 0, 1, &worldVertexBuffer, &vertexOffset);
        if (impl_->worldPrimitive == PreviewVulkanWorldPrimitive::Quads) {
            impl_->vk.CmdBindPipeline(sync.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      impl_->colorTrianglePipeline);
            impl_->vk.CmdBindIndexBuffer(sync.commandBuffer, impl_->worldIndexBuffer.buffer,
                                         0, VK_INDEX_TYPE_UINT32);
            impl_->vk.CmdDrawIndexed(sync.commandBuffer, impl_->worldIndexCount, 1, 0, 0, 0);
            localStats.worldTriangles = impl_->worldIndexCount / 3;
        } else if (impl_->worldPrimitive == PreviewVulkanWorldPrimitive::Triangles) {
            impl_->vk.CmdBindPipeline(sync.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      impl_->colorTrianglePipeline);
            impl_->vk.CmdDraw(sync.commandBuffer, impl_->worldVertexCount, 1, 0, 0);
            localStats.worldTriangles = impl_->worldVertexCount / 3;
        } else {
            impl_->vk.CmdBindPipeline(sync.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      impl_->colorPointPipeline);
            impl_->vk.CmdDraw(sync.commandBuffer, impl_->worldVertexCount, 1, 0, 0);
            localStats.worldPoints = impl_->worldVertexCount;
        }
        ++localStats.drawCalls;
    }

    if (frame.showModel && impl_->modelVertexBuffer.buffer != VK_NULL_HANDLE) {
        const std::size_t level = std::min<std::size_t>(
            frame.lodLevel, impl_->modelIndexBuffers.size() - 1);
        if (impl_->modelIndexBuffers[level].buffer && impl_->modelIndexCounts[level]) {
            const XMMATRIX world = BuildModelMatrix(frame.model);
            impl_->PushFrameConstants(sync.commandBuffer, world, viewProjection);
            const VkBuffer modelVertexBuffer = impl_->modelVertexBuffer.buffer;
            impl_->vk.CmdBindVertexBuffers(sync.commandBuffer, 0, 1,
                                           &modelVertexBuffer, &vertexOffset);
            impl_->vk.CmdBindIndexBuffer(sync.commandBuffer,
                                         impl_->modelIndexBuffers[level].buffer,
                                         0, VK_INDEX_TYPE_UINT32);
            const bool drawSolid = frame.drawMode == PreviewVulkanDrawMode::Solid ||
                                   frame.drawMode == PreviewVulkanDrawMode::SolidWire ||
                                   !impl_->wireframeSupported;
            if (drawSolid) {
                impl_->vk.CmdBindPipeline(sync.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                          impl_->modelSolidPipeline);
                impl_->vk.CmdDrawIndexed(sync.commandBuffer,
                                         impl_->modelIndexCounts[level], 1, 0, 0, 0);
                ++localStats.drawCalls;
            }
            if (impl_->wireframeSupported &&
                (frame.drawMode == PreviewVulkanDrawMode::Wireframe ||
                 frame.drawMode == PreviewVulkanDrawMode::SolidWire)) {
                impl_->vk.CmdBindPipeline(sync.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                          impl_->modelWirePipeline);
                impl_->vk.CmdDrawIndexed(sync.commandBuffer,
                                         impl_->modelIndexCounts[level], 1, 0, 0, 0);
                ++localStats.drawCalls;
            }
            localStats.modelTriangles = impl_->modelIndexCounts[level] / 3;
        }
    }

    if (gizmoVertexCount > 0) {
        const VkBuffer overlayBuffer = impl_->overlayVertexBuffers[impl_->currentFrame].buffer;
        impl_->PushFrameConstants(sync.commandBuffer, XMMatrixIdentity(), viewProjection);
        impl_->vk.CmdBindPipeline(sync.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  impl_->colorOverlayLinePipeline);
        impl_->vk.CmdBindVertexBuffers(sync.commandBuffer, 0, 1, &overlayBuffer, &vertexOffset);
        impl_->vk.CmdDraw(sync.commandBuffer, gizmoVertexCount, 1, gizmoFirstVertex, 0);
        ++localStats.drawCalls;
    }

    impl_->vk.CmdEndRenderPass(sync.commandBuffer);
    result = impl_->vk.EndCommandBuffer(sync.commandBuffer);
    if (result != VK_SUCCESS) {
        impl_->operational = false;
        SetError(errorText, VkError("vkEndCommandBuffer", result));
        return false;
    }
    result = impl_->vk.ResetFences(impl_->device, 1, &sync.fence);
    if (result != VK_SUCCESS) {
        impl_->operational = false;
        SetError(errorText, VkError("vkResetFences", result));
        return false;
    }
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &sync.imageAvailable;
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &sync.commandBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &impl_->renderFinishedSemaphores[imageIndex];
    result = impl_->vk.QueueSubmit(impl_->graphicsQueue, 1, &submitInfo, sync.fence);
    if (result != VK_SUCCESS) {
        impl_->RecoverFrameFenceAfterSubmitFailure(&sync);
        impl_->operational = false;
        SetError(errorText, VkError("vkQueueSubmit", result));
        return false;
    }
    impl_->imagesInFlight[imageIndex] = sync.fence;

    VkPresentInfoKHR presentInfo{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &impl_->renderFinishedSemaphores[imageIndex];
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &impl_->swapchain;
    presentInfo.pImageIndices = &imageIndex;
    const VkResult presentResult = impl_->vk.QueuePresentKHR(impl_->presentQueue, &presentInfo);
    if (presentResult != VK_SUCCESS && presentResult != VK_ERROR_OUT_OF_DATE_KHR &&
        presentResult != VK_SUBOPTIMAL_KHR) {
        impl_->operational = false;
        SetError(errorText, VkError("vkQueuePresentKHR", presentResult));
        return false;
    }
    impl_->currentFrame = (impl_->currentFrame + 1) % impl_->frames.size();
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR ||
        acquireSuboptimal) {
        if (!impl_->RecreateSwapchain(clientWidth, clientHeight, errorText)) return false;
    }

    localStats.cpuMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    if (stats) *stats = localStats;
    return true;
}

bool PreviewVulkanRenderer::IsReady() const noexcept {
    if (!impl_ || !impl_->operational || !impl_->loader || impl_->instance == VK_NULL_HANDLE ||
        impl_->surface == VK_NULL_HANDLE || impl_->physicalDevice == VK_NULL_HANDLE ||
        impl_->device == VK_NULL_HANDLE || impl_->graphicsQueue == VK_NULL_HANDLE ||
        impl_->presentQueue == VK_NULL_HANDLE ||
        impl_->swapchain == VK_NULL_HANDLE || impl_->renderPass == VK_NULL_HANDLE ||
        impl_->pipelineLayout == VK_NULL_HANDLE || impl_->modelSolidPipeline == VK_NULL_HANDLE ||
        impl_->colorTrianglePipeline == VK_NULL_HANDLE ||
        impl_->colorPointPipeline == VK_NULL_HANDLE || impl_->colorLinePipeline == VK_NULL_HANDLE ||
        impl_->colorOverlayLinePipeline == VK_NULL_HANDLE || impl_->frames.empty()) return false;
    if (impl_->wireframeSupported && impl_->modelWirePipeline == VK_NULL_HANDLE) return false;
    const std::size_t imageCount = impl_->swapchainImages.size();
    if (imageCount == 0 || impl_->swapchainImageViews.size() != imageCount ||
        impl_->depthTargets.size() != imageCount || impl_->framebuffers.size() != imageCount ||
        impl_->imagesInFlight.size() != imageCount ||
        impl_->renderFinishedSemaphores.size() != imageCount) return false;
    if (std::any_of(impl_->swapchainImageViews.begin(), impl_->swapchainImageViews.end(),
                    [](VkImageView view) { return view == VK_NULL_HANDLE; }) ||
        std::any_of(impl_->framebuffers.begin(), impl_->framebuffers.end(),
                    [](VkFramebuffer framebuffer) { return framebuffer == VK_NULL_HANDLE; }) ||
        std::any_of(impl_->depthTargets.begin(), impl_->depthTargets.end(),
                    [](const DepthTarget& target) {
                        return target.image == VK_NULL_HANDLE || target.memory == VK_NULL_HANDLE ||
                               target.view == VK_NULL_HANDLE;
                    }) ||
        std::any_of(impl_->renderFinishedSemaphores.begin(),
                    impl_->renderFinishedSemaphores.end(),
                    [](VkSemaphore semaphore) { return semaphore == VK_NULL_HANDLE; }) ||
        std::any_of(impl_->frames.begin(), impl_->frames.end(),
                    [](const FrameSync& frame) {
                        return frame.commandBuffer == VK_NULL_HANDLE ||
                               frame.imageAvailable == VK_NULL_HANDLE || frame.fence == VK_NULL_HANDLE;
                    })) return false;
    return true;
}

bool PreviewVulkanRenderer::IsHardwareDevice() const noexcept {
    return impl_ && impl_->hardwareDevice;
}

bool PreviewVulkanRenderer::SupportsWireframe() const noexcept {
    return impl_ && impl_->wireframeSupported && impl_->modelWirePipeline != VK_NULL_HANDLE;
}

const std::string& PreviewVulkanRenderer::AdapterName() const noexcept {
    static const std::string empty;
    return impl_ ? impl_->adapterName : empty;
}

std::uint32_t PreviewVulkanRenderer::ApiVersion() const noexcept {
    return impl_ ? impl_->physicalProperties.apiVersion : 0;
}

}  // namespace native_mc
