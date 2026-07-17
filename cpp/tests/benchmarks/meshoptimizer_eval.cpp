#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb_image.h"

#include "native/preview_mesh.h"
#include "meshoptimizer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Vertex {
    float position[3];
    float normal[3];
    std::uint8_t color[4];
};

struct PackedVertex {
    std::uint16_t position[3];
    std::int8_t normalOct[4];
    std::uint8_t color[4];
    std::uint16_t padding;
};

static_assert(sizeof(PackedVertex) == 16, "Packed preview vertex must remain 16 bytes");

struct Metrics {
    std::size_t triangles = 0;
    std::size_t vertices = 0;
    double area = 0.0;
    std::size_t encodedVertices = 0;
    std::size_t encodedIndices = 0;
    std::size_t encodedPackedVertices = 0;
};

double TriangleArea(const native_mc::PreviewMesh& mesh, std::uint32_t ia, std::uint32_t ib, std::uint32_t ic) {
    const float* a = &mesh.positions[static_cast<std::size_t>(ia) * 3];
    const float* b = &mesh.positions[static_cast<std::size_t>(ib) * 3];
    const float* c = &mesh.positions[static_cast<std::size_t>(ic) * 3];
    const double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
    const double vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
    const double nx = uy * vz - uz * vy;
    const double ny = uz * vx - ux * vz;
    const double nz = ux * vy - uy * vx;
    return 0.5 * std::sqrt(nx * nx + ny * ny + nz * nz);
}

Metrics Measure(const native_mc::PreviewMesh& mesh, const std::vector<std::uint32_t>& source) {
    Metrics result;
    result.triangles = source.size() / 3;
    std::unordered_set<std::uint32_t> used;
    used.reserve(source.size());
    for (std::size_t i = 0; i + 2 < source.size(); i += 3) {
        used.insert(source[i]);
        used.insert(source[i + 1]);
        used.insert(source[i + 2]);
        result.area += TriangleArea(mesh, source[i], source[i + 1], source[i + 2]);
    }
    result.vertices = used.size();

    const std::size_t sourceVertexCount = mesh.positions.size() / 3;
    std::vector<Vertex> vertices(sourceVertexCount);
    for (std::size_t i = 0; i < sourceVertexCount; ++i) {
        std::copy_n(&mesh.positions[i * 3], 3, vertices[i].position);
        std::copy_n(&mesh.normals[i * 3], 3, vertices[i].normal);
        std::copy_n(&mesh.colors[i * 4], 4, vertices[i].color);
    }

    std::vector<std::uint32_t> indices = source;
    meshopt_optimizeVertexCache(indices.data(), indices.data(), indices.size(), sourceVertexCount);
    std::vector<Vertex> compact(sourceVertexCount);
    const std::size_t compactCount = meshopt_optimizeVertexFetch(
        compact.data(), indices.data(), indices.size(), vertices.data(), vertices.size(), sizeof(Vertex));
    compact.resize(compactCount);

    std::vector<unsigned char> vbuf(meshopt_encodeVertexBufferBound(compact.size(), sizeof(Vertex)));
    result.encodedVertices = meshopt_encodeVertexBuffer(vbuf.data(), vbuf.size(), compact.data(), compact.size(), sizeof(Vertex));
    std::vector<unsigned char> ibuf(meshopt_encodeIndexBufferBound(indices.size(), compact.size()));
    result.encodedIndices = meshopt_encodeIndexBuffer(ibuf.data(), ibuf.size(), indices.data(), indices.size());

    std::vector<float> normals(compact.size() * 4);
    for (std::size_t i = 0; i < compact.size(); ++i) {
        normals[i * 4 + 0] = compact[i].normal[0];
        normals[i * 4 + 1] = compact[i].normal[1];
        normals[i * 4 + 2] = compact[i].normal[2];
        normals[i * 4 + 3] = 1.0f;
    }
    std::vector<std::int8_t> oct(compact.size() * 4);
    meshopt_encodeFilterOct(oct.data(), compact.size(), 4, 8, normals.data());
    std::vector<PackedVertex> packed(compact.size());
    for (std::size_t i = 0; i < compact.size(); ++i) {
        for (int axis = 0; axis < 3; ++axis) {
            const float extent = mesh.max[axis] - mesh.min[axis];
            const float value = extent > 1e-12f ? (compact[i].position[axis] - mesh.min[axis]) / extent : 0.0f;
            packed[i].position[axis] = static_cast<std::uint16_t>(meshopt_quantizeUnorm(value, 16));
        }
        std::copy_n(&oct[i * 4], 4, packed[i].normalOct);
        std::copy_n(compact[i].color, 4, packed[i].color);
        packed[i].padding = 0;
    }
    std::vector<unsigned char> pbuf(meshopt_encodeVertexBufferBound(packed.size(), sizeof(PackedVertex)));
    result.encodedPackedVertices = meshopt_encodeVertexBuffer(
        pbuf.data(), pbuf.size(), packed.data(), packed.size(), sizeof(PackedVertex));
    return result;
}

