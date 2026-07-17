#include "palette.h"

#include "../third_party/json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace native_mc {

namespace {

struct ExtraBlock {
    const char* name;
    int r;
    int g;
    int b;
};

const std::vector<std::string> kDenySubstrings = { "_ore", "redstone", "glowstone" };
const std::vector<ExtraBlock> kSceneExtraBlocks = {
    {"minecraft:oak_leaves", 72, 111, 49},
    {"minecraft:jungle_leaves", 48, 103, 43},
    {"minecraft:dark_oak_leaves", 41, 81, 33},
    {"minecraft:birch_leaves", 91, 132, 49},
    {"minecraft:acacia_leaves", 68, 104, 43},
    {"minecraft:spruce_leaves", 39, 72, 55},
    {"minecraft:mangrove_leaves", 61, 96, 42},
    {"minecraft:azalea_leaves", 75, 113, 54},
    {"minecraft:flowering_azalea_leaves", 86, 120, 61},
};

std::string ReadTextFile(const fs::path& filePath) {
    std::ifstream in(filePath, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::set<std::string> CsvSet(const std::string& value) {
    std::set<std::string> out;
    std::stringstream stream(value);
    std::string part;
    while (std::getline(stream, part, ',')) {
        part.erase(part.begin(), std::find_if(part.begin(), part.end(), [](unsigned char ch) { return !std::isspace(ch); }));
        part.erase(std::find_if(part.rbegin(), part.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), part.end());
        if (!part.empty()) {
            out.insert(part);
        }
    }
    return out;
}

bool ContainsAny(const std::string& value, const std::vector<std::string>& needles) {
    for (const auto& needle : needles) {
        if (value.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool RegexAny(const std::string& value, const std::vector<std::regex>& patterns) {
    for (const auto& pattern : patterns) {
        if (std::regex_search(value, pattern)) {
            return true;
        }
    }
    return false;
}

bool IsSceneBlockName(const std::string& name) {
    if (ContainsAny(name, kDenySubstrings)) return false;
    if (name.find("glazed_terracotta") != std::string::npos) return false;
    if (name.find("concrete_powder") != std::string::npos) return false;
    if (name.find("suspicious_") != std::string::npos) return false;
    if (name.size() >= 11 && name.rfind("_horizontal") == name.size() - 11) return false;
    if (name.size() >= 12 && name.rfind("_slab_double") == name.size() - 12) return false;
    static const std::vector<std::regex> patterns = {
        std::regex("_concrete$"),
        std::regex("_terracotta$"),
        std::regex("_wool$"),
        std::regex("_stained_glass$"),
        std::regex("^(minecraft:)?glass$"),
        std::regex("^(minecraft:)?tinted_glass$"),
        std::regex("^(minecraft:)?terracotta$"),
        std::regex("^(minecraft:)?bricks$"),
        std::regex("^(minecraft:)?stone$"),
        std::regex("cobblestone$"),
        std::regex("stone_bricks$"),
        std::regex("deepslate_bricks$"),
        std::regex("blackstone"),
        std::regex("basalt$"),
        std::regex("tuff$"),
        std::regex("calcite$"),
        std::regex("dripstone_block$"),
        std::regex("sand$"),
        std::regex("sandstone$"),
        std::regex("gravel$"),
        std::regex("clay$"),
        std::regex("mud"),
        std::regex("dirt$"),
        std::regex("podzol$"),
        std::regex("moss_block$"),
        std::regex("mossy_"),
        std::regex("leaves$"),
        std::regex("_log$"),
        std::regex("_wood$"),
        std::regex("_planks$"),
        std::regex("bamboo_block$"),
        std::regex("bamboo_mosaic$"),
        std::regex("hay_block$"),
        std::regex("melon$"),
        std::regex("mushroom"),
        std::regex("quartz"),
        std::regex("prismarine"),
        std::regex("copper"),
        std::regex("iron_block$"),
        std::regex("raw_iron_block$")
    };
    return RegexAny(name, patterns);
}

std::set<std::string> BlockTypesForName(const std::string& name) {
    std::set<std::string> out;
    if (std::regex_search(name, std::regex("glass"))) out.insert("glass");
    if (std::regex_search(name, std::regex("leaves$"))) out.insert("leaves");
    if (std::regex_search(name, std::regex("_log$|_wood$|_planks$|bamboo|bookshelf|stem$|hyphae$"))) out.insert("wood");
    if (std::regex_search(name, std::regex("stone|cobble|deepslate|blackstone|basalt|tuff|calcite|dripstone|andesite|diorite|granite"))) out.insert("stone");
    if (std::regex_search(name, std::regex("dirt$|grass|mud|sand|sandstone|gravel|clay|moss|podzol|mycelium|netherrack|nylium|soul_"))) out.insert("terrain");
    if (std::regex_search(name, std::regex("brick|terracotta|quartz|prismarine|concrete|wool"))) out.insert("color");
    if (std::regex_search(name, std::regex("copper|iron_block|raw_iron_block|gold_block|raw_copper_block"))) out.insert("metal");
    if (std::regex_search(name, std::regex("mushroom|melon|hay_block|leaf|leaves"))) out.insert("plant");
    if (out.empty()) out.insert("misc");
    return out;
}

bool BlockMatchesTypes(const std::string& name, const std::set<std::string>& includeTypes, const std::set<std::string>& excludeTypes) {
    const std::set<std::string> types = BlockTypesForName(name);
    if (!includeTypes.empty()) {
        bool match = includeTypes.find("all") != includeTypes.end();
        for (const auto& type : types) {
            if (includeTypes.find(type) != includeTypes.end()) {
                match = true;
                break;
            }
        }
        if (!match) {
            return false;
        }
    }
    for (const auto& type : types) {
        if (excludeTypes.find(type) != excludeTypes.end()) {
            return false;
        }
    }
    return true;
}

fs::path FirstExisting(const std::vector<fs::path>& candidates) {
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (fs::exists(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}

fs::path OtsResource(const fs::path& projectRoot, const std::vector<std::string>& parts) {
    auto joinParts = [&](const fs::path& root) {
        fs::path out = root;
        for (const auto& part : parts) {
            out /= part;
        }
        return out;
    };
    const std::vector<fs::path> candidates = {
        joinParts(projectRoot / "vendor" / "ots" / "res"),
        joinParts(projectRoot / "ObjToSchematic-ots-1.0-src" / "res")
    };
    return FirstExisting(candidates);
}

struct PaletteSourceData {
    std::vector<std::string> atlasNames;
    std::vector<std::string> colourfulNames;
    std::map<std::string, PaletteBlock> byName;
};

bool LoadPaletteSourceData(const fs::path& projectRoot, PaletteSourceData* outData, std::string* errorText) {
    if (!outData) {
        if (errorText) *errorText = "Palette source output pointer was null";
        return false;
    }

    const fs::path atlasPath = OtsResource(projectRoot, {"atlases", "vanilla.atlas"});
    const fs::path colourfulPath = OtsResource(projectRoot, {"palettes", "colourful.ts"});
    if (atlasPath.empty() || colourfulPath.empty()) {
        if (errorText) *errorText = "Could not find palette resources under vendor/ots/res";
        return false;
    }

    outData->atlasNames.clear();
    outData->colourfulNames.clear();
    outData->byName.clear();

    const json atlas = json::parse(ReadTextFile(atlasPath));
    for (const auto& block : atlas["blocks"]) {
        const std::string name = block["name"].get<std::string>();
        outData->atlasNames.push_back(name);
        outData->byName[name] = PaletteBlock{
            name,
            static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::lround(block["colour"]["r"].get<double>() * 255.0)), 0, 255)),
            static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::lround(block["colour"]["g"].get<double>() * 255.0)), 0, 255)),
            static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::lround(block["colour"]["b"].get<double>() * 255.0)), 0, 255))
        };
    }

    const std::string colourfulText = ReadTextFile(colourfulPath);
    std::regex rx("minecraft:[^']+");
    for (std::sregex_iterator it(colourfulText.begin(), colourfulText.end(), rx), end; it != end; ++it) {
        outData->colourfulNames.push_back(it->str());
    }

    for (const auto& block : kSceneExtraBlocks) {
        if (outData->byName.find(block.name) == outData->byName.end()) {
            outData->byName[block.name] = PaletteBlock{
                block.name,
                static_cast<std::uint8_t>(block.r),
                static_cast<std::uint8_t>(block.g),
                static_cast<std::uint8_t>(block.b)
            };
        }
    }
    return true;
}

