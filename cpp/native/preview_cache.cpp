#include "preview_cache.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>
#include <type_traits>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace native_mc {
namespace {

constexpr std::array<std::uint8_t, 8> kCacheMagic = { 'P', 'M', 'E', 'S', 'H', 'C', '0', '1' };
constexpr std::size_t kCacheHeaderSize = 184;
constexpr std::uint64_t kMaxCachePayloadBytes = 512ull * 1024ull * 1024ull;
constexpr std::uint64_t kPayloadSourceRatio = 32;
constexpr std::uint64_t kPayloadSourceSlack = 64ull * 1024ull * 1024ull;
constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

struct FileStamp {
    std::uint64_t pathHash = 0;
    std::uint64_t size = 0;
    std::int64_t writeTime = 0;
    std::uint64_t fingerprint = 0;
    bool exists = false;
};

struct CacheHeader {
    std::uint64_t sourcePathHash = 0;
    std::uint64_t sourceSize = 0;
    std::int64_t sourceWriteTime = 0;
    std::uint64_t sourceFingerprint = 0;
    std::uint64_t dependencyFingerprint = 0;
    std::uint64_t dependencyCount = 0;
    std::uint64_t cacheProfileHash = 0;
    std::uint64_t positionCount = 0;
    std::uint64_t normalCount = 0;
    std::uint64_t colorCount = 0;
    std::uint64_t indexCount = 0;
    std::array<std::uint64_t, 4> lodIndexCounts{};
    std::array<float, 3> min{};
    std::array<float, 3> max{};
    std::uint64_t payloadBytes = 0;
    std::uint64_t payloadHash = 0;
    std::uint64_t headerHash = 0;
};

void SetError(std::string* errorText, const std::string& value) {
    if (errorText) *errorText = value;
}

std::string ErrorMessage(const char* action, const std::error_code& ec) {
    std::ostringstream out;
    out << action;
    if (ec) out << ": " << ec.message();
    return out.str();
}

bool IsLittleEndian() {
    const std::uint16_t value = 1;
    return *reinterpret_cast<const std::uint8_t*>(&value) == 1;
}

void HashBytes(std::uint64_t* hash, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        *hash ^= bytes[i];
        *hash *= kFnvPrime;
    }
}

class FastHash64 {
public:
    void Update(const void* data, std::size_t size) {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        totalBytes_ += static_cast<std::uint64_t>(size);
        if (tailSize_ != 0) {
            const std::size_t needed = 8 - tailSize_;
            const std::size_t copied = std::min(needed, size);
            std::memcpy(tail_.data() + tailSize_, bytes, copied);
            tailSize_ += copied;
            bytes += copied;
            size -= copied;
            if (tailSize_ == 8) {
                Mix(ReadU64Le(tail_.data()));
                tailSize_ = 0;
            }
        }
        while (size >= 8) {
            Mix(ReadU64Le(bytes));
            bytes += 8;
            size -= 8;
        }
        if (size != 0) {
            std::memcpy(tail_.data(), bytes, size);
            tailSize_ = size;
        }
    }

    std::uint64_t Digest() const {
        std::uint64_t hash = state_ ^ (totalBytes_ * 0x9e3779b185ebca87ull);
        if (tailSize_ != 0) {
            std::uint64_t tail = 0;
            for (std::size_t i = 0; i < tailSize_; ++i) {
                tail |= static_cast<std::uint64_t>(tail_[i]) << (i * 8);
            }
            hash ^= tail * 0xc2b2ae3d27d4eb4full;
            hash = RotateLeft(hash, 31) * 0x165667b19e3779f9ull;
        }
        hash ^= hash >> 33;
        hash *= 0xff51afd7ed558ccdull;
        hash ^= hash >> 33;
        hash *= 0xc4ceb9fe1a85ec53ull;
        hash ^= hash >> 33;
        return hash;
    }

private:
    static std::uint64_t RotateLeft(std::uint64_t value, unsigned shift) {
        return (value << shift) | (value >> (64 - shift));
    }

    static std::uint64_t ReadU64Le(const std::uint8_t* bytes) {
        std::uint64_t value = 0;
        for (int i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(bytes[i]) << (i * 8);
        return value;
    }

    void Mix(std::uint64_t value) {
        state_ ^= value + 0x9e3779b185ebca87ull;
        state_ = RotateLeft(state_, 27) * 0x3c79ac492ba7b653ull + 0x1c69b3f74ac4ae35ull;
    }

    std::uint64_t state_ = 0x2d358dccaa6c78a5ull;
    std::uint64_t totalBytes_ = 0;
    std::array<std::uint8_t, 8> tail_{};
    std::size_t tailSize_ = 0;
};

fs::path NormalizeCachePath(const fs::path& sourcePath) {
    std::error_code ec;
    fs::path normalized = fs::weakly_canonical(sourcePath, ec);
    if (ec) {
        ec.clear();
        normalized = fs::absolute(sourcePath, ec);
        if (ec) normalized = sourcePath;
        normalized = normalized.lexically_normal();
    }
    return normalized;
}

std::uint64_t HashNormalizedSourcePath(const fs::path& sourcePath) {
    const fs::path normalized = NormalizeCachePath(sourcePath);

#ifdef _WIN32
    std::wstring text = normalized.native();
    std::uint64_t hash = kFnvOffset;
    for (wchar_t value : text) {
        std::uint32_t ch = static_cast<std::uint32_t>(value);
        if (ch == static_cast<std::uint32_t>(L'\\')) ch = static_cast<std::uint32_t>(L'/');
        if (ch >= static_cast<std::uint32_t>(L'A') && ch <= static_cast<std::uint32_t>(L'Z')) {
            ch += static_cast<std::uint32_t>(L'a' - L'A');
        }
        const std::array<std::uint8_t, 4> encoded = {
            static_cast<std::uint8_t>(ch),
            static_cast<std::uint8_t>(ch >> 8),
            static_cast<std::uint8_t>(ch >> 16),
            static_cast<std::uint8_t>(ch >> 24),
        };
        HashBytes(&hash, encoded.data(), encoded.size());
    }
#else
    const std::string text = normalized.generic_u8string();
    std::uint64_t hash = kFnvOffset;
    HashBytes(&hash, text.data(), text.size());
#endif
    return hash;
}

std::uint64_t HashCacheProfile(const std::string& profile) {
    std::uint64_t hash = kFnvOffset;
    HashBytes(&hash, profile.data(), profile.size());
    const std::uint32_t version = kPreviewMeshCacheFormatVersion;
    HashBytes(&hash, &version, sizeof(version));
    return hash;
}

std::wstring LowercaseExtension(const fs::path& path) {
    std::wstring extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t value) {
        return value >= L'A' && value <= L'Z' ? static_cast<wchar_t>(value + (L'a' - L'A')) : value;
    });
    return extension;
}

