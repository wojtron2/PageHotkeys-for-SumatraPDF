#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <ddeml.h>
#include <shellapi.h>

#include <string.h>
#include <wchar.h>

#include "resource.h"

namespace {

constexpr wchar_t kAppClassName[] = L"PageHotkeysWindow";
constexpr wchar_t kInfoClassName[] = L"PageHotkeysInfoWindow";
constexpr wchar_t kAppTitle[] = L"PageHotkeys";
constexpr wchar_t kConfigFileName[] = L"PageHotkeys.ini";
constexpr wchar_t kLegacyConfigFileName[] = L"PdfController.ini";
constexpr wchar_t kSettingsSection[] = L"PageHotkeys";
constexpr wchar_t kLegacySettingsSection[] = L"PdfController";

constexpr int kActionNextPage = 1;
constexpr int kActionPrevPage = 2;
constexpr int kActionScrollDown = 3;
constexpr int kActionScrollUp = 4;
constexpr int kActionQuit = 99;

constexpr int kIdSumatraEdit = 100;
constexpr int kIdBrowse = 101;
constexpr int kIdFindPath = 102;
constexpr int kIdSaveApply = 103;
constexpr int kIdDefaults = 104;
constexpr int kIdTray = 105;
constexpr int kIdInfo = 106;
constexpr int kIdExit = 107;
constexpr int kIdStatus = 108;
constexpr int kIdConsumeKey = 109;
constexpr int kIdInfoCredit = 110;

constexpr int kIdHotkeyBase = 200;
constexpr int kIdEnableBase = 300;
constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kTrayIconId = 1;

constexpr size_t kMaxPathChars = 4096;
constexpr size_t kMaxEnvironmentChars = 32767;
constexpr size_t kMaxStatusChars = 512;
constexpr DWORD kDdeTimeoutMs = 5000;

struct Config {
    wchar_t sumatraPath[kMaxPathChars];
    bool usePathSearch;
};

struct HotkeyAction {
    int id;
    int controlId;
    int enableControlId;
    const wchar_t* label;
    const wchar_t* iniKey;
    const wchar_t* enabledIniKey;
    const wchar_t* ddeCommand;
    bool quits;
    WORD defaultHotkey;
    WORD hotkey;
    bool defaultEnabled;
    bool enabled;
    HWND control;
    HWND enableControl;
    bool registered;
    bool rawPressed;
};

struct RegistrationCounts {
    int commandConfigured;
    int commandRegistered;
    int failed;
    bool rawInputMode;
    bool rawFallback;
};

HINSTANCE g_instance = nullptr;
HWND g_mainWindow = nullptr;
HWND g_sumatraEdit = nullptr;
HWND g_status = nullptr;
HWND g_consumeKeyCheckbox = nullptr;
HFONT g_font = nullptr;
HFONT g_infoTitleFont = nullptr;
HFONT g_infoBodyFont = nullptr;
HFONT g_infoCreditFont = nullptr;
HBRUSH g_infoBackgroundBrush = nullptr;
Config g_config = {};
wchar_t g_configPath[kMaxPathChars] = {};
bool g_trayIconVisible = false;
bool g_consumeKey = false;
bool g_updatingUi = false;
bool g_rawInputRegistered = false;
bool g_rawCtrlDown = false;
bool g_rawAltDown = false;
bool g_rawShiftDown = false;

template <typename T, size_t N>
constexpr size_t CountOf(T (&)[N]) {
    return N;
}

constexpr WORD MakeHotkey(WORD vk, WORD flags) {
    return static_cast<WORD>((vk & 0x00ff) | ((flags & 0x00ff) << 8));
}

HotkeyAction g_actions[] = {
    {kActionScrollUp, kIdHotkeyBase + 3, kIdEnableBase + 3,
     L"Scroll up", L"ScrollUpHotkey", L"ScrollUpEnabled",
     L"CmdScrollUpPage", false,
     MakeHotkey(VK_SUBTRACT, 0), 0,
     true, false, nullptr, nullptr, false, false},
    {kActionScrollDown, kIdHotkeyBase + 2, kIdEnableBase + 2,
     L"Scroll down", L"ScrollDownHotkey", L"ScrollDownEnabled",
     L"CmdScrollDownPage", false,
     MakeHotkey(VK_ADD, 0), 0,
     true, false, nullptr, nullptr, false, false},
    {kActionPrevPage, kIdHotkeyBase + 1, kIdEnableBase + 1,
     L"Previous page", L"PrevPageHotkey", L"PrevPageEnabled",
     L"CmdGoToPrevPage", false,
     MakeHotkey(VK_PRIOR, HOTKEYF_CONTROL | HOTKEYF_ALT), 0,
     false, false, nullptr, nullptr, false, false},
    {kActionNextPage, kIdHotkeyBase + 0, kIdEnableBase + 0,
     L"Next page", L"NextPageHotkey", L"NextPageEnabled",
     L"CmdGoToNextPage", false,
     MakeHotkey(VK_NEXT, HOTKEYF_CONTROL | HOTKEYF_ALT), 0,
     false, false, nullptr, nullptr, false, false},
    {kActionQuit, kIdHotkeyBase + 4, kIdEnableBase + 4,
     L"Quit", L"QuitHotkey", L"QuitEnabled",
     nullptr, true,
     MakeHotkey(VK_END, HOTKEYF_CONTROL | HOTKEYF_ALT), 0,
     false, false, nullptr, nullptr, false, false},
};

bool CopyString(wchar_t* destination, size_t capacity, const wchar_t* source) {
    if (capacity == 0) {
        return false;
    }

    size_t i = 0;
    while (source[i] != L'\0') {
        if (i + 1 >= capacity) {
            destination[0] = L'\0';
            return false;
        }

        destination[i] = source[i];
        ++i;
    }

    destination[i] = L'\0';
    return true;
}

bool AppendChar(wchar_t* destination, size_t capacity, size_t* length, wchar_t ch) {
    if (*length + 1 >= capacity) {
        return false;
    }

    destination[*length] = ch;
    ++(*length);
    destination[*length] = L'\0';
    return true;
}

bool AppendString(wchar_t* destination,
                  size_t capacity,
                  size_t* length,
                  const wchar_t* source) {
    for (size_t i = 0; source[i] != L'\0'; ++i) {
        if (!AppendChar(destination, capacity, length, source[i])) {
            return false;
        }
    }

    return true;
}

bool AppendAnsiChar(char* destination, size_t capacity, size_t* length, char ch) {
    if (*length + 1 >= capacity) {
        return false;
    }

    destination[*length] = ch;
    ++(*length);
    destination[*length] = '\0';
    return true;
}

bool AppendWideAsAnsi(char* destination,
                      size_t capacity,
                      size_t* length,
                      const wchar_t* source) {
    for (size_t i = 0; source[i] != L'\0'; ++i) {
        if (source[i] < 1 || source[i] > 0x7f) {
            return false;
        }
        if (!AppendAnsiChar(destination, capacity, length, static_cast<char>(source[i]))) {
            return false;
        }
    }

    return true;
}

void TrimInPlace(wchar_t* text) {
    size_t start = 0;
    while (text[start] == L' ' || text[start] == L'\t' ||
           text[start] == L'\r' || text[start] == L'\n') {
        ++start;
    }

    if (start > 0) {
        size_t i = 0;
        while (text[start + i] != L'\0') {
            text[i] = text[start + i];
            ++i;
        }
        text[i] = L'\0';
    }

    size_t end = wcslen(text);
    while (end > 0 &&
           (text[end - 1] == L' ' || text[end - 1] == L'\t' ||
            text[end - 1] == L'\r' || text[end - 1] == L'\n')) {
        text[end - 1] = L'\0';
        --end;
    }

    end = wcslen(text);
    if (end >= 2 && text[0] == L'"' && text[end - 1] == L'"') {
        for (size_t i = 1; i < end - 1; ++i) {
            text[i - 1] = text[i];
        }
        text[end - 2] = L'\0';
    }
}

void FormatLastError(DWORD error, wchar_t* destination, size_t capacity) {
    if (capacity == 0) {
        return;
    }

    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        destination,
        static_cast<DWORD>(capacity),
        nullptr);

