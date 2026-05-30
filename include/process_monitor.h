#pragma once

#include "globals.h"
#include <unordered_map>

/**
 * Unity 프로세스들 감지
 */
class ProcessMonitor {
private:
    std::unordered_map<DWORD, UnityInstance> activeInstances;

    /**
     * NtQueryInformationProcess + PEB 읽기로 프로세스 커맨드 라인을 가져온다.
     * WMI/RPC를 거치지 않아 Wmiprvse를 깨우지 않고 비용이 낮다.
     * (PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ 권한 필요, 64-bit 전제)
     * @param pid 대상 프로세스 ID
     * @return 커맨드 라인, 실패시 빈 문자열
     */
    std::string GetCommandLineViaPeb(DWORD pid);

    /**
     * 특정 프로세스의 커맨드 라인을 가져와 Unity 프로젝트 경로로 해석
     * @param pid 대상 프로세스 ID
     * @return 프로젝트 경로, 실패시 빈문자열
     */
    std::string GetProcessCommandLine(DWORD pid);

    /**
     * 스냅샷 엔트리가 Unity 프로세스이면 인스턴스 정보로 해석
     * @param pid 대상 프로세스 ID
     * @param instance 해석된 인스턴스 (출력)
     * @return Unity 프로젝트로 해석되면 true
     */
    bool ResolveUnityInstance(DWORD pid, UnityInstance& instance);

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
     * 현재 실행 중인 모든 Unity 인스턴스를 스캔 (초기 스캔용).
     * 결과를 activeInstances에도 등록하여 이후 PollChanges가 중복 보고하지 않도록 한다.
     * @return 발견된 Unity 인스턴스들
     */
    std::vector<UnityInstance> ScanUnityProcesses();

    /**
     * 단일 스냅샷으로 새로 시작/종료된 Unity 프로세스를 한 번에 diff한다.
     * 이미 알려진 PID는 재해석(PEB/디스크 조회)하지 않는다.
     * @param started 새로 감지된 인스턴스 (출력)
     * @param closed 종료된 인스턴스 (출력)
     */
    void PollChanges(std::vector<UnityInstance>& started, std::vector<UnityInstance>& closed);

    /**
     * 특정 프로세스 ID가 실행 중인지 확인
     * @param processId 프로세스 ID
     * @return 실행중이면 true
     */
    bool IsProcessRunning(DWORD processId);
};