bool AppendAutomaticObjDependencies(const fs::path& sourcePath, std::vector<fs::path>* dependencies,
                                    std::string* errorText) {
    if (LowercaseExtension(sourcePath) != L".obj") return true;
    std::vector<fs::path> materialPaths;
    std::ifstream obj(sourcePath);
    if (obj) {
        std::string line;
        while (std::getline(obj, line)) {
            if (line.rfind("mtllib ", 0) == 0) {
                std::string materialName = line.substr(7);
                while (!materialName.empty() && std::isspace(static_cast<unsigned char>(materialName.back()))) {
                    materialName.pop_back();
                }
                while (!materialName.empty() && std::isspace(static_cast<unsigned char>(materialName.front()))) {
                    materialName.erase(materialName.begin());
                }
                if (!materialName.empty()) materialPaths.push_back(sourcePath.parent_path() / fs::path(materialName));
            } else if (line.rfind("v ", 0) == 0 || line.rfind("f ", 0) == 0) {
                break;
            }
        }
    }
    if (materialPaths.empty()) {
        materialPaths.push_back(sourcePath.parent_path() / (sourcePath.stem().wstring() + L".mtl"));
    }

    for (const fs::path& mtlPath : materialPaths) {
        dependencies->push_back(mtlPath);
        std::error_code ec;
        const bool exists = fs::exists(mtlPath, ec);
        if (ec) {
            SetError(errorText, ErrorMessage("Could not inspect OBJ material dependency", ec));
            return false;
        }
        if (!exists) continue;

        std::ifstream in(mtlPath);
        if (!in) {
            SetError(errorText, "Could not open OBJ material dependency");
            return false;
        }
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("map_Kd ", 0) != 0) continue;
            std::string textureName = line.substr(7);
            while (!textureName.empty() && std::isspace(static_cast<unsigned char>(textureName.back()))) {
                textureName.pop_back();
            }
            while (!textureName.empty() && std::isspace(static_cast<unsigned char>(textureName.front()))) {
                textureName.erase(textureName.begin());
            }
            if (textureName.size() >= 2 && textureName.front() == '"' && textureName.back() == '"') {
                textureName = textureName.substr(1, textureName.size() - 2);
            }
            if (!textureName.empty()) dependencies->push_back(mtlPath.parent_path() / fs::path(textureName));
        }
    }
    return true;
}

bool ReadFileFingerprint(const fs::path& filePath, std::uint64_t fileSize, std::uint64_t* fingerprint,
                         std::string* errorText) {
    std::ifstream in(filePath, std::ios::binary);
    if (!in) {
        SetError(errorText, "Could not open preview cache source for fingerprinting");
        return false;
    }
    constexpr std::uint64_t kSampleBytes = 64ull * 1024ull;
    const std::uint64_t sampleSize = std::min(fileSize, kSampleBytes);
    std::array<std::uint64_t, 3> offsets = {
        0,
        fileSize > sampleSize ? (fileSize - sampleSize) / 2 : 0,
        fileSize > sampleSize ? fileSize - sampleSize : 0,
    };
    std::sort(offsets.begin(), offsets.end());
    FastHash64 hash;
    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(sampleSize));
    std::uint64_t previous = std::numeric_limits<std::uint64_t>::max();
    for (std::uint64_t offset : offsets) {
        if (offset == previous) continue;
        previous = offset;
        hash.Update(&offset, sizeof(offset));
        if (sampleSize == 0) continue;
        if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
            SetError(errorText, "Preview cache source is too large to fingerprint");
            return false;
        }
        in.clear();
        in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        if (!in) {
            SetError(errorText, "Could not read preview cache source fingerprint sample");
            return false;
        }
        hash.Update(buffer.data(), buffer.size());
    }
    *fingerprint = hash.Digest();
    return true;
}

