#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include "imgui\imgui.h"
#include "imgui\imgui_impl_win32.h"
#include "imgui\imgui_impl_dx9.h"
#include "imgui\imgui_internal.h"
#include "Include.h"
#include "ProcessMemoryScanner.h"
#include <thread>
#include <mutex>
#include <atomic>
#include <sstream>
#include <vector>
#include <string>
#include <algorithm>
#include <unordered_map>
#include <shellapi.h>
#include "font.h"

// Icon loading logic
struct IconData {
    LPDIRECT3DTEXTURE9 Texture = nullptr;
    int Width = 0;
    int Height = 0;
    bool IsLoaded = false;
};

std::string ExtractBasePathFromADS(const std::string& fullPath) {
    size_t adsPos = fullPath.find(':');
    if (adsPos != std::string::npos && adsPos < 3) {
        adsPos = fullPath.find(':', adsPos + 1);
    }
    if (adsPos != std::string::npos) {
        return fullPath.substr(0, adsPos);
    }
    return fullPath;
}

bool LoadFileIcon(const std::string& filePath, IconData* outIconData, LPDIRECT3DDEVICE9 device) {
    if (filePath.empty() || device == nullptr)
        return false;
    std::string basePath = ExtractBasePathFromADS(filePath);
    DWORD fileAttributes = GetFileAttributesA(basePath.c_str());
    bool fileExists = (fileAttributes != INVALID_FILE_ATTRIBUTES);
    SHFILEINFOA shfi = { 0 };
    DWORD dwFlags = SHGFI_ICON | SHGFI_LARGEICON;
    if (!fileExists) {
        dwFlags |= SHGFI_USEFILEATTRIBUTES;
    }
    DWORD_PTR result = SHGetFileInfoA(basePath.c_str(), fileExists ? 0 : FILE_ATTRIBUTE_NORMAL, &shfi, sizeof(SHFILEINFOA), dwFlags);
    if (result == 0 || shfi.hIcon == NULL)
        return false;
    ICONINFO iconInfo;
    if (!GetIconInfo(shfi.hIcon, &iconInfo)) {
        DestroyIcon(shfi.hIcon);
        return false;
    }
    BITMAP bm;
    if (!GetObject(iconInfo.hbmColor, sizeof(BITMAP), &bm)) {
        DeleteObject(iconInfo.hbmMask);
        DeleteObject(iconInfo.hbmColor);
        DestroyIcon(shfi.hIcon);
        return false;
    }
    HDC hDC = CreateCompatibleDC(NULL);
    HBITMAP oldBitmap = (HBITMAP)SelectObject(hDC, iconInfo.hbmColor);
    int width = bm.bmWidth;
    int height = bm.bmHeight;
    LPDIRECT3DTEXTURE9 texture = nullptr;
    HRESULT hr = device->CreateTexture(width, height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, NULL);
    if (FAILED(hr)) {
        SelectObject(hDC, oldBitmap);
        DeleteDC(hDC);
        DeleteObject(iconInfo.hbmMask);
        DeleteObject(iconInfo.hbmColor);
        DestroyIcon(shfi.hIcon);
        return false;
    }
    D3DLOCKED_RECT lockedRect;
    texture->LockRect(0, &lockedRect, NULL, 0);
    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(BITMAPINFO));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    GetDIBits(hDC, iconInfo.hbmColor, 0, height, lockedRect.pBits, &bmi, DIB_RGB_COLORS);
    texture->UnlockRect(0);
    texture->SetAutoGenFilterType(D3DTEXF_LINEAR);
    outIconData->Texture = texture;
    outIconData->Width = width;
    outIconData->Height = height;
    outIconData->IsLoaded = true;
    SelectObject(hDC, oldBitmap);
    DeleteDC(hDC);
    DeleteObject(iconInfo.hbmMask);
    DeleteObject(iconInfo.hbmColor);
    DestroyIcon(shfi.hIcon);
    return true;
}
// -----------------------------------------------------------------------
// D3D9 globals
// -----------------------------------------------------------------------
static LPDIRECT3D9              g_pD3D = nullptr;
static LPDIRECT3DDEVICE9        g_pd3dDevice = nullptr;
static D3DPRESENT_PARAMETERS    g_d3dpp = {};
static HWND                     g_hWnd = nullptr;

