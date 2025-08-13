#pragma once

#include "globals.h"
#include <Wbemidl.h>    // WMI 인터페이스
#include <map>

#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

/**
 * Unity 프로세스들 감지
 */
class ProcessMonitor {
private:
    std::map<DWORD, UnityInstance> activeInstances;
    IWbemLocator* pLocator = nullptr;
    IWbemServices* pService = nullptr;
    bool wmiInitialized = false;

    /**
     * 문자열을 BSTR로 변환하는 헬퍼 함수
     * @param str 변환할 문자열
     * @return BSTR 포인터 (사용 후 SysFreeString으로 해제 필요)
     */
    BSTR StringToBSTR(const std::wstring& str);

    /**
     * BSTR을 문자열로 변환하는 헬퍼 함수
     * @param bstr 변환할 BSTR
     * @return 변환된 문자열
     */
    std::string BSTRToString(BSTR bstr);

    /**
     * WMI를 사용해서 실제 프로세스 커맨드 라인 가져오기
     * @param pid 대상 프로세스 ID
     * @return 커맨드 라인
     */
    std::string GetRealCommandLine(DWORD pid);

    /**
     * WMI 초기화 (COM 초기화)
     */
    bool InitializeWMI();

    /**
     * WMI 정리
     */
    void CleanupWMI();

    /**
     * 특정 프로세스의 커맨드 라인을 가져오기
     * @param pid 대상 프로세스 ID
     * @return 커맨드 라인, 실패시 빈문자열
     */
    std::string GetProcessCommandLine(DWORD pid);

    /**
     * 커맨드 라인에서 Unity 프로젝트 경로 추출
     * @param commandLine 커맨드 라인
     * @return 프로젝트 경로, 실패시 빈문자열
     */
    std::string ExtractProjectPath(const std::string& commandLine);

    /**
     * 프로젝트 경로에서 프로젝트 이름 가져오기
     * @param projectPath 프로젝트 경로
     * @return 프로젝트 이름, 실패시 빈문자열
     */
    std::string GetProjectName(const std::string& projectPath);

    /**
     * Unity 프로젝트의 에디터 버전 추출
     * @param projectPath Unity 프로젝트 경로
     * @return Unity 에디터 버전, 실패시 빈 문자열
     */
    std::string GetUnityEditorVersion(const std::string& projectPath);

    /**
     * ProjectVersion.txt 파일에서 버전 정보 파싱
     * @param versionFilePath ProjectVersion.txt 파일 경로
     * @return Unity 버전 문자열
     */
    std::string ParseProjectVersionFile(const std::string& versionFilePath);

    /**
     * 유니티 프로젝트인지 확인
     * @param projectPath 프로젝트 경로
     * @return 유니티 프로젝트면 true
     */
    bool IsUnityProject(const std::string &projectPath);

public:
    ProcessMonitor();
    ~ProcessMonitor();

    /**
     * 현재 실행 중인 모든 Unity 인스턴스를 스캔
     * @return 발견된 Unity 인스턴스들
     */
    std::vector<UnityInstance> ScanUnityProcesses();

    /**
     * 새로 시작된 Unity 프로세스가 있는지 확인
     * @return 새로운 인스턴스들
     */
    std::vector<UnityInstance> GetNewInstances();

    /**
     * 종료된 Unity 프로세스가 있는지 확인
     * @return 종료된 인스턴스들
     */
    std::vector<UnityInstance> GetClosedInstances();

    /**
     * 특정 프로세스 ID가 실행 중인지 확인
     * @param processId 프로세스 ID
     * @return 실행중이면 true
     */
    bool IsProcessRunning(DWORD processId);

    /**
     * 현재 활성화된 모든 Unity 인스턴스 반환
     * @return 현재 실행중인 인스턴스들
     */
    const std::map<DWORD, UnityInstance>& GetActiveInstances() const;
};