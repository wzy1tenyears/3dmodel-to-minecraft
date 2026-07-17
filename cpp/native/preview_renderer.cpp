#include "preview_renderer.h"

#include "preview_d3d11_renderer.h"
#include "preview_d3d12_renderer.h"
#include "preview_vulkan_renderer.h"

#include <algorithm>
#include <sstream>
#include <utility>
#include <vector>

namespace native_mc {
namespace {

void SetOutputError(std::string* output, const std::string& message) {
    if (output) *output = message;
}

std::string JoinFailures(const std::vector<std::string>& failures) {
    std::ostringstream output;
    for (std::size_t index = 0; index < failures.size(); ++index) {
        if (index) output << " | ";
        output << failures[index];
    }
    return output.str();
}

template <typename Camera>
Camera ConvertCamera(const PreviewRendererCamera& source) {
    Camera output;
    output.yawDegrees = source.yawDegrees;
    output.pitchDegrees = source.pitchDegrees;
    output.distance = source.distance;
    output.panX = source.panX;
    output.panY = source.panY;
    output.fieldOfViewDegrees = source.fieldOfViewDegrees;
    output.nearPlane = source.nearPlane;
    output.farPlane = source.farPlane;
    return output;
}

template <typename Transform>
Transform ConvertTransform(const PreviewRendererModelTransform& source) {
    Transform output;
    output.position = source.position;
    output.sourceQuaternion = source.sourceQuaternion;
    output.quaternion = source.quaternion;
    output.scale = source.scale;
    output.center = source.center;
    return output;
}

template <typename Overlay>
Overlay ConvertOverlay(const PreviewRendererOverlay& source) {
    Overlay output;
    output.showGrid = source.showGrid;
    output.showAxes = source.showAxes;
    output.gridStep = source.gridStep;
    output.gridLineCount = source.gridLineCount;
    output.gizmoPosition = source.gizmoPosition;
    output.gizmoLength = source.gizmoLength;
    return output;
}

PreviewD3D11DrawMode ConvertD3D11DrawMode(PreviewRendererDrawMode mode) {
    switch (mode) {
    case PreviewRendererDrawMode::Solid: return PreviewD3D11DrawMode::Solid;
    case PreviewRendererDrawMode::Wireframe: return PreviewD3D11DrawMode::Wireframe;
    case PreviewRendererDrawMode::SolidWire: return PreviewD3D11DrawMode::SolidWire;
    }
    return PreviewD3D11DrawMode::SolidWire;
}

PreviewD3D12DrawMode ConvertD3D12DrawMode(PreviewRendererDrawMode mode) {
    switch (mode) {
    case PreviewRendererDrawMode::Solid: return PreviewD3D12DrawMode::Solid;
    case PreviewRendererDrawMode::Wireframe: return PreviewD3D12DrawMode::Wireframe;
    case PreviewRendererDrawMode::SolidWire: return PreviewD3D12DrawMode::SolidWire;
    }
    return PreviewD3D12DrawMode::SolidWire;
}

PreviewVulkanDrawMode ConvertVulkanDrawMode(PreviewRendererDrawMode mode) {
    switch (mode) {
    case PreviewRendererDrawMode::Solid: return PreviewVulkanDrawMode::Solid;
    case PreviewRendererDrawMode::Wireframe: return PreviewVulkanDrawMode::Wireframe;
    case PreviewRendererDrawMode::SolidWire: return PreviewVulkanDrawMode::SolidWire;
    }
    return PreviewVulkanDrawMode::SolidWire;
}

PreviewD3D11WorldPrimitive ConvertD3D11Primitive(PreviewRendererWorldPrimitive primitive) {
    switch (primitive) {
    case PreviewRendererWorldPrimitive::Points: return PreviewD3D11WorldPrimitive::Points;
    case PreviewRendererWorldPrimitive::Triangles: return PreviewD3D11WorldPrimitive::Triangles;
    case PreviewRendererWorldPrimitive::Quads: return PreviewD3D11WorldPrimitive::Quads;
    }
    return PreviewD3D11WorldPrimitive::Quads;
}

PreviewD3D12WorldPrimitive ConvertD3D12Primitive(PreviewRendererWorldPrimitive primitive) {
    switch (primitive) {
    case PreviewRendererWorldPrimitive::Points: return PreviewD3D12WorldPrimitive::Points;
    case PreviewRendererWorldPrimitive::Triangles: return PreviewD3D12WorldPrimitive::Triangles;
    case PreviewRendererWorldPrimitive::Quads: return PreviewD3D12WorldPrimitive::Quads;
    }
    return PreviewD3D12WorldPrimitive::Quads;
}

PreviewVulkanWorldPrimitive ConvertVulkanPrimitive(PreviewRendererWorldPrimitive primitive) {
    switch (primitive) {
    case PreviewRendererWorldPrimitive::Points: return PreviewVulkanWorldPrimitive::Points;
    case PreviewRendererWorldPrimitive::Triangles: return PreviewVulkanWorldPrimitive::Triangles;
    case PreviewRendererWorldPrimitive::Quads: return PreviewVulkanWorldPrimitive::Quads;
    }
    return PreviewVulkanWorldPrimitive::Quads;
}

template <typename BackendStats>
void CopyStats(const BackendStats& source, PreviewRendererBackend backend,
               PreviewRendererStats* output) {
    if (!output) return;
    output->backend = backend;
    output->frameNumber = source.frameNumber;
    output->modelTriangles = source.modelTriangles;
    output->worldTriangles = source.worldTriangles;
    output->worldPoints = source.worldPoints;
    output->drawCalls = source.drawCalls;
    output->cpuMilliseconds = source.cpuMilliseconds;
}

}  // namespace

const char* PreviewRendererBackendName(PreviewRendererBackend backend) noexcept {
    switch (backend) {
    case PreviewRendererBackend::None: return "None";
    case PreviewRendererBackend::Auto: return "Auto";
    case PreviewRendererBackend::D3D11: return "D3D11";
    case PreviewRendererBackend::D3D12: return "D3D12";
    case PreviewRendererBackend::Vulkan: return "Vulkan";
    case PreviewRendererBackend::OpenGL: return "OpenGL";
    }
    return "Unknown";
}

struct PreviewRenderer::Impl {
    PreviewRendererOptions options{};
    PreviewRendererBackend requestedBackend = PreviewRendererBackend::None;
    PreviewRendererBackend activeBackend = PreviewRendererBackend::None;
    std::string adapterName;
    std::string lastError;
    std::unique_ptr<PreviewD3D11Renderer> d3d11;
    std::unique_ptr<PreviewD3D12Renderer> d3d12;
    std::unique_ptr<PreviewVulkanRenderer> vulkan;

