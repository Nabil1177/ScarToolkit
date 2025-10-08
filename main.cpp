#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include <sstream>
#include <d3d11.h>
#include <Psapi.h>
#include <functional>
#include <thread>
#include <chrono>


#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <cctype>  // for std::isdigit / std::isspace overlay
#include "OverlayWebView.h"



// --- UI helpers for overlay row ---
static void HelpMarker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

struct InputTint {
    ImVec4 old[3];
    InputTint(const ImVec4& normal, const ImVec4& hover, const ImVec4& active) {
        auto& c = ImGui::GetStyle().Colors;
        old[0] = c[ImGuiCol_FrameBg];
        old[1] = c[ImGuiCol_FrameBgHovered];
        old[2] = c[ImGuiCol_FrameBgActive];
        c[ImGuiCol_FrameBg] = normal;
        c[ImGuiCol_FrameBgHovered] = hover;
        c[ImGuiCol_FrameBgActive] = active;
    }
    ~InputTint() {
        auto& c = ImGui::GetStyle().Colors;
        c[ImGuiCol_FrameBg] = old[0];
        c[ImGuiCol_FrameBgHovered] = old[1];
        c[ImGuiCol_FrameBgActive] = old[2];
    }
};

// --- Alternative help hint with yellow tooltip text ---
static inline void HelpHint(const char* desc) {
    ImGui::TextColored(ImVec4(0.3f, 0.7f, 1.0f, 1.0f), "(i)");  // blue info marker
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.3f, 1.0f)); // yellow
        ImGui::TextUnformatted(desc);
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// ===== Compact action table helpers (Label | Amt | +/- | Btn | Fill) =====
namespace { // keep these local to this file
    inline bool BeginCompactActionTable(const char* table_id) {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 3));
        if (ImGui::BeginTable(table_id, 5,
            ImGuiTableFlags_SizingFixedFit |
            ImGuiTableFlags_NoSavedSettings |
            ImGuiTableFlags_BordersInnerV))
        {
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("Amt", ImGuiTableColumnFlags_WidthFixed, 130.0f);
            ImGui::TableSetupColumn("+/-", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn("Btn", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Fill", ImGuiTableColumnFlags_WidthStretch);
            return true;
        }
        ImGui::PopStyleVar(); // if BeginTable failed
        return false;
    }

    inline void EndCompactActionTable() {
        ImGui::EndTable();
        ImGui::PopStyleVar();
    }

    inline void DrawStepperActionRow(
        const char* label,
        const char* id_suffix,
        int& value,
        int step,
        int min_value,
        int max_value,
        const char* button_text,
        const std::function<void(int)>& on_click)
    {
        ImGui::TableNextRow();

        // Col 0: label
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(label);

        // Col 1: amount
        ImGui::TableSetColumnIndex(1);
        ImGui::PushID(id_suffix);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputInt("##amt", &value, 0, 0);
        if (value < min_value) value = min_value;
        if (value > max_value) value = max_value;

        // Col 2: [-][+]
        ImGui::TableSetColumnIndex(2);
        if (ImGui::SmallButton("-")) value = (value - step < min_value ? min_value : value - step);
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        if (ImGui::SmallButton("+")) value = (value + step > max_value ? max_value : value + step);

        // Col 3: action
        ImGui::TableSetColumnIndex(3);
        if (ImGui::Button(button_text, ImVec2(80, 0))) on_click(value);

        // Col 4: filler
        ImGui::TableSetColumnIndex(4);
        ImGui::Dummy(ImVec2(0, 0));

        ImGui::PopID();
    }
} // namespace



// --- Global State & Forward Declarations ---
// Data
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;

// Forward declarations of helper functions
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
// ---- forward decls for window-finder helpers ----
static BOOL CALLBACK EnumFindWindowProc(HWND hwnd, LPARAM lParam);
static HWND FindGameWindowByPid();


// --- Core Injection Logic ---
constexpr wchar_t   GAME_EXE[] = L"RelicCardinal.exe";
static DWORD g_pid = 0;
static HANDLE g_processHandle = nullptr;
static uintptr_t g_helperAddress = 0;
static char g_statusText[256] = "Click 'Find Game' to begin.";

static int g_foodAmount = 50000;
static int g_woodAmount = 50000;
static int g_goldAmount = 50000;
static int g_stoneAmount = 50000;
static int g_oilAmount = 50000;
static char g_customScriptBuffer[2048] = "function CustomScript()\n    -- Paste your script here\nend\nRule_AddOneShot(CustomScript)";
static int g_popCapAmount = 500;
static int g_sheepCount = 10; // default shown in your UI design
static bool g_resourceHUDEnabled = false;
static bool g_godModeEnabled = false;
static bool g_idleTCEnabled = false;

// --- Auto Villager UI macro (key-sim) ---
static bool       g_autoVillMacroEnabled = false;
static int        g_autoVillEverySec = 60;   // every 1 minute
static int        g_autoVillCount = 4;    // Q x4
static ULONGLONG  g_autoVillLastTickMs = 0;
// HWND for the game (used to bring it to the foreground for macros)

static HWND g_gameHwnd = nullptr;

// --- Score HUD ---
static bool        g_scoreHUDEnabled = false;
static std::string g_scoreHudScript = "";   // holds loaded ScoreHUD.lua text


// Keys (top-row number 4 + Q)
static WORD VK_TC_GROUP = '4';        // select TC control-group (top row "4")
static WORD VK_VILLAGER = 'Q';        // villager train hotkey
static WORD VK_CLOSE = VK_ESCAPE;  // close

// ==== AOE4 OVERLAY state ====
static bool        g_overlayPidLoaded = false;
static std::string g_overlayPid;           // saved in overlay_pid.txt
static bool        g_overlayEnabled = false; // UI toggle only (we can persist later if you want)
static char        g_overlayInput[128] = {};
static std::string g_overlayErr;
//overlay

static std::string g_hudScript;


// ===== Overlay helpers (no external deps) =====
static std::wstring GetExeDirW() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring exe(buf);
    size_t p = exe.find_last_of(L"\\/");
    return (p == std::wstring::npos) ? L"." : exe.substr(0, p);
}
static std::wstring OverlayPidFileW() { return GetExeDirW() + L"\\overlay_pid.txt"; }

static bool LoadOverlayPid(std::string& out) {
    out.clear();
    std::wifstream in(OverlayPidFileW());
    if (!in) return false;
    std::wstring w; std::getline(in, w);
    std::string s(w.begin(), w.end());
    auto ltrim = [](std::string& t) { size_t i = 0; while (i < t.size() && std::isspace((unsigned char)t[i])) ++i; t.erase(0, i); };
    auto rtrim = [](std::string& t) { size_t i = t.size(); while (i > 0 && std::isspace((unsigned char)t[i - 1])) --i; t.erase(i); };
    ltrim(s); rtrim(s);
    out = s;
    return !out.empty();
}
static bool SaveOverlayPid(const std::string& pid) {
    std::wofstream out(OverlayPidFileW(), std::ios::trunc);
    if (!out) return false;
    std::wstring w(pid.begin(), pid.end());
    out << w << L"\n";
    return true;
}
static void DeleteOverlayPidFile() { DeleteFileW(OverlayPidFileW().c_str()); }

