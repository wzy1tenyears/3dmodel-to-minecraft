#include "obj_textured_importer.h"

#include "world_ops.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <future>
#include <functional>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <tuple>

#include "../third_party/json.hpp"
#include "../third_party/stb_image.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace native_mc {

namespace {

constexpr double kDefaultObjScale = 4.0;
constexpr int kDefaultBaseY = -32;
constexpr double kPi = 3.14159265358979323846;

struct Material {
    std::string name;
    std::array<int, 3> kd = { 128, 128, 128 };
    std::string mapKd;
};

struct Texture {
    fs::path path;
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

struct ComponentInfo {
    int root = 0;
    int vertices = 0;
    std::array<double, 3> min = { INFINITY, INFINITY, INFINITY };
    std::array<double, 3> max = { -INFINITY, -INFINITY, -INFINITY };
};

struct ComponentSummary {
    int components = 0;
    int kept = 0;
    int dropped = 0;
    int largestVertices = 0;
    double minVertices = 0.0;
    double minKeepZ = 0.0;
    int keptVertices = 0;
    int droppedVertices = 0;
};

struct PreparedObj {
    std::vector<double> vertices;
    std::vector<double> uvs;
    std::vector<int> parent;
    std::set<int> keptRoots;
    bool hasKeptRoots = false;
    std::optional<ComponentSummary> componentSummary;
    int faces = 0;
};

struct ClipPlane {
    fs::path file;
    double a = 0.0;
    double b = 0.0;
    double c = 0.0;
    std::array<double, 3> normal = { 0.0, 0.0, 1.0 };
    double belowMeters = 0.0;
};

struct BoundsSummary {
    std::array<double, 3> min = { INFINITY, INFINITY, INFINITY };
    std::array<double, 3> max = { -INFINITY, -INFINITY, -INFINITY };
    std::size_t keptVertices = 0;
    std::size_t clippedVertices = 0;
};

struct LevelCorrection {
    std::array<double, 3> axis = { 1.0, 0.0, 0.0 };
    double angle = 0.0;
};

struct ObjBounds {
    std::array<double, 3> min = { 0.0, 0.0, 0.0 };
    std::array<double, 3> max = { 0.0, 0.0, 0.0 };
    std::array<int, 3> origin = { 0, kDefaultBaseY, 0 };
};

struct VertexRef {
    int vi = -1;
    int ti = -1;
};

struct BuildStats {
    std::map<ChunkCoord, PackedBlockMap> chunkMap;
    std::size_t triangles = 0;
    std::size_t skipped = 0;
    std::size_t clippedTriangles = 0;
    std::size_t clippedSamples = 0;
    std::size_t duplicateSamples = 0;
    std::size_t blocks = 0;
    std::size_t chunks = 0;
    std::set<std::string> texturesUsed;
    std::size_t textureCacheSize = 0;
    std::array<int, 3> min = { 0, 0, 0 };
    std::array<int, 3> max = { 0, 0, 0 };
    bool hasBounds = false;
};

std::string Trim(const std::string& value) {
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

std::vector<std::string> SplitWhitespace(const std::string& value) {
    std::istringstream stream(value);
    std::vector<std::string> parts;
    std::string part;
    while (stream >> part) {
        parts.push_back(part);
    }
    return parts;
}

std::uint32_t PackLocalBlock(int x, int y, int z) {
    return static_cast<std::uint32_t>(((y - kMinY) << 8) | (z << 4) | x);
}

std::vector<fs::path> CollectObjFiles(const fs::path& objDir) {
    std::vector<fs::path> files;
    std::error_code ec;
    if (!fs::exists(objDir, ec)) {
        return files;
    }
    for (const auto& entry : fs::directory_iterator(objDir)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](char ch) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        });
        if (ext == ".obj") {
            files.push_back(entry.path());
        }
    }
    auto naturalNumber = [](const fs::path& pathValue) {
        std::string stem = pathValue.stem().string();
        std::string digits;
        for (char ch : stem) {
            if (std::isdigit(static_cast<unsigned char>(ch))) digits.push_back(ch);
        }
        return digits.empty() ? 0 : std::stoi(digits);
    };
    std::sort(files.begin(), files.end(), [&](const fs::path& a, const fs::path& b) {
        const int an = naturalNumber(a);
        const int bn = naturalNumber(b);
        if (an != bn) return an < bn;
        return a.filename().string() < b.filename().string();
    });
    return files;
}

