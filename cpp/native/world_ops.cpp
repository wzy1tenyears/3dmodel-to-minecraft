#include "world_ops.h"

#include "nbt.h"

#include "../third_party/miniz.h"

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace fs = std::filesystem;

namespace native_mc {

namespace {

constexpr int kSectionMin = kMinY / 16;

std::vector<std::uint8_t> ReadFileBytes(const fs::path& filePath) {
    std::ifstream in(filePath, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool WriteFileBytesAtomic(const fs::path& filePath, const std::vector<std::uint8_t>& bytes,
                          const fs::path& backupPath, std::string* errorText) {
    std::error_code ec;
    fs::create_directories(filePath.parent_path(), ec);
    if (ec) {
        if (errorText) *errorText = "Could not create output directory";
        return false;
    }
    const fs::path tempPath = fs::path(filePath.wstring() + L".cpp-tmp-" + std::to_wstring(GetCurrentProcessId()));
    HANDLE handle = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        if (errorText) *errorText = "Could not create temporary region file";
        return false;
    }
    std::size_t offset = 0;
    bool ok = true;
    while (offset < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 16u * 1024u * 1024u));
        DWORD written = 0;
        if (!WriteFile(handle, bytes.data() + offset, chunk, &written, nullptr) || written != chunk) {
            ok = false;
            break;
        }
        offset += written;
    }
    if (ok) ok = FlushFileBuffers(handle) != FALSE;
    CloseHandle(handle);
    if (!ok || fs::file_size(tempPath, ec) != bytes.size()) {
        fs::remove(tempPath, ec);
        if (errorText) *errorText = "Failed while flushing temporary region file";
        return false;
    }
    if (fs::exists(filePath, ec) && !backupPath.empty()) {
        fs::create_directories(backupPath.parent_path(), ec);
        if (ec || !fs::copy_file(filePath, backupPath, fs::copy_options::overwrite_existing, ec)) {
            fs::remove(tempPath, ec);
            if (errorText) *errorText = "Could not back up the original region file";
            return false;
        }
    }
    if (!MoveFileExW(tempPath.c_str(), filePath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        fs::remove(tempPath, ec);
        if (errorText) *errorText = "Could not atomically replace the region file";
        return false;
    }
    return true;
}

std::vector<std::uint8_t> EmptyRegionBuffer() {
    return std::vector<std::uint8_t>(8192, 0);
}

int BitsForPalette(std::size_t size) {
    int bits = 4;
    while ((1u << bits) < size) {
        ++bits;
    }
    return bits;
}

struct ChunkRecord {
    std::vector<std::uint8_t> payload;
    std::uint32_t sector = 0;
    std::uint8_t sectorCount = 0;
};

bool ReadChunkRecord(const std::vector<std::uint8_t>& regionBuffer, int index, ChunkRecord* outRecord, std::string* errorText) {
    if (regionBuffer.size() < 8192 || index < 0 || index >= 1024 || !outRecord) {
        if (errorText) *errorText = "Invalid region buffer or chunk index";
        return false;
    }
    const std::size_t headerOffset = static_cast<std::size_t>(index) * 4;
    const std::uint32_t sector =
        (static_cast<std::uint32_t>(regionBuffer[headerOffset]) << 16) |
        (static_cast<std::uint32_t>(regionBuffer[headerOffset + 1]) << 8) |
        static_cast<std::uint32_t>(regionBuffer[headerOffset + 2]);
    const std::uint8_t sectors = regionBuffer[headerOffset + 3];
    outRecord->sector = sector;
    outRecord->sectorCount = sectors;
    outRecord->payload.clear();
    if (!sector || !sectors) {
        return true;
    }
    const std::size_t pos = static_cast<std::size_t>(sector) * 4096;
    if (pos + 5 > regionBuffer.size()) {
        if (errorText) *errorText = "Chunk header points outside the region file";
        return false;
    }
    const std::uint32_t chunkLength =
        (static_cast<std::uint32_t>(regionBuffer[pos]) << 24) |
        (static_cast<std::uint32_t>(regionBuffer[pos + 1]) << 16) |
        (static_cast<std::uint32_t>(regionBuffer[pos + 2]) << 8) |
        static_cast<std::uint32_t>(regionBuffer[pos + 3]);
    if (chunkLength == 0 || pos + 4 + chunkLength > regionBuffer.size()) {
        if (errorText) *errorText = "Chunk length is invalid";
        return false;
    }
    const std::uint8_t compression = regionBuffer[pos + 4];
    const std::uint8_t* compressed = regionBuffer.data() + pos + 5;
    const std::size_t compressedSize = chunkLength - 1;
    if (compression == 3) {
        outRecord->payload.assign(compressed, compressed + compressedSize);
        return true;
    }
    if (compression != 2) {
        if (errorText) *errorText = "Unsupported chunk compression type";
        return false;
    }

    mz_stream stream{};
    stream.next_in = const_cast<mz_uint8*>(compressed);
    stream.avail_in = static_cast<mz_uint32>(compressedSize);
    if (mz_inflateInit(&stream) != MZ_OK) {
        if (errorText) *errorText = "inflateInit failed";
        return false;
    }

    std::vector<std::uint8_t> output(65536);
    int status = MZ_OK;
    while (status == MZ_OK) {
        if (stream.total_out >= output.size()) {
            output.resize(output.size() * 2);
        }
        stream.next_out = output.data() + stream.total_out;
        stream.avail_out = static_cast<mz_uint32>(output.size() - stream.total_out);
        status = mz_inflate(&stream, MZ_NO_FLUSH);
    }
    if (status != MZ_STREAM_END) {
        mz_inflateEnd(&stream);
        if (errorText) *errorText = "inflate failed";
        return false;
    }
    output.resize(stream.total_out);
    mz_inflateEnd(&stream);
    outRecord->payload = std::move(output);
    return true;
}

bool CompressZlib(const std::vector<std::uint8_t>& input, std::vector<std::uint8_t>* output, std::string* errorText) {
    mz_ulong outSize = mz_compressBound(static_cast<mz_ulong>(input.size()));
    output->assign(outSize, 0);
    const int result = mz_compress2(output->data(), &outSize, input.data(), static_cast<mz_ulong>(input.size()), 6);
    if (result != MZ_OK) {
        if (errorText) *errorText = "zlib compression failed";
        return false;
    }
    output->resize(outSize);
    return true;
}

Tag MakePalette(const std::vector<std::string>& blockNames) {
    std::vector<Tag> values;
    values.reserve(blockNames.size());
    for (const auto& name : blockNames) {
        values.push_back(Tag::Compound({ {"Name", Tag::String(name)} }));
    }
    return Tag::List(TagType::Compound, std::move(values));
}

Tag MakeLongArray(const std::vector<std::int64_t>& values) {
    return Tag::LongArray(values);
}

std::vector<std::int64_t> PackBlockStates(const std::vector<std::int32_t>& indices, std::size_t paletteSize) {
    const int bits = BitsForPalette(paletteSize);
    const int valuesPerLong = 64 / bits;
    const int longCount = static_cast<int>((4096 + valuesPerLong - 1) / valuesPerLong);
    std::vector<std::int64_t> longs(longCount, 0);
    const std::uint64_t mask = bits == 64 ? ~0ull : ((1ull << bits) - 1ull);
    for (int i = 0; i < static_cast<int>(indices.size()); ++i) {
        const std::uint64_t value = static_cast<std::uint64_t>(indices[i]) & mask;
        const int longIndex = i / valuesPerLong;
        const int bitOffset = (i % valuesPerLong) * bits;
        std::uint64_t current = static_cast<std::uint64_t>(longs[longIndex]);
        current |= value << bitOffset;
        longs[longIndex] = static_cast<std::int64_t>(current);
    }
    return longs;
}

struct DecodedSection {
    std::vector<std::int32_t> indices;
    std::vector<std::string> paletteNames;
};

DecodedSection DecodeSectionIndices(Tag* section) {
    DecodedSection decoded;
    decoded.indices.assign(4096, 0);
    if (!section || section->type != TagType::Compound) {
        return decoded;
    }
    Tag* blockStates = section->Find("block_states");
    if (!blockStates || blockStates->type != TagType::Compound) {
        return decoded;
    }
    Tag* palette = blockStates->Find("palette");
    if (!palette || palette->type != TagType::List) {
        return decoded;
    }
    for (const Tag& entry : palette->listValue) {
        const Tag* name = entry.Find("Name");
        decoded.paletteNames.push_back(name ? name->stringValue : "minecraft:air");
    }
    if (decoded.paletteNames.empty()) {
        decoded.paletteNames.push_back("minecraft:air");
    }
    Tag* data = blockStates->Find("data");
    if (!data || data->type != TagType::LongArray) {
        return decoded;
    }

    const int bits = BitsForPalette(decoded.paletteNames.size());
    const int valuesPerLong = 64 / bits;
    const std::uint64_t mask = (1ull << bits) - 1ull;
    for (std::size_t longIndex = 0; longIndex < data->longArrayValue.size(); ++longIndex) {
        const std::uint64_t raw = static_cast<std::uint64_t>(data->longArrayValue[longIndex]);
        for (int j = 0; j < valuesPerLong; ++j) {
            const int blockIndex = static_cast<int>(longIndex) * valuesPerLong + j;
            if (blockIndex >= 4096) {
                break;
            }
            decoded.indices[blockIndex] = static_cast<std::int32_t>((raw >> (j * bits)) & mask);
        }
    }
    return decoded;
}

Tag MakeAirSection(int sectionY) {
    return Tag::Compound({
        {"Y", Tag::Byte(static_cast<std::int8_t>(sectionY))},
        {"biomes", Tag::Compound({
            {"palette", Tag::List(TagType::String, { Tag::String("minecraft:plains") })}
        })},
        {"block_states", Tag::Compound({
            {"palette", MakePalette({ "minecraft:air" })}
        })}
    });
}

Tag MakeSuperflatBaseSection(int sectionY) {
    std::vector<std::int32_t> indices(4096, 0);
    const std::vector<std::string> paletteNames = {
        "minecraft:air",
        "minecraft:bedrock",
        "minecraft:dirt",
        "minecraft:grass_block",
    };
    if (sectionY == kSectionMin) {
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                indices[z * 16 + x] = 1;
                indices[256 + z * 16 + x] = 2;
                indices[512 + z * 16 + x] = 2;
                indices[768 + z * 16 + x] = 3;
            }
        }
    }
    Tag section = MakeAirSection(sectionY);
    Tag* blockStates = section.Find("block_states");
    blockStates->compoundValue["palette"] = MakePalette(paletteNames);
    blockStates->compoundValue["data"] = MakeLongArray(PackBlockStates(indices, paletteNames.size()));
    return section;
}