    if (length == 0) {
        CopyString(destination, capacity, L"unknown error");
        return;
    }

    size_t end = wcslen(destination);
    while (end > 0 && (destination[end - 1] == L'\r' || destination[end - 1] == L'\n')) {
        destination[end - 1] = L'\0';
        --end;
    }
}

void SetStatus(const wchar_t* text) {
    if (g_status != nullptr) {
        SetWindowTextW(g_status, text);
    }
}

void SetStatusWithErrorPrefix(const wchar_t* prefix, DWORD error) {
    wchar_t errorText[256] = {};
    wchar_t status[kMaxStatusChars] = {};
    FormatLastError(error, errorText, CountOf(errorText));
    wsprintfW(status, L"%ls Error %lu: %ls", prefix, static_cast<unsigned long>(error), errorText);
    SetStatus(status);
}

bool FileExists(const wchar_t* path) {
    const DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

HMENU ControlId(int id) {
    return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
}

void SetControlFont(HWND control) {
    if (control != nullptr && g_font != nullptr) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    }
}

HWND CreateChild(const wchar_t* className,
                 const wchar_t* text,
                 DWORD style,
                 DWORD exStyle,
                 int x,
                 int y,
                 int width,
                 int height,
                 HWND parent,
                 int id) {
    HWND control = CreateWindowExW(
        exStyle,
        className,
        text,
        style | WS_CHILD | WS_VISIBLE,
        x,
        y,
        width,
        height,
        parent,
        ControlId(id),
        g_instance,
        nullptr);
    SetControlFont(control);
    return control;
}

bool BuildConfigPath() {
    wchar_t modulePath[kMaxPathChars] = {};
    const DWORD length = GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(CountOf(modulePath)));
    if (length == 0 || length >= CountOf(modulePath)) {
        return false;
    }

    size_t slash = length;
    while (slash > 0 && modulePath[slash - 1] != L'\\' && modulePath[slash - 1] != L'/') {
        --slash;
    }

    modulePath[slash] = L'\0';
    if (!CopyString(g_configPath, CountOf(g_configPath), modulePath)) {
        return false;
    }

    size_t current = wcslen(g_configPath);
    if (!AppendString(g_configPath, CountOf(g_configPath), &current, kConfigFileName)) {
        return false;
    }

    wchar_t legacyConfigPath[kMaxPathChars] = {};
    if (!CopyString(legacyConfigPath, CountOf(legacyConfigPath), modulePath)) {
        return false;
    }

    size_t legacyCurrent = wcslen(legacyConfigPath);
    if (!AppendString(
            legacyConfigPath,
            CountOf(legacyConfigPath),
            &legacyCurrent,
            kLegacyConfigFileName)) {
        return false;
    }

    if (!FileExists(g_configPath) && FileExists(legacyConfigPath)) {
        CopyFileW(legacyConfigPath, g_configPath, TRUE);
    }

    return true;
}

bool FindDefaultSumatraPath(wchar_t* destination, size_t capacity) {
    const wchar_t* commonPaths[] = {
        L"C:\\Program Files\\SumatraPDF\\SumatraPDF.exe",
        L"C:\\Program Files (x86)\\SumatraPDF\\SumatraPDF.exe",
    };

    for (size_t i = 0; i < CountOf(commonPaths); ++i) {
        if (FileExists(commonPaths[i])) {
            return CopyString(destination, capacity, commonPaths[i]);
        }
    }

    return false;
}

bool FindLocalAppDataSumatraPath(wchar_t* destination, size_t capacity) {
    wchar_t localAppData[kMaxPathChars] = {};
    const DWORD localAppDataLength = GetEnvironmentVariableW(
        L"LOCALAPPDATA",
        localAppData,
        static_cast<DWORD>(CountOf(localAppData)));

    if (localAppDataLength == 0 || localAppDataLength >= CountOf(localAppData)) {
        return false;
    }

    wchar_t candidate[kMaxPathChars] = {};
    if (!CopyString(candidate, CountOf(candidate), localAppData)) {
        return false;
    }

    size_t length = wcslen(candidate);
    if (!AppendString(candidate, CountOf(candidate), &length, L"\\SumatraPDF\\SumatraPDF.exe")) {
        return false;
    }

    if (!FileExists(candidate)) {
        return false;
    }

    return CopyString(destination, capacity, candidate);
}

bool FindSumatraPathFromEnvironment(wchar_t* destination, size_t capacity) {
    wchar_t envPath[kMaxPathChars] = {};
    const DWORD envLength = GetEnvironmentVariableW(
        L"SUMATRA_PATH",
        envPath,
        static_cast<DWORD>(CountOf(envPath)));

    if (envLength > 0 && envLength < CountOf(envPath) && FileExists(envPath)) {
        return CopyString(destination, capacity, envPath);
    }

    return false;
}