std::vector<std::string> BuildModeNames(const PaletteSourceData& source, const std::string& mode) {
    std::vector<std::string> names;
    if (mode == "concrete") {
        names = source.colourfulNames;
        names.erase(std::remove_if(names.begin(), names.end(), [](const std::string& name) {
            return !std::regex_search(name, std::regex("_concrete$"));
        }), names.end());
    } else if (mode == "scene") {
        for (const auto& name : source.atlasNames) {
            if (IsSceneBlockName(name)) {
                names.push_back(name);
            }
        }
    } else if (mode == "colourful") {
        names = source.colourfulNames;
        names.erase(std::remove_if(names.begin(), names.end(), [](const std::string& name) {
            return name.find("concrete_powder") != std::string::npos || name.find("glazed_terracotta") != std::string::npos;
        }), names.end());
    } else {
        names = source.colourfulNames;
        names.erase(std::remove_if(names.begin(), names.end(), [](const std::string& name) {
            return !std::regex_search(name, std::regex("_concrete$|_terracotta$")) || name.find("glazed") != std::string::npos;
        }), names.end());
    }
    names.erase(std::remove_if(names.begin(), names.end(), [](const std::string& name) {
        return ContainsAny(name, kDenySubstrings);
    }), names.end());
    return names;
}

std::vector<PaletteBlock> BuildPaletteBlockList(const PaletteSourceData& source, const PaletteOptions& options) {
    const std::string mode = options.mode.empty() ? "scene" : options.mode;
    const std::vector<std::string> names = BuildModeNames(source, mode);

    std::set<std::string> includeTypes = CsvSet(options.blockTypes);
    std::set<std::string> includeBlocks = CsvSet(options.blocks);
    std::set<std::string> excludeTypes = CsvSet(options.excludeBlockTypes);
    std::set<std::string> excludeBlocks = CsvSet(options.excludeBlocks);
    if (options.noGlass) {
        excludeTypes.insert("glass");
    }

    std::vector<PaletteBlock> filtered;
    std::set<std::string> seen;
    auto addBlockIfAllowed = [&](const std::string& name) {
        if (seen.find(name) != seen.end()) return;
        auto it = source.byName.find(name);
        if (it == source.byName.end()) return;
        if (!BlockMatchesTypes(name, includeTypes, excludeTypes)) return;
        if (excludeBlocks.find(name) != excludeBlocks.end()) return;
        filtered.push_back(it->second);
        seen.insert(name);
    };

    for (const auto& name : names) {
        addBlockIfAllowed(name);
    }

    for (const auto& name : includeBlocks) {
        addBlockIfAllowed(name);
    }

    if (mode == "scene" && excludeTypes.find("leaves") == excludeTypes.end()) {
        for (const auto& block : kSceneExtraBlocks) {
            addBlockIfAllowed(block.name);
        }
    }

    return filtered;
}

}  // namespace