bool ReadFileStamp(const fs::path& filePath, bool requireExists, FileStamp* stamp, std::string* errorText) {
    if (!stamp) {
        SetError(errorText, "Preview cache file stamp output pointer was null");
        return false;
    }
    *stamp = {};
    stamp->pathHash = HashNormalizedSourcePath(filePath);
    std::error_code ec;
    const bool exists = fs::exists(filePath, ec);
    if (ec) {
        SetError(errorText, ErrorMessage("Could not inspect preview cache source", ec));
        return false;
    }
    if (!exists) {
        if (requireExists) SetError(errorText, "Preview cache source file does not exist");
        return !requireExists;
    }
    stamp->exists = true;
    const auto size = fs::file_size(filePath, ec);
    if (ec) {
        SetError(errorText, ErrorMessage("Could not read preview source size", ec));
        return false;
    }
    const auto writeTime = fs::last_write_time(filePath, ec);
    if (ec) {
        SetError(errorText, ErrorMessage("Could not read preview source last-write time", ec));
        return false;
    }
    const auto ticks = writeTime.time_since_epoch().count();
    using TickType = decltype(ticks);
    if constexpr (std::numeric_limits<TickType>::is_signed) {
        if (ticks < static_cast<TickType>(std::numeric_limits<std::int64_t>::min()) ||
            ticks > static_cast<TickType>(std::numeric_limits<std::int64_t>::max())) {
            SetError(errorText, "Preview source last-write time is out of cache range");
            return false;
        }
    } else if (ticks > static_cast<TickType>(std::numeric_limits<std::int64_t>::max())) {
        SetError(errorText, "Preview source last-write time is out of cache range");
        return false;
    }
    stamp->size = static_cast<std::uint64_t>(size);
    stamp->writeTime = static_cast<std::int64_t>(ticks);
    return ReadFileFingerprint(filePath, stamp->size, &stamp->fingerprint, errorText);
}

void AddFileStampToHash(const FileStamp& stamp, FastHash64* hash) {
    const std::uint8_t exists = stamp.exists ? 1 : 0;
    hash->Update(&stamp.pathHash, sizeof(stamp.pathHash));
    hash->Update(&exists, sizeof(exists));
    hash->Update(&stamp.size, sizeof(stamp.size));
    hash->Update(&stamp.writeTime, sizeof(stamp.writeTime));
    hash->Update(&stamp.fingerprint, sizeof(stamp.fingerprint));
}

bool CaptureSourceStamp(const fs::path& sourcePath, const std::vector<fs::path>& dependencyPaths,
                        std::uint64_t cacheProfileHash, PreviewMeshCacheSourceStamp* stamp,
                        std::string* errorText) {
    if (!stamp) {
        SetError(errorText, "Preview cache source stamp output pointer was null");
        return false;
    }
    const fs::path normalizedSourcePath = NormalizeCachePath(sourcePath);
    FileStamp source;
    if (!ReadFileStamp(normalizedSourcePath, true, &source, errorText)) return false;

    std::vector<fs::path> requestedDependencies = dependencyPaths;
    if (!AppendAutomaticObjDependencies(normalizedSourcePath, &requestedDependencies, errorText)) return false;

    std::vector<std::pair<std::uint64_t, fs::path>> dependencies;
    dependencies.reserve(requestedDependencies.size());
    for (const fs::path& dependency : requestedDependencies) {
        if (dependency.empty()) continue;
        const fs::path resolved = NormalizeCachePath(dependency.is_absolute()
                                                         ? dependency
                                                         : normalizedSourcePath.parent_path() / dependency);
        dependencies.push_back({ HashNormalizedSourcePath(resolved), resolved });
    }
    std::sort(dependencies.begin(), dependencies.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });
    dependencies.erase(std::unique(dependencies.begin(), dependencies.end(), [](const auto& left, const auto& right) {
        return left.first == right.first;
    }), dependencies.end());

    FastHash64 dependencyHash;
    std::vector<fs::path> normalizedDependencies;
    normalizedDependencies.reserve(dependencies.size());
    for (const auto& entry : dependencies) {
        FileStamp dependency;
        if (!ReadFileStamp(entry.second, false, &dependency, errorText)) return false;
        AddFileStampToHash(dependency, &dependencyHash);
        normalizedDependencies.push_back(entry.second);
    }

    stamp->sourcePath = normalizedSourcePath;
    stamp->dependencyPaths = std::move(normalizedDependencies);
    stamp->sourcePathHash = source.pathHash;
    stamp->sourceSize = source.size;
    stamp->sourceWriteTime = source.writeTime;
    stamp->sourceFingerprint = source.fingerprint;
    stamp->dependencyFingerprint = dependencyHash.Digest();
    stamp->dependencyCount = static_cast<std::uint64_t>(stamp->dependencyPaths.size());
    stamp->cacheProfileHash = cacheProfileHash;
    return true;
}

bool SameSourceStamp(const PreviewMeshCacheSourceStamp& left, const PreviewMeshCacheSourceStamp& right) {
    return left.sourcePathHash == right.sourcePathHash && left.sourceSize == right.sourceSize &&
           left.sourceWriteTime == right.sourceWriteTime && left.sourceFingerprint == right.sourceFingerprint &&
           left.dependencyFingerprint == right.dependencyFingerprint && left.dependencyCount == right.dependencyCount &&
           left.cacheProfileHash == right.cacheProfileHash;
}

std::string Hex64(std::uint64_t value) {
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << value;
    return out.str();
}

bool AddChecked(std::uint64_t value, std::uint64_t* total) {
    if (!total || value > std::numeric_limits<std::uint64_t>::max() - *total) return false;
    *total += value;
    return true;
}

bool MultiplyChecked(std::uint64_t count, std::uint64_t elementSize, std::uint64_t* bytes) {
    if (!bytes || (elementSize != 0 && count > std::numeric_limits<std::uint64_t>::max() / elementSize)) {
        return false;
    }
    *bytes = count * elementSize;
    return true;
}

bool ComputePayloadBytes(const CacheHeader& header, std::uint64_t* payloadBytes) {
    std::uint64_t total = 0;
    std::uint64_t bytes = 0;
    if (!MultiplyChecked(header.positionCount, sizeof(float), &bytes) || !AddChecked(bytes, &total) ||
        !MultiplyChecked(header.normalCount, sizeof(float), &bytes) || !AddChecked(bytes, &total) ||
        !MultiplyChecked(header.colorCount, sizeof(std::uint8_t), &bytes) || !AddChecked(bytes, &total) ||
        !MultiplyChecked(header.indexCount, sizeof(std::uint32_t), &bytes) || !AddChecked(bytes, &total)) {
        return false;
    }
    for (std::uint64_t count : header.lodIndexCounts) {
        if (!MultiplyChecked(count, sizeof(std::uint32_t), &bytes) || !AddChecked(bytes, &total)) return false;
    }
    if (total > kMaxCachePayloadBytes) return false;
    *payloadBytes = total;
    return true;
}

