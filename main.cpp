#include "globals.h"
#include "process_monitor.h"
#include "file_watcher.h"
#include "wakatime_client.h"
#include "app_registry.h"
#include "tray_icon.h"
#include "focus_detector.h"
#include "windows_dark_mode.h"

WakaTimeClient *g_wakatimeClient = nullptr;
FileWatcher *g_fileWatcher = nullptr;
ProcessMonitor *g_processMonitor = nullptr;
TrayIcon *g_trayIcon = nullptr;
FocusDetector *g_focusDetector = nullptr;
std::atomic<bool> g_shouldExit(false);
std::atomic<bool> g_monitoringPaused(false);

// Globals 네임스페이스 구현
namespace Globals
{
    void Cleanup()
    {
        g_wakatimeClient = nullptr;
        g_fileWatcher = nullptr;
        g_processMonitor = nullptr;
        g_trayIcon = nullptr;
        g_focusDetector = nullptr;
    }
}

namespace
{
    // 트레이 컨텍스트 라벨: "Unity - MyProject"
    std::string ContextLabel(const std::string &appId, const std::string &project)
    {
        const AppDefinition *def = AppRegistry::FindById(appId);
        std::string label = def ? def->displayName : appId;
        if (!project.empty()) label += " - " + project;
        return label;
    }

    // 활성 앱이 하나라도 있으면 프로세스 스캔 타이머를 켜고, 0이면 끈다.
    void UpdateProcessScanTimer()
    {
        if (g_trayIcon) g_trayIcon->SetProcessScanActive(AppRegistry::EnabledCount() > 0);
    }
}

// 파일 변경 이벤트 처리 (DirectoryWatch 앱)
void OnFileChanged(const FileChangeEvent &event)
{
    WT_LOG("[HEARTBEAT] " << event.fileName << " (" << event.projectName << ")");

    bool queued = false;
    if (g_wakatimeClient && g_wakatimeClient->IsInitialized())
    {
        queued = g_wakatimeClient->SendHeartbeatFromEvent(event);
    }

    if (g_trayIcon)
    {
        if (queued) g_trayIcon->IncrementHeartbeats();
        g_trayIcon->SetActiveContext(ContextLabel(event.appId, event.projectName));
    }
}

// focus heartbeat 처리 (Unity 포커스 / WindowTitle 앱)
void OnFocusHeartbeat(const std::string &appId, const std::string &entity,
                      const std::string &project, const std::string &editorVersion)
{
    if (!g_wakatimeClient || !g_wakatimeClient->IsInitialized()) return;

    const bool queued = g_wakatimeClient->SendHeartbeat(appId, entity, project, editorVersion, false);

    if (g_trayIcon)
    {
        if (queued) g_trayIcon->IncrementHeartbeats();
        g_trayIcon->SetActiveContext(ContextLabel(appId, project));
    }
}

void HandleNewInstances(const std::vector<AppInstance> &started)
{
    for (const auto &inst: started)
    {
        const AppDefinition *def = AppRegistry::FindById(inst.appId);
        if (def == nullptr) continue;

        if (def->strategy == TrackStrategy::DirectoryWatch)
        {
            WT_LOG("[Main] New " << def->displayName << " instance: " << inst.projectName);

            if (g_fileWatcher && g_fileWatcher->StartWatching(inst.appId, inst.projectPath, inst.projectName, inst.editorVersion))
            {
                if (g_trayIcon)
                {
                    g_trayIcon->SetActiveContext(ContextLabel(inst.appId, inst.projectName));
                    std::string note = def->displayName + " detected: " + inst.projectName;
                    if (!inst.editorVersion.empty()) note += " (" + inst.editorVersion + ")";
                    g_trayIcon->ShowInfoNotification(note);
                }
            }
        }
        else
        {
            // WindowTitle: 파일 감시 없음. 포커스 추적이 heartbeat를 담당.
            WT_LOG("[Main] New " << def->displayName << " instance detected");
            if (g_trayIcon)
            {
                std::string note = def->displayName + " detected";
                if (!inst.projectName.empty()) note += ": " + inst.projectName;
                g_trayIcon->ShowInfoNotification(note);
            }
        }
    }
}

