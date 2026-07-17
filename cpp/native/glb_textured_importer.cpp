#include "glb_textured_importer.h"

#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb_image.h"
#include "../third_party/json.hpp"

#include "world_ops.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace native_mc {

namespace {

constexpr int kTexturedScale = 4;

struct AccessorReader {
    std::size_t length = 0;
    int componentType = 0;
    int componentSize = 0;
    int componentCount = 0;
    std::size_t baseOffset = 0;
    std::size_t stride = 0;
    const std::vector<std::uint8_t>* bytes = nullptr;

    bool IsValid() const {
        return bytes != nullptr && componentSize > 0 && componentCount > 0;
    }

    std::vector<double> Get(std::size_t index) const {
        std::vector<double> out;
        out.reserve(componentCount);
        if (!IsValid()) return out;
        const std::size_t start = baseOffset + index * stride;
        for (int i = 0; i < componentCount; ++i) {
            const std::size_t offset = start + static_cast<std::size_t>(i * componentSize);
            switch (componentType) {
            case 5120:
                out.push_back(static_cast<std::int8_t>((*bytes)[offset]));
                break;
            case 5121:
                out.push_back((*bytes)[offset]);
                break;
            case 5122: {
                std::int16_t value = static_cast<std::int16_t>((*bytes)[offset] | ((*bytes)[offset + 1] << 8));
                out.push_back(value);
                break;
            }
            case 5123: {
                std::uint16_t value = static_cast<std::uint16_t>((*bytes)[offset] | ((*bytes)[offset + 1] << 8));
                out.push_back(value);
                break;
            }
            case 5125: {
                std::uint32_t value =
                    static_cast<std::uint32_t>((*bytes)[offset]) |
                    (static_cast<std::uint32_t>((*bytes)[offset + 1]) << 8) |
                    (static_cast<std::uint32_t>((*bytes)[offset + 2]) << 16) |
                    (static_cast<std::uint32_t>((*bytes)[offset + 3]) << 24);
                out.push_back(static_cast<double>(value));
                break;
            }
            case 5126: {
                float value = 0.0f;
                std::uint32_t bits =
                    static_cast<std::uint32_t>((*bytes)[offset]) |
                    (static_cast<std::uint32_t>((*bytes)[offset + 1]) << 8) |
                    (static_cast<std::uint32_t>((*bytes)[offset + 2]) << 16) |
                    (static_cast<std::uint32_t>((*bytes)[offset + 3]) << 24);
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
};

struct GlbDocument {
    json doc;
    std::vector<std::uint8_t> bin;
};

struct DecodedImage {
    int width = 0;
    int height = 0;
    int channels = 0;
    std::vector<std::uint8_t> rgba;
};

struct NodeSceneEntry {
    const json* node = nullptr;
    std::array<double, 3> scale = {1.0, 1.0, 1.0};
    std::array<double, 4> rotation = {0.0, 0.0, 0.0, 1.0};
    std::array<double, 3> translation = {0.0, 0.0, 0.0};
};

struct Manifest {
    std::array<double, 3> globalMin = {0.0, 0.0, 0.0};
    std::array<double, 3> globalMax = {0.0, 0.0, 0.0};
    std::array<int, 3> sizeBlocks = {0, 0, 0};
    std::array<int, 3> origin = {0, 0, 0};
    double blocksPerMeter = 4.0;
};

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

bool ReadGlb(const fs::path& filePath, GlbDocument* outDoc, std::string* errorText) {
    if (!outDoc) {
        if (errorText) *errorText = "GLB output pointer was null";
        return false;
    }
    const auto bytes = ReadFileBytes(filePath);
    if (bytes.size() < 20) {
        if (errorText) *errorText = "GLB file is too small";
        return false;
    }
    if (!(bytes[0] == 'g' && bytes[1] == 'l' && bytes[2] == 'T' && bytes[3] == 'F')) {
        if (errorText) *errorText = "File is not a GLB";
        return false;
    }
    std::size_t offset = 12;
    bool foundJson = false;
    bool foundBin = false;
    while (offset + 8 <= bytes.size()) {
        const std::uint32_t length = ReadU32Le(bytes, offset);
        const std::uint32_t type = ReadU32Le(bytes, offset + 4);
        offset += 8;
        if (offset + length > bytes.size()) {
            if (errorText) *errorText = "GLB chunk length is invalid";
            return false;
        }
        if (type == 0x4E4F534A) {
            std::string jsonText(reinterpret_cast<const char*>(bytes.data() + offset), length);
            while (!jsonText.empty() && (jsonText.back() == '\0' || std::isspace(static_cast<unsigned char>(jsonText.back())))) {
                jsonText.pop_back();
            }
            outDoc->doc = json::parse(jsonText);
            foundJson = true;
        } else if (type == 0x004E4942 && !foundBin) {
            outDoc->bin.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
            foundBin = true;
        }
        offset += length;
    }
    if (!foundJson || !foundBin) {
        if (errorText) *errorText = "GLB file is missing JSON or BIN chunk";
        return false;
    }
    return true;
}

int TypeCount(const std::string& typeName) {
    if (typeName == "SCALAR") return 1;
    if (typeName == "VEC2") return 2;
    if (typeName == "VEC3") return 3;
    if (typeName == "VEC4") return 4;
    return 0;
}

int ComponentSize(int componentType) {
    switch (componentType) {
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

bool BuildAccessorReader(const GlbDocument& glb, int accessorIndex, AccessorReader* outReader, std::string* errorText) {
    if (!outReader) {
        if (errorText) *errorText = "Accessor output pointer was null";
        return false;
    }
    if (!glb.doc.contains("accessors") || accessorIndex < 0 || accessorIndex >= glb.doc["accessors"].size()) {
        if (errorText) *errorText = "Accessor index is out of range";
        return false;
    }
    const json& accessor = glb.doc["accessors"][accessorIndex];
    const int bufferViewIndex = accessor.value("bufferView", -1);
    if (!glb.doc.contains("bufferViews") || bufferViewIndex < 0 || bufferViewIndex >= glb.doc["bufferViews"].size()) {
        if (errorText) *errorText = "Accessor is missing a valid buffer view";
        return false;
    }
    const json& view = glb.doc["bufferViews"][bufferViewIndex];
    outReader->componentType = accessor.value("componentType", 0);
    outReader->componentSize = ComponentSize(outReader->componentType);
    outReader->componentCount = TypeCount(accessor.value("type", ""));
    outReader->length = accessor.value("count", 0);
    outReader->baseOffset = view.value("byteOffset", 0u) + accessor.value("byteOffset", 0u);
    outReader->stride = view.value("byteStride", static_cast<unsigned int>(outReader->componentSize * outReader->componentCount));
    outReader->bytes = &glb.bin;
    if (!outReader->IsValid()) {
        if (errorText) *errorText = "Unsupported accessor type";
        return false;
    }
    return true;
}

std::vector<std::uint8_t> BufferViewSlice(const GlbDocument& glb, int viewIndex) {
    const json& view = glb.doc["bufferViews"][viewIndex];
    const std::size_t start = view.value("byteOffset", 0u);
    const std::size_t length = view.value("byteLength", 0u);
    return std::vector<std::uint8_t>(glb.bin.begin() + static_cast<std::ptrdiff_t>(start),
        glb.bin.begin() + static_cast<std::ptrdiff_t>(start + length));
}

bool DecodeImageFromBufferView(const GlbDocument& glb, int imageIndex, DecodedImage* outImage, std::string* errorText) {
    if (!outImage) {
        if (errorText) *errorText = "Image output pointer was null";
        return false;
    }
    if (!glb.doc.contains("images") || imageIndex < 0 || imageIndex >= glb.doc["images"].size()) {
        if (errorText) *errorText = "Image index is out of range";
        return false;
    }
    const json& image = glb.doc["images"][imageIndex];
    if (!image.contains("bufferView")) {
        if (errorText) *errorText = "Image is missing a bufferView";
        return false;
    }
    const auto bytes = BufferViewSlice(glb, image["bufferView"].get<int>());
    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* rgba = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels, 4);
    if (!rgba) {
        if (errorText) *errorText = "stb_image failed to decode embedded image";
        return false;
    }
    outImage->width = width;
    outImage->height = height;
    outImage->channels = 4;
    outImage->rgba.assign(rgba, rgba + (static_cast<std::size_t>(width) * height * 4));
    stbi_image_free(rgba);
    return true;
}

std::array<double, 4> QuatMultiply(const std::array<double, 4>& a, const std::array<double, 4>& b) {
    const double ax = a[0], ay = a[1], az = a[2], aw = a[3];
    const double bx = b[0], by = b[1], bz = b[2], bw = b[3];
    return {
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz
    };
}

std::array<double, 3> QuatRotate(const std::array<double, 3>& v, const std::array<double, 4>& q) {
    const double x = v[0], y = v[1], z = v[2];
    const double qx = q[0], qy = q[1], qz = q[2], qw = q[3];
    const double tx = 2.0 * (qy * z - qz * y);
    const double ty = 2.0 * (qz * x - qx * z);
    const double tz = 2.0 * (qx * y - qy * x);
    return {
        x + qw * tx + (qy * tz - qz * ty),
        y + qw * ty + (qz * tx - qx * tz),
        z + qw * tz + (qx * ty - qy * tx)
    };
}

NodeSceneEntry ComposeTransform(const NodeSceneEntry& parent, const json& node) {
    std::array<double, 3> ns = {1.0, 1.0, 1.0};
    std::array<double, 4> nr = {0.0, 0.0, 0.0, 1.0};
    std::array<double, 3> nt = {0.0, 0.0, 0.0};
    if (node.contains("scale") && node["scale"].size() >= 3) {
        for (int i = 0; i < 3; ++i) ns[i] = node["scale"][i].get<double>();
    }
    if (node.contains("rotation") && node["rotation"].size() >= 4) {
        for (int i = 0; i < 4; ++i) nr[i] = node["rotation"][i].get<double>();
    }
    if (node.contains("translation") && node["translation"].size() >= 3) {
        for (int i = 0; i < 3; ++i) nt[i] = node["translation"][i].get<double>();
    }
    std::array<double, 3> scaledT = {
        nt[0] * parent.scale[0],
        nt[1] * parent.scale[1],
        nt[2] * parent.scale[2]
    };
    std::array<double, 3> rotatedT = QuatRotate(scaledT, parent.rotation);
    NodeSceneEntry result;
    result.node = &node;
    result.scale = {
        parent.scale[0] * ns[0],
        parent.scale[1] * ns[1],
        parent.scale[2] * ns[2]
    };
    result.rotation = QuatMultiply(parent.rotation, nr);
    result.translation = {
        parent.translation[0] + rotatedT[0],
        parent.translation[1] + rotatedT[1],
        parent.translation[2] + rotatedT[2]
    };
    return result;
}

std::array<double, 3> ApplyTransform(const std::array<double, 3>& point, const NodeSceneEntry& transform) {
    const std::array<double, 3> scaled = {
        point[0] * transform.scale[0],
        point[1] * transform.scale[1],
        point[2] * transform.scale[2]
    };
    const std::array<double, 3> rotated = QuatRotate(scaled, transform.rotation);
    return {
        rotated[0] + transform.translation[0],
        rotated[1] + transform.translation[1],
        rotated[2] + transform.translation[2]
    };
}

std::array<double, 3> ApplyUserTransform(std::array<double, 3> point, const GlbTexturedOptions& options) {
    if (options.flipX) point[0] = -point[0];
    if (options.flipZ) point[2] = -point[2];
    if (options.modelRotation.has_value()) {
        const auto& wxyz = *options.modelRotation;
        return QuatRotate(point, { wxyz[1], wxyz[2], wxyz[3], wxyz[0] });
    }
    const double radians = options.rotateYDeg * 3.14159265358979323846 / 180.0;
    const double cosine = std::cos(radians);
    const double sine = std::sin(radians);
    return { point[0] * cosine + point[2] * sine, point[1], -point[0] * sine + point[2] * cosine };
}

std::vector<NodeSceneEntry> SceneNodes(const GlbDocument& glb) {
    std::vector<NodeSceneEntry> out;
    const int sceneIndex = glb.doc.value("scene", 0);
    if (!glb.doc.contains("scenes") || sceneIndex < 0 || sceneIndex >= glb.doc["scenes"].size()) {
        return out;
    }
    const json& scene = glb.doc["scenes"][sceneIndex];
    const NodeSceneEntry identity;
    std::function<void(int, const NodeSceneEntry&)> visit = [&](int nodeIndex, const NodeSceneEntry& parent) {
        const json& node = glb.doc["nodes"][nodeIndex];
        NodeSceneEntry current = ComposeTransform(parent, node);
        if (node.contains("mesh")) {
            out.push_back(current);
        }
        if (node.contains("children")) {
            for (const auto& child : node["children"]) {
                visit(child.get<int>(), current);
            }
        }
    };
    if (scene.contains("nodes")) {
        for (const auto& nodeIndex : scene["nodes"]) {
            visit(nodeIndex.get<int>(), identity);
        }
    }
    return out;
}

std::vector<fs::path> CollectAllFiles(const fs::path& glbDir) {
    std::vector<fs::path> files;
    if (!fs::exists(glbDir)) {
        return files;
    }
    for (const auto& entry : fs::directory_iterator(glbDir)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](char ch) { return static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); });
        if (ext == ".glb") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::vector<fs::path> CollectSelectedFiles(const GlbTexturedOptions& options) {
    if (!options.onlyFile.empty()) {
        if (options.onlyFile.is_absolute()) return { options.onlyFile };
        return { options.glbDir / options.onlyFile };
    }
    return CollectAllFiles(options.glbDir);
}

std::vector<std::array<double, 3>> BBoxCorners(const std::vector<double>& min, const std::vector<double>& max) {
    std::vector<std::array<double, 3>> out;
    for (double x : { min[0], max[0] }) {
        for (double y : { min[1], max[1] }) {
            for (double z : { min[2], max[2] }) {
                out.push_back({x, y, z});
            }
        }
    }
    return out;
}

void Extend(std::array<double, 3>* min, std::array<double, 3>* max, const std::array<double, 3>& p) {
    for (int i = 0; i < 3; ++i) {
        (*min)[i] = std::min((*min)[i], p[i]);
        (*max)[i] = std::max((*max)[i], p[i]);
    }
}

bool ComputeManifest(const std::vector<fs::path>& allFiles, const GlbTexturedOptions& options, Manifest* outManifest, std::string* errorText) {
    if (!outManifest) {
        if (errorText) *errorText = "Manifest output pointer was null";
        return false;
    }
    std::array<double, 3> min = { INFINITY, INFINITY, INFINITY };
    std::array<double, 3> max = { -INFINITY, -INFINITY, -INFINITY };
    for (const auto& filePath : allFiles) {
        GlbDocument glb;
        if (!ReadGlb(filePath, &glb, errorText)) return false;
        for (const auto& entry : SceneNodes(glb)) {
            const json& node = *entry.node;
            const json& mesh = glb.doc["meshes"][node["mesh"].get<int>()];
            for (const auto& primitive : mesh["primitives"]) {
                const json& accessor = glb.doc["accessors"][primitive["attributes"]["POSITION"].get<int>()];
                const auto accessorMin = accessor["min"].get<std::vector<double>>();
                const auto accessorMax = accessor["max"].get<std::vector<double>>();
                for (const auto& corner : BBoxCorners(accessorMin, accessorMax)) {
                    Extend(&min, &max, ApplyUserTransform(ApplyTransform(corner, entry), options));
                }
            }
        }
    }
    outManifest->globalMin = min;
    outManifest->globalMax = max;
    outManifest->blocksPerMeter = options.scaleBlocksPerMeter;
    for (int i = 0; i < 3; ++i) {
        outManifest->sizeBlocks[i] = static_cast<int>(std::ceil((max[i] - min[i]) * outManifest->blocksPerMeter));
    }
    outManifest->origin[0] = options.center.x ? (*options.center.x - outManifest->sizeBlocks[0] / 2) : 0;
    outManifest->origin[1] = options.center.y ? (*options.center.y - outManifest->sizeBlocks[1] / 2) : options.baseY;
    outManifest->origin[2] = options.center.z ? (*options.center.z - outManifest->sizeBlocks[2] / 2) : 0;
    return true;
}

std::array<int, 3> WorldPoint(const std::array<double, 3>& point, const Manifest& manifest) {
    return {
        static_cast<int>(std::llround((point[0] - manifest.globalMin[0]) * manifest.blocksPerMeter + manifest.origin[0])),
        static_cast<int>(std::llround((point[1] - manifest.globalMin[1]) * manifest.blocksPerMeter + manifest.origin[1])),
        static_cast<int>(std::llround((point[2] - manifest.globalMin[2]) * manifest.blocksPerMeter + manifest.origin[2]))
    };
}

double Dist(const std::array<int, 3>& a, const std::array<int, 3>& b) {
    const double dx = static_cast<double>(a[0] - b[0]);
    const double dy = static_cast<double>(a[1] - b[1]);
    const double dz = static_cast<double>(a[2] - b[2]);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

std::uint32_t PackLocalBlock(int x, int y, int z) {
    return static_cast<std::uint32_t>(((y - kMinY) << 8) | (z << 4) | x);
}

using ChunkBlockMap = std::map<ChunkCoord, std::map<std::uint32_t, std::string>>;

void AddWorldBlock(ChunkBlockMap* chunkMap, int x, int y, int z, const std::string& blockName) {
    if (y < kMinY || y >= kMinY + kHeight) return;
    const int cx = static_cast<int>(std::floor(static_cast<double>(x) / 16.0));
    const int cz = static_cast<int>(std::floor(static_cast<double>(z) / 16.0));
    const int lx = ((x % 16) + 16) % 16;
    const int lz = ((z % 16) + 16) % 16;
    (*chunkMap)[ChunkCoord{cx, cz}][PackLocalBlock(lx, y, lz)] = blockName;
}

std::optional<std::array<int, 3>> TextureColour(const DecodedImage* image, double u, double v) {
    if (!image || image->width <= 0 || image->height <= 0) return std::nullopt;
    const double uu = u - std::floor(u);
    const double vv = v - std::floor(v);
    const int x = std::clamp(static_cast<int>(std::floor(uu * image->width)), 0, image->width - 1);
    const int y = std::clamp(static_cast<int>(std::floor((1.0 - vv) * image->height)), 0, image->height - 1);
    const std::size_t offset = (static_cast<std::size_t>(y) * image->width + x) * 4;
    return std::array<int, 3>{
        image->rgba[offset],
        image->rgba[offset + 1],
        image->rgba[offset + 2]
    };
}

const DecodedImage* TextureForPrimitive(const GlbDocument& glb, const json& primitive, std::map<int, DecodedImage>* textureCache, std::string* errorText) {
    if (!primitive.contains("material")) return nullptr;
    const json& material = glb.doc["materials"][primitive["material"].get<int>()];
    if (!material.contains("pbrMetallicRoughness")) return nullptr;
    const json& pbr = material["pbrMetallicRoughness"];
    if (!pbr.contains("baseColorTexture")) return nullptr;
    const int textureIndex = pbr["baseColorTexture"]["index"].get<int>();
    if (!glb.doc.contains("textures") || textureIndex < 0 || textureIndex >= glb.doc["textures"].size()) return nullptr;
    const int imageIndex = glb.doc["textures"][textureIndex].value("source", -1);
    if (imageIndex < 0) return nullptr;
    auto it = textureCache->find(imageIndex);
    if (it != textureCache->end()) return &it->second;
    DecodedImage decoded;
    if (!DecodeImageFromBufferView(glb, imageIndex, &decoded, errorText)) return nullptr;
    auto [insertedIt, _] = textureCache->emplace(imageIndex, std::move(decoded));
    return &insertedIt->second;
}

std::array<double, 4> BaseColorFactorForPrimitive(const GlbDocument& glb, const json& primitive) {
    if (!primitive.contains("material")) return {1.0, 1.0, 1.0, 1.0};
    const json& material = glb.doc["materials"][primitive["material"].get<int>()];
    if (!material.contains("pbrMetallicRoughness")) return {1.0, 1.0, 1.0, 1.0};
    const json& factor = material["pbrMetallicRoughness"].value("baseColorFactor", json::array());
    if (!factor.is_array() || factor.size() < 4) return {1.0, 1.0, 1.0, 1.0};
    return {factor[0].get<double>(), factor[1].get<double>(), factor[2].get<double>(), factor[3].get<double>()};
}

struct BuildStats {
    ChunkBlockMap chunkMap;
    std::size_t triangles = 0;
    std::size_t blocks = 0;
    std::size_t chunks = 0;
    std::size_t textures = 0;
    std::size_t skippedNoIndex = 0;
    std::size_t skippedNoUv = 0;
    std::size_t duplicateSamples = 0;
};

void SampleTriangle(ChunkBlockMap* chunkMap,
    const std::array<int, 3>& a,
    const std::array<int, 3>& b,
    const std::array<int, 3>& c,
    const std::vector<double>* auv,
    const std::vector<double>* buv,
    const std::vector<double>* cuv,
    const DecodedImage* texture,
    const std::array<double, 4>& baseColorFactor,
    const Palette& palette,
    const std::string& fallbackBlock,
    BuildStats* stats,
    int dedupeSampleSteps) {
    const int steps = std::max(1, static_cast<int>(std::ceil(std::max({Dist(a, b), Dist(b, c), Dist(c, a)}))));
    std::set<std::tuple<int, int, int>> seen;
    const bool useSeen = steps <= dedupeSampleSteps;
    for (int i = 0; i <= steps; ++i) {
        for (int j = 0; j <= steps - i; ++j) {
            const double u = static_cast<double>(i) / steps;
            const double v = static_cast<double>(j) / steps;
            const double w = 1.0 - u - v;
            const int x = static_cast<int>(std::llround(a[0] * w + b[0] * u + c[0] * v));
            const int y = static_cast<int>(std::llround(a[1] * w + b[1] * u + c[1] * v));
            const int z = static_cast<int>(std::llround(a[2] * w + b[2] * u + c[2] * v));
            if (useSeen) {
                auto key = std::make_tuple(x, y, z);
                if (seen.find(key) != seen.end()) {
                    stats->duplicateSamples += 1;
                    continue;
                }
                seen.insert(key);
            }
            std::string block = fallbackBlock;
            if (texture && auv && buv && cuv) {
                const double tu = (*auv)[0] * w + (*buv)[0] * u + (*cuv)[0] * v;
                const double tv = (*auv)[1] * w + (*buv)[1] * u + (*cuv)[1] * v;
                auto colour = TextureColour(texture, tu, tv);
                if (colour.has_value()) {
                    block = palette.Nearest(
                        static_cast<int>(std::llround(std::clamp((*colour)[0] * baseColorFactor[0], 0.0, 255.0))),
                        static_cast<int>(std::llround(std::clamp((*colour)[1] * baseColorFactor[1], 0.0, 255.0))),
                        static_cast<int>(std::llround(std::clamp((*colour)[2] * baseColorFactor[2], 0.0, 255.0))));
                }
            }
            AddWorldBlock(chunkMap, x, y, z, block);
        }
    }
}

bool BuildBlocksForGlb(const fs::path& filePath, const Manifest& manifest, const Palette& palette, const GlbTexturedOptions& options, BuildStats* outStats, std::string* errorText) {
    if (!outStats) {
        if (errorText) *errorText = "BuildStats output pointer was null";
        return false;
    }
    GlbDocument glb;
    if (!ReadGlb(filePath, &glb, errorText)) return false;
    std::map<int, DecodedImage> textureCache;
    for (const auto& entry : SceneNodes(glb)) {
        const json& node = *entry.node;
        const json& mesh = glb.doc["meshes"][node["mesh"].get<int>()];
        for (const auto& primitive : mesh["primitives"]) {
            if (!primitive.contains("indices")) {
                outStats->skippedNoIndex += 1;
                continue;
            }
            AccessorReader positions;
            AccessorReader indices;
            if (!BuildAccessorReader(glb, primitive["attributes"]["POSITION"].get<int>(), &positions, errorText) ||
                !BuildAccessorReader(glb, primitive["indices"].get<int>(), &indices, errorText)) {
                return false;
            }
            std::optional<AccessorReader> uvs;
            if (primitive.contains("attributes") && primitive["attributes"].contains("TEXCOORD_0")) {
                AccessorReader uvReader;
                if (!BuildAccessorReader(glb, primitive["attributes"]["TEXCOORD_0"].get<int>(), &uvReader, errorText)) {
                    return false;
                }
                uvs = uvReader;
            } else {
                outStats->skippedNoUv += 1;
            }
            const DecodedImage* texture = TextureForPrimitive(glb, primitive, &textureCache, errorText);
            const auto baseColorFactor = BaseColorFactorForPrimitive(glb, primitive);
            for (std::size_t i = 0; i + 2 < indices.length; i += 3) {
                const auto ia = static_cast<std::size_t>(indices.Get(i)[0]);
                const auto ib = static_cast<std::size_t>(indices.Get(i + 1)[0]);
                const auto ic = static_cast<std::size_t>(indices.Get(i + 2)[0]);
                const auto pa = positions.Get(ia);
                const auto pb = positions.Get(ib);
                const auto pc = positions.Get(ic);
                const std::array<double, 3> aSrc = {pa[0], pa[1], pa[2]};
                const std::array<double, 3> bSrc = {pb[0], pb[1], pb[2]};
                const std::array<double, 3> cSrc = {pc[0], pc[1], pc[2]};
                const auto a = WorldPoint(ApplyUserTransform(ApplyTransform(aSrc, entry), options), manifest);
                const auto b = WorldPoint(ApplyUserTransform(ApplyTransform(bSrc, entry), options), manifest);
                const auto c = WorldPoint(ApplyUserTransform(ApplyTransform(cSrc, entry), options), manifest);
                std::optional<std::vector<double>> auv;
                std::optional<std::vector<double>> buv;
                std::optional<std::vector<double>> cuv;
                if (uvs.has_value()) {
                    auv = uvs->Get(ia);
                    buv = uvs->Get(ib);
                    cuv = uvs->Get(ic);
                }
                SampleTriangle(&outStats->chunkMap, a, b, c,
                    auv ? &*auv : nullptr, buv ? &*buv : nullptr, cuv ? &*cuv : nullptr,
                    texture, baseColorFactor, palette, options.fallbackBlock, outStats, options.dedupeSampleSteps);
                outStats->triangles += 1;
            }
        }
    }
    outStats->textures = textureCache.size();
    for (const auto& [coord, packed] : outStats->chunkMap) {
        (void)coord;
        outStats->blocks += packed.size();
    }
    outStats->chunks = outStats->chunkMap.size();
    return true;
}

std::map<ChunkCoord, PackedBlockMap> ConvertChunkMap(const ChunkBlockMap& source) {
    std::map<ChunkCoord, PackedBlockMap> out;
    for (const auto& [coord, blockMap] : source) {
        PackedBlockMap items;
        items.reserve(blockMap.size());
        for (const auto& [packed, block] : blockMap) {
            items.push_back({packed, block});
        }
        out.emplace(coord, std::move(items));
    }
    return out;
}

}  // namespace

bool ImportGlbTextured(const GlbTexturedOptions& options, GlbTexturedImportResult* outResult, std::string* errorText) {
    if (!outResult) {
        if (errorText) *errorText = "Import result output pointer was null";
        return false;
    }
    const auto allFiles = CollectAllFiles(options.glbDir);
    if (allFiles.empty()) {
        if (errorText) *errorText = "No GLB files were found under the GLB directory";
        return false;
    }
    if (options.onlyFile.empty() && !options.allFiles) {
        if (errorText) *errorText = "Directory import requires allFiles=true or an explicit onlyFile";
        return false;
    }
    const auto selected = CollectSelectedFiles(options);
    if (selected.empty()) {
        if (errorText) *errorText = "No GLB files were selected";
        return false;
    }

    Palette palette;
    if (!LoadPalette(options.projectRoot, options.palette, &palette, errorText)) {
        return false;
    }

    Manifest manifest;
    if (!ComputeManifest(selected, options, &manifest, errorText)) {
        return false;
    }

    outResult->worldDir = options.worldDir;
    outResult->sizeBlocks = {manifest.sizeBlocks[0], manifest.sizeBlocks[1], manifest.sizeBlocks[2]};
    outResult->globalMin = {manifest.globalMin[0], manifest.globalMin[1], manifest.globalMin[2]};
    outResult->globalMax = {manifest.globalMax[0], manifest.globalMax[1], manifest.globalMax[2]};
    outResult->origin = {manifest.origin[0], manifest.origin[1], manifest.origin[2]};
    outResult->paletteSize = palette.blocks().size();
    outResult->files.clear();

    const fs::path regionDir = ResolveOverworldRegionDir(options.worldDir);
    for (const auto& filePath : selected) {
        BuildStats stats;
        const auto started = std::chrono::steady_clock::now();
        if (!BuildBlocksForGlb(filePath, manifest, palette, options, &stats, errorText)) {
            return false;
        }
        if (options.batchBlockLimit > 0 && stats.blocks > static_cast<std::size_t>(options.batchBlockLimit)) {
            if (errorText) *errorText = "Generated block count exceeds batchBlockLimit";
            return false;
        }
        GlbTexturedFileResult fileResult;
        fileResult.name = filePath.filename().string();
        fileResult.triangles = stats.triangles;
        fileResult.blocks = stats.blocks;
        fileResult.chunks = stats.chunks;
        fileResult.textures = stats.textures;
        fileResult.skippedNoIndex = stats.skippedNoIndex;
        fileResult.skippedNoUv = stats.skippedNoUv;
        fileResult.duplicateSamples = stats.duplicateSamples;
        std::size_t written = 0;
        std::size_t regions = 0;
        const auto chunkBlocks = ConvertChunkMap(stats.chunkMap);
        if (!WriteManyChunkBlockMap(regionDir, chunkBlocks, true, options.overwrite, &written, &regions, errorText)) {
            return false;
        }
        fileResult.written = written;
        fileResult.regions = regions;
        fileResult.milliseconds = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count());
        outResult->files.push_back(std::move(fileResult));
    }
    return true;
}

}  // namespace native_mc