void PutU32(std::array<std::uint8_t, kCacheHeaderSize>* bytes, std::size_t* offset, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) (*bytes)[(*offset)++] = static_cast<std::uint8_t>(value >> (i * 8));
}

void PutU64(std::array<std::uint8_t, kCacheHeaderSize>* bytes, std::size_t* offset, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) (*bytes)[(*offset)++] = static_cast<std::uint8_t>(value >> (i * 8));
}

void PutFloat(std::array<std::uint8_t, kCacheHeaderSize>* bytes, std::size_t* offset, float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "Unexpected float size");
    std::memcpy(&bits, &value, sizeof(bits));
    PutU32(bytes, offset, bits);
}

std::array<std::uint8_t, kCacheHeaderSize> EncodeHeader(const CacheHeader& header) {
    std::array<std::uint8_t, kCacheHeaderSize> bytes{};
    std::copy(kCacheMagic.begin(), kCacheMagic.end(), bytes.begin());
    std::size_t offset = kCacheMagic.size();
    PutU32(&bytes, &offset, kPreviewMeshCacheFormatVersion);
    PutU32(&bytes, &offset, static_cast<std::uint32_t>(kCacheHeaderSize));
    PutU64(&bytes, &offset, header.sourcePathHash);
    PutU64(&bytes, &offset, header.sourceSize);
    PutU64(&bytes, &offset, static_cast<std::uint64_t>(header.sourceWriteTime));
    PutU64(&bytes, &offset, header.sourceFingerprint);
    PutU64(&bytes, &offset, header.dependencyFingerprint);
    PutU64(&bytes, &offset, header.dependencyCount);
    PutU64(&bytes, &offset, header.cacheProfileHash);
    PutU64(&bytes, &offset, header.positionCount);
    PutU64(&bytes, &offset, header.normalCount);
    PutU64(&bytes, &offset, header.colorCount);
    PutU64(&bytes, &offset, header.indexCount);
    for (std::uint64_t count : header.lodIndexCounts) PutU64(&bytes, &offset, count);
    for (float value : header.min) PutFloat(&bytes, &offset, value);
    for (float value : header.max) PutFloat(&bytes, &offset, value);
    PutU64(&bytes, &offset, header.payloadBytes);
    PutU64(&bytes, &offset, header.payloadHash);
    std::uint64_t headerHash = kFnvOffset;
    HashBytes(&headerHash, bytes.data(), offset);
    PutU64(&bytes, &offset, headerHash);
    return bytes;
}

std::uint32_t GetU32(const std::array<std::uint8_t, kCacheHeaderSize>& bytes, std::size_t* offset) {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(bytes[(*offset)++]) << (i * 8);
    return value;
}

std::uint64_t GetU64(const std::array<std::uint8_t, kCacheHeaderSize>& bytes, std::size_t* offset) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(bytes[(*offset)++]) << (i * 8);
    return value;
}