bool FindSumatraOnSystemPath(wchar_t* destination, size_t capacity) {
    wchar_t pathEnvironment[kMaxEnvironmentChars] = {};
    const DWORD pathLength = GetEnvironmentVariableW(
        L"PATH",
        pathEnvironment,
        static_cast<DWORD>(CountOf(pathEnvironment)));

    if (pathLength == 0 || pathLength >= CountOf(pathEnvironment)) {
        return false;
    }

    wchar_t found[kMaxPathChars] = {};
    const DWORD foundLength = SearchPathW(
        pathEnvironment,
        L"SumatraPDF.exe",
        nullptr,
        static_cast<DWORD>(CountOf(found)),
        found,
        nullptr);

    if (foundLength == 0 || foundLength >= CountOf(found) || !FileExists(found)) {
        return false;
    }

    return CopyString(destination, capacity, found);
}

bool DiscoverSumatraPath(wchar_t* destination, size_t capacity, bool allowExecutableNameFallback) {
    destination[0] = L'\0';

    if (FindSumatraPathFromEnvironment(destination, capacity)) {
        return true;
    }

    if (FindDefaultSumatraPath(destination, capacity)) {
        return true;
    }

    if (FindLocalAppDataSumatraPath(destination, capacity)) {
        return true;
    }

    if (FindSumatraOnSystemPath(destination, capacity)) {
        return true;
    }

    if (allowExecutableNameFallback) {
        return CopyString(destination, capacity, L"SumatraPDF.exe");
    }

    return false;
}

void LoadConfig() {
    g_config.sumatraPath[0] = L'\0';
    g_config.usePathSearch = true;
    g_consumeKey = false;

    for (size_t i = 0; i < CountOf(g_actions); ++i) {
        g_actions[i].hotkey = g_actions[i].defaultHotkey;
        g_actions[i].enabled = g_actions[i].defaultEnabled;
        g_actions[i].registered = false;
        g_actions[i].rawPressed = false;
    }

    if (g_configPath[0] == L'\0') {
        DiscoverSumatraPath(g_config.sumatraPath, CountOf(g_config.sumatraPath), true);
        return;
    }

    GetPrivateProfileStringW(
        kSettingsSection,
        L"SumatraPath",
        L"",
        g_config.sumatraPath,
        static_cast<DWORD>(CountOf(g_config.sumatraPath)),
        g_configPath);
    TrimInPlace(g_config.sumatraPath);

    if (g_config.sumatraPath[0] == L'\0') {
        GetPrivateProfileStringW(
            kLegacySettingsSection,
            L"SumatraPath",
            L"",
            g_config.sumatraPath,
            static_cast<DWORD>(CountOf(g_config.sumatraPath)),
            g_configPath);
        TrimInPlace(g_config.sumatraPath);
    }

    if (g_config.sumatraPath[0] == L'\0' ||
        lstrcmpiW(g_config.sumatraPath, L"SumatraPDF.exe") == 0) {
        DiscoverSumatraPath(g_config.sumatraPath, CountOf(g_config.sumatraPath), true);
    }

    g_consumeKey = GetPrivateProfileIntW(
        L"Hotkeys",
        L"ConsumeKey",
        0,
        g_configPath) != 0;

    for (size_t i = 0; i < CountOf(g_actions); ++i) {
        const UINT value = GetPrivateProfileIntW(
            L"Hotkeys",
            g_actions[i].iniKey,
            g_actions[i].defaultHotkey,
            g_configPath);
        g_actions[i].hotkey = static_cast<WORD>(value & 0xffff);
        g_actions[i].enabled = GetPrivateProfileIntW(
            L"Hotkeys",
            g_actions[i].enabledIniKey,
            g_actions[i].defaultEnabled ? 1 : 0,
            g_configPath) != 0;
    }
}

bool SaveConfig() {
    if (g_configPath[0] == L'\0') {
        return false;
    }

    if (!WritePrivateProfileStringW(
            kSettingsSection,
            L"SumatraPath",
            g_config.sumatraPath,
            g_configPath)) {
        return false;
    }

    wchar_t consumeValue[32] = {};
    wsprintfW(consumeValue, L"%u", g_consumeKey ? 1u : 0u);
    if (!WritePrivateProfileStringW(
            L"Hotkeys",
            L"ConsumeKey",
            consumeValue,
            g_configPath)) {
        return false;
    }

    for (size_t i = 0; i < CountOf(g_actions); ++i) {
        wchar_t value[32] = {};
        wsprintfW(value, L"%u", static_cast<unsigned int>(g_actions[i].hotkey));
        if (!WritePrivateProfileStringW(
                L"Hotkeys",
                g_actions[i].iniKey,
                value,
                g_configPath)) {
            return false;
        }

        wsprintfW(value, L"%u", g_actions[i].enabled ? 1u : 0u);
        if (!WritePrivateProfileStringW(
                L"Hotkeys",
                g_actions[i].enabledIniKey,
                value,
                g_configPath)) {
            return false;
        }
    }

    return true;
}

void SetUiFromConfig() {
    g_updatingUi = true;

    if (g_sumatraEdit != nullptr) {
        SetWindowTextW(g_sumatraEdit, g_config.sumatraPath);
    }

    if (g_consumeKeyCheckbox != nullptr) {
        CheckDlgButton(
            g_mainWindow,
            kIdConsumeKey,
            g_consumeKey ? BST_CHECKED : BST_UNCHECKED);
    }

    for (size_t i = 0; i < CountOf(g_actions); ++i) {
        if (g_actions[i].control != nullptr) {
            SendMessageW(g_actions[i].control, HKM_SETHOTKEY, g_actions[i].hotkey, 0);
        }
        if (g_actions[i].enableControl != nullptr) {
            CheckDlgButton(
                g_mainWindow,
                g_actions[i].enableControlId,
                g_actions[i].enabled ? BST_CHECKED : BST_UNCHECKED);
        }
    }

    g_updatingUi = false;
}

void ReadUiToConfig() {
    if (g_sumatraEdit != nullptr) {
        GetWindowTextW(g_sumatraEdit, g_config.sumatraPath, static_cast<int>(CountOf(g_config.sumatraPath)));
        TrimInPlace(g_config.sumatraPath);
        SetWindowTextW(g_sumatraEdit, g_config.sumatraPath);
    }

    if (g_consumeKeyCheckbox != nullptr) {
        g_consumeKey = IsDlgButtonChecked(g_mainWindow, kIdConsumeKey) == BST_CHECKED;
    }

    for (size_t i = 0; i < CountOf(g_actions); ++i) {
        if (g_actions[i].control != nullptr) {
            g_actions[i].hotkey = static_cast<WORD>(
                SendMessageW(g_actions[i].control, HKM_GETHOTKEY, 0, 0) & 0xffff);
        }
        if (g_actions[i].enableControl != nullptr) {
            g_actions[i].enabled =
                IsDlgButtonChecked(g_mainWindow, g_actions[i].enableControlId) == BST_CHECKED;
        }
    }
}