    void ResetBackend() {
        if (d3d11) d3d11->Shutdown();
        if (d3d12) d3d12->Shutdown();
        if (vulkan) vulkan->Shutdown();
        d3d11.reset();
        d3d12.reset();
        vulkan.reset();
        activeBackend = PreviewRendererBackend::None;
        adapterName.clear();
    }

    bool TryInitialize(PreviewRendererBackend backend, PreviewRendererNativeWindow window,
                       bool allowSoftware, bool requireHardware, std::string* failure) {
        ResetBackend();
        std::string error;
        switch (backend) {
        case PreviewRendererBackend::D3D11: {
            auto renderer = std::make_unique<PreviewD3D11Renderer>();
            PreviewD3D11Options backendOptions;
            backendOptions.allowWarpFallback = allowSoftware;
            backendOptions.verticalSync = options.verticalSync;
            backendOptions.maximumFrameLatency = options.maximumFrameLatency;
            if (!renderer->Initialize(window, backendOptions, &error)) break;
            if (requireHardware && !renderer->IsHardwareDevice()) {
                error = "initialized a software device while a hardware device was required";
                renderer->Shutdown();
                break;
            }
            adapterName = renderer->AdapterName();
            d3d11 = std::move(renderer);
            activeBackend = backend;
            return true;
        }
        case PreviewRendererBackend::D3D12: {
            auto renderer = std::make_unique<PreviewD3D12Renderer>();
            PreviewD3D12Options backendOptions;
            backendOptions.allowWarpFallback = allowSoftware;
            backendOptions.verticalSync = options.verticalSync;
            backendOptions.maximumFrameLatency = options.maximumFrameLatency;
            if (!renderer->Initialize(window, backendOptions, &error)) break;
            if (requireHardware && !renderer->IsHardwareDevice()) {
                error = "initialized a software device while a hardware device was required";
                renderer->Shutdown();
                break;
            }
            adapterName = renderer->AdapterName();
            d3d12 = std::move(renderer);
            activeBackend = backend;
            return true;
        }
        case PreviewRendererBackend::Vulkan: {
            auto renderer = std::make_unique<PreviewVulkanRenderer>();
            PreviewVulkanOptions backendOptions;
            backendOptions.verticalSync = options.verticalSync;
            backendOptions.preferDiscreteGpu = options.preferDiscreteGpu;
            backendOptions.framesInFlight = options.framesInFlight;
            if (!renderer->Initialize(window, backendOptions, &error)) break;
            if (requireHardware && !renderer->IsHardwareDevice()) {
                error = "initialized a software device while a hardware device was required";
                renderer->Shutdown();
                break;
            }
            adapterName = renderer->AdapterName();
            vulkan = std::move(renderer);
            activeBackend = backend;
            return true;
        }
        case PreviewRendererBackend::OpenGL:
            error = "OpenGL rendering is owned by main.cpp and is not created by PreviewRenderer";
            break;
        case PreviewRendererBackend::Auto:
        case PreviewRendererBackend::None:
            error = "backend cannot be initialized directly";
            break;
        }
        if (error.empty()) error = "backend initialization failed without an error message";
        if (failure) *failure = std::string(PreviewRendererBackendName(backend)) + ": " + error;
        ResetBackend();
        return false;
    }

