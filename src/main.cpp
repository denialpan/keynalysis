#include <windows.h>
#include <shellapi.h>
#include <d3d11.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <cmath>
#include <string>
#include <vector>

#include "imgui.h"
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
        std::string windowTitle;
        DWORD processId = 0;
        std::string detail;
    };

    struct AppInputStats
    {
        std::string appName;
        std::string windowTitle;
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
        std::string lastWindowTitle;
        std::deque<InputEvent> events;
        std::vector<AppInputStats> appTotals;
    };

    struct FocusContext
    {
        std::string appName = "Unknown";
        std::string windowTitle;
        DWORD processId = 0;
    };

    struct MouseDeltaSample
    {
        float dx = 0.0f;
        float dy = 0.0f;
    };

    struct MonitorHeatmap
    {
        RECT rect{};
        std::string name;
        std::array<uint32_t, 48 * 27> bins{};
        uint32_t maxBin = 0;
    };

    HWND g_hwnd = nullptr;
    ID3D11Device* g_pd3dDevice = nullptr;
    ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
    IDXGISwapChain* g_pSwapChain = nullptr;
    ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
    std::vector<InputStats> g_inputs;
    std::vector<MonitorHeatmap> g_monitors;
    std::string g_selectedInputId;
    int g_selectedMonitorIndex = 0;
    std::array<bool, 256> g_keysDown{};
    std::deque<MouseDeltaSample> g_mouseDeltaSamples;
    POINT g_mouseDelta{};
    int g_wheelDelta = 0;
    int g_leftClicks = 0;
    int g_rightClicks = 0;
    int g_middleClicks = 0;
    bool g_captureEnabled = true;
    bool g_rawInputRegistered = false;
    bool g_trayIconVisible = false;
    auto g_startTime = std::chrono::steady_clock::now();

    constexpr UINT WM_TRAYICON = WM_APP + 1;
    constexpr UINT TRAY_ICON_ID = 1;
    constexpr int HEATMAP_COLUMNS = 48;
    constexpr int HEATMAP_ROWS = 27;

    double NowSeconds()
    {
        using namespace std::chrono;
        return duration<double>(steady_clock::now() - g_startTime).count();
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

        wchar_t title[256]{};
        if (GetWindowTextW(foreground, title, static_cast<int>(std::size(title))) > 0)
            context.windowTitle = WideToUtf8(title);

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

        g_monitors.push_back(std::move(heatmap));
        return TRUE;
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

    void AddMouseDeltaSample(LONG dx, LONG dy)
    {
        g_mouseDeltaSamples.push_back({ static_cast<float>(dx), static_cast<float>(dy) });
        while (g_mouseDeltaSamples.size() > 240)
            g_mouseDeltaSamples.pop_front();
    }

    void AddCursorHeatSample()
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
        const int rawBinX = static_cast<int>((cursor.x - rect.left) * HEATMAP_COLUMNS / width);
        const int rawBinY = static_cast<int>((cursor.y - rect.top) * HEATMAP_ROWS / height);
        const int binX = std::clamp(rawBinX, 0, HEATMAP_COLUMNS - 1);
        const int binY = std::clamp(rawBinY, 0, HEATMAP_ROWS - 1);
        const int binIndex = binY * HEATMAP_COLUMNS + binX;

        ++heatmap.bins[binIndex];
        heatmap.maxBin = std::max(heatmap.maxBin, heatmap.bins[binIndex]);
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

        input.appTotals.push_back({ focus.appName, focus.windowTitle, focus.processId });
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
        input.lastWindowTitle = focus.windowTitle;

        AppInputStats& appStats = GetAppStats(input, focus);
        ++appStats.total;
        appStats.windowTitle = focus.windowTitle;

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

        input.events.push_front({ now, std::move(action), focus.appName, focus.windowTitle, focus.processId, std::move(detail) });
        while (input.events.size() > 100)
            input.events.pop_back();
    }

    void ClearInputData()
    {
        g_inputs.clear();
        g_selectedInputId.clear();
        g_keysDown.fill(false);
        g_mouseDeltaSamples.clear();
        for (MonitorHeatmap& monitor : g_monitors)
        {
            monitor.bins.fill(0);
            monitor.maxBin = 0;
        }
        g_mouseDelta = {};
        g_wheelDelta = 0;
        g_leftClicks = 0;
        g_rightClicks = 0;
        g_middleClicks = 0;
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
        AddCursorHeatSample();

        if (mouse.lLastX != 0 || mouse.lLastY != 0)
        {
            g_mouseDelta.x += mouse.lLastX;
            g_mouseDelta.y += mouse.lLastY;
            AddMouseDeltaSample(mouse.lLastX, mouse.lLastY);
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
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("mouse-delta-graph", size);
        const ImVec2 end(origin.x + size.x, origin.y + size.y);

        drawList->AddRectFilled(origin, end, IM_COL32(20, 23, 28, 255));
        drawList->AddRect(origin, end, IM_COL32(80, 88, 98, 255));

        const float midY = origin.y + size.y * 0.5f;
        drawList->AddLine(ImVec2(origin.x, midY), ImVec2(end.x, midY), IM_COL32(70, 76, 84, 255));

        if (g_mouseDeltaSamples.size() < 2)
        {
            drawList->AddText(ImVec2(origin.x + 8.0f, origin.y + 8.0f), IM_COL32(160, 166, 176, 255), "Move the mouse to populate delta data");
            return;
        }

        float maxAbs = 1.0f;
        for (const MouseDeltaSample& sample : g_mouseDeltaSamples)
        {
            maxAbs = std::max(maxAbs, std::abs(sample.dx));
            maxAbs = std::max(maxAbs, std::abs(sample.dy));
        }

        auto pointForSample = [&](size_t index, float value) {
            const float t = static_cast<float>(index) / static_cast<float>(g_mouseDeltaSamples.size() - 1);
            const float x = origin.x + t * size.x;
            const float y = midY - (value / maxAbs) * (size.y * 0.45f);
            return ImVec2(x, y);
        };

        for (size_t i = 1; i < g_mouseDeltaSamples.size(); ++i)
        {
            drawList->AddLine(pointForSample(i - 1, g_mouseDeltaSamples[i - 1].dx), pointForSample(i, g_mouseDeltaSamples[i].dx), IM_COL32(92, 180, 255, 255), 1.5f);
            drawList->AddLine(pointForSample(i - 1, g_mouseDeltaSamples[i - 1].dy), pointForSample(i, g_mouseDeltaSamples[i].dy), IM_COL32(255, 174, 92, 255), 1.5f);
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

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("cursor-heatmap", size);
        const ImVec2 end(origin.x + size.x, origin.y + size.y);

        drawList->AddRectFilled(origin, end, IM_COL32(20, 23, 28, 255));
        drawList->AddRect(origin, end, IM_COL32(80, 88, 98, 255));

        if (g_monitors.empty())
        {
            drawList->AddText(ImVec2(origin.x + 8.0f, origin.y + 8.0f), IM_COL32(160, 166, 176, 255), "No monitors detected");
            return;
        }

        g_selectedMonitorIndex = std::clamp(g_selectedMonitorIndex, 0, static_cast<int>(g_monitors.size()) - 1);
        const MonitorHeatmap& heatmap = g_monitors[g_selectedMonitorIndex];
        const float cellW = size.x / static_cast<float>(HEATMAP_COLUMNS);
        const float cellH = size.y / static_cast<float>(HEATMAP_ROWS);

        for (int y = 0; y < HEATMAP_ROWS; ++y)
        {
            for (int x = 0; x < HEATMAP_COLUMNS; ++x)
            {
                const uint32_t count = heatmap.bins[y * HEATMAP_COLUMNS + x];
                if (count == 0)
                    continue;

                const float value = heatmap.maxBin > 0 ? static_cast<float>(count) / static_cast<float>(heatmap.maxBin) : 0.0f;
                const ImVec2 cellMin(origin.x + x * cellW, origin.y + y * cellH);
                const ImVec2 cellMax(origin.x + (x + 1) * cellW, origin.y + (y + 1) * cellH);
                drawList->AddRectFilled(cellMin, cellMax, HeatColor(value));
            }
        }

        for (int x = 1; x < HEATMAP_COLUMNS; ++x)
        {
            const float gx = origin.x + x * cellW;
            drawList->AddLine(ImVec2(gx, origin.y), ImVec2(gx, end.y), IM_COL32(45, 50, 58, 75));
        }
        for (int y = 1; y < HEATMAP_ROWS; ++y)
        {
            const float gy = origin.y + y * cellH;
            drawList->AddLine(ImVec2(origin.x, gy), ImVec2(end.x, gy), IM_COL32(45, 50, 58, 75));
        }

        char label[128]{};
        snprintf(label, sizeof(label), "max cell: %u", heatmap.maxBin);
        drawList->AddText(ImVec2(origin.x + 8.0f, origin.y + 8.0f), IM_COL32(220, 224, 230, 255), label);
    }

    bool MatchesDeviceFilter(const InputStats& input, const char* deviceFilter)
    {
        return deviceFilter == nullptr || input.device == deviceFilter;
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

    void DrawToolbar()
    {
        if (!g_rawInputRegistered)
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "Raw Input registration failed.");
        else
            ImGui::TextColored(ImVec4(0.25f, 0.8f, 0.45f, 1.0f), "Raw Input is active. Events continue while unfocused or minimized.");

        ImGui::Checkbox("Capture input", &g_captureEnabled);
        ImGui::SameLine();
        if (ImGui::Button("Clear data"))
            ClearInputData();
        ImGui::SameLine();
        if (ImGui::Button("Minimize"))
            ShowWindow(g_hwnd, SW_MINIMIZE);
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

    void DrawInputSummaryTable(const char* tableId, const char* deviceFilter, float height)
    {
        if (ImGui::BeginTable(tableId, 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0, height)))
        {
            ImGui::TableSetupColumn("Device");
            ImGui::TableSetupColumn("Input");
            ImGui::TableSetupColumn("Total");
            ImGui::TableSetupColumn("Down");
            ImGui::TableSetupColumn("Up");
            ImGui::TableSetupColumn("State");
            ImGui::TableSetupColumn("Last App");
            ImGui::TableSetupColumn("Last");
            ImGui::TableHeadersRow();

            for (InputStats& input : g_inputs)
            {
                if (!MatchesDeviceFilter(input, deviceFilter))
                    continue;

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
        if (ImGui::BeginTable("selected-app-totals", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0, appTableHeight)))
        {
            ImGui::TableSetupColumn("Program");
            ImGui::TableSetupColumn("PID");
            ImGui::TableSetupColumn("Total");
            ImGui::TableSetupColumn("Down");
            ImGui::TableSetupColumn("Up");
            ImGui::TableHeadersRow();

            for (const AppInputStats& app : selectedInput->appTotals)
            {
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
        if (ImGui::BeginTable("selected-input-details", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0, detailHeight)))
        {
            ImGui::TableSetupColumn("Time");
            ImGui::TableSetupColumn("Event");
            ImGui::TableSetupColumn("Program");
            ImGui::TableSetupColumn("PID");
            ImGui::TableSetupColumn("Window");
            ImGui::TableSetupColumn("Detail");
            ImGui::TableHeadersRow();

            for (const InputEvent& event : selectedInput->events)
            {
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
                ImGui::TextUnformatted(event.windowTitle.c_str());
                ImGui::TableSetColumnIndex(5);
                ImGui::TextUnformatted(event.detail.c_str());
            }
            ImGui::EndTable();
        }
    }

    void DrawHomeTab()
    {
        DrawOverviewStats();
        ImGui::Spacing();

        const float availableHeight = ImGui::GetContentRegionAvail().y;
        DrawInputSummaryTable("home-input-summary", nullptr, std::max(160.0f, availableHeight * 0.48f));
        ImGui::Spacing();
        DrawSelectedInputDetails(ImGui::GetContentRegionAvail().y);
    }

    void DrawKeyboardTab()
    {
        int keysDown = 0;
        for (const bool down : g_keysDown)
            keysDown += down ? 1 : 0;

        ImGui::Text("Keys down: %d", keysDown);
        ImGui::Text("Tracked keyboard inputs: %d", TrackedInputs("Keyboard"));
        ImGui::Text("Keyboard invocations: %llu", static_cast<unsigned long long>(TotalInvocations("Keyboard")));
        ImGui::Spacing();

        const float availableHeight = ImGui::GetContentRegionAvail().y;
        DrawInputSummaryTable("keyboard-input-summary", "Keyboard", std::max(170.0f, availableHeight * 0.55f));
        ImGui::Spacing();
        DrawSelectedInputDetails(ImGui::GetContentRegionAvail().y);
    }

    void DrawMouseTab()
    {
        ImGui::Text("Mouse delta total: x %ld, y %ld", g_mouseDelta.x, g_mouseDelta.y);
        ImGui::Text("Wheel total: %d", g_wheelDelta);
        ImGui::Text("Clicks: left %d, right %d, middle %d", g_leftClicks, g_rightClicks, g_middleClicks);
        ImGui::Text("Tracked mouse inputs: %d", TrackedInputs("Mouse"));
        ImGui::Spacing();

        const float graphWidth = ImGui::GetContentRegionAvail().x;
        const float graphHeight = 220.0f;
        if (graphWidth >= 720.0f)
        {
            const float panelWidth = (graphWidth - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            ImGui::BeginGroup();
            DrawMouseDeltaGraph(ImVec2(panelWidth, graphHeight));
            ImGui::EndGroup();
            ImGui::SameLine();
            ImGui::BeginGroup();
            DrawCursorHeatmap(ImVec2(panelWidth, graphHeight - ImGui::GetFrameHeightWithSpacing()));
            ImGui::EndGroup();
        }
        else
        {
            DrawMouseDeltaGraph(ImVec2(graphWidth, graphHeight));
            DrawCursorHeatmap(ImVec2(graphWidth, graphHeight));
        }

        ImGui::Spacing();
        DrawInputSummaryTable("mouse-input-summary", "Mouse", std::max(150.0f, ImGui::GetContentRegionAvail().y * 0.45f));
    }

    struct ProgramTotals
    {
        std::string appName;
        DWORD processId = 0;
        uint64_t total = 0;
        uint64_t keyboardTotal = 0;
        uint64_t mouseTotal = 0;
        std::string lastWindowTitle;
    };

    void DrawProgramTab()
    {
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
                totals->lastWindowTitle = app.windowTitle;
                if (input.device == "Keyboard")
                    totals->keyboardTotal += app.total;
                else if (input.device == "Mouse")
                    totals->mouseTotal += app.total;
            }
        }

        std::sort(programs.begin(), programs.end(), [](const ProgramTotals& a, const ProgramTotals& b) {
            return a.total > b.total;
        });

        ImGui::Text("Focused programs tracked: %d", static_cast<int>(programs.size()));
        ImGui::Spacing();

        if (ImGui::BeginTable("program-summary", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0, ImGui::GetContentRegionAvail().y)))
        {
            ImGui::TableSetupColumn("Program");
            ImGui::TableSetupColumn("PID");
            ImGui::TableSetupColumn("Total");
            ImGui::TableSetupColumn("Keyboard");
            ImGui::TableSetupColumn("Mouse");
            ImGui::TableSetupColumn("Last Window");
            ImGui::TableHeadersRow();

            for (const ProgramTotals& program : programs)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(program.appName.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%lu", static_cast<unsigned long>(program.processId));
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%llu", static_cast<unsigned long long>(program.total));
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%llu", static_cast<unsigned long long>(program.keyboardTotal));
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%llu", static_cast<unsigned long long>(program.mouseTotal));
                ImGui::TableSetColumnIndex(5);
                ImGui::TextUnformatted(program.lastWindowTitle.c_str());
            }

            ImGui::EndTable();
        }
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
            ImGuiWindowFlags_NoNavFocus;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("keynalysis", nullptr, windowFlags);
        ImGui::PopStyleVar(2);

        ImGui::TextUnformatted("Keyboard and mouse input test process");
        ImGui::Separator();
        DrawToolbar();
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

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}
