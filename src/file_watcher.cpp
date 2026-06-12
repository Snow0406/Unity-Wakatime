#include "file_watcher.h"
#include "app_registry.h"
#include <utility>
#include <system_error>

namespace
{
    constexpr size_t kMaxPendingEvents = 1024;

    std::wstring Utf8ToWide(const std::string& s)
    {
        if (s.empty()) return L"";
        const int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        if (len <= 1) return L"";
        std::wstring w(static_cast<size_t>(len), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
        w.resize(static_cast<size_t>(len - 1));
        return w;
    }

    std::string WideToUtf8(const std::wstring& w)
    {
        if (w.empty()) return "";
        const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                            nullptr, 0, nullptr, nullptr);
        if (len <= 0) return "";
        std::string s(static_cast<size_t>(len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                            s.data(), len, nullptr, nullptr);
        return s;
    }
}

FileWatcher::FileWatcher()
{
    WT_LOG("[FileWatcher] Initialized");
}

FileWatcher::~FileWatcher()
{
    StopAllWatching();
    WT_LOG("[FileWatcher] Destroyed");
}

void FileWatcher::SetChangeCallback(std::function<void(const FileChangeEvent &)> callback)
{
    changeCallback = std::move(callback);
    WT_LOG("[FileWatcher] Change callback set");
}

void FileWatcher::SetNotifyCallback(std::function<void()> callback)
{
    notifyCallback = std::move(callback);
    WT_LOG("[FileWatcher] Notify callback set");
}

bool FileWatcher::StartWatching(const std::string &appId, const std::string &projectPath,
                                const std::string &projectName, const std::string &editorVersion)
{
    std::lock_guard<std::mutex> lock(projectsMutex);

    // 이미 감시 중인지 확인
    for (const auto &project: watchedProjects)
    {
        if (project->projectPath == projectPath) return true;
    }

    if (!fs::exists(Utf8ToWide(projectPath)))
    {
        WT_ERR("[FileWatcher] Project path does not exist: " << projectPath);
        return false;
    }

    auto project = std::make_unique<WatchedProject>();
    if (project->stopEvent == nullptr || project->ioEvent == nullptr)
    {
        WT_ERR("[FileWatcher] Failed to create watcher events for: " << projectPath);
        return false;
    }

    project->appId = appId;
    project->projectPath = projectPath;
    project->projectName = projectName;
    project->editorVersion = editorVersion;
    project->shouldStop = false;

    // 앱 정의의 확장자 필터를 소문자 set으로 적재 (워커 스레드는 immutable 정의만 읽음)
    if (const AppDefinition *def = AppRegistry::FindById(appId))
    {
        for (const auto &ext : def->fileExtensions)
        {
            std::string lower = ext;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](const unsigned char c) { return static_cast<char>(::tolower(c)); });
            project->extensions.insert(std::move(lower));
        }
    }

    ZeroMemory(&project->overlapped, sizeof(OVERLAPPED));
    project->overlapped.hEvent = project->ioEvent;
    ZeroMemory(project->buffer, sizeof(project->buffer));

    // 한글/비ASCII 경로 보존을 위해 wide path로 디렉토리 핸들 오픈.
    project->directoryHandle = CreateFileW(
        Utf8ToWide(projectPath).c_str(),
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
        WT_ERR("[FileWatcher] Failed to open directory: " << projectPath << " (Error: " << error << ")");
        return false;
    }

    WT_LOG("[FileWatcher] Started watching: " << projectName << " at " << projectPath);

    WatchedProject *projectPtr = project.get();
    try
    {
        project->watchThread = std::thread(&FileWatcher::WatchProjectThread, this, projectPtr);
    }
    catch (const std::system_error &e)
    {
        WT_ERR("[FileWatcher] Failed to start watch thread for " << projectName << ": " << e.what());
        return false;
    }

    watchedProjects.push_back(std::move(project));

    return true;
}

