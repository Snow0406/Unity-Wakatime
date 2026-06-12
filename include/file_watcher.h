#pragma once

#include "globals.h"
#include <deque>
#include <unordered_set>

/**
 * 추적 대상 앱(현재 Unity)의 프로젝트 폴더 변경을 ReadDirectoryChangesW로 재귀 감시한다.
 * 확장자 필터는 앱 정의(AppRegistry)의 fileExtensions를 사용한다.
 */
class FileWatcher {
private:
    // 감시 중인 프로젝트 정보
    struct WatchedProject {
        std::string appId;              // 앱 정의 id
        std::string projectPath;        // 프로젝트 경로 (UTF-8)
        std::string projectName;        // 프로젝트 이름
        std::string editorVersion;      // 에디터 버전
        std::unordered_set<std::string> extensions; // 추적 확장자 (소문자)
        HANDLE directoryHandle;         // 디렉토리 핸들
        std::thread watchThread;        // 감시 스레드
        std::atomic<bool> shouldStop;   // 스레드 종료 플래그
        OVERLAPPED overlapped;          // 비동기 I/O용 구조체
        char buffer[Config::FILE_WATCHER_BUFFER_SIZE]; // 변경 정보를 받을 버퍼
        HANDLE stopEvent;
        HANDLE ioEvent;

        WatchedProject() :
            directoryHandle(INVALID_HANDLE_VALUE),
            shouldStop(false),
            stopEvent(nullptr),
            ioEvent(nullptr)
        {
            stopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
            ioEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
            ZeroMemory(&overlapped, sizeof(OVERLAPPED));
            overlapped.hEvent = ioEvent;
            ZeroMemory(buffer, sizeof(buffer));
        }

        ~WatchedProject() {
            if (stopEvent != nullptr) {
                CloseHandle(stopEvent);
                stopEvent = nullptr;
            }
            if (ioEvent != nullptr) {
                CloseHandle(ioEvent);
                ioEvent = nullptr;
            }
            if (directoryHandle != nullptr && directoryHandle != INVALID_HANDLE_VALUE) {
                CloseHandle(directoryHandle);
                directoryHandle = INVALID_HANDLE_VALUE;
            }
        }
    };

    std::vector<std::unique_ptr<WatchedProject>> watchedProjects;
    mutable std::mutex projectsMutex;  // 스레드 안전성을 위한 뮤텍스
    std::deque<FileChangeEvent> pendingEvents;
    mutable std::mutex pendingEventsMutex;
    std::atomic<bool> notifyScheduled{false};  // PostMessage 코얼레싱 (큐 적재 통지 1회로 합침)

    // 파일 변경 이벤트 콜백 함수
    std::function<void(const FileChangeEvent&)> changeCallback;
    // 큐에 이벤트가 적재되었음을 메인 스레드에 통지 (PostMessage 등)
    std::function<void()> notifyCallback;

    /**
     * 특정 프로젝트 폴더를 감시하는 워커 스레드 함수
     */
    void WatchProjectThread(WatchedProject* project);

    /**
     * ReadDirectoryChangesW로부터 받은 데이터를 파싱
     */
    void ProcessFileChanges(char* buffer, DWORD bytesReturned, WatchedProject* project);

    /**
     * ReadDirectoryChangesW 버퍼 오버플로 시 최근 수정된 추적 파일을 합성 이벤트로 적재한다.
     */
    void QueueSyntheticProjectScan(WatchedProject* project);

    /**
     * 파일 변경 이벤트를 pending 큐에 적재하고 메인 스레드 통지를 예약한다.
     */
    void QueueFileEvent(WatchedProject* project, const std::string& fileName,
                        const std::string& fullPath, DWORD action);

    /**
     * 파일이 추적 확장자에 해당하는지 확인
     */
    bool IsTrackedFile(const std::string& fileName, const std::unordered_set<std::string>& extensions) const;

    /**
     * 무시해야 할 폴더인지 확인
     */
    bool ShouldIgnoreFolder(const std::string& folderName) const;

public:
    FileWatcher();
    ~FileWatcher();

    /**
     * 파일 변경 이벤트 콜백 함수 설정
     */
    void SetChangeCallback(std::function<void(const FileChangeEvent&)> callback);

    /**
     * 큐 적재 통지 콜백 설정 (워커 스레드 → 메인 스레드 마샬링용).
     */
    void SetNotifyCallback(std::function<void()> callback);

    /**
     * 프로젝트 감시 시작.
     * @param appId 앱 정의 id (확장자 필터 조회용)
     * @param projectPath 감시할 프로젝트 경로
     * @param projectName 프로젝트 이름
     * @param editorVersion 에디터 버전
     * @return 성공하면 true
     */
    bool StartWatching(const std::string& appId, const std::string& projectPath,
                       const std::string& projectName, const std::string& editorVersion);

    /**
     * 특정 프로젝트 감시 중지
     */
    void StopWatching(const std::string& projectPath);

    /**
     * 특정 앱의 모든 프로젝트 감시 중지 (앱 토글 off 시)
     */
    void StopWatchingByApp(const std::string& appId);

    /**
     * 모든 프로젝트 감시 중지
     */
    void StopAllWatching();

    /**
     * 워커 스레드에서 수집된 이벤트를 현재(호출) 스레드에서 전달
     */
    void DrainPendingEvents(size_t maxEvents = 1024);

    /**
     * 현재 감시 중인 프로젝트 수 반환
     */
    size_t GetWatchedProjectCount() const;

    /**
     * 감시 중인 모든 프로젝트 정보 반환
     */
    std::vector<WatchedProjectInfo> GetWatchedProjects() const;
};
