#include "wakatime_client.h"

WakaTimeClient::WakaTimeClient() : hSession(nullptr),
                                   initialized(false),
                                   shouldStop(false),
                                   totalSent(0),
                                   totalFailed(0)
{
    userAgent = "unity-wakatime/1.0 (Windows)";
    machineName = GetMachineName();

    std::cout << "[WakaTimeClient] Created for machine: " << machineName << std::endl;
}

WakaTimeClient::~WakaTimeClient()
{
    shouldStop = true;
    if (senderThread.joinable())
    {
        senderThread.join();
    }

    CleanupHttpSession(); // HTTP 세션 정리

    std::cout << "[WakaTimeClient] Destroyed (Sent: " << totalSent.load() << ", Failed: " << totalFailed.load() << ")"
            << std::endl;
}

bool WakaTimeClient::Initialize(const std::string &providedApiKey)
{
    std::cout << "[WakaTimeClient] Initializing..." << std::endl;

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
            std::cerr << "[WakaTimeClient] No API key provided and failed to load from file" << std::endl;
            return false;
        }
    }

    // HTTP 세션 초기화
    if (!InitializeHttpSession())
    {
        std::cerr << "[WakaTimeClient] Failed to initialize HTTP session" << std::endl;
        return false;
    }

    // 백그라운드 전송 스레드 시작
    senderThread = std::thread(&WakaTimeClient::SenderThreadFunction, this);

    initialized = true;
    std::cout << "[WakaTimeClient] Initialized successfully with API key" << std::endl;

    return true;
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
        std::cerr << "[WakaTimeClient] WinHttpOpen failed (Error: " << error << ")" << std::endl;
        return false;
    }

    std::cout << "[WakaTimeClient] HTTP session created" << std::endl;
    return true;
}

void WakaTimeClient::CleanupHttpSession()
{
    if (hSession != nullptr)
    {
        WinHttpCloseHandle(hSession);
        hSession = nullptr;
        std::cout << "[WakaTimeClient] HTTP session closed" << std::endl;
    }
}

std::string WakaTimeClient::GetMachineName()
{
    WCHAR computerName[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;

    if (GetComputerNameW(computerName, &size))
    {
        // 와이드 문자를 일반 문자로 변환
        if (const int len = WideCharToMultiByte(CP_UTF8, 0, computerName, -1, nullptr, 0, nullptr, nullptr); len > 0)
        {
            std::string result(len - 1, 0);
            WideCharToMultiByte(CP_UTF8, 0, computerName, -1, &result[0], len, nullptr, nullptr);
            return result;
        }
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
            << R"("machine":")" << EscapeJsonString(heartbeat.machine) << "\","
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

bool WakaTimeClient::SendHttpRequest(const std::string &jsonData)
{
    if (!initialized || hSession == nullptr)
    {
        std::cerr << "[WakaTimeClient] Not initialized" << std::endl;
        return false;
    }

    // WinHttpConnect: 서버 연결
    const HINTERNET hConnect = WinHttpConnect(
        hSession,
        L"api.wakatime.com",            // 서버 주소
        INTERNET_DEFAULT_HTTPS_PORT,    // HTTPS 포트 (443)
        0
    );

    if (hConnect == nullptr)
    {
        const DWORD error = GetLastError();
        std::cerr << "[WakaTimeClient] WinHttpConnect failed (Error: " << error << ")" << std::endl;
        return false;
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
        std::cerr << "[WakaTimeClient] WinHttpOpenRequest failed (Error: " << error << ")" << std::endl;
        WinHttpCloseHandle(hConnect);
        return false;
    }

    // HTTP 헤더 설정
    std::string authHeader = "Authorization: Basic " + Base64Encode(apiKey + ":");
    std::string contentTypeHeader = "Content-Type: application/json";
    std::string userAgentHeader = "User-Agent: " + userAgent;

    // 와이드 문자로 변환해서 헤더 추가
    const std::wstring wAuthHeader(authHeader.begin(), authHeader.end());
    const std::wstring wContentTypeHeader(contentTypeHeader.begin(), contentTypeHeader.end());
    const std::wstring wUserAgentHeader(userAgentHeader.begin(), userAgentHeader.end());

    WinHttpAddRequestHeaders(hRequest, wAuthHeader.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD);
    WinHttpAddRequestHeaders(hRequest, wContentTypeHeader.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD);
    WinHttpAddRequestHeaders(hRequest, wUserAgentHeader.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD);

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
                    std::cout << "[WakaTimeClient] ❌ Heartbeat failed (HTTP " << statusCode << ")" << std::endl;
                }
            }
        }
    }
    else
    {
        const DWORD error = GetLastError();
        ++totalFailed;
        std::cerr << "[WakaTimeClient] WinHttpSendRequest failed (Error: " << error << ")" << std::endl;
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);

    return success;
}