std::string ReadTextFile(const fs::path& filePath) {
    std::ifstream in(filePath, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string BuildBoundsCacheKey(const std::vector<fs::path>& allFiles, const ObjTexturedOptions& options) {
    std::ostringstream out;
    out << "objDir=" << fs::absolute(options.objDir).string() << "\n";
    out << "transform=" << options.transform << "\n";
    out << "scaleBlocksPerMeter=" << options.scaleBlocksPerMeter << "\n";
    out << "rotateYDeg=" << options.rotateYDeg << "\n";
    if (options.modelRotation.has_value()) {
        out << "modelRotation=" << (*options.modelRotation)[0] << ',' << (*options.modelRotation)[1] << ','
            << (*options.modelRotation)[2] << ',' << (*options.modelRotation)[3] << "\n";
    }
    out << "componentFilter=" << options.componentFilter << "\n";
    out << "componentMinRatio=" << options.componentMinRatio << "\n";
    out << "componentBelowGap=" << options.componentBelowGap << "\n";
    out << "clipReference=" << options.clipReferenceObj.string() << "\n";
    out << "clipBelowMeters=" << options.clipBelowMeters << "\n";
    out << "clipCellSize=" << options.clipCellSize << "\n";
    out << "clipLowFraction=" << options.clipLowFraction << "\n";
    out << "clipPasses=" << options.clipPasses << "\n";
    out << "clipTrimSigma=" << options.clipTrimSigma << "\n";
    if (options.levelNormal.has_value()) {
        out << "levelNormal=" << (*options.levelNormal)[0] << "," << (*options.levelNormal)[1] << "," << (*options.levelNormal)[2] << "\n";
    }
    for (const auto& filePath : allFiles) {
        std::error_code ec;
        const auto size = fs::file_size(filePath, ec);
        const auto time = fs::last_write_time(filePath, ec).time_since_epoch().count();
        out << filePath.filename().string() << "|" << size << "|" << time << "\n";
    }
    return std::to_string(std::hash<std::string>{}(out.str()));
}

fs::path BoundsCachePath(const ObjTexturedOptions& options, const std::vector<fs::path>& allFiles) {
    if (options.cacheRoot.empty()) return {};
    return options.cacheRoot / ("obj-bounds-" + BuildBoundsCacheKey(allFiles, options) + ".json");
}

std::optional<BoundsSummary> LoadBoundsCache(const fs::path& cachePath) {
    std::error_code ec;
    if (cachePath.empty() || !fs::exists(cachePath, ec)) return std::nullopt;
    try {
        const json value = json::parse(ReadTextFile(cachePath));
        BoundsSummary summary;
        summary.min = {
            value["min"][0].get<double>(),
            value["min"][1].get<double>(),
            value["min"][2].get<double>()
        };
        summary.max = {
            value["max"][0].get<double>(),
            value["max"][1].get<double>(),
            value["max"][2].get<double>()
        };
        summary.keptVertices = value.value("keptVertices", 0);
        summary.clippedVertices = value.value("clippedVertices", 0);
        return summary;
    } catch (...) {
        return std::nullopt;
    }
}

void SaveBoundsCache(const fs::path& cachePath, const BoundsSummary& summary) {
    if (cachePath.empty()) return;
    std::error_code ec;
    fs::create_directories(cachePath.parent_path(), ec);
    json value = {
        {"min", { summary.min[0], summary.min[1], summary.min[2] }},
        {"max", { summary.max[0], summary.max[1], summary.max[2] }},
        {"keptVertices", summary.keptVertices},
        {"clippedVertices", summary.clippedVertices}
    };
    std::ofstream out(cachePath, std::ios::binary);
    if (!out) return;
    out << value.dump(2);
}

std::vector<fs::path> CollectSelectedFiles(const ObjTexturedOptions& options) {
    if (!options.onlyFile.empty()) {
        if (options.onlyFile.is_absolute()) return { options.onlyFile };
        return { options.objDir / options.onlyFile };
    }
    return CollectObjFiles(options.objDir);
}

int FindRoot(std::vector<int>& parent, int x) {
    int r = x;
    while (parent[r] != r) r = parent[r];
    while (parent[x] != x) {
        int next = parent[x];
        parent[x] = r;
        x = next;
    }
    return r;
}

void Union(std::vector<int>& parent, std::vector<int>& rank, int a, int b) {
    int ra = FindRoot(parent, a);
    int rb = FindRoot(parent, b);
    if (ra == rb) return;
    if (rank[ra] < rank[rb]) std::swap(ra, rb);
    parent[rb] = ra;
    if (rank[ra] == rank[rb]) rank[ra] += 1;
}

VertexRef ParseVertexRef(const std::string& ref, int vertCount, int uvCount) {
    VertexRef out;
    const auto firstSlash = ref.find('/');
    const auto secondSlash = firstSlash == std::string::npos ? std::string::npos : ref.find('/', firstSlash + 1);
    const std::string viText = firstSlash == std::string::npos ? ref : ref.substr(0, firstSlash);
    const std::string tiText = firstSlash == std::string::npos ? "" : ref.substr(firstSlash + 1, secondSlash == std::string::npos ? std::string::npos : secondSlash - firstSlash - 1);
    const int viRaw = viText.empty() ? 0 : std::stoi(viText);
    const int tiRaw = tiText.empty() ? 0 : std::stoi(tiText);
    out.vi = viRaw < 0 ? vertCount + viRaw : viRaw - 1;
    out.ti = tiRaw ? (tiRaw < 0 ? uvCount + tiRaw : tiRaw - 1) : -1;
    return out;
}

std::array<double, 3> VecCross(const std::array<double, 3>& a, const std::array<double, 3>& b) {
    return {
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0]
    };
}

double VecDot(const std::array<double, 3>& a, const std::array<double, 3>& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

double VecNorm(const std::array<double, 3>& a) {
    return std::sqrt(VecDot(a, a));
}

std::array<double, 3> VecScale(const std::array<double, 3>& a, double s) {
    return { a[0] * s, a[1] * s, a[2] * s };
}

std::array<double, 3> VecNormalize(const std::array<double, 3>& a) {
    const double n = VecNorm(a);
    return n > 0 ? VecScale(a, 1.0 / n) : std::array<double, 3>{ 0.0, 0.0, 0.0 };
}

std::array<double, 3> RotateAroundAxis(const std::array<double, 3>& v, const std::array<double, 3>& axis, double angle) {
    const double cosA = std::cos(angle);
    const double sinA = std::sin(angle);
    const auto cross = VecCross(axis, v);
    const auto along = VecScale(axis, VecDot(axis, v) * (1.0 - cosA));
    return {
        v[0] * cosA + cross[0] * sinA + along[0],
        v[1] * cosA + cross[1] * sinA + along[1],
        v[2] * cosA + cross[2] * sinA + along[2]
    };
}

std::optional<LevelCorrection> BuildLevelCorrection(const std::optional<std::array<double, 3>>& normal) {
    if (!normal.has_value()) return std::nullopt;
    const auto from = VecNormalize(*normal);
    const std::array<double, 3> to = { 0.0, 0.0, 1.0 };
    const auto axisRaw = VecCross(from, to);
    const double axisLen = VecNorm(axisRaw);
    const double cosV = std::clamp(VecDot(from, to), -1.0, 1.0);
    LevelCorrection correction;
    if (axisLen < 1e-12) {
        correction.axis = { 1.0, 0.0, 0.0 };
        correction.angle = cosV > 0 ? 0.0 : kPi;
        return correction;
    }
    correction.axis = VecScale(axisRaw, 1.0 / axisLen);
    correction.angle = std::atan2(axisLen, cosV);
    return correction;
}

std::array<double, 3> ApplyLevel(const std::array<double, 3>& point, const std::optional<LevelCorrection>& correction) {
    if (!correction.has_value()) return point;
    return RotateAroundAxis(point, correction->axis, correction->angle);
}

std::array<double, 3> ApplyWorldYRotation(const std::array<double, 3>& point, double degrees) {
    if (std::abs(degrees) < 1e-12) return point;
    const double angle = degrees * kPi / 180.0;
    const double cosA = std::cos(angle);
    const double sinA = std::sin(angle);
    return {
        point[0] * cosA + point[2] * sinA,
        point[1],
        -point[0] * sinA + point[2] * cosA
    };
}

std::array<double, 3> QuatRotate(const std::array<double, 3>& point, std::array<double, 4> q) {
    const double length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!std::isfinite(length) || length < 1e-12) return point;
    for (double& value : q) value /= length;
    const std::array<double, 3> u = { q[1], q[2], q[3] };
    const double dot = u[0] * point[0] + u[1] * point[1] + u[2] * point[2];
    const double uu = u[0] * u[0] + u[1] * u[1] + u[2] * u[2];
    const std::array<double, 3> cross = {
        u[1] * point[2] - u[2] * point[1],
        u[2] * point[0] - u[0] * point[2],
        u[0] * point[1] - u[1] * point[0]
    };
    return {
        2.0 * dot * u[0] + (q[0] * q[0] - uu) * point[0] + 2.0 * q[0] * cross[0],
        2.0 * dot * u[1] + (q[0] * q[0] - uu) * point[1] + 2.0 * q[0] * cross[1],
        2.0 * dot * u[2] + (q[0] * q[0] - uu) * point[2] + 2.0 * q[0] * cross[2]
    };
}

std::array<double, 3> ApplyObjUserTransform(std::array<double, 3> worldPoint, const ObjTexturedOptions& options) {
    if (options.flipX) worldPoint[0] = -worldPoint[0];
    if (options.flipZ) worldPoint[2] = -worldPoint[2];
    if (options.modelRotation.has_value()) return QuatRotate(worldPoint, *options.modelRotation);
    return ApplyWorldYRotation(worldPoint, options.rotateYDeg);
}

std::array<double, 3> TransformObjPoint(const std::array<double, 3>& point, const ObjTexturedOptions& options, const std::optional<LevelCorrection>& correction) {
    const auto leveled = ApplyLevel(point, correction);
    return ApplyObjUserTransform({ leveled[0], leveled[2], -leveled[1] }, options);
}

std::array<double, 3> TransformDefaultObjPoint(const std::array<double, 3>& point, const ObjTexturedOptions& options) {
    return ApplyObjUserTransform(point, options);
}

void Extend(std::array<double, 3>* min, std::array<double, 3>* max, const std::array<double, 3>& point) {
    for (int i = 0; i < 3; ++i) {
        (*min)[i] = std::min((*min)[i], point[i]);
        (*max)[i] = std::max((*max)[i], point[i]);
    }
}

void ExtendInt(std::array<int, 3>* min, std::array<int, 3>* max, const std::array<int, 3>& point) {
    for (int i = 0; i < 3; ++i) {
        (*min)[i] = std::min((*min)[i], point[i]);
        (*max)[i] = std::max((*max)[i], point[i]);
    }
}

bool BelowClipPlane(const std::array<double, 3>& point, const std::optional<ClipPlane>& clipPlane) {
    if (!clipPlane.has_value()) return false;
    return point[2] < (clipPlane->a * point[0] + clipPlane->b * point[1] + clipPlane->c - clipPlane->belowMeters);
}

std::vector<std::array<double, 3>> ReadObjVertices(const fs::path& filePath, std::string* errorText) {
    std::ifstream in(filePath);
    if (!in) {
        if (errorText) *errorText = "Could not open clip reference OBJ";
        return {};
    }
    std::vector<std::array<double, 3>> vertices;
    std::string rawLine;
    while (std::getline(in, rawLine)) {
        if (rawLine.rfind("v ", 0) != 0) continue;
        const auto parts = SplitWhitespace(rawLine);
        if (parts.size() < 4) continue;
        vertices.push_back({ std::stod(parts[1]), std::stod(parts[2]), std::stod(parts[3]) });
    }
    return vertices;
}

struct GridSelection {
    std::size_t buckets = 0;
    std::vector<std::array<double, 3>> ground;
};

GridSelection SelectGroundByGrid(const std::vector<std::array<double, 3>>& vertices, const ObjTexturedOptions& options) {
    GridSelection out;
    if (vertices.empty()) return out;
    std::array<double, 3> min = vertices.front();
    for (const auto& point : vertices) {
        for (int i = 0; i < 3; ++i) min[i] = std::min(min[i], point[i]);
    }
    std::map<std::pair<int, int>, std::vector<std::array<double, 3>>> buckets;
    for (const auto& point : vertices) {
        const int gx = static_cast<int>(std::floor((point[0] - min[0]) / options.clipCellSize));
        const int gy = static_cast<int>(std::floor((point[1] - min[1]) / options.clipCellSize));
        buckets[{ gx, gy }].push_back(point);
    }
    out.buckets = buckets.size();
    for (auto& [key, bucket] : buckets) {
        (void)key;
        std::sort(bucket.begin(), bucket.end(), [](const auto& a, const auto& b) { return a[2] < b[2]; });
        const std::size_t keepCount = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(bucket.size() * options.clipLowFraction)));
        out.ground.insert(out.ground.end(), bucket.begin(), bucket.begin() + static_cast<std::ptrdiff_t>(std::min(bucket.size(), keepCount)));
    }
    return out;
}

