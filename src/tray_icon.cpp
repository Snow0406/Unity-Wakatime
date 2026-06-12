#include "tray_icon.h"
#include "file_watcher.h"
#include "wakatime_client.h"
#include "process_monitor.h"
#include "app_registry.h"
#include "windows_dark_mode.h"

#include <wincodec.h>
#include <comdef.h>

namespace
{
    std::wstring Utf8ToWideTray(const std::string& s)
    {
        if (s.empty()) return L"";
        const int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        if (len <= 1) return L"";
        std::wstring w(static_cast<size_t>(len), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
        w.resize(static_cast<size_t>(len - 1));
        return w;
    }
}

TrayIcon::TrayIcon() : hwnd(nullptr),
                       hMenu(nullptr),
                       isMonitoring(false),
                       totalHeartbeats(0),
                       initialized(false)
{
    // NOTIFYICONDATA 구조체 초기화
    ZeroMemory(&nid, sizeof(NOTIFYICONDATAW));
    ZeroMemory(&wc, sizeof(WNDCLASSW));

    WT_LOG("[TrayIcon] Created");
}

TrayIcon::~TrayIcon()
{
    Shutdown();

    WT_LOG("[TrayIcon] Destroyed");
}

bool TrayIcon::Initialize(const std::string &appName)
{
    WT_LOG("[TrayIcon] Initializing: " << appName);

    // 숨겨진 창 생성
    if (!CreateHiddenWindow())
    {
        WT_ERR("[TrayIcon] Failed to create hidden window");
        return false;
    }

    // 트레이 아이콘 생성
    if (!CreateTrayIcon())
    {
        WT_ERR("[TrayIcon] Failed to create tray icon");
        return false;
    }

    // 컨텍스트 메뉴 생성
    hMenu = CreateContextMenu();
    if (!hMenu)
    {
        WT_ERR("[TrayIcon] Failed to create context menu");
        return false;
    }

    // 초기 툴팁 설정
    UpdateTooltip(appName + " - Ready");

    initialized = true;
    WT_LOG("[TrayIcon] Initialized successfully");

    return true;
}

bool TrayIcon::CreateHiddenWindow()
{
    // 창 클래스 등록
    wc.lpfnWndProc = WindowProc; // 윈도우 프로시저
    wc.hInstance = GetModuleHandle(nullptr); // 현재 모듈 핸들
    wc.lpszClassName = L"CreativeWakaTimeTray"; // 클래스 이름
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH) (COLOR_WINDOW + 1);

    if (!RegisterClassW(&wc))
    {
        if (const DWORD error = GetLastError(); error != ERROR_CLASS_ALREADY_EXISTS)
        {
            // 이미 등록된 경우는 정상
            WT_ERR("[TrayIcon] RegisterClass failed (Error: " << error << ")");
            return false;
        }
    }

    // 숨겨진 창 생성
    // WS_OVERLAPPED: 기본 창 스타일
    // CW_USEDEFAULT: 기본 위치/크기
    hwnd = CreateWindowW(
        L"CreativeWakaTimeTray",        // 클래스 이름
        L"Creative WakaTime",           // 창 제목
        WS_OVERLAPPED,                  // 창 스타일
        CW_USEDEFAULT, CW_USEDEFAULT,   // 위치
        1, 1,                           // 크기 (최소)
        nullptr,                        // 부모 창
        nullptr,                        // 메뉴
        GetModuleHandle(nullptr),       // 인스턴스
        this                            // 추가 데이터 (this 포인터 전달)
    );

    if (!hwnd)
    {
        const DWORD error = GetLastError();
        WT_ERR("[TrayIcon] CreateWindow failed (Error: " << error << ")");
        return false;
    }

    WindowsDarkMode::ApplyToWindow(hwnd);

    WT_LOG("[TrayIcon] Hidden window created");
    return true;
}