void FileWatcher::WatchProjectThread(WatchedProject *project)
{
    if (project == nullptr ||
        project->directoryHandle == nullptr ||
        project->directoryHandle == INVALID_HANDLE_VALUE ||
        project->stopEvent == nullptr ||
        project->ioEvent == nullptr)
    {
        WT_ERR("[FileWatcher] Invalid watch project state, thread exit");
        return;
    }

    WT_LOG("[FileWatcher] Watch thread started for: " << project->projectName);

    while (!project->shouldStop)
    {
        DWORD bytesReturned = 0;
        ZeroMemory(&project->overlapped, sizeof(OVERLAPPED));
        project->overlapped.hEvent = project->ioEvent;
        ResetEvent(project->ioEvent);

        const BOOL result = ReadDirectoryChangesW(
            project->directoryHandle,
            project->buffer,
            sizeof(project->buffer),
            TRUE, // 하위 디렉토리 포함 (재귀)
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION | FILE_NOTIFY_CHANGE_FILE_NAME,
            &bytesReturned,
            &project->overlapped,
            nullptr
        );

        if (!result)
        {
            const DWORD error = GetLastError();
            if (error == ERROR_NOTIFY_ENUM_DIR)
            {
                // 버퍼 오버플로(대량 변경): 개별 이벤트 유실. 재발급으로 계속 감시.
                WT_ERR("[FileWatcher] Notify buffer overflow for " << project->projectName << ", continuing");
                QueueSyntheticProjectScan(project);
                continue;
            }
            if (error != ERROR_IO_PENDING)
            {
                WT_ERR("[FileWatcher] ReadDirectoryChangesW failed for " << project->projectName << " (Error: " << error << ")");
                break;
            }
        }
        else
        {
            // 드물게 동기 완료될 수 있으므로 즉시 처리
            if (bytesReturned > 0)
            {
                ProcessFileChanges(project->buffer, bytesReturned, project);
                continue;
            }
        }

        const HANDLE waitHandles[2] = {
            project->ioEvent,
            project->stopEvent
        };

        const DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);

        switch (waitResult)
        {
            case WAIT_OBJECT_0: // ioEvent 신호 (I/O 완료)
            {
                if (GetOverlappedResult(project->directoryHandle, &project->overlapped, &bytesReturned, FALSE))
                {
                    if (bytesReturned > 0)
                        ProcessFileChanges(project->buffer, bytesReturned, project);
                    else
                    {
                        WT_ERR("[FileWatcher] Notify buffer overflow (0 bytes) for " << project->projectName);
                        QueueSyntheticProjectScan(project);
                    }
                }
                else
                {
                    if (const DWORD error = GetLastError(); error != ERROR_OPERATION_ABORTED && error != ERROR_IO_INCOMPLETE)
                    {
                        WT_ERR("[FileWatcher] GetOverlappedResult failed for " << project->projectName << " (Error: " << error << ")");
                    }
                    if (project->shouldStop)
                    {
                        goto thread_exit;
                    }
                }
                break;
            }

            case (WAIT_OBJECT_0 + 1): // stopEvent 신호 (종료 요청)
                WT_LOG("[FileWatcher] Stop event received for: " << project->projectName);
                goto thread_exit;

            default:
                WT_ERR("[FileWatcher] WaitForMultipleObjects failed for " << project->projectName << " (Error: " << GetLastError() << ")");
                goto thread_exit;
        }
    }

thread_exit:
    WT_LOG("[FileWatcher] Watch thread stopped for: " << project->projectName);
}

void FileWatcher::QueueFileEvent(WatchedProject *project, const std::string &fileName,
                                 const std::string &fullPath, const DWORD action)
{
    if (project == nullptr) return;

    FileChangeEvent event;
    event.appId = project->appId;
    event.filePath = fullPath;
    event.fileName = fileName;
    event.projectPath = project->projectPath;
    event.projectName = project->projectName;
    event.action = action;
    event.timestamp = std::chrono::system_clock::now();

    {
        std::lock_guard<std::mutex> lock(pendingEventsMutex);
        while (pendingEvents.size() >= kMaxPendingEvents)
        {
            pendingEvents.pop_front();
        }
        pendingEvents.emplace_back(std::move(event));
    }

    if (notifyCallback && !notifyScheduled.exchange(true))
    {
        notifyCallback();
    }
}