void HandleClosedInstances(const std::vector<AppInstance> &closed)
{
    for (const auto &inst: closed)
    {
        const AppDefinition *def = AppRegistry::FindById(inst.appId);
        const std::string name = def ? def->displayName : inst.appId;

        WT_LOG("[Main] " << name << " instance closed: " << inst.projectName);

        if (def && def->strategy == TrackStrategy::DirectoryWatch && g_fileWatcher)
        {
            g_fileWatcher->StopWatching(inst.projectPath);
        }
        if (g_focusDetector)
        {
            g_focusDetector->ClearFocusForProcess(inst.processId);
        }

        if (g_trayIcon)
        {
            std::string note = name + " closed";
            if (!inst.projectName.empty()) note += ": " + inst.projectName;
            g_trayIcon->ShowInfoNotification(note);

            if (g_fileWatcher)
            {
                if (const auto &remaining = g_fileWatcher->GetWatchedProjects(); !remaining.empty())
                {
                    g_trayIcon->SetActiveContext(ContextLabel(remaining[0].appId, remaining[0].projectName));
                }
                else
                {
                    g_trayIcon->SetActiveContext("");
                }
            }
        }
    }
}

void InitialScan()
{
    if (!g_processMonitor) return;

    WT_LOG("[Main] Performing initial process scan...");
    const auto &instances = g_processMonitor->ScanProcesses();
    HandleNewInstances(instances);
    WT_LOG("[Main] Initial scan complete. Found " << instances.size() << " instances");
}

// 트레이 콜백 함수들
void OnTrayExit()
{
    WT_LOG("[Main] Exit requested from tray");
    Globals::RequestExit();
    PostQuitMessage(0);
}

void OnTrayShowStatus()
{
    if (g_trayIcon)
    {
        g_trayIcon->ShowInfoNotification(
            g_trayIcon->IsMonitoring() ? "Monitoring is active" : "Monitoring is paused");
    }
}

void OnTrayToggleMonitoring(const bool enabled)
{
    WT_LOG("[Main] Monitoring " << (enabled ? "enabled" : "disabled"));

    // Pause 전역 게이트. enabled=true → 추적, false → heartbeat enqueue 전면 차단.
    g_monitoringPaused.store(!enabled, std::memory_order_release);

    if (g_trayIcon)
    {
        g_trayIcon->SetMonitoringState(enabled);
        g_trayIcon->ShowInfoNotification(enabled ? "Monitoring resumed" : "Monitoring paused");
    }
}

void OnTrayOpenDashboard()
{
    WT_LOG("[Main] Opening WakaTime dashboard");
    ShellExecuteW(nullptr, L"open", L"https://wakatime.com/dashboard",
                  nullptr, nullptr, SW_SHOWNORMAL);
}

void OnApiKeyChanged(const std::string &newApiKey)
{
    WT_LOG("[Main] API Key changed, updating WakaTime client...");

    if (!g_wakatimeClient) return;

    const bool success = g_wakatimeClient->ReInitialize(newApiKey);
    if (g_trayIcon)
    {
        if (success) g_trayIcon->ShowInfoNotification("API Key saved and client reinitialized!");
        else g_trayIcon->ShowErrorNotification("Failed to initialize with new API key");
    }
}

// 앱 추적 토글: 켜면 즉시 스캔, 끄면 watcher 중지 + active instance purge.
void OnToggleApp(const std::string &appId, const bool enabled)
{
    WT_LOG("[Main] Toggle app " << appId << " → " << (enabled ? "enabled" : "disabled"));

    AppRegistry::SetEnabled(appId, enabled);

    const AppDefinition *def = AppRegistry::FindById(appId);
    const std::string name = def ? def->displayName : appId;

    if (enabled)
    {
        UpdateProcessScanTimer(); // 0→1 전이 시 스캔 전에 타이머를 켜둔다

        if (g_processMonitor)
        {
            std::vector<AppInstance> started;
            std::vector<AppInstance> closed;
            g_processMonitor->PollChanges(started, closed);
            if (!started.empty()) HandleNewInstances(started);
            if (!closed.empty()) HandleClosedInstances(closed);
        }

        if (g_trayIcon) g_trayIcon->ShowInfoNotification(name + " tracking enabled");
    }
    else
    {
        if (g_focusDetector) g_focusDetector->ClearFocusForApp(appId);
        if (g_fileWatcher) g_fileWatcher->StopWatchingByApp(appId);
        if (g_processMonitor) g_processMonitor->PurgeApp(appId);
        UpdateProcessScanTimer(); // 1→0 전이 시 타이머를 끈다

        if (g_trayIcon) g_trayIcon->ShowInfoNotification(name + " tracking disabled");
    }
}

