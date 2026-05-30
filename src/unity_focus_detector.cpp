#include "unity_focus_detector.h"

#include <utility>

void UnityFocusDetector::OnForegroundChanged(const HWND hwnd)
{
    bool isCurrentUnityFocused = false;

    if (hwnd != nullptr)
    {
        WCHAR className[256];
        if (GetClassNameW(hwnd, className, 256) > 0)
        {
            isCurrentUnityFocused = (std::wstring(className).find(L"Unity") != std::wstring::npos);
        }
    }

    if (isCurrentUnityFocused && !isUnityFocused)
    {
        isUnityFocused = true;
        lastHeartbeat = std::chrono::steady_clock::now();
        if (focusCallback) focusCallback();
    }
    else if (!isCurrentUnityFocused && isUnityFocused)
    {
        isUnityFocused = false;
        if (unfocusCallback) unfocusCallback();
    }
}

void UnityFocusDetector::SendPeriodicHeartbeat()
{
    if (!isUnityFocused) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - lastHeartbeat >= heartbeatInterval)
    {
        if (periodicHeartbeatCallback) periodicHeartbeatCallback();
        lastHeartbeat = now;
    }
}

void UnityFocusDetector::SetFocusCallback(std::function<void()> callback)
{
    focusCallback = std::move(callback);
}

void UnityFocusDetector::SetUnfocusCallback(std::function<void()> callback)
{
    unfocusCallback = std::move(callback);
}

void UnityFocusDetector::SetPeriodicHeartbeatCallback(std::function<void()> callback)
{
    periodicHeartbeatCallback = std::move(callback);
}


