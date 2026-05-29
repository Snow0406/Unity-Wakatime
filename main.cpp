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

    UnityFocusDetector unityFocusDetector;
    g_unityFocusDetector = &unityFocusDetector;

    unityFocusDetector.SetFocusCallback(OnUnityFocusHeartbeat);
    unityFocusDetector.SetUnfocusCallback([]() {});
    unityFocusDetector.SetPeriodicHeartbeatCallback(OnUnityFocusHeartbeat);

    InitialUnityProjectScan();

    trayIcon.SetMonitoringState(true);

    WT_LOG("\n[Main] Unity WakaTime is now running in background!");

    auto lastScan = std::chrono::steady_clock::now();
    const auto scanInterval = std::chrono::seconds(10);

    while (!Globals::ShouldExit())
    {
        if (g_fileWatcher)
        {
            g_fileWatcher->DrainPendingEvents();
        }

        if (g_unityFocusDetector) {
            g_unityFocusDetector->CheckFocused();
            g_unityFocusDetector->SendPeriodicHeartbeat();
        }

        int msgCount = trayIcon.ProcessMessages();
        if (msgCount > 5)  std::this_thread::sleep_for(std::chrono::milliseconds(50)); // 많은 메시지 → 빠른 처리
        else if (msgCount > 0) std::this_thread::sleep_for(std::chrono::milliseconds(100)); // 일부 메시지 → 보통 처리
        else std::this_thread::sleep_for(std::chrono::milliseconds(1000)); // 메시지 없음 → 여유 있게

        if (auto now = std::chrono::steady_clock::now(); now - lastScan >= scanInterval)
        {
            // 새로운 Unity 인스턴스 감지
            if (auto newInstances = processMonitor.GetNewInstances(); !newInstances.empty())
            {
                HandleNewUnityInstances(newInstances);
            }

            // 종료된 Unity 인스턴스 감지
            if (auto closedInstances = processMonitor.GetClosedInstances(); !closedInstances.empty())
            {
                HandleClosedUnityInstances(closedInstances);
            }

            lastScan = now;
        }
    }

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