BYTE HotkeyVk(WORD hotkey) {
    return static_cast<BYTE>(hotkey & 0x00ff);
}

BYTE HotkeyFlags(WORD hotkey) {
    return static_cast<BYTE>((hotkey >> 8) & 0x00ff);
}

UINT HotkeyRegisterModifiers(WORD hotkey) {
    const BYTE flags = HotkeyFlags(hotkey);
    UINT modifiers = MOD_NOREPEAT;

    if ((flags & HOTKEYF_ALT) != 0) {
        modifiers |= MOD_ALT;
    }
    if ((flags & HOTKEYF_CONTROL) != 0) {
        modifiers |= MOD_CONTROL;
    }
    if ((flags & HOTKEYF_SHIFT) != 0) {
        modifiers |= MOD_SHIFT;
    }

    return modifiers;
}

bool IsAllowedBareHotkey(WORD hotkey) {
    const BYTE vk = HotkeyVk(hotkey);
    const BYTE flags = static_cast<BYTE>(HotkeyFlags(hotkey) &
        ~(HOTKEYF_EXT));
    if (flags != 0) {
        return false;
    }

    return (vk >= VK_F13 && vk <= VK_F24) ||
           vk == VK_ADD ||
           vk == VK_SUBTRACT;
}

bool HasCtrlAltShiftModifier(WORD hotkey) {
    const BYTE flags = HotkeyFlags(hotkey);
    return (flags & (HOTKEYF_ALT | HOTKEYF_CONTROL | HOTKEYF_SHIFT)) != 0;
}

bool ValidateHotkeys() {
    for (size_t i = 0; i < CountOf(g_actions); ++i) {
        if (!g_actions[i].enabled) {
            continue;
        }

        const WORD hotkey = g_actions[i].hotkey;
        if (hotkey == 0) {
            continue;
        }

        if (!HasCtrlAltShiftModifier(hotkey) && !IsAllowedBareHotkey(hotkey)) {
            wchar_t status[kMaxStatusChars] = {};
            wsprintfW(
                status,
                L"%ls needs Ctrl/Alt/Shift, bare F13-F24, or numpad +/-.",
                g_actions[i].label);
            SetStatus(status);
            return false;
        }
    }

    for (size_t i = 0; i < CountOf(g_actions); ++i) {
        if (!g_actions[i].enabled || g_actions[i].hotkey == 0) {
            continue;
        }

        const BYTE vkA = HotkeyVk(g_actions[i].hotkey);
        const UINT modA = HotkeyRegisterModifiers(g_actions[i].hotkey) & ~MOD_NOREPEAT;

        for (size_t j = i + 1; j < CountOf(g_actions); ++j) {
            if (!g_actions[j].enabled || g_actions[j].hotkey == 0) {
                continue;
            }

            const BYTE vkB = HotkeyVk(g_actions[j].hotkey);
            const UINT modB = HotkeyRegisterModifiers(g_actions[j].hotkey) & ~MOD_NOREPEAT;
            if (vkA == vkB && modA == modB) {
                wchar_t status[kMaxStatusChars] = {};
                wsprintfW(
                    status,
                    L"%ls and %ls use the same hotkey.",
                    g_actions[i].label,
                    g_actions[j].label);
                SetStatus(status);
                return false;
            }
        }
    }

    return true;
}

void ResetRawPressedStates() {
    for (size_t i = 0; i < CountOf(g_actions); ++i) {
        g_actions[i].rawPressed = false;
    }
    g_rawCtrlDown = false;
    g_rawAltDown = false;
    g_rawShiftDown = false;
}

bool RegisterRawInputListener() {
    if (g_rawInputRegistered) {
        return true;
    }

    RAWINPUTDEVICE device = {};
    device.usUsagePage = 0x01;
    device.usUsage = 0x06;
    device.dwFlags = RIDEV_INPUTSINK;
    device.hwndTarget = g_mainWindow;

    if (!RegisterRawInputDevices(&device, 1, sizeof(device))) {
        SetStatusWithErrorPrefix(L"Could not register Raw Input.", GetLastError());
        return false;
    }

    ResetRawPressedStates();
    g_rawInputRegistered = true;
    return true;
}

void UnregisterRawInputListener() {
    if (!g_rawInputRegistered) {
        return;
    }

    RAWINPUTDEVICE device = {};
    device.usUsagePage = 0x01;
    device.usUsage = 0x06;
    device.dwFlags = RIDEV_REMOVE;
    device.hwndTarget = nullptr;
    RegisterRawInputDevices(&device, 1, sizeof(device));

    g_rawInputRegistered = false;
    ResetRawPressedStates();
}

void UnregisterAllHotkeys() {
    for (size_t i = 0; i < CountOf(g_actions); ++i) {
        if (g_actions[i].registered) {
            UnregisterHotKey(g_mainWindow, g_actions[i].id);
            g_actions[i].registered = false;
        }
    }
    UnregisterRawInputListener();
}

RegistrationCounts RegisterAllHotkeys() {
    RegistrationCounts counts = {};

    for (size_t i = 0; i < CountOf(g_actions); ++i) {
        const WORD hotkey = g_actions[i].hotkey;
        if (!g_actions[i].enabled || hotkey == 0) {
            continue;
        }

        if (!g_actions[i].quits) {
            ++counts.commandConfigured;
        }

        if (!g_consumeKey) {
            continue;
        }

        const BOOL ok = RegisterHotKey(
            g_mainWindow,
            g_actions[i].id,
            HotkeyRegisterModifiers(hotkey),
            HotkeyVk(hotkey));
        if (ok) {
            g_actions[i].registered = true;
            if (!g_actions[i].quits) {
                ++counts.commandRegistered;
            }
        } else {
            ++counts.failed;
            wchar_t status[kMaxStatusChars] = {};
            wchar_t errorText[256] = {};
            const DWORD error = GetLastError();
            FormatLastError(error, errorText, CountOf(errorText));
            wsprintfW(
                status,
                L"Could not register %ls. Error %lu: %ls",
                g_actions[i].label,
                static_cast<unsigned long>(error),
                errorText);
            SetStatus(status);
        }
    }

    if (!g_consumeKey || (counts.commandConfigured > 0 && counts.commandRegistered == 0 && counts.failed > 0)) {
        bool anyConfigured = false;
        for (size_t i = 0; i < CountOf(g_actions); ++i) {
            if (g_actions[i].enabled && g_actions[i].hotkey != 0) {
                anyConfigured = true;
                break;
            }
        }

        if (anyConfigured) {
            if (RegisterRawInputListener()) {
                counts.rawInputMode = true;
                counts.rawFallback = g_consumeKey;
                counts.commandRegistered = counts.commandConfigured;
            } else {
                ++counts.failed;
            }
        }
    }

    return counts;
}