std::vector<float> BuildAttributes(const native_mc::PreviewMesh& mesh) {
    const std::size_t count = mesh.positions.size() / 3;
    std::vector<float> attributes(count * 6);
    for (std::size_t i = 0; i < count; ++i) {
        attributes[i * 6 + 0] = mesh.normals[i * 3 + 0];
        attributes[i * 6 + 1] = mesh.normals[i * 3 + 1];
        attributes[i * 6 + 2] = mesh.normals[i * 3 + 2];
        attributes[i * 6 + 3] = mesh.colors[i * 4 + 0] / 255.0f;
        attributes[i * 6 + 4] = mesh.colors[i * 4 + 1] / 255.0f;
        attributes[i * 6 + 5] = mesh.colors[i * 4 + 2] / 255.0f;
    }
    return attributes;
}

void Run(const native_mc::PreviewMesh& mesh, const std::vector<float>& attributes,
         const char* mode, unsigned int options, double ratio, double baseArea) {
    const std::size_t sourceTriangles = mesh.indices.size() / 3;
    const std::size_t targetTriangles = std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(sourceTriangles * ratio)));
    const std::size_t targetIndices = targetTriangles * 3;
    std::vector<std::uint32_t> lod(mesh.indices.size());
    constexpr std::array<float, 6> weights = { 0.35f, 0.35f, 0.35f, 0.5f, 0.5f, 0.5f };
    float error = 0.0f;
    const auto begin = Clock::now();
    const std::size_t resultCount = meshopt_simplifyWithAttributes(
        lod.data(), mesh.indices.data(), mesh.indices.size(), mesh.positions.data(),
        mesh.positions.size() / 3, sizeof(float) * 3, attributes.data(), sizeof(float) * 6,
        weights.data(), weights.size(), nullptr, targetIndices, 1.0f, options, &error);
    const auto simplified = Clock::now();
    lod.resize(resultCount);
    const Metrics metrics = Measure(mesh, lod);
    const auto measured = Clock::now();
    const double simplifyMs = std::chrono::duration<double, std::milli>(simplified - begin).count();
    const double measureMs = std::chrono::duration<double, std::milli>(measured - simplified).count();
    std::cout << std::left << std::setw(18) << mode
              << " target=" << std::setw(7) << targetTriangles
              << " actual=" << std::setw(7) << metrics.triangles
              << " used_v=" << std::setw(7) << metrics.vertices
              << " rel_err=" << std::fixed << std::setprecision(6) << error
              << " area=" << std::setprecision(3) << (baseArea > 0.0 ? metrics.area / baseArea * 100.0 : 0.0) << "%"
              << " simplify=" << std::setprecision(1) << simplifyMs << "ms"
              << " post=" << measureMs << "ms"
              << " encoded=" << (metrics.encodedVertices + metrics.encodedIndices) / 1024.0 << "KiB"
              << " packed=" << (metrics.encodedPackedVertices + metrics.encodedIndices) / 1024.0 << "KiB"
              << "\n";
}

