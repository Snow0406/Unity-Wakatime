#pragma once

#include "globals.h"

/**
 * Unity 프로젝트 폴더의 파일 변경사항을 실시간으로 감지 <br/>
 * Windows ReadDirectoryChangesW API 사용
 */
class FileWatcher {
private:
    // 감시 중인 프로젝트 정보
    struct WatchedProject {
        std::string projectPath;        // 프로젝트 경로
        std::string projectName;        // 프로젝트 이름
        std::string unityVersion;       // 유니티 버전
        HANDLE directoryHandle;         // 디렉토리 핸들
        std::thread watchThread;        // 감시 스레드
        std::atomic<bool> shouldStop;   // 스레드 종료 플래그
        OVERLAPPED overlapped;          // 비동기 I/O용 구조체
        char buffer[4096];              // 변경 정보를 받을 버퍼
        HANDLE stopEvent;

        WatchedProject() : shouldStop(false) {
            stopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
            ZeroMemory(&overlapped, sizeof(OVERLAPPED));
            ZeroMemory(buffer, sizeof(buffer));
        }

        ~WatchedProject() {
            if (stopEvent != INVALID_HANDLE_VALUE) {
                CloseHandle(stopEvent);
            }
        }
    };

    std::vector<std::unique_ptr<WatchedProject>> watchedProjects;
    mutable std::mutex projectsMutex;  // 스레드 안전성을 위한 뮤텍스
    
    // 파일 변경 이벤트 콜백 함수
    std::function<void(const FileChangeEvent&)> changeCallback;

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
     * Unity 프로젝트 감시 시작
     * @param projectPath 감시할 프로젝트 경로
     * @param projectName 프로젝트 이름
     * @param unityVersion 유니티 에디터 버전
     * @return 성공하면 true
     */
    bool StartWatching(const std::string& projectPath, const std::string& projectName, const std::string& unityVersion);
    
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
