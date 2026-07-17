#pragma once

#include "preview_directstorage.h"
#include "preview_mesh.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace native_mc {

constexpr std::uint32_t kPreviewMeshCacheFormatVersion = 1;

enum class PreviewMeshCacheLoadStatus {
    Hit,
    Miss,
    Stale,
    Invalid,
    IoError,
};

struct PreviewMeshCacheLoadResult {
    PreviewMeshCacheLoadStatus status = PreviewMeshCacheLoadStatus::Miss;
    std::filesystem::path cachePath;
    std::string message;
    PreviewFileReadResult fileRead;

    bool hit() const { return status == PreviewMeshCacheLoadStatus::Hit; }
};

// Capture this before parsing. Passing the same stamp to SavePreviewMeshCache
// proves that the source (and any supplied MTL/texture dependencies) did not
// change while the mesh was being generated. Paths exist only in process memory;
// cache files store hashes and metadata, never path text.
struct PreviewMeshCacheSourceStamp {
    std::filesystem::path sourcePath;
    std::vector<std::filesystem::path> dependencyPaths;
    std::uint64_t sourcePathHash = 0;
    std::uint64_t sourceSize = 0;
    std::int64_t sourceWriteTime = 0;
    std::uint64_t sourceFingerprint = 0;
    std::uint64_t dependencyFingerprint = 0;
    std::uint64_t dependencyCount = 0;
    std::uint64_t cacheProfileHash = 0;
};

bool CapturePreviewMeshCacheSourceStamp(
    const std::filesystem::path& sourcePath,
    const std::vector<std::filesystem::path>& dependencyPaths,
    PreviewMeshCacheSourceStamp* outStamp,
    std::string* errorText = nullptr,
    const std::string& cacheProfile = {});

// The returned path is derived from a stable hash of the normalized source path.
// No source path text is stored in the cache file or used in its filename.
std::filesystem::path PreviewMeshCachePath(const std::filesystem::path& cacheDirectory,
                                           const std::filesystem::path& sourcePath);
std::filesystem::path PreviewMeshCachePath(const std::filesystem::path& cacheDirectory,
                                           const PreviewMeshCacheSourceStamp& sourceStamp);

// A cache hit is accepted only when its format version, normalized source-path
// hash, source size, and source last-write time all match the current source.
PreviewMeshCacheLoadResult LoadPreviewMeshCache(const std::filesystem::path& cacheDirectory,
                                                const std::filesystem::path& sourcePath,
                                                PreviewMesh* outMesh);
PreviewMeshCacheLoadResult LoadPreviewMeshCache(const std::filesystem::path& cacheDirectory,
                                                const PreviewMeshCacheSourceStamp& sourceStamp,
                                                PreviewMesh* outMesh);

// Writes to a same-directory temporary file and atomically renames it into place.
// The source is restatted before the rename so a cache is never published for a
// source that changed while the payload was being written.
bool SavePreviewMeshCache(const std::filesystem::path& cacheDirectory,
                          const std::filesystem::path& sourcePath,
                          const PreviewMesh& mesh,
                          std::string* errorText = nullptr);
bool SavePreviewMeshCache(const std::filesystem::path& cacheDirectory,
                          const PreviewMeshCacheSourceStamp& sourceStamp,
                          const PreviewMesh& mesh,
                          std::string* errorText = nullptr);

}  // namespace native_mc