std::array<double, 3> Solve3(std::array<std::array<double, 3>, 3> a, std::array<double, 3> b) {
    std::array<std::array<double, 4>, 3> m{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) m[r][c] = a[r][c];
        m[r][3] = b[r];
    }
    for (int col = 0; col < 3; ++col) {
        int pivot = col;
        for (int r = col + 1; r < 3; ++r) {
            if (std::abs(m[r][col]) > std::abs(m[pivot][col])) pivot = r;
        }
        if (std::abs(m[pivot][col]) < 1e-12) {
            throw std::runtime_error("Clip plane fit matrix is singular");
        }
        std::swap(m[col], m[pivot]);
        const double div = m[col][col];
        for (int c = col; c < 4; ++c) m[col][c] /= div;
        for (int r = 0; r < 3; ++r) {
            if (r == col) continue;
            const double factor = m[r][col];
            for (int c = col; c < 4; ++c) m[r][c] -= factor * m[col][c];
        }
    }
    return { m[0][3], m[1][3], m[2][3] };
}

ClipPlane FitPlane(const std::vector<std::array<double, 3>>& points) {
    std::array<std::array<double, 3>, 3> ata{};
    std::array<double, 3> atb{};
    for (const auto& point : points) {
        const std::array<double, 3> row = { point[0], point[1], 1.0 };
        for (int r = 0; r < 3; ++r) {
            atb[r] += row[r] * point[2];
            for (int c = 0; c < 3; ++c) ata[r][c] += row[r] * row[c];
        }
    }
    const auto coeffs = Solve3(ata, atb);
    ClipPlane plane;
    plane.a = coeffs[0];
    plane.b = coeffs[1];
    plane.c = coeffs[2];
    plane.normal = VecNormalize({ -plane.a, -plane.b, 1.0 });
    return plane;
}

