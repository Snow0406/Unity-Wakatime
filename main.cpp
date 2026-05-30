#include "globals.h"
#include "process_monitor.h"
#include "file_watcher.h"
#include "wakatime_client.h"
#include "tray_icon.h"
#include "unity_focus_detector.h"
#include "windows_dark_mode.h"

WakaTimeClient *g_wakatimeClient = nullptr;
FileWatcher *g_fileWatcher = nullptr;
ProcessMonitor *g_processMonitor = nullptr;
TrayIcon *g_trayIcon = nullptr;
UnityFocusDetector *g_unityFocusDetector = nullptr;
std::atomic<bool> g_shouldExit(false);

// Globals 네임스페이스 구현
namespace Globals
{
    void Cleanup()
    {
        g_wakatimeClient = nullptr;
        g_fileWatcher = nullptr;
        g_processMonitor = nullptr;
        g_trayIcon = nullptr;
        g_unityFocusDetector = nullptr;
    }
}

// 파일 변경 이벤트 처리 - TrayIcon도 업데이트
void OnFileChanged(const FileChangeEvent &event)
{
    WT_LOG("[HEARTBEAT] " << event.fileName << " (" << event.projectName << ")");

    // WakaTime API에 heartbeat 전송
    if (g_wakatimeClient)
    {
        if (g_wakatimeClient->IsInitialized())
        {
            g_wakatimeClient->SendHeartbeatFromEvent(event);
            WT_LOG("[HEARTBEAT] ✅ Sent to WakaTime API");
        }
        else
        {
            WT_LOG("[HEARTBEAT] ⚠️ Skipped - WakaTime client not initialized");
        }
    }

    // 트레이 아이콘 업데이트
    if (g_trayIcon)
    {
        g_trayIcon->IncrementHeartbeats();
        g_trayIcon->SetCurrentProject(event.projectName);
    }
}

// 트레이 아이콘 콜백 함수들
void OnTrayExit()
{
    WT_LOG("[Main] Exit requested from tray");
    Globals::RequestExit();
    // 메시지 펌프(GetMessage)를 깨워 정상 종료시킨다. 트레이 메뉴 핸들러는
    // 메인 스레드(WndProc)에서 동기 호출되므로 여기서 PostQuitMessage가 안전하다.
    PostQuitMessage(0);
}

void OnTrayShowStatus()
{
    if (g_trayIcon)
    {
        g_trayIcon->RefreshStatusMenu();
        g_trayIcon->ShowInfoNotification("Status menu updated!");
    }
}

void OnTrayToggleMonitoring(const bool enabled)
{
    WT_LOG("[Main] Monitoring " << (enabled ? "enabled" : "disabled"));

    if (g_trayIcon)
    {
        g_trayIcon->SetMonitoringState(enabled);
        g_trayIcon->ShowInfoNotification(
            enabled ? "Monitoring resumed" : "Monitoring paused"
        );
    }
}

void OnTrayOpenDashboard()
{
    WT_LOG("[Main] Opening WakaTime dashboard");

    ShellExecuteW(nullptr, L"open", L"https://wakatime.com/dashboard",
                  nullptr, nullptr, SW_SHOWNORMAL);
}

void OnTrayShowSettings()
{
    WT_LOG("[Main] Settings requested");

    if (g_trayIcon && g_wakatimeClient)
    {
        std::ostringstream settings;
        settings << "API Key: " << g_wakatimeClient->GetMaskedApiKey() << "\n"
                << "Watching " << g_fileWatcher->GetWatchedProjectCount() << " Unity projects";

        g_trayIcon->ShowInfoNotification(settings.str());
    }
}

void OnApiKeyChanged(const std::string &newApiKey)
{
    WT_LOG("[Main] API Key changed, updating WakaTime client...");

    if (g_wakatimeClient)
    {
        const bool success = g_wakatimeClient->ReInitialize(newApiKey);

        if (g_trayIcon)
        {
            if (success)
            {
                g_trayIcon->ShowInfoNotification("✅ API Key saved and client reinitialized!");
                g_trayIcon->RefreshStatusMenu();
                WT_LOG("[Main] ✅ WakaTime client successfully reinitialized");
            }
            else
            {
                g_trayIcon->ShowErrorNotification("❌ Failed to initialize with new API key");
                WT_ERR("[Main] ❌ WakaTime client reinitialization failed");
            }
        }
    }
}

