#include "Include.h"
#include "Replaceparser.h"
#include <regex>

static std::wstring utf8_to_wstring(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring ws(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &ws[0], len);
    return ws;
}

static std::string wstring_to_utf8(const std::wstring& ws) {
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), &s[0], len, nullptr, nullptr);
    return s;
}

inline std::string replaceParserDir;
inline std::unordered_map<std::string, std::vector<ReplacementEntry>> replacementCache;
static std::map<std::pair<std::string, std::string>, ReplacementEntry> gLatestResults;

std::string ToLower(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

bool isDllFile(const std::string& filePath) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file) return false;
    IMAGE_DOS_HEADER dosHeader;
    file.read(reinterpret_cast<char*>(&dosHeader), sizeof(dosHeader));
    if (!file || dosHeader.e_magic != IMAGE_DOS_SIGNATURE) return false;
    file.seekg(dosHeader.e_lfanew, std::ios::beg);
    uint32_t peSignature;
    file.read(reinterpret_cast<char*>(&peSignature), sizeof(peSignature));
    if (!file || peSignature != IMAGE_NT_SIGNATURE) return false;
    IMAGE_FILE_HEADER fileHeader;
    file.read(reinterpret_cast<char*>(&fileHeader), sizeof(fileHeader));
    if (!file) return false;
    return (fileHeader.Characteristics & IMAGE_FILE_DLL) != 0;
}

bool WriteExeToTemp(const std::string& replaceParserDir) {
    std::string exePath = replaceParserDir + "\\replaceparser.exe";
    std::ofstream exeFile(exePath, std::ios::binary);
    if (!exeFile) {
        std::cerr << "Failed to create executable file: " << exePath << std::endl;
        return false;
    }
    exeFile.write(reinterpret_cast<const char*>(ReplaceParserHex), sizeof(ReplaceParserHex));
    exeFile.close();
    return true;
}

bool DeleteReplaceParserDir(const std::string& replaceParserDir) {
    try {
        std::filesystem::remove_all(replaceParserDir);
        return true;
    }
    catch (const std::filesystem::filesystem_error&) {
        return false;
    }
}

bool ExecuteReplaceParser(const std::string& replaceParserDir) {
    std::string exePath = replaceParserDir + "\\replaceparser.exe";
    std::string replacesTxtPath = replaceParserDir + "\\replaces.txt";
    std::string commandLine = "\"" + exePath + "\" \"" + replacesTxtPath + "\"";
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    HANDLE hNull = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hNull == INVALID_HANDLE_VALUE) return false;
    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdOutput = hNull;
    si.hStdError = hNull;
    if (!CreateProcessA(NULL, const_cast<char*>(commandLine.c_str()), NULL, NULL, TRUE, 0, NULL, replaceParserDir.c_str(), &si, &pi)) {
        CloseHandle(hNull);
        return false;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hNull);
    return true;
}

void PreProcessReplacements(const std::string& logFilePath) {
    std::ifstream file(logFilePath);
    if (!file.is_open()) return;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        std::string replaceType, pattern;
        if (line.rfind("Explorer replacement found in file: ", 0) == 0) {
            replaceType = "Explorer";
            pattern = "Explorer replacement found in file: ";
        }
        else if (line.rfind("Copy replacement found in file: ", 0) == 0) {
            replaceType = "Copy";
            pattern = "Copy replacement found in file: ";
        }
        else if (line.rfind("Type pattern found in file: ", 0) == 0) {
            replaceType = "Type";
            pattern = "Type pattern found in file: ";
        }
        else if (line.rfind("Delete pattern found in file: ", 0) == 0) {
            replaceType = "Delete";
            pattern = "Delete pattern found in file: ";
        }
        else {
            continue;
        }
        size_t pos = line.find(pattern);
        if (pos == std::string::npos) continue;
        std::string foundFileName = line.substr(pos + pattern.size());
        std::string foundFileNameLower = ToLower(foundFileName);
        bool openBraceFound = false;
        std::string detailsCollected, detailsLine;
        while (std::getline(file, detailsLine)) {
            if (!openBraceFound) {
                size_t bracePos = detailsLine.find('{');
                if (bracePos != std::string::npos) {
                    openBraceFound = true;
                    if (bracePos + 1 < detailsLine.size())
                        detailsCollected += detailsLine.substr(bracePos + 1) + "\n";
                }
            }
            else {
                size_t closePos = detailsLine.find('}');
                if (closePos != std::string::npos) {
                    if (closePos > 0)
                        detailsCollected += detailsLine.substr(0, closePos);
                    break;
                }
                else {
                    detailsCollected += detailsLine + "\n";
                }
            }
        }
        ReplacementEntry entry = { foundFileName, replaceType, detailsCollected };
        replacementCache[foundFileNameLower].push_back(entry);
    }
}