bool TrayIcon::CreateTrayIcon() {
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_INFO;
    nid.uCallbackMessage = WM_TRAYICON;

    nid.hIcon = LoadPngIcon("logo_32.png");

    wcscpy_s(nid.szTip, L"Creative WakaTime - Starting...");

    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        DWORD error = GetLastError();
        WT_ERR("[TrayIcon] Shell_NotifyIconW(NIM_ADD) failed (Error: " << error << ")");
        return false;
    }

    WT_LOG("[TrayIcon] Tray icon added to system tray");
    return true;
}

HICON TrayIcon::LoadPngIcon(const std::string& filePath) {
    WT_LOG("[TrayIcon] Loading PNG icon using WIC: " << filePath);

    // 파일 존재 확인
    if (!fs::exists(filePath)) {
        WT_ERR("[TrayIcon] PNG file not found: " << filePath);
        return LoadIcon(nullptr, IDI_APPLICATION);
    }

    // COM 초기화 (이미 되어있을 수도 있지만 안전하게)
    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool comInitialized = SUCCEEDED(hrCom);

    // WIC 팩토리 생성
    IWICImagingFactory* pFactory = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_IWICImagingFactory,
        (LPVOID*)&pFactory
    );

    if (FAILED(hr)) {
        WT_ERR("[TrayIcon] Failed to create WIC factory (HRESULT: 0x" << std::hex << hr << ")");
        if (comInitialized) CoUninitialize();
        return LoadIcon(nullptr, IDI_APPLICATION);
    }

    WT_LOG("[TrayIcon] ✅ WIC factory created successfully");

    const auto wFilePath = Utf8ToWideTray(filePath);

    // PNG 디코더 생성
    IWICBitmapDecoder* pDecoder = nullptr;
    hr = pFactory->CreateDecoderFromFilename(
        wFilePath.c_str(),
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnLoad,
        &pDecoder
    );

    if (FAILED(hr)) {
        WT_ERR("[TrayIcon] Failed to create PNG decoder (HRESULT: 0x" << std::hex << hr << ")");
        pFactory->Release();
        if (comInitialized) CoUninitialize();
        return LoadIcon(nullptr, IDI_APPLICATION);
    }

    // 첫 번째 프레임 가져오기
    IWICBitmapFrameDecode* pFrame = nullptr;
    hr = pDecoder->GetFrame(0, &pFrame);

    if (FAILED(hr)) {
        WT_ERR("[TrayIcon] Failed to get frame from PNG");
        pDecoder->Release();
        pFactory->Release();
        if (comInitialized) CoUninitialize();
        return LoadIcon(nullptr, IDI_APPLICATION);
    }

    // 원본 크기 확인
    UINT originalWidth, originalHeight;
    pFrame->GetSize(&originalWidth, &originalHeight);
    WT_LOG("[TrayIcon] Original PNG size: " << originalWidth << "x" << originalHeight);

    // 트레이 아이콘 크기 (시스템에서 권장하는 크기)
    int iconSize = GetSystemMetrics(SM_CXSMICON);  // 보통 16x16
    if (iconSize <= 0) iconSize = 32;  // 기본값

    WT_LOG("[TrayIcon] System tray icon size: " << iconSize << "x" << iconSize);

    // 스케일러 생성 (크기 조정)
    IWICBitmapScaler* pScaler = nullptr;
    hr = pFactory->CreateBitmapScaler(&pScaler);

    if (SUCCEEDED(hr)) {
        hr = pScaler->Initialize(
            pFrame,
            iconSize,
            iconSize,
            WICBitmapInterpolationModeCubic
        );
    }

    IWICBitmapSource* pSource = pScaler ? (IWICBitmapSource*)pScaler : (IWICBitmapSource*)pFrame;

    // 포맷 컨버터 생성 (BGRA로 변환)
    IWICFormatConverter* pConverter = nullptr;
    hr = pFactory->CreateFormatConverter(&pConverter);

    if (SUCCEEDED(hr)) {
        hr = pConverter->Initialize(
            pSource,
            GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeMedianCut
        );
    }

    HICON hIcon = nullptr;

    if (SUCCEEDED(hr)) {
        // DIB 비트맵 생성
        BITMAPINFOHEADER bmih = {};
        bmih.biSize = sizeof(BITMAPINFOHEADER);
        bmih.biWidth = iconSize;
        bmih.biHeight = -iconSize;  // Top-down
        bmih.biPlanes = 1;
        bmih.biBitCount = 32;
        bmih.biCompression = BI_RGB;

        void* pBits = nullptr;
        HDC hdc = GetDC(nullptr);
        HBITMAP hBitmap = CreateDIBSection(hdc, (BITMAPINFO*)&bmih, DIB_RGB_COLORS, &pBits, nullptr, 0);
        ReleaseDC(nullptr, hdc);

        if (hBitmap && pBits) {
            // WIC에서 비트맵 데이터 복사
            UINT stride = iconSize * 4;  // 32bpp = 4 bytes per pixel
            UINT imageSize = stride * iconSize;

            hr = pConverter->CopyPixels(
                nullptr,
                stride,
                imageSize,
                (BYTE*)pBits
            );

            if (SUCCEEDED(hr)) {
                // 마스크 비트맵 생성 (알파 채널 기반)
                HBITMAP hMaskBitmap = CreateBitmap(iconSize, iconSize, 1, 1, nullptr);

                // HBITMAP을 HICON으로 변환
                ICONINFO iconInfo = {};
                iconInfo.fIcon = TRUE;
                iconInfo.xHotspot = iconSize / 2;
                iconInfo.yHotspot = iconSize / 2;
                iconInfo.hbmColor = hBitmap;
                iconInfo.hbmMask = hMaskBitmap;

                hIcon = CreateIconIndirect(&iconInfo);

                DeleteObject(hMaskBitmap);

                if (hIcon) {
                    WT_LOG("[TrayIcon] ✅ PNG icon converted to HICON successfully!");
                } else {
                    DWORD error = GetLastError();
                    WT_ERR("[TrayIcon] CreateIconIndirect failed (Error: " << error << ")");
                }
            } else {
                WT_ERR("[TrayIcon] Failed to copy pixels from WIC converter");
            }

            DeleteObject(hBitmap);
        } else {
            WT_ERR("[TrayIcon] Failed to create DIB section");
        }
    }

    // 리소스 정리
    if (pConverter) pConverter->Release();
    if (pScaler) pScaler->Release();
    if (pFrame) pFrame->Release();
    if (pDecoder) pDecoder->Release();
    if (pFactory) pFactory->Release();

    if (comInitialized && hrCom != RPC_E_CHANGED_MODE) {
        CoUninitialize();
    }

    if (hIcon) {
        WT_LOG("[TrayIcon] ✅ PNG icon loaded successfully using WIC!");
        return hIcon;
    } else {
        WT_ERR("[TrayIcon] ❌ Failed to load PNG, using fallback icon");
        return LoadIcon(nullptr, IDI_APPLICATION);
    }
}