double QuantileSorted(const std::vector<double>& values, double q) {
    if (values.empty()) return 0.0;
    const std::size_t index = static_cast<std::size_t>(std::clamp(q, 0.0, 1.0) * (values.size() - 1));
    return values[index];
}

std::optional<ClipPlane> LoadClipPlane(const ObjTexturedOptions& options, std::string* errorText) {
    if (options.clipReferenceObj.empty()) return std::nullopt;
    fs::path filePath = options.clipReferenceObj;
    if (!filePath.is_absolute()) filePath = options.objDir / filePath;
    if (!fs::exists(filePath)) {
        if (errorText) *errorText = "Clip reference OBJ not found";
        return std::nullopt;
    }
    auto vertices = ReadObjVertices(filePath, errorText);
    if (vertices.size() < 3) {
        if (errorText) *errorText = "Clip reference OBJ has too few vertices";
        return std::nullopt;
    }
    auto selected = SelectGroundByGrid(vertices, options);
    auto points = selected.ground;
    ClipPlane fit;
    for (int pass = 0; pass < options.clipPasses; ++pass) {
        fit = FitPlane(points);
        std::vector<double> residuals;
        residuals.reserve(points.size());
        for (const auto& point : points) {
            residuals.push_back(std::abs(point[2] - (fit.a * point[0] + fit.b * point[1] + fit.c)));
        }
        std::sort(residuals.begin(), residuals.end());
        const double med = QuantileSorted(residuals, 0.5);
        const double cutoff = std::max(0.02, med * options.clipTrimSigma);
        std::vector<std::array<double, 3>> filtered;
        for (const auto& point : points) {
            if (std::abs(point[2] - (fit.a * point[0] + fit.b * point[1] + fit.c)) <= cutoff) {
                filtered.push_back(point);
            }
        }
        points = std::move(filtered);
    }
    fit = FitPlane(points);
    fit.file = filePath;
    fit.belowMeters = options.clipBelowMeters;
    return fit;
}

PreparedObj PrepareObj(const fs::path& filePath, const ObjTexturedOptions& options, std::string* errorText) {
    PreparedObj prepared;
    std::ifstream in(filePath);
    if (!in) {
        if (errorText) *errorText = "Could not open OBJ file";
        return prepared;
    }
    std::vector<int> rank;
    std::string rawLine;
    while (std::getline(in, rawLine)) {
        const std::string line = Trim(rawLine);
        if (line.rfind("v ", 0) == 0) {
            const auto parts = SplitWhitespace(line);
            if (parts.size() < 4) continue;
            prepared.vertices.push_back(std::stod(parts[1]));
            prepared.vertices.push_back(std::stod(parts[2]));
            prepared.vertices.push_back(std::stod(parts[3]));
            prepared.parent.push_back(static_cast<int>(prepared.parent.size()));
            rank.push_back(0);
        } else if (line.rfind("vt ", 0) == 0) {
            const auto parts = SplitWhitespace(line);
            if (parts.size() < 3) continue;
            prepared.uvs.push_back(std::stod(parts[1]));
            prepared.uvs.push_back(std::stod(parts[2]));
        } else if (line.rfind("f ", 0) == 0) {
            prepared.faces += 1;
            if (!options.componentFilter) continue;
            const auto refsText = SplitWhitespace(line.substr(2));
            if (refsText.size() < 3) continue;
            std::vector<int> refs;
            refs.reserve(refsText.size());
            for (const auto& refText : refsText) {
                refs.push_back(ParseVertexRef(refText, static_cast<int>(prepared.vertices.size() / 3), static_cast<int>(prepared.uvs.size() / 2)).vi);
            }
            for (std::size_t i = 1; i < refs.size(); ++i) {
                if (refs[0] >= 0 && refs[i] >= 0) Union(prepared.parent, rank, refs[0], refs[i]);
            }
        }
    }
    if (!options.componentFilter || prepared.parent.empty()) {
        return prepared;
    }
    std::map<int, ComponentInfo> components;
    for (int i = 0; i < static_cast<int>(prepared.vertices.size() / 3); ++i) {
        const int root = FindRoot(prepared.parent, i);
        auto& component = components[root];
        if (component.vertices == 0) {
            component.root = root;
        }
        component.vertices += 1;
        const std::array<double, 3> point = {
            prepared.vertices[i * 3],
            prepared.vertices[i * 3 + 1],
            prepared.vertices[i * 3 + 2]
        };
        Extend(&component.min, &component.max, point);
    }
    if (components.empty()) return prepared;
    std::vector<ComponentInfo> sorted;
    sorted.reserve(components.size());
    for (const auto& [root, component] : components) {
        (void)root;
        sorted.push_back(component);
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.vertices > b.vertices; });
    const auto& largest = sorted.front();
    const double minVertices = largest.vertices * options.componentMinRatio;
    const double minKeepZ = largest.min[2] - options.componentBelowGap;
    prepared.hasKeptRoots = true;
    ComponentSummary summary;
    summary.components = static_cast<int>(sorted.size());
    summary.largestVertices = largest.vertices;
    summary.minVertices = minVertices;
    summary.minKeepZ = minKeepZ;
    for (const auto& component : sorted) {
        const bool keep = component.vertices >= minVertices || component.max[2] >= minKeepZ;
        if (keep) {
            prepared.keptRoots.insert(component.root);
            summary.kept += 1;
            summary.keptVertices += component.vertices;
        } else {
            summary.droppedVertices += component.vertices;
        }
    }
    summary.dropped = summary.components - summary.kept;
    prepared.componentSummary = summary;
    return prepared;
}

