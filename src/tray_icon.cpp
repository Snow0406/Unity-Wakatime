#include "tray_icon.h"
#include "file_watcher.h"
#include "wakatime_client.h"

#include <wincodec.h>
#include <comdef.h>

TrayIcon::TrayIcon() : hwnd(nullptr),
                       hMenu(nullptr),
                       isMonitoring(false),
                       totalHeartbeats(0),
                       initialized(false)
{
    // NOTIFYICONDATA 구조체 초기화
    ZeroMemory(&nid, sizeof(NOTIFYICONDATAW));
    ZeroMemory(&wc, sizeof(WNDCLASSW));

    std::cout << "[TrayIcon] Created" << std::endl;
}

TrayIcon::~TrayIcon()
{
    Shutdown();

    std::cout << "[TrayIcon] Destroyed" << std::endl;
}

bool TrayIcon::Initialize(const std::string &appName)
{
    std::cout << "[TrayIcon] Initializing: " << appName << std::endl;

    // 숨겨진 창 생성
    if (!CreateHiddenWindow())
    {
        std::cerr << "[TrayIcon] Failed to create hidden window" << std::endl;
        return false;
    }

    // 트레이 아이콘 생성
    if (!CreateTrayIcon())
    {
        std::cerr << "[TrayIcon] Failed to create tray icon" << std::endl;
        return false;
    }

    // 컨텍스트 메뉴 생성
    hMenu = CreateContextMenu();
    if (!hMenu)
    {
        std::cerr << "[TrayIcon] Failed to create context menu" << std::endl;
        return false;
    }

    // 초기 툴팁 설정
    UpdateTooltip(appName + " - Ready");

    initialized = true;
    std::cout << "[TrayIcon] Initialized successfully" << std::endl;

    return true;
}

bool TrayIcon::CreateHiddenWindow()
{
    // 창 클래스 등록
    wc.lpfnWndProc = WindowProc; // 윈도우 프로시저
    wc.hInstance = GetModuleHandle(nullptr); // 현재 모듈 핸들
    wc.lpszClassName = L"UnityWakaTimeTray"; // 클래스 이름
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH) (COLOR_WINDOW + 1);

    if (!RegisterClassW(&wc))
    {
        if (const DWORD error = GetLastError(); error != ERROR_CLASS_ALREADY_EXISTS)
        {
            // 이미 등록된 경우는 정상
            std::cerr << "[TrayIcon] RegisterClass failed (Error: " << error << ")" << std::endl;
            return false;
        }
    }

    // 숨겨진 창 생성
    // WS_OVERLAPPED: 기본 창 스타일
    // CW_USEDEFAULT: 기본 위치/크기
    hwnd = CreateWindowW(
        L"UnityWakaTimeTray",           // 클래스 이름
        L"Unity WakaTime",              // 창 제목
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
        std::cerr << "[TrayIcon] CreateWindow failed (Error: " << error << ")" << std::endl;
        return false;
    }

    std::cout << "[TrayIcon] Hidden window created" << std::endl;
    return true;
}

bool TrayIcon::CreateTrayIcon() {
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_INFO;
    nid.uCallbackMessage = WM_TRAYICON;

    nid.hIcon = LoadPngIcon("logo_32.png");

    wcscpy_s(nid.szTip, L"Unity WakaTime - Starting...");

    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        DWORD error = GetLastError();
        std::cerr << "[TrayIcon] Shell_NotifyIconW(NIM_ADD) failed (Error: " << error << ")" << std::endl;
        return false;
    }

    std::cout << "[TrayIcon] Tray icon added to system tray" << std::endl;
    return true;
}