void FindReplace(const std::string& inputFileName) {
    std::string key = ToLower(inputFileName);
    auto it = replacementCache.find(key);
    if (it != replacementCache.end()) {
        for (auto& entry : it->second) {
            gLatestResults[{ entry.fileName, entry.replaceType }] = entry;
        }
    }
}

void WriteAllReplacementsToFileAndPrintSummary() {
    if (gLatestResults.empty()) {
        std::cout << "\n\nNo replacements found." << std::endl;
        return;
    }
    std::string outputFileName = "replaces.txt";
    std::ofstream outFile(outputFileName);
    for (auto& kv : gLatestResults) {
        outFile << "Found replacement type: " << kv.second.replaceType << "\n";
        outFile << "In file: " << kv.second.fileName << "\n";
        outFile << "Replacement details:\n" << kv.second.details << "\n\n";
    }
    outFile.close();
    std::cout << "\n\nFound " << gLatestResults.size() << " possible replacements, check " << outputFileName << std::endl;
    std::string cmd = "start \"\" \"" + outputFileName + "\"";
    std::system(cmd.c_str());
}

bool initReplaceParser() {
    wchar_t tmp[MAX_PATH];
    DWORD len = GetTempPathW(MAX_PATH, tmp);
    if (len == 0 || len > MAX_PATH) {
        std::cerr << "Failed to get temporary directory." << std::endl;
        return false;
    }
    std::wstring wtemp(tmp, tmp + len);
    if (!wtemp.empty() && (wtemp.back() == L'\\' || wtemp.back() == L'/'))
        wtemp.pop_back();
    replaceParserDir = wstring_to_utf8(wtemp) + "\\replaceparser";
    auto wdir = utf8_to_wstring(replaceParserDir);
    if (!CreateDirectoryW(wdir.c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        std::cerr << "Failed to create directory: " << replaceParserDir << std::endl;
        return false;
    }
    if (!WriteExeToTemp(replaceParserDir) || !ExecuteReplaceParser(replaceParserDir))
        return false;
    return true;
}

bool DestroyReplaceParser() {
    if (!DeleteReplaceParserDir(replaceParserDir)) {
        std::cerr << "There was a problem deleting the replaceparser folder." << std::endl;
        return false;
    }
    return true;
}

__int64 privilege(const char* priv) {
    HANDLE thandle;
    LUID identifier;
    TOKEN_PRIVILEGES privileges{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &thandle)) {
        std::cerr << "OpenProcessToken error: " << GetLastError() << std::endl;
        return 0;
    }
    if (!LookupPrivilegeValueA(nullptr, priv, &identifier)) {
        std::cerr << "LookupPrivilegeValueA error: " << GetLastError() << std::endl;
        CloseHandle(thandle);
        return 0;
    }
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Luid = identifier;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!AdjustTokenPrivileges(thandle, FALSE, &privileges, sizeof(privileges), nullptr, nullptr)) {
        std::cerr << "AdjustTokenPrivileges error: " << GetLastError() << std::endl;
        CloseHandle(thandle);
        return 0;
    }
    DWORD error = GetLastError();
    if (error == ERROR_NOT_ALL_ASSIGNED) {
        std::cerr << "privileges error at assign." << std::endl;
        CloseHandle(thandle);
        return 0;
    }
    CloseHandle(thandle);
    return 1;
}

bool file_exists(const std::string& path) {
    auto wpath = utf8_to_wstring(path);
    return GetFileAttributesW(wpath.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::string getOwnPath() {
    wchar_t buffer[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, buffer, MAX_PATH);
    return wstring_to_utf8(std::wstring(buffer, buffer + len));
}

bool iequals(const std::string& a, const std::string& b) {
    return a.size() == b.size() &&
        std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return tolower(x) == tolower(y); });
}