std::optional<BoundsSummary> ScanRawBounds(const std::vector<fs::path>& files, const ObjTexturedOptions& options, const std::optional<ClipPlane>& clipPlane, std::string* errorText) {
    BoundsSummary summary;
    bool found = false;
    for (const auto& filePath : files) {
        PreparedObj prepared = PrepareObj(filePath, options, errorText);
        if (!errorText->empty()) return std::nullopt;
        for (int i = 0; i < static_cast<int>(prepared.vertices.size() / 3); ++i) {
            if (prepared.hasKeptRoots) {
                const int root = FindRoot(prepared.parent, i);
                if (prepared.keptRoots.find(root) == prepared.keptRoots.end()) continue;
            }
            const std::array<double, 3> point = {
                prepared.vertices[i * 3],
                prepared.vertices[i * 3 + 1],
                prepared.vertices[i * 3 + 2]
            };
            if (BelowClipPlane(point, clipPlane)) {
                summary.clippedVertices += 1;
                continue;
            }
            Extend(&summary.min, &summary.max, point);
            summary.keptVertices += 1;
            found = true;
        }
    }
    if (!found) {
        if (errorText) *errorText = "No OBJ vertices remained after filtering";
        return std::nullopt;
    }
    return summary;
}

ObjBounds PrepareBounds(const BoundsSummary& rawBounds, const ObjTexturedOptions& options, const std::optional<LevelCorrection>& correction) {
    ObjBounds bounds;
    const bool objZUp = options.transform == "obj-z-up";
    std::array<double, 3> min = { INFINITY, INFINITY, INFINITY };
    std::array<double, 3> max = { -INFINITY, -INFINITY, -INFINITY };
    for (double x : { rawBounds.min[0], rawBounds.max[0] }) {
        for (double y : { rawBounds.min[1], rawBounds.max[1] }) {
            for (double z : { rawBounds.min[2], rawBounds.max[2] }) {
                const std::array<double, 3> point = { x, y, z };
                Extend(&min, &max, objZUp
                    ? TransformObjPoint(point, options, correction)
                    : TransformDefaultObjPoint(point, options));
            }
        }
    }
    bounds.min = min;
    bounds.max = max;
    const double scale = options.scaleBlocksPerMeter > 0.0 ? options.scaleBlocksPerMeter : kDefaultObjScale;
    const double widthX = (max[0] - min[0]) * scale;
    const double heightY = (max[1] - min[1]) * scale;
    const double widthZ = (max[2] - min[2]) * scale;
    bounds.origin[0] = options.center.x.has_value() ? static_cast<int>(std::llround(*options.center.x - widthX / 2.0)) : 0;
    bounds.origin[1] = options.center.y.has_value() ? static_cast<int>(std::llround(*options.center.y - heightY / 2.0)) : options.baseY;
    bounds.origin[2] = options.center.z.has_value() ? static_cast<int>(std::llround(*options.center.z - widthZ / 2.0)) : 0;
    return bounds;
}

std::array<int, 3> WorldPoint(const std::array<double, 3>& point, const ObjBounds& bounds, const ObjTexturedOptions& options, const std::optional<LevelCorrection>& correction) {
    const double scale = options.scaleBlocksPerMeter > 0.0 ? options.scaleBlocksPerMeter : kDefaultObjScale;
    const auto transformed = options.transform == "obj-z-up"
        ? TransformObjPoint(point, options, correction)
        : TransformDefaultObjPoint(point, options);
    return {
        static_cast<int>(std::llround((transformed[0] - bounds.min[0]) * scale + bounds.origin[0])),
        static_cast<int>(std::llround((transformed[1] - bounds.min[1]) * scale + bounds.origin[1])),
        static_cast<int>(std::llround((transformed[2] - bounds.min[2]) * scale + bounds.origin[2]))
    };
}

std::map<std::string, Material> ParseMtl(const fs::path& mtlPath) {
    std::map<std::string, Material> materials;
    std::ifstream in(mtlPath);
    if (!in) return materials;
    std::string rawLine;
    Material* current = nullptr;
    while (std::getline(in, rawLine)) {
        const std::string line = Trim(rawLine);
        if (line.empty() || line[0] == '#') continue;
        const auto parts = SplitWhitespace(line);
        if (parts.empty()) continue;
        if (parts[0] == "newmtl") {
            Material material;
            material.name = Trim(line.substr(7));
            current = &materials[material.name];
            *current = material;
        } else if (current && parts[0] == "Kd" && parts.size() >= 4) {
            current->kd = {
                std::clamp(static_cast<int>(std::lround(std::stod(parts[1]) * 255.0)), 0, 255),
                std::clamp(static_cast<int>(std::lround(std::stod(parts[2]) * 255.0)), 0, 255),
                std::clamp(static_cast<int>(std::lround(std::stod(parts[3]) * 255.0)), 0, 255)
            };
        } else if (current && parts[0] == "map_Kd") {
            current->mapKd = Trim(line.substr(7));
        }
    }
    return materials;
}

Texture* GetTexture(const Material* material, const fs::path& objDir, std::map<fs::path, Texture>* textureCache, std::string* errorText) {
    if (!material || material->mapKd.empty()) return nullptr;
    fs::path texturePath = objDir / fs::u8path(material->mapKd);
    texturePath = fs::absolute(texturePath);
    auto it = textureCache->find(texturePath);
    if (it != textureCache->end()) return &it->second;
    int width = 0;
    int height = 0;
    int channels = 0;
    std::string pathString = texturePath.string();
    unsigned char* rgba = stbi_load(pathString.c_str(), &width, &height, &channels, 4);
    if (!rgba) {
        if (errorText && errorText->empty()) *errorText = "Failed to decode OBJ texture image";
        return nullptr;
    }
    Texture tex;
    tex.path = texturePath;
    tex.width = width;
    tex.height = height;
    tex.rgba.assign(rgba, rgba + (static_cast<std::size_t>(width) * height * 4));
    stbi_image_free(rgba);
    auto [inserted, _] = textureCache->emplace(texturePath, std::move(tex));
    return &inserted->second;
}