static std::mutex               g_resultMutex;

// Raw terminal output (kept for backward compat)
static std::vector<std::pair<ImVec4, std::string>> g_results;

// Structured file results for table view
struct FileResult {
    std::string path;
    bool        fileExists   = false;
    bool        isValidPE    = false;
    std::string sigStatus;
    std::vector<std::string> yaraMatches;
    bool        hasReplace   = false;
};
static std::mutex               g_fileResultMutex;
static std::vector<FileResult>  g_fileResults;

static std::atomic<bool>        g_scanRunning{ false };
static std::atomic<float>       g_scanProgress{ 0.0f };

// GUI state
static bool g_scanExplorer = true;
static bool g_scanAppInfo = true;
static bool g_scanCSRSS = true;
static bool g_scanMyYara = true;
static bool g_scanOwnYara = false;
static bool g_scanForReplaces = false;
static bool g_scanForDLLsOnly = false;
static bool g_memScanOnly = false;

static std::string g_statusText = "Ready.";



// -----------------------------------------------------------------------
// Forward declares
// -----------------------------------------------------------------------
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
void    AddResult(const ImVec4& color, const std::string& text);
void    RunAnalysis();

// -----------------------------------------------------------------------
// Glue: forward the console output helpers to our GUI log
// -----------------------------------------------------------------------
static std::wstring utf8_to_wstring(const std::string& str) {
    if (str.empty()) return {};
    int sz = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0);
    std::wstring w(sz, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), &w[0], sz);
    return w;
}
static std::string wstring_to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int sz = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    std::string s(sz, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), &s[0], sz, nullptr, nullptr);
    return s;
}

YR_RULES* g_compiled_rules = nullptr;
bool scanMyYara = false;
bool scanOwnYara = false;
bool scanForReplaces = false;
bool scanForDLLsOnly = false;

std::mutex cacheMutex;
std::mutex consoleMutex;
std::mutex replaceMutex;

