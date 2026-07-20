#include "Include.h"
#include "MemoryScanner.h"

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Psapi.lib")

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

static void ExtractAsciiStrings(const BYTE* buffer, SIZE_T size, std::unordered_set<std::string>& out)
{
    const SIZE_T minLen = 4;
    SIZE_T start = 0;
    for (SIZE_T i = 0; i < size; ++i) {
        char c = static_cast<char>(buffer[i]);
        if (c >= 32 && c <= 126) {
            continue;
        }
        if (i - start >= minLen) {
            out.emplace(reinterpret_cast<const char*>(buffer + start), i - start);
        }
        start = i + 1;
    }
    if (size - start >= minLen) {
        out.emplace(reinterpret_cast<const char*>(buffer + start), size - start);
    }
}

struct ProcEntry {
    DWORD pid;
    SIZE_T ws;
};

static std::vector<ProcEntry> FindProcesses(const std::wstring& name)
{
    std::vector<ProcEntry> entries;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return entries;

    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name.c_str()) == 0) {
                SIZE_T ws = 0;
                HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pe.th32ProcessID);
                if (hProc) {
                    PROCESS_MEMORY_COUNTERS pmc = { sizeof(pmc) };
                    if (GetProcessMemoryInfo(hProc, &pmc, sizeof(pmc))) {
                        ws = pmc.WorkingSetSize;
                    }
                    CloseHandle(hProc);
                }
                entries.push_back({ pe.th32ProcessID, ws });
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return entries;
}

// Locate the svchost.exe hosting a specific service by name (e.g. Appinfo)
static DWORD FindServicePid(const wchar_t* serviceName)
{
    DWORD pid = 0;
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return 0;

    SC_HANDLE svc = OpenServiceW(scm, serviceName, SERVICE_QUERY_STATUS);
    if (svc) {
        SERVICE_STATUS_PROCESS ssp = { sizeof(ssp) };
        DWORD needed = 0;
        if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &needed)) {
            pid = ssp.dwProcessId;
        }
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return pid;
}

static void ScanProcessMemory(DWORD pid,
    const std::vector<MemoryScanRule>& rules,
    std::unordered_set<std::string>& results)
{
    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProc) return;

    struct CompiledRule {
        MemoryScanRule::Type type;
        std::wstring containsA;
        std::wstring containsB;
        std::wregex regex;
    };
    std::vector<CompiledRule> compiled;
    compiled.reserve(rules.size());
    for (auto& r : rules) {
        if (r.type == MemoryScanRule::Type::Regex) {
            compiled.push_back({ r.type, L"", L"", std::wregex(r.pattern, std::regex_constants::icase) });
        }
        else {
            compiled.push_back({ r.type, r.containsA, r.containsB, std::wregex() });
        }
    }

    std::unordered_set<std::string> rawStrings;

    BYTE* pageBuf = nullptr;
    SIZE_T pageBufSize = 0;
    const SIZE_T READ_CHUNK = 256 * 1024;

    MEMORY_BASIC_INFORMATION mbi = {};
    BYTE* addr = nullptr;
    while (VirtualQueryEx(hProc, addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) &&
            !(mbi.Protect & PAGE_GUARD))
        {
            SIZE_T regionSize = mbi.RegionSize;
            BYTE* regionBase = static_cast<BYTE*>(mbi.BaseAddress);

            SIZE_T offset = 0;
            while (offset < regionSize) {
                SIZE_T chunk = (regionSize - offset > READ_CHUNK) ? READ_CHUNK : (regionSize - offset);
                if (chunk > pageBufSize) {
                    delete[] pageBuf;
                    pageBuf = new BYTE[chunk];
                    pageBufSize = chunk;
                }
                SIZE_T bytesRead = 0;
                if (ReadProcessMemory(hProc, regionBase + offset, pageBuf, chunk, &bytesRead) && bytesRead > 0) {
                    ExtractAsciiStrings(pageBuf, bytesRead, rawStrings);
                }
                offset += chunk;
            }
        }
        addr = static_cast<BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
    }
    delete[] pageBuf;

    if (hProc) CloseHandle(hProc);

    for (auto& s : rawStrings) {
        std::wstring ws = utf8_to_wide(s);
        for (auto& rule : compiled) {
            bool matched = false;
            if (rule.type == MemoryScanRule::Type::Regex) {
                try {
                    matched = std::regex_search(ws, rule.regex);
                }
                catch (...) {
                    continue;
                }
            }
            else {
                if (!rule.containsA.empty() && !rule.containsB.empty()) {
                    matched = (ws.find(rule.containsA) != std::wstring::npos &&
                        ws.find(rule.containsB) != std::wstring::npos);
                }
            }
            if (matched) {
                results.insert(s);
                break;
            }
        }
    }
}

std::vector<std::string> GetSuspiciousPathsFromMemory()
{
    std::unordered_set<std::string> allResults;

    // ---- 1. CSRSS.EXE ----
    {
        auto procs = FindProcesses(L"csrss.exe");
        if (procs.size() >= 2) {
            std::sort(procs.begin(), procs.end(),
                [](const ProcEntry& a, const ProcEntry& b) { return a.ws < b.ws; });

            std::vector<MemoryScanRule> rulesLow = {
                { MemoryScanRule::Type::Regex, LR"(^(?:\\\\\?\\)?[A-Za-z]:\\.+$)", L"", L"" }
            };
            ScanProcessMemory(procs[0].pid, rulesLow, allResults);

            std::vector<MemoryScanRule> rulesHigh = {
                { MemoryScanRule::Type::Regex, LR"(^(?!.*\.dll$)(?:\\\\\?\\)?[A-Za-z]:\\.+$)", L"", L"" }
            };
            ScanProcessMemory(procs.back().pid, rulesHigh, allResults);
        }
        else if (procs.size() == 1) {
            std::vector<MemoryScanRule> rules = {
                { MemoryScanRule::Type::Regex, LR"(^(?:\\\\\?\\)?[A-Za-z]:\\.+$)", L"", L"" },
                { MemoryScanRule::Type::Regex, LR"(^(?!.*\.dll$)(?:\\\\\?\\)?[A-Za-z]:\\.+$)", L"", L"" }
            };
            ScanProcessMemory(procs[0].pid, rules, allResults);
        }
    }

    // ---- 2. EXPLORER.EXE ----
    {
        auto procs = FindProcesses(L"explorer.exe");
        for (auto& p : procs) {
            std::vector<MemoryScanRule> rules = {
                { MemoryScanRule::Type::Regex, LR"(^[A-Z]:\\.+\.(exe)$)", L"", L"" },
                { MemoryScanRule::Type::Regex, LR"(^[A-Za-z]:\\.*\.dll$)", L"", L"" }
            };
            ScanProcessMemory(p.pid, rules, allResults);
        }
    }

    // ---- 3. APPINFO (svchost.exe hosting the Appinfo service) ----
    {
        DWORD appinfoPid = FindServicePid(L"Appinfo");
        if (appinfoPid != 0) {
            std::vector<MemoryScanRule> rules = {
                { MemoryScanRule::Type::Contains, L"", L"C:", L".exe" }
            };
            ScanProcessMemory(appinfoPid, rules, allResults);
        }
    }

    std::vector<std::string> result(allResults.begin(), allResults.end());
    std::sort(result.begin(), result.end());
    return result;
}