const wchar_t* DdeErrorText(UINT error) {
    switch (error) {
        case DMLERR_NO_ERROR: return L"no error";
        case DMLERR_ADVACKTIMEOUT: return L"advice acknowledgement timeout";
        case DMLERR_BUSY: return L"DDE server is busy";
        case DMLERR_DATAACKTIMEOUT: return L"data acknowledgement timeout";
        case DMLERR_DLL_NOT_INITIALIZED: return L"DDE is not initialized";
        case DMLERR_DLL_USAGE: return L"DDEML usage error";
        case DMLERR_EXECACKTIMEOUT: return L"execute acknowledgement timeout";
        case DMLERR_INVALIDPARAMETER: return L"invalid DDE parameter";
        case DMLERR_LOW_MEMORY: return L"low memory";
        case DMLERR_MEMORY_ERROR: return L"memory error";
        case DMLERR_NOTPROCESSED: return L"command was not processed; SumatraPDF 3.5+ is needed for named commands";
        case DMLERR_NO_CONV_ESTABLISHED: return L"could not connect to SumatraPDF DDE server";
        case DMLERR_POKEACKTIMEOUT: return L"poke acknowledgement timeout";
        case DMLERR_POSTMSG_FAILED: return L"could not post DDE message";
        case DMLERR_REENTRANCY: return L"DDE reentrancy error";
        case DMLERR_SERVER_DIED: return L"SumatraPDF DDE server stopped responding";
        case DMLERR_SYS_ERROR: return L"system error";
        case DMLERR_UNADVACKTIMEOUT: return L"unadvise acknowledgement timeout";
        case DMLERR_UNFOUND_QUEUE_ID: return L"unknown DDE transaction";
        default: return L"unknown DDE error";
    }
}

HDDEDATA CALLBACK DdeCallback(UINT type,
                              UINT format,
                              HCONV conversation,
                              HSZ string1,
                              HSZ string2,
                              HDDEDATA data,
                              ULONG_PTR data1,
                              ULONG_PTR data2) {
    (void)type;
    (void)format;
    (void)conversation;
    (void)string1;
    (void)string2;
    (void)data;
    (void)data1;
    (void)data2;
    return nullptr;
}

bool BuildDdeExecuteString(const wchar_t* ddeCommand, char* destination, size_t capacity) {
    destination[0] = '\0';
    size_t length = 0;
    return AppendAnsiChar(destination, capacity, &length, '[') &&
           AppendWideAsAnsi(destination, capacity, &length, ddeCommand) &&
           AppendAnsiChar(destination, capacity, &length, ']');
}

void SetStatusWithDdeError(const wchar_t* prefix, UINT error) {
    wchar_t status[kMaxStatusChars] = {};
    wsprintfW(status, L"%ls DDE error %u: %ls", prefix, error, DdeErrorText(error));
    SetStatus(status);
}

bool RunSumatraDdeCommand(const wchar_t* ddeCommand) {
    char executeString[128] = {};
    if (!BuildDdeExecuteString(ddeCommand, executeString, CountOf(executeString))) {
        SetStatus(L"DDE command is too long.");
        return false;
    }

    DWORD ddeInstance = 0;
    const UINT initResult = DdeInitializeA(
        &ddeInstance,
        DdeCallback,
        APPCLASS_STANDARD | APPCMD_CLIENTONLY,
        0);
    if (initResult != DMLERR_NO_ERROR) {
        SetStatusWithDdeError(L"Could not initialize DDE.", initResult);
        return false;
    }

    bool ok = false;
    HSZ service = nullptr;
    HSZ topic = nullptr;
    HCONV conversation = nullptr;

    service = DdeCreateStringHandleA(ddeInstance, "SUMATRA", CP_WINANSI);
    topic = DdeCreateStringHandleA(ddeInstance, "control", CP_WINANSI);
    if (service == nullptr || topic == nullptr) {
        SetStatusWithDdeError(L"Could not create DDE handles.", DdeGetLastError(ddeInstance));
        goto cleanup;
    }

    conversation = DdeConnect(ddeInstance, service, topic, nullptr);
    if (conversation == nullptr) {
        SetStatusWithDdeError(
            L"Could not connect to SumatraPDF. Is SumatraPDF already running?",
            DdeGetLastError(ddeInstance));
        goto cleanup;
    }

    {
        HDDEDATA result = DdeClientTransaction(
            reinterpret_cast<LPBYTE>(executeString),
            static_cast<DWORD>(strlen(executeString) + 1),
            conversation,
            nullptr,
            0,
            XTYP_EXECUTE,
            kDdeTimeoutMs,
            nullptr);

        if (result == nullptr) {
            SetStatusWithDdeError(L"SumatraPDF did not accept the DDE command.", DdeGetLastError(ddeInstance));
            goto cleanup;
        }
    }

    ok = true;

cleanup:
    if (conversation != nullptr) {
        DdeDisconnect(conversation);
    }
    if (service != nullptr) {
        DdeFreeStringHandle(ddeInstance, service);
    }
    if (topic != nullptr) {
        DdeFreeStringHandle(ddeInstance, topic);
    }
    DdeUninitialize(ddeInstance);
    return ok;
}

bool IsModifierVk(BYTE vk) {
    return vk == VK_SHIFT ||
           vk == VK_LSHIFT ||
           vk == VK_RSHIFT ||
           vk == VK_CONTROL ||
           vk == VK_LCONTROL ||
           vk == VK_RCONTROL ||
           vk == VK_MENU ||
           vk == VK_LMENU ||
           vk == VK_RMENU;
}

void UpdateRawModifierState(BYTE vk, bool isDown) {
    if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) {
        g_rawShiftDown = isDown;
    } else if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) {
        g_rawCtrlDown = isDown;
    } else if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) {
        g_rawAltDown = isDown;
    }
}

bool RawModifiersMatch(WORD hotkey) {
    const BYTE flags = HotkeyFlags(hotkey);
    const bool wantCtrl = (flags & HOTKEYF_CONTROL) != 0;
    const bool wantAlt = (flags & HOTKEYF_ALT) != 0;
    const bool wantShift = (flags & HOTKEYF_SHIFT) != 0;

    return wantCtrl == g_rawCtrlDown &&
           wantAlt == g_rawAltDown &&
           wantShift == g_rawShiftDown;
}

void ResetRawPressedForVk(BYTE vk) {
    for (size_t i = 0; i < CountOf(g_actions); ++i) {
        if (HotkeyVk(g_actions[i].hotkey) == vk) {
            g_actions[i].rawPressed = false;
        }
    }
}

