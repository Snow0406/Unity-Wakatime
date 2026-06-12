#include "process_monitor.h"
#include <tlhelp32.h>   // Windows 프로세스 스냅샷 API
#include <winternl.h>   // NtQueryInformationProcess, PEB, RTL_USER_PROCESS_PARAMETERS
#include <shellapi.h>   // CommandLineToArgvW

namespace
{
    // ntdll의 NtQueryInformationProcess를 동적 로드해 ntdll 링크 의존을 피한다.
    using NtQueryInformationProcess_t = NTSTATUS(NTAPI *)(
        HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);

    NtQueryInformationProcess_t GetNtQueryInformationProcess()
    {
        static const auto fn = []() -> NtQueryInformationProcess_t
        {
            if (const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"))
            {
                return reinterpret_cast<NtQueryInformationProcess_t>(
                    reinterpret_cast<void *>(GetProcAddress(ntdll, "NtQueryInformationProcess")));
            }
            return nullptr;
        }();
        return fn;
    }

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

    std::string ToLower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](const unsigned char c) { return static_cast<char>(::tolower(c)); });
        return s;
    }

    std::vector<std::string> CommandLineToArgsUtf8(const std::string& commandLine)
    {
        std::vector<std::string> args;
        const std::wstring wide = Utf8ToWide(commandLine);
        if (wide.empty()) return args;

        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(wide.c_str(), &argc);
        if (argv == nullptr) return args;

        args.reserve(static_cast<size_t>(argc));
        for (int i = 0; i < argc; ++i)
        {
            args.push_back(WideToUtf8(argv[i]));
        }
        LocalFree(argv);
        return args;
    }

    std::string ExtensionLower(const std::string& path)
    {
        const size_t slash = path.find_last_of("/\\");
        const size_t dot = path.find_last_of('.');
        if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
        return ToLower(path.substr(dot));
    }

    bool IsRegularFileUtf8(const std::string& path)
    {
        if (path.empty()) return false;
        std::error_code ec;
        return fs::is_regular_file(fs::path(Utf8ToWide(path)), ec) && !ec;
    }

    std::unordered_set<std::string> ExtensionSet(const AppDefinition& def)
    {
        std::unordered_set<std::string> extensions;
        for (const auto& ext : def.fileExtensions)
        {
            extensions.insert(ToLower(ext));
        }
        return extensions;
    }
}

ProcessMonitor::ProcessMonitor()
{
    WT_LOG("[ProcessMonitor] Initialized");
}

ProcessMonitor::~ProcessMonitor()
{
    activeInstances.clear();
    WT_LOG("[ProcessMonitor] Destroyed");
}

std::string ProcessMonitor::GetCommandLineViaPeb(const DWORD pid)
{
    const auto NtQueryInformationProcess = GetNtQueryInformationProcess();
    if (NtQueryInformationProcess == nullptr) return "";

    const HANDLE hProcess = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (hProcess == nullptr) return "";

    std::string commandLine;

    do
    {
        // 1) PEB 주소 조회
        PROCESS_BASIC_INFORMATION pbi;
        ZeroMemory(&pbi, sizeof(pbi));
        ULONG returnLength = 0;
        if (const NTSTATUS status = NtQueryInformationProcess(
                hProcess, ProcessBasicInformation, &pbi, sizeof(pbi), &returnLength);
            status < 0 || pbi.PebBaseAddress == nullptr)
        {
            break;
        }

        // 2) PEB 읽기 → ProcessParameters 포인터
        PEB peb;
        ZeroMemory(&peb, sizeof(peb));
        if (!ReadProcessMemory(hProcess, pbi.PebBaseAddress, &peb, sizeof(peb), nullptr) ||
            peb.ProcessParameters == nullptr)
        {
            break;
        }

        // 3) RTL_USER_PROCESS_PARAMETERS 읽기 → CommandLine(UNICODE_STRING)
        RTL_USER_PROCESS_PARAMETERS params;
        ZeroMemory(&params, sizeof(params));
        if (!ReadProcessMemory(hProcess, peb.ProcessParameters, &params, sizeof(params), nullptr) ||
            params.CommandLine.Buffer == nullptr ||
            params.CommandLine.Length == 0)
        {
            break;
        }

        // 4) CommandLine 버퍼 읽기 (Length는 바이트 단위)
        const USHORT byteLen = params.CommandLine.Length;
        std::wstring wCommandLine(byteLen / sizeof(WCHAR), L'\0');
        if (!ReadProcessMemory(hProcess, params.CommandLine.Buffer,
                               wCommandLine.data(), byteLen, nullptr))
        {
            break;
        }

        // 5) UTF-8 변환
        if (const int len = WideCharToMultiByte(CP_UTF8, 0, wCommandLine.c_str(),
                                                static_cast<int>(wCommandLine.size()),
                                                nullptr, 0, nullptr, nullptr); len > 0)
        {
            commandLine.resize(len);
            WideCharToMultiByte(CP_UTF8, 0, wCommandLine.c_str(),
                                static_cast<int>(wCommandLine.size()),
                                commandLine.data(), len, nullptr, nullptr);
        }
    } while (false);

    CloseHandle(hProcess);
    return commandLine;
}