HMENU TrayIcon::CreateContextMenu()
{
    const HMENU menu = CreatePopupMenu();

    // 헤더 (비활성 타이틀)
    AppendMenuW(menu, MF_STRING | MF_GRAYED | MF_DISABLED, 0, L"Creative WakaTime");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    HMENU trackedAppsSubMenu = CreateTrackedAppsSubMenu();
    AppendMenuW(menu, MF_STRING | MF_POPUP, (UINT_PTR) trackedAppsSubMenu, L"Tracked Apps");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    HMENU statusSubMenu = CreateStatusSubMenu();
    AppendMenuW(menu, MF_STRING | MF_POPUP, (UINT_PTR) statusSubMenu, L"Status");

    AppendMenuW(menu, MF_STRING, IDM_TOGGLE_MONITORING,
                isMonitoring ? L"Pause Monitoring" : L"Resume Monitoring");
    AppendMenuW(menu, MF_STRING, IDM_OPEN_DASHBOARD, L"Open Dashboard");
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"Setup API Key");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

    return menu;
}

HMENU TrayIcon::CreateTrackedAppsSubMenu()
{
    const HMENU subMenu = CreatePopupMenu();

    // 활성 앱 중 실행 중인 것 표시 (선택 앱만 스캔하므로 비활성 앱 실행 상태는 알 수 없음)
    std::unordered_set<std::string> activeAppIds;
    if (const auto *monitor = Globals::GetProcessMonitor())
    {
        activeAppIds = monitor->GetActiveAppIds();
    }

    const auto &apps = AppRegistry::All();
    for (size_t i = 0; i < apps.size(); ++i)
    {
        const auto &def = apps[i];
        const bool enabled = AppRegistry::IsEnabled(def.id);
        const bool running = enabled && activeAppIds.count(def.id) > 0;

        std::wstring label = Utf8ToWideTray(def.displayName);
        if (running) label += L"   ● Running"; // ● running

        const UINT flags = MF_STRING | (enabled ? MF_CHECKED : MF_UNCHECKED);
        AppendMenuW(subMenu, flags, IDM_APP_BASE + static_cast<UINT>(i), label.c_str());
    }

    return subMenu;
}


