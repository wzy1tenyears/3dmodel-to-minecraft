#include "glb_surface_importer.h"

#include "world_ops.h"

#include "../third_party/json.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace native_mc {

namespace {

constexpr int kSurfaceScale = 4;

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
        if (!IsValid()) {
            return out;
        }
        const std::size_t start = baseOffset + index * stride;
        for (int i = 0; i < componentCount; ++i) {
            const std::size_t offset = start + static_cast<std::size_t>(i * componentSize);
            switch (componentType) {
            case 5121:
                out.push_back((*bytes)[offset]);
                break;
            case 5123:
                out.push_back((*bytes)[offset] | ((*bytes)[offset + 1] << 8));
                break;
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

std::vector<std::uint8_t> ReadFileBytes(const fs::path& filePath) {
    std::ifstream in(filePath, std::ios::binary);
    if (!in) {
        return {};
    }
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
    const std::vector<std::uint8_t> bytes = ReadFileBytes(filePath);
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
            while (!jsonText.empty() && (jsonText.back() == '\0' || jsonText.back() == ' ' || jsonText.back() == '\n' || jsonText.back() == '\r' || jsonText.back() == '\t')) {
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
    case 5121: return 1;
    case 5123: return 2;
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

std::array<double, 3> QuatRotate(const std::array<double, 3>& value, const std::array<double, 4>& quat) {
    const double x = value[0];
    const double y = value[1];
    const double z = value[2];
    const double qx = quat[0];
    const double qy = quat[1];
    const double qz = quat[2];
    const double qw = quat[3];
    const double tx = 2.0 * (qy * z - qz * y);
    const double ty = 2.0 * (qz * x - qx * z);
    const double tz = 2.0 * (qx * y - qy * x);
    return {
        x + qw * tx + (qy * tz - qz * ty),
        y + qw * ty + (qz * tx - qx * tz),
        z + qw * tz + (qx * ty - qy * tx),
    };
}

std::array<double, 3> ApplyNodeTransform(const std::array<double, 3>& value, const json& node) {
    std::array<double, 3> scale = { 1.0, 1.0, 1.0 };
    std::array<double, 4> rotation = { 0.0, 0.0, 0.0, 1.0 };
    std::array<double, 3> translation = { 0.0, 0.0, 0.0 };
    if (node.contains("scale") && node["scale"].is_array() && node["scale"].size() >= 3) {
        for (int i = 0; i < 3; ++i) scale[i] = node["scale"][i].get<double>();
    }
    if (node.contains("rotation") && node["rotation"].is_array() && node["rotation"].size() >= 4) {
        for (int i = 0; i < 4; ++i) rotation[i] = node["rotation"][i].get<double>();
    }
    if (node.contains("translation") && node["translation"].is_array() && node["translation"].size() >= 3) {
        for (int i = 0; i < 3; ++i) translation[i] = node["translation"][i].get<double>();
    }
    const std::array<double, 3> scaled = {
        value[0] * scale[0],
        value[1] * scale[1],
        value[2] * scale[2],
    };
    const std::array<double, 3> rotated = QuatRotate(scaled, rotation);
    return {
        rotated[0] + translation[0],
        rotated[1] + translation[1],
        rotated[2] + translation[2],
    };
}

std::array<double, 3> ApplyUserTransform(std::array<double, 3> point, const GlbSurfaceOptions& options) {
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

std::vector<std::array<double, 3>> BBoxCorners(const std::vector<double>& min, const std::vector<double>& max) {
    std::vector<std::array<double, 3>> corners;
    for (double x : { min[0], max[0] }) {
        for (double y : { min[1], max[1] }) {
            for (double z : { min[2], max[2] }) {
                corners.push_back({ x, y, z });
            }
        }
    }
    return corners;
}

void ExtendBounds(std::array<double, 3>* min, std::array<double, 3>* max, const std::array<double, 3>& point) {
    for (int i = 0; i < 3; ++i) {
        (*min)[i] = std::min((*min)[i], point[i]);
        (*max)[i] = std::max((*max)[i], point[i]);
    }
}

struct SurfaceManifest {
    std::array<double, 3> globalMin = { 0.0, 0.0, 0.0 };
    std::array<double, 3> globalMax = { 0.0, 0.0, 0.0 };
    std::array<int, 3> sizeBlocks = { 0, 0, 0 };
    std::array<int, 3> origin = { 0, 0, 0 };
    std::size_t triangles = 0;
    double blocksPerMeter = 4.0;
};

bool ComputeManifest(const std::vector<fs::path>& filePaths, const GlbSurfaceOptions& options, SurfaceManifest* outManifest, std::string* errorText) {
    if (!outManifest) {
        if (errorText) *errorText = "Manifest output pointer was null";
        return false;
    }
    std::array<double, 3> min = { INFINITY, INFINITY, INFINITY };
    std::array<double, 3> max = { -INFINITY, -INFINITY, -INFINITY };
    std::size_t triangles = 0;

    for (const auto& filePath : filePaths) {
        GlbDocument glb;
        if (!ReadGlb(filePath, &glb, errorText)) {
            return false;
        }
        const int sceneIndex = glb.doc.value("scene", 0);
        if (!glb.doc.contains("scenes") || sceneIndex < 0 || sceneIndex >= glb.doc["scenes"].size()) {
            if (errorText) *errorText = "GLB scene index is invalid";
            return false;
        }
        const json& scene = glb.doc["scenes"][sceneIndex];
        for (const auto& nodeIndexValue : scene.value("nodes", json::array())) {
            const int nodeIndex = nodeIndexValue.get<int>();
            const json& node = glb.doc["nodes"][nodeIndex];
            if (!node.contains("mesh")) {
                continue;
            }
            const json& mesh = glb.doc["meshes"][node["mesh"].get<int>()];
            for (const auto& primitive : mesh["primitives"]) {
                const int positionAccessorIndex = primitive["attributes"]["POSITION"].get<int>();
                const json& accessor = glb.doc["accessors"][positionAccessorIndex];
                const std::vector<double> accessorMin = accessor["min"].get<std::vector<double>>();
                const std::vector<double> accessorMax = accessor["max"].get<std::vector<double>>();
                for (const auto& corner : BBoxCorners(accessorMin, accessorMax)) {
                    ExtendBounds(&min, &max, ApplyUserTransform(ApplyNodeTransform(corner, node), options));
                }
                if (primitive.contains("indices")) {
                    triangles += glb.doc["accessors"][primitive["indices"].get<int>()].value("count", 0) / 3;
                }
            }
        }
    }

    outManifest->globalMin = min;
    outManifest->globalMax = max;
    outManifest->triangles = triangles;
    outManifest->blocksPerMeter = options.scaleBlocksPerMeter;
    for (int i = 0; i < 3; ++i) {
        outManifest->sizeBlocks[i] = static_cast<int>(std::ceil((max[i] - min[i]) * outManifest->blocksPerMeter));
    }
    outManifest->origin[0] = options.center.x ? (*options.center.x - outManifest->sizeBlocks[0] / 2) : 0;
    outManifest->origin[1] = options.center.y ? (*options.center.y - outManifest->sizeBlocks[1] / 2) : options.baseY;
    outManifest->origin[2] = options.center.z ? (*options.center.z - outManifest->sizeBlocks[2] / 2) : 0;
    return true;
}

std::array<int, 3> WorldPoint(const std::array<double, 3>& point, const SurfaceManifest& manifest) {
    return {
        static_cast<int>(std::llround((point[0] - manifest.globalMin[0]) * manifest.blocksPerMeter + manifest.origin[0])),
        static_cast<int>(std::llround((point[1] - manifest.globalMin[1]) * manifest.blocksPerMeter + manifest.origin[1])),
        static_cast<int>(std::llround((point[2] - manifest.globalMin[2]) * manifest.blocksPerMeter + manifest.origin[2])),
    };
}

double Distance(const std::array<int, 3>& a, const std::array<int, 3>& b) {
    const double dx = static_cast<double>(a[0] - b[0]);
    const double dy = static_cast<double>(a[1] - b[1]);
    const double dz = static_cast<double>(a[2] - b[2]);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

std::uint32_t PackLocalBlock(int x, int y, int z) {
    const int cy = y - kMinY;
    return static_cast<std::uint32_t>(((cy << 8) | (z << 4) | x) & 0xFFFFFFFFu);
}

void AddWorldBlock(std::map<ChunkCoord, std::set<std::uint32_t>>* chunkMap, int x, int y, int z) {
    if (y < kMinY || y >= kMinY + kHeight) {
        return;
    }
    const int cx = static_cast<int>(std::floor(static_cast<double>(x) / 16.0));
    const int cz = static_cast<int>(std::floor(static_cast<double>(z) / 16.0));
    const int lx = ((x % 16) + 16) % 16;
    const int lz = ((z % 16) + 16) % 16;
    (*chunkMap)[ChunkCoord{ cx, cz }].insert(PackLocalBlock(lx, y, lz));
}

void SampleTriangle(std::map<ChunkCoord, std::set<std::uint32_t>>* chunkMap, const std::array<int, 3>& a, const std::array<int, 3>& b, const std::array<int, 3>& c) {
    const double ab = Distance(a, b);
    const double bc = Distance(b, c);
    const double ca = Distance(c, a);
    const int steps = std::max(1, static_cast<int>(std::ceil(std::max({ ab, bc, ca }))));
    for (int i = 0; i <= steps; ++i) {
        for (int j = 0; j <= steps - i; ++j) {
            const double u = static_cast<double>(i) / steps;
            const double v = static_cast<double>(j) / steps;
            const double w = 1.0 - u - v;
            const int x = static_cast<int>(std::llround(a[0] * w + b[0] * u + c[0] * v));
            const int y = static_cast<int>(std::llround(a[1] * w + b[1] * u + c[1] * v));
            const int z = static_cast<int>(std::llround(a[2] * w + b[2] * u + c[2] * v));
            AddWorldBlock(chunkMap, x, y, z);
        }
    }
}

struct BuiltSurface {
    std::map<ChunkCoord, std::set<std::uint32_t>> chunkMap;
    std::size_t triangles = 0;
    std::size_t blocks = 0;
    std::size_t chunks = 0;
};

bool BuildSurfaceBlocks(const fs::path& filePath, const SurfaceManifest& manifest, const GlbSurfaceOptions& options,
                        BuiltSurface* outSurface, std::string* errorText) {
    if (!outSurface) {
        if (errorText) *errorText = "Build output pointer was null";
        return false;
    }
    GlbDocument glb;
    if (!ReadGlb(filePath, &glb, errorText)) {
        return false;
    }
    const int sceneIndex = glb.doc.value("scene", 0);
    const json& scene = glb.doc["scenes"][sceneIndex];
    for (const auto& nodeIndexValue : scene.value("nodes", json::array())) {
        const json& node = glb.doc["nodes"][nodeIndexValue.get<int>()];
        if (!node.contains("mesh")) {
            continue;
        }
        const json& mesh = glb.doc["meshes"][node["mesh"].get<int>()];
        for (const auto& primitive : mesh["primitives"]) {
            if (!primitive.contains("indices")) {
                continue;
            }
            AccessorReader positions;
            AccessorReader indices;
            if (!BuildAccessorReader(glb, primitive["attributes"]["POSITION"].get<int>(), &positions, errorText) ||
                !BuildAccessorReader(glb, primitive["indices"].get<int>(), &indices, errorText)) {
                return false;
            }
            for (std::size_t i = 0; i + 2 < indices.length; i += 3) {
                const auto ia = static_cast<std::size_t>(indices.Get(i)[0]);
                const auto ib = static_cast<std::size_t>(indices.Get(i + 1)[0]);
                const auto ic = static_cast<std::size_t>(indices.Get(i + 2)[0]);
                const auto pa = positions.Get(ia);
                const auto pb = positions.Get(ib);
                const auto pc = positions.Get(ic);
                const std::array<double, 3> aSrc = { pa[0], pa[1], pa[2] };
                const std::array<double, 3> bSrc = { pb[0], pb[1], pb[2] };
                const std::array<double, 3> cSrc = { pc[0], pc[1], pc[2] };
                const std::array<int, 3> a = WorldPoint(ApplyUserTransform(ApplyNodeTransform(aSrc, node), options), manifest);
                const std::array<int, 3> b = WorldPoint(ApplyUserTransform(ApplyNodeTransform(bSrc, node), options), manifest);
                const std::array<int, 3> c = WorldPoint(ApplyUserTransform(ApplyNodeTransform(cSrc, node), options), manifest);
                SampleTriangle(&outSurface->chunkMap, a, b, c);
                ++outSurface->triangles;
            }
        }
    }
    for (const auto& [coord, packed] : outSurface->chunkMap) {
        (void)coord;
        outSurface->blocks += packed.size();
    }
    outSurface->chunks = outSurface->chunkMap.size();
    return true;
}

std::vector<fs::path> CollectSelectedFiles(const GlbSurfaceOptions& options) {
    std::vector<fs::path> files;
    if (!options.onlyFile.empty()) {
        if (options.onlyFile.is_absolute()) {
            files.push_back(options.onlyFile);
        } else {
            files.push_back(options.glbDir / options.onlyFile);
        }
        return files;
    }
    if (!fs::exists(options.glbDir)) {
        return files;
    }
    for (const auto& entry : fs::directory_iterator(options.glbDir)) {
        if (entry.is_regular_file()) {
            const std::string ext = entry.path().extension().string();
            std::string lowerExt = ext;
            std::transform(lowerExt.begin(), lowerExt.end(), lowerExt.begin(), [](char ch) { return static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); });
            if (lowerExt == ".glb") {
                files.push_back(entry.path());
            }
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::vector<fs::path> CollectAllFiles(const fs::path& glbDir) {
    std::vector<fs::path> files;
    if (!fs::exists(glbDir)) {
        return files;
    }
    for (const auto& entry : fs::directory_iterator(glbDir)) {
        if (entry.is_regular_file()) {
            const std::string ext = entry.path().extension().string();
            std::string lowerExt = ext;
            std::transform(lowerExt.begin(), lowerExt.end(), lowerExt.begin(), [](char ch) { return static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); });
            if (lowerExt == ".glb") {
                files.push_back(entry.path());
            }
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::map<ChunkCoord, PackedBlockMap> ConvertSingleBlockMap(const std::map<ChunkCoord, std::set<std::uint32_t>>& source, const std::string& blockName) {
    std::map<ChunkCoord, PackedBlockMap> out;
    for (const auto& [coord, packed] : source) {
        PackedBlockMap items;
        items.reserve(packed.size());
        for (std::uint32_t value : packed) {
            items.push_back({ value, blockName });
        }
        out.emplace(coord, std::move(items));
    }
    return out;
}

}  // namespace

bool ImportGlbSurface(const GlbSurfaceOptions& options, GlbSurfaceImportResult* outResult, std::string* errorText) {
    if (!outResult) {
        if (errorText) *errorText = "Import result output pointer was null";
        return false;
    }
    const std::vector<fs::path> allFiles = CollectAllFiles(options.glbDir);
    if (options.onlyFile.empty() && !options.allFiles) {
        if (errorText) *errorText = "Directory import requires allFiles=true or an explicit onlyFile";
        return false;
    }
    const std::vector<fs::path> selected = CollectSelectedFiles(options);
    if (selected.empty()) {
        if (errorText) *errorText = "No GLB files were selected";
        return false;
    }
    if (allFiles.empty()) {
        if (errorText) *errorText = "No GLB files were found under the GLB directory";
        return false;
    }

    SurfaceManifest manifest;
    if (!ComputeManifest(selected, options, &manifest, errorText)) {
        return false;
    }

    outResult->worldDir = options.worldDir;
    outResult->sizeBlocks = { manifest.sizeBlocks[0], manifest.sizeBlocks[1], manifest.sizeBlocks[2] };
    outResult->globalMin = { manifest.globalMin[0], manifest.globalMin[1], manifest.globalMin[2] };
    outResult->globalMax = { manifest.globalMax[0], manifest.globalMax[1], manifest.globalMax[2] };
    outResult->origin = { manifest.origin[0], manifest.origin[1], manifest.origin[2] };
    outResult->files.clear();

    const fs::path regionDir = ResolveOverworldRegionDir(options.worldDir);
    for (const auto& filePath : selected) {
        BuiltSurface surface;
        const auto started = std::chrono::steady_clock::now();
        if (!BuildSurfaceBlocks(filePath, manifest, options, &surface, errorText)) {
            return false;
        }
        if (options.batchBlockLimit > 0 && surface.blocks > static_cast<std::size_t>(options.batchBlockLimit)) {
            if (errorText) *errorText = "Generated block count exceeds batchBlockLimit";
            return false;
        }

        GlbSurfaceFileResult fileResult;
        fileResult.name = filePath.filename().string();
        fileResult.triangles = surface.triangles;
        fileResult.blocks = surface.blocks;
        fileResult.chunks = surface.chunks;

        std::size_t written = 0;
        std::size_t regions = 0;
        const auto chunkBlocks = ConvertSingleBlockMap(surface.chunkMap, options.blockName);
        if (!WriteManyChunkBlockMap(regionDir, chunkBlocks, false, options.overwrite, &written, &regions, errorText)) {
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