// -----------------------------------------------------------------------
// Worker — identical logic to original but writes to g_results
// -----------------------------------------------------------------------
void process_paths_worker(const std::vector<std::string>& paths, size_t start_index, size_t end_index)
{
    for (size_t i = start_index; i < end_index; ++i) {
        const std::string& path = paths[i];
        FileInfo info;
        bool found_in_cache = false;
        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            auto it = fileCache.find(path);
            if (it != fileCache.end()) { info = it->second; found_in_cache = true; }
        }
        if (!found_in_cache) {
            info.exists = file_exists(path);
            if (info.exists) {
                info.isDirectory = is_directory(path);
                info.isValidMZ = !info.isDirectory && isMZFile(path);
            }
            if (info.exists && info.isValidMZ) {
                info.signatureStatus = getDigitalSignature(path);
                if (info.signatureStatus != "Signed" && (scanMyYara || scanOwnYara)) {
                    if (!iequals(path, getOwnPath()))
                        scan_with_yara(path, info.matched_rules, g_compiled_rules);
                }
            }
            { std::lock_guard<std::mutex> lock(cacheMutex); fileCache.insert_or_assign(path, info); }
        }

        if (info.exists && info.isDirectory) continue;

        // Build structured FileResult for table view
        FileResult fr;
        fr.path = path;
        fr.fileExists = info.exists;
        fr.isValidPE = info.isValidMZ;
        fr.sigStatus = info.signatureStatus;
        fr.hasReplace = false;

        if (info.exists && info.isValidMZ) {
            if (info.signatureStatus == "Signed") {
                AddResult(ImVec4(0.0f,1.0f,0.0f,1.0f), "Mevcut    ");
                AddResult(ImVec4(0.0f,1.0f,0.0f,1.0f), info.signatureStatus + "    ");
            }
            else if (info.signatureStatus == "Not signed") {
                AddResult(ImVec4(0.0f,1.0f,0.0f,1.0f), "Mevcut    ");
                AddResult(ImVec4(1.0f,0.0f,0.0f,1.0f), info.signatureStatus + "    ");
            }
            else {
                AddResult(ImVec4(0.0f,1.0f,0.0f,1.0f), "Mevcut    ");
                AddResult(ImVec4(1.0f,0.0f,0.0f,1.0f), info.signatureStatus + "  ");
            }
            AddResult(ImVec4(1.0f,1.0f,1.0f,1.0f), path);

            std::string filename;
            size_t pos = path.find_last_of("\\/");
            filename = (pos != std::string::npos) ? path.substr(pos + 1) : path;
            if (scanForReplaces) {
                std::lock_guard<std::mutex> repl(replaceMutex);
                FindReplace(filename);
                fr.hasReplace = true;
            }
            if (!info.matched_rules.empty()) {
                std::string rules;
                for (auto& rn : info.matched_rules) {
                    rules += "[" + rn + "]";
                    fr.yaraMatches.push_back(rn);
                }
                AddResult(ImVec4(1.0f,0.0f,0.0f,1.0f), rules);
            }
        }
        else if (info.exists && !info.isValidMZ) {
            AddResult(ImVec4(0.0f,1.0f,0.0f,1.0f), "Mevcut    ");
            AddResult(ImVec4(1.0f,1.0f,0.0f,1.0f), "PE değil   ");
            AddResult(ImVec4(1.0f,1.0f,1.0f,1.0f), path);
        }
        else if (!info.exists) {
            AddResult(ImVec4(1.0f,0.0f,0.0f,1.0f), "Silinmiş    ");
            AddResult(ImVec4(1.0f,1.0f,1.0f,1.0f), path);
        }
        AddResult(ImVec4(1.0f,1.0f,1.0f,1.0f), "\n");

        // Store structured result
        {
            std::lock_guard<std::mutex> lock(g_fileResultMutex);
            g_fileResults.push_back(fr);
        }
    }
}

// -----------------------------------------------------------------------
// Thread-safe result logging
// -----------------------------------------------------------------------
void AddResult(const ImVec4& color, const std::string& text)
{
    std::lock_guard<std::mutex> lock(g_resultMutex);
    g_results.emplace_back(color, text);
}

