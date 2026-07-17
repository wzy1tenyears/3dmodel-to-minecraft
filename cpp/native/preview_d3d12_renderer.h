#pragma once

#include "preview_mesh.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace native_mc {

enum class PreviewD3D12DrawMode {
    Solid,
    Wireframe,
    SolidWire,
};

enum class PreviewD3D12WorldPrimitive {
    Points,
    Triangles,
    Quads,
};

struct PreviewD3D12Options {
    bool allowWarpFallback = true;
    bool verticalSync = false;
    std::uint32_t maximumFrameLatency = 1;
};

// This view maps directly to the existing WorldPreviewGpuMesh vectors.
struct PreviewD3D12WorldMeshView {
    const float* positions = nullptr;
    std::size_t positionFloatCount = 0;
    const std::uint8_t* colors = nullptr;
    std::size_t colorByteCount = 0;
    std::uint32_t colorChannels = 3;
    PreviewD3D12WorldPrimitive primitive = PreviewD3D12WorldPrimitive::Quads;
};

struct PreviewD3D12Camera {
    float yawDegrees = 35.0f;
    float pitchDegrees = 20.0f;
    float distance = 8.0f;
    float panX = 0.0f;
    float panY = 0.0f;
    float fieldOfViewDegrees = 50.0f;
    float nearPlane = 0.1f;
    float farPlane = 5000.0f;
};

struct PreviewD3D12ModelTransform {
    std::array<float, 3> position = { 0.0f, 0.0f, 0.0f };
    // Source-coordinate conversion and leveling rotation (W, X, Y, Z). It is
    // applied in source-local space before scale and the user quaternion.
    std::array<float, 4> sourceQuaternion = { 1.0f, 0.0f, 0.0f, 0.0f };
    // W, X, Y, Z, matching the GUI's existing quaternion edit order.
    std::array<float, 4> quaternion = { 1.0f, 0.0f, 0.0f, 0.0f };
    std::array<float, 3> scale = { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> center = { 0.0f, 0.0f, 0.0f };
};

struct PreviewD3D12Overlay {
    bool showGrid = true;
    bool showAxes = true;
    float gridStep = 1.0f;
    std::uint32_t gridLineCount = 64;
    std::array<float, 3> gizmoPosition = { 0.0f, 0.0f, 0.0f };
    float gizmoLength = 0.0f;
};

struct PreviewD3D12Frame {
    PreviewD3D12Camera camera;
    PreviewD3D12ModelTransform model;
    PreviewD3D12Overlay overlay;
    PreviewD3D12DrawMode drawMode = PreviewD3D12DrawMode::SolidWire;
    std::uint32_t lodLevel = 0;
    bool showModel = true;
    bool showWorld = true;
    std::array<float, 3> worldOffset = { 0.0f, 0.0f, 0.0f };
    std::array<float, 4> clearColor = { 0.11f, 0.12f, 0.13f, 1.0f };
};

struct PreviewD3D12RenderStats {
    std::uint64_t frameNumber = 0;
    std::uint64_t modelTriangles = 0;
    std::uint64_t worldTriangles = 0;
    std::uint64_t worldPoints = 0;
    std::uint32_t drawCalls = 0;
    double cpuMilliseconds = 0.0;
};

using PreviewD3D12NativeWindow = void*;

class PreviewD3D12Renderer final {
public:
    PreviewD3D12Renderer();
    ~PreviewD3D12Renderer();

    PreviewD3D12Renderer(const PreviewD3D12Renderer&) = delete;
    PreviewD3D12Renderer& operator=(const PreviewD3D12Renderer&) = delete;
    PreviewD3D12Renderer(PreviewD3D12Renderer&&) noexcept;
    PreviewD3D12Renderer& operator=(PreviewD3D12Renderer&&) noexcept;

    bool Initialize(PreviewD3D12NativeWindow window, const PreviewD3D12Options& options,
                    std::string* errorText);
    bool Resize(std::uint32_t width, std::uint32_t height, std::string* errorText);
    void Shutdown();

    bool UploadMesh(const PreviewMesh& mesh, std::string* errorText);
    bool UploadWorldMesh(const PreviewD3D12WorldMeshView& mesh, std::string* errorText);
    void ClearMesh();
    void ClearWorldMesh();

    bool Render(const PreviewD3D12Frame& frame, PreviewD3D12RenderStats* stats,
                std::string* errorText);

    bool IsReady() const noexcept;
    bool IsHardwareDevice() const noexcept;
    const std::string& AdapterName() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace native_mc
