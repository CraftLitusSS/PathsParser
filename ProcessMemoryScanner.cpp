#include "Include.h"
#include "ProcessMemoryScanner.h"
#include <psapi.h>
#include <thread>
#include <future>
#include <regex>

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Psapi.lib")

// ---------------------------------------------------------------------------
// Utility: string encoding converters
// ---------------------------------------------------------------------------
static std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring ws(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &ws[0], len);
    return ws;
}

static std::string wide_to_utf8(const std::wstring& ws) {
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), &s[0], len, nullptr, nullptr);
    return s;
}

// ---------------------------------------------------------------------------
// Extract both ASCII and UTF-16 strings (min length 4) from a memory buffer
// ---------------------------------------------------------------------------
static void ExtractStrings(const BYTE* buffer, SIZE_T size,
    std::unordered_set<std::string>& results)
{
    const SIZE_T MIN_LEN = 4;

    // --- ASCII (single-byte) scan ---
    SIZE_T start = 0;
    for (SIZE_T i = 0; i < size; ++i) {
        char c = static_cast<char>(buffer[i]);
        if (c >= 32 && c <= 126) continue;
        if (i - start >= MIN_LEN) {
            std::string s(reinterpret_cast<const char*>(buffer + start), i - start);
            if (s.find(':') != std::string::npos && s.find('\\') != std::string::npos)
                results.emplace(std::move(s));
        }
        start = i + 1;
    }
    if (size - start >= MIN_LEN) {
        std::string s(reinterpret_cast<const char*>(buffer + start), size - start);
        if (s.find(':') != std::string::npos && s.find('\\') != std::string::npos)
            results.emplace(std::move(s));
    }

    // --- UTF-16 (wide) scan ---
    start = 0;
    for (SIZE_T i = 0; i + 1 < size; i += 2) {
        wchar_t wc = *reinterpret_cast<const wchar_t*>(buffer + i);
        if (wc >= 32 && wc <= 0x7E) continue;
        SIZE_T len = (i - start) / 2;
        if (len >= MIN_LEN) {
            std::wstring ws(reinterpret_cast<const wchar_t*>(buffer + start), len);
            std::string s = wide_to_utf8(ws);
            if (s.find(':') != std::string::npos && s.find('\\') != std::string::npos)
                results.emplace(std::move(s));
        }
        start = i + 2;
    }
    SIZE_T trailing = (size - start) / 2;
    if (trailing >= MIN_LEN) {
        std::wstring ws(reinterpret_cast<const wchar_t*>(buffer + start), trailing);
        std::string s = wide_to_utf8(ws);
        if (s.find(':') != std::string::npos && s.find('\\') != std::string::npos)
            results.emplace(std::move(s));
    }
}

// ---------------------------------------------------------------------------
// Check if a memory region is almost entirely zero-filled (fast skip hint)
// ---------------------------------------------------------------------------
static bool IsLikelyZeroPage(HANDLE hProc, LPCVOID base, SIZE_T size)
{
    BYTE sample[16];
    SIZE_T read = 0;
    if (!ReadProcessMemory(hProc, base, sample, min(size, (SIZE_T)16), &read) || read < 8)
        return false;
    for (SIZE_T i = 0; i < read; ++i)
        if (sample[i] != 0) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Process enumeration by name, sorted by Private Bytes (descending).
// Returns (pid, privateBytes) pairs.
// ---------------------------------------------------------------------------
struct ProcEntry {
    DWORD pid;
    SIZE_T privateBytes;
};

static std::vector<ProcEntry> FindProcessesSortedByPrivateBytes(const std::wstring& name)
{
    std::vector<ProcEntry> entries;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return entries;

    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name.c_str()) != 0) continue;
            HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                FALSE, pe.th32ProcessID);
            if (!hProc) continue;

            PROCESS_MEMORY_COUNTERS_EX pmc = { sizeof(pmc) };
            SIZE_T priv = 0;
            if (GetProcessMemoryInfo(hProc,
                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
            {
                priv = pmc.PrivateUsage;
            }
            CloseHandle(hProc);
            entries.push_back({ pe.th32ProcessID, priv });
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    std::sort(entries.begin(), entries.end(),
        [](const ProcEntry& a, const ProcEntry& b) {
            return a.privateBytes < b.privateBytes;
        });
    return entries;
}

// ---------------------------------------------------------------------------
// Basic process list by name (no sorting, for svchost and explorer)
// ---------------------------------------------------------------------------
static std::vector<DWORD> FindAllProcessIds(const std::wstring& name)
{
    std::vector<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;

    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name.c_str()) == 0)
                pids.push_back(pe.th32ProcessID);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pids;
}