// -----------------------------------------------------------------------
// Main analysis runner (runs in a background thread)
// -----------------------------------------------------------------------
void RunAnalysis()
{
    g_results.clear();
    { std::lock_guard<std::mutex> lock(g_fileResultMutex); g_fileResults.clear(); }
    g_scanRunning = true;
    g_scanProgress = 0.0f;
    g_statusText = "Taranıyor...";

    std::thread([&]() {
        // Enable privilege
        privilege("SeDebugPrivilege");

        // Init YARA
        if (yr_initialize() != ERROR_SUCCESS) {
            g_statusText = "YARA init failed!";
            g_scanRunning = false;
            return;
        }

        scanMyYara = g_scanMyYara;
        scanOwnYara = g_scanOwnYara;
        scanForReplaces = g_scanForReplaces;
        scanForDLLsOnly = g_scanForDLLsOnly;

        genericRules.clear();
        if (scanMyYara)  initializeGenericRules();
        if (scanOwnYara) initializateCustomRules();

        if (scanMyYara || scanOwnYara) {
            YR_COMPILER* compiler = nullptr;
            yr_compiler_create(&compiler);
            yr_compiler_set_callback(compiler, compiler_error_callback, nullptr);
            int compile_errors = 0;
            for (auto& r : genericRules)
                compile_errors += yr_compiler_add_string(compiler, r.rule.c_str(), r.name.c_str());
            
            if (compile_errors == 0) {
                yr_compiler_get_rules(compiler, &g_compiled_rules);
            } else {
                AddResult(ImVec4(1.0f,0.0f,0.0f,1.0f), "[YARA] Kurallar derlenirken " + std::to_string(compile_errors) + " adet hata tespit edildi!\n");
            }
            yr_compiler_destroy(compiler);
        }

        if (scanForReplaces) {
            initReplaceParser();
            PreProcessReplacements(replaceParserDir + "\\replaces.txt");
        }

        // --- Memory scan ---
        std::vector<std::string> memoryPaths;
        if (g_scanCSRSS || g_scanExplorer || g_scanAppInfo) {
            MemoryScanConfig memCfg;
            memCfg.scanCSRSS = g_scanCSRSS;
            memCfg.scanExplorer = g_scanExplorer;
            memCfg.scanAppInfo = g_scanAppInfo;
            memoryPaths = ProcessMemoryScanner::GetSuspiciousPathsFromMemory(memCfg);
            AddResult(ImVec4(0.0f,1.0f,1.0f,1.0f),
                "[Bellek] " + std::to_string(memoryPaths.size()) + " yol bulundu.\n");
        }

        // --- Get paths from files ---
        auto paths = getAllTargetPaths();
        size_t before = paths.size();

        for (auto& mp : memoryPaths) {
            std::string extPath = extractValidPath(mp);
            if (!extPath.empty()) {
                if (std::find(paths.begin(), paths.end(), extPath) == paths.end())
                    paths.push_back(extPath);
            }
        }
        AddResult(ImVec4(0.0f,1.0f,1.0f,1.0f),
            "[Bellek] " + std::to_string(paths.size() - before) + " yeni yol eklendi.\n");

        if (paths.empty()) {
            AddResult(ImVec4(1.0f,0.0f,0.0f,1.0f), "Geçerli yol bulunamadı.\n");
            g_statusText = "Yol bulunamadı.";
            g_scanRunning = false;
            if (g_compiled_rules) yr_rules_destroy(g_compiled_rules);
            yr_finalize();
            return;
        }

        // --- Multi-threaded processing ---
        unsigned th = std::thread::hardware_concurrency();
        unsigned num_threads = th > 0 ? th : 1;
        size_t total = paths.size();
        size_t per = (total + num_threads - 1) / num_threads;

        AddResult(ImVec4(0.0f,1.0f,1.0f,1.0f), std::to_string(total)
            + " yol " + std::to_string(num_threads) + " iş parçacığı ile işleniyor...\n");

        std::vector<std::thread> workers;
        size_t idx = 0;
        for (unsigned t = 0; t < num_threads && idx < total; ++t) {
            size_t end = std::min(idx + per, total);
            workers.emplace_back(process_paths_worker, std::cref(paths), idx, end);
            idx = end;
        }
        for (auto& w : workers) if (w.joinable()) w.join();

        g_statusText = "Tamamlandı.";
        g_scanProgress = 1.0f;
        g_scanRunning = false;

        // Cleanup
        if (scanForReplaces) {
            DestroyReplaceParser();
            WriteAllReplacementsToFileAndPrintSummary();
        }
        if (g_compiled_rules) { yr_rules_destroy(g_compiled_rules); g_compiled_rules = nullptr; }
        yr_finalize();
    }).detach();
}

// ========================================================================
// ImGui / Win32 / D3D9 setup
// ========================================================================
// ImVec4 helper: hex #RRGGBB -> normalized float4
static ImVec4 hex(unsigned int rgb, float a = 1.0f) {
    return ImVec4(
        ((rgb >> 16) & 0xFF) / 255.0f,
        ((rgb >> 8) & 0xFF) / 255.0f,
        (rgb & 0xFF) / 255.0f,
        a
    );
}

// Card helper: rounded dark panel with neon border
static void BeginCard(const char* label, float w, float h) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, hex(0x121212));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, hex(0x2A2A2A));
    ImGui::BeginChild(label, ImVec2(w, h), true,
        ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}
static void EndCard() {
    ImGui::EndChild();
}

static void SetupImGuiStyle()
{
    ImGui::StyleColorsDark();
}

