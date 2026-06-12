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
#include <cctype>
#include <utility>
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
class FocusDetector;

struct AppInstance
{
    std::string appId;          // AppRegistry 정의 id
    DWORD processId;
    std::string projectPath;    // DirectoryWatch: 감시할 프로젝트 폴더
    std::string projectName;    // 프로젝트/폴더 이름
    std::string editorVersion;  // Unity 에디터 버전 등 (없으면 빈 값)
    std::string entity;         // WindowTitle 앱의 초기 파일 경로 (커맨드라인에서 추출)
};

struct FileChangeEvent
{
    std::string appId;
    std::string filePath;
    std::string fileName;
    std::string projectPath;
    std::string projectName;
    DWORD action;
    std::chrono::system_clock::time_point timestamp;
};

struct WatchedProjectInfo {
    std::string appId;
    std::string projectPath;
    std::string projectName;
    std::string editorVersion;
};

namespace Config
{
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
    // 대량 import/save 시 ReadDirectoryChangesW 이벤트 유실을 줄이기 위한 큰 버퍼(64KB).
    const int FILE_WATCHER_BUFFER_SIZE = 65536;
    const int HEARTBEAT_DEBOUNCE_MS = 2000;

    inline const std::unordered_set<std::string> &GetIgnoreFolders()
    {
        static const auto folders = []()
        {
            std::unordered_set<std::string> result;
            for (const auto &folder : IGNORE_FOLDERS)
            {
                std::string lower = folder;
                std::transform(lower.begin(), lower.end(), lower.begin(),
                               [](const unsigned char c) { return static_cast<char>(::tolower(c)); });
                result.insert(std::move(lower));
            }
            return result;
        }();
        return folders;
    }

    /**
     * 앱 데이터 디렉토리 경로 반환 (%APPDATA%/creative-wakatime/).
     * 폴더가 없으면 생성한다. 실패 시 빈 문자열.
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

    /**
     * 추적 활성 앱 목록 파일 경로 (%APPDATA%/creative-wakatime/apps.txt).
     * 앱 데이터 디렉토리를 얻지 못하면 빈 문자열.
     */
    inline std::string GetAppsConfigFilePath()
    {
        const std::string base = GetAppDataDir();
        if (base.empty())
        {
            return "";
        }

        return base + "\\apps.txt";
    }
}

extern WakaTimeClient *g_wakatimeClient;
extern FileWatcher *g_fileWatcher;
extern ProcessMonitor *g_processMonitor;
extern TrayIcon *g_trayIcon;
extern FocusDetector *g_focusDetector;
extern std::atomic<bool> g_shouldExit;
// Pause Monitoring 전역 게이트. true면 모든 heartbeat enqueue를 차단한다.
// (watcher/스캐너/포커스 추적은 계속 돌려 상태는 신선하게 유지)
extern std::atomic<bool> g_monitoringPaused;

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

    inline FocusDetector *GetFocusDetector() noexcept
    {
        return g_focusDetector;
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