std::wstring ProcessMonitor::GetProcessExeName(const DWORD pid)
{
    const HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProcess == nullptr) return L"";

    std::wstring result;
    WCHAR buf[MAX_PATH];
    DWORD size = MAX_PATH;
    if (QueryFullProcessImageNameW(hProcess, 0, buf, &size))
    {
        std::wstring full(buf, size);
        const size_t pos = full.find_last_of(L"/\\");
        result = (pos == std::wstring::npos) ? full : full.substr(pos + 1);
    }

    CloseHandle(hProcess);
    return result;
}

bool ProcessMonitor::ResolveInstance(const DWORD pid, const AppDefinition& def, AppInstance& instance)
{
    instance.appId = def.id;
    instance.processId = pid;

    if (def.cmdLineEntity == CmdLineEntity::UnityProjectPath)
    {
        const std::string commandLine = GetCommandLineViaPeb(pid);
        if (commandLine.empty()) return false;

        std::string projectPath = ExtractProjectPath(commandLine);
        if (projectPath.empty() || !IsUnityProject(projectPath))
        {
            WT_LOG("[ProcessMonitor] Not a valid Unity project for PID " << pid);
            return false;
        }

        instance.projectName = GetLeafName(projectPath);
        instance.editorVersion = GetUnityEditorVersion(projectPath);
        instance.projectPath = std::move(projectPath);
        return true;
    }

    if (def.cmdLineEntity == CmdLineEntity::PositionalFile)
    {
        // WindowTitle 앱: 프로세스 존재만으로 추적 대상. 커맨드라인 파일은 초기 entity(선택적).
        const std::string commandLine = GetCommandLineViaPeb(pid);
        if (const std::string file = commandLine.empty() ? "" : ExtractPositionalFile(commandLine, def);
            !file.empty())
        {
            instance.entity = file;
            instance.projectPath = GetParentPath(file);
            instance.projectName = GetLeafName(instance.projectPath);
        }
        return true;
    }

    return true;
}

std::vector<AppInstance> ProcessMonitor::ScanProcesses()
{
    std::vector<AppInstance> foundInstances;

    const HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE)
    {
        WT_ERR("[ProcessMonitor] CreateToolhelp32Snapshot failed");
        return foundInstances;
    }

    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(PROCESSENTRY32W);

    if (Process32FirstW(hSnapshot, &entry))
    {
        do
        {
            const AppDefinition* def = AppRegistry::FindByProcessName(entry.szExeFile);
            if (def == nullptr || !AppRegistry::IsEnabled(def->id)) continue;

            if (AppInstance instance; ResolveInstance(entry.th32ProcessID, *def, instance))
            {
                activeInstances[instance.processId] = instance;
                foundInstances.push_back(std::move(instance));
            }
        } while (Process32NextW(hSnapshot, &entry));
    }

    CloseHandle(hSnapshot);

    WT_LOG("[ProcessMonitor] Scan complete. Found " << foundInstances.size() << " app instances");
    return foundInstances;
}