void FileWatcher::QueueSyntheticProjectScan(WatchedProject *project)
{
    if (project == nullptr) return;

    struct Candidate
    {
        std::string fileName;
        std::string fullPath;
        fs::file_time_type writeTime;
    };

    constexpr size_t kMaxSyntheticEvents = 128;
    std::vector<Candidate> candidates;

    const fs::path root(Utf8ToWide(project->projectPath));
    std::error_code ec;
    if (!fs::exists(root, ec)) return;

    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;

    for (; !ec && it != end; it.increment(ec))
    {
        const fs::directory_entry& entry = *it;
        const fs::path path = entry.path();
        const std::string leaf = WideToUtf8(path.filename().wstring());

        const bool isDirectory = entry.is_directory(ec);
        if (ec)
        {
            ec.clear();
            continue;
        }
        if (isDirectory)
        {
            if (ShouldIgnoreFolder(leaf))
            {
                it.disable_recursion_pending();
            }
            continue;
        }

        const bool isRegularFile = entry.is_regular_file(ec);
        if (ec)
        {
            ec.clear();
            continue;
        }
        if (!isRegularFile) continue;

        const fs::path relativePath = path.lexically_relative(root);
        std::string fileName = WideToUtf8(relativePath.generic_wstring());
        if (fileName.empty()) continue;

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
        if (shouldIgnore || !IsTrackedFile(fileName, project->extensions)) continue;

        std::string fullPath = project->projectPath + "/" + fileName;
        std::replace(fullPath.begin(), fullPath.end(), '\\', '/');

        const auto writeTime = entry.last_write_time(ec);
        if (ec)
        {
            ec.clear();
            continue;
        }

        candidates.push_back({std::move(fileName), std::move(fullPath), writeTime});
    }

    const size_t count = std::min(kMaxSyntheticEvents, candidates.size());
    std::partial_sort(candidates.begin(), candidates.begin() + count, candidates.end(),
                      [](const Candidate& a, const Candidate& b)
                      {
                          return a.writeTime > b.writeTime;
                      });

    for (size_t i = 0; i < count; ++i)
    {
        QueueFileEvent(project, candidates[i].fileName, candidates[i].fullPath, FILE_ACTION_MODIFIED);
    }

    if (count > 0)
    {
        WT_LOG("[FileWatcher] Queued " << count << " synthetic event(s) for " << project->projectName);
    }
}

void FileWatcher::ProcessFileChanges(char *buffer, DWORD bytesReturned, WatchedProject *project)
{
    auto *info = reinterpret_cast<FILE_NOTIFY_INFORMATION *>(buffer);

    do
    {
        std::wstring wFileName(info->FileName, info->FileNameLength / sizeof(WCHAR));

        if (int len = WideCharToMultiByte(CP_UTF8, 0, wFileName.c_str(), -1, nullptr, 0, nullptr, nullptr); len > 0)
        {
            std::string fileName(len - 1, 0); // null terminator 제외
            WideCharToMultiByte(CP_UTF8, 0, wFileName.c_str(), -1, &fileName[0], len, nullptr, nullptr);

            std::string fullPath = project->projectPath + "\\" + fileName;

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

            if (!shouldIgnore && IsTrackedFile(fileName, project->extensions))
            {
                WT_LOG("[FileWatcher] Change: " << fileName << " in " << project->projectName);
                QueueFileEvent(project, fileName, fullPath, info->Action);
            }
        }

        if (info->NextEntryOffset == 0)
        {
            break;
        }

        info = reinterpret_cast<FILE_NOTIFY_INFORMATION *>(reinterpret_cast<char *>(info) + info->NextEntryOffset);
    } while (true);
}

bool FileWatcher::IsTrackedFile(const std::string &fileName, const std::unordered_set<std::string> &extensions) const
{
    const size_t dotPos = fileName.find_last_of('.');
    if (dotPos == std::string::npos) return false;

    std::string extension = fileName.substr(dotPos);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](const unsigned char c) { return static_cast<char>(::tolower(c)); });

    return extensions.find(extension) != extensions.end();
}

bool FileWatcher::ShouldIgnoreFolder(const std::string &folderName) const
{
    std::string lower = folderName;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](const unsigned char c) { return static_cast<char>(::tolower(c)); });

    return Config::GetIgnoreFolders().count(lower) > 0;
}

