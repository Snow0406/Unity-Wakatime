#include "focus_detector.h"
#include "process_monitor.h"
#include "app_registry.h"

#include <utility>

namespace
{
    std::string WideToUtf8(const std::wstring& w)
    {
        if (w.empty()) return "";
        const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                            nullptr, 0, nullptr, nullptr);
        if (len <= 0) return "";
        std::string out(len, 0);
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                            out.data(), len, nullptr, nullptr);
        return out;
    }

    std::string GetWindowTitleUtf8(const HWND hwnd)
    {
        const int len = GetWindowTextLengthW(hwnd);
        if (len <= 0) return "";
        std::wstring w(static_cast<size_t>(len) + 1, L'\0');
        const int copied = GetWindowTextW(hwnd, w.data(), len + 1);
        if (copied <= 0) return "";
        w.resize(static_cast<size_t>(copied));
        return WideToUtf8(w);
    }

    std::string ToLower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](const unsigned char c) { return static_cast<char>(::tolower(c)); });
        return s;
    }

    bool IEquals(const std::string& a, const std::string& b)
    {
        return ToLower(a) == ToLower(b);
    }

    std::string Trim(std::string s)
    {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
            s.pop_back();
        return s;
    }

    // dirty marker(`*`, `●`, `•`)와 주변 공백을 앞뒤에서 반복 제거.
    std::string StripMarkers(std::string s)
    {
        static const std::vector<std::string> markers = {
            "\xE2\x97\x8F", // ● U+25CF
            "\xE2\x80\xA2", // • U+2022
            "*",
        };
        bool changed = true;
        while (changed)
        {
            changed = false;
            s = Trim(s);
            for (const auto& m : markers)
            {
                if (s.size() >= m.size() && s.compare(0, m.size(), m) == 0)
                {
                    s.erase(0, m.size());
                    changed = true;
                }
                if (s.size() >= m.size() && s.compare(s.size() - m.size(), m.size(), m) == 0)
                {
                    s.erase(s.size() - m.size());
                    changed = true;
                }
            }
        }
        return s;
    }

    std::vector<std::string> SplitByDash(const std::string& s)
    {
        std::vector<std::string> out;
        const std::string sep = " - ";
        size_t start = 0;
        size_t pos;
        while ((pos = s.find(sep, start)) != std::string::npos)
        {
            out.push_back(s.substr(start, pos - start));
            start = pos + sep.size();
        }
        out.push_back(s.substr(start));
        return out;
    }

    bool IsFullPath(const std::string& s)
    {
        if (s.find('/') != std::string::npos || s.find('\\') != std::string::npos) return true;
        return s.size() > 1 && s[1] == ':'; // 드라이브 문자
    }

    std::string NormalizeSlashes(std::string s)
    {
        std::replace(s.begin(), s.end(), '\\', '/');
        return s;
    }

    std::string LeafName(const std::string& path)
    {
        const std::string p = NormalizeSlashes(path);
        const size_t pos = p.find_last_of('/');
        return pos == std::string::npos ? p : p.substr(pos + 1);
    }

    std::string ParentLeafName(const std::string& path)
    {
        const std::string p = NormalizeSlashes(path);
        const size_t pos = p.find_last_of('/');
        if (pos == std::string::npos) return "";
        const std::string parent = p.substr(0, pos);
        const size_t pos2 = parent.find_last_of('/');
        return pos2 == std::string::npos ? parent : parent.substr(pos2 + 1);
    }

    // 창 제목에서 활성 파일 토큰을 추출. full path / 파일명 / 빈 문자열(=Untitled 또는 없음) 반환.
    std::string ParseTitleFile(const std::string& displayName, const std::string& titleIn)
    {
        const std::string title = Trim(titleIn);
        if (title.empty()) return "";

        // Blender 류: "... [C:\path\file.blend]" 또는 "*Blender* [path]"
        const size_t lb = title.find_last_of('[');
        const size_t rb = title.find_last_of(']');
        if (lb != std::string::npos && rb != std::string::npos && rb > lb)
        {
            std::string inner = StripMarkers(title.substr(lb + 1, rb - lb - 1));
            if (!inner.empty() && !IEquals(inner, "Untitled")) return inner;
        }

        // 일반: " - " 분할 후 앱 이름/버전이 아닌 첫 세그먼트가 파일.
        const std::string appLower = ToLower(displayName);
        for (const auto& seg : SplitByDash(title))
        {
            std::string s = StripMarkers(seg);
            if (s.empty()) continue;

            const std::string lower = ToLower(s);
            if (lower == appLower) continue;                          // "Aseprite"
            if (lower.rfind(appLower + " ", 0) == 0) continue;        // "Aseprite v1.3"
            if (!s.empty() && (s[0] == 'v' || s[0] == 'V') && s.size() > 1 && isdigit(static_cast<unsigned char>(s[1])))
                continue;                                             // "v1.3.2"
            if (IEquals(s, "Untitled")) return "";                    // 저장 안 됨 → entity 없음
            return s;
        }
        return "";
    }
}

void FocusDetector::SetHeartbeatCallback(HeartbeatCallback callback)
{
    heartbeatCallback = std::move(callback);
}