static std::wstring ToW(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], len);
    return w;
}
// Helper: encode path for file:// URL
static std::wstring EncodePathForFileURL(const std::wstring& path)
{
    std::wstring out; out.reserve(path.size() * 3);
    for (wchar_t c : path) {
        switch (c) {
        case L'\\': out.push_back(L'/'); break;
        case L' ':  out += L"%20"; break;
        case L'#':  out += L"%23"; break;
        case L'?':  out += L"%3F"; break; // only in path part
        case L'%':  out += L"%25"; break;
        case L'"':  out += L"%22"; break;
        default:    out.push_back(c); break;
        }
    }
    return out;
}


static bool LaunchOverlayHtml(const std::string& profileId, int pollMs = 2000)
{
    std::wstring path = GetExeDirW() + L"\\overlay.html";

    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(nullptr, (L"overlay.html not found:\n" + path).c_str(),
            L"Overlay launch failed", MB_ICONERROR);
        return false;
    }

    // Use both query and hash for redundancy
    std::wstring url = L"file:///" + EncodePathForFileURL(path) +
        L"?pid=" + ToW(profileId) + L"&poll=" + ToW(std::to_string(pollMs)) +
        L"#pid=" + ToW(profileId);

    HINSTANCE r = ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)r > 32) return true;

    // Fallback: cmd /c start
    std::wstring params = L"/c start \"\" \"" + url + L"\"";
    r = ShellExecuteW(nullptr, L"open", L"cmd.exe", params.c_str(), nullptr, SW_HIDE);
    return (INT_PTR)r > 32;
}


static std::string MaskId(const std::string& id) {
    if (id.size() <= 6) return id;
    return id.substr(0, 3) + std::string(id.size() - 6, '*') + id.substr(id.size() - 3);
}
// Accept AoE4World players URL or pure digits
static bool ExtractProfileId(const std::string& input, std::string& outPid) {
    outPid.clear();
    std::string s = input;
    auto ltrim = [](std::string& t) { size_t i = 0; while (i < t.size() && std::isspace((unsigned char)t[i])) ++i; t.erase(0, i); };
    auto rtrim = [](std::string& t) { size_t i = t.size(); while (i > 0 && std::isspace((unsigned char)t[i - 1])) --i; t.erase(i); };
    ltrim(s); rtrim(s);
    if (s.empty()) return false;

    size_t pos = s.find("/players/");
    if (pos != std::string::npos) {
        pos += 9;
        while (pos < s.size() && std::isdigit((unsigned char)s[pos])) { outPid.push_back(s[pos]); ++pos; }
        return !outPid.empty();
    }
    bool digits = !s.empty();
    for (unsigned char c : s) { if (!std::isdigit(c)) { digits = false; break; } }
    if (digits) { outPid = s; return true; }
    return false;
}

static void PositionOverlayTopRightOn(HWND refWnd, int w, int h, int margin = 16)
{
    HMONITOR mon = MonitorFromWindow(refWnd ? refWnd : GetDesktopWindow(), MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    if (!GetMonitorInfoW(mon, &mi)) {
        RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        int x = wa.right - w - margin, y = wa.top + margin;
        OV_SetBounds(x, y, w, h);
        return;
    }
    RECT wa = mi.rcWork; // work area excludes taskbar
    int x = wa.right - w - margin;
    int y = wa.top + margin;
    OV_SetBounds(x, y, w, h);
}

//overlay

// ===== AOE4World Overlay Panel (collapsing, compact) =====
static void DrawAoE4OverlayPanel()
{
    if (!g_overlayPidLoaded) { LoadOverlayPid(g_overlayPid); g_overlayPidLoaded = true; }

    ImGui::PushID("Aoe4OverlayInline");
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 4));

    if (g_overlayPid.empty())
    {
        // First-run setup (full width, no table constraints)
        ImGui::TextDisabled("AOE4World Overlay — paste your players link or numeric profile id:");

        const float saveW = 120.0f;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        float inputW = ImGui::GetContentRegionAvail().x - saveW - spacing;
        if (inputW < 240.0f) inputW = 240.0f;

        ImGui::SetNextItemWidth(inputW);
        bool submitted = ImGui::InputTextWithHint(
            "##overlay_pid_input",
            "https://aoe4world.com/players/123456-yourname   or   123456",
            g_overlayInput, IM_ARRAYSIZE(g_overlayInput),
            ImGuiInputTextFlags_EnterReturnsTrue
        );

        ImGui::SameLine();
        if (ImGui::Button("Save", ImVec2(saveW, 0)) || submitted)
        {
            g_overlayErr.clear();
            std::string pid;
            if (ExtractProfileId(g_overlayInput, pid)) {
                g_overlayPid = pid;
                SaveOverlayPid(g_overlayPid);
                memset(g_overlayInput, 0, sizeof(g_overlayInput));
            }
            else {
                g_overlayErr = "Invalid ID/link. Example: https://aoe4world.com/players/123456-yourname";
            }
        }

        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Example:\nhttps://aoe4world.com/players/123456-yourname\n\nOr paste the numeric profile id (digits only).");
        }

        if (!g_overlayErr.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.47f, 0.47f, 1.0f), "%s", g_overlayErr.c_str());
        }
    }
    else
    {
        // Saved ID: draw like other rows, with a second row for ID + Delete
        if (ImGui::BeginTable("OverlayRowInline", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV))
        {
            ImGui::TableSetupColumn("Feature", ImGuiTableColumnFlags_WidthFixed, 170.0f);
            ImGui::TableSetupColumn("ON", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("OFF", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch);

            // ---------- Row 1: label + ON/OFF + Status ----------
            ImGui::TableNextRow();

            // Col 0: label
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted("Player Stats (AOE4World):");

            // Col 1: ON
            ImGui::TableSetColumnIndex(1);
            if (ImGui::Button("ON", ImVec2(80, 0))) {
                const int W = 1120, H = 340;
                if (!OV_Show(g_overlayPid, 2000)) {
                    LaunchOverlayHtml(g_overlayPid, 2000);
                }
                PositionOverlayTopRightOn(g_gameHwnd /* or nullptr */, W, H, 16);
                g_overlayEnabled = true;
            }

            // Col 2: OFF
            ImGui::TableSetColumnIndex(2);
            if (ImGui::Button("OFF", ImVec2(80, 0))) {
                OV_Hide();
                g_overlayEnabled = false;
            }

            // Col 3: Status
            ImGui::TableSetColumnIndex(3);
            ImGui::TextColored(
                g_overlayEnabled ? ImVec4(0.2f, 1.0f, 0.2f, 1.0f) : ImVec4(1.0f, 0.2f, 0.2f, 1.0f),
                g_overlayEnabled ? "Enabled" : "Disabled"
            );

            // ---------- Row 2: ID (left) + Delete (under ON) ----------
            ImGui::TableNextRow();

            // Col 0: ID (normal white)
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("ID: %s", MaskId(g_overlayPid).c_str());

            // Col 1: Delete button in the same column as ON, aligned under it
            ImGui::TableSetColumnIndex(1);
            if (ImGui::Button("Delete", ImVec2(80, 0))) {
                g_overlayPid.clear();
                if (g_overlayEnabled) { OV_Hide(); g_overlayEnabled = false; }
                DeleteOverlayPidFile();
                g_overlayErr.clear();
                memset(g_overlayInput, 0, sizeof(g_overlayInput));
            }

            // Cols 2 & 3 empty (keeps table grid aligned)
            ImGui::TableSetColumnIndex(2); ImGui::Dummy(ImVec2(0, 0));
            ImGui::TableSetColumnIndex(3); ImGui::Dummy(ImVec2(0, 0));

            ImGui::EndTable();
        }
    }

    ImGui::PopStyleVar();
    ImGui::PopID();
}


