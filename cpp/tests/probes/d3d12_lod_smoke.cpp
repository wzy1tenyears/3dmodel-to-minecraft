#include "native/preview_gpu_lod.h"
#include "native/preview_gpu_lod_d3d12.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

native_mc::PreviewMesh MakeGrid(std::uint32_t cells) {
    native_mc::PreviewMesh mesh;
    const std::uint32_t side = cells + 1;
    mesh.positions.reserve(static_cast<std::size_t>(side) * side * 3);
    for (std::uint32_t z = 0; z < side; ++z) {
        for (std::uint32_t x = 0; x < side; ++x) {
            const float y = std::sin(static_cast<float>(x) * 0.21f) * 2.0f +
                std::cos(static_cast<float>(z) * 0.17f) * 1.5f;
            mesh.positions.push_back(static_cast<float>(x));
            mesh.positions.push_back(y);
            mesh.positions.push_back(static_cast<float>(z));
        }
    }
    mesh.indices.reserve(static_cast<std::size_t>(cells) * cells * 6);
    for (std::uint32_t z = 0; z < cells; ++z) {
        for (std::uint32_t x = 0; x < cells; ++x) {
            const std::uint32_t a = z * side + x;
            const std::uint32_t b = a + 1;
            const std::uint32_t c = a + side;
            const std::uint32_t d = c + 1;
            mesh.indices.insert(mesh.indices.end(), { a, c, b, b, c, d });
        }
    }
    mesh.min = { 0.0f, -3.5f, 0.0f };
    mesh.max = { static_cast<float>(cells), 3.5f, static_cast<float>(cells) };
    return mesh;
}

bool ValidateIndices(
    const std::vector<std::uint32_t>& indices,
    std::size_t vertexCount,
    const char* label) {
    if (indices.empty() || indices.size() % 3 != 0) {
        std::cerr << label << " was empty or malformed\n";
        return false;
    }
    for (std::size_t offset = 0; offset < indices.size(); offset += 3) {
        const std::uint32_t a = indices[offset];
        const std::uint32_t b = indices[offset + 1];
        const std::uint32_t c = indices[offset + 2];
        if (a >= vertexCount || b >= vertexCount || c >= vertexCount ||
            a == b || b == c || a == c) {
            std::cerr << label << " contained an invalid triangle\n";
            return false;
        }
    }
    return true;
}

bool ValidateMeshLods(const native_mc::PreviewMesh& mesh, const char* label) {
    std::size_t previous = mesh.indices.size();
    for (std::size_t level = 1; level < mesh.lodIndices.size(); ++level) {
        const auto& indices = mesh.lodIndices[level];
        if (!ValidateIndices(indices, mesh.positions.size() / 3, label) ||
            indices.size() > previous) {
            return false;
        }
        previous = indices.size();
    }
    return true;
}

}  // namespace

int main() {
    const native_mc::PreviewMesh source = MakeGrid(64);
    native_mc::PreviewGpuLodOptions options;
    options.resolutions = { 0, 32, 16, 8 };

    native_mc::PreviewMesh direct = source;
    const auto directResult = native_mc::BuildPreviewMeshLodsD3D12(&direct, options);
    if (!directResult.success || !ValidateMeshLods(direct, "D3D12 LOD")) {
        std::cerr << "D3D12 direct failed: " << directResult.error << '\n';
        return 2;
    }

    native_mc::PreviewMesh automatic = source;
    const auto automaticResult = native_mc::BuildPreviewMeshLodsGpu(&automatic, options);
    if (!automaticResult.success ||
        automaticResult.activeBackend != native_mc::PreviewGpuLodBackend::D3D12 ||
        automaticResult.usedBackendFallback ||
        !ValidateMeshLods(automatic, "automatic LOD")) {
        std::cerr << "automatic dispatch failed: " << automaticResult.error << '\n';
        return 3;
    }

    native_mc::PreviewMesh legacy = source;
    const auto legacyResult = native_mc::BuildPreviewMeshLodsD3D11(&legacy, options);
    if (!legacyResult.success || !ValidateMeshLods(legacy, "D3D11 LOD")) {
        std::cerr << "D3D11 compatibility failed: " << legacyResult.error << '\n';
        return 4;
    }
    for (std::size_t level = 1; level < direct.lodIndices.size(); ++level) {
        if (direct.lodIndices[level] != legacy.lodIndices[level]) {
            std::cerr << "D3D12/D3D11 LOD parity failed at level " << level << '\n';
            return 5;
        }
    }

    std::array<std::vector<std::uint32_t>, 4> proxies;
    const std::array<std::uint32_t, 4> proxyResolutions = { 24, 16, 10, 6 };
    const auto proxyResult = native_mc::BuildPreviewMeshGpuProxies(
        source, proxyResolutions, &proxies);
    std::size_t previousProxy = source.indices.size();
    for (const auto& proxy : proxies) {
        if (!ValidateIndices(proxy, source.positions.size() / 3, "D3D12 proxy") ||
            proxy.size() > previousProxy) {
            std::cerr << "proxy validation failed: " << proxyResult.error << '\n';
            return 6;
        }
        previousProxy = proxy.size();
    }
    if (!proxyResult.success ||
        proxyResult.activeBackend != native_mc::PreviewGpuLodBackend::D3D12) {
        std::cerr << "proxy dispatch failed: " << proxyResult.error << '\n';
        return 7;
    }

    std::array<std::vector<std::uint32_t>, 4> legacyProxies;
    const auto legacyProxyResult = native_mc::BuildPreviewMeshGpuProxiesD3D11(
        source, proxyResolutions, &legacyProxies);
    if (!legacyProxyResult.success || proxies != legacyProxies) {
        std::cerr << "D3D12/D3D11 proxy parity failed: " << legacyProxyResult.error << '\n';
        return 8;
    }

    std::atomic<int> threadedFailures{0};
    std::vector<std::thread> threads;
    for (int index = 0; index < 8; ++index) {
        threads.emplace_back([&]() {
            native_mc::PreviewMesh mesh = source;
            const auto result = native_mc::BuildPreviewMeshLodsGpu(&mesh, options);
            if (!result.success ||
                result.activeBackend != native_mc::PreviewGpuLodBackend::D3D12 ||
                !ValidateMeshLods(mesh, "threaded LOD")) {
                threadedFailures.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& thread : threads) thread.join();
    if (threadedFailures.load(std::memory_order_relaxed) != 0) return 9;

    std::cout << "backend=" << automaticResult.backendName
              << " source_triangles=" << source.indices.size() / 3
              << " lod=" << automatic.lodIndices[1].size() / 3 << '/'
              << automatic.lodIndices[2].size() / 3 << '/'
              << automatic.lodIndices[3].size() / 3
              << " proxies=" << proxies[0].size() / 3 << '/'
              << proxies[1].size() / 3 << '/'
              << proxies[2].size() / 3 << '/'
              << proxies[3].size() / 3
              << " d3d12_ms=" << automaticResult.elapsedMilliseconds
              << " d3d11_ms=" << legacyResult.elapsedMilliseconds
              << " threaded=8\n";
    return 0;
}