// ---------------------------------------------------------------------------
// Scan a single process memory, extract strings matching given criteria
// ---------------------------------------------------------------------------
static void ScanProcessMemory(DWORD pid,
    const std::vector<std::wstring>& regexPatterns,
    const std::vector<std::pair<std::wstring, std::wstring>>& containsRules,
    std::unordered_set<std::string>& results)
{
    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
        FALSE, pid);
    if (!hProc) return;
    // Ensure cleanup
    auto closeProc = std::unique_ptr<std::remove_pointer_t<HANDLE>, decltype(&CloseHandle)>(
        hProc, CloseHandle);

    // Pre-compile regex
    std::vector<std::wregex> compiledRegex;
    compiledRegex.reserve(regexPatterns.size());
    for (auto& p : regexPatterns)
        compiledRegex.emplace_back(p, std::regex_constants::icase);

    // Extract raw strings from memory
    std::unordered_set<std::string> rawStrings;

    BYTE* chunkBuf = nullptr;
    SIZE_T chunkBufSize = 0;
    const SIZE_T LARGE_CHUNK = 256 * 1024;   // 256 KB
    const SIZE_T SMALL_CHUNK = 64 * 1024;    // 64 KB

    MEMORY_BASIC_INFORMATION mbi = {};
    BYTE* addr = nullptr;

    while (VirtualQueryEx(hProc, addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE |
                PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) &&
            !(mbi.Protect & PAGE_GUARD))
        {
            BYTE* regionBase = static_cast<BYTE*>(mbi.BaseAddress);
            SIZE_T regionSize = mbi.RegionSize;

            if (IsLikelyZeroPage(hProc, regionBase, regionSize))
                goto skip_region;

            // Adaptive chunk: read small regions whole, large ones in 256KB
            SIZE_T chunk = (regionSize <= SMALL_CHUNK) ? regionSize : LARGE_CHUNK;
            if (chunk > chunkBufSize) {
                delete[] chunkBuf;
                chunkBuf = new BYTE[chunk];
                chunkBufSize = chunk;
            }

            for (SIZE_T offset = 0; offset < regionSize; offset += chunk) {
                SIZE_T toRead = min(chunk, regionSize - offset);
                SIZE_T bytesRead = 0;
                if (ReadProcessMemory(hProc, regionBase + offset,
                    chunkBuf, toRead, &bytesRead) && bytesRead > 0)
                {
                    ExtractStrings(chunkBuf, bytesRead, rawStrings);
                }
            }
        }
    skip_region:
        addr = static_cast<BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
    }
    delete[] chunkBuf;

    // Apply rules
    for (auto& s : rawStrings) {
        std::wstring ws = utf8_to_wide(s);

        // Check regex patterns
        bool matched = false;
        for (auto& re : compiledRegex) {
            try {
                if (std::regex_search(ws, re)) { matched = true; break; }
            }
            catch (...) { continue; }
        }
        if (!matched) {
            // Check contains rules
            for (auto& rule : containsRules) {
                if (!rule.first.empty() && !rule.second.empty()) {
                    if (ws.find(rule.first) != std::wstring::npos &&
                        ws.find(rule.second) != std::wstring::npos)
                    {
                        matched = true;
                        break;
                    }
                }
            }
        }
        if (matched)
            results.insert(s);
    }
}

// ===========================================================================
// Public API — full scan (all targets)
// ===========================================================================
std::vector<std::string> ProcessMemoryScanner::GetSuspiciousPathsFromMemory()
{
    return GetSuspiciousPathsFromMemory({ true, true, true });
}

// ===========================================================================
// Public API — selective scan
// ===========================================================================
std::vector<std::string> ProcessMemoryScanner::GetSuspiciousPathsFromMemory(const MemoryScanConfig& config)
{
    std::unordered_set<std::string> allResults;

    // ---- 1. CSRSS.EXE ----
    if (config.scanCSRSS) {
        auto entries = FindProcessesSortedByPrivateBytes(L"csrss.exe");
        if (entries.size() >= 2) {
            ScanProcessMemory(entries[0].pid,
                { LR"(^(?:\\\\\?\\)?[A-Za-z]:\\.+$)" },
                {}, allResults);
            ScanProcessMemory(entries.back().pid,
                { LR"(^(?!.*\.dll$)(?:\\\\\?\\)?[A-Za-z]:\\.+$)" },
                {}, allResults);
        }
        else if (entries.size() == 1) {
            ScanProcessMemory(entries[0].pid,
                {
                    LR"(^(?:\\\\\?\\)?[A-Za-z]:\\.+$)",
                    LR"(^(?!.*\.dll$)(?:\\\\\?\\)?[A-Za-z]:\\.+$)"
                },
                {}, allResults);
        }
    }

    // ---- 2. EXPLORER.EXE ----
    if (config.scanExplorer) {
        auto pids = FindAllProcessIds(L"explorer.exe");
        std::vector<std::future<std::unordered_set<std::string>>> futures;
        for (DWORD pid : pids) {
            futures.push_back(std::async(std::launch::async, [pid]() {
                std::unordered_set<std::string> local;
                ScanProcessMemory(pid,
                    {
                        LR"(^[A-Z]:\\.+\.(exe)$)",
                        LR"(^[A-Za-z]:\\.*\.dll$)"
                    },
                    {}, local);
                return local;
            }));
        }
        for (auto& f : futures) {
            auto local = f.get();
            for (auto& s : local) allResults.insert(s);
        }
    }

    // ---- 3. APPINFO (svchost.exe hosting Appinfo service) ----
    if (config.scanAppInfo) {
        auto pids = FindAllProcessIds(L"svchost.exe");
        std::vector<std::future<std::unordered_set<std::string>>> futures;
        for (DWORD pid : pids) {
            futures.push_back(std::async(std::launch::async, [pid]() {
                std::unordered_set<std::string> local;
                ScanProcessMemory(pid,
                    {},
                    { { L"C:", L".exe" } }, local);
                return local;
            }));
        }
        for (auto& f : futures) {
            auto local = f.get();
            for (auto& s : local) allResults.insert(s);
        }
    }

    // Deduplicate and sort
    std::vector<std::string> result(allResults.begin(), allResults.end());
    std::sort(result.begin(), result.end());
    return result;
}
