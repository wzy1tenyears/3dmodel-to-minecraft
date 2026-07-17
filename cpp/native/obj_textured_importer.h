#pragma once

#include "glb_surface_importer.h"
#include "palette.h"

#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace native_mc {

struct ObjTexturedOptions {
    std::filesystem::path projectRoot;
    std::filesystem::path cacheRoot;
    std::filesystem::path objDir;
    std::filesystem::path worldDir;
    std::filesystem::path onlyFile;
    bool allFiles = false;
    bool overwrite = true;
    int workers = 1;
    int batchBlockLimit = 2000000;
    std::string paletteMode = "clean";
    std::string transform = "obj-z-up";
    bool flipX = false;
    bool flipZ = false;
    double scaleBlocksPerMeter = 4.0;
    int baseY = -32;
    Center3i center;
    std::optional<std::array<double, 3>> levelNormal;
    double rotateYDeg = 0.0;
    std::optional<std::array<double, 4>> modelRotation;
    int dedupeSampleSteps = 2;
    bool textured = true;
    std::string surfaceBlockName = "minecraft:light_gray_concrete";
    bool componentFilter = true;
    double componentMinRatio = 0.001;
    double componentBelowGap = 12.0;
    std::filesystem::path clipReferenceObj;
    double clipBelowMeters = 0.0;
    double clipCellSize = 1.0;
    double clipLowFraction = 0.25;
    int clipPasses = 3;
    double clipTrimSigma = 1.5;
    PaletteOptions palette;
};

struct ObjTexturedFileResult {
    std::string name;
    std::size_t triangles = 0;
    std::size_t skipped = 0;
    std::size_t clippedTriangles = 0;
    std::size_t clippedSamples = 0;
    std::size_t duplicateSamples = 0;
    std::size_t blocks = 0;
    std::size_t chunks = 0;
    std::size_t textures = 0;
    std::size_t textureCacheSize = 0;
    std::size_t written = 0;
    std::size_t regions = 0;
    std::array<int, 3> min = { 0, 0, 0 };
    std::array<int, 3> max = { 0, 0, 0 };
    bool hasBounds = false;
    std::uint64_t milliseconds = 0;
};

struct ObjTexturedImportResult {
    std::filesystem::path worldDir;
    std::vector<int> sizeBlocks;
    std::vector<double> globalMin;
    std::vector<double> globalMax;
    std::vector<int> origin;
    std::size_t paletteSize = 0;
    std::vector<ObjTexturedFileResult> files;
};

bool ImportObjTextured(const ObjTexturedOptions& options, ObjTexturedImportResult* outResult, std::string* errorText);
bool EstimateObjLevelNormal(const std::filesystem::path& filePath, double cellSize, double lowFraction,
                            int passes, double trimSigma, std::array<double, 3>* normal,
                            std::string* errorText);

}  // namespace native_mc