std::optional<std::array<int, 3>> TextureColour(const Texture* texture, double u, double v) {
    if (!texture || texture->width <= 0 || texture->height <= 0) return std::nullopt;
    const double uu = u - std::floor(u);
    const double vv = v - std::floor(v);
    const int x = std::clamp(static_cast<int>(std::floor(uu * texture->width)), 0, texture->width - 1);
    const int y = std::clamp(static_cast<int>(std::floor((1.0 - vv) * texture->height)), 0, texture->height - 1);
    const std::size_t offset = (static_cast<std::size_t>(y) * texture->width + x) * 4;
    return std::array<int, 3>{
        texture->rgba[offset],
        texture->rgba[offset + 1],
        texture->rgba[offset + 2]
    };
}

std::string MaterialBlock(const Material* material, const Palette& palette) {
    const auto kd = material ? material->kd : std::array<int, 3>{ 128, 128, 128 };
    return palette.Nearest(kd[0], kd[1], kd[2]);
}

double Dist(const std::array<int, 3>& a, const std::array<int, 3>& b) {
    const double dx = static_cast<double>(a[0] - b[0]);
    const double dy = static_cast<double>(a[1] - b[1]);
    const double dz = static_cast<double>(a[2] - b[2]);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool AddWorldBlock(std::map<ChunkCoord, PackedBlockMap>* chunkMap, int x, int y, int z, const std::string& blockName) {
    if (y < kMinY || y >= kMinY + kHeight) return false;
    const int cx = static_cast<int>(std::floor(static_cast<double>(x) / 16.0));
    const int cz = static_cast<int>(std::floor(static_cast<double>(z) / 16.0));
    const int lx = ((x % 16) + 16) % 16;
    const int lz = ((z % 16) + 16) % 16;
    (*chunkMap)[ChunkCoord{ cx, cz }].push_back({ PackLocalBlock(lx, y, lz), blockName });
    return true;
}

void SampleTriangle(
    std::map<ChunkCoord, PackedBlockMap>* chunkMap,
    const std::array<int, 3>& a,
    const std::array<int, 3>& b,
    const std::array<int, 3>& c,
    const std::optional<std::array<double, 2>>& auv,
    const std::optional<std::array<double, 2>>& buv,
    const std::optional<std::array<double, 2>>& cuv,
    const Material* material,
    const Texture* texture,
    const Palette& palette,
    BuildStats* stats,
    const ObjTexturedOptions& options,
    const std::optional<std::array<double, 3>>& sa,
    const std::optional<std::array<double, 3>>& sb,
    const std::optional<std::array<double, 3>>& sc,
    const std::optional<ClipPlane>& clipPlane) {
    const int steps = std::max(1, static_cast<int>(std::ceil(std::max({ Dist(a, b), Dist(b, c), Dist(c, a) }))));
    const std::string fallbackBlock = options.textured
        ? MaterialBlock(material, palette)
        : options.surfaceBlockName;
    std::set<std::tuple<int, int, int>> seen;
    const bool useSeen = steps <= options.dedupeSampleSteps;
    std::map<ChunkCoord, std::map<std::uint32_t, std::string>> perChunk;
    for (int i = 0; i <= steps; ++i) {
        for (int j = 0; j <= steps - i; ++j) {
            const double u = static_cast<double>(i) / steps;
            const double v = static_cast<double>(j) / steps;
            const double w = 1.0 - u - v;
            if (sa && sb && sc && clipPlane.has_value()) {
                const std::array<double, 3> sourcePoint = {
                    (*sa)[0] * w + (*sb)[0] * u + (*sc)[0] * v,
                    (*sa)[1] * w + (*sb)[1] * u + (*sc)[1] * v,
                    (*sa)[2] * w + (*sb)[2] * u + (*sc)[2] * v
                };
                if (BelowClipPlane(sourcePoint, clipPlane)) {
                    stats->clippedSamples += 1;
                    continue;
                }
            }
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
            if (options.textured && texture && auv && buv && cuv) {
                const double tu = (*auv)[0] * w + (*buv)[0] * u + (*cuv)[0] * v;
                const double tv = (*auv)[1] * w + (*buv)[1] * u + (*cuv)[1] * v;
                auto colour = TextureColour(texture, tu, tv);
                if (colour.has_value()) {
                    block = palette.Nearest((*colour)[0], (*colour)[1], (*colour)[2]);
                }
            }
            if (y < kMinY || y >= kMinY + kHeight) continue;
            const int cx = static_cast<int>(std::floor(static_cast<double>(x) / 16.0));
            const int cz = static_cast<int>(std::floor(static_cast<double>(z) / 16.0));
            const int lx = ((x % 16) + 16) % 16;
            const int lz = ((z % 16) + 16) % 16;
            perChunk[ChunkCoord{ cx, cz }][PackLocalBlock(lx, y, lz)] = block;
        }
    }
    for (auto& [coord, localMap] : perChunk) {
        auto& target = (*chunkMap)[coord];
        for (const auto& [packed, block] : localMap) {
            target.push_back({ packed, block });
        }
    }
}

bool BuildBlocksForObj(
    const fs::path& filePath,
    const ObjBounds& bounds,
    const Palette& palette,
    const ObjTexturedOptions& options,
    const std::optional<LevelCorrection>& correction,
    const std::optional<ClipPlane>& clipPlane,
    BuildStats* outStats,
    std::string* errorText) {
    if (!outStats) {
        if (errorText) *errorText = "BuildStats output pointer was null";
        return false;
    }
    const fs::path objDir = filePath.parent_path();
    const fs::path mtlPath = objDir / (filePath.stem().wstring() + std::wstring(L".mtl"));
    const auto materials = options.textured ? ParseMtl(mtlPath) : std::map<std::string, Material>{};
    std::map<fs::path, Texture> textureCache;
    PreparedObj prepared = PrepareObj(filePath, options, errorText);
    if (errorText && !errorText->empty()) return false;
    std::ifstream in(filePath);
    if (!in) {
        if (errorText) *errorText = "Could not open OBJ file for block generation";
        return false;
    }
    std::string rawLine;
    std::string currentMaterialName;
    while (std::getline(in, rawLine)) {
        const std::string line = Trim(rawLine);
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("usemtl ", 0) == 0) {
            currentMaterialName = Trim(line.substr(7));
            continue;
        }
        if (line.rfind("f ", 0) != 0) continue;
        const auto refsText = SplitWhitespace(line.substr(2));
        if (refsText.size() < 3) continue;
        std::vector<VertexRef> refs;
        refs.reserve(refsText.size());
        for (const auto& refText : refsText) {
            refs.push_back(ParseVertexRef(refText, static_cast<int>(prepared.vertices.size() / 3), static_cast<int>(prepared.uvs.size() / 2)));
        }
        if (prepared.hasKeptRoots) {
            const int root = FindRoot(prepared.parent, refs[0].vi);
            if (prepared.keptRoots.find(root) == prepared.keptRoots.end()) continue;
        }
        const Material* material = nullptr;
        auto materialIt = materials.find(currentMaterialName);
        if (materialIt != materials.end()) material = &materialIt->second;
        Texture* texture = options.textured ? GetTexture(material, objDir, &textureCache, errorText) : nullptr;
        if (texture && !texture->path.empty()) outStats->texturesUsed.insert(texture->path.string());
        for (std::size_t i = 1; i + 1 < refs.size(); ++i) {
            const std::array<VertexRef, 3> triRefs = { refs[0], refs[i], refs[i + 1] };
            bool invalid = false;
            std::array<std::array<int, 3>, 3> points{};
            std::array<std::array<double, 3>, 3> sourcePoints{};
            std::array<std::optional<std::array<double, 2>>, 3> triUvs;
            for (int k = 0; k < 3; ++k) {
                const auto& ref = triRefs[k];
                if (ref.vi < 0 || ref.vi * 3 + 2 >= static_cast<int>(prepared.vertices.size())) {
                    invalid = true;
                    break;
                }
                const std::array<double, 3> source = {
                    prepared.vertices[ref.vi * 3],
                    prepared.vertices[ref.vi * 3 + 1],
                    prepared.vertices[ref.vi * 3 + 2]
                };
                sourcePoints[k] = source;
                points[k] = WorldPoint(source, bounds, options, correction);
                if (ref.ti >= 0 && ref.ti * 2 + 1 < static_cast<int>(prepared.uvs.size())) {
                    triUvs[k] = std::array<double, 2>{
                        prepared.uvs[ref.ti * 2],
                        prepared.uvs[ref.ti * 2 + 1]
                    };
                }
            }
            if (invalid) {
                outStats->skipped += 1;
                continue;
            }
            if (clipPlane.has_value() &&
                BelowClipPlane(sourcePoints[0], clipPlane) &&
                BelowClipPlane(sourcePoints[1], clipPlane) &&
                BelowClipPlane(sourcePoints[2], clipPlane)) {
                outStats->clippedTriangles += 1;
                continue;
            }
            if (!outStats->hasBounds) {
                outStats->min = points[0];
                outStats->max = points[0];
                outStats->hasBounds = true;
            }
            for (const auto& point : points) ExtendInt(&outStats->min, &outStats->max, point);
            SampleTriangle(
                &outStats->chunkMap,
                points[0], points[1], points[2],
                triUvs[0], triUvs[1], triUvs[2],
                material, texture, palette, outStats, options,
                sourcePoints[0], sourcePoints[1], sourcePoints[2], clipPlane);
            outStats->triangles += 1;
        }
    }
    outStats->textureCacheSize = textureCache.size();
    for (auto& [coord, blocks] : outStats->chunkMap) {
        std::map<std::uint32_t, std::string> deduped;
        for (const auto& [packed, block] : blocks) deduped[packed] = block;
        PackedBlockMap collapsed;
        collapsed.reserve(deduped.size());
        for (const auto& [packed, block] : deduped) collapsed.push_back({ packed, block });
        blocks = std::move(collapsed);
        outStats->blocks += blocks.size();
    }
    outStats->chunks = outStats->chunkMap.size();
    return true;
}

}  // namespace

