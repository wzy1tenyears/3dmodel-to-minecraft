#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace native_mc {

struct PaletteOptions {
    std::string mode = "scene";
    std::string blockTypes;
    std::string blocks;
    std::string excludeBlockTypes;
    std::string excludeBlocks;
    bool noGlass = false;
};

struct PaletteBlock {
    std::string name;
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

struct PaletteCatalog {
    std::vector<PaletteBlock> availableBlocks;
    std::vector<PaletteBlock> baseBlocks;
    std::vector<PaletteBlock> includedBlocks;
};

class Palette {
public:
    Palette() = default;
    explicit Palette(std::vector<PaletteBlock> blocks);

    const std::vector<PaletteBlock>& blocks() const;
    std::string Nearest(int r, int g, int b) const;

private:
    std::vector<PaletteBlock> blocks_;
    mutable std::vector<std::optional<std::string>> cache_;
};

bool LoadPalette(const std::filesystem::path& projectRoot, const PaletteOptions& options, Palette* outPalette, std::string* errorText);
bool LoadPaletteCatalog(const std::filesystem::path& projectRoot, const PaletteOptions& options, PaletteCatalog* outCatalog, std::string* errorText);

}  // namespace native_mc