NamedTag MakeEmptyChunk(int cx, int cz) {
    return NamedTag{
        "",
        Tag::Compound({
            {"xPos", Tag::Int(cx)},
            {"yPos", Tag::Int(kSectionMin)},
            {"zPos", Tag::Int(cz)},
            {"Status", Tag::String("minecraft:full")},
            {"DataVersion", Tag::Int(kDataVersion)},
            {"LastUpdate", Tag::Long(0)},
            {"InhabitedTime", Tag::Long(0)},
            {"isLightOn", Tag::Byte(0)},
            {"block_ticks", Tag::List(TagType::Compound, {})},
            {"fluid_ticks", Tag::List(TagType::Compound, {})},
            {"block_entities", Tag::List(TagType::Compound, {})},
            {"PostProcessing", Tag::List(TagType::List, {})},
            {"structures", Tag::Compound({
                {"References", Tag::Compound()},
                {"starts", Tag::Compound()},
            })},
            {"Heightmaps", Tag::Compound()},
            {"sections", Tag::List(TagType::Compound, {})},
        })
    };
}

NamedTag MakeSuperflatChunk(int cx, int cz) {
    NamedTag chunk = MakeEmptyChunk(cx, cz);
    Tag* sections = chunk.root.Find("sections");
    sections->listValue.push_back(MakeSuperflatBaseSection(kSectionMin));
    return chunk;
}

