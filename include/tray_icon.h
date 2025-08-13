#pragma once

#include "globals.h"
#include <shellapi.h>   // 시스템 트레이 API

// Shell32 라이브러리 링크
#pragma comment(lib, "shell32.lib")

// 트레이 아이콘 관련 상수들
#define WM_TRAYICON (WM_USER + 1)    // 트레이 아이콘 메시지
#define IDM_EXIT 100                 // 종료 메뉴 ID
#define IDM_SHOW_STATUS 101          // 상태 보기 메뉴 ID
#define IDM_TOGGLE_MONITORING 102    // 모니터링 토글 메뉴 ID
#define IDM_OPEN_DASHBOARD 103       // WakaTime 대시보드 열기
#define IDM_SETTINGS 104             // 설정 메뉴 ID
#define IDM_GITHUB 105               // Github 링크

/**
 * Windows 시스템 트레이에 아이콘을 표시하고 사용자 인터랙션 처리
 */
class TrayIcon {
private:
    // Windows 창 관련
    HWND hwnd;                       // 숨겨진 창 핸들
    WNDCLASSW wc;                     // 창 클래스
    NOTIFYICONDATAW nid;              // 트레이 아이콘 데이터
    HMENU hMenu;                     // 컨텍스트 메뉴
    
    // 상태 정보
    bool isMonitoring;               // 모니터링 활성 상태
    std::string currentProject;      // 현재 활성 프로젝트
    int totalHeartbeats;             // 총 heartbeat 수
    bool initialized;                // 초기화 상태
    
    // 콜백 함수들
    std::function<void()> onExit;                               // 종료 콜백
    std::function<void()> onShowStatus;                         // 상태 보기 콜백
    std::function<void(bool)> onToggleMonitoring;               // 모니터링 토글 콜백
    std::function<void()> onOpenDashboard;                      // 대시보드 열기 콜백
    std::function<void()> onShowSettings;                       // 설정 보기 콜백
    std::function<void(const std::string&)> onApiKeyChange;     // API 키 변경 콜백
    
    /**
     * 숨겨진 창 생성 (트레이 아이콘 메시지 수신용)
     * @return 성공하면 true
     */
    bool CreateHiddenWindow();
    
    /**
     * 트레이 아이콘 생성 및 등록
     * @return 성공하면 true
     */
    bool CreateTrayIcon();
    
    /**
     * 컨텍스트 메뉴 생성
     * @return 메뉴 핸들
     */
    HMENU CreateContextMenu();
    
    /**
     * 트레이 아이콘 툴팁 업데이트
     * @param tooltip 새로운 툴팁 텍스트
     */
    void UpdateTooltip(const std::string& tooltip);
    
    /**
     * 컨텍스트 메뉴 표시
     * @param x 마우스 X 좌표
     * @param y 마우스 Y 좌표
     */
    void ShowContextMenu(int x, int y);
    
    /**
     * 메뉴 항목 처리
     * @param menuId 선택된 메뉴 ID
     */
    void HandleMenuCommand(int menuId);
    
    /**
     * PNG 파일에서 아이콘 로드
     * @param filePath PNG 파일 경로
     * @return 아이콘 핸들, 실패 시 기본 아이콘
     */
    HICON LoadPngIcon(const std::string& filePath);

    /**
     * 상태 서브메뉴 생성/업데이트
     * @return 서브메뉴 핸들
     */
    HMENU CreateStatusSubMenu();

    /**
     * 컨텍스트 메뉴 동적 업데이트
     */
    void UpdateContextMenu();

    /**
     * GitHub 저장소 열기
     */
    void OpenGitHubRepository();
    
    /**
     * 풍선 알림 표시
     * @param title 알림 제목
     * @param message 알림 메시지
     * @param timeout 표시 시간 (ms)
     * @param iconType 아이콘 타입
     */
    void ShowBalloonNotification(const std::string& title, 
                                const std::string& message,
                                DWORD timeout = 3000,
                                DWORD iconType = NIIF_INFO);

    /**
     * API Key 입력 대화상자 표시
     * @return 입력된 API Key, 취소 시 빈 문자열
     */
    std::string ShowApiKeyInputDialog();

    /**
     * 클립보드에서 텍스트 가져오기
     * @return 클립보드의 텍스트 내용
     */
    std::string GetClipboardText();

public:
    TrayIcon();
    ~TrayIcon();
    
    /**
     * 트레이 아이콘 초기화
     * @param appName 애플리케이션 이름
     * @return 성공하면 true
     */
    bool Initialize(const std::string& appName = "Unity WakaTime");
    
    /**
     * 현재 프로젝트 설정 (툴팁 업데이트)
     * @param projectName 프로젝트 이름
     */
    void SetCurrentProject(const std::string& projectName);
    
    /**
     * heartbeat 카운터 증가
     */
    void IncrementHeartbeats();
    
    /**
     * 모니터링 상태 설정
     * @param monitoring 모니터링 활성화 여부
     */
    void SetMonitoringState(bool monitoring);
    
    /**
     * 에러 알림 표시
     * @param message 에러 메시지
     */
    void ShowErrorNotification(const std::string& message);
    
    /**
     * 정보 알림 표시
     * @param message 정보 메시지
     */
    void ShowInfoNotification(const std::string& message);
    
    /**
     * 트레이 아이콘 제거 및 정리
     */
    void Shutdown();

    /**
     * 메시지 처리 (메인 스레드에서 호출)
     * @return 처리된 메시지 수
     */
    int ProcessMessages();

    /**
     * 상태 정보 업데이트 (서브메뉴에 반영)
     */
    void RefreshStatusMenu();

    /**
     * 모니터링 상태 반환
     * @return 모니터링 중이면 true
     */
    bool IsMonitoring() const { return isMonitoring; }

public:
    /**
     * 종료 콜백 설정
     * @param callback 종료 시 호출될 함수
     */
    void SetExitCallback(const std::function<void()> &callback);

    /**
     * 상태 보기 콜백 설정
     * @param callback 상태 보기 시 호출될 함수
     */
    void SetShowStatusCallback(const std::function<void()> &callback);

    /**
     * 모니터링 토글 콜백 설정
     * @param callback 모니터링 토글 시 호출될 함수
     */
    void SetToggleMonitoringCallback(const std::function<void(bool)> &callback);

    /**
     * 대시보드 열기 콜백 설정
     * @param callback 대시보드 열기 시 호출될 함수
     */
    void SetOpenDashboardCallback(const std::function<void()> &callback);

    /**
     * 설정 보기 콜백 설정
     * @param callback 설정 보기 시 호출될 함수
     */
    void SetShowSettingsCallback(const std::function<void()> &callback);

    /**
     * API Key 설정 콜백 함수 설정
     * @param callback API Key 변경 시 호출될 함수
     */
    void SetApiKeyChangeCallback(const std::function<void(const std::string&)> &callback);

private:
    /**
     * 정적 윈도우 프로시저 - Windows API 콜백용
     */
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    /**
     * 인스턴스별 윈도우 메시지 처리 (실제 구현)
     */
    LRESULT HandleWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
};
