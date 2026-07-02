#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <d3d11.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#pragma comment(lib, "d3d11.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
    struct InputEvent
    {
        double timeSeconds = 0.0;
        std::string action;
        std::string appName;
        DWORD processId = 0;
        std::string detail;
    };

    struct AppInputStats
    {
        std::string appName;
        DWORD processId = 0;
        uint64_t total = 0;
        uint64_t downTotal = 0;
        uint64_t upTotal = 0;
    };

    struct InputStats
    {
        std::string id;
        std::string device;
        std::string label;
        uint64_t total = 0;
        uint64_t downTotal = 0;
        uint64_t upTotal = 0;
        bool isDown = false;
        double lastTimeSeconds = 0.0;
        std::string lastAppName;
        std::deque<InputEvent> events;
        std::vector<AppInputStats> appTotals;
    };

    struct FocusContext
    {
        std::string appName = "Unknown";
        DWORD processId = 0;
    };

    struct MouseDeltaSample
    {
        float dx = 0.0f;
        float dy = 0.0f;
        std::string appName;
        DWORD processId = 0;
    };

    struct ProgramHeatmap
    {
        std::string appName;
        DWORD processId = 0;
        std::vector<uint32_t> bins;
        uint32_t maxBin = 0;
    };

    struct MonitorHeatmap
    {
        RECT rect{};
        std::string name;
        int columns = 0;
        int rows = 0;
        std::vector<uint32_t> bins;
        uint32_t maxBin = 0;
        std::vector<ProgramHeatmap> programHeatmaps;
    };

    HWND g_hwnd = nullptr;
    ID3D11Device* g_pd3dDevice = nullptr;
    ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
    IDXGISwapChain* g_pSwapChain = nullptr;
    ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
    std::vector<InputStats> g_inputs;
    std::vector<MonitorHeatmap> g_monitors;
    std::string g_selectedInputId;
    std::string g_selectedProgramName;
    DWORD g_selectedProgramPid = 0;
    std::string g_visualFilterProgramName;
    DWORD g_visualFilterProgramPid = 0;
    int g_selectedMonitorIndex = 0;
    bool g_rebuildHomeDockLayout = true;
    bool g_rebuildKeyboardDockLayout = true;
    bool g_rebuildMouseDockLayout = true;
    bool g_rebuildProgramsDockLayout = true;
    std::array<bool, 256> g_keysDown{};
    std::array<std::string, 256> g_activeCombosByKey{};
    std::deque<MouseDeltaSample> g_mouseDeltaSamples;
    POINT g_mouseDelta{};
    int g_wheelDelta = 0;
    int g_leftClicks = 0;
    int g_rightClicks = 0;
    int g_middleClicks = 0;
    bool g_captureEnabled = true;
    bool g_rawInputRegistered = false;
    bool g_trayIconVisible = false;
    bool g_autoSaveEnabled = true;
    float g_heatmapCellScale = 0.02f;
    int g_cursorHeatRadiusPixels = 24;
    int g_historyMouseMoveLimit = 25;
    std::string g_dataFilePath = "keynalysis_autosave.kna";
    std::string g_settingsFilePath = "keynalysis_settings.cfg";
    std::string g_imguiIniPath = "keynalysis_imgui.ini";
    std::string g_saveLoadStatus;
    auto g_lastAutoSaveTime = std::chrono::steady_clock::now();
    double g_loadedRuntimeSeconds = 0.0;
    auto g_startTime = std::chrono::steady_clock::now();

    constexpr UINT WM_TRAYICON = WM_APP + 1;
    constexpr UINT TRAY_ICON_ID = 1;
    constexpr int LEGACY_HEATMAP_COLUMNS = 48;
    constexpr int LEGACY_HEATMAP_ROWS = 27;
    constexpr int AUTO_SAVE_SECONDS = 5 * 60;

    double NowSeconds()
    {
        using namespace std::chrono;
        return g_loadedRuntimeSeconds + duration<double>(steady_clock::now() - g_startTime).count();
    }

    std::string WideToUtf8(const wchar_t* text)
    {
        if (!text || !*text)
            return {};

        const int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
        if (needed <= 1)
            return {};

        std::string result(static_cast<size_t>(needed - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), needed, nullptr, nullptr);
        return result;
    }

    std::string KeyName(USHORT virtualKey, USHORT scanCode, USHORT flags)
    {
        wchar_t name[128]{};
        LONG lParam = static_cast<LONG>(scanCode) << 16;
        if ((flags & RI_KEY_E0) != 0)
            lParam |= 1 << 24;

        if (GetKeyNameTextW(lParam, name, static_cast<int>(std::size(name))) > 0)
            return WideToUtf8(name);

        char fallback[32]{};
        snprintf(fallback, sizeof(fallback), "VK 0x%02X", virtualKey);
        return fallback;
    }

    bool IsModifierKey(USHORT virtualKey)
    {
        return virtualKey == VK_CONTROL ||
            virtualKey == VK_LCONTROL ||
            virtualKey == VK_RCONTROL ||
            virtualKey == VK_MENU ||
            virtualKey == VK_LMENU ||
            virtualKey == VK_RMENU ||
            virtualKey == VK_SHIFT ||
            virtualKey == VK_LSHIFT ||
            virtualKey == VK_RSHIFT;
    }

    bool IsCtrlDown()
    {
        return g_keysDown[VK_CONTROL] ||
            g_keysDown[VK_LCONTROL] ||
            g_keysDown[VK_RCONTROL] ||
            (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 ||
            (GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0 ||
            (GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0;
    }

    bool IsAltDown()
    {
        return g_keysDown[VK_MENU] ||
            g_keysDown[VK_LMENU] ||
            g_keysDown[VK_RMENU] ||
            (GetAsyncKeyState(VK_MENU) & 0x8000) != 0 ||
            (GetAsyncKeyState(VK_LMENU) & 0x8000) != 0 ||
            (GetAsyncKeyState(VK_RMENU) & 0x8000) != 0;
    }

    bool IsShiftDown()
    {
        return g_keysDown[VK_SHIFT] ||
            g_keysDown[VK_LSHIFT] ||
            g_keysDown[VK_RSHIFT] ||
            (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 ||
            (GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0 ||
            (GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0;
    }

    std::string ComboLabelForKey(const std::string& key)
    {
        std::string label;
        if (IsCtrlDown())
            label += "Ctrl+";
        if (IsAltDown())
            label += "Alt+";
        if (IsShiftDown())
            label += "Shift+";

        if (label.empty())
            return {};

        label += key;
        return label;
    }

    std::string FileNameFromPath(const std::string& path)
    {
        const size_t slash = path.find_last_of("\\/");
        if (slash == std::string::npos)
            return path;

        return path.substr(slash + 1);
    }

    FocusContext GetFocusContext()
    {
        FocusContext context;
        HWND foreground = GetForegroundWindow();
        if (!foreground)
            return context;

        GetWindowThreadProcessId(foreground, &context.processId);
        if (context.processId == 0)
            return context;

        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, context.processId);
        if (!process)
            return context;

        wchar_t imagePath[MAX_PATH]{};
        DWORD imagePathSize = static_cast<DWORD>(std::size(imagePath));
        if (QueryFullProcessImageNameW(process, 0, imagePath, &imagePathSize))
            context.appName = FileNameFromPath(WideToUtf8(imagePath));

        CloseHandle(process);
        return context;
    }

    BOOL CALLBACK MonitorEnumProc(HMONITOR monitor, HDC, LPRECT, LPARAM)
    {
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info))
            return TRUE;

        MonitorHeatmap heatmap{};
        heatmap.rect = info.rcMonitor;
        heatmap.name = WideToUtf8(info.szDevice);
        if ((info.dwFlags & MONITORINFOF_PRIMARY) != 0)
            heatmap.name += " (Primary)";

        const int width = static_cast<int>(std::max(1L, heatmap.rect.right - heatmap.rect.left));
        const int height = static_cast<int>(std::max(1L, heatmap.rect.bottom - heatmap.rect.top));
        const float cellPixels = std::max(1.0f, static_cast<float>(std::max(width, height)) * g_heatmapCellScale);
        heatmap.columns = std::clamp(static_cast<int>(std::ceil(static_cast<float>(width) / cellPixels)), 1, 512);
        heatmap.rows = std::clamp(static_cast<int>(std::ceil(static_cast<float>(height) / cellPixels)), 1, 512);
        heatmap.bins.assign(static_cast<size_t>(heatmap.columns * heatmap.rows), 0);

        g_monitors.push_back(std::move(heatmap));
        return TRUE;
    }

    void ClearHeatmapData()
    {
        for (MonitorHeatmap& monitor : g_monitors)
        {
            monitor.bins.assign(static_cast<size_t>(monitor.columns * monitor.rows), 0);
            monitor.maxBin = 0;
            monitor.programHeatmaps.clear();
        }
    }

    void RefreshMonitors()
    {
        g_monitors.clear();
        EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, 0);
        if (g_selectedMonitorIndex >= static_cast<int>(g_monitors.size()))
            g_selectedMonitorIndex = 0;
    }

    int FindMonitorIndexForPoint(const POINT& point)
    {
        for (int i = 0; i < static_cast<int>(g_monitors.size()); ++i)
        {
            const RECT& rect = g_monitors[i].rect;
            if (point.x >= rect.left && point.x < rect.right && point.y >= rect.top && point.y < rect.bottom)
                return i;
        }

        return -1;
    }

    bool MatchesVisualProgramFilter(const std::string& appName, DWORD processId)
    {
        return g_visualFilterProgramPid == 0 ||
            (processId == g_visualFilterProgramPid && appName == g_visualFilterProgramName);
    }

    ProgramHeatmap& GetProgramHeatmap(MonitorHeatmap& monitor, const FocusContext& focus)
    {
        for (ProgramHeatmap& programHeatmap : monitor.programHeatmaps)
        {
            if (programHeatmap.processId == focus.processId && programHeatmap.appName == focus.appName)
                return programHeatmap;
        }

        ProgramHeatmap programHeatmap;
        programHeatmap.appName = focus.appName;
        programHeatmap.processId = focus.processId;
        programHeatmap.bins.assign(static_cast<size_t>(monitor.columns * monitor.rows), 0);
        monitor.programHeatmaps.push_back(std::move(programHeatmap));
        return monitor.programHeatmaps.back();
    }

    void AddMouseDeltaSample(LONG dx, LONG dy, const FocusContext& focus)
    {
        g_mouseDeltaSamples.push_back({ static_cast<float>(dx), static_cast<float>(dy), focus.appName, focus.processId });
        while (g_mouseDeltaSamples.size() > 240)
            g_mouseDeltaSamples.pop_front();
    }

    void AddCursorHeatSample(const FocusContext& focus)
    {
        if (g_monitors.empty())
            RefreshMonitors();

        POINT cursor{};
        if (!GetCursorPos(&cursor))
            return;

        const int monitorIndex = FindMonitorIndexForPoint(cursor);
        if (monitorIndex < 0)
            return;

        MonitorHeatmap& heatmap = g_monitors[monitorIndex];
        const RECT& rect = heatmap.rect;
        const int width = static_cast<int>(std::max(1L, rect.right - rect.left));
        const int height = static_cast<int>(std::max(1L, rect.bottom - rect.top));
        const int rawBinX = static_cast<int>((cursor.x - rect.left) * heatmap.columns / width);
        const int rawBinY = static_cast<int>((cursor.y - rect.top) * heatmap.rows / height);
        ProgramHeatmap& programHeatmap = GetProgramHeatmap(heatmap, focus);
        const int centerBinX = std::clamp(rawBinX, 0, heatmap.columns - 1);
        const int centerBinY = std::clamp(rawBinY, 0, heatmap.rows - 1);
        const float cellW = static_cast<float>(width) / static_cast<float>(std::max(1, heatmap.columns));
        const float cellH = static_cast<float>(height) / static_cast<float>(std::max(1, heatmap.rows));
        const int radiusBinsX = std::max(0, static_cast<int>(std::ceil(static_cast<float>(g_cursorHeatRadiusPixels) / std::max(1.0f, cellW))));
        const int radiusBinsY = std::max(0, static_cast<int>(std::ceil(static_cast<float>(g_cursorHeatRadiusPixels) / std::max(1.0f, cellH))));
        const int minX = std::clamp(centerBinX - radiusBinsX, 0, heatmap.columns - 1);
        const int maxX = std::clamp(centerBinX + radiusBinsX, 0, heatmap.columns - 1);
        const int minY = std::clamp(centerBinY - radiusBinsY, 0, heatmap.rows - 1);
        const int maxY = std::clamp(centerBinY + radiusBinsY, 0, heatmap.rows - 1);

        for (int y = minY; y <= maxY; ++y)
        {
            for (int x = minX; x <= maxX; ++x)
            {
                const float cellCenterX = rect.left + (static_cast<float>(x) + 0.5f) * cellW;
                const float cellCenterY = rect.top + (static_cast<float>(y) + 0.5f) * cellH;
                const float dx = cellCenterX - static_cast<float>(cursor.x);
                const float dy = cellCenterY - static_cast<float>(cursor.y);
                if ((dx * dx + dy * dy) > static_cast<float>(g_cursorHeatRadiusPixels * g_cursorHeatRadiusPixels))
                    continue;

                const int binIndex = y * heatmap.columns + x;
                ++heatmap.bins[binIndex];
                heatmap.maxBin = std::max(heatmap.maxBin, heatmap.bins[binIndex]);
                ++programHeatmap.bins[binIndex];
                programHeatmap.maxBin = std::max(programHeatmap.maxBin, programHeatmap.bins[binIndex]);
            }
        }
    }

    InputStats& GetInputStats(const std::string& id, const std::string& device, const std::string& label)
    {
        for (InputStats& input : g_inputs)
        {
            if (input.id == id)
                return input;
        }

        g_inputs.push_back({ id, device, label });
        return g_inputs.back();
    }

    InputStats* FindInputStats(const std::string& id)
    {
        for (InputStats& input : g_inputs)
        {
            if (input.id == id)
                return &input;
        }

        return nullptr;
    }

    AppInputStats& GetAppStats(InputStats& input, const FocusContext& focus)
    {
        for (AppInputStats& app : input.appTotals)
        {
            if (app.processId == focus.processId && app.appName == focus.appName)
                return app;
        }

        input.appTotals.push_back({ focus.appName, focus.processId });
        return input.appTotals.back();
    }

    void AddInputEvent(const std::string& id, const std::string& device, const std::string& label, std::string action, std::string detail)
    {
        InputStats& input = GetInputStats(id, device, label);
        const FocusContext focus = GetFocusContext();
        const double now = NowSeconds();

        ++input.total;
        input.lastTimeSeconds = now;
        input.lastAppName = focus.appName;

        AppInputStats& appStats = GetAppStats(input, focus);
        ++appStats.total;

        if (action == "Down")
        {
            ++input.downTotal;
            ++appStats.downTotal;
            input.isDown = true;
        }
        else if (action == "Up")
        {
            ++input.upTotal;
            ++appStats.upTotal;
            input.isDown = false;
        }

        input.events.push_front({ now, std::move(action), focus.appName, focus.processId, std::move(detail) });
        while (input.events.size() > 100)
            input.events.pop_back();
    }

    void AddComboEvent(const std::string& comboLabel, std::string action, std::string detail)
    {
        AddInputEvent("combo:" + comboLabel, "Keyboard Combo", comboLabel, std::move(action), std::move(detail));
    }

    void PollSystemCombos()
    {
        const bool altTabDown = IsAltDown() && (GetAsyncKeyState(VK_TAB) & 0x8000) != 0;
        std::string& activeAltTab = g_activeCombosByKey[VK_TAB];

        if (altTabDown && activeAltTab.empty())
        {
            activeAltTab = "Alt+Tab";
            AddComboEvent(activeAltTab, "Down", "Polled system combo");
        }
        else if (!altTabDown && activeAltTab == "Alt+Tab")
        {
            AddComboEvent(activeAltTab, "Up", "Polled system combo released");
            activeAltTab.clear();
        }
    }

    template <typename T>
    bool WriteValue(std::ofstream& out, const T& value)
    {
        out.write(reinterpret_cast<const char*>(&value), sizeof(T));
        return out.good();
    }

    template <typename T>
    bool ReadValue(std::ifstream& in, T& value)
    {
        in.read(reinterpret_cast<char*>(&value), sizeof(T));
        return in.good();
    }

    bool WriteString(std::ofstream& out, const std::string& value)
    {
        const uint32_t size = static_cast<uint32_t>(value.size());
        if (!WriteValue(out, size))
            return false;

        if (size > 0)
            out.write(value.data(), size);

        return out.good();
    }

    bool ReadString(std::ifstream& in, std::string& value)
    {
        uint32_t size = 0;
        if (!ReadValue(in, size))
            return false;

        if (size > 1024 * 1024)
            return false;

        value.assign(size, '\0');
        if (size > 0)
            in.read(value.data(), size);

        return in.good();
    }

    bool FileExists(const std::string& path)
    {
        const DWORD attributes = GetFileAttributesA(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    void DisableDefaultDockRebuilds()
    {
        g_rebuildHomeDockLayout = false;
        g_rebuildKeyboardDockLayout = false;
        g_rebuildMouseDockLayout = false;
        g_rebuildProgramsDockLayout = false;
    }

    bool SaveAppSettings()
    {
        std::ofstream out(g_settingsFilePath, std::ios::binary);
        if (!out)
            return false;

        const char magic[8] = { 'K', 'N', 'S', 'E', 'T', '1', '\0', '\0' };
        out.write(magic, sizeof(magic));
        WriteString(out, g_dataFilePath);
        WriteString(out, g_imguiIniPath);
        const uint8_t autoSave = g_autoSaveEnabled ? 1 : 0;
        WriteValue(out, autoSave);
        WriteValue(out, g_heatmapCellScale);
        WriteValue(out, g_cursorHeatRadiusPixels);
        return out.good();
    }

    bool LoadAppSettings()
    {
        std::ifstream in(g_settingsFilePath, std::ios::binary);
        if (!in)
            return false;

        char magic[8]{};
        in.read(magic, sizeof(magic));
        const char expected[8] = { 'K', 'N', 'S', 'E', 'T', '1', '\0', '\0' };
        if (memcmp(magic, expected, sizeof(expected)) != 0)
            return false;

        std::string dataPath;
        std::string iniPath;
        uint8_t autoSave = 1;
        float heatmapCellScale = g_heatmapCellScale;
        int cursorHeatRadiusPixels = g_cursorHeatRadiusPixels;
        if (!ReadString(in, dataPath) || !ReadString(in, iniPath) || !ReadValue(in, autoSave))
            return false;

        ReadValue(in, heatmapCellScale);
        ReadValue(in, cursorHeatRadiusPixels);

        if (!dataPath.empty())
            g_dataFilePath = dataPath;
        if (!iniPath.empty())
            g_imguiIniPath = iniPath;
        g_autoSaveEnabled = autoSave != 0;
        g_heatmapCellScale = std::clamp(heatmapCellScale, 0.005f, 0.08f);
        g_cursorHeatRadiusPixels = std::clamp(cursorHeatRadiusPixels, 0, 250);
        return true;
    }

    bool SaveSnapshotToFile(const std::string& path)
    {
        std::ofstream out(path, std::ios::binary);
        if (!out)
        {
            g_saveLoadStatus = "Save failed: unable to open file.";
            return false;
        }

        const char magic[8] = { 'K', 'N', 'A', 'L', 'Y', 'S', '4', '\0' };
        out.write(magic, sizeof(magic));

        const double runtimeSeconds = NowSeconds();
        WriteValue(out, runtimeSeconds);
        WriteValue(out, g_heatmapCellScale);
        WriteValue(out, g_cursorHeatRadiusPixels);
        WriteValue(out, g_mouseDelta.x);
        WriteValue(out, g_mouseDelta.y);
        WriteValue(out, g_wheelDelta);
        WriteValue(out, g_leftClicks);
        WriteValue(out, g_rightClicks);
        WriteValue(out, g_middleClicks);

        const uint32_t inputCount = static_cast<uint32_t>(g_inputs.size());
        WriteValue(out, inputCount);
        for (const InputStats& input : g_inputs)
        {
            WriteString(out, input.id);
            WriteString(out, input.device);
            WriteString(out, input.label);
            WriteValue(out, input.total);
            WriteValue(out, input.downTotal);
            WriteValue(out, input.upTotal);
            WriteValue(out, input.lastTimeSeconds);
            WriteString(out, input.lastAppName);

            const uint32_t appCount = static_cast<uint32_t>(input.appTotals.size());
            WriteValue(out, appCount);
            for (const AppInputStats& app : input.appTotals)
            {
                WriteString(out, app.appName);
                WriteValue(out, app.processId);
                WriteValue(out, app.total);
                WriteValue(out, app.downTotal);
                WriteValue(out, app.upTotal);
            }
        }

        const uint32_t monitorCount = static_cast<uint32_t>(g_monitors.size());
        WriteValue(out, monitorCount);
        for (const MonitorHeatmap& monitor : g_monitors)
        {
            WriteValue(out, monitor.rect.left);
            WriteValue(out, monitor.rect.top);
            WriteValue(out, monitor.rect.right);
            WriteValue(out, monitor.rect.bottom);
            WriteString(out, monitor.name);
            WriteValue(out, monitor.columns);
            WriteValue(out, monitor.rows);
            WriteValue(out, monitor.maxBin);
            out.write(reinterpret_cast<const char*>(monitor.bins.data()), static_cast<std::streamsize>(monitor.bins.size() * sizeof(uint32_t)));

            const uint32_t programHeatmapCount = static_cast<uint32_t>(monitor.programHeatmaps.size());
            WriteValue(out, programHeatmapCount);
            for (const ProgramHeatmap& programHeatmap : monitor.programHeatmaps)
            {
                WriteString(out, programHeatmap.appName);
                WriteValue(out, programHeatmap.processId);
                WriteValue(out, programHeatmap.maxBin);
                out.write(reinterpret_cast<const char*>(programHeatmap.bins.data()), static_cast<std::streamsize>(programHeatmap.bins.size() * sizeof(uint32_t)));
            }
        }

        if (!out.good())
        {
            g_saveLoadStatus = "Save failed: write error.";
            return false;
        }

        g_dataFilePath = path;
        g_lastAutoSaveTime = std::chrono::steady_clock::now();
        g_saveLoadStatus = "Saved: " + path;
        SaveAppSettings();
        return true;
    }

    bool LoadSnapshotFromFile(const std::string& path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            g_saveLoadStatus = "Load failed: unable to open file.";
            return false;
        }

        char magic[8]{};
        in.read(magic, sizeof(magic));
        const char expectedV1[8] = { 'K', 'N', 'A', 'L', 'Y', 'S', '1', '\0' };
        const char expectedV2[8] = { 'K', 'N', 'A', 'L', 'Y', 'S', '2', '\0' };
        const char expectedV3[8] = { 'K', 'N', 'A', 'L', 'Y', 'S', '3', '\0' };
        const char expectedV4[8] = { 'K', 'N', 'A', 'L', 'Y', 'S', '4', '\0' };
        const bool isV1 = memcmp(magic, expectedV1, sizeof(expectedV1)) == 0;
        const bool isV2 = memcmp(magic, expectedV2, sizeof(expectedV2)) == 0;
        const bool isV3 = memcmp(magic, expectedV3, sizeof(expectedV3)) == 0;
        const bool isV4 = memcmp(magic, expectedV4, sizeof(expectedV4)) == 0;
        if (!isV1 && !isV2 && !isV3 && !isV4)
        {
            g_saveLoadStatus = "Load failed: invalid file format.";
            return false;
        }

        std::vector<InputStats> loadedInputs;
        std::vector<MonitorHeatmap> loadedMonitors;
        POINT loadedMouseDelta{};
        int loadedWheelDelta = 0;
        int loadedLeftClicks = 0;
        int loadedRightClicks = 0;
        int loadedMiddleClicks = 0;
        float loadedHeatmapCellScale = g_heatmapCellScale;
        int loadedCursorHeatRadiusPixels = g_cursorHeatRadiusPixels;
        double loadedRuntimeSeconds = 0.0;

        if (isV4)
        {
            if (!ReadValue(in, loadedRuntimeSeconds))
            {
                g_saveLoadStatus = "Load failed: invalid runtime data.";
                return false;
            }
        }

        if (isV3 || isV4)
        {
            if (!ReadValue(in, loadedHeatmapCellScale) ||
                !ReadValue(in, loadedCursorHeatRadiusPixels))
            {
                g_saveLoadStatus = "Load failed: invalid heatmap settings.";
                return false;
            }
        }

        if (!ReadValue(in, loadedMouseDelta.x) ||
            !ReadValue(in, loadedMouseDelta.y) ||
            !ReadValue(in, loadedWheelDelta) ||
            !ReadValue(in, loadedLeftClicks) ||
            !ReadValue(in, loadedRightClicks) ||
            !ReadValue(in, loadedMiddleClicks))
        {
            g_saveLoadStatus = "Load failed: truncated file.";
            return false;
        }

        uint32_t inputCount = 0;
        if (!ReadValue(in, inputCount) || inputCount > 100000)
        {
            g_saveLoadStatus = "Load failed: invalid input count.";
            return false;
        }

        loadedInputs.reserve(inputCount);
        for (uint32_t i = 0; i < inputCount; ++i)
        {
            InputStats input;
            if (!ReadString(in, input.id) ||
                !ReadString(in, input.device) ||
                !ReadString(in, input.label) ||
                !ReadValue(in, input.total) ||
                !ReadValue(in, input.downTotal) ||
                !ReadValue(in, input.upTotal) ||
                !ReadValue(in, input.lastTimeSeconds) ||
                !ReadString(in, input.lastAppName))
            {
                g_saveLoadStatus = "Load failed: invalid input data.";
                return false;
            }

            uint32_t appCount = 0;
            if (!ReadValue(in, appCount) || appCount > 100000)
            {
                g_saveLoadStatus = "Load failed: invalid app count.";
                return false;
            }

            input.appTotals.reserve(appCount);
            for (uint32_t appIndex = 0; appIndex < appCount; ++appIndex)
            {
                AppInputStats app;
                if (!ReadString(in, app.appName) ||
                    !ReadValue(in, app.processId) ||
                    !ReadValue(in, app.total) ||
                    !ReadValue(in, app.downTotal) ||
                    !ReadValue(in, app.upTotal))
                {
                    g_saveLoadStatus = "Load failed: invalid app data.";
                    return false;
                }

                input.appTotals.push_back(std::move(app));
            }

            loadedInputs.push_back(std::move(input));
        }

        if (!isV4)
        {
            for (const InputStats& input : loadedInputs)
                loadedRuntimeSeconds = std::max(loadedRuntimeSeconds, input.lastTimeSeconds);
        }

        uint32_t monitorCount = 0;
        if (!ReadValue(in, monitorCount) || monitorCount > 128)
        {
            g_saveLoadStatus = "Load failed: invalid monitor count.";
            return false;
        }

        loadedMonitors.reserve(monitorCount);
        for (uint32_t i = 0; i < monitorCount; ++i)
        {
            MonitorHeatmap monitor;
            if (!ReadValue(in, monitor.rect.left) ||
                !ReadValue(in, monitor.rect.top) ||
                !ReadValue(in, monitor.rect.right) ||
                !ReadValue(in, monitor.rect.bottom) ||
                !ReadString(in, monitor.name))
            {
                g_saveLoadStatus = "Load failed: invalid monitor data.";
                return false;
            }

            if (isV3 || isV4)
            {
                if (!ReadValue(in, monitor.columns) ||
                    !ReadValue(in, monitor.rows) ||
                    !ReadValue(in, monitor.maxBin))
                {
                    g_saveLoadStatus = "Load failed: invalid monitor grid data.";
                    return false;
                }
            }
            else
            {
                monitor.columns = LEGACY_HEATMAP_COLUMNS;
                monitor.rows = LEGACY_HEATMAP_ROWS;
                if (!ReadValue(in, monitor.maxBin))
                {
                    g_saveLoadStatus = "Load failed: invalid legacy monitor data.";
                    return false;
                }
            }

            if (monitor.columns <= 0 || monitor.rows <= 0 || monitor.columns > 512 || monitor.rows > 512)
            {
                g_saveLoadStatus = "Load failed: invalid heatmap dimensions.";
                return false;
            }

            monitor.bins.assign(static_cast<size_t>(monitor.columns * monitor.rows), 0);
            in.read(reinterpret_cast<char*>(monitor.bins.data()), static_cast<std::streamsize>(monitor.bins.size() * sizeof(uint32_t)));
            if (!in.good())
            {
                g_saveLoadStatus = "Load failed: invalid heatmap data.";
                return false;
            }

            if (isV2 || isV3 || isV4)
            {
                uint32_t programHeatmapCount = 0;
                if (!ReadValue(in, programHeatmapCount) || programHeatmapCount > 100000)
                {
                    g_saveLoadStatus = "Load failed: invalid program heatmap count.";
                    return false;
                }

                monitor.programHeatmaps.reserve(programHeatmapCount);
                for (uint32_t programIndex = 0; programIndex < programHeatmapCount; ++programIndex)
                {
                    ProgramHeatmap programHeatmap;
                    if (!ReadString(in, programHeatmap.appName) ||
                        !ReadValue(in, programHeatmap.processId) ||
                        !ReadValue(in, programHeatmap.maxBin))
                    {
                        g_saveLoadStatus = "Load failed: invalid program heatmap data.";
                        return false;
                    }

                    programHeatmap.bins.assign(static_cast<size_t>(monitor.columns * monitor.rows), 0);
                    in.read(reinterpret_cast<char*>(programHeatmap.bins.data()), static_cast<std::streamsize>(programHeatmap.bins.size() * sizeof(uint32_t)));
                    if (!in.good())
                    {
                        g_saveLoadStatus = "Load failed: invalid program heatmap bins.";
                        return false;
                    }

                    monitor.programHeatmaps.push_back(std::move(programHeatmap));
                }
            }

            loadedMonitors.push_back(std::move(monitor));
        }

        g_inputs = std::move(loadedInputs);
        g_monitors = std::move(loadedMonitors);
        g_mouseDelta = loadedMouseDelta;
        g_wheelDelta = loadedWheelDelta;
        g_leftClicks = loadedLeftClicks;
        g_rightClicks = loadedRightClicks;
        g_middleClicks = loadedMiddleClicks;
        g_heatmapCellScale = std::clamp(loadedHeatmapCellScale, 0.005f, 0.08f);
        g_cursorHeatRadiusPixels = std::clamp(loadedCursorHeatRadiusPixels, 0, 250);
        g_loadedRuntimeSeconds = std::max(0.0, loadedRuntimeSeconds);
        g_startTime = std::chrono::steady_clock::now();
        g_selectedInputId.clear();
        g_selectedProgramName.clear();
        g_selectedProgramPid = 0;
        g_keysDown.fill(false);
        g_activeCombosByKey.fill({});
        g_mouseDeltaSamples.clear();
        g_dataFilePath = path;
        g_lastAutoSaveTime = std::chrono::steady_clock::now();
        g_saveLoadStatus = "Loaded: " + path;
        SaveAppSettings();
        return true;
    }

    std::string ShowSnapshotFileDialog(bool save)
    {
        wchar_t fileName[MAX_PATH]{};
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = g_hwnd;
        ofn.lpstrFile = fileName;
        ofn.nMaxFile = static_cast<DWORD>(std::size(fileName));
        ofn.lpstrFilter = L"Keynalysis Data (*.kna)\0*.kna\0All Files (*.*)\0*.*\0";
        ofn.nFilterIndex = 1;
        ofn.lpstrDefExt = L"kna";
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

        if (save)
        {
            ofn.Flags |= OFN_OVERWRITEPROMPT;
            if (!GetSaveFileNameW(&ofn))
                return {};
        }
        else
        {
            ofn.Flags |= OFN_FILEMUSTEXIST;
            if (!GetOpenFileNameW(&ofn))
                return {};
        }

        return WideToUtf8(fileName);
    }

    void MaybeAutoSave()
    {
        if (!g_autoSaveEnabled || g_dataFilePath.empty())
            return;

        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - g_lastAutoSaveTime).count();
        if (elapsed >= AUTO_SAVE_SECONDS)
            SaveSnapshotToFile(g_dataFilePath);
    }

    void ClearInputData()
    {
        g_inputs.clear();
        g_selectedInputId.clear();
        g_keysDown.fill(false);
        g_activeCombosByKey.fill({});
        g_mouseDeltaSamples.clear();
        for (MonitorHeatmap& monitor : g_monitors)
        {
            monitor.bins.assign(static_cast<size_t>(monitor.columns * monitor.rows), 0);
            monitor.maxBin = 0;
            monitor.programHeatmaps.clear();
        }
        g_mouseDelta = {};
        g_wheelDelta = 0;
        g_leftClicks = 0;
        g_rightClicks = 0;
        g_middleClicks = 0;
        g_loadedRuntimeSeconds = 0.0;
        g_startTime = std::chrono::steady_clock::now();
    }

    void CreateRenderTarget()
    {
        ID3D11Texture2D* pBackBuffer = nullptr;
        g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
        if (pBackBuffer)
        {
            g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
            pBackBuffer->Release();
        }
    }

    void CleanupRenderTarget()
    {
        if (g_mainRenderTargetView)
        {
            g_mainRenderTargetView->Release();
            g_mainRenderTargetView = nullptr;
        }
    }

    bool CreateDeviceD3D(HWND hWnd)
    {
        DXGI_SWAP_CHAIN_DESC sd{};
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
        const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
        const HRESULT res = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            createDeviceFlags,
            featureLevelArray,
            2,
            D3D11_SDK_VERSION,
            &sd,
            &g_pSwapChain,
            &g_pd3dDevice,
            &featureLevel,
            &g_pd3dDeviceContext);

        if (res == DXGI_ERROR_UNSUPPORTED)
        {
            return SUCCEEDED(D3D11CreateDeviceAndSwapChain(
                nullptr,
                D3D_DRIVER_TYPE_WARP,
                nullptr,
                createDeviceFlags,
                featureLevelArray,
                2,
                D3D11_SDK_VERSION,
                &sd,
                &g_pSwapChain,
                &g_pd3dDevice,
                &featureLevel,
                &g_pd3dDeviceContext));
        }

        if (FAILED(res))
            return false;

        CreateRenderTarget();
        return true;
    }

    void CleanupDeviceD3D()
    {
        CleanupRenderTarget();
        if (g_pSwapChain)
        {
            g_pSwapChain->Release();
            g_pSwapChain = nullptr;
        }
        if (g_pd3dDeviceContext)
        {
            g_pd3dDeviceContext->Release();
            g_pd3dDeviceContext = nullptr;
        }
        if (g_pd3dDevice)
        {
            g_pd3dDevice->Release();
            g_pd3dDevice = nullptr;
        }
    }

    NOTIFYICONDATAW CreateTrayIconData(HWND hWnd)
    {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hWnd;
        nid.uID = TRAY_ICON_ID;
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        nid.uCallbackMessage = WM_TRAYICON;
        nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(nid.szTip, L"keynalysis");
        return nid;
    }

    void AddTrayIcon(HWND hWnd)
    {
        if (g_trayIconVisible)
            return;

        NOTIFYICONDATAW nid = CreateTrayIconData(hWnd);
        g_trayIconVisible = Shell_NotifyIconW(NIM_ADD, &nid) == TRUE;
    }

    void RemoveTrayIcon(HWND hWnd)
    {
        if (!g_trayIconVisible)
            return;

        NOTIFYICONDATAW nid = CreateTrayIconData(hWnd);
        Shell_NotifyIconW(NIM_DELETE, &nid);
        g_trayIconVisible = false;
    }

    void MinimizeToTray(HWND hWnd)
    {
        AddTrayIcon(hWnd);
        ShowWindow(hWnd, SW_HIDE);
    }

    void RestoreFromTray(HWND hWnd)
    {
        ShowWindow(hWnd, SW_SHOW);
        ShowWindow(hWnd, SW_RESTORE);
        SetForegroundWindow(hWnd);
        RemoveTrayIcon(hWnd);
    }

    bool RegisterRawInput(HWND hWnd)
    {
        RAWINPUTDEVICE devices[2]{};
        devices[0].usUsagePage = 0x01;
        devices[0].usUsage = 0x06;
        devices[0].dwFlags = RIDEV_INPUTSINK;
        devices[0].hwndTarget = hWnd;

        devices[1].usUsagePage = 0x01;
        devices[1].usUsage = 0x02;
        devices[1].dwFlags = RIDEV_INPUTSINK;
        devices[1].hwndTarget = hWnd;

        g_rawInputRegistered = RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE)) == TRUE;
        return g_rawInputRegistered;
    }

    void HandleRawKeyboard(const RAWKEYBOARD& keyboard)
    {
        const USHORT virtualKey = keyboard.VKey;
        if (virtualKey >= g_keysDown.size())
            return;

        const bool wasDown = g_keysDown[virtualKey];
        const bool isUp = (keyboard.Flags & RI_KEY_BREAK) != 0;
        const bool isDown = !isUp;
        g_keysDown[virtualKey] = isDown;

        const std::string key = KeyName(virtualKey, keyboard.MakeCode, keyboard.Flags);
        const std::string id = "key:" + std::to_string(virtualKey);
        if (isDown && !wasDown)
            AddInputEvent(id, "Keyboard", key, "Down", "Scan " + std::to_string(keyboard.MakeCode));
        else if (isUp && wasDown)
            AddInputEvent(id, "Keyboard", key, "Up", "Scan " + std::to_string(keyboard.MakeCode));

        if (!IsModifierKey(virtualKey))
        {
            if (isDown && !wasDown)
            {
                const std::string comboLabel = ComboLabelForKey(key);
                if (!comboLabel.empty())
                {
                    g_activeCombosByKey[virtualKey] = comboLabel;
                    AddComboEvent(comboLabel, "Down", "Primary key down: " + key);
                }
            }
            else if (isDown && virtualKey == VK_TAB && g_activeCombosByKey[virtualKey].empty() && IsAltDown())
            {
                const std::string comboLabel = ComboLabelForKey(key);
                if (!comboLabel.empty())
                {
                    g_activeCombosByKey[virtualKey] = comboLabel;
                    AddComboEvent(comboLabel, "Down", "System key down: " + key);
                }
            }
            else if (isUp && !g_activeCombosByKey[virtualKey].empty())
            {
                AddComboEvent(g_activeCombosByKey[virtualKey], "Up", "Primary key up: " + key);
                g_activeCombosByKey[virtualKey].clear();
            }
        }
        else if (isUp)
        {
            const bool ctrlReleased = (virtualKey == VK_CONTROL || virtualKey == VK_LCONTROL || virtualKey == VK_RCONTROL) && !IsCtrlDown();
            const bool altReleased = (virtualKey == VK_MENU || virtualKey == VK_LMENU || virtualKey == VK_RMENU) && !IsAltDown();
            const bool shiftReleased = (virtualKey == VK_SHIFT || virtualKey == VK_LSHIFT || virtualKey == VK_RSHIFT) && !IsShiftDown();

            for (std::string& comboLabel : g_activeCombosByKey)
            {
                if (comboLabel.empty())
                    continue;

                const bool shouldRelease =
                    (ctrlReleased && comboLabel.find("Ctrl+") != std::string::npos) ||
                    (altReleased && comboLabel.find("Alt+") != std::string::npos) ||
                    (shiftReleased && comboLabel.find("Shift+") != std::string::npos);

                if (shouldRelease)
                {
                    AddComboEvent(comboLabel, "Up", "Modifier released: " + key);
                    comboLabel.clear();
                }
            }
        }
    }

    void HandleMouseButton(USHORT flags, USHORT downFlag, USHORT upFlag, const char* name, int* counter)
    {
        const std::string id = std::string("mouse:") + name;

        if ((flags & downFlag) != 0)
        {
            ++(*counter);
            AddInputEvent(id, "Mouse", name, "Down", "Button pressed");
        }

        if ((flags & upFlag) != 0)
            AddInputEvent(id, "Mouse", name, "Up", "Button released");
    }

    void HandleRawMouse(const RAWMOUSE& mouse)
    {
        const FocusContext focus = GetFocusContext();
        AddCursorHeatSample(focus);

        if (mouse.lLastX != 0 || mouse.lLastY != 0)
        {
            g_mouseDelta.x += mouse.lLastX;
            g_mouseDelta.y += mouse.lLastY;
            AddMouseDeltaSample(mouse.lLastX, mouse.lLastY, focus);
            AddInputEvent("mouse:move", "Mouse", "Movement", "Move", "dx " + std::to_string(mouse.lLastX) + ", dy " + std::to_string(mouse.lLastY));
        }

        const USHORT flags = mouse.usButtonFlags;
        HandleMouseButton(flags, RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, "Left", &g_leftClicks);
        HandleMouseButton(flags, RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, "Right", &g_rightClicks);
        HandleMouseButton(flags, RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, "Middle", &g_middleClicks);

        if ((flags & RI_MOUSE_WHEEL) != 0)
        {
            const SHORT wheel = static_cast<SHORT>(mouse.usButtonData);
            g_wheelDelta += wheel;
            AddInputEvent("mouse:wheel", "Mouse", "Wheel", "Wheel", std::to_string(wheel));
        }
    }

    void HandleRawInput(HRAWINPUT hRawInput)
    {
        if (!g_captureEnabled)
            return;

        UINT size = 0;
        GetRawInputData(hRawInput, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
        if (size == 0)
            return;

        std::vector<BYTE> buffer(size);
        if (GetRawInputData(hRawInput, RID_INPUT, buffer.data(), &size, sizeof(RAWINPUTHEADER)) != size)
            return;

        const RAWINPUT* raw = reinterpret_cast<const RAWINPUT*>(buffer.data());
        if (raw->header.dwType == RIM_TYPEKEYBOARD)
            HandleRawKeyboard(raw->data.keyboard);
        else if (raw->header.dwType == RIM_TYPEMOUSE)
            HandleRawMouse(raw->data.mouse);
    }

    void DrawMouseDeltaGraph(const ImVec2& size)
    {
        ImGui::TextUnformatted("Mouse Delta");
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const ImVec2 canvasSize(
            std::max(1.0f, std::min(size.x, available.x)),
            std::max(1.0f, std::min(size.y, available.y)));

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("mouse-delta-graph", canvasSize);
        const ImVec2 end(origin.x + canvasSize.x, origin.y + canvasSize.y);

        drawList->AddRectFilled(origin, end, IM_COL32(20, 23, 28, 255));
        drawList->AddRect(origin, end, IM_COL32(80, 88, 98, 255));

        const float midY = origin.y + canvasSize.y * 0.5f;
        drawList->AddLine(ImVec2(origin.x, midY), ImVec2(end.x, midY), IM_COL32(70, 76, 84, 255));

        std::vector<const MouseDeltaSample*> samples;
        for (const MouseDeltaSample& sample : g_mouseDeltaSamples)
        {
            if (MatchesVisualProgramFilter(sample.appName, sample.processId))
                samples.push_back(&sample);
        }

        if (samples.size() < 2)
        {
            drawList->AddText(ImVec2(origin.x + 8.0f, origin.y + 8.0f), IM_COL32(160, 166, 176, 255), "Move the mouse to populate delta data");
            return;
        }

        float maxAbs = 1.0f;
        for (const MouseDeltaSample* sample : samples)
        {
            maxAbs = std::max(maxAbs, std::abs(sample->dx));
            maxAbs = std::max(maxAbs, std::abs(sample->dy));
        }

        auto pointForSample = [&](size_t index, float value) {
            const float t = static_cast<float>(index) / static_cast<float>(samples.size() - 1);
            const float x = origin.x + t * canvasSize.x;
            const float y = midY - (value / maxAbs) * (canvasSize.y * 0.45f);
            return ImVec2(x, y);
        };

        for (size_t i = 1; i < samples.size(); ++i)
        {
            drawList->AddLine(pointForSample(i - 1, samples[i - 1]->dx), pointForSample(i, samples[i]->dx), IM_COL32(92, 180, 255, 255), 1.5f);
            drawList->AddLine(pointForSample(i - 1, samples[i - 1]->dy), pointForSample(i, samples[i]->dy), IM_COL32(255, 174, 92, 255), 1.5f);
        }

        drawList->AddText(ImVec2(origin.x + 8.0f, origin.y + 8.0f), IM_COL32(92, 180, 255, 255), "dx");
        drawList->AddText(ImVec2(origin.x + 38.0f, origin.y + 8.0f), IM_COL32(255, 174, 92, 255), "dy");
    }

    ImU32 HeatColor(float value)
    {
        value = std::clamp(value, 0.0f, 1.0f);
        const int alpha = static_cast<int>(35.0f + value * 220.0f);
        const int red = static_cast<int>(55.0f + value * 200.0f);
        const int green = static_cast<int>(95.0f + value * 110.0f);
        const int blue = static_cast<int>(135.0f - value * 80.0f);
        return IM_COL32(red, green, blue, alpha);
    }

    void DrawCursorHeatmap(const ImVec2& size)
    {
        ImGui::TextUnformatted("Cursor Heatmap");
        if (g_monitors.empty())
            RefreshMonitors();

        float pendingCellScale = g_heatmapCellScale;
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::SliderFloat("Cell scale", &pendingCellScale, 0.005f, 0.08f, "%.3f"))
        {
            g_heatmapCellScale = pendingCellScale;
            RefreshMonitors();
            SaveAppSettings();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::SliderInt("Radius px", &g_cursorHeatRadiusPixels, 0, 250))
            SaveAppSettings();

        if (!g_monitors.empty())
        {
            std::vector<const char*> monitorNames;
            monitorNames.reserve(g_monitors.size());
            for (const MonitorHeatmap& monitor : g_monitors)
                monitorNames.push_back(monitor.name.c_str());

            ImGui::SetNextItemWidth(std::min(320.0f, size.x));
            ImGui::Combo("Monitor", &g_selectedMonitorIndex, monitorNames.data(), static_cast<int>(monitorNames.size()));
            ImGui::SameLine();
            if (ImGui::Button("Refresh"))
                RefreshMonitors();
        }

        const ImVec2 available = ImGui::GetContentRegionAvail();
        const ImVec2 canvasSize(
            std::max(1.0f, std::min(size.x, available.x)),
            std::max(1.0f, std::min(size.y, available.y)));

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("cursor-heatmap", canvasSize);
        const ImVec2 end(origin.x + canvasSize.x, origin.y + canvasSize.y);

        drawList->AddRectFilled(origin, end, IM_COL32(20, 23, 28, 255));
        drawList->AddRect(origin, end, IM_COL32(80, 88, 98, 255));

        if (g_monitors.empty())
        {
            drawList->AddText(ImVec2(origin.x + 8.0f, origin.y + 8.0f), IM_COL32(160, 166, 176, 255), "No monitors detected");
            return;
        }

        g_selectedMonitorIndex = std::clamp(g_selectedMonitorIndex, 0, static_cast<int>(g_monitors.size()) - 1);
        const MonitorHeatmap& heatmap = g_monitors[g_selectedMonitorIndex];
        const std::vector<uint32_t>* bins = &heatmap.bins;
        uint32_t maxBin = heatmap.maxBin;
        if (g_visualFilterProgramPid != 0)
        {
            bins = nullptr;
            maxBin = 0;
            for (const ProgramHeatmap& programHeatmap : heatmap.programHeatmaps)
            {
                if (MatchesVisualProgramFilter(programHeatmap.appName, programHeatmap.processId))
                {
                    bins = &programHeatmap.bins;
                    maxBin = programHeatmap.maxBin;
                    break;
                }
            }
        }

        const int columns = std::max(1, heatmap.columns);
        const int rows = std::max(1, heatmap.rows);
        const float cellW = canvasSize.x / static_cast<float>(columns);
        const float cellH = canvasSize.y / static_cast<float>(rows);

        if (bins)
        {
            for (int y = 0; y < rows; ++y)
            {
                for (int x = 0; x < columns; ++x)
                {
                    const size_t binIndex = static_cast<size_t>(y * columns + x);
                    if (binIndex >= bins->size())
                        continue;

                    const uint32_t count = (*bins)[binIndex];
                    if (count == 0)
                        continue;

                    const float value = maxBin > 0 ? static_cast<float>(count) / static_cast<float>(maxBin) : 0.0f;
                    const ImVec2 cellMin(origin.x + x * cellW, origin.y + y * cellH);
                    const ImVec2 cellMax(origin.x + (x + 1) * cellW, origin.y + (y + 1) * cellH);
                    drawList->AddRectFilled(cellMin, cellMax, HeatColor(value));
                }
            }
        }

        for (int x = 1; x < columns; ++x)
        {
            const float gx = origin.x + x * cellW;
            drawList->AddLine(ImVec2(gx, origin.y), ImVec2(gx, end.y), IM_COL32(45, 50, 58, 75));
        }
        for (int y = 1; y < rows; ++y)
        {
            const float gy = origin.y + y * cellH;
            drawList->AddLine(ImVec2(origin.x, gy), ImVec2(end.x, gy), IM_COL32(45, 50, 58, 75));
        }

        char label[128]{};
        snprintf(label, sizeof(label), "max cell: %u", maxBin);
        drawList->AddText(ImVec2(origin.x + 8.0f, origin.y + 8.0f), IM_COL32(220, 224, 230, 255), label);
    }

    bool MatchesDeviceFilter(const InputStats& input, const char* deviceFilter)
    {
        if (deviceFilter == nullptr)
            return true;

        const std::string filter(deviceFilter);
        if (filter == "Keyboard")
            return input.device == "Keyboard" || input.device == "Keyboard Combo";

        return input.device == filter;
    }

    bool SortAscending(const ImGuiTableSortSpecs* sortSpecs)
    {
        return !sortSpecs || sortSpecs->SpecsCount == 0 || sortSpecs->Specs[0].SortDirection != ImGuiSortDirection_Descending;
    }

    int SortColumn(const ImGuiTableSortSpecs* sortSpecs, int fallbackColumn = 0)
    {
        return sortSpecs && sortSpecs->SpecsCount > 0 ? sortSpecs->Specs[0].ColumnIndex : fallbackColumn;
    }

    template <typename T, typename Compare>
    void SortWithDirection(std::vector<T>& items, bool ascending, Compare compare)
    {
        std::sort(items.begin(), items.end(), [&](const T& a, const T& b) {
            return ascending ? compare(a, b) : compare(b, a);
        });
    }

    uint64_t TotalInvocations(const char* deviceFilter = nullptr)
    {
        uint64_t total = 0;
        for (const InputStats& input : g_inputs)
        {
            if (MatchesDeviceFilter(input, deviceFilter))
                total += input.total;
        }

        return total;
    }

    int TrackedInputs(const char* deviceFilter = nullptr)
    {
        int total = 0;
        for (const InputStats& input : g_inputs)
        {
            if (MatchesDeviceFilter(input, deviceFilter))
                ++total;
        }

        return total;
    }

    uint64_t TotalInvocationsExactDevice(const char* device)
    {
        uint64_t total = 0;
        for (const InputStats& input : g_inputs)
        {
            if (input.device == device)
                total += input.total;
        }

        return total;
    }

    int CountTrackedPrograms()
    {
        struct TrackedProgram
        {
            std::string appName;
            DWORD processId = 0;
        };

        std::vector<TrackedProgram> programs;
        for (const InputStats& input : g_inputs)
        {
            for (const AppInputStats& app : input.appTotals)
            {
                const bool exists = std::any_of(programs.begin(), programs.end(), [&](const TrackedProgram& program) {
                    return program.processId == app.processId && program.appName == app.appName;
                });
                if (!exists)
                    programs.push_back({ app.appName, app.processId });
            }
        }

        return static_cast<int>(programs.size());
    }

    std::string FormatDuration(double seconds)
    {
        uint64_t totalSeconds = static_cast<uint64_t>(std::max(0.0, seconds));
        const uint64_t hours = totalSeconds / 3600;
        totalSeconds %= 3600;
        const uint64_t minutes = totalSeconds / 60;
        const uint64_t secs = totalSeconds % 60;

        char buffer[64]{};
        snprintf(buffer, sizeof(buffer), "%lluh %02llum %02llus",
            static_cast<unsigned long long>(hours),
            static_cast<unsigned long long>(minutes),
            static_cast<unsigned long long>(secs));
        return buffer;
    }

    void DrawGlobalSummary()
    {
        ImGui::SeparatorText("Global Summary");
        ImGui::Text("Total inputs: %llu |", static_cast<unsigned long long>(TotalInvocations()));
        ImGui::SameLine();
        ImGui::Text("Keyboard: %llu |", static_cast<unsigned long long>(TotalInvocationsExactDevice("Keyboard")));
        ImGui::SameLine();
        ImGui::Text("Combos: %llu |", static_cast<unsigned long long>(TotalInvocationsExactDevice("Keyboard Combo")));
        ImGui::SameLine();
        ImGui::Text("Mouse: %llu |", static_cast<unsigned long long>(TotalInvocationsExactDevice("Mouse")));
        ImGui::SameLine();
        ImGui::Text("Programs: %d |", CountTrackedPrograms());
        ImGui::SameLine();
        ImGui::Text("File time: %s |", FormatDuration(NowSeconds()).c_str());
    }

    void DrawToolbar()
    {
        if (ImGui::BeginMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("Save"))
                    SaveSnapshotToFile(g_dataFilePath);
                if (ImGui::MenuItem("Save As..."))
                {
                    const std::string path = ShowSnapshotFileDialog(true);
                    if (!path.empty())
                        SaveSnapshotToFile(path);
                }
                if (ImGui::MenuItem("Load..."))
                {
                    const std::string path = ShowSnapshotFileDialog(false);
                    if (!path.empty())
                        LoadSnapshotFromFile(path);
                }
                ImGui::Separator();
                const bool previousAutoSave = g_autoSaveEnabled;
                ImGui::MenuItem("Auto save", nullptr, &g_autoSaveEnabled);
                if (previousAutoSave != g_autoSaveEnabled)
                    SaveAppSettings();
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Options"))
            {
                ImGui::MenuItem("Capture input", nullptr, &g_captureEnabled);
                if (ImGui::MenuItem("Clear data"))
                    ClearInputData();
                if (ImGui::MenuItem("Minimize to tray"))
                    ShowWindow(g_hwnd, SW_MINIMIZE);
                ImGui::EndMenu();
            }

            ImGui::EndMenuBar();
        }

        if (!g_rawInputRegistered)
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "Raw Input registration failed.");
        else
            ImGui::TextColored(ImVec4(0.25f, 0.8f, 0.45f, 1.0f), "Raw Input is active. Events continue while unfocused or minimized.");

        if (!g_saveLoadStatus.empty())
            ImGui::TextUnformatted(g_saveLoadStatus.c_str());
    }

    void DrawOverviewStats()
    {
        int keysDown = 0;
        for (const bool down : g_keysDown)
            keysDown += down ? 1 : 0;

        ImGui::Text("Keys down: %d", keysDown);
        ImGui::Text("Tracked inputs: %d", static_cast<int>(g_inputs.size()));
        ImGui::Text("Total input invocations: %llu", static_cast<unsigned long long>(TotalInvocations()));
        ImGui::Text("Mouse delta total: x %ld, y %ld", g_mouseDelta.x, g_mouseDelta.y);
        ImGui::Text("Wheel total: %d", g_wheelDelta);
        ImGui::Text("Clicks: left %d, right %d, middle %d", g_leftClicks, g_rightClicks, g_middleClicks);
    }

    struct ProgramFilterOption
    {
        std::string appName;
        DWORD processId = 0;
    };

    std::vector<ProgramFilterOption> BuildProgramFilterOptions()
    {
        std::vector<ProgramFilterOption> options;
        for (const InputStats& input : g_inputs)
        {
            for (const AppInputStats& app : input.appTotals)
            {
                const bool exists = std::any_of(options.begin(), options.end(), [&](const ProgramFilterOption& option) {
                    return option.processId == app.processId && option.appName == app.appName;
                });

                if (!exists)
                    options.push_back({ app.appName, app.processId });
            }
        }

        std::sort(options.begin(), options.end(), [](const ProgramFilterOption& a, const ProgramFilterOption& b) {
            if (a.appName == b.appName)
                return a.processId < b.processId;
            return a.appName < b.appName;
        });
        return options;
    }

    void DrawProgramFilterControl(const char* label)
    {
        std::string preview = "All programs";
        if (g_visualFilterProgramPid != 0)
            preview = g_visualFilterProgramName + " (" + std::to_string(g_visualFilterProgramPid) + ")";

        ImGui::SetNextItemWidth(280.0f);
        if (ImGui::BeginCombo(label, preview.c_str()))
        {
            const bool allSelected = g_visualFilterProgramPid == 0;
            if (ImGui::Selectable("All programs", allSelected))
            {
                g_visualFilterProgramPid = 0;
                g_visualFilterProgramName.clear();
            }
            if (allSelected)
                ImGui::SetItemDefaultFocus();

            for (const ProgramFilterOption& option : BuildProgramFilterOptions())
            {
                const bool selected = option.processId == g_visualFilterProgramPid && option.appName == g_visualFilterProgramName;
                const std::string item = option.appName + " (" + std::to_string(option.processId) + ")";
                if (ImGui::Selectable(item.c_str(), selected))
                {
                    g_visualFilterProgramPid = option.processId;
                    g_visualFilterProgramName = option.appName;
                }
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }

            ImGui::EndCombo();
        }
    }

    void DrawVerticalSplitter(const char* id, float& leftWidth, float totalWidth, float minLeft, float minRight)
    {
        const float thickness = 6.0f;
        ImGui::SameLine();
        ImGui::InvisibleButton(id, ImVec2(thickness, -1.0f));
        if (ImGui::IsItemActive())
            leftWidth += ImGui::GetIO().MouseDelta.x;

        leftWidth = std::clamp(leftWidth, minLeft, std::max(minLeft, totalWidth - minRight - thickness));

        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        ImU32 color = ImGui::IsItemHovered() || ImGui::IsItemActive()
            ? IM_COL32(110, 125, 145, 255)
            : IM_COL32(62, 68, 76, 255);
        ImGui::GetWindowDrawList()->AddRectFilled(min, max, color);
        ImGui::SameLine();
    }

    void DrawHorizontalSplitter(const char* id, float& topHeight, float totalHeight, float minTop, float minBottom)
    {
        const float thickness = 6.0f;
        ImGui::InvisibleButton(id, ImVec2(-1.0f, thickness));
        if (ImGui::IsItemActive())
            topHeight += ImGui::GetIO().MouseDelta.y;

        topHeight = std::clamp(topHeight, minTop, std::max(minTop, totalHeight - minBottom - thickness));

        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        ImU32 color = ImGui::IsItemHovered() || ImGui::IsItemActive()
            ? IM_COL32(110, 125, 145, 255)
            : IM_COL32(62, 68, 76, 255);
        ImGui::GetWindowDrawList()->AddRectFilled(min, max, color);
    }

    void DrawInputSummaryTable(const char* tableId, const char* deviceFilter, float height)
    {
        constexpr ImGuiTableFlags tableFlags =
            ImGuiTableFlags_Borders |
            ImGuiTableFlags_RowBg |
            ImGuiTableFlags_Resizable |
            ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Sortable |
            ImGuiTableFlags_SortMulti;

        if (ImGui::BeginTable(tableId, 8, tableFlags, ImVec2(0, height)))
        {
            ImGui::TableSetupColumn("Device");
            ImGui::TableSetupColumn("Input");
            ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_DefaultSort);
            ImGui::TableSetupColumn("Down");
            ImGui::TableSetupColumn("Up");
            ImGui::TableSetupColumn("State");
            ImGui::TableSetupColumn("Last App");
            ImGui::TableSetupColumn("Last");
            ImGui::TableHeadersRow();

            std::vector<InputStats*> rows;
            for (InputStats& input : g_inputs)
            {
                if (MatchesDeviceFilter(input, deviceFilter))
                    rows.push_back(&input);
            }

            const ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
            const int column = SortColumn(sortSpecs, 2);
            const bool ascending = SortAscending(sortSpecs);
            SortWithDirection(rows, ascending, [&](const InputStats* a, const InputStats* b) {
                switch (column)
                {
                case 0: return a->device < b->device;
                case 1: return a->label < b->label;
                case 2: return a->total < b->total;
                case 3: return a->downTotal < b->downTotal;
                case 4: return a->upTotal < b->upTotal;
                case 5: return a->isDown < b->isDown;
                case 6: return a->lastAppName < b->lastAppName;
                case 7: return a->lastTimeSeconds < b->lastTimeSeconds;
                default: return a->total < b->total;
                }
            });

            for (InputStats* row : rows)
            {
                InputStats& input = *row;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(input.device.c_str());
                ImGui::TableSetColumnIndex(1);
                const bool selected = input.id == g_selectedInputId;
                const std::string selectableLabel = input.label + "##" + tableId + input.id;
                if (ImGui::Selectable(selectableLabel.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns))
                    g_selectedInputId = input.id;
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%llu", static_cast<unsigned long long>(input.total));
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%llu", static_cast<unsigned long long>(input.downTotal));
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%llu", static_cast<unsigned long long>(input.upTotal));
                ImGui::TableSetColumnIndex(5);
                ImGui::TextUnformatted(input.isDown ? "Down" : "-");
                ImGui::TableSetColumnIndex(6);
                ImGui::TextUnformatted(input.lastAppName.c_str());
                ImGui::TableSetColumnIndex(7);
                ImGui::Text("%.3f", input.lastTimeSeconds);
            }
            ImGui::EndTable();
        }
    }

    void DrawCompactInputListTable(const char* tableId, const char* deviceFilter, float height)
    {
        constexpr ImGuiTableFlags tableFlags =
            ImGuiTableFlags_Borders |
            ImGuiTableFlags_RowBg |
            ImGuiTableFlags_Resizable |
            ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Sortable;

        if (ImGui::BeginTable(tableId, 3, tableFlags, ImVec2(0, height)))
        {
            ImGui::TableSetupColumn("Device");
            ImGui::TableSetupColumn("Input");
            ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_DefaultSort);
            ImGui::TableHeadersRow();

            std::vector<InputStats*> rows;
            for (InputStats& input : g_inputs)
            {
                if (MatchesDeviceFilter(input, deviceFilter))
                    rows.push_back(&input);
            }

            const ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
            const int column = SortColumn(sortSpecs, 2);
            const bool ascending = SortAscending(sortSpecs);
            SortWithDirection(rows, ascending, [&](const InputStats* a, const InputStats* b) {
                switch (column)
                {
                case 0: return a->device < b->device;
                case 1: return a->label < b->label;
                case 2: return a->total < b->total;
                default: return a->total < b->total;
                }
            });

            for (InputStats* row : rows)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(row->device.c_str());
                ImGui::TableSetColumnIndex(1);
                const bool selected = row->id == g_selectedInputId;
                const std::string selectableLabel = row->label + "##" + tableId + row->id;
                if (ImGui::Selectable(selectableLabel.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns))
                    g_selectedInputId = row->id;
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%llu", static_cast<unsigned long long>(row->total));
            }

            ImGui::EndTable();
        }
    }

    void DrawSelectedInputSummary()
    {
        InputStats* selectedInput = FindInputStats(g_selectedInputId);
        if (!selectedInput)
        {
            ImGui::TextUnformatted("Select an input to view its summary.");
            return;
        }

        ImGui::Text("Device: %s", selectedInput->device.c_str());
        ImGui::Text("Input: %s", selectedInput->label.c_str());
        ImGui::Text("Total: %llu", static_cast<unsigned long long>(selectedInput->total));
        ImGui::Text("Down: %llu", static_cast<unsigned long long>(selectedInput->downTotal));
        ImGui::Text("Up: %llu", static_cast<unsigned long long>(selectedInput->upTotal));
        ImGui::Text("State: %s", selectedInput->isDown ? "Down" : "-");
        ImGui::Text("Last app: %s", selectedInput->lastAppName.c_str());
        ImGui::Text("Last time: %.3f", selectedInput->lastTimeSeconds);
    }

    void DrawKeyButton(const char* label, int virtualKey, float widthUnits = 1.0f, float heightUnits = 1.0f, const std::string& widgetSuffix = {})
    {
        const float unitWidth = 42.0f;
        const float unitHeight = 34.0f;
        const float gap = 4.0f;
        const ImVec2 size(
            unitWidth * widthUnits + gap * std::max(0.0f, widthUnits - 1.0f),
            unitHeight * heightUnits + gap * std::max(0.0f, heightUnits - 1.0f));
        const std::string id = "key:" + std::to_string(virtualKey);
        InputStats* stats = FindInputStats(id);
        const bool selected = g_selectedInputId == id;
        const bool down = stats && stats->isDown;
        uint64_t displayTotal = stats ? stats->total : 0;
        uint64_t displayDown = stats ? stats->downTotal : 0;
        uint64_t displayUp = stats ? stats->upTotal : 0;
        if (stats && g_visualFilterProgramPid != 0)
        {
            displayTotal = 0;
            displayDown = 0;
            displayUp = 0;
            for (const AppInputStats& app : stats->appTotals)
            {
                if (MatchesVisualProgramFilter(app.appName, app.processId))
                {
                    displayTotal += app.total;
                    displayDown += app.downTotal;
                    displayUp += app.upTotal;
                }
            }
        }

        ImVec4 base = ImGui::GetStyleColorVec4(ImGuiCol_Button);
        ImVec4 hovered = ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered);
        ImVec4 active = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
        if (displayTotal > 0)
        {
            const float intensity = std::min(1.0f, static_cast<float>(displayTotal) / 25.0f);
            base = ImVec4(0.18f + intensity * 0.25f, 0.32f + intensity * 0.25f, 0.50f + intensity * 0.18f, 1.0f);
            hovered = ImVec4(base.x + 0.08f, base.y + 0.08f, base.z + 0.08f, 1.0f);
            active = ImVec4(0.25f, 0.55f, 0.82f, 1.0f);
        }
        if (down)
        {
            base = ImVec4(0.85f, 0.45f, 0.18f, 1.0f);
            hovered = ImVec4(0.95f, 0.55f, 0.24f, 1.0f);
            active = hovered;
        }
        if (selected)
        {
            base = ImVec4(0.30f, 0.62f, 0.90f, 1.0f);
            hovered = ImVec4(0.38f, 0.70f, 1.0f, 1.0f);
            active = hovered;
        }

        ImGui::PushStyleColor(ImGuiCol_Button, base);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hovered);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, active);

        const std::string buttonLabel = std::string(label) + "##diagram:" + id + ":" + widgetSuffix;
        if (ImGui::Button(buttonLabel.c_str(), size))
            g_selectedInputId = id;

        ImGui::PopStyleColor(3);

        if (stats && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\nTotal: %llu\nDown: %llu\nUp: %llu", label, static_cast<unsigned long long>(displayTotal), static_cast<unsigned long long>(displayDown), static_cast<unsigned long long>(displayUp));
    }

    void DrawKeyGap(float units = 1.0f)
    {
        ImGui::Dummy(ImVec2(42.0f * units, 34.0f));
    }

    void SameKeyboardRow()
    {
        ImGui::SameLine(0.0f, 4.0f);
    }

    void DrawKeyboardDiagram()
    {
        ImGui::TextUnformatted("100% QWERTY Layout");
        ImGui::TextUnformatted("Blue intensity reflects total input count. Orange indicates currently down.");
        DrawProgramFilterControl("Program Filter##keyboard-diagram");
        ImGui::Separator();

        ImGui::BeginChild("keyboard-diagram-scroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);

        constexpr float keyW = 42.0f;
        constexpr float keyH = 34.0f;
        constexpr float gap = 4.0f;
        const float stepX = keyW + gap;
        const float stepY = keyH + gap;
        const ImVec2 start = ImGui::GetCursorPos();

        auto place = [&](float col, float row, const char* label, int vk, float width = 1.0f, float height = 1.0f) {
            ImGui::SetCursorPos(ImVec2(start.x + col * stepX, start.y + row * stepY));
            char suffix[64]{};
            snprintf(suffix, sizeof(suffix), "%.2f:%.2f", col, row);
            DrawKeyButton(label, vk, width, height, suffix);
        };

        place(0.0f, 0.0f, "Esc", VK_ESCAPE);
        place(2.0f, 0.0f, "F1", VK_F1); place(3.0f, 0.0f, "F2", VK_F2); place(4.0f, 0.0f, "F3", VK_F3); place(5.0f, 0.0f, "F4", VK_F4);
        place(6.5f, 0.0f, "F5", VK_F5); place(7.5f, 0.0f, "F6", VK_F6); place(8.5f, 0.0f, "F7", VK_F7); place(9.5f, 0.0f, "F8", VK_F8);
        place(11.0f, 0.0f, "F9", VK_F9); place(12.0f, 0.0f, "F10", VK_F10); place(13.0f, 0.0f, "F11", VK_F11); place(14.0f, 0.0f, "F12", VK_F12);
        place(15.5f, 0.0f, "Prt", VK_SNAPSHOT); place(16.5f, 0.0f, "Scr", VK_SCROLL); place(17.5f, 0.0f, "Pause", VK_PAUSE);

        place(0.0f, 1.4f, "`", VK_OEM_3); place(1.0f, 1.4f, "1", '1'); place(2.0f, 1.4f, "2", '2'); place(3.0f, 1.4f, "3", '3'); place(4.0f, 1.4f, "4", '4'); place(5.0f, 1.4f, "5", '5'); place(6.0f, 1.4f, "6", '6'); place(7.0f, 1.4f, "7", '7'); place(8.0f, 1.4f, "8", '8'); place(9.0f, 1.4f, "9", '9'); place(10.0f, 1.4f, "0", '0'); place(11.0f, 1.4f, "-", VK_OEM_MINUS); place(12.0f, 1.4f, "=", VK_OEM_PLUS); place(13.0f, 1.4f, "Backspace", VK_BACK, 2.0f);
        place(15.5f, 1.4f, "Ins", VK_INSERT); place(16.5f, 1.4f, "Home", VK_HOME); place(17.5f, 1.4f, "PgUp", VK_PRIOR);
        place(19.0f, 1.4f, "Num", VK_NUMLOCK); place(20.0f, 1.4f, "/", VK_DIVIDE); place(21.0f, 1.4f, "*", VK_MULTIPLY); place(22.0f, 1.4f, "-", VK_SUBTRACT);

        place(0.0f, 2.4f, "Tab", VK_TAB, 1.5f); place(1.5f, 2.4f, "Q", 'Q'); place(2.5f, 2.4f, "W", 'W'); place(3.5f, 2.4f, "E", 'E'); place(4.5f, 2.4f, "R", 'R'); place(5.5f, 2.4f, "T", 'T'); place(6.5f, 2.4f, "Y", 'Y'); place(7.5f, 2.4f, "U", 'U'); place(8.5f, 2.4f, "I", 'I'); place(9.5f, 2.4f, "O", 'O'); place(10.5f, 2.4f, "P", 'P'); place(11.5f, 2.4f, "[", VK_OEM_4); place(12.5f, 2.4f, "]", VK_OEM_6); place(13.5f, 2.4f, "\\", VK_OEM_5, 1.5f);
        place(15.5f, 2.4f, "Del", VK_DELETE); place(16.5f, 2.4f, "End", VK_END); place(17.5f, 2.4f, "PgDn", VK_NEXT);
        place(19.0f, 2.4f, "7", VK_NUMPAD7); place(20.0f, 2.4f, "8", VK_NUMPAD8); place(21.0f, 2.4f, "9", VK_NUMPAD9); place(22.0f, 2.4f, "+", VK_ADD, 1.0f, 2.0f);

        place(0.0f, 3.4f, "Caps", VK_CAPITAL, 1.75f); place(1.75f, 3.4f, "A", 'A'); place(2.75f, 3.4f, "S", 'S'); place(3.75f, 3.4f, "D", 'D'); place(4.75f, 3.4f, "F", 'F'); place(5.75f, 3.4f, "G", 'G'); place(6.75f, 3.4f, "H", 'H'); place(7.75f, 3.4f, "J", 'J'); place(8.75f, 3.4f, "K", 'K'); place(9.75f, 3.4f, "L", 'L'); place(10.75f, 3.4f, ";", VK_OEM_1); place(11.75f, 3.4f, "'", VK_OEM_7); place(12.75f, 3.4f, "Enter", VK_RETURN, 2.25f);
        place(19.0f, 3.4f, "4", VK_NUMPAD4); place(20.0f, 3.4f, "5", VK_NUMPAD5); place(21.0f, 3.4f, "6", VK_NUMPAD6);

        place(0.0f, 4.4f, "Shift", VK_SHIFT, 2.25f); place(2.25f, 4.4f, "Z", 'Z'); place(3.25f, 4.4f, "X", 'X'); place(4.25f, 4.4f, "C", 'C'); place(5.25f, 4.4f, "V", 'V'); place(6.25f, 4.4f, "B", 'B'); place(7.25f, 4.4f, "N", 'N'); place(8.25f, 4.4f, "M", 'M'); place(9.25f, 4.4f, ",", VK_OEM_COMMA); place(10.25f, 4.4f, ".", VK_OEM_PERIOD); place(11.25f, 4.4f, "/", VK_OEM_2); place(12.25f, 4.4f, "Shift", VK_SHIFT, 2.75f);
        place(16.5f, 4.4f, "Up", VK_UP);
        place(19.0f, 4.4f, "1", VK_NUMPAD1); place(20.0f, 4.4f, "2", VK_NUMPAD2); place(21.0f, 4.4f, "3", VK_NUMPAD3); place(22.0f, 4.4f, "Enter", VK_RETURN, 1.0f, 2.0f);

        place(0.0f, 5.4f, "Ctrl", VK_CONTROL, 1.5f); place(1.5f, 5.4f, "Alt", VK_MENU, 1.25f); place(2.75f, 5.4f, "Space", VK_SPACE, 8.25f); place(11.0f, 5.4f, "Alt", VK_MENU, 1.25f); place(12.25f, 5.4f, "Menu", VK_APPS, 1.25f); place(13.5f, 5.4f, "Ctrl", VK_CONTROL, 1.5f);
        place(15.5f, 5.4f, "Left", VK_LEFT); place(16.5f, 5.4f, "Down", VK_DOWN); place(17.5f, 5.4f, "Right", VK_RIGHT);
        place(19.0f, 5.4f, "0", VK_NUMPAD0, 2.0f); place(21.0f, 5.4f, ".", VK_DECIMAL);

        ImGui::SetCursorPos(ImVec2(start.x + 23.2f * stepX, start.y + 6.5f * stepY));
        ImGui::Dummy(ImVec2(1.0f, 1.0f));

        ImGui::EndChild();
    }

    void DrawDockPanel(const char* name, void (*drawPanel)())
    {
        if (ImGui::Begin(name))
            drawPanel();
        ImGui::End();
    }

    void DrawRecentHistoryTable(float height)
    {
        struct HistoryRow
        {
            const InputStats* input = nullptr;
            const InputEvent* event = nullptr;
        };

        std::vector<HistoryRow> rows;
        int mouseMoveRows = 0;
        for (const InputStats& input : g_inputs)
        {
            for (const InputEvent& event : input.events)
            {
                if (input.id == "mouse:move")
                {
                    if (mouseMoveRows >= g_historyMouseMoveLimit)
                        continue;
                    ++mouseMoveRows;
                }

                rows.push_back({ &input, &event });
            }
        }

        std::sort(rows.begin(), rows.end(), [](const HistoryRow& a, const HistoryRow& b) {
            return a.event->timeSeconds > b.event->timeSeconds;
        });

        if (rows.size() > 300)
            rows.resize(300);

        constexpr ImGuiTableFlags tableFlags =
            ImGuiTableFlags_Borders |
            ImGuiTableFlags_RowBg |
            ImGuiTableFlags_Resizable |
            ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Sortable;

        if (ImGui::BeginTable("home-history", 6, tableFlags, ImVec2(0, height)))
        {
            ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_DefaultSort);
            ImGui::TableSetupColumn("Device");
            ImGui::TableSetupColumn("Input");
            ImGui::TableSetupColumn("Event");
            ImGui::TableSetupColumn("Program");
            ImGui::TableSetupColumn("Detail");
            ImGui::TableHeadersRow();

            const ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
            const int column = SortColumn(sortSpecs, 0);
            const bool ascending = SortAscending(sortSpecs);
            SortWithDirection(rows, ascending, [&](const HistoryRow& a, const HistoryRow& b) {
                switch (column)
                {
                case 0: return a.event->timeSeconds < b.event->timeSeconds;
                case 1: return a.input->device < b.input->device;
                case 2: return a.input->label < b.input->label;
                case 3: return a.event->action < b.event->action;
                case 4: return a.event->appName < b.event->appName;
                case 5: return a.event->detail < b.event->detail;
                default: return a.event->timeSeconds < b.event->timeSeconds;
                }
            });

            for (const HistoryRow& row : rows)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%.3f", row.event->timeSeconds);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(row.input->device.c_str());
                ImGui::TableSetColumnIndex(2);
                const bool selected = row.input->id == g_selectedInputId;
                const std::string selectableLabel = row.input->label + "##history:" + row.input->id + ":" + std::to_string(row.event->timeSeconds);
                if (ImGui::Selectable(selectableLabel.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns))
                    g_selectedInputId = row.input->id;
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(row.event->action.c_str());
                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(row.event->appName.c_str());
                ImGui::TableSetColumnIndex(5);
                ImGui::TextUnformatted(row.event->detail.c_str());
            }

            ImGui::EndTable();
        }
    }

    void DrawSelectedInputDetails(float height)
    {
        InputStats* selectedInput = FindInputStats(g_selectedInputId);
        if (!selectedInput)
        {
            ImGui::TextUnformatted("Select an input row to view recent event details.");
            return;
        }

        ImGui::Text("Details: %s %s", selectedInput->device.c_str(), selectedInput->label.c_str());
        const float appTableHeight = std::max(90.0f, height * 0.35f);
        constexpr ImGuiTableFlags tableFlags =
            ImGuiTableFlags_Borders |
            ImGuiTableFlags_RowBg |
            ImGuiTableFlags_Resizable |
            ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Sortable;

        if (ImGui::BeginTable("selected-app-totals", 5, tableFlags, ImVec2(0, appTableHeight)))
        {
            ImGui::TableSetupColumn("Program");
            ImGui::TableSetupColumn("PID");
            ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_DefaultSort);
            ImGui::TableSetupColumn("Down");
            ImGui::TableSetupColumn("Up");
            ImGui::TableHeadersRow();

            std::vector<const AppInputStats*> rows;
            for (const AppInputStats& app : selectedInput->appTotals)
                rows.push_back(&app);

            const ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
            const int column = SortColumn(sortSpecs, 2);
            const bool ascending = SortAscending(sortSpecs);
            SortWithDirection(rows, ascending, [&](const AppInputStats* a, const AppInputStats* b) {
                switch (column)
                {
                case 0: return a->appName < b->appName;
                case 1: return a->processId < b->processId;
                case 2: return a->total < b->total;
                case 3: return a->downTotal < b->downTotal;
                case 4: return a->upTotal < b->upTotal;
                default: return a->total < b->total;
                }
            });

            for (const AppInputStats* row : rows)
            {
                const AppInputStats& app = *row;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(app.appName.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%lu", static_cast<unsigned long>(app.processId));
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%llu", static_cast<unsigned long long>(app.total));
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%llu", static_cast<unsigned long long>(app.downTotal));
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%llu", static_cast<unsigned long long>(app.upTotal));
            }
            ImGui::EndTable();
        }

        const float detailHeight = std::max(100.0f, ImGui::GetContentRegionAvail().y);
        if (ImGui::BeginTable("selected-input-details", 5, tableFlags, ImVec2(0, detailHeight)))
        {
            ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_DefaultSort);
            ImGui::TableSetupColumn("Event");
            ImGui::TableSetupColumn("Program");
            ImGui::TableSetupColumn("PID");
            ImGui::TableSetupColumn("Detail");
            ImGui::TableHeadersRow();

            std::vector<const InputEvent*> rows;
            for (const InputEvent& event : selectedInput->events)
                rows.push_back(&event);

            const ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
            const int detailColumn = SortColumn(sortSpecs, 0);
            const bool detailAscending = SortAscending(sortSpecs);
            SortWithDirection(rows, detailAscending, [&](const InputEvent* a, const InputEvent* b) {
                switch (detailColumn)
                {
                case 0: return a->timeSeconds < b->timeSeconds;
                case 1: return a->action < b->action;
                case 2: return a->appName < b->appName;
                case 3: return a->processId < b->processId;
                case 4: return a->detail < b->detail;
                default: return a->timeSeconds < b->timeSeconds;
                }
            });

            for (const InputEvent* row : rows)
            {
                const InputEvent& event = *row;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%.3f", event.timeSeconds);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(event.action.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(event.appName.c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%lu", static_cast<unsigned long>(event.processId));
                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(event.detail.c_str());
            }
            ImGui::EndTable();
        }
    }

    void DrawHomeTab()
    {
        if (ImGui::Button("Reset Home docking"))
            g_rebuildHomeDockLayout = true;

        const ImGuiID dockspaceId = ImGui::GetID("home-dockspace");
        const ImVec2 dockspaceSize = ImGui::GetContentRegionAvail();

        if (g_rebuildHomeDockLayout)
        {
            g_rebuildHomeDockLayout = false;

            ImGui::DockBuilderRemoveNode(dockspaceId);
            ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockspaceId, dockspaceSize);

            ImGuiID historyDockId = 0;
            ImGuiID rightDockId = 0;
            ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.42f, &historyDockId, &rightDockId);

            ImGuiID lowerRightDockId = 0;

            ImGuiID detailsDockId = 0;
            ImGuiID inputTotalsDockId = 0;
            ImGui::DockBuilderSplitNode(rightDockId, ImGuiDir_Down, 0.45f, &detailsDockId, &inputTotalsDockId);

            ImGui::DockBuilderDockWindow("Home / History", historyDockId);
            ImGui::DockBuilderDockWindow("Home / Input Totals", inputTotalsDockId);
            ImGui::DockBuilderDockWindow("Home / Details", detailsDockId);
            ImGui::DockBuilderFinish(dockspaceId);
        }

        ImGui::DockSpace(dockspaceId, dockspaceSize, ImGuiDockNodeFlags_None);

        if (ImGui::Begin("Home / History"))
        {
            ImGui::SetNextItemWidth(220.0f);
            ImGui::SliderInt("Mouse movement rows", &g_historyMouseMoveLimit, 0, 300);
            ImGui::Separator();
            DrawRecentHistoryTable(ImGui::GetContentRegionAvail().y);
        }
        ImGui::End();

        if (ImGui::Begin("Home / Input Totals"))
        {
            DrawOverviewStats();
            ImGui::Separator();
            DrawInputSummaryTable("home-input-summary", nullptr, ImGui::GetContentRegionAvail().y);
        }
        ImGui::End();

        if (ImGui::Begin("Home / Details"))
        {
            DrawSelectedInputDetails(ImGui::GetContentRegionAvail().y);
        }
        ImGui::End();
    }

    void DrawKeyboardTab()
    {
        if (ImGui::Button("Reset Keyboard docking"))
            g_rebuildKeyboardDockLayout = true;

        const ImGuiID dockspaceId = ImGui::GetID("keyboard-dockspace");
        const ImVec2 dockspaceSize = ImGui::GetContentRegionAvail();

        if (g_rebuildKeyboardDockLayout)
        {
            g_rebuildKeyboardDockLayout = false;

            ImGui::DockBuilderRemoveNode(dockspaceId);
            ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockspaceId, dockspaceSize);

            ImGuiID listDockId = 0;
            ImGuiID rightDockId = 0;
            ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.42f, &listDockId, &rightDockId);

            ImGuiID diagramDockId = 0;
            ImGuiID inputListDockId = 0;
            ImGui::DockBuilderSplitNode(listDockId, ImGuiDir_Up, 0.58f, &diagramDockId, &inputListDockId);

            ImGuiID summaryDockId = 0;
            ImGuiID detailsDockId = 0;
            ImGui::DockBuilderSplitNode(rightDockId, ImGuiDir_Up, 0.28f, &summaryDockId, &detailsDockId);

            ImGui::DockBuilderDockWindow("Keyboard / Diagram", diagramDockId);
            ImGui::DockBuilderDockWindow("Keyboard / Inputs", inputListDockId);
            ImGui::DockBuilderDockWindow("Keyboard / Selected Summary", summaryDockId);
            ImGui::DockBuilderDockWindow("Keyboard / Selected Details", detailsDockId);
            ImGui::DockBuilderFinish(dockspaceId);
        }

        ImGui::DockSpace(dockspaceId, dockspaceSize, ImGuiDockNodeFlags_None);

        if (ImGui::Begin("Keyboard / Diagram"))
        {
            DrawKeyboardDiagram();
        }
        ImGui::End();

        if (ImGui::Begin("Keyboard / Inputs"))
        {
            int keysDown = 0;
            for (const bool down : g_keysDown)
                keysDown += down ? 1 : 0;

            ImGui::Text("Keys down: %d", keysDown);
            ImGui::Text("Tracked: %d", TrackedInputs("Keyboard"));
            ImGui::Text("Invocations: %llu", static_cast<unsigned long long>(TotalInvocations("Keyboard")));
            ImGui::Separator();
            DrawCompactInputListTable("keyboard-compact-inputs", "Keyboard", ImGui::GetContentRegionAvail().y);
        }
        ImGui::End();

        if (ImGui::Begin("Keyboard / Selected Summary"))
        {
            DrawSelectedInputSummary();
        }
        ImGui::End();

        if (ImGui::Begin("Keyboard / Selected Details"))
        {
            DrawSelectedInputDetails(ImGui::GetContentRegionAvail().y);
        }
        ImGui::End();
    }

    void DrawMouseTab()
    {
        if (ImGui::Button("Reset Mouse docking"))
            g_rebuildMouseDockLayout = true;

        const ImGuiID dockspaceId = ImGui::GetID("mouse-dockspace");
        const ImVec2 dockspaceSize = ImGui::GetContentRegionAvail();

        if (g_rebuildMouseDockLayout)
        {
            g_rebuildMouseDockLayout = false;

            ImGui::DockBuilderRemoveNode(dockspaceId);
            ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockspaceId, dockspaceSize);

            ImGuiID leftDockId = 0;
            ImGuiID rightDockId = 0;
            ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.50f, &leftDockId, &rightDockId);

            ImGuiID deltaDockId = 0;
            ImGuiID heatmapDockId = 0;
            ImGui::DockBuilderSplitNode(leftDockId, ImGuiDir_Up, 0.50f, &deltaDockId, &heatmapDockId);

            ImGuiID inputsDockId = 0;
            ImGuiID detailsDockId = 0;
            ImGui::DockBuilderSplitNode(rightDockId, ImGuiDir_Up, 0.50f, &inputsDockId, &detailsDockId);

            ImGui::DockBuilderDockWindow("Mouse / Delta Graph", deltaDockId);
            ImGui::DockBuilderDockWindow("Mouse / Cursor Heatmap", heatmapDockId);
            ImGui::DockBuilderDockWindow("Mouse / Input Totals", inputsDockId);
            ImGui::DockBuilderDockWindow("Mouse / Selected Details", detailsDockId);
            ImGui::DockBuilderFinish(dockspaceId);
        }

        ImGui::DockSpace(dockspaceId, dockspaceSize, ImGuiDockNodeFlags_None);

        if (ImGui::Begin("Mouse / Delta Graph"))
        {
            ImGui::Text("Mouse delta total: x %ld, y %ld", g_mouseDelta.x, g_mouseDelta.y);
            ImGui::Text("Wheel total: %d", g_wheelDelta);
            ImGui::Text("Clicks: left %d, right %d, middle %d", g_leftClicks, g_rightClicks, g_middleClicks);
            ImGui::Text("Tracked mouse inputs: %d", TrackedInputs("Mouse"));
            DrawProgramFilterControl("Program Filter##mouse-delta");
            ImGui::Spacing();

            DrawMouseDeltaGraph(ImGui::GetContentRegionAvail());
        }
        ImGui::End();

        if (ImGui::Begin("Mouse / Cursor Heatmap"))
        {
            DrawProgramFilterControl("Program Filter##mouse-heatmap");
            DrawCursorHeatmap(ImGui::GetContentRegionAvail());
        }
        ImGui::End();

        if (ImGui::Begin("Mouse / Input Totals"))
        {
            DrawInputSummaryTable("mouse-input-summary", "Mouse", ImGui::GetContentRegionAvail().y);
        }
        ImGui::End();

        if (ImGui::Begin("Mouse / Selected Details"))
        {
            DrawSelectedInputDetails(ImGui::GetContentRegionAvail().y);
        }
        ImGui::End();
    }

    struct ProgramTotals
    {
        std::string appName;
        DWORD processId = 0;
        uint64_t total = 0;
        uint64_t keyboardTotal = 0;
        uint64_t mouseTotal = 0;
    };

    void DrawProgramTab()
    {
        if (ImGui::Button("Reset Programs docking"))
            g_rebuildProgramsDockLayout = true;

        const ImGuiID dockspaceId = ImGui::GetID("programs-dockspace");
        const ImVec2 dockspaceSize = ImGui::GetContentRegionAvail();

        if (g_rebuildProgramsDockLayout)
        {
            g_rebuildProgramsDockLayout = false;

            ImGui::DockBuilderRemoveNode(dockspaceId);
            ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockspaceId, dockspaceSize);

            ImGuiID programsDockId = 0;
            ImGuiID inputsDockId = 0;
            ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.52f, &programsDockId, &inputsDockId);

            ImGui::DockBuilderDockWindow("Programs / List", programsDockId);
            ImGui::DockBuilderDockWindow("Programs / Selected Inputs", inputsDockId);
            ImGui::DockBuilderFinish(dockspaceId);
        }

        ImGui::DockSpace(dockspaceId, dockspaceSize, ImGuiDockNodeFlags_None);

        std::vector<ProgramTotals> programs;
        for (const InputStats& input : g_inputs)
        {
            for (const AppInputStats& app : input.appTotals)
            {
                ProgramTotals* totals = nullptr;
                for (ProgramTotals& program : programs)
                {
                    if (program.processId == app.processId && program.appName == app.appName)
                    {
                        totals = &program;
                        break;
                    }
                }

                if (!totals)
                {
                    programs.push_back({ app.appName, app.processId });
                    totals = &programs.back();
                }

                totals->total += app.total;
                if (input.device == "Keyboard" || input.device == "Keyboard Combo")
                    totals->keyboardTotal += app.total;
                else if (input.device == "Mouse")
                    totals->mouseTotal += app.total;
            }
        }

        constexpr ImGuiTableFlags tableFlags =
            ImGuiTableFlags_Borders |
            ImGuiTableFlags_RowBg |
            ImGuiTableFlags_Resizable |
            ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Sortable;

        if (ImGui::Begin("Programs / List"))
        {
            ImGui::Text("Focused programs tracked: %d", static_cast<int>(programs.size()));
            ImGui::Separator();

            if (ImGui::BeginTable("program-summary", 5, tableFlags, ImVec2(0, ImGui::GetContentRegionAvail().y)))
            {
                ImGui::TableSetupColumn("Program");
                ImGui::TableSetupColumn("PID");
                ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_DefaultSort);
                ImGui::TableSetupColumn("Keyboard");
                ImGui::TableSetupColumn("Mouse");
                ImGui::TableHeadersRow();

                const ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
                const int column = SortColumn(sortSpecs, 2);
                const bool ascending = SortAscending(sortSpecs);
                SortWithDirection(programs, ascending, [&](const ProgramTotals& a, const ProgramTotals& b) {
                    switch (column)
                    {
                    case 0: return a.appName < b.appName;
                    case 1: return a.processId < b.processId;
                    case 2: return a.total < b.total;
                    case 3: return a.keyboardTotal < b.keyboardTotal;
                    case 4: return a.mouseTotal < b.mouseTotal;
                    default: return a.total < b.total;
                    }
                });

                for (const ProgramTotals& program : programs)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    const bool selected = program.processId == g_selectedProgramPid && program.appName == g_selectedProgramName;
                    const std::string selectableLabel = program.appName + "##program:" + std::to_string(program.processId);
                    if (ImGui::Selectable(selectableLabel.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns))
                    {
                        g_selectedProgramPid = program.processId;
                        g_selectedProgramName = program.appName;
                    }
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%lu", static_cast<unsigned long>(program.processId));
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%llu", static_cast<unsigned long long>(program.total));
                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%llu", static_cast<unsigned long long>(program.keyboardTotal));
                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("%llu", static_cast<unsigned long long>(program.mouseTotal));
                }

                ImGui::EndTable();
            }
        }
        ImGui::End();

        if (ImGui::Begin("Programs / Selected Inputs"))
        {
            if (g_selectedProgramPid == 0 && g_selectedProgramName.empty())
            {
                ImGui::TextUnformatted("Select a program row to view inputs recorded while it was focused.");
                ImGui::End();
                return;
            }

            ImGui::Text("Inputs for: %s (%lu)", g_selectedProgramName.c_str(), static_cast<unsigned long>(g_selectedProgramPid));
            ImGui::Separator();
            if (ImGui::BeginTable("program-inputs", 5, tableFlags, ImVec2(0, ImGui::GetContentRegionAvail().y)))
            {
                ImGui::TableSetupColumn("Device");
                ImGui::TableSetupColumn("Input");
                ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_DefaultSort);
                ImGui::TableSetupColumn("Down");
                ImGui::TableSetupColumn("Up");
                ImGui::TableHeadersRow();

                struct ProgramInputRow
                {
                    const InputStats* input = nullptr;
                    const AppInputStats* app = nullptr;
                };

                std::vector<ProgramInputRow> rows;
                for (const InputStats& input : g_inputs)
                {
                    for (const AppInputStats& app : input.appTotals)
                    {
                        if (app.processId == g_selectedProgramPid && app.appName == g_selectedProgramName)
                            rows.push_back({ &input, &app });
                    }
                }

                const ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
                const int column = SortColumn(sortSpecs, 2);
                const bool ascending = SortAscending(sortSpecs);
                SortWithDirection(rows, ascending, [&](const ProgramInputRow& a, const ProgramInputRow& b) {
                    switch (column)
                    {
                    case 0: return a.input->device < b.input->device;
                    case 1: return a.input->label < b.input->label;
                    case 2: return a.app->total < b.app->total;
                    case 3: return a.app->downTotal < b.app->downTotal;
                    case 4: return a.app->upTotal < b.app->upTotal;
                    default: return a.app->total < b.app->total;
                    }
                });

                for (const ProgramInputRow& row : rows)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(row.input->device.c_str());
                    ImGui::TableSetColumnIndex(1);
                    const bool selected = row.input->id == g_selectedInputId;
                    const std::string selectableLabel = row.input->label + "##program-input:" + row.input->id;
                    if (ImGui::Selectable(selectableLabel.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns))
                        g_selectedInputId = row.input->id;
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%llu", static_cast<unsigned long long>(row.app->total));
                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%llu", static_cast<unsigned long long>(row.app->downTotal));
                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("%llu", static_cast<unsigned long long>(row.app->upTotal));
                }

                ImGui::EndTable();
            }
        }
        ImGui::End();
    }

    void DrawUi()
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);

        constexpr ImGuiWindowFlags windowFlags =
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoNavFocus |
            ImGuiWindowFlags_MenuBar;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("keynalysis", nullptr, windowFlags);
        ImGui::PopStyleVar(2);

        ImGui::TextUnformatted("Keyboard and mouse input test process");
        ImGui::Separator();
        DrawToolbar();
        ImGui::Spacing();
        DrawGlobalSummary();
        ImGui::Spacing();

        if (ImGui::BeginTabBar("main-tabs"))
        {
            if (ImGui::BeginTabItem("Home"))
            {
                DrawHomeTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Keyboard"))
            {
                DrawKeyboardTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Mouse & Monitors"))
            {
                DrawMouseTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Programs"))
            {
                DrawProgramTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::End();
    }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_CREATE:
        g_hwnd = hWnd;
        RegisterRawInput(hWnd);
        return 0;
    case WM_INPUT:
        HandleRawInput(reinterpret_cast<HRAWINPUT>(lParam));
        return 0;
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
        {
            MinimizeToTray(hWnd);
            return 0;
        }

        if (wParam != SIZE_MINIMIZED && g_pd3dDevice != nullptr)
        {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, static_cast<UINT>(LOWORD(lParam)), static_cast<UINT>(HIWORD(lParam)), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_TRAYICON:
        if (lParam == WM_LBUTTONDBLCLK || lParam == WM_LBUTTONUP)
            RestoreFromTray(hWnd);
        return 0;
    case WM_DESTROY:
        RemoveTrayIcon(hWnd);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
    WNDCLASSEXW wc{
        sizeof(WNDCLASSEXW),
        CS_CLASSDC,
        WndProc,
        0L,
        0L,
        hInstance,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        L"keynalysisWindow",
        nullptr
    };
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowW(
        wc.lpszClassName,
        L"keynalysis",
        WS_OVERLAPPEDWINDOW,
        100,
        100,
        900,
        620,
        nullptr,
        nullptr,
        wc.hInstance,
        nullptr);

    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);
    RefreshMonitors();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    const bool loadedAppSettings = LoadAppSettings();
    io.IniFilename = g_imguiIniPath.c_str();
    RefreshMonitors();
    if (FileExists(g_imguiIniPath))
        DisableDefaultDockRebuilds();

    if (loadedAppSettings && FileExists(g_dataFilePath))
        LoadSnapshotFromFile(g_dataFilePath);

    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    bool done = false;
    while (!done)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        if (g_captureEnabled)
            PollSystemCombos();

        MaybeAutoSave();

        if (IsIconic(hwnd))
        {
            Sleep(50);
            continue;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        DrawUi();

        ImGui::Render();
        const float clearColor[4] = { 0.08f, 0.09f, 0.10f, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    ImGui::SaveIniSettingsToDisk(g_imguiIniPath.c_str());
    SaveAppSettings();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}
