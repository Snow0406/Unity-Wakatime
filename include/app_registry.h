#pragma once

#include <string>
#include <vector>

/**
 * 앱별 추적 전략.
 * - DirectoryWatch: 프로젝트 폴더를 ReadDirectoryChangesW로 재귀 감시 (Unity).
 * - WindowTitle: 프로세스 존재 = 실행, 포그라운드 창 제목 파싱으로 활성 파일 추정.
 */
enum class TrackStrategy { DirectoryWatch, WindowTitle };

/**
 * 커맨드라인에서 초기 entity/project를 뽑아내는 방식.
 * - UnityProjectPath: `-projectPath <dir>` 추출.
 * - PositionalFile: 첫 positional 파일 인자 추출 (full path).
 * - None: 추출하지 않음.
 */
enum class CmdLineEntity { UnityProjectPath, PositionalFile, None };

/**
 * 추적 대상 앱의 정적 정의를 상수로 보유한다.
 */
struct AppDefinition {
    std::string id;                          // AppRegistry 정의 id
    std::string displayName;                 // "Unity"
    std::vector<std::wstring> processNames;   // L"Unity.exe" 등 (소문자 비교)
    std::vector<std::string> fileExtensions;  // 감시/커맨드라인 후보 확장자 필터
    std::string language;                    // WakaTime language
    std::string editor;                      // WakaTime editor 베이스 이름
    TrackStrategy strategy;
    CmdLineEntity cmdLineEntity;
};

namespace AppRegistry
{
    /**
     * 빌트인 앱 정의 전체를 반환 (정적, 변경 없음).
     */
    const std::vector<AppDefinition>& All();

    /**
     * 프로세스 exe 이름으로 정의 조회 (대소문자 무시).
     * @return 매칭되는 정의, 없으면 nullptr
     */
    const AppDefinition* FindByProcessName(const std::wstring& exe);

    /**
     * 앱 id로 정의 조회.
     * @return 매칭되는 정의, 없으면 nullptr
     */
    const AppDefinition* FindById(const std::string& id);

    /**
     * 해당 앱이 추적 활성 상태인지 확인.
     */
    bool IsEnabled(const std::string& id);

    /**
     * 앱 추적 활성 상태 변경 후 즉시 영속화(Save).
     */
    void SetEnabled(const std::string& id, bool enabled);

    /**
     * 활성 상태를 apps.txt에서 로드. 파일이 없으면 기본값(Unity만 활성).
     */
    void Load();

    /**
     * 활성 상태를 apps.txt에 저장 (활성 id 한 줄에 하나).
     */
    void Save();

    /**
     * 현재 활성 앱 개수.
     */
    int EnabledCount();
}