void OnUnityFocusHeartbeat()
{
    if (!g_fileWatcher || !g_wakatimeClient) return;
    if (!g_wakatimeClient->IsInitialized()) return;

    const auto& watchedProjects = g_fileWatcher->GetWatchedProjects();
    if (watchedProjects.empty()) return;

    const auto& project = watchedProjects[0];
    g_wakatimeClient->SendHeartbeat(
        project.projectPath,
        project.projectName,
        project.unityVersion,
        false
        );

    if (g_trayIcon)
    {
        g_trayIcon->IncrementHeartbeats();
        g_trayIcon->SetCurrentProject(project.projectName);
    }
}

void HandleNewUnityInstances(const std::vector<UnityInstance> &newInstances)
{
    for (const auto &instance: newInstances)
    {
        WT_LOG("[Main] New Unity instance detected: " << instance.projectName
                << " (Unity " << instance.editorVersion << ")");

        if (g_fileWatcher && g_fileWatcher->StartWatching(instance.projectPath, instance.projectName, instance.editorVersion))
        {
            WT_LOG("[Main] Started watching: " << instance.projectName);

            if (g_trayIcon)
            {
                g_trayIcon->SetCurrentProject(instance.projectName);
                g_trayIcon->ShowInfoNotification("New Unity project: " + instance.projectName +
                                                 " (Unity " + instance.editorVersion + ")");
            }
        }
        else
        {
            WT_LOG("[Main] Failed to start watching: " << instance.projectName);
        }
    }
}

void HandleClosedUnityInstances(const std::vector<UnityInstance> &closedInstances)
{
    for (const auto &instance: closedInstances)
    {
        WT_LOG("[Main] Unity instance closed: " << instance.projectName);

        if (g_fileWatcher)
        {
            g_fileWatcher->StopWatching(instance.projectPath);
        }

        if (g_trayIcon)
        {
            g_trayIcon->ShowInfoNotification("Unity project closed: " + instance.projectName);

            // 다른 활성 프로젝트로 전환
            if (g_fileWatcher)
            {
                if (const auto& remainingProjects = g_fileWatcher->GetWatchedProjects(); !remainingProjects.empty())
                {
                    const auto& projectName = remainingProjects[0].projectName;
                    g_trayIcon->SetCurrentProject(projectName);
                    WT_LOG("[Main] Switched to remaining project: " << projectName);
                } else
                {
                    g_trayIcon->SetCurrentProject(""); // 모든 프로젝트 종료
                    WT_LOG("[Main] No Unity projects are being watched");
                }
            }
        }
    }
}

void InitialUnityProjectScan()
{
    if (!g_processMonitor || !g_fileWatcher) return;

    WT_LOG("[Main] Performing initial Unity project scan...");

    const auto& instances = g_processMonitor->ScanUnityProcesses();

    if (instances.empty())
    {
        WT_LOG("[Main] No Unity processes found during initial scan");
        return;
    }

    for (const auto &instance: instances)
    {
        if (g_fileWatcher->StartWatching(instance.projectPath, instance.projectName, instance.editorVersion))
        {
            WT_LOG("[Main] ✅ Started watching: " << instance.projectName
                    << " (Unity " << instance.editorVersion << ")");

            if (g_trayIcon)
            {
                g_trayIcon->SetCurrentProject(instance.projectName);
            }
        }
    }

    WT_LOG("[Main] Initial scan complete. Watching " << instances.size() << " Unity projects");
}

// 포그라운드 창 변경 이벤트 콜백 (SetWinEventHook).
// WINEVENT_OUTOFCONTEXT라 메인 스레드의 메시지 펌프 중 디스패치되므로 마샬링 불필요.
void CALLBACK FocusWinEventProc(HWINEVENTHOOK, const DWORD event, const HWND hwnd,
                                const LONG idObject, LONG, DWORD, DWORD)
{
    if (event != EVENT_SYSTEM_FOREGROUND || idObject != OBJID_WINDOW) return;
    if (g_unityFocusDetector) g_unityFocusDetector->OnForegroundChanged(hwnd);
}

