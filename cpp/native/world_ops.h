#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace native_mc {

struct ChunkCoord {
    int x = 0;
    int z = 0;

    bool operator<(const ChunkCoord& other) const {
        return x < other.x || (x == other.x && z < other.z);
    }
};

using PackedBlockMap = std::vector<std::pair<std::uint32_t, std::string>>;

struct ScanSummary {
    std::uint64_t totalBlocks = 0;
    std::uint64_t regions = 0;
    std::uint64_t chunks = 0;
    bool hasBounds = false;
    int minX = 0;
    int minY = 0;
    int minZ = 0;
    int maxX = 0;
    int maxY = 0;
    int maxZ = 0;
    std::map<std::string, std::uint64_t> byBlock;
};

struct WorldPreviewBlock {
    int x = 0;
    int y = 0;
    int bottomY = 0;
    int z = 0;
    int sampleSizeX = 1;
    int sampleSizeZ = 1;
    std::string blockName;
};

struct WorldPreviewSlice {
    std::vector<WorldPreviewBlock> blocks;
    int centerX = 0;
    int centerZ = 0;
    bool truncated = false;
};

constexpr int kMinY = -64;
constexpr int kHeight = 1536;
constexpr int kDataVersion = 4903;

std::filesystem::path RegionPathForChunk(const std::filesystem::path& regionDir, int cx, int cz);
// Java Edition stores the overworld at <world>/region. Older test worlds made
// by this project used the dimension-style path, so keep that as a fallback.
std::filesystem::path ResolveOverworldRegionDir(const std::filesystem::path& worldDir);
int LocalChunkIndex(int cx, int cz);
bool ResetWorldToBlankSuperflat(const std::filesystem::path& worldDir, std::filesystem::path* backupRoot, std::string* errorText);
bool ReadWorldDataVersion(const std::filesystem::path& worldDir, int* dataVersion, std::string* errorText);
bool ScanWorldBlocks(const std::filesystem::path& worldDir, std::optional<int> minY, std::optional<int> maxY, ScanSummary* summary, std::string* errorText, const std::set<std::string>* allowedBlocks = nullptr);
bool FindWorldPreviewAnchor(const std::filesystem::path& worldDir, int* centerX, int* centerZ, std::string* errorText);
bool LoadWorldPreviewSlice(const std::filesystem::path& worldDir, int centerX, int centerZ, int radiusX, int radiusZ, std::optional<int> minY, std::optional<int> maxY, std::size_t maxBlocks, WorldPreviewSlice* slice, std::string* errorText);
bool WriteManyChunkBlockMap(const std::filesystem::path& regionDir, const std::map<ChunkCoord, PackedBlockMap>& chunkMap,
                            bool superflat, bool overwriteExisting, std::size_t* written,
                            std::size_t* regionCount, std::string* errorText);

}  // namespace native_mc