HMENU TrayIcon::CreateStatusSubMenu()
{
    const HMENU subMenu = CreatePopupMenu();

    // Monitoring state
    const std::wstring monitoringStatus = isMonitoring ? L"Monitoring: Active" : L"Monitoring: Paused";
    AppendMenuW(subMenu, MF_STRING | MF_GRAYED, 0, monitoringStatus.c_str());

    // Active context
    if (!activeContext.empty())
    {
        const std::wstring contextInfo = L"Active: " + Utf8ToWideTray(activeContext);
        AppendMenuW(subMenu, MF_STRING | MF_GRAYED, 0, contextInfo.c_str());
    } else
    {
        AppendMenuW(subMenu, MF_STRING | MF_GRAYED, 0, L"No active creative tool detected");
    }

    // Heartbeat summary
    int sent = 0;
    int failed = 0;
    if (const auto *client = Globals::GetWakaTimeClient())
    {
        client->GetStats(sent, failed);
    }

    std::wstring heartbeatInfo = L"Heartbeats: " + std::to_wstring(totalHeartbeats) +
                                 L" (Sent: " + std::to_wstring(sent) +
                                 L", Failed: " + std::to_wstring(failed) + L")";
    AppendMenuW(subMenu, MF_STRING | MF_GRAYED, 0, heartbeatInfo.c_str());

    AppendMenuW(subMenu, MF_SEPARATOR, 0, nullptr);

    // Actions
    AppendMenuW(subMenu, MF_STRING, IDM_SHOW_STATUS, L"Refresh Status");

    return subMenu;
}


void TrayIcon::RefreshStatusMenu()
{
    // 메뉴는 표시 시점(ShowContextMenu)에 매번 새로 빌드하므로 별도 갱신은 불필요.
}