HICON TrayIcon::LoadPngIcon(const std::string& filePath) {
    std::cout << "[TrayIcon] Loading PNG icon using WIC: " << filePath << std::endl;

    // 파일 존재 확인
    if (!fs::exists(filePath)) {
        std::cerr << "[TrayIcon] PNG file not found: " << filePath << std::endl;
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
        std::cerr << "[TrayIcon] Failed to create WIC factory (HRESULT: 0x"
                  << std::hex << hr << ")" << std::endl;
        if (comInitialized) CoUninitialize();
        return LoadIcon(nullptr, IDI_APPLICATION);
    }

    std::cout << "[TrayIcon] ✅ WIC factory created successfully" << std::endl;

    const std::wstring wFilePath(filePath.begin(), filePath.end());

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
        std::cerr << "[TrayIcon] Failed to create PNG decoder (HRESULT: 0x"
                  << std::hex << hr << ")" << std::endl;
        pFactory->Release();
        if (comInitialized) CoUninitialize();
        return LoadIcon(nullptr, IDI_APPLICATION);
    }

    // 첫 번째 프레임 가져오기
    IWICBitmapFrameDecode* pFrame = nullptr;
    hr = pDecoder->GetFrame(0, &pFrame);

    if (FAILED(hr)) {
        std::cerr << "[TrayIcon] Failed to get frame from PNG" << std::endl;
        pDecoder->Release();
        pFactory->Release();
        if (comInitialized) CoUninitialize();
        return LoadIcon(nullptr, IDI_APPLICATION);
    }

    // 원본 크기 확인
    UINT originalWidth, originalHeight;
    pFrame->GetSize(&originalWidth, &originalHeight);
    std::cout << "[TrayIcon] Original PNG size: " << originalWidth << "x" << originalHeight << std::endl;

    // 트레이 아이콘 크기 (시스템에서 권장하는 크기)
    int iconSize = GetSystemMetrics(SM_CXSMICON);  // 보통 16x16
    if (iconSize <= 0) iconSize = 32;  // 기본값

    std::cout << "[TrayIcon] System tray icon size: " << iconSize << "x" << iconSize << std::endl;

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
                    std::cout << "[TrayIcon] ✅ PNG icon converted to HICON successfully!" << std::endl;
                } else {
                    DWORD error = GetLastError();
                    std::cerr << "[TrayIcon] CreateIconIndirect failed (Error: " << error << ")" << std::endl;
                }
            } else {
                std::cerr << "[TrayIcon] Failed to copy pixels from WIC converter" << std::endl;
            }

            DeleteObject(hBitmap);
        } else {
            std::cerr << "[TrayIcon] Failed to create DIB section" << std::endl;
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
        std::cout << "[TrayIcon] ✅ PNG icon loaded successfully using WIC!" << std::endl;
        return hIcon;
    } else {
        std::cerr << "[TrayIcon] ❌ Failed to load PNG, using fallback icon" << std::endl;
        return LoadIcon(nullptr, IDI_APPLICATION);
    }
}

HMENU TrayIcon::CreateContextMenu()
{
    const HMENU menu = CreatePopupMenu();

    HMENU statusSubMenu = CreateStatusSubMenu();
    AppendMenuW(menu, MF_STRING | MF_POPUP, (UINT_PTR) statusSubMenu, L"📊 Status");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING, IDM_TOGGLE_MONITORING, L"Pause Monitoring");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING, IDM_OPEN_DASHBOARD, L"Open WakaTime Dashboard");
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"🔑 Setup API Key");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING, IDM_GITHUB, L"ℹ️ Unity WakaTime v1.0.0");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

    return menu;
}


