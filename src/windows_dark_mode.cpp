#include "windows_dark_mode.h"

#include <dwmapi.h>

namespace
{
    enum class PreferredAppMode
    {
        Default,
        AllowDark,
        ForceDark,
        ForceLight,
        Max
    };

    using AllowDarkModeForWindowFn = BOOL(WINAPI *)(HWND, BOOL);
    using SetPreferredAppModeFn = PreferredAppMode(WINAPI *)(PreferredAppMode);
    using FlushMenuThemesFn = void(WINAPI *)();

    struct DarkModeApi
    {
        HMODULE uxTheme = nullptr;
        AllowDarkModeForWindowFn allowDarkModeForWindow = nullptr;
        SetPreferredAppModeFn setPreferredAppMode = nullptr;
        FlushMenuThemesFn flushMenuThemes = nullptr;
        bool loaded = false;
    };

    DarkModeApi &GetDarkModeApi()
    {
        static DarkModeApi api;
        if (api.loaded)
        {
            return api;
        }

        api.loaded = true;
        api.uxTheme = LoadLibraryW(L"uxtheme.dll");
        if (!api.uxTheme)
        {
            return api;
        }

        api.allowDarkModeForWindow = reinterpret_cast<AllowDarkModeForWindowFn>(
            GetProcAddress(api.uxTheme, MAKEINTRESOURCEA(133)));
        api.setPreferredAppMode = reinterpret_cast<SetPreferredAppModeFn>(
            GetProcAddress(api.uxTheme, MAKEINTRESOURCEA(135)));
        api.flushMenuThemes = reinterpret_cast<FlushMenuThemesFn>(
            GetProcAddress(api.uxTheme, MAKEINTRESOURCEA(136)));

        return api;
    }
}

bool WindowsDarkMode::EnableForApp()
{
    auto &api = GetDarkModeApi();
    if (!api.setPreferredAppMode)
    {
        return false;
    }

    api.setPreferredAppMode(PreferredAppMode::AllowDark);
    if (api.flushMenuThemes)
    {
        api.flushMenuThemes();
    }

    return true;
}

void WindowsDarkMode::ApplyToWindow(const HWND hwnd)
{
    if (!hwnd)
    {
        return;
    }

    auto &api = GetDarkModeApi();
    if (api.allowDarkModeForWindow)
    {
        api.allowDarkModeForWindow(hwnd, TRUE);
    }

    BOOL useDark = TRUE;

    constexpr DWORD kDwmwaUseImmersiveDarkMode = 20;
    HRESULT hr = DwmSetWindowAttribute(hwnd,
                                       kDwmwaUseImmersiveDarkMode,
                                       &useDark,
                                       sizeof(useDark));
    if (FAILED(hr))
    {
        // Older Windows 10 builds used attribute id 19 for immersive dark mode.
        constexpr DWORD kDwmwaUseImmersiveDarkModeLegacy = 19;
        DwmSetWindowAttribute(hwnd,
                              kDwmwaUseImmersiveDarkModeLegacy,
                              &useDark,
                              sizeof(useDark));
    }
}