bool EstimateObjLevelNormal(const fs::path& filePath, double cellSize, double lowFraction,
                            int passes, double trimSigma, std::array<double, 3>* normal,
                            std::string* errorText) {
    if (!normal) {
        if (errorText) *errorText = "Level normal output pointer was null";
        return false;
    }
    std::string extension = filePath.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (!fs::exists(filePath) || extension != ".obj") {
        if (errorText) *errorText = "The leveling reference must be an existing OBJ file";
        return false;
    }
    ObjTexturedOptions options;
    options.clipCellSize = std::max(0.001, cellSize);
    options.clipLowFraction = std::clamp(lowFraction, 0.001, 1.0);
    options.clipPasses = std::max(1, passes);
    options.clipTrimSigma = std::max(0.01, trimSigma);
    auto vertices = ReadObjVertices(filePath, errorText);
    if (vertices.size() < 3) {
        if (errorText && errorText->empty()) *errorText = "The leveling reference has too few vertices";
        return false;
    }
    auto points = SelectGroundByGrid(vertices, options).ground;
    if (points.size() < 3) {
        if (errorText) *errorText = "Could not select enough ground points from the reference model";
        return false;
    }
    try {
        ClipPlane fit;
        for (int pass = 0; pass < options.clipPasses; ++pass) {
            fit = FitPlane(points);
            std::vector<double> residuals;
            residuals.reserve(points.size());
            for (const auto& point : points) {
                residuals.push_back(std::abs(point[2] - (fit.a * point[0] + fit.b * point[1] + fit.c)));
            }
            std::sort(residuals.begin(), residuals.end());
            const double cutoff = std::max(0.02, QuantileSorted(residuals, 0.5) * options.clipTrimSigma);
            std::vector<std::array<double, 3>> filtered;
            filtered.reserve(points.size());
            for (const auto& point : points) {
                if (std::abs(point[2] - (fit.a * point[0] + fit.b * point[1] + fit.c)) <= cutoff) {
                    filtered.push_back(point);
                }
            }
            if (filtered.size() < 3) break;
            points = std::move(filtered);
        }
        *normal = FitPlane(points).normal;
        return true;
    } catch (const std::exception& ex) {
        if (errorText) *errorText = ex.what();
        return false;
    }
}