void FireAction(HotkeyAction* action) {
    if (action == nullptr) {
        return;
    }

    if (action->quits) {
        DestroyWindow(g_mainWindow);
        return;
    }

    if (action->ddeCommand != nullptr && RunSumatraDdeCommand(action->ddeCommand)) {
        wchar_t status[kMaxStatusChars] = {};
        wsprintfW(status, L"%ls sent to SumatraPDF.", action->label);
        SetStatus(status);
    }
}

void HandleRawKeyDown(BYTE vk) {
    if (IsModifierVk(vk)) {
        return;
    }

    for (size_t i = 0; i < CountOf(g_actions); ++i) {
        HotkeyAction* action = &g_actions[i];
        if (!action->enabled || action->hotkey == 0 || action->rawPressed) {
            continue;
        }

        if (HotkeyVk(action->hotkey) == vk && RawModifiersMatch(action->hotkey)) {
            action->rawPressed = true;
            FireAction(action);
        }
    }
}

void HandleRawInput(HRAWINPUT rawInputHandle) {
    if (!g_rawInputRegistered) {
        return;
    }

    RAWINPUT rawInput = {};
    UINT size = sizeof(rawInput);
    const UINT read = GetRawInputData(
        rawInputHandle,
        RID_INPUT,
        &rawInput,
        &size,
        sizeof(RAWINPUTHEADER));

    if (read == static_cast<UINT>(-1) || rawInput.header.dwType != RIM_TYPEKEYBOARD) {
        return;
    }

    const RAWKEYBOARD* keyboard = &rawInput.data.keyboard;
    if (keyboard->VKey == 0 || keyboard->VKey == 0xff) {
        return;
    }

    const BYTE vk = static_cast<BYTE>(keyboard->VKey & 0xff);
    const bool isUp = (keyboard->Flags & RI_KEY_BREAK) != 0;
    const bool isDown = !isUp;

    UpdateRawModifierState(vk, isDown);

    if (isUp) {
        ResetRawPressedForVk(vk);
        return;
    }

    HandleRawKeyDown(vk);
}

bool ApplyCurrentSettings(bool save) {
    ReadUiToConfig();

    if (!ValidateHotkeys()) {
        return false;
    }

    UnregisterAllHotkeys();
    const RegistrationCounts counts = RegisterAllHotkeys();

    if (counts.commandConfigured == 0) {
        SetStatus(L"No PDF command hotkeys are configured.");
        return false;
    }

    if (save && !SaveConfig()) {
        SetStatusWithErrorPrefix(L"Could not save settings.", GetLastError());
        return false;
    }

    wchar_t status[kMaxStatusChars] = {};
    if (counts.rawFallback) {
        if (save) {
            wsprintfW(
                status,
                L"Settings saved. Consume key failed; %d PDF hotkey(s) active non-consuming.",
                counts.commandRegistered);
        } else {
            wsprintfW(
                status,
                L"Consume key failed; %d PDF hotkey(s) active non-consuming.",
                counts.commandRegistered);
        }
    } else if (counts.failed == 0) {
        const wchar_t* modeText = counts.rawInputMode ? L"non-consuming" : L"consuming";
        if (save) {
            wsprintfW(
                status,
                L"Settings saved. %d PDF hotkey(s) active (%ls).",
                counts.commandRegistered,
                modeText);
        } else {
            wsprintfW(status, L"%d PDF hotkey(s) active (%ls).", counts.commandRegistered, modeText);
        }
    } else {
        wsprintfW(
            status,
            L"%d PDF hotkey(s) active, %d registration failed.",
            counts.commandRegistered,
            counts.failed);
    }
    SetStatus(status);
    return counts.failed == 0 || counts.rawFallback;
}

void RestoreDefaultHotkeys() {
    g_updatingUi = true;
    g_consumeKey = false;
    if (g_consumeKeyCheckbox != nullptr) {
        CheckDlgButton(g_mainWindow, kIdConsumeKey, BST_UNCHECKED);
    }

    for (size_t i = 0; i < CountOf(g_actions); ++i) {
        g_actions[i].hotkey = g_actions[i].defaultHotkey;
        g_actions[i].enabled = g_actions[i].defaultEnabled;
        if (g_actions[i].control != nullptr) {
            SendMessageW(g_actions[i].control, HKM_SETHOTKEY, g_actions[i].hotkey, 0);
        }
        if (g_actions[i].enableControl != nullptr) {
            CheckDlgButton(
                g_mainWindow,
                g_actions[i].enableControlId,
                g_actions[i].enabled ? BST_CHECKED : BST_UNCHECKED);
        }
    }

    g_updatingUi = false;
    ApplyCurrentSettings(false);
    SetStatus(L"Default hotkeys applied. Save to keep them.");
}

void BrowseForSumatra() {
    wchar_t selected[kMaxPathChars] = {};
    GetWindowTextW(g_sumatraEdit, selected, static_cast<int>(CountOf(selected)));
    TrimInPlace(selected);

    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_mainWindow;
    dialog.lpstrFilter = L"SumatraPDF.exe\0SumatraPDF.exe\0Programs (*.exe)\0*.exe\0All files (*.*)\0*.*\0";
    dialog.lpstrFile = selected;
    dialog.nMaxFile = static_cast<DWORD>(CountOf(selected));
    dialog.lpstrTitle = L"Select SumatraPDF.exe";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&dialog)) {
        SetWindowTextW(g_sumatraEdit, selected);
        SetStatus(L"Path selected. Save and apply to keep it.");
    }
}

void FindSumatraInPathButton() {
    wchar_t found[kMaxPathChars] = {};
    if (FindSumatraOnSystemPath(found, CountOf(found))) {
        SetWindowTextW(g_sumatraEdit, found);
        SetStatus(L"Found SumatraPDF.exe in system PATH. Save and apply to keep it.");
        return;
    }

    SetStatus(L"SumatraPDF.exe was not found in system PATH.");
}

bool AddTrayIcon(HWND window) {
    if (g_trayIconVisible) {
        return true;
    }

    NOTIFYICONDATAW iconData = {};
    iconData.cbSize = sizeof(iconData);
    iconData.hWnd = window;
    iconData.uID = kTrayIconId;
    iconData.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    iconData.uCallbackMessage = kTrayCallbackMessage;
    iconData.hIcon = LoadIconW(g_instance, MAKEINTRESOURCEW(IDI_PAGEHOTKEYS));
    CopyString(iconData.szTip, CountOf(iconData.szTip), L"PageHotkeys");

    if (!Shell_NotifyIconW(NIM_ADD, &iconData)) {
        SetStatusWithErrorPrefix(L"Could not add tray icon.", GetLastError());
        return false;
    }

    g_trayIconVisible = true;
    return true;
}

