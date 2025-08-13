#include "globals.h"
#include "process_monitor.h"
#include "file_watcher.h"
#include "wakatime_client.h"
#include "tray_icon.h"

WakaTimeClient *g_wakatimeClient = nullptr;
FileWatcher *g_fileWatcher = nullptr;
ProcessMonitor *g_processMonitor = nullptr;
TrayIcon *g_trayIcon = nullptr;
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
    }
}

// 파일 변경 이벤트 처리 - TrayIcon도 업데이트
void OnFileChanged(const FileChangeEvent &event)
{
    std::cout << "[HEARTBEAT] " << event.fileName << " (" << event.projectName << ")" << std::endl;

    // WakaTime API에 heartbeat 전송
    if (g_wakatimeClient)
    {
        g_wakatimeClient->SendHeartbeatFromEvent(event);
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
    std::cout << "[Main] Exit requested from tray" << std::endl;
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
    std::cout << "[Main] Monitoring " << (enabled ? "enabled" : "disabled") << std::endl;

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
    std::cout << "[Main] Opening WakaTime dashboard" << std::endl;

    ShellExecuteW(nullptr, L"open", L"https://wakatime.com/dashboard",
                  nullptr, nullptr, SW_SHOWNORMAL);
}

void OnTrayShowSettings()
{
    std::cout << "[Main] Settings requested" << std::endl;

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
    std::cout << "[Main] API Key changed, updating WakaTime client..." << std::endl;

    if (g_wakatimeClient)
    {
        g_wakatimeClient->SetApiKey(newApiKey);

        if (g_trayIcon)
        {
            g_trayIcon->ShowInfoNotification("API Key saved");
        }
    }
}

void HandleNewUnityInstances(const std::vector<UnityInstance> &newInstances)
{
    for (const auto &instance: newInstances)
    {
        std::cout << "[Main] New Unity instance detected: " << instance.projectName
                << " (Unity " << instance.editorVersion << ")" << std::endl;

        if (g_fileWatcher && g_fileWatcher->StartWatching(instance.projectPath, instance.projectName))
        {
            std::cout << "[Main] Started watching: " << instance.projectName << std::endl;

            if (g_trayIcon)
            {
                g_trayIcon->SetCurrentProject(instance.projectName);
                g_trayIcon->ShowInfoNotification("New Unity project: " + instance.projectName +
                                                 " (Unity " + instance.editorVersion + ")");
            }
        } else
        {
            std::cout << "[Main] Failed to start watching: " << instance.projectName << std::endl;
        }
    }
}

void HandleClosedUnityInstances(const std::vector<UnityInstance> &closedInstances)
{
    for (const auto &instance: closedInstances)
    {
        std::cout << "[Main] Unity instance closed: " << instance.projectName << std::endl;

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
                if (auto remainingProjects = g_fileWatcher->GetWatchedProjects(); !remainingProjects.empty())
                {
                    auto projectName = fs::path(remainingProjects[0]).filename().string();
                    g_trayIcon->SetCurrentProject(projectName);
                    std::cout << "[Main] Switched to remaining project: " << projectName << std::endl;
                } else
                {
                    g_trayIcon->SetCurrentProject(""); // 모든 프로젝트 종료
                    std::cout << "[Main] No Unity projects are being watched" << std::endl;
                }
            }
        }
    }
}

void InitialUnityProjectScan()
{
    if (!g_processMonitor || !g_fileWatcher) return;

    std::cout << "[Main] Performing initial Unity project scan..." << std::endl;

    const auto instances = g_processMonitor->ScanUnityProcesses();

    if (instances.empty())
    {
        std::cout << "[Main] No Unity processes found during initial scan" << std::endl;
        return;
    }

    for (const auto &instance: instances)
    {
        if (g_fileWatcher->StartWatching(instance.projectPath, instance.projectName))
        {
            std::cout << "[Main] ✅ Started watching: " << instance.projectName
                    << " (Unity " << instance.editorVersion << ")" << std::endl;

            if (g_trayIcon)
            {
                g_trayIcon->SetCurrentProject(instance.projectName);
            }
        }
    }

    std::cout << "[Main] Initial scan complete. Watching " << instances.size() << " Unity projects" << std::endl;
}

int main()
{
    std::cout << "[Main] Unity WakaTime Monitor Starting..." << std::endl;

    TrayIcon trayIcon;
    g_trayIcon = &trayIcon;

    if (!trayIcon.Initialize("Unity WakaTime"))
    {
        std::cerr << "[Main] Failed to initialize tray icon!" << std::endl;
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
        std::cerr << "[Main] Failed to initialize WakaTime client!" << std::endl;
        trayIcon.ShowErrorNotification("WakaTime client not initialized. Click 'Setup API Key' in menu.");
    }

    ProcessMonitor processMonitor;
    FileWatcher fileWatcher;
    g_processMonitor = &processMonitor;
    g_fileWatcher = &fileWatcher;

    fileWatcher.SetChangeCallback(OnFileChanged);

    InitialUnityProjectScan();

    trayIcon.SetMonitoringState(true);

    std::cout << "\n[Main] Unity WakaTime is now running in background!" << std::endl;

    auto lastScan = std::chrono::steady_clock::now();
    const auto scanInterval = std::chrono::seconds(10);

    while (!Globals::ShouldExit())
    {
        int msgCount = trayIcon.ProcessMessages();

        if (msgCount > 5)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50)); // 많은 메시지 → 빠른 처리
        } else if (msgCount > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100)); // 일부 메시지 → 보통 처리
        } else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000)); // 메시지 없음 → 여유 있게
        }

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

    std::cout << "\n[Main] Shutting down Unity WakaTime..." << std::endl;
    trayIcon.ShowInfoNotification("Unity WakaTime shutting down...");

    // 남은 heartbeat 전송
    if (g_wakatimeClient)
    {
        std::cout << "[Main] Flushing remaining heartbeats..." << std::endl;
        wakatimeClient.FlushQueue();
    }

    // 모든 파일 감시 중지
    if (g_fileWatcher)
    {
        std::cout << "[Main] Stopping all file watchers..." << std::endl;
        g_fileWatcher->StopAllWatching();
    }

    Globals::Cleanup();

    std::cout << "[Main] Unity WakaTime stopped gracefully." << std::endl;
    return 0;
}