static bool VerifyFileViaCatalog(LPCWSTR filePath) {
    HANDLE hCatAdmin = NULL;
    if (!CryptCATAdminAcquireContext(&hCatAdmin, NULL, 0))
        return false;
    HANDLE hFile = CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        CryptCATAdminReleaseContext(hCatAdmin, 0);
        return false;
    }
    DWORD dwHashSize = 0;
    if (!CryptCATAdminCalcHashFromFileHandle(hFile, &dwHashSize, NULL, 0)) {
        CloseHandle(hFile);
        CryptCATAdminReleaseContext(hCatAdmin, 0);
        return false;
    }
    BYTE* pbHash = new BYTE[dwHashSize];
    if (!CryptCATAdminCalcHashFromFileHandle(hFile, &dwHashSize, pbHash, 0)) {
        delete[] pbHash;
        CloseHandle(hFile);
        CryptCATAdminReleaseContext(hCatAdmin, 0);
        return false;
    }
    CloseHandle(hFile);
    CATALOG_INFO catInfo = { 0 };
    catInfo.cbStruct = sizeof(catInfo);
    HANDLE hCatInfo = CryptCATAdminEnumCatalogFromHash(hCatAdmin, pbHash, dwHashSize, 0, NULL);
    bool isCatalogSigned = false;
    while (hCatInfo && CryptCATCatalogInfoFromContext(hCatInfo, &catInfo, 0)) {
        WINTRUST_CATALOG_INFO wtc = {};
        wtc.cbStruct = sizeof(wtc);
        wtc.pcwszCatalogFilePath = catInfo.wszCatalogFile;
        wtc.pbCalculatedFileHash = pbHash;
        wtc.cbCalculatedFileHash = dwHashSize;
        wtc.pcwszMemberFilePath = filePath;
        WINTRUST_DATA wtd = {};
        wtd.cbStruct = sizeof(wtd);
        wtd.dwUnionChoice = WTD_CHOICE_CATALOG;
        wtd.pCatalog = &wtc;
        wtd.dwUIChoice = WTD_UI_NONE;
        wtd.fdwRevocationChecks = WTD_REVOKE_NONE;
        wtd.dwStateAction = WTD_STATEACTION_VERIFY;
        GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
        LONG res = WinVerifyTrust(NULL, &action, &wtd);
        wtd.dwStateAction = WTD_STATEACTION_CLOSE;
        WinVerifyTrust(NULL, &action, &wtd);
        if (res == ERROR_SUCCESS) {
            isCatalogSigned = true;
            break;
        }
        hCatInfo = CryptCATAdminEnumCatalogFromHash(hCatAdmin, pbHash, dwHashSize, 0, &hCatInfo);
    }
    if (hCatInfo)
        CryptCATAdminReleaseCatalogContext(hCatAdmin, hCatInfo, 0);
    CryptCATAdminReleaseContext(hCatAdmin, 0);
    delete[] pbHash;
    return isCatalogSigned;
}