    bool FinishCall(bool success, const std::string& error, std::string* errorText) {
        if (success) {
            lastError.clear();
            SetOutputError(errorText, {});
            return true;
        }
        lastError = std::string(PreviewRendererBackendName(activeBackend)) + ": " +
            (error.empty() ? "operation failed without an error message" : error);
        SetOutputError(errorText, lastError);
        return false;
    }
};

PreviewRenderer::PreviewRenderer() : impl_(std::make_unique<Impl>()) {}
PreviewRenderer::~PreviewRenderer() { Shutdown(); }
PreviewRenderer::PreviewRenderer(PreviewRenderer&&) noexcept = default;
PreviewRenderer& PreviewRenderer::operator=(PreviewRenderer&&) noexcept = default;

bool PreviewRenderer::Initialize(PreviewRendererNativeWindow window,
                                 const PreviewRendererOptions& options,
                                 std::string* errorText) {
    Shutdown();
    impl_ = std::make_unique<Impl>();
    impl_->options = options;
    impl_->requestedBackend = options.backend;
    if (!window) {
        impl_->lastError = "PreviewRenderer requires a valid native window";
        SetOutputError(errorText, impl_->lastError);
        return false;
    }
    if (options.backend == PreviewRendererBackend::None) {
        impl_->lastError = "None is not an initializable preview renderer backend";
        SetOutputError(errorText, impl_->lastError);
        return false;
    }
    if (options.backend == PreviewRendererBackend::OpenGL) {
        impl_->lastError = "OpenGL rendering is owned by main.cpp; use its existing OpenGL path";
        SetOutputError(errorText, impl_->lastError);
        return false;
    }

    if (options.backend != PreviewRendererBackend::Auto) {
        std::string failure;
        const bool requireHardware = !options.allowSoftwareFallback;
        if (impl_->TryInitialize(options.backend, window, options.allowSoftwareFallback,
                                 requireHardware, &failure)) {
            impl_->lastError.clear();
            SetOutputError(errorText, {});
            return true;
        }
        impl_->lastError = failure;
        SetOutputError(errorText, failure);
        return false;
    }

    std::vector<std::string> failures;
    for (const PreviewRendererBackend backend : {
            PreviewRendererBackend::D3D12,
            PreviewRendererBackend::Vulkan,
            PreviewRendererBackend::D3D11 }) {
        std::string failure;
        if (impl_->TryInitialize(backend, window, false, true, &failure)) {
            impl_->lastError = JoinFailures(failures);
            SetOutputError(errorText, {});
            return true;
        }
        failures.push_back(std::move(failure));
    }
    if (options.allowSoftwareFallback) {
        std::string failure;
        if (impl_->TryInitialize(PreviewRendererBackend::D3D11, window, true, false, &failure)) {
            impl_->lastError = JoinFailures(failures);
            SetOutputError(errorText, {});
            return true;
        }
        failures.push_back("D3D11 software fallback: " + failure);
    }
    impl_->lastError = JoinFailures(failures);
    SetOutputError(errorText, impl_->lastError);
    return false;
}

bool PreviewRenderer::Resize(std::uint32_t width, std::uint32_t height,
                             std::string* errorText) {
    if (!impl_) {
        SetOutputError(errorText, "PreviewRenderer is not initialized");
        return false;
    }
    std::string error;
    bool success = false;
    switch (impl_->activeBackend) {
    case PreviewRendererBackend::D3D11: success = impl_->d3d11->Resize(width, height, &error); break;
    case PreviewRendererBackend::D3D12: success = impl_->d3d12->Resize(width, height, &error); break;
    case PreviewRendererBackend::Vulkan: success = impl_->vulkan->Resize(width, height, &error); break;
    default: error = "no native preview renderer backend is active"; break;
    }
    return impl_->FinishCall(success, error, errorText);
}

void PreviewRenderer::Shutdown() {
    if (!impl_) return;
    impl_->ResetBackend();
    impl_.reset();
}

bool PreviewRenderer::UploadMesh(const PreviewMesh& mesh, std::string* errorText) {
    if (!impl_) {
        SetOutputError(errorText, "PreviewRenderer is not initialized");
        return false;
    }
    std::string error;
    bool success = false;
    switch (impl_->activeBackend) {
    case PreviewRendererBackend::D3D11: success = impl_->d3d11->UploadMesh(mesh, &error); break;
    case PreviewRendererBackend::D3D12: success = impl_->d3d12->UploadMesh(mesh, &error); break;
    case PreviewRendererBackend::Vulkan: success = impl_->vulkan->UploadMesh(mesh, &error); break;
    default: error = "no native preview renderer backend is active"; break;
    }
    return impl_->FinishCall(success, error, errorText);
}

bool PreviewRenderer::UploadWorldMesh(const PreviewRendererWorldMeshView& mesh,
                                      std::string* errorText) {
    if (!impl_) {
        SetOutputError(errorText, "PreviewRenderer is not initialized");
        return false;
    }
    std::string error;
    bool success = false;
    switch (impl_->activeBackend) {
    case PreviewRendererBackend::D3D11: {
        PreviewD3D11WorldMeshView view;
        view.positions = mesh.positions;
        view.positionFloatCount = mesh.positionFloatCount;
        view.colors = mesh.colors;
        view.colorByteCount = mesh.colorByteCount;
        view.colorChannels = mesh.colorChannels;
        view.primitive = ConvertD3D11Primitive(mesh.primitive);
        success = impl_->d3d11->UploadWorldMesh(view, &error);
        break;
    }
    case PreviewRendererBackend::D3D12: {
        PreviewD3D12WorldMeshView view;
        view.positions = mesh.positions;
        view.positionFloatCount = mesh.positionFloatCount;
        view.colors = mesh.colors;
        view.colorByteCount = mesh.colorByteCount;
        view.colorChannels = mesh.colorChannels;
        view.primitive = ConvertD3D12Primitive(mesh.primitive);
        success = impl_->d3d12->UploadWorldMesh(view, &error);
        break;
    }
    case PreviewRendererBackend::Vulkan: {
        PreviewVulkanWorldMeshView view;
        view.positions = mesh.positions;
        view.positionFloatCount = mesh.positionFloatCount;
        view.colors = mesh.colors;
        view.colorByteCount = mesh.colorByteCount;
        view.colorChannels = mesh.colorChannels;
        view.primitive = ConvertVulkanPrimitive(mesh.primitive);
        success = impl_->vulkan->UploadWorldMesh(view, &error);
        break;
    }
    default: error = "no native preview renderer backend is active"; break;
    }
    return impl_->FinishCall(success, error, errorText);
}

void PreviewRenderer::ClearMesh() {
    if (!impl_) return;
    switch (impl_->activeBackend) {
    case PreviewRendererBackend::D3D11: impl_->d3d11->ClearMesh(); break;
    case PreviewRendererBackend::D3D12: impl_->d3d12->ClearMesh(); break;
    case PreviewRendererBackend::Vulkan: impl_->vulkan->ClearMesh(); break;
    default: break;
    }
}

void PreviewRenderer::ClearWorldMesh() {
    if (!impl_) return;
    switch (impl_->activeBackend) {
    case PreviewRendererBackend::D3D11: impl_->d3d11->ClearWorldMesh(); break;
    case PreviewRendererBackend::D3D12: impl_->d3d12->ClearWorldMesh(); break;
    case PreviewRendererBackend::Vulkan: impl_->vulkan->ClearWorldMesh(); break;
    default: break;
    }
}

bool PreviewRenderer::Render(const PreviewRendererFrame& frame,
                             PreviewRendererStats* stats,
                             std::string* errorText) {
    if (stats) *stats = PreviewRendererStats{};
    if (!impl_) {
        SetOutputError(errorText, "PreviewRenderer is not initialized");
        return false;
    }
    std::string error;
    bool success = false;
    switch (impl_->activeBackend) {
    case PreviewRendererBackend::D3D11: {
        PreviewD3D11Frame backendFrame;
        backendFrame.camera = ConvertCamera<PreviewD3D11Camera>(frame.camera);
        backendFrame.model = ConvertTransform<PreviewD3D11ModelTransform>(frame.model);
        backendFrame.overlay = ConvertOverlay<PreviewD3D11Overlay>(frame.overlay);
        backendFrame.drawMode = ConvertD3D11DrawMode(frame.drawMode);
        backendFrame.lodLevel = frame.lodLevel;
        backendFrame.showModel = frame.showModel;
        backendFrame.showWorld = frame.showWorld;
        backendFrame.worldOffset = frame.worldOffset;
        backendFrame.clearColor = frame.clearColor;
        PreviewD3D11RenderStats backendStats;
        success = impl_->d3d11->Render(backendFrame, &backendStats, &error);
        if (success) CopyStats(backendStats, impl_->activeBackend, stats);
        break;
    }
    case PreviewRendererBackend::D3D12: {
        PreviewD3D12Frame backendFrame;
        backendFrame.camera = ConvertCamera<PreviewD3D12Camera>(frame.camera);
        backendFrame.model = ConvertTransform<PreviewD3D12ModelTransform>(frame.model);
        backendFrame.overlay = ConvertOverlay<PreviewD3D12Overlay>(frame.overlay);
        backendFrame.drawMode = ConvertD3D12DrawMode(frame.drawMode);
        backendFrame.lodLevel = frame.lodLevel;
        backendFrame.showModel = frame.showModel;
        backendFrame.showWorld = frame.showWorld;
        backendFrame.worldOffset = frame.worldOffset;
        backendFrame.clearColor = frame.clearColor;
        PreviewD3D12RenderStats backendStats;
        success = impl_->d3d12->Render(backendFrame, &backendStats, &error);
        if (success) CopyStats(backendStats, impl_->activeBackend, stats);
        break;
    }
    case PreviewRendererBackend::Vulkan: {
        PreviewVulkanFrame backendFrame;
        backendFrame.camera = ConvertCamera<PreviewVulkanCamera>(frame.camera);
        backendFrame.model = ConvertTransform<PreviewVulkanModelTransform>(frame.model);
        backendFrame.overlay = ConvertOverlay<PreviewVulkanOverlay>(frame.overlay);
        backendFrame.drawMode = ConvertVulkanDrawMode(frame.drawMode);
        backendFrame.lodLevel = frame.lodLevel;
        backendFrame.showModel = frame.showModel;
        backendFrame.showWorld = frame.showWorld;
        backendFrame.worldOffset = frame.worldOffset;
        backendFrame.clearColor = frame.clearColor;
        PreviewVulkanRenderStats backendStats;
        success = impl_->vulkan->Render(backendFrame, &backendStats, &error);
        if (success) CopyStats(backendStats, impl_->activeBackend, stats);
        break;
    }
    default: error = "no native preview renderer backend is active"; break;
    }
    return impl_->FinishCall(success, error, errorText);
}

bool PreviewRenderer::IsReady() const noexcept {
    if (!impl_) return false;
    switch (impl_->activeBackend) {
    case PreviewRendererBackend::D3D11: return impl_->d3d11 && impl_->d3d11->IsReady();
    case PreviewRendererBackend::D3D12: return impl_->d3d12 && impl_->d3d12->IsReady();
    case PreviewRendererBackend::Vulkan: return impl_->vulkan && impl_->vulkan->IsReady();
    default: return false;
    }
}

bool PreviewRenderer::IsHardware() const noexcept {
    if (!impl_) return false;
    switch (impl_->activeBackend) {
    case PreviewRendererBackend::D3D11: return impl_->d3d11 && impl_->d3d11->IsHardwareDevice();
    case PreviewRendererBackend::D3D12: return impl_->d3d12 && impl_->d3d12->IsHardwareDevice();
    case PreviewRendererBackend::Vulkan: return impl_->vulkan && impl_->vulkan->IsHardwareDevice();
    default: return false;
    }
}

PreviewRendererBackend PreviewRenderer::RequestedBackend() const noexcept {
    return impl_ ? impl_->requestedBackend : PreviewRendererBackend::None;
}

PreviewRendererBackend PreviewRenderer::ActiveBackend() const noexcept {
    return impl_ ? impl_->activeBackend : PreviewRendererBackend::None;
}

const std::string& PreviewRenderer::AdapterName() const noexcept {
    static const std::string empty;
    return impl_ ? impl_->adapterName : empty;
}

const std::string& PreviewRenderer::LastError() const noexcept {
    static const std::string empty;
    return impl_ ? impl_->lastError : empty;
}

}  // namespace native_mc
