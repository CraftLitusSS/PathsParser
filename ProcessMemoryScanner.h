#pragma once

#include <string>
#include <vector>

struct MemoryScanConfig {
    bool scanCSRSS = true;
    bool scanExplorer = true;
    bool scanAppInfo = true;
};

class ProcessMemoryScanner {
public:
    static std::vector<std::string> GetSuspiciousPathsFromMemory();
    static std::vector<std::string> GetSuspiciousPathsFromMemory(const MemoryScanConfig& config);
};