void ProcessMonitor::PollChanges(std::vector<AppInstance>& started, std::vector<AppInstance>& closed)
{
    const HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE)
    {
        WT_ERR("[ProcessMonitor] CreateToolhelp32Snapshot failed");
        return;
    }

    // 1) 현재 살아 있는 활성 앱 PID → 정의 매핑 (exe 이름 비교 — 저비용)
    std::unordered_map<DWORD, const AppDefinition*> currentPids;

    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(PROCESSENTRY32W);

    if (Process32FirstW(hSnapshot, &entry))
    {
        do
        {
            const AppDefinition* def = AppRegistry::FindByProcessName(entry.szExeFile);
            if (def == nullptr || !AppRegistry::IsEnabled(def->id)) continue;
            currentPids[entry.th32ProcessID] = def;
        } while (Process32NextW(hSnapshot, &entry));
    }

    CloseHandle(hSnapshot);

    // 2) 새로 등장한 PID만 비싼 해석 수행
    for (const auto& [pid, def] : currentPids)
    {
        if (activeInstances.find(pid) != activeInstances.end()) continue;

        if (AppInstance instance; ResolveInstance(pid, *def, instance))
        {
            activeInstances[pid] = instance;
            started.push_back(std::move(instance));
        }
    }

    // 3) 더 이상 보이지 않는 PID는 종료된 것으로 판정 (동일 스냅샷으로 diff)
    auto it = activeInstances.begin();
    while (it != activeInstances.end())
    {
        if (currentPids.find(it->first) == currentPids.end())
        {
            closed.push_back(it->second);
            it = activeInstances.erase(it);
        }
        else ++it;
    }
}

const AppInstance* ProcessMonitor::ResolveByPid(const DWORD pid)
{
    if (const auto it = activeInstances.find(pid); it != activeInstances.end())
    {
        return &it->second;
    }

    // 맵에 없으면 즉석 해석 (포커스가 스캔보다 먼저 도착한 경우)
    const std::wstring exe = GetProcessExeName(pid);
    if (exe.empty()) return nullptr;

    const AppDefinition* def = AppRegistry::FindByProcessName(exe);
    if (def == nullptr || !AppRegistry::IsEnabled(def->id)) return nullptr;

    AppInstance instance;
    if (!ResolveInstance(pid, *def, instance)) return nullptr;

    const auto [iter, _] = activeInstances.insert_or_assign(pid, std::move(instance));
    return &iter->second;
}

void ProcessMonitor::PurgeApp(const std::string& appId)
{
    for (auto it = activeInstances.begin(); it != activeInstances.end();)
    {
        if (it->second.appId == appId) it = activeInstances.erase(it);
        else ++it;
    }
}

std::unordered_set<std::string> ProcessMonitor::GetActiveAppIds() const
{
    std::unordered_set<std::string> ids;
    for (const auto& [pid, instance] : activeInstances)
    {
        ids.insert(instance.appId);
    }
    return ids;
}

std::string ProcessMonitor::ExtractProjectPath(const std::string &commandLine)
{
    std::string lowerCommandLine = commandLine;
    std::transform(lowerCommandLine.begin(), lowerCommandLine.end(), lowerCommandLine.begin(),
                   [](const unsigned char c) { return static_cast<char>(::tolower(c)); });

    constexpr char projectPathArg[] = "-projectpath";
    size_t pos = lowerCommandLine.find(projectPathArg);
    if (pos == std::string::npos) return "";

    pos += sizeof(projectPathArg) - 1;

    // 공백/탭 건너뛰기
    pos = commandLine.find_first_not_of(" \t", pos);
    if (pos == std::string::npos)
    {
        return "";
    }

    std::string projectPath;

    // 따옴표로 둘러싸인 경로 처리
    if (commandLine[pos] == '"')
    {
        pos++;
        if (const size_t endPos = commandLine.find('"', pos); endPos != std::string::npos)
        {
            projectPath = commandLine.substr(pos, endPos - pos);
        }
    }
    else
    {
        size_t endPos = commandLine.find_first_of(" \t", pos);
        if (endPos == std::string::npos)
        {
            endPos = commandLine.length();
        }
        projectPath = commandLine.substr(pos, endPos - pos);
    }
    return projectPath;
}

