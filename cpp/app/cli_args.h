#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

struct ParsedArgs {
    std::wstring command;
    std::map<std::wstring, std::wstring> values;
    std::set<std::wstring> flags;
    std::vector<std::wstring> passthrough;
    std::vector<std::wstring> positionals;
    std::vector<std::wstring> errors;
    bool hadArgs = false;
};

ParsedArgs ParseArgs(int argc, wchar_t* argv[]);
bool ValidateImportNumbers(const ParsedArgs& args, std::wstring* errorText);
