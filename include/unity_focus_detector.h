#pragma once
#include "globals.h"

class UnityFocusDetector {
private:
   bool isUnityFocused = false;
    std::chrono::steady_clock::time_point lastHeartbeat;
    std::chrono::seconds heartbeatInterval{120}; // 2분

    std::function<void()> focusCallback;
    std::function<void()> unfocusCallback;
    std::function<void()> periodicHeartbeatCallback;

public:
   /**
    * 포그라운드 창 변경 시 호출 (SetWinEventHook 콜백에서 구동).
    * 클래스명에 "Unity"가 포함되면 포커스 전이로 판정한다.
    * @param hwnd 새 포그라운드 창 핸들 (nullptr 가능)
    */
   void OnForegroundChanged(HWND hwnd);

   /**
    * 2분마다 호출
    */
   void SendPeriodicHeartbeat();

    bool IsUnityFocused() const { return isUnityFocused; }

   /**
    * 포커스 콜백 설정
    * @param callback 포커스 될때 호출될 함수
    */
   void SetFocusCallback(std::function<void()> callback);

   /**
    * 포커스 해제 콜백 설정
    * @param callback 포커스 해제될때 호출될 함수
    */
   void SetUnfocusCallback(std::function<void()> callback);

   /**
    * 이전 하트비트 콜백 설정
    * @param callback 이전 하트비트 보낼때 호출될 함수
    */
   void SetPeriodicHeartbeatCallback(std::function<void()> callback);
};