std::string getDigitalSignature(const std::string& filePath) {
    auto wpath = utf8_to_wstring(filePath);
    if (GetFileAttributesW(wpath.c_str()) == INVALID_FILE_ATTRIBUTES)
        return "Not signed";
    WINTRUST_FILE_INFO fileInfo = {};
    fileInfo.cbStruct = sizeof(fileInfo);
    fileInfo.pcwszFilePath = wpath.c_str();
    GUID guidAction = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA winTrustData = {};
    winTrustData.cbStruct = sizeof(winTrustData);
    winTrustData.dwUIChoice = WTD_UI_NONE;
    winTrustData.fdwRevocationChecks = WTD_REVOKE_NONE;
    winTrustData.dwUnionChoice = WTD_CHOICE_FILE;
    winTrustData.dwStateAction = WTD_STATEACTION_VERIFY;
    winTrustData.pFile = &fileInfo;
    LONG status = WinVerifyTrust(NULL, &guidAction, &winTrustData);
    std::string result = (status == ERROR_SUCCESS) ? "Signed" : "Not signed";
    if (status == ERROR_SUCCESS) {
        auto pProvData = WTHelperProvDataFromStateData(winTrustData.hWVTStateData);
        if (pProvData) {
            auto nonConst = const_cast<CRYPT_PROVIDER_DATA*>(pProvData);
            auto pProvSigner = WTHelperGetProvSignerFromChain(nonConst, 0, FALSE, 0);
            if (pProvSigner) {
                auto pProvCert = WTHelperGetProvCertFromChain(pProvSigner, 0);
                if (pProvCert && pProvCert->pCert) {
                    PCCERT_CONTEXT signingCert = pProvCert->pCert;
                    char subjName[256] = {};
                    if (CertNameToStrA(
                        signingCert->dwCertEncodingType,
                        &signingCert->pCertInfo->Subject,
                        CERT_X500_NAME_STR,
                        subjName,
                        sizeof(subjName)) > 1) {
                        std::string subj(subjName);
                        std::transform(subj.begin(), subj.end(), subj.begin(), ::tolower);
                        static const char* cheats[] = {
                            "manthe industries, llc",
                            "slinkware",
                            "amstion limited",
                            "55.604.504 rafael ferreira de carvalho"
                        };
                        for (auto c : cheats) {
                            if (subj.find(c) != std::string::npos) {
                                result = "Cheat Signature";
                                break;
                            }
                        }
                    }
                    DWORD hashLen = 0;
                    if (CertGetCertificateContextProperty(signingCert, CERT_SHA1_HASH_PROP_ID, nullptr, &hashLen)) {
                        std::vector<BYTE> hash(hashLen);
                        if (CertGetCertificateContextProperty(signingCert, CERT_SHA1_HASH_PROP_ID, hash.data(), &hashLen)) {
                            CRYPT_HASH_BLOB blob = { hashLen, hash.data() };
                            static const LPCWSTR storeNames[] = {
                                L"MY", L"Root", L"Trust", L"CA", L"UserDS",
                                L"TrustedPublisher", L"Disallowed", L"AuthRoot",
                                L"TrustedPeople", L"ClientAuthIssuer",
                                L"CertificateEnrollment", L"SmartCardRoot"
                            };
                            const DWORD contexts[] = {
                                CERT_SYSTEM_STORE_CURRENT_USER | CERT_STORE_OPEN_EXISTING_FLAG,
                                CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_OPEN_EXISTING_FLAG
                            };
                            bool foundAnywhere = false;
                            for (DWORD ctx : contexts) {
                                for (auto storeName : storeNames) {
                                    HCERTSTORE hStore = CertOpenStore(
                                        CERT_STORE_PROV_SYSTEM_W,
                                        0,
                                        NULL,
                                        ctx,
                                        storeName
                                    );
                                    if (!hStore) continue;
                                    PCCERT_CONTEXT foundContext = CertFindCertificateInStore(
                                        hStore,
                                        signingCert->dwCertEncodingType,
                                        0,
                                        CERT_FIND_SHA1_HASH,
                                        &blob,
                                        NULL
                                    );
                                    if (foundContext) {
                                        foundAnywhere = true;
                                        CertFreeCertificateContext(foundContext);
                                    }
                                    CertCloseStore(hStore, 0);
                                    if (foundAnywhere) break;
                                }
                                if (foundAnywhere) break;
                            }
                            if (foundAnywhere) {
                                result = "Fake Signature";
                            }
                        }
                    }
                }
            }
        }
    }
    else {
        if (VerifyFileViaCatalog(wpath.c_str())) {
            result = "Signed";
        }
    }
    winTrustData.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(NULL, &guidAction, &winTrustData);
    return result;
}

bool isMZFile(const std::string& path) {
    auto wpath = utf8_to_wstring(path);
    HANDLE hFile = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    WORD mzHeader;
    DWORD bytesRead;
    bool result = ReadFile(hFile, &mzHeader, sizeof(mzHeader), &bytesRead, NULL) &&
        bytesRead == sizeof(mzHeader) &&
        mzHeader == 0x5A4D;
    CloseHandle(hFile);
    return result;
}

std::string getOwnDirectory() {
    std::string own = getOwnPath();
    std::wstring wown = utf8_to_wstring(own);
    size_t pos = wown.find_last_of(L"\\/");
    return wstring_to_utf8(wown.substr(0, pos + 1));
}

