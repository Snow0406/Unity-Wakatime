#include "wakatime_client.h"

namespace
{
    constexpr size_t kMaxHeartbeatQueueSize = 256;
    constexpr size_t kMaxDebounceEntries = 256;
    constexpr auto kSameFileHeartbeatInterval = std::chrono::seconds(120);
    constexpr auto kSameFileWriteInterval = std::chrono::seconds(2);

    std::string WideToUtf8(const wchar_t *value)
    {
        if (value == nullptr || value[0] == L'\0') return "";

        const int len = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
        if (len <= 1) return "";

        std::string result(len - 1, 0);
        WideCharToMultiByte(CP_UTF8, 0, value, -1, &result[0], len, nullptr, nullptr);
        return result;
    }
}

WakaTimeClient::WakaTimeClient() : hSession(nullptr),
                                   hConnect(nullptr),
                                   initialized(false),
                                   shouldStop(false),
                                   totalSent(0),
                                   totalFailed(0)
{
    userAgent = "creative-wakatime/" + Config::APP_VERSION + " (Windows)";
    machineName = GetMachineName();

    WT_LOG("[WakaTimeClient] Created for machine: " << machineName);
}

WakaTimeClient::~WakaTimeClient()
{
    shouldStop = true;
    queueCv.notify_all();
    if (senderThread.joinable())
    {
        senderThread.join();
    }

    CleanupHttpSession(); // HTTP 세션 정리

    WT_LOG("[WakaTimeClient] Destroyed (Sent: " << totalSent.load() << ", Failed: " << totalFailed.load() << ")");
}

bool WakaTimeClient::Initialize(const std::string &providedApiKey)
{
    WT_LOG("[WakaTimeClient] Initializing...");

    if (!providedApiKey.empty()) // API 키 설정
    {
        apiKey = providedApiKey;
        SaveApiKeyToFile(apiKey); // 파일에 저장
    }
    else
    {
        // 파일에서 로드 시도
        if (!LoadApiKeyFromFile())
        {
            WT_ERR("[WakaTimeClient] No API key provided and failed to load from file");
            return false;
        }
    }

    // HTTP 세션 초기화
    if (!InitializeHttpSession())
    {
        WT_ERR("[WakaTimeClient] Failed to initialize HTTP session");
        return false;
    }

    // 백그라운드 전송 스레드 시작
    senderThread = std::thread(&WakaTimeClient::SenderThreadFunction, this);

    initialized = true;
    WT_LOG("[WakaTimeClient] Initialized successfully with API key");

    return true;
}

bool WakaTimeClient::ReInitialize(const std::string& newApiKey)
{
    WT_LOG("[WakaTimeClient] Reinitializing with new API key...");

    shouldStop = true;
    queueCv.notify_all();
    if (senderThread.joinable())
    {
        senderThread.join();
    }

    CleanupHttpSession();

    initialized = false;
    shouldStop = false;

    return Initialize(newApiKey);
}