void WakaTimeClient::SenderThreadFunction()
{
    std::cout << "[WakaTimeClient] Sender thread started" << std::endl;

    while (!shouldStop)
    {
        HeartbeatData heartbeat;
        bool hasData = false;

        // 큐에서 heartbeat 가져오기
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            if (!heartbeatQueue.empty())
            {
                heartbeat = heartbeatQueue.front();
                heartbeatQueue.pop();
                hasData = true;
            }
        }

        if (hasData)
        {
            std::string jsonData = HeartbeatToJson(heartbeat);

            SendHttpRequest(jsonData);

            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
        else
        {
            // 큐가 비어있으면 잠시 대기
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

    std::cout << "[WakaTimeClient] Sender thread stopped" << std::endl;
}

void WakaTimeClient::SendHeartbeat(const std::string &filePath, const std::string &projectName, const std::string& unityVersion, const bool isWrite)
{
    if (!initialized)
    {
        std::cerr << "[WakaTimeClient] Not initialized, cannot send heartbeat" << std::endl;
        return;
    }

    // HeartbeatData 생성
    HeartbeatData heartbeat;
    heartbeat.entity = filePath;
    heartbeat.project = projectName;
    heartbeat.language = "Unity";
    heartbeat.machine = machineName;
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

    // 큐에 추가 (비동기 전송)
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        heartbeatQueue.push(heartbeat);
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
    return apiKey.substr(0, 8) + "****" + apiKey.substr(apiKey.length() - 4);
}

void WakaTimeClient::SetApiKey(const std::string &newApiKey)
{
    apiKey = newApiKey;
    SaveApiKeyToFile(apiKey);
    std::cout << "[WakaTimeClient] API key updated" << std::endl;
}

bool WakaTimeClient::LoadApiKeyFromFile()
{
    const std::string configPath = "wakatime_config.txt";

    std::ifstream file(configPath);
    if (!file.is_open())
    {
        std::cout << "[WakaTimeClient] Config file not found: " << configPath << std::endl;
        return false;
    }

    std::getline(file, apiKey);
    file.close();

    apiKey.erase(std::remove_if(apiKey.begin(), apiKey.end(), isspace), apiKey.end());

    if (apiKey.empty())
    {
        std::cout << "[WakaTimeClient] Empty API key in config file" << std::endl;
        return false;
    }

    std::cout << "[WakaTimeClient] API key loaded from file: " << GetMaskedApiKey() << std::endl;
    return true;
}

bool WakaTimeClient::SaveApiKeyToFile(const std::string &key)
{
    const std::string configPath = "wakatime_config.txt";

    std::ofstream file(configPath);
    if (!file.is_open())
    {
        std::cerr << "[WakaTimeClient] Failed to save API key to: " << configPath << std::endl;
        return false;
    }

    file << key << std::endl;
    file.close();

    std::cout << "[WakaTimeClient] API key saved to: " << configPath << std::endl;
    return true;
}

void WakaTimeClient::FlushQueue()
{
    std::cout << "[WakaTimeClient] Flushing queue..." << std::endl;

    if (const size_t remaining = GetQueueSize(); remaining == 0)
    {
        std::cout << "[WakaTimeClient] Queue is empty" << std::endl;
        return;
    }

    // 큐가 빌 때까지 대기 (최대 30초)
    const auto startTime = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::seconds(30);

    while (GetQueueSize() > 0)
    {
        if (auto elapsed = std::chrono::steady_clock::now() - startTime; elapsed > timeout)
        {
            std::cout << "[WakaTimeClient] Flush timeout, " << GetQueueSize() << " items remaining" << std::endl;
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "[WakaTimeClient] Queue flushed" << std::endl;
}