Tag* EnsureSection(Tag* chunkRoot, int sectionY) {
    if (!chunkRoot || chunkRoot->type != TagType::Compound) {
        return nullptr;
    }
    Tag* sections = chunkRoot->Find("sections");
    if (!sections || sections->type != TagType::List) {
        chunkRoot->compoundValue["sections"] = Tag::List(TagType::Compound, {});
        sections = chunkRoot->Find("sections");
    }
    for (Tag& section : sections->listValue) {
        Tag* y = section.Find("Y");
        if (y && y->byteValue == static_cast<std::int8_t>(sectionY)) {
            return &section;
        }
    }
    sections->listValue.push_back(MakeAirSection(sectionY));
    std::sort(sections->listValue.begin(), sections->listValue.end(), [](const Tag& a, const Tag& b) {
        const Tag* ay = a.Find("Y");
        const Tag* by = b.Find("Y");
        return (ay ? ay->byteValue : 0) < (by ? by->byteValue : 0);
    });
    for (Tag& section : sections->listValue) {
        Tag* y = section.Find("Y");
        if (y && y->byteValue == static_cast<std::int8_t>(sectionY)) {
            return &section;
        }
    }
    return nullptr;
}

void ApplyDecodedSection(Tag* section, const DecodedSection& decoded) {
    if (!section) {
        return;
    }
    Tag* blockStates = section->Find("block_states");
    if (!blockStates) {
        section->compoundValue["block_states"] = Tag::Compound();
        blockStates = section->Find("block_states");
    }
    blockStates->compoundValue["palette"] = MakePalette(decoded.paletteNames);
    if (decoded.paletteNames.size() <= 1) {
        blockStates->compoundValue.erase("data");
    } else {
        blockStates->compoundValue["data"] = MakeLongArray(PackBlockStates(decoded.indices, decoded.paletteNames.size()));
    }
}

std::size_t SetBlocks(Tag* chunkRoot, const PackedBlockMap& blockMap, bool overwriteExisting) {
    std::map<int, std::vector<std::tuple<int, int, int, std::string>>> bySection;
    for (const auto& [packed, blockName] : blockMap) {
        const int localX = packed & 15;
        const int localZ = (packed >> 4) & 15;
        const int y = static_cast<int>(packed >> 8) + kMinY;
        if (y < kMinY || y >= kMinY + kHeight) {
            continue;
        }
        const int sectionY = y / 16;
        const int localY = ((y % 16) + 16) % 16;
        bySection[sectionY].push_back({ localX, localY, localZ, blockName });
    }

    std::size_t written = 0;
    for (auto& [sectionY, blocks] : bySection) {
        Tag* section = EnsureSection(chunkRoot, sectionY);
        DecodedSection decoded = DecodeSectionIndices(section);
        std::map<std::string, std::int32_t> paletteIndex;
        for (std::size_t i = 0; i < decoded.paletteNames.size(); ++i) {
            paletteIndex.emplace(decoded.paletteNames[i], static_cast<std::int32_t>(i));
        }
        for (const auto& [localX, localY, localZ, blockName] : blocks) {
            const std::size_t blockIndex = static_cast<std::size_t>(localY * 256 + localZ * 16 + localX);
            const std::int32_t existingIndex = decoded.indices[blockIndex];
            if (!overwriteExisting && existingIndex >= 0 && existingIndex < static_cast<std::int32_t>(decoded.paletteNames.size())) {
                const std::string& existing = decoded.paletteNames[existingIndex];
                if (existing != "minecraft:air" && existing != "minecraft:cave_air" && existing != "minecraft:void_air") continue;
            }
            auto it = paletteIndex.find(blockName);
            if (it == paletteIndex.end()) {
                decoded.paletteNames.push_back(blockName);
                const std::int32_t newIndex = static_cast<std::int32_t>(decoded.paletteNames.size() - 1);
                paletteIndex.emplace(blockName, newIndex);
                it = paletteIndex.find(blockName);
            }
            decoded.indices[blockIndex] = it->second;
            ++written;
        }
        ApplyDecodedSection(section, decoded);
    }
    return written;
}

bool LoadChunk(const std::vector<std::uint8_t>& regionBuffer, int index, int cx, int cz, bool superflat, NamedTag* outChunk, std::string* errorText) {
    ChunkRecord record;
    if (!ReadChunkRecord(regionBuffer, index, &record, errorText)) {
        return false;
    }
    if (record.payload.empty()) {
        *outChunk = superflat ? MakeSuperflatChunk(cx, cz) : MakeEmptyChunk(cx, cz);
        return true;
    }
    if (!ReadNamedTag(record.payload, outChunk, errorText)) {
        return false;
    }
    return true;
}