float GetFloat(const std::array<std::uint8_t, kCacheHeaderSize>& bytes, std::size_t* offset) {
    const std::uint32_t bits = GetU32(bytes, offset);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool DecodeHeader(const std::array<std::uint8_t, kCacheHeaderSize>& bytes, CacheHeader* header,
                  std::string* errorText) {
    if (!std::equal(kCacheMagic.begin(), kCacheMagic.end(), bytes.begin())) {
        SetError(errorText, "Preview cache magic does not match");
        return false;
    }
    std::size_t offset = kCacheMagic.size();
    const std::uint32_t version = GetU32(bytes, &offset);
    const std::uint32_t headerSize = GetU32(bytes, &offset);
    if (version != kPreviewMeshCacheFormatVersion || headerSize != kCacheHeaderSize) {
        SetError(errorText, "Preview cache format version does not match");
        return false;
    }
    std::uint64_t computedHeaderHash = kFnvOffset;
    HashBytes(&computedHeaderHash, bytes.data(), kCacheHeaderSize - sizeof(std::uint64_t));
    header->sourcePathHash = GetU64(bytes, &offset);
    header->sourceSize = GetU64(bytes, &offset);
    header->sourceWriteTime = static_cast<std::int64_t>(GetU64(bytes, &offset));
    header->sourceFingerprint = GetU64(bytes, &offset);
    header->dependencyFingerprint = GetU64(bytes, &offset);
    header->dependencyCount = GetU64(bytes, &offset);
    header->cacheProfileHash = GetU64(bytes, &offset);
    header->positionCount = GetU64(bytes, &offset);
    header->normalCount = GetU64(bytes, &offset);
    header->colorCount = GetU64(bytes, &offset);
    header->indexCount = GetU64(bytes, &offset);
    for (auto& count : header->lodIndexCounts) count = GetU64(bytes, &offset);
    for (auto& value : header->min) value = GetFloat(bytes, &offset);
    for (auto& value : header->max) value = GetFloat(bytes, &offset);
    header->payloadBytes = GetU64(bytes, &offset);
    header->payloadHash = GetU64(bytes, &offset);
    header->headerHash = GetU64(bytes, &offset);
    if (header->headerHash != computedHeaderHash) {
        SetError(errorText, "Preview cache header checksum does not match");
        return false;
    }
    return offset == kCacheHeaderSize;
}

bool CountsFitMemory(const CacheHeader& header) {
    const auto maxSize = static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
    if (header.positionCount > maxSize || header.normalCount > maxSize || header.colorCount > maxSize ||
        header.indexCount > maxSize) {
        return false;
    }
    return std::all_of(header.lodIndexCounts.begin(), header.lodIndexCounts.end(),
                       [maxSize](std::uint64_t value) { return value <= maxSize; });
}

bool ValidateHeaderShape(const CacheHeader& header, std::string* errorText) {
    if (header.positionCount == 0 || header.positionCount % 3 != 0 || header.indexCount == 0 ||
        header.indexCount % 3 != 0 || header.normalCount != header.positionCount) {
        SetError(errorText, "Preview cache header has invalid position, normal, or triangle counts");
        return false;
    }
    const std::uint64_t vertexCount = header.positionCount / 3;
    std::uint64_t expectedColors = 0;
    if (!MultiplyChecked(vertexCount, 4, &expectedColors) || header.colorCount != expectedColors) {
        SetError(errorText, "Preview cache header vertex attributes do not match");
        return false;
    }
    for (std::uint64_t count : header.lodIndexCounts) {
        if (count % 3 != 0) {
            SetError(errorText, "Preview cache header LOD count is not a triangle list");
            return false;
        }
    }
    return true;
}

bool PayloadSizeFitsSource(std::uint64_t payloadBytes, std::uint64_t sourceBytes) {
    std::uint64_t sourceAllowance = 0;
    if (!MultiplyChecked(sourceBytes, kPayloadSourceRatio, &sourceAllowance) ||
        !AddChecked(kPayloadSourceSlack, &sourceAllowance)) {
        sourceAllowance = kMaxCachePayloadBytes;
    }
    return payloadBytes <= std::min(sourceAllowance, kMaxCachePayloadBytes);
}

bool ValidateMeshShape(const PreviewMesh& mesh, std::string* errorText) {
    if (mesh.positions.empty() || mesh.indices.empty() || mesh.positions.size() % 3 != 0 ||
        mesh.indices.size() % 3 != 0) {
        SetError(errorText, "Preview cache mesh has invalid position or triangle counts");
        return false;
    }
    const std::size_t vertexCount = mesh.positions.size() / 3;
    if (mesh.normals.size() != mesh.positions.size() ||
        vertexCount > std::numeric_limits<std::size_t>::max() / 4 ||
        mesh.colors.size() != vertexCount * 4) {
        SetError(errorText, "Preview cache mesh vertex attributes do not match");
        return false;
    }
    for (std::size_t level = 0; level < mesh.lodIndices.size(); ++level) {
        if (mesh.lodIndices[level].size() % 3 != 0) {
            SetError(errorText, "Preview cache LOD index count is not a triangle list");
            return false;
        }
    }
    for (float value : mesh.positions) {
        if (!std::isfinite(value)) {
            SetError(errorText, "Preview cache contains a non-finite position");
            return false;
        }
    }
    for (float value : mesh.normals) {
        if (!std::isfinite(value)) {
            SetError(errorText, "Preview cache contains a non-finite normal");
            return false;
        }
    }
    for (std::uint32_t index : mesh.indices) {
        if (index >= vertexCount) {
            SetError(errorText, "Preview cache contains an out-of-range source index");
            return false;
        }
    }
    for (const auto& lod : mesh.lodIndices) {
        for (std::uint32_t index : lod) {
            if (index >= vertexCount) {
                SetError(errorText, "Preview cache contains an out-of-range LOD index");
                return false;
            }
        }
    }
    for (int axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(mesh.min[axis]) || !std::isfinite(mesh.max[axis]) || mesh.min[axis] > mesh.max[axis]) {
            SetError(errorText, "Preview cache contains invalid bounds");
            return false;
        }
    }
    return true;
}

class PayloadWriter {
public:
    explicit PayloadWriter(std::ofstream* stream) : stream_(stream) {}

    bool Write(const void* data, std::size_t size) {
        if (!stream_ || !*stream_) return false;
        if (size > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) return false;
        stream_->write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!*stream_) return false;
        hash_.Update(data, size);
        return true;
    }

    std::uint64_t hash() const { return hash_.Digest(); }

private:
    std::ofstream* stream_ = nullptr;
    FastHash64 hash_;
};

class PayloadReader {
public:
    PayloadReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    bool Read(void* data, std::size_t size) {
        if (!data || (!data_ && size_ != 0) || offset_ > size_ || size > size_ - offset_) return false;
        if (size != 0) std::memcpy(data, data_ + offset_, size);
        offset_ += size;
        hash_.Update(data, size);
        return true;
    }

    std::uint64_t hash() const { return hash_.Digest(); }
    std::size_t bytesRead() const { return offset_; }

private:
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t offset_ = 0;
    FastHash64 hash_;
};

template <typename T>
bool WriteVector(PayloadWriter* writer, const std::vector<T>& values) {
    static_assert(std::is_same<T, float>::value || std::is_same<T, std::uint32_t>::value ||
                      std::is_same<T, std::uint8_t>::value,
                  "Unsupported preview cache payload type");
    if (values.empty()) return true;
    if (values.size() > std::numeric_limits<std::size_t>::max() / sizeof(T)) return false;
    if (IsLittleEndian() || sizeof(T) == 1) return writer->Write(values.data(), values.size() * sizeof(T));

    std::array<std::uint8_t, 64 * 1024> bytes{};
    const std::size_t valuesPerChunk = bytes.size() / sizeof(T);
    for (std::size_t base = 0; base < values.size(); base += valuesPerChunk) {
        const std::size_t count = std::min(valuesPerChunk, values.size() - base);
        for (std::size_t i = 0; i < count; ++i) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &values[base + i], sizeof(T));
            for (std::size_t byte = 0; byte < sizeof(T); ++byte) {
                bytes[i * sizeof(T) + byte] = static_cast<std::uint8_t>(bits >> (byte * 8));
            }
        }
        if (!writer->Write(bytes.data(), count * sizeof(T))) return false;
    }
    return true;
}

