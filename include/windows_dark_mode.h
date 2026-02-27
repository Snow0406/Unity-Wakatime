#pragma once

#include <windows.h>

namespace WindowsDarkMode
{
    /**
     * Enable dark-mode preference for Win32 menus on supported Windows versions.
     * Returns true when the API path is available and invoked.
     */
    bool EnableForApp();

    /**
     * Apply dark-mode attributes to a specific window when supported.
     * Safe to call on unsupported versions (no-op fallback).
     */
    void ApplyToWindow(HWND hwnd);
}
