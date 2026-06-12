<div align="center">

## Creative - WakaTime

[![License: MIT](https://img.shields.io/badge/License-MIT-skyblue.svg?style=for-the-badge&logo=github)](LICENSE)
![GitHub Repo stars](https://img.shields.io/github/stars/snow0406/creative-wakatime?style=for-the-badge&logo=github&color=%23ef8d9d)

🎯 Automatic time tracking for **Unity**, **Aseprite**, **Blender** and **Clip Studio Paint** via WakaTime

---

</div>

### 🚀 Features

- **Multi-app tracking**: Unity, Aseprite, Blender and Clip Studio Paint activity
  is reported to WakaTime from one Creative WakaTime client.
- **Pick what you track**: enable or disable apps from the tray's `Tracked Apps`
  submenu. Only the apps you select are scanned.
- **Unity**: project auto-detection + real-time file-change tracking, with editor
  version detection (e.g. "Unity 2022.3"). Multiple Unity instances are mapped to
  their own project.
- **Aseprite / Blender / Clip Studio Paint**: presence + foreground window-title
  tracking estimates the active file. See the note below about how this is an
  *activity-time estimate*.
- **Pause Monitoring**: a single global gate that blocks all heartbeats instantly.
- **System Tray**: runs quietly in the background with a right-click menu.

### 🔧 System Requirements

- **OS**: Windows
- **API key**: WakaTime API key (free at wakatime.com)

### 📦 Installation

1. Go to [Releases](https://github.com/Snow0406/creative-wakatime/releases)
2. Download the latest `Creative-Wakatime_vX.X.X.zip`
3. Extract to your preferred location
4. Run `creative_wakatime.exe`

### ⚙️ Setup

1. **Get a WakaTime API Key**:
    - Visit https://wakatime.com/api-key
    - Copy your API key

2. **Configure Creative WakaTime**:
    - Right-click the system tray icon
    - Click "Setup API Key"
    - The WakaTime website opens automatically
    - Copy your API key and click OK (it is read from the clipboard)
    - The API key is stored at `%APPDATA%/creative-wakatime/wakatime_config.txt`

3. **Choose tracked apps**:
    - Right-click the tray icon → `Tracked Apps`
    - Check the apps you want to track (Unity / Aseprite / Blender / Clip Studio Paint)
    - Your selection is saved to `%APPDATA%/creative-wakatime/apps.txt`
    - By default only **Unity** is enabled on first run.

4. **Start working** — open a tracked app and Creative WakaTime detects it
   automatically.

### 🧩 How tracking works

Two strategies are used depending on the app:

```
Unity                                 -> ProcessMonitor (-projectPath) -> FileWatcher (file writes) -> WakaTime API
Aseprite / Blender / Clip Studio Paint -> ProcessMonitor (presence)     -> FocusDetector (window title) -> WakaTime API
```

- **Unity (DirectoryWatch)** — the project folder is watched recursively and each
  relevant file write produces a `is_write=true` heartbeat. Focus on a Unity window
  also produces a periodic keep-alive heartbeat for the focused project.

- **Aseprite / Blender / Clip Studio Paint (WindowTitle)** — these apps are not
  hooked directly. The app being running means it is "active", and the foreground
  window title is parsed to estimate which file you are editing. Heartbeats are
  sent on focus, on title change, and periodically while focused (`is_write=false`).

> **Note:** WindowTitle tracking is an **activity-time estimate**, not a record of
> real saves/edits. The entity is derived from the launch command line and the live
> window title. Unsaved / "Untitled" documents are skipped, and dirty markers (`*`,
> `●`) are normalized so the same file is not double-counted.

Window-title formats differ per app and per version, so the parser is heuristic. If
your file is not detected correctly, the command-line path (the file you opened at
launch) is used as a fallback.

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

---

Made with ♥ by hy
