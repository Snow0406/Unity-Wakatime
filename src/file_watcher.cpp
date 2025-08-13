#include "file_watcher.h"
#include <utility>

FileWatcher::FileWatcher()
{
    std::cout << "[FileWatcher] Initialized" << std::endl;
}

FileWatcher::~FileWatcher()
{
    StopAllWatching();
    std::cout << "[FileWatcher] Destroyed" << std::endl;
}

void FileWatcher::SetChangeCallback(std::function<void(const FileChangeEvent &)> callback)
{
    changeCallback = std::move(callback);
    std::cout << "[FileWatcher] Change callback set" << std::endl;
}

bool FileWatcher::StartWatching(const std::string &projectPath, const std::string &projectName)
{
    std::lock_guard<std::mutex> lock(projectsMutex);

    // 이미 감시 중인지 확인
    for (const auto &project: watchedProjects)
    {
        if (project->projectPath == projectPath) return true;
    }

    // 프로젝트 경로가 존재하는지 확인
    if (!fs::exists(projectPath))
    {
        std::cerr << "[FileWatcher] Project path does not exist: " << projectPath << std::endl;
        return false;
    }

    // 새로운 WatchedProject 생성
    auto project = std::make_unique<WatchedProject>();
    project->projectPath = projectPath;
    project->projectName = projectName;
    project->shouldStop = false;

    // ZeroMemory: 메모리를 0으로 초기화 (Windows API 함수)
    ZeroMemory(&project->overlapped, sizeof(OVERLAPPED));
    ZeroMemory(project->buffer, sizeof(project->buffer));

    // CreateFile: 디렉토리 핸들 열기
    // FILE_LIST_DIRECTORY: 디렉토리 내용을 나열할 권한
    // FILE_SHARE_*: 다른 프로세스와 공유 허용
    // FILE_FLAG_BACKUP_SEMANTICS: 디렉토리를 열 때 필요
    // FILE_FLAG_OVERLAPPED: 비동기 I/O 사용
    project->directoryHandle = CreateFileA(
        projectPath.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr
    );

    if (project->directoryHandle == INVALID_HANDLE_VALUE)
    {
        const DWORD error = GetLastError();
        std::cerr << "[FileWatcher] Failed to open directory: " << projectPath << " (Error: " << error << ")" << std::endl;
        return false;
    }

    std::cout << "[FileWatcher] Started watching: " << projectName << " at " << projectPath << std::endl;

    // 감시 스레드 시작
    WatchedProject *projectPtr = project.get();
    project->watchThread = std::thread(&FileWatcher::WatchProjectThread, this, projectPtr);
    watchedProjects.push_back(std::move(project));

    return true;
}

