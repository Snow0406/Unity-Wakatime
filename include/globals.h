#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <string>
#include <vector>
#include <atomic>
#include <chrono>
#include <memory>
#include <iostream>
#include <unordered_set>
#include <functional>
#include <thread>
#include <mutex>
#include <filesystem>
#include <future>
#include <sstream>
#include <fstream>
#include <algorithm>

namespace fs = std::filesystem;

class WakaTimeClient;
class FileWatcher;
class ProcessMonitor;
class TrayIcon;

struct UnityInstance
{
    DWORD processId;
    std::string projectPath;
    std::string projectName;
    std::string editorVersion;
};

struct FileChangeEvent
{
    std::string filePath;
    std::string fileName;
    std::string projectPath;
    std::string projectName;
    std::string unityVersion;
    DWORD action;
    std::chrono::system_clock::time_point timestamp;
};

namespace Config
{
    const std::vector<std::string> UNITY_FILE_EXTENSIONS = {
        ".unity",
        ".prefab",
        ".asset",
        ".mat",
        ".shader",
        ".hlsl",
        ".anim",
        ".controller",
        ".json",
    };

    const std::vector<std::string> IGNORE_FOLDERS = {
        "Library",
        "Temp",
        "Logs",
        "obj",
        "bin",
        "UserSettings",
        ".vs",
        ".idea",
        ".vscode",
        ".git",
        "Build",
    };

    // WakaTime 설정
    const std::string WAKATIME_API_URL = "https://api.wakatime.com/api/v1/users/current/heartbeats";
    const std::string USER_AGENT = "unity-wakatime/1.0";
    const int HEARTBEAT_TIMEOUT_MS = 5000;
    const int FILE_WATCHER_BUFFER_SIZE = 4096;
    const int HEARTBEAT_DEBOUNCE_MS = 2000;

    /**
     * 벡터를 unordered_set으로 변환 (컴파일 타임 최적화)
     */
    inline const std::unordered_set<std::string> &GetUnityExtensions()
    {
        static const auto extensions = std::unordered_set<std::string>(UNITY_FILE_EXTENSIONS.begin(),
                                                                       UNITY_FILE_EXTENSIONS.end());
        return extensions;
    }

    inline const std::unordered_set<std::string> &GetIgnoreFolders()
    {
        static const auto folders = std::unordered_set<std::string>(IGNORE_FOLDERS.begin(), IGNORE_FOLDERS.end());
        return folders;
    }
}

extern WakaTimeClient *g_wakatimeClient;
extern FileWatcher *g_fileWatcher;
extern ProcessMonitor *g_processMonitor;
extern TrayIcon *g_trayIcon;
extern std::atomic<bool> g_shouldExit;

namespace Globals
{
    /**
     * 전역 객체 정리
     */
    void Cleanup();

    /**
     * 안전한 접근 함수들 (null 체크 + 성능 최적화)
     */
    inline WakaTimeClient *GetWakaTimeClient() noexcept
    {
        return g_wakatimeClient;
    }

    inline FileWatcher *GetFileWatcher() noexcept
    {
        return g_fileWatcher;
    }

    inline ProcessMonitor *GetProcessMonitor() noexcept
    {
        return g_processMonitor;
    }

    inline TrayIcon *GetTrayIcon() noexcept
    {
        return g_trayIcon;
    }

    /**
     * 애플리케이션 종료 요청
     */
    inline void RequestExit() noexcept
    {
        g_shouldExit.store(true, std::memory_order_release);
    }

    /**
     * 종료 요청 확인
     */
    inline bool ShouldExit() noexcept
    {
        return g_shouldExit.load(std::memory_order_acquire);
    }
}
