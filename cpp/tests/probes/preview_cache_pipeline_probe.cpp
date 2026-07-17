#include "native/preview_cache.h"
#include "native/preview_gpu_lod.h"
#include "native/preview_mesh.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr std::array<std::uint32_t, 4> kResolutions = { 64, 48, 32, 24 };
constexpr char kProfile[] = "overview-tile-proxy-v4-oriented-monotonic-r64-48-32-24";

bool Compact(native_mc::PreviewMesh* mesh) {
    const std::size_t vertexCount = mesh->positions.size() / 3;
    std::vector<std::uint32_t> remap(vertexCount, std::numeric_limits<std::uint32_t>::max());
    std::vector<std::uint32_t> used;
    const std::array<const std::vector<std::uint32_t>*, 4> levels = {
        &mesh->indices, &mesh->lodIndices[1], &mesh->lodIndices[2], &mesh->lodIndices[3]
    };
    for (const auto* level : levels) {
        for (const std::uint32_t index : *level) {
            if (index >= vertexCount || remap[index] != std::numeric_limits<std::uint32_t>::max()) continue;
            remap[index] = 0;
            used.push_back(index);
        }
    }

    native_mc::PreviewMesh compact;
    compact.min = mesh->min;
    compact.max = mesh->max;
    for (const std::uint32_t oldIndex : used) {
        const std::uint32_t newIndex = static_cast<std::uint32_t>(compact.positions.size() / 3);
        remap[oldIndex] = newIndex;
        compact.positions.insert(compact.positions.end(), mesh->positions.begin() + oldIndex * 3,
                                 mesh->positions.begin() + oldIndex * 3 + 3);
        compact.normals.insert(compact.normals.end(), mesh->normals.begin() + oldIndex * 3,
                               mesh->normals.begin() + oldIndex * 3 + 3);
        compact.colors.insert(compact.colors.end(), mesh->colors.begin() + oldIndex * 4,
                              mesh->colors.begin() + oldIndex * 4 + 4);
    }
    for (std::size_t level = 0; level < levels.size(); ++level) {
        auto& output = level == 0 ? compact.indices : compact.lodIndices[level];
        output.reserve(levels[level]->size());
        for (const std::uint32_t index : *levels[level]) output.push_back(remap[index]);
    }
    *mesh = std::move(compact);
    return !mesh->indices.empty();
}

void PrintMesh(const char* label, const native_mc::PreviewMesh& mesh) {
    std::cout << label << " vertices=" << mesh.positions.size() / 3
              << " triangles=" << mesh.indices.size() / 3 << " lod=";
    for (std::size_t i = 0; i < mesh.lodIndices.size(); ++i) {
        if (i) std::cout << '/';
        std::cout << mesh.lodIndices[i].size() / 3;
    }
    std::cout << '\n';
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    const std::filesystem::path source(argv[1]);
    const std::filesystem::path cacheDirectory(argv[2]);
    native_mc::PreviewMeshCacheSourceStamp stamp;
    std::string error;
    if (!native_mc::CapturePreviewMeshCacheSourceStamp(source, {}, &stamp, &error, kProfile)) {
        std::cerr << error << '\n';
        return 1;
    }

    native_mc::PreviewMesh mesh;
    auto cached = native_mc::LoadPreviewMeshCache(cacheDirectory, stamp, &mesh);
    std::cout << "initial_cache_status=" << static_cast<int>(cached.status)
              << " backend=" << native_mc::PreviewFileReadBackendName(cached.fileRead.backend)
              << " read_ms=" << cached.fileRead.elapsedMilliseconds << '\n';
    if (!cached.hit()) {
        const auto started = std::chrono::steady_clock::now();
        if (!native_mc::LoadPreviewMesh(source, &mesh, &error, false)) {
            std::cerr << error << '\n';
            return 1;
        }
        std::array<std::vector<std::uint32_t>, 4> proxies;
        const auto gpu = native_mc::BuildPreviewMeshGpuProxiesD3D11(mesh, kResolutions, &proxies);
        if (!gpu.success) proxies = native_mc::BuildPreviewMeshProxyIndices(mesh, kResolutions);
        mesh.indices = std::move(proxies[0]);
        mesh.lodIndices[0].clear();
        for (std::size_t level = 1; level < proxies.size(); ++level) {
            mesh.lodIndices[level] = std::move(proxies[level]);
        }
        if (!Compact(&mesh) || !native_mc::SavePreviewMeshCache(cacheDirectory, stamp, mesh, &error)) {
            std::cerr << error << '\n';
            return 1;
        }
        const auto finished = std::chrono::steady_clock::now();
        std::cout << "generated_backend=" << (gpu.success ? gpu.backendName : "CPU fallback")
                  << " total_ms=" << std::chrono::duration<double, std::milli>(finished - started).count() << '\n';
        PrintMesh("generated", mesh);
    }

    native_mc::PreviewMesh loaded;
    const auto second = native_mc::LoadPreviewMeshCache(cacheDirectory, stamp, &loaded);
    std::cout << "second_cache_status=" << static_cast<int>(second.status)
              << " backend=" << native_mc::PreviewFileReadBackendName(second.fileRead.backend)
              << " fallback=" << second.fileRead.usedFallback
              << " bytes=" << second.fileRead.bytesRead
              << " read_ms=" << second.fileRead.elapsedMilliseconds
              << " message=" << second.message << '\n';
    if (!second.hit()) return 1;
    PrintMesh("loaded", loaded);
    return 0;
}
