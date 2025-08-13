#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <windows.h>
#include <chrono>

struct UnityInstanceData {
    DWORD processId;
    std::string projectPath;
    std::string projectName;
};

namespace Config {
    const std::vector<std::string> UNITY_FILE_EXTENSIONS = {
        ".cs", ".unity", ".prefab", ".anim", ".controller", ".asset", ".shader"
    };

    const std::vector<std::string> IGNORE_FOLDERS = {
        "Library", "Logs", "obj", "Temp", "UserSettings", ".idea", ".vs"
    };
}