bool ImportObjTextured(const ObjTexturedOptions& options, ObjTexturedImportResult* outResult, std::string* errorText) {
    if (!outResult) {
        if (errorText) *errorText = "Import result output pointer was null";
        return false;
    }
    if (!options.textured && options.surfaceBlockName.empty()) {
        if (errorText) *errorText = "OBJ surface block name cannot be empty";
        return false;
    }
    auto allFiles = CollectObjFiles(options.objDir);
    if (allFiles.empty()) {
        if (errorText) *errorText = "No OBJ files were found under the OBJ directory";
        return false;
    }
    if (options.onlyFile.empty() && !options.allFiles) {
        if (errorText) *errorText = "Directory import requires allFiles=true or an explicit onlyFile";
        return false;
    }
    auto selectedFiles = CollectSelectedFiles(options);
    if (selectedFiles.empty()) {
        if (errorText) *errorText = "No OBJ files were selected";
        return false;
    }
    Palette palette;
    if (options.textured && !LoadPalette(options.projectRoot, options.palette, &palette, errorText)) {
        return false;
    }
    auto correction = BuildLevelCorrection(options.levelNormal);
    auto clipPlane = LoadClipPlane(options, errorText);
    if (errorText && !errorText->empty()) return false;
    const fs::path cachePath = BoundsCachePath(options, selectedFiles);
    auto rawBounds = LoadBoundsCache(cachePath);
    if (!rawBounds.has_value()) {
        rawBounds = ScanRawBounds(selectedFiles, options, clipPlane, errorText);
        if (rawBounds.has_value()) SaveBoundsCache(cachePath, *rawBounds);
    }
    if (!rawBounds.has_value()) return false;
    const ObjBounds bounds = PrepareBounds(*rawBounds, options, correction);

    outResult->worldDir = options.worldDir;
    const double scale = options.scaleBlocksPerMeter > 0.0 ? options.scaleBlocksPerMeter : kDefaultObjScale;
    auto ceilExtent = [](double value) {
        const double epsilon = std::max(1e-9, std::abs(value) * 1e-12);
        return static_cast<int>(std::ceil(std::max(0.0, value) - epsilon));
    };
    outResult->sizeBlocks = {
        ceilExtent((bounds.max[0] - bounds.min[0]) * scale),
        ceilExtent((bounds.max[1] - bounds.min[1]) * scale),
        ceilExtent((bounds.max[2] - bounds.min[2]) * scale)
    };
    outResult->globalMin = { bounds.min[0], bounds.min[1], bounds.min[2] };
    outResult->globalMax = { bounds.max[0], bounds.max[1], bounds.max[2] };
    outResult->origin = { bounds.origin[0], bounds.origin[1], bounds.origin[2] };
    outResult->paletteSize = options.textured ? palette.blocks().size() : 1;
    outResult->files.clear();

    const fs::path regionDir = ResolveOverworldRegionDir(options.worldDir);
    const std::size_t workerCount = static_cast<std::size_t>(std::max(1, options.workers));
    struct PendingBuild {
        fs::path filePath;
        BuildStats stats;
        std::string error;
        bool success = false;
        std::chrono::steady_clock::time_point started;
    };
    for (std::size_t batchStart = 0; batchStart < selectedFiles.size(); batchStart += workerCount) {
        const std::size_t batchEnd = std::min(selectedFiles.size(), batchStart + workerCount);
        std::vector<std::future<PendingBuild>> pending;
        pending.reserve(batchEnd - batchStart);
        for (std::size_t index = batchStart; index < batchEnd; ++index) {
            const fs::path filePath = selectedFiles[index];
            pending.push_back(std::async(std::launch::async, [&, filePath]() {
                PendingBuild built;
                built.filePath = filePath;
                built.started = std::chrono::steady_clock::now();
                built.success = BuildBlocksForObj(filePath, bounds, palette, options, correction, clipPlane,
                                                  &built.stats, &built.error);
                return built;
            }));
        }
        for (auto& future : pending) {
            PendingBuild built = future.get();
            if (!built.success) {
                if (errorText) *errorText = built.error;
                return false;
            }
            const fs::path& filePath = built.filePath;
            BuildStats& stats = built.stats;
        if (options.batchBlockLimit > 0 && stats.blocks > static_cast<std::size_t>(options.batchBlockLimit)) {
            if (errorText) *errorText = "Generated block count exceeds batchBlockLimit";
            return false;
        }
        ObjTexturedFileResult fileResult;
        fileResult.name = filePath.filename().string();
        fileResult.triangles = stats.triangles;
        fileResult.skipped = stats.skipped;
        fileResult.clippedTriangles = stats.clippedTriangles;
        fileResult.clippedSamples = stats.clippedSamples;
        fileResult.duplicateSamples = stats.duplicateSamples;
        fileResult.blocks = stats.blocks;
        fileResult.chunks = stats.chunks;
        fileResult.textures = stats.texturesUsed.size();
        fileResult.textureCacheSize = stats.textureCacheSize;
        fileResult.hasBounds = stats.hasBounds;
        fileResult.min = stats.min;
        fileResult.max = stats.max;
        std::size_t written = 0;
        std::size_t regions = 0;
        if (!WriteManyChunkBlockMap(regionDir, stats.chunkMap, true, options.overwrite, &written, &regions, errorText)) {
            return false;
        }
        fileResult.written = written;
        fileResult.regions = regions;
        fileResult.milliseconds = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - built.started).count());
        outResult->files.push_back(std::move(fileResult));
        }
    }
    return true;
}

}  // namespace native_mc