bool WriteChunkToBuffer(std::vector<std::uint8_t>* regionBuffer, int index, const NamedTag& chunkNbt, std::string* errorText) {
    std::vector<std::uint8_t> raw;
    if (!WriteNamedTag(chunkNbt, &raw, errorText)) {
        return false;
    }
    std::vector<std::uint8_t> compressed;
    if (!CompressZlib(raw, &compressed, errorText)) {
        return false;
    }
    const std::uint32_t chunkLength = static_cast<std::uint32_t>(compressed.size() + 1);
    const std::uint8_t sectorsNeeded = static_cast<std::uint8_t>((chunkLength + 4 + 4095) / 4096);
    const std::size_t headerOffset = static_cast<std::size_t>(index) * 4;
    const std::uint32_t oldSector =
        (static_cast<std::uint32_t>((*regionBuffer)[headerOffset]) << 16) |
        (static_cast<std::uint32_t>((*regionBuffer)[headerOffset + 1]) << 8) |
        static_cast<std::uint32_t>((*regionBuffer)[headerOffset + 2]);
    const std::uint8_t oldSectors = (*regionBuffer)[headerOffset + 3];

    std::uint32_t sector = oldSector;
    if (!oldSector || oldSectors < sectorsNeeded) {
        sector = static_cast<std::uint32_t>((*regionBuffer).size() / 4096);
        (*regionBuffer).resize((sector + sectorsNeeded) * 4096, 0);
    }

    const std::size_t pos = static_cast<std::size_t>(sector) * 4096;
    (*regionBuffer)[pos] = static_cast<std::uint8_t>((chunkLength >> 24) & 0xff);
    (*regionBuffer)[pos + 1] = static_cast<std::uint8_t>((chunkLength >> 16) & 0xff);
    (*regionBuffer)[pos + 2] = static_cast<std::uint8_t>((chunkLength >> 8) & 0xff);
    (*regionBuffer)[pos + 3] = static_cast<std::uint8_t>(chunkLength & 0xff);
    (*regionBuffer)[pos + 4] = 2;
    std::copy(compressed.begin(), compressed.end(), regionBuffer->begin() + static_cast<std::ptrdiff_t>(pos + 5));
    std::fill(regionBuffer->begin() + static_cast<std::ptrdiff_t>(pos + 5 + compressed.size()),
        regionBuffer->begin() + static_cast<std::ptrdiff_t>(pos + sectorsNeeded * 4096), 0);

    (*regionBuffer)[headerOffset] = static_cast<std::uint8_t>((sector >> 16) & 0xff);
    (*regionBuffer)[headerOffset + 1] = static_cast<std::uint8_t>((sector >> 8) & 0xff);
    (*regionBuffer)[headerOffset + 2] = static_cast<std::uint8_t>(sector & 0xff);
    (*regionBuffer)[headerOffset + 3] = sectorsNeeded;

    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    const std::size_t tsOffset = 4096 + static_cast<std::size_t>(index) * 4;
    (*regionBuffer)[tsOffset] = static_cast<std::uint8_t>((now >> 24) & 0xff);
    (*regionBuffer)[tsOffset + 1] = static_cast<std::uint8_t>((now >> 16) & 0xff);
    (*regionBuffer)[tsOffset + 2] = static_cast<std::uint8_t>((now >> 8) & 0xff);
    (*regionBuffer)[tsOffset + 3] = static_cast<std::uint8_t>(now & 0xff);
    return true;
}

bool AssertInside(const fs::path& parent, const fs::path& child, std::string* errorText) {
    std::error_code ec;
    const fs::path rel = fs::relative(child, parent, ec);
    if (ec || rel.empty() || rel.string().rfind("..", 0) == 0 || rel.is_absolute()) {
        if (errorText) *errorText = "Refusing to move a path outside the world folder";
        return false;
    }
    return true;
}

std::string TimeStampForBackup() {
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm localTm{};
    localtime_s(&localTm, &tt);
    std::ostringstream out;
    out << (1900 + localTm.tm_year)
        << (localTm.tm_mon + 1 < 10 ? "0" : "") << (localTm.tm_mon + 1)
        << (localTm.tm_mday < 10 ? "0" : "") << localTm.tm_mday
        << "_"
        << (localTm.tm_hour < 10 ? "0" : "") << localTm.tm_hour
        << (localTm.tm_min < 10 ? "0" : "") << localTm.tm_min
        << (localTm.tm_sec < 10 ? "0" : "") << localTm.tm_sec;
    return out.str();
}

bool MoveContents(const fs::path& srcDir, const fs::path& backupRoot, const std::string& label, std::string* errorText) {
    std::error_code ec;
    if (!fs::exists(srcDir, ec)) {
        return true;
    }
    const fs::path destDir = backupRoot / label;
    fs::create_directories(destDir, ec);
    if (ec) {
        if (errorText) *errorText = "Could not create backup directory";
        return false;
    }
    for (const auto& entry : fs::directory_iterator(srcDir, ec)) {
        if (ec) {
            if (errorText) *errorText = "Could not enumerate world contents";
            return false;
        }
        if (!AssertInside(srcDir, entry.path(), errorText)) {
            return false;
        }
        fs::rename(entry.path(), destDir / entry.path().filename(), ec);
        if (ec) {
            if (errorText) *errorText = "Could not move world data into backup";
            return false;
        }
    }
    return true;
}

}  // namespace

std::filesystem::path RegionPathForChunk(const fs::path& regionDir, int cx, int cz) {
    const int rx = static_cast<int>(std::floor(static_cast<double>(cx) / 32.0));
    const int rz = static_cast<int>(std::floor(static_cast<double>(cz) / 32.0));
    return regionDir / ("r." + std::to_string(rx) + "." + std::to_string(rz) + ".mca");
}

std::filesystem::path ResolveOverworldRegionDir(const fs::path& worldDir) {
    const fs::path standard = worldDir / "region";
    const fs::path legacy = worldDir / "dimensions" / "minecraft" / "overworld" / "region";
    std::error_code ec;
    if (fs::is_directory(standard, ec)) return standard;
    ec.clear();
    if (fs::is_directory(legacy, ec)) return legacy;
    return standard;
}

int LocalChunkIndex(int cx, int cz) {
    const int lx = ((cx % 32) + 32) % 32;
    const int lz = ((cz % 32) + 32) % 32;
    return lx + lz * 32;
}

bool ResetWorldToBlankSuperflat(const fs::path& worldDir, fs::path* backupRoot, std::string* errorText) {
    if (!fs::exists(worldDir / "level.dat")) {
        if (errorText) *errorText = "The world directory is missing level.dat";
        return false;
    }
    const fs::path regionDir = ResolveOverworldRegionDir(worldDir);
    const fs::path overworld = regionDir.parent_path();
    const fs::path backup = worldDir / "cpp-world-backups" / ("before-native-reset-" + TimeStampForBackup());
    std::error_code ec;
    fs::create_directories(backup, ec);
    if (ec) {
        if (errorText) *errorText = "Could not create the backup root";
        return false;
    }
    if (!MoveContents(regionDir, backup, "region", errorText) ||
        !MoveContents(overworld / "poi", backup, "poi", errorText) ||
        !MoveContents(overworld / "entities", backup, "entities", errorText)) {
        return false;
    }
    fs::create_directories(regionDir, ec);
    fs::create_directories(overworld / "poi", ec);
    fs::create_directories(overworld / "entities", ec);
    if (backupRoot) {
        *backupRoot = backup;
    }
    return true;
}