bool WakaTimeClient::InitializeHttpSession()
{
    // WinHttpOpen: HTTP 세션 생성
    // WINHTTP_ACCESS_TYPE_DEFAULT_PROXY: 시스템 기본 프록시 설정 사용
    hSession = WinHttpOpen(
        std::wstring(userAgent.begin(), userAgent.end()).c_str(), // User-Agent
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0 // 동기 모드
    );

    if (hSession == nullptr)
    {
        const DWORD error = GetLastError();
        WT_ERR("[WakaTimeClient] WinHttpOpen failed (Error: " << error << ")");
        return false;
    }

    // 연결 핸들을 한 번 생성해 heartbeat 간 재사용한다(keep-alive로 TLS 핸드셰이크 절감).
    hConnect = WinHttpConnect(hSession, L"api.wakatime.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (hConnect == nullptr)
    {
        const DWORD error = GetLastError();
        WT_ERR("[WakaTimeClient] WinHttpConnect failed (Error: " << error << ")");
        WinHttpCloseHandle(hSession);
        hSession = nullptr;
        return false;
    }

    WT_LOG("[WakaTimeClient] HTTP session created");
    return true;
}

void WakaTimeClient::CleanupHttpSession()
{
    if (hConnect != nullptr)
    {
        WinHttpCloseHandle(hConnect);
        hConnect = nullptr;
    }
    if (hSession != nullptr)
    {
        WinHttpCloseHandle(hSession);
        hSession = nullptr;
        WT_LOG("[WakaTimeClient] HTTP session closed");
    }
}

std::string WakaTimeClient::GetMachineName()
{
    WCHAR computerName[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;

    if (GetComputerNameW(computerName, &size))
    {
        // 와이드 문자를 일반 문자로 변환
        return WideToUtf8(computerName);
    }

    return "Unknown";
}

int64_t WakaTimeClient::GetUnixTimestamp()
{
    // std::chrono를 사용해서 Unix timestamp 생성
    const auto now = std::chrono::system_clock::now();
    const auto epoch = now.time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(epoch);
    return seconds.count();
}

std::string WakaTimeClient::EscapeJsonString(const std::string &str)
{
    std::string escaped;
    escaped.reserve(str.length() * 2); // 성능 최적화

    for (const char c: str)
    {
        switch (c)
        {
            case '"': escaped += "\\\"";
                break;
            case '\\': escaped += "\\\\";
                break;
            case '\b': escaped += "\\b";
                break;
            case '\f': escaped += "\\f";
                break;
            case '\n': escaped += "\\n";
                break;
            case '\r': escaped += "\\r";
                break;
            case '\t': escaped += "\\t";
                break;
            default:
                if (c < 0x20)
                {
                    escaped += "\\u";
                    escaped += "0000";
                } else
                {
                    escaped += c;
                }
                break;
        }
    }

    return escaped;
}

std::string WakaTimeClient::HeartbeatToJson(const HeartbeatData &heartbeat)
{
    std::ostringstream json;
    json << "{"
            << R"("entity":")" << EscapeJsonString(heartbeat.entity) << "\","
            << R"("type":")" << heartbeat.type << "\","
            << R"("category":")" << heartbeat.category << "\","
            << R"("project":")" << EscapeJsonString(heartbeat.project) << "\","
            << R"("language":")" << heartbeat.language << "\","
            << R"("editor":")" << heartbeat.editor << "\","
            << R"("operating_system":")" << heartbeat.operating_system << "\","
            << R"("time":)" << heartbeat.time << ","
            << R"("is_write":)" << (heartbeat.is_write ? "true" : "false")
            << "}";

    return json.str();
}

std::string WakaTimeClient::Base64Encode(const std::string &input)
{
    const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;

    int i = 0;
    unsigned char char_array_3[3];
    unsigned char char_array_4[4];

    for (const char c: input)
    {
        char_array_3[i++] = c;
        if (i == 3)
        {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;

            for (i = 0; i < 4; i++)
            {
                encoded += chars[char_array_4[i]];
            }
            i = 0;
        }
    }

    if (i)
    {
        for (int j = i; j < 3; j++)
        {
            char_array_3[j] = '\0';
        }

        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
        char_array_4[3] = char_array_3[2] & 0x3f;

        for (int j = 0; j < i + 1; j++)
        {
            encoded += chars[char_array_4[j]];
        }

        while (i++ < 3)
        {
            encoded += '=';
        }
    }

    return encoded;
}

std::string WakaTimeClient::BuildUserAgent(const HeartbeatData &heartbeat) const
{
    // WakaTime 대시보드는 User-Agent 끝의 plugin 토큰으로 editor를 식별한다.
    // 형식: creative-wakatime/{ver} (Windows) {editor} {plugin}/{ver}
    const std::string base = "creative-wakatime/" + Config::APP_VERSION + " (Windows) ";

    // editor 문자열에서 첫 토큰으로 어떤 도구인지 판단 ("Unity 2022.3" → "Unity").
    std::string editorName = heartbeat.editor;
    if (const size_t spacePos = editorName.find(' '); spacePos != std::string::npos)
    {
        editorName = editorName.substr(0, spacePos);
    }

    std::string lowered = editorName;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](const unsigned char c) { return static_cast<char>(::tolower(c)); });

    if (lowered == "aseprite")
    {
        return base + "Aseprite aseprite-wakatime/" + Config::APP_VERSION;
    }

    // 기본값: Unity (editor 비어있을 때 포함)
    const std::string editorToken = heartbeat.editor.empty() ? "Unity" : heartbeat.editor;
    return base + editorToken + " unity-wakatime/" + Config::APP_VERSION;
}