void RemoveTrayIcon(HWND window) {
    if (!g_trayIconVisible) {
        return;
    }

    NOTIFYICONDATAW iconData = {};
    iconData.cbSize = sizeof(iconData);
    iconData.hWnd = window;
    iconData.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &iconData);
    g_trayIconVisible = false;
}

void HideToTray(HWND window) {
    if (!AddTrayIcon(window)) {
        return;
    }

    ShowWindow(window, SW_HIDE);
}

void RestoreFromTray(HWND window) {
    RemoveTrayIcon(window);
    ShowWindow(window, SW_SHOW);
}

void CenterWindow(HWND window) {
    RECT rect = {};
    GetWindowRect(window, &rect);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;

    const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    const int x = (screenWidth - width) / 2;
    const int y = (screenHeight - height) / 2;
    SetWindowPos(window, nullptr, x, y, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
}

void CreateInfoFonts() {
    if (g_infoTitleFont == nullptr) {
        g_infoTitleFont = CreateFontW(
            -22,
            0,
            0,
            0,
            FW_SEMIBOLD,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI");
    }

    if (g_infoBodyFont == nullptr) {
        g_infoBodyFont = CreateFontW(
            -15,
            0,
            0,
            0,
            FW_NORMAL,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI");
    }

    if (g_infoCreditFont == nullptr) {
        g_infoCreditFont = CreateFontW(
            -17,
            0,
            0,
            0,
            FW_SEMIBOLD,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI");
    }
}

LRESULT CALLBACK InfoWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            CreateInfoFonts();

            HWND title = CreateChild(
                L"STATIC",
                L"Page Hotkeys for Sumatra PDF",
                SS_CENTER,
                0,
                22,
                18,
                500,
                28,
                window,
                -1);
            if (title != nullptr && g_infoTitleFont != nullptr) {
                SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(g_infoTitleFont), TRUE);
            }

            HWND body = CreateChild(
                L"STATIC",
                L"Tool for control of SumatraPDF even from fullscreen applications.\r\n"
                L"\r\n"
                L"project is an independent tool and is not affiliated with, endorsed by,\r\n"
                L"or maintained by the SumatraPDF project. SumatraPDF is a separate\r\n"
                L"open-source PDF reader available at https://www.sumatrapdfreader.org/.\r\n"
                L"\r\n"
                L"This program needs SumatraPDF 3.5+ in order to work properly !",
                SS_CENTER,
                0,
                22,
                68,
                500,
                154,
                window,
                -1);
            if (body != nullptr && g_infoBodyFont != nullptr) {
                SendMessageW(body, WM_SETFONT, reinterpret_cast<WPARAM>(g_infoBodyFont), TRUE);
            }

            HWND version = CreateChild(L"STATIC", L"version 0.1.5", SS_CENTER, 0,
                                       22, 240, 500, 24, window, -1);
            if (version != nullptr && g_infoBodyFont != nullptr) {
                SendMessageW(version, WM_SETFONT, reinterpret_cast<WPARAM>(g_infoBodyFont), TRUE);
            }

            HWND credit = CreateChild(L"STATIC", L"by wojtron", SS_CENTER, 0,
                                      22, 266, 500, 26, window, kIdInfoCredit);
            if (credit != nullptr && g_infoCreditFont != nullptr) {
                SendMessageW(credit, WM_SETFONT, reinterpret_cast<WPARAM>(g_infoCreditFont), TRUE);
            }

            HWND closeButton = CreateChild(L"BUTTON", L"Close", WS_TABSTOP | BS_DEFPUSHBUTTON, 0,
                                           232, 300, 96, 30, window, IDOK);
            if (closeButton != nullptr && g_infoBodyFont != nullptr) {
                SendMessageW(closeButton, WM_SETFONT, reinterpret_cast<WPARAM>(g_infoBodyFont), TRUE);
            }
            return 0;
        }

        case WM_CTLCOLORSTATIC:
            if (reinterpret_cast<HWND>(lParam) == GetDlgItem(window, kIdInfoCredit)) {
                SetTextColor(reinterpret_cast<HDC>(wParam), RGB(192, 0, 0));
                SetBkColor(reinterpret_cast<HDC>(wParam), GetSysColor(COLOR_BTNFACE));
                return reinterpret_cast<LRESULT>(g_infoBackgroundBrush);
            }
            break;

        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
                DestroyWindow(window);
                return 0;
            }
            break;

        case WM_CLOSE:
            DestroyWindow(window);
            return 0;

        default:
            return DefWindowProcW(window, message, wParam, lParam);
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

bool RegisterInfoWindowClass() {
    WNDCLASSW windowClass = {};
    windowClass.lpfnWndProc = InfoWindowProc;
    windowClass.hInstance = g_instance;
    windowClass.lpszClassName = kInfoClassName;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(g_instance, MAKEINTRESOURCEW(IDI_PAGEHOTKEYS));
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);

    return RegisterClassW(&windowClass) != 0;
}

void ShowInfoWindow(HWND owner) {
    HWND infoWindow = CreateWindowExW(
        WS_EX_TOPMOST,
        kInfoClassName,
        L"Info",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        560,
        382,
        owner,
        nullptr,
        g_instance,
        nullptr);

    if (infoWindow == nullptr) {
        SetStatusWithErrorPrefix(L"Could not open info window.", GetLastError());
        return;
    }

    CenterWindow(infoWindow);
    ShowWindow(infoWindow, SW_SHOW);
    UpdateWindow(infoWindow);
}

