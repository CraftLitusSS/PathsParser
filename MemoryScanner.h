#pragma once

#include <string>
#include <vector>
#include <regex>
#include <unordered_set>

struct MemoryScanRule {
    enum class Type { Regex, Contains };
    Type type;
    std::wstring pattern;    // regex pattern or contains string
    std::wstring containsA;  // for Contains type: first substring
    std::wstring containsB;  // for Contains type: second substring
};

struct MemoryTarget {
    std::wstring processName;       // e.g. L"csrss.exe"
    bool sortByMemory;             // if true: sort csrss instances by WS
    enum SortDir { Lowest, Highest } sortDir;
    std::vector<MemoryScanRule> rules;
};

// Main entry point: scans all target processes and returns unique paths
std::vector<std::string> GetSuspiciousPathsFromMemory();