bool WakaTimeClient::SendHttpRequest(const std::string &jsonData, const HeartbeatData &heartbeat)
{
    if (!initialized || hSession == nullptr)
    {
        WT_ERR("[WakaTimeClient] Not initialized");
        return false;
    }

    // 끊겼던 경우 재연결
    if (hConnect == nullptr)
    {
        hConnect = WinHttpConnect(hSession, L"api.wakatime.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (hConnect == nullptr)
        {
            const DWORD error = GetLastError();
            WT_ERR("[WakaTimeClient] WinHttpConnect failed (Error: " << error << ")");
            return false;
        }
    }

    // WinHttpOpenRequest: HTTP 요청 생성
    const HINTERNET hRequest = WinHttpOpenRequest(
        hConnect,
        L"POST",                                // HTTP 메서드
        L"/api/v1/users/current/heartbeats",    // URL 경로
        nullptr,                                // HTTP 버전 (기본값 사용)
        WINHTTP_NO_REFERER,                     // Referer 없음
        WINHTTP_DEFAULT_ACCEPT_TYPES,           // Accept 헤더 기본값
        WINHTTP_FLAG_SECURE                     // HTTPS 사용
    );

    if (hRequest == nullptr)
    {
        const DWORD error = GetLastError();
        WT_ERR("[WakaTimeClient] WinHttpOpenRequest failed (Error: " << error << ")");
        return false;
    }

    // HTTP 헤더 설정
    std::string authHeader = "Authorization: Basic " + Base64Encode(apiKey + ":");
    std::string contentTypeHeader = "Content-Type: application/json";
    std::string userAgentHeader = "User-Agent: " + BuildUserAgent(heartbeat);
    std::string machineHeader = "X-Machine-Name: " + machineName;

    // 와이드 문자로 변환해서 헤더 추가
    const std::wstring wAuthHeader(authHeader.begin(), authHeader.end());
    const std::wstring wContentTypeHeader(contentTypeHeader.begin(), contentTypeHeader.end());
    const std::wstring wUserAgentHeader(userAgentHeader.begin(), userAgentHeader.end());
    const std::wstring wMachineHeader(machineHeader.begin(), machineHeader.end());

    WinHttpAddRequestHeaders(hRequest, wAuthHeader.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD);
    WinHttpAddRequestHeaders(hRequest, wContentTypeHeader.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD);
    WinHttpAddRequestHeaders(hRequest, wUserAgentHeader.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD);
    WinHttpAddRequestHeaders(hRequest, wMachineHeader.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD);

    // HTTP 요청 전송
    const BOOL result = WinHttpSendRequest(
        hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS,  // 추가 헤더 없음
        0,                              // 추가 헤더 길이
        (LPVOID)jsonData.c_str(),       // 요청 본문
        jsonData.length(),              // 본문 길이
        jsonData.length(),              // 총 길이
        0                               // 컨텍스트
    );

    bool success = false;
    bool transportError = false;

    if (result)
    {
        // 응답 수신 대기
        if (WinHttpReceiveResponse(hRequest, nullptr))
        {
            // HTTP 상태 코드 확인
            DWORD statusCode = 0;
            DWORD statusCodeSize = sizeof(statusCode);

            if (WinHttpQueryHeaders(hRequest,
                                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX,
                                    &statusCode,
                                    &statusCodeSize,
                                    WINHTTP_NO_HEADER_INDEX))
            {
                if (statusCode >= 200 && statusCode < 300)
                {
                    success = true;
                    ++totalSent;
                }
                else
                {
                    ++totalFailed;
                    WT_LOG("[WakaTimeClient] ❌ Heartbeat failed (HTTP " << statusCode << ")");
                }
            }
        }
        else
        {
            transportError = true;
            ++totalFailed;
            WT_ERR("[WakaTimeClient] WinHttpReceiveResponse failed (Error: " << GetLastError() << ")");
        }
    }
    else
    {
        const DWORD error = GetLastError();
        transportError = true;
        ++totalFailed;
        WT_ERR("[WakaTimeClient] WinHttpSendRequest failed (Error: " << error << ")");
    }

    WinHttpCloseHandle(hRequest);

    // 전송/수신 단계 실패는 연결이 끊겼을 수 있으므로 캐시한 연결을 폐기 → 다음 전송에서 재연결
    if (transportError && hConnect != nullptr)
    {
        WinHttpCloseHandle(hConnect);
        hConnect = nullptr;
    }

    return success;
}

void WakaTimeClient::SenderThreadFunction()
{
    WT_LOG("[WakaTimeClient] Sender thread started");

    while (true)
    {
        HeartbeatData heartbeat;

        // 큐에 데이터가 들어오거나 종료될 때까지 블록 (busy-poll 제거)
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            queueCv.wait(lock, [this] { return shouldStop.load() || !heartbeatQueue.empty(); });

            if (shouldStop && heartbeatQueue.empty())
            {
                break;
            }

            heartbeat = heartbeatQueue.front();
            heartbeatQueue.pop();
        }

        const std::string jsonData = HeartbeatToJson(heartbeat);
        SendHttpRequest(jsonData, heartbeat);

        // 연속 전송 레이트리밋. 종료 시 즉시 깨어나도록 wait_for 사용
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            queueCv.wait_for(lock, std::chrono::milliseconds(1000), [this] { return shouldStop.load(); });
        }
    }

    WT_LOG("[WakaTimeClient] Sender thread stopped");
}