//overlay


DWORD findProcess(const wchar_t* exe) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{ sizeof(pe) };
    for (BOOL ok = Process32FirstW(s, &pe); ok; ok = Process32NextW(s, &pe))
        if (!_wcsicmp(pe.szExeFile, exe)) { CloseHandle(s); return pe.th32ProcessID; }
    CloseHandle(s); return 0;
}

uintptr_t findBase(DWORD pid, const wchar_t* mod) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    MODULEENTRY32W me{ sizeof(me) };
    for (BOOL ok = Module32FirstW(s, &me); ok; ok = Module32NextW(s, &me))
        if (!_wcsicmp(me.szModule, mod)) { CloseHandle(s); return (uintptr_t)me.modBaseAddr; }
    CloseHandle(s); return 0;
}

void loadHUDScript() {
    std::ifstream file("ResourceHUD.txt");
    if (file) {
        g_hudScript.assign((std::istreambuf_iterator<char>(file)), (std::istreambuf_iterator<char>()));
    } else {
        // Fallback message if the file is missing
        g_hudScript = "UI_DisplayMessage(\"Error: ResourceHUD.txt not found.\", 5)";
    }
}

static bool LoadTextFile(const char* path, std::string& out) {
    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f) return false;
    std::ostringstream ss; ss << f.rdbuf();
    out = ss.str();
    return true;
}//test

static void LoadScoreHUD() {
    if (!LoadTextFile("ScoreHUD.lua", g_scoreHudScript)) {
        g_scoreHudScript.clear();
    }
}//score




void runRemoteScript(const char* txt) {
    if (!g_processHandle || !g_helperAddress) {
        strcpy_s(g_statusText, "Error: Game not found or helper address is invalid.");
        return;
    }
    SIZE_T len = strlen(txt) + 1;
    void* rBuf = VirtualAllocEx(g_processHandle, nullptr, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!rBuf) { strcpy_s(g_statusText, "Error: VirtualAllocEx failed."); return; }
    
    WriteProcessMemory(g_processHandle, rBuf, txt, len, nullptr);

    HANDLE th = CreateRemoteThread(g_processHandle, nullptr, 0, (LPTHREAD_START_ROUTINE)g_helperAddress, rBuf, 0, nullptr);
    if (!th) {
        VirtualFreeEx(g_processHandle, rBuf, 0, MEM_RELEASE);
        strcpy_s(g_statusText, "Error: CreateRemoteThread failed.");
        return;
    }
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    VirtualFreeEx(g_processHandle, rBuf, 0, MEM_RELEASE);
}

uintptr_t ScanPattern(HANDLE hProcess, uintptr_t base, size_t size, const BYTE* pattern, const char* mask) {
    size_t maskLen = strlen(mask);
    if (maskLen == 0 || size < maskLen) return 0;

    std::vector<BYTE> buffer(size);
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(hProcess, (LPCVOID)base, buffer.data(), size, &bytesRead))
        return 0;

    if (bytesRead < maskLen) return 0;

    for (size_t i = 0; i <= bytesRead - maskLen; ++i) {
        bool found = true;
        for (size_t j = 0; j < maskLen; ++j) {
            if (mask[j] == 'x' && pattern[j] != buffer[i + j]) {
                found = false;
                break;
            }
            // if mask[j] == '?' (or any non-'x') it's a wildcard — skip compare
        }
        if (found) return base + i;
    }
    return 0;
}


// --- Cheat Implementations ---
void DoFindGame() {
    if (g_processHandle) {
        CloseHandle(g_processHandle);
        g_processHandle = nullptr;
    }
    g_pid = findProcess(GAME_EXE);
    if (!g_pid) {
        strcpy_s(g_statusText, "RelicCardinal.exe not found. Is the game running?");
        return;
    }

    g_processHandle = OpenProcess(PROCESS_ALL_ACCESS, FALSE, g_pid);
    if (!g_processHandle) {
        strcpy_s(g_statusText, "Error: Failed to open game process.");
        return;
    }

    uintptr_t base = findBase(g_pid, GAME_EXE);
    // Signature: 48 89 5C 24 08 57 48 81 EC ?? ?? ?? ?? 48 8B D9 BA 52 41 43 53
    BYTE pattern[] = {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x81, 0xEC,
        0x00, 0x00, 0x00, 0x00, // wildcards
        0x48, 0x8B, 0xD9, 0xBA, 0x52, 0x41, 0x43, 0x53
    };
    // mask for 9 fixed, 4 wild, 8 fixed = 21 chars
    const char* mask = "xxxxxxxxx????xxxxxxxx";

    // get module size using GetModuleInformation (robust)
    MODULEINFO mi{};
    if (!GetModuleInformation(g_processHandle, (HMODULE)base, &mi, sizeof(mi))) {
        strcpy_s(g_statusText, "Error: GetModuleInformation failed.");
        return;
    }
    uintptr_t scanStart = base;
    size_t scanSize = (size_t)mi.SizeOfImage;

    // sanity clamp (avoid absurd sizes)
    if (scanSize == 0 || scanSize > 0x80000000) {
        strcpy_s(g_statusText, "Error: weird module size.");
        return;
    }

    uintptr_t matchAddr = ScanPattern(g_processHandle, scanStart, scanSize, pattern, mask);

    if (!matchAddr) {
        strcpy_s(g_statusText, "Signature scan failed.");
        return;
    }

    uintptr_t rva = matchAddr - base;
    g_helperAddress = base + rva;

    // Find the top-level game window so macros can focus it
    g_gameHwnd = FindGameWindowByPid();

    
    char buffer[256];
    sprintf_s(buffer, "Game found! PID: %lu, Helper: 0x%llX (Offset: 0x%llX)", g_pid, g_helperAddress, rva);
    strcpy_s(g_statusText, buffer);
}

void DoRevealMap() {
    runRemoteScript("FOW_UIRevealAll_Transition(0.5); FOW_UIRevealAllEntities(); FOW_ExploreAll()");
    strcpy_s(g_statusText, "Injected: Reveal Map.");
}