template <typename T>
bool ReadVector(PayloadReader* reader, std::uint64_t count, std::vector<T>* values) {
    if (count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) return false;
    const std::size_t size = static_cast<std::size_t>(count);
    if (size > std::numeric_limits<std::size_t>::max() / sizeof(T)) return false;
    try {
        values->resize(size);
    } catch (const std::bad_alloc&) {
        return false;
    } catch (const std::length_error&) {
        return false;
    }
    if (size == 0) return true;
    if (IsLittleEndian() || sizeof(T) == 1) return reader->Read(values->data(), size * sizeof(T));

    std::array<std::uint8_t, 64 * 1024> bytes{};
    const std::size_t valuesPerChunk = bytes.size() / sizeof(T);
    for (std::size_t base = 0; base < size; base += valuesPerChunk) {
        const std::size_t chunkCount = std::min(valuesPerChunk, size - base);
        if (!reader->Read(bytes.data(), chunkCount * sizeof(T))) return false;
        for (std::size_t i = 0; i < chunkCount; ++i) {
            std::uint32_t bits = 0;
            for (std::size_t byte = 0; byte < sizeof(T); ++byte) {
                bits |= static_cast<std::uint32_t>(bytes[i * sizeof(T) + byte]) << (byte * 8);
            }
            std::memcpy(&(*values)[base + i], &bits, sizeof(T));
        }
    }
    return true;
}

CacheHeader HeaderForMesh(const PreviewMeshCacheSourceStamp& source, const PreviewMesh& mesh) {
    CacheHeader header;
    header.sourcePathHash = source.sourcePathHash;
    header.sourceSize = source.sourceSize;
    header.sourceWriteTime = source.sourceWriteTime;
    header.sourceFingerprint = source.sourceFingerprint;
    header.dependencyFingerprint = source.dependencyFingerprint;
    header.dependencyCount = source.dependencyCount;
    header.cacheProfileHash = source.cacheProfileHash;
    header.positionCount = static_cast<std::uint64_t>(mesh.positions.size());
    header.normalCount = static_cast<std::uint64_t>(mesh.normals.size());
    header.colorCount = static_cast<std::uint64_t>(mesh.colors.size());
    header.indexCount = static_cast<std::uint64_t>(mesh.indices.size());
    for (std::size_t level = 0; level < mesh.lodIndices.size(); ++level) {
        header.lodIndexCounts[level] = static_cast<std::uint64_t>(mesh.lodIndices[level].size());
    }
    header.min = mesh.min;
    header.max = mesh.max;
    ComputePayloadBytes(header, &header.payloadBytes);
    return header;
}

fs::path TemporaryCachePath(const fs::path& cachePath) {
    static std::atomic<std::uint64_t> counter{ 0 };
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
#ifdef _WIN32
    const std::uint64_t process = static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
    const std::uint64_t process = 0;
#endif
    fs::path result = cachePath;
    result += ".tmp-" + Hex64(process) + "-" + Hex64(static_cast<std::uint64_t>(now)) + "-" +
              Hex64(counter.fetch_add(1, std::memory_order_relaxed));
    return result;
}

bool PublishTemporaryCache(const fs::path& temporaryPath, const fs::path& cachePath, std::string* errorText) {
#ifdef _WIN32
    if (::MoveFileExW(temporaryPath.c_str(), cachePath.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        const std::error_code ec(static_cast<int>(::GetLastError()), std::system_category());
        SetError(errorText, ErrorMessage("Could not publish preview cache", ec));
        return false;
    }
    return true;
#else
    std::error_code ec;
    fs::rename(temporaryPath, cachePath, ec);
    if (ec) {
        SetError(errorText, ErrorMessage("Could not publish preview cache", ec));
        return false;
    }
    return true;
#endif
}

}  // namespace

bool CapturePreviewMeshCacheSourceStamp(const fs::path& sourcePath,
                                        const std::vector<fs::path>& dependencyPaths,
                                        PreviewMeshCacheSourceStamp* outStamp,
                                        std::string* errorText,
                                        const std::string& cacheProfile) {
    if (errorText) errorText->clear();
    return CaptureSourceStamp(sourcePath, dependencyPaths, HashCacheProfile(cacheProfile), outStamp, errorText);
}

fs::path PreviewMeshCachePath(const fs::path& cacheDirectory,
                              const PreviewMeshCacheSourceStamp& sourceStamp) {
    if (cacheDirectory.empty() || sourceStamp.sourcePath.empty() || sourceStamp.sourcePathHash == 0) return {};
    const std::string hash = Hex64(sourceStamp.sourcePathHash);
    const std::string profile = Hex64(sourceStamp.cacheProfileHash);
    return cacheDirectory / "preview-mesh" / ("v" + std::to_string(kPreviewMeshCacheFormatVersion)) /
           hash.substr(0, 2) / (hash + "-" + profile + ".pmesh");
}

fs::path PreviewMeshCachePath(const fs::path& cacheDirectory, const fs::path& sourcePath) {
    if (cacheDirectory.empty() || sourcePath.empty()) return {};
    PreviewMeshCacheSourceStamp stamp;
    stamp.sourcePath = sourcePath;
    stamp.sourcePathHash = HashNormalizedSourcePath(sourcePath);
    stamp.cacheProfileHash = HashCacheProfile({});
    return PreviewMeshCachePath(cacheDirectory, stamp);
}

