#include "app_registry.h"
#include "globals.h"

#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <fstream>

namespace
{
    std::mutex g_enabledMutex;
    std::unordered_set<std::string> g_enabledIds;
    bool g_loaded = false;

    std::wstring ToLowerW(std::wstring s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](const wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
        return s;
    }
}

const std::vector<AppDefinition>& AppRegistry::All()
{
    static const std::vector<AppDefinition> definitions = []
    {
        std::vector<AppDefinition> defs;

        AppDefinition unity;
        unity.id = "unity";
        unity.displayName = "Unity";
        unity.processNames = {L"Unity.exe", L"Unity"};
        unity.fileExtensions = {
            ".unity", ".prefab", ".asset", ".mat", ".shader",
            ".hlsl", ".anim", ".controller", ".json",
        };
        unity.language = "Unity";
        unity.editor = "Unity";
        unity.strategy = TrackStrategy::DirectoryWatch;
        unity.cmdLineEntity = CmdLineEntity::UnityProjectPath;
        defs.push_back(std::move(unity));

        AppDefinition aseprite;
        aseprite.id = "aseprite";
        aseprite.displayName = "Aseprite";
        aseprite.processNames = {L"aseprite.exe", L"Aseprite.exe"};
        aseprite.fileExtensions = {
            ".ase", ".aseprite",
            ".png", ".gif", ".jpg", ".jpeg", ".bmp", ".tga", ".webp",
        };
        aseprite.language = "Aseprite";
        aseprite.editor = "Aseprite";
        aseprite.strategy = TrackStrategy::WindowTitle;
        aseprite.cmdLineEntity = CmdLineEntity::PositionalFile;
        defs.push_back(std::move(aseprite));

        AppDefinition blender;
        blender.id = "blender";
        blender.displayName = "Blender";
        blender.processNames = {L"blender.exe"};
        blender.fileExtensions = {".blend"};
        blender.language = "Blender";
        blender.editor = "Blender";
        blender.strategy = TrackStrategy::WindowTitle;
        blender.cmdLineEntity = CmdLineEntity::PositionalFile;
        defs.push_back(std::move(blender));

        AppDefinition clipStudio;
        clipStudio.id = "clipstudio";
        clipStudio.displayName = "Clip Studio Paint";
        clipStudio.processNames = {L"CLIPStudioPaint.exe"};
        clipStudio.fileExtensions = {
            ".clip", ".cmc", ".lip", ".csnf",
            ".psd", ".psb",
            ".png", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff", ".tga", ".ipv",
        };
        clipStudio.language = "Clip Studio Paint";
        clipStudio.editor = "Clip Studio Paint";
        clipStudio.strategy = TrackStrategy::WindowTitle;
        clipStudio.cmdLineEntity = CmdLineEntity::PositionalFile;
        defs.push_back(std::move(clipStudio));

        return defs;
    }();

    return definitions;
}

const AppDefinition* AppRegistry::FindByProcessName(const std::wstring& exe)
{
    // lowercase exe → AppDefinition* 맵을 1회 빌드해 재사용.
    static const std::unordered_map<std::wstring, const AppDefinition*> byProcess = []
    {
        std::unordered_map<std::wstring, const AppDefinition*> map;
        for (const auto& def : All())
        {
            for (const auto& name : def.processNames)
            {
                map[ToLowerW(name)] = &def;
            }
        }
        return map;
    }();

    const auto it = byProcess.find(ToLowerW(exe));
    return it != byProcess.end() ? it->second : nullptr;
}

const AppDefinition* AppRegistry::FindById(const std::string& id)
{
    for (const auto& def : All())
    {
        if (def.id == id) return &def;
    }
    return nullptr;
}

bool AppRegistry::IsEnabled(const std::string& id)
{
    std::lock_guard<std::mutex> lock(g_enabledMutex);
    return g_enabledIds.find(id) != g_enabledIds.end();
}

void AppRegistry::SetEnabled(const std::string& id, const bool enabled)
{
    {
        std::lock_guard<std::mutex> lock(g_enabledMutex);
        if (enabled) g_enabledIds.insert(id);
        else g_enabledIds.erase(id);
    }
    Save();
}

void AppRegistry::Load()
{
    std::lock_guard<std::mutex> lock(g_enabledMutex);

    g_enabledIds.clear();
    g_loaded = true;

    const std::string path = Config::GetAppsConfigFilePath();
    if (!path.empty())
    {
        std::ifstream file(path);
        if (file.is_open())
        {
            std::string line;
            while (std::getline(file, line))
            {
                line.erase(std::remove_if(line.begin(), line.end(),
                                          [](const unsigned char c) { return std::isspace(c) != 0; }),
                           line.end());
                if (!line.empty() && FindById(line) != nullptr)
                {
                    g_enabledIds.insert(line);
                }
            }
            return;
        }
    }

    // 파일이 없으면 기본값: Unity만 활성.
    g_enabledIds.insert("unity");
}

void AppRegistry::Save()
{
    std::string path = Config::GetAppsConfigFilePath();
    if (path.empty()) return;

    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(g_enabledMutex);
        ids.assign(g_enabledIds.begin(), g_enabledIds.end());
    }

    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) return;
    for (const auto& id : ids)
    {
        file << id << "\n";
    }
}

int AppRegistry::EnabledCount()
{
    std::lock_guard<std::mutex> lock(g_enabledMutex);
    return static_cast<int>(g_enabledIds.size());
}