Palette::Palette(std::vector<PaletteBlock> blocks)
    : blocks_(std::move(blocks)), cache_(32768) {}

const std::vector<PaletteBlock>& Palette::blocks() const {
    return blocks_;
}

std::string Palette::Nearest(int r, int g, int b) const {
    if (blocks_.empty()) {
        return "minecraft:air";
    }
    r = std::clamp(r, 0, 255);
    g = std::clamp(g, 0, 255);
    b = std::clamp(b, 0, 255);
    const int key = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
    if (key >= 0 && key < static_cast<int>(cache_.size()) && cache_[key].has_value()) {
        return *cache_[key];
    }
    const PaletteBlock* best = &blocks_.front();
    int bestErr = std::numeric_limits<int>::max();
    for (const auto& block : blocks_) {
        const int dr = r - block.r;
        const int dg = g - block.g;
        const int db = b - block.b;
        const int err = dr * dr + dg * dg + db * db;
        if (err < bestErr) {
            bestErr = err;
            best = &block;
        }
    }
    if (key >= 0 && key < static_cast<int>(cache_.size())) {
        cache_[key] = best->name;
    }
    return best->name;
}

bool LoadPalette(const fs::path& projectRoot, const PaletteOptions& options, Palette* outPalette, std::string* errorText) {
    PaletteCatalog catalog;
    if (!LoadPaletteCatalog(projectRoot, options, &catalog, errorText)) {
        return false;
    }
    if (!outPalette) {
        if (errorText) *errorText = "Palette output pointer was null";
        return false;
    }
    if (catalog.includedBlocks.empty()) {
        if (errorText) *errorText = "No palette blocks were loaded after filtering";
        return false;
    }
    *outPalette = Palette(std::move(catalog.includedBlocks));
    return true;
}

bool LoadPaletteCatalog(const fs::path& projectRoot, const PaletteOptions& options, PaletteCatalog* outCatalog, std::string* errorText) {
    if (!outCatalog) {
        if (errorText) *errorText = "Palette catalog output pointer was null";
        return false;
    }

    PaletteSourceData source;
    if (!LoadPaletteSourceData(projectRoot, &source, errorText)) {
        return false;
    }

    std::vector<PaletteBlock> availableBlocks;
    availableBlocks.reserve(source.byName.size());
    for (const auto& [name, block] : source.byName) {
        (void)name;
        availableBlocks.push_back(block);
    }
    std::sort(availableBlocks.begin(), availableBlocks.end(), [](const PaletteBlock& a, const PaletteBlock& b) {
        return a.name < b.name;
    });

    PaletteOptions baseOptions = options;
    baseOptions.blocks.clear();
    baseOptions.excludeBlocks.clear();

    outCatalog->availableBlocks = std::move(availableBlocks);
    outCatalog->baseBlocks = BuildPaletteBlockList(source, baseOptions);
    outCatalog->includedBlocks = BuildPaletteBlockList(source, options);
    return true;
}

}  // namespace native_mc