PreviewMeshCacheLoadResult LoadPreviewMeshCache(const fs::path& cacheDirectory,
                                                const PreviewMeshCacheSourceStamp& sourceStamp,
                                                PreviewMesh* outMesh) {
    PreviewMeshCacheLoadResult result;
    result.cachePath = PreviewMeshCachePath(cacheDirectory, sourceStamp);
    if (!outMesh) {
        result.status = PreviewMeshCacheLoadStatus::IoError;
        result.message = "Preview cache output pointer was null";
        return result;
    }
    if (result.cachePath.empty()) {
        result.status = PreviewMeshCacheLoadStatus::Miss;
        return result;
    }

    std::error_code ec;
    const bool exists = fs::exists(result.cachePath, ec);
    if (ec) {
        result.status = PreviewMeshCacheLoadStatus::IoError;
        result.message = ErrorMessage("Could not inspect preview cache", ec);
        return result;
    }
    if (!exists) {
        result.status = PreviewMeshCacheLoadStatus::Miss;
        return result;
    }

    const std::uintmax_t fileSize = fs::file_size(result.cachePath, ec);
    if (ec) {
        result.status = PreviewMeshCacheLoadStatus::IoError;
        result.message = ErrorMessage("Could not read preview cache size", ec);
        return result;
    }
    if (fileSize < kCacheHeaderSize || fileSize > kCacheHeaderSize + kMaxCachePayloadBytes) {
        result.status = PreviewMeshCacheLoadStatus::Invalid;
        result.message = "Preview cache file size is outside the supported range";
        return result;
    }
    const std::uint64_t payloadFileBytes = static_cast<std::uint64_t>(fileSize - kCacheHeaderSize);
    if (!PayloadSizeFitsSource(payloadFileBytes, sourceStamp.sourceSize)) {
        result.status = PreviewMeshCacheLoadStatus::Invalid;
        result.message = "Preview cache payload is too large for its source model";
        return result;
    }

    std::vector<std::uint8_t> fileBytes;
    result.fileRead = ReadPreviewFileToMemory(
        result.cachePath, &fileBytes, kCacheHeaderSize + kMaxCachePayloadBytes);
    if (!result.fileRead.success) {
        result.status = PreviewMeshCacheLoadStatus::IoError;
        result.message = result.fileRead.error.empty()
            ? "Could not read preview cache"
            : result.fileRead.error;
        return result;
    }
    if (fileBytes.size() != static_cast<std::size_t>(fileSize)) {
        result.status = PreviewMeshCacheLoadStatus::Invalid;
        result.message = "Preview cache size changed while it was being read";
        return result;
    }
    std::array<std::uint8_t, kCacheHeaderSize> headerBytes{};
    std::memcpy(headerBytes.data(), fileBytes.data(), headerBytes.size());

    CacheHeader header;
    if (!DecodeHeader(headerBytes, &header, &result.message)) {
        result.status = PreviewMeshCacheLoadStatus::Invalid;
        return result;
    }
    if (header.sourcePathHash != sourceStamp.sourcePathHash ||
        header.sourceSize != sourceStamp.sourceSize ||
        header.sourceWriteTime != sourceStamp.sourceWriteTime ||
        header.sourceFingerprint != sourceStamp.sourceFingerprint ||
        header.dependencyFingerprint != sourceStamp.dependencyFingerprint ||
        header.dependencyCount != sourceStamp.dependencyCount ||
        header.cacheProfileHash != sourceStamp.cacheProfileHash) {
        result.status = PreviewMeshCacheLoadStatus::Stale;
        result.message = "Preview cache source metadata no longer matches";
        return result;
    }

    std::uint64_t computedPayloadBytes = 0;
    if (!ValidateHeaderShape(header, &result.message) || !CountsFitMemory(header) ||
        !ComputePayloadBytes(header, &computedPayloadBytes) ||
        computedPayloadBytes != header.payloadBytes ||
        !PayloadSizeFitsSource(header.payloadBytes, sourceStamp.sourceSize) ||
        header.payloadBytes != static_cast<std::uint64_t>(fileSize - kCacheHeaderSize)) {
        result.status = PreviewMeshCacheLoadStatus::Invalid;
        if (result.message.empty()) result.message = "Preview cache payload lengths are invalid";
        return result;
    }

    PreviewMesh loaded;
    PayloadReader reader(fileBytes.data() + kCacheHeaderSize, fileBytes.size() - kCacheHeaderSize);
    if (!ReadVector(&reader, header.positionCount, &loaded.positions) ||
        !ReadVector(&reader, header.normalCount, &loaded.normals) ||
        !ReadVector(&reader, header.colorCount, &loaded.colors) ||
        !ReadVector(&reader, header.indexCount, &loaded.indices)) {
        result.status = PreviewMeshCacheLoadStatus::Invalid;
        result.message = "Preview cache payload is truncated or too large for memory";
        return result;
    }
    for (std::size_t level = 0; level < loaded.lodIndices.size(); ++level) {
        if (!ReadVector(&reader, header.lodIndexCounts[level], &loaded.lodIndices[level])) {
            result.status = PreviewMeshCacheLoadStatus::Invalid;
            result.message = "Preview cache LOD payload is truncated or too large for memory";
            return result;
        }
    }
    if (reader.hash() != header.payloadHash) {
        result.status = PreviewMeshCacheLoadStatus::Invalid;
        result.message = "Preview cache payload checksum does not match";
        return result;
    }
    if (reader.bytesRead() != header.payloadBytes) {
        result.status = PreviewMeshCacheLoadStatus::Invalid;
        result.message = "Preview cache payload has trailing or unread bytes";
        return result;
    }
    loaded.min = header.min;
    loaded.max = header.max;
    if (!ValidateMeshShape(loaded, &result.message)) {
        result.status = PreviewMeshCacheLoadStatus::Invalid;
        return result;
    }

    PreviewMeshCacheSourceStamp sourceAfter;
    if (!CaptureSourceStamp(sourceStamp.sourcePath, sourceStamp.dependencyPaths,
                            sourceStamp.cacheProfileHash, &sourceAfter, &result.message)) {
        result.status = PreviewMeshCacheLoadStatus::IoError;
        return result;
    }
    if (!SameSourceStamp(sourceStamp, sourceAfter)) {
        result.status = PreviewMeshCacheLoadStatus::Stale;
        result.message = "Preview source changed while its cache was being read";
        return result;
    }

    *outMesh = std::move(loaded);
    result.status = PreviewMeshCacheLoadStatus::Hit;
    result.message.clear();
    return result;
}

