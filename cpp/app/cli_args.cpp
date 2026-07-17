#include "cli_args.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <sstream>
#include <tuple>

namespace {

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(::towlower(ch));
    });
    return value;
}
std::wstring TrimValue(const std::wstring& value) {
    std::size_t start = 0;
    while (start < value.size() && ::iswspace(value[start])) ++start;
    std::size_t end = value.size();
    while (end > start && ::iswspace(value[end - 1])) --end;
    return value.substr(start, end - start);
}

bool ParseStrictInt(const std::wstring& text, int minimum, int maximum) {
    try {
        const std::wstring trimmed = TrimValue(text);
        std::size_t used = 0;
        const long long value = std::stoll(trimmed, &used, 10);
        return used == trimmed.size() && value >= minimum && value <= maximum;
    } catch (...) {
        return false;
    }
}

bool ParseStrictDouble(const std::wstring& text, double minimum, double maximum) {
    try {
        const std::wstring trimmed = TrimValue(text);
        std::size_t used = 0;
        const double value = std::stod(trimmed, &used);
        return used == trimmed.size() && std::isfinite(value) && value >= minimum && value <= maximum;
    } catch (...) {
        return false;
    }
}

}  // namespace

bool ValidateImportNumbers(const ParsedArgs& args, std::wstring* errorText) {
    const std::vector<std::tuple<std::wstring, int, int>> ints = {
        { L"--batch-block-limit", 1, 100000000 }, { L"--workers", 1, 64 },
        { L"--base-y", -2048, 2048 }, { L"--dedupe-sample-steps", 1, 32 },
        { L"--clip-passes", 1, 32 }, { L"--center-x", -30000000, 30000000 },
        { L"--center-y", -2048, 2048 }, { L"--center-z", -30000000, 30000000 }
    };
    for (const auto& [flag, minimum, maximum] : ints) {
        auto it = args.values.find(flag);
        if (it != args.values.end() && !ParseStrictInt(it->second, minimum, maximum)) {
            if (errorText) *errorText = flag + L" 必须是 " + std::to_wstring(minimum) + L" 到 " + std::to_wstring(maximum) + L" 的整数。";
            return false;
        }
    }

    const std::vector<std::tuple<std::wstring, double, double>> doubles = {
        { L"--rotate-y-deg", -360000.0, 360000.0 }, { L"--scale-blocks-per-meter", 0.0001, 1000.0 },
        { L"--scale", 0.0001, 1000.0 }, { L"--component-min-ratio", 0.0, 1.0 },
        { L"--component-below-gap", 0.0, 1000000.0 }, { L"--clip-below-meters", -1000000.0, 1000000.0 },
        { L"--clip-cell-size", 0.0001, 1000000.0 }, { L"--clip-low-fraction", 0.0001, 1.0 },
        { L"--clip-trim-sigma", 0.0001, 1000.0 }
    };
    for (const auto& [flag, minimum, maximum] : doubles) {
        auto it = args.values.find(flag);
        if (it != args.values.end() && !ParseStrictDouble(it->second, minimum, maximum)) {
            if (errorText) *errorText = flag + L" 的数值超出有效范围。";
            return false;
        }
    }

    auto validateVector = [](const std::wstring& raw, int count, bool integer) {
        std::wstringstream stream(raw);
        std::wstring part;
        int found = 0;
        while (std::getline(stream, part, L',')) {
            const bool valid = integer
                ? ParseStrictInt(part, -30000000, 30000000)
                : ParseStrictDouble(part, -1000000.0, 1000000.0);
            if (!valid) return false;
            ++found;
        }
        return found == count;
    };

    if (auto it = args.values.find(L"--center"); it != args.values.end()) {
        std::wstringstream stream(it->second);
        std::wstring part;
        int count = 0;
        while (std::getline(stream, part, L',')) ++count;
        if ((count != 2 && count != 3) || !validateVector(it->second, count, true)) {
            if (errorText) *errorText = L"--center 必须是 x,z 或 x,y,z 整数。";
            return false;
        }
    }
    if (auto it = args.values.find(L"--level-normal"); it != args.values.end() &&
        !validateVector(it->second, 3, false)) {
        if (errorText) *errorText = L"--level-normal 必须是三个有限数字 x,y,z。";
        return false;
    }
    return true;
}

