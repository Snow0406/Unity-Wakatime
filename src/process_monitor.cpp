#include "process_monitor.h"
#include <tlhelp32.h>   // Windows 프로세스 스냅샷 API

ProcessMonitor::ProcessMonitor()
{
    std::cout << "[ProcessMonitor] Initialized" << std::endl;
}

ProcessMonitor::~ProcessMonitor()
{
    CleanupWMI();
    activeInstances.clear();
    std::cout << "[ProcessMonitor] Destroyed" << std::endl;
}

BSTR ProcessMonitor::StringToBSTR(const std::wstring &str)
{
    return SysAllocString(str.c_str());
}

std::string ProcessMonitor::BSTRToString(const BSTR bstr)
{
    if (bstr == nullptr) return "";

    const int len = WideCharToMultiByte(CP_UTF8, 0, bstr, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";

    std::string result(len - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, bstr, -1, &result[0], len, nullptr, nullptr);

    return result;
}

bool ProcessMonitor::InitializeWMI()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr))
    {
        std::cerr << "[ProcessMonitor] COM initialization failed" << std::endl;
        return false;
    }

    hr = CoInitializeSecurity(
        nullptr, -1, nullptr, nullptr,
        RPC_C_AUTHN_LEVEL_NONE, RPC_C_IMP_LEVEL_IMPERSONATE,
        nullptr, EOAC_NONE, nullptr);

    hr = CoCreateInstance(CLSID_WbemLocator, nullptr,
                          CLSCTX_INPROC_SERVER, IID_IWbemLocator, (LPVOID *) &pLocator);

    if (FAILED(hr))
    {
        std::cerr << "[ProcessMonitor] WMI Locator creation failed" << std::endl;
        CoUninitialize();
        return false;
    }

    const BSTR strNetworkResource = StringToBSTR(L"ROOT\\CIMV2");

    hr = pLocator->ConnectServer(
        strNetworkResource,
        nullptr, nullptr, nullptr,
        0, nullptr, nullptr,
        &pService
    );

    SysFreeString(strNetworkResource);

    if (FAILED(hr))
    {
        std::cerr << "[ProcessMonitor] WMI Service connection failed" << std::endl;
        pLocator->Release();
        CoUninitialize();
        return false;
    }

    wmiInitialized = true;
    std::cout << "[ProcessMonitor] WMI initialized successfully" << std::endl;
    return true;
}

std::string ProcessMonitor::GetRealCommandLine(const DWORD pid)
{
    if (!wmiInitialized && !InitializeWMI()) return "";

    // WQL 쿼리 생성 - 특정 프로세스 ID의 커맨드 라인 가져오기
    std::string commandLine;
    const std::wstring query = L"SELECT CommandLine FROM Win32_Process WHERE ProcessId = " + std::to_wstring(pid);

    IEnumWbemClassObject *pEnumerator = nullptr;

    const BSTR strQueryLanguage = StringToBSTR(L"WQL");
    const BSTR strQuery = StringToBSTR(query);

    // 쿼리 실행
    HRESULT hr = pService->ExecQuery(
        strQueryLanguage,
        strQuery,
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        nullptr,
        &pEnumerator);

    SysFreeString(strQueryLanguage);
    SysFreeString(strQuery);

    if (SUCCEEDED(hr))
    {
        IWbemClassObject *pObj = nullptr;
        ULONG returnValue = 0;

        // 결과 가져오기
        hr = pEnumerator->Next(WBEM_INFINITE, 1, &pObj, &returnValue);
        if (SUCCEEDED(hr) && returnValue > 0)
        {
            VARIANT cmdLineVariant;
            VariantInit(&cmdLineVariant);

            // CommandLine 속성 가져오기
            hr = pObj->Get(L"CommandLine", 0, &cmdLineVariant, nullptr, nullptr);
            if (SUCCEEDED(hr) && cmdLineVariant.vt == VT_BSTR && cmdLineVariant.bstrVal != nullptr)
            {
                const int len = WideCharToMultiByte(CP_UTF8, 0, cmdLineVariant.bstrVal, -1,
                                                    nullptr, 0, nullptr, nullptr);
                if (len > 0)
                {
                    commandLine.resize(len - 1);
                    WideCharToMultiByte(CP_UTF8, 0, cmdLineVariant.bstrVal, -1, &commandLine[0],
                                        len, nullptr, nullptr);
                }
            }

            VariantClear(&cmdLineVariant);
            pObj->Release();
        }

        pEnumerator->Release();
    }

    return commandLine;
}

