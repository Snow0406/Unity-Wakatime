#pragma once
#include "globals.h"

class UnityFocusDetector {
private:
   bool isUnityFocused = false;
    std::chrono::steady_clock::time_point lastHeartbeat;
    std::chrono::seconds heartbeatInterval{120}; // 5분

    std::function<void()> focusCallback;
    std::function<void()> unfocusCallback;
    std::function<void()> periodicHeartbeatCallback;

public:
   /**
    * 포커스 상태 확인
    */
   void CheckFocused();

   /**
    * 5분마다 호출
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