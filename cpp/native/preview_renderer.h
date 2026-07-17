#pragma once

#include "preview_mesh.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace native_mc {

enum class PreviewRendererBackend {
    None,
    Auto,
    D3D11,
    D3D12,
    Vulkan,
    OpenGL,
};

const char* PreviewRendererBackendName(PreviewRendererBackend backend) noexcept;

enum class PreviewRendererDrawMode {
    Solid,
    Wireframe,
    SolidWire,
};

enum class PreviewRendererWorldPrimitive {
    Points,
    Triangles,
    Quads,
};

struct PreviewRendererOptions {
    PreviewRendererBackend backend = PreviewRendererBackend::Auto;
    bool verticalSync = false;
    bool allowSoftwareFallback = true;
    bool preferDiscreteGpu = true;
    std::uint32_t maximumFrameLatency = 1;
    std::uint32_t framesInFlight = 2;
};

// Maps directly to the existing WorldPreviewGpuMesh vectors. OpenGL's
// GL_POINTS maps to Points; its generated GL_QUADS map to Quads.
struct PreviewRendererWorldMeshView {
    const float* positions = nullptr;
    std::size_t positionFloatCount = 0;
    const std::uint8_t* colors = nullptr;
    std::size_t colorByteCount = 0;
    std::uint32_t colorChannels = 3;
    PreviewRendererWorldPrimitive primitive = PreviewRendererWorldPrimitive::Quads;
};

struct PreviewRendererCamera {
    float yawDegrees = 35.0f;
    float pitchDegrees = 20.0f;
    float distance = 8.0f;
    float panX = 0.0f;
    float panY = 0.0f;
    float fieldOfViewDegrees = 50.0f;
    float nearPlane = 0.1f;
    float farPlane = 5000.0f;
};

struct PreviewRendererModelTransform {
    std::array<float, 3> position = { 0.0f, 0.0f, 0.0f };
    // Both quaternions use W, X, Y, Z. Backends apply sourceQuaternion first
    // in source-local space, then signed scale, then the user quaternion.
    std::array<float, 4> sourceQuaternion = { 1.0f, 0.0f, 0.0f, 0.0f };
    std::array<float, 4> quaternion = { 1.0f, 0.0f, 0.0f, 0.0f };
    std::array<float, 3> scale = { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> center = { 0.0f, 0.0f, 0.0f };
};

struct PreviewRendererOverlay {
    bool showGrid = true;
    bool showAxes = true;
    float gridStep = 1.0f;
    std::uint32_t gridLineCount = 64;
    std::array<float, 3> gizmoPosition = { 0.0f, 0.0f, 0.0f };
    float gizmoLength = 0.0f;
};

struct PreviewRendererFrame {
    PreviewRendererCamera camera;
    PreviewRendererModelTransform model;
    PreviewRendererOverlay overlay;
    PreviewRendererDrawMode drawMode = PreviewRendererDrawMode::SolidWire;
    std::uint32_t lodLevel = 0;
    bool showModel = true;
    bool showWorld = true;
    std::array<float, 3> worldOffset = { 0.0f, 0.0f, 0.0f };
    std::array<float, 4> clearColor = { 0.11f, 0.12f, 0.13f, 1.0f };
};

struct PreviewRendererStats {
    PreviewRendererBackend backend = PreviewRendererBackend::None;
    std::uint64_t frameNumber = 0;
    std::uint64_t modelTriangles = 0;
    std::uint64_t worldTriangles = 0;
    std::uint64_t worldPoints = 0;
    std::uint32_t drawCalls = 0;
    double cpuMilliseconds = 0.0;
};

using PreviewRendererNativeWindow = void*;

// The adapter owns exactly one native backend at a time. Auto probes hardware
// in D3D12 -> Vulkan -> D3D11 order. A software D3D11 WARP attempt is made only
// after every hardware backend fails and allowSoftwareFallback is true.
// OpenGL remains owned by main.cpp; requesting it returns a clear unsupported
// result so the caller can continue through the existing OpenGL path.
class PreviewRenderer final {
public:
    PreviewRenderer();
    ~PreviewRenderer();

    PreviewRenderer(const PreviewRenderer&) = delete;
    PreviewRenderer& operator=(const PreviewRenderer&) = delete;
    PreviewRenderer(PreviewRenderer&&) noexcept;
    PreviewRenderer& operator=(PreviewRenderer&&) noexcept;

    bool Initialize(PreviewRendererNativeWindow window, const PreviewRendererOptions& options,
                    std::string* errorText);
    bool Resize(std::uint32_t width, std::uint32_t height, std::string* errorText);
    void Shutdown();

    bool UploadMesh(const PreviewMesh& mesh, std::string* errorText);
    bool UploadWorldMesh(const PreviewRendererWorldMeshView& mesh, std::string* errorText);
    void ClearMesh();
    void ClearWorldMesh();

    bool Render(const PreviewRendererFrame& frame, PreviewRendererStats* stats,
                std::string* errorText);

    bool IsReady() const noexcept;
    bool IsHardware() const noexcept;
    PreviewRendererBackend RequestedBackend() const noexcept;
    PreviewRendererBackend ActiveBackend() const noexcept;
    const std::string& AdapterName() const noexcept;
    // On successful Auto fallback this retains the failed earlier attempts as
    // diagnostics; it is empty when the first requested backend succeeds.
    const std::string& LastError() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace native_mc