PreviewMeshCacheLoadResult LoadPreviewMeshCache(const fs::path& cacheDirectory,
                                                const fs::path& sourcePath,
                                                PreviewMesh* outMesh) {
    PreviewMeshCacheSourceStamp stamp;
    std::string error;
    if (!CapturePreviewMeshCacheSourceStamp(sourcePath, {}, &stamp, &error)) {
        PreviewMeshCacheLoadResult result;
        result.status = PreviewMeshCacheLoadStatus::IoError;
        result.message = error;
        return result;
    }
    return LoadPreviewMeshCache(cacheDirectory, stamp, outMesh);
}

bool SavePreviewMeshCache(const fs::path& cacheDirectory,
                          const PreviewMeshCacheSourceStamp& sourceStamp,
                          const PreviewMesh& mesh, std::string* errorText) {
    if (errorText) errorText->clear();
    const fs::path cachePath = PreviewMeshCachePath(cacheDirectory, sourceStamp);
    if (cachePath.empty()) {
        SetError(errorText, "Preview cache or source path was empty");
        return false;
    }
    if (!ValidateMeshShape(mesh, errorText)) return false;

    PreviewMeshCacheSourceStamp sourceBefore;
    if (!CaptureSourceStamp(sourceStamp.sourcePath, sourceStamp.dependencyPaths,
                            sourceStamp.cacheProfileHash, &sourceBefore, errorText) ||
        !SameSourceStamp(sourceStamp, sourceBefore)) {
        if (errorText && errorText->empty()) *errorText = "Preview source changed before its cache was written";
        return false;
    }
    CacheHeader header = HeaderForMesh(sourceStamp, mesh);
    std::uint64_t checkedPayloadBytes = 0;
    if (!ComputePayloadBytes(header, &checkedPayloadBytes) || checkedPayloadBytes != header.payloadBytes ||
        !PayloadSizeFitsSource(header.payloadBytes, sourceStamp.sourceSize)) {
        SetError(errorText, "Preview mesh is too large for the cache format");
        return false;
    }

    std::error_code ec;
    fs::create_directories(cachePath.parent_path(), ec);
    if (ec) {
        SetError(errorText, ErrorMessage("Could not create preview cache directory", ec));
        return false;
    }

    const fs::path temporaryPath = TemporaryCachePath(cachePath);
    bool published = false;
    auto cleanup = [&]() {
        if (published) return;
        std::error_code ignored;
        fs::remove(temporaryPath, ignored);
    };

    std::ofstream out(temporaryPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        SetError(errorText, "Could not open temporary preview cache for writing");
        cleanup();
        return false;
    }
    auto headerBytes = EncodeHeader(header);
    out.write(reinterpret_cast<const char*>(headerBytes.data()), static_cast<std::streamsize>(headerBytes.size()));
    PayloadWriter writer(&out);
    bool wrotePayload = static_cast<bool>(out) && WriteVector(&writer, mesh.positions) &&
                        WriteVector(&writer, mesh.normals) && WriteVector(&writer, mesh.colors) &&
                        WriteVector(&writer, mesh.indices);
    for (const auto& lod : mesh.lodIndices) wrotePayload = wrotePayload && WriteVector(&writer, lod);
    if (!wrotePayload) {
        SetError(errorText, "Could not write preview cache payload");
        out.close();
        cleanup();
        return false;
    }

    header.payloadHash = writer.hash();
    headerBytes = EncodeHeader(header);
    out.seekp(0, std::ios::beg);
    out.write(reinterpret_cast<const char*>(headerBytes.data()), static_cast<std::streamsize>(headerBytes.size()));
    out.flush();
    if (!out) {
        SetError(errorText, "Could not finalize preview cache header");
        out.close();
        cleanup();
        return false;
    }
    out.close();
    if (!out) {
        SetError(errorText, "Could not close temporary preview cache cleanly");
        cleanup();
        return false;
    }

    PreviewMeshCacheSourceStamp sourceAfter;
    if (!CaptureSourceStamp(sourceStamp.sourcePath, sourceStamp.dependencyPaths,
                            sourceStamp.cacheProfileHash, &sourceAfter, errorText) ||
        !SameSourceStamp(sourceStamp, sourceAfter)) {
        if (errorText && errorText->empty()) *errorText = "Preview source changed while its cache was being written";
        cleanup();
        return false;
    }
    if (!PublishTemporaryCache(temporaryPath, cachePath, errorText)) {
        cleanup();
        return false;
    }
    published = true;
    return true;
}

bool SavePreviewMeshCache(const fs::path& cacheDirectory, const fs::path& sourcePath,
                          const PreviewMesh& mesh, std::string* errorText) {
    PreviewMeshCacheSourceStamp stamp;
    if (!CapturePreviewMeshCacheSourceStamp(sourcePath, {}, &stamp, errorText)) return false;
    return SavePreviewMeshCache(cacheDirectory, stamp, mesh, errorText);
}

}  // namespace native_mc