void FocusDetector::ClearFocus()
{
    titleTrackedHwnd = nullptr;
    focusedProcessId = 0;
    focusedAppId.clear();
    hasFocusTarget = false;
    lastProcessId = 0;
    lastAppId.clear();
    lastEntity.clear();
    lastProject.clear();
    lastEditorVersion.clear();
}

void FocusDetector::EmitHeartbeat(const std::string& appId, const std::string& entity,
                                  const std::string& project, const std::string& editorVersion)
{
    lastProcessId = focusedProcessId;
    lastAppId = appId;
    lastEntity = entity;
    lastProject = project;
    lastEditorVersion = editorVersion;
    hasFocusTarget = true;
    lastHeartbeat = std::chrono::steady_clock::now();

    if (heartbeatCallback) heartbeatCallback(appId, entity, project, editorVersion);
}

void FocusDetector::EmitForWindow(const HWND hwnd)
{
    if (focusedAppId.empty() || g_processMonitor == nullptr) return;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return;

    const AppInstance* inst = g_processMonitor->ResolveByPid(pid);
    if (inst == nullptr) return;

    const AppDefinition* def = AppRegistry::FindById(inst->appId);
    if (def == nullptr || !AppRegistry::IsEnabled(def->id)) return;

    focusedProcessId = pid;
    focusedAppId = def->id;

    if (def->strategy == TrackStrategy::DirectoryWatch)
    {
        if (inst->projectPath.empty()) return;
        EmitHeartbeat(def->id, inst->projectPath, inst->projectName, inst->editorVersion);
        return;
    }

    // WindowTitle: 제목 파싱 + 커맨드라인 entity로 fallback (plan 4단계 우선순위)
    const std::string titleFile = ParseTitleFile(def->displayName, GetWindowTitleUtf8(hwnd));
    const std::string& cmdEntity = inst->entity;

    std::string entity;
    std::string project;

    if (IsFullPath(titleFile))
    {
        entity = NormalizeSlashes(titleFile);
        project = ParentLeafName(entity);
    }
    else if (!titleFile.empty())
    {
        if (IsFullPath(cmdEntity) && LeafName(cmdEntity) == titleFile)
        {
            entity = NormalizeSlashes(cmdEntity);
            project = ParentLeafName(entity);
        }
        else
        {
            entity = titleFile;
            project = IsFullPath(cmdEntity) ? ParentLeafName(cmdEntity) : def->displayName;
        }
    }
    else if (IsFullPath(cmdEntity))
    {
        entity = NormalizeSlashes(cmdEntity);
        project = ParentLeafName(entity);
    }
    else
    {
        return; // entity 없음(Untitled/미저장) → 전송 스킵
    }

    if (entity.empty()) return;
    EmitHeartbeat(def->id, entity, project, inst->editorVersion);
}

void FocusDetector::OnForegroundChanged(const HWND hwnd)
{
    // 포커스 전이 시 추적 상태 초기화.
    ClearFocus();

    if (hwnd == nullptr || g_processMonitor == nullptr) return;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return;

    const AppInstance* inst = g_processMonitor->ResolveByPid(pid);
    if (inst == nullptr) return;

    const AppDefinition* def = AppRegistry::FindById(inst->appId);
    if (def == nullptr || !AppRegistry::IsEnabled(def->id)) return;

    focusedAppId = def->id;
    if (def->strategy == TrackStrategy::WindowTitle)
    {
        titleTrackedHwnd = hwnd; // 이 창의 NAMECHANGE만 수용
    }

    EmitForWindow(hwnd);
}

void FocusDetector::OnTitleChanged(const HWND hwnd)
{
    // 현재 추적 중인 WindowTitle 창이 아니면 즉시 무시 (저비용 가드).
    if (hwnd == nullptr || hwnd != titleTrackedHwnd || focusedAppId.empty()) return;

    const AppDefinition* def = AppRegistry::FindById(focusedAppId);
    if (def == nullptr || def->strategy != TrackStrategy::WindowTitle || !AppRegistry::IsEnabled(def->id))
        return;

    EmitForWindow(hwnd);
}

void FocusDetector::SendPeriodicHeartbeat()
{
    if (!hasFocusTarget) return;

    if (lastAppId.empty() || !AppRegistry::IsEnabled(lastAppId))
    {
        ClearFocus();
        return;
    }

    if (lastProcessId != 0 && g_processMonitor != nullptr &&
        !g_processMonitor->IsProcessRunning(lastProcessId))
    {
        ClearFocus();
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - lastHeartbeat >= heartbeatInterval)
    {
        focusedProcessId = lastProcessId;
        focusedAppId = lastAppId;
        EmitHeartbeat(lastAppId, lastEntity, lastProject, lastEditorVersion);
    }
}

void FocusDetector::ClearFocusForApp(const std::string& appId)
{
    if (appId.empty()) return;
    if (focusedAppId == appId || lastAppId == appId)
    {
        ClearFocus();
    }
}

void FocusDetector::ClearFocusForProcess(const DWORD pid)
{
    if (pid == 0) return;
    if (focusedProcessId == pid || lastProcessId == pid)
    {
        ClearFocus();
    }
}