void TrayIcon::UpdateTooltip(const std::string &tooltip)
{
    if (!initialized) return;

    auto wTooltip = Utf8ToWideTray(tooltip);

    // 툴팁 길이 제한 (Windows 제한)
    if (wTooltip.length() >= 128)
    {
        wTooltip = wTooltip.substr(0, 124) + L"...";
    }

    wcscpy_s(nid.szTip, wTooltip.c_str());
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayIcon::ShowContextMenu(const int x, const int y)
{
    // 앱 활성/실행 상태가 동적이므로 표시할 때마다 메뉴를 새로 빌드한다.
    if (hMenu)
    {
        DestroyMenu(hMenu);
        hMenu = nullptr;
    }
    hMenu = CreateContextMenu();
    if (!hMenu)
    {
        WT_LOG("[TrayIcon] Failed to rebuild context menu!");
        return;
    }

    SetForegroundWindow(hwnd);

    const int selected = TrackPopupMenu(
        hMenu,
        TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
        x, y,
        0,
        hwnd,
        nullptr
    );

    if (selected > 0)
    {
        HandleMenuCommand(selected);
    }

    PostMessage(hwnd, WM_NULL, 0, 0);
}


void TrayIcon::HandleMenuCommand(int menuId)
{
    // Tracked Apps 항목 (IDM_APP_BASE + index)
    if (const auto &apps = AppRegistry::All();
        menuId >= IDM_APP_BASE && menuId < IDM_APP_BASE + static_cast<int>(apps.size()))
    {
        const auto &def = apps[menuId - IDM_APP_BASE];
        const bool newEnabled = !AppRegistry::IsEnabled(def.id);
        if (onToggleApp) onToggleApp(def.id, newEnabled);
        return;
    }

    switch (menuId)
    {
        case IDM_SHOW_STATUS:
            if (onShowStatus) onShowStatus();
            break;

        case IDM_TOGGLE_MONITORING:
            isMonitoring = !isMonitoring;
            if (onToggleMonitoring) onToggleMonitoring(isMonitoring);
            break;

        case IDM_OPEN_DASHBOARD:
            if (onOpenDashboard) onOpenDashboard();
            break;

        case IDM_SETTINGS:
            {
                if (const std::string newApiKey = ShowApiKeyInputDialog(); !newApiKey.empty())
                {
                    if (onApiKeyChange) onApiKeyChange(newApiKey);
                    ShowInfoNotification("API Key updated successfully !");
                }
            }
            break;

        case IDM_EXIT:
            if (onExit) onExit();
            break;
    }
}


int TrayIcon::RunMessageLoop()
{
    if (!initialized) return 0;

    // 포커스 유지 시 주기 heartbeat(2분)은 항상 설치.
    // 프로세스 스캔 타이머는 활성 앱 수에 따라 SetProcessScanActive로 동적 제어한다.
    SetTimer(hwnd, TIMER_PERIODIC_HEARTBEAT, PERIODIC_HEARTBEAT_INTERVAL_MS, nullptr);

    // 정석 메시지 펌프: idle 시 GetMessage가 커널에서 블록되어 CPU ≈ 0.
    // hwnd=nullptr → 스레드 메시지(WM_QUIT)와 WinEvent 훅 콜백까지 모두 수신/디스패치.
    MSG msg;
    BOOL result;
    while ((result = GetMessage(&msg, nullptr, 0, 0)) != 0)
    {
        if (result == -1) break; // GetMessage 오류
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    KillTimer(hwnd, TIMER_PROCESS_SCAN);
    KillTimer(hwnd, TIMER_PERIODIC_HEARTBEAT);

    return static_cast<int>(msg.wParam);
}

void TrayIcon::SetProcessScanActive(const bool active)
{
    if (!initialized || hwnd == nullptr) return;
    if (active)
    {
        SetTimer(hwnd, TIMER_PROCESS_SCAN, PROCESS_SCAN_INTERVAL_MS, nullptr);
    }
    else
    {
        KillTimer(hwnd, TIMER_PROCESS_SCAN);
    }
}

void TrayIcon::NotifyFileEvent()
{
    if (!initialized || hwnd == nullptr) return;
    PostMessage(hwnd, WM_APP_FILE_EVENT, 0, 0);
}

std::string TrayIcon::ShowApiKeyInputDialog()
{
    std::string currentApiKey = Globals::GetWakaTimeClient()->GetMaskedApiKey();

    WT_LOG("[TrayIcon] Opening WakaTime API key page...");
    ShellExecuteW(nullptr, L"open", L"https://wakatime.com/api-key",
                  nullptr, nullptr, SW_SHOWNORMAL);

    // 잠시 대기 (브라우저가 열릴 시간을 줌)
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    std::string message = "WakaTime API key page opened in browser !\n\n";
    message += "Steps to setup:\n";
    message += "1. Copy your API key from the opened webpage\n";
    message += "2. Click OK below\n";
    message += "3. API key will be read from clipboard\n\n";
    message += "Current API Key: " + (currentApiKey.empty() ? "Not set" : currentApiKey) + "\n\n";

    const auto wMessage = Utf8ToWideTray(message);

    if (const int result = MessageBoxW(hwnd, wMessage.c_str(), L"WakaTime API Key Setup",
        MB_OKCANCEL | MB_ICONINFORMATION | MB_TOPMOST); result == IDOK)
    {
        if (const std::string clipboardText = GetClipboardText(); !clipboardText.empty())
        {
            return clipboardText;
        }
        else
        {
            std::wstring retryMessage = L"No valid API key found in clipboard.\n\n";
            retryMessage += L"Please:\n";
            retryMessage += L"1. Go to the opened WakaTime page\n";
            retryMessage += L"2. Copy your API key\n";
            retryMessage += L"3. Try again from the tray menu\n\n";

            MessageBoxW(hwnd, retryMessage.c_str(), L"API Key Not Found",
                        MB_OK | MB_ICONWARNING | MB_TOPMOST);
        }
    }

    return "";
}

std::string TrayIcon::GetClipboardText()
{
    if (!OpenClipboard(hwnd))
    {
        const DWORD error = GetLastError();
        WT_ERR("[TrayIcon] Failed to open clipboard (Error: " << error << ")");
        return "";
    }

    // 유니코드 텍스트
    const HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    std::string result;

    if (hData != nullptr)
    {
        if (const auto pszText = static_cast<wchar_t *>(GlobalLock(hData)); pszText != nullptr)
        {
            if (const int len = WideCharToMultiByte(CP_UTF8, 0, pszText, -1,
                nullptr, 0, nullptr, nullptr); len > 1)
            {
                result.resize(len - 1);
                WideCharToMultiByte(CP_UTF8, 0, pszText, -1, &result[0], len, nullptr, nullptr);
            }
            GlobalUnlock(hData);
        }
    }

    CloseClipboard();

    if (!result.empty()) return result;

    return "";
}


LRESULT CALLBACK TrayIcon::WindowProc(const HWND hwnd, const UINT msg, const WPARAM wParam, const LPARAM lParam)
{
    TrayIcon *instance = nullptr;

    if (msg == WM_NCCREATE)
    {
        const auto *cs = (CREATESTRUCT *) lParam;
        instance = (TrayIcon *) cs->lpCreateParams;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR) instance);
    }
    else
    {
        instance = (TrayIcon *) GetWindowLongPtr(hwnd, GWLP_USERDATA);
    }

    if (instance)
    {
        return instance->HandleWindowMessage(hwnd, msg, wParam, lParam);
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

LRESULT TrayIcon::HandleWindowMessage(const HWND hwnd, const UINT msg, const WPARAM wParam, const LPARAM lParam)
{
    switch (msg)
    {
        case WM_TRAYICON:
            switch (lParam)
            {
            case WM_RBUTTONUP:
            {
                POINT pt;
                GetCursorPos(&pt);
                ShowContextMenu(pt.x, pt.y);
                break;
            }

            case WM_LBUTTONDBLCLK:
                    if (onShowStatus) onShowStatus();
                    break;
            }
            return 0;

        case WM_COMMAND:
            HandleMenuCommand(LOWORD(wParam));
            return 0;

        case WM_APP_FILE_EVENT:
            if (onFileEvent) onFileEvent();
            return 0;

        case WM_TIMER:
            if (wParam == TIMER_PROCESS_SCAN)
            {
                if (onProcessScan) onProcessScan();
            }
            else if (wParam == TIMER_PERIODIC_HEARTBEAT)
            {
                if (onPeriodicTick) onPeriodicTick();
            }
            return 0;

        case WM_DESTROY:
            return 0;  // PostQuitMessage 제거

        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

void TrayIcon::ShowBalloonNotification(const std::string &title,
                                       const std::string &message,
                                       const DWORD timeout,
                                       const DWORD iconType)
{
    if (!initialized) return;

    auto wTitle = Utf8ToWideTray(title);
    auto wMessage = Utf8ToWideTray(message);

    // 길이 제한
    if (wTitle.length() >= 64) wTitle = wTitle.substr(0, 60) + L"...";
    if (wMessage.length() >= 256) wMessage = wMessage.substr(0, 252) + L"...";

    // 풍선 알림 설정
    nid.uFlags |= NIF_INFO;
    wcscpy_s(nid.szInfoTitle, wTitle.c_str());
    wcscpy_s(nid.szInfo, wMessage.c_str());
    nid.dwInfoFlags = iconType;
    nid.uTimeout = timeout;

    Shell_NotifyIconW(NIM_MODIFY, &nid);

    // 플래그 제거 (다음 업데이트를 위해)
    nid.uFlags &= ~NIF_INFO;
}

void TrayIcon::SetActiveContext(const std::string &contextName)
{
    const bool contextChanged = (activeContext != contextName);
    activeContext = contextName;

    std::ostringstream tooltip;
    tooltip << "Creative WakaTime";
    if (!contextName.empty())
    {
        tooltip << " - " << contextName;
    }
    tooltip << " (" << totalHeartbeats << " heartbeats)";

    UpdateTooltip(tooltip.str());
    if (contextChanged)
    {
        RefreshStatusMenu();
    }
}

void TrayIcon::SetCurrentProject(const std::string &projectName)
{
    SetActiveContext(projectName);
}

void TrayIcon::IncrementHeartbeats()
{
    totalHeartbeats++;
    SetActiveContext(activeContext);
}

void TrayIcon::SetMonitoringState(const bool monitoring)
{
    isMonitoring = monitoring;

    std::ostringstream tooltip;
    tooltip << "Creative WakaTime - " << (monitoring ? "Active" : "Paused");
    if (!activeContext.empty())
    {
        tooltip << " - " << activeContext;
    }

    UpdateTooltip(tooltip.str());
    RefreshStatusMenu();
}

void TrayIcon::Shutdown()
{
    if (!initialized) return;

    WT_LOG("[TrayIcon] Shutting down...");

    Shell_NotifyIconW(NIM_DELETE, &nid);

    if (hMenu)
    {
        DestroyMenu(hMenu);
        hMenu = nullptr;
    }

    if (nid.hIcon)
    {
        DestroyIcon(nid.hIcon);
        nid.hIcon = nullptr;
    }

    if (hwnd)
    {
        DestroyWindow(hwnd);
        hwnd = nullptr;
    }

    initialized = false;
    WT_LOG("[TrayIcon] Shutdown complete");
}

#pragma region Notification

void TrayIcon::ShowErrorNotification(const std::string &message)
{
    ShowBalloonNotification("Creative WakaTime Error", message, 5000, NIIF_ERROR);
}

void TrayIcon::ShowInfoNotification(const std::string &message)
{
    ShowBalloonNotification("Creative WakaTime", message, 2000, NIIF_INFO);
}

#pragma endregion Notification

#pragma region Callbacks

void TrayIcon::SetExitCallback(const std::function<void()> &callback)
{
    onExit = callback;
}

void TrayIcon::SetShowStatusCallback(const std::function<void()> &callback)
{
    onShowStatus = callback;
}

void TrayIcon::SetToggleMonitoringCallback(const std::function<void(bool)> &callback)
{
    onToggleMonitoring = callback;
}

void TrayIcon::SetOpenDashboardCallback(const std::function<void()> &callback)
{
    onOpenDashboard = callback;
}

void TrayIcon::SetApiKeyChangeCallback(const std::function<void(const std::string &)> &callback)
{
    onApiKeyChange = callback;
}

void TrayIcon::SetToggleAppCallback(const std::function<void(const std::string &, bool)> &callback)
{
    onToggleApp = callback;
}

void TrayIcon::SetFileEventCallback(const std::function<void()> &callback)
{
    onFileEvent = callback;
}

void TrayIcon::SetProcessScanCallback(const std::function<void()> &callback)
{
    onProcessScan = callback;
}

void TrayIcon::SetPeriodicTickCallback(const std::function<void()> &callback)
{
    onPeriodicTick = callback;
}

#pragma endregion Callbacks
