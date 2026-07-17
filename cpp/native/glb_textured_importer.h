#pragma once

#include "glb_surface_importer.h"
#include "palette.h"

#include <filesystem>
#include <array>
#include <optional>
#include <string>
#include <vector>

namespace native_mc {

struct GlbTexturedOptions {
    std::filesystem::path projectRoot;
    std::filesystem::path glbDir;
    std::filesystem::path worldDir;
    std::filesystem::path onlyFile;
    bool allFiles = false;
    bool overwrite = true;
    int batchBlockLimit = 1500000;
    int baseY = -32;
    double scaleBlocksPerMeter = 4.0;
    double rotateYDeg = 0.0;
    std::optional<std::array<double, 4>> modelRotation;
    bool flipX = false;
    bool flipZ = false;
    Center3i center;
    std::string fallbackBlock = "minecraft:light_gray_concrete";
    int dedupeSampleSteps = 2;
    PaletteOptions palette;
};

struct GlbTexturedFileResult {
    std::string name;
    std::size_t triangles = 0;
    std::size_t blocks = 0;
    std::size_t chunks = 0;
    std::size_t regions = 0;
    std::size_t written = 0;
    std::size_t textures = 0;
    std::size_t skippedNoIndex = 0;
    std::size_t skippedNoUv = 0;
    std::size_t duplicateSamples = 0;
    std::uint64_t milliseconds = 0;
};

struct GlbTexturedImportResult {
    std::filesystem::path worldDir;
    std::vector<int> sizeBlocks;
    std::vector<double> globalMin;
    std::vector<double> globalMax;
    std::vector<int> origin;
    std::size_t paletteSize = 0;
    std::vector<GlbTexturedFileResult> files;
};

bool ImportGlbTextured(const GlbTexturedOptions& options, GlbTexturedImportResult* outResult, std::string* errorText);

}  // namespace native_mc