// Scan-code key taps (more reliable in games)
static void TapKeyScan(WORD vk) {
    WORD sc = (WORD)MapVirtualKey(vk, MAPVK_VK_TO_VSC);
    INPUT in[2]{};
    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = 0; in[0].ki.wScan = sc; in[0].ki.dwFlags = KEYEVENTF_SCANCODE;
    in[1] = in[0];               in[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}
static void TapKeyScanNTimes(WORD vk, int n, DWORD gap_ms = 60) {
    for (int i = 0; i < n; ++i) { TapKeyScan(vk); Sleep(gap_ms); }
}


static bool IsGameForeground() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
    return (pid == g_pid && g_pid != 0);
}
static void AutoVillagerMacroTick() {
    // If game lost focus, try bringing it forward once (optional)
    if (!IsGameForeground() && g_gameHwnd) {
        SetForegroundWindow(g_gameHwnd);
        Sleep(60);
    }

    // Select TCs (your 4 key) twice for reliability
    TapKeyScan('4');  Sleep(120);
    TapKeyScan('4');  Sleep(60);

    // Train villagers (QQQQ) with friendly gaps
    TapKeyScanNTimes('Q', g_autoVillCount, 70);

    Sleep(60);
    TapKeyScan(VK_ESCAPE); // close
}


// Idle Town Center Alert - ON
void DoIdleTownCenterAlertOn() {
    const char* idleTCScript = R"(
-- Idle Town Center Alert Lua Script
local isMessageVisible = 0

local function showMsg()
    if isMessageVisible == 1 then return end
    local m = Loc_Empty(); m.LocString = "IDLE TC"
    UI_SystemMessageShow(m)
    isMessageVisible = 1
end

local function hideMsg()
    if isMessageVisible == 0 then return end
    local m = Loc_Empty(); m.LocString = "IDLE TC"
    UI_SystemMessageHide(m)
    isMessageVisible = 0
end

function CheckIdleTownCenters_Fast()
    local p  = Game_GetLocalPlayer()
    local eg = Player_GetEntities(p)
    EGroup_Filter(eg, "town_center", FILTER_KEEP)
    local cnt = EGroup_Count(eg)

    local hasIdle = false
    for i = 1, cnt do
        local tc = EGroup_GetEntityAt(eg, i)
        local q  = Entity_GetProductionQueueSize(tc)
        if q == 0 then
            hasIdle = true
            break
        end
    end

    if hasIdle then showMsg() else hideMsg() end
end

pcall(function() Rule_Remove(CheckIdleTownCenters) end)
pcall(function() Rule_Remove(CheckIdleTownCenters_Safe) end)
pcall(function() Rule_Remove(CheckIdleTownCenters_Fast) end)
hideMsg()

Rule_AddOneShot(CheckIdleTownCenters_Fast)
Rule_AddInterval(CheckIdleTownCenters_Fast, 0.2)
    )";

    runRemoteScript(idleTCScript);
    strcpy_s(g_statusText, "Injected: Idle Town Center Alert (ON).");
    g_idleTCEnabled = true;
}

// Idle Town Center Alert - OFF
void DoIdleTownCenterAlertOff() {
    const char* off = R"(
        pcall(function()
            local G = _G or {}
            local Rule_Remove_ = rawget(G, "Rule_Remove")
            if Rule_Remove_ then
                for _, ruleName in ipairs({"CheckIdleTownCenters", "CheckIdleTownCenters_Safe", "CheckIdleTownCenters_Fast"}) do
                    local rule = rawget(G, ruleName)
                    if rule then
                        pcall(Rule_Remove_, rule)
                    end
                end
            end

            local Loc_Empty_ = rawget(G, "Loc_Empty")
            local UI_Hide    = rawget(G, "UI_SystemMessageHide")
            if Loc_Empty_ and UI_Hide then
                local m = Loc_Empty_()
                if m then
                    m.LocString = "IDLE TC"
                    pcall(UI_Hide, m)
                end
            end
        end)
    )";
    runRemoteScript(off);
    strcpy_s(g_statusText, "Injected: Idle Town Center Alert (OFF).");
    g_idleTCEnabled = false;
}


void DoShowResourceHUD() {
    if (g_hudScript.empty()) {
        strcpy_s(g_statusText, "Error: ResourceHUD script is not loaded.");
        return;
    }

    // Prevent duplicate intervals if user clicks Show multiple times
    const char* pre = R"( pcall(function() Rule_Remove(UpdateResourceHUD) end) )";
    runRemoteScript(pre);

    runRemoteScript(g_hudScript.c_str());
    strcpy_s(g_statusText, "Injected: Resource HUD script.");
}

void DoHideResourceHUD() {
    const char* script = R"(
        function HidePlayerResources()
            UI_Remove("TestName")
            pcall(function() Rule_Remove(UpdateResourceHUD) end)
        end
        Rule_AddOneShot(HidePlayerResources)
    )";
    runRemoteScript(script);
    strcpy_s(g_statusText, "Injected: Hide Resource HUD.");
}

void DoAddResources(const char* resourceType, int amount) {
    char scriptBuffer[1024];   // increased buffer size
    sprintf_s(scriptBuffer,
        "function GrantRes() Player_SetResource(Game_GetLocalPlayer(), %s, %d); end Rule_AddOneShot(GrantRes)",
        resourceType, amount);
    runRemoteScript(scriptBuffer);

    char statusBuffer[128];
    sprintf_s(statusBuffer, "Injected: Set %s to %d.", resourceType + 3, amount);
    strcpy_s(g_statusText, statusBuffer);
}


void DoSetPopulationCap(int amount) {
    char scriptBuffer[256];
    sprintf_s(scriptBuffer,
        "function SetPop() Player_SetPopCapOverride(Game_GetLocalPlayer(), %d); end Rule_AddOneShot(SetPop)",
        amount);
    runRemoteScript(scriptBuffer);
}

// Instant Build
void DoInstantBuild() {
    const char* script = R"(
        function __InstantBuildOnce()
            CheatInstantBuildAndGather(Game_GetLocalPlayer(), {})
        end
        Rule_AddOneShot(__InstantBuildOnce)
    )";
    runRemoteScript(script);
    strcpy_s(g_statusText, "Injected: Instant Build activated.");
}

// God Mode ON
void DoGodModeOn() {
    const char* script = R"(
        function __InvulnOn_LocalOnce()
            local p = Game_GetLocalPlayer()

            local function _s(_, __, sid)
                Squad_SetInvulnerableMinCap(sid, Squad_GetHealthPercentage(sid, false), -1)
            end
            SGroup_ForEach(Player_GetSquads(p), _s)

            local function _e(_, __, eid)
                Entity_SetInvulnerableMinCap(eid, Entity_GetHealthPercentage(eid), -1)
            end
            EGroup_ForEach(Player_GetEntities(p), _e)
        end
        Rule_AddOneShot(__InvulnOn_LocalOnce)

        function __Invuln_OnSpawn(evt)
            local ent = evt and evt.entity
            if not ent then return end

            local p = Game_GetLocalPlayer()
            if Entity_GetPlayerOwner(ent) ~= p then return end

            Entity_SetInvulnerableMinCap(ent, Entity_GetHealthPercentage(ent), -1)

            local sq = Entity_GetSquad(ent)
            if sq then
                Squad_SetInvulnerableMinCap(sq, Squad_GetHealthPercentage(sq, false), -1)
            end
        end
        Rule_AddPlayerEvent(__Invuln_OnSpawn, Game_GetLocalPlayer(), GE_EntitySpawn)
    )";
    runRemoteScript(script);
    strcpy_s(g_statusText, "Injected: God Mode ON.");
    g_godModeEnabled = true;
}