void CreateMainControls(HWND window) {
    CreateChild(L"STATIC", L"SumatraPDF.exe", WS_TABSTOP, 0,
                18, 18, 160, 20, window, -1);
    g_sumatraEdit = CreateChild(L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
                                WS_EX_CLIENTEDGE, 18, 42, 330, 24, window, kIdSumatraEdit);
    CreateChild(L"BUTTON", L"Browse...", WS_TABSTOP | BS_PUSHBUTTON, 0,
                362, 40, 96, 28, window, kIdBrowse);
    CreateChild(L"BUTTON", L"Find in PATH", WS_TABSTOP | BS_PUSHBUTTON, 0,
                470, 40, 110, 28, window, kIdFindPath);

    CreateChild(L"BUTTON", L"Hotkeys", BS_GROUPBOX, 0,
                18, 84, 562, 210, window, -1);
    g_consumeKeyCheckbox = CreateChild(
        L"BUTTON",
        L"Consume key",
        WS_TABSTOP | BS_AUTOCHECKBOX,
        0,
        455,
        116,
        110,
        24,
        window,
        kIdConsumeKey);

    const int checkboxX = 42;
    const int hotkeyX = 220;
    const int firstY = 116;
    const int rowHeight = 34;

    for (size_t i = 0; i < CountOf(g_actions); ++i) {
        const int y = firstY + static_cast<int>(i) * rowHeight;
        g_actions[i].enableControl = CreateChild(
            L"BUTTON",
            g_actions[i].label,
            WS_TABSTOP | BS_AUTOCHECKBOX,
            0,
            checkboxX,
            y,
            150,
            24,
            window,
            g_actions[i].enableControlId);
        g_actions[i].control = CreateChild(
            HOTKEY_CLASSW,
            L"",
            WS_TABSTOP | WS_BORDER,
            WS_EX_CLIENTEDGE,
            hotkeyX,
            y,
            220,
            24,
            window,
            g_actions[i].controlId);
    }

    CreateChild(L"BUTTON", L"Save and apply", WS_TABSTOP | BS_DEFPUSHBUTTON, 0,
                18, 314, 140, 32, window, kIdSaveApply);
    CreateChild(L"BUTTON", L"Defaults", WS_TABSTOP | BS_PUSHBUTTON, 0,
                170, 314, 95, 32, window, kIdDefaults);
    CreateChild(L"BUTTON", L"Tray", WS_TABSTOP | BS_PUSHBUTTON, 0,
                277, 314, 105, 32, window, kIdTray);
    CreateChild(L"BUTTON", L"Info", WS_TABSTOP | BS_PUSHBUTTON, 0,
                394, 314, 79, 32, window, kIdInfo);
    CreateChild(L"BUTTON", L"Exit", WS_TABSTOP | BS_PUSHBUTTON, 0,
                485, 314, 95, 32, window, kIdExit);

    g_status = CreateChild(L"STATIC", L"", 0, 0,
                           18, 366, 562, 42, window, kIdStatus);
}

HotkeyAction* FindActionById(int id) {
    for (size_t i = 0; i < CountOf(g_actions); ++i) {
        if (g_actions[i].id == id) {
            return &g_actions[i];
        }
    }

    return nullptr;
}

bool IsSettingsControlChange(int id, int notificationCode) {
    if (id == kIdConsumeKey && notificationCode == BN_CLICKED) {
        return true;
    }

    for (size_t i = 0; i < CountOf(g_actions); ++i) {
        if (id == g_actions[i].enableControlId && notificationCode == BN_CLICKED) {
            return true;
        }
        if (id == g_actions[i].controlId && notificationCode == EN_CHANGE) {
            return true;
        }
    }

    return false;
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            g_mainWindow = window;
            CreateMainControls(window);
            SetUiFromConfig();
            ApplyCurrentSettings(false);
            return 0;

        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            const int notificationCode = HIWORD(wParam);
            switch (id) {
                case kIdBrowse:
                    BrowseForSumatra();
                    return 0;
                case kIdFindPath:
                    FindSumatraInPathButton();
                    return 0;
                case kIdSaveApply:
                    ApplyCurrentSettings(true);
                    return 0;
                case kIdDefaults:
                    RestoreDefaultHotkeys();
                    return 0;
                case kIdTray:
                    HideToTray(window);
                    return 0;
                case kIdInfo:
                    ShowInfoWindow(window);
                    return 0;
                case kIdExit:
                    DestroyWindow(window);
                    return 0;
                default:
                    if (!g_updatingUi && IsSettingsControlChange(id, notificationCode)) {
                        ApplyCurrentSettings(false);
                        return 0;
                    }
                    break;
            }
            break;
        }

        case WM_HOTKEY: {
            HotkeyAction* action = FindActionById(static_cast<int>(wParam));
            FireAction(action);
            return 0;
        }

        case WM_INPUT:
            HandleRawInput(reinterpret_cast<HRAWINPUT>(lParam));
            return 0;

        case kTrayCallbackMessage:
            if (lParam == WM_LBUTTONUP ||
                lParam == WM_LBUTTONDBLCLK ||
                lParam == WM_RBUTTONUP) {
                RestoreFromTray(window);
            }
            return 0;

        case WM_CLOSE:
            DestroyWindow(window);
            return 0;

        case WM_DESTROY:
            RemoveTrayIcon(window);
            UnregisterAllHotkeys();
            if (g_infoTitleFont != nullptr) {
                DeleteObject(g_infoTitleFont);
                g_infoTitleFont = nullptr;
            }
            if (g_infoBodyFont != nullptr) {
                DeleteObject(g_infoBodyFont);
                g_infoBodyFont = nullptr;
            }
            if (g_infoCreditFont != nullptr) {
                DeleteObject(g_infoCreditFont);
                g_infoCreditFont = nullptr;
            }
            if (g_infoBackgroundBrush != nullptr) {
                DeleteObject(g_infoBackgroundBrush);
                g_infoBackgroundBrush = nullptr;
            }
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(window, message, wParam, lParam);
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

bool RegisterMainWindowClass() {
    WNDCLASSW windowClass = {};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = g_instance;
    windowClass.lpszClassName = kAppClassName;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(g_instance, MAKEINTRESOURCEW(IDI_PAGEHOTKEYS));
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);

    return RegisterClassW(&windowClass) != 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance,
                    HINSTANCE previousInstance,
                    PWSTR commandLine,
                    int showCommand) {
    (void)previousInstance;
    (void)commandLine;

    g_instance = instance;
    g_font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    BuildConfigPath();
    LoadConfig();
    g_infoBackgroundBrush = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));

    INITCOMMONCONTROLSEX commonControls = {};
    commonControls.dwSize = sizeof(commonControls);
    commonControls.dwICC = ICC_HOTKEY_CLASS;
    InitCommonControlsEx(&commonControls);

    if (!RegisterMainWindowClass()) {
        MessageBoxW(nullptr, L"Could not register the main window class.", kAppTitle, MB_ICONERROR);
        return 1;
    }

    if (!RegisterInfoWindowClass()) {
        MessageBoxW(nullptr, L"Could not register the info window class.", kAppTitle, MB_ICONERROR);
        return 1;
    }

    g_mainWindow = CreateWindowExW(
        0,
        kAppClassName,
        kAppTitle,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        616,
        458,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (g_mainWindow == nullptr) {
        MessageBoxW(nullptr, L"Could not create the main window.", kAppTitle, MB_ICONERROR);
        return 1;
    }

    CenterWindow(g_mainWindow);
    ShowWindow(g_mainWindow, showCommand);
    UpdateWindow(g_mainWindow);

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return static_cast<int>(message.wParam);
}
