#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace native_mc {

struct PreviewMesh {
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<std::uint8_t> colors;
    std::vector<std::uint32_t> indices;
    // indices is LOD 0 (the complete source mesh). Lower-detail index buffers
    // reference the same vertices, so loading never discards source geometry.
    std::array<std::vector<std::uint32_t>, 4> lodIndices;
    std::array<float, 3> min = { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> max = { 0.0f, 0.0f, 0.0f };
};

bool LoadPreviewMesh(const std::filesystem::path& filePath, PreviewMesh* outMesh, std::string* errorText,
                     bool buildLods = true);
void BuildPreviewMeshLods(PreviewMesh* mesh);
std::array<std::vector<std::uint32_t>, 4> BuildPreviewMeshProxyIndices(
    const PreviewMesh& mesh,
    const std::array<std::uint32_t, 4>& resolutions);

}  // namespace native_mc