// God Mode OFF (removes invulnerability by setting cap back to 0)
void DoGodModeOff() {
    const char* script = R"(
        function __InvulnOff()
            local p = Game_GetLocalPlayer()

            local function _s(_, __, sid)
                Squad_SetInvulnerableMinCap(sid, 0, -1)
            end
            SGroup_ForEach(Player_GetSquads(p), _s)

            local function _e(_, __, eid)
                Entity_SetInvulnerableMinCap(eid, 0, -1)
            end
            EGroup_ForEach(Player_GetEntities(p), _e)
        end
        Rule_AddOneShot(__InvulnOff)
    )";
    runRemoteScript(script);
    strcpy_s(g_statusText, "Injected: God Mode OFF.");
    g_godModeEnabled = false;
}

// Instant Win
void DoInstantWin() {
    const char* script = R"(
        function Test1()
            local localPlayer = Game_GetLocalPlayer()
            local localTeam = Player_GetTeam(localPlayer)
            local playerCount = World_GetPlayerCount()

            for i = 1, playerCount do
                local otherPlayer = World_GetPlayerAt(i)
                if otherPlayer ~= localPlayer then
                    local otherTeam = Player_GetTeam(otherPlayer)
                    if otherTeam ~= localTeam then
                        World_SetPlayerLose(otherPlayer)
                    end
                end
            end

            World_SetPlayerWin(localPlayer)
            Core_CallDelegateFunctions("PreGameOver", WR_NONE)
            World_SetGameOver(WR_NONE)
            Core_SetPostGameState()
        end
        Rule_AddOneShot(Test1)
    )";
    runRemoteScript(script);
    strcpy_s(g_statusText, "Injected: Instant Win.");
}

// Spawn Photon Man at TC (one-shot)
// Spawn Photon Man at TC (one-shot)
void DoSpawnPhotonMan() {
    const char* script = R"(
hasLoadedPhoton = hasLoadedPhoton or false
function CheatPhotonMan()
    local p = Game_GetLocalPlayer()
    if not p then return end
    local pos = GetTownCentrePosition(p)
    if not pos then return end
    local needsLoad = (hasLoadedPhoton == false)
    if SpawnUnitBPForPlayer then
        SpawnUnitBPForPlayer("unit_photon_man", p, pos, needsLoad)
        hasLoadedPhoton = true
        if UI_DisplayMessage then UI_DisplayMessage("Photon Man spawned!", 3) end
    end
end
Rule_AddOneShot(CheatPhotonMan)
    )";
    runRemoteScript(script);
    strcpy_s(g_statusText, "Injected: Photon Man (one-shot).");
}

// --- Age Up (one-shot) ---
void DoAgeUp() {
    const char* script = R"(
function CustomScript()
    local p = Game_GetLocalPlayer()
    if not p then return end
    ChatCheatAgeUpMultiplayer(p)
end
Rule_AddOneShot(CustomScript)
    )";
    runRemoteScript(script);
    strcpy_s(g_statusText, "Injected: Age Up (one-shot).");
}

// --- Spawn Cheat Army (one-shot) ---
void DoSpawnCheatArmyQueued(int batches, float dt = 0.15f) {
    if (batches < 1)  batches = 1;
    if (batches > 200) batches = 200;

    char script[1200];
    std::snprintf(script, sizeof(script),
        R"(
local p = Game_GetLocalPlayer(); if not p then return end
g_army_q  = (g_army_q or 0) + %d
g_army_dt = %f

if not ArmyTick then
  function ArmyTick()
    if (g_army_q or 0) <= 0 then Rule_Remove(ArmyTick); ArmyTick = nil; return end
    g_army_q = g_army_q - 1

    if SpawnCheatArmy then
      pcall(SpawnCheatArmy, p)                               -- your 5–7 unit batch
    else
      local pos = GetTownCentrePosition and GetTownCentrePosition(p) or {X=0,Y=0,Z=0}
      if SpawnUnitBPForPlayer then pcall(SpawnUnitBPForPlayer, "unit_photon_man", p, pos, false) end
    end
  end
  Rule_AddInterval(ArmyTick, g_army_dt)
end
)", batches, dt);

    runRemoteScript(script);
    std::snprintf(g_statusText, sizeof(g_statusText), "Queued %d army batches.", batches);
}






// --- Jeanne d'Arc: Add EXP (one-shot) ---
void DoJeanneExpAdd(int amount) {
    if (amount < 0) amount = 0;
    char buf[512];
    sprintf_s(buf,
        R"(function __jeanne_exp_add()
    local p = Game_GetLocalPlayer(); if not p then return end
    local cur = Player_GetStateModelFloat(p, "jeanne_d_arc_total_experience") or 0
    Player_SetStateModelFloat(p, "jeanne_d_arc_total_experience", cur + %d)
end
Rule_AddOneShot(__jeanne_exp_add))",
amount);
    runRemoteScript(buf);
    snprintf(g_statusText, sizeof(g_statusText), "Injected: Jeanne d'Arc +%d EXP", amount);
}



// Score HUD
void DoShowScoreHUD() {
    if (g_scoreHudScript.empty()) {
        strcpy_s(g_statusText, "Error: ScoreHUD.lua not loaded (check path).");
        return;
    }

    // Clean old updater/UI if it exists so we don’t duplicate
    runRemoteScript(R"( pcall(function() Rule_Remove(UpdateScoreHUD) end) )");
    runRemoteScript(R"( pcall(function() UI_Remove("ScoreHUD") end) )");

    // Inject ScoreHUD.lua text (defines ShowScoreHUD/UpdateScoreHUD/HideScoreHUD)
    runRemoteScript(g_scoreHudScript.c_str());

    // Call it
    runRemoteScript(R"( pcall(function() ShowScoreHUD() end) )");

    strcpy_s(g_statusText, "Injected & called: Score HUD.");
}

// Hide Score HUD (and nuke old inline overlay if it ever existed)
static void DoHideScoreHUD() {
    runRemoteScript(R"(
        pcall(function()
            if Rule_Remove and UpdateScoreHUD then Rule_Remove(UpdateScoreHUD) end
            if UI_Remove then UI_Remove("ScoreHUD") end
            if HideScoreHUD then HideScoreHUD() end

            -- also clean any old inline overlay variant
            if Rule_Remove and __InlineScores_Tick then Rule_Remove(__InlineScores_Tick) end
            if UI_Remove then UI_Remove("InlineScores") end
        end)
    )");
    strcpy_s(g_statusText, "Score HUD hidden.");
}

