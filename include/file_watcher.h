#pragma once

#include "globals.h"
#include <deque>

/**
 * Unity 프로젝트 폴더의 파일 변경사항을 실시간으로 감지 <br/>
 * Windows ReadDirectoryChangesW API 사용
 */
class FileWatcher {
private:
    // 감시 종류: Unity 프로젝트 폴더(재귀) vs 외부 이벤트 inbox 폴더(평면)
    enum class Kind { Unity, Inbox };

    // 감시 중인 프로젝트 정보
    struct WatchedProject {
        std::string projectPath;        // 프로젝트 경로
        std::string projectName;        // 프로젝트 이름
        std::string unityVersion;       // 유니티 버전
        Kind kind;                      // 감시 종류 (기본 Unity)
        BOOL recursive;                 // 하위 디렉토리 포함 여부 (Unity: TRUE, Inbox: FALSE)
        HANDLE directoryHandle;         // 디렉토리 핸들
        std::thread watchThread;        // 감시 스레드
        std::atomic<bool> shouldStop;   // 스레드 종료 플래그
        OVERLAPPED overlapped;          // 비동기 I/O용 구조체
        char buffer[4096];              // 변경 정보를 받을 버퍼
        HANDLE stopEvent;
        HANDLE ioEvent;

        WatchedProject() :
            kind(Kind::Unity),
            recursive(TRUE),
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
    std::deque<std::string> pendingInboxFiles;     // inbox에서 감지된 .json 경로 큐
    mutable std::mutex pendingInboxMutex;
    std::atomic<bool> notifyScheduled{false};  // PostMessage 코얼레싱 (큐 적재 통지 1회로 합침)

    // 파일 변경 이벤트 콜백 함수
    std::function<void(const FileChangeEvent&)> changeCallback;
    // inbox에서 .json 파일이 감지될 때 호출 (전체 경로 전달)
    std::function<void(const std::string&)> inboxCallback;
    // 큐에 이벤트가 적재되었음을 메인 스레드에 통지 (PostMessage 등)
    std::function<void()> notifyCallback;

    /**
     * 특정 프로젝트 폴더를 감시하는 워커 스레드 함수
     * @param project 감시할 프로젝트 정보
     */
    void WatchProjectThread(WatchedProject* project);
    
    /**
     * ReadDirectoryChangesW로부터 받은 데이터를 파싱
     * @param buffer 변경 정보 버퍼
     * @param bytesReturned 받은 데이터 크기
     * @param project 해당 프로젝트 정보
     */
    void ProcessFileChanges(char* buffer, DWORD bytesReturned, WatchedProject* project);
    
    /**
     * 파일이 Unity 관련 파일인지 확인
     * @param fileName 파일 이름
     * @return Unity 파일이면 true
     */
    bool IsUnityFile(const std::string& fileName) const;
    
    /**
     * 무시해야 할 폴더인지 확인
     * @param folderName 폴더 이름
     * @return 무시해야 하면 true
     */
    bool ShouldIgnoreFolder(const std::string& folderName) const;

public:
    FileWatcher();
    ~FileWatcher();
    
    /**
     * 파일 변경 이벤트 콜백 함수 설정
     * @param callback 파일이 변경될 때 호출될 함수
     */
    void SetChangeCallback(std::function<void(const FileChangeEvent&)> callback);

    /**
     * 큐 적재 통지 콜백 설정 (워커 스레드 → 메인 스레드 마샬링용).
     * 워커 스레드가 이벤트를 큐에 넣으면 이 콜백을 호출한다(코얼레싱됨).
     * @param callback 통지 시 호출될 함수 (예: PostMessage)
     */
    void SetNotifyCallback(std::function<void()> callback);

    /**
     * inbox 콜백 설정 (외부 이벤트 .json 파일이 감지될 때 호출).
     * 콜백은 메인 스레드의 DrainPendingEvents 흐름에서 디스패치된다.
     * @param callback .json 전체 경로를 받는 함수
     */
    void SetInboxCallback(std::function<void(const std::string&)> callback);

    /**
     * Unity 프로젝트 감시 시작
     * @param projectPath 감시할 프로젝트 경로
     * @param projectName 프로젝트 이름
     * @param unityVersion 유니티 에디터 버전
     * @return 성공하면 true
     */
    bool StartWatching(const std::string& projectPath, const std::string& projectName, const std::string& unityVersion);

    /**
     * 외부 이벤트 inbox 폴더 감시 시작 (평면 구조, 비재귀).
     * 폴더 내 .json 파일 생성/이름변경/수정 시 inboxCallback이 호출된다.
     * @param inboxPath 감시할 inbox 폴더 경로 (예: %APPDATA%/creative-wakatime/events)
     * @return 성공하면 true
     */
    bool StartWatchingInbox(const std::string& inboxPath);
    
    /**
     * 특정 프로젝트 감시 중지
     * @param projectPath 중지할 프로젝트 경로
     */
    void StopWatching(const std::string& projectPath);
    
    /**
     * 모든 프로젝트 감시 중지
     */
    void StopAllWatching();

    /**
     * 워커 스레드에서 수집된 이벤트를 현재(호출) 스레드에서 전달
     * @param maxEvents 한 번에 처리할 최대 이벤트 수
     */
    void DrainPendingEvents(size_t maxEvents = 1024);
    
    /**
     * 현재 감시 중인 프로젝트 수 반환
     * @return 감시 중인 프로젝트 개수
     */
    size_t GetWatchedProjectCount() const;
    
    /**
     * 감시 중인 모든 프로젝트 경로 반환
     * @return 프로젝트 경로들
     */
    std::vector<WatchedProjectInfo> GetWatchedProjects() const;
};