int main()
{
    WT_LOG("[Main] Unity WakaTime Monitor Starting...");
    const bool darkModeAvailable = WindowsDarkMode::EnableForApp();
    WT_LOG("[Main] Dark mode menu opt-in: "
              << (darkModeAvailable ? "enabled" : "not available"));

    TrayIcon trayIcon;
    g_trayIcon = &trayIcon;

    if (!trayIcon.Initialize("Unity WakaTime"))
    {
        WT_ERR("[Main] Failed to initialize tray icon!");
        return 1;
    }

    // 트레이 콜백 설정
    trayIcon.SetExitCallback(OnTrayExit);
    trayIcon.SetShowStatusCallback(OnTrayShowStatus);
    trayIcon.SetToggleMonitoringCallback(OnTrayToggleMonitoring);
    trayIcon.SetOpenDashboardCallback(OnTrayOpenDashboard);
    trayIcon.SetShowSettingsCallback(OnTrayShowSettings);
    trayIcon.SetApiKeyChangeCallback(OnApiKeyChanged);

    trayIcon.ShowInfoNotification("Unity WakaTime started !");

    WakaTimeClient wakatimeClient;
    g_wakatimeClient = &wakatimeClient;

    if (!wakatimeClient.Initialize())
    {
        WT_ERR("[Main] Failed to initialize WakaTime client!");
        trayIcon.ShowErrorNotification("WakaTime client not initialized. Click 'Setup API Key' in menu.");
    }

    ProcessMonitor processMonitor;
    FileWatcher fileWatcher;
    g_processMonitor = &processMonitor;
    g_fileWatcher = &fileWatcher;

    fileWatcher.SetChangeCallback(OnFileChanged);
    // 워커 스레드의 파일 변경 → 메인 스레드로 PostMessage 마샬링 (InitialScan 이전에 설치)
    fileWatcher.SetNotifyCallback([&trayIcon]()
    {
        trayIcon.NotifyFileEvent();
    });

    UnityFocusDetector unityFocusDetector;
    g_unityFocusDetector = &unityFocusDetector;

    unityFocusDetector.SetFocusCallback(OnUnityFocusHeartbeat);
    unityFocusDetector.SetUnfocusCallback([]() {});
    unityFocusDetector.SetPeriodicHeartbeatCallback(OnUnityFocusHeartbeat);

    InitialUnityProjectScan();

    trayIcon.SetMonitoringState(true);

    WT_LOG("\n[Main] Unity WakaTime is now running in background!");

    // 이벤트 허브 콜백 배선 (TrayIcon의 메시지 펌프에서 디스패치)
    trayIcon.SetFileEventCallback([]()
    {
        if (g_fileWatcher) g_fileWatcher->DrainPendingEvents();
    });

    trayIcon.SetProcessScanCallback([&processMonitor]()
    {
        std::vector<UnityInstance> started;
        std::vector<UnityInstance> closed;
        processMonitor.PollChanges(started, closed);
        if (!started.empty()) HandleNewUnityInstances(started);
        if (!closed.empty()) HandleClosedUnityInstances(closed);
    });

    trayIcon.SetPeriodicTickCallback([]()
    {
        if (g_unityFocusDetector) g_unityFocusDetector->SendPeriodicHeartbeat();
    });

    // 포커스 추적: SetWinEventHook(OUTOFCONTEXT) → 콜백이 메인 펌프에서 디스패치됨 (매초 폴링 제거)
    const HWINEVENTHOOK focusHook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        nullptr, FocusWinEventProc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    // 훅 설치 시점에 이미 Unity가 포그라운드일 수 있으므로 초기 상태 1회 캡처
    unityFocusDetector.OnForegroundChanged(GetForegroundWindow());

    // 메시지 펌프: idle 시 GetMessage가 커널에서 블록되어 CPU ≈ 0,
    // 파일/포커스/타이머/트레이 이벤트에만 깨어난다. WM_QUIT까지 블록.
    trayIcon.RunMessageLoop();

    if (focusHook) UnhookWinEvent(focusHook);

    WT_LOG("\n[Main] Shutting down Unity WakaTime...");
    trayIcon.ShowInfoNotification("Unity WakaTime shutting down...");

    // 남은 heartbeat 전송
    if (g_wakatimeClient)
    {
        if (g_fileWatcher)
        {
            g_fileWatcher->DrainPendingEvents(2048);
        }

        WT_LOG("[Main] Flushing remaining heartbeats...");
        wakatimeClient.FlushQueue();
    }

    // 모든 파일 감시 중지
    if (g_fileWatcher)
    {
        WT_LOG("[Main] Stopping all file watchers...");
        g_fileWatcher->StopAllWatching();
    }

    Globals::Cleanup();

    WT_LOG("[Main] Unity WakaTime stopped gracefully.");
    return 0;
}
