#pragma once

#include "preview_gpu_lod.h"

namespace native_mc {

// D3D12 equivalents of the D3D11 GPU LOD entry points. The shared compute
// runtime is initialized lazily on the high-performance hardware adapter.
// Calls are thread-safe and leave caller-owned output unchanged on failure.
PreviewGpuLodResult BuildPreviewMeshLodsD3D12(
    PreviewMesh* mesh,
    const PreviewGpuLodOptions& options = PreviewGpuLodOptions{});

PreviewGpuLodResult BuildPreviewMeshGpuProxiesD3D12(
    const PreviewMesh& mesh,
    const std::array<std::uint32_t, 4>& resolutions,
    std::array<std::vector<std::uint32_t>, 4>* proxyIndices,
    std::uint64_t maxWorkingSetBytes = 768ull * 1024ull * 1024ull);

}  // namespace native_mc