HMENU TrayIcon::CreateStatusSubMenu()
{
    const HMENU subMenu = CreatePopupMenu();

    // === API Key 상태 ===
    if (Globals::GetWakaTimeClient())
    {
        std::string maskedKey = Globals::GetWakaTimeClient()->GetMaskedApiKey();
        const std::wstring apiKeyInfo = L"🔑 API Key: " + std::wstring(maskedKey.begin(), maskedKey.end());
        AppendMenuW(subMenu, MF_STRING | MF_GRAYED, 0, apiKeyInfo.c_str());
    }
    else
    {
        AppendMenuW(subMenu, MF_STRING | MF_GRAYED, 0, L"🔑 API Key: Not configured");
    }

    AppendMenuW(subMenu, MF_SEPARATOR, 0, nullptr);

    // === 모니터링 상태 ===
    const std::wstring monitoringStatus = isMonitoring ? L"✅ Monitoring: Active" : L"⏸️ Monitoring: Paused";
    AppendMenuW(subMenu, MF_STRING | MF_GRAYED, 0, monitoringStatus.c_str());

    // === 현재 프로젝트 ===
    if (!currentProject.empty())
    {
        const std::wstring projectInfo = L"🎮 Current: " + std::wstring(currentProject.begin(), currentProject.end());
        AppendMenuW(subMenu, MF_STRING | MF_GRAYED, 0, projectInfo.c_str());
    } else
    {
        AppendMenuW(subMenu, MF_STRING | MF_GRAYED, 0, L"🎮 No Unity project detected");
    }

    // === Heartbeat 통계 ===
    const std::wstring heartbeatInfo = L"💓 Total Heartbeats: " + std::to_wstring(totalHeartbeats);
    AppendMenuW(subMenu, MF_STRING | MF_GRAYED, 0, heartbeatInfo.c_str());
    AppendMenuW(subMenu, MF_SEPARATOR, 0, nullptr);

    // === WakaTime 통계 ===
    if (Globals::GetWakaTimeClient())
    {
        int sent, failed;
        Globals::GetWakaTimeClient()->GetStats(sent, failed);

        const std::wstring sentInfo = L"📤 Sent: " + std::to_wstring(sent);
        const std::wstring failedInfo = L"❌ Failed: " + std::to_wstring(failed);

        AppendMenuW(subMenu, MF_STRING | MF_GRAYED, 0, sentInfo.c_str());
        if (failed > 0)
        {
            AppendMenuW(subMenu, MF_STRING | MF_GRAYED, 0, failedInfo.c_str());
        }

        // 성공률 계산
        if (sent + failed > 0)
        {
            const int successRate = (sent * 100) / (sent + failed);
            const std::wstring rateInfo = L"📊 Success Rate: " + std::to_wstring(successRate) + L"%";
            AppendMenuW(subMenu, MF_STRING | MF_GRAYED, 0, rateInfo.c_str());
        }
    }
    else
    {
        AppendMenuW(subMenu, MF_STRING | MF_GRAYED, 0, L"⚠️ WakaTime client not initialized");
    }

    AppendMenuW(subMenu, MF_SEPARATOR, 0, nullptr);

    // === 파일 감시 상태 ===
    if (Globals::GetFileWatcher())
    {
        const size_t watchedCount = Globals::GetFileWatcher()->GetWatchedProjectCount();
        const std::wstring watchInfo = L"👁️ Watching: " + std::to_wstring(watchedCount) + L" projects";
        AppendMenuW(subMenu, MF_STRING | MF_GRAYED, 0, watchInfo.c_str());

        // 감시 중인 프로젝트 목록 (최대 3개)
        const auto watchedProjects = Globals::GetFileWatcher()->GetWatchedProjects();
        for (size_t i = 0; i < std::min((size_t) 3, watchedProjects.size()); i++)
        {
            std::string projectName = std::filesystem::path(watchedProjects[i]).filename().string();
            std::wstring projectItem = L"  📁 " + std::wstring(projectName.begin(), projectName.end());
            AppendMenuW(subMenu, MF_STRING | MF_GRAYED, 0, projectItem.c_str());
        }

        if (watchedProjects.size() > 3)
        {
            const std::wstring moreInfo = L"  ... and " + std::to_wstring(watchedProjects.size() - 3) + L" more";
            AppendMenuW(subMenu, MF_STRING | MF_GRAYED, 0, moreInfo.c_str());
        }
    }

    AppendMenuW(subMenu, MF_SEPARATOR, 0, nullptr);

    // === 액션 항목들 ===
    AppendMenuW(subMenu, MF_STRING, IDM_SHOW_STATUS, L"🔄 Refresh Status");

    return subMenu;
}


void TrayIcon::UpdateContextMenu()
{
    if (!hMenu) return;

    // 기존 서브메뉴 찾기 및 제거
    if (const HMENU oldSubMenu = GetSubMenu(hMenu, 0))
    {
        RemoveMenu(hMenu, 0, MF_BYPOSITION);
        DestroyMenu(oldSubMenu);
    }

    // 새로운 서브메뉴 생성 및 삽입
    HMENU newStatusSubMenu = CreateStatusSubMenu();
    InsertMenuW(hMenu, 0, MF_BYPOSITION | MF_STRING | MF_POPUP,
                (UINT_PTR)newStatusSubMenu, L"📊 Status");
}