void FileWatcher::WatchProjectThread(WatchedProject *project)
{
    std::cout << "[FileWatcher] Watch thread started for: " << project->projectName << std::endl;

    while (!project->shouldStop)
    {
        DWORD bytesReturned = 0;

        // ReadDirectoryChangesW: 디렉토리 변경사항을 감지하는 핵심 함수
        // TRUE: 하위 폴더도 감시
        const BOOL result = ReadDirectoryChangesW(
            project->directoryHandle,           // 디렉토리 핸들
            project->buffer,                    // 결과를 받을 버퍼
            sizeof(project->buffer),            // 버퍼 크기
            TRUE,                               // 하위 디렉토리 포함
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION | FILE_NOTIFY_CHANGE_FILE_NAME,
            &bytesReturned,                     // 받은 데이터 크기
            &project->overlapped,               // 비동기 I/O 구조체
            nullptr                             // 완료 콜백 (사용 안함)
        );

        if (!result)
        {
            if (const DWORD error = GetLastError(); error != ERROR_IO_PENDING)
            {
                std::cerr << "[FileWatcher] ReadDirectoryChangesW failed for " << project->projectName << " (Error: " << error << ")" << std::endl;
                break;
            }
        }

        const HANDLE waitHandles[2] = {
            project->directoryHandle,   // I/O 완료 대기
            project->stopEvent          // 종료 신호 대기
        };

        // WaitForMultipleObjects: 여러 이벤트 중 하나라도 신호되면 반환
        const DWORD waitResult = WaitForMultipleObjects(
            2,              // 대기할 핸들 개수
            waitHandles,    // 핸들 배열
            FALSE,          // 모든 핸들이 아닌 하나만 신호되면 됨
            1000            // 1초 타임아웃
        );

        switch (waitResult)
        {
            case WAIT_OBJECT_0: // directoryHandle 신호 (I/O 완료)
            {
                // GetOverlappedResult로 결과 확인
                if (GetOverlappedResult(project->directoryHandle, &project->overlapped, &bytesReturned, FALSE))
                {
                    if (bytesReturned > 0)
                        ProcessFileChanges(project->buffer, bytesReturned, project);
                }
                else
                {
                    if (const DWORD error = GetLastError(); error != ERROR_OPERATION_ABORTED)
                    {
                        std::cerr << "[FileWatcher] GetOverlappedResult failed for " << project->projectName << " (Error: " << error << ")" << std::endl;
                    }
                    break;
                }
                break;
            }

            case (WAIT_OBJECT_0 + 1): // stopEvent 신호 (종료 요청)
                std::cout << "[FileWatcher] Stop event received for: " << project->projectName << std::endl;
                goto thread_exit; // 즉시 종료

            case WAIT_TIMEOUT: // 타임아웃 (정상, 계속 진행)
                if (project->shouldStop)
                {
                    std::cout << "[FileWatcher] Stop flag detected for: " << project->projectName << std::endl;
                    goto thread_exit;
                }
                break;

            default: // 에러
                std::cerr << "[FileWatcher] WaitForMultipleObjects failed for " << project->projectName << " (Error: " << GetLastError() << ")" << std::endl;
                goto thread_exit;
        }

        ZeroMemory(&project->overlapped, sizeof(OVERLAPPED));
    }

thread_exit:
    std::cout << "[FileWatcher] Watch thread stopped for: " << project->projectName << std::endl;
}

void FileWatcher::ProcessFileChanges(char *buffer, DWORD bytesReturned, WatchedProject *project)
{
    // FILE_NOTIFY_INFORMATION: Windows에서 파일 변경 정보를 담는 구조체
    auto *info = reinterpret_cast<FILE_NOTIFY_INFORMATION *>(buffer);

    do
    {
        // 파일명을 와이드 문자에서 일반 문자열로 변환
        // info->FileNameLength는 바이트 단위, WCHAR는 2바이트
        std::wstring wFileName(info->FileName, info->FileNameLength / sizeof(WCHAR));

        // 와이드 문자열을 UTF-8으로 변환
        if (int len = WideCharToMultiByte(CP_UTF8, 0, wFileName.c_str(), -1, nullptr, 0, nullptr, nullptr); len > 0)
        {
            std::string fileName(len - 1, 0); // null terminator 제외
            WideCharToMultiByte(CP_UTF8, 0, wFileName.c_str(), -1, &fileName[0], len, nullptr, nullptr);

            // 전체 파일 경로 생성
            std::string fullPath = project->projectPath + "\\" + fileName;

            // 백슬래시를 슬래시로 정규화 (경로 처리 편의를 위해)
            std::replace(fullPath.begin(), fullPath.end(), '\\', '/');
            std::replace(fileName.begin(), fileName.end(), '\\', '/');

            // 무시할 폴더인지 확인
            bool shouldIgnore = false;
            std::istringstream pathStream(fileName);
            std::string segment;

            while (std::getline(pathStream, segment, '/'))
            {
                if (ShouldIgnoreFolder(segment))
                {
                    shouldIgnore = true;
                    break;
                }
            }

            // Unity 파일이고 무시할 폴더가 아닌 경우에만 처리
            if (!shouldIgnore && IsUnityFile(fileName))
            {
                // FileChangeEvent 생성
                FileChangeEvent event;
                event.filePath = fullPath;
                event.fileName = fileName;
                event.projectPath = project->projectPath;
                event.projectName = project->projectName;
                event.action = info->Action;
                event.timestamp = std::chrono::system_clock::now();

                std::cout << "[FileWatcher] Change" << ": " << fileName << " in " << project->projectName << std::endl;

                // 콜백 함수 호출 (WakaTime에 heartbeat 전송 등)
                if (changeCallback)
                {
                    changeCallback(event);
                }
            }
        }

        // 다음 FILE_NOTIFY_INFORMATION으로 이동
        // NextEntryOffset이 0이면 마지막 항목
        if (info->NextEntryOffset == 0)
        {
            break;
        }

        // 포인터를 다음 구조체로 이동
        info = reinterpret_cast<FILE_NOTIFY_INFORMATION *>(reinterpret_cast<char *>(info) + info->NextEntryOffset);
    } while (true);
}

