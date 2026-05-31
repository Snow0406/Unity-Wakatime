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
#include <cstdlib>
#include <system_error>

namespace fs = std::filesystem;

// Release(NDEBUG)에서는 인자 평가까지 컴파일아웃된다.
#ifdef NDEBUG
    #define WT_LOG(msg) ((void)0)
    #define WT_ERR(msg) ((void)0)
#else
    #define WT_LOG(msg) do { std::cout << msg << std::endl; } while (0)
    #define WT_ERR(msg) do { std::cerr << msg << std::endl; } while (0)
#endif

class WakaTimeClient;
class FileWatcher;
class ProcessMonitor;
class TrayIcon;
class UnityFocusDetector;

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

struct WatchedProjectInfo {
    std::string projectPath;
    std::string projectName;
    std::string unityVersion;
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
    const std::string APP_NAME = "creative-wakatime";
    const std::string APP_VERSION = "2.0";
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

    /**
     * 앱 데이터 디렉토리 경로 반환 (%APPDATA%/creative-wakatime/).
     * 폴더가 없으면 events/ 하위까지 생성한다. 실패 시 빈 문자열.
     * @return 앱 데이터 디렉토리 경로 (끝에 구분자 없음), 실패 시 ""
     */
    inline std::string GetAppDataDir()
    {
        const char *appData = std::getenv("APPDATA");
        if (appData == nullptr || appData[0] == '\0')
        {
            return "";
        }

        const std::string dir = std::string(appData) + "\\" + APP_NAME;

        std::error_code ec;
        fs::create_directories(dir, ec); // 이미 존재해도 에러 아님
        if (ec)
        {
            return "";
        }

        return dir;
    }

    /**
     * 이벤트 inbox 디렉토리 경로 반환 (%APPDATA%/creative-wakatime/events/).
     * 폴더가 없으면 생성한다. 실패 시 빈 문자열.
     */
    inline std::string GetEventsDir()
    {
        const std::string base = GetAppDataDir();
        if (base.empty())
        {
            return "";
        }

        const std::string dir = base + "\\events";

        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec)
        {
            return "";
        }

        return dir;
    }

    /**
     * API 키 config 파일 경로 반환 (%APPDATA%/creative-wakatime/wakatime_config.txt).
     * 앱 데이터 디렉토리를 얻지 못하면 빈 문자열.
     */
    inline std::string GetConfigFilePath()
    {
        const std::string base = GetAppDataDir();
        if (base.empty())
        {
            return "";
        }

        return base + "\\wakatime_config.txt";
    }
}

extern WakaTimeClient *g_wakatimeClient;
extern FileWatcher *g_fileWatcher;
extern ProcessMonitor *g_processMonitor;
extern TrayIcon *g_trayIcon;
extern UnityFocusDetector *g_unityFocusDetector;
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

    inline UnityFocusDetector *GetUnityFocusDetector() noexcept
    {
        return g_unityFocusDetector;
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