void TrayIcon::OpenGitHubRepository() {
    const auto githubUrl = "https://github.com/Snow0406/Unity-Wakatime";

    const std::wstring wGithubUrl(githubUrl, githubUrl + strlen(githubUrl));
    std::cout << "[TrayIcon] Opening GitHub repository: " << githubUrl << std::endl;

    ShellExecuteW(nullptr, L"open", wGithubUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void TrayIcon::RefreshStatusMenu()
{
    UpdateContextMenu();
}


void TrayIcon::UpdateTooltip(const std::string &tooltip)
{
    if (!initialized) return;

    // std::string을 std::wstring으로 변환
    std::wstring wTooltip(tooltip.begin(), tooltip.end());

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
    if (!hMenu)
    {
        std::cout << "[TrayIcon] hMenu is NULL!" << std::endl;
        return;
    }

    // 모니터링 상태에 따라 메뉴 텍스트 변경
    ModifyMenuW(hMenu, IDM_TOGGLE_MONITORING, MF_BYCOMMAND | MF_STRING,
                IDM_TOGGLE_MONITORING,
                isMonitoring ? L"Pause Monitoring" : L"Resume Monitoring");

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

        case IDM_GITHUB:
            OpenGitHubRepository();
            break;

        case IDM_EXIT:
            if (onExit) onExit();
            break;
    }
}


int TrayIcon::ProcessMessages()
{
    if (!initialized) return 0;

    MSG msg;
    int processedCount = 0;

    // PeekMessage로 논블로킹 방식으로 메시지 처리
    while (PeekMessage(&msg, hwnd, 0, 0, PM_REMOVE))
    {
        processedCount++;
        TranslateMessage(&msg);
        DispatchMessage(&msg);

        if (msg.message == WM_QUIT)
        {
            break;
        }
    }

    return processedCount;
}

std::string TrayIcon::ShowApiKeyInputDialog()
{
    std::string currentApiKey = Globals::GetWakaTimeClient()->GetMaskedApiKey();

    std::cout << "[TrayIcon] Opening WakaTime API key page..." << std::endl;
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

    const std::wstring wMessage(message.begin(), message.end());

    if (const int result = MessageBoxW(hwnd, wMessage.c_str(), L"🔑 WakaTime API Key Setup",
        MB_OKCANCEL | MB_ICONINFORMATION | MB_TOPMOST); result == IDOK)
    {
        if (const std::string clipboardText = GetClipboardText(); !clipboardText.empty())
        {
            return clipboardText;
        }
        else
        {
            std::wstring retryMessage = L"❌ No valid API key found in clipboard!\n\n";
            retryMessage += L"Please:\n";
            retryMessage += L"1. Go to the opened WakaTime page\n";
            retryMessage += L"2. Copy your API key\n";
            retryMessage += L"3. Try again from the tray menu\n\n";

            MessageBoxW(hwnd, retryMessage.c_str(), L"⚠️ API Key Not Found",
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
        std::cerr << "[TrayIcon] Failed to open clipboard (Error: " << error << ")" << std::endl;
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

    // 문자열을 와이드 문자로 변환
    std::wstring wTitle(title.begin(), title.end());
    std::wstring wMessage(message.begin(), message.end());

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

void TrayIcon::SetCurrentProject(const std::string &projectName)
{
    currentProject = projectName;

    std::ostringstream tooltip;
    tooltip << "Unity WakaTime";
    if (!projectName.empty())
    {
        tooltip << " - " << projectName;
    }
    tooltip << " (" << totalHeartbeats << " heartbeats)";

    UpdateTooltip(tooltip.str());
    RefreshStatusMenu();
}

void TrayIcon::IncrementHeartbeats()
{
    totalHeartbeats++;
    SetCurrentProject(currentProject);
}

void TrayIcon::SetMonitoringState(const bool monitoring)
{
    isMonitoring = monitoring;

    std::ostringstream tooltip;
    tooltip << "Unity WakaTime - " << (monitoring ? "Active" : "Paused");
    if (!currentProject.empty())
    {
        tooltip << " - " << currentProject;
    }

    UpdateTooltip(tooltip.str());
    RefreshStatusMenu();
}

void TrayIcon::Shutdown()
{
    if (!initialized) return;

    std::cout << "[TrayIcon] Shutting down..." << std::endl;

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
    std::cout << "[TrayIcon] Shutdown complete" << std::endl;
}

#pragma region Notification

void TrayIcon::ShowErrorNotification(const std::string &message)
{
    ShowBalloonNotification("Unity WakaTime Error", message, 5000, NIIF_ERROR);
}

void TrayIcon::ShowInfoNotification(const std::string &message)
{
    ShowBalloonNotification("Unity WakaTime", message, 2000, NIIF_INFO);
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

void TrayIcon::SetShowSettingsCallback(const std::function<void()> &callback)
{
    onShowSettings = callback;
}

void TrayIcon::SetApiKeyChangeCallback(const std::function<void(const std::string &)> &callback)
{
    onApiKeyChange = callback;
}

#pragma endregion Callbacks
