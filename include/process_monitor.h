#pragma once

#include "globals.h"
#include "app_registry.h"
#include <unordered_map>
#include <unordered_set>

/**
 * 활성화된 앱들의 프로세스를 스냅샷으로 감지하고 AppInstance로 해석한다.
 * Unity는 커맨드라인 -projectPath, Aseprite/Blender는 positional 파일 인자(hybrid)로
 * 초기 entity를 확보한다.
 */
class ProcessMonitor {
private:
    std::unordered_map<DWORD, AppInstance> activeInstances;

    /**
     * NtQueryInformationProcess + PEB 읽기로 프로세스 커맨드 라인을 가져온다.
     * (PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ 권한 필요, 64-bit 전제)
     * @param pid 대상 프로세스 ID
     * @return 커맨드 라인, 실패시 빈 문자열
     */
    std::string GetCommandLineViaPeb(DWORD pid);

    /**
     * 프로세스의 실행 파일 이름(예: "blender.exe")을 가져온다.
     * @param pid 대상 프로세스 ID
     * @return exe 파일명(소문자 아님), 실패 시 빈 문자열
     */
    std::wstring GetProcessExeName(DWORD pid);

    /**
     * 주어진 정의/PID로 AppInstance를 해석한다.
     * DirectoryWatch(Unity): 프로젝트 경로 검증 실패 시 false.
     * WindowTitle(Aseprite/Blender): 프로세스 존재만으로 true(entity는 선택적).
     * @param pid 대상 프로세스 ID
     * @param def 앱 정의
     * @param instance 해석된 인스턴스 (출력)
     * @return 추적 대상으로 해석되면 true
     */
    bool ResolveInstance(DWORD pid, const AppDefinition& def, AppInstance& instance);

    /**
     * 커맨드 라인에서 Unity `-projectPath` 경로 추출
     */
    std::string ExtractProjectPath(const std::string& commandLine);

    /**
     * 커맨드 라인에서 첫 positional 파일 인자 추출 (Aseprite/Blender).
     * @return 파일 경로, 없으면 빈 문자열
     */
    std::string ExtractPositionalFile(const std::string& commandLine, const AppDefinition& def);

    /**
     * 경로에서 마지막 구성요소(파일/폴더 이름) 추출
     */
    std::string GetLeafName(const std::string& path);

    /**
     * 경로의 상위 폴더 경로 추출
     */
    std::string GetParentPath(const std::string& path);

    /**
     * Unity 프로젝트의 에디터 버전 추출 (ProjectVersion.txt)
     */
    std::string GetUnityEditorVersion(const std::string& projectPath);

    /**
     * ProjectVersion.txt 파일에서 버전 정보 파싱
     */
    std::string ParseProjectVersionFile(const fs::path& versionFilePath);

    /**
     * 유니티 프로젝트인지 확인 (Assets + ProjectSettings 존재)
     */
    bool IsUnityProject(const std::string &projectPath);

public:
    ProcessMonitor();
    ~ProcessMonitor();

    /**
     * 현재 실행 중인 활성 앱 인스턴스를 전부 스캔 (초기 스캔용).
     * 결과를 activeInstances에 등록하여 이후 PollChanges가 중복 보고하지 않도록 한다.
     * @return 발견된 인스턴스들
     */
    std::vector<AppInstance> ScanProcesses();

    /**
     * 단일 스냅샷으로 새로 시작/종료된 인스턴스를 diff한다.
     * 활성 앱의 프로세스 이름만 매칭하고, 알려진 PID는 재해석하지 않는다.
     * @param started 새로 감지된 인스턴스 (출력)
     * @param closed 종료된 인스턴스 (출력)
     */
    void PollChanges(std::vector<AppInstance>& started, std::vector<AppInstance>& closed);

    /**
     * PID로 활성 인스턴스 조회. 맵에 없으면 즉석에서 해석을 시도해 등록한다
     * (포커스 직후 스캔 전이라도 즉시 매핑되도록).
     * @return 인스턴스 포인터(맵 소유), 추적 대상이 아니면 nullptr
     */
    const AppInstance* ResolveByPid(DWORD pid);

    /**
     * 해당 앱의 모든 활성 인스턴스를 제거 (앱 토글 off 시).
     */
    void PurgeApp(const std::string& appId);

    /**
     * 현재 활성 인스턴스를 가진 앱 id 집합 (트레이 "실행중" 표시용).
     */
    std::unordered_set<std::string> GetActiveAppIds() const;

    /**
     * 특정 프로세스 ID가 실행 중인지 확인
     */
    bool IsProcessRunning(DWORD processId);
};
