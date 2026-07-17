#include "native/preview_gpu_lod.h"
#include "native/preview_mesh.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

double TriangleArea(const native_mc::PreviewMesh& mesh, std::uint32_t ia, std::uint32_t ib, std::uint32_t ic) {
    const auto vertexCount = mesh.positions.size() / 3;
    if (ia >= vertexCount || ib >= vertexCount || ic >= vertexCount) return 0.0;
    const float* a = mesh.positions.data() + static_cast<std::size_t>(ia) * 3;
    const float* b = mesh.positions.data() + static_cast<std::size_t>(ib) * 3;
    const float* c = mesh.positions.data() + static_cast<std::size_t>(ic) * 3;
    const double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
    const double vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
    const double cx = uy * vz - uz * vy;
    const double cy = uz * vx - ux * vz;
    const double cz = ux * vy - uy * vx;
    return 0.5 * std::sqrt(cx * cx + cy * cy + cz * cz);
}

double MeshArea(const native_mc::PreviewMesh& mesh, const std::vector<std::uint32_t>& indices) {
    double area = 0.0;
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        area += TriangleArea(mesh, indices[i], indices[i + 1], indices[i + 2]);
    }
    return area;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    native_mc::PreviewMesh mesh;
    std::string error;
    const auto start = std::chrono::steady_clock::now();
    if (!native_mc::LoadPreviewMesh(std::filesystem::path(argv[1]), &mesh, &error, false)) {
        std::cerr << error << '\n';
        return 1;
    }
    const auto loaded = std::chrono::steady_clock::now();
    std::array<std::vector<std::uint32_t>, 4> proxies;
    const std::array<std::uint32_t, 4> resolutions = { 64, 48, 32, 24 };
    const auto result = native_mc::BuildPreviewMeshGpuProxiesD3D11(mesh, resolutions, &proxies);
    const double fullArea = MeshArea(mesh, mesh.indices);
    std::cout << "load_ms=" << std::chrono::duration<double, std::milli>(loaded - start).count()
              << " vertices=" << mesh.positions.size() / 3 << " triangles=" << mesh.indices.size() / 3
              << " full_area=" << fullArea << '\n';
    std::cout << "gpu_success=" << result.success << " backend=" << result.backendName
              << " gpu_ms=" << result.elapsedMilliseconds << " error=" << result.error << '\n';
    for (std::size_t level = 0; level < proxies.size(); ++level) {
        const double area = MeshArea(mesh, proxies[level]);
        std::cout << "level=" << level << " resolution=" << resolutions[level]
                  << " triangles=" << proxies[level].size() / 3
                  << " area_ratio=" << (fullArea > 0.0 ? area / fullArea : 0.0)
                  << " level_ms=" << result.levelMilliseconds[level] << '\n';
    }
    return result.success ? 0 : 1;
}