ParsedArgs ParseArgs(int argc, wchar_t* argv[]) {
    ParsedArgs parsed;
    parsed.hadArgs = argc > 1;
    const std::set<std::wstring> valueOptions = {
        L"--run", L"--command", L"--project-root", L"--lang",
        L"--input", L"--input-type", L"--mode", L"--world-dir", L"--world",
        L"--saves-dir", L"--mc-root", L"--mc-version", L"--test-world-root",
        L"--copy-to", L"--obj-dir", L"--glb-dir", L"--only", L"--center", L"--level-normal",
        L"--center-x", L"--center-y", L"--center-z", L"--batch-block-limit", L"--workers",
        L"--rotate-y-deg", L"--block-types", L"--blocks", L"--exclude-block-types", L"--exclude-blocks",
        L"--clip-reference-obj", L"--clip-below-meters", L"--clip-cell-size", L"--clip-low-fraction",
        L"--clip-passes", L"--clip-trim-sigma", L"--dedupe-sample-steps", L"--component-min-ratio",
        L"--component-below-gap", L"--base-y", L"--scale-blocks-per-meter", L"--scale",
        L"--fallback-block", L"--palette", L"--transform", L"--block", L"--min-y", L"--max-y"
    };
    const std::set<std::wstring> flagOptions = {
        L"--help", L"-h", L"/?", L"--yes", L"--direct", L"--copy-world", L"--overwrite",
        L"--no-glass", L"--list-worlds", L"--self-test", L"--flip-x", L"--flip-z", L"--no-flip-z",
        L"--no-component-filter", L"--all", L"--force-version"
    };

    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--") {
            for (int j = i + 1; j < argc; ++j) parsed.errors.push_back(L"未知参数: " + std::wstring(argv[j]));
            break;
        }
        const std::size_t eq = arg.find(L'=');
        if (eq != std::wstring::npos && arg.rfind(L"--", 0) == 0) {
            const std::wstring key = arg.substr(0, eq);
            if (valueOptions.count(key)) parsed.values[key] = arg.substr(eq + 1);
            else parsed.errors.push_back(L"未知参数: " + key);
            continue;
        }
        if (valueOptions.count(arg)) {
            std::wstring value;
            if (i + 1 < argc && std::wstring(argv[i + 1]).rfind(L"--", 0) != 0) value = argv[++i];
            else parsed.errors.push_back(L"参数缺少值: " + arg);
            if (arg == L"--run" || arg == L"--command") parsed.command = Lower(value);
            parsed.values[arg] = value;
            continue;
        }
        if (flagOptions.count(arg)) {
            parsed.flags.insert(arg);
            continue;
        }
        if (!arg.empty() && arg[0] == L'-') {
            parsed.errors.push_back(L"未知参数: " + arg);
            continue;
        }
        if (parsed.command.empty()) {
            const std::wstring lower = Lower(arg);
            const std::set<std::wstring> commands = {
                L"doctor", L"setup", L"import", L"scan", L"reset", L"copy-world", L"self-test", L"help",
                L"obj-copy", L"obj", L"obj-surface", L"obj-rotate", L"glb-copy", L"glb", L"glb-surface", L"list-worlds"
            };
            if (commands.count(lower)) {
                parsed.command = lower;
                continue;
            }
        }
        parsed.positionals.push_back(arg);
    }

    if (parsed.command.empty() && parsed.flags.count(L"--self-test")) parsed.command = L"self-test";
    if (parsed.command.empty() && parsed.flags.count(L"--help")) parsed.command = L"help";
    if (parsed.command.empty() && parsed.flags.count(L"--list-worlds")) parsed.command = L"list-worlds";
    const int safetyFlags = static_cast<int>(parsed.flags.count(L"--copy-world")) +
        static_cast<int>(parsed.flags.count(L"--direct"));
    if (safetyFlags > 1) parsed.errors.push_back(L"--copy-world 和 --direct 只能选择一个。 ");
    return parsed;
}