std::string ProcessMonitor::ExtractPositionalFile(const std::string& commandLine, const AppDefinition& def)
{
    const std::vector<std::string> args = CommandLineToArgsUtf8(commandLine);
    if (args.size() <= 1) return "";

    const std::unordered_set<std::string> knownExtensions = ExtensionSet(def);
    std::string lastExistingCandidate;
    std::string lastExtensionCandidate;

    for (size_t k = 1; k < args.size(); ++k)
    {
        const std::string& arg = args[k];
        if (arg.empty() || arg[0] == '-') continue;

        const std::string ext = ExtensionLower(arg);
        if (ext.empty()) continue;
        if (!knownExtensions.empty() && knownExtensions.find(ext) == knownExtensions.end()) continue;

        // 실제 파일이면 가장 신뢰한다. 상대 경로처럼 이 프로세스에서 존재 확인이 어려운
        // 경우도 제목 파싱 fallback에 쓸 수 있도록 확장자 후보를 별도로 보관한다.
        if (IsRegularFileUtf8(arg))
        {
            lastExistingCandidate = arg;
        }
        lastExtensionCandidate = arg;
    }

    if (!lastExistingCandidate.empty()) return lastExistingCandidate;
    return lastExtensionCandidate;
}

std::string ProcessMonitor::GetLeafName(const std::string& path)
{
    if (path.empty()) return "";
    const size_t pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::string ProcessMonitor::GetParentPath(const std::string& path)
{
    if (path.empty()) return "";
    const size_t pos = path.find_last_of("/\\");
    return pos == std::string::npos ? "" : path.substr(0, pos);
}

std::string ProcessMonitor::GetUnityEditorVersion(const std::string& projectPath)
{
    const fs::path versionFilePath =
        fs::path(Utf8ToWide(projectPath)) / L"ProjectSettings" / L"ProjectVersion.txt";

    if (!std::filesystem::exists(versionFilePath)) return "";
    return ParseProjectVersionFile(versionFilePath);
}

std::string ProcessMonitor::ParseProjectVersionFile(const fs::path &versionFilePath)
{
    std::ifstream file(versionFilePath);
    if (!file.is_open()) return "";

    std::string line;
    while (std::getline(file, line))
    {
        if (line.find("m_EditorVersion:") != std::string::npos)
        {
            if (const size_t colonPos = line.find(':'); colonPos != std::string::npos)
            {
                std::string version = line.substr(colonPos + 1);

                version.erase(0, version.find_first_not_of(" \t"));
                version.erase(version.find_last_not_of(" \t\r\n") + 1);

                if (!version.empty())
                {
                    std::vector<std::string> parts;
                    std::stringstream ss(version);
                    std::string part;

                    while (std::getline(ss, part, '.'))
                    {
                        parts.push_back(part);
                        if (parts.size() >= 2) break;
                    }

                    if (parts.size() >= 2)
                    {
                        return parts[0] + "." + parts[1];
                    }
                }
            }
        }
    }

    return "";
}

bool ProcessMonitor::IsUnityProject(const std::string &projectPath)
{
    const fs::path root(Utf8ToWide(projectPath));
    return fs::exists(root / L"Assets") && fs::exists(root / L"ProjectSettings");
}

bool ProcessMonitor::IsProcessRunning(const DWORD pid)
{
    const HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProcess == nullptr) return false;

    DWORD exitCode;
    const bool isRunning = GetExitCodeProcess(hProcess, &exitCode) && (exitCode == STILL_ACTIVE);
    CloseHandle(hProcess);
    return isRunning;
}