bool ReadWorldDataVersion(const fs::path& worldDir, int* dataVersion, std::string* errorText) {
    if (!dataVersion) {
        if (errorText) *errorText = "DataVersion output pointer was null";
        return false;
    }
    const auto bytes = ReadFileBytes(worldDir / "level.dat");
    if (bytes.size() < 18 || bytes[0] != 0x1f || bytes[1] != 0x8b || bytes[2] != 8) {
        if (errorText) *errorText = "level.dat is not a supported gzip NBT file";
        return false;
    }
    std::size_t offset = 10;
    const std::uint8_t flags = bytes[3];
    if (flags & 0x04) {
        if (offset + 2 > bytes.size()) return false;
        const std::size_t extra = bytes[offset] | (static_cast<std::size_t>(bytes[offset + 1]) << 8);
        offset += 2 + extra;
    }
    auto skipCString = [&]() {
        while (offset < bytes.size() && bytes[offset++] != 0) {}
    };
    if (flags & 0x08) skipCString();
    if (flags & 0x10) skipCString();
    if (flags & 0x02) offset += 2;
    if (offset + 8 >= bytes.size()) {
        if (errorText) *errorText = "level.dat gzip header is truncated";
        return false;
    }
    std::size_t rawSize = 0;
    void* raw = tinfl_decompress_mem_to_heap(bytes.data() + offset, bytes.size() - offset - 8, &rawSize, 0);
    if (!raw || rawSize == 0) {
        if (raw) mz_free(raw);
        if (errorText) *errorText = "Could not decompress level.dat";
        return false;
    }
    std::vector<std::uint8_t> nbtBytes(static_cast<std::uint8_t*>(raw), static_cast<std::uint8_t*>(raw) + rawSize);
    mz_free(raw);
    NamedTag level;
    if (!ReadNamedTag(nbtBytes, &level, errorText)) return false;
    const Tag* data = level.root.Find("Data");
    if (!data || data->type != TagType::Compound) data = &level.root;
    const Tag* version = data->Find("DataVersion");
    if (!version || version->type != TagType::Int) {
        if (errorText) *errorText = "level.dat does not contain an integer DataVersion";
        return false;
    }
    *dataVersion = version->intValue;
    return true;
}

bool WriteManyChunkBlockMap(const fs::path& regionDir, const std::map<ChunkCoord, PackedBlockMap>& chunkMap,
                            bool superflat, bool overwriteExisting, std::size_t* written,
                            std::size_t* regionCount, std::string* errorText) {
    std::error_code ec;
    fs::create_directories(regionDir, ec);
    if (ec) {
        if (errorText) *errorText = "Could not create the region directory";
        return false;
    }

    std::map<fs::path, std::vector<std::pair<ChunkCoord, PackedBlockMap>>> byRegion;
    for (const auto& [coord, blocks] : chunkMap) {
        byRegion[RegionPathForChunk(regionDir, coord.x, coord.z)].push_back({ coord, blocks });
    }

    std::size_t totalWritten = 0;
    std::size_t totalRegions = 0;
    const fs::path backupRoot = regionDir.parent_path() / "cpp-region-backups" / TimeStampForBackup();
    for (auto& [regionPath, chunkList] : byRegion) {
        std::vector<std::uint8_t> regionBuffer = fs::exists(regionPath, ec) ? ReadFileBytes(regionPath) : EmptyRegionBuffer();
        if (regionBuffer.size() < 8192) {
            regionBuffer = EmptyRegionBuffer();
        }
        for (auto& [coord, blocks] : chunkList) {
            const int index = LocalChunkIndex(coord.x, coord.z);
            NamedTag chunk;
            if (!LoadChunk(regionBuffer, index, coord.x, coord.z, superflat, &chunk, errorText)) {
                return false;
            }
            chunk.root.compoundValue["xPos"] = Tag::Int(coord.x);
            chunk.root.compoundValue["zPos"] = Tag::Int(coord.z);
            chunk.root.compoundValue["yPos"] = Tag::Int(kSectionMin);
            chunk.root.compoundValue["DataVersion"] = Tag::Int(kDataVersion);
            totalWritten += SetBlocks(&chunk.root, blocks, overwriteExisting);
            if (!WriteChunkToBuffer(&regionBuffer, index, chunk, errorText)) {
                return false;
            }
        }
        if (!WriteFileBytesAtomic(regionPath, regionBuffer, backupRoot / regionPath.filename(), errorText)) {
            return false;
        }
        ++totalRegions;
    }

    if (written) {
        *written = totalWritten;
    }
    if (regionCount) {
        *regionCount = totalRegions;
    }
    return true;
}

