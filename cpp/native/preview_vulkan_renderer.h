#pragma once

#include "preview_mesh.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace native_mc {

enum class PreviewVulkanDrawMode {
    Solid,
    Wireframe,
    SolidWire,
};

enum class PreviewVulkanWorldPrimitive {
    Points,
    Triangles,
    Quads,
};

struct PreviewVulkanOptions {
    bool verticalSync = false;
    bool preferDiscreteGpu = true;
    std::uint32_t framesInFlight = 2;
};

struct PreviewVulkanWorldMeshView {
    const float* positions = nullptr;
    std::size_t positionFloatCount = 0;
    const std::uint8_t* colors = nullptr;
    std::size_t colorByteCount = 0;
    std::uint32_t colorChannels = 3;
    PreviewVulkanWorldPrimitive primitive = PreviewVulkanWorldPrimitive::Quads;
};

struct PreviewVulkanCamera {
    float yawDegrees = 35.0f;
    float pitchDegrees = 20.0f;
    float distance = 8.0f;
    float panX = 0.0f;
    float panY = 0.0f;
    float fieldOfViewDegrees = 50.0f;
    float nearPlane = 0.1f;
    float farPlane = 5000.0f;
};

struct PreviewVulkanModelTransform {
    std::array<float, 3> position = { 0.0f, 0.0f, 0.0f };
    // Source-coordinate conversion and leveling rotation (W, X, Y, Z). It is
    // applied in source-local space before scale and the user quaternion.
    std::array<float, 4> sourceQuaternion = { 1.0f, 0.0f, 0.0f, 0.0f };
    // W, X, Y, Z, matching the GUI's existing quaternion edit order.
    std::array<float, 4> quaternion = { 1.0f, 0.0f, 0.0f, 0.0f };
    std::array<float, 3> scale = { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> center = { 0.0f, 0.0f, 0.0f };
};

struct PreviewVulkanOverlay {
    bool showGrid = true;
    bool showAxes = true;
    float gridStep = 1.0f;
    std::uint32_t gridLineCount = 64;
    std::array<float, 3> gizmoPosition = { 0.0f, 0.0f, 0.0f };
    float gizmoLength = 0.0f;
};

struct PreviewVulkanFrame {
    PreviewVulkanCamera camera;
    PreviewVulkanModelTransform model;
    PreviewVulkanOverlay overlay;
    PreviewVulkanDrawMode drawMode = PreviewVulkanDrawMode::SolidWire;
    std::uint32_t lodLevel = 0;
    bool showModel = true;
    bool showWorld = true;
    std::array<float, 3> worldOffset = { 0.0f, 0.0f, 0.0f };
    std::array<float, 4> clearColor = { 0.11f, 0.12f, 0.13f, 1.0f };
};

struct PreviewVulkanRenderStats {
    std::uint64_t frameNumber = 0;
    std::uint64_t modelTriangles = 0;
    std::uint64_t worldTriangles = 0;
    std::uint64_t worldPoints = 0;
    std::uint32_t drawCalls = 0;
    double cpuMilliseconds = 0.0;
};

using PreviewVulkanNativeWindow = void*;

class PreviewVulkanRenderer final {
public:
    PreviewVulkanRenderer();
    ~PreviewVulkanRenderer();

    PreviewVulkanRenderer(const PreviewVulkanRenderer&) = delete;
    PreviewVulkanRenderer& operator=(const PreviewVulkanRenderer&) = delete;
    PreviewVulkanRenderer(PreviewVulkanRenderer&&) noexcept;
    PreviewVulkanRenderer& operator=(PreviewVulkanRenderer&&) noexcept;

    bool Initialize(PreviewVulkanNativeWindow window, const PreviewVulkanOptions& options,
                    std::string* errorText);
    bool Resize(std::uint32_t width, std::uint32_t height, std::string* errorText);
    void Shutdown();

    bool UploadMesh(const PreviewMesh& mesh, std::string* errorText);
    bool UploadWorldMesh(const PreviewVulkanWorldMeshView& mesh, std::string* errorText);
    void ClearMesh();
    void ClearWorldMesh();

    bool Render(const PreviewVulkanFrame& frame, PreviewVulkanRenderStats* stats,
                std::string* errorText);

    bool IsReady() const noexcept;
    bool IsHardwareDevice() const noexcept;
    bool SupportsWireframe() const noexcept;
    const std::string& AdapterName() const noexcept;
    std::uint32_t ApiVersion() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace native_mc