// --- Fog of War Toggle (one-shot) ---
void DoFoWToggleOneShot() {
    const char* script = R"(
function CheatFoW()
    local p = Game_GetLocalPlayer()
    if gFoWCheat == nil then gFoWCheat = {} end
    if gFoWCheat[p] == nil then gFoWCheat[p] = false end

    if gFoWCheat[p] == false then
        FOW_PlayerRevealAll(p)
        gFoWCheat[p] = true
    else
        FOW_PlayerUnRevealAll(p)
        FOW_PlayerUnExploreAll(p)
        gFoWCheat[p] = false
    end
end

function CustomScript()
    CheatFoW()
end

Rule_AddOneShot(CustomScript)
    )";
    runRemoteScript(script);
    strcpy_s(g_statusText, "Injected: FoW toggle (one-shot).");
}

void DoSpawnSheepAtTC(int count) {
    if (count < 1)  count = 1;
    if (count > 200) count = 200;

    // minimal, known-good one-shot Lua (spawns ONE sheep at TC)
    static const char* kOneSheepLua = R"(
function SpawnSheep()
    local sbp = BP_GetSquadBlueprint("gaia_herdable_sheep")
    local p   = Game_GetLocalPlayer()
    if not sbp or not p then return end
    local pos = GetTownCentrePosition and GetTownCentrePosition(p)
    if not pos then return end
    Squad_CreateAndSpawnToward(sbp, p, 0, pos, pos)
end
Rule_AddOneShot(SpawnSheep)
)";

    // run N times on a background thread so the UI doesn't block
    std::thread([=]() {
        for (int i = 0; i < count; ++i) {
            runRemoteScript(kOneSheepLua);
            // small gap so the game script VM + spawn system can breathe
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
        }
        std::snprintf(g_statusText, sizeof(g_statusText),
            "Spawned %d sheep at Town Center.", count);
        }).detach();

    std::snprintf(g_statusText, sizeof(g_statusText),
        "Queuing %d sheep @ TC...", count);
}





static BOOL CALLBACK EnumFindWindowProc(HWND hwnd, LPARAM lParam) {
    DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
    if (pid == g_pid && GetWindow(hwnd, GW_OWNER) == nullptr && IsWindowVisible(hwnd)) {
        *reinterpret_cast<HWND*>(lParam) = hwnd;
        return FALSE; // stop
    }
    return TRUE; // continue
}
static HWND FindGameWindowByPid() {
    HWND out = nullptr;
    EnumWindows(EnumFindWindowProc, reinterpret_cast<LPARAM>(&out));
    return out;
}