bool ScanWorldBlocks(const fs::path& worldDir, std::optional<int> minY, std::optional<int> maxY, ScanSummary* summary, std::string* errorText, const std::set<std::string>* allowedBlocks) {
    if (!summary) {
        if (errorText) *errorText = "Scan summary output pointer was null";
        return false;
    }
    summary->byBlock.clear();
    summary->totalBlocks = 0;
    summary->regions = 0;
    summary->chunks = 0;
    summary->hasBounds = false;

    const fs::path regionDir = ResolveOverworldRegionDir(worldDir);
    if (!fs::exists(regionDir)) {
        if (errorText) *errorText = "The world has no overworld region directory";
        return false;
    }

    std::vector<fs::path> regionFiles;
    for (const auto& entry : fs::directory_iterator(regionDir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".mca") regionFiles.push_back(entry.path());
    }
    std::sort(regionFiles.begin(), regionFiles.end());

    std::atomic<std::size_t> nextRegion{ 0 };
    std::atomic<bool> failed{ false };
    std::mutex mergeMutex;
    std::string firstError;
    auto mergeSummary = [&](const ScanSummary& local) {
        std::lock_guard<std::mutex> lock(mergeMutex);
        summary->regions += local.regions;
        summary->chunks += local.chunks;
        summary->totalBlocks += local.totalBlocks;
        for (const auto& [name, count] : local.byBlock) summary->byBlock[name] += count;
        if (!local.hasBounds) return;
        if (!summary->hasBounds) {
            summary->hasBounds = true;
            summary->minX = local.minX; summary->maxX = local.maxX;
            summary->minY = local.minY; summary->maxY = local.maxY;
            summary->minZ = local.minZ; summary->maxZ = local.maxZ;
        } else {
            summary->minX = std::min(summary->minX, local.minX); summary->maxX = std::max(summary->maxX, local.maxX);
            summary->minY = std::min(summary->minY, local.minY); summary->maxY = std::max(summary->maxY, local.maxY);
            summary->minZ = std::min(summary->minZ, local.minZ); summary->maxZ = std::max(summary->maxZ, local.maxZ);
        }
    };
    auto fail = [&](const std::string& message) {
        std::lock_guard<std::mutex> lock(mergeMutex);
        if (firstError.empty()) firstError = message;
        failed = true;
    };
    auto worker = [&]() {
        while (!failed) {
            const std::size_t fileIndex = nextRegion.fetch_add(1);
            if (fileIndex >= regionFiles.size()) break;
            const fs::path& filePath = regionFiles[fileIndex];
            int rx = 0;
            int rz = 0;
            if (std::sscanf(filePath.filename().string().c_str(), "r.%d.%d.mca", &rx, &rz) != 2) continue;
            ScanSummary local;
            local.regions = 1;
            const std::vector<std::uint8_t> regionBuffer = ReadFileBytes(filePath);
            if (regionBuffer.size() < 8192) {
                mergeSummary(local);
                continue;
            }
            for (int index = 0; index < 1024 && !failed; ++index) {
                ChunkRecord record;
                std::string localError;
                if (!ReadChunkRecord(regionBuffer, index, &record, &localError)) {
                    fail(localError);
                    break;
                }
                if (record.payload.empty()) continue;
                NamedTag chunk;
                if (!ReadNamedTag(record.payload, &chunk, &localError)) {
                    fail(localError);
                    break;
                }
                ++local.chunks;
                Tag* sections = chunk.root.Find("sections");
                if (!sections || sections->type != TagType::List) continue;
                const int cx = rx * 32 + index % 32;
                const int cz = rz * 32 + index / 32;
                for (Tag& section : sections->listValue) {
                    Tag* yTag = section.Find("Y");
                    if (!yTag) continue;
                    const int sectionY = yTag->byteValue;
                    if (minY && sectionY * 16 + 15 < *minY) continue;
                    if (maxY && sectionY * 16 > *maxY) continue;
                    DecodedSection decoded = DecodeSectionIndices(&section);
                    std::vector<std::uint32_t> paletteCounts(decoded.paletteNames.size(), 0);
                    for (int i = 0; i < 4096; ++i) {
                        const int paletteIndex = decoded.indices[i];
                        if (paletteIndex < 0 || paletteIndex >= static_cast<int>(decoded.paletteNames.size())) continue;
                        const std::string& blockName = decoded.paletteNames[paletteIndex];
                        if (blockName == "minecraft:air") continue;
                        if (allowedBlocks && allowedBlocks->find(blockName) == allowedBlocks->end()) continue;
                        const int localY = i / 256;
                        const int y = sectionY * 16 + localY;
                        if ((minY && y < *minY) || (maxY && y > *maxY)) continue;
                        ++paletteCounts[paletteIndex];
                        const int rem = i % 256;
                        const int x = cx * 16 + rem % 16;
                        const int z = cz * 16 + rem / 16;
                        if (!local.hasBounds) {
                            local.hasBounds = true;
                            local.minX = local.maxX = x; local.minY = local.maxY = y; local.minZ = local.maxZ = z;
                        } else {
                            local.minX = std::min(local.minX, x); local.maxX = std::max(local.maxX, x);
                            local.minY = std::min(local.minY, y); local.maxY = std::max(local.maxY, y);
                            local.minZ = std::min(local.minZ, z); local.maxZ = std::max(local.maxZ, z);
                        }
                    }
                    for (std::size_t i = 0; i < paletteCounts.size(); ++i) {
                        if (!paletteCounts[i]) continue;
                        local.totalBlocks += paletteCounts[i];
                        local.byBlock[decoded.paletteNames[i]] += paletteCounts[i];
                    }
                }
            }
            if (!failed) mergeSummary(local);
        }
    };

    const unsigned int hardware = std::max(1u, std::thread::hardware_concurrency());
    const std::size_t workerCount = std::min<std::size_t>({ regionFiles.size(), 4u, hardware });
    std::vector<std::thread> threads;
    threads.reserve(workerCount);
    for (std::size_t i = 0; i < workerCount; ++i) threads.emplace_back(worker);
    for (auto& thread : threads) thread.join();
    if (failed) {
        if (errorText) *errorText = firstError.empty() ? "World scan failed" : firstError;
        return false;
    }
    return true;
}

bool FindWorldPreviewAnchor(const fs::path& worldDir, int* centerX, int* centerZ, std::string* errorText) {
    if (!centerX || !centerZ) {
        if (errorText) *errorText = "World preview anchor output pointer was null";
        return false;
    }
    const fs::path regionDir = ResolveOverworldRegionDir(worldDir);
    if (!fs::exists(regionDir)) {
        if (errorText) *errorText = "The world has no overworld region directory";
        return false;
    }
    std::array<std::uint8_t, 4096> locations{};
    for (const auto& entry : fs::directory_iterator(regionDir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".mca") continue;
        int rx = 0;
        int rz = 0;
        if (std::sscanf(entry.path().filename().string().c_str(), "r.%d.%d.mca", &rx, &rz) != 2) continue;
        std::ifstream in(entry.path(), std::ios::binary);
        if (!in.read(reinterpret_cast<char*>(locations.data()), locations.size())) continue;
        for (int index = 0; index < 1024; ++index) {
            const std::size_t offset = static_cast<std::size_t>(index) * 4;
            if (locations[offset] == 0 && locations[offset + 1] == 0 && locations[offset + 2] == 0) continue;
            const int chunkX = rx * 32 + index % 32;
            const int chunkZ = rz * 32 + index / 32;
            *centerX = chunkX * 16 + 8;
            *centerZ = chunkZ * 16 + 8;
            return true;
        }
    }
    if (errorText) *errorText = "The world has no generated overworld chunks";
    return false;
}

