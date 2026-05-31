<div align="center">

## Creative - WakaTime

[![License: MIT](https://img.shields.io/badge/License-MIT-skyblue.svg?style=for-the-badge&logo=github)](LICENSE)
![GitHub Repo stars](https://img.shields.io/github/stars/snow0406/creative-wakatime?style=for-the-badge&logo=github&color=%23ef8d9d)

🎯 Automatic time tracking for **Unity** and **Aseprite** via WakaTime

---

</div>

### 🚀 Features

- **Unity**: Automatic project detection + real-time file change tracking
- **Aseprite**: Editing & saving activity tracking via a lightweight Lua extension
- **Version Support**: Detects Unity editor versions (e.g., "Unity 2022.3")
- **System Tray**: Runs quietly in background with right-click menu
- **Smart Notifications**: Shows project start/stop alerts
- **Separate dashboards**: Unity and Aseprite are reported as distinct editors on WakaTime

### 🔧 System Requirements

- **OS**: Windows
- **API key**: WakaTime API KEY (free at wakatime.com)
- **Aseprite** (optional): for sprite/pixel-art tracking

### 📦 Installation

1. Go to [Releases](https://github.com/Snow0406/creative-wakatime/releases)
2. Download the latest `Creative-Wakatime_vX.X.X.zip`
3. Extract to your preferred location
4. Run `creative_wakatime.exe`

### ⚙️ Setup

1. **Get WakaTime API Key**:
    - Visit https://wakatime.com/api-key
    - Copy your API key

2. **Configure Creative WakaTime**:
    - Right-click the system tray icon
    - Click "🔑 Setup API Key"
    - The WakaTime website will open automatically
    - Copy your API key and click OK
    - Wait for validation ✅
    - The API key is stored at `%APPDATA%/creative-wakatime/wakatime_config.txt`

3. **Unity** — Start and Code!
    - Open any Unity project
    - The app automatically detects and starts tracking

4. **Aseprite** — Install the extension (optional):
    - Download `creative-wakatime.aseprite-extension` from the same release.
    - Double-click it, or install it from Aseprite via
      **Edit > Preferences > Extensions > Add Extension**.
    - Restart Aseprite.
    - The extension writes local event files to `%APPDATA%/creative-wakatime/events/`.
    - Creative WakaTime watches that folder and forwards heartbeats.

### 🧩 How Aseprite tracking works

Aseprite cannot be hooked directly from C++, so a small Aseprite **Lua extension**
emits local JSON event files on edit/save. The tray app watches
`%APPDATA%/creative-wakatime/events/` (event-driven, no polling) and converts each
event into a WakaTime heartbeat, then deletes the file.

```
Unity     -> ProcessMonitor / FileWatcher -> WakaTimeClient -> WakaTime API
Aseprite  -> Lua extension -> events/*.json -> Inbox bridge -> WakaTimeClient -> WakaTime API
```

Aseprite events are intentionally optional. If the extension is not installed,
Creative WakaTime only watches an empty local inbox and no Aseprite heartbeat is
created.

#### Aseprite extension behavior

- **Edit activity** (`sitechange`): active sprite/layer/frame changes are reported
  as `is_write=false` with a 2-minute per-file debounce.
- **Save activity** (`aftercommand` → `SaveFile`/`SaveFileAs`/`SaveFileCopyAs`):
  reported immediately as `is_write=true`.
- Unsaved sprites without a file path are ignored.
- `project` is the parent folder name of the sprite file.
- The extension does not call the WakaTime API directly; it only writes local
  `.json` event files for the tray app to consume.

#### Manual Aseprite extension install

If you are building from source instead of using the release asset, zip the
contents of `aseprite-extension/` so that `package.json` and `wakatime.lua` are at
the root of the archive, then rename the archive to
`creative-wakatime.aseprite-extension`.

You can also copy the folder manually:

```text
%APPDATA%/Aseprite/extensions/creative-wakatime/
```

The folder must contain both `package.json` and `wakatime.lua`.

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

---

Made with ♥ by hy
