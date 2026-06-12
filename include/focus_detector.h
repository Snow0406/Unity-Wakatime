#pragma once
#include "globals.h"

/**
 * 포그라운드 창 전이/제목 변경 이벤트를 받아, 포커스된 추적 대상 앱의
 * focus heartbeat를 만든다.
 * - DirectoryWatch(Unity): 해당 PID의 실제 프로젝트로 heartbeat (다중 Unity 정확 매핑).
 * - WindowTitle(Aseprite/Blender): 창 제목을 파싱해 활성 파일을 추정.
 */
class FocusDetector {
public:
    // (appId, entity, project, editorVersion) — focus heartbeat는 항상 is_write=false.
    using HeartbeatCallback = std::function<void(const std::string& appId,
                                                 const std::string& entity,
                                                 const std::string& project,
                                                 const std::string& editorVersion)>;

private:
    HWND titleTrackedHwnd = nullptr;   // WindowTitle 앱 포커스 시에만 비-null (NAMECHANGE 수신 대상)
    DWORD focusedProcessId = 0;
    std::string focusedAppId;
    std::chrono::steady_clock::time_point lastHeartbeat;
    std::chrono::seconds heartbeatInterval{120}; // 2분

    // 주기 keep-alive 재전송용 마지막 focus heartbeat 파라미터
    bool hasFocusTarget = false;
    DWORD lastProcessId = 0;
    std::string lastAppId;
    std::string lastEntity;
    std::string lastProject;
    std::string lastEditorVersion;

    HeartbeatCallback heartbeatCallback;

    void EmitHeartbeat(const std::string& appId, const std::string& entity,
                       const std::string& project, const std::string& editorVersion);
    void EmitForWindow(HWND hwnd);
    void ClearFocus();

public:
    /**
     * 포그라운드 창 전이 시 호출 (EVENT_SYSTEM_FOREGROUND).
     */
    void OnForegroundChanged(HWND hwnd);

    /**
     * 창 제목 변경 시 호출 (EVENT_OBJECT_NAMECHANGE).
     * 현재 추적 중인 WindowTitle 앱의 포그라운드 창일 때만 처리한다.
     */
    void OnTitleChanged(HWND hwnd);

    /**
     * 포커스 유지 중 주기 heartbeat (2분 간격) keep-alive.
     */
    void SendPeriodicHeartbeat();

    /**
     * 앱 비활성화 시 해당 앱의 stale focus/keep-alive 상태를 제거한다.
     */
    void ClearFocusForApp(const std::string& appId);

    /**
     * 프로세스 종료 시 해당 PID의 stale focus/keep-alive 상태를 제거한다.
     */
    void ClearFocusForProcess(DWORD pid);

    /**
     * focus heartbeat 전송 콜백 설정.
     */
    void SetHeartbeatCallback(HeartbeatCallback callback);
};