static bool CreateDeviceD3D(HWND hWnd)
{
    if ((g_pD3D = Direct3DCreate9(D3D_SDK_VERSION)) == nullptr)
        return false;

    ZeroMemory(&g_d3dpp, sizeof(g_d3dpp));
    g_d3dpp.Windowed = TRUE;
    g_d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    g_d3dpp.BackBufferFormat = D3DFMT_UNKNOWN;
    g_d3dpp.EnableAutoDepthStencil = TRUE;
    g_d3dpp.AutoDepthStencilFormat = D3DFMT_D16;
    g_d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;

    if (g_pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd,
        D3DCREATE_HARDWARE_VERTEXPROCESSING, &g_d3dpp, &g_pd3dDevice) < 0)
        return false;
    return true;
}

static void CleanupDeviceD3D()
{
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
    if (g_pD3D) { g_pD3D->Release(); g_pD3D = nullptr; }
}

static void ResetDevice()
{
    ImGui_ImplDX9_InvalidateDeviceObjects();
    HRESULT hr = g_pd3dDevice->Reset(&g_d3dpp);
    if (hr == D3DERR_INVALIDCALL) IM_ASSERT(0);
    ImGui_ImplDX9_CreateDeviceObjects();
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;
    switch (msg) {
        case WM_SIZE: {
            if (g_pd3dDevice && wParam != SIZE_MINIMIZED) {
                g_d3dpp.BackBufferWidth = LOWORD(lParam);
                g_d3dpp.BackBufferHeight = HIWORD(lParam);
                ResetDevice();
            }
            return 0;
        }
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ========================================================================
// WinMain — entry point
// ========================================================================
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
    // Create window
    WNDCLASSEXW wc = {
        sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L,
        hInstance, nullptr, nullptr, nullptr, nullptr,
        L"PathsParser", nullptr
    };
    RegisterClassExW(&wc);
    g_hWnd = CreateWindowW(L"PathsParser", L"PathsParser",
        WS_OVERLAPPEDWINDOW, 100, 100, 1280, 800, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hWnd) return 1;

    if (!CreateDeviceD3D(g_hWnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(L"PathsParser", wc.hInstance);
        return 1;
    }

    ShowWindow(g_hWnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hWnd);

    // Setup ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    SetupImGuiStyle();
    ImFontConfig CustomFont;
    CustomFont.FontDataOwnedByAtlas = false;
    static const ImWchar turkish_ranges[] = {
        0x0020, 0x00FF,
        0x0100, 0x017F,
        0
    };
    CustomFont.GlyphRanges = turkish_ranges;
    io.Fonts->AddFontFromMemoryTTF((void*)Custom.data(), (int)Custom.size(), 17.5f, &CustomFont);
    io.Fonts->AddFontDefault();

    ImGui_ImplWin32_Init(g_hWnd);
    ImGui_ImplDX9_Init(g_pd3dDevice);

    // Main loop
    MSG msg;
    ZeroMemory(&msg, sizeof(msg));
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }

        // Render
        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // -------- GUI ----------  (BAM-parser yapısının birebir kopyası)
        {
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(5, 5));
            ImGui::Begin("##MainWindow", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
            ImGui::GetIO().IniFilename = nullptr;

            static bool showYaraOnly = false;
            static bool showNotSignedOnly = false;
            static bool showReplaceOnly = false;
            static bool showDeletedOnly = false;
            static bool showDetailsPopup = false;
            static char searchBuffer[256] = "";
            static std::unordered_map<std::string, IconData> iconCache;

            if (g_scanRunning) {
                const float windowCenterX = (ImGui::GetWindowSize().x - ImGui::CalcTextSize("Taranıyor...").x) * 0.5f;
                const float windowCenterY = (ImGui::GetWindowSize().y - ImGui::CalcTextSize("Taranıyor...").y) * 0.5f;
                ImGui::SetCursorPos(ImVec2(windowCenterX, windowCenterY));
                ImGui::Text("Taranıyor...");
            }
            else {
                if (showDetailsPopup) {
                    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.0f, 0.0f, 0.0f, 0.6f));
                    ImGui::OpenPopup("Değiştirme Detayları");
                    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
                    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
                    ImGui::SetNextWindowSize(ImVec2(800, 600));
                    if (ImGui::BeginPopupModal("Değiştirme Detayları", &showDetailsPopup, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
                        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 20));
                        ImGui::Text("Replace Bilgisi:");
                        ImGui::Spacing();
                        ImGui::Spacing();
                        ImGui::Text("(Replace detayları burada gösterilecek)");
                        ImGui::PopStyleVar();
                        ImGui::EndPopup();
                    }
                    ImGui::PopStyleColor();
                }

                ImGui::BeginDisabled(g_scanRunning);
                if (ImGui::Button("TARA", ImVec2(100, 30))) {
                    RunAnalysis();
                }
                ImGui::EndDisabled();

                ImGui::SameLine();
                ImGui::BeginDisabled(g_scanRunning);
                ImGui::Checkbox("YARA",       &g_scanMyYara);
                ImGui::SameLine();
                ImGui::Checkbox("Özel YARA",  &g_scanOwnYara);
                ImGui::SameLine();
                ImGui::Checkbox("Replace",    &g_scanForReplaces);
                ImGui::SameLine();
                ImGui::Checkbox("Sadece DLL", &g_scanForDLLsOnly);
                ImGui::SameLine();
                ImGui::Text("Bellek:");
                ImGui::SameLine();
                ImGui::Checkbox("CSRSS",    &g_scanCSRSS);
                ImGui::SameLine();
                ImGui::Checkbox("Explorer", &g_scanExplorer);
                ImGui::SameLine();
                ImGui::Checkbox("AppInfo",  &g_scanAppInfo);
                ImGui::EndDisabled();

                ImGui::Checkbox("Sadece YARA",    &showYaraOnly);
                ImGui::SameLine();
                ImGui::Checkbox("Sadece İmzasız", &showNotSignedOnly);
                ImGui::SameLine();
                ImGui::Checkbox("Sadece Replace", &showReplaceOnly);
                ImGui::SameLine();
                ImGui::Checkbox("Sadece Silinen", &showDeletedOnly);

                float searchWidth = 450.0f;
                float padding = 25.0f;
                ImGui::SameLine(ImGui::GetWindowWidth() - searchWidth - padding - ImGui::CalcTextSize("Ara").x);
                ImGui::InputTextEx("Ara", NULL, searchBuffer, (int)IM_ARRAYSIZE(searchBuffer), ImVec2(searchWidth, 0), 0, NULL, NULL);

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                {
                    std::lock_guard<std::mutex> lock(g_fileResultMutex);

                    if (g_fileResults.empty()) {
                        ImGui::Text("%s", g_statusText.c_str());
                    }
                    else {
                        float pathMaxWidth = ImGui::CalcTextSize("Yol").x;
                        float durumMaxWidth = ImGui::CalcTextSize("Durum").x;
                        float imzaMaxWidth = ImGui::CalcTextSize("İmza").x;
                        float yaraMaxWidth = ImGui::CalcTextSize("YARA").x;
                        float replMaxWidth = ImGui::CalcTextSize("Replace").x;
                        for (const auto& r : g_fileResults) {
                            std::string yaraStr;
                            for (size_t i = 0; i < r.yaraMatches.size(); i++) {
                                if (i > 0) yaraStr += ", ";
                                yaraStr += r.yaraMatches[i];
                            }
                            pathMaxWidth = std::max(pathMaxWidth, ImGui::CalcTextSize(r.path.c_str()).x);
                            imzaMaxWidth = std::max(imzaMaxWidth, ImGui::CalcTextSize(r.sigStatus.c_str()).x);
                            yaraMaxWidth = std::max(yaraMaxWidth, ImGui::CalcTextSize(yaraStr.c_str()).x);
                        }
                        pathMaxWidth += 30;
                        durumMaxWidth += 30;
                        imzaMaxWidth += 30;
                        yaraMaxWidth += 30;
                        replMaxWidth += 30;

                        int colIdCounter = 0;
                        auto SelectableText = [&](const char* label, const char* text_to_copy, bool& clicked) {
                            ImGui::PushID(colIdCounter++);
                            bool selected = false;
                            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.2f, 0.2f, 0.2f, 0.5f));
                            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.3f, 0.3f, 0.3f, 0.5f));
                            ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.4f, 0.4f, 0.4f, 0.5f));
                            if (ImGui::Selectable(label, &selected, ImGuiSelectableFlags_None)) {
                                if (ImGui::GetIO().KeyCtrl) {
                                    ImGui::SetClipboardText(text_to_copy);
                                    clicked = true;
                                }
                                else {
                                    clicked = true;
                                }
                            }
                            ImGui::PopStyleColor(3);
                            ImGui::PopID();
                        };

                        auto toLower = [](const std::string& s) -> std::string {
                            std::string lower = s;
                            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
                            return lower;
                        };

                        if (ImGui::BeginTable("PathsTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY)) {
                            ImGui::TableSetupColumn("Yol",     ImGuiTableColumnFlags_WidthStretch, pathMaxWidth);
                            ImGui::TableSetupColumn("Durum",   ImGuiTableColumnFlags_WidthFixed, durumMaxWidth);
                            ImGui::TableSetupColumn("İmza",    ImGuiTableColumnFlags_WidthFixed, imzaMaxWidth);
                            ImGui::TableSetupColumn("YARA",    ImGuiTableColumnFlags_WidthFixed, yaraMaxWidth);
                            ImGui::TableSetupColumn("Replace", ImGuiTableColumnFlags_WidthFixed, replMaxWidth);
                            ImGui::TableHeadersRow();
                            int rowIndex = 0;
                            for (const auto& r : g_fileResults) {
                                bool shouldShow = true;
                                if (showYaraOnly && r.yaraMatches.empty()) shouldShow = false;
                                if (showNotSignedOnly && r.sigStatus == "Signed") shouldShow = false;
                                if (showReplaceOnly && !r.hasReplace) shouldShow = false;
                                if (showDeletedOnly && r.fileExists) shouldShow = false;
                                if (!shouldShow) continue;

                                std::string searchQuery(searchBuffer);
                                if (!searchQuery.empty()) {
                                    std::string lowerSearch = toLower(searchQuery);
                                    std::string lowerPath = toLower(r.path);
                                    std::string lowerSig = toLower(r.sigStatus);
                                    std::string yaraStr;
                                    for (size_t i = 0; i < r.yaraMatches.size(); i++) {
                                        if (i > 0) yaraStr += ", ";
                                        yaraStr += r.yaraMatches[i];
                                    }
                                    std::string lowerYara = toLower(yaraStr);
                                    if (lowerPath.find(lowerSearch) == std::string::npos &&
                                        lowerSig.find(lowerSearch) == std::string::npos &&
                                        lowerYara.find(lowerSearch) == std::string::npos) {
                                        continue;
                                    }
                                }

                                ImGui::PushID(rowIndex++);
                                colIdCounter = 0; // Her satırda sıfırla
                                ImGui::TableNextRow();
                                bool clicked = false;

                                // Yol
                                ImGui::TableNextColumn();
                                IconData icon;
                                bool hasIcon = false;
                                auto it = iconCache.find(r.path);
                                if (it == iconCache.end()) {
                                    IconData loadedIcon;
                                    if (LoadFileIcon(r.path, &loadedIcon, g_pd3dDevice)) {
                                        iconCache[r.path] = loadedIcon;
                                        icon = loadedIcon;
                                        hasIcon = true;
                                    }
                                }
                                else {
                                    icon = it->second;
                                    hasIcon = icon.IsLoaded;
                                }
                                if (hasIcon) {
                                    ImGui::Image((ImTextureID)icon.Texture, ImVec2((float)icon.Width * 0.5f, (float)icon.Height * 0.5f));
                                    ImGui::SameLine();
                                }
                                if (!r.yaraMatches.empty() || r.hasReplace)
                                    ImGui::PushStyleColor(ImGuiCol_Text, hex(0xFF6644));
                                else if (!r.fileExists)
                                    ImGui::PushStyleColor(ImGuiCol_Text, hex(0x888888));
                                else
                                    ImGui::PushStyleColor(ImGuiCol_Text, hex(0xCCCCCC));
                                SelectableText(r.path.c_str(), r.path.c_str(), clicked);
                                ImGui::PopStyleColor();

                                // Durum
                                ImGui::TableNextColumn();
                                if (!r.fileExists)
                                    ImGui::PushStyleColor(ImGuiCol_Text, hex(0xFF4444));
                                else if (!r.isValidPE)
                                    ImGui::PushStyleColor(ImGuiCol_Text, hex(0xFFAA00));
                                else
                                    ImGui::PushStyleColor(ImGuiCol_Text, hex(0x00CC66));
                                SelectableText(
                                    !r.fileExists ? "Silinmiş" :
                                    !r.isValidPE  ? "PE değil" : "Mevcut",
                                    !r.fileExists ? "Silinmiş" :
                                    !r.isValidPE  ? "PE değil" : "Mevcut",
                                    clicked);
                                ImGui::PopStyleColor();

                                // İmza
                                ImGui::TableNextColumn();
                                if (r.sigStatus == "Signed")
                                    ImGui::PushStyleColor(ImGuiCol_Text, hex(0x00CC66));
                                else if (r.sigStatus == "Not signed")
                                    ImGui::PushStyleColor(ImGuiCol_Text, hex(0xFF4444));
                                else
                                    ImGui::PushStyleColor(ImGuiCol_Text, hex(0xFFAA00));
                                SelectableText(
                                    r.sigStatus == "Signed"    ? "İmzalı" :
                                    r.sigStatus == "Not signed"? "İmzasız" :
                                    r.sigStatus.empty()        ? "-" : r.sigStatus.c_str(),
                                    r.sigStatus.c_str(), clicked);
                                ImGui::PopStyleColor();

                                // YARA
                                ImGui::TableNextColumn();
                                if (!r.yaraMatches.empty()) {
                                    std::string yaraStr;
                                    for (size_t i = 0; i < r.yaraMatches.size(); i++) {
                                        if (i > 0) yaraStr += ", ";
                                        yaraStr += r.yaraMatches[i];
                                    }
                                    ImGui::PushStyleColor(ImGuiCol_Text, hex(0xFF4444));
                                    SelectableText(yaraStr.c_str(), yaraStr.c_str(), clicked);
                                    ImGui::PopStyleColor();
                                }
                                else {
                                    ImGui::PushStyleColor(ImGuiCol_Text, hex(0x444444));
                                    SelectableText("-", "-", clicked);
                                    ImGui::PopStyleColor();
                                }

                                // Replace
                                ImGui::TableNextColumn();
                                if (r.hasReplace) {
                                    ImGui::PushStyleColor(ImGuiCol_Text, hex(0xFF8800));
                                    SelectableText("Var", "Var", clicked);
                                    ImGui::PopStyleColor();
                                }
                                else {
                                    ImGui::PushStyleColor(ImGuiCol_Text, hex(0x444444));
                                    SelectableText("-", "-", clicked);
                                    ImGui::PopStyleColor();
                                }
                                ImGui::PopID(); // Pop rowIndex
                            }
                            ImGui::EndTable();
                        }
                    }
                }
            }

            ImGui::End();
            ImGui::PopStyleVar();
        }

        ImGui::EndFrame();
        g_pd3dDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
        g_pd3dDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        g_pd3dDevice->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        D3DCOLOR clearColor = D3DCOLOR_RGBA(10, 10, 10, 255);
        g_pd3dDevice->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, clearColor, 1.0f, 0);
        if (g_pd3dDevice->BeginScene() >= 0) {
            ImGui::Render();
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            g_pd3dDevice->EndScene();
        }
        HRESULT hr = g_pd3dDevice->Present(nullptr, nullptr, nullptr, nullptr);
        if (hr == D3DERR_DEVICELOST && g_pd3dDevice->TestCooperativeLevel() == D3DERR_DEVICENOTRESET)
            ResetDevice();
    }

    // Cleanup
    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(g_hWnd);
    UnregisterClassW(L"PathsParser", wc.hInstance);
    return 0;
}