int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow) {
    // Load external scripts
    loadHUDScript();
    LoadScoreHUD();
    
    // Create application window
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"AoE4 Injector", nullptr };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"Age of Empires IV Injector", WS_OVERLAPPEDWINDOW, 100, 100, 500, 350, nullptr, nullptr, wc.hInstance, nullptr);

    // Initialize Direct3D
    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    // Show the window
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();

    // Setup Platform/Renderer backends
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // Register Hotkeys
    RegisterHotKey(hwnd, 1, MOD_NOREPEAT, VK_F1); // Find Game
    RegisterHotKey(hwnd, 2, MOD_NOREPEAT, VK_F2); // Reveal Map

    // Main loop
    bool done = false;
    while (!done) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        // Start the Dear ImGui frame
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // --- UI Drawing ---
        {
            ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::Begin("Main", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);

            // Top: Status + Find Game
            ImGui::Text("Status: %s", g_statusText);
            if (ImGui::Button("Find Game (F1)", ImVec2(-1, 0))) {
                DoFindGame();
            }
            ImGui::Separator();

            // helper: ToggleRow (re-usable)
            auto ToggleRow = [&](const char* label, const char* id,
                bool enabled,
                auto onFn, auto offFn)
                {
                    ImGui::TableNextRow();

                    // Label
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(label);

                    // ON
                    ImGui::TableSetColumnIndex(1);
                    ImGui::PushID(id);
                    if (ImGui::Button("ON", ImVec2(80, 0))) onFn();

                    // OFF
                    ImGui::TableSetColumnIndex(2);
                    if (ImGui::Button("OFF", ImVec2(80, 0))) offFn();
                    ImGui::PopID();

                    // Status (green/red)
                    ImGui::TableSetColumnIndex(3);
                    ImVec4 col = enabled ? ImVec4(0.2f, 1.0f, 0.2f, 1.0f)
                        : ImVec4(1.0f, 0.2f, 0.2f, 1.0f);
                    ImGui::TextColored(col, enabled ? "Enabled" : "Disabled");
                };


            // ===== Overlay panel goes here =====
          // --- ONLINE CHEATS (green, collapsible, overlay at bottom) ---
            {
                // Green header styling
                ImVec4 txt = ImVec4(0.20f, 1.00f, 0.20f, 1.0f);
                ImVec4 hdr = ImVec4(0.08f, 0.20f, 0.08f, 0.85f);
                ImVec4 hov = ImVec4(0.12f, 0.30f, 0.12f, 0.95f);
                ImVec4 act = ImVec4(0.14f, 0.38f, 0.14f, 1.0f);

                ImGui::PushStyleColor(ImGuiCol_Text, txt);
                ImGui::PushStyleColor(ImGuiCol_Header, hdr);
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, hov);
                ImGui::PushStyleColor(ImGuiCol_HeaderActive, act);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 6));

                ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen
                    | ImGuiTreeNodeFlags_Framed
                    | ImGuiTreeNodeFlags_SpanAvailWidth;

                bool onlineOpen = ImGui::CollapsingHeader("ONLINE CHEATS  (works in Quick Match or Ranked)", flags);

                // restore styles immediately
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(4);

                if (onlineOpen)
                {
                    // Online cheats table
                    if (ImGui::BeginTable("OnlineCheats", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
                        ImGui::TableSetupColumn("Feature", ImGuiTableColumnFlags_WidthFixed, 170.0f);
                        ImGui::TableSetupColumn("ON", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                        ImGui::TableSetupColumn("OFF", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch);

                        // --- Reveal Map (one-shot) ---
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted("Reveal Map: (F2)");

                        ImGui::TableSetColumnIndex(1);
                        ImGui::PushID("RevealMap");
                        if (ImGui::Button("Activate", ImVec2(80, 0))) {
                            DoRevealMap();
                        }
                        ImGui::PopID();

                        // Column 2: help marker
                        ImGui::TableSetColumnIndex(2);
                        HelpHint("Reveals the entity and resources and expands vision");

                        // Column 3: type label
                        ImGui::TableSetColumnIndex(3);
                        ImGui::TextDisabled("One-shot");


                        // Idle TC
                        ToggleRow("Idle TC Alert:", "IdleTC", g_idleTCEnabled,
                            [] { DoIdleTownCenterAlertOn(); }, [] { DoIdleTownCenterAlertOff(); });

                        // Auto Vill Macro
                        ToggleRow("Auto Vill Macro:", "AutoVillMacro", g_autoVillMacroEnabled,
                            [] {
                                g_autoVillMacroEnabled = true;
                                AutoVillagerMacroTick();                 // fire now
                                g_autoVillLastTickMs = GetTickCount64(); // schedule next
                                strcpy_s(g_statusText, "Auto Vill Macro: ON (fired)");
                            },
                            [] {
                                g_autoVillMacroEnabled = false;
                                strcpy_s(g_statusText, "Auto Vill Macro: OFF");
                            });

                        // Resource HUD
                        ToggleRow("Resource HUD:", "ResourceHUD", g_resourceHUDEnabled,
                            [] { DoShowResourceHUD(); g_resourceHUDEnabled = true; },
                            [] { DoHideResourceHUD(); g_resourceHUDEnabled = false; });

                        // Make sure you have: static bool g_scoreHUDEnabled = false;

                        ToggleRow("Score HUD:", "ScoreHUD", g_scoreHUDEnabled,
                            [] { DoShowScoreHUD(); g_scoreHUDEnabled = true;  },
                            [] { DoHideScoreHUD(); g_scoreHUDEnabled = false; }
                        );



                        ImGui::EndTable();
                    }

                    // Draw AoE4 overlay row at the bottom of ONLINE CHEATS
                    DrawAoE4OverlayPanel();
                }
            }

            // ===== OFFLINE CHEATS (orange, collapsible — no Child windows) =====
            {
                // Orange header styling (affects header only)
                ImVec4 colText = ImVec4(0.95f, 0.70f, 0.20f, 1.0f);
                ImVec4 colHdr = ImVec4(0.28f, 0.20f, 0.08f, 0.75f);
                ImVec4 colHov = ImVec4(0.36f, 0.26f, 0.10f, 0.90f);
                ImVec4 colAct = ImVec4(0.44f, 0.31f, 0.12f, 0.95f);

                ImGui::PushStyleColor(ImGuiCol_Text, colText);
                ImGui::PushStyleColor(ImGuiCol_Header, colHdr);
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, colHov);
                ImGui::PushStyleColor(ImGuiCol_HeaderActive, colAct);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 6));

                ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen
                    | ImGuiTreeNodeFlags_Framed
                    | ImGuiTreeNodeFlags_SpanAvailWidth;

                bool offlineOpen = ImGui::CollapsingHeader("OFFLINE CHEATS (Strictly Campaign or Skirmish)", flags);

                // Restore styles so only header is tinted
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(4);

                if (offlineOpen)
                {
                    // ----- Table: God/Instant Build/Instant Win -----
                    if (ImGui::BeginTable("OfflineCheats", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
                        ImGui::TableSetupColumn("Feature", ImGuiTableColumnFlags_WidthFixed, 170.0f);
                        ImGui::TableSetupColumn("ON", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                        ImGui::TableSetupColumn("OFF", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch);

                        ToggleRow("God Mode:", "GodMode", g_godModeEnabled,
                            [] { DoGodModeOn(); },
                            [] { DoGodModeOff(); });

                        // --- Instant Build (one-shot) ---
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted("Instant Build:");

                        ImGui::TableSetColumnIndex(1);
                        ImGui::PushID("InstantBuild");
                        if (ImGui::Button("Activate", ImVec2(80, 0))) { DoInstantBuild(); }
                        ImGui::PopID();

                        // Column 2: help marker
                        ImGui::TableSetColumnIndex(2);
                        HelpHint("All units, buildings, and research complete instantly.");

                        // Column 3: type label
                        ImGui::TableSetColumnIndex(3);
                        ImGui::TextDisabled("One-shot");


                        // --- Instant Win (AI only) ---
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted("Instant Win (AI only):");

                        ImGui::TableSetColumnIndex(1);
                        ImGui::PushID("InstantWin");
                        if (ImGui::Button("Activate", ImVec2(80, 0))) { DoInstantWin(); }
                        ImGui::PopID();

                        // Column 2: help marker
                        ImGui::TableSetColumnIndex(2);
                        HelpHint("Triggers defeat for AI opponents only. Useful for finishing mastery quickly.");

                        // Column 3: type label
                        ImGui::TableSetColumnIndex(3);
                        ImGui::TextDisabled("One-shot");


                        // --- Age Up (one-shot) ---
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted("Age Up:");

                        ImGui::TableSetColumnIndex(1);
                        ImGui::PushID("AgeUp");
                        if (ImGui::Button("Activate", ImVec2(80, 0))) { DoAgeUp(); }
                        ImGui::PopID();

                        // Column 2: help marker
                        ImGui::TableSetColumnIndex(2);
                        HelpHint("Advances your civilization to the next Age (no effect if already at max).");

                        // Column 3: type label
                        ImGui::TableSetColumnIndex(3);
                        ImGui::TextDisabled("One-shot");

                        // --- Remove Fog (one-shot) ---
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted("Remove Fog:");

                        ImGui::TableSetColumnIndex(1);
                        ImGui::PushID("FoWTgl");
                        if (ImGui::Button("Activate", ImVec2(80, 0))) { DoFoWToggleOneShot(); }
                        ImGui::PopID();

                        // Column 2: help marker
                        ImGui::TableSetColumnIndex(2);
                        HelpHint("Toggles fog of war. Reveals everything(⚠ Will desync online.)");

                        // Column 3: type label
                        ImGui::TableSetColumnIndex(3);
                        ImGui::TextDisabled("One-shot");



                        ImGui::EndTable();
                    }

                    ImGui::Spacing();
                    ImGui::Separator();

                    // ----- Resources -----
#ifdef IMGUI_HAS_SEPARATOR_TEXT
                    ImGui::SeparatorText("Resources");
#else
                    ImGui::TextUnformatted("Resources");
                    ImGui::Separator();
#endif

                    // Compact resources UI: label | amount | (+/- in one column) | Add(80) | filler
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 3));
                    if (ImGui::BeginTable("ResCompactTbl", 5,
                        ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_BordersInnerV))
                    {
                        ImGui::TableSetupColumn("Res", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                        ImGui::TableSetupColumn("Amt", ImGuiTableColumnFlags_WidthFixed, 130.0f); // bigger input box
                        ImGui::TableSetupColumn("+/-", ImGuiTableColumnFlags_WidthFixed, 60.0f); // both buttons in one cell
                        ImGui::TableSetupColumn("Add", ImGuiTableColumnFlags_WidthFixed, 80.0f); // same as ON/OFF width
                        ImGui::TableSetupColumn("Fill", ImGuiTableColumnFlags_WidthStretch);

                        const int step = 1000; // per click

                        auto Row = [&](const char* label, const char* id, int& value, const char* rtTag)
                            {
                                ImGui::TableNextRow();

                                // Col 0: label
                                ImGui::TableSetColumnIndex(0);
                                ImGui::TextUnformatted(label);

                                // Col 1: amount (disable built-in steppers with 0,0)
                                ImGui::TableSetColumnIndex(1);
                                ImGui::PushID(id);
                                ImGui::SetNextItemWidth(-1);
                                ImGui::InputInt("##amt", &value, 0, 0);

                                // Col 2: +/- in one cell
                                ImGui::TableSetColumnIndex(2);
                                if (ImGui::SmallButton("-")) value = (value > step ? value - step : 0);
                                ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
                                if (ImGui::SmallButton("+")) value += step;

                                // Col 3: Add (80 width to match ON/OFF)
                                ImGui::TableSetColumnIndex(3);
                                if (ImGui::Button("Add", ImVec2(80, 0))) DoAddResources(rtTag, value);

                                // Col 4: filler
                                ImGui::TableSetColumnIndex(4);
                                ImGui::Dummy(ImVec2(0, 0));

                                ImGui::PopID();
                            };

                        Row("Food", "food", g_foodAmount, "RT_Food");
                        Row("Wood", "wood", g_woodAmount, "RT_Wood");
                        Row("Gold", "gold", g_goldAmount, "RT_Gold");
                        Row("Stone", "stone", g_stoneAmount, "RT_Stone");
                        Row("Olive Oil", "oil", g_oilAmount, "RT_Merc_Byz");

                        ImGui::EndTable();
                    }
                    ImGui::PopStyleVar();

                    // Full-width Add All
                    if (ImGui::Button("Add All", ImVec2(-1, 0))) {
                        DoAddResources("RT_Food", g_foodAmount);
                        DoAddResources("RT_Wood", g_woodAmount);
                        DoAddResources("RT_Gold", g_goldAmount);
                        DoAddResources("RT_Stone", g_stoneAmount);
                        DoAddResources("RT_Merc_Byz", g_oilAmount); // safe: ignored if not Byzantine
                    }

                    ImGui::Spacing();
                    ImGui::Separator();


                    // ----- Population -----
                    // Persisted controls
                    static int s_popCapAmt = g_popCapAmount; // keep in sync with your global, start from it
                    static int s_photonCount = 10;
                    static int s_jeanneExp = 3000;
                    static int s_armyBatches = 10;             // 1 batch ≈ 5–7 units

                    if (BeginCompactActionTable("CheatCompactTbl"))
                    {
                        // Pop Cap
                        DrawStepperActionRow("Pop Cap", "popcap", s_popCapAmt,
                            /*step*/ 10, /*min*/ 1, /*max*/ 10000,
                            "Set",
                            [&](int v) {
                                DoSetPopulationCap(v);
                                g_popCapAmount = v; // optional: keep global aligned
                                std::snprintf(g_statusText, sizeof(g_statusText),
                                    "Injected: Set Pop Cap to %d.", v);
                            }
                        );

                        // Photon Man
                        DrawStepperActionRow("Photon Man", "photonAmt", s_photonCount,
                            /*step*/ 10, /*min*/ 1, /*max*/ 100000,
                            "Spawn",
                            [&](int v) {
                                for (int i = 0; i < v; ++i) DoSpawnPhotonMan();
                                std::snprintf(g_statusText, sizeof(g_statusText),
                                    "Spawned %d Photon Men.", v);
                            }
                        );

                        // Jeanne EXP
                        DrawStepperActionRow("Jeanne EXP", "jeanneExp", s_jeanneExp,
                            /*step*/ 100, /*min*/ 0, /*max*/ 1000000,
                            "Set EXP",
                            [&](int v) {
                                DoJeanneExpAdd(v);
                                std::snprintf(g_statusText, sizeof(g_statusText),
                                    "Injected: Jeanne EXP %d.", v);
                            }
                        );

                        // Army Batches (queued spawner to avoid crashes)
                        DrawStepperActionRow("Army: ", "armyBatches", s_armyBatches,
                            /*step*/ 10, /*min*/ 1, /*max*/ 200,   // 200×(5–7) ≈ 1000+ units total
                            "Spawn",
                            [&](int v) {
                                DoSpawnCheatArmyQueued(v, 0.15f);  // 1 batch every 0.15s
                                std::snprintf(g_statusText, sizeof(g_statusText),
                                    "Queued %d army batches.", v);
                            }
                        );

                        DrawStepperActionRow("Sheep: ", "sheepCount", g_sheepCount,
                            /*step*/ 10, /*min*/ 1, /*max*/ 200,
                            "Spawn",
                            [&](int v) {
                                DoSpawnSheepAtTC(v);
                                std::snprintf(g_statusText, sizeof(g_statusText),
                                    "Spawned %d sheep at Town Center.", v);
                            }
                        );



                        EndCompactActionTable();
                    }

                    ImGui::TextDisabled("Hint: Dont spam the button gently press once to prevent crashes.");




                }

            } // end OFFLINE CHEATS
            // (Custom Script Injector stays AFTER this block, unchanged)


            ImGui::Separator();

            // --- Custom Script Injector (yellow letters + blue header) ---
            {
                // Yellow text, blue-themed header
                ImVec4 colText = ImVec4(1.0f, 1.0f, 0.2f, 1.0f);    // Bright yellow letters
                ImVec4 colHdr = ImVec4(0.10f, 0.15f, 0.35f, 0.85f); // Dark navy header
                ImVec4 colHov = ImVec4(0.15f, 0.25f, 0.55f, 0.95f); // Hover (lighter blue)
                ImVec4 colAct = ImVec4(0.20f, 0.30f, 0.65f, 1.0f);  // Active (bright blue)

                ImGui::PushStyleColor(ImGuiCol_Text, colText);
                ImGui::PushStyleColor(ImGuiCol_Header, colHdr);
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, colHov);
                ImGui::PushStyleColor(ImGuiCol_HeaderActive, colAct);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 6));

                ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen
                    | ImGuiTreeNodeFlags_Framed
                    | ImGuiTreeNodeFlags_SpanAvailWidth;

                bool open = ImGui::CollapsingHeader("CUSTOM SCRIPT INJECTOR", flags);

                ImGui::PopStyleVar();
                ImGui::PopStyleColor(4);

                if (open)
                {
                    ImGui::BeginChild("CustomScriptChild",
                        ImVec2(0, ImGui::GetTextLineHeight() * 12 + ImGui::GetStyle().FramePadding.y * 2),
                        false);

                    ImGui::Text("Paste or edit Lua script below:");

                    ImGui::InputTextMultiline(
                        "##CustomScript",
                        g_customScriptBuffer,
                        IM_ARRAYSIZE(g_customScriptBuffer),
                        ImVec2(-1, ImGui::GetTextLineHeight() * 8),
                        ImGuiInputTextFlags_AllowTabInput
                    );

                    ImGui::Spacing();
                    if (ImGui::Button("Execute Custom Script", ImVec2(-1, 0))) {
                        runRemoteScript(g_customScriptBuffer);
                        strcpy_s(g_statusText, "Injected: Custom script.");
                    }

                    ImGui::EndChild();
                }
            }


            ImGui::End();
        }


        ULONGLONG now = GetTickCount64();
        if (g_autoVillMacroEnabled) {
            if (g_autoVillLastTickMs == 0) g_autoVillLastTickMs = now;
            if (now - g_autoVillLastTickMs >= (ULONGLONG)g_autoVillEverySec * 1000ULL) {
                AutoVillagerMacroTick();
                g_autoVillLastTickMs = now;
            }
        }


        // Rendering
        const float clear_color_with_alpha[4] = { 0.45f, 0.55f, 0.60f, 1.00f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0); // Present with vsync
    }

    // Cleanup
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}

// --- Win32/D3D11 Backend Functions ---
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
            return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
case WM_HOTKEY:
    switch (wParam) {
    case 1: // F1 - Find Game
        DoFindGame();
        break;
    case 2: // F2 - Reveal Map
        DoRevealMap();
        break;
    default:
        break;
    }
    return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK)
        return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}