void FileWatcher::StopWatching(const std::string &projectPath)
{
    std::unique_ptr<WatchedProject> projectToStop;

    {
        std::lock_guard<std::mutex> lock(projectsMutex);

        const auto it = std::find_if(watchedProjects.begin(), watchedProjects.end(),
                                     [&projectPath](const std::unique_ptr<WatchedProject> &project)
                                     {
                                         return project->projectPath == projectPath;
                                     });

        if (it == watchedProjects.end())
        {
            return;
        }

        projectToStop = std::move(*it);
        watchedProjects.erase(it);
    }

    WT_LOG("[FileWatcher] Stopping watch for: " << projectToStop->projectName);
    projectToStop->shouldStop = true;

    if (projectToStop->stopEvent != nullptr)
    {
        SetEvent(projectToStop->stopEvent);
    }
    if (projectToStop->directoryHandle != nullptr && projectToStop->directoryHandle != INVALID_HANDLE_VALUE)
    {
        CancelIoEx(projectToStop->directoryHandle, nullptr);
    }
    if (projectToStop->watchThread.joinable())
    {
        projectToStop->watchThread.join();
    }
}

void FileWatcher::StopWatchingByApp(const std::string &appId)
{
    std::vector<std::unique_ptr<WatchedProject>> projectsToStop;

    {
        std::lock_guard<std::mutex> lock(projectsMutex);
        auto it = watchedProjects.begin();
        while (it != watchedProjects.end())
        {
            if ((*it)->appId == appId)
            {
                projectsToStop.push_back(std::move(*it));
                it = watchedProjects.erase(it);
            }
            else ++it;
        }
    }

    for (const auto &project: projectsToStop)
    {
        project->shouldStop = true;
        if (project->stopEvent != nullptr) SetEvent(project->stopEvent);
        if (project->directoryHandle != nullptr && project->directoryHandle != INVALID_HANDLE_VALUE)
        {
            CancelIoEx(project->directoryHandle, nullptr);
        }
    }
    for (auto &project: projectsToStop)
    {
        if (project->watchThread.joinable()) project->watchThread.join();
    }
}

void FileWatcher::StopAllWatching()
{
    std::vector<std::unique_ptr<WatchedProject>> projectsToStop;

    {
        std::lock_guard<std::mutex> lock(projectsMutex);
        projectsToStop.swap(watchedProjects);
    }

    WT_LOG("[FileWatcher] Stopping all watches...");

    for (const auto &project: projectsToStop)
    {
        project->shouldStop = true;
        if (project->stopEvent != nullptr)
        {
            SetEvent(project->stopEvent);
        }
        if (project->directoryHandle != nullptr && project->directoryHandle != INVALID_HANDLE_VALUE)
        {
            CancelIoEx(project->directoryHandle, nullptr);
        }
    }

    for (auto &project: projectsToStop)
    {
        if (project->watchThread.joinable())
        {
            project->watchThread.join();
        }
    }

    WT_LOG("[FileWatcher] All watches stopped");
}

void FileWatcher::DrainPendingEvents(const size_t maxEvents)
{
    if (maxEvents == 0)
    {
        return;
    }

    // 드레인 시작 전에 예약 플래그를 내려, 이 시점 이후 도착하는 이벤트는 새 통지를 post하도록 한다.
    notifyScheduled.store(false);

    if (!changeCallback) return;

    bool moreRemaining = false;
    std::vector<FileChangeEvent> localEvents;
    {
        std::lock_guard<std::mutex> lock(pendingEventsMutex);
        const size_t count = std::min(maxEvents, pendingEvents.size());
        localEvents.reserve(count);
        for (size_t i = 0; i < count; ++i)
        {
            localEvents.emplace_back(std::move(pendingEvents.front()));
            pendingEvents.pop_front();
        }

        if (!pendingEvents.empty()) moreRemaining = true;
    }

    for (const auto &event: localEvents)
    {
        changeCallback(event);
    }

    // maxEvents 한도로 다 비우지 못했으면 다음 처리를 위해 통지를 다시 예약
    if (moreRemaining && notifyCallback && !notifyScheduled.exchange(true))
    {
        notifyCallback();
    }
}

size_t FileWatcher::GetWatchedProjectCount() const
{
    std::lock_guard<std::mutex> lock(projectsMutex);
    return watchedProjects.size();
}

std::vector<WatchedProjectInfo> FileWatcher::GetWatchedProjects() const
{
    std::lock_guard<std::mutex> lock(projectsMutex);

    std::vector<WatchedProjectInfo> projects;
    projects.reserve(watchedProjects.size());

    for (const auto &project: watchedProjects)
    {
        WatchedProjectInfo info;
        info.appId = project->appId;
        info.projectPath = project->projectPath;
        info.projectName = project->projectName;
        info.editorVersion = project->editorVersion;
        projects.emplace_back(std::move(info));
    }

    return projects;
}