bool hasInvalidSemicolonPath(const std::string& str) {
    size_t pos = 0;
    while ((pos = str.find(';', pos)) != std::string::npos) {
        if (pos + 2 < str.size() && str.substr(pos + 2, 2) == ":\\") {
            return true;
        }
        pos++;
    }
    return false;
}

bool isValidPathToProcess(const std::string& path, bool searchfordll) {
    if (!path.empty() && path.back() == '\\' && !file_exists(path)) {
        return false;
    }
    if (hasInvalidSemicolonPath(path)) {
        return false;
    }
    if (searchfordll) {
        return isDllFile(path);
    }
    else {
        return true;
    }
}

std::string extractValidPath(const std::string& line) {
    // Sadece path benzeri kısımları ayıklayan RegEx
    // Örnek yakalama: "C:\Users\USER\Desktop\injector.exe" 
    // JSON çöplerini ve komut satırı parametrelerini atlar.
    static const std::regex pathRegex(R"([A-Za-z]:\\[^\s"\*\?\|<>]+)");
    std::smatch match;
    
    if (std::regex_search(line, match, pathRegex)) {
        std::string path = match.str(0);
        // Eğer sonda virgül vs. kaldıysa temizle
        while (!path.empty() && (path.back() == ',' || path.back() == '}' || path.back() == ']' || path.back() == ';' || path.back() == '\'')) {
            path.pop_back();
        }
        if (isValidPathToProcess(path, scanForDLLsOnly)) {
            return path;
        }
    }

    // Fallback: Eski basit mantık (RegEx patlarsa diye)
    size_t p = line.find(":\\"), p2 = p;
    if (p == std::string::npos) p2 = line.find(":/");
    if (p2 == std::string::npos || p2 == 0) return "";
    
    // Geçerli bir karakterle mi başlıyor? (Drive letter)
    char dl = line[p2 - 1];
    if (!std::isalpha(dl)) return "";
    
    // Virgüle, tırnağa veya boşluğa kadar al
    size_t endPos = line.find_first_of(" \",;{}[]*", p2);
    std::string path;
    if (endPos != std::string::npos) {
        path = line.substr(p2 - 1, endPos - (p2 - 1));
    } else {
        path = line.substr(p2 - 1);
    }
    
    std::replace(path.begin(), path.end(), '/', '\\');
    
    // Sondaki gereksiz karakterleri tekrar temizle
    while (!path.empty() && (path.back() == ',' || path.back() == '}' || path.back() == ']' || path.back() == ';' || path.back() == '\'')) {
        path.pop_back();
    }
    
    if (!isValidPathToProcess(path, scanForDLLsOnly)) return "";
    return path;
}
bool is_directory(const std::string& path) {
    DWORD attributes = GetFileAttributesA(path.c_str());
    return (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY));
}

std::string rtrim(const std::string& s) {
    size_t end = s.find_last_not_of(" \t");
    return (end == std::string::npos) ? "" : s.substr(0, end + 1);
}

std::vector<std::string> readPathsFromFile(const std::string& filePath) {
    std::vector<std::string> paths;
    std::unordered_set<std::string> uniquePaths;
    std::ifstream f(filePath);
    std::string line;
    while (std::getline(f, line)) {
        line = rtrim(line);
        std::string p = extractValidPath(line);
        if (!p.empty() && uniquePaths.insert(p).second) {
            paths.push_back(p);
        }
    }
    return paths;
}

std::vector<std::string> getAllTargetPaths() {
    std::vector<std::string> allPaths;
    std::vector<std::string> targetFiles = {
        "search results.txt",
        "paths.txt",
        "p.txt"
    };
    for (auto& fn : targetFiles) {
        std::string full = "C:\\" + fn;
        if (file_exists(full)) {
            std::cout << "Found and reading: " << full << std::endl;
            auto v = readPathsFromFile(full);
            allPaths.insert(allPaths.end(), v.begin(), v.end());
        }
    }
    std::string dir = getOwnDirectory();
    for (auto& fn : targetFiles) {
        std::string full = dir + fn;
        if (file_exists(full)) {
            std::cout << "Found and reading: " << full << std::endl;
            auto v = readPathsFromFile(full);
            allPaths.insert(allPaths.end(), v.begin(), v.end());
        }
    }
    return allPaths;
}