void ProcessMonitor::CleanupWMI()
{
    if (wmiInitialized)
    {
        if (pService)
        {
            pService->Release();
            pService = nullptr;
        }
        if (pLocator)
        {
            pLocator->Release();
            pLocator = nullptr;
        }
        CoUninitialize();
        wmiInitialized = false;
        std::cout << "[ProcessMonitor] WMI cleaned up" << std::endl;
    }
}

std::vector<UnityInstance> ProcessMonitor::ScanUnityProcesses()
{
    std::vector<UnityInstance> foundInstances;

    // CreateToolhelp32Snapshot: 현재 시스템의 프로세스 스냅샷 생성
    // TH32CS_SNAPPROCESS: 모든 프로세스 정보를 가져옴
    const HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE)
    {
        std::cerr << "[ProcessMonitor] CreateToolhelp32Snapshot failed" << std::endl;
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
                if (std::string projectPath = GetProcessCommandLine(entry.th32ProcessID); !projectPath.empty())
                {
                    UnityInstance instance;
                    instance.processId = entry.th32ProcessID;
                    instance.projectPath = projectPath;
                    instance.projectName = GetProjectName(projectPath);
                    instance.editorVersion = GetUnityEditorVersion(projectPath);

                    foundInstances.push_back(instance);
                }
            }
        } while (Process32NextW(hSnapshot, &entry));
    }

    CloseHandle(hSnapshot);

    std::cout << "[ProcessMonitor] Scan complete. Found " << foundInstances.size() << " Unity instances" << std::endl;
    return foundInstances;
}

std::string ProcessMonitor::GetProcessCommandLine(const DWORD pid)
{
    const std::string fullCommandLine = GetRealCommandLine(pid);

    if (fullCommandLine.empty())
    {
        std::cout << "[ProcessMonitor] Could not get command line for PID " << pid << std::endl;
        return "";
    }

    std::string projectPath = ExtractProjectPath(fullCommandLine);

    if (projectPath.empty())
    {
        std::cout << "[ProcessMonitor] No -projectPath found in command line: " << fullCommandLine << std::endl;
        return "";
    }

    if (!IsUnityProject(projectPath))
    {
        std::cout << "[ProcessMonitor] Path is not a valid Unity project: " << projectPath << std::endl;
        return "";
    }

    return projectPath;
}

std::string ProcessMonitor::ExtractProjectPath(const std::string &commandLine)
{
    std::string lowerCommandLine = commandLine;
    std::transform(lowerCommandLine.begin(), lowerCommandLine.end(), lowerCommandLine.begin(), tolower);

    size_t pos = lowerCommandLine.find("-projectpath");
    if (pos == std::string::npos) return "";

    // 원래 문자열에서 해당 위치 찾기
    pos = commandLine.find("-projectpath", pos - (lowerCommandLine.length() - commandLine.length()));
    if (pos == std::string::npos)
    {
        pos = commandLine.find("-projectpath", pos - (lowerCommandLine.length() - commandLine.length()));
    }

    // -projectPath 다음 공백이나 탭 찾기
    pos = commandLine.find_first_of(" \t", pos);
    if (pos == std::string::npos)
    {
        return "";
    }

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

std::vector<UnityInstance> ProcessMonitor::GetNewInstances()
{
    const std::vector<UnityInstance> currentInstances = ScanUnityProcesses();
    std::vector<UnityInstance> newInstances;

    for (const auto &instance: currentInstances)
    {
        if (activeInstances.find(instance.processId) == activeInstances.end())
        {
            newInstances.push_back(instance);
            activeInstances[instance.processId] = instance;
        }
    }

    return newInstances;
}

std::vector<UnityInstance> ProcessMonitor::GetClosedInstances()
{
    std::vector<UnityInstance> closedInstances;

    auto it = activeInstances.begin();
    while (it != activeInstances.end())
    {
        if (!IsProcessRunning(it->first))
        {
            closedInstances.push_back(it->second);
            it = activeInstances.erase(it);
        } else ++it;
    }

    return closedInstances;
}

bool ProcessMonitor::IsUnityProject(const std::string &projectPath)
{
    return fs::exists(projectPath + "\\Assets") && fs::exists(projectPath + "\\ProjectSettings");
}

bool ProcessMonitor::IsProcessRunning(const DWORD pid)
{
    const HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (hProcess == nullptr) return false;

    DWORD exitCode;
    const bool isRunning = GetExitCodeProcess(hProcess, &exitCode) && (exitCode == STILL_ACTIVE);
    CloseHandle(hProcess);
    return isRunning;
}
