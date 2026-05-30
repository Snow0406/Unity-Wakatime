#include "process_monitor.h"
#include <tlhelp32.h>   // Windows 프로세스 스냅샷 API
#include <winternl.h>   // NtQueryInformationProcess, PEB, RTL_USER_PROCESS_PARAMETERS

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

std::string ProcessMonitor::GetProcessCommandLine(const DWORD pid)
{
    const std::string fullCommandLine = GetCommandLineViaPeb(pid);

    if (fullCommandLine.empty())
    {
        WT_LOG("[ProcessMonitor] Could not get command line for PID " << pid);
        return "";
    }

    std::string projectPath = ExtractProjectPath(fullCommandLine);

    if (projectPath.empty())
    {
        WT_LOG("[ProcessMonitor] No -projectPath found in command line: " << fullCommandLine);
        return "";
    }

    if (!IsUnityProject(projectPath))
    {
        WT_LOG("[ProcessMonitor] Path is not a valid Unity project: " << projectPath);
        return "";
    }

    return projectPath;
}

bool ProcessMonitor::ResolveUnityInstance(const DWORD pid, UnityInstance& instance)
{
    std::string projectPath = GetProcessCommandLine(pid);
    if (projectPath.empty()) return false;

    instance.processId = pid;
    instance.projectName = GetProjectName(projectPath);
    instance.editorVersion = GetUnityEditorVersion(projectPath);
    instance.projectPath = std::move(projectPath);
    return true;
}

std::vector<UnityInstance> ProcessMonitor::ScanUnityProcesses()
{
    std::vector<UnityInstance> foundInstances;

    // CreateToolhelp32Snapshot: 현재 시스템의 프로세스 스냅샷 생성
    // TH32CS_SNAPPROCESS: 모든 프로세스 정보를 가져옴
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
            if (_wcsicmp(entry.szExeFile, L"Unity.exe") == 0 || _wcsicmp(entry.szExeFile, L"Unity") == 0)
            {
                if (UnityInstance instance; ResolveUnityInstance(entry.th32ProcessID, instance))
                {
                    activeInstances[instance.processId] = instance;
                    foundInstances.push_back(std::move(instance));
                }
            }
        } while (Process32NextW(hSnapshot, &entry));
    }

    CloseHandle(hSnapshot);

    WT_LOG("[ProcessMonitor] Scan complete. Found " << foundInstances.size() << " Unity instances");
    return foundInstances;
}

void ProcessMonitor::PollChanges(std::vector<UnityInstance>& started, std::vector<UnityInstance>& closed)
{
    const HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE)
    {
        WT_ERR("[ProcessMonitor] CreateToolhelp32Snapshot failed");
        return;
    }

    // 1) 현재 살아 있는 Unity PID 집합 수집 (exe 이름만 비교 — 저비용)
    std::unordered_set<DWORD> currentUnityPids;

    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(PROCESSENTRY32W);

    if (Process32FirstW(hSnapshot, &entry))
    {
        do
        {
            if (_wcsicmp(entry.szExeFile, L"Unity.exe") == 0 || _wcsicmp(entry.szExeFile, L"Unity") == 0)
            {
                currentUnityPids.insert(entry.th32ProcessID);
            }
        } while (Process32NextW(hSnapshot, &entry));
    }

    CloseHandle(hSnapshot);

    // 2) 새로 등장한 PID만 비싼 해석 수행
    for (const DWORD pid : currentUnityPids)
    {
        if (activeInstances.find(pid) != activeInstances.end()) continue; // 이미 알려진 PID는 재조회 생략

        if (UnityInstance instance; ResolveUnityInstance(pid, instance))
        {
            activeInstances[pid] = instance;
            started.push_back(std::move(instance));
        }
    }

    // 3) 더 이상 보이지 않는 PID는 종료된 것으로 판정 (동일 스냅샷으로 diff)
    auto it = activeInstances.begin();
    while (it != activeInstances.end())
    {
        if (currentUnityPids.find(it->first) == currentUnityPids.end())
        {
            closed.push_back(it->second);
            it = activeInstances.erase(it);
        }
        else ++it;
    }
}

std::string ProcessMonitor::ExtractProjectPath(const std::string &commandLine)
{
    std::string lowerCommandLine = commandLine;
    std::transform(lowerCommandLine.begin(), lowerCommandLine.end(), lowerCommandLine.begin(), tolower);

    constexpr char projectPathArg[] = "-projectpath";
    size_t pos = lowerCommandLine.find(projectPathArg);
    if (pos == std::string::npos) return "";

    // lowerCommandLine and commandLine have identical lengths, so the index can
    // be reused to parse the original-cased command line.
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
        pos++; // 시작 따옴표 건너뛰기
        if (const size_t endPos = commandLine.find('"', pos); endPos != std::string::npos)
        {
            projectPath = commandLine.substr(pos, endPos - pos);
        }
    }
    else
    {
        // 따옴표 없는 경우 다음 공백까지
        size_t endPos = commandLine.find_first_of(" \t", pos);
        if (endPos == std::string::npos)
        {
            endPos = commandLine.length();
        }
        projectPath = commandLine.substr(pos, endPos - pos);
    }
    return projectPath;
}

std::string ProcessMonitor::GetProjectName(const std::string &projectPath)
{
    if (projectPath.empty()) return "";
    const fs::path path(projectPath);
    return path.filename().string();
}

std::string ProcessMonitor::GetUnityEditorVersion(const std::string& projectPath)
{
    const std::string versionFilePath = projectPath + "\\ProjectSettings\\ProjectVersion.txt";

    if (!std::filesystem::exists(versionFilePath)) return "";
    return ParseProjectVersionFile(versionFilePath);
}

std::string ProcessMonitor::ParseProjectVersionFile(const std::string &versionFilePath)
{
    std::ifstream file(versionFilePath);
    if (!file.is_open()) return "";

    std::string line;
    while (std::getline(file, line))
    {
        if (line.find("m_EditorVersion:") != std::string::npos)
        {
            // "m_EditorVersion: " 이후의 버전 문자열 추출
            if (const size_t colonPos = line.find(':'); colonPos != std::string::npos)
            {
                std::string version = line.substr(colonPos + 1);

                // 앞뒤 공백 제거
                version.erase(0, version.find_first_not_of(" \t"));
                version.erase(version.find_last_not_of(" \t\r\n") + 1);

                if (!version.empty())
                {
                    std::vector<std::string> parts;
                    std::stringstream ss(version);
                    std::string part;

                    // 점을 구분자로 분할
                    while (std::getline(ss, part, '.'))
                    {
                        parts.push_back(part);
                        if (parts.size() >= 2) break;
                    }

                    if (parts.size() >= 2)
                    {
                        std::string majorMinor = parts[0] + "." + parts[1];
                        return majorMinor;
                    }
                }
            }
        }
    }

    return "";
}

bool ProcessMonitor::IsUnityProject(const std::string &projectPath)
{
    return fs::exists(projectPath + "\\Assets") && fs::exists(projectPath + "\\ProjectSettings");
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
