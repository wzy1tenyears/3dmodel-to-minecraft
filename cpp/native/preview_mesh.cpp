#include "preview_mesh.h"

#include "../third_party/json.hpp"
#include "../third_party/stb_image.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace native_mc {

namespace {

std::vector<std::uint8_t> ReadFileBytes(const fs::path& filePath) {
    std::ifstream in(filePath, std::ios::binary);
    if (!in) return {};
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::uint32_t ReadU32Le(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return
        static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
        (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
        (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

void ExtendBounds(const std::array<float, 3>& p, std::array<float, 3>* min, std::array<float, 3>* max) {
    for (int i = 0; i < 3; ++i) {
        (*min)[i] = std::min((*min)[i], p[i]);
        (*max)[i] = std::max((*max)[i], p[i]);
    }
}

struct ClusterRepresentative {
    std::uint32_t index = 0;
    float centerDistanceSquared = std::numeric_limits<float>::max();
};

struct TriangleKey {
    std::array<std::uint32_t, 3> vertices{};
    bool operator==(const TriangleKey& other) const { return vertices == other.vertices; }
};

struct TriangleKeyHash {
    std::size_t operator()(const TriangleKey& key) const {
        std::size_t hash = static_cast<std::size_t>(key.vertices[0]) * 0x9e3779b1u;
        hash ^= static_cast<std::size_t>(key.vertices[1]) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
        hash ^= static_cast<std::size_t>(key.vertices[2]) + 0x85ebca6bu + (hash << 6) + (hash >> 2);
        return hash;
    }
};

TriangleKey OrientedTriangleKey(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    if (a < b && a < c) return TriangleKey{ { a, b, c } };
    if (b < a && b < c) return TriangleKey{ { b, c, a } };
    return TriangleKey{ { c, a, b } };
}

std::uint64_t PreviewClusterKey(const PreviewMesh& mesh, std::size_t vertex, int resolution,
                                std::array<int, 3>* outCell = nullptr) {
    std::array<int, 3> cell{};
    for (int axis = 0; axis < 3; ++axis) {
        const float extent = mesh.max[axis] - mesh.min[axis];
        const float value = mesh.positions[vertex * 3 + axis];
        const float normalized = extent > 0.000001f ? (value - mesh.min[axis]) / extent : 0.5f;
        cell[axis] = std::clamp(static_cast<int>(normalized * resolution), 0, resolution - 1);
    }
    if (outCell) *outCell = cell;
    return (static_cast<std::uint64_t>(cell[0]) << 42) |
           (static_cast<std::uint64_t>(cell[1]) << 21) |
           static_cast<std::uint64_t>(cell[2]);
}

std::vector<std::uint32_t> BuildClusterLod(const PreviewMesh& mesh, int resolution) {
    const std::size_t vertexCount = mesh.positions.size() / 3;
    std::unordered_map<std::uint64_t, ClusterRepresentative> clusters;
    clusters.reserve(std::min<std::size_t>(vertexCount, static_cast<std::size_t>(resolution) * resolution * resolution));
    for (std::size_t vertex = 0; vertex < vertexCount; ++vertex) {
        std::array<int, 3> cell{};
        const std::uint64_t key = PreviewClusterKey(mesh, vertex, resolution, &cell);
        float distanceSquared = 0.0f;
        for (int axis = 0; axis < 3; ++axis) {
            const float extent = mesh.max[axis] - mesh.min[axis];
            const float center = mesh.min[axis] + (static_cast<float>(cell[axis]) + 0.5f) * extent / resolution;
            const float scale = std::max(extent, 0.000001f);
            const float delta = (mesh.positions[vertex * 3 + axis] - center) / scale;
            distanceSquared += delta * delta;
        }
        auto [it, inserted] = clusters.emplace(key, ClusterRepresentative{ static_cast<std::uint32_t>(vertex), distanceSquared });
        if (!inserted && distanceSquared < it->second.centerDistanceSquared) {
            it->second = { static_cast<std::uint32_t>(vertex), distanceSquared };
        }
    }

    std::vector<std::uint32_t> remap(vertexCount);
    for (std::size_t vertex = 0; vertex < vertexCount; ++vertex) {
        remap[vertex] = clusters.find(PreviewClusterKey(mesh, vertex, resolution))->second.index;
    }
    clusters.clear();
    clusters.rehash(0);

    std::vector<std::uint32_t> lod;
    lod.reserve(mesh.indices.size());
    std::unordered_set<TriangleKey, TriangleKeyHash> seen;
    seen.reserve(mesh.indices.size() / 6);
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        if (mesh.indices[i] >= vertexCount || mesh.indices[i + 1] >= vertexCount || mesh.indices[i + 2] >= vertexCount) continue;
        const std::uint32_t a = remap[mesh.indices[i]];
        const std::uint32_t b = remap[mesh.indices[i + 1]];
        const std::uint32_t c = remap[mesh.indices[i + 2]];
        if (a == b || b == c || a == c) continue;
        const TriangleKey key = OrientedTriangleKey(a, b, c);
        if (!seen.insert(key).second) continue;
        lod.push_back(a);
        lod.push_back(b);
        lod.push_back(c);
    }
    lod.shrink_to_fit();
    return lod;
}

bool FinalizeMesh(PreviewMesh* mesh, std::string* errorText, bool buildLods = true) {
    if (!mesh || mesh->indices.empty() || mesh->positions.empty()) {
        if (errorText) *errorText = "Preview mesh is empty";
        return false;
    }
    mesh->min = { std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
    mesh->max = { std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest() };
    for (std::size_t i = 0; i + 2 < mesh->positions.size(); i += 3) {
        ExtendBounds({ mesh->positions[i], mesh->positions[i + 1], mesh->positions[i + 2] }, &mesh->min, &mesh->max);
    }

    const std::size_t vertexCount = mesh->positions.size() / 3;
    if (mesh->colors.size() != vertexCount * 4) {
        mesh->colors.resize(vertexCount * 4);
        for (std::size_t vertex = 0; vertex < vertexCount; ++vertex) {
            mesh->colors[vertex * 4] = 69;
            mesh->colors[vertex * 4 + 1] = 176;
            mesh->colors[vertex * 4 + 2] = 200;
            mesh->colors[vertex * 4 + 3] = 255;
        }
    }
    mesh->normals.assign(mesh->positions.size(), 0.0f);
    for (std::size_t i = 0; i + 2 < mesh->indices.size(); i += 3) {
        const std::uint32_t ia = mesh->indices[i];
        const std::uint32_t ib = mesh->indices[i + 1];
        const std::uint32_t ic = mesh->indices[i + 2];
        if (ia >= vertexCount || ib >= vertexCount || ic >= vertexCount) continue;
        const std::size_t a = static_cast<std::size_t>(ia) * 3;
        const std::size_t b = static_cast<std::size_t>(ib) * 3;
        const std::size_t c = static_cast<std::size_t>(ic) * 3;
        const float ux = mesh->positions[b] - mesh->positions[a];
        const float uy = mesh->positions[b + 1] - mesh->positions[a + 1];
        const float uz = mesh->positions[b + 2] - mesh->positions[a + 2];
        const float vx = mesh->positions[c] - mesh->positions[a];
        const float vy = mesh->positions[c + 1] - mesh->positions[a + 1];
        const float vz = mesh->positions[c + 2] - mesh->positions[a + 2];
        const float nx = uy * vz - uz * vy;
        const float ny = uz * vx - ux * vz;
        const float nz = ux * vy - uy * vx;
        for (std::size_t base : { a, b, c }) {
            mesh->normals[base] += nx;
            mesh->normals[base + 1] += ny;
            mesh->normals[base + 2] += nz;
        }
    }
    for (std::size_t i = 0; i + 2 < mesh->normals.size(); i += 3) {
        const float nx = mesh->normals[i];
        const float ny = mesh->normals[i + 1];
        const float nz = mesh->normals[i + 2];
        const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (length > 0.000001f) {
            mesh->normals[i] /= length;
            mesh->normals[i + 1] /= length;
            mesh->normals[i + 2] /= length;
        } else {
            mesh->normals[i + 1] = 1.0f;
        }
    }

    mesh->lodIndices[0].clear();
    if (buildLods) BuildPreviewMeshLods(mesh);
    return true;
}

struct ObjPreviewMaterial {
    std::array<double, 4> factor = { 0.27, 0.69, 0.78, 1.0 };
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

std::map<std::string, ObjPreviewMaterial> LoadObjPreviewMaterials(const fs::path& mtlPath) {
    std::map<std::string, ObjPreviewMaterial> materials;
    std::ifstream in(mtlPath);
    if (!in) return materials;
    ObjPreviewMaterial* current = nullptr;
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("newmtl ", 0) == 0) {
            std::string name = line.substr(7);
            while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
            current = &materials[name];
        } else if (current && line.rfind("Kd ", 0) == 0) {
            std::istringstream ss(line.substr(3));
            ss >> current->factor[0] >> current->factor[1] >> current->factor[2];
        } else if (current && line.rfind("d ", 0) == 0) {
            std::istringstream(line.substr(2)) >> current->factor[3];
        } else if (current && line.rfind("map_Kd ", 0) == 0) {
            std::string textureName = line.substr(7);
            while (!textureName.empty() && std::isspace(static_cast<unsigned char>(textureName.back()))) textureName.pop_back();
            while (!textureName.empty() && std::isspace(static_cast<unsigned char>(textureName.front()))) textureName.erase(textureName.begin());
            if (textureName.size() >= 2 && textureName.front() == '"' && textureName.back() == '"') {
                textureName = textureName.substr(1, textureName.size() - 2);
            }
            const auto encoded = ReadFileBytes(mtlPath.parent_path() / fs::path(textureName));
            int channels = 0;
            unsigned char* decoded = encoded.empty() ? nullptr : stbi_load_from_memory(encoded.data(),
                static_cast<int>(std::min<std::size_t>(encoded.size(), std::numeric_limits<int>::max())),
                &current->width, &current->height, &channels, 4);
            if (decoded && current->width > 0 && current->height > 0) {
                current->rgba.assign(decoded, decoded + static_cast<std::size_t>(current->width) * current->height * 4);
            }
            if (decoded) stbi_image_free(decoded);
        }
    }
    return materials;
}

std::array<std::uint8_t, 4> SampleObjPreviewMaterial(const ObjPreviewMaterial* material,
                                                     const std::array<float, 2>* uv) {
    if (!material) return { 69, 176, 200, 255 };
    std::array<double, 4> sampled = { 255.0, 255.0, 255.0, 255.0 };
    if (uv && material->width > 0 && material->height > 0 && !material->rgba.empty()) {
        const double u = (*uv)[0] - std::floor((*uv)[0]);
        const double v = (*uv)[1] - std::floor((*uv)[1]);
        const int x = std::clamp(static_cast<int>(u * material->width), 0, material->width - 1);
        const int y = std::clamp(static_cast<int>((1.0 - v) * material->height), 0, material->height - 1);
        const std::size_t offset = (static_cast<std::size_t>(y) * material->width + x) * 4;
        for (int component = 0; component < 4; ++component) sampled[component] = material->rgba[offset + component];
    }
    std::array<std::uint8_t, 4> color{};
    for (int component = 0; component < 4; ++component) {
        color[component] = static_cast<std::uint8_t>(std::clamp(std::lround(sampled[component] * material->factor[component]), 0L, 255L));
    }
    return color;
}

struct ObjPreviewVertexKey {
    int vertex = -1;
    int texcoord = -1;
    std::uint32_t material = 0;

    bool operator==(const ObjPreviewVertexKey& other) const {
        return vertex == other.vertex && texcoord == other.texcoord && material == other.material;
    }
};

struct ObjPreviewVertexKeyHash {
    std::size_t operator()(const ObjPreviewVertexKey& key) const {
        std::size_t hash = static_cast<std::uint32_t>(key.vertex) * 0x9e3779b1u;
        hash ^= static_cast<std::uint32_t>(key.texcoord) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
        hash ^= key.material + 0x85ebca6bu + (hash << 6) + (hash >> 2);
        return hash;
    }
};

void SkipObjWhitespace(const char** cursor, const char* end) {
    while (*cursor < end && std::isspace(static_cast<unsigned char>(**cursor))) ++*cursor;
}

void ParseObjFloat(const char** cursor, const char* end, float* value) {
    SkipObjWhitespace(cursor, end);
    if (*cursor >= end) return;
    const auto result = std::from_chars(*cursor, end, *value, std::chars_format::general);
    if (result.ec == std::errc()) *cursor = result.ptr;
}

int ResolveObjIndex(const char* begin, const char* end, int count) {
    if (begin == end) return -1;
    if (*begin == '+') ++begin;
    int value = 0;
    const auto result = std::from_chars(begin, end, value);
    if (result.ec == std::errc::invalid_argument) throw std::invalid_argument("stoi");
    if (result.ec == std::errc::result_out_of_range) throw std::out_of_range("stoi");
    return value < 0 ? count + value : value - 1;
}

bool LoadObjPreview(const fs::path& filePath, PreviewMesh* outMesh, std::string* errorText, bool buildLods) {
    std::ifstream in(filePath);
    if (!in) {
        if (errorText) *errorText = "Could not open OBJ file";
        return false;
    }

    std::vector<std::array<float, 3>> vertices;
    std::vector<std::array<float, 2>> texcoords;
    std::map<std::string, ObjPreviewMaterial> materials;
    std::unordered_map<ObjPreviewVertexKey, std::uint32_t, ObjPreviewVertexKeyHash> vertexMap;
    std::unordered_map<std::string, std::uint32_t> materialIds;
    std::string currentMaterial;
    std::uint32_t currentMaterialId = 0;
    const ObjPreviewMaterial* currentMaterialData = nullptr;
    outMesh->positions.clear();
    outMesh->colors.clear();
    outMesh->indices.clear();

    std::error_code sizeError;
    const std::uintmax_t fileBytes = fs::file_size(filePath, sizeError);
    if (!sizeError) {
        const std::size_t estimatedVertices = static_cast<std::size_t>(std::min<std::uintmax_t>(
            fileBytes / 128 + 1, std::numeric_limits<std::uint32_t>::max()));
        vertices.reserve(estimatedVertices);
        texcoords.reserve(estimatedVertices);
        vertexMap.reserve(estimatedVertices);
        outMesh->positions.reserve(estimatedVertices * 3);
        outMesh->colors.reserve(estimatedVertices * 4);
        outMesh->indices.reserve(static_cast<std::size_t>(std::min<std::uintmax_t>(
            fileBytes / 28 + 3, std::numeric_limits<std::size_t>::max())));
    }
    vertexMap.max_load_factor(0.8f);
    materialIds.reserve(8);
    materialIds.emplace(std::string(), 0);
    std::vector<std::pair<int, int>> refs;
    refs.reserve(4);

    std::string rawLine;
    while (std::getline(in, rawLine)) {
        if (rawLine.rfind("v ", 0) == 0) {
            float x = 0, y = 0, z = 0;
            const char* cursor = rawLine.data() + 2;
            const char* end = rawLine.data() + rawLine.size();
            ParseObjFloat(&cursor, end, &x);
            ParseObjFloat(&cursor, end, &y);
            ParseObjFloat(&cursor, end, &z);
            vertices.push_back({ x, y, z });
        } else if (rawLine.rfind("vt ", 0) == 0) {
            float u = 0.0f, v = 0.0f;
            const char* cursor = rawLine.data() + 3;
            const char* end = rawLine.data() + rawLine.size();
            ParseObjFloat(&cursor, end, &u);
            ParseObjFloat(&cursor, end, &v);
            texcoords.push_back({ u, v });
        } else if (rawLine.rfind("mtllib ", 0) == 0) {
            const auto loaded = LoadObjPreviewMaterials(filePath.parent_path() / fs::path(rawLine.substr(7)));
            materials.insert(loaded.begin(), loaded.end());
            const auto materialIt = materials.find(currentMaterial);
            currentMaterialData = materialIt == materials.end() ? nullptr : &materialIt->second;
        } else if (rawLine.rfind("usemtl ", 0) == 0) {
            currentMaterial = rawLine.substr(7);
            while (!currentMaterial.empty() && std::isspace(static_cast<unsigned char>(currentMaterial.back()))) currentMaterial.pop_back();
            const auto idIt = materialIds.try_emplace(
                currentMaterial, static_cast<std::uint32_t>(materialIds.size())).first;
            currentMaterialId = idIt->second;
            const auto materialIt = materials.find(currentMaterial);
            currentMaterialData = materialIt == materials.end() ? nullptr : &materialIt->second;
        } else if (rawLine.rfind("f ", 0) == 0) {
            refs.clear();
            const char* cursor = rawLine.data() + 2;
            const char* end = rawLine.data() + rawLine.size();
            while (true) {
                SkipObjWhitespace(&cursor, end);
                if (cursor >= end) break;
                const char* tokenBegin = cursor;
                while (cursor < end && !std::isspace(static_cast<unsigned char>(*cursor))) ++cursor;
                const char* tokenEnd = cursor;
                const char* firstSlash = std::find(tokenBegin, tokenEnd, '/');
                const char* secondSlash = firstSlash == tokenEnd ? tokenEnd :
                    std::find(firstSlash + 1, tokenEnd, '/');
                refs.push_back({
                    ResolveObjIndex(tokenBegin, firstSlash, static_cast<int>(vertices.size())),
                    firstSlash == tokenEnd ? -1 :
                        ResolveObjIndex(firstSlash + 1, secondSlash, static_cast<int>(texcoords.size()))
                });
            }
            if (refs.size() < 3) continue;
            for (std::size_t i = 1; i + 1 < refs.size(); ++i) {
                const std::array<std::pair<int, int>, 3> tri = { refs[0], refs[i], refs[i + 1] };
                if (std::any_of(tri.begin(), tri.end(), [&](const auto& ref) {
                    return ref.first < 0 || ref.first >= static_cast<int>(vertices.size());
                })) continue;
                for (const auto& ref : tri) {
                    const ObjPreviewVertexKey key{ ref.first, ref.second, currentMaterialId };
                    auto [it, inserted] = vertexMap.emplace(key, static_cast<std::uint32_t>(outMesh->positions.size() / 3));
                    if (inserted) {
                        const auto& p = vertices[ref.first];
                        outMesh->positions.insert(outMesh->positions.end(), p.begin(), p.end());
                        const std::array<float, 2>* uv = ref.second >= 0 && ref.second < static_cast<int>(texcoords.size()) ? &texcoords[ref.second] : nullptr;
                        const auto color = SampleObjPreviewMaterial(currentMaterialData, uv);
                        outMesh->colors.insert(outMesh->colors.end(), color.begin(), color.end());
                    }
                    outMesh->indices.push_back(it->second);
                }
            }
        }
    }
    return FinalizeMesh(outMesh, errorText, buildLods);
}

struct AccessorReader {
    int componentType = 0;
    int componentSize = 0;
    int componentCount = 0;
    std::size_t length = 0;
    std::size_t stride = 0;
    std::size_t baseOffset = 0;
    const std::vector<std::uint8_t>* bytes = nullptr;
};

struct PreviewImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

struct PreviewMaterial {
    std::array<double, 4> factor = { 1.0, 1.0, 1.0, 1.0 };
    const PreviewImage* image = nullptr;
};

int ComponentSize(int type) {
    switch (type) {
    case 5120:
    case 5121:
        return 1;
    case 5122:
    case 5123:
        return 2;
    case 5125:
    case 5126:
        return 4;
    default:
        return 0;
    }
}

int TypeCount(const std::string& typeName) {
    if (typeName == "SCALAR") return 1;
    if (typeName == "VEC2") return 2;
    if (typeName == "VEC3") return 3;
    if (typeName == "VEC4") return 4;
    return 0;
}

bool BuildAccessorReader(const json& gltf, const std::vector<std::uint8_t>& bytes, std::size_t binOffset,
                         std::size_t binLength, int accessorIndex, AccessorReader* outReader, std::string* errorText) {
    if (!outReader) {
        if (errorText) *errorText = "Accessor output pointer was null";
        return false;
    }
    if (!gltf.contains("accessors") || accessorIndex < 0 || accessorIndex >= gltf["accessors"].size()) {
        if (errorText) *errorText = "Accessor index out of range";
        return false;
    }
    const json& accessor = gltf["accessors"][accessorIndex];
    const int bufferViewIndex = accessor.value("bufferView", -1);
    if (!gltf.contains("bufferViews") || bufferViewIndex < 0 || bufferViewIndex >= gltf["bufferViews"].size()) {
        if (errorText) *errorText = "Missing buffer view";
        return false;
    }
    const json& view = gltf["bufferViews"][bufferViewIndex];
    outReader->componentType = accessor.value("componentType", 0);
    outReader->componentSize = ComponentSize(outReader->componentType);
    outReader->componentCount = TypeCount(accessor.value("type", ""));
    outReader->length = accessor.value("count", 0);
    outReader->stride = view.value("byteStride", static_cast<unsigned int>(outReader->componentSize * outReader->componentCount));
    const std::size_t localOffset = view.value("byteOffset", 0u) + accessor.value("byteOffset", 0u);
    const std::size_t elementBytes = outReader->length == 0 ? 0 :
        ((outReader->length - 1) * outReader->stride +
         static_cast<std::size_t>(outReader->componentSize * outReader->componentCount));
    if (localOffset > binLength || elementBytes > binLength - localOffset) {
        if (errorText) *errorText = "Accessor exceeds GLB BIN chunk";
        return false;
    }
    outReader->baseOffset = binOffset + localOffset;
    outReader->bytes = &bytes;
    if (outReader->componentSize <= 0 || outReader->componentCount <= 0) {
        if (errorText) *errorText = "Unsupported accessor type";
        return false;
    }
    return true;
}

const PreviewImage* DecodePreviewImage(const json& gltf, const std::vector<std::uint8_t>& bytes,
                                       std::size_t binOffset, std::size_t binLength, int imageIndex,
                                       std::map<int, PreviewImage>* cache) {
    if (!cache || !gltf.contains("images") || imageIndex < 0 || imageIndex >= gltf["images"].size()) return nullptr;
    if (auto it = cache->find(imageIndex); it != cache->end()) return &it->second;
    const json& image = gltf["images"][imageIndex];
    const int viewIndex = image.value("bufferView", -1);
    if (!gltf.contains("bufferViews") || viewIndex < 0 || viewIndex >= gltf["bufferViews"].size()) return nullptr;
    const json& view = gltf["bufferViews"][viewIndex];
    const std::size_t localOffset = view.value("byteOffset", 0u);
    const std::size_t byteLength = view.value("byteLength", 0u);
    if (localOffset > binLength || byteLength > binLength - localOffset || byteLength > static_cast<std::size_t>(std::numeric_limits<int>::max())) return nullptr;
    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* decoded = stbi_load_from_memory(bytes.data() + binOffset + localOffset,
        static_cast<int>(byteLength), &width, &height, &channels, 4);
    if (!decoded || width <= 0 || height <= 0) {
        if (decoded) stbi_image_free(decoded);
        return nullptr;
    }
    PreviewImage result;
    result.width = width;
    result.height = height;
    result.rgba.assign(decoded, decoded + static_cast<std::size_t>(width) * height * 4);
    stbi_image_free(decoded);
    return &cache->emplace(imageIndex, std::move(result)).first->second;
}

PreviewMaterial ResolvePreviewMaterial(const json& gltf, const json& primitive,
                                       const std::vector<std::uint8_t>& bytes, std::size_t binOffset,
                                       std::size_t binLength, std::map<int, PreviewImage>* imageCache) {
    PreviewMaterial result;
    const int materialIndex = primitive.value("material", -1);
    if (!gltf.contains("materials") || materialIndex < 0 || materialIndex >= gltf["materials"].size()) return result;
    const json& material = gltf["materials"][materialIndex];
    if (!material.contains("pbrMetallicRoughness")) return result;
    const json& pbr = material["pbrMetallicRoughness"];
    if (pbr.contains("baseColorFactor") && pbr["baseColorFactor"].size() >= 4) {
        for (int component = 0; component < 4; ++component) result.factor[component] = pbr["baseColorFactor"][component].get<double>();
    }
    if (!pbr.contains("baseColorTexture")) return result;
    const int textureIndex = pbr["baseColorTexture"].value("index", -1);
    if (!gltf.contains("textures") || textureIndex < 0 || textureIndex >= gltf["textures"].size()) return result;
    const int imageIndex = gltf["textures"][textureIndex].value("source", -1);
    result.image = DecodePreviewImage(gltf, bytes, binOffset, binLength, imageIndex, imageCache);
    return result;
}

std::array<std::uint8_t, 4> SamplePreviewMaterial(const PreviewMaterial& material, const std::vector<double>& uv) {
    std::array<double, 4> sampled = { 255.0, 255.0, 255.0, 255.0 };
    if (material.image && uv.size() >= 2 && !material.image->rgba.empty()) {
        const double wrappedU = uv[0] - std::floor(uv[0]);
        const double wrappedV = uv[1] - std::floor(uv[1]);
        const int x = std::clamp(static_cast<int>(wrappedU * material.image->width), 0, material.image->width - 1);
        const int y = std::clamp(static_cast<int>((1.0 - wrappedV) * material.image->height), 0, material.image->height - 1);
        const std::size_t offset = (static_cast<std::size_t>(y) * material.image->width + x) * 4;
        for (int component = 0; component < 4; ++component) sampled[component] = material.image->rgba[offset + component];
    }
    std::array<std::uint8_t, 4> color{};
    for (int component = 0; component < 4; ++component) {
        color[component] = static_cast<std::uint8_t>(std::clamp(std::lround(sampled[component] * material.factor[component]), 0L, 255L));
    }
    return color;
}

std::vector<double> ReadAccessorValues(const AccessorReader& reader, std::size_t index) {
    std::vector<double> out;
    out.reserve(reader.componentCount);
    const std::size_t start = reader.baseOffset + index * reader.stride;
    for (int i = 0; i < reader.componentCount; ++i) {
        const std::size_t offset = start + static_cast<std::size_t>(i * reader.componentSize);
        switch (reader.componentType) {
        case 5120: out.push_back(static_cast<std::int8_t>((*reader.bytes)[offset])); break;
        case 5121: out.push_back((*reader.bytes)[offset]); break;
        case 5122: {
            std::int16_t value = static_cast<std::int16_t>((*reader.bytes)[offset] | ((*reader.bytes)[offset + 1] << 8));
            out.push_back(value);
            break;
        }
        case 5123: {
            std::uint16_t value = static_cast<std::uint16_t>((*reader.bytes)[offset] | ((*reader.bytes)[offset + 1] << 8));
            out.push_back(value);
            break;
        }
        case 5125: {
            std::uint32_t value =
                static_cast<std::uint32_t>((*reader.bytes)[offset]) |
                (static_cast<std::uint32_t>((*reader.bytes)[offset + 1]) << 8) |
                (static_cast<std::uint32_t>((*reader.bytes)[offset + 2]) << 16) |
                (static_cast<std::uint32_t>((*reader.bytes)[offset + 3]) << 24);
            out.push_back(static_cast<double>(value));
            break;
        }
        case 5126: {
            float value = 0.0f;
            std::uint32_t bits =
                static_cast<std::uint32_t>((*reader.bytes)[offset]) |
                (static_cast<std::uint32_t>((*reader.bytes)[offset + 1]) << 8) |
                (static_cast<std::uint32_t>((*reader.bytes)[offset + 2]) << 16) |
                (static_cast<std::uint32_t>((*reader.bytes)[offset + 3]) << 24);
            std::memcpy(&value, &bits, sizeof(bits));
            out.push_back(value);
            break;
        }
        default:
            out.push_back(0.0);
            break;
        }
    }
    return out;
}

std::array<double, 4> QuatMultiply(const std::array<double, 4>& a, const std::array<double, 4>& b) {
    const auto [ax, ay, az, aw] = a;
    const auto [bx, by, bz, bw] = b;
    return {
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz
    };
}

std::array<double, 3> QuatRotate(const std::array<double, 3>& v, const std::array<double, 4>& q) {
    const auto [x, y, z] = v;
    const auto [qx, qy, qz, qw] = q;
    const double tx = 2.0 * (qy * z - qz * y);
    const double ty = 2.0 * (qz * x - qx * z);
    const double tz = 2.0 * (qx * y - qy * x);
    return {
        x + qw * tx + (qy * tz - qz * ty),
        y + qw * ty + (qz * tx - qx * tz),
        z + qw * tz + (qx * ty - qy * tx)
    };
}

struct NodeTransform {
    int mesh = -1;
    std::array<double, 3> scale = { 1.0, 1.0, 1.0 };
    std::array<double, 4> rotation = { 0.0, 0.0, 0.0, 1.0 };
    std::array<double, 3> translation = { 0.0, 0.0, 0.0 };
};

NodeTransform ComposeTransform(const NodeTransform& parent, const json& node) {
    std::array<double, 3> ns = { 1.0, 1.0, 1.0 };
    std::array<double, 4> nr = { 0.0, 0.0, 0.0, 1.0 };
    std::array<double, 3> nt = { 0.0, 0.0, 0.0 };
    if (node.contains("scale") && node["scale"].size() >= 3) for (int i = 0; i < 3; ++i) ns[i] = node["scale"][i].get<double>();
    if (node.contains("rotation") && node["rotation"].size() >= 4) for (int i = 0; i < 4; ++i) nr[i] = node["rotation"][i].get<double>();
    if (node.contains("translation") && node["translation"].size() >= 3) for (int i = 0; i < 3; ++i) nt[i] = node["translation"][i].get<double>();
    const std::array<double, 3> scaledT = { nt[0] * parent.scale[0], nt[1] * parent.scale[1], nt[2] * parent.scale[2] };
    const std::array<double, 3> rotatedT = QuatRotate(scaledT, parent.rotation);
    NodeTransform current;
    current.mesh = node.value("mesh", -1);
    current.scale = { parent.scale[0] * ns[0], parent.scale[1] * ns[1], parent.scale[2] * ns[2] };
    current.rotation = QuatMultiply(parent.rotation, nr);
    current.translation = { parent.translation[0] + rotatedT[0], parent.translation[1] + rotatedT[1], parent.translation[2] + rotatedT[2] };
    return current;
}

std::array<double, 3> ApplyTransform(const std::array<double, 3>& p, const NodeTransform& t) {
    const std::array<double, 3> scaled = { p[0] * t.scale[0], p[1] * t.scale[1], p[2] * t.scale[2] };
    const std::array<double, 3> rotated = QuatRotate(scaled, t.rotation);
    return { rotated[0] + t.translation[0], rotated[1] + t.translation[1], rotated[2] + t.translation[2] };
}

void CollectSceneNodes(const json& gltf, std::vector<NodeTransform>* outNodes) {
    if (!outNodes || !gltf.contains("scenes")) return;
    const int sceneIndex = gltf.value("scene", 0);
    if (sceneIndex < 0 || sceneIndex >= gltf["scenes"].size()) return;
    const json& scene = gltf["scenes"][sceneIndex];
    const NodeTransform identity;
    std::function<void(int, const NodeTransform&)> visit = [&](int nodeIndex, const NodeTransform& parent) {
        const json& node = gltf["nodes"][nodeIndex];
        NodeTransform current = ComposeTransform(parent, node);
        if (current.mesh >= 0) outNodes->push_back(current);
        if (node.contains("children")) {
            for (const auto& child : node["children"]) visit(child.get<int>(), current);
        }
    };
    if (scene.contains("nodes")) {
        for (const auto& nodeIndex : scene["nodes"]) visit(nodeIndex.get<int>(), identity);
    }
}

bool LoadGlbPreview(const fs::path& filePath, PreviewMesh* outMesh, std::string* errorText, bool buildLods) {
    auto bytes = ReadFileBytes(filePath);
    if (bytes.size() < 20) {
        if (errorText) *errorText = "GLB file is too small";
        return false;
    }
    if (!(bytes[0] == 'g' && bytes[1] == 'l' && bytes[2] == 'T' && bytes[3] == 'F')) {
        if (errorText) *errorText = "File is not a GLB";
        return false;
    }
    json gltf;
    std::size_t binOffset = 0;
    std::size_t binLength = 0;
    std::size_t offset = 12;
    while (offset + 8 <= bytes.size()) {
        const std::uint32_t length = ReadU32Le(bytes, offset);
        const std::uint32_t type = ReadU32Le(bytes, offset + 4);
        offset += 8;
        if (offset + length > bytes.size()) break;
        if (type == 0x4E4F534A) {
            std::string jsonText(reinterpret_cast<const char*>(bytes.data() + offset), length);
            while (!jsonText.empty() && (jsonText.back() == '\0' || std::isspace(static_cast<unsigned char>(jsonText.back())))) jsonText.pop_back();
            gltf = json::parse(jsonText);
        } else if (type == 0x004E4942 && binLength == 0) {
            binOffset = offset;
            binLength = length;
        }
        offset += length;
    }
    if (gltf.is_null() || binLength == 0) {
        if (errorText) *errorText = "Missing GLB JSON or BIN chunk";
        return false;
    }

    outMesh->positions.clear();
    outMesh->colors.clear();
    outMesh->indices.clear();
    outMesh->positions.reserve(300000);
    outMesh->colors.reserve(400000);
    outMesh->indices.reserve(300000);

    std::vector<NodeTransform> sceneNodes;
    std::map<int, PreviewImage> imageCache;
    CollectSceneNodes(gltf, &sceneNodes);
    for (const auto& node : sceneNodes) {
        const json& mesh = gltf["meshes"][node.mesh];
        for (const auto& primitive : mesh["primitives"]) {
            if (!primitive.contains("attributes") || !primitive["attributes"].contains("POSITION")) continue;
            if (primitive.value("mode", 4) != 4) continue;
            AccessorReader positionsReader{};
            if (!BuildAccessorReader(gltf, bytes, binOffset, binLength,
                                     primitive["attributes"]["POSITION"].get<int>(), &positionsReader, errorText)) return false;
            AccessorReader uvReader{};
            const bool hasUv = primitive["attributes"].contains("TEXCOORD_0");
            if (hasUv && !BuildAccessorReader(gltf, bytes, binOffset, binLength,
                                              primitive["attributes"]["TEXCOORD_0"].get<int>(), &uvReader, errorText)) return false;
            const PreviewMaterial material = ResolvePreviewMaterial(gltf, primitive, bytes, binOffset, binLength, &imageCache);
            AccessorReader indexReader{};
            const bool hasIndices = primitive.contains("indices");
            if (hasIndices && !BuildAccessorReader(gltf, bytes, binOffset, binLength,
                                                   primitive["indices"].get<int>(), &indexReader, errorText)) return false;
            const std::size_t indexCount = hasIndices ? indexReader.length : positionsReader.length;
            const std::uint32_t vertexBase = static_cast<std::uint32_t>(outMesh->positions.size() / 3);
            for (std::size_t vertex = 0; vertex < positionsReader.length; ++vertex) {
                const auto values = ReadAccessorValues(positionsReader, vertex);
                if (values.size() < 3) continue;
                const auto p = ApplyTransform({ values[0], values[1], values[2] }, node);
                outMesh->positions.push_back(static_cast<float>(p[0]));
                outMesh->positions.push_back(static_cast<float>(p[1]));
                outMesh->positions.push_back(static_cast<float>(p[2]));
                const auto uv = hasUv && vertex < uvReader.length ? ReadAccessorValues(uvReader, vertex) : std::vector<double>{};
                const auto color = SamplePreviewMaterial(material, uv);
                outMesh->colors.insert(outMesh->colors.end(), color.begin(), color.end());
            }
            for (std::size_t i = 0; i + 2 < indexCount; i += 3) {
                std::array<std::size_t, 3> tri{};
                if (hasIndices) {
                    tri = {
                        static_cast<std::size_t>(ReadAccessorValues(indexReader, i)[0]),
                        static_cast<std::size_t>(ReadAccessorValues(indexReader, i + 1)[0]),
                        static_cast<std::size_t>(ReadAccessorValues(indexReader, i + 2)[0])
                    };
                } else {
                    tri = { i, i + 1, i + 2 };
                }
                if (std::any_of(tri.begin(), tri.end(), [&](std::size_t idx) { return idx >= positionsReader.length; })) continue;
                for (std::size_t idx : tri) outMesh->indices.push_back(vertexBase + static_cast<std::uint32_t>(idx));
            }
        }
    }
    bytes.clear();
    bytes.shrink_to_fit();
    return FinalizeMesh(outMesh, errorText, buildLods);
}

}  // namespace

bool LoadPreviewMesh(const fs::path& filePath, PreviewMesh* outMesh, std::string* errorText, bool buildLods) {
    if (!outMesh) {
        if (errorText) *errorText = "PreviewMesh output pointer was null";
        return false;
    }
    if (filePath.empty() || !fs::exists(filePath)) {
        if (errorText) *errorText = "Preview source file does not exist";
        return false;
    }
    std::wstring ext = filePath.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t ch) { return static_cast<wchar_t>(::towlower(ch)); });
    if (ext == L".obj") {
        if (!LoadObjPreview(filePath, outMesh, errorText, buildLods)) return false;
    } else if (ext == L".glb") {
        if (!LoadGlbPreview(filePath, outMesh, errorText, buildLods)) return false;
    } else {
        if (errorText) *errorText = "Preview currently supports only OBJ and GLB";
        return false;
    }
    if (!buildLods) {
        for (auto& lod : outMesh->lodIndices) lod.clear();
    }
    return true;
}

void BuildPreviewMeshLods(PreviewMesh* mesh) {
    if (!mesh || mesh->positions.empty() || mesh->indices.empty()) return;
    mesh->lodIndices[0].clear();
    constexpr std::array<int, 4> kLodResolution = { 0, 192, 96, 48 };
    for (std::size_t level = 1; level < mesh->lodIndices.size(); ++level) {
        mesh->lodIndices[level] = BuildClusterLod(*mesh, kLodResolution[level]);
    }
}

std::array<std::vector<std::uint32_t>, 4> BuildPreviewMeshProxyIndices(
    const PreviewMesh& mesh,
    const std::array<std::uint32_t, 4>& resolutions) {
    std::array<std::vector<std::uint32_t>, 4> result;
    if (mesh.positions.empty() || mesh.indices.empty()) return result;
    for (std::size_t level = 0; level < result.size(); ++level) {
        const std::uint32_t resolution = std::max<std::uint32_t>(1, resolutions[level]);
        result[level] = BuildClusterLod(mesh, static_cast<int>(resolution));
    }
    return result;
}

}  // namespace native_mc
