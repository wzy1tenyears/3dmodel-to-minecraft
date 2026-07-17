#pragma once

#include "preview_mesh.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace native_mc {

// GPU LOD generation is intentionally scoped to one mesh. Call this once for
// each source Tile before combining meshes so clustering uses the Tile's AABB.
struct PreviewGpuLodOptions {
    std::array<std::uint32_t, 4> resolutions = { 0, 192, 96, 48 };

    // Peak memory includes input buffers, cluster state, remapped indices and
    // the readback buffer. Set to zero to disable this caller-defined limit.
    std::uint64_t maxWorkingSetBytes = 768ull * 1024ull * 1024ull;
};

enum class PreviewGpuLodBackend {
    None,
    D3D11,
    D3D12,
};

struct PreviewGpuLodResult {
    bool success = false;
    PreviewGpuLodBackend activeBackend = PreviewGpuLodBackend::None;
    bool usedBackendFallback = false;
    std::string backendName;
    std::string fallbackReason;
    double elapsedMilliseconds = 0.0;
    std::array<double, 4> levelMilliseconds{};
    std::array<std::size_t, 4> triangleCounts{};
    std::string error;
};

// Generates lodIndices[1..3] without replacing the source vertex/index data.
// The shared D3D11 device and compute shaders are initialized lazily and reused
// across calls. Calls are thread-safe; GPU work is serialized on one context.
// On failure the mesh is left unchanged so the caller can use the CPU path.
PreviewGpuLodResult BuildPreviewMeshLodsD3D11(
    PreviewMesh* mesh,
    const PreviewGpuLodOptions& options = PreviewGpuLodOptions{});

// Builds four custom-resolution proxy index buffers for one source Tile. This
// is intended for an all-model overview where even proxy level 0 must be much
// smaller than the complete Tile. The source mesh and output are unchanged on
// failure. The returned result contains timings and triangle counts per proxy.
PreviewGpuLodResult BuildPreviewMeshGpuProxiesD3D11(
    const PreviewMesh& mesh,
    const std::array<std::uint32_t, 4>& resolutions,
    std::array<std::vector<std::uint32_t>, 4>* proxyIndices,
    std::uint64_t maxWorkingSetBytes = 768ull * 1024ull * 1024ull);

// Uses the hardware D3D12 compute path first, then D3D11. The caller can fall
// back to the CPU implementation only when both GPU backends fail.
PreviewGpuLodResult BuildPreviewMeshLodsGpu(
    PreviewMesh* mesh,
    const PreviewGpuLodOptions& options = PreviewGpuLodOptions{});

PreviewGpuLodResult BuildPreviewMeshGpuProxies(
    const PreviewMesh& mesh,
    const std::array<std::uint32_t, 4>& resolutions,
    std::array<std::vector<std::uint32_t>, 4>* proxyIndices,
    std::uint64_t maxWorkingSetBytes = 768ull * 1024ull * 1024ull);

}  // namespace native_mc