void WakaTimeClient::SendHeartbeat(const std::string &filePath, const std::string &projectName, const std::string& unityVersion, const bool isWrite)
{
    if (!initialized)
    {
        WT_ERR("[WakaTimeClient] Not initialized, cannot send heartbeat");
        return;
    }

    // HeartbeatData 생성 (Unity 컨텍스트)
    HeartbeatData heartbeat;
    heartbeat.entity = filePath;
    heartbeat.project = projectName;
    heartbeat.language = "Unity";
    heartbeat.time = GetUnixTimestamp();
    heartbeat.is_write = isWrite;

    if (!unityVersion.empty())
    {
        heartbeat.editor = "Unity " + unityVersion;
    }
    else
    {
        heartbeat.editor = "Unity";
    }

    EnqueueHeartbeat(heartbeat);
}

void WakaTimeClient::EnqueueHeartbeat(const HeartbeatData &heartbeat)
{
    if (!initialized)
    {
        WT_ERR("[WakaTimeClient] Not initialized, cannot enqueue heartbeat");
        return;
    }

    // 큐에 추가 (비동기 전송)
    {
        std::lock_guard<std::mutex> lock(queueMutex);

        const auto now = std::chrono::steady_clock::now();
        // entity + project를 \x1f(unit separator)로 결합해 컨텍스트별 키 생성
        const std::string key = heartbeat.entity + '\x1f' + heartbeat.project;

        if (const auto it = lastQueuedByEntity.find(key); it != lastQueuedByEntity.end())
        {
            const auto elapsed = now - it->second;
            const auto minInterval = heartbeat.is_write ? kSameFileWriteInterval : kSameFileHeartbeatInterval;
            if (elapsed < minInterval)
            {
                return;
            }
        }

        while (heartbeatQueue.size() >= kMaxHeartbeatQueueSize)
        {
            heartbeatQueue.pop(); // 메모리 폭주 방지를 위해 가장 오래된 heartbeat 제거
        }
        heartbeatQueue.push(heartbeat);
        lastQueuedByEntity[key] = now;

        // debounce 맵 무한 증가 방지: 상한 초과 시 만료(>heartbeat interval)된 엔트리 정리.
        // 그래도 상한을 넘으면 가장 오래된 엔트리를 제거한다.
        if (lastQueuedByEntity.size() > kMaxDebounceEntries)
        {
            for (auto it = lastQueuedByEntity.begin(); it != lastQueuedByEntity.end();)
            {
                if (now - it->second > kSameFileHeartbeatInterval)
                {
                    it = lastQueuedByEntity.erase(it);
                }
                else
                {
                    ++it;
                }
            }

            while (lastQueuedByEntity.size() > kMaxDebounceEntries)
            {
                auto oldest = lastQueuedByEntity.begin();
                for (auto it = lastQueuedByEntity.begin(); it != lastQueuedByEntity.end(); ++it)
                {
                    if (it->second < oldest->second) oldest = it;
                }
                lastQueuedByEntity.erase(oldest);
            }
        }

        queueCv.notify_one();
    }
}

