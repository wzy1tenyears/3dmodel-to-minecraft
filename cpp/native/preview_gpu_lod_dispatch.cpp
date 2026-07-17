#include "preview_gpu_lod.h"

#include "preview_gpu_lod_d3d12.h"

#include <mutex>
#include <string>
#include <utility>

namespace native_mc {
namespace {

std::mutex& GpuLodDispatchMutex() {
    static std::mutex mutex;
    return mutex;
}

PreviewGpuLodResult MarkBackend(
    PreviewGpuLodResult result,
    PreviewGpuLodBackend backend,
    bool usedFallback,
    std::string fallbackReason = {}) {
    result.activeBackend = result.success ? backend : PreviewGpuLodBackend::None;
    result.usedBackendFallback = usedFallback;
    result.fallbackReason = std::move(fallbackReason);
    return result;
}

PreviewGpuLodResult JoinFailures(
    PreviewGpuLodResult d3d12Result,
    PreviewGpuLodResult d3d11Result) {
    const std::string d3d12Error = d3d12Result.error.empty()
        ? "D3D12 compute failed without diagnostics" : d3d12Result.error;
    const std::string d3d11Error = d3d11Result.error.empty()
        ? "D3D11 compute failed without diagnostics" : d3d11Result.error;
    d3d11Result.success = false;
    d3d11Result.activeBackend = PreviewGpuLodBackend::None;
    d3d11Result.usedBackendFallback = true;
    d3d11Result.backendName.clear();
    d3d11Result.fallbackReason = d3d12Error;
    d3d11Result.error = "D3D12: " + d3d12Error + " | D3D11: " + d3d11Error;
    return d3d11Result;
}

}  // namespace

PreviewGpuLodResult BuildPreviewMeshLodsGpu(
    PreviewMesh* mesh,
    const PreviewGpuLodOptions& options) {
    std::lock_guard<std::mutex> lock(GpuLodDispatchMutex());
    PreviewGpuLodResult d3d12Result = BuildPreviewMeshLodsD3D12(mesh, options);
    if (d3d12Result.success) {
        return MarkBackend(std::move(d3d12Result), PreviewGpuLodBackend::D3D12, false);
    }
    const std::string fallbackReason = d3d12Result.error;
    PreviewGpuLodResult d3d11Result = BuildPreviewMeshLodsD3D11(mesh, options);
    if (d3d11Result.success) {
        return MarkBackend(
            std::move(d3d11Result), PreviewGpuLodBackend::D3D11, true, fallbackReason);
    }
    return JoinFailures(std::move(d3d12Result), std::move(d3d11Result));
}

PreviewGpuLodResult BuildPreviewMeshGpuProxies(
    const PreviewMesh& mesh,
    const std::array<std::uint32_t, 4>& resolutions,
    std::array<std::vector<std::uint32_t>, 4>* proxyIndices,
    std::uint64_t maxWorkingSetBytes) {
    std::lock_guard<std::mutex> lock(GpuLodDispatchMutex());
    PreviewGpuLodResult d3d12Result = BuildPreviewMeshGpuProxiesD3D12(
        mesh, resolutions, proxyIndices, maxWorkingSetBytes);
    if (d3d12Result.success) {
        return MarkBackend(std::move(d3d12Result), PreviewGpuLodBackend::D3D12, false);
    }
    const std::string fallbackReason = d3d12Result.error;
    PreviewGpuLodResult d3d11Result = BuildPreviewMeshGpuProxiesD3D11(
        mesh, resolutions, proxyIndices, maxWorkingSetBytes);
    if (d3d11Result.success) {
        return MarkBackend(
            std::move(d3d11Result), PreviewGpuLodBackend::D3D11, true, fallbackReason);
    }
    return JoinFailures(std::move(d3d12Result), std::move(d3d11Result));
}

}  // namespace native_mc