// 포커스/제목 변경 이벤트 콜백 (SetWinEventHook, OUTOFCONTEXT → 메인 펌프에서 디스패치).
void CALLBACK FocusWinEventProc(HWINEVENTHOOK, const DWORD event, const HWND hwnd,
                                const LONG idObject, LONG, DWORD, DWORD)
{
    if (idObject != OBJID_WINDOW || hwnd == nullptr || g_focusDetector == nullptr) return;

    if (event == EVENT_SYSTEM_FOREGROUND)
    {
        g_focusDetector->OnForegroundChanged(hwnd);
    }
    else if (event == EVENT_OBJECT_NAMECHANGE)
    {
        g_focusDetector->OnTitleChanged(hwnd);
    }
}

int main()
{
    WT_LOG("[Main] Creative WakaTime Monitor Starting...");
    const bool darkModeAvailable = WindowsDarkMode::EnableForApp();
    WT_LOG("[Main] Dark mode menu opt-in: " << (darkModeAvailable ? "enabled" : "not available"));

    // 추적 활성 앱 목록 로드 (트레이 메뉴 빌드 전에).
    AppRegistry::Load();

    TrayIcon trayIcon;
    g_trayIcon = &trayIcon;

    if (!trayIcon.Initialize("Creative WakaTime"))
    {
        WT_ERR("[Main] Failed to initialize tray icon!");
        return 1;
    }

    trayIcon.SetExitCallback(OnTrayExit);
    trayIcon.SetShowStatusCallback(OnTrayShowStatus);
    trayIcon.SetToggleMonitoringCallback(OnTrayToggleMonitoring);
    trayIcon.SetOpenDashboardCallback(OnTrayOpenDashboard);
    trayIcon.SetApiKeyChangeCallback(OnApiKeyChanged);
    trayIcon.SetToggleAppCallback(OnToggleApp);

    trayIcon.ShowInfoNotification("Creative WakaTime started !");

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
    // 워커 스레드의 파일 변경 → 메인 스레드로 PostMessage 마샬링
    fileWatcher.SetNotifyCallback([&trayIcon]()
    {
        trayIcon.NotifyFileEvent();
    });

    FocusDetector focusDetector;
    g_focusDetector = &focusDetector;
    focusDetector.SetHeartbeatCallback(OnFocusHeartbeat);

    // 활성 앱 초기 스캔 + 프로세스 스캔 타이머 동적 설정
    InitialScan();
    UpdateProcessScanTimer();

    trayIcon.SetMonitoringState(true);
    g_monitoringPaused.store(false, std::memory_order_release);

    WT_LOG("\n[Main] Creative WakaTime is now running in background!");

    // 이벤트 허브 콜백 배선 (TrayIcon의 메시지 펌프에서 디스패치)
    trayIcon.SetFileEventCallback([]()
    {
        if (g_fileWatcher) g_fileWatcher->DrainPendingEvents();
    });

    trayIcon.SetProcessScanCallback([&processMonitor]()
    {
        std::vector<AppInstance> started;
        std::vector<AppInstance> closed;
        processMonitor.PollChanges(started, closed);
        if (!started.empty()) HandleNewInstances(started);
        if (!closed.empty()) HandleClosedInstances(closed);
    });

    trayIcon.SetPeriodicTickCallback([]()
    {
        if (g_focusDetector) g_focusDetector->SendPeriodicHeartbeat();
    });

    // 포커스 추적 훅 2개: 포그라운드 전이 + 제목 변경.
    const HWINEVENTHOOK foregroundHook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        nullptr, FocusWinEventProc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    const HWINEVENTHOOK nameChangeHook = SetWinEventHook(
        EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE,
        nullptr, FocusWinEventProc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    // 훅 설치 시점에 이미 추적 대상이 포그라운드일 수 있으므로 초기 상태 1회 캡처
    focusDetector.OnForegroundChanged(GetForegroundWindow());

    // 메시지 펌프: idle 시 GetMessage가 커널에서 블록되어 CPU ≈ 0.
    trayIcon.RunMessageLoop();

    if (foregroundHook) UnhookWinEvent(foregroundHook);
    if (nameChangeHook) UnhookWinEvent(nameChangeHook);

    WT_LOG("\n[Main] Shutting down Creative WakaTime...");
    trayIcon.ShowInfoNotification("Creative WakaTime shutting down...");

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

    if (g_fileWatcher)
    {
        WT_LOG("[Main] Stopping all file watchers...");
        g_fileWatcher->StopAllWatching();
    }

    Globals::Cleanup();

    WT_LOG("[Main] Creative WakaTime stopped gracefully.");
    return 0;
}