void WakaTimeClient::SendHeartbeatFromEvent(const FileChangeEvent &event)
{
    // FileChangeEvent를 HeartbeatData로 변환
    const bool isWrite = (event.action == FILE_ACTION_MODIFIED || event.action == FILE_ACTION_ADDED || event.action == FILE_ACTION_RENAMED_NEW_NAME);
    SendHeartbeat(event.filePath, event.projectName, event.unityVersion, isWrite);
}

size_t WakaTimeClient::GetQueueSize() const
{
    std::lock_guard<std::mutex> lock(queueMutex);
    return heartbeatQueue.size();
}

void WakaTimeClient::GetStats(int& sent, int& failed) const {
    sent = totalSent.load();
    failed = totalFailed.load();
}

std::string WakaTimeClient::GetMaskedApiKey() const
{
    if (apiKey.empty()) return "[ Not Set ]";
    if (apiKey.length() <= 8 ) return "****";
    return apiKey.substr(0, 8) + "****" + apiKey.substr(apiKey.length() - 4);
}

bool WakaTimeClient::LoadApiKeyFromFile()
{
    const std::string configPath = Config::GetConfigFilePath();
    if (configPath.empty())
    {
        WT_ERR("[WakaTimeClient] Failed to resolve config path (APPDATA missing)");
        return false;
    }

    std::ifstream file(configPath);
    if (!file.is_open())
    {
        WT_LOG("[WakaTimeClient] Config file not found: " << configPath);
        return false;
    }

    std::getline(file, apiKey);
    file.close();

    apiKey.erase(std::remove_if(apiKey.begin(), apiKey.end(),
                                [](const unsigned char c) { return std::isspace(c) != 0; }),
                 apiKey.end());

    if (apiKey.empty())
    {
        WT_LOG("[WakaTimeClient] Empty API key in config file");
        return false;
    }

    WT_LOG("[WakaTimeClient] API key loaded from file: " << GetMaskedApiKey());
    return true;
}

bool WakaTimeClient::SaveApiKeyToFile(const std::string &key)
{
    const std::string configPath = Config::GetConfigFilePath();
    if (configPath.empty())
    {
        WT_ERR("[WakaTimeClient] Failed to resolve config path (APPDATA missing)");
        return false;
    }

    std::ofstream file(configPath);
    if (!file.is_open())
    {
        WT_ERR("[WakaTimeClient] Failed to save API key to: " << configPath);
        return false;
    }

    file << key << std::endl;
    file.close();

    WT_LOG("[WakaTimeClient] API key saved to: " << configPath);
    return true;
}

void WakaTimeClient::FlushQueue()
{
    WT_LOG("[WakaTimeClient] Flushing queue...");

    if (const size_t remaining = GetQueueSize(); remaining == 0)
    {
        WT_LOG("[WakaTimeClient] Queue is empty");
        return;
    }

    // 큐가 빌 때까지 대기 (최대 30초)
    const auto startTime = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::seconds(30);

    while (GetQueueSize() > 0)
    {
        if (auto elapsed = std::chrono::steady_clock::now() - startTime; elapsed > timeout)
        {
            WT_LOG("[WakaTimeClient] Flush timeout, " << GetQueueSize() << " items remaining");
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    WT_LOG("[WakaTimeClient] Queue flushed");
}

