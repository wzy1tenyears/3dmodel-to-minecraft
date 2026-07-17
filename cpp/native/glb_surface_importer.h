#pragma once

#include <filesystem>
#include <array>
#include <optional>
#include <string>
#include <vector>

namespace native_mc {

struct Center3i {
    std::optional<int> x;
    std::optional<int> y;
    std::optional<int> z;
};

struct GlbSurfaceOptions {
    std::filesystem::path glbDir;
    std::filesystem::path worldDir;
    std::filesystem::path onlyFile;
    bool allFiles = false;
    bool overwrite = true;
    int batchBlockLimit = 0;
    int baseY = -32;
    double scaleBlocksPerMeter = 4.0;
    double rotateYDeg = 0.0;
    std::optional<std::array<double, 4>> modelRotation;
    bool flipX = false;
    bool flipZ = false;
    std::string blockName = "minecraft:light_gray_concrete";
    Center3i center;
};

struct GlbSurfaceFileResult {
    std::string name;
    std::size_t triangles = 0;
    std::size_t blocks = 0;
    std::size_t chunks = 0;
    std::size_t regions = 0;
    std::size_t written = 0;
    std::uint64_t milliseconds = 0;
};

struct GlbSurfaceImportResult {
    std::filesystem::path worldDir;
    std::vector<int> sizeBlocks;
    std::vector<double> globalMin;
    std::vector<double> globalMax;
    std::vector<int> origin;
    std::vector<GlbSurfaceFileResult> files;
};

bool ImportGlbSurface(const GlbSurfaceOptions& options, GlbSurfaceImportResult* outResult, std::string* errorText);

}  // namespace native_mc
