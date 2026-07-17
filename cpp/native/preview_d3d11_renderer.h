#pragma once

#include "preview_mesh.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace native_mc {

enum class PreviewD3D11DrawMode {
    Solid,
    Wireframe,
    SolidWire,
};

enum class PreviewD3D11WorldPrimitive {
    Points,
    Triangles,
    Quads,
};

struct PreviewD3D11Options {
    bool allowWarpFallback = true;
    bool verticalSync = false;
    std::uint32_t maximumFrameLatency = 1;
};

// This view maps directly to the existing WorldPreviewGpuMesh vectors. The
// current OpenGL builder emits XYZ floats plus three-byte RGB colors and GL
// quads, so callers can pass those buffers without rebuilding the world slice.
struct PreviewD3D11WorldMeshView {
    const float* positions = nullptr;
    std::size_t positionFloatCount = 0;
    const std::uint8_t* colors = nullptr;
    std::size_t colorByteCount = 0;
    std::uint32_t colorChannels = 3;
    PreviewD3D11WorldPrimitive primitive = PreviewD3D11WorldPrimitive::Quads;
};

struct PreviewD3D11Camera {
    float yawDegrees = 35.0f;
    float pitchDegrees = 20.0f;
    float distance = 8.0f;
    float panX = 0.0f;
    float panY = 0.0f;
    float fieldOfViewDegrees = 50.0f;
    float nearPlane = 0.1f;
    float farPlane = 5000.0f;
};

struct PreviewD3D11ModelTransform {
    std::array<float, 3> position = { 0.0f, 0.0f, 0.0f };
    // Source-coordinate conversion and leveling rotation (W, X, Y, Z). It is
    // applied in source-local space before scale and the user quaternion.
    std::array<float, 4> sourceQuaternion = { 1.0f, 0.0f, 0.0f, 0.0f };
    // W, X, Y, Z, matching the GUI's existing quaternion edit order.
    std::array<float, 4> quaternion = { 1.0f, 0.0f, 0.0f, 0.0f };
    std::array<float, 3> scale = { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> center = { 0.0f, 0.0f, 0.0f };
};

struct PreviewD3D11Overlay {
    bool showGrid = true;
    bool showAxes = true;
    float gridStep = 1.0f;
    std::uint32_t gridLineCount = 64;
    std::array<float, 3> gizmoPosition = { 0.0f, 0.0f, 0.0f };
    float gizmoLength = 0.0f;
};

struct PreviewD3D11Frame {
    PreviewD3D11Camera camera;
    PreviewD3D11ModelTransform model;
    PreviewD3D11Overlay overlay;
    PreviewD3D11DrawMode drawMode = PreviewD3D11DrawMode::SolidWire;
    std::uint32_t lodLevel = 0;
    bool showModel = true;
    bool showWorld = true;
    std::array<float, 3> worldOffset = { 0.0f, 0.0f, 0.0f };
    std::array<float, 4> clearColor = { 0.11f, 0.12f, 0.13f, 1.0f };
};

struct PreviewD3D11RenderStats {
    std::uint64_t frameNumber = 0;
    std::uint64_t modelTriangles = 0;
    std::uint64_t worldTriangles = 0;
    std::uint64_t worldPoints = 0;
    std::uint32_t drawCalls = 0;
    double cpuMilliseconds = 0.0;
};

// Native-window handle is HWND on Windows. It is kept opaque here so the
// public header does not force Windows.h (and its min/max macros) on importers.
using PreviewD3D11NativeWindow = void*;

class PreviewD3D11Renderer final {
public:
    PreviewD3D11Renderer();
    ~PreviewD3D11Renderer();

    PreviewD3D11Renderer(const PreviewD3D11Renderer&) = delete;
    PreviewD3D11Renderer& operator=(const PreviewD3D11Renderer&) = delete;
    PreviewD3D11Renderer(PreviewD3D11Renderer&&) noexcept;
    PreviewD3D11Renderer& operator=(PreviewD3D11Renderer&&) noexcept;

    bool Initialize(PreviewD3D11NativeWindow window, const PreviewD3D11Options& options,
                    std::string* errorText);
    bool Resize(std::uint32_t width, std::uint32_t height, std::string* errorText);
    void Shutdown();

    // Uploads interleaved immutable vertex data and all four LOD index buffers.
    // The source PreviewMesh remains owned by the caller and can be released
    // immediately after this call returns.
    bool UploadMesh(const PreviewMesh& mesh, std::string* errorText);
    bool UploadWorldMesh(const PreviewD3D11WorldMeshView& mesh, std::string* errorText);
    void ClearMesh();
    void ClearWorldMesh();

    bool Render(const PreviewD3D11Frame& frame, PreviewD3D11RenderStats* stats,
                std::string* errorText);

    bool IsReady() const noexcept;
    bool IsHardwareDevice() const noexcept;
    const std::string& AdapterName() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace native_mc