bool LoadWorldPreviewSlice(const fs::path& worldDir, int centerX, int centerZ, int radiusX, int radiusZ, std::optional<int> minY, std::optional<int> maxY, std::size_t maxBlocks, WorldPreviewSlice* slice, std::string* errorText) {
    if (!slice) {
        if (errorText) *errorText = "World preview slice output pointer was null";
        return false;
    }
    slice->blocks.clear();
    slice->centerX = centerX;
    slice->centerZ = centerZ;
    slice->truncated = false;

    const fs::path regionDir = ResolveOverworldRegionDir(worldDir);
    if (!fs::exists(regionDir)) {
        if (errorText) *errorText = "The world has no overworld region directory";
        return false;
    }

    struct TopBlock {
        int y = std::numeric_limits<int>::min();
        std::uint64_t occupiedBelowTop = 0;
        int sampleSizeX = 1;
        int sampleSizeZ = 1;
        std::string name;
    };
    std::unordered_map<std::int64_t, TopBlock> topByColumn;
    const int minX = centerX - std::max(1, radiusX);
    const int maxX = centerX + std::max(1, radiusX);
    const int minZ = centerZ - std::max(1, radiusZ);
    const int maxZ = centerZ + std::max(1, radiusZ);
    int denseRadius = std::min({ std::max(1, radiusX), std::max(1, radiusZ), 128 });

    struct PreviewRegion {
        fs::path path;
        int rx = 0;
        int rz = 0;
    };
    std::vector<PreviewRegion> regions;
    double generatedColumns = 0.0;
    double generatedDenseColumns = 0.0;
    std::array<std::uint8_t, 4096> locations{};
    for (const auto& entry : fs::directory_iterator(regionDir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".mca") continue;
        int rx = 0;
        int rz = 0;
        if (std::sscanf(entry.path().filename().string().c_str(), "r.%d.%d.mca", &rx, &rz) != 2) continue;
        const int regionMinX = rx * 512;
        const int regionMaxX = regionMinX + 511;
        const int regionMinZ = rz * 512;
        const int regionMaxZ = regionMinZ + 511;
        if (regionMaxX < minX || regionMinX > maxX || regionMaxZ < minZ || regionMinZ > maxZ) continue;
        regions.push_back({ entry.path(), rx, rz });

        std::ifstream header(entry.path(), std::ios::binary);
        if (!header.read(reinterpret_cast<char*>(locations.data()), locations.size())) continue;
        for (int index = 0; index < 1024; ++index) {
            const std::size_t offset = static_cast<std::size_t>(index) * 4;
            if (locations[offset] == 0 && locations[offset + 1] == 0 && locations[offset + 2] == 0) continue;
            const int chunkMinX = (rx * 32 + index % 32) * 16;
            const int chunkMaxX = chunkMinX + 15;
            const int chunkMinZ = (rz * 32 + index / 32) * 16;
            const int chunkMaxZ = chunkMinZ + 15;
            const int clippedMinX = std::max(chunkMinX, minX);
            const int clippedMaxX = std::min(chunkMaxX, maxX);
            const int clippedMinZ = std::max(chunkMinZ, minZ);
            const int clippedMaxZ = std::min(chunkMaxZ, maxZ);
            if (clippedMinX > clippedMaxX || clippedMinZ > clippedMaxZ) continue;
            generatedColumns += static_cast<double>(clippedMaxX - clippedMinX + 1) *
                static_cast<double>(clippedMaxZ - clippedMinZ + 1);
            const int denseMinX = std::max(clippedMinX, centerX - denseRadius);
            const int denseMaxX = std::min(clippedMaxX, centerX + denseRadius);
            const int denseMinZ = std::max(clippedMinZ, centerZ - denseRadius);
            const int denseMaxZ = std::min(clippedMaxZ, centerZ + denseRadius);
            if (denseMinX <= denseMaxX && denseMinZ <= denseMaxZ) {
                generatedDenseColumns += static_cast<double>(denseMaxX - denseMinX + 1) *
                    static_cast<double>(denseMaxZ - denseMinZ + 1);
            }
        }
    }

    int sampleStepX = 1;
    int sampleStepZ = 1;
    if (maxBlocks > 0) {
        const double requestedColumns = static_cast<double>(maxX - minX + 1) * static_cast<double>(maxZ - minZ + 1);
        const double columns = generatedColumns > 0.0 ? generatedColumns : requestedColumns;
        const double denseColumns = std::min(columns, generatedDenseColumns);
        const double sparseBudget = std::max(1.0, static_cast<double>(maxBlocks) - denseColumns);
        const double sparseColumns = std::max(0.0, columns - denseColumns);
        const double targetSampleArea = sparseColumns / sparseBudget;
        sampleStepX = std::max(1, static_cast<int>(std::floor(std::sqrt(targetSampleArea))));
        sampleStepZ = std::max(1, static_cast<int>(std::ceil(targetSampleArea / sampleStepX)));
    }

    auto packColumn = [](int x, int z) -> std::int64_t {
        return (static_cast<std::int64_t>(x) << 32) ^ static_cast<std::uint32_t>(z);
    };
    auto rangeHasSample = [](int low, int high, int origin, int sampleStep) {
        if (sampleStep <= 1) return true;
        const int remainder = (low - origin) % sampleStep;
        const int first = low + (remainder == 0 ? 0 : sampleStep - remainder);
        return first <= high;
    };

    for (const auto& region : regions) {
        const int rx = region.rx;
        const int rz = region.rz;
        const std::vector<std::uint8_t> regionBuffer = ReadFileBytes(region.path);
        if (regionBuffer.size() < 8192) {
            continue;
        }

        for (int index = 0; index < 1024; ++index) {
            const int lxChunk = index % 32;
            const int lzChunk = index / 32;
            const int cx = rx * 32 + lxChunk;
            const int cz = rz * 32 + lzChunk;
            const int chunkMinX = cx * 16;
            const int chunkMaxX = chunkMinX + 15;
            const int chunkMinZ = cz * 16;
            const int chunkMaxZ = chunkMinZ + 15;
            if (chunkMaxX < minX || chunkMinX > maxX || chunkMaxZ < minZ || chunkMinZ > maxZ) {
                continue;
            }
            const bool intersectsDense = chunkMaxX >= centerX - denseRadius && chunkMinX <= centerX + denseRadius &&
                chunkMaxZ >= centerZ - denseRadius && chunkMinZ <= centerZ + denseRadius;
            if (!intersectsDense &&
                (!rangeHasSample(std::max(chunkMinX, minX), std::min(chunkMaxX, maxX), minX, sampleStepX) ||
                 !rangeHasSample(std::max(chunkMinZ, minZ), std::min(chunkMaxZ, maxZ), minZ, sampleStepZ))) {
                continue;
            }

            ChunkRecord record;
            std::string localError;
            if (!ReadChunkRecord(regionBuffer, index, &record, &localError)) {
                if (errorText) *errorText = localError;
                return false;
            }
            if (record.payload.empty()) {
                continue;
            }
            NamedTag chunk;
            if (!ReadNamedTag(record.payload, &chunk, &localError)) {
                if (errorText) *errorText = localError;
                return false;
            }
            Tag* sections = chunk.root.Find("sections");
            if (!sections || sections->type != TagType::List) {
                continue;
            }
            for (Tag& section : sections->listValue) {
                Tag* yTag = section.Find("Y");
                if (!yTag) continue;
                const int sectionY = yTag->byteValue;
                DecodedSection decoded = DecodeSectionIndices(&section);
                for (int i = 0; i < 4096; ++i) {
                    const int paletteIndex = decoded.indices[i];
                    if (paletteIndex < 0 || paletteIndex >= static_cast<int>(decoded.paletteNames.size())) continue;
                    const std::string& blockName = decoded.paletteNames[paletteIndex];
                    if (blockName == "minecraft:air") continue;
                    const int localY = i / 256;
                    const int rem = i % 256;
                    const int localZ = rem / 16;
                    const int localX = rem % 16;
                    const int x = cx * 16 + localX;
                    const int y = sectionY * 16 + localY;
                    const int z = cz * 16 + localZ;
                    if (x < minX || x > maxX || z < minZ || z > maxZ) continue;
                    const bool dense = std::abs(x - centerX) <= denseRadius && std::abs(z - centerZ) <= denseRadius;
                    if (!dense && (((x - minX) % sampleStepX) != 0 || ((z - minZ) % sampleStepZ) != 0)) continue;
                    if (minY && y < *minY) continue;
                    if (maxY && y > *maxY) continue;
                    TopBlock& top = topByColumn[packColumn(x, z)];
                    if (top.y == std::numeric_limits<int>::min()) {
                        top.y = y;
                        top.occupiedBelowTop = 1;
                        top.sampleSizeX = dense ? 1 : sampleStepX;
                        top.sampleSizeZ = dense ? 1 : sampleStepZ;
                        top.name = blockName;
                    } else if (y > top.y) {
                        const int delta = y - top.y;
                        top.occupiedBelowTop = delta >= 64 ? 1 : (top.occupiedBelowTop << delta) | 1;
                        top.y = y;
                        top.sampleSizeX = dense ? 1 : sampleStepX;
                        top.sampleSizeZ = dense ? 1 : sampleStepZ;
                        top.name = blockName;
                    } else {
                        const int depth = top.y - y;
                        if (depth < 64) top.occupiedBelowTop |= std::uint64_t{1} << depth;
                    }
                }
            }
        }
    }

    std::vector<WorldPreviewBlock> blocks;
    blocks.reserve(topByColumn.size());
    for (const auto& [key, top] : topByColumn) {
        WorldPreviewBlock block;
        block.x = static_cast<int>(key >> 32);
        block.z = static_cast<int>(static_cast<std::int32_t>(key & 0xffffffff));
        block.y = top.y;
        int solidDepth = 1;
        while (solidDepth < 64 && (top.occupiedBelowTop & (std::uint64_t{1} << solidDepth)) != 0) {
            ++solidDepth;
        }
        block.bottomY = top.y - solidDepth + 1;
        block.sampleSizeX = top.sampleSizeX;
        block.sampleSizeZ = top.sampleSizeZ;
        block.blockName = top.name;
        blocks.push_back(std::move(block));
    }
    if (maxBlocks > 0 && blocks.size() > maxBlocks) {
        slice->truncated = true;
        std::sort(blocks.begin(), blocks.end(), [](const WorldPreviewBlock& a, const WorldPreviewBlock& b) {
            if (a.z != b.z) return a.z < b.z;
            if (a.x != b.x) return a.x < b.x;
            return a.y < b.y;
        });
        std::vector<WorldPreviewBlock> reduced;
        reduced.reserve(maxBlocks);
        for (std::size_t i = 0; i < maxBlocks; ++i) {
            reduced.push_back(std::move(blocks[i * blocks.size() / maxBlocks]));
        }
        blocks = std::move(reduced);
    }
    std::sort(blocks.begin(), blocks.end(), [](const WorldPreviewBlock& a, const WorldPreviewBlock& b) {
        if (a.y != b.y) return a.y < b.y;
        if (a.z != b.z) return a.z < b.z;
        return a.x < b.x;
    });
    slice->blocks = std::move(blocks);
    return true;
}

}  // namespace native_mc
