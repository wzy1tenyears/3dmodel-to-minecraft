#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <gl/GL.h>
#include <mmsystem.h>
#include <shlobj.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <io.h>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "native/glb_textured_importer.h"
#include "native/glb_surface_importer.h"
#include "native/obj_textured_importer.h"
#include "native/palette.h"
#include "native/preview_cache.h"
#include "native/preview_gpu_lod.h"
#include "native/preview_mesh.h"
#include "native/preview_renderer.h"
#include "native/world_ops.h"
#include "app/cli_args.h"

namespace fs = std::filesystem;

#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STATIC_DRAW 0x88E4
#endif

using GlBufferSize = std::ptrdiff_t;
using PFNGLGENBUFFERSPROC = void (APIENTRY*)(GLsizei, GLuint*);
using PFNGLBINDBUFFERPROC = void (APIENTRY*)(GLenum, GLuint);
using PFNGLBUFFERDATAPROC = void (APIENTRY*)(GLenum, GlBufferSize, const void*, GLenum);
using PFNGLDELETEBUFFERSPROC = void (APIENTRY*)(GLsizei, const GLuint*);

PFNGLGENBUFFERSPROC gGlGenBuffers = nullptr;
PFNGLBINDBUFFERPROC gGlBindBuffer = nullptr;
PFNGLBUFFERDATAPROC gGlBufferData = nullptr;
PFNGLDELETEBUFFERSPROC gGlDeleteBuffers = nullptr;

#pragma comment(lib, "Comctl32.lib")

#if !defined(CODEX_GUI_EXE) && !defined(CODEX_CLI_EXE)
#define CODEX_GUI_EXE 1
#endif

enum class Lang {
    Zh,
    En,
};

int DefaultWorkerCount() {
    unsigned int logicalProcessors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (logicalProcessors == 0) logicalProcessors = std::thread::hardware_concurrency();
    if (logicalProcessors == 0) return 1;
    return static_cast<int>(std::clamp(logicalProcessors / 2, 1u, 64u));
}

int NormalizePreviewRefreshRate(int value) {
    if (value == 0) return 0;
    if (value < 0) return 60;
    constexpr std::array<int, 6> supportedRates = { 30, 60, 120, 144, 180, 200 };
    int closest = supportedRates.front();
    int closestDistance = std::abs(value - closest);
    for (const int rate : supportedRates) {
        const int distance = std::abs(value - rate);
        if (distance < closestDistance) {
            closest = rate;
            closestDistance = distance;
        }
    }
    return closest;
}

struct Config {
    fs::path projectRoot;
    fs::path inputPath;
    fs::path objDir;
    fs::path glbDir;
    fs::path worldDir;
    fs::path savesDir;
    fs::path mcRoot;
    fs::path testWorldRoot;
    fs::path copyTo;
    std::wstring worldName;
    std::wstring mcVersion;
    std::wstring onlyFile;
    std::wstring center;
    std::wstring centerX;
    std::wstring centerY;
    std::wstring centerZ;
    std::wstring levelNormal;
    std::wstring lang;
    std::wstring lastMode;
    std::wstring lastSafety;
    std::wstring rotateYDeg;
    std::wstring rotationMode = L"euler";
    std::wstring rotationW = L"1";
    std::wstring rotationX = L"0";
    std::wstring rotationY;
    std::wstring rotationZ = L"0";
    std::wstring baseY;
    std::wstring scaleBlocksPerMeter;
    std::wstring fallbackBlock;
    std::wstring blockTypes;
    std::wstring blocks;
    std::wstring excludeBlockTypes;
    std::wstring excludeBlocks;
    std::wstring palette;
    std::wstring transform;
    std::wstring block;
    std::wstring clipReferenceObj;
    std::wstring clipBelowMeters;
    std::wstring clipCellSize;
    std::wstring clipLowFraction;
    std::wstring clipPasses;
    std::wstring clipTrimSigma;
    std::wstring dedupeSampleSteps;
    std::wstring componentMinRatio;
    std::wstring componentBelowGap;
    std::wstring minY;
    std::wstring maxY;
    std::wstring previewPresetA;
    std::wstring previewPresetB;
    std::wstring previewPresetC;
    std::wstring previewWorldStyle;
    std::wstring previewRenderer = L"auto";
    std::wstring previewOrbitSensitivity = L"1.5";
    std::wstring previewPanSensitivity = L"2";
    std::wstring previewZoomSensitivity = L"1.5";
    bool direct = false;
    bool copyWorld = true;
    bool overwrite = false;
    bool noGlass = false;
    bool allFiles = false;
    bool flipX = false;
    bool flipZ = false;
    bool noFlipZ = false;
    bool noComponentFilter = false;
    bool previewWorldVisible = true;
    bool previewShowGrid = true;
    bool previewShowAxes = true;
    int workers = DefaultWorkerCount();
    int batchBlockLimit = 1500000;
    int previewRefreshRate = 60;
    int previewWorldRadius = 48;
    int previewWorldMaxBlocks = 500000;
};

struct BackendLayout {
    fs::path root;
};

struct ImportPlan {
    std::wstring mode;
    fs::path sourceDir;
    std::wstring onlyFile;
    fs::path worldDir;
    fs::path originalWorldDir;
    fs::path copiedWorldDir;
    std::wstring safetyMode;
    bool allFiles = false;
};

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(::towlower(ch));
    });
    return value;
}

bool IEquals(const std::wstring& a, const std::wstring& b) {
    return ToLower(a) == ToLower(b);
}

std::wstring Trim(const std::wstring& value) {
    size_t start = 0;
    while (start < value.size() && ::iswspace(value[start])) {
        ++start;
    }
    size_t end = value.size();
    while (end > start && ::iswspace(value[end - 1])) {
        --end;
    }
    return value.substr(start, end - start);
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return L"";
    }
    int length = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) {
        return L"";
    }
    std::wstring result(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return "";
    }
    int length = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return "";
    }
    std::string result(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring FormatNumber(std::uint64_t value) {
    std::wstring digits = std::to_wstring(value);
    std::wstring out;
    const int firstGroup = static_cast<int>(digits.size()) % 3;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i > 0) {
            const bool firstSplit = firstGroup != 0 && static_cast<int>(i) == firstGroup;
            const bool regularSplit = firstGroup == 0 ? (i % 3 == 0) : (i > static_cast<size_t>(firstGroup) && (i - firstGroup) % 3 == 0);
            if (firstSplit || regularSplit) {
                out.push_back(L',');
            }
        }
        out.push_back(digits[i]);
    }
    return out;
}

std::vector<std::wstring> SplitCsvWide(const std::wstring& text) {
    std::vector<std::wstring> out;
    std::wstringstream stream(text);
    std::wstring part;
    while (std::getline(stream, part, L',')) {
        part = Trim(part);
        if (!part.empty()) {
            out.push_back(part);
        }
    }
    return out;
}

std::wstring JoinCsvWide(const std::vector<std::wstring>& values) {
    std::wstring out;
    for (const auto& value : values) {
        const std::wstring trimmed = Trim(value);
        if (trimmed.empty()) continue;
        if (!out.empty()) out += L",";
        out += trimmed;
    }
    return out;
}

std::optional<float> ParseOptionalFloatText(const std::wstring& text) {
    const std::wstring trimmed = Trim(text);
    if (trimmed.empty()) return std::nullopt;
    try {
        return std::stof(trimmed);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<int> ParseOptionalIntText(const std::wstring& text) {
    const std::wstring trimmed = Trim(text);
    if (trimmed.empty()) return std::nullopt;
    try {
        return std::stoi(trimmed);
    } catch (...) {
        return std::nullopt;
    }
}

std::wstring TrimTrailingZeros(std::wstring value) {
    if (value.find(L'.') == std::wstring::npos) return value;
    while (!value.empty() && value.back() == L'0') value.pop_back();
    if (!value.empty() && value.back() == L'.') value.pop_back();
    return value.empty() ? L"0" : value;
}

std::wstring FormatFloatCompact(float value, int decimals = 1) {
    std::wostringstream ss;
    ss.setf(std::ios::fixed);
    ss.precision(decimals);
    ss << value;
    return TrimTrailingZeros(ss.str());
}

std::wstring ConfigRotationSummary(const Config& config, const std::wstring& overrideY = L"") {
    if (ToLower(config.rotationMode) == L"quaternion") {
        return L"WXYZ(" + (config.rotationW.empty() ? L"1" : config.rotationW) + L", "
            + (config.rotationX.empty() ? L"0" : config.rotationX) + L", "
            + (config.rotationY.empty() ? L"0" : config.rotationY) + L", "
            + (config.rotationZ.empty() ? L"0" : config.rotationZ) + L")";
    }
    const std::wstring y = !overrideY.empty() ? overrideY
        : (!config.rotationY.empty() ? config.rotationY : (config.rotateYDeg.empty() ? L"0" : config.rotateYDeg));
    return L"XYZ(" + (config.rotationX.empty() ? L"0" : config.rotationX) + L", " + y + L", "
        + (config.rotationZ.empty() ? L"0" : config.rotationZ) + L") deg";
}

std::wstring PathToDisplay(const fs::path& value) {
    return value.empty() ? L"" : value.wstring();
}

std::wstring JsonEscape(const std::wstring& value) {
    std::wstring out;
    out.reserve(value.size() + 8);
    for (wchar_t ch : value) {
        switch (ch) {
        case L'\\': out += L"\\\\"; break;
        case L'"': out += L"\\\""; break;
        case L'\n': out += L"\\n"; break;
        case L'\r': out += L"\\r"; break;
        case L'\t': out += L"\\t"; break;
        default: out.push_back(ch); break;
        }
    }
    return out;
}

std::wstring JsonUnescape(const std::wstring& value) {
    std::wstring out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        wchar_t ch = value[i];
        if (ch == L'\\' && i + 1 < value.size()) {
            wchar_t next = value[++i];
            switch (next) {
            case L'\\': out.push_back(L'\\'); break;
            case L'"': out.push_back(L'"'); break;
            case L'n': out.push_back(L'\n'); break;
            case L'r': out.push_back(L'\r'); break;
            case L't': out.push_back(L'\t'); break;
            case L'/': out.push_back(L'/'); break;
            default:
                out.push_back(next);
                break;
            }
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

std::optional<std::wstring> ExtractJsonString(const std::wstring& text, const std::wstring& key) {
    const std::wstring pattern = L"\"" + key + L"\"\\s*:\\s*\"((?:\\\\.|[^\"])*)\"";
    std::wregex rx(pattern, std::regex_constants::icase);
    std::wsmatch match;
    if (std::regex_search(text, match, rx) && match.size() >= 2) {
        return JsonUnescape(match[1].str());
    }
    return std::nullopt;
}

std::optional<int> ExtractJsonInt(const std::wstring& text, const std::wstring& key) {
    const std::wstring pattern = L"\"" + key + L"\"\\s*:\\s*(-?\\d+)";
    std::wregex rx(pattern, std::regex_constants::icase);
    std::wsmatch match;
    if (std::regex_search(text, match, rx) && match.size() >= 2) {
        return std::stoi(match[1].str());
    }
    return std::nullopt;
}

std::optional<bool> ExtractJsonBool(const std::wstring& text, const std::wstring& key) {
    const std::wstring pattern = L"\"" + key + L"\"\\s*:\\s*(true|false)";
    std::wregex rx(pattern, std::regex_constants::icase);
    std::wsmatch match;
    if (std::regex_search(text, match, rx) && match.size() >= 2) {
        return IEquals(match[1].str(), L"true");
    }
    return std::nullopt;
}

std::wstring ReadWholeFileUtf8(const fs::path& filePath) {
    std::ifstream in(filePath, std::ios::binary);
    if (!in) {
        return L"";
    }
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return Utf8ToWide(bytes);
}

bool WriteWholeFileUtf8(const fs::path& filePath, const std::wstring& content) {
    std::error_code ec;
    fs::create_directories(filePath.parent_path(), ec);
    std::ofstream out(filePath, std::ios::binary);
    if (!out) {
        return false;
    }
    const std::string utf8 = WideToUtf8(content);
    out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    return static_cast<bool>(out);
}

std::wstring JoinLines(const std::vector<std::wstring>& lines) {
    std::wostringstream out;
    for (size_t i = 0; i < lines.size(); ++i) {
        out << lines[i];
        if (i + 1 < lines.size()) {
            out << L"\n";
        }
    }
    return out.str();
}

void PrintLine(const std::wstring& text = L"") {
    std::wcout << text << L"\n";
}

std::wstring T(Lang lang, const wchar_t* zh, const wchar_t* en) {
    return lang == Lang::Zh ? zh : en;
}

bool IsZhLocale() {
    LANGID lang = GetUserDefaultUILanguage();
    return PRIMARYLANGID(lang) == LANG_CHINESE;
}

std::wstring GetEnvVar(const std::wstring& name) {
    DWORD needed = GetEnvironmentVariableW(name.c_str(), nullptr, 0);
    if (needed == 0) {
        return L"";
    }
    std::wstring value(needed, L'\0');
    GetEnvironmentVariableW(name.c_str(), value.data(), needed);
    if (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    return value;
}

fs::path NormalizeIfExists(const fs::path& pathValue) {
    if (pathValue.empty()) {
        return {};
    }
    std::error_code ec;
    if (fs::exists(pathValue, ec)) {
        return fs::weakly_canonical(pathValue, ec);
    }
    return fs::absolute(pathValue, ec);
}

std::wstring QuoteForWindows(const std::wstring& value) {
    if (value.empty()) {
        return L"\"\"";
    }
    bool needsQuotes = value.find_first_of(L" \t\"") != std::wstring::npos;
    if (!needsQuotes) {
        return value;
    }
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
            continue;
        }
        if (backslashes) {
            out.append(backslashes, L'\\');
            backslashes = 0;
        }
        out.push_back(ch);
    }
    if (backslashes) {
        out.append(backslashes * 2, L'\\');
    }
    out.push_back(L'"');
    return out;
}

fs::path GetExePath() {
    std::wstring buffer(32768, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return fs::path(buffer);
}

std::wstring StemLower(const fs::path& value) {
    return ToLower(value.stem().wstring());
}

bool HasFile(const fs::path& filePath) {
    std::error_code ec;
    return fs::exists(filePath, ec) && fs::is_regular_file(filePath, ec);
}

bool HasDir(const fs::path& dirPath) {
    std::error_code ec;
    return fs::exists(dirPath, ec) && fs::is_directory(dirPath, ec);
}

bool HasNativeResourceRoot(const fs::path& rootPath) {
    return HasFile(rootPath / "vendor" / "ots" / "res" / "atlases" / "vanilla.atlas")
        && HasFile(rootPath / "vendor" / "ots" / "res" / "palettes" / "colourful.ts");
}

bool IsReadableWorldDir(const fs::path& dirPath) {
    return HasDir(dirPath) && HasFile(dirPath / "level.dat");
}

bool IsWorldSessionActive(const fs::path& worldDir) {
    const fs::path lockPath = worldDir / "session.lock";
    if (!HasFile(lockPath)) return false;
    HANDLE handle = CreateFileW(lockPath.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_SHARING_VIOLATION;
    CloseHandle(handle);
    return false;
}

std::vector<fs::path> CollectWorldsFromSaves(const fs::path& savesDir) {
    std::vector<fs::path> worlds;
    if (!HasDir(savesDir)) {
        return worlds;
    }
    for (const auto& entry : fs::directory_iterator(savesDir)) {
        if (!entry.is_directory()) {
            continue;
        }
        if (HasFile(entry.path() / "level.dat")) {
            worlds.push_back(entry.path());
        }
    }
    std::sort(worlds.begin(), worlds.end(), [](const fs::path& a, const fs::path& b) {
        std::error_code ec1;
        std::error_code ec2;
        auto ta = fs::last_write_time(a, ec1);
        auto tb = fs::last_write_time(b, ec2);
        if (!ec1 && !ec2) {
            return ta > tb;
        }
        return a.wstring() < b.wstring();
    });
    return worlds;
}

std::vector<fs::path> CandidateSavesDirs(const fs::path& projectRoot, const Config& config, const ParsedArgs& args) {
    std::vector<fs::path> candidates;
    auto add = [&](const fs::path& pathValue) {
        if (pathValue.empty()) {
            return;
        }
        fs::path normalized = NormalizeIfExists(pathValue);
        if (normalized.empty()) {
            normalized = pathValue;
        }
        if (std::find(candidates.begin(), candidates.end(), normalized) == candidates.end()) {
            candidates.push_back(normalized);
        }
    };

    auto valueIt = args.values.find(L"--saves-dir");
    if (valueIt != args.values.end()) {
        add(valueIt->second);
    }
    if (!config.savesDir.empty()) {
        add(config.savesDir);
    }

    std::wstring mcVersion;
    auto versionIt = args.values.find(L"--mc-version");
    if (versionIt != args.values.end()) {
        mcVersion = versionIt->second;
    } else if (!config.mcVersion.empty()) {
        mcVersion = config.mcVersion;
    }

    fs::path mcRoot;
    auto mcRootIt = args.values.find(L"--mc-root");
    if (mcRootIt != args.values.end()) {
        mcRoot = mcRootIt->second;
    } else if (!config.mcRoot.empty()) {
        mcRoot = config.mcRoot;
    } else {
        const fs::path parent = projectRoot.parent_path();
        if (HasDir(parent / ".minecraft")) {
            mcRoot = parent / ".minecraft";
        } else if (HasDir(projectRoot / ".minecraft")) {
            mcRoot = projectRoot / ".minecraft";
        } else {
            std::error_code cwdEc;
            const fs::path cwd = fs::current_path(cwdEc);
            if (!cwdEc && HasDir(cwd / ".minecraft")) {
                mcRoot = cwd / ".minecraft";
            } else if (!cwdEc && HasDir(cwd.parent_path() / ".minecraft")) {
                mcRoot = cwd.parent_path() / ".minecraft";
            }
        }
    }

    if (!mcRoot.empty()) {
        add(mcRoot / "saves");
        if (!mcVersion.empty()) {
            add(mcRoot / "versions" / mcVersion / "saves");
        }
        const fs::path versionsDir = mcRoot / "versions";
        if (HasDir(versionsDir)) {
            for (const auto& entry : fs::directory_iterator(versionsDir)) {
                if (!entry.is_directory()) {
                    continue;
                }
                add(entry.path() / "saves");
            }
        }
    }

    return candidates;
}

std::vector<fs::path> AutoDetectInputDirs(const fs::path& projectRoot, const std::wstring& mode, const Config& config, const ParsedArgs& args) {
    std::vector<fs::path> candidates;
    auto add = [&](const fs::path& value) {
        if (value.empty()) {
            return;
        }
        fs::path normalized = NormalizeIfExists(value);
        if (normalized.empty()) {
            normalized = value;
        }
        if (std::find(candidates.begin(), candidates.end(), normalized) == candidates.end()) {
            candidates.push_back(normalized);
        }
    };

    if (mode == L"obj") {
        auto it = args.values.find(L"--obj-dir");
        if (it != args.values.end()) {
            add(it->second);
        }
        if (!config.objDir.empty()) {
            add(config.objDir);
        }
    } else {
        auto it = args.values.find(L"--glb-dir");
        if (it != args.values.end()) {
            add(it->second);
        }
        if (!config.glbDir.empty()) {
            add(config.glbDir);
        }
    }

    auto inputIt = args.values.find(L"--input");
    if (inputIt != args.values.end()) {
        add(inputIt->second);
    } else if (!config.inputPath.empty()) {
        add(config.inputPath);
    }

    const fs::path parent = projectRoot.parent_path();
    if (mode == L"obj") {
        add(parent / "all");
        add(projectRoot / "all");
    } else {
        add(parent / "output");
        add(projectRoot / "output");
    }
    return candidates;
}

std::vector<fs::path> CollectFilesByExtension(const fs::path& dirPath, const std::wstring& extension) {
    std::vector<fs::path> files;
    if (!HasDir(dirPath)) {
        return files;
    }
    for (const auto& entry : fs::directory_iterator(dirPath)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (IEquals(entry.path().extension().wstring(), extension)) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::optional<fs::path> FirstExistingFile(const std::vector<fs::path>& files) {
    for (const auto& filePath : files) {
        if (HasFile(filePath)) {
            return NormalizeIfExists(filePath);
        }
    }
    return std::nullopt;
}

Lang ResolveLanguage(const ParsedArgs& args, const Config& config) {
    auto it = args.values.find(L"--lang");
    std::wstring raw;
    if (it != args.values.end()) {
        raw = it->second;
    } else if (!config.lang.empty()) {
        raw = config.lang;
    } else {
        raw = GetEnvVar(L"LANG_UI");
        if (raw.empty()) {
            raw = GetEnvVar(L"LANG");
        }
    }
    raw = ToLower(raw);
    if (raw.find(L"zh") == 0) {
        return Lang::Zh;
    }
    if (raw.find(L"en") == 0) {
        return Lang::En;
    }
    return IsZhLocale() ? Lang::Zh : Lang::En;
}

std::wstring ReadInputLine() {
    std::wstring line;
    std::getline(std::wcin, line);
    return Trim(line);
}

std::wstring Prompt(const std::wstring& label, const std::wstring& fallback = L"") {
    if (fallback.empty()) {
        std::wcout << label;
    } else {
        std::wcout << label << L" [" << fallback << L"] ";
    }
    std::wstring value = ReadInputLine();
    return value.empty() ? fallback : value;
}

void PauseForUser(Lang lang) {
    std::wcout << T(lang, L"按回车继续...", L"Press Enter to continue...");
    ReadInputLine();
}

bool Confirm(const std::wstring& message) {
    std::wcout << message;
    return IEquals(ReadInputLine(), L"YES");
}

bool PromptBool(Lang lang, const std::wstring& label, bool fallback) {
    const std::wstring fallbackText = fallback ? L"Y" : L"N";
    const std::wstring value = ToLower(Prompt(label + T(lang, L" (Y/N)", L" (Y/N)"), fallbackText));
    return value == L"y" || value == L"yes" || value == L"true" || value == L"1";
}

Config LoadConfig(const fs::path& configPath) {
    Config config;
    if (!HasFile(configPath)) {
        return config;
    }
    const std::wstring text = ReadWholeFileUtf8(configPath);
    auto fillPath = [&](const std::wstring& key, fs::path& target) {
        if (auto value = ExtractJsonString(text, key)) {
            target = fs::path(*value);
        }
    };
    auto fillString = [&](const std::wstring& key, std::wstring& target) {
        if (auto value = ExtractJsonString(text, key)) {
            target = *value;
        }
    };
    fillPath(L"projectRoot", config.projectRoot);
    fillPath(L"inputPath", config.inputPath);
    fillPath(L"objDir", config.objDir);
    fillPath(L"glbDir", config.glbDir);
    fillPath(L"worldDir", config.worldDir);
    fillPath(L"savesDir", config.savesDir);
    fillPath(L"mcRoot", config.mcRoot);
    fillPath(L"testWorldRoot", config.testWorldRoot);
    fillPath(L"copyTo", config.copyTo);
    fillString(L"worldName", config.worldName);
    fillString(L"mcVersion", config.mcVersion);
    fillString(L"onlyFile", config.onlyFile);
    fillString(L"center", config.center);
    fillString(L"centerX", config.centerX);
    fillString(L"centerY", config.centerY);
    fillString(L"centerZ", config.centerZ);
    fillString(L"levelNormal", config.levelNormal);
    fillString(L"lang", config.lang);
    fillString(L"lastMode", config.lastMode);
    fillString(L"lastSafety", config.lastSafety);
    fillString(L"rotateYDeg", config.rotateYDeg);
    fillString(L"rotationMode", config.rotationMode);
    fillString(L"rotationW", config.rotationW);
    fillString(L"rotationX", config.rotationX);
    fillString(L"rotationY", config.rotationY);
    fillString(L"rotationZ", config.rotationZ);
    if (config.rotationY.empty()) config.rotationY = config.rotateYDeg.empty() ? L"0" : config.rotateYDeg;
    fillString(L"baseY", config.baseY);
    fillString(L"scaleBlocksPerMeter", config.scaleBlocksPerMeter);
    fillString(L"fallbackBlock", config.fallbackBlock);
    fillString(L"blockTypes", config.blockTypes);
    fillString(L"blocks", config.blocks);
    fillString(L"excludeBlockTypes", config.excludeBlockTypes);
    fillString(L"excludeBlocks", config.excludeBlocks);
    fillString(L"palette", config.palette);
    fillString(L"transform", config.transform);
    fillString(L"block", config.block);
    fillString(L"clipReferenceObj", config.clipReferenceObj);
    fillString(L"clipBelowMeters", config.clipBelowMeters);
    fillString(L"clipCellSize", config.clipCellSize);
    fillString(L"clipLowFraction", config.clipLowFraction);
    fillString(L"clipPasses", config.clipPasses);
    fillString(L"clipTrimSigma", config.clipTrimSigma);
    fillString(L"dedupeSampleSteps", config.dedupeSampleSteps);
    fillString(L"componentMinRatio", config.componentMinRatio);
    fillString(L"componentBelowGap", config.componentBelowGap);
    fillString(L"minY", config.minY);
    fillString(L"maxY", config.maxY);
    fillString(L"previewPresetA", config.previewPresetA);
    fillString(L"previewPresetB", config.previewPresetB);
    fillString(L"previewPresetC", config.previewPresetC);
    fillString(L"previewWorldStyle", config.previewWorldStyle);
    fillString(L"previewRenderer", config.previewRenderer);
    fillString(L"previewOrbitSensitivity", config.previewOrbitSensitivity);
    fillString(L"previewPanSensitivity", config.previewPanSensitivity);
    fillString(L"previewZoomSensitivity", config.previewZoomSensitivity);
    if (auto value = ExtractJsonBool(text, L"direct")) config.direct = *value;
    if (auto value = ExtractJsonBool(text, L"copyWorld")) config.copyWorld = *value;
    if (auto value = ExtractJsonBool(text, L"overwrite")) config.overwrite = *value;
    if (auto value = ExtractJsonBool(text, L"noGlass")) config.noGlass = *value;
    if (auto value = ExtractJsonBool(text, L"all")) config.allFiles = *value;
    if (auto value = ExtractJsonBool(text, L"flipX")) config.flipX = *value;
    if (auto value = ExtractJsonBool(text, L"flipZ")) config.flipZ = *value;
    if (auto value = ExtractJsonBool(text, L"noFlipZ")) config.noFlipZ = *value;
    if (auto value = ExtractJsonBool(text, L"noComponentFilter")) config.noComponentFilter = *value;
    if (auto value = ExtractJsonBool(text, L"previewWorldVisible")) config.previewWorldVisible = *value;
    if (auto value = ExtractJsonBool(text, L"previewShowGrid")) config.previewShowGrid = *value;
    if (auto value = ExtractJsonBool(text, L"previewShowAxes")) config.previewShowAxes = *value;
    if (auto value = ExtractJsonInt(text, L"workers")) {
        config.workers = std::clamp(*value, 1, 64);
    }
    if (auto value = ExtractJsonInt(text, L"batchBlockLimit")) {
        config.batchBlockLimit = *value;
    }
    if (auto value = ExtractJsonInt(text, L"previewRefreshRate")) {
        config.previewRefreshRate = NormalizePreviewRefreshRate(*value);
    }
    if (auto value = ExtractJsonInt(text, L"previewWorldRadius")) {
        config.previewWorldRadius = *value;
    }
    if (auto value = ExtractJsonInt(text, L"previewWorldMaxBlocks")) {
        config.previewWorldMaxBlocks = *value == 250000 ? 500000 : *value;
    }
    return config;
}

void AppendJsonField(std::vector<std::wstring>& lines, const std::wstring& key, const std::wstring& value, bool& first) {
    if (value.empty()) {
        return;
    }
    const std::wstring prefix = first ? L"  " : L"  ";
    lines.push_back(prefix + L"\"" + key + L"\": \"" + JsonEscape(value) + L"\"");
    first = false;
}

void AppendJsonField(std::vector<std::wstring>& lines, const std::wstring& key, const fs::path& value, bool& first) {
    if (value.empty()) {
        return;
    }
    AppendJsonField(lines, key, value.generic_wstring(), first);
}

void AppendJsonField(std::vector<std::wstring>& lines, const std::wstring& key, int value, bool& first) {
    const std::wstring prefix = first ? L"  " : L"  ";
    lines.push_back(prefix + L"\"" + key + L"\": " + std::to_wstring(value));
    first = false;
}

void AppendJsonField(std::vector<std::wstring>& lines, const std::wstring& key, bool value, bool& first) {
    const std::wstring prefix = first ? L"  " : L"  ";
    lines.push_back(prefix + L"\"" + key + L"\": " + (value ? L"true" : L"false"));
    first = false;
}

std::wstring AddCommas(const std::vector<std::wstring>& rawLines) {
    std::wostringstream out;
    out << L"{\n";
    for (size_t i = 0; i < rawLines.size(); ++i) {
        out << rawLines[i];
        if (i + 1 < rawLines.size()) {
            out << L",";
        }
        out << L"\n";
    }
    out << L"}\n";
    return out.str();
}

bool SaveConfig(const fs::path& configPath, const Config& config) {
    std::vector<std::wstring> fields;
    bool first = true;
    AppendJsonField(fields, L"projectRoot", config.projectRoot, first);
    AppendJsonField(fields, L"inputPath", config.inputPath, first);
    AppendJsonField(fields, L"objDir", config.objDir, first);
    AppendJsonField(fields, L"glbDir", config.glbDir, first);
    AppendJsonField(fields, L"worldDir", config.worldDir, first);
    AppendJsonField(fields, L"savesDir", config.savesDir, first);
    AppendJsonField(fields, L"mcRoot", config.mcRoot, first);
    AppendJsonField(fields, L"testWorldRoot", config.testWorldRoot, first);
    AppendJsonField(fields, L"copyTo", config.copyTo, first);
    AppendJsonField(fields, L"worldName", config.worldName, first);
    AppendJsonField(fields, L"mcVersion", config.mcVersion, first);
    AppendJsonField(fields, L"onlyFile", config.onlyFile, first);
    AppendJsonField(fields, L"center", config.center, first);
    AppendJsonField(fields, L"centerX", config.centerX, first);
    AppendJsonField(fields, L"centerY", config.centerY, first);
    AppendJsonField(fields, L"centerZ", config.centerZ, first);
    AppendJsonField(fields, L"levelNormal", config.levelNormal, first);
    AppendJsonField(fields, L"lang", config.lang, first);
    AppendJsonField(fields, L"lastMode", config.lastMode, first);
    AppendJsonField(fields, L"lastSafety", config.lastSafety, first);
    AppendJsonField(fields, L"rotateYDeg", config.rotateYDeg, first);
    AppendJsonField(fields, L"rotationMode", config.rotationMode, first);
    AppendJsonField(fields, L"rotationW", config.rotationW, first);
    AppendJsonField(fields, L"rotationX", config.rotationX, first);
    AppendJsonField(fields, L"rotationY", config.rotationY, first);
    AppendJsonField(fields, L"rotationZ", config.rotationZ, first);
    AppendJsonField(fields, L"baseY", config.baseY, first);
    AppendJsonField(fields, L"scaleBlocksPerMeter", config.scaleBlocksPerMeter, first);
    AppendJsonField(fields, L"fallbackBlock", config.fallbackBlock, first);
    AppendJsonField(fields, L"blockTypes", config.blockTypes, first);
    AppendJsonField(fields, L"blocks", config.blocks, first);
    AppendJsonField(fields, L"excludeBlockTypes", config.excludeBlockTypes, first);
    AppendJsonField(fields, L"excludeBlocks", config.excludeBlocks, first);
    AppendJsonField(fields, L"palette", config.palette, first);
    AppendJsonField(fields, L"transform", config.transform, first);
    AppendJsonField(fields, L"block", config.block, first);
    AppendJsonField(fields, L"clipReferenceObj", config.clipReferenceObj, first);
    AppendJsonField(fields, L"clipBelowMeters", config.clipBelowMeters, first);
    AppendJsonField(fields, L"clipCellSize", config.clipCellSize, first);
    AppendJsonField(fields, L"clipLowFraction", config.clipLowFraction, first);
    AppendJsonField(fields, L"clipPasses", config.clipPasses, first);
    AppendJsonField(fields, L"clipTrimSigma", config.clipTrimSigma, first);
    AppendJsonField(fields, L"dedupeSampleSteps", config.dedupeSampleSteps, first);
    AppendJsonField(fields, L"componentMinRatio", config.componentMinRatio, first);
    AppendJsonField(fields, L"componentBelowGap", config.componentBelowGap, first);
    AppendJsonField(fields, L"minY", config.minY, first);
    AppendJsonField(fields, L"maxY", config.maxY, first);
    AppendJsonField(fields, L"previewPresetA", config.previewPresetA, first);
    AppendJsonField(fields, L"previewPresetB", config.previewPresetB, first);
    AppendJsonField(fields, L"previewPresetC", config.previewPresetC, first);
    AppendJsonField(fields, L"previewWorldStyle", config.previewWorldStyle, first);
    AppendJsonField(fields, L"previewRenderer", config.previewRenderer.empty() ? L"auto" : config.previewRenderer, first);
    AppendJsonField(fields, L"previewOrbitSensitivity", config.previewOrbitSensitivity, first);
    AppendJsonField(fields, L"previewPanSensitivity", config.previewPanSensitivity, first);
    AppendJsonField(fields, L"previewZoomSensitivity", config.previewZoomSensitivity, first);
    AppendJsonField(fields, L"direct", config.direct, first);
    AppendJsonField(fields, L"copyWorld", config.copyWorld, first);
    AppendJsonField(fields, L"overwrite", config.overwrite, first);
    AppendJsonField(fields, L"noGlass", config.noGlass, first);
    AppendJsonField(fields, L"all", config.allFiles, first);
    AppendJsonField(fields, L"flipX", config.flipX, first);
    AppendJsonField(fields, L"flipZ", config.flipZ, first);
    AppendJsonField(fields, L"noFlipZ", config.noFlipZ, first);
    AppendJsonField(fields, L"noComponentFilter", config.noComponentFilter, first);
    AppendJsonField(fields, L"previewWorldVisible", config.previewWorldVisible, first);
    AppendJsonField(fields, L"previewShowGrid", config.previewShowGrid, first);
    AppendJsonField(fields, L"previewShowAxes", config.previewShowAxes, first);
    AppendJsonField(fields, L"workers", config.workers, first);
    AppendJsonField(fields, L"batchBlockLimit", config.batchBlockLimit, first);
    AppendJsonField(fields, L"previewRefreshRate", NormalizePreviewRefreshRate(config.previewRefreshRate), first);
    AppendJsonField(fields, L"previewWorldRadius", config.previewWorldRadius, first);
    AppendJsonField(fields, L"previewWorldMaxBlocks", config.previewWorldMaxBlocks, first);
    return WriteWholeFileUtf8(configPath, AddCommas(fields));
}

void ApplyConfigDefaults(ParsedArgs& args, const Config& config) {
    auto setValue = [&](const std::wstring& flag, const std::wstring& value) {
        if (!value.empty() && args.values.find(flag) == args.values.end()) {
            args.values[flag] = value;
        }
    };
    auto setPath = [&](const std::wstring& flag, const fs::path& value) {
        if (!value.empty() && args.values.find(flag) == args.values.end()) {
            args.values[flag] = value.wstring();
        }
    };
    auto setFlag = [&](const std::wstring& flag, bool value) {
        if (value) {
            args.flags.insert(flag);
        }
    };

    setPath(L"--project-root", config.projectRoot);
    setPath(L"--input", config.inputPath);
    setPath(L"--obj-dir", config.objDir);
    setPath(L"--glb-dir", config.glbDir);
    setPath(L"--world-dir", config.worldDir);
    setPath(L"--saves-dir", config.savesDir);
    setPath(L"--mc-root", config.mcRoot);
    setPath(L"--test-world-root", config.testWorldRoot);
    setPath(L"--copy-to", config.copyTo);
    setValue(L"--world", config.worldName);
    setValue(L"--mc-version", config.mcVersion);
    setValue(L"--only", config.onlyFile);
    setValue(L"--center", config.center);
    setValue(L"--center-x", config.centerX);
    setValue(L"--center-y", config.centerY);
    setValue(L"--center-z", config.centerZ);
    setValue(L"--level-normal", config.levelNormal);
    setValue(L"--lang", config.lang);
    setValue(L"--mode", config.lastMode);
    setValue(L"--rotate-y-deg", config.rotateYDeg);
    setValue(L"--base-y", config.baseY);
    setValue(L"--scale-blocks-per-meter", config.scaleBlocksPerMeter);
    setValue(L"--fallback-block", config.fallbackBlock);
    setValue(L"--block-types", config.blockTypes);
    setValue(L"--blocks", config.blocks);
    setValue(L"--exclude-block-types", config.excludeBlockTypes);
    setValue(L"--exclude-blocks", config.excludeBlocks);
    setValue(L"--palette", config.palette);
    setValue(L"--transform", config.transform);
    setValue(L"--block", config.block);
    setValue(L"--clip-reference-obj", config.clipReferenceObj);
    setValue(L"--clip-below-meters", config.clipBelowMeters);
    setValue(L"--clip-cell-size", config.clipCellSize);
    setValue(L"--clip-low-fraction", config.clipLowFraction);
    setValue(L"--clip-passes", config.clipPasses);
    setValue(L"--clip-trim-sigma", config.clipTrimSigma);
    setValue(L"--dedupe-sample-steps", config.dedupeSampleSteps);
    setValue(L"--component-min-ratio", config.componentMinRatio);
    setValue(L"--component-below-gap", config.componentBelowGap);
    setValue(L"--min-y", config.minY);
    setValue(L"--max-y", config.maxY);
    if (config.workers > 0 && args.values.find(L"--workers") == args.values.end()) {
        args.values[L"--workers"] = std::to_wstring(config.workers);
    }
    if (config.batchBlockLimit > 0 && args.values.find(L"--batch-block-limit") == args.values.end()) {
        args.values[L"--batch-block-limit"] = std::to_wstring(config.batchBlockLimit);
    }
    if (args.flags.find(L"--direct") == args.flags.end() && args.flags.find(L"--copy-world") == args.flags.end()) {
        setFlag(L"--direct", config.direct);
        setFlag(L"--copy-world", config.copyWorld && !config.direct);
    }
    setFlag(L"--overwrite", config.overwrite);
    setFlag(L"--no-glass", config.noGlass);
    setFlag(L"--all", config.allFiles);
    setFlag(L"--flip-x", config.flipX);
    setFlag(L"--flip-z", config.flipZ);
    setFlag(L"--no-flip-z", config.noFlipZ);
    setFlag(L"--no-component-filter", config.noComponentFilter);
}

void ForceCommandMode(ParsedArgs& args) {
    if (args.command == L"obj-surface") {
        args.values[L"--mode"] = L"obj-surface";
    } else if (args.command == L"obj" || args.command == L"obj-copy" || args.command == L"obj-rotate") {
        args.values[L"--mode"] = L"obj";
    } else if (args.command == L"glb-surface") {
        args.values[L"--mode"] = L"glb-surface";
    } else if (args.command == L"glb" || args.command == L"glb-copy") {
        args.values[L"--mode"] = L"glb-textured";
    }
}

std::wstring ConfigValueForFlag(const Config& config, const std::wstring& flag) {
    if (flag == L"--only") return config.onlyFile;
    if (flag == L"--center") return config.center;
    if (flag == L"--center-x") return config.centerX;
    if (flag == L"--center-y") return config.centerY;
    if (flag == L"--center-z") return config.centerZ;
    if (flag == L"--level-normal") return config.levelNormal;
    if (flag == L"--rotate-y-deg") return config.rotateYDeg;
    if (flag == L"--base-y") return config.baseY;
    if (flag == L"--scale-blocks-per-meter" || flag == L"--scale") return config.scaleBlocksPerMeter;
    if (flag == L"--fallback-block") return config.fallbackBlock;
    if (flag == L"--block-types") return config.blockTypes;
    if (flag == L"--blocks") return config.blocks;
    if (flag == L"--exclude-block-types") return config.excludeBlockTypes;
    if (flag == L"--exclude-blocks") return config.excludeBlocks;
    if (flag == L"--palette") return config.palette;
    if (flag == L"--transform") return config.transform;
    if (flag == L"--block") return config.block;
    if (flag == L"--clip-reference-obj") return config.clipReferenceObj;
    if (flag == L"--clip-below-meters") return config.clipBelowMeters;
    if (flag == L"--clip-cell-size") return config.clipCellSize;
    if (flag == L"--clip-low-fraction") return config.clipLowFraction;
    if (flag == L"--clip-passes") return config.clipPasses;
    if (flag == L"--clip-trim-sigma") return config.clipTrimSigma;
    if (flag == L"--dedupe-sample-steps") return config.dedupeSampleSteps;
    if (flag == L"--component-min-ratio") return config.componentMinRatio;
    if (flag == L"--component-below-gap") return config.componentBelowGap;
    if (flag == L"--min-y") return config.minY;
    if (flag == L"--max-y") return config.maxY;
    return L"";
}

int ConfigIntForFlag(const Config& config, const std::wstring& flag, int fallback) {
    if (flag == L"--workers") return config.workers > 0 ? config.workers : fallback;
    if (flag == L"--batch-block-limit") return config.batchBlockLimit > 0 ? config.batchBlockLimit : fallback;
    return fallback;
}

void SetConfigValueForFlag(Config& config, const std::wstring& flag, const std::wstring& value) {
    if (flag == L"--only") config.onlyFile = value;
    else if (flag == L"--center") config.center = value;
    else if (flag == L"--center-x") config.centerX = value;
    else if (flag == L"--center-y") config.centerY = value;
    else if (flag == L"--center-z") config.centerZ = value;
    else if (flag == L"--level-normal") config.levelNormal = value;
    else if (flag == L"--rotate-y-deg") config.rotateYDeg = value;
    else if (flag == L"--base-y") config.baseY = value;
    else if (flag == L"--scale-blocks-per-meter" || flag == L"--scale") config.scaleBlocksPerMeter = value;
    else if (flag == L"--fallback-block") config.fallbackBlock = value;
    else if (flag == L"--block-types") config.blockTypes = value;
    else if (flag == L"--blocks") config.blocks = value;
    else if (flag == L"--exclude-block-types") config.excludeBlockTypes = value;
    else if (flag == L"--exclude-blocks") config.excludeBlocks = value;
    else if (flag == L"--palette") config.palette = value;
    else if (flag == L"--transform") config.transform = value;
    else if (flag == L"--block") config.block = value;
    else if (flag == L"--clip-reference-obj") config.clipReferenceObj = value;
    else if (flag == L"--clip-below-meters") config.clipBelowMeters = value;
    else if (flag == L"--clip-cell-size") config.clipCellSize = value;
    else if (flag == L"--clip-low-fraction") config.clipLowFraction = value;
    else if (flag == L"--clip-passes") config.clipPasses = value;
    else if (flag == L"--clip-trim-sigma") config.clipTrimSigma = value;
    else if (flag == L"--dedupe-sample-steps") config.dedupeSampleSteps = value;
    else if (flag == L"--component-min-ratio") config.componentMinRatio = value;
    else if (flag == L"--component-below-gap") config.componentBelowGap = value;
    else if (flag == L"--min-y") config.minY = value;
    else if (flag == L"--max-y") config.maxY = value;
}

fs::path FindBackendRootFrom(const fs::path& start) {
    if (start.empty()) {
        return {};
    }
    std::error_code ec;
    fs::path current = fs::absolute(start, ec);
    if (current.empty()) {
        current = start;
    }
    if (HasNativeResourceRoot(current)) {
        return current;
    }
    for (fs::path probe = current; !probe.empty(); probe = probe.parent_path()) {
        if (HasNativeResourceRoot(probe)) {
            return probe;
        }
        if (probe == probe.root_path()) {
            break;
        }
    }
    return {};
}

BackendLayout ResolveBackendLayout(const fs::path& exeDir, const Config& config, const ParsedArgs& args) {
    std::vector<fs::path> candidates;
    auto add = [&](const fs::path& value) {
        if (!value.empty()) {
            candidates.push_back(value);
        }
    };
    auto it = args.values.find(L"--project-root");
    if (it != args.values.end()) {
        add(it->second);
    }
    add(exeDir / "backend");
    add(exeDir);
    add(fs::current_path());
    add(exeDir / "..");
    add(fs::current_path() / "3dmodel-to-minecraft");
    add(exeDir / "3dmodel-to-minecraft");
    if (!config.projectRoot.empty()) {
        add(config.projectRoot);
    }

    BackendLayout layout;
    for (const auto& candidate : candidates) {
        const fs::path root = FindBackendRootFrom(candidate);
        if (root.empty()) {
            continue;
        }
        layout.root = NormalizeIfExists(root);
        return layout;
    }
    return layout;
}

std::wstring DetectMode(const ParsedArgs& args, const Config& config) {
    auto readMode = [&](const std::wstring& key) -> std::wstring {
        auto it = args.values.find(key);
        if (it != args.values.end()) {
            return ToLower(it->second);
        }
        return L"";
    };
    std::wstring mode = readMode(L"--input-type");
    if (mode.empty()) {
        mode = readMode(L"--mode");
    }
    if (mode.empty()) {
        if (args.command == L"obj-surface") {
            mode = L"obj-surface";
        } else if (args.command == L"obj" || args.command == L"obj-copy" || args.command == L"obj-rotate") {
            mode = L"obj";
        } else if (args.command == L"glb-surface") {
            mode = L"glb-surface";
        } else if (args.command == L"glb" || args.command == L"glb-copy") {
            mode = L"glb-textured";
        }
    }

    auto inputIt = args.values.find(L"--input");
    if (mode.empty() && inputIt != args.values.end()) {
        fs::path inputPath = inputIt->second;
        if (IEquals(inputPath.extension().wstring(), L".obj")) {
            mode = L"obj";
        } else if (IEquals(inputPath.extension().wstring(), L".glb")) {
            mode = L"glb-textured";
        }
    }

    if (mode.empty() && !config.lastMode.empty()) {
        mode = ToLower(config.lastMode);
    }
    if (mode.empty()) {
        mode = L"glb-textured";
    }
    if (mode == L"glb") {
        mode = L"glb-textured";
    }
    return mode;
}

bool IsObjMode(const std::wstring& mode) {
    return mode == L"obj" || mode == L"obj-surface";
}

std::wstring DetectSafety(const ParsedArgs& args, const Config& config) {
    if (args.flags.count(L"--direct")) {
        return L"direct";
    }
    if (args.flags.count(L"--copy-world")) {
        return L"copy-world";
    }
    if (!config.lastSafety.empty()) {
        const std::wstring savedSafety = ToLower(config.lastSafety);
        return savedSafety;
    }
    return L"copy-world";
}

fs::path ResolveWorldDir(const BackendLayout& backend, const Config& config, const ParsedArgs& args, std::wstring* chosenWorldName = nullptr) {
    auto worldDirIt = args.values.find(L"--world-dir");
    if (worldDirIt != args.values.end()) {
        fs::path value = NormalizeIfExists(worldDirIt->second);
        if (chosenWorldName && !value.empty()) {
            *chosenWorldName = value.filename().wstring();
        }
        return value;
    }
    if (!config.worldDir.empty() && IsReadableWorldDir(config.worldDir)) {
        fs::path value = NormalizeIfExists(config.worldDir);
        if (chosenWorldName) {
            *chosenWorldName = value.filename().wstring();
        }
        return value;
    }

    std::wstring worldName;
    auto worldNameIt = args.values.find(L"--world");
    if (worldNameIt != args.values.end()) {
        worldName = worldNameIt->second;
    } else if (!config.worldName.empty()) {
        worldName = config.worldName;
    }

    for (const auto& savesDir : CandidateSavesDirs(backend.root, config, args)) {
        std::vector<fs::path> worlds = CollectWorldsFromSaves(savesDir);
        if (worlds.empty()) {
            continue;
        }
        if (!worldName.empty()) {
            for (const auto& world : worlds) {
                const std::wstring name = world.filename().wstring();
                if (IEquals(name, worldName) || ToLower(name).find(ToLower(worldName)) != std::wstring::npos) {
                    if (chosenWorldName) {
                        *chosenWorldName = name;
                    }
                    return NormalizeIfExists(world);
                }
            }
        }
        if (chosenWorldName) {
            *chosenWorldName = worlds.front().filename().wstring();
        }
        return NormalizeIfExists(worlds.front());
    }
    return {};
}

fs::path ResolveTestWorldRoot(const fs::path& exeDir, const BackendLayout& backend, const Config& config, const ParsedArgs& args) {
    auto normalizeForCompare = [&](const fs::path& value) -> std::wstring {
        std::error_code ec;
        fs::path absoluteValue = fs::absolute(value, ec);
        if (absoluteValue.empty()) absoluteValue = value;
        return ToLower(absoluteValue.lexically_normal().wstring());
    };
    auto normalizeLegacyCppTestWorldRoot = [&](const fs::path& value) -> fs::path {
        if (value.empty() || backend.root.empty()) {
            return value;
        }
        if (!IEquals(backend.root.filename().wstring(), L"cpp")) {
            return value;
        }
        const fs::path expectedLegacy = backend.root / "cpp" / "test-worlds";
        if (normalizeForCompare(value) == normalizeForCompare(expectedLegacy)) {
            return backend.root / "test-worlds";
        }
        return value;
    };

    auto it = args.values.find(L"--test-world-root");
    if (it != args.values.end()) {
        return NormalizeIfExists(it->second);
    }
    if (!config.testWorldRoot.empty()) {
        return NormalizeIfExists(normalizeLegacyCppTestWorldRoot(config.testWorldRoot));
    }
    const fs::path normalizedExeDir = NormalizeIfExists(exeDir);
    const fs::path normalizedBackendRoot = NormalizeIfExists(backend.root);
    if (!backend.root.empty() && normalizedBackendRoot == NormalizeIfExists(exeDir / "backend")) {
        return exeDir / "test-worlds";
    }
    if (!backend.root.empty() && normalizedBackendRoot == normalizedExeDir) {
        return exeDir / "test-worlds";
    }
    if (!backend.root.empty()) {
        if (IEquals(backend.root.filename().wstring(), L"cpp")) {
            return backend.root / "test-worlds";
        }
        return backend.root / "cpp" / "test-worlds";
    }
    return exeDir / "test-worlds";
}

std::optional<std::wstring> FirstFileNameByExtension(const fs::path& dirPath, const std::wstring& extension) {
    const auto files = CollectFilesByExtension(dirPath, extension);
    if (!files.empty()) {
        return files.front().filename().wstring();
    }
    return std::nullopt;
}

std::optional<ImportPlan> MakeImportPlan(const fs::path& exeDir, const BackendLayout& backend, const Config& config,
                                         const ParsedArgs& args, std::wstring* errorText = nullptr) {
    ImportPlan plan;
    plan.mode = DetectMode(args, config);
    plan.safetyMode = DetectSafety(args, config);
    plan.originalWorldDir = ResolveWorldDir(backend, config, args);
    if (!IsReadableWorldDir(plan.originalWorldDir)) {
        if (errorText) *errorText = L"目标存档无效或缺少 level.dat: " + plan.originalWorldDir.wstring();
        return std::nullopt;
    }
    plan.allFiles = args.flags.count(L"--all") > 0;

    fs::path rawInput;
    auto inputIt = args.values.find(L"--input");
    if (inputIt != args.values.end()) {
        rawInput = inputIt->second;
    } else if (IsObjMode(plan.mode)) {
        auto objIt = args.values.find(L"--obj-dir");
        if (objIt != args.values.end()) {
            rawInput = objIt->second;
        } else if (!config.objDir.empty()) {
            rawInput = config.objDir;
        }
    } else {
        auto glbIt = args.values.find(L"--glb-dir");
        if (glbIt != args.values.end()) {
            rawInput = glbIt->second;
        } else if (!config.glbDir.empty()) {
            rawInput = config.glbDir;
        }
    }

    if (rawInput.empty() && !args.positionals.empty()) {
        fs::path maybePath = args.positionals.front();
        std::error_code ec;
        if (fs::exists(maybePath, ec)) {
            rawInput = maybePath;
        }
    }

    if (rawInput.empty()) {
        const std::wstring modeBase = IsObjMode(plan.mode) ? L"obj" : L"glb-textured";
        const auto candidates = AutoDetectInputDirs(backend.root, modeBase == L"obj" ? L"obj" : L"glb", config, args);
        for (const auto& candidate : candidates) {
            if (HasDir(candidate)) {
                rawInput = candidate;
                break;
            }
        }
    }

    if (rawInput.empty()) {
        if (errorText) *errorText = L"没有找到模型输入路径。";
        return std::nullopt;
    }

    rawInput = NormalizeIfExists(rawInput);
    if (HasFile(rawInput)) {
        plan.allFiles = false;
        const std::wstring ext = ToLower(rawInput.extension().wstring());
        if (ext == L".obj") {
            if (plan.mode != L"obj-surface") plan.mode = L"obj";
        } else if (ext == L".glb" && plan.mode != L"glb-surface") {
            plan.mode = L"glb-textured";
        }
        plan.sourceDir = rawInput.parent_path();
        plan.onlyFile = rawInput.filename().wstring();
    } else {
        plan.sourceDir = rawInput;
        auto onlyIt = args.values.find(L"--only");
        if (onlyIt != args.values.end()) plan.onlyFile = Trim(onlyIt->second);
    }

    if (!HasDir(plan.sourceDir)) {
        if (errorText) *errorText = L"模型目录不存在: " + plan.sourceDir.wstring();
        return std::nullopt;
    }
    if (plan.allFiles && !plan.onlyFile.empty()) {
        if (errorText) *errorText = L"--all 与 --only 不能同时使用。";
        return std::nullopt;
    }
    if (!plan.allFiles && plan.onlyFile.empty()) {
        if (errorText) *errorText = L"目录输入必须明确指定 --only <文件名> 或 --all。";
        return std::nullopt;
    }

    if (IsObjMode(plan.mode)) {
        if (plan.onlyFile.empty()) {
            auto first = FirstFileNameByExtension(plan.sourceDir, L".obj");
            if (!first) {
                if (errorText) *errorText = L"OBJ 目录中没有 .obj 文件。";
                return std::nullopt;
            }
        }
    } else {
        if (plan.onlyFile.empty()) {
            auto first = FirstFileNameByExtension(plan.sourceDir, L".glb");
            if (!first) {
                if (errorText) *errorText = L"GLB 目录中没有 .glb 文件。";
                return std::nullopt;
            }
        }
    }
    if (!plan.onlyFile.empty() && !HasFile(plan.sourceDir / fs::path(plan.onlyFile))) {
        if (errorText) *errorText = L"--only 指定的模型不存在: " + (plan.sourceDir / fs::path(plan.onlyFile)).wstring();
        return std::nullopt;
    }

    if (plan.safetyMode == L"copy-world") {
        plan.worldDir.clear();
        plan.copiedWorldDir.clear();
    } else {
        plan.worldDir = plan.originalWorldDir;
    }
    (void)exeDir;
    return plan;
}

std::wstring TimestampForName() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buffer[32];
    swprintf_s(buffer, L"%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buffer;
}

bool EnsureWritableDirectory(const fs::path& dirPath) {
    std::error_code ec;
    fs::create_directories(dirPath, ec);
    if (ec) {
        return false;
    }
    const fs::path probe = dirPath / ".write-test.tmp";
    std::ofstream out(probe, std::ios::binary);
    if (!out) {
        return false;
    }
    out << "ok";
    out.close();
    fs::remove(probe, ec);
    return true;
}

std::uint64_t DirectorySizeBytes(const fs::path& root) {
    std::uint64_t total = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec) || IEquals(it->path().filename().wstring(), L"session.lock")) continue;
        total += static_cast<std::uint64_t>(it->file_size(ec));
        ec.clear();
    }
    return total;
}

std::uint64_t AvailableDiskBytes(const fs::path& path) {
    ULARGE_INTEGER available{};
    return GetDiskFreeSpaceExW(path.c_str(), &available, nullptr, nullptr)
        ? static_cast<std::uint64_t>(available.QuadPart) : 0;
}

bool CopyDirectoryRecursive(const fs::path& source, const fs::path& target, std::wstring& errorText) {
    std::error_code ec;
    if (!IsReadableWorldDir(source)) {
        errorText = L"Source world is missing level.dat.";
        return false;
    }
    if (fs::exists(target, ec)) {
        errorText = L"Target world already exists.";
        return false;
    }
    fs::create_directories(target, ec);
    if (ec) {
        errorText = L"Failed to create target folder.";
        return false;
    }

    for (fs::recursive_directory_iterator it(source, ec), end; it != end && !ec; it.increment(ec)) {
        const fs::path rel = fs::relative(it->path(), source, ec);
        if (ec) {
            break;
        }
        const fs::path dest = target / rel;
        if (it->is_directory()) {
            fs::create_directories(dest, ec);
            if (ec) {
                break;
            }
            continue;
        }
        if (!it->is_regular_file()) {
            continue;
        }
        if (IEquals(it->path().filename().wstring(), L"session.lock")) {
            continue;
        }
        fs::create_directories(dest.parent_path(), ec);
        if (ec) {
            break;
        }
        fs::copy_file(it->path(), dest, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            break;
        }
    }

    if (ec) {
        errorText = Utf8ToWide(ec.message());
        return false;
    }
    return true;
}

fs::path MakeCopiedWorldPath(const fs::path& sourceWorld, const fs::path& targetRoot) {
    const std::wstring baseName = sourceWorld.filename().wstring();
    return targetRoot / fs::path(baseName + L"_cpp_test_" + TimestampForName());
}

DWORD RunProcess(const fs::path& application, const fs::path& workingDir, const std::vector<std::wstring>& arguments) {
    std::wstring cmdline = QuoteForWindows(application.wstring());
    for (const auto& arg : arguments) {
        cmdline += L" ";
        cmdline += QuoteForWindows(arg);
    }

    std::vector<wchar_t> buffer(cmdline.begin(), cmdline.end());
    buffer.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(
        application.wstring().c_str(),
        buffer.data(),
        nullptr,
        nullptr,
        TRUE,
        0,
        nullptr,
        workingDir.empty() ? nullptr : workingDir.wstring().c_str(),
        &si,
        &pi);
    if (!ok) {
        return static_cast<DWORD>(GetLastError());
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return exitCode;
}

void PrintHelp(Lang lang, const fs::path& exePath) {
    PrintLine(T(lang, L"3dmodel-to-minecraft 原生 C++ 工具", L"3dmodel-to-minecraft native C++ tool"));
    PrintLine(T(lang, L"用于把 OBJ / GLB 模型导入 Minecraft Java 存档，并提供安全副本、扫描和诊断功能。", L"Import OBJ / GLB models into Minecraft Java saves with safe copies, scanning, and diagnostics."));
    PrintLine();
    PrintLine(T(lang, L"常用命令：", L"Common commands:"));
    PrintLine(L"  " + exePath.filename().wstring() + L" doctor");
    PrintLine(L"  " + exePath.filename().wstring() + L" setup");
    PrintLine(L"  " + exePath.filename().wstring() + L" import --input \"C:\\path\\to\\tile.obj\" --world-dir \"C:\\path\\to\\world\" --copy-world --yes");
    PrintLine(L"  " + exePath.filename().wstring() + L" copy-world --world-dir \"...\"");
    PrintLine(L"  " + exePath.filename().wstring() + L" scan --world-dir \"...\"");
    PrintLine();
    PrintLine(T(lang, L"泛用输入参数：", L"General input parameters:"));
    PrintLine(T(lang, L"  --input <文件或目录>      自动识别 OBJ/GLB；目录输入可配合 --all。", L"  --input <file or dir>      Auto-detect OBJ/GLB; directory input can be combined with --all."));
    PrintLine(T(lang, L"  --mode obj|obj-surface|glb-textured|glb-surface", L"  --mode obj|obj-surface|glb-textured|glb-surface"));
    PrintLine(T(lang, L"  --world-dir <世界目录> 或 --world <名字> + --mc-root/--mc-version", L"  --world-dir <world> or --world <name> + --mc-root/--mc-version"));
    PrintLine(T(lang, L"  --copy-world | --direct", L"  --copy-world | --direct"));
    PrintLine(T(lang, L"  --center --level-normal --workers --batch-block-limit --base-y --rotate-y-deg --scale-blocks-per-meter", L"  --center --level-normal --workers --batch-block-limit --base-y --rotate-y-deg --scale-blocks-per-meter"));
    PrintLine(T(lang, L"  --fallback-block <方块名>    textured GLB 无纹理采样时的回退方块", L"  --fallback-block <block>    Fallback block when textured GLB has no sampled texture color"));
    PrintLine(T(lang, L"  --block-types --blocks --exclude-block-types --exclude-blocks --no-glass", L"  --block-types --blocks --exclude-block-types --exclude-blocks --no-glass"));
    PrintLine();
    PrintLine(T(lang, L"配置文件：", L"Config file:"));
    PrintLine(T(lang, L"  setup 会把本地配置保存到 exe 同目录下的 config.local.json", L"  setup saves local settings to config.local.json next to the exe"));
    PrintLine(T(lang, L"  典型字段：", L"  Typical fields:"));
    PrintLine(L"  {");
    PrintLine(L"    \"projectRoot\": \"C:/path/to/3dmodel-to-minecraft\",");
    PrintLine(L"    \"objDir\": \"C:/path/to/obj-tiles\",");
    PrintLine(L"    \"glbDir\": \"C:/path/to/glb-output\",");
    PrintLine(L"    \"worldDir\": \"C:/path/to/.minecraft/saves/WorldName\",");
    PrintLine(L"    \"testWorldRoot\": \"C:/path/to/test-worlds\",");
    PrintLine(L"    \"lastMode\": \"glb-textured\",");
    PrintLine(L"    \"lastSafety\": \"copy-world\",");
    PrintLine(L"    \"workers\": " + std::to_wstring(DefaultWorkerCount()) + L",");
    PrintLine(L"    \"batchBlockLimit\": 1500000,");
    PrintLine(L"    \"previewRenderer\": \"auto\",");
    PrintLine(L"    \"previewRefreshRate\": 60,");
    PrintLine(L"    \"lang\": \"en\"");
    PrintLine(L"  }");
    PrintLine();
    PrintLine(T(lang, L"说明：", L"Notes:"));
    PrintLine(T(lang, L"  1. 默认安全模式是 copy-world。", L"  1. The default safety mode is copy-world."));
    PrintLine(T(lang, L"  2. `--input` 支持文件路径，所以把模型文件拖到 exe 上也能触发导入命令。", L"  2. `--input` accepts file paths, so drag-and-drop onto the exe also works."));
    PrintLine(T(lang, L"  3. 调色板筛选可使用 --palette 及方块包含/排除参数。", L"  3. Palette filtering is available through --palette and block include/exclude options."));
}

bool PrintDoctor(Lang lang, const fs::path& exeDir, const fs::path& configPath, const Config& config, const ParsedArgs& args, const BackendLayout& backend) {
    PrintLine(T(lang, L"诊断结果", L"Doctor report"));
    PrintLine(L"  exe           : " + PathToDisplay(exeDir));
    PrintLine(L"  config        : " + PathToDisplay(configPath) + (HasFile(configPath) ? T(lang, L" (已存在)", L" (present)") : T(lang, L" (未创建)", L" (missing)")));
    PrintLine(L"  resource root : " + (backend.root.empty() ? T(lang, L"(未找到)", L"(missing)") : PathToDisplay(backend.root)));

    bool ok = true;
    if (backend.root.empty() || !HasNativeResourceRoot(backend.root)) {
        PrintLine(T(lang, L"  找不到原生资源：请确认 vendor/ots/res 存在。", L"  Native resources are missing: make sure vendor/ots/res exists."));
        ok = false;
    }

    std::wstring worldName;
    const fs::path worldDir = ResolveWorldDir(backend, config, args, &worldName);
    PrintLine(L"  world         : " + (worldDir.empty() ? T(lang, L"(未解析)", L"(unresolved)") : PathToDisplay(worldDir)));
    if (!worldDir.empty() && !IsReadableWorldDir(worldDir)) {
        PrintLine(T(lang, L"  世界目录存在但缺少 level.dat。", L"  The world directory exists but is missing level.dat."));
        ok = false;
    }

    const auto objCandidates = AutoDetectInputDirs(backend.root, L"obj", config, args);
    for (const auto& dirPath : objCandidates) {
        if (HasDir(dirPath)) {
            PrintLine(L"  obj dir       : " + PathToDisplay(dirPath) + L" (" + std::to_wstring(CollectFilesByExtension(dirPath, L".obj").size()) + L" obj)");
            break;
        }
    }
    const auto glbCandidates = AutoDetectInputDirs(backend.root, L"glb", config, args);
    for (const auto& dirPath : glbCandidates) {
        if (HasDir(dirPath)) {
            PrintLine(L"  glb dir       : " + PathToDisplay(dirPath) + L" (" + std::to_wstring(CollectFilesByExtension(dirPath, L".glb").size()) + L" glb)");
            break;
        }
    }

    const fs::path testWorldRoot = ResolveTestWorldRoot(exeDir, backend, config, args);
    PrintLine(L"  test worlds   : " + PathToDisplay(testWorldRoot));
    if (!EnsureWritableDirectory(testWorldRoot)) {
        PrintLine(T(lang, L"  测试存档目录不可写。", L"  The test-world directory is not writable."));
        ok = false;
    }

    PrintLine(ok ? T(lang, L"诊断通过。", L"Doctor passed.") : T(lang, L"诊断未通过。", L"Doctor did not pass."));
    return ok;
}

std::vector<std::wstring> BuildImportArgs(const ImportPlan& plan, const Config& config, const ParsedArgs& args) {
    std::vector<std::wstring> result;
    auto addFlag = [&](const std::wstring& flag) {
        if (!args.flags.count(flag)) {
            result.push_back(flag);
        }
    };
    auto addKnownValue = [&](const std::wstring& flag, const std::wstring& fallback) {
        auto it = args.values.find(flag);
        if (it != args.values.end() && !it->second.empty()) {
            result.push_back(flag);
            result.push_back(it->second);
        } else if (!fallback.empty()) {
            result.push_back(flag);
            result.push_back(fallback);
        }
    };

    if (plan.onlyFile.empty()) {
        result.push_back(L"--all");
    } else {
        result.push_back(L"--only");
        result.push_back(plan.onlyFile);
    }

    if (IsObjMode(plan.mode)) {
        result.push_back(L"--obj-dir");
        result.push_back(plan.sourceDir.wstring());
        result.push_back(L"--world-dir");
        result.push_back(plan.worldDir.wstring());
        addFlag(L"--overwrite");
        if (!args.values.count(L"--palette")) {
            result.push_back(L"--palette");
            result.push_back(L"scene");
        }
        if (!args.values.count(L"--transform")) {
            result.push_back(L"--transform");
            result.push_back(L"obj-z-up");
        }
        addKnownValue(L"--workers", std::to_wstring(config.workers > 0 ? config.workers : 1));
        addKnownValue(L"--batch-block-limit", std::to_wstring(config.batchBlockLimit > 0 ? config.batchBlockLimit : 1500000));
        addKnownValue(L"--component-min-ratio", L"0.02");
        addKnownValue(L"--component-below-gap", L"0");
        addKnownValue(L"--dedupe-sample-steps", L"2");
        if (plan.mode == L"obj-surface") addKnownValue(L"--block", L"minecraft:light_gray_concrete");
    } else {
        result.push_back(L"--glb-dir");
        result.push_back(plan.sourceDir.wstring());
        result.push_back(L"--world-dir");
        result.push_back(plan.worldDir.wstring());
        addFlag(L"--overwrite");
        addKnownValue(L"--batch-block-limit", std::to_wstring(config.batchBlockLimit > 0 ? config.batchBlockLimit : 1500000));
        if (plan.mode == L"glb-surface") {
            addKnownValue(L"--block", L"minecraft:light_gray_concrete");
        } else {
            if (!args.values.count(L"--palette")) {
                result.push_back(L"--palette");
                result.push_back(L"scene");
            }
            addKnownValue(L"--dedupe-sample-steps", L"2");
        }
    }

    if (args.flags.count(L"--no-glass")) {
        result.push_back(L"--no-glass");
    }

    addKnownValue(L"--center", config.center);
    if (IsObjMode(plan.mode)) {
        addKnownValue(L"--level-normal", config.levelNormal);
        addKnownValue(L"--rotate-y-deg", L"");
        addKnownValue(L"--base-y", L"");
        addKnownValue(L"--scale-blocks-per-meter", L"");
        addKnownValue(L"--scale", L"");
        addKnownValue(L"--clip-reference-obj", L"");
        addKnownValue(L"--clip-below-meters", L"");
    }
    addKnownValue(L"--block-types", L"");
    addKnownValue(L"--blocks", L"");
    addKnownValue(L"--exclude-block-types", L"");
    addKnownValue(L"--exclude-blocks", L"");
    addKnownValue(L"--palette", L"");
    addKnownValue(L"--transform", L"");
    addKnownValue(L"--min-y", L"");
    addKnownValue(L"--max-y", L"");
    for (const auto& passthrough : args.passthrough) {
        result.push_back(passthrough);
    }
    return result;
}

bool RunImport(Lang lang, const fs::path& exeDir, const BackendLayout& backend, Config& config, const ParsedArgs& args) {
    std::wstring planError;
    if (!ValidateImportNumbers(args, &planError)) {
        PrintLine(T(lang, L"参数错误：", L"Invalid parameter: ") + planError);
        return false;
    }
    auto planOpt = MakeImportPlan(exeDir, backend, config, args, &planError);
    if (!planOpt) {
        PrintLine(T(lang, L"无法解析导入参数：", L"Could not resolve import parameters: ") +
                  (planError.empty() ? T(lang, L"请检查模型路径和世界目录。", L"check the model and world paths.") : planError));
        return false;
    }
    ImportPlan plan = *planOpt;
    const fs::path testWorldRoot = ResolveTestWorldRoot(exeDir, backend, config, args);

    if (plan.safetyMode == L"direct" && args.flags.count(L"--yes") && !args.values.count(L"--world-dir")) {
        PrintLine(T(lang, L"--yes --direct 必须同时显式提供 --world-dir。",
                          L"--yes --direct requires an explicit --world-dir."));
        return false;
    }
    if (IsWorldSessionActive(plan.originalWorldDir)) {
        PrintLine(T(lang, L"存档正在被 Minecraft 或服务器占用。请先关闭世界再导入。",
                          L"The world is currently locked by Minecraft or a server. Close it before importing."));
        return false;
    }
    int worldDataVersion = 0;
    std::string versionError;
    if (native_mc::ReadWorldDataVersion(plan.originalWorldDir, &worldDataVersion, &versionError)) {
        PrintLine(L"  DataVersion: " + std::to_wstring(worldDataVersion) + L" (writer " + std::to_wstring(native_mc::kDataVersion) + L")");
        if (worldDataVersion != native_mc::kDataVersion && !args.flags.count(L"--force-version")) {
            PrintLine(T(lang,
                L"存档版本与写入器不一致，已拒绝写入。确认兼容风险后可显式添加 --force-version。",
                L"The save DataVersion does not match the writer. Writing was refused; use --force-version only if you accept the compatibility risk."));
            return false;
        }
    } else if (!args.flags.count(L"--force-version")) {
        PrintLine(T(lang, L"无法验证存档版本：", L"Could not verify the save version: ") + Utf8ToWide(versionError));
        return false;
    }

    const std::wstring extension = IsObjMode(plan.mode) ? L".obj" : L".glb";
    const std::size_t fileCount = plan.allFiles ? CollectFilesByExtension(plan.sourceDir, extension).size() : 1;
    PrintLine(T(lang, L"导入计划", L"Import plan"));
    PrintLine(L"  mode      : " + plan.mode);
    PrintLine(L"  models    : " + std::to_wstring(fileCount) +
              (plan.allFiles ? L" (all)" : L" (" + plan.onlyFile + L")"));
    PrintLine(L"  source    : " + plan.sourceDir.wstring());
    PrintLine(L"  world     : " + plan.originalWorldDir.wstring());
    PrintLine(L"  safety    : " + plan.safetyMode);
    PrintLine(L"  center    : " + (args.values.count(L"--center") ? args.values.at(L"--center") : config.center));
    PrintLine(L"  rotation  : " + ConfigRotationSummary(config,
        args.values.count(L"--rotate-y-deg") ? args.values.at(L"--rotate-y-deg") : L""));
    PrintLine(L"  scale     : " + (args.values.count(L"--scale-blocks-per-meter") ? args.values.at(L"--scale-blocks-per-meter")
        : (args.values.count(L"--scale") ? args.values.at(L"--scale") : L"4")));
    PrintLine(L"  flip X/Z  : " + std::wstring(args.flags.count(L"--flip-x") ? L"on" : L"off") + L"/" +
              (args.flags.count(L"--flip-z") ? L"on" : L"off"));
    PrintLine(L"  overwrite : " + std::wstring(args.flags.count(L"--overwrite") ? L"touched blocks" : L"air only"));

    if (plan.safetyMode == L"copy-world") {
        fs::path copyTo;
        auto copyIt = args.values.find(L"--copy-to");
        if (copyIt != args.values.end()) {
            copyTo = NormalizeIfExists(copyIt->second);
        } else {
            copyTo = MakeCopiedWorldPath(plan.originalWorldDir, testWorldRoot);
        }
        std::wstring copyError;
        if (!EnsureWritableDirectory(copyTo.parent_path())) {
            PrintLine(T(lang, L"测试存档目录不可写。", L"The test-world directory is not writable."));
            return false;
        }
        const std::uint64_t sourceBytes = DirectorySizeBytes(plan.originalWorldDir);
        const std::uint64_t availableBytes = AvailableDiskBytes(copyTo.parent_path());
        PrintLine(L"  copy size : " + std::to_wstring(sourceBytes / 1024 / 1024) + L" MiB");
        if (availableBytes > 0 && sourceBytes > 0 && availableBytes < sourceBytes + sourceBytes / 10) {
            PrintLine(T(lang, L"可用磁盘空间不足，至少需要源存档大小的 110%。",
                              L"Not enough free space; at least 110% of the source save size is required."));
            return false;
        }
        if (!args.flags.count(L"--yes") && !Confirm(T(lang,
            (L"将复制测试存档并写入：\n源存档: " + plan.originalWorldDir.wstring() + L"\n测试存档: " + copyTo.wstring() + L"\n输入 YES 继续：").c_str(),
            (L"A copied test world will be created and written:\nSource: " + plan.originalWorldDir.wstring() + L"\nCopy: " + copyTo.wstring() + L"\nType YES to continue: ").c_str()))) {
            PrintLine(T(lang, L"已取消。", L"Cancelled."));
            return false;
        }
        if (!CopyDirectoryRecursive(plan.originalWorldDir, copyTo, copyError)) {
            std::error_code renameError;
            if (fs::exists(copyTo, renameError)) {
                fs::rename(copyTo, fs::path(copyTo.wstring() + L".incomplete"), renameError);
            }
            PrintLine(T(lang, L"复制测试存档失败：", L"Failed to copy test world: ") + copyError);
            return false;
        }
        plan.worldDir = copyTo;
        plan.copiedWorldDir = copyTo;
    } else {
        if (!args.flags.count(L"--yes") &&
            !Confirm(T(lang,
                (L"将直接写入这个世界：\n" + plan.worldDir.wstring() + L"\n输入 YES 继续：").c_str(),
                (L"This will write directly into this world:\n" + plan.worldDir.wstring() + L"\nType YES to continue: ").c_str()))) {
            PrintLine(T(lang, L"已取消。", L"Cancelled."));
            return false;
        }
    }

    auto parseIntValue = [&](const std::wstring& flag, int fallback) -> int {
        auto it = args.values.find(flag);
        if (it == args.values.end() || it->second.empty()) {
            return fallback;
        }
        try {
            return std::stoi(it->second);
        } catch (...) {
            return fallback;
        }
    };
    auto parseCommonDouble = [&](const std::wstring& flag, double fallback) -> double {
        auto it = args.values.find(flag);
        return (it == args.values.end() || it->second.empty()) ? fallback : std::stod(it->second);
    };
    auto parseConfigDouble = [](const std::wstring& text, double fallback) {
        try { return text.empty() ? fallback : std::stod(text); } catch (...) { return fallback; }
    };
    auto modelRotationFor = [&](double effectiveY) -> std::optional<std::array<double, 4>> {
        const std::wstring rotationMode = ToLower(config.rotationMode);
        std::array<double, 4> q{};
        if (rotationMode == L"quaternion") {
            q = {
                parseConfigDouble(config.rotationW, 1.0), parseConfigDouble(config.rotationX, 0.0),
                parseConfigDouble(config.rotationY, 0.0), parseConfigDouble(config.rotationZ, 0.0)
            };
        } else {
            const double x = parseConfigDouble(config.rotationX, 0.0);
            const double z = parseConfigDouble(config.rotationZ, 0.0);
            if (std::abs(x) < 1e-12 && std::abs(z) < 1e-12) return std::nullopt;
            constexpr double halfRadians = 3.14159265358979323846 / 360.0;
            const double cx = std::cos(x * halfRadians), sx = std::sin(x * halfRadians);
            const double cy = std::cos(effectiveY * halfRadians), sy = std::sin(effectiveY * halfRadians);
            const double cz = std::cos(z * halfRadians), sz = std::sin(z * halfRadians);
            q = { cz * cy * cx + sz * sy * sx, cz * cy * sx - sz * sy * cx,
                  cz * sy * cx + sz * cy * sx, sz * cy * cx - cz * sy * sx };
        }
        const double length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        if (!std::isfinite(length) || length < 1e-12) return std::array<double, 4>{ 1.0, 0.0, 0.0, 0.0 };
        for (double& value : q) value /= length;
        return q;
    };

    auto parseCenterValue = [&](const std::wstring& raw, native_mc::Center3i* center) {
        if (!center || raw.empty()) {
            return;
        }
        std::wstringstream stream(raw);
        std::wstring part;
        std::vector<int> values;
        while (std::getline(stream, part, L',')) {
            part = Trim(part);
            if (part.empty()) {
                return;
            }
            try {
                values.push_back(std::stoi(part));
            } catch (...) {
                return;
            }
        }
        if (values.size() == 2) {
            center->x = values[0];
            center->z = values[1];
        } else if (values.size() == 3) {
            center->x = values[0];
            center->y = values[1];
            center->z = values[2];
        }
    };

    auto applyCenterFlags = [&](native_mc::Center3i* center) {
        if (!center) {
            return;
        }
        auto readOne = [&](const std::wstring& flag, std::optional<int>* target) {
            auto it = args.values.find(flag);
            if (it == args.values.end() || it->second.empty()) {
                return;
            }
            try {
                *target = std::stoi(it->second);
            } catch (...) {
            }
        };
        readOne(L"--center-x", &center->x);
        readOne(L"--center-y", &center->y);
        readOne(L"--center-z", &center->z);
    };

    if (plan.mode == L"glb-textured") {
        native_mc::GlbTexturedOptions nativeOptions;
        nativeOptions.projectRoot = backend.root;
        nativeOptions.glbDir = plan.sourceDir;
        nativeOptions.worldDir = plan.worldDir;
        nativeOptions.onlyFile = plan.onlyFile.empty() ? fs::path() : fs::path(plan.onlyFile);
        nativeOptions.allFiles = plan.allFiles;
        nativeOptions.overwrite = args.flags.count(L"--overwrite") > 0;
        nativeOptions.batchBlockLimit = parseIntValue(L"--batch-block-limit", config.batchBlockLimit > 0 ? config.batchBlockLimit : 1500000);
        nativeOptions.baseY = parseIntValue(L"--base-y", -32);
        nativeOptions.scaleBlocksPerMeter = parseCommonDouble(L"--scale-blocks-per-meter", parseCommonDouble(L"--scale", 4.0));
        nativeOptions.rotateYDeg = parseCommonDouble(L"--rotate-y-deg", 0.0);
        nativeOptions.modelRotation = modelRotationFor(nativeOptions.rotateYDeg);
        if (nativeOptions.modelRotation.has_value()) nativeOptions.rotateYDeg = 0.0;
        nativeOptions.flipX = args.flags.count(L"--flip-x") > 0;
        nativeOptions.flipZ = args.flags.count(L"--flip-z") > 0 && args.flags.count(L"--no-flip-z") == 0;
        nativeOptions.dedupeSampleSteps = parseIntValue(L"--dedupe-sample-steps", 2);
        auto fallbackIt = args.values.find(L"--fallback-block");
        if (fallbackIt != args.values.end() && !fallbackIt->second.empty()) {
            nativeOptions.fallbackBlock = WideToUtf8(fallbackIt->second);
        }
        auto paletteIt = args.values.find(L"--palette");
        if (paletteIt != args.values.end() && !paletteIt->second.empty()) {
            nativeOptions.palette.mode = WideToUtf8(paletteIt->second);
        }
        auto blockTypesIt = args.values.find(L"--block-types");
        if (blockTypesIt != args.values.end()) nativeOptions.palette.blockTypes = WideToUtf8(blockTypesIt->second);
        auto blocksIt = args.values.find(L"--blocks");
        if (blocksIt != args.values.end()) nativeOptions.palette.blocks = WideToUtf8(blocksIt->second);
        auto excludeTypesIt = args.values.find(L"--exclude-block-types");
        if (excludeTypesIt != args.values.end()) nativeOptions.palette.excludeBlockTypes = WideToUtf8(excludeTypesIt->second);
        auto excludeBlocksIt = args.values.find(L"--exclude-blocks");
        if (excludeBlocksIt != args.values.end()) nativeOptions.palette.excludeBlocks = WideToUtf8(excludeBlocksIt->second);
        nativeOptions.palette.noGlass = args.flags.count(L"--no-glass") > 0;
        const std::wstring centerText = args.values.count(L"--center") ? args.values.at(L"--center") : config.center;
        parseCenterValue(centerText, &nativeOptions.center);
        applyCenterFlags(&nativeOptions.center);

        native_mc::GlbTexturedImportResult nativeResult;
        std::string errorText;
        if (!native_mc::ImportGlbTextured(nativeOptions, &nativeResult, &errorText)) {
            PrintLine(T(lang, L"原生 GLB textured 导入失败：", L"Native GLB textured import failed: ") + Utf8ToWide(errorText));
            return false;
        }

        PrintLine(T(lang, L"原生 GLB textured 导入完成。", L"Native GLB textured import complete."));
        PrintLine(L"  world   : " + nativeResult.worldDir.wstring());
        PrintLine(L"  palette : " + std::to_wstring(nativeResult.paletteSize));
        PrintLine(L"  size    : " + std::to_wstring(nativeResult.sizeBlocks[0]) + L"x" + std::to_wstring(nativeResult.sizeBlocks[1]) + L"x" + std::to_wstring(nativeResult.sizeBlocks[2]));
        PrintLine(L"  origin  : " + std::to_wstring(nativeResult.origin[0]) + L"," + std::to_wstring(nativeResult.origin[1]) + L"," + std::to_wstring(nativeResult.origin[2]));
        for (const auto& item : nativeResult.files) {
            PrintLine(L"  [" + Utf8ToWide(item.name) + L"] triangles=" + std::to_wstring(item.triangles)
                + L" blocks=" + std::to_wstring(item.blocks)
                + L" chunks=" + std::to_wstring(item.chunks)
                + L" textures=" + std::to_wstring(item.textures)
                + L" dupSamples=" + std::to_wstring(item.duplicateSamples)
                + L" regions=" + std::to_wstring(item.regions)
                + L" written=" + std::to_wstring(item.written)
                + L" ms=" + std::to_wstring(item.milliseconds));
        }

        config.projectRoot = backend.root;
        config.glbDir = plan.sourceDir;
        config.inputPath = plan.sourceDir;
        config.worldDir = plan.originalWorldDir;
        config.worldName = plan.originalWorldDir.filename().wstring();
        config.testWorldRoot = testWorldRoot;
        config.lastMode = plan.mode;
        config.lastSafety = plan.safetyMode;
        return true;
    }

    if (IsObjMode(plan.mode)) {
        native_mc::ObjTexturedOptions nativeOptions;
        nativeOptions.projectRoot = backend.root;
        nativeOptions.cacheRoot = testWorldRoot.parent_path() / "cache";
        nativeOptions.objDir = plan.sourceDir;
        nativeOptions.worldDir = plan.worldDir;
        nativeOptions.onlyFile = plan.onlyFile.empty() ? fs::path() : fs::path(plan.onlyFile);
        nativeOptions.allFiles = plan.allFiles;
        nativeOptions.textured = plan.mode != L"obj-surface";
        nativeOptions.overwrite = args.flags.count(L"--overwrite") > 0;
        nativeOptions.batchBlockLimit = parseIntValue(L"--batch-block-limit", config.batchBlockLimit > 0 ? config.batchBlockLimit : 2000000);
        nativeOptions.workers = parseIntValue(L"--workers", std::clamp(config.workers, 1, 64));
        auto paletteIt = args.values.find(L"--palette");
        if (paletteIt != args.values.end() && !paletteIt->second.empty()) {
            nativeOptions.paletteMode = WideToUtf8(paletteIt->second);
            nativeOptions.palette.mode = nativeOptions.paletteMode;
        } else {
            nativeOptions.palette.mode = "clean";
        }
        auto transformIt = args.values.find(L"--transform");
        if (transformIt != args.values.end() && !transformIt->second.empty()) {
            nativeOptions.transform = WideToUtf8(transformIt->second);
        }
        nativeOptions.flipX = args.flags.count(L"--flip-x") > 0;
        nativeOptions.flipZ = args.flags.count(L"--flip-z") > 0 && args.flags.count(L"--no-flip-z") == 0;
        nativeOptions.baseY = parseIntValue(L"--base-y", -32);
        const std::wstring centerText = args.values.count(L"--center") ? args.values.at(L"--center") : config.center;
        parseCenterValue(centerText, &nativeOptions.center);
        applyCenterFlags(&nativeOptions.center);
        auto levelNormalIt = args.values.find(L"--level-normal");
        if (levelNormalIt != args.values.end() && !levelNormalIt->second.empty()) {
            std::wstringstream stream(levelNormalIt->second);
            std::wstring part;
            std::vector<double> values;
            while (std::getline(stream, part, L',')) {
                part = Trim(part);
                if (part.empty()) break;
                try {
                    values.push_back(std::stod(part));
                } catch (...) {
                    values.clear();
                    break;
                }
            }
            if (values.size() == 3) {
                nativeOptions.levelNormal = std::array<double, 3>{ values[0], values[1], values[2] };
            }
        }
        nativeOptions.rotateYDeg = args.values.count(L"--rotate-y-deg") ? std::stod(args.values.at(L"--rotate-y-deg")) : 0.0;
        nativeOptions.modelRotation = modelRotationFor(nativeOptions.rotateYDeg);
        if (nativeOptions.modelRotation.has_value()) nativeOptions.rotateYDeg = 0.0;
        nativeOptions.dedupeSampleSteps = parseIntValue(L"--dedupe-sample-steps", 2);
        nativeOptions.componentFilter = args.flags.count(L"--no-component-filter") == 0;
        auto parseDoubleValue = [&](const std::wstring& flag, double fallback) -> double {
            auto it = args.values.find(flag);
            if (it == args.values.end() || it->second.empty()) return fallback;
            try {
                return std::stod(it->second);
            } catch (...) {
                return fallback;
            }
        };
        nativeOptions.scaleBlocksPerMeter = parseDoubleValue(L"--scale-blocks-per-meter",
            parseDoubleValue(L"--scale", 4.0));
        nativeOptions.componentMinRatio = parseDoubleValue(L"--component-min-ratio", 0.001);
        nativeOptions.componentBelowGap = parseDoubleValue(L"--component-below-gap", 12.0);
        auto clipRefIt = args.values.find(L"--clip-reference-obj");
        if (clipRefIt != args.values.end() && !clipRefIt->second.empty()) nativeOptions.clipReferenceObj = clipRefIt->second;
        nativeOptions.clipBelowMeters = parseDoubleValue(L"--clip-below-meters", 0.0);
        nativeOptions.clipCellSize = parseDoubleValue(L"--clip-cell-size", 1.0);
        nativeOptions.clipLowFraction = parseDoubleValue(L"--clip-low-fraction", 0.25);
        nativeOptions.clipPasses = parseIntValue(L"--clip-passes", 3);
        nativeOptions.clipTrimSigma = parseDoubleValue(L"--clip-trim-sigma", 1.5);
        auto blockTypesIt = args.values.find(L"--block-types");
        if (blockTypesIt != args.values.end()) nativeOptions.palette.blockTypes = WideToUtf8(blockTypesIt->second);
        auto blocksIt = args.values.find(L"--blocks");
        if (blocksIt != args.values.end()) nativeOptions.palette.blocks = WideToUtf8(blocksIt->second);
        auto excludeTypesIt = args.values.find(L"--exclude-block-types");
        if (excludeTypesIt != args.values.end()) nativeOptions.palette.excludeBlockTypes = WideToUtf8(excludeTypesIt->second);
        auto excludeBlocksIt = args.values.find(L"--exclude-blocks");
        if (excludeBlocksIt != args.values.end()) nativeOptions.palette.excludeBlocks = WideToUtf8(excludeBlocksIt->second);
        nativeOptions.palette.noGlass = args.flags.count(L"--no-glass") > 0;
        if (auto blockIt = args.values.find(L"--block"); blockIt != args.values.end() && !blockIt->second.empty()) {
            nativeOptions.surfaceBlockName = WideToUtf8(blockIt->second);
        }

        native_mc::ObjTexturedImportResult nativeResult;
        std::string errorText;
        if (!native_mc::ImportObjTextured(nativeOptions, &nativeResult, &errorText)) {
            PrintLine(T(lang, L"原生 OBJ 导入失败：", L"Native OBJ import failed: ") + Utf8ToWide(errorText));
            return false;
        }

        PrintLine(T(lang, plan.mode == L"obj-surface" ? L"原生 OBJ surface 导入完成。" : L"原生 OBJ textured 导入完成。",
                          plan.mode == L"obj-surface" ? L"Native OBJ surface import complete." : L"Native OBJ textured import complete."));
        PrintLine(L"  world   : " + nativeResult.worldDir.wstring());
        PrintLine(L"  palette : " + std::to_wstring(nativeResult.paletteSize));
        PrintLine(L"  size    : " + std::to_wstring(nativeResult.sizeBlocks[0]) + L"x" + std::to_wstring(nativeResult.sizeBlocks[1]) + L"x" + std::to_wstring(nativeResult.sizeBlocks[2]));
        PrintLine(L"  origin  : " + std::to_wstring(nativeResult.origin[0]) + L"," + std::to_wstring(nativeResult.origin[1]) + L"," + std::to_wstring(nativeResult.origin[2]));
        for (const auto& item : nativeResult.files) {
            PrintLine(L"  [" + Utf8ToWide(item.name) + L"] triangles=" + std::to_wstring(item.triangles)
                + L" blocks=" + std::to_wstring(item.blocks)
                + L" chunks=" + std::to_wstring(item.chunks)
                + L" textures=" + std::to_wstring(item.textures)
                + L" clippedTriangles=" + std::to_wstring(item.clippedTriangles)
                + L" dupSamples=" + std::to_wstring(item.duplicateSamples)
                + L" regions=" + std::to_wstring(item.regions)
                + L" written=" + std::to_wstring(item.written)
                + L" ms=" + std::to_wstring(item.milliseconds));
        }

        config.projectRoot = backend.root;
        config.objDir = plan.sourceDir;
        config.inputPath = plan.sourceDir;
        config.worldDir = plan.originalWorldDir;
        config.worldName = plan.originalWorldDir.filename().wstring();
        config.testWorldRoot = testWorldRoot;
        config.lastMode = plan.mode;
        config.lastSafety = plan.safetyMode;
        return true;
    }

    if (plan.mode == L"glb-surface") {
        native_mc::GlbSurfaceOptions nativeOptions;
        nativeOptions.glbDir = plan.sourceDir;
        nativeOptions.worldDir = plan.worldDir;
        nativeOptions.onlyFile = plan.onlyFile.empty() ? fs::path() : fs::path(plan.onlyFile);
        nativeOptions.allFiles = plan.allFiles;
        nativeOptions.overwrite = args.flags.count(L"--overwrite") > 0;
        nativeOptions.batchBlockLimit = parseIntValue(L"--batch-block-limit", config.batchBlockLimit > 0 ? config.batchBlockLimit : 0);
        nativeOptions.baseY = parseIntValue(L"--base-y", -32);
        nativeOptions.scaleBlocksPerMeter = parseCommonDouble(L"--scale-blocks-per-meter", parseCommonDouble(L"--scale", 4.0));
        nativeOptions.rotateYDeg = parseCommonDouble(L"--rotate-y-deg", 0.0);
        nativeOptions.modelRotation = modelRotationFor(nativeOptions.rotateYDeg);
        if (nativeOptions.modelRotation.has_value()) nativeOptions.rotateYDeg = 0.0;
        nativeOptions.flipX = args.flags.count(L"--flip-x") > 0;
        nativeOptions.flipZ = args.flags.count(L"--flip-z") > 0 && args.flags.count(L"--no-flip-z") == 0;
        auto blockIt = args.values.find(L"--block");
        if (blockIt != args.values.end() && !blockIt->second.empty()) {
            nativeOptions.blockName = WideToUtf8(blockIt->second);
        }
        const std::wstring centerText = args.values.count(L"--center") ? args.values.at(L"--center") : config.center;
        parseCenterValue(centerText, &nativeOptions.center);
        applyCenterFlags(&nativeOptions.center);

        native_mc::GlbSurfaceImportResult nativeResult;
        std::string errorText;
        if (!native_mc::ImportGlbSurface(nativeOptions, &nativeResult, &errorText)) {
            PrintLine(T(lang, L"原生 GLB surface 导入失败：", L"Native GLB surface import failed: ") + Utf8ToWide(errorText));
            return false;
        }

        PrintLine(T(lang, L"原生 GLB surface 导入完成。", L"Native GLB surface import complete."));
        PrintLine(L"  world   : " + nativeResult.worldDir.wstring());
        PrintLine(L"  size    : " + std::to_wstring(nativeResult.sizeBlocks[0]) + L"x" + std::to_wstring(nativeResult.sizeBlocks[1]) + L"x" + std::to_wstring(nativeResult.sizeBlocks[2]));
        PrintLine(L"  origin  : " + std::to_wstring(nativeResult.origin[0]) + L"," + std::to_wstring(nativeResult.origin[1]) + L"," + std::to_wstring(nativeResult.origin[2]));
        for (const auto& item : nativeResult.files) {
            PrintLine(L"  [" + Utf8ToWide(item.name) + L"] triangles=" + std::to_wstring(item.triangles)
                + L" blocks=" + std::to_wstring(item.blocks)
                + L" chunks=" + std::to_wstring(item.chunks)
                + L" regions=" + std::to_wstring(item.regions)
                + L" written=" + std::to_wstring(item.written)
                + L" ms=" + std::to_wstring(item.milliseconds));
        }

        config.projectRoot = backend.root;
        config.glbDir = plan.sourceDir;
        config.inputPath = plan.sourceDir;
        config.worldDir = plan.originalWorldDir;
        config.worldName = plan.originalWorldDir.filename().wstring();
        config.testWorldRoot = testWorldRoot;
        config.lastMode = plan.mode;
        config.lastSafety = plan.safetyMode;
        return true;
    }

    PrintLine(T(lang, L"当前模式没有匹配到可用的原生导入实现。", L"No native importer matched the current mode."));
    return false;
}

bool RunCopyWorld(Lang lang, const fs::path& exeDir, const BackendLayout& backend, Config& config, const ParsedArgs& args) {
    const fs::path worldDir = ResolveWorldDir(backend, config, args);
    if (worldDir.empty()) {
        PrintLine(T(lang, L"找不到可复制的世界目录。", L"Could not resolve a world to copy."));
        return false;
    }
    const fs::path testWorldRoot = ResolveTestWorldRoot(exeDir, backend, config, args);
    fs::path target;
    auto it = args.values.find(L"--copy-to");
    if (it != args.values.end()) {
        target = it->second;
    } else {
        target = MakeCopiedWorldPath(worldDir, testWorldRoot);
    }
    if (!args.flags.count(L"--yes") && !Confirm(T(lang,
        (L"将复制世界：\n源存档: " + worldDir.wstring() + L"\n目标存档: " + target.wstring() + L"\n输入 YES 继续：").c_str(),
        (L"This will copy the world:\nSource: " + worldDir.wstring() + L"\nTarget: " + target.wstring() + L"\nType YES to continue: ").c_str()))) {
        PrintLine(T(lang, L"已取消。", L"Cancelled."));
        return false;
    }
    std::wstring errorText;
    if (!CopyDirectoryRecursive(worldDir, target, errorText)) {
        PrintLine(T(lang, L"复制失败：", L"Copy failed: ") + errorText);
        return false;
    }
    config.worldDir = worldDir;
    config.worldName = worldDir.filename().wstring();
    config.testWorldRoot = testWorldRoot;
    PrintLine(T(lang, L"复制完成：", L"Copied to: ") + target.wstring());
    return true;
}

bool RunScan(Lang lang, const BackendLayout& backend, Config& config, const ParsedArgs& args) {
    const fs::path worldDir = ResolveWorldDir(backend, config, args);
    if (worldDir.empty()) {
        PrintLine(T(lang, L"找不到要扫描的世界目录。", L"Could not resolve a world to scan."));
        return false;
    }

    auto parseOptionalInt = [&](const std::wstring& flag) -> std::optional<int> {
        auto it = args.values.find(flag);
        if (it == args.values.end() || it->second.empty()) {
            return std::nullopt;
        }
        try {
            return std::stoi(it->second);
        } catch (...) {
            return std::nullopt;
        }
    };

    native_mc::ScanSummary summary;
    std::set<std::string> allowedBlocks;
    const std::wstring paletteModeWide = args.values.count(L"--palette") ? args.values.at(L"--palette") : L"scene";
    const std::string paletteMode = WideToUtf8(paletteModeWide);
    std::set<std::string>* allowedPtr = nullptr;
    if (paletteMode != "raw") {
        native_mc::PaletteOptions paletteOptions;
        paletteOptions.mode = paletteMode.empty() ? "scene" : paletteMode;
        auto blockTypesIt = args.values.find(L"--block-types");
        if (blockTypesIt != args.values.end()) paletteOptions.blockTypes = WideToUtf8(blockTypesIt->second);
        auto blocksIt = args.values.find(L"--blocks");
        if (blocksIt != args.values.end()) paletteOptions.blocks = WideToUtf8(blocksIt->second);
        auto excludeTypesIt = args.values.find(L"--exclude-block-types");
        if (excludeTypesIt != args.values.end()) paletteOptions.excludeBlockTypes = WideToUtf8(excludeTypesIt->second);
        auto excludeBlocksIt = args.values.find(L"--exclude-blocks");
        if (excludeBlocksIt != args.values.end()) paletteOptions.excludeBlocks = WideToUtf8(excludeBlocksIt->second);
        paletteOptions.noGlass = args.flags.count(L"--no-glass") > 0;
        native_mc::Palette palette;
        std::string paletteError;
        if (!native_mc::LoadPalette(backend.root, paletteOptions, &palette, &paletteError)) {
            PrintLine(T(lang, L"扫描前加载调色板失败：", L"Failed to load palette before scan: ") + Utf8ToWide(paletteError));
            return false;
        }
        for (const auto& block : palette.blocks()) {
            allowedBlocks.insert(block.name);
        }
        allowedPtr = &allowedBlocks;
    }
    std::string errorText;
    if (!native_mc::ScanWorldBlocks(worldDir, parseOptionalInt(L"--min-y"), parseOptionalInt(L"--max-y"), &summary, &errorText, allowedPtr)) {
        PrintLine(T(lang, L"原生扫描失败：", L"Native scan failed: ") + Utf8ToWide(errorText));
        return false;
    }

    PrintLine(T(lang, L"原生扫描完成。", L"Native scan complete."));
    PrintLine(L"  world   : " + worldDir.wstring());
    PrintLine(L"  palette : " + Utf8ToWide(paletteMode.empty() ? std::string("scene") : paletteMode));
    PrintLine(L"  regions : " + std::to_wstring(summary.regions));
    PrintLine(L"  chunks  : " + std::to_wstring(summary.chunks));
    PrintLine(L"  blocks  : " + std::to_wstring(summary.totalBlocks));
    if (summary.hasBounds) {
        PrintLine(L"  bounds  : (" + std::to_wstring(summary.minX) + L"," + std::to_wstring(summary.minY) + L"," + std::to_wstring(summary.minZ)
            + L") -> (" + std::to_wstring(summary.maxX) + L"," + std::to_wstring(summary.maxY) + L"," + std::to_wstring(summary.maxZ) + L")");
    }
    std::vector<std::pair<std::string, std::uint64_t>> sorted(summary.byBlock.begin(), summary.byBlock.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });
    int shown = 0;
    for (const auto& [name, count] : sorted) {
        if (shown >= 12) break;
        PrintLine(L"  " + Utf8ToWide(name) + L" : " + std::to_wstring(count));
        ++shown;
    }
    config.worldDir = worldDir;
    config.worldName = worldDir.filename().wstring();
    return true;
}

bool RunReset(Lang lang, const BackendLayout& backend, Config& config, const ParsedArgs& args) {
    const fs::path worldDir = ResolveWorldDir(backend, config, args);
    if (worldDir.empty()) {
        PrintLine(T(lang, L"找不到要重置的世界目录。", L"Could not resolve a world to reset."));
        return false;
    }
    if (!args.flags.count(L"--yes") &&
        !Confirm(T(lang,
            (L"将重置这个世界的主世界 region/poi/entities：\n" + worldDir.wstring() + L"\n输入 YES 继续：").c_str(),
            (L"This will reset the overworld region/poi/entities for:\n" + worldDir.wstring() + L"\nType YES to continue: ").c_str()))) {
        PrintLine(T(lang, L"已取消。", L"Cancelled."));
        return false;
    }
    fs::path backupRoot;
    std::string errorText;
    if (!native_mc::ResetWorldToBlankSuperflat(worldDir, &backupRoot, &errorText)) {
        PrintLine(T(lang, L"原生重置失败：", L"Native reset failed: ") + Utf8ToWide(errorText));
        return false;
    }
    PrintLine(T(lang, L"原生重置完成。备份目录：", L"Native reset complete. Backup root: ") + backupRoot.wstring());
    config.worldDir = worldDir;
    config.worldName = worldDir.filename().wstring();
    return true;
}

bool GuiRunRotationMathSelfTest(std::wstring* errorText);

bool RunSelfTest(Lang lang, const fs::path& exeDir, const fs::path& configPath, const Config& config, const ParsedArgs& args, const BackendLayout& backend) {
    PrintLine(T(lang, L"自检开始", L"Self-test started"));
    bool ok = PrintDoctor(lang, exeDir, configPath, config, args, backend);
    ParsedArgs planArgs = args;
    ApplyConfigDefaults(planArgs, config);
    if (!planArgs.values.count(L"--only") && !planArgs.flags.count(L"--all")) {
        planArgs.flags.insert(L"--all");
    }
    auto planOpt = MakeImportPlan(exeDir, backend, config, planArgs);
    if (!planOpt) {
        PrintLine(T(lang, L"导入计划解析失败：缺少模型路径或世界目录。", L"Import-plan resolution failed: missing model path or world directory."));
        ok = false;
    } else {
        PrintLine(T(lang, L"导入计划解析成功。", L"Import plan resolved successfully."));
        PrintLine(L"  mode          : " + planOpt->mode);
        PrintLine(L"  source dir    : " + planOpt->sourceDir.wstring());
        PrintLine(L"  world         : " + planOpt->originalWorldDir.wstring());
    }
    std::wstring rotationError;
    if (GuiRunRotationMathSelfTest(&rotationError)) {
        PrintLine(T(lang, L"Blender WXYZ 与找平旋转检查通过。", L"Blender WXYZ and leveling rotation checks passed."));
    } else {
        PrintLine(T(lang, L"旋转数学检查失败：", L"Rotation math check failed: ") + rotationError);
        ok = false;
    }
    PrintLine(ok ? T(lang, L"自检通过。", L"Self-test passed.") : T(lang, L"自检未通过。", L"Self-test did not pass."));
    return ok;
}

void PrintWorlds(Lang lang, const BackendLayout& backend, const Config& config, const ParsedArgs& args) {
    const auto savesDirs = CandidateSavesDirs(backend.root, config, args);
    if (savesDirs.empty()) {
        PrintLine(T(lang, L"没有发现可扫描的 saves 目录。", L"No saves directories were found."));
        return;
    }
    for (const auto& savesDir : savesDirs) {
        if (!HasDir(savesDir)) {
            continue;
        }
        PrintLine(T(lang, L"saves 目录：", L"Saves directory: ") + savesDir.wstring());
        const auto worlds = CollectWorldsFromSaves(savesDir);
        if (worlds.empty()) {
            PrintLine(T(lang, L"  (没有世界)", L"  (no worlds)"));
            continue;
        }
        for (const auto& world : worlds) {
            PrintLine(L"  - " + world.filename().wstring() + L" -> " + world.wstring());
        }
    }
}

bool RunSetup(Lang lang, const fs::path& exeDir, const fs::path& configPath, Config& config, const BackendLayout& backend) {
    PrintLine(T(lang, L"设置向导", L"Setup wizard"));
    Config next = config;

    auto promptString = [&](const std::wstring& zh, const std::wstring& en, std::wstring& target) {
        target = Prompt(T(lang, zh.c_str(), en.c_str()), target);
    };
    auto promptPath = [&](const std::wstring& zh, const std::wstring& en, fs::path& target) {
        const std::wstring value = Prompt(T(lang, zh.c_str(), en.c_str()), target.empty() ? L"" : target.wstring());
        if (!value.empty()) {
            target = value;
        }
    };
    auto promptInt = [&](const std::wstring& zh, const std::wstring& en, int& target, int minimum) {
        const std::wstring value = Prompt(T(lang, zh.c_str(), en.c_str()), std::to_wstring(target));
        if (!value.empty()) {
            try {
                target = std::max(minimum, std::stoi(value));
            } catch (...) {
            }
        }
    };

    const std::wstring backendRoot = Prompt(T(lang, L"项目根目录（带 vendor/ots/res 的目录）", L"Project root (folder with vendor/ots/res)"), backend.root.empty() ? L"" : backend.root.wstring());
    if (!backendRoot.empty()) {
        next.projectRoot = backendRoot;
    }
    const std::wstring mode = Prompt(T(lang, L"默认输入类型（obj / obj-surface / glb-textured / glb-surface）", L"Default input type (obj / obj-surface / glb-textured / glb-surface)"), next.lastMode.empty() ? L"glb-textured" : next.lastMode);
    next.lastMode = mode;

    PrintLine(T(lang, L"\n路径参数", L"\nPath options"));
    promptPath(L"默认 input 文件/目录（可留空）", L"Default input file/folder (optional)", next.inputPath);
    promptPath(L"OBJ 目录", L"OBJ directory", next.objDir);
    promptPath(L"GLB 目录", L"GLB directory", next.glbDir);
    promptPath(L"默认世界目录", L"Default world directory", next.worldDir);
    if (!next.worldDir.empty()) {
        next.worldName = next.worldDir.filename().wstring();
    }
    promptPath(L"saves 目录（可留空）", L"Saves directory (optional)", next.savesDir);
    promptPath(L"Minecraft 根目录（可留空）", L"Minecraft root (optional)", next.mcRoot);
    promptString(L"世界名称（配合 saves/mc-root，可留空）", L"World name (with saves/mc-root, optional)", next.worldName);
    promptString(L"Minecraft 版本目录名（可留空）", L"Minecraft version folder (optional)", next.mcVersion);
    promptPath(L"测试存档根目录", L"Test-world root", next.testWorldRoot);
    promptPath(L"复制测试存档目标 --copy-to（可留空）", L"Copy target --copy-to (optional)", next.copyTo);

    PrintLine(T(lang, L"\n通用导入参数", L"\nGeneral import options"));
    promptString(L"只导入文件 --only（可留空）", L"Only import file --only (optional)", next.onlyFile);
    promptString(L"默认 center（x,z 或 x,y,z，可留空）", L"Default center (x,z or x,y,z, optional)", next.center);
    promptString(L"center-x（可留空）", L"center-x (optional)", next.centerX);
    promptString(L"center-y（可留空）", L"center-y (optional)", next.centerY);
    promptString(L"center-z（可留空）", L"center-z (optional)", next.centerZ);
    promptString(L"默认 base-y（可留空）", L"Default base-y (optional)", next.baseY);
    promptInt(L"默认 workers（1-64，仅 OBJ）", L"Default workers (1-64, OBJ only)", next.workers, 1);
    next.workers = std::clamp(next.workers, 1, 64);
    promptInt(L"默认 batch-block-limit", L"Default batch-block-limit", next.batchBlockLimit, 1);
    promptString(L"默认 palette（scene/clean/colourful/concrete，可留空）", L"Default palette (scene/clean/colourful/concrete, optional)", next.palette);
    promptString(L"允许的 block-types CSV（可留空）", L"Allowed block-types CSV (optional)", next.blockTypes);
    promptString(L"额外允许 blocks CSV（可留空）", L"Extra allowed blocks CSV (optional)", next.blocks);
    promptString(L"排除 block-types CSV（可留空）", L"Excluded block-types CSV (optional)", next.excludeBlockTypes);
    promptString(L"排除 blocks CSV（可留空）", L"Excluded blocks CSV (optional)", next.excludeBlocks);
    next.noGlass = PromptBool(lang, T(lang, L"排除玻璃 --no-glass", L"Exclude glass --no-glass"), next.noGlass);
    next.allFiles = PromptBool(lang, T(lang, L"默认导入全部文件 --all", L"Default import all files --all"), next.allFiles);
    next.overwrite = PromptBool(lang, T(lang, L"默认覆盖 --overwrite", L"Default overwrite --overwrite"), next.overwrite);
    next.copyWorld = PromptBool(lang, T(lang, L"默认复制测试存档 --copy-world", L"Default copy-world --copy-world"), next.copyWorld);
    next.direct = PromptBool(lang, T(lang, L"默认直接写入 --direct", L"Default direct write --direct"), next.direct);
    next.lastSafety = next.direct ? L"direct" : (next.copyWorld ? L"copy-world" : L"copy-world");

    PrintLine(T(lang, L"\nGLB 参数", L"\nGLB options"));
    promptString(L"textured GLB fallback-block（可留空）", L"Textured GLB fallback-block (optional)", next.fallbackBlock);
    promptString(L"GLB surface 单方块 --block（可留空）", L"GLB surface single --block (optional)", next.block);
    promptString(L"dedupe-sample-steps（可留空）", L"dedupe-sample-steps (optional)", next.dedupeSampleSteps);

    PrintLine(T(lang, L"\nOBJ 参数", L"\nOBJ options"));
    promptString(L"OBJ transform（默认 obj-z-up，可留空）", L"OBJ transform (default obj-z-up, optional)", next.transform);
    promptString(L"OBJ level-normal（x,y,z，可留空）", L"OBJ level-normal (x,y,z, optional)", next.levelNormal);
    promptString(L"OBJ rotate-y-deg（可留空）", L"OBJ rotate-y-deg (optional)", next.rotateYDeg);
    promptString(L"OBJ scale-blocks-per-meter / scale（可留空）", L"OBJ scale-blocks-per-meter / scale (optional)", next.scaleBlocksPerMeter);
    promptString(L"clip-reference-obj（可留空）", L"clip-reference-obj (optional)", next.clipReferenceObj);
    promptString(L"clip-below-meters（可留空）", L"clip-below-meters (optional)", next.clipBelowMeters);
    promptString(L"clip-cell-size（可留空）", L"clip-cell-size (optional)", next.clipCellSize);
    promptString(L"clip-low-fraction（可留空）", L"clip-low-fraction (optional)", next.clipLowFraction);
    promptString(L"clip-passes（可留空）", L"clip-passes (optional)", next.clipPasses);
    promptString(L"clip-trim-sigma（可留空）", L"clip-trim-sigma (optional)", next.clipTrimSigma);
    promptString(L"component-min-ratio（可留空）", L"component-min-ratio (optional)", next.componentMinRatio);
    promptString(L"component-below-gap（可留空）", L"component-below-gap (optional)", next.componentBelowGap);
    next.flipX = PromptBool(lang, T(lang, L"OBJ flip-x", L"OBJ flip-x"), next.flipX);
    next.flipZ = PromptBool(lang, T(lang, L"OBJ flip-z", L"OBJ flip-z"), next.flipZ);
    next.noFlipZ = PromptBool(lang, T(lang, L"OBJ no-flip-z", L"OBJ no-flip-z"), next.noFlipZ);
    next.noComponentFilter = PromptBool(lang, T(lang, L"关闭 OBJ component filter", L"Disable OBJ component filter"), next.noComponentFilter);

    PrintLine(T(lang, L"\n扫描参数", L"\nScan options"));
    promptString(L"scan min-y（可留空）", L"scan min-y (optional)", next.minY);
    promptString(L"scan max-y（可留空）", L"scan max-y (optional)", next.maxY);
    promptString(L"界面语言 lang（zh/en，可留空）", L"UI language lang (zh/en, optional)", next.lang);

    if (next.lang.empty()) {
        next.lang = (lang == Lang::Zh) ? L"zh" : L"en";
    }
    if (!SaveConfig(configPath, next)) {
        PrintLine(T(lang, L"保存配置失败。", L"Failed to save the configuration."));
        return false;
    }
    config = next;
    PrintLine(T(lang, L"配置已保存：", L"Saved config: ") + configPath.wstring());
    return true;
}

int InteractiveMenu(Lang lang, const fs::path& exePath) {
    PrintLine(T(lang, L"请选择任务：", L"Choose a task:"));
    PrintLine(T(lang, L"  1. 设置向导", L"  1. Setup wizard"));
    PrintLine(T(lang, L"  2. 诊断", L"  2. Doctor"));
    PrintLine(T(lang, L"  3. GLB 导入到复制测试存档", L"  3. GLB import into copied test world"));
    PrintLine(T(lang, L"  4. OBJ 导入到复制测试存档", L"  4. OBJ import into copied test world"));
    PrintLine(T(lang, L"  5. 复制一个新测试存档", L"  5. Copy a new test world"));
    PrintLine(T(lang, L"  6. 扫描当前世界", L"  6. Scan current world"));
    PrintLine(T(lang, L"  7. 重置当前世界", L"  7. Reset current world"));
    PrintLine(T(lang, L"  9. 帮助", L"  9. Help"));
    PrintLine(T(lang, L"  0. 退出", L"  0. Exit"));
    std::wcout << T(lang, L"输入编号：", L"Enter choice: ");
    std::wstring choice = ReadInputLine();
    if (choice == L"1") return 1;
    if (choice == L"2") return 2;
    if (choice == L"3") return 3;
    if (choice == L"4") return 4;
    if (choice == L"5") return 5;
    if (choice == L"6") return 6;
    if (choice == L"7") return 7;
    if (choice == L"9") return 9;
    (void)exePath;
    return 0;
}

constexpr UINT WM_GUI_LOG = WM_APP + 1;
constexpr UINT WM_GUI_DONE = WM_APP + 2;
constexpr UINT WM_GUI_BREAKDOWN_READY = WM_APP + 3;
constexpr UINT WM_GUI_WORLD_READY = WM_APP + 4;
constexpr UINT WM_GUI_PREVIEW_PROGRESS = WM_APP + 5;
constexpr UINT WM_GUI_LEVEL_READY = WM_APP + 6;
constexpr UINT WM_GUI_WORLD_GPU_READY = WM_APP + 7;
constexpr UINT WM_GUI_PREVIEW_DONE = WM_APP + 8;
constexpr UINT WM_GUI_RENDER_TICK = WM_APP + 9;
constexpr UINT_PTR PREVIEW_DXGI_OVERLAY_REFRESH_TIMER = 0x4D435046;

enum GuiControlId {
    IDC_GUI_TAB = 3000,
    IDC_GUI_LOG,
    IDC_GUI_STATUS,
    IDC_GUI_PREVIEW,
    IDC_GUI_ACTION_SAVE,
    IDC_GUI_ACTION_LOAD,
    IDC_GUI_ACTION_DOCTOR,
    IDC_GUI_ACTION_COPY_WORLD,
    IDC_GUI_ACTION_GLB,
    IDC_GUI_ACTION_OBJ,
    IDC_GUI_ACTION_SCAN,
    IDC_GUI_ACTION_RESET,
    IDC_GUI_ACTION_HELP,
    IDC_GUI_ACTION_PREVIEW_LOAD,
    IDC_GUI_ACTION_PALETTE_EDITOR,
    IDC_GUI_ACTION_BREAKDOWN,
    IDC_GUI_ACTION_CANCEL,
    IDC_GUI_PREVIEW_INFO,
    IDC_GUI_PREVIEW_HINT,
    IDC_GUI_PREVIEW_RENDERER_INFO,
    IDC_GUI_PREVIEW_FIT,
    IDC_GUI_PREVIEW_VIEW_TOP,
    IDC_GUI_PREVIEW_VIEW_FRONT,
    IDC_GUI_PREVIEW_VIEW_LEFT,
    IDC_GUI_PREVIEW_VIEW_RIGHT,
    IDC_GUI_PREVIEW_UNDO,
    IDC_GUI_PREVIEW_REDO,
    IDC_GUI_PREVIEW_MODE,
    IDC_GUI_PREVIEW_LOD,
    IDC_GUI_PREVIEW_GRID,
    IDC_GUI_PREVIEW_AXES,
    IDC_GUI_PREVIEW_WORLD,
    IDC_GUI_PREVIEW_WORLD_REFRESH,
    IDC_GUI_PREVIEW_WORLD_RADIUS,
    IDC_GUI_PREVIEW_WORLD_DENSITY,
    IDC_GUI_PREVIEW_WORLD_STYLE,
    IDC_GUI_PREVIEW_TOOL_CAMERA,
    IDC_GUI_PREVIEW_TOOL_PLACE,
    IDC_GUI_PREVIEW_SNAP,
    IDC_GUI_PREVIEW_INSPECTOR,
    IDC_GUI_TRANSFORM_ROTATE_LABEL,
    IDC_GUI_TRANSFORM_ROTATE_TRACK,
    IDC_GUI_TRANSFORM_ROTATE_LEFT,
    IDC_GUI_TRANSFORM_ROTATE_RIGHT,
    IDC_GUI_TRANSFORM_SCALE_LABEL,
    IDC_GUI_TRANSFORM_SCALE_TRACK,
    IDC_GUI_TRANSFORM_SCALE_DOWN,
    IDC_GUI_TRANSFORM_SCALE_UP,
    IDC_GUI_TRANSFORM_BASEY_LABEL,
    IDC_GUI_TRANSFORM_BASEY_TRACK,
    IDC_GUI_TRANSFORM_BASEY_DOWN,
    IDC_GUI_TRANSFORM_BASEY_UP,
    IDC_GUI_TRANSFORM_FLIPX,
    IDC_GUI_TRANSFORM_FLIPZ,
    IDC_GUI_TRANSFORM_SYSTEM,
    IDC_GUI_TRANSFORM_RESET,
    IDC_GUI_ROTATION_MODE,
    IDC_GUI_ROTATION_W_LABEL,
    IDC_GUI_ROTATION_X_LABEL,
    IDC_GUI_ROTATION_Y_LABEL,
    IDC_GUI_ROTATION_Z_LABEL,
    IDC_GUI_ROTATION_W_EDIT,
    IDC_GUI_ROTATION_X_EDIT,
    IDC_GUI_ROTATION_Y_EDIT,
    IDC_GUI_ROTATION_Z_EDIT,
    IDC_GUI_LEVEL_NORMAL_LABEL,
    IDC_GUI_LEVEL_NORMAL_EDIT,
    IDC_GUI_LEVEL_APPLY,
    IDC_GUI_SCALE_EDIT,
    IDC_GUI_BASEY_EDIT,
    IDC_GUI_PLACE_X_LABEL,
    IDC_GUI_PLACE_X_LEFT,
    IDC_GUI_PLACE_X_RIGHT,
    IDC_GUI_PLACE_Z_LABEL,
    IDC_GUI_PLACE_Z_LEFT,
    IDC_GUI_PLACE_Z_RIGHT,
    IDC_GUI_PLACE_Y_LABEL,
    IDC_GUI_PLACE_Y_DOWN,
    IDC_GUI_PLACE_Y_UP,
    IDC_GUI_PLACE_CLEAR_CENTER,
    IDC_GUI_PLACE_ORIGIN_CENTER,
    IDC_GUI_PRESET_A_LABEL,
    IDC_GUI_PRESET_A_SAVE,
    IDC_GUI_PRESET_A_LOAD,
    IDC_GUI_PRESET_B_LABEL,
    IDC_GUI_PRESET_B_SAVE,
    IDC_GUI_PRESET_B_LOAD,
    IDC_GUI_PRESET_C_LABEL,
    IDC_GUI_PRESET_C_SAVE,
    IDC_GUI_PRESET_C_LOAD,
    IDC_GUI_MODEL_SUMMARY,
    IDC_GUI_MODEL_CURRENT,
    IDC_GUI_MODEL_SELECTED,
    IDC_GUI_MODEL_FILTER,
    IDC_GUI_MODEL_LIST,
    IDC_GUI_MODEL_REFRESH,
    IDC_GUI_MODEL_USE,
    IDC_GUI_INPUT_FILE_PICKER,
    IDC_GUI_INPUT_DIR_PICKER,
    IDC_GUI_WORLD_DIR_PICKER,
    IDC_GUI_MODEL_PREV,
    IDC_GUI_MODEL_NEXT,
    IDC_GUI_MODEL_SHOW_ALL,
    IDC_GUI_MODEL_LEVEL_REFERENCE,
    IDC_GUI_BROWSE_BASE = 4000,
};

enum PaletteDialogControlId {
    IDC_PALETTE_SUMMARY = 5000,
    IDC_PALETTE_INCLUDED_SEARCH,
    IDC_PALETTE_INCLUDED_LIST,
    IDC_PALETTE_EXCLUDED_SEARCH,
    IDC_PALETTE_EXCLUDED_LIST,
    IDC_PALETTE_EXCLUDE,
    IDC_PALETTE_INCLUDE,
    IDC_PALETTE_APPLY,
    IDC_PALETTE_CANCEL,
};

enum BreakdownDialogControlId {
    IDC_BREAKDOWN_SUMMARY = 5200,
    IDC_BREAKDOWN_SEARCH,
    IDC_BREAKDOWN_LIST,
    IDC_BREAKDOWN_REFRESH,
    IDC_BREAKDOWN_CLOSE,
};

struct BlockBreakdownPayload {
    bool ok = false;
    std::wstring message;
    native_mc::ScanSummary summary;
};

struct PreviewProgressPayload {
    int completed = 0;
    int total = 0;
    std::wstring message;
};

struct LevelReferencePayload {
    bool ok = false;
    std::uint64_t generation = 0;
    fs::path source;
    std::wstring transform;
    bool flipX = false;
    bool flipZ = false;
    std::wstring message;
    std::wstring normal;
};

enum class PreviewDrawMode {
    Solid,
    Wireframe,
    SolidWire,
};

enum class PreviewToolMode {
    Camera,
    Place,
};

enum class PreviewWorldStyle {
    Points,
    Faces,
    Voxels,
};

enum class PreviewPlacementDragMode {
    None,
    Plane,
    Height,
    AxisX,
    AxisY,
    AxisZ,
};

struct PreviewEditState {
    std::wstring rotateYDeg;
    std::wstring rotationMode;
    std::wstring rotationW;
    std::wstring rotationX;
    std::wstring rotationY;
    std::wstring rotationZ;
    std::wstring levelNormal;
    std::wstring scaleBlocksPerMeter;
    std::wstring baseY;
    std::wstring transform;
    std::wstring center;
    std::wstring centerX;
    std::wstring centerY;
    std::wstring centerZ;
    bool flipX = false;
    bool flipZ = false;
};

struct WorldPreviewGpuMesh {
    PreviewWorldStyle style = PreviewWorldStyle::Faces;
    GLenum primitive = GL_QUADS;
    std::vector<float> positions;
    std::vector<std::uint8_t> colors;
    bool densityReduced = false;
};

struct WorldPreviewPayload {
    bool ok = false;
    std::wstring message;
    std::wstring worldLabel;
    native_mc::WorldPreviewSlice slice;
    std::map<std::string, COLORREF> colors;
    WorldPreviewGpuMesh gpuMesh;
};

struct WorldPreviewGpuPayload {
    std::uint64_t generation = 0;
    WorldPreviewGpuMesh mesh;
};

struct GuiState {
    HWND hwnd = nullptr;
    HWND tab = nullptr;
    HWND log = nullptr;
    HWND status = nullptr;
    HWND preview = nullptr;
    HWND previewInfoLabel = nullptr;
    HWND previewHintLabel = nullptr;
    HWND previewRendererLabel = nullptr;
    HWND previewInspector = nullptr;
    HWND previewRotateLabel = nullptr;
    HWND previewRotateTrack = nullptr;
    HWND previewScaleLabel = nullptr;
    HWND previewScaleTrack = nullptr;
    HWND previewBaseYLabel = nullptr;
    HWND previewBaseYTrack = nullptr;
    HWND previewFlipXCheck = nullptr;
    HWND previewFlipZCheck = nullptr;
    HWND previewTransformButton = nullptr;
    HWND previewRotationModeCombo = nullptr;
    std::array<HWND, 4> previewRotationLabels{};
    std::array<HWND, 4> previewRotationEdits{};
    HWND previewLevelNormalLabel = nullptr;
    HWND previewLevelNormalEdit = nullptr;
    HWND previewLevelButton = nullptr;
    HWND previewScaleEdit = nullptr;
    HWND previewBaseYEdit = nullptr;
    HWND previewPlaceXLabel = nullptr;
    HWND previewPlaceZLabel = nullptr;
    HWND previewPlaceYLabel = nullptr;
    std::array<HWND, 3> previewPresetLabels{};
    HWND modelSummaryLabel = nullptr;
    HWND modelCurrentLabel = nullptr;
    HWND modelSelectedLabel = nullptr;
    HWND modelFilterEdit = nullptr;
    HWND modelList = nullptr;
    HWND previewProgressWindow = nullptr;
    HWND previewProgressLabel = nullptr;
    HWND previewProgressBar = nullptr;
    HWND paletteDialog = nullptr;
    HWND breakdownDialog = nullptr;
    HDC previewDc = nullptr;
    HGLRC previewRc = nullptr;
    std::unique_ptr<native_mc::PreviewRenderer> previewNativeRenderer;
    native_mc::PreviewRendererBackend previewActiveRenderer = native_mc::PreviewRendererBackend::None;
    std::wstring previewRendererStatus;
    std::wstring previewRendererError;
    bool previewDxgiOverlayRefreshPending = false;
    GLuint previewPositionBuffer = 0;
    GLuint previewNormalBuffer = 0;
    GLuint previewColorBuffer = 0;
    std::array<GLuint, 4> previewIndexBuffers{};
    std::array<GLsizei, 4> previewIndexCounts{};
    bool previewGpuDirty = true;
    bool previewGpuReady = false;
    GLuint previewWorldPositionBuffer = 0;
    GLuint previewWorldColorBuffer = 0;
    GLsizei previewWorldVertexCount = 0;
    GLenum previewWorldPrimitive = GL_QUADS;
    PreviewWorldStyle previewWorldGpuStyle = PreviewWorldStyle::Faces;
    bool previewWorldGpuDirty = true;
    bool previewWorldGpuReady = false;
    bool previewWorldGpuBuilding = false;
    bool previewWorldGpuDensityReduced = false;
    std::uint64_t previewWorldGpuGeneration = 0;
    WorldPreviewGpuMesh previewWorldPendingGpuMesh;
    HFONT font = nullptr;
    std::vector<HWND> pages;
    std::map<HWND, int> pageContentHeight;
    std::map<HWND, int> pageScrollPos;
    std::map<std::wstring, HWND> edits;
    std::map<std::wstring, HWND> checks;
    std::map<int, std::pair<std::wstring, bool>> browseMap;
    std::vector<HWND> actionButtons;
    std::vector<HWND> previewButtons;
    std::vector<fs::path> modelFiles;
    std::vector<fs::path> visibleModelFiles;
    fs::path exePath;
    fs::path exeDir;
    fs::path configPath;
    Config config;
    BackendLayout backend;
    Lang lang = Lang::Zh;
    bool running = false;
    bool commandCancelled = false;
    bool populatingControls = false;
    bool previewLoading = false;
    std::atomic<bool> previewLoadCancel{ false };
    std::thread previewLoadThread;
    std::uint64_t previewLevelGeneration = 0;
    std::atomic<bool> previewFrameStop{ false };
    std::atomic<bool> previewFrameTickPending{ false };
    std::atomic<bool> previewFrameActive{ false };
    std::atomic<bool> previewFrameSchedulerRunning{ false };
    std::atomic<int> previewRefreshHz{ 60 };
    std::thread previewFrameThread;
    bool previewFramePermit = false;
    bool previewHighResolutionTimerActive = false;
    bool previewLoaded = false;
    bool previewAllModels = false;
    bool previewAllAvailableModels = false;
    std::size_t previewModelCount = 0;
    bool orbiting = false;
    bool panning = false;
    bool previewDraggingPlacement = false;
    bool previewDraggingHeight = false;
    POINT lastMouse{ 0, 0 };
    POINT previewDragAnchor{ 0, 0 };
    float previewYaw = 35.0f;
    float previewPitch = 20.0f;
    float previewDistance = 8.0f;
    float previewPanX = 0.0f;
    float previewPanY = 0.0f;
    bool previewShowGrid = true;
    bool previewShowAxes = true;
    bool previewWorldVisible = true;
    bool previewWorldLoading = false;
    bool previewWorldDirty = false;
    int previewWorldRadius = 48;
    int previewWorldMaxBlocks = 500000;
    PreviewWorldStyle previewWorldStyle = PreviewWorldStyle::Faces;
    PreviewDrawMode previewDrawMode = PreviewDrawMode::SolidWire;
    PreviewToolMode previewToolMode = PreviewToolMode::Camera;
    int previewSnapStep = 16;
    int previewActiveLod = 0;
    int previewForcedLod = -1;
    std::size_t previewRenderedTriangles = 0;
    double previewFrameMs = 0.0;
    double previewPresentFps = 0.0;
    native_mc::PreviewRendererBackend previewPresentBackend = native_mc::PreviewRendererBackend::None;
    std::uint64_t previewPresentFrameNumber = 0;
    std::uint64_t previewPresentSampleFrame = 0;
    std::uint64_t previewOpenGlFrameNumber = 0;
    std::chrono::steady_clock::time_point previewPresentSampleStarted{};
    std::chrono::steady_clock::time_point previewLastPresentAt{};
    ULONGLONG previewChromeUpdatedAt = 0;
    int previewAdaptiveLodBias = 0;
    int previewFastFrameCount = 0;
    int previewLodUpgradeCooldown = 0;
    int previewSettleFrames = 0;
    PreviewPlacementDragMode previewPlacementDragMode = PreviewPlacementDragMode::None;
    PreviewPlacementDragMode previewHoverDragMode = PreviewPlacementDragMode::None;
    int previewDragStartCenterX = 0;
    int previewDragStartCenterY = 0;
    int previewDragStartCenterZ = 0;
    std::vector<PreviewEditState> previewHistory;
    std::size_t previewHistoryIndex = 0;
    std::array<std::optional<PreviewEditState>, 3> previewPresets;
    native_mc::PreviewMesh previewMesh;
    native_mc::WorldPreviewSlice previewWorldSlice;
    std::map<std::string, COLORREF> previewWorldColors;
    std::wstring previewWorldSourceName;
    std::wstring previewWorldStatus;
    std::wstring previewInfo;
    fs::path previewSource;
    HANDLE runningProcess = nullptr;
    HANDLE runningJob = nullptr;
    std::thread commandThread;
    std::mutex commandMutex;
};

struct PaletteDialogState {
    HWND hwnd = nullptr;
    GuiState* owner = nullptr;
    HFONT font = nullptr;
    HWND summary = nullptr;
    HWND includedSearch = nullptr;
    HWND includedList = nullptr;
    HWND excludedSearch = nullptr;
    HWND excludedList = nullptr;
    std::vector<std::wstring> allBlocks;
    std::set<std::wstring> baseBlocks;
    std::set<std::wstring> includedBlocks;
    std::vector<std::wstring> visibleIncluded;
    std::vector<std::wstring> visibleExcluded;
};

struct BreakdownDialogState {
    HWND hwnd = nullptr;
    GuiState* owner = nullptr;
    HFONT font = nullptr;
    HWND summary = nullptr;
    HWND search = nullptr;
    HWND list = nullptr;
    bool loading = false;
    std::vector<std::pair<std::wstring, std::uint64_t>> rows;
    std::vector<std::pair<std::wstring, std::uint64_t>> visibleRows;
};

struct PreviewCenterInput {
    std::optional<int> x;
    std::optional<int> y;
    std::optional<int> z;
};

struct PreviewPlacement {
    float sizeX = 0.0f;
    float sizeY = 0.0f;
    float sizeZ = 0.0f;
    float worldCenterX = 0.0f;
    float worldCenterY = 0.0f;
    float worldCenterZ = 0.0f;
    float previewCenterX = 0.0f;
    float previewCenterY = 0.0f;
    float previewCenterZ = 0.0f;
    bool hasCenterX = false;
    bool hasCenterY = false;
    bool hasCenterZ = false;
    bool clampedX = false;
    bool clampedY = false;
    bool clampedZ = false;
};

struct PreviewProjectedPoint {
    float x = 0.0f;
    float y = 0.0f;
    bool visible = false;
};

struct PreviewProjectionSetup {
    int width = 1;
    int height = 1;
    std::array<float, 4> modelRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    float modelScale = 1.0f;
    bool flipX = false;
    bool flipZ = false;
    bool objZUp = false;
    float levelAngleDeg = 0.0f;
    std::array<float, 3> levelAxis{ 1.0f, 0.0f, 0.0f };
    float camYaw = 0.0f;
    float camPitch = 0.0f;
    float panX = 0.0f;
    float panY = 0.0f;
    float distance = 8.0f;
    PreviewPlacement placement;
};

std::wstring GuiReadMappedText(const GuiState* state, const std::wstring& key, const std::wstring& fallback);
float GuiReadMappedFloat(const GuiState* state, const std::wstring& key, float fallback);

bool GuiReadMappedCheckValue(const GuiState* state, const std::wstring& key) {
    if (!state) return false;
    const auto it = state->checks.find(key);
    return it != state->checks.end() && SendMessageW(it->second, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

bool GuiReadEffectiveFlipZ(const GuiState* state) {
    return GuiReadMappedCheckValue(state, L"flipZ") && !GuiReadMappedCheckValue(state, L"noFlipZ");
}

std::optional<std::array<float, 4>> GuiParseLevelRotation(const std::wstring& text) {
    std::wstringstream stream(text);
    std::wstring part;
    std::array<float, 3> normal{};
    int index = 0;
    try {
        while (index < 3 && std::getline(stream, part, L',')) normal[index++] = std::stof(Trim(part));
    } catch (...) {
        return std::nullopt;
    }
    if (index != 3 || std::getline(stream, part, L',')) return std::nullopt;
    const float length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
    if (!std::isfinite(length) || length < 1e-6f) return std::nullopt;
    for (float& value : normal) value /= length;
    const float axisX = normal[1];
    const float axisY = -normal[0];
    const float axisLength = std::sqrt(axisX * axisX + axisY * axisY);
    const float dot = std::clamp(normal[2], -1.0f, 1.0f);
    if (axisLength < 1e-6f) {
        return std::array<float, 4>{ dot >= 0.0f ? 0.0f : 180.0f, 1.0f, 0.0f, 0.0f };
    }
    const float angle = std::atan2(axisLength, dot) * 180.0f / 3.1415926535f;
    return std::array<float, 4>{ angle, axisX / axisLength, axisY / axisLength, 0.0f };
}

std::array<float, 4> GuiNormalizeQuaternion(std::array<float, 4> q) {
    const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!std::isfinite(length) || length < 1e-6f) return { 1.0f, 0.0f, 0.0f, 0.0f };
    for (float& value : q) value /= length;
    return q;
}

std::array<float, 4> GuiMultiplyQuaternions(
    const std::array<float, 4>& a, const std::array<float, 4>& b) {
    return GuiNormalizeQuaternion({
        a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3],
        a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2],
        a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1],
        a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0]
    });
}

std::array<float, 4> GuiConjugateQuaternion(std::array<float, 4> q) {
    q = GuiNormalizeQuaternion(q);
    return { q[0], -q[1], -q[2], -q[3] };
}

std::array<float, 4> GuiConjugateQuaternionByFlip(
    std::array<float, 4> q, bool flipX, bool flipZ) {
    q = GuiNormalizeQuaternion(q);
    const float sx = flipX ? -1.0f : 1.0f;
    const float sz = flipZ ? -1.0f : 1.0f;
    const float determinant = sx * sz;
    q[1] *= determinant * sx;
    q[2] *= determinant;
    q[3] *= determinant * sz;
    return q;
}

std::array<float, 4> GuiAxisAngleQuaternion(float angleDegrees, float x, float y, float z) {
    const float axisLength = std::sqrt(x * x + y * y + z * z);
    if (!std::isfinite(axisLength) || axisLength < 1e-6f) return { 1.0f, 0.0f, 0.0f, 0.0f };
    const float halfAngle = angleDegrees * 3.1415926535f / 360.0f;
    const float scale = std::sin(halfAngle) / axisLength;
    return GuiNormalizeQuaternion({ std::cos(halfAngle), x * scale, y * scale, z * scale });
}

std::array<float, 4> GuiReadSourceQuaternion(const GuiState* state) {
    if (!state || !IEquals(GuiReadMappedText(state, L"transform", L"default"), L"obj-z-up")) {
        return { 1.0f, 0.0f, 0.0f, 0.0f };
    }
    const auto zUp = GuiAxisAngleQuaternion(-90.0f, 1.0f, 0.0f, 0.0f);
    if (const auto level = GuiParseLevelRotation(GuiReadMappedText(state, L"levelNormal", L""))) {
        const auto leveling = GuiAxisAngleQuaternion((*level)[0], (*level)[1], (*level)[2], (*level)[3]);
        return GuiMultiplyQuaternions(zUp, leveling);
    }
    return zUp;
}

std::array<float, 4> GuiEulerXyzToQuaternion(float xDeg, float yDeg, float zDeg) {
    constexpr float kHalfRadians = 3.1415926535f / 360.0f;
    const float cx = std::cos(xDeg * kHalfRadians), sx = std::sin(xDeg * kHalfRadians);
    const float cy = std::cos(yDeg * kHalfRadians), sy = std::sin(yDeg * kHalfRadians);
    const float cz = std::cos(zDeg * kHalfRadians), sz = std::sin(zDeg * kHalfRadians);
    return GuiNormalizeQuaternion({
        cz * cy * cx + sz * sy * sx,
        cz * cy * sx - sz * sy * cx,
        cz * sy * cx + sz * cy * sx,
        sz * cy * cx - cz * sy * sx
    });
}

std::array<float, 3> GuiQuaternionToEulerXyz(std::array<float, 4> q) {
    q = GuiNormalizeQuaternion(q);
    constexpr float kDegrees = 180.0f / 3.1415926535f;
    const float x = std::atan2(2.0f * (q[0] * q[1] + q[2] * q[3]),
        1.0f - 2.0f * (q[1] * q[1] + q[2] * q[2]));
    const float y = std::asin(std::clamp(2.0f * (q[0] * q[2] - q[3] * q[1]), -1.0f, 1.0f));
    const float z = std::atan2(2.0f * (q[0] * q[3] + q[1] * q[2]),
        1.0f - 2.0f * (q[2] * q[2] + q[3] * q[3]));
    return { x * kDegrees, y * kDegrees, z * kDegrees };
}

std::array<float, 4> GuiReadModelQuaternion(const GuiState* state) {
    const std::wstring mode = ToLower(GuiReadMappedText(state, L"rotationMode", L"euler"));
    if (mode == L"quaternion") {
        return GuiNormalizeQuaternion({
            GuiReadMappedFloat(state, L"rotationW", 1.0f),
            GuiReadMappedFloat(state, L"rotationX", 0.0f),
            GuiReadMappedFloat(state, L"rotationY", 0.0f),
            GuiReadMappedFloat(state, L"rotationZ", 0.0f)
        });
    }
    return GuiEulerXyzToQuaternion(
        GuiReadMappedFloat(state, L"rotationX", 0.0f),
        GuiReadMappedFloat(state, L"rotationY", GuiReadMappedFloat(state, L"rotateYDeg", 0.0f)),
        GuiReadMappedFloat(state, L"rotationZ", 0.0f));
}

std::array<float, 3> GuiRotatePointByQuaternion(std::array<float, 3> point, std::array<float, 4> q) {
    q = GuiNormalizeQuaternion(q);
    const std::array<float, 3> u{ q[1], q[2], q[3] };
    const float dot = u[0] * point[0] + u[1] * point[1] + u[2] * point[2];
    const float uu = u[0] * u[0] + u[1] * u[1] + u[2] * u[2];
    const std::array<float, 3> cross{
        u[1] * point[2] - u[2] * point[1],
        u[2] * point[0] - u[0] * point[2],
        u[0] * point[1] - u[1] * point[0]
    };
    for (int i = 0; i < 3; ++i) point[i] = 2.0f * dot * u[i] + (q[0] * q[0] - uu) * point[i] + 2.0f * q[0] * cross[i];
    return point;
}

bool GuiRunRotationMathSelfTest(std::wstring* errorText) {
    auto fail = [&](const wchar_t* message) {
        if (errorText) *errorText = message;
        return false;
    };
    auto nearlyEqual = [](const std::array<float, 3>& a, const std::array<float, 3>& b, float epsilon = 2e-4f) {
        for (int axis = 0; axis < 3; ++axis) {
            if (std::abs(a[axis] - b[axis]) > epsilon) return false;
        }
        return true;
    };
    auto applyFlip = [](std::array<float, 3> point, bool flipX, bool flipZ) {
        if (flipX) point[0] = -point[0];
        if (flipZ) point[2] = -point[2];
        return point;
    };

    const float halfRoot = std::sqrt(0.5f);
    const auto blenderX90 = GuiNormalizeQuaternion({ halfRoot, halfRoot, 0.0f, 0.0f });
    const auto rotatedY = GuiRotatePointByQuaternion({ 0.0f, 1.0f, 0.0f }, blenderX90);
    if (!nearlyEqual(rotatedY, { 0.0f, 0.0f, 1.0f })) return fail(L"Blender WXYZ 顺序不正确");

    const auto changedW = GuiNormalizeQuaternion({ 0.5f, halfRoot, 0.0f, 0.0f });
    const auto changedWPoint = GuiRotatePointByQuaternion({ 0.0f, 1.0f, 0.0f }, changedW);
    if (nearlyEqual(rotatedY, changedWPoint, 1e-3f)) return fail(L"XYZ 非零时 W 没有改变旋转角度");
    const auto negatedPoint = GuiRotatePointByQuaternion(
        { 0.0f, 1.0f, 0.0f }, { -blenderX90[0], -blenderX90[1], -blenderX90[2], -blenderX90[3] });
    if (!nearlyEqual(rotatedY, negatedPoint)) return fail(L"q 与 -q 未保持等价");

    const auto sourceToWorld = GuiAxisAngleQuaternion(-90.0f, 1.0f, 0.0f, 0.0f);
    const auto leveling = GuiAxisAngleQuaternion(37.0f, 0.2f, 0.8f, 0.5f);
    const auto worldLeveling = GuiMultiplyQuaternions(
        GuiMultiplyQuaternions(sourceToWorld, leveling), GuiConjugateQuaternion(sourceToWorld));
    const std::array<float, 3> testPoint{ 1.25f, -0.75f, 2.5f };
    for (const bool flipX : { false, true }) {
        for (const bool flipZ : { false, true }) {
            auto oldPipeline = GuiRotatePointByQuaternion(testPoint, leveling);
            oldPipeline = GuiRotatePointByQuaternion(oldPipeline, sourceToWorld);
            oldPipeline = applyFlip(oldPipeline, flipX, flipZ);

            auto newPipeline = GuiRotatePointByQuaternion(testPoint, sourceToWorld);
            newPipeline = applyFlip(newPipeline, flipX, flipZ);
            newPipeline = GuiRotatePointByQuaternion(
                newPipeline, GuiConjugateQuaternionByFlip(worldLeveling, flipX, flipZ));
            if (!nearlyEqual(oldPipeline, newPipeline)) return fail(L"找平旋转写入字段后与原变换不等价");
        }
    }
    if (errorText) errorText->clear();
    return true;
}

void GuiPopulatePreviewTransformSetup(const GuiState* state, PreviewProjectionSetup* setup) {
    if (!state || !setup) return;
    setup->modelRotation = GuiReadModelQuaternion(state);
    setup->modelScale = std::max(0.001f, GuiReadMappedFloat(state, L"scaleBlocksPerMeter", 4.0f));
    setup->flipX = GuiReadMappedCheckValue(state, L"flipX");
    setup->flipZ = GuiReadEffectiveFlipZ(state);
    setup->objZUp = IEquals(GuiReadMappedText(state, L"transform", L"default"), L"obj-z-up");
    if (setup->objZUp) {
        if (const auto level = GuiParseLevelRotation(GuiReadMappedText(state, L"levelNormal", L""))) {
            setup->levelAngleDeg = (*level)[0];
            setup->levelAxis = { (*level)[1], (*level)[2], (*level)[3] };
        }
    }
}

std::array<float, 3> GuiTransformPreviewModelPoint(const PreviewProjectionSetup& setup, float x, float y, float z) {
    if (setup.objZUp && std::abs(setup.levelAngleDeg) > 1e-5f) {
        const float angle = setup.levelAngleDeg * 3.1415926535f / 180.0f;
        const float cosA = std::cos(angle);
        const float sinA = std::sin(angle);
        const auto& axis = setup.levelAxis;
        const float dot = axis[0] * x + axis[1] * y + axis[2] * z;
        const float crossX = axis[1] * z - axis[2] * y;
        const float crossY = axis[2] * x - axis[0] * z;
        const float crossZ = axis[0] * y - axis[1] * x;
        const float rx = x * cosA + crossX * sinA + axis[0] * dot * (1.0f - cosA);
        const float ry = y * cosA + crossY * sinA + axis[1] * dot * (1.0f - cosA);
        const float rz = z * cosA + crossZ * sinA + axis[2] * dot * (1.0f - cosA);
        x = rx;
        y = ry;
        z = rz;
    }
    if (setup.objZUp) {
        const float worldY = z;
        const float worldZ = -y;
        y = worldY;
        z = worldZ;
    }
    x *= setup.flipX ? -setup.modelScale : setup.modelScale;
    y *= setup.modelScale;
    z *= setup.flipZ ? -setup.modelScale : setup.modelScale;
    return GuiRotatePointByQuaternion({ x, y, z }, setup.modelRotation);
}

std::array<float, 3> GuiComputePreviewWorldSize(const GuiState* state, const native_mc::PreviewMesh& mesh) {
    PreviewProjectionSetup setup;
    GuiPopulatePreviewTransformSetup(state, &setup);
    std::array<float, 3> min{ INFINITY, INFINITY, INFINITY };
    std::array<float, 3> max{ -INFINITY, -INFINITY, -INFINITY };
    for (float x : { mesh.min[0], mesh.max[0] }) {
        for (float y : { mesh.min[1], mesh.max[1] }) {
            for (float z : { mesh.min[2], mesh.max[2] }) {
                const auto point = GuiTransformPreviewModelPoint(setup, x, y, z);
                for (int axis = 0; axis < 3; ++axis) {
                    min[axis] = std::min(min[axis], point[axis]);
                    max[axis] = std::max(max[axis], point[axis]);
                }
            }
        }
    }
    return { max[0] - min[0], max[1] - min[1], max[2] - min[2] };
}

void GuiApplyQuaternionOpenGl(std::array<float, 4> q) {
    q = GuiNormalizeQuaternion(q);
    const float angle = 2.0f * std::acos(std::clamp(q[0], -1.0f, 1.0f));
    const float sinHalf = std::sqrt(std::max(0.0f, 1.0f - q[0] * q[0]));
    if (angle < 1e-6f || sinHalf < 1e-6f) return;
    glRotatef(angle * 180.0f / 3.1415926535f, q[1] / sinHalf, q[2] / sinHalf, q[3] / sinHalf);
}

Config GuiReadConfig(GuiState* state);
void GuiPopulateControls(GuiState* state, const Config& config);
void GuiUpdateWorkflow(GuiState* state);
void GuiRunCommandAsync(GuiState* state, const std::wstring& command);
LRESULT CALLBACK GuiPaletteDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK GuiBreakdownDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
void GuiOpenPaletteDialog(GuiState* state);
void GuiOpenBreakdownDialog(GuiState* state);
void GuiUpdatePreviewChrome(GuiState* state);
void GuiRenderPreviewScene(GuiState* state, HWND hwnd);
void GuiSyncPreviewTransformInspector(GuiState* state);
void GuiRenderPreviewNow(GuiState* state);
void GuiStartPreviewFrameScheduler(GuiState* state);
void GuiStopPreviewFrameScheduler(GuiState* state);
void GuiLoadPreviewAsync(GuiState* state);
void GuiLoadAllPreviewsAsync(GuiState* state);
void GuiShowPreviewProgress(GuiState* state, const std::wstring& title, int total, bool marquee);
void GuiClosePreviewProgress(GuiState* state);
void GuiRefreshWorldPreviewAsync(GuiState* state);
void GuiScheduleWorldPreviewRefresh(GuiState* state);
PreviewCenterInput GuiResolvePreviewCenterInput(const GuiState* state);
void GuiWritePreviewCenterInput(GuiState* state, const PreviewCenterInput& center);
PreviewPlacement GuiResolvePreviewPlacement(const GuiState* state, float sizeX, float sizeY, float sizeZ);
void GuiSetPreviewToolMode(GuiState* state, PreviewToolMode mode);
void GuiNudgePreviewCenter(GuiState* state, int dx, int dy, int dz);
PreviewPlacementDragMode GuiHitTestPreviewGizmo(GuiState* state, HWND hwnd, const POINT& pt);
bool GuiBuildPreviewProjectionSetup(GuiState* state, int width, int height, PreviewProjectionSetup* out);
PreviewProjectedPoint GuiProjectWorldPoint(const PreviewProjectionSetup& setup, float x, float y, float z);
PreviewEditState GuiCapturePreviewEditState(GuiState* state);
void GuiApplyPreviewEditState(GuiState* state, const PreviewEditState& editState);
void GuiResetPreviewHistory(GuiState* state);
void GuiRecordPreviewHistory(GuiState* state);
bool GuiPreviewCanUndo(const GuiState* state);
bool GuiPreviewCanRedo(const GuiState* state);
void GuiPreviewUndo(GuiState* state);
void GuiPreviewRedo(GuiState* state);
std::wstring GuiPreviewPresetSummary(const PreviewEditState& editState);
std::wstring EncodePreviewEditState(const PreviewEditState& editState);
bool DecodePreviewEditState(const std::wstring& text, PreviewEditState* outState);
void GuiLoadPreviewPresetsFromConfig(GuiState* state, const Config& config);
void GuiSyncPreviewPresetsToConfig(GuiState* state);
void GuiSavePreviewPreset(GuiState* state, int slot);
void GuiLoadPreviewPreset(GuiState* state, int slot);
void GuiRefreshModelList(GuiState* state);
void GuiApplySelectedModel(GuiState* state, int index);
void GuiLoadSelectedPreviewsAsync(GuiState* state);
void GuiSelectAdjacentModel(GuiState* state, int delta);
void GuiUpdateModelPanel(GuiState* state);
void GuiPersistPreviewWorldSettings(GuiState* state);

std::wstring GuiControlText(HWND hwnd) {
    int length = GetWindowTextLengthW(hwnd);
    std::wstring value(length, L'\0');
    GetWindowTextW(hwnd, value.data(), length + 1);
    return value;
}

std::optional<fs::path> GuiBrowseFile(HWND owner, const wchar_t* filter) {
    wchar_t buffer[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = filter;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        return fs::path(buffer);
    }
    return std::nullopt;
}

std::optional<fs::path> GuiBrowseFolder(HWND owner, const wchar_t* title) {
    BROWSEINFOW bi{};
    bi.hwndOwner = owner;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return std::nullopt;
    wchar_t pathBuffer[MAX_PATH] = {};
    std::optional<fs::path> result;
    if (SHGetPathFromIDListW(pidl, pathBuffer)) {
        result = fs::path(pathBuffer);
    }
    CoTaskMemFree(pidl);
    return result;
}

void GuiSetControlText(HWND hwnd, const std::wstring& value) {
    if (!hwnd) return;
    wchar_t className[32] = {};
    GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
    if (IEquals(className, L"ComboBox")) {
        const LRESULT index = SendMessageW(hwnd, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(value.c_str()));
        if (index != CB_ERR) SendMessageW(hwnd, CB_SETCURSEL, index, 0);
        return;
    }
    SetWindowTextW(hwnd, value.c_str());
}

std::wstring GuiDecodeOptionValue(const std::wstring& key, const std::wstring& display) {
    if (key == L"lastMode") {
        if (display == L"GLB 贴图导入") return L"glb-textured";
        if (display == L"GLB 单方块表面") return L"glb-surface";
        if (display == L"OBJ 贴图导入") return L"obj";
        if (display == L"OBJ 单方块表面") return L"obj-surface";
    } else if (key == L"palette") {
        if (display == L"场景均衡（推荐）") return L"scene";
        if (display == L"高彩度方块") return L"colourful";
        if (display == L"仅混凝土") return L"concrete";
        if (display == L"全部可用方块") return L"raw";
    } else if (key == L"transform") {
        if (display == L"默认坐标系") return L"default";
        if (display == L"OBJ：Z 轴向上") return L"obj-z-up";
    } else if (key == L"rotationMode") {
        if (display == L"XYZ 欧拉") return L"euler";
        if (display == L"Blender 四元数 (WXYZ)") return L"quaternion";
        if (display == L"四元数 (WXYZ)") return L"quaternion";
    } else if (key == L"lang") {
        if (display == L"中文") return L"zh";
        if (display == L"English") return L"en";
    } else if (key == L"previewRefreshRate") {
        if (display == L"30 Hz") return L"30";
        if (display == L"60 Hz") return L"60";
        if (display == L"120 Hz") return L"120";
        if (display == L"144 Hz") return L"144";
        if (display == L"180 Hz") return L"180";
        if (display == L"200 Hz") return L"200";
        if (display == L"无上限") return L"0";
    } else if (key == L"previewRenderer") {
        if (display.rfind(L"自动", 0) == 0) return L"auto";
        if (display == L"Direct3D 11") return L"d3d11";
        if (display == L"Direct3D 12") return L"d3d12";
        if (display == L"Vulkan") return L"vulkan";
        if (display == L"OpenGL（兼容）") return L"opengl";
    }
    return display;
}

std::wstring GuiEncodeOptionValue(const std::wstring& key, const std::wstring& value) {
    const std::wstring lower = ToLower(value);
    if (key == L"lastMode") {
        if (lower == L"obj") return L"OBJ 贴图导入";
        if (lower == L"obj-surface") return L"OBJ 单方块表面";
        if (lower == L"glb-surface") return L"GLB 单方块表面";
        return L"GLB 贴图导入";
    }
    if (key == L"palette") {
        if (lower == L"colourful") return L"高彩度方块";
        if (lower == L"concrete") return L"仅混凝土";
        if (lower == L"raw") return L"全部可用方块";
        return L"场景均衡（推荐）";
    }
    if (key == L"transform") return lower == L"obj-z-up" ? L"OBJ：Z 轴向上" : L"默认坐标系";
    if (key == L"rotationMode") return lower == L"quaternion" ? L"Blender 四元数 (WXYZ)" : L"XYZ 欧拉";
    if (key == L"lang") return lower == L"en" ? L"English" : L"中文";
    if (key == L"previewRefreshRate") {
        const int refreshRate = NormalizePreviewRefreshRate(ParseOptionalIntText(value).value_or(60));
        return refreshRate == 0 ? L"无上限" : std::to_wstring(refreshRate) + L" Hz";
    }
    if (key == L"previewRenderer") {
        if (lower == L"d3d11") return L"Direct3D 11";
        if (lower == L"d3d12") return L"Direct3D 12";
        if (lower == L"vulkan") return L"Vulkan";
        if (lower == L"opengl") return L"OpenGL（兼容）";
        return L"自动（D3D12 / Vulkan / D3D11）";
    }
    return value;
}

std::wstring GuiReadMappedText(const GuiState* state, const std::wstring& key, const std::wstring& fallback = L"") {
    if (!state) return fallback;
    auto it = state->edits.find(key);
    if (it == state->edits.end()) return fallback;
    const std::wstring text = GuiDecodeOptionValue(key, Trim(GuiControlText(it->second)));
    return text.empty() ? fallback : text;
}

float GuiReadMappedFloat(const GuiState* state, const std::wstring& key, float fallback) {
    return ParseOptionalFloatText(GuiReadMappedText(state, key)).value_or(fallback);
}

int GuiReadMappedInt(const GuiState* state, const std::wstring& key, int fallback) {
    const std::wstring text = GuiReadMappedText(state, key);
    if (text.empty()) return fallback;
    try {
        return std::stoi(text);
    } catch (...) {
        return fallback;
    }
}

void GuiWriteMappedText(GuiState* state, const std::wstring& key, const std::wstring& value) {
    if (!state) return;
    auto it = state->edits.find(key);
    if (it != state->edits.end()) {
        GuiSetControlText(it->second, GuiEncodeOptionValue(key, value));
    }
}

bool GuiGetCheck(HWND hwnd) {
    return SendMessageW(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void GuiSetCheck(HWND hwnd, bool value) {
    SendMessageW(hwnd, BM_SETCHECK, value ? BST_CHECKED : BST_UNCHECKED, 0);
}

void GuiAppendLog(GuiState* state, const std::wstring& text) {
    if (!state || !state->log) return;
    int end = GetWindowTextLengthW(state->log);
    SendMessageW(state->log, EM_SETSEL, end, end);
    SendMessageW(state->log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
}

void GuiSetStatus(GuiState* state, const std::wstring& text) {
    if (state && state->status) {
        SetWindowTextW(state->status, text.c_str());
    }
}

fs::path GuiResolvePreviewSource(const Config& config) {
    auto resolveDirFile = [](const fs::path& dir, const std::wstring& only, const std::wstring& ext) -> fs::path {
        if (dir.empty()) return {};
        if (!only.empty()) return dir / fs::path(only);
        const auto files = CollectFilesByExtension(dir, ext);
        return files.empty() ? fs::path() : files.front();
    };

    if (!config.inputPath.empty()) {
        std::error_code ec;
        if (fs::is_regular_file(config.inputPath, ec)) return config.inputPath;
        if (fs::is_directory(config.inputPath, ec)) {
            if (!config.onlyFile.empty()) return config.inputPath / fs::path(config.onlyFile);
            const std::wstring mode = ToLower(config.lastMode);
            if (mode == L"obj") {
                const auto objFiles = CollectFilesByExtension(config.inputPath, L".obj");
                if (!objFiles.empty()) return objFiles.front();
                const auto glbFiles = CollectFilesByExtension(config.inputPath, L".glb");
                if (!glbFiles.empty()) return glbFiles.front();
            } else {
                const auto glbFiles = CollectFilesByExtension(config.inputPath, L".glb");
                if (!glbFiles.empty()) return glbFiles.front();
                const auto objFiles = CollectFilesByExtension(config.inputPath, L".obj");
                if (!objFiles.empty()) return objFiles.front();
            }
        }
    }
    if (!config.onlyFile.empty()) {
        const std::wstring ext = ToLower(fs::path(config.onlyFile).extension().wstring());
        if (ext == L".obj") return resolveDirFile(config.objDir, config.onlyFile, L".obj");
        if (ext == L".glb") return resolveDirFile(config.glbDir, config.onlyFile, L".glb");
    }
    if (IsObjMode(ToLower(config.lastMode))) return resolveDirFile(config.objDir, config.onlyFile, L".obj");
    if (ToLower(config.lastMode) == L"glb-surface" || ToLower(config.lastMode) == L"glb-textured") return resolveDirFile(config.glbDir, config.onlyFile, L".glb");
    return {};
}

std::vector<fs::path> GuiCollectModelFiles(const Config& config) {
    std::vector<fs::path> files;
    std::set<std::wstring> seen;
    auto addDir = [&](const fs::path& dir) {
        if (dir.empty() || !HasDir(dir)) return;
        for (const std::wstring& ext : { std::wstring(L".obj"), std::wstring(L".glb") }) {
            for (const auto& file : CollectFilesByExtension(dir, ext)) {
                const std::wstring key = ToLower(file.lexically_normal().wstring());
                if (seen.insert(key).second) files.push_back(file);
            }
        }
    };

    if (!config.inputPath.empty()) {
        std::error_code ec;
        const std::wstring inputExt = ToLower(config.inputPath.extension().wstring());
        if (fs::is_regular_file(config.inputPath, ec) && (inputExt == L".obj" || inputExt == L".glb")) {
            const fs::path normalized = NormalizeIfExists(config.inputPath);
            const std::wstring key = ToLower(normalized.lexically_normal().wstring());
            if (seen.insert(key).second) files.push_back(normalized);
        } else if (fs::is_directory(config.inputPath, ec)) {
            addDir(config.inputPath);
        }
    }
    addDir(config.objDir);
    addDir(config.glbDir);

    std::sort(files.begin(), files.end());
    return files;
}

std::vector<int> GuiGetSelectedModelIndices(const GuiState* state) {
    std::vector<int> indices;
    if (!state || !state->modelList) return indices;
    const LRESULT count = SendMessageW(state->modelList, LB_GETSELCOUNT, 0, 0);
    if (count == LB_ERR || count <= 0) return indices;
    indices.resize(static_cast<std::size_t>(count));
    const LRESULT copied = SendMessageW(
        state->modelList, LB_GETSELITEMS, static_cast<WPARAM>(indices.size()),
        reinterpret_cast<LPARAM>(indices.data()));
    if (copied == LB_ERR || copied <= 0) {
        indices.clear();
    } else {
        indices.resize(static_cast<std::size_t>(copied));
    }
    return indices;
}

int GuiGetPrimarySelectedModelIndex(const GuiState* state) {
    if (!state || !state->modelList) return -1;
    const int caret = static_cast<int>(SendMessageW(state->modelList, LB_GETCARETINDEX, 0, 0));
    if (caret >= 0 && caret < static_cast<int>(state->visibleModelFiles.size()) &&
        SendMessageW(state->modelList, LB_GETSEL, static_cast<WPARAM>(caret), 0) > 0) {
        return caret;
    }
    const auto indices = GuiGetSelectedModelIndices(state);
    return indices.empty() ? -1 : indices.front();
}

void GuiSelectOnlyModel(GuiState* state, int index) {
    if (!state || !state->modelList || index < 0 ||
        index >= static_cast<int>(state->visibleModelFiles.size())) return;
    SendMessageW(state->modelList, LB_SETSEL, FALSE, static_cast<LPARAM>(-1));
    SendMessageW(state->modelList, LB_SETSEL, TRUE, index);
    SendMessageW(state->modelList, LB_SETCARETINDEX, index, FALSE);
}

void GuiRefreshModelList(GuiState* state) {
    if (!state || !state->modelList || !state->modelSummaryLabel || !state->modelFilterEdit) return;
    std::set<std::wstring> selectedPaths;
    for (const int index : GuiGetSelectedModelIndices(state)) {
        if (index >= 0 && index < static_cast<int>(state->visibleModelFiles.size())) {
            selectedPaths.insert(ToLower(state->visibleModelFiles[index].lexically_normal().wstring()));
        }
    }
    state->config = GuiReadConfig(state);
    state->modelFiles = GuiCollectModelFiles(state->config);
    const std::wstring filter = ToLower(Trim(GuiControlText(state->modelFilterEdit)));
    state->visibleModelFiles.clear();
    for (const auto& file : state->modelFiles) {
        const std::wstring name = ToLower(file.filename().wstring());
        if (filter.empty() || name.find(filter) != std::wstring::npos) {
            state->visibleModelFiles.push_back(file);
        }
    }

    SendMessageW(state->modelList, LB_RESETCONTENT, 0, 0);
    std::vector<int> selectedIndices;
    int fallbackIndex = -1;
    const std::wstring currentOnly = ToLower(state->config.onlyFile);
    const std::wstring currentPreview = ToLower(state->previewSource.filename().wstring());
    for (std::size_t i = 0; i < state->visibleModelFiles.size(); ++i) {
        const std::wstring label = state->visibleModelFiles[i].filename().wstring();
        SendMessageW(state->modelList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        const std::wstring fileKey = ToLower(state->visibleModelFiles[i].filename().wstring());
        const std::wstring pathKey = ToLower(state->visibleModelFiles[i].lexically_normal().wstring());
        if (selectedPaths.find(pathKey) != selectedPaths.end()) {
            selectedIndices.push_back(static_cast<int>(i));
        }
        if ((fallbackIndex < 0 && !currentOnly.empty() && fileKey == currentOnly) ||
            (fallbackIndex < 0 && !currentPreview.empty() && fileKey == currentPreview)) {
            fallbackIndex = static_cast<int>(i);
        }
    }
    if (selectedIndices.empty() && fallbackIndex >= 0) {
        selectedIndices.push_back(fallbackIndex);
    }
    for (const int index : selectedIndices) {
        SendMessageW(state->modelList, LB_SETSEL, TRUE, index);
    }
    if (!selectedIndices.empty()) {
        SendMessageW(state->modelList, LB_SETCARETINDEX, selectedIndices.front(), FALSE);
    }

    std::size_t objCount = 0;
    std::size_t glbCount = 0;
    for (const auto& file : state->visibleModelFiles) {
        if (IEquals(file.extension().wstring(), L".obj")) ++objCount;
        else if (IEquals(file.extension().wstring(), L".glb")) ++glbCount;
    }
    const std::wstring modeLabel = objCount > 0 && glbCount > 0 ? L"OBJ + GLB" : (objCount > 0 ? L"OBJ" : L"GLB");
    std::wstring dirLabel;
    if (!state->config.inputPath.empty()) dirLabel = PathToDisplay(state->config.inputPath);
    else if (objCount > 0) dirLabel = PathToDisplay(state->config.objDir);
    else dirLabel = PathToDisplay(state->config.glbDir);
    const std::wstring summary = L"模式: " + modeLabel + L"  共 " + FormatNumber(state->visibleModelFiles.size()) +
        L" 个文件" + (dirLabel.empty() ? L"" : L"  目录: " + dirLabel);
    SetWindowTextW(state->modelSummaryLabel, summary.c_str());
    GuiUpdateModelPanel(state);
}

void GuiApplySelectedModel(GuiState* state, int index) {
    if (!state || index < 0 || index >= static_cast<int>(state->visibleModelFiles.size())) return;
    GuiSelectOnlyModel(state, index);
    const fs::path selected = state->visibleModelFiles[index];
    const std::wstring ext = ToLower(selected.extension().wstring());
    GuiSetControlText(state->edits[L"onlyFile"], selected.filename().wstring());
    GuiSetControlText(state->edits[L"inputPath"], selected.parent_path().wstring());
    if (ext == L".obj") {
        GuiSetControlText(state->edits[L"objDir"], selected.parent_path().wstring());
        const std::wstring currentMode = ToLower(GuiReadMappedText(state, L"lastMode", L""));
        GuiWriteMappedText(state, L"lastMode", currentMode == L"obj-surface" ? L"obj-surface" : L"obj");
    } else if (ext == L".glb") {
        GuiSetControlText(state->edits[L"glbDir"], selected.parent_path().wstring());
        const std::wstring currentMode = ToLower(GuiReadMappedText(state, L"lastMode", L""));
        if (currentMode != L"glb-surface") {
            GuiWriteMappedText(state, L"lastMode", L"glb-textured");
        }
    }
    state->config = GuiReadConfig(state);
    GuiRefreshModelList(state);
    GuiLoadPreviewAsync(state);
}

void GuiSelectAdjacentModel(GuiState* state, int delta) {
    if (!state || state->visibleModelFiles.empty()) return;
    int index = GuiGetPrimarySelectedModelIndex(state);
    if (index < 0) index = 0;
    index = std::clamp(index + delta, 0, static_cast<int>(state->visibleModelFiles.size()) - 1);
    GuiSelectOnlyModel(state, index);
    GuiApplySelectedModel(state, index);
}

void GuiEstimateLevelFromSelected(GuiState* state) {
    if (!state || state->previewLoading || !state->modelList) return;
    const int index = GuiGetPrimarySelectedModelIndex(state);
    if (index < 0 || index >= static_cast<int>(state->visibleModelFiles.size())) {
        MessageBoxW(state->hwnd, L"请先在模型列表中选择一个 OBJ 模型。", L"找平面参考", MB_ICONINFORMATION);
        return;
    }
    const fs::path source = state->visibleModelFiles[index];
    if (!IEquals(source.extension().wstring(), L".obj")) {
        MessageBoxW(state->hwnd, L"找平面参考目前需要 OBJ 模型。", L"找平面参考", MB_ICONWARNING);
        return;
    }
    state->config = GuiReadConfig(state);
    auto parseDouble = [](const std::wstring& text, double fallback) {
        try { return text.empty() ? fallback : std::stod(text); } catch (...) { return fallback; }
    };
    auto parseInt = [](const std::wstring& text, int fallback) {
        try { return text.empty() ? fallback : std::stoi(text); } catch (...) { return fallback; }
    };
    const double cellSize = parseDouble(state->config.clipCellSize, 1.0);
    const double lowFraction = parseDouble(state->config.clipLowFraction, 0.25);
    const int passes = parseInt(state->config.clipPasses, 3);
    const double trimSigma = parseDouble(state->config.clipTrimSigma, 1.5);
    state->previewLoading = true;
    GuiShowPreviewProgress(state, L"正在计算找平面", 1, true);
    if (state->previewProgressLabel) SetWindowTextW(state->previewProgressLabel, (L"正在分析 " + source.filename().wstring()).c_str());
    GuiSetStatus(state, L"正在用所选模型估算找平法线...");
    GuiUpdateWorkflow(state);
    const HWND hwnd = state->hwnd;
    const std::uint64_t generation = ++state->previewLevelGeneration;
    const std::wstring transform = GuiReadMappedText(state, L"transform", L"default");
    const bool flipX = state->config.flipX;
    const bool flipZ = state->config.flipZ && !state->config.noFlipZ;
    std::thread([hwnd, generation, source, transform, flipX, flipZ, cellSize, lowFraction, passes, trimSigma]() {
        auto* payload = new LevelReferencePayload();
        payload->generation = generation;
        payload->source = source;
        payload->transform = transform;
        payload->flipX = flipX;
        payload->flipZ = flipZ;
        std::array<double, 3> normal{};
        std::string error;
        payload->ok = native_mc::EstimateObjLevelNormal(source, cellSize, lowFraction, passes, trimSigma, &normal, &error);
        if (payload->ok) {
            payload->normal = FormatFloatCompact(static_cast<float>(normal[0]), 8) + L"," +
                FormatFloatCompact(static_cast<float>(normal[1]), 8) + L"," +
                FormatFloatCompact(static_cast<float>(normal[2]), 8);
            payload->message = L"已使用 " + source.filename().wstring() + L" 计算找平面: " + payload->normal;
        } else {
            payload->message = Utf8ToWide(error.empty() ? std::string("找平面计算失败") : error);
        }
        if (!PostMessageW(hwnd, WM_GUI_LEVEL_READY, 0, reinterpret_cast<LPARAM>(payload))) delete payload;
    }).detach();
}

COLORREF GuiFallbackBlockColor(const std::string& blockName) {
    unsigned int hash = 2166136261u;
    for (unsigned char ch : blockName) {
        hash ^= ch;
        hash *= 16777619u;
    }
    return RGB(80 + (hash & 0x5f), 80 + ((hash >> 8) & 0x5f), 80 + ((hash >> 16) & 0x5f));
}

std::map<std::string, COLORREF> GuiBuildWorldColorMap(const fs::path& projectRoot, const native_mc::WorldPreviewSlice& slice) {
    std::map<std::string, COLORREF> colors;
    native_mc::PaletteCatalog catalog;
    native_mc::PaletteOptions options;
    options.mode = "scene";
    std::string error;
    if (native_mc::LoadPaletteCatalog(projectRoot, options, &catalog, &error)) {
        for (const auto& block : catalog.availableBlocks) {
            colors[block.name] = RGB(block.r, block.g, block.b);
        }
    }
    for (const auto& block : slice.blocks) {
        if (colors.find(block.blockName) == colors.end()) {
            colors[block.blockName] = GuiFallbackBlockColor(block.blockName);
        }
    }
    return colors;
}

WorldPreviewGpuMesh GuiBuildWorldPreviewGpuMesh(
    const native_mc::WorldPreviewSlice& slice,
    const std::map<std::string, COLORREF>& colorMap,
    PreviewWorldStyle style) {
    WorldPreviewGpuMesh mesh;
    mesh.style = style;
    mesh.primitive = style == PreviewWorldStyle::Points ? GL_POINTS : GL_QUADS;
    const std::size_t verticesPerBlock = style == PreviewWorldStyle::Points ? 1u :
        (style == PreviewWorldStyle::Faces ? 4u : 20u);
    constexpr std::size_t kGpuBudgetBytes = 256u * 1024u * 1024u;
    const std::size_t bytesPerBlock = verticesPerBlock * (sizeof(float) * 3u + sizeof(std::uint8_t) * 3u);
    const std::size_t budgetBlocks = std::max<std::size_t>(1, kGpuBudgetBytes / bytesPerBlock);
    const std::size_t glBlocks = static_cast<std::size_t>(std::numeric_limits<GLsizei>::max()) / verticesPerBlock;
    const std::size_t blockLimit = std::min(budgetBlocks, glBlocks);
    const std::size_t blockStep = std::max<std::size_t>(1, (slice.blocks.size() + blockLimit - 1) / blockLimit);
    const std::size_t blockCount = slice.blocks.empty() ? 0 : (slice.blocks.size() + blockStep - 1) / blockStep;
    mesh.densityReduced = blockStep > 1;
    mesh.positions.reserve(blockCount * verticesPerBlock * 3);
    mesh.colors.reserve(blockCount * verticesPerBlock * 3);

    auto addVertex = [&](float x, float y, float z, COLORREF color, float shade) {
        mesh.positions.insert(mesh.positions.end(), { x, y, z });
        mesh.colors.push_back(static_cast<std::uint8_t>(std::clamp(GetRValue(color) * shade, 0.0f, 255.0f)));
        mesh.colors.push_back(static_cast<std::uint8_t>(std::clamp(GetGValue(color) * shade, 0.0f, 255.0f)));
        mesh.colors.push_back(static_cast<std::uint8_t>(std::clamp(GetBValue(color) * shade, 0.0f, 255.0f)));
    };
    auto addQuad = [&](const std::array<std::array<float, 3>, 4>& quad, COLORREF color, float shade) {
        for (const auto& vertex : quad) addVertex(vertex[0], vertex[1], vertex[2], color, shade);
    };

    auto columnKey = [](int x, int z) {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) |
            static_cast<std::uint32_t>(z);
    };
    std::unordered_map<std::uint64_t, const native_mc::WorldPreviewBlock*> columns;
    if (style == PreviewWorldStyle::Voxels) {
        columns.reserve(slice.blocks.size());
        for (const auto& block : slice.blocks) columns.emplace(columnKey(block.x, block.z), &block);
    }
    auto exposedBottom = [&](const native_mc::WorldPreviewBlock& block, int neighborX, int neighborZ) {
        float bottom = static_cast<float>(block.bottomY) - 0.5f;
        if (const auto it = columns.find(columnKey(neighborX, neighborZ)); it != columns.end()) {
            bottom = std::max(bottom, static_cast<float>(it->second->y) + 0.5f);
        }
        return bottom;
    };

    for (std::size_t i = 0; i < slice.blocks.size(); i += blockStep) {
        const auto& block = slice.blocks[i];
        COLORREF color = GuiFallbackBlockColor(block.blockName);
        if (const auto it = colorMap.find(block.blockName); it != colorMap.end()) color = it->second;
        const float x = static_cast<float>(block.x), y = static_cast<float>(block.y), z = static_cast<float>(block.z);
        if (style == PreviewWorldStyle::Points) {
            addVertex(x, y, z, color, 1.0f);
            continue;
        }
        const float halfSpanX = static_cast<float>(std::max(1, block.sampleSizeX)) * 0.5f;
        const float halfSpanZ = static_cast<float>(std::max(1, block.sampleSizeZ)) * 0.5f;
        const float x0 = x - halfSpanX, x1 = x + halfSpanX;
        const float y1 = y + 0.5f;
        const float z0 = z - halfSpanZ, z1 = z + halfSpanZ;
        addQuad({{{x0,y1,z0},{x1,y1,z0},{x1,y1,z1},{x0,y1,z1}}}, color, 1.0f);
        if (style == PreviewWorldStyle::Faces) continue;
        const float x0Bottom = exposedBottom(block, block.x - std::max(1, block.sampleSizeX), block.z);
        if (x0Bottom < y1) addQuad({{{x0,x0Bottom,z0},{x0,y1,z0},{x0,y1,z1},{x0,x0Bottom,z1}}}, color, 0.72f);
        const float z0Bottom = exposedBottom(block, block.x, block.z - std::max(1, block.sampleSizeZ));
        if (z0Bottom < y1) addQuad({{{x0,z0Bottom,z0},{x1,z0Bottom,z0},{x1,y1,z0},{x0,y1,z0}}}, color, 0.82f);
        if (style == PreviewWorldStyle::Voxels) {
            const float x1Bottom = exposedBottom(block, block.x + std::max(1, block.sampleSizeX), block.z);
            if (x1Bottom < y1) addQuad({{{x1,x1Bottom,z1},{x1,y1,z1},{x1,y1,z0},{x1,x1Bottom,z0}}}, color, 0.68f);
            const float z1Bottom = exposedBottom(block, block.x, block.z + std::max(1, block.sampleSizeZ));
            if (z1Bottom < y1) addQuad({{{x1,z1Bottom,z1},{x0,z1Bottom,z1},{x0,y1,z1},{x1,y1,z1}}}, color, 0.78f);
        }
    }
    return mesh;
}

void GuiScheduleWorldGpuBuild(GuiState* state) {
    if (!state || state->previewWorldSlice.blocks.empty()) return;
    const std::uint64_t generation = ++state->previewWorldGpuGeneration;
    state->previewWorldGpuBuilding = true;
    const HWND hwnd = state->hwnd;
    const PreviewWorldStyle style = state->previewWorldStyle;
    native_mc::WorldPreviewSlice slice = state->previewWorldSlice;
    std::map<std::string, COLORREF> colors = state->previewWorldColors;
    std::thread([hwnd, generation, style, slice = std::move(slice), colors = std::move(colors)]() mutable {
        auto* payload = new WorldPreviewGpuPayload();
        payload->generation = generation;
        payload->mesh = GuiBuildWorldPreviewGpuMesh(slice, colors, style);
        if (!PostMessageW(hwnd, WM_GUI_WORLD_GPU_READY, 0, reinterpret_cast<LPARAM>(payload))) delete payload;
    }).detach();
}

bool GuiWorldHasRegionFiles(const fs::path& worldDir) {
    if (worldDir.empty()) return false;
    const fs::path regionDir = native_mc::ResolveOverworldRegionDir(worldDir);
    if (!HasDir(regionDir)) return false;
    for (const auto& entry : fs::directory_iterator(regionDir)) {
        if (entry.is_regular_file() && IEquals(entry.path().extension().wstring(), L".mca")) {
            return true;
        }
    }
    return false;
}

std::optional<fs::path> GuiNewestPreviewWorldUnder(const fs::path& root) {
    if (!HasDir(root)) return std::nullopt;
    std::optional<fs::path> best;
    std::filesystem::file_time_type bestTime{};
    for (const auto& entry : fs::directory_iterator(root)) {
        if (!entry.is_directory()) continue;
        if (!GuiWorldHasRegionFiles(entry.path())) continue;
        std::error_code ec;
        const auto stamp = fs::last_write_time(entry.path(), ec);
        if (!best.has_value() || (!ec && stamp > bestTime)) {
            best = entry.path();
            if (!ec) bestTime = stamp;
        }
    }
    return best;
}

fs::path GuiResolveBestWorldPreviewDir(GuiState* state, Config* resolvedConfig = nullptr) {
    if (!state) return {};
    Config current = GuiReadConfig(state);
    ParsedArgs args;
    auto tryAdopt = [&](const fs::path& worldPath) -> fs::path {
        if (worldPath.empty()) return {};
        if (resolvedConfig) {
            resolvedConfig->worldDir = worldPath;
            resolvedConfig->worldName = worldPath.filename().wstring();
        }
        return worldPath;
    };

    if (GuiWorldHasRegionFiles(current.worldDir)) return tryAdopt(NormalizeIfExists(current.worldDir));
    const fs::path resolvedMain = ResolveWorldDir(state->backend, current, args);
    if (GuiWorldHasRegionFiles(resolvedMain)) return tryAdopt(resolvedMain);
    const fs::path testRoot = ResolveTestWorldRoot(state->exeDir, state->backend, current, args);
    if (auto newest = GuiNewestPreviewWorldUnder(testRoot)) return tryAdopt(*newest);
    return resolvedMain;
}

void GuiRefreshWorldPreviewAsync(GuiState* state) {
    if (!state || state->previewWorldLoading) return;
    Config resolvedConfig;
    const fs::path worldDir = GuiResolveBestWorldPreviewDir(state, &resolvedConfig);
    if (worldDir.empty()) {
        GuiSetStatus(state, L"找不到可叠加的世界目录");
        return;
    }
    // Preview fallback worlds are read-only context and must not replace the
    // world path explicitly selected by the user.
    state->config = GuiReadConfig(state);

    int centerX = 0;
    int centerZ = 0;
    int minY = -64;
    int maxY = 128;
    int radiusX = state->previewWorldRadius;
    int radiusZ = state->previewWorldRadius;
    if (state->previewLoaded && !state->previewMesh.indices.empty()) {
        const auto& mesh = state->previewMesh;
        const auto worldSize = GuiComputePreviewWorldSize(state, mesh);
        const PreviewPlacement placement = GuiResolvePreviewPlacement(
            state, worldSize[0], worldSize[1], worldSize[2]);
        centerX = static_cast<int>(std::lround(placement.worldCenterX));
        centerZ = static_cast<int>(std::lround(placement.worldCenterZ));
        minY = static_cast<int>(std::floor(placement.worldCenterY - placement.sizeY * 0.5f - 24.0f));
        maxY = static_cast<int>(std::ceil(placement.worldCenterY + placement.sizeY * 0.5f + 24.0f));
    }

    state->previewWorldLoading = true;
    state->previewWorldDirty = false;
    GuiSetStatus(state, L"正在加载存档实景...");
    const fs::path projectRoot = !state->backend.root.empty() ? state->backend.root : state->config.projectRoot;
    HWND hwnd = state->hwnd;
    const PreviewWorldStyle gpuStyle = state->previewWorldStyle;
    const std::size_t maxBlocks = static_cast<std::size_t>(std::max(1000, state->previewWorldMaxBlocks));
    std::thread([hwnd, worldDir, projectRoot, centerX, centerZ, radiusX, radiusZ, minY, maxY, maxBlocks, gpuStyle]() {
        auto* payload = new WorldPreviewPayload();
        std::string error;
        payload->ok = native_mc::LoadWorldPreviewSlice(worldDir, centerX, centerZ, radiusX, radiusZ, minY, maxY, maxBlocks, &payload->slice, &error);
        const bool hasNearbyTerrain = payload->ok && std::any_of(
            payload->slice.blocks.begin(), payload->slice.blocks.end(),
            [centerX, centerZ](const native_mc::WorldPreviewBlock& block) {
                return std::abs(block.x - centerX) <= 128 && std::abs(block.z - centerZ) <= 128;
            });
        if (payload->ok && !hasNearbyTerrain) {
            int fallbackCenterX = 0;
            int fallbackCenterZ = 0;
            std::string anchorError;
            if (native_mc::FindWorldPreviewAnchor(worldDir, &fallbackCenterX, &fallbackCenterZ, &anchorError)) {
                payload->ok = native_mc::LoadWorldPreviewSlice(worldDir, fallbackCenterX, fallbackCenterZ,
                    radiusX, radiusZ, std::nullopt, std::nullopt, maxBlocks, &payload->slice, &error);
            }
        }
        if (payload->ok) {
            payload->colors = GuiBuildWorldColorMap(projectRoot, payload->slice);
            payload->gpuMesh = GuiBuildWorldPreviewGpuMesh(payload->slice, payload->colors, gpuStyle);
            payload->worldLabel = worldDir.filename().wstring();
            payload->message = L"已加载存档实景 " + FormatNumber(payload->slice.blocks.size()) + L" 个点  [" + worldDir.filename().wstring() + L"]";
            if (payload->slice.truncated) payload->message += L"（已抽样）";
        } else {
            payload->message = Utf8ToWide(error.empty() ? std::string("加载存档实景失败") : error);
        }
        PostMessageW(hwnd, WM_GUI_WORLD_READY, 0, reinterpret_cast<LPARAM>(payload));
    }).detach();
}

void GuiScheduleWorldPreviewRefresh(GuiState* state) {
    if (!state || !state->previewWorldVisible) return;
    if (state->previewWorldLoading) {
        state->previewWorldDirty = true;
        return;
    }
    GuiRefreshWorldPreviewAsync(state);
}

std::wstring GuiPreviewModelLabel(const GuiState* state) {
    if (!state) return L"(未选择)";
    if (state->previewAllModels) {
        return (state->previewAllAvailableModels ? L"全部模型 (" : L"所选模型 (") +
            FormatNumber(state->previewModelCount) + L")";
    }
    return state->previewSource.empty() ? L"(未选择)" : state->previewSource.filename().wstring();
}

void GuiUpdateModelPanel(GuiState* state) {
    if (!state) return;
    auto currentText = std::wstring(L"当前模型: ");
    if (state->previewLoaded && !state->previewSource.empty()) {
        currentText += GuiPreviewModelLabel(state);
        currentText += L"  顶点 " + FormatNumber(state->previewMesh.positions.size() / 3);
        currentText += L"  三角形 " + FormatNumber(state->previewMesh.indices.size() / 3);
    } else {
        currentText += L"未加载";
    }
    if (state->modelCurrentLabel) {
        SetWindowTextW(state->modelCurrentLabel, currentText.c_str());
    }

    std::wstring selectedText = L"所选模型: ";
    const auto selectedIndices = GuiGetSelectedModelIndices(state);
    if (selectedIndices.size() == 1 && selectedIndices.front() >= 0 &&
        selectedIndices.front() < static_cast<int>(state->visibleModelFiles.size())) {
        selectedText += state->visibleModelFiles[selectedIndices.front()].filename().wstring();
    } else if (!selectedIndices.empty()) {
        const bool selectedAll = selectedIndices.size() == state->modelFiles.size() &&
            state->visibleModelFiles.size() == state->modelFiles.size();
        selectedText += selectedAll ? L"全部 " : L"";
        selectedText += FormatNumber(selectedIndices.size()) + L" 个模型";
    } else {
        selectedText += L"(无)";
    }
    if (state->modelSelectedLabel) {
        SetWindowTextW(state->modelSelectedLabel, selectedText.c_str());
    }
}

void GuiPersistPreviewWorldSettings(GuiState* state) {
    if (!state) return;
    state->config.previewWorldVisible = state->previewWorldVisible;
    state->config.previewWorldRadius = state->previewWorldRadius;
    state->config.previewWorldMaxBlocks = state->previewWorldMaxBlocks;
    switch (state->previewWorldStyle) {
    case PreviewWorldStyle::Points: state->config.previewWorldStyle = L"points"; break;
    case PreviewWorldStyle::Faces: state->config.previewWorldStyle = L"faces"; break;
    case PreviewWorldStyle::Voxels: state->config.previewWorldStyle = L"voxels"; break;
    }
    SaveConfig(state->configPath, state->config);
}

native_mc::PreviewRendererBackend GuiPreviewRendererBackend(const std::wstring& value) {
    const std::wstring lower = ToLower(Trim(value));
    if (lower == L"d3d11" || lower == L"direct3d11") return native_mc::PreviewRendererBackend::D3D11;
    if (lower == L"d3d12" || lower == L"direct3d12") return native_mc::PreviewRendererBackend::D3D12;
    if (lower == L"vulkan") return native_mc::PreviewRendererBackend::Vulkan;
    if (lower == L"opengl") return native_mc::PreviewRendererBackend::OpenGL;
    return native_mc::PreviewRendererBackend::Auto;
}

std::wstring GuiPreviewRendererLabel(native_mc::PreviewRendererBackend backend) {
    switch (backend) {
    case native_mc::PreviewRendererBackend::D3D11: return L"Direct3D 11";
    case native_mc::PreviewRendererBackend::D3D12: return L"Direct3D 12";
    case native_mc::PreviewRendererBackend::Vulkan: return L"Vulkan";
    case native_mc::PreviewRendererBackend::OpenGL: return L"OpenGL";
    case native_mc::PreviewRendererBackend::Auto: return L"自动";
    default: return L"未初始化";
    }
}

bool GuiUsingNativePreviewRenderer(const GuiState* state) {
    return state && state->previewNativeRenderer && state->previewNativeRenderer->IsReady() &&
        state->previewActiveRenderer != native_mc::PreviewRendererBackend::OpenGL;
}

void GuiResetPreviewPresentRate(GuiState* state) {
    if (!state) return;
    state->previewPresentFps = 0.0;
    state->previewPresentBackend = native_mc::PreviewRendererBackend::None;
    state->previewPresentFrameNumber = 0;
    state->previewPresentSampleFrame = 0;
    state->previewOpenGlFrameNumber = 0;
    state->previewPresentSampleStarted = {};
    state->previewLastPresentAt = {};
}

bool GuiInitPreviewContext(GuiState* state) {
    if (!state || !state->preview) return false;
    if (GuiUsingNativePreviewRenderer(state)) return true;
    if (state->previewActiveRenderer == native_mc::PreviewRendererBackend::OpenGL && state->previewRc) return true;
    const HWND rootWindow = GetAncestor(state->preview, GA_ROOT);
    if (!rootWindow || !IsWindowVisible(rootWindow)) return false;

    const auto requested = GuiPreviewRendererBackend(state->config.previewRenderer);
    if (state->previewActiveRenderer != native_mc::PreviewRendererBackend::OpenGL &&
        requested != native_mc::PreviewRendererBackend::OpenGL) {
        auto renderer = std::make_unique<native_mc::PreviewRenderer>();
        native_mc::PreviewRendererOptions options;
        options.backend = requested;
        options.verticalSync = false;
        options.allowSoftwareFallback = true;
        options.preferDiscreteGpu = true;
        options.maximumFrameLatency = 1;
        options.framesInFlight = 2;
        std::string error;
        if (renderer->Initialize(state->preview, options, &error)) {
            state->previewActiveRenderer = renderer->ActiveBackend();
            state->previewRendererStatus = GuiPreviewRendererLabel(state->previewActiveRenderer);
            if (!renderer->AdapterName().empty()) {
                state->previewRendererStatus += L" / " + Utf8ToWide(renderer->AdapterName());
            }
            state->previewRendererError = Utf8ToWide(renderer->LastError());
            state->previewNativeRenderer = std::move(renderer);
            return true;
        }
        state->previewRendererError = Utf8ToWide(error);
        state->previewNativeRenderer.reset();
    }

    state->previewActiveRenderer = native_mc::PreviewRendererBackend::OpenGL;
    state->previewRendererStatus = state->previewRendererError.empty() ? L"OpenGL（兼容）" : L"OpenGL（后端回退）";
    if (state->previewRc && state->previewDc) return true;
    if (!state->previewDc) state->previewDc = GetDC(state->preview);
    if (!state->previewDc) return false;

    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 24;
    pfd.cDepthBits = 24;
    pfd.iLayerType = PFD_MAIN_PLANE;
    if (GetPixelFormat(state->previewDc) == 0) {
        int pf = ChoosePixelFormat(state->previewDc, &pfd);
        if (pf == 0) return false;
        if (!SetPixelFormat(state->previewDc, pf, &pfd)) return false;
    }
    state->previewRc = wglCreateContext(state->previewDc);
    if (!state->previewRc) return false;
    wglMakeCurrent(state->previewDc, state->previewRc);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    if (!gGlGenBuffers) {
        gGlGenBuffers = reinterpret_cast<PFNGLGENBUFFERSPROC>(wglGetProcAddress("glGenBuffers"));
        gGlBindBuffer = reinterpret_cast<PFNGLBINDBUFFERPROC>(wglGetProcAddress("glBindBuffer"));
        gGlBufferData = reinterpret_cast<PFNGLBUFFERDATAPROC>(wglGetProcAddress("glBufferData"));
        gGlDeleteBuffers = reinterpret_cast<PFNGLDELETEBUFFERSPROC>(wglGetProcAddress("glDeleteBuffers"));
    }
    return true;
}

void GuiRenderPreviewNow(GuiState* state) {
    if (!state || !state->preview || !IsWindowVisible(state->preview)) return;
    state->previewFrameActive.store(false, std::memory_order_release);
    state->previewFrameTickPending.store(false, std::memory_order_release);
    state->previewFramePermit = false;
    RedrawWindow(state->preview, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
}

bool GuiPreviewIsInteracting(const GuiState* state) {
    return state && (state->orbiting || state->panning ||
        state->previewDraggingPlacement || state->previewDraggingHeight);
}

void GuiRequestPreviewFrame(GuiState* state) {
    if (!state || !state->previewLoaded) return;
    state->previewFrameActive.store(true, std::memory_order_release);
}

void GuiReleasePreviewGpu(GuiState* state) {
    if (!state) return;
    if (state->previewNativeRenderer) state->previewNativeRenderer->ClearMesh();
    if (state->previewRc && state->previewDc) wglMakeCurrent(state->previewDc, state->previewRc);
    if (gGlDeleteBuffers) {
        if (state->previewPositionBuffer) gGlDeleteBuffers(1, &state->previewPositionBuffer);
        if (state->previewNormalBuffer) gGlDeleteBuffers(1, &state->previewNormalBuffer);
        if (state->previewColorBuffer) gGlDeleteBuffers(1, &state->previewColorBuffer);
        for (GLuint& buffer : state->previewIndexBuffers) {
            if (buffer) gGlDeleteBuffers(1, &buffer);
            buffer = 0;
        }
    }
    state->previewPositionBuffer = 0;
    state->previewNormalBuffer = 0;
    state->previewColorBuffer = 0;
    state->previewIndexCounts.fill(0);
    state->previewGpuReady = false;
}

bool GuiUploadPreviewGpu(GuiState* state) {
    if (!state || !state->previewGpuDirty) return state && state->previewGpuReady;
    GuiReleasePreviewGpu(state);
    state->previewGpuDirty = false;
    if (GuiUsingNativePreviewRenderer(state)) {
        if (state->previewMesh.positions.empty() || state->previewMesh.indices.empty()) return false;
        std::string error;
        state->previewGpuReady = state->previewNativeRenderer->UploadMesh(state->previewMesh, &error);
        if (!state->previewGpuReady) state->previewRendererError = Utf8ToWide(error);
        return state->previewGpuReady;
    }
    if (!gGlGenBuffers || !gGlBindBuffer || !gGlBufferData || !gGlDeleteBuffers ||
        state->previewMesh.positions.empty() || state->previewMesh.indices.empty()) return false;

    const auto& mesh = state->previewMesh;
    gGlGenBuffers(1, &state->previewPositionBuffer);
    gGlBindBuffer(GL_ARRAY_BUFFER, state->previewPositionBuffer);
    gGlBufferData(GL_ARRAY_BUFFER, static_cast<GlBufferSize>(mesh.positions.size() * sizeof(float)),
                  mesh.positions.data(), GL_STATIC_DRAW);
    gGlGenBuffers(1, &state->previewNormalBuffer);
    gGlBindBuffer(GL_ARRAY_BUFFER, state->previewNormalBuffer);
    gGlBufferData(GL_ARRAY_BUFFER, static_cast<GlBufferSize>(mesh.normals.size() * sizeof(float)),
                  mesh.normals.data(), GL_STATIC_DRAW);
    gGlGenBuffers(1, &state->previewColorBuffer);
    gGlBindBuffer(GL_ARRAY_BUFFER, state->previewColorBuffer);
    gGlBufferData(GL_ARRAY_BUFFER, static_cast<GlBufferSize>(mesh.colors.size() * sizeof(std::uint8_t)),
                  mesh.colors.data(), GL_STATIC_DRAW);

    for (std::size_t level = 0; level < state->previewIndexBuffers.size(); ++level) {
        const auto& indices = (level == 0 || mesh.lodIndices[level].empty()) ? mesh.indices : mesh.lodIndices[level];
        if (indices.empty()) continue;
        gGlGenBuffers(1, &state->previewIndexBuffers[level]);
        gGlBindBuffer(GL_ELEMENT_ARRAY_BUFFER, state->previewIndexBuffers[level]);
        gGlBufferData(GL_ELEMENT_ARRAY_BUFFER,
                      static_cast<GlBufferSize>(indices.size() * sizeof(std::uint32_t)), indices.data(), GL_STATIC_DRAW);
        state->previewIndexCounts[level] = static_cast<GLsizei>(
            std::min<std::size_t>(indices.size(), static_cast<std::size_t>(std::numeric_limits<GLsizei>::max())));
    }
    gGlBindBuffer(GL_ARRAY_BUFFER, 0);
    gGlBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    state->previewGpuReady = state->previewPositionBuffer && state->previewNormalBuffer &&
        state->previewColorBuffer && state->previewIndexBuffers[0];
    if (!state->previewGpuReady) GuiReleasePreviewGpu(state);
    return state->previewGpuReady;
}

void GuiReleasePreviewWorldGpu(GuiState* state) {
    if (!state) return;
    if (state->previewNativeRenderer) state->previewNativeRenderer->ClearWorldMesh();
    if (state->previewRc && state->previewDc) wglMakeCurrent(state->previewDc, state->previewRc);
    if (gGlDeleteBuffers) {
        if (state->previewWorldPositionBuffer) gGlDeleteBuffers(1, &state->previewWorldPositionBuffer);
        if (state->previewWorldColorBuffer) gGlDeleteBuffers(1, &state->previewWorldColorBuffer);
    }
    state->previewWorldPositionBuffer = 0;
    state->previewWorldColorBuffer = 0;
    state->previewWorldVertexCount = 0;
    state->previewWorldGpuReady = false;
}

bool GuiUploadPreviewWorldGpu(GuiState* state) {
    if (!state || !state->previewWorldGpuDirty) return state && state->previewWorldGpuReady;
    GuiReleasePreviewWorldGpu(state);
    state->previewWorldGpuDirty = false;
    if (GuiUsingNativePreviewRenderer(state)) {
        const auto& mesh = state->previewWorldPendingGpuMesh;
        if (mesh.style != state->previewWorldStyle || mesh.positions.empty() || mesh.colors.empty()) return false;
        native_mc::PreviewRendererWorldMeshView view;
        view.positions = mesh.positions.data();
        view.positionFloatCount = mesh.positions.size();
        view.colors = mesh.colors.data();
        view.colorByteCount = mesh.colors.size();
        view.colorChannels = 3;
        if (mesh.primitive == GL_POINTS) view.primitive = native_mc::PreviewRendererWorldPrimitive::Points;
        else if (mesh.primitive == GL_TRIANGLES) view.primitive = native_mc::PreviewRendererWorldPrimitive::Triangles;
        else view.primitive = native_mc::PreviewRendererWorldPrimitive::Quads;
        std::string error;
        state->previewWorldGpuReady = state->previewNativeRenderer->UploadWorldMesh(view, &error);
        if (!state->previewWorldGpuReady) {
            state->previewRendererError = Utf8ToWide(error);
            return false;
        }
        state->previewWorldVertexCount = static_cast<GLsizei>(std::min<std::size_t>(
            mesh.positions.size() / 3, static_cast<std::size_t>(std::numeric_limits<GLsizei>::max())));
        state->previewWorldPrimitive = mesh.primitive;
        state->previewWorldGpuStyle = mesh.style;
        state->previewWorldGpuDensityReduced = mesh.densityReduced;
        return true;
    }
    if (!gGlGenBuffers || !gGlBindBuffer || !gGlBufferData || !gGlDeleteBuffers ||
        state->previewWorldPendingGpuMesh.style != state->previewWorldStyle ||
        state->previewWorldPendingGpuMesh.positions.empty() || state->previewWorldPendingGpuMesh.colors.empty()) return false;

    WorldPreviewGpuMesh mesh = std::move(state->previewWorldPendingGpuMesh);
    gGlGenBuffers(1, &state->previewWorldPositionBuffer);
    gGlBindBuffer(GL_ARRAY_BUFFER, state->previewWorldPositionBuffer);
    gGlBufferData(GL_ARRAY_BUFFER, static_cast<GlBufferSize>(mesh.positions.size() * sizeof(float)), mesh.positions.data(), GL_STATIC_DRAW);
    gGlGenBuffers(1, &state->previewWorldColorBuffer);
    gGlBindBuffer(GL_ARRAY_BUFFER, state->previewWorldColorBuffer);
    gGlBufferData(GL_ARRAY_BUFFER, static_cast<GlBufferSize>(mesh.colors.size() * sizeof(std::uint8_t)), mesh.colors.data(), GL_STATIC_DRAW);
    gGlBindBuffer(GL_ARRAY_BUFFER, 0);
    state->previewWorldVertexCount = static_cast<GLsizei>(mesh.positions.size() / 3);
    state->previewWorldPrimitive = mesh.primitive;
    state->previewWorldGpuStyle = mesh.style;
    state->previewWorldGpuDensityReduced = mesh.densityReduced;
    state->previewWorldGpuReady = state->previewWorldPositionBuffer && state->previewWorldColorBuffer;
    if (!state->previewWorldGpuReady) GuiReleasePreviewWorldGpu(state);
    return state->previewWorldGpuReady;
}

void GuiDestroyPreviewContext(GuiState* state) {
    if (!state) return;
    GuiReleasePreviewWorldGpu(state);
    GuiReleasePreviewGpu(state);
    if (state->previewNativeRenderer) {
        state->previewNativeRenderer->Shutdown();
        state->previewNativeRenderer.reset();
    }
    state->previewActiveRenderer = native_mc::PreviewRendererBackend::None;
    if (state->previewRc) {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(state->previewRc);
        state->previewRc = nullptr;
    }
    if (state->previewDc && state->preview) {
        ReleaseDC(state->preview, state->previewDc);
        state->previewDc = nullptr;
    }
}

void GuiSwitchPreviewRenderer(GuiState* state, const std::wstring& requestedValue,
                              bool forceRefresh = false, bool persistConfig = true) {
    if (!state || !state->hwnd) return;
    const std::wstring requested = ToLower(Trim(requestedValue.empty() ? L"auto" : requestedValue));
    const auto requestedBackend = GuiPreviewRendererBackend(requested);
    if (!forceRefresh &&
        requestedBackend == GuiPreviewRendererBackend(state->config.previewRenderer) &&
        state->previewActiveRenderer != native_mc::PreviewRendererBackend::None) return;

    if (!forceRefresh && state->previewDxgiOverlayRefreshPending) {
        KillTimer(state->hwnd, PREVIEW_DXGI_OVERLAY_REFRESH_TIMER);
        state->previewDxgiOverlayRefreshPending = false;
    }

    const bool restartScheduler = state->previewFrameThread.joinable();
    GuiStopPreviewFrameScheduler(state);
    RECT previewRect{ 0, 0, 100, 100 };
    HWND oldPreview = state->preview;
    if (oldPreview && GetCapture() == oldPreview) ReleaseCapture();
    state->orbiting = false;
    state->panning = false;
    state->previewDraggingPlacement = false;
    state->previewDraggingHeight = false;
    state->previewPlacementDragMode = PreviewPlacementDragMode::None;
    state->previewHoverDragMode = PreviewPlacementDragMode::None;
    if (oldPreview && IsWindow(oldPreview)) {
        GetWindowRect(oldPreview, &previewRect);
        MapWindowPoints(nullptr, state->hwnd, reinterpret_cast<POINT*>(&previewRect), 2);
    }

    state->config.previewRenderer = requested;
    if (persistConfig) SaveConfig(state->configPath, state->config);
    GuiDestroyPreviewContext(state);
    if (oldPreview && IsWindow(oldPreview)) DestroyWindow(oldPreview);
    state->preview = nullptr;
    state->previewRendererStatus.clear();
    state->previewRendererError.clear();
    state->previewGpuDirty = true;
    state->previewWorldGpuDirty = true;
    GuiResetPreviewPresentRate(state);

    state->preview = CreateWindowExW(0, L"MCPreviewWindow", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_CLIPSIBLINGS,
        previewRect.left, previewRect.top,
        std::max(1L, previewRect.right - previewRect.left),
        std::max(1L, previewRect.bottom - previewRect.top),
        state->hwnd, (HMENU)IDC_GUI_PREVIEW, nullptr, nullptr);
    if (!state->preview) {
        GuiSetStatus(state, L"重建预览画布失败，请重新启动程序");
        GuiAppendLog(state, L"切换渲染 API 时无法重建预览画布。\r\n");
        if (restartScheduler) GuiStartPreviewFrameScheduler(state);
        return;
    }

    const bool initialized = GuiInitPreviewContext(state);
    SendMessageW(state->hwnd, WM_SIZE, 0, 0);
    if (state->previewWorldVisible && !state->previewWorldSlice.blocks.empty() &&
        state->previewWorldPendingGpuMesh.positions.empty()) {
        GuiScheduleWorldGpuBuild(state);
    }
    if (restartScheduler) {
        GuiStartPreviewFrameScheduler(state);
        state->previewFrameActive.store(false, std::memory_order_release);
    }
    const std::wstring status = initialized
        ? (L"已刷新预览 API: " + state->previewRendererStatus)
        : L"预览图形后端初始化失败";
    GuiSetStatus(state, status);
    if (!state->previewRendererError.empty()) {
        GuiAppendLog(state, L"预览后端信息: " + state->previewRendererError + L"\r\n");
    }
    GuiUpdatePreviewChrome(state);
    GuiRenderPreviewNow(state);
}

void GuiApplyPerspective(float aspect, float fovYDeg, float zNear, float zFar) {
    const float fovRad = fovYDeg * 3.1415926535f / 180.0f;
    const float top = std::tan(fovRad * 0.5f) * zNear;
    const float right = top * aspect;
    glFrustum(-right, right, -top, top, zNear, zFar);
}

struct PreviewGridLayout {
    float step = 1.0f;
    int lineCount = 64;
    float extent = 64.0f;
};

PreviewGridLayout GuiComputePreviewGridLayout(const GuiState* state) {
    PreviewGridLayout grid;
    const float distance = state ? state->previewDistance : 8.0f;
    const float requestedExtent = std::max(64.0f, distance * 0.8f);
    while (requestedExtent / grid.step > 64.0f) grid.step *= 4.0f;
    grid.lineCount = std::max(1, static_cast<int>(std::ceil(requestedExtent / grid.step)));
    grid.extent = grid.lineCount * grid.step;
    return grid;
}

void GuiComputePreviewFit(GuiState* state) {
    if (!state || !state->previewLoaded) return;
    const auto& mesh = state->previewMesh;
    const float scale = std::max(0.001f, GuiReadMappedFloat(state, L"scaleBlocksPerMeter", 4.0f));
    const float dx = (mesh.max[0] - mesh.min[0]) * scale;
    const float dy = (mesh.max[1] - mesh.min[1]) * scale;
    const float dz = (mesh.max[2] - mesh.min[2]) * scale;
    float radius = std::sqrt(dx * dx + dy * dy + dz * dz) * 0.5f;

    if (state->previewWorldVisible && !state->previewWorldSlice.blocks.empty()) {
        const auto& first = state->previewWorldSlice.blocks.front();
        float minX = static_cast<float>(first.x) - std::max(1, first.sampleSizeX) * 0.5f;
        float maxX = static_cast<float>(first.x) + std::max(1, first.sampleSizeX) * 0.5f;
        float minY = static_cast<float>(state->previewWorldStyle == PreviewWorldStyle::Voxels ? first.bottomY : first.y) - 0.5f;
        float maxY = static_cast<float>(first.y) + 0.5f;
        float minZ = static_cast<float>(first.z) - std::max(1, first.sampleSizeZ) * 0.5f;
        float maxZ = static_cast<float>(first.z) + std::max(1, first.sampleSizeZ) * 0.5f;
        for (const auto& block : state->previewWorldSlice.blocks) {
            const float halfX = std::max(1, block.sampleSizeX) * 0.5f;
            const float halfZ = std::max(1, block.sampleSizeZ) * 0.5f;
            minX = std::min(minX, static_cast<float>(block.x) - halfX);
            maxX = std::max(maxX, static_cast<float>(block.x) + halfX);
            const int visibleBottom = state->previewWorldStyle == PreviewWorldStyle::Voxels ? block.bottomY : block.y;
            minY = std::min(minY, static_cast<float>(visibleBottom) - 0.5f);
            maxY = std::max(maxY, static_cast<float>(block.y) + 0.5f);
            minZ = std::min(minZ, static_cast<float>(block.z) - halfZ);
            maxZ = std::max(maxZ, static_cast<float>(block.z) + halfZ);
        }
        const float worldDx = maxX - minX;
        const float worldDy = maxY - minY;
        const float worldDz = maxZ - minZ;
        radius = std::max(radius, std::sqrt(worldDx * worldDx + worldDy * worldDy + worldDz * worldDz) * 0.5f);
    }

    radius = std::max(0.5f, radius);
    state->previewDistance = std::max(3.0f, radius * 2.5f);
    state->previewPanX = 0.0f;
    state->previewPanY = 0.0f;
    state->previewYaw = 35.0f;
    state->previewPitch = 20.0f;
}

constexpr int kPreviewRotateTrackBias = 180;
constexpr int kPreviewScaleTrackMin = 1;
constexpr int kPreviewScaleTrackMax = 160;
constexpr int kPreviewBaseYTrackBias = 64;
constexpr int kPreviewBaseYTrackMax = 320;

int PreviewRotateToTrack(float value) {
    return std::clamp(static_cast<int>(std::lround(value)), -180, 180) + kPreviewRotateTrackBias;
}

float PreviewTrackToRotate(int value) {
    return static_cast<float>(std::clamp(value - kPreviewRotateTrackBias, -180, 180));
}

int PreviewScaleToTrack(float value) {
    return std::clamp(static_cast<int>(std::lround(value * 10.0f)), kPreviewScaleTrackMin, kPreviewScaleTrackMax);
}

float PreviewTrackToScale(int value) {
    return std::clamp(value, kPreviewScaleTrackMin, kPreviewScaleTrackMax) / 10.0f;
}

int PreviewBaseYToTrack(int value) {
    return std::clamp(value, -kPreviewBaseYTrackBias, kPreviewBaseYTrackMax) + kPreviewBaseYTrackBias;
}

int PreviewTrackToBaseY(int value) {
    return std::clamp(value - kPreviewBaseYTrackBias, -kPreviewBaseYTrackBias, kPreviewBaseYTrackMax);
}

void GuiSetMappedCheckValue(GuiState* state, const std::wstring& key, bool value) {
    if (!state) return;
    auto it = state->checks.find(key);
    if (it != state->checks.end()) {
        GuiSetCheck(it->second, value);
    }
}

void GuiUpdateRotationModeControls(GuiState* state) {
    if (!state) return;
    const bool quaternion = ToLower(GuiReadMappedText(state, L"rotationMode", L"euler")) == L"quaternion";
    const wchar_t* quaternionLabels[] = { L"四元数 W", L"四元数 X", L"四元数 Y", L"四元数 Z" };
    const wchar_t* eulerLabels[] = { L"", L"旋转 X", L"旋转 Y", L"旋转 Z" };
    for (int i = 0; i < 4; ++i) {
        const bool visible = quaternion || i > 0;
        if (state->previewRotationLabels[i]) {
            SetWindowTextW(state->previewRotationLabels[i], quaternion ? quaternionLabels[i] : eulerLabels[i]);
            ShowWindow(state->previewRotationLabels[i], visible ? SW_SHOW : SW_HIDE);
        }
        if (state->previewRotationEdits[i]) ShowWindow(state->previewRotationEdits[i], visible ? SW_SHOW : SW_HIDE);
    }
}

bool GuiWriteLevelRotationToControls(GuiState* state, const std::wstring& normalText) {
    if (!state) return false;
    const auto level = GuiParseLevelRotation(normalText);
    if (!level) return false;

    auto modelQuaternion = GuiAxisAngleQuaternion((*level)[0], (*level)[1], (*level)[2], (*level)[3]);
    if (IEquals(GuiReadMappedText(state, L"transform", L"default"), L"obj-z-up")) {
        const auto sourceToWorld = GuiAxisAngleQuaternion(-90.0f, 1.0f, 0.0f, 0.0f);
        modelQuaternion = GuiMultiplyQuaternions(
            GuiMultiplyQuaternions(sourceToWorld, modelQuaternion),
            GuiConjugateQuaternion(sourceToWorld));
    }
    const bool flipX = GuiReadMappedCheckValue(state, L"flipX");
    const bool flipZ = GuiReadEffectiveFlipZ(state);
    modelQuaternion = GuiConjugateQuaternionByFlip(modelQuaternion, flipX, flipZ);

    const bool wasPopulating = state->populatingControls;
    state->populatingControls = true;
    if (ToLower(GuiReadMappedText(state, L"rotationMode", L"euler")) == L"quaternion") {
        GuiWriteMappedText(state, L"rotationW", FormatFloatCompact(modelQuaternion[0], 7));
        GuiWriteMappedText(state, L"rotationX", FormatFloatCompact(modelQuaternion[1], 7));
        GuiWriteMappedText(state, L"rotationY", FormatFloatCompact(modelQuaternion[2], 7));
        GuiWriteMappedText(state, L"rotationZ", FormatFloatCompact(modelQuaternion[3], 7));
    } else {
        const auto euler = GuiQuaternionToEulerXyz(modelQuaternion);
        GuiWriteMappedText(state, L"rotationX", FormatFloatCompact(euler[0], 4));
        GuiWriteMappedText(state, L"rotationY", FormatFloatCompact(euler[1], 4));
        GuiWriteMappedText(state, L"rotationZ", FormatFloatCompact(euler[2], 4));
        GuiWriteMappedText(state, L"rotateYDeg", FormatFloatCompact(euler[1], 4));
    }
    GuiWriteMappedText(state, L"levelNormal", L"");
    state->populatingControls = wasPopulating;
    return true;
}

void GuiConvertRotationMode(GuiState* state, const std::wstring& nextMode) {
    if (!state) return;
    const std::wstring currentMode = ToLower(state->config.rotationMode.empty()
        ? GuiReadMappedText(state, L"rotationMode", L"euler") : state->config.rotationMode);
    if (currentMode == nextMode) return;
    const bool wasPopulating = state->populatingControls;
    state->populatingControls = true;
    if (nextMode == L"quaternion") {
        const auto q = GuiEulerXyzToQuaternion(
            GuiReadMappedFloat(state, L"rotationX", 0.0f),
            GuiReadMappedFloat(state, L"rotationY", 0.0f),
            GuiReadMappedFloat(state, L"rotationZ", 0.0f));
        GuiWriteMappedText(state, L"rotationW", FormatFloatCompact(q[0], 6));
        GuiWriteMappedText(state, L"rotationX", FormatFloatCompact(q[1], 6));
        GuiWriteMappedText(state, L"rotationY", FormatFloatCompact(q[2], 6));
        GuiWriteMappedText(state, L"rotationZ", FormatFloatCompact(q[3], 6));
    } else {
        const auto euler = GuiQuaternionToEulerXyz({
            GuiReadMappedFloat(state, L"rotationW", 1.0f),
            GuiReadMappedFloat(state, L"rotationX", 0.0f),
            GuiReadMappedFloat(state, L"rotationY", 0.0f),
            GuiReadMappedFloat(state, L"rotationZ", 0.0f)
        });
        GuiWriteMappedText(state, L"rotationX", FormatFloatCompact(euler[0], 3));
        GuiWriteMappedText(state, L"rotationY", FormatFloatCompact(euler[1], 3));
        GuiWriteMappedText(state, L"rotationZ", FormatFloatCompact(euler[2], 3));
    }
    GuiWriteMappedText(state, L"rotationMode", nextMode);
    state->config.rotationMode = nextMode;
    state->populatingControls = wasPopulating;
    GuiUpdateRotationModeControls(state);
}

void GuiSyncPreviewTransformInspector(GuiState* state) {
    if (!state) return;
    const float rotate = GuiReadMappedFloat(state, L"rotateYDeg", 0.0f);
    const float scale = GuiReadMappedFloat(state, L"scaleBlocksPerMeter", 4.0f);
    const int baseY = GuiReadMappedInt(state, L"baseY", 0);
    const bool flipX = state->checks.count(L"flipX") ? GuiGetCheck(state->checks[L"flipX"]) : false;
    const bool flipZ = state->checks.count(L"flipZ") ? GuiGetCheck(state->checks[L"flipZ"]) : false;
    const std::wstring transformMode = ToLower(GuiReadMappedText(state, L"transform", L""));
    GuiUpdateRotationModeControls(state);

    if (state->previewRotateTrack) {
        SendMessageW(state->previewRotateTrack, TBM_SETPOS, TRUE, PreviewRotateToTrack(rotate));
    }
    if (state->previewRotateLabel) {
        SetWindowTextW(state->previewRotateLabel, (L"Y 旋转: " + FormatFloatCompact(rotate, 0) + L"°").c_str());
    }
    if (state->previewScaleTrack) {
        SendMessageW(state->previewScaleTrack, TBM_SETPOS, TRUE, PreviewScaleToTrack(scale));
    }
    if (state->previewScaleLabel) {
        SetWindowTextW(state->previewScaleLabel, L"缩放 block/m");
    }
    if (state->previewBaseYTrack) {
        SendMessageW(state->previewBaseYTrack, TBM_SETPOS, TRUE, PreviewBaseYToTrack(baseY));
    }
    if (state->previewBaseYLabel) {
        SetWindowTextW(state->previewBaseYLabel, L"基准 Y");
    }
    if (state->previewFlipXCheck) GuiSetCheck(state->previewFlipXCheck, flipX);
    if (state->previewFlipZCheck) GuiSetCheck(state->previewFlipZCheck, flipZ);
    if (state->previewTransformButton) {
        SetWindowTextW(state->previewTransformButton, (transformMode == L"obj-z-up") ? L"坐标系: OBJ Z-Up" : L"坐标系: 默认");
    }
    const PreviewCenterInput center = GuiResolvePreviewCenterInput(state);
    if (state->previewPlaceXLabel) {
        const std::wstring text = L"落点 X: " + (center.x ? std::to_wstring(*center.x) : std::wstring(L"自动(0)"));
        SetWindowTextW(state->previewPlaceXLabel, text.c_str());
    }
    if (state->previewPlaceZLabel) {
        const std::wstring text = L"落点 Z: " + (center.z ? std::to_wstring(*center.z) : std::wstring(L"自动(0)"));
        SetWindowTextW(state->previewPlaceZLabel, text.c_str());
    }
    if (state->previewPlaceYLabel) {
        const std::wstring yText = center.y ? std::to_wstring(*center.y) : (L"自动(" + std::to_wstring(baseY) + L"+高度/2)");
        SetWindowTextW(state->previewPlaceYLabel, (L"中心 Y: " + yText).c_str());
    }
}

void GuiApplyPreviewTransformState(GuiState* state) {
    if (!state) return;
    state->config = GuiReadConfig(state);
    GuiSyncPreviewTransformInspector(state);
    GuiUpdatePreviewChrome(state);
    GuiRenderPreviewNow(state);
    GuiScheduleWorldPreviewRefresh(state);
}

void GuiSetPreviewToolMode(GuiState* state, PreviewToolMode mode) {
    if (!state) return;
    state->previewToolMode = mode;
    state->orbiting = false;
    state->panning = false;
    state->previewDraggingPlacement = false;
    state->previewDraggingHeight = false;
    state->previewPlacementDragMode = PreviewPlacementDragMode::None;
    state->previewHoverDragMode = PreviewPlacementDragMode::None;
    GuiUpdatePreviewChrome(state);
}

int PreviewSnapValue(int value, int step) {
    if (step <= 1) return value;
    return static_cast<int>(std::lround(static_cast<double>(value) / static_cast<double>(step))) * step;
}

int PreviewNextSnapStep(int current) {
    if (current <= 1) return 4;
    if (current <= 4) return 16;
    return 1;
}

void GuiNudgePreviewCenter(GuiState* state, int dx, int dy, int dz) {
    if (!state) return;
    PreviewCenterInput center = GuiResolvePreviewCenterInput(state);
    const float scale = std::max(0.001f, GuiReadMappedFloat(state, L"scaleBlocksPerMeter", 4.0f));
    const float modelHeight = state->previewLoaded ? ((state->previewMesh.max[1] - state->previewMesh.min[1]) * scale) : 0.0f;
    const int baseY = GuiReadMappedInt(state, L"baseY", 0);
    center.x = center.x.value_or(0) + dx;
    center.z = center.z.value_or(0) + dz;
    if (dy != 0) {
        center.y = center.y.value_or(static_cast<int>(std::lround(baseY + modelHeight * 0.5f))) + dy;
    }
    GuiWritePreviewCenterInput(state, center);
    GuiApplyPreviewTransformState(state);
}

void GuiPrimePreviewPlacementDrag(GuiState* state, PreviewPlacementDragMode mode) {
    if (!state) return;
    const PreviewCenterInput center = GuiResolvePreviewCenterInput(state);
    const float scale = std::max(0.001f, GuiReadMappedFloat(state, L"scaleBlocksPerMeter", 4.0f));
    const float modelHeight = state->previewLoaded ? ((state->previewMesh.max[1] - state->previewMesh.min[1]) * scale) : 0.0f;
    const int baseY = GuiReadMappedInt(state, L"baseY", 0);
    state->previewDragStartCenterX = center.x.value_or(0);
    state->previewDragStartCenterZ = center.z.value_or(0);
    state->previewDragStartCenterY = center.y.value_or(static_cast<int>(std::lround(baseY + modelHeight * 0.5f)));
    state->previewPlacementDragMode = mode;
    state->previewDraggingPlacement = (mode == PreviewPlacementDragMode::Plane || mode == PreviewPlacementDragMode::AxisX || mode == PreviewPlacementDragMode::AxisZ);
    state->previewDraggingHeight = (mode == PreviewPlacementDragMode::Height || mode == PreviewPlacementDragMode::AxisY);
}

void GuiApplyPreviewPlacementDrag(GuiState* state, const POINT& pt) {
    if (!state) return;
    const int dx = pt.x - state->previewDragAnchor.x;
    const int dy = pt.y - state->previewDragAnchor.y;
    PreviewCenterInput center = GuiResolvePreviewCenterInput(state);
    if (state->previewPlacementDragMode == PreviewPlacementDragMode::Plane) {
        const float unitsPerPixel = std::max(0.02f, state->previewDistance / 850.0f);
        const float yawRad = state->previewYaw * 3.1415926535f / 180.0f;
        const float rightX = std::cos(yawRad);
        const float rightZ = std::sin(yawRad);
        const float forwardX = -std::sin(yawRad);
        const float forwardZ = std::cos(yawRad);
        const float worldX = static_cast<float>(state->previewDragStartCenterX) + (dx * rightX - dy * forwardX) * unitsPerPixel;
        const float worldZ = static_cast<float>(state->previewDragStartCenterZ) + (dx * rightZ - dy * forwardZ) * unitsPerPixel;
        center.x = PreviewSnapValue(static_cast<int>(std::lround(worldX)), state->previewSnapStep);
        center.z = PreviewSnapValue(static_cast<int>(std::lround(worldZ)), state->previewSnapStep);
    }
    if (state->previewPlacementDragMode == PreviewPlacementDragMode::Height) {
        const float unitsPerPixelY = std::max(0.05f, state->previewDistance / 220.0f);
        center.y = PreviewSnapValue(static_cast<int>(std::lround(static_cast<float>(state->previewDragStartCenterY) - dy * unitsPerPixelY)), state->previewSnapStep);
    }
    if (state->previewPlacementDragMode == PreviewPlacementDragMode::AxisX ||
        state->previewPlacementDragMode == PreviewPlacementDragMode::AxisY ||
        state->previewPlacementDragMode == PreviewPlacementDragMode::AxisZ) {
        RECT rc{};
        GetClientRect(state->preview, &rc);
        PreviewProjectionSetup setup;
        if (GuiBuildPreviewProjectionSetup(state, std::max(1L, rc.right - rc.left), std::max(1L, rc.bottom - rc.top), &setup)) {
            const auto centerPt = GuiProjectWorldPoint(setup, setup.placement.previewCenterX, setup.placement.previewCenterY, setup.placement.previewCenterZ);
            const float gizmoLength = std::clamp(std::max({ setup.placement.sizeX, setup.placement.sizeY, setup.placement.sizeZ, 16.0f }) * 0.2f, 8.0f, 20.0f);
            PreviewProjectedPoint endPt;
            int startValue = 0;
            if (state->previewPlacementDragMode == PreviewPlacementDragMode::AxisX) {
                endPt = GuiProjectWorldPoint(setup, setup.placement.previewCenterX + gizmoLength, setup.placement.previewCenterY, setup.placement.previewCenterZ);
                startValue = state->previewDragStartCenterX;
            } else if (state->previewPlacementDragMode == PreviewPlacementDragMode::AxisY) {
                endPt = GuiProjectWorldPoint(setup, setup.placement.previewCenterX, setup.placement.previewCenterY + gizmoLength, setup.placement.previewCenterZ);
                startValue = state->previewDragStartCenterY;
            } else {
                endPt = GuiProjectWorldPoint(setup, setup.placement.previewCenterX, setup.placement.previewCenterY, setup.placement.previewCenterZ + gizmoLength);
                startValue = state->previewDragStartCenterZ;
            }
            const float vx = endPt.x - centerPt.x;
            const float vy = endPt.y - centerPt.y;
            const float screenLen = std::max(1.0f, std::sqrt(vx * vx + vy * vy));
            const float alongPixels = (dx * vx + dy * vy) / screenLen;
            const float worldDelta = alongPixels * (gizmoLength / screenLen);
            const int snapped = PreviewSnapValue(static_cast<int>(std::lround(static_cast<float>(startValue) + worldDelta)), state->previewSnapStep);
            if (state->previewPlacementDragMode == PreviewPlacementDragMode::AxisX) center.x = snapped;
            else if (state->previewPlacementDragMode == PreviewPlacementDragMode::AxisY) center.y = snapped;
            else center.z = snapped;
        }
    }
    GuiWritePreviewCenterInput(state, center);
}

PreviewCenterInput GuiResolvePreviewCenterInput(const GuiState* state) {
    PreviewCenterInput result;
    const std::wstring centerText = GuiReadMappedText(state, L"center", L"");
    if (!centerText.empty()) {
        std::vector<int> values;
        std::wstringstream stream(centerText);
        std::wstring part;
        while (std::getline(stream, part, L',')) {
            part = Trim(part);
            if (part.empty()) continue;
            try {
                values.push_back(std::stoi(part));
            } catch (...) {
            }
        }
        if (values.size() >= 2) {
            result.x = values[0];
            result.z = values[1];
        }
        if (values.size() >= 3) {
            result.y = values[1];
            result.z = values[2];
        }
    }

    auto overrideOne = [&](const std::wstring& key, std::optional<int>* out) {
        const std::wstring text = GuiReadMappedText(state, key, L"");
        if (text.empty()) return;
        try {
            *out = std::stoi(text);
        } catch (...) {
        }
    };
    overrideOne(L"centerX", &result.x);
    overrideOne(L"centerY", &result.y);
    overrideOne(L"centerZ", &result.z);
    return result;
}

void GuiWritePreviewCenterInput(GuiState* state, const PreviewCenterInput& center) {
    if (!state) return;
    GuiWriteMappedText(state, L"center", L"");
    GuiWriteMappedText(state, L"centerX", center.x ? std::to_wstring(*center.x) : std::wstring());
    GuiWriteMappedText(state, L"centerY", center.y ? std::to_wstring(*center.y) : std::wstring());
    GuiWriteMappedText(state, L"centerZ", center.z ? std::to_wstring(*center.z) : std::wstring());
}

PreviewPlacement GuiResolvePreviewPlacement(const GuiState* state, float sizeX, float sizeY, float sizeZ) {
    PreviewPlacement placement;
    placement.sizeX = sizeX;
    placement.sizeY = sizeY;
    placement.sizeZ = sizeZ;
    const PreviewCenterInput center = GuiResolvePreviewCenterInput(state);
    const float baseY = GuiReadMappedFloat(state, L"baseY", 0.0f);
    placement.hasCenterX = center.x.has_value();
    placement.hasCenterY = center.y.has_value();
    placement.hasCenterZ = center.z.has_value();
    placement.worldCenterX = center.x ? static_cast<float>(*center.x) : 0.0f;
    placement.worldCenterY = center.y ? static_cast<float>(*center.y) : (baseY + sizeY * 0.5f);
    placement.worldCenterZ = center.z ? static_cast<float>(*center.z) : 0.0f;

    auto clampPreview = [](float value, float limit, bool* clamped) {
        const float bounded = std::clamp(value, -limit, limit);
        if (clamped) *clamped = bounded != value;
        return bounded;
    };
    placement.previewCenterX = clampPreview(placement.worldCenterX, 96.0f, &placement.clampedX);
    placement.previewCenterY = clampPreview(placement.worldCenterY, 96.0f, &placement.clampedY);
    placement.previewCenterZ = clampPreview(placement.worldCenterZ, 96.0f, &placement.clampedZ);
    return placement;
}

PreviewEditState GuiCapturePreviewEditState(GuiState* state) {
    PreviewEditState result;
    if (!state) return result;
    result.rotateYDeg = GuiReadMappedText(state, L"rotateYDeg", L"0");
    result.rotationMode = GuiReadMappedText(state, L"rotationMode", L"euler");
    result.rotationW = GuiReadMappedText(state, L"rotationW", L"1");
    result.rotationX = GuiReadMappedText(state, L"rotationX", L"0");
    result.rotationY = GuiReadMappedText(state, L"rotationY", result.rotateYDeg);
    result.rotationZ = GuiReadMappedText(state, L"rotationZ", L"0");
    result.levelNormal = GuiReadMappedText(state, L"levelNormal", L"");
    result.scaleBlocksPerMeter = GuiReadMappedText(state, L"scaleBlocksPerMeter", L"4");
    result.baseY = GuiReadMappedText(state, L"baseY", L"0");
    result.transform = GuiReadMappedText(state, L"transform", L"");
    result.center = GuiReadMappedText(state, L"center", L"");
    result.centerX = GuiReadMappedText(state, L"centerX", L"");
    result.centerY = GuiReadMappedText(state, L"centerY", L"");
    result.centerZ = GuiReadMappedText(state, L"centerZ", L"");
    result.flipX = state->checks.count(L"flipX") ? GuiGetCheck(state->checks[L"flipX"]) : false;
    result.flipZ = state->checks.count(L"flipZ") ? GuiGetCheck(state->checks[L"flipZ"]) : false;
    return result;
}

bool operator==(const PreviewEditState& a, const PreviewEditState& b) {
    return a.rotateYDeg == b.rotateYDeg &&
        a.rotationMode == b.rotationMode &&
        a.rotationW == b.rotationW &&
        a.rotationX == b.rotationX &&
        a.rotationY == b.rotationY &&
        a.rotationZ == b.rotationZ &&
        a.levelNormal == b.levelNormal &&
        a.scaleBlocksPerMeter == b.scaleBlocksPerMeter &&
        a.baseY == b.baseY &&
        a.transform == b.transform &&
        a.center == b.center &&
        a.centerX == b.centerX &&
        a.centerY == b.centerY &&
        a.centerZ == b.centerZ &&
        a.flipX == b.flipX &&
        a.flipZ == b.flipZ;
}

void GuiApplyPreviewEditState(GuiState* state, const PreviewEditState& editState) {
    if (!state) return;
    GuiWriteMappedText(state, L"rotateYDeg", editState.rotateYDeg);
    GuiWriteMappedText(state, L"rotationMode", editState.rotationMode);
    GuiWriteMappedText(state, L"rotationW", editState.rotationW);
    GuiWriteMappedText(state, L"rotationX", editState.rotationX);
    GuiWriteMappedText(state, L"rotationY", editState.rotationY);
    GuiWriteMappedText(state, L"rotationZ", editState.rotationZ);
    GuiWriteMappedText(state, L"levelNormal", editState.levelNormal);
    GuiWriteMappedText(state, L"scaleBlocksPerMeter", editState.scaleBlocksPerMeter);
    GuiWriteMappedText(state, L"baseY", editState.baseY);
    GuiWriteMappedText(state, L"transform", editState.transform);
    GuiWriteMappedText(state, L"center", editState.center);
    GuiWriteMappedText(state, L"centerX", editState.centerX);
    GuiWriteMappedText(state, L"centerY", editState.centerY);
    GuiWriteMappedText(state, L"centerZ", editState.centerZ);
    GuiSetMappedCheckValue(state, L"flipX", editState.flipX);
    GuiSetMappedCheckValue(state, L"flipZ", editState.flipZ);
    GuiApplyPreviewTransformState(state);
}

void GuiResetPreviewHistory(GuiState* state) {
    if (!state) return;
    state->previewHistory.clear();
    state->previewHistory.push_back(GuiCapturePreviewEditState(state));
    state->previewHistoryIndex = 0;
    GuiUpdatePreviewChrome(state);
}

void GuiRecordPreviewHistory(GuiState* state) {
    if (!state) return;
    const PreviewEditState current = GuiCapturePreviewEditState(state);
    if (!state->previewHistory.empty() && state->previewHistoryIndex < state->previewHistory.size()
        && state->previewHistory[state->previewHistoryIndex] == current) {
        return;
    }
    if (!state->previewHistory.empty() && state->previewHistoryIndex + 1 < state->previewHistory.size()) {
        state->previewHistory.erase(state->previewHistory.begin() + static_cast<std::ptrdiff_t>(state->previewHistoryIndex + 1), state->previewHistory.end());
    }
    state->previewHistory.push_back(current);
    state->previewHistoryIndex = state->previewHistory.size() - 1;
    GuiUpdatePreviewChrome(state);
}

bool GuiPreviewCanUndo(const GuiState* state) {
    return state && !state->previewHistory.empty() && state->previewHistoryIndex > 0;
}

bool GuiPreviewCanRedo(const GuiState* state) {
    return state && !state->previewHistory.empty() && (state->previewHistoryIndex + 1) < state->previewHistory.size();
}

void GuiPreviewUndo(GuiState* state) {
    if (!GuiPreviewCanUndo(state)) return;
    state->previewHistoryIndex -= 1;
    GuiApplyPreviewEditState(state, state->previewHistory[state->previewHistoryIndex]);
}

void GuiPreviewRedo(GuiState* state) {
    if (!GuiPreviewCanRedo(state)) return;
    state->previewHistoryIndex += 1;
    GuiApplyPreviewEditState(state, state->previewHistory[state->previewHistoryIndex]);
}

std::wstring GuiPreviewPresetSummary(const PreviewEditState& editState) {
    auto readOr = [&](const std::wstring& value, const wchar_t* fallback) {
        return value.empty() ? std::wstring(fallback) : value;
    };
    const std::wstring x = readOr(editState.centerX, L"auto");
    const std::wstring y = readOr(editState.centerY, L"auto");
    const std::wstring z = readOr(editState.centerZ, L"auto");
    const std::wstring rotate = readOr(editState.rotateYDeg, L"0");
    const std::wstring scale = readOr(editState.scaleBlocksPerMeter, L"4");
    return L"(" + x + L"," + y + L"," + z + L")  r" + rotate + L"  s" + scale;
}

std::wstring EncodePreviewEditState(const PreviewEditState& editState) {
    auto encodeBool = [](bool value) { return value ? L"1" : L"0"; };
    std::vector<std::wstring> parts = {
        editState.rotateYDeg,
        editState.scaleBlocksPerMeter,
        editState.baseY,
        editState.transform,
        editState.center,
        editState.centerX,
        editState.centerY,
        editState.centerZ,
        encodeBool(editState.flipX),
        encodeBool(editState.flipZ),
        editState.rotationMode,
        editState.rotationW,
        editState.rotationX,
        editState.rotationY,
        editState.rotationZ,
        editState.levelNormal
    };
    std::wstring out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out += L"|";
        out += parts[i];
    }
    return out;
}

bool DecodePreviewEditState(const std::wstring& text, PreviewEditState* outState) {
    if (!outState) return false;
    if (text.empty()) return false;
    std::vector<std::wstring> parts;
    std::wstringstream stream(text);
    std::wstring part;
    while (std::getline(stream, part, L'|')) {
        parts.push_back(part);
    }
    if (parts.size() < 10) return false;
    outState->rotateYDeg = parts[0];
    outState->scaleBlocksPerMeter = parts[1];
    outState->baseY = parts[2];
    outState->transform = parts[3];
    outState->center = parts[4];
    outState->centerX = parts[5];
    outState->centerY = parts[6];
    outState->centerZ = parts[7];
    outState->flipX = (parts[8] == L"1");
    outState->flipZ = (parts[9] == L"1");
    if (parts.size() >= 16) {
        outState->rotationMode = parts[10];
        outState->rotationW = parts[11];
        outState->rotationX = parts[12];
        outState->rotationY = parts[13];
        outState->rotationZ = parts[14];
        outState->levelNormal = parts[15];
    } else {
        outState->rotationMode = L"euler";
        outState->rotationW = L"1";
        outState->rotationX = L"0";
        outState->rotationY = outState->rotateYDeg;
        outState->rotationZ = L"0";
    }
    return true;
}

void GuiLoadPreviewPresetsFromConfig(GuiState* state, const Config& config) {
    if (!state) return;
    state->previewPresets = {};
    const std::array<std::wstring, 3> values = { config.previewPresetA, config.previewPresetB, config.previewPresetC };
    for (int i = 0; i < 3; ++i) {
        PreviewEditState decoded;
        if (DecodePreviewEditState(values[i], &decoded)) {
            state->previewPresets[i] = decoded;
        }
    }
}

void GuiSyncPreviewPresetsToConfig(GuiState* state) {
    if (!state) return;
    auto encodeSlot = [&](int index) -> std::wstring {
        return state->previewPresets[index].has_value() ? EncodePreviewEditState(*state->previewPresets[index]) : L"";
    };
    state->config.previewPresetA = encodeSlot(0);
    state->config.previewPresetB = encodeSlot(1);
    state->config.previewPresetC = encodeSlot(2);
}

void GuiSavePreviewPreset(GuiState* state, int slot) {
    if (!state || slot < 0 || slot >= static_cast<int>(state->previewPresets.size())) return;
    state->config = GuiReadConfig(state);
    state->previewPresets[slot] = GuiCapturePreviewEditState(state);
    GuiSyncPreviewPresetsToConfig(state);
    SaveConfig(state->configPath, state->config);
    GuiUpdatePreviewChrome(state);
}

void GuiLoadPreviewPreset(GuiState* state, int slot) {
    if (!state || slot < 0 || slot >= static_cast<int>(state->previewPresets.size()) || !state->previewPresets[slot].has_value()) return;
    GuiRecordPreviewHistory(state);
    GuiApplyPreviewEditState(state, *state->previewPresets[slot]);
    GuiRecordPreviewHistory(state);
}

std::wstring PreviewDrawModeLabel(PreviewDrawMode mode) {
    switch (mode) {
    case PreviewDrawMode::Solid: return L"实体";
    case PreviewDrawMode::Wireframe: return L"线框";
    case PreviewDrawMode::SolidWire: return L"混合";
    }
    return L"混合";
}

std::wstring PreviewToolModeLabel(PreviewToolMode mode) {
    return mode == PreviewToolMode::Place ? L"摆放模式" : L"相机模式";
}

std::wstring PreviewSnapLabel(int step) {
    return L"吸附: " + std::to_wstring(step);
}

std::wstring PreviewWorldRadiusLabel(int radius) {
    const int chunks = std::max(1, static_cast<int>(std::ceil(radius / 16.0f)));
    return L"实景半径: " + std::to_wstring(chunks) + L" 区块";
}

std::wstring PreviewWorldStyleLabel(PreviewWorldStyle style) {
    switch (style) {
    case PreviewWorldStyle::Points: return L"实景样式: 点";
    case PreviewWorldStyle::Faces: return L"实景样式: 面";
    case PreviewWorldStyle::Voxels: return L"实景样式: 体素";
    }
    return L"实景样式: 面";
}

int PreviewNextWorldRadius(int current) {
    constexpr int values[] = { 32, 48, 64, 96, 128 };
    for (int i = 0; i < static_cast<int>(std::size(values)); ++i) {
        if (values[i] == current) return values[(i + 1) % static_cast<int>(std::size(values))];
    }
    return values[0];
}

std::wstring PreviewDragModeLabel(PreviewPlacementDragMode mode) {
    switch (mode) {
    case PreviewPlacementDragMode::AxisX: return L"X 轴";
    case PreviewPlacementDragMode::AxisY: return L"Y 轴";
    case PreviewPlacementDragMode::AxisZ: return L"Z 轴";
    case PreviewPlacementDragMode::Plane: return L"平面中心";
    case PreviewPlacementDragMode::Height: return L"高度";
    default: return L"";
    }
}

void GuiSetPreviewPreset(GuiState* state, float yaw, float pitch) {
    if (!state) return;
    state->previewYaw = yaw;
    state->previewPitch = std::clamp(pitch, -89.0f, 89.0f);
    state->previewPanX = 0.0f;
    state->previewPanY = 0.0f;
    InvalidateRect(state->preview, nullptr, FALSE);
    GuiUpdatePreviewChrome(state);
}

void GuiCyclePreviewDrawMode(GuiState* state) {
    if (!state) return;
    switch (state->previewDrawMode) {
    case PreviewDrawMode::Solid: state->previewDrawMode = PreviewDrawMode::Wireframe; break;
    case PreviewDrawMode::Wireframe: state->previewDrawMode = PreviewDrawMode::SolidWire; break;
    case PreviewDrawMode::SolidWire: state->previewDrawMode = PreviewDrawMode::Solid; break;
    }
    InvalidateRect(state->preview, nullptr, FALSE);
    GuiUpdatePreviewChrome(state);
}

void GuiUpdatePreviewChrome(GuiState* state) {
    if (!state) return;
    state->previewChromeUpdatedAt = GetTickCount64();
    auto setIf = [&](int id, const std::wstring& text) {
        HWND h = GetDlgItem(state->hwnd, id);
        if (!h && state->previewInspector) h = GetDlgItem(state->previewInspector, id);
        if (h) SetWindowTextW(h, text.c_str());
    };
    setIf(IDC_GUI_PREVIEW_TOOL_CAMERA, state->previewToolMode == PreviewToolMode::Camera ? L"相机模式*" : L"相机模式");
    setIf(IDC_GUI_PREVIEW_TOOL_PLACE, state->previewToolMode == PreviewToolMode::Place ? L"摆放模式*" : L"摆放模式");
    setIf(IDC_GUI_PREVIEW_SNAP, PreviewSnapLabel(state->previewSnapStep));
    setIf(IDC_GUI_PREVIEW_MODE, L"显示: " + PreviewDrawModeLabel(state->previewDrawMode));
    const std::wstring lodLabel = state->previewForcedLod < 0 ? L"LOD: 自动" :
        (state->previewForcedLod == 0
            ? (state->previewAllModels ? L"LOD: 总览最高" : L"LOD: 完整")
            : L"LOD: " + std::to_wstring(state->previewForcedLod));
    setIf(IDC_GUI_PREVIEW_LOD, lodLabel);
    setIf(IDC_GUI_PREVIEW_WORLD, state->previewWorldVisible ? L"实景: 开" : L"实景: 关");
    setIf(IDC_GUI_PREVIEW_WORLD_REFRESH, state->previewWorldLoading ? L"实景加载中..." : L"刷新实景");
    setIf(IDC_GUI_PREVIEW_WORLD_RADIUS, PreviewWorldRadiusLabel(state->previewWorldRadius));
    setIf(IDC_GUI_PREVIEW_WORLD_STYLE, PreviewWorldStyleLabel(state->previewWorldStyle));
    if (HWND undo = GetDlgItem(state->hwnd, IDC_GUI_PREVIEW_UNDO)) EnableWindow(undo, GuiPreviewCanUndo(state));
    if (HWND redo = GetDlgItem(state->hwnd, IDC_GUI_PREVIEW_REDO)) EnableWindow(redo, GuiPreviewCanRedo(state));
    setIf(IDC_GUI_PLACE_X_LEFT, L"-" + std::to_wstring(state->previewSnapStep));
    setIf(IDC_GUI_PLACE_X_RIGHT, L"+" + std::to_wstring(state->previewSnapStep));
    setIf(IDC_GUI_PLACE_Z_LEFT, L"-" + std::to_wstring(state->previewSnapStep));
    setIf(IDC_GUI_PLACE_Z_RIGHT, L"+" + std::to_wstring(state->previewSnapStep));
    setIf(IDC_GUI_PLACE_Y_DOWN, L"-" + std::to_wstring(state->previewSnapStep));
    setIf(IDC_GUI_PLACE_Y_UP, L"+" + std::to_wstring(state->previewSnapStep));
    for (int i = 0; i < 3; ++i) {
        if (state->previewPresetLabels[i]) {
            const std::wstring label = state->previewPresets[i].has_value()
                ? (L"方案 " + std::wstring(1, static_cast<wchar_t>(L'A' + i)) + L": " + GuiPreviewPresetSummary(*state->previewPresets[i]))
                : (L"方案 " + std::wstring(1, static_cast<wchar_t>(L'A' + i)) + L": 未保存");
            SetWindowTextW(state->previewPresetLabels[i], label.c_str());
        }
        if (HWND load = GetDlgItem(state->previewInspector ? state->previewInspector : state->hwnd, IDC_GUI_PRESET_A_LOAD + i * 3)) {
            EnableWindow(load, state->previewPresets[i].has_value());
        }
    }

    if (state->previewInfoLabel) {
        std::wstring infoText;
        if (state->previewLoaded) {
            const auto& mesh = state->previewMesh;
            const Config current = GuiReadConfig(state);
            const auto worldSize = GuiComputePreviewWorldSize(state, mesh);
            const int baseY = ParseOptionalIntText(current.baseY).value_or(0);
            const PreviewPlacement placement = GuiResolvePreviewPlacement(state, worldSize[0], worldSize[1], worldSize[2]);
            std::wostringstream ss;
            ss << L"模型: " << GuiPreviewModelLabel(state)
               << L"  顶点 " << FormatNumber(mesh.positions.size() / 3)
               << L"  三角形 " << FormatNumber(mesh.indices.size() / 3)
               << L"  LOD[" << FormatNumber(mesh.indices.size() / 3);
            for (std::size_t level = 1; level < mesh.lodIndices.size(); ++level) {
                ss << L"/" << FormatNumber(mesh.lodIndices[level].size() / 3);
            }
            ss << L"]"
               << L"  尺寸 " << static_cast<int>(std::ceil(worldSize[0])) << L" x "
               << static_cast<int>(std::ceil(worldSize[1])) << L" x " << static_cast<int>(std::ceil(worldSize[2]))
               << L"  旋转 " << ConfigRotationSummary(current)
               << L"  baseY " << baseY
               << L"  中心(" << FormatFloatCompact(placement.worldCenterX, 0) << L","
               << FormatFloatCompact(placement.worldCenterY, 0) << L","
               << FormatFloatCompact(placement.worldCenterZ, 0) << L")";
            infoText = ss.str();
        } else if (state->previewLoading) {
            infoText = L"预览正在加载...";
        } else {
            infoText = L"未加载模型预览";
        }
        SetWindowTextW(state->previewInfoLabel, infoText.c_str());
    }

    if (state->previewHintLabel) {
        std::wostringstream hint;
        hint << PreviewToolModeLabel(state->previewToolMode) << L"  ";
        if (state->previewToolMode == PreviewToolMode::Place) {
            hint << L"左键拖拽摆放  右键拖拽高度  方向键微调  PgUp/PgDn 高度";
        } else {
            hint << L"左键旋转  右键平移  滚轮缩放";
        }
        hint << L"  " << PreviewSnapLabel(state->previewSnapStep);
        if (state->previewHoverDragMode != PreviewPlacementDragMode::None) {
            hint << L"  命中: " << PreviewDragModeLabel(state->previewHoverDragMode);
        }
        if (state->previewWorldVisible) {
            hint << L"  存档实景: " << FormatNumber(state->previewWorldSlice.blocks.size());
            if (!state->previewWorldSourceName.empty()) {
                hint << L" [" << state->previewWorldSourceName << L"]";
            }
            if (!state->previewWorldSlice.blocks.empty()) {
                if (state->previewWorldGpuBuilding) hint << L" 实景GPU准备中";
                else hint << (state->previewWorldGpuReady ? L" 实景GPU" : L" 实景CPU");
                if (state->previewWorldGpuReady && state->previewWorldGpuDensityReduced) hint << L"(显存抽样)";
            }
        }
        hint << L"  视角 yaw=" << static_cast<int>(state->previewYaw)
             << L"° pitch=" << static_cast<int>(state->previewPitch)
             << L"°  距离 " << static_cast<int>(state->previewDistance * 10.0f) / 10.0f;
        const auto& mesh = state->previewMesh;
        if (state->previewLoaded && !mesh.indices.empty()) {
            hint << L"  LOD " << state->previewActiveLod
                 << L" (绘制 " << FormatNumber(state->previewRenderedTriangles)
                 << (state->previewAllModels ? L" / 总览最高 " : L" / 完整 ")
                  << FormatNumber(mesh.indices.size() / 3) << L")"
                  << L"  帧 " << FormatFloatCompact(static_cast<float>(state->previewFrameMs), 1) << L" ms"
                  << (state->previewPresentFps > 0.0
                      ? L"  FPS " + FormatFloatCompact(static_cast<float>(state->previewPresentFps), 1)
                      : L"  FPS 采样中")
                  << (state->previewGpuReady ? L"  GPU缓冲" : L"  CPU回退");
            const auto worldSize = GuiComputePreviewWorldSize(state, mesh);
            const PreviewPlacement placement = GuiResolvePreviewPlacement(
                state, worldSize[0], worldSize[1], worldSize[2]);
            if (placement.clampedX || placement.clampedY || placement.clampedZ) {
                hint << L"  预览已局部钳制世界坐标";
            }
        }
        SetWindowTextW(state->previewHintLabel, hint.str().c_str());
    }
    if (state->previewRendererLabel) {
        std::wstring rendererInfo = L"当前渲染 API: ";
        rendererInfo += state->previewRendererStatus.empty() ? L"初始化中" : state->previewRendererStatus;
        const int refreshRate = NormalizePreviewRefreshRate(
            state->previewRefreshHz.load(std::memory_order_relaxed));
        rendererInfo += refreshRate == 0
            ? L"  |  全局刷新上限: 无上限"
            : L"  |  全局刷新上限: " + std::to_wstring(refreshRate) + L" Hz";
        SetWindowTextW(state->previewRendererLabel, rendererInfo.c_str());
    }
}

void GuiUpdatePreviewChromeThrottled(GuiState* state, ULONGLONG intervalMilliseconds = 100) {
    if (!state) return;
    const ULONGLONG now = GetTickCount64();
    if (now - state->previewChromeUpdatedAt >= intervalMilliseconds) {
        GuiUpdatePreviewChrome(state);
    }
}

PreviewProjectedPoint GuiProjectPreviewPoint(
    float x, float y, float z,
    float modelYawDeg, float modelScale, bool modelFlipX, bool modelFlipZ, bool objZUp,
    float camYawDeg, float camPitchDeg, float panX, float panY, float distance,
    int width, int height) {
    if (objZUp) {
        const float newY = z;
        const float newZ = -y;
        y = newY;
        z = newZ;
    }
    x *= modelFlipX ? -modelScale : modelScale;
    y *= modelScale;
    z *= modelFlipZ ? -modelScale : modelScale;

    const float modelYaw = modelYawDeg * 3.1415926535f / 180.0f;
    const float modelCos = std::cos(modelYaw);
    const float modelSin = std::sin(modelYaw);
    const float rx = x * modelCos + z * modelSin;
    const float rz = -x * modelSin + z * modelCos;
    x = rx;
    z = rz;

    const float camYaw = camYawDeg * 3.1415926535f / 180.0f;
    const float camCos = std::cos(camYaw);
    const float camSin = std::sin(camYaw);
    const float vx = x * camCos + z * camSin;
    const float vz = -x * camSin + z * camCos;
    x = vx;
    z = vz;

    const float camPitch = camPitchDeg * 3.1415926535f / 180.0f;
    const float pitchCos = std::cos(camPitch);
    const float pitchSin = std::sin(camPitch);
    const float vy = y * pitchCos - z * pitchSin;
    const float vz2 = y * pitchSin + z * pitchCos;
    y = vy + panY;
    z = vz2 - distance;
    x += panX;

    PreviewProjectedPoint out;
    if (z > -0.1f) {
        out.visible = false;
        return out;
    }
    const float focal = (static_cast<float>(height) * 0.5f) / std::tan(50.0f * 3.1415926535f / 360.0f);
    out.x = static_cast<float>(width) * 0.5f + (x / -z) * focal;
    out.y = static_cast<float>(height) * 0.5f - (y / -z) * focal;
    out.visible = true;
    return out;
}

PreviewProjectedPoint GuiProjectCameraPoint(
    float x, float y, float z,
    float camYawDeg, float camPitchDeg, float panX, float panY, float distance,
    int width, int height) {
    const float camYaw = camYawDeg * 3.1415926535f / 180.0f;
    const float camCos = std::cos(camYaw);
    const float camSin = std::sin(camYaw);
    const float vx = x * camCos + z * camSin;
    const float vz = -x * camSin + z * camCos;
    x = vx;
    z = vz;

    const float camPitch = camPitchDeg * 3.1415926535f / 180.0f;
    const float pitchCos = std::cos(camPitch);
    const float pitchSin = std::sin(camPitch);
    const float vy = y * pitchCos - z * pitchSin;
    const float vz2 = y * pitchSin + z * pitchCos;
    y = vy + panY;
    z = vz2 - distance;
    x += panX;

    PreviewProjectedPoint out;
    if (z > -0.1f) {
        out.visible = false;
        return out;
    }
    const float focal = (static_cast<float>(height) * 0.5f) / std::tan(50.0f * 3.1415926535f / 360.0f);
    out.x = static_cast<float>(width) * 0.5f + (x / -z) * focal;
    out.y = static_cast<float>(height) * 0.5f - (y / -z) * focal;
    out.visible = true;
    return out;
}

bool GuiBuildPreviewProjectionSetup(GuiState* state, int width, int height, PreviewProjectionSetup* out) {
    if (!state || !out || !state->previewLoaded || state->previewMesh.indices.empty()) return false;
    const Config current = GuiReadConfig(state);
    const auto& mesh = state->previewMesh;
    out->width = std::max(1, width);
    out->height = std::max(1, height);
    GuiPopulatePreviewTransformSetup(state, out);
    out->camYaw = state->previewYaw;
    out->camPitch = state->previewPitch;
    out->panX = state->previewPanX;
    out->panY = state->previewPanY;
    out->distance = state->previewDistance;
    const auto worldSize = GuiComputePreviewWorldSize(state, mesh);
    out->placement = GuiResolvePreviewPlacement(state, worldSize[0], worldSize[1], worldSize[2]);
    return true;
}

PreviewProjectedPoint GuiProjectWorldPoint(const PreviewProjectionSetup& setup, float x, float y, float z) {
    return GuiProjectCameraPoint(x, y, z, setup.camYaw, setup.camPitch, setup.panX, setup.panY, setup.distance, setup.width, setup.height);
}

PreviewProjectedPoint GuiProjectPlacedModelPoint(const PreviewProjectionSetup& setup, float x, float y, float z) {
    const auto transformed = GuiTransformPreviewModelPoint(setup, x, y, z);
    x = transformed[0] + setup.placement.previewCenterX;
    y = transformed[1] + setup.placement.previewCenterY;
    z = transformed[2] + setup.placement.previewCenterZ;

    return GuiProjectWorldPoint(setup, x, y, z);
}

float DistancePointToSegment(const POINT& point, const PreviewProjectedPoint& a, const PreviewProjectedPoint& b) {
    if (!a.visible || !b.visible) return 1e9f;
    const float px = static_cast<float>(point.x);
    const float py = static_cast<float>(point.y);
    const float vx = b.x - a.x;
    const float vy = b.y - a.y;
    const float length2 = vx * vx + vy * vy;
    if (length2 <= 0.0001f) {
        const float dx = px - a.x;
        const float dy = py - a.y;
        return std::sqrt(dx * dx + dy * dy);
    }
    const float t = std::clamp(((px - a.x) * vx + (py - a.y) * vy) / length2, 0.0f, 1.0f);
    const float projX = a.x + vx * t;
    const float projY = a.y + vy * t;
    const float dx = px - projX;
    const float dy = py - projY;
    return std::sqrt(dx * dx + dy * dy);
}

float PreviewPolygonArea(const POINT* points, int count) {
    if (!points || count < 3) return 0.0f;
    double area = 0.0;
    for (int i = 0; i < count; ++i) {
        const POINT& a = points[i];
        const POINT& b = points[(i + 1) % count];
        area += static_cast<double>(a.x) * b.y - static_cast<double>(b.x) * a.y;
    }
    return static_cast<float>(std::abs(area) * 0.5);
}

PreviewPlacementDragMode GuiHitTestPreviewGizmo(GuiState* state, HWND hwnd, const POINT& pt) {
    if (!state || !hwnd || !state->previewLoaded || state->previewMesh.indices.empty()) return PreviewPlacementDragMode::None;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    PreviewProjectionSetup setup;
    if (!GuiBuildPreviewProjectionSetup(state, std::max(1L, rc.right - rc.left), std::max(1L, rc.bottom - rc.top), &setup)) {
        return PreviewPlacementDragMode::None;
    }
    const auto center = GuiProjectWorldPoint(setup, setup.placement.previewCenterX, setup.placement.previewCenterY, setup.placement.previewCenterZ);
    if (!center.visible) return PreviewPlacementDragMode::None;
    const float gizmoLength = std::clamp(std::max({ setup.placement.sizeX, setup.placement.sizeY, setup.placement.sizeZ, 16.0f }) * 0.2f, 8.0f, 20.0f);
    const auto xEnd = GuiProjectWorldPoint(setup, setup.placement.previewCenterX + gizmoLength, setup.placement.previewCenterY, setup.placement.previewCenterZ);
    const auto yEnd = GuiProjectWorldPoint(setup, setup.placement.previewCenterX, setup.placement.previewCenterY + gizmoLength, setup.placement.previewCenterZ);
    const auto zEnd = GuiProjectWorldPoint(setup, setup.placement.previewCenterX, setup.placement.previewCenterY, setup.placement.previewCenterZ + gizmoLength);
    const float dx = static_cast<float>(pt.x) - center.x;
    const float dy = static_cast<float>(pt.y) - center.y;
    if (std::sqrt(dx * dx + dy * dy) <= 10.0f) {
        return PreviewPlacementDragMode::Plane;
    }
    const float xDist = DistancePointToSegment(pt, center, xEnd);
    const float yDist = DistancePointToSegment(pt, center, yEnd);
    const float zDist = DistancePointToSegment(pt, center, zEnd);
    const float threshold = 10.0f;
    float best = threshold;
    PreviewPlacementDragMode bestMode = PreviewPlacementDragMode::None;
    if (xDist <= best) { best = xDist; bestMode = PreviewPlacementDragMode::AxisX; }
    if (yDist <= best) { best = yDist; bestMode = PreviewPlacementDragMode::AxisY; }
    if (zDist <= best) { best = zDist; bestMode = PreviewPlacementDragMode::AxisZ; }
    return bestMode;
}

void GuiRenderPreviewFallback(GuiState* state, HWND hwnd, HDC hdc) {
    if (!state || !hdc) return;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int width = std::max(1L, rc.right - rc.left);
    const int height = std::max(1L, rc.bottom - rc.top);

    HBRUSH background = CreateSolidBrush(RGB(25, 28, 31));
    FillRect(hdc, &rc, background);
    DeleteObject(background);

    const Config current = GuiReadConfig(state);
    const auto& mesh = state->previewMesh;
    PreviewProjectionSetup setup;
    if (!GuiBuildPreviewProjectionSetup(state, width, height, &setup)) return;

    auto project = [&](float x, float y, float z) {
        return GuiProjectWorldPoint(setup, x, y, z);
    };
    auto projectPlaced = [&](float x, float y, float z) {
        return GuiProjectPlacedModelPoint(setup, x, y, z);
    };

    if (state->previewShowGrid) {
        const PreviewGridLayout grid = GuiComputePreviewGridLayout(state);
        HPEN gridPen = CreatePen(PS_SOLID, 1, RGB(52, 56, 61));
        HPEN oldPen = (HPEN)SelectObject(hdc, gridPen);
        for (int i = -grid.lineCount; i <= grid.lineCount; ++i) {
            const float line = i * grid.step;
            const auto a = project(line, 0.0f, -grid.extent);
            const auto b = project(line, 0.0f, grid.extent);
            const auto c = project(-grid.extent, 0.0f, line);
            const auto d = project(grid.extent, 0.0f, line);
            if (a.visible && b.visible) { MoveToEx(hdc, (int)a.x, (int)a.y, nullptr); LineTo(hdc, (int)b.x, (int)b.y); }
            if (c.visible && d.visible) { MoveToEx(hdc, (int)c.x, (int)c.y, nullptr); LineTo(hdc, (int)d.x, (int)d.y); }
        }
        SelectObject(hdc, oldPen);
        DeleteObject(gridPen);
    }

    if (state->previewShowAxes) {
        const auto origin = project(0.0f, 0.0f, 0.0f);
        const auto xAxis = project(4.0f, 0.0f, 0.0f);
        const auto yAxis = project(0.0f, 4.0f, 0.0f);
        const auto zAxis = project(0.0f, 0.0f, 4.0f);
        if (origin.visible) {
            HPEN pens[3] = {
                CreatePen(PS_SOLID, 2, RGB(255, 82, 82)),
                CreatePen(PS_SOLID, 2, RGB(118, 255, 3)),
                CreatePen(PS_SOLID, 2, RGB(64, 156, 255))
            };
            const PreviewProjectedPoint axes[3] = { xAxis, yAxis, zAxis };
            for (int i = 0; i < 3; ++i) {
                if (!axes[i].visible) continue;
                HPEN oldPen = (HPEN)SelectObject(hdc, pens[i]);
                MoveToEx(hdc, (int)origin.x, (int)origin.y, nullptr);
                LineTo(hdc, (int)axes[i].x, (int)axes[i].y);
                SelectObject(hdc, oldPen);
            }
            for (HPEN pen : pens) DeleteObject(pen);
        }
    }

    if (state->previewWorldVisible && !state->previewWorldSlice.blocks.empty()) {
        const float alignX = setup.placement.previewCenterX - static_cast<float>(state->previewWorldSlice.centerX);
        const float alignY = setup.placement.previewCenterY - setup.placement.worldCenterY;
        const float alignZ = setup.placement.previewCenterZ - static_cast<float>(state->previewWorldSlice.centerZ);

        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(DC_BRUSH));
        HGDIOBJ oldPen = SelectObject(hdc, GetStockObject(DC_PEN));
        auto darken = [](COLORREF color, int amount) -> COLORREF {
            return RGB(
                std::max(0, GetRValue(color) - amount),
                std::max(0, GetGValue(color) - amount),
                std::max(0, GetBValue(color) - amount));
        };
        const float yawRad = setup.camYaw * 3.1415926535f / 180.0f;
        const float xFaceDirection = std::cos(yawRad) >= 0.0f ? 1.0f : -1.0f;
        const float zFaceDirection = std::sin(yawRad) >= 0.0f ? 1.0f : -1.0f;
        for (const auto& block : state->previewWorldSlice.blocks) {
            COLORREF color = GuiFallbackBlockColor(block.blockName);
            auto it = state->previewWorldColors.find(block.blockName);
            if (it != state->previewWorldColors.end()) color = it->second;
            if (state->previewWorldStyle == PreviewWorldStyle::Faces || state->previewWorldStyle == PreviewWorldStyle::Voxels) {
                const float halfSpanX = static_cast<float>(std::max(1, block.sampleSizeX)) * 0.5f;
                const float halfSpanZ = static_cast<float>(std::max(1, block.sampleSizeZ)) * 0.5f;
                const float xFace = xFaceDirection * halfSpanX;
                const float zFace = zFaceDirection * halfSpanZ;
                const auto top0 = project(static_cast<float>(block.x) - halfSpanX + alignX, static_cast<float>(block.y) + 0.5f + alignY, static_cast<float>(block.z) - halfSpanZ + alignZ);
                const auto top1 = project(static_cast<float>(block.x) + halfSpanX + alignX, static_cast<float>(block.y) + 0.5f + alignY, static_cast<float>(block.z) - halfSpanZ + alignZ);
                const auto top2 = project(static_cast<float>(block.x) + halfSpanX + alignX, static_cast<float>(block.y) + 0.5f + alignY, static_cast<float>(block.z) + halfSpanZ + alignZ);
                const auto top3 = project(static_cast<float>(block.x) - halfSpanX + alignX, static_cast<float>(block.y) + 0.5f + alignY, static_cast<float>(block.z) + halfSpanZ + alignZ);
                if (top0.visible && top1.visible && top2.visible && top3.visible) {
                    POINT quad[4] = {
                        { static_cast<LONG>(top0.x), static_cast<LONG>(top0.y) },
                        { static_cast<LONG>(top1.x), static_cast<LONG>(top1.y) },
                        { static_cast<LONG>(top2.x), static_cast<LONG>(top2.y) },
                        { static_cast<LONG>(top3.x), static_cast<LONG>(top3.y) }
                    };
                    const float area = PreviewPolygonArea(quad, 4);
                    if (area >= 1.0f) {
                        SetDCBrushColor(hdc, color);
                        SetDCPenColor(hdc, darken(color, 18));
                        Polygon(hdc, quad, 4);
                        if (state->previewWorldStyle == PreviewWorldStyle::Voxels) {
                        const auto sx0 = project(static_cast<float>(block.x) + xFace + alignX, static_cast<float>(block.bottomY) - 0.5f + alignY, static_cast<float>(block.z) - halfSpanZ + alignZ);
                            const auto sx1 = project(static_cast<float>(block.x) + xFace + alignX, static_cast<float>(block.y) + 0.5f + alignY, static_cast<float>(block.z) - halfSpanZ + alignZ);
                            const auto sx2 = project(static_cast<float>(block.x) + xFace + alignX, static_cast<float>(block.y) + 0.5f + alignY, static_cast<float>(block.z) + halfSpanZ + alignZ);
                            const auto sx3 = project(static_cast<float>(block.x) + xFace + alignX, static_cast<float>(block.bottomY) - 0.5f + alignY, static_cast<float>(block.z) + halfSpanZ + alignZ);
                            if (sx0.visible && sx1.visible && sx2.visible && sx3.visible) {
                                POINT sideX[4] = {
                                    { static_cast<LONG>(sx0.x), static_cast<LONG>(sx0.y) },
                                    { static_cast<LONG>(sx1.x), static_cast<LONG>(sx1.y) },
                                    { static_cast<LONG>(sx2.x), static_cast<LONG>(sx2.y) },
                                    { static_cast<LONG>(sx3.x), static_cast<LONG>(sx3.y) }
                                };
                                if (PreviewPolygonArea(sideX, 4) >= 0.8f) {
                                    SetDCBrushColor(hdc, darken(color, 35));
                                    SetDCPenColor(hdc, darken(color, 52));
                                    Polygon(hdc, sideX, 4);
                                }
                            }
                            const auto sz0 = project(static_cast<float>(block.x) - halfSpanX + alignX, static_cast<float>(block.bottomY) - 0.5f + alignY, static_cast<float>(block.z) + zFace + alignZ);
                            const auto sz1 = project(static_cast<float>(block.x) + halfSpanX + alignX, static_cast<float>(block.bottomY) - 0.5f + alignY, static_cast<float>(block.z) + zFace + alignZ);
                            const auto sz2 = project(static_cast<float>(block.x) + halfSpanX + alignX, static_cast<float>(block.y) + 0.5f + alignY, static_cast<float>(block.z) + zFace + alignZ);
                            const auto sz3 = project(static_cast<float>(block.x) - halfSpanX + alignX, static_cast<float>(block.y) + 0.5f + alignY, static_cast<float>(block.z) + zFace + alignZ);
                            if (sz0.visible && sz1.visible && sz2.visible && sz3.visible) {
                                POINT sideZ[4] = {
                                    { static_cast<LONG>(sz0.x), static_cast<LONG>(sz0.y) },
                                    { static_cast<LONG>(sz1.x), static_cast<LONG>(sz1.y) },
                                    { static_cast<LONG>(sz2.x), static_cast<LONG>(sz2.y) },
                                    { static_cast<LONG>(sz3.x), static_cast<LONG>(sz3.y) }
                                };
                                if (PreviewPolygonArea(sideZ, 4) >= 0.8f) {
                                    SetDCBrushColor(hdc, darken(color, 24));
                                    SetDCPenColor(hdc, darken(color, 44));
                                    Polygon(hdc, sideZ, 4);
                                }
                            }
                        }
                        continue;
                    }
                }
            }

            const auto p = project(static_cast<float>(block.x) + alignX, static_cast<float>(block.y) + alignY, static_cast<float>(block.z) + alignZ);
            if (!p.visible) continue;
            SetDCPenColor(hdc, color);
            SetDCBrushColor(hdc, color);
            Rectangle(hdc,
                static_cast<int>(p.x) - (state->previewWorldStyle == PreviewWorldStyle::Points ? 2 : 1),
                static_cast<int>(p.y) - (state->previewWorldStyle == PreviewWorldStyle::Points ? 2 : 1),
                static_cast<int>(p.x) + (state->previewWorldStyle == PreviewWorldStyle::Points ? 3 : 2),
                static_cast<int>(p.y) + (state->previewWorldStyle == PreviewWorldStyle::Points ? 3 : 2));
        }
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
    }

    if (!state->previewLoaded || state->previewMesh.indices.empty()) {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(210, 216, 222));
        DrawTextW(hdc, L"未加载 3D 预览", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    const float cx = (mesh.min[0] + mesh.max[0]) * 0.5f;
    const float cy = (mesh.min[1] + mesh.max[1]) * 0.5f;
    const float cz = (mesh.min[2] + mesh.max[2]) * 0.5f;

    const auto targetCenter = project(setup.placement.previewCenterX, setup.placement.previewCenterY, setup.placement.previewCenterZ);
    if (targetCenter.visible) {
        HPEN centerPen = CreatePen(PS_SOLID, 2, RGB(255, 214, 10));
        HPEN oldCenterPen = (HPEN)SelectObject(hdc, centerPen);
        MoveToEx(hdc, static_cast<int>(targetCenter.x) - 8, static_cast<int>(targetCenter.y), nullptr);
        LineTo(hdc, static_cast<int>(targetCenter.x) + 8, static_cast<int>(targetCenter.y));
        MoveToEx(hdc, static_cast<int>(targetCenter.x), static_cast<int>(targetCenter.y) - 8, nullptr);
        LineTo(hdc, static_cast<int>(targetCenter.x), static_cast<int>(targetCenter.y) + 8);
        SelectObject(hdc, oldCenterPen);
        DeleteObject(centerPen);
    }

    const float gizmoLength = std::clamp(std::max({ setup.placement.sizeX, setup.placement.sizeY, setup.placement.sizeZ, 16.0f }) * 0.2f, 8.0f, 20.0f);
    const auto gx = project(setup.placement.previewCenterX + gizmoLength, setup.placement.previewCenterY, setup.placement.previewCenterZ);
    const auto gy = project(setup.placement.previewCenterX, setup.placement.previewCenterY + gizmoLength, setup.placement.previewCenterZ);
    const auto gz = project(setup.placement.previewCenterX, setup.placement.previewCenterY, setup.placement.previewCenterZ + gizmoLength);
    const PreviewPlacementDragMode hover = state->previewHoverDragMode;
    const PreviewPlacementDragMode active = state->previewPlacementDragMode;
    auto drawAxis = [&](const PreviewProjectedPoint& end, COLORREF color, PreviewPlacementDragMode axisMode, const wchar_t* axisLabel) {
        if (!targetCenter.visible || !end.visible) return;
        const bool highlighted = hover == axisMode || active == axisMode;
        HPEN pen = CreatePen(PS_SOLID, highlighted ? 3 : 2, color);
        HPEN oldPen = (HPEN)SelectObject(hdc, pen);
        MoveToEx(hdc, static_cast<int>(targetCenter.x), static_cast<int>(targetCenter.y), nullptr);
        LineTo(hdc, static_cast<int>(end.x), static_cast<int>(end.y));
        Rectangle(hdc, static_cast<int>(end.x) - (highlighted ? 5 : 4), static_cast<int>(end.y) - (highlighted ? 5 : 4),
            static_cast<int>(end.x) + (highlighted ? 5 : 4), static_cast<int>(end.y) + (highlighted ? 5 : 4));
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, color);
        TextOutW(hdc, static_cast<int>(end.x) + 6, static_cast<int>(end.y) - 8, axisLabel, 1);
    };
    drawAxis(gx, RGB(255, 92, 92), PreviewPlacementDragMode::AxisX, L"X");
    drawAxis(gy, RGB(118, 255, 3), PreviewPlacementDragMode::AxisY, L"Y");
    drawAxis(gz, RGB(64, 156, 255), PreviewPlacementDragMode::AxisZ, L"Z");

    const float hx = setup.placement.sizeX * 0.5f;
    const float hy = setup.placement.sizeY * 0.5f;
    const float hz = setup.placement.sizeZ * 0.5f;
    const std::array<std::array<float, 3>, 8> bboxCorners = {{
        {{-hx, -hy, -hz}}, {{hx, -hy, -hz}}, {{hx, -hy, hz}}, {{-hx, -hy, hz}},
        {{-hx, hy, -hz}}, {{hx, hy, -hz}}, {{hx, hy, hz}}, {{-hx, hy, hz}}
    }};
    const int bboxEdges[12][2] = {
        {0,1},{1,2},{2,3},{3,0},
        {4,5},{5,6},{6,7},{7,4},
        {0,4},{1,5},{2,6},{3,7}
    };
    std::array<PreviewProjectedPoint, 8> bboxPoints{};
    for (std::size_t i = 0; i < bboxCorners.size(); ++i) {
        bboxPoints[i] = project(
            setup.placement.previewCenterX + bboxCorners[i][0],
            setup.placement.previewCenterY + bboxCorners[i][1],
            setup.placement.previewCenterZ + bboxCorners[i][2]);
    }
    HPEN bboxPen = CreatePen(PS_DOT, 1, RGB(255, 214, 10));
    HPEN oldBboxPen = (HPEN)SelectObject(hdc, bboxPen);
    for (const auto& edge : bboxEdges) {
        const auto& a = bboxPoints[edge[0]];
        const auto& b = bboxPoints[edge[1]];
        if (!a.visible || !b.visible) continue;
        MoveToEx(hdc, static_cast<int>(a.x), static_cast<int>(a.y), nullptr);
        LineTo(hdc, static_cast<int>(b.x), static_cast<int>(b.y));
    }
    SelectObject(hdc, oldBboxPen);
    DeleteObject(bboxPen);

    const COLORREF wireColor = state->previewDrawMode == PreviewDrawMode::Wireframe ? RGB(105, 220, 242) : RGB(35, 53, 62);
    HPEN wirePen = CreatePen(PS_SOLID, state->previewDrawMode == PreviewDrawMode::Wireframe ? 1 : 1, wireColor);
    HPEN oldPen = (HPEN)SelectObject(hdc, wirePen);
    HBRUSH fillBrush = nullptr;
    HBRUSH oldBrush = nullptr;
    if (state->previewDrawMode == PreviewDrawMode::Solid || state->previewDrawMode == PreviewDrawMode::SolidWire) {
        fillBrush = CreateSolidBrush(RGB(79, 188, 212));
        oldBrush = (HBRUSH)SelectObject(hdc, fillBrush);
    } else {
        oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    }

    const std::size_t triangleCount = mesh.indices.size() / 3;
    const std::size_t triangleStride = std::max<std::size_t>(1, triangleCount > 50000 ? triangleCount / 50000 : 1);
    for (std::size_t tri = 0; tri < triangleCount; tri += triangleStride) {
        const std::size_t i0 = static_cast<std::size_t>(mesh.indices[tri * 3]) * 3;
        const std::size_t i1 = static_cast<std::size_t>(mesh.indices[tri * 3 + 1]) * 3;
        const std::size_t i2 = static_cast<std::size_t>(mesh.indices[tri * 3 + 2]) * 3;
        const auto p0 = projectPlaced(mesh.positions[i0] - cx, mesh.positions[i0 + 1] - cy, mesh.positions[i0 + 2] - cz);
        const auto p1 = projectPlaced(mesh.positions[i1] - cx, mesh.positions[i1 + 1] - cy, mesh.positions[i1 + 2] - cz);
        const auto p2 = projectPlaced(mesh.positions[i2] - cx, mesh.positions[i2 + 1] - cy, mesh.positions[i2 + 2] - cz);
        if (!p0.visible || !p1.visible || !p2.visible) continue;
        POINT points[3] = {
            { (LONG)p0.x, (LONG)p0.y },
            { (LONG)p1.x, (LONG)p1.y },
            { (LONG)p2.x, (LONG)p2.y }
        };
        if (state->previewDrawMode == PreviewDrawMode::Solid || state->previewDrawMode == PreviewDrawMode::SolidWire) {
            Polygon(hdc, points, 3);
        } else {
            MoveToEx(hdc, points[0].x, points[0].y, nullptr);
            LineTo(hdc, points[1].x, points[1].y);
            LineTo(hdc, points[2].x, points[2].y);
            LineTo(hdc, points[0].x, points[0].y);
        }
    }

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(wirePen);
    if (fillBrush) DeleteObject(fillBrush);
}

int GuiSelectPreviewLod(const GuiState* state) {
    if (!state || state->previewMesh.indices.empty()) return 0;
    if (state->previewForcedLod >= 0) {
        int forced = std::clamp(state->previewForcedLod, 0, static_cast<int>(state->previewMesh.lodIndices.size()) - 1);
        while (forced > 0 && state->previewMesh.lodIndices[forced].empty()) --forced;
        return forced;
    }
    const bool interacting = state->orbiting || state->panning ||
        state->previewDraggingPlacement || state->previewDraggingHeight;
    std::size_t triangleBudget = interacting ? 120000 : std::numeric_limits<std::size_t>::max();

    const auto& mesh = state->previewMesh;
    if (!state->previewAllModels) {
        const float dx = mesh.max[0] - mesh.min[0];
        const float dy = mesh.max[1] - mesh.min[1];
        const float dz = mesh.max[2] - mesh.min[2];
        const float modelScale = std::max(
            0.001f, ParseOptionalFloatText(state->config.scaleBlocksPerMeter).value_or(4.0f));
        const float radius = std::max(
            0.5f, std::sqrt(dx * dx + dy * dy + dz * dz) * 0.5f * modelScale);
        const float distanceRatio = state->previewDistance / radius;
        if (!interacting && distanceRatio > 8.0f) triangleBudget = 180000;
        else if (!interacting && distanceRatio > 5.0f) triangleBudget = 400000;
    }

    int level = 0;
    while (level + 1 < static_cast<int>(mesh.lodIndices.size()) &&
           ((level == 0 ? mesh.indices.size() : mesh.lodIndices[level].size()) / 3) > triangleBudget &&
           !mesh.lodIndices[level + 1].empty() &&
           mesh.lodIndices[level + 1].size() < (level == 0 ? mesh.indices.size() : mesh.lodIndices[level].size())) {
        ++level;
    }
    if (interacting) {
        level = std::max(level, std::clamp(state->previewAdaptiveLodBias, 0, 3));
    }
    while (level > 0 && mesh.lodIndices[level].empty()) --level;
    return level;
}

void GuiRenderPreviewWorldOpenGl(GuiState* state, const PreviewPlacement& placement) {
    if (!state || !state->previewWorldVisible || state->previewWorldSlice.blocks.empty()) return;
    const float alignX = placement.previewCenterX - static_cast<float>(state->previewWorldSlice.centerX);
    const float alignY = placement.previewCenterY - placement.worldCenterY;
    const float alignZ = placement.previewCenterZ - static_cast<float>(state->previewWorldSlice.centerZ);
    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);
    if (GuiUploadPreviewWorldGpu(state) && state->previewWorldGpuStyle == state->previewWorldStyle) {
        glPushMatrix();
        glTranslatef(alignX, alignY, alignZ);
        if (state->previewWorldPrimitive == GL_POINTS) glPointSize(3.0f);
        if (gGlBindBuffer) gGlBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        gGlBindBuffer(GL_ARRAY_BUFFER, state->previewWorldPositionBuffer);
        glVertexPointer(3, GL_FLOAT, 0, nullptr);
        gGlBindBuffer(GL_ARRAY_BUFFER, state->previewWorldColorBuffer);
        glColorPointer(3, GL_UNSIGNED_BYTE, 0, nullptr);
        glDrawArrays(state->previewWorldPrimitive, 0, state->previewWorldVertexCount);
        glDisableClientState(GL_COLOR_ARRAY);
        glDisableClientState(GL_VERTEX_ARRAY);
        gGlBindBuffer(GL_ARRAY_BUFFER, 0);
        glPopMatrix();
        glEnable(GL_CULL_FACE);
        return;
    }
    if (state->previewWorldGpuBuilding) {
        glEnable(GL_CULL_FACE);
        return;
    }
    if (state->previewWorldStyle == PreviewWorldStyle::Points) {
        glPointSize(3.0f);
        glBegin(GL_POINTS);
        for (const auto& block : state->previewWorldSlice.blocks) {
            COLORREF color = GuiFallbackBlockColor(block.blockName);
            const auto it = state->previewWorldColors.find(block.blockName);
            if (it != state->previewWorldColors.end()) color = it->second;
            glColor3ub(GetRValue(color), GetGValue(color), GetBValue(color));
            glVertex3f(block.x + alignX, block.y + alignY, block.z + alignZ);
        }
        glEnd();
    } else {
        glBegin(GL_QUADS);
        for (const auto& block : state->previewWorldSlice.blocks) {
            COLORREF color = GuiFallbackBlockColor(block.blockName);
            const auto it = state->previewWorldColors.find(block.blockName);
            if (it != state->previewWorldColors.end()) color = it->second;
            const float x = block.x + alignX, y = block.y + alignY, z = block.z + alignZ;
            const float halfSpanX = static_cast<float>(std::max(1, block.sampleSizeX)) * 0.5f;
            const float halfSpanZ = static_cast<float>(std::max(1, block.sampleSizeZ)) * 0.5f;
            const float x0 = x - halfSpanX, x1 = x + halfSpanX;
            const float y0 = static_cast<float>(block.bottomY) + alignY - 0.5f, y1 = y + 0.5f;
            const float z0 = z - halfSpanZ, z1 = z + halfSpanZ;
            glColor3ub(GetRValue(color), GetGValue(color), GetBValue(color));
            glVertex3f(x0, y1, z0); glVertex3f(x1, y1, z0); glVertex3f(x1, y1, z1); glVertex3f(x0, y1, z1);
            if (state->previewWorldStyle == PreviewWorldStyle::Faces) continue;
            glColor3ub(static_cast<GLubyte>(GetRValue(color) * 0.72f), static_cast<GLubyte>(GetGValue(color) * 0.72f), static_cast<GLubyte>(GetBValue(color) * 0.72f));
            glVertex3f(x0, y0, z0); glVertex3f(x0, y1, z0); glVertex3f(x0, y1, z1); glVertex3f(x0, y0, z1);
            glColor3ub(static_cast<GLubyte>(GetRValue(color) * 0.82f), static_cast<GLubyte>(GetGValue(color) * 0.82f), static_cast<GLubyte>(GetBValue(color) * 0.82f));
            glVertex3f(x0, y0, z0); glVertex3f(x1, y0, z0); glVertex3f(x1, y1, z0); glVertex3f(x0, y1, z0);
            glColor3ub(static_cast<GLubyte>(GetRValue(color) * 0.68f), static_cast<GLubyte>(GetGValue(color) * 0.68f), static_cast<GLubyte>(GetBValue(color) * 0.68f));
            glVertex3f(x1, y0, z1); glVertex3f(x1, y1, z1); glVertex3f(x1, y1, z0); glVertex3f(x1, y0, z0);
            glColor3ub(static_cast<GLubyte>(GetRValue(color) * 0.78f), static_cast<GLubyte>(GetGValue(color) * 0.78f), static_cast<GLubyte>(GetBValue(color) * 0.78f));
            glVertex3f(x1, y0, z1); glVertex3f(x0, y0, z1); glVertex3f(x0, y1, z1); glVertex3f(x1, y1, z1);
        }
        glEnd();
    }
    glEnable(GL_CULL_FACE);
}

void GuiRenderPreviewGizmoOpenGl(GuiState* state, const PreviewPlacement& placement) {
    if (!state) return;
    const float length = std::clamp(std::max({ placement.sizeX, placement.sizeY, placement.sizeZ, 16.0f }) * 0.2f, 8.0f, 20.0f);
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    glLineWidth(3.0f);
    glBegin(GL_LINES);
    glColor3ub(255, 92, 92); glVertex3f(placement.previewCenterX, placement.previewCenterY, placement.previewCenterZ); glVertex3f(placement.previewCenterX + length, placement.previewCenterY, placement.previewCenterZ);
    glColor3ub(118, 255, 3); glVertex3f(placement.previewCenterX, placement.previewCenterY, placement.previewCenterZ); glVertex3f(placement.previewCenterX, placement.previewCenterY + length, placement.previewCenterZ);
    glColor3ub(64, 156, 255); glVertex3f(placement.previewCenterX, placement.previewCenterY, placement.previewCenterZ); glVertex3f(placement.previewCenterX, placement.previewCenterY, placement.previewCenterZ + length);
    glEnd();
    glPointSize(8.0f);
    glBegin(GL_POINTS);
    glColor3ub(255, 214, 10); glVertex3f(placement.previewCenterX, placement.previewCenterY, placement.previewCenterZ);
    glColor3ub(255, 92, 92); glVertex3f(placement.previewCenterX + length, placement.previewCenterY, placement.previewCenterZ);
    glColor3ub(118, 255, 3); glVertex3f(placement.previewCenterX, placement.previewCenterY + length, placement.previewCenterZ);
    glColor3ub(64, 156, 255); glVertex3f(placement.previewCenterX, placement.previewCenterY, placement.previewCenterZ + length);
    glEnd();
    glEnable(GL_DEPTH_TEST);
}

void GuiUpdatePreviewFrameTiming(GuiState* state, double frameMilliseconds) {
    if (!state) return;
    state->previewFrameMs = state->previewFrameMs <= 0.0
        ? frameMilliseconds : (state->previewFrameMs * 0.8 + frameMilliseconds * 0.2);
    if (state->previewLodUpgradeCooldown > 0) --state->previewLodUpgradeCooldown;
    if (state->previewFrameMs > 34.0 && state->previewAdaptiveLodBias < 3) {
        ++state->previewAdaptiveLodBias;
        state->previewFastFrameCount = 0;
        state->previewLodUpgradeCooldown = 180;
        state->previewSettleFrames = std::max(state->previewSettleFrames, 24);
    } else if (state->previewFrameMs < 14.0 && state->previewAdaptiveLodBias > 0 &&
               state->previewLodUpgradeCooldown == 0) {
        if (++state->previewFastFrameCount >= 20) {
            --state->previewAdaptiveLodBias;
            state->previewFastFrameCount = 0;
            state->previewSettleFrames = std::max(state->previewSettleFrames, 16);
        }
    } else {
        state->previewFastFrameCount = 0;
    }
    if (state->previewSettleFrames > 0) {
        --state->previewSettleFrames;
        if (state->previewSettleFrames == 0) GuiUpdatePreviewChrome(state);
    }
}

void GuiUpdatePreviewPresentRate(GuiState* state,
                                 native_mc::PreviewRendererBackend backend,
                                 std::uint64_t frameNumber) {
    if (!state || frameNumber == 0) return;
    const auto now = std::chrono::steady_clock::now();
    if (state->previewPresentBackend != backend ||
        frameNumber < state->previewPresentFrameNumber) {
        state->previewPresentFps = 0.0;
        state->previewPresentBackend = backend;
        state->previewPresentFrameNumber = frameNumber;
        state->previewPresentSampleFrame = frameNumber;
        state->previewPresentSampleStarted = now;
        state->previewLastPresentAt = now;
        return;
    }
    if (frameNumber == state->previewPresentFrameNumber) return;

    if (state->previewLastPresentAt != std::chrono::steady_clock::time_point{} &&
        now - state->previewLastPresentAt > std::chrono::seconds(2)) {
        state->previewPresentFps = 0.0;
        state->previewPresentFrameNumber = frameNumber;
        state->previewPresentSampleFrame = frameNumber;
        state->previewPresentSampleStarted = now;
        state->previewLastPresentAt = now;
        GuiUpdatePreviewChromeThrottled(state, 250);
        return;
    }
    state->previewPresentFrameNumber = frameNumber;
    state->previewLastPresentAt = now;
    if (state->previewPresentSampleStarted == std::chrono::steady_clock::time_point{}) {
        state->previewPresentSampleStarted = now;
        state->previewPresentSampleFrame = frameNumber;
        return;
    }
    const double elapsedSeconds = std::chrono::duration<double>(
        now - state->previewPresentSampleStarted).count();
    if (elapsedSeconds < 0.5) return;

    const std::uint64_t presentedFrames = frameNumber - state->previewPresentSampleFrame;
    const double sampleFps = presentedFrames / elapsedSeconds;
    state->previewPresentFps = state->previewPresentFps <= 0.0
        ? sampleFps : state->previewPresentFps * 0.65 + sampleFps * 0.35;
    state->previewPresentSampleFrame = frameNumber;
    state->previewPresentSampleStarted = now;
    GuiUpdatePreviewChromeThrottled(state, 250);
}

bool GuiRenderPreviewSceneNative(GuiState* state) {
    if (!GuiUsingNativePreviewRenderer(state)) return false;
    const auto frameStartedAt = std::chrono::steady_clock::now();

    native_mc::PreviewRendererFrame frame;
    frame.camera.yawDegrees = state->previewYaw;
    frame.camera.pitchDegrees = state->previewPitch;
    frame.camera.distance = state->previewDistance;
    frame.camera.panX = state->previewPanX;
    frame.camera.panY = state->previewPanY;
    frame.camera.farPlane = std::max(5000.0f, state->previewDistance * 3.0f);
    const PreviewGridLayout grid = GuiComputePreviewGridLayout(state);
    frame.overlay.showGrid = state->previewShowGrid;
    frame.overlay.showAxes = state->previewShowAxes;
    frame.overlay.gridStep = grid.step;
    frame.overlay.gridLineCount = static_cast<std::uint32_t>(std::max(1, grid.lineCount));
    frame.showModel = state->previewLoaded && !state->previewMesh.indices.empty();
    frame.showWorld = false;

    if (frame.showModel) {
        const auto& mesh = state->previewMesh;
        state->previewActiveLod = GuiSelectPreviewLod(state);
        frame.lodLevel = static_cast<std::uint32_t>(state->previewActiveLod);
        if (!GuiUploadPreviewGpu(state)) frame.showModel = false;

        const float cx = (mesh.min[0] + mesh.max[0]) * 0.5f;
        const float cy = (mesh.min[1] + mesh.max[1]) * 0.5f;
        const float cz = (mesh.min[2] + mesh.max[2]) * 0.5f;
        const float scale = std::max(0.001f, GuiReadMappedFloat(state, L"scaleBlocksPerMeter", 4.0f));
        const bool flipX = GuiReadMappedCheckValue(state, L"flipX");
        const bool flipZ = GuiReadEffectiveFlipZ(state);
        const auto worldSize = GuiComputePreviewWorldSize(state, mesh);
        const PreviewPlacement placement = GuiResolvePreviewPlacement(state, worldSize[0], worldSize[1], worldSize[2]);

        frame.model.position = { placement.previewCenterX, placement.previewCenterY, placement.previewCenterZ };
        frame.model.center = { cx, cy, cz };
        frame.model.scale = { flipX ? -scale : scale, scale, flipZ ? -scale : scale };
        frame.model.sourceQuaternion = GuiReadSourceQuaternion(state);
        frame.model.quaternion = GuiReadModelQuaternion(state);
        frame.overlay.gizmoPosition = { placement.previewCenterX, placement.previewCenterY, placement.previewCenterZ };
        frame.overlay.gizmoLength = std::clamp(
            std::max({ placement.sizeX, placement.sizeY, placement.sizeZ, 16.0f }) * 0.2f, 8.0f, 20.0f);

        if (state->previewWorldVisible && !state->previewWorldSlice.blocks.empty() &&
            GuiUploadPreviewWorldGpu(state)) {
            frame.showWorld = true;
            frame.worldOffset = {
                placement.previewCenterX - static_cast<float>(state->previewWorldSlice.centerX),
                placement.previewCenterY - placement.worldCenterY,
                placement.previewCenterZ - static_cast<float>(state->previewWorldSlice.centerZ)
            };
        }
    }

    switch (state->previewDrawMode) {
    case PreviewDrawMode::Solid: frame.drawMode = native_mc::PreviewRendererDrawMode::Solid; break;
    case PreviewDrawMode::Wireframe: frame.drawMode = native_mc::PreviewRendererDrawMode::Wireframe; break;
    case PreviewDrawMode::SolidWire: frame.drawMode = native_mc::PreviewRendererDrawMode::SolidWire; break;
    }

    native_mc::PreviewRendererStats stats;
    std::string error;
    if (!state->previewNativeRenderer->Render(frame, &stats, &error)) {
        state->previewRendererError = Utf8ToWide(error);
        state->previewNativeRenderer->Shutdown();
        state->previewNativeRenderer.reset();
        state->previewActiveRenderer = native_mc::PreviewRendererBackend::OpenGL;
        state->previewRendererStatus = L"OpenGL（设备错误回退）";
        state->previewGpuDirty = true;
        state->previewWorldGpuDirty = true;
        GuiInitPreviewContext(state);
        GuiAppendLog(state, L"现代预览后端绘制失败，已回退 OpenGL: " + state->previewRendererError + L"\r\n");
        return false;
    }
    state->previewRenderedTriangles = static_cast<std::size_t>(stats.modelTriangles);
    GuiUpdatePreviewPresentRate(state, stats.backend, stats.frameNumber);
    const double frameMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - frameStartedAt).count();
    GuiUpdatePreviewFrameTiming(state, frameMs);
    return true;
}

void GuiRenderPreviewScene(GuiState* state, HWND hwnd) {
    if (!state) return;
    const ULONGLONG frameStartedAt = GetTickCount64();
    if (!GuiInitPreviewContext(state)) return;
    if (GuiUsingNativePreviewRenderer(state)) {
        if (GuiRenderPreviewSceneNative(state)) return;
    }
    if (!state->previewRc || !state->previewDc) return;
    wglMakeCurrent(state->previewDc, state->previewRc);

    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int width = std::max(1L, rc.right - rc.left);
    const int height = std::max(1L, rc.bottom - rc.top);
    glViewport(0, 0, width, height);
    glClearColor(0.11f, 0.12f, 0.13f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    const float farPlane = std::max(5000.0f, state->previewDistance * 3.0f);
    GuiApplyPerspective(static_cast<float>(width) / static_cast<float>(height), 50.0f, 0.1f, farPlane);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(state->previewPanX, state->previewPanY, -state->previewDistance);
    glRotatef(state->previewPitch, 1.0f, 0.0f, 0.0f);
    glRotatef(state->previewYaw, 0.0f, 1.0f, 0.0f);

    if (state->previewShowGrid) {
        const PreviewGridLayout grid = GuiComputePreviewGridLayout(state);
        glLineWidth(1.0f);
        glBegin(GL_LINES);
        glColor3ub(46, 49, 54);
        for (int i = -grid.lineCount; i <= grid.lineCount; ++i) {
            const float line = i * grid.step;
            glVertex3f(line, 0.0f, -grid.extent);
            glVertex3f(line, 0.0f, grid.extent);
            glVertex3f(-grid.extent, 0.0f, line);
            glVertex3f(grid.extent, 0.0f, line);
        }
        glEnd();
    }

    if (state->previewShowAxes) {
        glLineWidth(2.0f);
        glBegin(GL_LINES);
        glColor3ub(255, 82, 82); glVertex3f(0, 0, 0); glVertex3f(4, 0, 0);
        glColor3ub(118, 255, 3); glVertex3f(0, 0, 0); glVertex3f(0, 4, 0);
        glColor3ub(64, 156, 255); glVertex3f(0, 0, 0); glVertex3f(0, 0, 4);
        glEnd();
    }

    if (state->previewLoaded && !state->previewMesh.indices.empty()) {
        const auto& mesh = state->previewMesh;
        state->previewActiveLod = GuiSelectPreviewLod(state);
        const auto& drawIndices = mesh.lodIndices[state->previewActiveLod].empty()
            ? mesh.indices : mesh.lodIndices[state->previewActiveLod];
        state->previewRenderedTriangles = drawIndices.size() / 3;
        const bool gpuReady = GuiUploadPreviewGpu(state);
        const GLsizei drawCount = gpuReady
            ? state->previewIndexCounts[state->previewActiveLod]
            : static_cast<GLsizei>(std::min<std::size_t>(drawIndices.size(), static_cast<std::size_t>(std::numeric_limits<GLsizei>::max())));
        const auto modelRotation = GuiReadModelQuaternion(state);
        const float scale = std::max(
            0.001f, GuiReadMappedFloat(state, L"scaleBlocksPerMeter", 4.0f));
        const bool flipX = GuiReadMappedCheckValue(state, L"flipX");
        const bool flipZ = GuiReadEffectiveFlipZ(state);
        const std::wstring transformMode = ToLower(GuiReadMappedText(state, L"transform", L""));
        const std::wstring levelNormal = GuiReadMappedText(state, L"levelNormal", L"");
        const float cx = (mesh.min[0] + mesh.max[0]) * 0.5f;
        const float cy = (mesh.min[1] + mesh.max[1]) * 0.5f;
        const float cz = (mesh.min[2] + mesh.max[2]) * 0.5f;
        const auto worldSize = GuiComputePreviewWorldSize(state, mesh);
        const PreviewPlacement placement = GuiResolvePreviewPlacement(state, worldSize[0], worldSize[1], worldSize[2]);
        GuiRenderPreviewWorldOpenGl(state, placement);
        glPushMatrix();
        glTranslatef(placement.previewCenterX, placement.previewCenterY, placement.previewCenterZ);
        GuiApplyQuaternionOpenGl(modelRotation);
        glScalef(flipX ? -scale : scale, scale, flipZ ? -scale : scale);
        if (transformMode == L"obj-z-up") {
            glRotatef(-90.0f, 1.0f, 0.0f, 0.0f);
            if (const auto level = GuiParseLevelRotation(levelNormal)) {
                glRotatef((*level)[0], (*level)[1], (*level)[2], (*level)[3]);
            }
        }
        glTranslatef(-cx, -cy, -cz);

        glEnableClientState(GL_VERTEX_ARRAY);
        if (gpuReady) {
            gGlBindBuffer(GL_ARRAY_BUFFER, state->previewPositionBuffer);
            glVertexPointer(3, GL_FLOAT, 0, nullptr);
            gGlBindBuffer(GL_ELEMENT_ARRAY_BUFFER, state->previewIndexBuffers[state->previewActiveLod]);
        } else {
            if (gGlBindBuffer) {
                gGlBindBuffer(GL_ARRAY_BUFFER, 0);
                gGlBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
            }
            glVertexPointer(3, GL_FLOAT, 0, mesh.positions.data());
        }

        if (state->previewDrawMode == PreviewDrawMode::Solid || state->previewDrawMode == PreviewDrawMode::SolidWire) {
            GLfloat lightPos[] = { 0.4f, 1.0f, 0.6f, 0.0f };
            GLfloat lightDiffuse[] = { 0.85f, 0.9f, 0.95f, 1.0f };
            GLfloat lightAmbient[] = { 0.18f, 0.2f, 0.22f, 1.0f };
            glEnable(GL_LIGHTING);
            glEnable(GL_LIGHT0);
            glEnable(GL_NORMALIZE);
            glEnable(GL_COLOR_MATERIAL);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
            glLightfv(GL_LIGHT0, GL_POSITION, lightPos);
            glLightfv(GL_LIGHT0, GL_DIFFUSE, lightDiffuse);
            glLightfv(GL_LIGHT0, GL_AMBIENT, lightAmbient);
            glEnableClientState(GL_COLOR_ARRAY);
            if (gpuReady) {
                gGlBindBuffer(GL_ARRAY_BUFFER, state->previewColorBuffer);
                glColorPointer(4, GL_UNSIGNED_BYTE, 0, nullptr);
            } else {
                glColorPointer(4, GL_UNSIGNED_BYTE, 0, mesh.colors.data());
            }
            glEnableClientState(GL_NORMAL_ARRAY);
            if (gpuReady) {
                gGlBindBuffer(GL_ARRAY_BUFFER, state->previewNormalBuffer);
                glNormalPointer(GL_FLOAT, 0, nullptr);
            } else {
                glNormalPointer(GL_FLOAT, 0, mesh.normals.data());
            }
            glDrawElements(GL_TRIANGLES, drawCount, GL_UNSIGNED_INT, gpuReady ? nullptr : drawIndices.data());
            glDisableClientState(GL_NORMAL_ARRAY);
            glDisableClientState(GL_COLOR_ARRAY);
            if (gpuReady) {
                gGlBindBuffer(GL_ARRAY_BUFFER, state->previewPositionBuffer);
                glVertexPointer(3, GL_FLOAT, 0, nullptr);
            }
            glDisable(GL_COLOR_MATERIAL);
            glDisable(GL_BLEND);
            glDisable(GL_LIGHT0);
            glDisable(GL_LIGHTING);
        }

        if (state->previewDrawMode == PreviewDrawMode::Wireframe || state->previewDrawMode == PreviewDrawMode::SolidWire) {
            glDisable(GL_LIGHTING);
            glColor3ub(state->previewDrawMode == PreviewDrawMode::SolidWire ? 20 : 77,
                       state->previewDrawMode == PreviewDrawMode::SolidWire ? 38 : 208,
                       state->previewDrawMode == PreviewDrawMode::SolidWire ? 48 : 225);
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            glLineWidth(state->previewDrawMode == PreviewDrawMode::SolidWire ? 1.0f : 1.2f);
            glDrawElements(GL_TRIANGLES, drawCount, GL_UNSIGNED_INT, gpuReady ? nullptr : drawIndices.data());
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        }
        glDisableClientState(GL_VERTEX_ARRAY);
        if (gpuReady) {
            gGlBindBuffer(GL_ARRAY_BUFFER, 0);
            gGlBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        }
        glPopMatrix();
        GuiRenderPreviewGizmoOpenGl(state, placement);
    }

    if (SwapBuffers(state->previewDc)) {
        GuiUpdatePreviewPresentRate(
            state, native_mc::PreviewRendererBackend::OpenGL,
            ++state->previewOpenGlFrameNumber);
    }
    GuiUpdatePreviewFrameTiming(state, static_cast<double>(GetTickCount64() - frameStartedAt));
}

std::uint64_t GuiPreviewInputBytes(const std::vector<fs::path>& files) {
    std::uint64_t total = 0;
    for (const auto& file : files) {
        std::error_code ec;
        const auto size = fs::file_size(file, ec);
        if (!ec) total += static_cast<std::uint64_t>(size);
    }
    return total;
}

void GuiClosePreviewProgress(GuiState* state) {
    if (!state) return;
    if (state->previewProgressWindow && IsWindow(state->previewProgressWindow)) {
        DestroyWindow(state->previewProgressWindow);
    }
    state->previewProgressWindow = nullptr;
    state->previewProgressLabel = nullptr;
    state->previewProgressBar = nullptr;
}

bool GuiRestartApplication(GuiState* state) {
    if (!state || state->exePath.empty()) return false;
    std::wstring commandLine = QuoteForWindows(state->exePath.wstring());
    std::vector<wchar_t> buffer(commandLine.begin(), commandLine.end());
    buffer.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(
        state->exePath.wstring().c_str(), buffer.data(), nullptr, nullptr, FALSE, 0, nullptr,
        state->exeDir.empty() ? nullptr : state->exeDir.wstring().c_str(), &startup, &process);
    if (!started) return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    PostMessageW(state->hwnd, WM_CLOSE, 0, 0);
    return true;
}

void GuiShowPreviewProgress(GuiState* state, const std::wstring& title, int total, bool marquee) {
    if (!state) return;
    GuiClosePreviewProgress(state);
    RECT owner{};
    GetWindowRect(state->hwnd, &owner);
    const int width = 440;
    const int height = 132;
    const int x = static_cast<int>(owner.left) + std::max(0, (static_cast<int>(owner.right - owner.left) - width) / 2);
    const int y = static_cast<int>(owner.top) + std::max(0, (static_cast<int>(owner.bottom - owner.top) - height) / 2);
    state->previewProgressWindow = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOOLWINDOW, L"STATIC", title.c_str(),
        WS_POPUP | WS_CAPTION | WS_VISIBLE, x, y, width, height,
        state->hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!state->previewProgressWindow) return;
    state->previewProgressLabel = CreateWindowExW(0, L"STATIC", L"正在准备...", WS_CHILD | WS_VISIBLE,
        18, 18, width - 36, 24, state->previewProgressWindow, nullptr, nullptr, nullptr);
    state->previewProgressBar = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE | (marquee ? PBS_MARQUEE : 0),
        18, 52, width - 36, 22, state->previewProgressWindow, nullptr, nullptr, nullptr);
    SendMessageW(state->previewProgressLabel, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
    SendMessageW(state->previewProgressBar, PBM_SETRANGE32, 0, std::max(1, total));
    if (marquee) SendMessageW(state->previewProgressBar, PBM_SETMARQUEE, TRUE, 35);
    UpdateWindow(state->previewProgressWindow);
}

bool GuiConfirmLodProcessing(GuiState* state, const std::vector<fs::path>& files, bool allModels) {
    return state && !files.empty();
}

bool GuiAppendPreviewMesh(native_mc::PreviewMesh* target, const native_mc::PreviewMesh& source,
                          std::size_t sourceLod, std::string* errorText, bool includeLodLevels = true) {
    if (!target || source.positions.empty() || source.indices.empty()) return true;
    sourceLod = std::min<std::size_t>(sourceLod, source.lodIndices.size() - 1);
    const std::size_t vertexCount = source.positions.size() / 3;
    std::vector<std::uint32_t> remap(vertexCount, std::numeric_limits<std::uint32_t>::max());
    std::vector<std::uint32_t> used;
    const std::size_t outputLevels = includeLodLevels ? target->lodIndices.size() : 1;
    for (std::size_t outLevel = 0; outLevel < outputLevels; ++outLevel) {
        const std::size_t level = std::min<std::size_t>(sourceLod + outLevel, source.lodIndices.size() - 1);
        const auto& indices = level == 0 || source.lodIndices[level].empty() ? source.indices : source.lodIndices[level];
        for (const std::uint32_t index : indices) {
            if (index >= vertexCount || remap[index] != std::numeric_limits<std::uint32_t>::max()) continue;
            remap[index] = 0;
            used.push_back(index);
        }
    }
    const std::size_t targetVertexCount = target->positions.size() / 3;
    if (targetVertexCount + used.size() > std::numeric_limits<std::uint32_t>::max()) {
        if (errorText) *errorText = "Combined preview exceeds the 32-bit vertex limit";
        return false;
    }
    for (const std::uint32_t oldIndex : used) {
        const std::uint32_t newIndex = static_cast<std::uint32_t>(target->positions.size() / 3);
        remap[oldIndex] = newIndex;
        target->positions.insert(target->positions.end(), source.positions.begin() + oldIndex * 3, source.positions.begin() + oldIndex * 3 + 3);
        target->normals.insert(target->normals.end(), source.normals.begin() + oldIndex * 3, source.normals.begin() + oldIndex * 3 + 3);
        target->colors.insert(target->colors.end(), source.colors.begin() + oldIndex * 4, source.colors.begin() + oldIndex * 4 + 4);
    }
    for (std::size_t outLevel = 0; outLevel < outputLevels; ++outLevel) {
        const std::size_t level = std::min<std::size_t>(sourceLod + outLevel, source.lodIndices.size() - 1);
        const auto& indices = level == 0 || source.lodIndices[level].empty() ? source.indices : source.lodIndices[level];
        auto& output = outLevel == 0 ? target->indices : target->lodIndices[outLevel];
        output.reserve(output.size() + indices.size());
        for (const std::uint32_t index : indices) {
            if (index < remap.size() && remap[index] != std::numeric_limits<std::uint32_t>::max()) output.push_back(remap[index]);
        }
    }
    for (int axis = 0; axis < 3; ++axis) {
        if (targetVertexCount == 0) {
            target->min[axis] = source.min[axis];
            target->max[axis] = source.max[axis];
        } else {
            target->min[axis] = std::min(target->min[axis], source.min[axis]);
            target->max[axis] = std::max(target->max[axis], source.max[axis]);
        }
    }
    return true;
}

constexpr std::array<std::uint32_t, 4> kOverviewProxyResolutions = { 64, 48, 32, 24 };
constexpr char kOverviewCacheProfile[] = "overview-tile-proxy-v4-oriented-monotonic-r64-48-32-24";

bool GuiPrepareOverviewPreviewMesh(native_mc::PreviewMesh* mesh, bool* usedGpu,
                                   native_mc::PreviewGpuLodBackend* activeBackend,
                                   bool* usedBackendFallback,
                                   std::string* backendName, std::string* errorText) {
    if (usedGpu) *usedGpu = false;
    if (activeBackend) *activeBackend = native_mc::PreviewGpuLodBackend::None;
    if (usedBackendFallback) *usedBackendFallback = false;
    if (!mesh || mesh->positions.empty() || mesh->indices.empty()) {
        if (errorText) *errorText = "Overview source mesh was empty";
        return false;
    }

    std::array<std::vector<std::uint32_t>, 4> proxies;
    const auto gpuResult = native_mc::BuildPreviewMeshGpuProxies(
        *mesh, kOverviewProxyResolutions, &proxies);
    if (gpuResult.success) {
        if (usedGpu) *usedGpu = true;
        if (activeBackend) *activeBackend = gpuResult.activeBackend;
        if (usedBackendFallback) *usedBackendFallback = gpuResult.usedBackendFallback;
        if (backendName) *backendName = gpuResult.backendName;
    } else {
        proxies = native_mc::BuildPreviewMeshProxyIndices(*mesh, kOverviewProxyResolutions);
        if (backendName) *backendName = "CPU tile clustering";
    }
    if (proxies[0].empty()) {
        if (errorText) {
            *errorText = gpuResult.error.empty()
                ? "Could not generate an overview proxy"
                : gpuResult.error;
        }
        return false;
    }
    std::array<std::vector<std::uint32_t>, 4> monotonic;
    std::size_t levelCount = 0;
    for (auto& proxy : proxies) {
        if (proxy.empty()) continue;
        if (levelCount == 0 || proxy.size() < monotonic[levelCount - 1].size()) {
            monotonic[levelCount++] = std::move(proxy);
        }
    }
    while (levelCount < monotonic.size()) {
        monotonic[levelCount] = monotonic[levelCount - 1];
        ++levelCount;
    }
    proxies = std::move(monotonic);

    mesh->indices = std::move(proxies[0]);
    mesh->lodIndices[0].clear();
    for (std::size_t level = 1; level < proxies.size(); ++level) {
        mesh->lodIndices[level] = std::move(proxies[level]);
    }

    native_mc::PreviewMesh compact;
    if (!GuiAppendPreviewMesh(&compact, *mesh, 0, errorText, true)) return false;
    *mesh = std::move(compact);
    return true;
}

void GuiStartPreviewLoad(GuiState* state, std::vector<fs::path> files, bool allModels,
                         bool allAvailableModels) {
    if (!state || state->previewLoading || files.empty()) return;
    if (!GuiConfirmLodProcessing(state, files, allModels)) return;
    const bool showProgress = allModels || GuiPreviewInputBytes(files) >= 32ull * 1024ull * 1024ull;
    const std::wstring overviewName = allAvailableModels ? L"全部模型总览" : L"所选模型总览";
    if (showProgress) GuiShowPreviewProgress(
        state, allModels ? L"正在准备" + overviewName : L"正在生成模型 LOD",
        static_cast<int>(files.size()), !allModels);
    state->previewLoading = true;
    state->previewAllModels = allModels;
    state->previewAllAvailableModels = allModels && allAvailableModels;
    if (allModels && state->previewDrawMode == PreviewDrawMode::SolidWire) {
        state->previewDrawMode = PreviewDrawMode::Solid;
    }
    state->previewModelCount = allModels ? files.size() : 1;
    state->previewSource = allModels ? files.front().parent_path() : files.front();
    GuiSetStatus(state, allModels ? L"正在合并" + overviewName + L"并生成 LOD..." : L"正在加载预览并生成 LOD...");
    GuiUpdatePreviewChrome(state);
    const HWND hwnd = state->hwnd;
    if (state->previewLoadThread.joinable()) state->previewLoadThread.join();
    state->previewLoadCancel.store(false, std::memory_order_relaxed);
    std::atomic<bool>* const cancelRequested = &state->previewLoadCancel;
    const int previewWorkers = allModels
        ? std::clamp(state->config.workers, 1, 32)
        : 1;
    const fs::path previewCacheDirectory = state->exeDir / "cache";
    try {
    state->previewLoadThread = std::thread([hwnd, files = std::move(files), allModels, allAvailableModels, previewWorkers,
                 previewCacheDirectory, cancelRequested]() mutable {
        auto* payload = new std::pair<bool, std::wstring>();
        payload->first = false;
        try {
            native_mc::PreviewMesh combined;
            std::atomic<std::size_t> nextFile{0};
            std::atomic<std::size_t> completed{0};
            std::atomic<std::size_t> loaded{0};
            std::atomic<std::size_t> skipped{0};
            std::atomic<std::size_t> gpuLodMeshes{0};
            std::atomic<std::size_t> gpuLodD3D12Meshes{0};
            std::atomic<std::size_t> gpuLodD3D11Meshes{0};
            std::atomic<std::size_t> gpuLodBackendFallbacks{0};
            std::atomic<std::size_t> cpuLodMeshes{0};
            std::atomic<std::size_t> cacheHits{0};
            std::atomic<std::size_t> cacheMisses{0};
            std::atomic<std::size_t> cacheWrites{0};
            std::atomic<std::size_t> cacheErrors{0};
            std::atomic<std::size_t> cacheDirectStorageHits{0};
            std::atomic<std::size_t> cacheWin32Hits{0};
            std::atomic<std::size_t> cacheReadFallbacks{0};
            std::atomic<bool> failed{false};
            std::mutex combinedMutex;
            std::mutex errorMutex;
            std::mutex statsMutex;
            std::wstring loadError;
            std::string d3d12GpuBackendName;
            std::string d3d11GpuBackendName;
            auto loadWorker = [&]() {
                while (!failed.load(std::memory_order_relaxed) &&
                       !cancelRequested->load(std::memory_order_relaxed)) {
                    const std::size_t i = nextFile.fetch_add(1, std::memory_order_relaxed);
                    if (i >= files.size()) break;
                    bool loadedFromCache = false;
                    try {
                    native_mc::PreviewMesh mesh;
                    std::string error;
                    native_mc::PreviewMeshCacheSourceStamp cacheStamp;
                    bool cacheStampReady = false;
                    bool meshReady = false;

                    if (allModels) {
                        cacheStampReady = native_mc::CapturePreviewMeshCacheSourceStamp(
                            files[i], {}, &cacheStamp, &error, kOverviewCacheProfile);
                        if (cacheStampReady) {
                            const auto cacheResult = native_mc::LoadPreviewMeshCache(
                                previewCacheDirectory, cacheStamp, &mesh);
                            if (cacheResult.hit()) {
                                meshReady = true;
                                loadedFromCache = true;
                                cacheHits.fetch_add(1, std::memory_order_relaxed);
                                if (cacheResult.fileRead.backend == native_mc::PreviewFileReadBackend::DirectStorage) {
                                    cacheDirectStorageHits.fetch_add(1, std::memory_order_relaxed);
                                } else if (cacheResult.fileRead.backend == native_mc::PreviewFileReadBackend::Win32) {
                                    cacheWin32Hits.fetch_add(1, std::memory_order_relaxed);
                                }
                                if (cacheResult.fileRead.usedFallback) {
                                    cacheReadFallbacks.fetch_add(1, std::memory_order_relaxed);
                                }
                            } else {
                                cacheMisses.fetch_add(1, std::memory_order_relaxed);
                                if (cacheResult.status == native_mc::PreviewMeshCacheLoadStatus::Invalid ||
                                    cacheResult.status == native_mc::PreviewMeshCacheLoadStatus::IoError) {
                                    cacheErrors.fetch_add(1, std::memory_order_relaxed);
                                }
                            }
                        } else {
                            cacheErrors.fetch_add(1, std::memory_order_relaxed);
                        }
                    }

                    if (!meshReady && native_mc::LoadPreviewMesh(files[i], &mesh, &error, false)) {
                        bool usedGpu = false;
                        native_mc::PreviewGpuLodBackend activeGpuBackend =
                            native_mc::PreviewGpuLodBackend::None;
                        bool usedGpuBackendFallback = false;
                        std::string lodBackend;
                        bool lodOk = true;
                        if (allModels) {
                            lodOk = GuiPrepareOverviewPreviewMesh(
                                &mesh, &usedGpu, &activeGpuBackend, &usedGpuBackendFallback,
                                &lodBackend, &error);
                        } else {
                            const auto gpuResult = native_mc::BuildPreviewMeshLodsGpu(&mesh);
                            usedGpu = gpuResult.success;
                            activeGpuBackend = gpuResult.activeBackend;
                            usedGpuBackendFallback = gpuResult.usedBackendFallback;
                            lodBackend = gpuResult.backendName;
                            if (!gpuResult.success) {
                                native_mc::BuildPreviewMeshLods(&mesh);
                                lodBackend = "CPU tile clustering";
                            }
                        }
                        if (!lodOk) {
                            if (!allModels) {
                                std::lock_guard<std::mutex> lock(errorMutex);
                                loadError = Utf8ToWide(error);
                                failed.store(true, std::memory_order_relaxed);
                            } else {
                                skipped.fetch_add(1, std::memory_order_relaxed);
                            }
                        } else {
                            meshReady = true;
                            if (usedGpu) {
                                gpuLodMeshes.fetch_add(1, std::memory_order_relaxed);
                                if (activeGpuBackend == native_mc::PreviewGpuLodBackend::D3D12) {
                                    gpuLodD3D12Meshes.fetch_add(1, std::memory_order_relaxed);
                                } else if (activeGpuBackend == native_mc::PreviewGpuLodBackend::D3D11) {
                                    gpuLodD3D11Meshes.fetch_add(1, std::memory_order_relaxed);
                                }
                                if (usedGpuBackendFallback) {
                                    gpuLodBackendFallbacks.fetch_add(1, std::memory_order_relaxed);
                                }
                            } else {
                                cpuLodMeshes.fetch_add(1, std::memory_order_relaxed);
                            }
                            if (usedGpu && !lodBackend.empty()) {
                                std::lock_guard<std::mutex> statsLock(statsMutex);
                                if (activeGpuBackend == native_mc::PreviewGpuLodBackend::D3D12 &&
                                    d3d12GpuBackendName.empty()) {
                                    d3d12GpuBackendName = lodBackend;
                                } else if (activeGpuBackend == native_mc::PreviewGpuLodBackend::D3D11 &&
                                           d3d11GpuBackendName.empty()) {
                                    d3d11GpuBackendName = lodBackend;
                                }
                            }
                            if (allModels && cacheStampReady) {
                                std::string cacheError;
                                if (native_mc::SavePreviewMeshCache(
                                        previewCacheDirectory, cacheStamp, mesh, &cacheError)) {
                                    cacheWrites.fetch_add(1, std::memory_order_relaxed);
                                } else {
                                    cacheErrors.fetch_add(1, std::memory_order_relaxed);
                                }
                            }
                        }
                    } else if (!meshReady) {
                        if (!allModels) {
                            std::lock_guard<std::mutex> lock(errorMutex);
                            loadError = Utf8ToWide(error);
                            failed.store(true, std::memory_order_relaxed);
                        } else {
                            skipped.fetch_add(1, std::memory_order_relaxed);
                        }
                    }

                    if (meshReady) {
                        std::lock_guard<std::mutex> lock(combinedMutex);
                        if (!GuiAppendPreviewMesh(&combined, mesh, 0, &error, true)) {
                            std::lock_guard<std::mutex> errorLock(errorMutex);
                            loadError = Utf8ToWide(error);
                            failed.store(true, std::memory_order_relaxed);
                        } else {
                            loaded.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    } catch (const std::bad_alloc&) {
                        std::lock_guard<std::mutex> lock(errorMutex);
                        if (loadError.empty()) loadError = L"生成模型总览时内存不足，请降低 workers 后重试";
                        failed.store(true, std::memory_order_relaxed);
                    } catch (const std::exception& ex) {
                        if (!allModels) {
                            std::lock_guard<std::mutex> lock(errorMutex);
                            if (loadError.empty()) loadError = Utf8ToWide(ex.what());
                            failed.store(true, std::memory_order_relaxed);
                        } else {
                            skipped.fetch_add(1, std::memory_order_relaxed);
                        }
                    } catch (...) {
                        if (!allModels) {
                            std::lock_guard<std::mutex> lock(errorMutex);
                            if (loadError.empty()) loadError = L"预览加载线程发生未知错误";
                            failed.store(true, std::memory_order_relaxed);
                        } else {
                            skipped.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    const std::size_t done = completed.fetch_add(1, std::memory_order_relaxed) + 1;
                    auto* progress = new PreviewProgressPayload();
                    progress->completed = static_cast<int>(done);
                    progress->total = static_cast<int>(files.size());
                    progress->message = (loadedFromCache ? L"缓存命中 " : L"并行处理 ") +
                        files[i].filename().wstring() + L"  (" +
                        std::to_wstring(done) + L"/" + std::to_wstring(files.size()) + L")";
                    if (!PostMessageW(hwnd, WM_GUI_PREVIEW_PROGRESS, 0, reinterpret_cast<LPARAM>(progress))) {
                        delete progress;
                    }
                }
            };
            std::vector<std::thread> workers;
            workers.reserve(previewWorkers);
            try {
                for (int i = 0; i < previewWorkers; ++i) workers.emplace_back(loadWorker);
            } catch (...) {
                failed.store(true, std::memory_order_relaxed);
                for (auto& worker : workers) {
                    if (worker.joinable()) worker.join();
                }
                throw;
            }
            for (auto& worker : workers) worker.join();
            if (cancelRequested->load(std::memory_order_relaxed)) {
                delete payload;
                return;
            }
            if (!loadError.empty()) payload->second = loadError;
            if (loaded.load() > 0 && combined.indices.size() >= 3 && payload->second.empty()) {
                std::wostringstream info;
                info << (allModels
                         ? (allAvailableModels ? L"全部模型总览已加载: " : L"所选模型总览已加载: ")
                         : L"预览已加载: ")
                     << (allModels ? std::to_wstring(loaded.load()) + L" 个模型" : files.front().filename().wstring())
                     << L"  顶点 " << (combined.positions.size() / 3)
                     << L"  三角形 " << (combined.indices.size() / 3)
                     << L"  LOD[" << (combined.indices.size() / 3);
                for (std::size_t level = 1; level < combined.lodIndices.size(); ++level) info << L"/" << (combined.lodIndices[level].size() / 3);
                info << L"]";
                if (gpuLodMeshes.load()) {
                    info << L"  GPU Compute " << gpuLodMeshes.load();
                    info << L" (";
                    bool wroteBackend = false;
                    if (gpuLodD3D12Meshes.load()) {
                        info << Utf8ToWide(d3d12GpuBackendName.empty()
                            ? std::string("D3D12 Compute") : d3d12GpuBackendName);
                        if (gpuLodD3D11Meshes.load()) info << L" x" << gpuLodD3D12Meshes.load();
                        wroteBackend = true;
                    }
                    if (gpuLodD3D11Meshes.load()) {
                        if (wroteBackend) info << L" + ";
                        info << Utf8ToWide(d3d11GpuBackendName.empty()
                            ? std::string("D3D11 Compute") : d3d11GpuBackendName);
                        if (gpuLodD3D12Meshes.load()) info << L" x" << gpuLodD3D11Meshes.load();
                        wroteBackend = true;
                    }
                    if (!wroteBackend) info << L"未标识 GPU 后端";
                    info << L")";
                    if (gpuLodBackendFallbacks.load()) {
                        info << L"  GPU后端回退 " << gpuLodBackendFallbacks.load();
                    }
                }
                if (cpuLodMeshes.load()) info << L"  CPU回退 " << cpuLodMeshes.load();
                if (allModels) {
                    info << L"  缓存命中 " << cacheHits.load();
                    if (cacheDirectStorageHits.load()) {
                        info << L" (DirectStorage " << cacheDirectStorageHits.load() << L")";
                    } else if (cacheWin32Hits.load()) {
                        info << L" (Win32 " << cacheWin32Hits.load() << L")";
                    }
                    if (cacheWrites.load()) info << L"  新建缓存 " << cacheWrites.load();
                    if (cacheReadFallbacks.load()) info << L"  DS回退 " << cacheReadFallbacks.load();
                    if (cacheErrors.load()) info << L"  缓存降级 " << cacheErrors.load();
                }
                if (skipped.load()) info << L"  跳过 " << skipped.load() << L" 个无效模型";
                payload->first = true;
                payload->second = info.str();
                auto* readyMesh = new native_mc::PreviewMesh(std::move(combined));
                if (!PostMessageW(hwnd, WM_GUI_LOG, 1, reinterpret_cast<LPARAM>(readyMesh))) {
                    delete readyMesh;
                    payload->first = false;
                    payload->second = L"预览窗口已关闭，加载结果未提交";
                }
            } else if (payload->second.empty()) {
                payload->second = L"没有可显示的有效模型";
            }
        } catch (const std::exception& ex) {
            payload->second = Utf8ToWide(ex.what());
        } catch (...) {
            payload->second = L"预览加载失败";
        }
        if (!PostMessageW(hwnd, WM_GUI_PREVIEW_DONE, 0, reinterpret_cast<LPARAM>(payload))) {
            delete payload;
        }
    });
    } catch (const std::exception& ex) {
        state->previewLoading = false;
        GuiClosePreviewProgress(state);
        GuiSetStatus(state, L"无法启动预览线程: " + Utf8ToWide(ex.what()));
        GuiUpdatePreviewChrome(state);
    }
}

void GuiLoadPreviewAsync(GuiState* state) {
    if (!state || state->previewLoading) return;
    state->config = GuiReadConfig(state);
    SaveConfig(state->configPath, state->config);
    const fs::path source = GuiResolvePreviewSource(state->config);
    if (source.empty()) {
        GuiSetStatus(state, T(state->lang, L"没有可预览的输入路径", L"No preview source found"));
        state->previewLoaded = false;
        state->previewFrameActive.store(false, std::memory_order_release);
        GuiUpdatePreviewChrome(state);
        return;
    }
    GuiStartPreviewLoad(state, { source }, false, false);
}

void GuiLoadAllPreviewsAsync(GuiState* state) {
    if (!state || state->previewLoading) return;
    state->config = GuiReadConfig(state);
    SaveConfig(state->configPath, state->config);
    state->modelFiles = GuiCollectModelFiles(state->config);
    if (state->modelFiles.empty()) {
        GuiSetStatus(state, L"当前目录没有可显示的 OBJ 或 GLB 模型");
        return;
    }
    GuiStartPreviewLoad(state, state->modelFiles, true, true);
}

void GuiLoadSelectedPreviewsAsync(GuiState* state) {
    if (!state || state->previewLoading) return;
    const auto selectedIndices = GuiGetSelectedModelIndices(state);
    if (selectedIndices.empty()) {
        GuiSetStatus(state, L"请先在模型列表中选择一个或多个模型");
        return;
    }
    if (selectedIndices.size() == 1) {
        GuiApplySelectedModel(state, selectedIndices.front());
        return;
    }

    std::vector<fs::path> selectedFiles;
    selectedFiles.reserve(selectedIndices.size());
    for (const int index : selectedIndices) {
        if (index >= 0 && index < static_cast<int>(state->visibleModelFiles.size())) {
            selectedFiles.push_back(state->visibleModelFiles[index]);
        }
    }
    if (selectedFiles.empty()) return;

    state->config = GuiReadConfig(state);
    SaveConfig(state->configPath, state->config);
    state->modelFiles = GuiCollectModelFiles(state->config);
    std::set<std::wstring> availablePaths;
    for (const auto& file : state->modelFiles) {
        availablePaths.insert(ToLower(file.lexically_normal().wstring()));
    }
    const bool allAvailableModels = selectedFiles.size() == state->modelFiles.size() &&
        std::all_of(selectedFiles.begin(), selectedFiles.end(), [&](const fs::path& file) {
            return availablePaths.find(ToLower(file.lexically_normal().wstring())) != availablePaths.end();
        });
    if (allAvailableModels) {
        GuiStartPreviewLoad(state, state->modelFiles, true, true);
    } else {
        GuiStartPreviewLoad(state, std::move(selectedFiles), true, false);
    }
}

void GuiUpdatePageScrollBar(GuiState* state, HWND page) {
    if (!state || !page) return;
    RECT rc{};
    GetClientRect(page, &rc);
    const int visible = std::max(1, static_cast<int>(rc.bottom - rc.top));
    const int content = std::max(visible, state->pageContentHeight[page]);
    int& pos = state->pageScrollPos[page];
    pos = std::clamp(pos, 0, std::max(0, content - visible));
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = std::max(0, content - 1);
    si.nPage = visible;
    si.nPos = pos;
    SetScrollInfo(page, SB_VERT, &si, TRUE);
}

void GuiScrollPageTo(GuiState* state, HWND page, int newPos) {
    if (!state || !page) return;
    RECT rc{};
    GetClientRect(page, &rc);
    const int visible = std::max(1, static_cast<int>(rc.bottom - rc.top));
    const int content = std::max(visible, state->pageContentHeight[page]);
    int& pos = state->pageScrollPos[page];
    newPos = std::clamp(newPos, 0, std::max(0, content - visible));
    const int delta = pos - newPos;
    if (delta == 0) {
        GuiUpdatePageScrollBar(state, page);
        return;
    }
    pos = newPos;
    ScrollWindowEx(page, 0, delta, nullptr, nullptr, nullptr, nullptr, SW_INVALIDATE | SW_ERASE | SW_SCROLLCHILDREN);
    GuiUpdatePageScrollBar(state, page);
}

Config GuiReadConfig(GuiState* state) {
    Config out = state->config;
    auto readPath = [&](const std::wstring& key, fs::path& target) {
        auto it = state->edits.find(key);
        if (it != state->edits.end()) target = Trim(GuiControlText(it->second));
    };
    auto readString = [&](const std::wstring& key, std::wstring& target) {
        auto it = state->edits.find(key);
        if (it != state->edits.end()) target = GuiDecodeOptionValue(key, Trim(GuiControlText(it->second)));
    };
    auto readBool = [&](const std::wstring& key, bool& target) {
        auto it = state->checks.find(key);
        if (it != state->checks.end()) target = GuiGetCheck(it->second);
    };
    auto readInt = [&](const std::wstring& key, int& target, int minimum) {
        auto it = state->edits.find(key);
        if (it == state->edits.end()) return;
        try {
            const std::wstring text = GuiDecodeOptionValue(key, Trim(GuiControlText(it->second)));
            if (!text.empty()) target = std::max(minimum, std::stoi(text));
        } catch (...) {
        }
    };

    readPath(L"projectRoot", out.projectRoot);
    readPath(L"inputPath", out.inputPath);
    readPath(L"objDir", out.objDir);
    readPath(L"glbDir", out.glbDir);
    readPath(L"worldDir", out.worldDir);
    readPath(L"savesDir", out.savesDir);
    readPath(L"mcRoot", out.mcRoot);
    readPath(L"testWorldRoot", out.testWorldRoot);
    readPath(L"copyTo", out.copyTo);
    readString(L"worldName", out.worldName);
    readString(L"mcVersion", out.mcVersion);
    readString(L"onlyFile", out.onlyFile);
    readString(L"center", out.center);
    readString(L"centerX", out.centerX);
    readString(L"centerY", out.centerY);
    readString(L"centerZ", out.centerZ);
    readString(L"levelNormal", out.levelNormal);
    readString(L"lang", out.lang);
    readString(L"lastMode", out.lastMode);
    readString(L"rotateYDeg", out.rotateYDeg);
    readString(L"rotationMode", out.rotationMode);
    readString(L"rotationW", out.rotationW);
    readString(L"rotationX", out.rotationX);
    readString(L"rotationY", out.rotationY);
    readString(L"rotationZ", out.rotationZ);
    if (ToLower(out.rotationMode) == L"euler") out.rotateYDeg = out.rotationY;
    readString(L"baseY", out.baseY);
    readString(L"scaleBlocksPerMeter", out.scaleBlocksPerMeter);
    readString(L"fallbackBlock", out.fallbackBlock);
    readString(L"blockTypes", out.blockTypes);
    readString(L"blocks", out.blocks);
    readString(L"excludeBlockTypes", out.excludeBlockTypes);
    readString(L"excludeBlocks", out.excludeBlocks);
    readString(L"palette", out.palette);
    readString(L"transform", out.transform);
    readString(L"block", out.block);
    readString(L"clipReferenceObj", out.clipReferenceObj);
    readString(L"clipBelowMeters", out.clipBelowMeters);
    readString(L"clipCellSize", out.clipCellSize);
    readString(L"clipLowFraction", out.clipLowFraction);
    readString(L"clipPasses", out.clipPasses);
    readString(L"clipTrimSigma", out.clipTrimSigma);
    readString(L"dedupeSampleSteps", out.dedupeSampleSteps);
    readString(L"componentMinRatio", out.componentMinRatio);
    readString(L"componentBelowGap", out.componentBelowGap);
    readString(L"minY", out.minY);
    readString(L"maxY", out.maxY);
    readString(L"previewOrbitSensitivity", out.previewOrbitSensitivity);
    readString(L"previewPanSensitivity", out.previewPanSensitivity);
    readString(L"previewZoomSensitivity", out.previewZoomSensitivity);
    readString(L"previewRenderer", out.previewRenderer);
    if (out.previewRenderer.empty()) out.previewRenderer = L"auto";
    readBool(L"direct", out.direct);
    readBool(L"copyWorld", out.copyWorld);
    readBool(L"overwrite", out.overwrite);
    readBool(L"noGlass", out.noGlass);
    readBool(L"all", out.allFiles);
    readBool(L"flipX", out.flipX);
    readBool(L"flipZ", out.flipZ);
    readBool(L"noFlipZ", out.noFlipZ);
    readBool(L"noComponentFilter", out.noComponentFilter);
    readBool(L"previewShowGrid", out.previewShowGrid);
    readBool(L"previewShowAxes", out.previewShowAxes);
    readInt(L"workers", out.workers, DefaultWorkerCount());
    out.workers = std::clamp(out.workers, 1, 64);
    readInt(L"batchBlockLimit", out.batchBlockLimit, 1);
    readInt(L"previewRefreshRate", out.previewRefreshRate, 0);
    out.previewRefreshRate = NormalizePreviewRefreshRate(out.previewRefreshRate);
    if (auto it = state->edits.find(L"previewWorldChunks"); it != state->edits.end()) {
        try {
            const int chunks = std::clamp(std::stoi(Trim(GuiControlText(it->second))), 1, 1024);
            out.previewWorldRadius = chunks * 16;
        } catch (...) {
        }
    }

    out.copyWorld = !out.direct;
    out.lastSafety = out.direct ? L"direct" : L"copy-world";
    if (out.lang.empty()) out.lang = (state->lang == Lang::Zh) ? L"zh" : L"en";
    return out;
}

bool GuiWorldReady(const GuiState* state, const Config& config, fs::path* resolvedWorld = nullptr) {
    if (!state) return false;
    ParsedArgs args;
    fs::path world = config.worldDir;
    if (world.empty()) world = ResolveWorldDir(state->backend, config, args);
    std::error_code ec;
    const bool ready = !world.empty() && fs::is_directory(world, ec) && fs::exists(world / "level.dat", ec);
    if (ready && resolvedWorld) *resolvedWorld = world;
    return ready;
}

void GuiUpdateWorkflow(GuiState* state) {
    if (!state) return;
    const Config current = GuiReadConfig(state);
    const fs::path source = GuiResolvePreviewSource(current);
    std::error_code ec;
    const bool sourceReady = !source.empty() && fs::is_regular_file(source, ec);
    fs::path world;
    const bool worldReady = GuiWorldReady(state, current, &world);

    for (HWND button : state->actionButtons) EnableWindow(button, !state->running);

    const std::wstring ext = ToLower(source.extension().wstring());
    EnableWindow(GetDlgItem(state->hwnd, IDC_GUI_ACTION_PREVIEW_LOAD), sourceReady && !state->running && !state->previewLoading);
    EnableWindow(GetDlgItem(state->hwnd, IDC_GUI_ACTION_COPY_WORLD), worldReady && !state->running);
    EnableWindow(GetDlgItem(state->hwnd, IDC_GUI_ACTION_GLB), sourceReady && worldReady && ext == L".glb" && !state->running);
    EnableWindow(GetDlgItem(state->hwnd, IDC_GUI_ACTION_OBJ), sourceReady && worldReady && ext == L".obj" && !state->running);
    EnableWindow(GetDlgItem(state->hwnd, IDC_GUI_ACTION_SCAN), worldReady && !state->running);
    EnableWindow(GetDlgItem(state->hwnd, IDC_GUI_ACTION_RESET), worldReady && !state->running);
    EnableWindow(GetDlgItem(state->hwnd, IDC_GUI_ACTION_BREAKDOWN), worldReady && !state->running);
    if (state->pages.size() > 5) {
        EnableWindow(GetDlgItem(state->pages[5], IDC_GUI_MODEL_SHOW_ALL), !state->modelFiles.empty() && !state->previewLoading);
        EnableWindow(GetDlgItem(state->pages[5], IDC_GUI_MODEL_LEVEL_REFERENCE), !state->visibleModelFiles.empty() && !state->previewLoading);
    }
    if (state->previewLevelButton) EnableWindow(state->previewLevelButton, !state->visibleModelFiles.empty() && !state->previewLoading);
}

void GuiTryAutoPreview(GuiState* state, bool worldChanged) {
    if (!state) return;
    state->config = GuiReadConfig(state);
    const fs::path source = GuiResolvePreviewSource(state->config);
    std::error_code ec;
    const bool sourceReady = !source.empty() && fs::is_regular_file(source, ec);
    const bool worldReady = GuiWorldReady(state, state->config);

    if (sourceReady && !state->previewLoading &&
        (!state->previewLoaded || NormalizeIfExists(source) != NormalizeIfExists(state->previewSource))) {
        GuiRefreshModelList(state);
        GuiLoadPreviewAsync(state);
    }
    if (worldChanged && worldReady && state->previewWorldVisible) {
        GuiScheduleWorldPreviewRefresh(state);
    }
    if (sourceReady && worldReady) {
        GuiSetStatus(state, state->previewLoading ? L"模型与存档已选择，正在自动加载预览..." : L"模型与存档已选择");
    } else if (!sourceReady) {
        GuiSetStatus(state, L"请选择 OBJ / GLB 模型文件或模型目录");
    } else {
        GuiSetStatus(state, L"模型已选择，请选择包含 level.dat 的 Minecraft 存档");
    }
    GuiUpdateWorkflow(state);
}

bool GuiConfirmCommand(GuiState* state, const std::wstring& command) {
    if (!state) return false;
    const Config current = GuiReadConfig(state);
    if (command == L"reset") {
        return MessageBoxW(state->hwnd,
            L"这会清空所选存档中的区块数据。建议只对测试副本执行。\n\n确定继续吗？",
            L"确认重置存档", MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) == IDYES;
    }
    if (command == L"glb" || command == L"glb-surface" || command == L"obj" || command == L"obj-surface") {
        const fs::path source = GuiResolvePreviewSource(current);
        std::size_t modelCount = 1;
        if (current.allFiles) modelCount = GuiCollectModelFiles(current).size();
        if (!current.allFiles && current.onlyFile.empty() && HasDir(current.inputPath)) {
            MessageBoxW(state->hwnd, L"目录输入需要先在“模型列表”选择当前模型，或勾选“处理目录内全部模型”。",
                L"请选择导入范围", MB_OK | MB_ICONWARNING);
            return false;
        }
        std::wostringstream plan;
        plan << L"模型: " << (current.allFiles ? L"全部 " + std::to_wstring(modelCount) + L" 个" : source.filename().wstring())
             << L"\n模式: " << current.lastMode
             << L"\n源存档: " << current.worldDir.wstring()
             << L"\n写入方式: " << (current.direct ? L"直接写入所选存档" : L"复制到测试副本后写入")
             << L"\n测试副本根目录: " << current.testWorldRoot.wstring()
             << L"\n中心: " << (current.center.empty() ? L"自动" : current.center)
             << L"\n旋转: " << ConfigRotationSummary(current)
             << L"\n缩放: " << (current.scaleBlocksPerMeter.empty() ? L"4" : current.scaleBlocksPerMeter) << L" block/m"
             << L"\n镜像 X/Z: " << (current.flipX ? L"开" : L"关") << L" / " << (current.flipZ ? L"开" : L"关")
             << L"\n覆盖语义: " << (current.overwrite ? L"替换触及坐标的已有方块" : L"只填充空气")
             << L"\n\n确认执行这个计划吗？";
        return MessageBoxW(state->hwnd, plan.str().c_str(), L"确认完整导入计划",
            MB_YESNO | MB_DEFBUTTON2 | (current.direct ? MB_ICONWARNING : MB_ICONINFORMATION)) == IDYES;
    }
    return true;
}

void GuiPopulateControls(GuiState* state, const Config& config) {
    if (!state) return;
    state->populatingControls = true;
    auto setPath = [&](const std::wstring& key, const fs::path& value) {
        auto it = state->edits.find(key);
        if (it != state->edits.end()) GuiSetControlText(it->second, value.empty() ? L"" : value.wstring());
    };
    auto setString = [&](const std::wstring& key, const std::wstring& value) {
        auto it = state->edits.find(key);
        if (it != state->edits.end()) GuiSetControlText(it->second, GuiEncodeOptionValue(key, value));
    };
    auto setBool = [&](const std::wstring& key, bool value) {
        auto it = state->checks.find(key);
        if (it != state->checks.end()) GuiSetCheck(it->second, value);
    };
    auto setInt = [&](const std::wstring& key, int value) {
        auto it = state->edits.find(key);
        if (it != state->edits.end()) {
            GuiSetControlText(it->second, GuiEncodeOptionValue(key, std::to_wstring(value)));
        }
    };

    setPath(L"projectRoot", config.projectRoot);
    setPath(L"inputPath", config.inputPath);
    setPath(L"objDir", config.objDir);
    setPath(L"glbDir", config.glbDir);
    setPath(L"worldDir", config.worldDir);
    setPath(L"savesDir", config.savesDir);
    setPath(L"mcRoot", config.mcRoot);
    setPath(L"testWorldRoot", config.testWorldRoot);
    setPath(L"copyTo", config.copyTo);
    setString(L"worldName", config.worldName);
    setString(L"mcVersion", config.mcVersion);
    setString(L"onlyFile", config.onlyFile);
    setString(L"center", config.center);
    setString(L"centerX", config.centerX);
    setString(L"centerY", config.centerY);
    setString(L"centerZ", config.centerZ);
    setString(L"levelNormal", config.levelNormal);
    setString(L"lang", config.lang);
    setString(L"lastMode", config.lastMode);
    setString(L"rotateYDeg", config.rotateYDeg);
    setString(L"rotationMode", config.rotationMode);
    setString(L"rotationW", config.rotationW);
    setString(L"rotationX", config.rotationX);
    setString(L"rotationY", config.rotationY.empty() ? config.rotateYDeg : config.rotationY);
    setString(L"rotationZ", config.rotationZ);
    setString(L"baseY", config.baseY);
    setString(L"scaleBlocksPerMeter", config.scaleBlocksPerMeter);
    setString(L"fallbackBlock", config.fallbackBlock);
    setString(L"blockTypes", config.blockTypes);
    setString(L"blocks", config.blocks);
    setString(L"excludeBlockTypes", config.excludeBlockTypes);
    setString(L"excludeBlocks", config.excludeBlocks);
    setString(L"palette", config.palette);
    setString(L"transform", config.transform.empty() && IsObjMode(ToLower(config.lastMode)) ? L"obj-z-up" : config.transform);
    setString(L"block", config.block);
    setString(L"clipReferenceObj", config.clipReferenceObj);
    setString(L"clipBelowMeters", config.clipBelowMeters);
    setString(L"clipCellSize", config.clipCellSize);
    setString(L"clipLowFraction", config.clipLowFraction);
    setString(L"clipPasses", config.clipPasses);
    setString(L"clipTrimSigma", config.clipTrimSigma);
    setString(L"dedupeSampleSteps", config.dedupeSampleSteps);
    setString(L"componentMinRatio", config.componentMinRatio);
    setString(L"componentBelowGap", config.componentBelowGap);
    setString(L"minY", config.minY);
    setString(L"maxY", config.maxY);
    setString(L"previewOrbitSensitivity", config.previewOrbitSensitivity);
    setString(L"previewPanSensitivity", config.previewPanSensitivity);
    setString(L"previewZoomSensitivity", config.previewZoomSensitivity);
    setString(L"previewRenderer", config.previewRenderer.empty() ? L"auto" : config.previewRenderer);
    setBool(L"direct", config.direct);
    setBool(L"copyWorld", !config.direct);
    setBool(L"overwrite", config.overwrite);
    setBool(L"noGlass", config.noGlass);
    setBool(L"all", config.allFiles);
    setBool(L"flipX", config.flipX);
    setBool(L"flipZ", config.flipZ);
    setBool(L"noFlipZ", config.noFlipZ);
    setBool(L"noComponentFilter", config.noComponentFilter);
    setBool(L"previewShowGrid", config.previewShowGrid);
    setBool(L"previewShowAxes", config.previewShowAxes);
    setInt(L"workers", std::clamp(config.workers, 1, 64));
    setInt(L"batchBlockLimit", config.batchBlockLimit);
    setInt(L"previewRefreshRate", NormalizePreviewRefreshRate(config.previewRefreshRate));
    state->previewRefreshHz.store(
        NormalizePreviewRefreshRate(config.previewRefreshRate), std::memory_order_relaxed);
    setInt(L"previewWorldChunks", std::clamp(std::max(1, static_cast<int>(std::ceil(config.previewWorldRadius / 16.0f))), 1, 1024));
    state->previewWorldVisible = config.previewWorldVisible;
    state->previewShowGrid = config.previewShowGrid;
    state->previewShowAxes = config.previewShowAxes;
    state->previewWorldRadius = std::clamp(config.previewWorldRadius > 0 ? config.previewWorldRadius : 48, 16, 1024 * 16);
    state->previewWorldMaxBlocks = config.previewWorldMaxBlocks > 0 ? config.previewWorldMaxBlocks : 500000;
    const std::wstring worldStyle = ToLower(config.previewWorldStyle);
    if (worldStyle == L"points") state->previewWorldStyle = PreviewWorldStyle::Points;
    else if (worldStyle == L"voxels") state->previewWorldStyle = PreviewWorldStyle::Voxels;
    else state->previewWorldStyle = PreviewWorldStyle::Faces;
    GuiLoadPreviewPresetsFromConfig(state, config);
    state->populatingControls = false;
    GuiRefreshModelList(state);
    GuiSyncPreviewTransformInspector(state);
    GuiUpdatePreviewChrome(state);
    GuiUpdateWorkflow(state);
}

std::vector<std::wstring> GuiBuildArgs(const Config& config, const std::wstring& command) {
    std::vector<std::wstring> out;
    out.push_back(command);
    auto addValue = [&](const std::wstring& flag, const std::wstring& value) {
        if (!value.empty()) {
            out.push_back(flag);
            out.push_back(value);
        }
    };
    auto addPath = [&](const std::wstring& flag, const fs::path& value) {
        if (!value.empty()) addValue(flag, value.wstring());
    };
    auto addFlag = [&](const std::wstring& flag, bool value) {
        if (value) out.push_back(flag);
    };

    addPath(L"--project-root", config.projectRoot);
    addPath(L"--input", config.inputPath);
    addPath(L"--obj-dir", config.objDir);
    addPath(L"--glb-dir", config.glbDir);
    addPath(L"--world-dir", config.worldDir);
    addPath(L"--saves-dir", config.savesDir);
    addPath(L"--mc-root", config.mcRoot);
    addPath(L"--test-world-root", config.testWorldRoot);
    addPath(L"--copy-to", config.copyTo);
    addValue(L"--world", config.worldName);
    addValue(L"--mc-version", config.mcVersion);
    if (config.allFiles) addFlag(L"--all", true);
    else addValue(L"--only", config.onlyFile);
    addValue(L"--center", config.center);
    addValue(L"--center-x", config.centerX);
    addValue(L"--center-y", config.centerY);
    addValue(L"--center-z", config.centerZ);
    addValue(L"--level-normal", config.levelNormal);
    addValue(L"--lang", config.lang);
    addValue(L"--rotate-y-deg", config.rotateYDeg);
    addValue(L"--base-y", config.baseY);
    addValue(L"--scale-blocks-per-meter", config.scaleBlocksPerMeter);
    addValue(L"--fallback-block", config.fallbackBlock);
    addValue(L"--block-types", config.blockTypes);
    addValue(L"--blocks", config.blocks);
    addValue(L"--exclude-block-types", config.excludeBlockTypes);
    addValue(L"--exclude-blocks", config.excludeBlocks);
    addValue(L"--palette", config.palette);
    if (command.rfind(L"obj", 0) == 0) {
        addValue(L"--transform", config.transform.empty() ? L"obj-z-up" : config.transform);
    } else {
        addValue(L"--transform", config.transform);
    }
    addValue(L"--block", config.block);
    addValue(L"--clip-reference-obj", config.clipReferenceObj);
    addValue(L"--clip-below-meters", config.clipBelowMeters);
    addValue(L"--clip-cell-size", config.clipCellSize);
    addValue(L"--clip-low-fraction", config.clipLowFraction);
    addValue(L"--clip-passes", config.clipPasses);
    addValue(L"--clip-trim-sigma", config.clipTrimSigma);
    addValue(L"--dedupe-sample-steps", config.dedupeSampleSteps);
    addValue(L"--component-min-ratio", config.componentMinRatio);
    addValue(L"--component-below-gap", config.componentBelowGap);
    addValue(L"--min-y", config.minY);
    addValue(L"--max-y", config.maxY);
    if ((command == L"obj" || command == L"obj-surface" || command == L"obj-copy") && config.workers > 0) {
        addValue(L"--workers", std::to_wstring(std::clamp(config.workers, 1, 64)));
    }
    if (config.batchBlockLimit > 0) addValue(L"--batch-block-limit", std::to_wstring(config.batchBlockLimit));
    addFlag(L"--overwrite", config.overwrite);
    addFlag(L"--no-glass", config.noGlass);
    addFlag(L"--flip-x", config.flipX);
    addFlag(L"--flip-z", config.flipZ);
    addFlag(L"--no-flip-z", config.noFlipZ);
    addFlag(L"--no-component-filter", config.noComponentFilter);

    if (command == L"glb" || command == L"glb-surface" || command == L"obj" || command == L"obj-surface" ||
        command == L"copy-world" || command == L"reset") {
        out.push_back(L"--yes");
    }
    if (command == L"glb-copy" || command == L"obj-copy") {
        addFlag(L"--copy-world", true);
    } else if (command == L"glb" || command == L"glb-surface" || command == L"obj" || command == L"obj-surface") {
        if (config.direct) addFlag(L"--direct", true);
        else if (config.copyWorld) addFlag(L"--copy-world", true);
    }
    return out;
}

native_mc::PaletteOptions GuiMakePaletteOptions(const Config& config) {
    native_mc::PaletteOptions options;
    options.mode = WideToUtf8(config.palette.empty() ? L"scene" : config.palette);
    options.blockTypes = WideToUtf8(config.blockTypes);
    options.blocks = WideToUtf8(config.blocks);
    options.excludeBlockTypes = WideToUtf8(config.excludeBlockTypes);
    options.excludeBlocks = WideToUtf8(config.excludeBlocks);
    options.noGlass = config.noGlass;
    return options;
}

std::set<std::wstring> WideNameSet(const std::vector<native_mc::PaletteBlock>& blocks) {
    std::set<std::wstring> names;
    for (const auto& block : blocks) {
        names.insert(Utf8ToWide(block.name));
    }
    return names;
}

std::vector<std::wstring> SortedWideNames(const std::set<std::wstring>& values) {
    return std::vector<std::wstring>(values.begin(), values.end());
}

bool WideContainsFilter(const std::wstring& value, const std::wstring& filter) {
    if (filter.empty()) return true;
    return ToLower(value).find(ToLower(filter)) != std::wstring::npos;
}

std::vector<std::wstring> ListBoxSelectedValues(HWND list, const std::vector<std::wstring>& visibleValues) {
    std::vector<std::wstring> selected;
    if (!list) return selected;
    const int count = static_cast<int>(SendMessageW(list, LB_GETSELCOUNT, 0, 0));
    if (count <= 0) return selected;
    std::vector<int> indices(count);
    if (SendMessageW(list, LB_GETSELITEMS, count, reinterpret_cast<LPARAM>(indices.data())) == LB_ERR) {
        return selected;
    }
    for (int index : indices) {
        if (index >= 0 && index < static_cast<int>(visibleValues.size())) {
            selected.push_back(visibleValues[index]);
        }
    }
    return selected;
}

void PaletteDialogRefreshLists(PaletteDialogState* state) {
    if (!state) return;
    const std::wstring includedFilter = state->includedSearch ? Trim(GuiControlText(state->includedSearch)) : L"";
    const std::wstring excludedFilter = state->excludedSearch ? Trim(GuiControlText(state->excludedSearch)) : L"";

    state->visibleIncluded.clear();
    for (const auto& name : SortedWideNames(state->includedBlocks)) {
        if (WideContainsFilter(name, includedFilter)) {
            state->visibleIncluded.push_back(name);
        }
    }

    state->visibleExcluded.clear();
    for (const auto& name : state->allBlocks) {
        if (state->includedBlocks.find(name) != state->includedBlocks.end()) continue;
        if (WideContainsFilter(name, excludedFilter)) {
            state->visibleExcluded.push_back(name);
        }
    }

    SendMessageW(state->includedList, LB_RESETCONTENT, 0, 0);
    for (const auto& name : state->visibleIncluded) {
        SendMessageW(state->includedList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
    }

    SendMessageW(state->excludedList, LB_RESETCONTENT, 0, 0);
    for (const auto& name : state->visibleExcluded) {
        SendMessageW(state->excludedList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
    }

    if (state->summary) {
        const std::wstring text = L"当前启用 " + FormatNumber(state->includedBlocks.size()) +
            L" 个，候选总计 " + FormatNumber(state->allBlocks.size()) + L" 个";
        SetWindowTextW(state->summary, text.c_str());
    }
}

void PaletteDialogMoveSelection(PaletteDialogState* state, bool includeFromExcluded) {
    if (!state) return;
    const auto values = includeFromExcluded
        ? ListBoxSelectedValues(state->excludedList, state->visibleExcluded)
        : ListBoxSelectedValues(state->includedList, state->visibleIncluded);
    if (values.empty()) return;
    for (const auto& value : values) {
        if (includeFromExcluded) state->includedBlocks.insert(value);
        else state->includedBlocks.erase(value);
    }
    PaletteDialogRefreshLists(state);
}

void PaletteDialogApply(PaletteDialogState* state) {
    if (!state || !state->owner) return;
    Config current = GuiReadConfig(state->owner);

    std::vector<std::wstring> includeBlocks;
    for (const auto& name : state->includedBlocks) {
        if (state->baseBlocks.find(name) == state->baseBlocks.end()) {
            includeBlocks.push_back(name);
        }
    }

    std::vector<std::wstring> excludeBlocks;
    for (const auto& name : state->baseBlocks) {
        if (state->includedBlocks.find(name) == state->includedBlocks.end()) {
            excludeBlocks.push_back(name);
        }
    }

    current.blocks = JoinCsvWide(includeBlocks);
    current.excludeBlocks = JoinCsvWide(excludeBlocks);
    state->owner->config = current;
    GuiPopulateControls(state->owner, current);
    SaveConfig(state->owner->configPath, current);
    GuiSetStatus(state->owner, L"方块调色板已更新");
}

void GuiOpenPaletteDialog(GuiState* state) {
    if (!state) return;
    if (state->paletteDialog && IsWindow(state->paletteDialog)) {
        ShowWindow(state->paletteDialog, SW_SHOWNORMAL);
        SetForegroundWindow(state->paletteDialog);
        return;
    }

    Config current = GuiReadConfig(state);
    const fs::path resourceRoot = !state->backend.root.empty() ? state->backend.root : current.projectRoot;
    if (resourceRoot.empty()) {
        MessageBoxW(state->hwnd, L"找不到资源根目录，无法加载方块调色板。", L"3dmodel-to-minecraft", MB_OK | MB_ICONWARNING);
        return;
    }

    native_mc::PaletteCatalog catalog;
    std::string error;
    if (!native_mc::LoadPaletteCatalog(resourceRoot, GuiMakePaletteOptions(current), &catalog, &error)) {
        MessageBoxW(state->hwnd, Utf8ToWide(error).c_str(), L"3dmodel-to-minecraft", MB_OK | MB_ICONERROR);
        return;
    }

    auto* dialogState = new PaletteDialogState();
    dialogState->owner = state;
    dialogState->font = state->font;
    dialogState->allBlocks = SortedWideNames(WideNameSet(catalog.availableBlocks));
    dialogState->baseBlocks = WideNameSet(catalog.baseBlocks);
    dialogState->includedBlocks = WideNameSet(catalog.includedBlocks);

    state->paletteDialog = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        L"MCPaletteDialogWindow",
        L"方块调色板",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 860, 700,
        state->hwnd, nullptr, GetModuleHandleW(nullptr), dialogState);
    if (!state->paletteDialog) {
        delete dialogState;
        state->paletteDialog = nullptr;
    }
}

void BreakdownDialogRefreshList(BreakdownDialogState* state) {
    if (!state || !state->list) return;
    const std::wstring filter = state->search ? Trim(GuiControlText(state->search)) : L"";
    state->visibleRows.clear();
    for (const auto& row : state->rows) {
        if (WideContainsFilter(row.first, filter)) {
            state->visibleRows.push_back(row);
        }
    }

    ListView_DeleteAllItems(state->list);
    int index = 0;
    for (const auto& [name, count] : state->visibleRows) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = index;
        item.pszText = const_cast<wchar_t*>(name.c_str());
        ListView_InsertItem(state->list, &item);
        std::wstring countText = FormatNumber(count);
        ListView_SetItemText(state->list, index, 1, countText.data());
        ++index;
    }
}

void BreakdownDialogStartScan(BreakdownDialogState* state) {
    if (!state || !state->owner || state->loading) return;
    Config current = GuiReadConfig(state->owner);
    ParsedArgs args;
    const fs::path worldDir = GuiWorldHasRegionFiles(current.worldDir)
        ? NormalizeIfExists(current.worldDir)
        : ResolveWorldDir(state->owner->backend, current, args);
    if (worldDir.empty() || !GuiWorldHasRegionFiles(worldDir)) {
        state->rows.clear();
        BreakdownDialogRefreshList(state);
        SetWindowTextW(state->summary, L"找不到可扫描的世界目录");
        return;
    }

    const auto minY = ParseOptionalIntText(current.minY);
    const auto maxY = ParseOptionalIntText(current.maxY);
    state->loading = true;
    SetWindowTextW(state->summary, (L"正在扫描: " + worldDir.wstring()).c_str());
    HWND hwnd = state->hwnd;

    std::thread([hwnd, worldDir, minY, maxY]() {
        auto* payload = new BlockBreakdownPayload();
        std::string error;
        payload->ok = native_mc::ScanWorldBlocks(worldDir, minY, maxY, &payload->summary, &error, nullptr);
        if (payload->ok) {
            payload->message = L"扫描完成: " + worldDir.wstring();
        } else {
            payload->message = Utf8ToWide(error.empty() ? std::string("扫描失败") : error);
        }
        PostMessageW(hwnd, WM_GUI_BREAKDOWN_READY, 0, reinterpret_cast<LPARAM>(payload));
    }).detach();
}

void GuiOpenBreakdownDialog(GuiState* state) {
    if (!state) return;
    if (state->breakdownDialog && IsWindow(state->breakdownDialog)) {
        ShowWindow(state->breakdownDialog, SW_SHOWNORMAL);
        SetForegroundWindow(state->breakdownDialog);
        BreakdownDialogStartScan(reinterpret_cast<BreakdownDialogState*>(GetWindowLongPtrW(state->breakdownDialog, GWLP_USERDATA)));
        return;
    }

    auto* dialogState = new BreakdownDialogState();
    dialogState->owner = state;
    dialogState->font = state->font;
    state->breakdownDialog = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        L"MCBreakdownDialogWindow",
        L"方块统计",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 760, 720,
        state->hwnd, nullptr, GetModuleHandleW(nullptr), dialogState);
    if (!state->breakdownDialog) {
        delete dialogState;
        state->breakdownDialog = nullptr;
    }
}

std::wstring GuiDecodeOutput(const std::string& bytes) {
    return Utf8ToWide(bytes);
}

void GuiRunCommandAsync(GuiState* state, const std::wstring& command) {
    if (!state || state->running) return;
    state->config = GuiReadConfig(state);
    SaveConfig(state->configPath, state->config);
    std::wstring actionLabel = command;
    if (command == L"glb" || command == L"glb-surface") actionLabel = L"导入 GLB";
    else if (command == L"obj" || command == L"obj-surface") actionLabel = L"导入 OBJ";
    else if (command == L"copy-world") actionLabel = L"复制测试存档";
    else if (command == L"scan") actionLabel = L"扫描存档";
    else if (command == L"reset") actionLabel = L"重置存档";
    else if (command == L"doctor") actionLabel = L"环境诊断";
    GuiSetStatus(state, state->lang == Lang::Zh
        ? (L"正在执行：" + actionLabel + L"...")
        : (L"Running: " + command + L"..."));
    GuiSetControlText(state->log, L"");
    state->running = true;
    GuiUpdateWorkflow(state);

    const std::vector<std::wstring> args = GuiBuildArgs(state->config, command);
    const fs::path exePath = state->exePath;
    HWND hwnd = state->hwnd;

    std::thread([hwnd, exePath, args]() {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        HANDLE readPipe = nullptr;
        HANDLE writePipe = nullptr;
        std::string output;
        DWORD exitCode = 1;

        if (CreatePipe(&readPipe, &writePipe, &sa, 0)) {
            SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

            std::wstring cmdline = QuoteForWindows(exePath.wstring());
            for (const auto& arg : args) {
                cmdline += L" ";
                cmdline += QuoteForWindows(arg);
            }
            std::vector<wchar_t> buffer(cmdline.begin(), cmdline.end());
            buffer.push_back(L'\0');

            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            si.hStdOutput = writePipe;
            si.hStdError = writePipe;
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(exePath.wstring().c_str(), buffer.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, exePath.parent_path().wstring().c_str(), &si, &pi)) {
                CloseHandle(writePipe);
                writePipe = nullptr;
                char chunk[4096];
                DWORD read = 0;
                while (ReadFile(readPipe, chunk, sizeof(chunk), &read, nullptr) && read > 0) {
                    output.append(chunk, chunk + read);
                }
                WaitForSingleObject(pi.hProcess, INFINITE);
                GetExitCodeProcess(pi.hProcess, &exitCode);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
            }
        }

        if (writePipe) CloseHandle(writePipe);
        if (readPipe) CloseHandle(readPipe);

        auto* text = new std::wstring(GuiDecodeOutput(output));
        PostMessageW(hwnd, WM_GUI_LOG, 0, reinterpret_cast<LPARAM>(text));
        PostMessageW(hwnd, WM_GUI_DONE, exitCode, 0);
    }).detach();
}

LRESULT CALLBACK GuiPaletteDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<PaletteDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = reinterpret_cast<PaletteDialogState*>(cs->lpCreateParams);
        state->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

        auto createStatic = [&](const std::wstring& text, int id) {
            HWND h = CreateWindowExW(0, L"STATIC", text.c_str(), WS_CHILD | WS_VISIBLE, 0, 0, 100, 20, hwnd, (HMENU)(INT_PTR)id, nullptr, nullptr);
            SendMessageW(h, WM_SETFONT, (WPARAM)state->font, TRUE);
            return h;
        };
        auto createEdit = [&](int id) {
            HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 100, 22, hwnd, (HMENU)(INT_PTR)id, nullptr, nullptr);
            SendMessageW(h, WM_SETFONT, (WPARAM)state->font, TRUE);
            return h;
        };
        auto createButton = [&](const std::wstring& text, int id) {
            HWND h = CreateWindowExW(0, L"BUTTON", text.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 100, 26, hwnd, (HMENU)(INT_PTR)id, nullptr, nullptr);
            SendMessageW(h, WM_SETFONT, (WPARAM)state->font, TRUE);
            return h;
        };
        auto createList = [&](int id) {
            HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | LBS_EXTENDEDSEL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, 0, 0, 100, 100, hwnd, (HMENU)(INT_PTR)id, nullptr, nullptr);
            SendMessageW(h, WM_SETFONT, (WPARAM)state->font, TRUE);
            return h;
        };

        state->summary = createStatic(L"", IDC_PALETTE_SUMMARY);
        createStatic(L"已启用方块", -1);
        createStatic(L"未启用方块", -1);
        state->includedSearch = createEdit(IDC_PALETTE_INCLUDED_SEARCH);
        state->excludedSearch = createEdit(IDC_PALETTE_EXCLUDED_SEARCH);
        state->includedList = createList(IDC_PALETTE_INCLUDED_LIST);
        state->excludedList = createList(IDC_PALETTE_EXCLUDED_LIST);
        createButton(L"移到未启用", IDC_PALETTE_EXCLUDE);
        createButton(L"加入已启用", IDC_PALETTE_INCLUDE);
        createButton(L"应用", IDC_PALETTE_APPLY);
        createButton(L"取消", IDC_PALETTE_CANCEL);
        PaletteDialogRefreshLists(state);
        return 0;
    }
    case WM_SIZE:
        if (state) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            const int margin = 12;
            const int gap = 10;
            const int summaryH = 22;
            const int labelH = 20;
            const int searchH = 24;
            const int buttonH = 28;
            const int bottomH = 34;
            const int usableW = std::max(100L, rc.right - rc.left - margin * 2);
            const int columnW = (usableW - gap) / 2;
            const int listTop = margin + summaryH + gap + labelH + gap + searchH + gap;
            const int listBottom = rc.bottom - margin - bottomH - gap - buttonH - gap;
            const int listH = std::max(120, listBottom - listTop);

            const HWND summary = GetDlgItem(hwnd, IDC_PALETTE_SUMMARY);
            const HWND includedLabel = GetWindow(summary, GW_HWNDNEXT);
            const HWND excludedLabel = GetWindow(includedLabel, GW_HWNDNEXT);
            const HWND excludeButton = GetDlgItem(hwnd, IDC_PALETTE_EXCLUDE);
            const HWND includeButton = GetDlgItem(hwnd, IDC_PALETTE_INCLUDE);
            const HWND applyButton = GetDlgItem(hwnd, IDC_PALETTE_APPLY);
            const HWND cancelButton = GetDlgItem(hwnd, IDC_PALETTE_CANCEL);

            MoveWindow(summary, margin, margin, usableW, summaryH, TRUE);
            MoveWindow(includedLabel, margin, margin + summaryH + gap, columnW, labelH, TRUE);
            MoveWindow(excludedLabel, margin + columnW + gap, margin + summaryH + gap, columnW, labelH, TRUE);
            MoveWindow(state->includedSearch, margin, margin + summaryH + gap + labelH + gap, columnW, searchH, TRUE);
            MoveWindow(state->excludedSearch, margin + columnW + gap, margin + summaryH + gap + labelH + gap, columnW, searchH, TRUE);
            MoveWindow(state->includedList, margin, listTop, columnW, listH, TRUE);
            MoveWindow(state->excludedList, margin + columnW + gap, listTop, columnW, listH, TRUE);
            MoveWindow(excludeButton, margin, listTop + listH + gap, columnW, buttonH, TRUE);
            MoveWindow(includeButton, margin + columnW + gap, listTop + listH + gap, columnW, buttonH, TRUE);
            MoveWindow(cancelButton, rc.right - margin - 110, rc.bottom - margin - bottomH, 110, bottomH, TRUE);
            MoveWindow(applyButton, rc.right - margin - 230, rc.bottom - margin - bottomH, 110, bottomH, TRUE);
            return 0;
        }
        break;
    case WM_COMMAND:
        if (!state) break;
        switch (LOWORD(wParam)) {
        case IDC_PALETTE_INCLUDED_SEARCH:
        case IDC_PALETTE_EXCLUDED_SEARCH:
            if (HIWORD(wParam) == EN_CHANGE) {
                PaletteDialogRefreshLists(state);
                return 0;
            }
            break;
        case IDC_PALETTE_EXCLUDE:
            PaletteDialogMoveSelection(state, false);
            return 0;
        case IDC_PALETTE_INCLUDE:
            PaletteDialogMoveSelection(state, true);
            return 0;
        case IDC_PALETTE_APPLY:
            PaletteDialogApply(state);
            DestroyWindow(hwnd);
            return 0;
        case IDC_PALETTE_CANCEL:
            DestroyWindow(hwnd);
            return 0;
        case IDC_PALETTE_INCLUDED_LIST:
            if (HIWORD(wParam) == LBN_DBLCLK) {
                PaletteDialogMoveSelection(state, false);
                return 0;
            }
            break;
        case IDC_PALETTE_EXCLUDED_LIST:
            if (HIWORD(wParam) == LBN_DBLCLK) {
                PaletteDialogMoveSelection(state, true);
                return 0;
            }
            break;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (state) {
            if (state->owner) state->owner->paletteDialog = nullptr;
            delete state;
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK GuiBreakdownDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<BreakdownDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = reinterpret_cast<BreakdownDialogState*>(cs->lpCreateParams);
        state->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

        state->summary = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 100, 20, hwnd, (HMENU)(INT_PTR)IDC_BREAKDOWN_SUMMARY, nullptr, nullptr);
        state->search = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 100, 22, hwnd, (HMENU)(INT_PTR)IDC_BREAKDOWN_SEARCH, nullptr, nullptr);
        state->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS, 0, 0, 100, 100, hwnd, (HMENU)(INT_PTR)IDC_BREAKDOWN_LIST, nullptr, nullptr);
        HWND refreshButton = CreateWindowExW(0, L"BUTTON", L"刷新", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 100, 26, hwnd, (HMENU)(INT_PTR)IDC_BREAKDOWN_REFRESH, nullptr, nullptr);
        HWND closeButton = CreateWindowExW(0, L"BUTTON", L"关闭", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 100, 26, hwnd, (HMENU)(INT_PTR)IDC_BREAKDOWN_CLOSE, nullptr, nullptr);

        SendMessageW(state->summary, WM_SETFONT, (WPARAM)state->font, TRUE);
        SendMessageW(state->search, WM_SETFONT, (WPARAM)state->font, TRUE);
        SendMessageW(refreshButton, WM_SETFONT, (WPARAM)state->font, TRUE);
        SendMessageW(closeButton, WM_SETFONT, (WPARAM)state->font, TRUE);
        ListView_SetExtendedListViewStyle(state->list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);

        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = const_cast<wchar_t*>(L"方块");
        col.cx = 420;
        ListView_InsertColumn(state->list, 0, &col);
        col.pszText = const_cast<wchar_t*>(L"数量");
        col.cx = 140;
        ListView_InsertColumn(state->list, 1, &col);

        BreakdownDialogStartScan(state);
        return 0;
    }
    case WM_SIZE:
        if (state) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            const int margin = 12;
            const int gap = 10;
            const int topH = 22;
            const int searchH = 24;
            const int buttonH = 30;
            const int bottomY = rc.bottom - margin - buttonH;
            MoveWindow(state->summary, margin, margin, rc.right - margin * 2, topH, TRUE);
            MoveWindow(state->search, margin, margin + topH + gap, rc.right - margin * 2 - 110, searchH, TRUE);
            MoveWindow(GetDlgItem(hwnd, IDC_BREAKDOWN_REFRESH), rc.right - margin - 100, margin + topH + gap, 100, searchH, TRUE);
            MoveWindow(state->list, margin, margin + topH + gap + searchH + gap, rc.right - margin * 2, bottomY - (margin + topH + gap + searchH + gap), TRUE);
            MoveWindow(GetDlgItem(hwnd, IDC_BREAKDOWN_CLOSE), rc.right - margin - 100, bottomY, 100, buttonH, TRUE);
            RECT listRc{};
            GetClientRect(state->list, &listRc);
            ListView_SetColumnWidth(state->list, 0, std::max(260L, listRc.right - 170L));
            ListView_SetColumnWidth(state->list, 1, 140);
            return 0;
        }
        break;
    case WM_COMMAND:
        if (!state) break;
        switch (LOWORD(wParam)) {
        case IDC_BREAKDOWN_SEARCH:
            if (HIWORD(wParam) == EN_CHANGE) {
                BreakdownDialogRefreshList(state);
                return 0;
            }
            break;
        case IDC_BREAKDOWN_REFRESH:
            BreakdownDialogStartScan(state);
            return 0;
        case IDC_BREAKDOWN_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_GUI_BREAKDOWN_READY:
        if (state) {
            state->loading = false;
            std::unique_ptr<BlockBreakdownPayload> payload(reinterpret_cast<BlockBreakdownPayload*>(lParam));
            state->rows.clear();
            if (payload && payload->ok) {
                for (const auto& [name, count] : payload->summary.byBlock) {
                    state->rows.push_back({ Utf8ToWide(name), count });
                }
                std::sort(state->rows.begin(), state->rows.end(), [](const auto& a, const auto& b) {
                    if (a.second != b.second) return a.second > b.second;
                    return a.first < b.first;
                });
                const std::wstring summary = FormatNumber(payload->summary.byBlock.size()) + L" 种方块，" +
                    FormatNumber(payload->summary.totalBlocks) + L" 个方块";
                SetWindowTextW(state->summary, summary.c_str());
            } else {
                SetWindowTextW(state->summary, payload ? payload->message.c_str() : L"扫描失败");
            }
            BreakdownDialogRefreshList(state);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (state) {
            if (state->owner) state->owner->breakdownDialog = nullptr;
            delete state;
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK GuiPreviewProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    GuiState* state = reinterpret_cast<GuiState*>(GetWindowLongPtrW(GetParent(hwnd), GWLP_USERDATA));
    switch (msg) {
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN:
        if (state) {
            state->previewSettleFrames = 60;
            SetFocus(hwnd);
            state->lastMouse = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            state->previewDragAnchor = state->lastMouse;
            if (state->previewToolMode == PreviewToolMode::Place) {
                PreviewPlacementDragMode dragMode = GuiHitTestPreviewGizmo(state, hwnd, state->lastMouse);
                if (dragMode == PreviewPlacementDragMode::None) dragMode = PreviewPlacementDragMode::Plane;
                GuiRecordPreviewHistory(state);
                GuiPrimePreviewPlacementDrag(state, dragMode);
            } else {
                state->orbiting = true;
            }
            SetCapture(hwnd);
            GuiRequestPreviewFrame(state);
            return 0;
        }
        break;
    case WM_RBUTTONDOWN:
        if (state) {
            state->previewSettleFrames = 60;
            SetFocus(hwnd);
            state->lastMouse = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            state->previewDragAnchor = state->lastMouse;
            if (state->previewToolMode == PreviewToolMode::Place) {
                PreviewPlacementDragMode dragMode = GuiHitTestPreviewGizmo(state, hwnd, state->lastMouse);
                GuiRecordPreviewHistory(state);
                GuiPrimePreviewPlacementDrag(state, dragMode == PreviewPlacementDragMode::AxisY ? PreviewPlacementDragMode::AxisY : PreviewPlacementDragMode::Height);
            } else {
                state->panning = true;
            }
            SetCapture(hwnd);
            GuiRequestPreviewFrame(state);
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (state) {
            const bool draggedPlacement = state->previewDraggingPlacement || state->previewDraggingHeight;
            state->orbiting = false;
            state->previewDraggingPlacement = false;
            state->previewDraggingHeight = false;
            state->previewPlacementDragMode = PreviewPlacementDragMode::None;
            ReleaseCapture();
            GuiRequestPreviewFrame(state);
            if (draggedPlacement) {
                state->config = GuiReadConfig(state);
                GuiSyncPreviewTransformInspector(state);
                GuiRecordPreviewHistory(state);
                GuiScheduleWorldPreviewRefresh(state);
            } else {
                GuiUpdatePreviewChrome(state);
            }
            state->previewSettleFrames = 1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_RBUTTONUP:
        if (state) {
            const bool draggedPlacement = state->previewDraggingPlacement || state->previewDraggingHeight;
            state->panning = false;
            state->previewDraggingPlacement = false;
            state->previewDraggingHeight = false;
            state->previewPlacementDragMode = PreviewPlacementDragMode::None;
            ReleaseCapture();
            GuiRequestPreviewFrame(state);
            if (draggedPlacement) {
                state->config = GuiReadConfig(state);
                GuiSyncPreviewTransformInspector(state);
                GuiRecordPreviewHistory(state);
                GuiScheduleWorldPreviewRefresh(state);
            } else {
                GuiUpdatePreviewChrome(state);
            }
            state->previewSettleFrames = 1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_CANCELMODE:
    case WM_CAPTURECHANGED:
        if (state && GuiPreviewIsInteracting(state)) {
            state->orbiting = false;
            state->panning = false;
            state->previewDraggingPlacement = false;
            state->previewDraggingHeight = false;
            state->previewPlacementDragMode = PreviewPlacementDragMode::None;
            GuiRequestPreviewFrame(state);
            state->previewSettleFrames = 1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_MOUSEMOVE:
        if (state && (state->orbiting || state->panning || state->previewDraggingPlacement || state->previewDraggingHeight)) {
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            const int dx = pt.x - state->lastMouse.x;
            const int dy = pt.y - state->lastMouse.y;
            state->lastMouse = pt;
            if (state->previewDraggingPlacement || state->previewDraggingHeight) {
                GuiApplyPreviewPlacementDrag(state, pt);
            } else if (state->orbiting) {
                const float sensitivity = std::clamp(
                    GuiReadMappedFloat(state, L"previewOrbitSensitivity", 1.5f), 0.05f, 100.0f);
                state->previewYaw += dx * 0.5f * sensitivity;
                state->previewPitch = std::clamp(state->previewPitch + dy * 0.35f * sensitivity, -89.0f, 89.0f);
            } else {
                const float sensitivity = std::clamp(
                    GuiReadMappedFloat(state, L"previewPanSensitivity", 2.0f), 0.05f, 100.0f);
                state->previewPanX += dx * 0.01f * sensitivity;
                state->previewPanY -= dy * 0.01f * sensitivity;
            }
            GuiUpdatePreviewChromeThrottled(state);
            GuiRequestPreviewFrame(state);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (state && state->previewToolMode == PreviewToolMode::Place) {
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            const PreviewPlacementDragMode hover = GuiHitTestPreviewGizmo(state, hwnd, pt);
            if (hover != state->previewHoverDragMode) {
                state->previewHoverDragMode = hover;
                GuiUpdatePreviewChromeThrottled(state);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        break;
    case WM_MOUSEWHEEL:
        if (state) {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            const float sensitivity = std::clamp(
                GuiReadMappedFloat(state, L"previewZoomSensitivity", 1.5f), 0.05f, 100.0f);
            state->previewDistance = std::max(0.5f, state->previewDistance -
                (delta / static_cast<float>(WHEEL_DELTA)) * 0.8f * sensitivity);
            state->previewSettleFrames = 1;
            GuiUpdatePreviewChrome(state);
            GuiRequestPreviewFrame(state);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (state && (GetKeyState(VK_CONTROL) & 0x8000)) {
            if (wParam == VK_OEM_COMMA) { GuiSelectAdjacentModel(state, -1); return 0; }
            if (wParam == VK_OEM_PERIOD) { GuiSelectAdjacentModel(state, 1); return 0; }
            if (wParam >= '1' && wParam <= '3') {
                const int slot = static_cast<int>(wParam - '1');
                if (GetKeyState(VK_SHIFT) & 0x8000) GuiSavePreviewPreset(state, slot);
                else GuiLoadPreviewPreset(state, slot);
                return 0;
            }
            if (wParam == 'Z') { GuiPreviewUndo(state); return 0; }
            if (wParam == 'Y') { GuiPreviewRedo(state); return 0; }
        }
        if (state && state->previewToolMode == PreviewToolMode::Place) {
            switch (wParam) {
            case VK_OEM_4: GuiSelectAdjacentModel(state, -1); return 0;
            case VK_OEM_6: GuiSelectAdjacentModel(state, 1); return 0;
            case VK_LEFT: GuiRecordPreviewHistory(state); GuiNudgePreviewCenter(state, -state->previewSnapStep, 0, 0); GuiRecordPreviewHistory(state); return 0;
            case VK_RIGHT: GuiRecordPreviewHistory(state); GuiNudgePreviewCenter(state, state->previewSnapStep, 0, 0); GuiRecordPreviewHistory(state); return 0;
            case VK_UP: GuiRecordPreviewHistory(state); GuiNudgePreviewCenter(state, 0, 0, -state->previewSnapStep); GuiRecordPreviewHistory(state); return 0;
            case VK_DOWN: GuiRecordPreviewHistory(state); GuiNudgePreviewCenter(state, 0, 0, state->previewSnapStep); GuiRecordPreviewHistory(state); return 0;
            case VK_PRIOR: GuiRecordPreviewHistory(state); GuiNudgePreviewCenter(state, 0, state->previewSnapStep, 0); GuiRecordPreviewHistory(state); return 0;
            case VK_NEXT: GuiRecordPreviewHistory(state); GuiNudgePreviewCenter(state, 0, -state->previewSnapStep, 0); GuiRecordPreviewHistory(state); return 0;
            case 'W': GuiRecordPreviewHistory(state); GuiNudgePreviewCenter(state, 0, 0, -state->previewSnapStep); GuiRecordPreviewHistory(state); return 0;
            case 'S': GuiRecordPreviewHistory(state); GuiNudgePreviewCenter(state, 0, 0, state->previewSnapStep); GuiRecordPreviewHistory(state); return 0;
            case 'A': GuiRecordPreviewHistory(state); GuiNudgePreviewCenter(state, -state->previewSnapStep, 0, 0); GuiRecordPreviewHistory(state); return 0;
            case 'D': GuiRecordPreviewHistory(state); GuiNudgePreviewCenter(state, state->previewSnapStep, 0, 0); GuiRecordPreviewHistory(state); return 0;
            }
        }
        break;
    case WM_SIZE:
        if (state) {
            GuiInitPreviewContext(state);
            state->previewSettleFrames = 1;
            GuiRequestPreviewFrame(state);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_PAINT:
        if (state) {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            const bool scheduledPreview = state->previewLoaded &&
                (state->previewFrameActive.load(std::memory_order_acquire) ||
                 state->previewFrameTickPending.load(std::memory_order_acquire)) &&
                state->previewFrameSchedulerRunning.load(std::memory_order_acquire);
            const bool consumedScheduledFrame = scheduledPreview && state->previewFramePermit;
            const bool renderPreview = !scheduledPreview || consumedScheduledFrame;
            state->previewFramePermit = false;
            if (renderPreview) GuiRenderPreviewScene(state, hwnd);
            if (!state->previewLoaded) GuiRenderPreviewFallback(state, hwnd, ps.hdc);
            EndPaint(hwnd, &ps);
            if (consumedScheduledFrame) {
                state->previewFrameTickPending.store(false, std::memory_order_release);
            }
            return 0;
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK GuiScrollPageProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    GuiState* state = reinterpret_cast<GuiState*>(GetWindowLongPtrW(GetParent(hwnd), GWLP_USERDATA));
    switch (msg) {
    case WM_COMMAND:
        return SendMessageW(GetParent(hwnd), msg, wParam, lParam);
    case WM_HSCROLL:
        return SendMessageW(GetParent(hwnd), msg, wParam, lParam);
    case WM_VKEYTOITEM:
        if (state && reinterpret_cast<HWND>(lParam) == state->modelList) {
            if (LOWORD(wParam) == L'A' && (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
                SendMessageW(state->modelList, LB_SETSEL, TRUE, static_cast<LPARAM>(-1));
                GuiUpdateModelPanel(state);
                return -2;
            }
            if (LOWORD(wParam) == VK_RETURN) {
                GuiLoadSelectedPreviewsAsync(state);
                return -2;
            }
        }
        break;
    case WM_SIZE:
        GuiUpdatePageScrollBar(state, hwnd);
        return 0;
    case WM_MOUSEWHEEL:
        if (state) {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            GuiScrollPageTo(state, hwnd, state->pageScrollPos[hwnd] - (delta / WHEEL_DELTA) * 48);
            return 0;
        }
        break;
    case WM_VSCROLL:
        if (state) {
            SCROLLINFO si{};
            si.cbSize = sizeof(si);
            si.fMask = SIF_ALL;
            GetScrollInfo(hwnd, SB_VERT, &si);
            int newPos = si.nPos;
            switch (LOWORD(wParam)) {
            case SB_LINEUP: newPos -= 24; break;
            case SB_LINEDOWN: newPos += 24; break;
            case SB_PAGEUP: newPos -= static_cast<int>(si.nPage); break;
            case SB_PAGEDOWN: newPos += static_cast<int>(si.nPage); break;
            case SB_THUMBPOSITION:
            case SB_THUMBTRACK: newPos = si.nTrackPos; break;
            case SB_TOP: newPos = si.nMin; break;
            case SB_BOTTOM: newPos = si.nMax; break;
            default: break;
            }
            GuiScrollPageTo(state, hwnd, newPos);
            return 0;
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK GuiWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    GuiState* state = reinterpret_cast<GuiState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = reinterpret_cast<GuiState*>(cs->lpCreateParams);
        state->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        state->font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS };
        InitCommonControlsEx(&icc);

        WNDCLASSW scrollPageClass{};
        scrollPageClass.lpfnWndProc = GuiScrollPageProc;
        scrollPageClass.hInstance = GetModuleHandleW(nullptr);
        scrollPageClass.lpszClassName = L"MCScrollPage";
        scrollPageClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        scrollPageClass.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        RegisterClassW(&scrollPageClass);

        state->tab = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP, 0, 0, 100, 100, hwnd, (HMENU)IDC_GUI_TAB, nullptr, nullptr);
        state->preview = CreateWindowExW(0, L"MCPreviewWindow", L"", WS_CHILD | WS_VISIBLE | WS_BORDER, 0, 0, 100, 100, hwnd, (HMENU)IDC_GUI_PREVIEW, nullptr, nullptr);
        state->previewInfoLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 100, 20, hwnd, (HMENU)IDC_GUI_PREVIEW_INFO, nullptr, nullptr);
        state->previewHintLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 100, 20, hwnd, (HMENU)IDC_GUI_PREVIEW_HINT, nullptr, nullptr);
        state->previewRendererLabel = CreateWindowExW(0, L"STATIC", L"当前渲染 API: 初始化中", WS_CHILD | WS_VISIBLE, 0, 0, 100, 20, hwnd, (HMENU)IDC_GUI_PREVIEW_RENDERER_INFO, nullptr, nullptr);
        state->log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL, 0, 0, 100, 100, hwnd, (HMENU)IDC_GUI_LOG, nullptr, nullptr);
        state->status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 100, 20, hwnd, (HMENU)IDC_GUI_STATUS, nullptr, nullptr);
        SendMessageW(state->tab, WM_SETFONT, (WPARAM)state->font, TRUE);
        SendMessageW(state->previewInfoLabel, WM_SETFONT, (WPARAM)state->font, TRUE);
        SendMessageW(state->previewHintLabel, WM_SETFONT, (WPARAM)state->font, TRUE);
        SendMessageW(state->previewRendererLabel, WM_SETFONT, (WPARAM)state->font, TRUE);
        SendMessageW(state->log, WM_SETFONT, (WPARAM)state->font, TRUE);
        SendMessageW(state->status, WM_SETFONT, (WPARAM)state->font, TRUE);
        const std::vector<std::wstring> tabNames = { L"模型与存档", L"通用设置", L"GLB 设置", L"OBJ 设置", L"实景与扫描", L"模型列表", L"高级路径" };
        for (const auto& name : tabNames) {
            TCITEMW item{}; item.mask = TCIF_TEXT; item.pszText = const_cast<wchar_t*>(name.c_str());
            TabCtrl_InsertItem(state->tab, TabCtrl_GetItemCount(state->tab), &item);
        }

        for (size_t i = 0; i < tabNames.size(); ++i) {
            HWND page = CreateWindowExW(WS_EX_CONTROLPARENT, L"MCScrollPage", L"",
                WS_CHILD | WS_VSCROLL | (i == 0 ? WS_VISIBLE : 0),
                0, 0, 100, 100, hwnd, nullptr, nullptr, nullptr);
            state->pages.push_back(page);
            state->pageContentHeight[page] = 0;
            state->pageScrollPos[page] = 0;
        }

        auto createLabel = [&](HWND parent, const std::wstring& text, int x, int y, int w) {
            HWND h = CreateWindowExW(0, L"STATIC", text.c_str(), WS_CHILD | WS_VISIBLE, x, y, w, 20, parent, nullptr, nullptr, nullptr);
            SendMessageW(h, WM_SETFONT, (WPARAM)state->font, TRUE);
            return h;
        };
        auto createEdit = [&](HWND parent, const std::wstring& key, int x, int y, int w) {
            HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, x, y, w, 22, parent, nullptr, nullptr, nullptr);
            SendMessageW(h, WM_SETFONT, (WPARAM)state->font, TRUE);
            state->edits[key] = h;
            return h;
        };
        auto createCombo = [&](HWND parent, const std::wstring& key, int x, int y, int w,
                               const std::vector<std::wstring>& options) {
            HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, WC_COMBOBOXW, L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                x, y, w, 180, parent, nullptr, nullptr, nullptr);
            SendMessageW(h, WM_SETFONT, (WPARAM)state->font, TRUE);
            for (const auto& option : options) SendMessageW(h, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(option.c_str()));
            if (!options.empty()) SendMessageW(h, CB_SETCURSEL, 0, 0);
            state->edits[key] = h;
            return h;
        };
        auto createBrowse = [&](HWND parent, int id, int x, int y) {
            HWND h = CreateWindowExW(0, L"BUTTON", L"...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, x, y, 34, 22, parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
            SendMessageW(h, WM_SETFONT, (WPARAM)state->font, TRUE);
            return h;
        };
        auto createCheck = [&](HWND parent, const std::wstring& key, const std::wstring& text, int x, int y, int w) {
            HWND h = CreateWindowExW(0, L"BUTTON", text.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, x, y, w, 22, parent, nullptr, nullptr, nullptr);
            SendMessageW(h, WM_SETFONT, (WPARAM)state->font, TRUE);
            state->checks[key] = h;
            return h;
        };
        auto createRadio = [&](HWND parent, const std::wstring& key, const std::wstring& text, int x, int y, int w, bool first) {
            const DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON | (first ? WS_GROUP : 0);
            HWND h = CreateWindowExW(0, L"BUTTON", text.c_str(), style, x, y, w, 22, parent, nullptr, nullptr, nullptr);
            SendMessageW(h, WM_SETFONT, (WPARAM)state->font, TRUE);
            state->checks[key] = h;
            return h;
        };
        auto addEditRow = [&](int pageIndex, int& y, const std::wstring& label, const std::wstring& key, bool browseFolder = false, bool browseFile = false) {
            createLabel(state->pages[pageIndex], label, 10, y + 3, 135);
            createEdit(state->pages[pageIndex], key, 150, y, browseFolder || browseFile ? 180 : 220);
            if (browseFolder || browseFile) {
                const int id = IDC_GUI_BROWSE_BASE + static_cast<int>(state->browseMap.size());
                state->browseMap[id] = { key, browseFolder };
                createBrowse(state->pages[pageIndex], id, 336, y);
            }
            y += 28;
        };
        auto addComboRow = [&](int pageIndex, int& y, const std::wstring& label, const std::wstring& key,
                               const std::vector<std::wstring>& options) {
            createLabel(state->pages[pageIndex], label, 10, y + 3, 135);
            createCombo(state->pages[pageIndex], key, 150, y, 220, options);
            y += 28;
        };
        auto addCheckRow = [&](int pageIndex, int& y, const std::wstring& text, const std::wstring& key) {
            createCheck(state->pages[pageIndex], key, text, 10, y, 360);
            y += 24;
        };
        auto addRadioRow = [&](int pageIndex, int& y, const std::wstring& text, const std::wstring& key, bool first) {
            createRadio(state->pages[pageIndex], key, text, 10, y, 360, first);
            y += 24;
        };

        int y0 = 10;
        createLabel(state->pages[0], L"模型文件或目录", 10, y0 + 3, 115);
        createEdit(state->pages[0], L"inputPath", 130, y0, 166);
        HWND modelFilePicker = CreateWindowExW(0, L"BUTTON", L"文件",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 302, y0, 40, 24,
            state->pages[0], (HMENU)(INT_PTR)IDC_GUI_INPUT_FILE_PICKER, nullptr, nullptr);
        SendMessageW(modelFilePicker, WM_SETFONT, (WPARAM)state->font, TRUE);
        HWND modelDirPicker = CreateWindowExW(0, L"BUTTON", L"目录",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 348, y0, 40, 24,
            state->pages[0], (HMENU)(INT_PTR)IDC_GUI_INPUT_DIR_PICKER, nullptr, nullptr);
        SendMessageW(modelDirPicker, WM_SETFONT, (WPARAM)state->font, TRUE);
        y0 += 34;
        createLabel(state->pages[0], L"Minecraft 存档", 10, y0 + 3, 115);
        createEdit(state->pages[0], L"worldDir", 130, y0, 212);
        HWND worldDirPicker = CreateWindowExW(0, L"BUTTON", L"选择",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 348, y0, 40, 24,
            state->pages[0], (HMENU)(INT_PTR)IDC_GUI_WORLD_DIR_PICKER, nullptr, nullptr);
        SendMessageW(worldDirPicker, WM_SETFONT, (WPARAM)state->font, TRUE);
        y0 += 38;
        createLabel(state->pages[0], L"选择模型和存档后会自动加载预览；也可以直接拖入文件或文件夹。", 10, y0, 378);
        y0 += 28;
        state->pageContentHeight[state->pages[0]] = y0 + 12;

        int y1 = 10;
        addComboRow(1, y1, L"导入模式", L"lastMode",
            { L"GLB 贴图导入", L"GLB 单方块表面", L"OBJ 贴图导入", L"OBJ 单方块表面" });
        addEditRow(1, y1, L"中心坐标 (x,y,z)", L"center");
        addEditRow(1, y1, L"中心 X", L"centerX");
        addEditRow(1, y1, L"中心 Y", L"centerY");
        addEditRow(1, y1, L"中心 Z", L"centerZ");
        addEditRow(1, y1,
            L"OBJ 导入/总览并行线程数（1-64，默认 " +
                std::to_wstring(DefaultWorkerCount()) + L"）",
            L"workers");
        addEditRow(1, y1, L"单批方块上限", L"batchBlockLimit");
        addComboRow(1, y1, L"预览图形后端", L"previewRenderer",
            { L"自动（D3D12 / Vulkan / D3D11）", L"Direct3D 11", L"Direct3D 12", L"Vulkan", L"OpenGL（兼容）" });
        addComboRow(1, y1, L"全局预览刷新率上限", L"previewRefreshRate",
            { L"30 Hz", L"60 Hz", L"120 Hz", L"144 Hz", L"180 Hz", L"200 Hz", L"无上限" });
        addCheckRow(1, y1, L"显示预览网格", L"previewShowGrid");
        addCheckRow(1, y1, L"显示预览坐标轴", L"previewShowAxes");
        addEditRow(1, y1, L"相机旋转灵敏度倍率", L"previewOrbitSensitivity");
        addEditRow(1, y1, L"相机平移灵敏度倍率", L"previewPanSensitivity");
        addEditRow(1, y1, L"滚轮缩放灵敏度倍率", L"previewZoomSensitivity");
        addComboRow(1, y1, L"界面语言", L"lang", { L"中文", L"English" });
        addRadioRow(1, y1, L"导入到测试副本（推荐）", L"copyWorld", true);
        addRadioRow(1, y1, L"直接写入所选存档", L"direct", false);
        addCheckRow(1, y1, L"覆盖目标位置的非空气方块", L"overwrite");
        addCheckRow(1, y1, L"处理目录内全部模型", L"all");
        state->pageContentHeight[state->pages[1]] = y1 + 12;

        int y2 = 10;
        addEditRow(2, y2, L"无贴图时回退方块", L"fallbackBlock");
        addEditRow(2, y2, L"表面模式使用方块", L"block");
        addEditRow(2, y2, L"去重采样步数", L"dedupeSampleSteps");
        state->pageContentHeight[state->pages[2]] = y2 + 12;

        int y3 = 10;
        addEditRow(3, y3, L"clip 参考 OBJ", L"clipReferenceObj", false, true);
        addEditRow(3, y3, L"裁剪线以下米数", L"clipBelowMeters");
        addEditRow(3, y3, L"裁剪网格大小", L"clipCellSize");
        addEditRow(3, y3, L"低点占比", L"clipLowFraction");
        addEditRow(3, y3, L"裁剪迭代次数", L"clipPasses");
        addEditRow(3, y3, L"异常值裁剪强度", L"clipTrimSigma");
        addEditRow(3, y3, L"最小组件比例", L"componentMinRatio");
        addEditRow(3, y3, L"组件高度间隔", L"componentBelowGap");
        addCheckRow(3, y3, L"保持原始 Z 方向", L"noFlipZ");
        addCheckRow(3, y3, L"保留所有模型组件", L"noComponentFilter");
        state->pageContentHeight[state->pages[3]] = y3 + 12;

        int y4 = 10;
        addEditRow(4, y4, L"实景半径（区块）", L"previewWorldChunks");
        addComboRow(4, y4, L"扫描调色板", L"palette",
            { L"场景均衡（推荐）", L"高彩度方块", L"仅混凝土", L"全部可用方块" });
        addEditRow(4, y4, L"扫描最低 Y", L"minY");
        addEditRow(4, y4, L"扫描最高 Y", L"maxY");
        state->pageContentHeight[state->pages[4]] = y4 + 12;

        int y5 = 10;
        state->modelSummaryLabel = createLabel(state->pages[5], L"模式: GLB  共 0 个文件", 10, y5, 360);
        y5 += 26;
        state->modelCurrentLabel = createLabel(state->pages[5], L"当前模型: 未加载", 10, y5, 360);
        y5 += 22;
        state->modelSelectedLabel = createLabel(state->pages[5], L"所选模型: (无)", 10, y5, 360);
        y5 += 24;
        createLabel(state->pages[5], L"筛选", 10, y5 + 3, 60);
        state->modelFilterEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            70, y5, 260, 22, state->pages[5], (HMENU)(INT_PTR)IDC_GUI_MODEL_FILTER, nullptr, nullptr);
        SendMessageW(state->modelFilterEdit, WM_SETFONT, (WPARAM)state->font, TRUE);
        CreateWindowExW(0, L"BUTTON", L"刷新", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            336, y5, 52, 22, state->pages[5], (HMENU)(INT_PTR)IDC_GUI_MODEL_REFRESH, nullptr, nullptr);
        SendMessageW(GetDlgItem(state->pages[5], IDC_GUI_MODEL_REFRESH), WM_SETFONT, (WPARAM)state->font, TRUE);
        y5 += 30;
        state->modelList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_EXTENDEDSEL | LBS_WANTKEYBOARDINPUT | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            10, y5, 378, 260, state->pages[5], (HMENU)(INT_PTR)IDC_GUI_MODEL_LIST, nullptr, nullptr);
        SendMessageW(state->modelList, WM_SETFONT, (WPARAM)state->font, TRUE);
        y5 += 268;
        CreateWindowExW(0, L"BUTTON", L"上一项", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            10, y5, 184, 26, state->pages[5], (HMENU)(INT_PTR)IDC_GUI_MODEL_PREV, nullptr, nullptr);
        SendMessageW(GetDlgItem(state->pages[5], IDC_GUI_MODEL_PREV), WM_SETFONT, (WPARAM)state->font, TRUE);
        CreateWindowExW(0, L"BUTTON", L"下一项", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            204, y5, 184, 26, state->pages[5], (HMENU)(INT_PTR)IDC_GUI_MODEL_NEXT, nullptr, nullptr);
        SendMessageW(GetDlgItem(state->pages[5], IDC_GUI_MODEL_NEXT), WM_SETFONT, (WPARAM)state->font, TRUE);
        y5 += 34;
        CreateWindowExW(0, L"BUTTON", L"显示所选模型", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            10, y5, 378, 26, state->pages[5], (HMENU)(INT_PTR)IDC_GUI_MODEL_USE, nullptr, nullptr);
        SendMessageW(GetDlgItem(state->pages[5], IDC_GUI_MODEL_USE), WM_SETFONT, (WPARAM)state->font, TRUE);
        y5 += 34;
        CreateWindowExW(0, L"BUTTON", L"同时显示全部模型", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            10, y5, 378, 26, state->pages[5], (HMENU)(INT_PTR)IDC_GUI_MODEL_SHOW_ALL, nullptr, nullptr);
        SendMessageW(GetDlgItem(state->pages[5], IDC_GUI_MODEL_SHOW_ALL), WM_SETFONT, (WPARAM)state->font, TRUE);
        y5 += 34;
        CreateWindowExW(0, L"BUTTON", L"以所选 OBJ 作为找平面", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            10, y5, 378, 26, state->pages[5], (HMENU)(INT_PTR)IDC_GUI_MODEL_LEVEL_REFERENCE, nullptr, nullptr);
        SendMessageW(GetDlgItem(state->pages[5], IDC_GUI_MODEL_LEVEL_REFERENCE), WM_SETFONT, (WPARAM)state->font, TRUE);
        y5 += 34;
        state->pageContentHeight[state->pages[5]] = y5 + 12;

        int y6 = 10;
        addEditRow(6, y6, L"指定文件名", L"onlyFile");
        addEditRow(6, y6, L"OBJ 备用目录", L"objDir", true, false);
        addEditRow(6, y6, L"GLB 备用目录", L"glbDir", true, false);
        addEditRow(6, y6, L"存档总目录", L"savesDir", true, false);
        addEditRow(6, y6, L"Minecraft 目录", L"mcRoot", true, false);
        addEditRow(6, y6, L"存档名称", L"worldName");
        addEditRow(6, y6, L"版本目录", L"mcVersion");
        addEditRow(6, y6, L"测试副本目录", L"testWorldRoot", true, false);
        addEditRow(6, y6, L"复制目标目录", L"copyTo", true, false);
        addEditRow(6, y6, L"资源根目录", L"projectRoot", true, false);
        state->pageContentHeight[state->pages[6]] = y6 + 12;

        auto createAction = [&](int id, const std::wstring& text) {
            HWND h = CreateWindowExW(0, L"BUTTON", text.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 100, 26, hwnd, (HMENU)(INT_PTR)id, nullptr, nullptr);
            SendMessageW(h, WM_SETFONT, (WPARAM)state->font, TRUE);
            state->actionButtons.push_back(h);
            return h;
        };
        auto createPreviewButton = [&](int id, const std::wstring& text) {
            HWND h = CreateWindowExW(0, L"BUTTON", text.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 100, 26, hwnd, (HMENU)(INT_PTR)id, nullptr, nullptr);
            SendMessageW(h, WM_SETFONT, (WPARAM)state->font, TRUE);
            state->previewButtons.push_back(h);
            return h;
        };
        auto createInspectorLabel = [&](int id, const std::wstring& text) {
            HWND h = CreateWindowExW(0, L"STATIC", text.c_str(), WS_CHILD | WS_VISIBLE, 0, 0, 100, 20, state->previewInspector, (HMENU)(INT_PTR)id, nullptr, nullptr);
            SendMessageW(h, WM_SETFONT, (WPARAM)state->font, TRUE);
            return h;
        };
        auto createInspectorButton = [&](int id, const std::wstring& text, DWORD style = BS_PUSHBUTTON) {
            HWND h = CreateWindowExW(0, L"BUTTON", text.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, 0, 0, 100, 26, state->previewInspector, (HMENU)(INT_PTR)id, nullptr, nullptr);
            SendMessageW(h, WM_SETFONT, (WPARAM)state->font, TRUE);
            return h;
        };
        auto createInspectorTrack = [&](int id, int minValue, int maxValue) {
            HWND h = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_TOOLTIPS, 0, 0, 100, 30, state->previewInspector, (HMENU)(INT_PTR)id, nullptr, nullptr);
            SendMessageW(h, TBM_SETRANGE, TRUE, MAKELPARAM(minValue, maxValue));
            return h;
        };
        createAction(IDC_GUI_ACTION_SAVE, T(state->lang, L"保存配置", L"Save Config"));
        createAction(IDC_GUI_ACTION_LOAD, T(state->lang, L"重载配置", L"Reload Config"));
        createAction(IDC_GUI_ACTION_DOCTOR, T(state->lang, L"检查环境", L"Doctor"));
        createAction(IDC_GUI_ACTION_COPY_WORLD, T(state->lang, L"复制测试存档", L"Copy Test World"));
        createAction(IDC_GUI_ACTION_GLB, T(state->lang, L"导入 GLB", L"Import GLB"));
        createAction(IDC_GUI_ACTION_OBJ, T(state->lang, L"导入 OBJ", L"Import OBJ"));
        createAction(IDC_GUI_ACTION_SCAN, T(state->lang, L"扫描存档", L"Scan"));
        createAction(IDC_GUI_ACTION_RESET, T(state->lang, L"重置存档", L"Reset"));
        createAction(IDC_GUI_ACTION_PREVIEW_LOAD, T(state->lang, L"加载预览", L"Load Preview"));
        createAction(IDC_GUI_ACTION_PALETTE_EDITOR, L"方块调色板");
        createAction(IDC_GUI_ACTION_BREAKDOWN, L"方块统计");
        createAction(IDC_GUI_ACTION_HELP, T(state->lang, L"帮助", L"Help"));
        createPreviewButton(IDC_GUI_PREVIEW_TOOL_CAMERA, L"相机模式*");
        createPreviewButton(IDC_GUI_PREVIEW_TOOL_PLACE, L"摆放模式");
        createPreviewButton(IDC_GUI_PREVIEW_SNAP, L"吸附: 16");
        createPreviewButton(IDC_GUI_PREVIEW_WORLD, L"实景: 开");
        createPreviewButton(IDC_GUI_PREVIEW_WORLD_REFRESH, L"刷新实景");
        createPreviewButton(IDC_GUI_PREVIEW_WORLD_RADIUS, L"实景半径: 48");
        createPreviewButton(IDC_GUI_PREVIEW_WORLD_STYLE, L"实景样式: 面");
        createPreviewButton(IDC_GUI_PREVIEW_UNDO, L"撤销");
        createPreviewButton(IDC_GUI_PREVIEW_REDO, L"重做");
        createPreviewButton(IDC_GUI_PREVIEW_FIT, L"适应视图");
        createPreviewButton(IDC_GUI_PREVIEW_VIEW_TOP, L"顶视");
        createPreviewButton(IDC_GUI_PREVIEW_VIEW_FRONT, L"前视");
        createPreviewButton(IDC_GUI_PREVIEW_VIEW_LEFT, L"左视");
        createPreviewButton(IDC_GUI_PREVIEW_VIEW_RIGHT, L"右视");
        createPreviewButton(IDC_GUI_PREVIEW_MODE, L"显示: 混合");
        createPreviewButton(IDC_GUI_PREVIEW_LOD, L"LOD: 自动");
        state->previewInspector = CreateWindowExW(WS_EX_CLIENTEDGE, L"MCScrollPage", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN, 0, 0, 100, 100, hwnd, (HMENU)IDC_GUI_PREVIEW_INSPECTOR, nullptr, nullptr);
        state->pageContentHeight[state->previewInspector] = 0;
        state->pageScrollPos[state->previewInspector] = 0;
        state->previewRotationModeCombo = createCombo(state->previewInspector, L"rotationMode", 0, 0, 100,
            { L"XYZ 欧拉", L"Blender 四元数 (WXYZ)" });
        const int rotationLabelIds[] = { IDC_GUI_ROTATION_W_LABEL, IDC_GUI_ROTATION_X_LABEL, IDC_GUI_ROTATION_Y_LABEL, IDC_GUI_ROTATION_Z_LABEL };
        const int rotationEditIds[] = { IDC_GUI_ROTATION_W_EDIT, IDC_GUI_ROTATION_X_EDIT, IDC_GUI_ROTATION_Y_EDIT, IDC_GUI_ROTATION_Z_EDIT };
        const wchar_t* rotationKeys[] = { L"rotationW", L"rotationX", L"rotationY", L"rotationZ" };
        for (int i = 0; i < 4; ++i) {
            state->previewRotationLabels[i] = createInspectorLabel(rotationLabelIds[i], i == 0 ? L"四元数 W" : (i == 1 ? L"四元数 X" : (i == 2 ? L"四元数 Y" : L"四元数 Z")));
            state->previewRotationEdits[i] = createEdit(state->previewInspector, rotationKeys[i], 0, 0, 100);
            SetWindowLongPtrW(state->previewRotationEdits[i], GWLP_ID, rotationEditIds[i]);
        }
        HWND legacyRotate = createEdit(hwnd, L"rotateYDeg", 0, 0, 10);
        ShowWindow(legacyRotate, SW_HIDE);
        state->previewLevelNormalLabel = createInspectorLabel(IDC_GUI_LEVEL_NORMAL_LABEL, L"找平法线");
        state->previewLevelNormalEdit = createEdit(state->previewInspector, L"levelNormal", 0, 0, 100);
        SetWindowLongPtrW(state->previewLevelNormalEdit, GWLP_ID, IDC_GUI_LEVEL_NORMAL_EDIT);
        state->previewLevelButton = createInspectorButton(IDC_GUI_LEVEL_APPLY, L"自动找平");
        state->previewScaleLabel = createInspectorLabel(IDC_GUI_TRANSFORM_SCALE_LABEL, L"缩放: 4 block/m");
        state->previewScaleEdit = createEdit(state->previewInspector, L"scaleBlocksPerMeter", 0, 0, 100);
        SetWindowLongPtrW(state->previewScaleEdit, GWLP_ID, IDC_GUI_SCALE_EDIT);
        state->previewScaleTrack = createInspectorTrack(IDC_GUI_TRANSFORM_SCALE_TRACK, kPreviewScaleTrackMin, kPreviewScaleTrackMax);
        createInspectorButton(IDC_GUI_TRANSFORM_SCALE_DOWN, L"-0.5");
        createInspectorButton(IDC_GUI_TRANSFORM_SCALE_UP, L"+0.5");
        state->previewBaseYLabel = createInspectorLabel(IDC_GUI_TRANSFORM_BASEY_LABEL, L"基准 Y: 0");
        state->previewBaseYEdit = createEdit(state->previewInspector, L"baseY", 0, 0, 100);
        SetWindowLongPtrW(state->previewBaseYEdit, GWLP_ID, IDC_GUI_BASEY_EDIT);
        state->previewBaseYTrack = createInspectorTrack(IDC_GUI_TRANSFORM_BASEY_TRACK, 0, kPreviewBaseYTrackMax + kPreviewBaseYTrackBias);
        createInspectorButton(IDC_GUI_TRANSFORM_BASEY_DOWN, L"-8");
        createInspectorButton(IDC_GUI_TRANSFORM_BASEY_UP, L"+8");
        state->previewFlipXCheck = createInspectorButton(IDC_GUI_TRANSFORM_FLIPX, L"Flip X", BS_AUTOCHECKBOX);
        state->previewFlipZCheck = createInspectorButton(IDC_GUI_TRANSFORM_FLIPZ, L"Flip Z", BS_AUTOCHECKBOX);
        state->checks[L"flipX"] = state->previewFlipXCheck;
        state->checks[L"flipZ"] = state->previewFlipZCheck;
        HWND transformValue = createEdit(hwnd, L"transform", 0, 0, 10);
        ShowWindow(transformValue, SW_HIDE);
        state->previewTransformButton = createInspectorButton(IDC_GUI_TRANSFORM_SYSTEM, L"坐标系: 默认");
        createInspectorButton(IDC_GUI_TRANSFORM_RESET, L"重置变换");
        state->previewPlaceXLabel = createInspectorLabel(IDC_GUI_PLACE_X_LABEL, L"落点 X: 自动(0)");
        createInspectorButton(IDC_GUI_PLACE_X_LEFT, L"-16");
        createInspectorButton(IDC_GUI_PLACE_X_RIGHT, L"+16");
        state->previewPlaceZLabel = createInspectorLabel(IDC_GUI_PLACE_Z_LABEL, L"落点 Z: 自动(0)");
        createInspectorButton(IDC_GUI_PLACE_Z_LEFT, L"-16");
        createInspectorButton(IDC_GUI_PLACE_Z_RIGHT, L"+16");
        state->previewPlaceYLabel = createInspectorLabel(IDC_GUI_PLACE_Y_LABEL, L"中心 Y: 自动");
        createInspectorButton(IDC_GUI_PLACE_Y_DOWN, L"-16");
        createInspectorButton(IDC_GUI_PLACE_Y_UP, L"+16");
        createInspectorButton(IDC_GUI_PLACE_CLEAR_CENTER, L"清空 center");
        createInspectorButton(IDC_GUI_PLACE_ORIGIN_CENTER, L"落到原点");
        state->previewPresetLabels[0] = createInspectorLabel(IDC_GUI_PRESET_A_LABEL, L"方案 A: 未保存");
        createInspectorButton(IDC_GUI_PRESET_A_SAVE, L"保存 A");
        createInspectorButton(IDC_GUI_PRESET_A_LOAD, L"读取 A");
        state->previewPresetLabels[1] = createInspectorLabel(IDC_GUI_PRESET_B_LABEL, L"方案 B: 未保存");
        createInspectorButton(IDC_GUI_PRESET_B_SAVE, L"保存 B");
        createInspectorButton(IDC_GUI_PRESET_B_LOAD, L"读取 B");
        state->previewPresetLabels[2] = createInspectorLabel(IDC_GUI_PRESET_C_LABEL, L"方案 C: 未保存");
        createInspectorButton(IDC_GUI_PRESET_C_SAVE, L"保存 C");
        createInspectorButton(IDC_GUI_PRESET_C_LOAD, L"读取 C");

        GuiPopulateControls(state, state->config);
        GuiResetPreviewHistory(state);
        GuiSetStatus(state, T(state->lang, L"就绪", L"Ready"));
        DragAcceptFiles(hwnd, TRUE);
        GuiUpdatePreviewChrome(state);
        GuiUpdateWorkflow(state);
        GuiLoadPreviewAsync(state);
        return 0;
    }
    case WM_KEYDOWN:
        if (state && (GetKeyState(VK_CONTROL) & 0x8000) != 0 && wParam >= '1' && wParam <= '7') {
            const int pageIndex = static_cast<int>(wParam - '1');
            if (pageIndex >= 0 && pageIndex < static_cast<int>(state->pages.size())) {
                TabCtrl_SetCurSel(state->tab, pageIndex);
                for (std::size_t index = 0; index < state->pages.size(); ++index) {
                    ShowWindow(state->pages[index], static_cast<int>(index) == pageIndex ? SW_SHOW : SW_HIDE);
                }
                HWND firstControl = GetNextDlgTabItem(state->pages[pageIndex], nullptr, FALSE);
                SetFocus(firstControl ? firstControl : state->pages[pageIndex]);
                return 0;
            }
        }
        break;
    case WM_SIZE: {
        if (!state) break;
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const int leftW = 410;
        const int margin = 10;
        const int statusH = 22;
        const int logH = 170;
        const int buttonH = 26;
        const int buttonGap = 6;
        const int actionRows = static_cast<int>((state->actionButtons.size() + 1) / 2);
        const int actionH = actionRows * buttonH + std::max(0, actionRows - 1) * buttonGap;
        const int actionTop = rc.bottom - statusH - logH - actionH - margin * 2;
        const int tabTop = margin;
        MoveWindow(state->tab, margin, tabTop, leftW, actionTop - tabTop, TRUE);
        RECT tabRect{};
        GetWindowRect(state->tab, &tabRect);
        RECT pageRect{ margin + 8, tabTop + 32, margin + leftW - 8, actionTop - margin };
        TabCtrl_AdjustRect(state->tab, FALSE, &pageRect);
        for (HWND page : state->pages) {
            MoveWindow(page, pageRect.left, pageRect.top, pageRect.right - pageRect.left, pageRect.bottom - pageRect.top, TRUE);
            GuiUpdatePageScrollBar(state, page);
        }
        const int buttonW = (leftW - margin) / 2 - margin;
        for (size_t i = 0; i < state->actionButtons.size(); ++i) {
            int col = static_cast<int>(i % 2);
            int row = static_cast<int>(i / 2);
            MoveWindow(state->actionButtons[i], margin + col * (buttonW + margin), actionTop + row * (buttonH + buttonGap), buttonW, buttonH, TRUE);
        }

        MoveWindow(state->log, margin, rc.bottom - statusH - logH - margin, leftW, logH, TRUE);
        const int previewX = leftW + margin * 2;
        const int previewW = rc.right - leftW - margin * 3;
        const int previewToolbarCols = 4;
        const int previewToolbarRows = state->previewButtons.empty() ? 0 : static_cast<int>((state->previewButtons.size() + previewToolbarCols - 1) / previewToolbarCols);
        const int previewToolbarH = previewToolbarRows > 0 ? (previewToolbarRows * buttonH + std::max(0, previewToolbarRows - 1) * buttonGap) : 0;
        const int previewInfoH = 20;
        const int previewHintH = 20;
        const int previewRendererInfoH = 20;
        const int previewPanelTop = margin;
        const int previewPanelBottom = rc.bottom - statusH - margin;
        const int previewInnerTop = previewPanelTop + previewToolbarH + (previewToolbarH > 0 ? buttonGap : 0);
        const int previewInfoTop = previewPanelBottom - previewInfoH - previewHintH - previewRendererInfoH - buttonGap;
        const int previewHeight = std::max(120, previewInfoTop - previewInnerTop - buttonGap);

        const int previewButtonW = std::max(88, (previewW - (previewToolbarCols - 1) * buttonGap) / previewToolbarCols);
        for (size_t i = 0; i < state->previewButtons.size(); ++i) {
            const int col = static_cast<int>(i % previewToolbarCols);
            const int row = static_cast<int>(i / previewToolbarCols);
            MoveWindow(state->previewButtons[i], previewX + col * (previewButtonW + buttonGap), previewPanelTop + row * (buttonH + buttonGap), previewButtonW, buttonH, TRUE);
        }

        const int inspectorGap = 10;
        const int inspectorW = std::clamp(previewW / 4, 240, 300);
        const int previewCanvasW = std::max(220, previewW - inspectorW - inspectorGap);
        MoveWindow(state->preview, previewX, previewInnerTop, previewCanvasW, previewHeight, TRUE);
        MoveWindow(state->previewInspector, previewX + previewCanvasW + inspectorGap, previewInnerTop, inspectorW, previewHeight, TRUE);
        MoveWindow(state->previewInfoLabel, previewX, previewInfoTop, previewW, previewInfoH, TRUE);
        MoveWindow(state->previewHintLabel, previewX, previewInfoTop + previewInfoH, previewW, previewHintH, TRUE);
        MoveWindow(state->previewRendererLabel, previewX, previewInfoTop + previewInfoH + previewHintH, previewW, previewRendererInfoH, TRUE);

        RECT inspectorRc{};
        GetClientRect(state->previewInspector, &inspectorRc);
        const int inner = 12;
        const int labelW = std::max(140L, inspectorRc.right - inner * 2);
        const int trackW = std::max(140L, inspectorRc.right - inner * 2);
        const int halfButtonW = std::max(68L, (inspectorRc.right - inner * 2 - buttonGap) / 2);
        const int inspectorX = inner;
        const int inspectorScroll = state->pageScrollPos[state->previewInspector];
        auto inspectorTop = [&](int y) { return y - inspectorScroll; };
        int inspectorY = inner;
        MoveWindow(state->previewRotationModeCombo, inspectorX, inspectorTop(inspectorY), labelW, 120, TRUE);
        inspectorY += 30;
        const bool quaternionMode = ToLower(GuiReadMappedText(state, L"rotationMode", L"euler")) == L"quaternion";
        for (int i = quaternionMode ? 0 : 1; i < 4; ++i) {
            MoveWindow(state->previewRotationLabels[i], inspectorX, inspectorTop(inspectorY) + 2, 72, 20, TRUE);
            MoveWindow(state->previewRotationEdits[i], inspectorX + 76, inspectorTop(inspectorY), labelW - 76, 22, TRUE);
            inspectorY += 26;
        }
        MoveWindow(state->previewLevelNormalLabel, inspectorX, inspectorTop(inspectorY) + 2, 72, 20, TRUE);
        MoveWindow(state->previewLevelNormalEdit, inspectorX + 76, inspectorTop(inspectorY), labelW - 76, 22, TRUE);
        inspectorY += 26;
        MoveWindow(state->previewLevelButton, inspectorX, inspectorTop(inspectorY), labelW, buttonH, TRUE);
        inspectorY += buttonH + 10;

        MoveWindow(state->previewScaleLabel, inspectorX, inspectorTop(inspectorY) + 2, 86, 20, TRUE);
        MoveWindow(state->previewScaleEdit, inspectorX + 90, inspectorTop(inspectorY), labelW - 90, 22, TRUE);
        inspectorY += 22;
        MoveWindow(state->previewScaleTrack, inspectorX, inspectorTop(inspectorY), trackW, 30, TRUE);
        inspectorY += 34;
        MoveWindow(GetDlgItem(state->previewInspector, IDC_GUI_TRANSFORM_SCALE_DOWN), inspectorX, inspectorTop(inspectorY), halfButtonW, buttonH, TRUE);
        MoveWindow(GetDlgItem(state->previewInspector, IDC_GUI_TRANSFORM_SCALE_UP), inspectorX + halfButtonW + buttonGap, inspectorTop(inspectorY), halfButtonW, buttonH, TRUE);
        inspectorY += buttonH + 12;

        MoveWindow(state->previewBaseYLabel, inspectorX, inspectorTop(inspectorY) + 2, 86, 20, TRUE);
        MoveWindow(state->previewBaseYEdit, inspectorX + 90, inspectorTop(inspectorY), labelW - 90, 22, TRUE);
        inspectorY += 22;
        MoveWindow(state->previewBaseYTrack, inspectorX, inspectorTop(inspectorY), trackW, 30, TRUE);
        inspectorY += 34;
        MoveWindow(GetDlgItem(state->previewInspector, IDC_GUI_TRANSFORM_BASEY_DOWN), inspectorX, inspectorTop(inspectorY), halfButtonW, buttonH, TRUE);
        MoveWindow(GetDlgItem(state->previewInspector, IDC_GUI_TRANSFORM_BASEY_UP), inspectorX + halfButtonW + buttonGap, inspectorTop(inspectorY), halfButtonW, buttonH, TRUE);
        inspectorY += buttonH + 14;

        MoveWindow(state->previewFlipXCheck, inspectorX, inspectorTop(inspectorY), halfButtonW, 22, TRUE);
        MoveWindow(state->previewFlipZCheck, inspectorX + halfButtonW + buttonGap, inspectorTop(inspectorY), halfButtonW, 22, TRUE);
        inspectorY += 28;
        MoveWindow(state->previewTransformButton, inspectorX, inspectorTop(inspectorY), inspectorRc.right - inner * 2, buttonH, TRUE);
        inspectorY += buttonH + 10;
        MoveWindow(GetDlgItem(state->previewInspector, IDC_GUI_TRANSFORM_RESET), inspectorX, inspectorTop(inspectorY), inspectorRc.right - inner * 2, buttonH, TRUE);
        inspectorY += buttonH + 14;
        MoveWindow(state->previewPlaceXLabel, inspectorX, inspectorTop(inspectorY), labelW, 20, TRUE);
        inspectorY += 22;
        MoveWindow(GetDlgItem(state->previewInspector, IDC_GUI_PLACE_X_LEFT), inspectorX, inspectorTop(inspectorY), halfButtonW, buttonH, TRUE);
        MoveWindow(GetDlgItem(state->previewInspector, IDC_GUI_PLACE_X_RIGHT), inspectorX + halfButtonW + buttonGap, inspectorTop(inspectorY), halfButtonW, buttonH, TRUE);
        inspectorY += buttonH + 10;
        MoveWindow(state->previewPlaceZLabel, inspectorX, inspectorTop(inspectorY), labelW, 20, TRUE);
        inspectorY += 22;
        MoveWindow(GetDlgItem(state->previewInspector, IDC_GUI_PLACE_Z_LEFT), inspectorX, inspectorTop(inspectorY), halfButtonW, buttonH, TRUE);
        MoveWindow(GetDlgItem(state->previewInspector, IDC_GUI_PLACE_Z_RIGHT), inspectorX + halfButtonW + buttonGap, inspectorTop(inspectorY), halfButtonW, buttonH, TRUE);
        inspectorY += buttonH + 10;
        MoveWindow(state->previewPlaceYLabel, inspectorX, inspectorTop(inspectorY), labelW, 20, TRUE);
        inspectorY += 22;
        MoveWindow(GetDlgItem(state->previewInspector, IDC_GUI_PLACE_Y_DOWN), inspectorX, inspectorTop(inspectorY), halfButtonW, buttonH, TRUE);
        MoveWindow(GetDlgItem(state->previewInspector, IDC_GUI_PLACE_Y_UP), inspectorX + halfButtonW + buttonGap, inspectorTop(inspectorY), halfButtonW, buttonH, TRUE);
        inspectorY += buttonH + 10;
        MoveWindow(GetDlgItem(state->previewInspector, IDC_GUI_PLACE_CLEAR_CENTER), inspectorX, inspectorTop(inspectorY), inspectorRc.right - inner * 2, buttonH, TRUE);
        inspectorY += buttonH + 8;
        MoveWindow(GetDlgItem(state->previewInspector, IDC_GUI_PLACE_ORIGIN_CENTER), inspectorX, inspectorTop(inspectorY), inspectorRc.right - inner * 2, buttonH, TRUE);
        inspectorY += buttonH + 14;
        for (int i = 0; i < 3; ++i) {
            const int labelId = IDC_GUI_PRESET_A_LABEL + i * 3;
            const int saveId = IDC_GUI_PRESET_A_SAVE + i * 3;
            const int loadId = IDC_GUI_PRESET_A_LOAD + i * 3;
            MoveWindow(GetDlgItem(state->previewInspector, labelId), inspectorX, inspectorTop(inspectorY), labelW, 18, TRUE);
            inspectorY += 20;
            MoveWindow(GetDlgItem(state->previewInspector, saveId), inspectorX, inspectorTop(inspectorY), halfButtonW, buttonH, TRUE);
            MoveWindow(GetDlgItem(state->previewInspector, loadId), inspectorX + halfButtonW + buttonGap, inspectorTop(inspectorY), halfButtonW, buttonH, TRUE);
            inspectorY += buttonH + 8;
        }
        state->pageContentHeight[state->previewInspector] = inspectorY + inner;
        GuiUpdatePageScrollBar(state, state->previewInspector);
        GuiUpdatePreviewChrome(state);
        GuiRenderPreviewNow(state);
        MoveWindow(state->status, margin, rc.bottom - statusH, rc.right - margin * 2, statusH, TRUE);
        return 0;
    }
    case WM_HSCROLL: {
        if (!state) break;
        const HWND sender = reinterpret_cast<HWND>(lParam);
        if (sender == state->previewRotateTrack) {
            GuiRecordPreviewHistory(state);
            const int pos = static_cast<int>(SendMessageW(state->previewRotateTrack, TBM_GETPOS, 0, 0));
            GuiWriteMappedText(state, L"rotateYDeg", FormatFloatCompact(PreviewTrackToRotate(pos), 0));
            GuiApplyPreviewTransformState(state);
            GuiRecordPreviewHistory(state);
            return 0;
        }
        if (sender == state->previewScaleTrack) {
            GuiRecordPreviewHistory(state);
            const int pos = static_cast<int>(SendMessageW(state->previewScaleTrack, TBM_GETPOS, 0, 0));
            GuiWriteMappedText(state, L"scaleBlocksPerMeter", FormatFloatCompact(PreviewTrackToScale(pos), 1));
            GuiApplyPreviewTransformState(state);
            GuiRecordPreviewHistory(state);
            return 0;
        }
        if (sender == state->previewBaseYTrack) {
            GuiRecordPreviewHistory(state);
            const int pos = static_cast<int>(SendMessageW(state->previewBaseYTrack, TBM_GETPOS, 0, 0));
            GuiWriteMappedText(state, L"baseY", std::to_wstring(PreviewTrackToBaseY(pos)));
            GuiApplyPreviewTransformState(state);
            GuiRecordPreviewHistory(state);
            return 0;
        }
        break;
    }
    case WM_NOTIFY: {
        if (state && reinterpret_cast<LPNMHDR>(lParam)->idFrom == IDC_GUI_TAB && reinterpret_cast<LPNMHDR>(lParam)->code == TCN_SELCHANGE) {
            int index = TabCtrl_GetCurSel(state->tab);
            for (size_t i = 0; i < state->pages.size(); ++i) {
                ShowWindow(state->pages[i], static_cast<int>(i) == index ? SW_SHOW : SW_HIDE);
            }
            return 0;
        }
        break;
    }
    case WM_DROPFILES: {
        if (!state) break;
        HDROP drop = reinterpret_cast<HDROP>(wParam);
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        bool modelChanged = false;
        bool worldChanged = false;
        for (UINT index = 0; index < count; ++index) {
            const UINT length = DragQueryFileW(drop, index, nullptr, 0);
            std::wstring buffer(length + 1, L'\0');
            DragQueryFileW(drop, index, buffer.data(), length + 1);
            buffer.resize(length);
            const fs::path dropped(buffer);
            std::error_code ec;
            if (fs::is_regular_file(dropped, ec)) {
                const std::wstring ext = ToLower(dropped.extension().wstring());
                if (ext == L".obj" || ext == L".glb") {
                    GuiSetControlText(state->edits[L"inputPath"], dropped.wstring());
                    GuiSetControlText(state->edits[L"onlyFile"], L"");
                    const std::wstring currentMode = ToLower(GuiReadMappedText(state, L"lastMode", L""));
                    state->config.lastMode = ext == L".obj"
                        ? (currentMode == L"obj-surface" ? L"obj-surface" : L"obj")
                        : (currentMode == L"glb-surface" ? L"glb-surface" : L"glb-textured");
                    GuiWriteMappedText(state, L"lastMode", state->config.lastMode);
                    modelChanged = true;
                }
            } else if (fs::is_directory(dropped, ec)) {
                if (fs::exists(dropped / "level.dat", ec)) {
                    GuiSetControlText(state->edits[L"worldDir"], dropped.wstring());
                    GuiSetControlText(state->edits[L"worldName"], dropped.filename().wstring());
                    worldChanged = true;
                } else {
                    const auto glbFiles = CollectFilesByExtension(dropped, L".glb");
                    const auto objFiles = CollectFilesByExtension(dropped, L".obj");
                    if (!glbFiles.empty() || !objFiles.empty()) {
                        GuiSetControlText(state->edits[L"inputPath"], dropped.wstring());
                        GuiSetControlText(state->edits[L"onlyFile"], L"");
                        const std::wstring currentMode = ToLower(GuiReadMappedText(state, L"lastMode", L""));
                        state->config.lastMode = !glbFiles.empty()
                            ? (currentMode == L"glb-surface" ? L"glb-surface" : L"glb-textured")
                            : (currentMode == L"obj-surface" ? L"obj-surface" : L"obj");
                        GuiWriteMappedText(state, L"lastMode", state->config.lastMode);
                        modelChanged = true;
                    }
                }
            }
        }
        DragFinish(drop);
        state->config = GuiReadConfig(state);
        SaveConfig(state->configPath, state->config);
        if (modelChanged || worldChanged) GuiTryAutoPreview(state, worldChanged);
        else GuiSetStatus(state, L"未识别：请拖入 OBJ、GLB、模型目录或 Minecraft 存档目录");
        return 0;
    }
    case WM_COMMAND: {
        if (!state) break;
        if (state->populatingControls) return 0;
        const HWND sender = reinterpret_cast<HWND>(lParam);
        if (sender == state->edits[L"lang"] && HIWORD(wParam) == CBN_SELCHANGE) {
            state->config = GuiReadConfig(state);
            SaveConfig(state->configPath, state->config);
            MessageBoxW(hwnd,
                state->config.lang == L"en"
                    ? L"Language saved. The application will restart now to apply it to every page."
                    : L"语言设置已保存。程序将立即重启，使所有页面完整生效。",
                state->config.lang == L"en" ? L"Apply language" : L"应用语言",
                MB_OK | MB_ICONINFORMATION);
            if (!GuiRestartApplication(state)) {
                MessageBoxW(hwnd,
                    state->config.lang == L"en"
                        ? L"Automatic restart failed. Please restart the application manually."
                        : L"自动重启失败，请手动重新启动程序。",
                    state->config.lang == L"en" ? L"Restart failed" : L"重启失败",
                    MB_OK | MB_ICONERROR);
            }
            return 0;
        }
        if (sender && HIWORD(wParam) == BN_CLICKED) {
            if (sender == state->checks[L"direct"]) {
                const bool direct = GuiGetCheck(sender);
                GuiSetCheck(state->checks[L"copyWorld"], !direct);
                GuiUpdateWorkflow(state);
                return 0;
            }
            if (sender == state->checks[L"copyWorld"]) {
                const bool copy = GuiGetCheck(sender);
                GuiSetCheck(state->checks[L"direct"], !copy);
                if (!copy) GuiSetCheck(sender, true);
                GuiUpdateWorkflow(state);
                return 0;
            }
        }
        if (sender && HIWORD(wParam) == EN_CHANGE) {
            static const std::set<std::wstring> readinessKeys = {
                L"inputPath", L"objDir", L"glbDir", L"onlyFile", L"worldDir",
                L"savesDir", L"mcRoot", L"worldName", L"mcVersion"
            };
            const auto entry = std::find_if(state->edits.begin(), state->edits.end(),
                [&](const auto& item) { return item.second == sender; });
            if (entry != state->edits.end() && readinessKeys.find(entry->first) != readinessKeys.end()) GuiUpdateWorkflow(state);
            static const std::set<std::wstring> realtimeTransformKeys = {
                L"rotationW", L"rotationX", L"rotationY", L"rotationZ",
                L"levelNormal", L"scaleBlocksPerMeter", L"baseY"
            };
            if (entry != state->edits.end() && realtimeTransformKeys.find(entry->first) != realtimeTransformKeys.end()) {
                if (entry->first == L"rotationY" && ToLower(GuiReadMappedText(state, L"rotationMode", L"euler")) == L"euler") {
                    GuiWriteMappedText(state, L"rotateYDeg", GuiReadMappedText(state, L"rotationY", L"0"));
                }
                GuiApplyPreviewTransformState(state);
                return 0;
            }
        }
        if (sender == state->previewRotationModeCombo && HIWORD(wParam) == CBN_SELCHANGE) {
            GuiRecordPreviewHistory(state);
            const std::wstring nextMode = ToLower(GuiReadMappedText(state, L"rotationMode", L"euler"));
            GuiConvertRotationMode(state, nextMode);
            GuiApplyPreviewTransformState(state);
            state->config = GuiReadConfig(state);
            SaveConfig(state->configPath, state->config);
            GuiRecordPreviewHistory(state);
            SendMessageW(hwnd, WM_SIZE, 0, 0);
            return 0;
        }
        if (sender && sender == state->edits[L"previewWorldChunks"] && HIWORD(wParam) == EN_KILLFOCUS) {
            state->config = GuiReadConfig(state);
            state->previewWorldRadius = state->config.previewWorldRadius;
            GuiSetControlText(sender, std::to_wstring(std::max(1, state->previewWorldRadius / 16)));
            GuiPersistPreviewWorldSettings(state);
            GuiUpdatePreviewChrome(state);
            GuiScheduleWorldPreviewRefresh(state);
            return 0;
        }
        if (sender && HIWORD(wParam) == EN_KILLFOCUS &&
            (sender == state->edits[L"previewOrbitSensitivity"] ||
             sender == state->edits[L"previewPanSensitivity"] ||
             sender == state->edits[L"previewZoomSensitivity"])) {
            const wchar_t* key = sender == state->edits[L"previewOrbitSensitivity"] ? L"previewOrbitSensitivity" :
                (sender == state->edits[L"previewPanSensitivity"] ? L"previewPanSensitivity" : L"previewZoomSensitivity");
            const float fallback = key == std::wstring(L"previewPanSensitivity") ? 2.0f : 1.5f;
            const float value = std::clamp(GuiReadMappedFloat(state, key, fallback), 0.05f, 100.0f);
            GuiSetControlText(sender, FormatFloatCompact(value, 2));
            state->config = GuiReadConfig(state);
            SaveConfig(state->configPath, state->config);
            return 0;
        }
        if (sender && sender == state->edits[L"levelNormal"] && HIWORD(wParam) == EN_KILLFOCUS) {
            state->config = GuiReadConfig(state);
            SaveConfig(state->configPath, state->config);
            GuiRenderPreviewNow(state);
            GuiScheduleWorldPreviewRefresh(state);
            return 0;
        }
        if (sender && HIWORD(wParam) == EN_KILLFOCUS &&
            (sender == state->edits[L"inputPath"] || sender == state->edits[L"worldDir"])) {
            GuiTryAutoPreview(state, sender == state->edits[L"worldDir"]);
            return 0;
        }
        switch (LOWORD(wParam)) {
        case IDC_GUI_INPUT_FILE_PICKER: {
            const auto picked = GuiBrowseFile(hwnd, L"3D Models (*.obj;*.glb)\0*.obj;*.glb\0All Files (*.*)\0*.*\0");
            if (!picked) return 0;
            GuiSetControlText(state->edits[L"inputPath"], picked->wstring());
            GuiSetControlText(state->edits[L"onlyFile"], L"");
            const std::wstring currentMode = ToLower(GuiReadMappedText(state, L"lastMode", L""));
            state->config.lastMode = IEquals(picked->extension().wstring(), L".obj")
                ? (currentMode == L"obj-surface" ? L"obj-surface" : L"obj")
                : (currentMode == L"glb-surface" ? L"glb-surface" : L"glb-textured");
            GuiWriteMappedText(state, L"lastMode", state->config.lastMode);
            GuiTryAutoPreview(state, false);
            return 0;
        }
        case IDC_GUI_INPUT_DIR_PICKER: {
            const auto picked = GuiBrowseFolder(hwnd, L"选择包含 OBJ / GLB 模型的目录");
            if (!picked) return 0;
            GuiSetControlText(state->edits[L"inputPath"], picked->wstring());
            GuiSetControlText(state->edits[L"onlyFile"], L"");
            const auto glbFiles = CollectFilesByExtension(*picked, L".glb");
            const auto objFiles = CollectFilesByExtension(*picked, L".obj");
            const std::wstring currentMode = ToLower(GuiReadMappedText(state, L"lastMode", L""));
            state->config.lastMode = !glbFiles.empty()
                ? (currentMode == L"glb-surface" ? L"glb-surface" : L"glb-textured")
                : (currentMode == L"obj-surface" ? L"obj-surface" : L"obj");
            GuiWriteMappedText(state, L"lastMode", state->config.lastMode);
            if (!glbFiles.empty() || !objFiles.empty()) GuiTryAutoPreview(state, false);
            else GuiSetStatus(state, L"所选目录中没有找到 OBJ 或 GLB 文件");
            return 0;
        }
        case IDC_GUI_WORLD_DIR_PICKER: {
            const auto picked = GuiBrowseFolder(hwnd, L"选择 Minecraft 存档（文件夹内应包含 level.dat）");
            if (!picked) return 0;
            std::error_code ec;
            if (!fs::exists(*picked / "level.dat", ec)) {
                MessageBoxW(hwnd, L"这个文件夹不是 Minecraft 存档。\n请选择 saves 目录中包含 level.dat 的具体存档文件夹。",
                    L"未找到 level.dat", MB_OK | MB_ICONWARNING);
                return 0;
            }
            GuiSetControlText(state->edits[L"worldDir"], picked->wstring());
            GuiSetControlText(state->edits[L"worldName"], picked->filename().wstring());
            GuiTryAutoPreview(state, true);
            return 0;
        }
        case IDC_GUI_ACTION_SAVE:
            state->config = GuiReadConfig(state);
            SaveConfig(state->configPath, state->config);
            GuiSetStatus(state, T(state->lang, L"配置已保存", L"Config saved"));
            InvalidateRect(state->preview, nullptr, TRUE);
            return 0;
        case IDC_GUI_ACTION_LOAD:
            state->config = LoadConfig(state->configPath);
            GuiPopulateControls(state, state->config);
            GuiResetPreviewHistory(state);
            GuiSetStatus(state, T(state->lang, L"配置已重载", L"Config reloaded"));
            InvalidateRect(state->preview, nullptr, TRUE);
            return 0;
        case IDC_GUI_ACTION_DOCTOR:
            GuiRunCommandAsync(state, L"doctor");
            return 0;
        case IDC_GUI_ACTION_COPY_WORLD:
            GuiRunCommandAsync(state, L"copy-world");
            return 0;
        case IDC_GUI_ACTION_GLB:
            state->config = GuiReadConfig(state);
            if (const std::wstring command = ToLower(state->config.lastMode) == L"glb-surface" ? L"glb-surface" : L"glb";
                GuiConfirmCommand(state, command)) GuiRunCommandAsync(state, command);
            return 0;
        case IDC_GUI_ACTION_OBJ:
            state->config = GuiReadConfig(state);
            if (const std::wstring command = ToLower(state->config.lastMode) == L"obj-surface" ? L"obj-surface" : L"obj";
                GuiConfirmCommand(state, command)) GuiRunCommandAsync(state, command);
            return 0;
        case IDC_GUI_ACTION_SCAN:
            GuiRunCommandAsync(state, L"scan");
            return 0;
        case IDC_GUI_ACTION_RESET:
            if (GuiConfirmCommand(state, L"reset")) GuiRunCommandAsync(state, L"reset");
            return 0;
        case IDC_GUI_ACTION_HELP:
            GuiRunCommandAsync(state, L"help");
            return 0;
        case IDC_GUI_ACTION_PREVIEW_LOAD:
            GuiLoadPreviewAsync(state);
            return 0;
        case IDC_GUI_ACTION_PALETTE_EDITOR:
            GuiOpenPaletteDialog(state);
            return 0;
        case IDC_GUI_ACTION_BREAKDOWN:
            GuiOpenBreakdownDialog(state);
            return 0;
        case IDC_GUI_PREVIEW_FIT:
            GuiComputePreviewFit(state);
            GuiUpdatePreviewChrome(state);
            InvalidateRect(state->preview, nullptr, FALSE);
            return 0;
        case IDC_GUI_PREVIEW_VIEW_TOP:
            GuiSetPreviewPreset(state, 0.0f, -89.0f);
            return 0;
        case IDC_GUI_PREVIEW_VIEW_FRONT:
            GuiSetPreviewPreset(state, 0.0f, 0.0f);
            return 0;
        case IDC_GUI_PREVIEW_VIEW_LEFT:
            GuiSetPreviewPreset(state, -90.0f, 0.0f);
            return 0;
        case IDC_GUI_PREVIEW_VIEW_RIGHT:
            GuiSetPreviewPreset(state, 90.0f, 0.0f);
            return 0;
        case IDC_GUI_PREVIEW_TOOL_CAMERA:
            GuiSetPreviewToolMode(state, PreviewToolMode::Camera);
            return 0;
        case IDC_GUI_PREVIEW_TOOL_PLACE:
            GuiSetPreviewToolMode(state, PreviewToolMode::Place);
            return 0;
        case IDC_GUI_PREVIEW_SNAP:
            state->previewSnapStep = PreviewNextSnapStep(state->previewSnapStep);
            GuiUpdatePreviewChrome(state);
            GuiRenderPreviewNow(state);
            return 0;
        case IDC_GUI_PREVIEW_WORLD:
            state->previewWorldVisible = !state->previewWorldVisible;
            GuiPersistPreviewWorldSettings(state);
            GuiUpdatePreviewChrome(state);
            GuiRenderPreviewNow(state);
            if (state->previewWorldVisible) GuiScheduleWorldPreviewRefresh(state);
            return 0;
        case IDC_GUI_PREVIEW_WORLD_REFRESH:
            GuiRefreshWorldPreviewAsync(state);
            return 0;
        case IDC_GUI_PREVIEW_WORLD_RADIUS:
            state->previewWorldRadius = PreviewNextWorldRadius(state->previewWorldRadius);
            GuiSetControlText(state->edits[L"previewWorldChunks"], std::to_wstring(std::max(1, state->previewWorldRadius / 16)));
            GuiPersistPreviewWorldSettings(state);
            GuiUpdatePreviewChrome(state);
            GuiScheduleWorldPreviewRefresh(state);
            return 0;
        case IDC_GUI_PREVIEW_WORLD_STYLE:
            if (state->previewWorldStyle == PreviewWorldStyle::Faces) state->previewWorldStyle = PreviewWorldStyle::Points;
            else if (state->previewWorldStyle == PreviewWorldStyle::Points) state->previewWorldStyle = PreviewWorldStyle::Voxels;
            else state->previewWorldStyle = PreviewWorldStyle::Faces;
            GuiScheduleWorldGpuBuild(state);
            GuiPersistPreviewWorldSettings(state);
            GuiUpdatePreviewChrome(state);
            GuiRenderPreviewNow(state);
            return 0;
        case IDC_GUI_PREVIEW_UNDO:
            GuiPreviewUndo(state);
            return 0;
        case IDC_GUI_PREVIEW_REDO:
            GuiPreviewRedo(state);
            return 0;
        case IDC_GUI_MODEL_REFRESH:
            GuiRefreshModelList(state);
            return 0;
        case IDC_GUI_MODEL_PREV:
            GuiSelectAdjacentModel(state, -1);
            return 0;
        case IDC_GUI_MODEL_NEXT:
            GuiSelectAdjacentModel(state, 1);
            return 0;
        case IDC_GUI_MODEL_USE: {
            GuiLoadSelectedPreviewsAsync(state);
            return 0;
        }
        case IDC_GUI_MODEL_SHOW_ALL:
            GuiLoadAllPreviewsAsync(state);
            return 0;
        case IDC_GUI_MODEL_LEVEL_REFERENCE:
        case IDC_GUI_LEVEL_APPLY:
            GuiEstimateLevelFromSelected(state);
            return 0;
        case IDC_GUI_PRESET_A_SAVE:
        case IDC_GUI_PRESET_B_SAVE:
        case IDC_GUI_PRESET_C_SAVE:
            GuiSavePreviewPreset(state, (LOWORD(wParam) - IDC_GUI_PRESET_A_SAVE) / 3);
            return 0;
        case IDC_GUI_PRESET_A_LOAD:
        case IDC_GUI_PRESET_B_LOAD:
        case IDC_GUI_PRESET_C_LOAD:
            GuiLoadPreviewPreset(state, (LOWORD(wParam) - IDC_GUI_PRESET_A_LOAD) / 3);
            return 0;
        case IDC_GUI_PREVIEW_MODE:
            GuiCyclePreviewDrawMode(state);
            return 0;
        case IDC_GUI_PREVIEW_LOD:
            state->previewForcedLod = state->previewForcedLod >= 3 ? -1 : state->previewForcedLod + 1;
            state->previewAdaptiveLodBias = 0;
            state->previewFrameMs = 0.0;
            GuiUpdatePreviewChrome(state);
            GuiRenderPreviewNow(state);
            return 0;
        case IDC_GUI_TRANSFORM_ROTATE_LEFT:
            GuiRecordPreviewHistory(state);
            GuiWriteMappedText(state, L"rotateYDeg", FormatFloatCompact(GuiReadMappedFloat(state, L"rotateYDeg", 0.0f) - 90.0f, 0));
            GuiApplyPreviewTransformState(state);
            GuiRecordPreviewHistory(state);
            return 0;
        case IDC_GUI_TRANSFORM_ROTATE_RIGHT:
            GuiRecordPreviewHistory(state);
            GuiWriteMappedText(state, L"rotateYDeg", FormatFloatCompact(GuiReadMappedFloat(state, L"rotateYDeg", 0.0f) + 90.0f, 0));
            GuiApplyPreviewTransformState(state);
            GuiRecordPreviewHistory(state);
            return 0;
        case IDC_GUI_TRANSFORM_SCALE_DOWN:
            GuiRecordPreviewHistory(state);
            GuiWriteMappedText(state, L"scaleBlocksPerMeter", FormatFloatCompact(std::max(0.1f, GuiReadMappedFloat(state, L"scaleBlocksPerMeter", 4.0f) - 0.5f), 1));
            GuiApplyPreviewTransformState(state);
            GuiRecordPreviewHistory(state);
            return 0;
        case IDC_GUI_TRANSFORM_SCALE_UP:
            GuiRecordPreviewHistory(state);
            GuiWriteMappedText(state, L"scaleBlocksPerMeter", FormatFloatCompact(std::min(16.0f, GuiReadMappedFloat(state, L"scaleBlocksPerMeter", 4.0f) + 0.5f), 1));
            GuiApplyPreviewTransformState(state);
            GuiRecordPreviewHistory(state);
            return 0;
        case IDC_GUI_TRANSFORM_BASEY_DOWN:
            GuiRecordPreviewHistory(state);
            GuiWriteMappedText(state, L"baseY", std::to_wstring(GuiReadMappedInt(state, L"baseY", 0) - 8));
            GuiApplyPreviewTransformState(state);
            GuiRecordPreviewHistory(state);
            return 0;
        case IDC_GUI_TRANSFORM_BASEY_UP:
            GuiRecordPreviewHistory(state);
            GuiWriteMappedText(state, L"baseY", std::to_wstring(GuiReadMappedInt(state, L"baseY", 0) + 8));
            GuiApplyPreviewTransformState(state);
            GuiRecordPreviewHistory(state);
            return 0;
        case IDC_GUI_TRANSFORM_FLIPX:
            GuiRecordPreviewHistory(state);
            GuiSetMappedCheckValue(state, L"flipX", GuiGetCheck(state->previewFlipXCheck));
            GuiApplyPreviewTransformState(state);
            GuiRecordPreviewHistory(state);
            return 0;
        case IDC_GUI_TRANSFORM_FLIPZ:
            GuiRecordPreviewHistory(state);
            GuiSetMappedCheckValue(state, L"flipZ", GuiGetCheck(state->previewFlipZCheck));
            GuiApplyPreviewTransformState(state);
            GuiRecordPreviewHistory(state);
            return 0;
        case IDC_GUI_TRANSFORM_SYSTEM: {
            GuiRecordPreviewHistory(state);
            const std::wstring current = ToLower(GuiReadMappedText(state, L"transform", L""));
            GuiWriteMappedText(state, L"transform", current == L"obj-z-up" ? L"default" : L"obj-z-up");
            GuiApplyPreviewTransformState(state);
            GuiRecordPreviewHistory(state);
            return 0;
        }
        case IDC_GUI_TRANSFORM_RESET:
            GuiRecordPreviewHistory(state);
            GuiWriteMappedText(state, L"rotateYDeg", L"0");
            GuiWriteMappedText(state, L"rotationMode", L"euler");
            GuiWriteMappedText(state, L"rotationW", L"1");
            GuiWriteMappedText(state, L"rotationX", L"0");
            GuiWriteMappedText(state, L"rotationY", L"0");
            GuiWriteMappedText(state, L"rotationZ", L"0");
            GuiWriteMappedText(state, L"levelNormal", L"");
            GuiWriteMappedText(state, L"scaleBlocksPerMeter", L"4");
            GuiWriteMappedText(state, L"baseY", L"0");
            GuiSetMappedCheckValue(state, L"flipX", false);
            GuiSetMappedCheckValue(state, L"flipZ", false);
            GuiWriteMappedText(state, L"transform", IsObjMode(ToLower(GuiReadMappedText(state, L"lastMode", L""))) ? L"obj-z-up" : L"");
            GuiApplyPreviewTransformState(state);
            GuiRecordPreviewHistory(state);
            return 0;
        case IDC_GUI_PLACE_X_LEFT:
        case IDC_GUI_PLACE_X_RIGHT:
        case IDC_GUI_PLACE_Z_LEFT:
        case IDC_GUI_PLACE_Z_RIGHT:
        case IDC_GUI_PLACE_Y_DOWN:
        case IDC_GUI_PLACE_Y_UP:
        case IDC_GUI_PLACE_CLEAR_CENTER:
        case IDC_GUI_PLACE_ORIGIN_CENTER: {
            GuiRecordPreviewHistory(state);
            PreviewCenterInput center = GuiResolvePreviewCenterInput(state);
            const float scale = std::max(0.001f, GuiReadMappedFloat(state, L"scaleBlocksPerMeter", 4.0f));
            const float modelHeight = state->previewLoaded ? ((state->previewMesh.max[1] - state->previewMesh.min[1]) * scale) : 0.0f;
            const int baseY = GuiReadMappedInt(state, L"baseY", 0);
            switch (LOWORD(wParam)) {
            case IDC_GUI_PLACE_X_LEFT: center.x = center.x.value_or(0) - state->previewSnapStep; break;
            case IDC_GUI_PLACE_X_RIGHT: center.x = center.x.value_or(0) + state->previewSnapStep; break;
            case IDC_GUI_PLACE_Z_LEFT: center.z = center.z.value_or(0) - state->previewSnapStep; break;
            case IDC_GUI_PLACE_Z_RIGHT: center.z = center.z.value_or(0) + state->previewSnapStep; break;
            case IDC_GUI_PLACE_Y_DOWN: center.y = center.y.value_or(static_cast<int>(std::lround(baseY + modelHeight * 0.5f))) - state->previewSnapStep; break;
            case IDC_GUI_PLACE_Y_UP: center.y = center.y.value_or(static_cast<int>(std::lround(baseY + modelHeight * 0.5f))) + state->previewSnapStep; break;
            case IDC_GUI_PLACE_CLEAR_CENTER: center = PreviewCenterInput{}; break;
            case IDC_GUI_PLACE_ORIGIN_CENTER: center.x = 0; center.z = 0; if (center.y.has_value()) center.y = 0; break;
            default: break;
            }
            GuiWritePreviewCenterInput(state, center);
            GuiApplyPreviewTransformState(state);
            GuiRecordPreviewHistory(state);
            return 0;
        }
        default:
            if (LOWORD(wParam) >= IDC_GUI_BROWSE_BASE && state->browseMap.find(LOWORD(wParam)) != state->browseMap.end()) {
                const auto [key, isFolder] = state->browseMap[LOWORD(wParam)];
                std::optional<fs::path> picked;
                if (isFolder) {
                    picked = GuiBrowseFolder(hwnd, L"选择文件夹");
                } else {
                    picked = GuiBrowseFile(hwnd, L"3D Files (*.obj;*.glb)\0*.obj;*.glb\0All Files (*.*)\0*.*\0");
                }
                if (picked.has_value()) {
                    auto it = state->edits.find(key);
                    if (it != state->edits.end()) {
                        GuiSetControlText(it->second, picked->wstring());
                    }
                    GuiSetStatus(state, picked->wstring());
                    if (key == L"inputPath" || key == L"objDir" || key == L"glbDir" || key == L"onlyFile") {
                        GuiRefreshModelList(state);
                        GuiLoadPreviewAsync(state);
                    }
                    GuiUpdateWorkflow(state);
                }
                return 0;
            }
            InvalidateRect(state->preview, nullptr, TRUE);
            break;
        }
        if (HIWORD(wParam) == EN_CHANGE || HIWORD(wParam) == BN_CLICKED || HIWORD(wParam) == CBN_SELCHANGE) {
            if (LOWORD(wParam) == IDC_GUI_MODEL_FILTER) {
                GuiRefreshModelList(state);
                return 0;
            }
            const HWND sender = reinterpret_cast<HWND>(lParam);
            bool isConfigControl = false;
            std::wstring changedKey;
            for (const auto& [key, edit] : state->edits) {
                if (edit == sender) {
                    isConfigControl = true;
                    changedKey = key;
                    break;
                }
            }
            if (!isConfigControl) {
                for (const auto& [key, check] : state->checks) {
                    if (check == sender) {
                        isConfigControl = true;
                        changedKey = key;
                        break;
                    }
                }
            }
            if (isConfigControl) {
                if (changedKey == L"previewRefreshRate" && HIWORD(wParam) == CBN_SELCHANGE) {
                    const int refreshHz = NormalizePreviewRefreshRate(
                        GuiReadMappedInt(state, L"previewRefreshRate", 60));
                    state->config.previewRefreshRate = refreshHz;
                    state->previewRefreshHz.store(refreshHz, std::memory_order_release);
                    state->config = GuiReadConfig(state);
                    SaveConfig(state->configPath, state->config);
                }
                if (changedKey == L"previewRenderer" && HIWORD(wParam) == CBN_SELCHANGE) {
                    GuiSwitchPreviewRenderer(state, GuiReadMappedText(state, L"previewRenderer", L"auto"));
                }
                if ((changedKey == L"previewShowGrid" || changedKey == L"previewShowAxes") &&
                    HIWORD(wParam) == BN_CLICKED) {
                    state->previewShowGrid = GuiGetCheck(state->checks[L"previewShowGrid"]);
                    state->previewShowAxes = GuiGetCheck(state->checks[L"previewShowAxes"]);
                    state->config = GuiReadConfig(state);
                    SaveConfig(state->configPath, state->config);
                }
                if (changedKey == L"lastMode" || changedKey == L"inputPath" || changedKey == L"objDir" || changedKey == L"glbDir" || changedKey == L"onlyFile") {
                    GuiRefreshModelList(state);
                }
                GuiSyncPreviewTransformInspector(state);
                GuiUpdatePreviewChrome(state);
                InvalidateRect(state->preview, nullptr, FALSE);
                return 0;
            }
        }
        if (LOWORD(wParam) == IDC_GUI_MODEL_LIST) {
            if (HIWORD(wParam) == LBN_SELCHANGE) {
                GuiUpdateModelPanel(state);
                return 0;
            }
            if (HIWORD(wParam) == LBN_DBLCLK) {
                const int index = GuiGetPrimarySelectedModelIndex(state);
                if (index >= 0) GuiApplySelectedModel(state, index);
                return 0;
            }
        }
        break;
    }
    case WM_GUI_PREVIEW_PROGRESS: {
        if (state) {
            std::unique_ptr<PreviewProgressPayload> payload(reinterpret_cast<PreviewProgressPayload*>(lParam));
            if (payload) {
                if (state->previewProgressLabel) SetWindowTextW(state->previewProgressLabel, payload->message.c_str());
                if (state->previewProgressBar && payload->total > 0) {
                    SendMessageW(state->previewProgressBar, PBM_SETRANGE32, 0, payload->total);
                    SendMessageW(state->previewProgressBar, PBM_SETPOS, payload->completed, 0);
                }
            }
        }
        return 0;
    }
    case WM_GUI_LEVEL_READY: {
        std::unique_ptr<LevelReferencePayload> payload(reinterpret_cast<LevelReferencePayload*>(lParam));
        if (state) {
            if (payload && payload->generation != state->previewLevelGeneration) return 0;
            state->previewLoading = false;
            GuiClosePreviewProgress(state);
            fs::path selectedSource;
            const int selectedIndex = GuiGetPrimarySelectedModelIndex(state);
            if (selectedIndex >= 0 && selectedIndex < static_cast<int>(state->visibleModelFiles.size())) {
                selectedSource = state->visibleModelFiles[selectedIndex];
            }
            const std::wstring currentTransform = GuiReadMappedText(state, L"transform", L"default");
            const bool currentFlipX = GuiReadMappedCheckValue(state, L"flipX");
            const bool currentFlipZ = GuiReadEffectiveFlipZ(state);
            const bool stale = payload && (selectedSource.empty() ||
                !IEquals(NormalizeIfExists(selectedSource).wstring(), NormalizeIfExists(payload->source).wstring()) ||
                !IEquals(currentTransform, payload->transform) || currentFlipX != payload->flipX || currentFlipZ != payload->flipZ);
            if (stale) {
                const std::wstring message = L"找平结果已过期：模型、坐标系或 Flip 设置已改变，请重新计算。";
                GuiAppendLog(state, message + L"\r\n");
                GuiSetStatus(state, message);
                GuiUpdateWorkflow(state);
                GuiUpdatePreviewChrome(state);
                return 0;
            }
            if (payload && payload->ok) {
                GuiRecordPreviewHistory(state);
                if (!GuiWriteLevelRotationToControls(state, payload->normal)) {
                    GuiSetControlText(state->edits[L"levelNormal"], payload->normal);
                }
                state->config = GuiReadConfig(state);
                SaveConfig(state->configPath, state->config);
                GuiAppendLog(state, payload->message + L"\r\n");
                GuiApplyPreviewTransformState(state);
                GuiRecordPreviewHistory(state);
            }
            GuiSetStatus(state, payload ? payload->message : L"找平面计算失败");
            GuiUpdateWorkflow(state);
            GuiUpdatePreviewChrome(state);
        }
        return 0;
    }
    case WM_GUI_LOG: {
        if (state) {
            if (wParam == 1) {
                std::unique_ptr<native_mc::PreviewMesh> mesh(reinterpret_cast<native_mc::PreviewMesh*>(lParam));
                GuiReleasePreviewGpu(state);
                state->previewMesh = std::move(*mesh);
                state->previewGpuDirty = true;
                state->previewFrameMs = 0.0;
                state->previewAdaptiveLodBias = 0;
                state->previewFastFrameCount = 0;
                state->previewLodUpgradeCooldown = 0;
                state->previewSettleFrames = 90;
                state->previewLoaded = true;
                state->previewFrameActive.store(false, std::memory_order_release);
                GuiComputePreviewFit(state);
                GuiUpdatePreviewChrome(state);
                GuiUpdateModelPanel(state);
                if (!state->previewInfo.empty()) {
                    GuiAppendLog(state, state->previewInfo + L"\r\n");
                }
                InvalidateRect(state->preview, nullptr, FALSE);
                GuiRenderPreviewNow(state);
            } else {
                std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lParam));
                GuiAppendLog(state, *text);
            }
        }
        return 0;
    }
    case WM_GUI_PREVIEW_DONE:
        if (state) {
            if (state->previewLoadThread.joinable()) state->previewLoadThread.join();
            state->previewLoading = false;
            GuiClosePreviewProgress(state);
            std::unique_ptr<std::pair<bool, std::wstring>> payload(reinterpret_cast<std::pair<bool, std::wstring>*>(lParam));
            if (payload) {
                state->previewInfo = payload->second;
                GuiSetStatus(state, payload->second);
                if (!payload->first) {
                    state->previewLoaded = false;
                    state->previewFrameActive.store(false, std::memory_order_release);
                    InvalidateRect(state->preview, nullptr, FALSE);
                }
                GuiUpdatePreviewChrome(state);
                GuiUpdateWorkflow(state);
                GuiRenderPreviewNow(state);
                if (payload->first && state->previewWorldVisible) {
                    GuiRefreshWorldPreviewAsync(state);
                }
            }
        }
        return 0;
    case WM_GUI_DONE:
        if (state) {
            state->running = false;
            GuiSetStatus(state, wParam == 0 ? T(state->lang, L"执行完成", L"Done") : T(state->lang, L"执行失败", L"Failed"));
            GuiUpdateWorkflow(state);
        }
        return 0;
    case WM_GUI_WORLD_READY:
        if (state) {
            state->previewWorldLoading = false;
            std::unique_ptr<WorldPreviewPayload> payload(reinterpret_cast<WorldPreviewPayload*>(lParam));
            if (payload && payload->ok) {
                const bool firstWorldView = state->previewWorldSlice.blocks.empty();
                state->previewWorldSlice = std::move(payload->slice);
                state->previewWorldColors = std::move(payload->colors);
                ++state->previewWorldGpuGeneration;
                state->previewWorldGpuBuilding = false;
                if (payload->gpuMesh.style == state->previewWorldStyle) {
                    state->previewWorldPendingGpuMesh = std::move(payload->gpuMesh);
                    state->previewWorldGpuDirty = true;
                } else {
                    GuiScheduleWorldGpuBuild(state);
                }
                state->previewWorldSourceName = payload->worldLabel;
                state->previewWorldStatus = payload->message;
                GuiSetStatus(state, payload->message);
                if (firstWorldView) GuiComputePreviewFit(state);
            } else {
                state->previewWorldSlice.blocks.clear();
                state->previewWorldColors.clear();
                state->previewWorldGpuDirty = true;
                state->previewWorldGpuBuilding = false;
                ++state->previewWorldGpuGeneration;
                state->previewWorldSourceName.clear();
                state->previewWorldStatus = payload ? payload->message : L"加载存档实景失败";
                GuiSetStatus(state, payload ? payload->message : L"加载存档实景失败");
            }
            GuiUpdatePreviewChrome(state);
            GuiUpdateWorkflow(state);
            GuiRenderPreviewNow(state);
            if (state->previewWorldDirty) {
                state->previewWorldDirty = false;
                GuiRefreshWorldPreviewAsync(state);
            }
        }
        return 0;
    case WM_GUI_WORLD_GPU_READY:
        if (state) {
            std::unique_ptr<WorldPreviewGpuPayload> payload(reinterpret_cast<WorldPreviewGpuPayload*>(lParam));
            if (payload && payload->generation == state->previewWorldGpuGeneration &&
                payload->mesh.style == state->previewWorldStyle) {
                state->previewWorldPendingGpuMesh = std::move(payload->mesh);
                state->previewWorldGpuBuilding = false;
                state->previewWorldGpuDirty = true;
                GuiRenderPreviewNow(state);
                GuiUpdatePreviewChrome(state);
            }
        }
        return 0;
    case WM_TIMER:
        if (wParam == PREVIEW_DXGI_OVERLAY_REFRESH_TIMER) {
            if (state && state->previewDxgiOverlayRefreshPending) {
                const bool interacting = state->orbiting || state->panning ||
                    state->previewDraggingPlacement || state->previewDraggingHeight;
                if (interacting) return 0;

                KillTimer(hwnd, PREVIEW_DXGI_OVERLAY_REFRESH_TIMER);
                state->previewDxgiOverlayRefreshPending = false;
                if (state->previewActiveRenderer == native_mc::PreviewRendererBackend::D3D11 ||
                    state->previewActiveRenderer == native_mc::PreviewRendererBackend::D3D12) {
                    const std::wstring requestedRenderer = state->config.previewRenderer;
                    const bool previewWasLoaded = state->previewLoaded;
                    state->previewLoaded = false;
                    GuiSwitchPreviewRenderer(state, L"vulkan", true, false);
                    if (state->previewActiveRenderer == native_mc::PreviewRendererBackend::Vulkan) {
                        GuiRenderPreviewNow(state);
                    }
                    state->previewLoaded = previewWasLoaded;
                    GuiSwitchPreviewRenderer(state, requestedRenderer, true, false);
                }
            }
            return 0;
        }
        break;
    case WM_GUI_RENDER_TICK:
        if (state) {
            if (state->previewLoaded &&
                state->previewFrameTickPending.load(std::memory_order_acquire) &&
                state->preview && IsWindowVisible(state->preview) &&
                IsWindowVisible(hwnd) && !IsIconic(hwnd)) {
                state->previewFramePermit = true;
                if (!InvalidateRect(state->preview, nullptr, FALSE)) {
                    state->previewFramePermit = false;
                    state->previewFrameTickPending.store(false, std::memory_order_release);
                }
            } else {
                state->previewFrameTickPending.store(false, std::memory_order_release);
            }
        }
        return 0;
    case WM_DESTROY:
        if (state) {
            KillTimer(hwnd, PREVIEW_DXGI_OVERLAY_REFRESH_TIMER);
            state->previewDxgiOverlayRefreshPending = false;
            GuiStopPreviewFrameScheduler(state);
            state->previewLoadCancel.store(true, std::memory_order_relaxed);
            if (state->previewLoadThread.joinable()) state->previewLoadThread.join();
            if (state->paletteDialog && IsWindow(state->paletteDialog)) DestroyWindow(state->paletteDialog);
            if (state->breakdownDialog && IsWindow(state->breakdownDialog)) DestroyWindow(state->breakdownDialog);
            GuiClosePreviewProgress(state);
        }
        GuiDestroyPreviewContext(state);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void GuiStartPreviewFrameScheduler(GuiState* state) {
    if (!state || state->previewFrameThread.joinable()) return;

    state->previewFrameStop.store(false, std::memory_order_release);
    state->previewFrameTickPending.store(false, std::memory_order_release);
    state->previewFrameSchedulerRunning.store(true, std::memory_order_release);
    state->previewHighResolutionTimerActive = timeBeginPeriod(1) == TIMERR_NOERROR;
    const HWND hwnd = state->hwnd;
    const HWND preview = state->preview;
    try {
        state->previewFrameThread = std::thread([state, hwnd, preview]() {
            using Clock = std::chrono::steady_clock;
            int currentHz = NormalizePreviewRefreshRate(
                state->previewRefreshHz.load(std::memory_order_acquire));
            auto nextFrame = Clock::now();
            while (!state->previewFrameStop.load(std::memory_order_acquire)) {
                const int requestedHz = NormalizePreviewRefreshRate(
                    state->previewRefreshHz.load(std::memory_order_acquire));
                if (requestedHz != currentHz) {
                    currentHz = requestedHz;
                    nextFrame = Clock::now();
                }
                const bool unlimited = currentHz == 0;
                const auto frameInterval = unlimited
                    ? std::chrono::nanoseconds::zero()
                    : std::chrono::nanoseconds(1'000'000'000LL / currentHz);
                if (!unlimited) {
                    nextFrame += frameInterval;
                    std::this_thread::sleep_until(nextFrame);
                } else {
                    std::this_thread::yield();
                }
                if (state->previewFrameStop.load(std::memory_order_acquire)) break;

                const auto now = Clock::now();
                if (!unlimited && now - nextFrame > frameInterval * 4) nextFrame = now;
                if (!state->previewFrameActive.load(std::memory_order_acquire)) {
                    if (unlimited) Sleep(10);
                    continue;
                }
                if (!IsWindow(hwnd) || !IsWindow(preview)) break;
                if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || !IsWindowVisible(preview)) {
                    if (unlimited) Sleep(10);
                    continue;
                }
                if (state->previewFrameTickPending.exchange(true, std::memory_order_acq_rel)) {
                    state->previewFrameActive.store(false, std::memory_order_release);
                    if (unlimited) Sleep(1);
                    continue;
                }
                if (!state->previewFrameActive.exchange(false, std::memory_order_acq_rel)) {
                    state->previewFrameTickPending.store(false, std::memory_order_release);
                    continue;
                }
                if (!PostMessageW(hwnd, WM_GUI_RENDER_TICK, 0, 0)) {
                    state->previewFrameTickPending.store(false, std::memory_order_release);
                    break;
                }
            }
            state->previewFrameSchedulerRunning.store(false, std::memory_order_release);
        });
    } catch (...) {
        state->previewFrameSchedulerRunning.store(false, std::memory_order_release);
        if (state->previewHighResolutionTimerActive) {
            timeEndPeriod(1);
            state->previewHighResolutionTimerActive = false;
        }
    }
}

void GuiStopPreviewFrameScheduler(GuiState* state) {
    if (!state) return;
    state->previewFrameActive.store(false, std::memory_order_release);
    state->previewFrameStop.store(true, std::memory_order_release);
    state->previewFrameSchedulerRunning.store(false, std::memory_order_release);
    if (state->previewFrameThread.joinable()) state->previewFrameThread.join();
    state->previewFrameTickPending.store(false, std::memory_order_release);
    state->previewFramePermit = false;
    if (state->previewHighResolutionTimerActive) {
        timeEndPeriod(1);
        state->previewHighResolutionTimerActive = false;
    }
}

int RunGui(const fs::path& exePath, const fs::path& exeDir, const fs::path& configPath, Config config, BackendLayout backend, Lang lang) {
    WNDCLASSW previewClass{};
    previewClass.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    previewClass.lpfnWndProc = GuiPreviewProc;
    previewClass.hInstance = GetModuleHandleW(nullptr);
    previewClass.lpszClassName = L"MCPreviewWindow";
    previewClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&previewClass);

    WNDCLASSW paletteDialogClass{};
    paletteDialogClass.lpfnWndProc = GuiPaletteDialogProc;
    paletteDialogClass.hInstance = GetModuleHandleW(nullptr);
    paletteDialogClass.lpszClassName = L"MCPaletteDialogWindow";
    paletteDialogClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    paletteDialogClass.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&paletteDialogClass);

    WNDCLASSW breakdownDialogClass{};
    breakdownDialogClass.lpfnWndProc = GuiBreakdownDialogProc;
    breakdownDialogClass.hInstance = GetModuleHandleW(nullptr);
    breakdownDialogClass.lpszClassName = L"MCBreakdownDialogWindow";
    breakdownDialogClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    breakdownDialogClass.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&breakdownDialogClass);

    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = GuiWindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"MCImportGuiWindow";
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&windowClass);

    auto* state = new GuiState();
    state->exePath = exePath;
    state->exeDir = exeDir;
    state->configPath = configPath;
    state->config = std::move(config);
    state->backend = std::move(backend);
    state->lang = lang;

    HWND hwnd = CreateWindowExW(0, L"MCImportGuiWindow", L"3dmodel-to-minecraft 图形界面", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1500, 920, nullptr, nullptr, GetModuleHandleW(nullptr), state);
    if (!hwnd) {
        delete state;
        return 1;
    }
    ShowWindow(hwnd, SW_MAXIMIZE);
    UpdateWindow(hwnd);
    GuiInitPreviewContext(state);
    GuiUpdatePreviewChrome(state);
    GuiRenderPreviewNow(state);
    GuiStartPreviewFrameScheduler(state);
    const bool dxgiRenderer =
        state->previewActiveRenderer == native_mc::PreviewRendererBackend::D3D11 ||
        state->previewActiveRenderer == native_mc::PreviewRendererBackend::D3D12;
    const bool nvidiaAdapter = state->previewNativeRenderer &&
        ToLower(Utf8ToWide(state->previewNativeRenderer->AdapterName())).find(L"nvidia") !=
            std::wstring::npos;
    if (dxgiRenderer && nvidiaAdapter) {
        state->previewDxgiOverlayRefreshPending =
            SetTimer(hwnd, PREVIEW_DXGI_OVERLAY_REFRESH_TIMER, 1000, nullptr) != 0;
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        const bool appWindowMessage = msg.hwnd == hwnd || IsChild(hwnd, msg.hwnd);
        const bool ctrlPageShortcut = msg.message == WM_KEYDOWN &&
            (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool altPageShortcut = msg.message == WM_SYSKEYDOWN;
        if (appWindowMessage && (ctrlPageShortcut || altPageShortcut) &&
            msg.wParam >= '1' && msg.wParam <= '7') {
            const int pageIndex = static_cast<int>(msg.wParam - '1');
            TabCtrl_SetCurSel(state->tab, pageIndex);
            for (std::size_t index = 0; index < state->pages.size(); ++index) {
                ShowWindow(state->pages[index], static_cast<int>(index) == pageIndex ? SW_SHOW : SW_HIDE);
            }
            HWND firstControl = GetNextDlgTabItem(state->pages[pageIndex], nullptr, FALSE);
            SetFocus(firstControl ? firstControl : state->pages[pageIndex]);
            continue;
        }
        if (IsDialogMessageW(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    GuiStopPreviewFrameScheduler(state);
    delete state;
    return static_cast<int>(msg.wParam);
}

void ConfigureConsole() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_U8TEXT);
    _setmode(_fileno(stderr), _O_U8TEXT);
    _setmode(_fileno(stdin), _O_U8TEXT);
}

int wmain(int argc, wchar_t* argv[]) {
    ConfigureConsole();

    const fs::path exePath = GetExePath();
    const fs::path exeDir = exePath.parent_path();
    const fs::path configPath = exeDir / "config.local.json";

    ParsedArgs args = ParseArgs(argc, argv);
    Config config = LoadConfig(configPath);
    BackendLayout backend = ResolveBackendLayout(exeDir, config, args);
    if (backend.root.empty() && !config.projectRoot.empty()) {
        backend = ResolveBackendLayout(exeDir, config, args);
    }
    if (args.values.find(L"--test-world-root") == args.values.end() && !config.testWorldRoot.empty()) {
        config.testWorldRoot = ResolveTestWorldRoot(exeDir, backend, config, args);
    }
    Lang lang = ResolveLanguage(args, config);

    if (!args.errors.empty()) {
        for (const auto& error : args.errors) {
            PrintLine(T(lang, L"参数错误：", L"Argument error: ") + error);
        }
        PrintLine(T(lang, L"运行 help 查看支持的参数。", L"Run help to see supported options."));
        return 2;
    }

    if (args.command.empty() && args.positionals.size() == 1) {
        std::error_code ec;
        if (fs::exists(args.positionals.front(), ec)) {
            args.command = L"import";
            args.values[L"--input"] = args.positionals.front();
            args.positionals.clear();
        }
    }

    auto executeCurrentCommand = [&]() -> bool {
        if (args.command.empty()) {
            args.command = L"help";
        }

        if (args.command != L"help" && args.command != L"setup" && args.command != L"self-test") {
            ApplyConfigDefaults(args, config);
            ForceCommandMode(args);
        }

        bool success = false;
        if (args.command == L"help") {
            PrintHelp(lang, exePath);
            success = true;
        } else if (args.command == L"doctor") {
            success = PrintDoctor(lang, exeDir, configPath, config, args, backend);
        } else if (args.command == L"setup") {
            success = RunSetup(lang, exeDir, configPath, config, backend);
        } else if (args.command == L"self-test") {
            success = RunSelfTest(lang, exeDir, configPath, config, args, backend);
        } else if (args.command == L"list-worlds") {
            PrintWorlds(lang, backend, config, args);
            success = true;
        } else if (args.command == L"copy-world") {
            success = RunCopyWorld(lang, exeDir, backend, config, args);
        } else if (args.command == L"scan") {
            success = RunScan(lang, backend, config, args);
        } else if (args.command == L"reset") {
            success = RunReset(lang, backend, config, args);
        } else if (args.command == L"import" || args.command == L"obj" || args.command == L"obj-surface" || args.command == L"obj-copy" || args.command == L"obj-rotate" ||
                   args.command == L"glb" || args.command == L"glb-copy" || args.command == L"glb-surface") {
            if (args.command == L"import" && args.values.find(L"--mode") == args.values.end()) {
                args.values[L"--mode"] = L"glb-textured";
            }
            if (args.command == L"obj-copy" || args.command == L"glb-copy") {
                args.flags.insert(L"--copy-world");
            }
            success = RunImport(lang, exeDir, backend, config, args);
        } else {
            PrintLine(T(lang, L"未知命令。", L"Unknown command."));
        }

        if (success && (args.command == L"setup" || args.command == L"copy-world" || args.command == L"import" ||
                        args.command == L"obj" || args.command == L"obj-surface" || args.command == L"obj-copy" || args.command == L"obj-rotate" ||
                        args.command == L"glb" || args.command == L"glb-copy" || args.command == L"glb-surface" ||
                        args.command == L"scan" || args.command == L"reset")) {
            config.lang = (lang == Lang::Zh) ? L"zh" : L"en";
            config.projectRoot = backend.root;
            SaveConfig(configPath, config);
        }
        return success;
    };

    if (!args.hadArgs) {
#if defined(CODEX_GUI_EXE)
        if (HWND console = GetConsoleWindow()) {
            ShowWindow(console, SW_HIDE);
        }
        return RunGui(exePath, exeDir, configPath, config, backend, lang);
#else
        while (true) {
            if (args.command.empty()) {
                const int choice = InteractiveMenu(lang, exePath);
                switch (choice) {
                case 1:
                    args.command = L"setup";
                    break;
                case 2:
                    args.command = L"doctor";
                    break;
                case 3:
                    args.command = L"import";
                    args.values[L"--mode"] = L"glb-textured";
                    args.flags.insert(L"--copy-world");
                    break;
                case 4:
                    args.command = L"import";
                    args.values[L"--mode"] = L"obj";
                    args.flags.insert(L"--copy-world");
                    break;
                case 5:
                    args.command = L"copy-world";
                    break;
                case 6:
                    args.command = L"scan";
                    break;
                case 7:
                    args.command = L"reset";
                    break;
                case 9:
                    args.command = L"help";
                    break;
                default:
                    return 0;
                }
            }

            executeCurrentCommand();
            PauseForUser(lang);
            args = {};
        }
#endif
    }

    return executeCurrentCommand() ? 0 : 1;
}