bool FileWatcher::IsUnityFile(const std::string &fileName) const
{
    // 파일 확장자 추출
    const size_t dotPos = fileName.find_last_of('.');
    if (dotPos == std::string::npos) return false; // 확장자가 없으면 무시

    std::string extension = fileName.substr(dotPos);
    std::transform(extension.begin(), extension.end(), extension.begin(), tolower); // 소문자로 변환

    const auto &extensions = Config::GetUnityExtensions();
    return extensions.find(extension) != extensions.end();
}

bool FileWatcher::ShouldIgnoreFolder(const std::string &folderName) const
{
    const auto &ignoreFolders = Config::GetIgnoreFolders();
    return ignoreFolders.find(folderName) != ignoreFolders.end();
}

void FileWatcher::StopWatching(const std::string &projectPath)
{
    std::lock_guard<std::mutex> lock(projectsMutex);

    const auto it = std::remove_if(watchedProjects.begin(), watchedProjects.end(),
                             [&projectPath](const std::unique_ptr<WatchedProject> &project)
                             {
                                 if (project->projectPath == projectPath)
                                 {
                                     std::cout << "[FileWatcher] Stopping watch for: " << project->projectName << std::endl;

                                     project->shouldStop = true;

                                     SetEvent(project->stopEvent);
                                     CancelIo(project->directoryHandle); // I/O 작업 취소

                                     // 스레드 종료 대기
                                     if (project->watchThread.joinable())
                                     {
                                         const auto future = std::async(std::launch::async, [&project]()
                                         {
                                             project->watchThread.join();
                                         });

                                         // 5초 타임아웃으로 join 대기
                                         if (future.wait_for(std::chrono::seconds(5)) == std::future_status::timeout)
                                         {
                                             std::cout << "[FileWatcher] Thread join timeout, forcing termination" << std::endl;
                                             project->watchThread.detach(); // 강제 종료
                                         }
                                     }

                                     CloseHandle(project->directoryHandle);
                                     return true;
                                 }
                                 return false;
                             });

    watchedProjects.erase(it, watchedProjects.end());
}

void FileWatcher::StopAllWatching()
{
    std::lock_guard<std::mutex> lock(projectsMutex);

    std::cout << "[FileWatcher] Stopping all watches..." << std::endl;

    for (const auto &project: watchedProjects)
    {
        project->shouldStop = true;
        SetEvent(project->stopEvent);
        CancelIo(project->directoryHandle);
    }

    // 모든 스레드 종료 대기
    int joinedCount = 0;
    for (auto &project: watchedProjects)
    {
        if (project->watchThread.joinable())
        {
            auto future = std::async(std::launch::async, [&project]()
            {
                project->watchThread.join();
            });

            if (future.wait_for(std::chrono::seconds(3)) == std::future_status::ready)
            {
                joinedCount++;
            }
            else
            {
                std::cout << "[FileWatcher] Thread join timeout: " << project->projectName << std::endl;
                project->watchThread.detach();
            }
        }

        CloseHandle(project->directoryHandle);
    }

    watchedProjects.clear();
    std::cout << "[FileWatcher] All watches stopped" << std::endl;
}

size_t FileWatcher::GetWatchedProjectCount() const
{
    std::lock_guard<std::mutex> lock(projectsMutex);
    return watchedProjects.size();
}

std::vector<std::string> FileWatcher::GetWatchedProjects() const
{
    std::lock_guard<std::mutex> lock(projectsMutex);

    std::vector<std::string> projects;
    for (const auto &project: watchedProjects)
    {
        projects.push_back(project->projectPath);
    }

    return projects;
}