void RunLegacySample(const native_mc::PreviewMesh& mesh, double ratio, double baseArea) {
    const std::size_t sourceTriangles = mesh.indices.size() / 3;
    const std::size_t targetTriangles = std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(sourceTriangles * ratio)));
    std::vector<std::uint32_t> sampled;
    sampled.reserve(targetTriangles * 3);
    for (std::size_t i = 0; i < targetTriangles; ++i) {
        const std::size_t triangle = std::min(sourceTriangles - 1, i * sourceTriangles / targetTriangles);
        sampled.insert(sampled.end(), mesh.indices.begin() + triangle * 3, mesh.indices.begin() + triangle * 3 + 3);
    }
    const Metrics metrics = Measure(mesh, sampled);
    std::cout << std::left << std::setw(18) << "legacy-stride"
              << " target=" << std::setw(7) << targetTriangles
              << " actual=" << std::setw(7) << metrics.triangles
              << " used_v=" << std::setw(7) << metrics.vertices
              << " reuse=" << std::fixed << std::setprecision(2)
              << (metrics.vertices ? static_cast<double>(sampled.size()) / metrics.vertices : 0.0)
              << " area=" << std::setprecision(3) << (baseArea > 0.0 ? metrics.area / baseArea * 100.0 : 0.0) << "%"
              << " encoded=" << std::setprecision(1)
              << (metrics.encodedVertices + metrics.encodedIndices) / 1024.0 << "KiB"
              << " packed=" << (metrics.encodedPackedVertices + metrics.encodedIndices) / 1024.0 << "KiB\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: eval_preview <obj-or-glb>\n";
        return 2;
    }
    native_mc::PreviewMesh mesh;
    std::string error;
    const auto begin = Clock::now();
    if (!native_mc::LoadPreviewMesh(argv[1], &mesh, &error, false)) {
        std::cerr << "load failed: " << error << "\n";
        return 1;
    }
    const auto loaded = Clock::now();
    const Metrics base = Measure(mesh, mesh.indices);
    const auto measured = Clock::now();
    std::cout << "source vertices=" << mesh.positions.size() / 3
              << " triangles=" << mesh.indices.size() / 3
              << " load=" << std::chrono::duration<double>(loaded - begin).count() << "s"
              << " base_measure=" << std::chrono::duration<double>(measured - loaded).count() << "s"
              << " raw=" << (mesh.positions.size() * sizeof(float) + mesh.normals.size() * sizeof(float) +
                              mesh.colors.size() + mesh.indices.size() * sizeof(std::uint32_t)) / 1048576.0 << "MiB"
              << " encoded=" << (base.encodedVertices + base.encodedIndices) / 1048576.0 << "MiB\n";
    std::cout << "source packed_encoded=" << (base.encodedPackedVertices + base.encodedIndices) / 1048576.0 << "MiB\n";
    const std::vector<float> attributes = BuildAttributes(mesh);
    constexpr std::array<double, 4> ratios = { 0.5, 0.1, 0.02, 0.009 };
    for (const double ratio : ratios) {
        std::cout << std::defaultfloat << "ratio=" << ratio << "\n";
        RunLegacySample(mesh, ratio, base.area);
        Run(mesh, attributes, "conservative", meshopt_SimplifyRegularizeLight, ratio, base.area);
        Run(mesh, attributes, "permissive", meshopt_SimplifyPermissive | meshopt_SimplifyRegularizeLight, ratio, base.area);
        Run(mesh, attributes, "permissive+border", meshopt_SimplifyPermissive | meshopt_SimplifyRegularizeLight |
            meshopt_SimplifyLockBorder, ratio, base.area);
    }
    return 0;
}
