#include "wakatime_client.h"
#include "app_registry.h"

#include <cctype>

namespace
{
    constexpr size_t kMaxHeartbeatQueueSize = 256;
    constexpr size_t kMaxDebounceEntries = 256;
    constexpr DWORD kHttpResolveTimeoutMs = 5000;
    constexpr DWORD kHttpConnectTimeoutMs = 10000;
    constexpr DWORD kHttpSendTimeoutMs = 15000;
    constexpr DWORD kHttpReceiveTimeoutMs = 15000;
    constexpr int kMaxRetryAttempts = 3;
    constexpr auto kSameFileHeartbeatInterval = std::chrono::seconds(120);
    constexpr auto kSameFileWriteInterval = std::chrono::seconds(2);

    size_t FindJsonArrayEnd(const std::string& text, const size_t openBracket)
    {
        int depth = 0;
        bool inString = false;
        bool escaped = false;

        for (size_t i = openBracket; i < text.size(); ++i)
        {
            const char c = text[i];
            if (inString)
            {
                if (escaped)
                {
                    escaped = false;
                }
                else if (c == '\\')
                {
                    escaped = true;
                }
                else if (c == '"')
                {
                    inString = false;
                }
                continue;
            }

            if (c == '"')
            {
                inString = true;
            }
            else if (c == '[')
            {
                ++depth;
            }
            else if (c == ']')
            {
                --depth;
                if (depth == 0) return i;
            }
        }

        return std::string::npos;
    }

    bool TryParseBulkItemStatus(const std::string& text, const size_t itemStart, const size_t itemEnd, int& statusCode)
    {
        int bracketDepth = 0;
        int braceDepth = 0;
        bool inString = false;
        bool escaped = false;

        for (size_t i = itemStart; i <= itemEnd && i < text.size(); ++i)
        {
            const char c = text[i];
            if (inString)
            {
                if (escaped)
                {
                    escaped = false;
                }
                else if (c == '\\')
                {
                    escaped = true;
                }
                else if (c == '"')
                {
                    inString = false;
                }
                continue;
            }

            if (c == '"')
            {
                inString = true;
            }
            else if (c == '[')
            {
                ++bracketDepth;
            }
            else if (c == ']')
            {
                --bracketDepth;
            }
            else if (c == '{')
            {
                ++braceDepth;
            }
            else if (c == '}')
            {
                --braceDepth;
            }
            else if (c == ',' && bracketDepth == 1 && braceDepth == 0)
            {
                size_t pos = i + 1;
                while (pos <= itemEnd && std::isspace(static_cast<unsigned char>(text[pos])) != 0)
                {
                    ++pos;
                }

                int sign = 1;
                if (pos <= itemEnd && text[pos] == '-')
                {
                    sign = -1;
                    ++pos;
                }

                if (pos > itemEnd || std::isdigit(static_cast<unsigned char>(text[pos])) == 0)
                {
                    return false;
                }

                int value = 0;
                while (pos <= itemEnd && std::isdigit(static_cast<unsigned char>(text[pos])) != 0)
                {
                    value = value * 10 + (text[pos] - '0');
                    ++pos;
                }

                statusCode = sign * value;
                return true;
            }
        }

        return false;
    }

    bool IsRetryableStatus(const int statusCode)
    {
        return statusCode == 429 || statusCode >= 500;
    }

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

    if (!WinHttpSetTimeouts(hSession,
                            kHttpResolveTimeoutMs,
                            kHttpConnectTimeoutMs,
                            kHttpSendTimeoutMs,
                            kHttpReceiveTimeoutMs))
    {
        WT_ERR("[WakaTimeClient] WinHttpSetTimeouts failed (Error: " << GetLastError() << ")");
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
    const std::string base = "creative-wakatime/" + Config::APP_VERSION + " (Windows) ";
    const std::string editorToken = heartbeat.editor.empty() ? "Unknown" : heartbeat.editor;
    return base + editorToken + " creative-wakatime/" + Config::APP_VERSION;
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

std::string WakaTimeClient::HeartbeatsToJsonArray(const std::vector<HeartbeatData> &heartbeats)
{
    std::ostringstream json;
    json << "[";
    for (size_t i = 0; i < heartbeats.size(); ++i)
    {
        if (i > 0) json << ",";
        json << HeartbeatToJson(heartbeats[i]);
    }
    json << "]";
    return json.str();
}

BulkSendResult WakaTimeClient::ParseBulkResponse(const std::string &responseBody)
{
    BulkSendResult result;

    const size_t responsesKey = responseBody.find("\"responses\"");
    if (responsesKey == std::string::npos)
    {
        result.parseError = true;
        return result;
    }

    const size_t responsesArrayStart = responseBody.find('[', responsesKey);
    if (responsesArrayStart == std::string::npos)
    {
        result.parseError = true;
        return result;
    }

    const size_t responsesArrayEnd = FindJsonArrayEnd(responseBody, responsesArrayStart);
    if (responsesArrayEnd == std::string::npos)
    {
        result.parseError = true;
        return result;
    }

    size_t pos = responsesArrayStart + 1;
    while (pos < responsesArrayEnd)
    {
        while (pos < responsesArrayEnd && (std::isspace(static_cast<unsigned char>(responseBody[pos])) != 0 || responseBody[pos] == ','))
        {
            ++pos;
        }
        if (pos >= responsesArrayEnd) break;

        if (responseBody[pos] != '[')
        {
            result.parseError = true;
            return result;
        }

        const size_t itemEnd = FindJsonArrayEnd(responseBody, pos);
        if (itemEnd == std::string::npos || itemEnd > responsesArrayEnd)
        {
            result.parseError = true;
            return result;
        }

        int statusCode = 0;
        if (!TryParseBulkItemStatus(responseBody, pos, itemEnd, statusCode))
        {
            result.parseError = true;
            return result;
        }

        result.perItemStatus.push_back(statusCode);
        pos = itemEnd + 1;
    }

    return result;
}

BulkSendResult WakaTimeClient::SendBulkHttpRequest(const std::string &jsonData, const std::vector<HeartbeatData> &batch)
{
    BulkSendResult result;

    if (!initialized || hSession == nullptr)
    {
        WT_ERR("[WakaTimeClient] Not initialized");
        result.transportError = true;
        return result;
    }
    if (batch.empty())
    {
        result.parseError = true;
        return result;
    }

    if (hConnect == nullptr)
    {
        hConnect = WinHttpConnect(hSession, L"api.wakatime.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (hConnect == nullptr)
        {
            const DWORD error = GetLastError();
            WT_ERR("[WakaTimeClient] WinHttpConnect failed (Error: " << error << ")");
            result.transportError = true;
            return result;
        }
    }

    const HINTERNET hRequest = WinHttpOpenRequest(
        hConnect,
        L"POST",
        L"/api/v1/users/current/heartbeats.bulk",
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE
    );

    if (hRequest == nullptr)
    {
        const DWORD error = GetLastError();
        WT_ERR("[WakaTimeClient] WinHttpOpenRequest failed (Error: " << error << ")");
        result.transportError = true;
        return result;
    }

    const std::string authHeader = "Authorization: Basic " + Base64Encode(apiKey + ":");
    const std::string contentTypeHeader = "Content-Type: application/json";
    const std::string userAgentHeader = "User-Agent: " + BuildUserAgent(batch.front());
    const std::string machineHeader = "X-Machine-Name: " + machineName;

    const std::wstring wAuthHeader(authHeader.begin(), authHeader.end());
    const std::wstring wContentTypeHeader(contentTypeHeader.begin(), contentTypeHeader.end());
    const std::wstring wUserAgentHeader(userAgentHeader.begin(), userAgentHeader.end());
    const std::wstring wMachineHeader(machineHeader.begin(), machineHeader.end());

    WinHttpAddRequestHeaders(hRequest, wAuthHeader.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD);
    WinHttpAddRequestHeaders(hRequest, wContentTypeHeader.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD);
    WinHttpAddRequestHeaders(hRequest, wUserAgentHeader.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD);
    WinHttpAddRequestHeaders(hRequest, wMachineHeader.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD);

    const BOOL sent = WinHttpSendRequest(
        hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS,
        0,
        (LPVOID) jsonData.c_str(),
        static_cast<DWORD>(jsonData.length()),
        static_cast<DWORD>(jsonData.length()),
        0
    );

    if (!sent)
    {
        const DWORD error = GetLastError();
        WT_ERR("[WakaTimeClient] WinHttpSendRequest failed (Error: " << error << ")");
        result.transportError = true;
        WinHttpCloseHandle(hRequest);
        if (hConnect != nullptr)
        {
            WinHttpCloseHandle(hConnect);
            hConnect = nullptr;
        }
        return result;
    }

    if (!WinHttpReceiveResponse(hRequest, nullptr))
    {
        WT_ERR("[WakaTimeClient] WinHttpReceiveResponse failed (Error: " << GetLastError() << ")");
        result.transportError = true;
        WinHttpCloseHandle(hRequest);
        if (hConnect != nullptr)
        {
            WinHttpCloseHandle(hConnect);
            hConnect = nullptr;
        }
        return result;
    }

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(hRequest,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             &statusCode,
                             &statusCodeSize,
                             WINHTTP_NO_HEADER_INDEX))
    {
        WT_ERR("[WakaTimeClient] WinHttpQueryHeaders failed (Error: " << GetLastError() << ")");
        result.transportError = true;
        WinHttpCloseHandle(hRequest);
        if (hConnect != nullptr)
        {
            WinHttpCloseHandle(hConnect);
            hConnect = nullptr;
        }
        return result;
    }

    result.httpStatusCode = static_cast<int>(statusCode);

    std::string responseBody;
    DWORD bytesAvailable = 0;
    while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0)
    {
        std::string chunk(bytesAvailable, '\0');
        DWORD bytesRead = 0;
        if (!WinHttpReadData(hRequest, chunk.data(), bytesAvailable, &bytesRead))
        {
            WT_ERR("[WakaTimeClient] WinHttpReadData failed (Error: " << GetLastError() << ")");
            result.transportError = true;
            break;
        }
        chunk.resize(bytesRead);
        responseBody += chunk;
    }

    WinHttpCloseHandle(hRequest);

    if (result.transportError)
    {
        if (hConnect != nullptr)
        {
            WinHttpCloseHandle(hConnect);
            hConnect = nullptr;
        }
        return result;
    }

    if (statusCode >= 200 && statusCode < 300)
    {
        BulkSendResult parsed = ParseBulkResponse(responseBody);
        parsed.httpStatusCode = static_cast<int>(statusCode);
        return parsed;
    }

    WT_LOG("[WakaTimeClient] Bulk heartbeat failed (HTTP " << statusCode << ")");
    return result;
}

void WakaTimeClient::SenderThreadFunction()
{
    WT_LOG("[WakaTimeClient] Sender thread started");

    auto requeueHeartbeats = [this](std::vector<HeartbeatData> retryList)
    {
        if (retryList.empty()) return;

        std::lock_guard<std::mutex> lock(queueMutex);
        for (auto &heartbeat : retryList)
        {
            if (heartbeat.retryCount >= kMaxRetryAttempts)
            {
                ++totalFailed;
                continue;
            }

            ++heartbeat.retryCount;
            while (heartbeatQueue.size() >= kMaxHeartbeatQueueSize)
            {
                heartbeatQueue.pop();
            }
            heartbeatQueue.push(std::move(heartbeat));
        }
        queueCv.notify_one();
    };

    while (true)
    {
        std::vector<HeartbeatData> batch;

        {
            std::unique_lock<std::mutex> lock(queueMutex);
            queueCv.wait(lock, [this] { return shouldStop.load() || !heartbeatQueue.empty(); });

            if (shouldStop && heartbeatQueue.empty())
            {
                break;
            }

            const size_t count = std::min(kMaxBatchSize, heartbeatQueue.size());
            batch.reserve(count);
            for (size_t i = 0; i < count; ++i)
            {
                batch.emplace_back(std::move(heartbeatQueue.front()));
                heartbeatQueue.pop();
            }
        }

        const std::string jsonData = HeartbeatsToJsonArray(batch);
        const BulkSendResult result = SendBulkHttpRequest(jsonData, batch);

        std::vector<HeartbeatData> retryList;
        if (result.transportError || IsRetryableStatus(result.httpStatusCode))
        {
            retryList = std::move(batch);
        }
        else if (result.httpStatusCode >= 400 && result.httpStatusCode < 500)
        {
            totalFailed.fetch_add(static_cast<int>(batch.size()));
        }
        else if (result.httpStatusCode >= 200 && result.httpStatusCode < 300)
        {
            if (result.parseError || result.perItemStatus.size() != batch.size())
            {
                retryList = std::move(batch);
            }
            else
            {
                for (size_t i = 0; i < batch.size(); ++i)
                {
                    const int itemStatus = result.perItemStatus[i];
                    if (itemStatus == 201 || itemStatus == 202)
                    {
                        ++totalSent;
                    }
                    else if (IsRetryableStatus(itemStatus))
                    {
                        retryList.push_back(std::move(batch[i]));
                    }
                    else
                    {
                        ++totalFailed;
                    }
                }
            }
        }
        else
        {
            retryList = std::move(batch);
        }

        requeueHeartbeats(std::move(retryList));

        {
            std::unique_lock<std::mutex> lock(queueMutex);
            queueCv.wait_for(lock, std::chrono::milliseconds(1000), [this]
            {
                return shouldStop.load() || heartbeatQueue.size() >= kMaxBatchSize;
            });
        }
    }

    WT_LOG("[WakaTimeClient] Sender thread stopped");
}

bool WakaTimeClient::SendHeartbeat(const std::string &appId, const std::string &entity,
                                   const std::string &project, const std::string &editorVersion,
                                   const bool isWrite)
{
    if (!initialized)
    {
        WT_ERR("[WakaTimeClient] Not initialized, cannot send heartbeat");
        return false;
    }

    HeartbeatData heartbeat;
    heartbeat.entity = entity;
    heartbeat.project = project;
    heartbeat.time = GetUnixTimestamp();
    heartbeat.is_write = isWrite;

    if (const AppDefinition *def = AppRegistry::FindById(appId))
    {
        heartbeat.language = def->language;
        heartbeat.editor = editorVersion.empty() ? def->editor : def->editor + " " + editorVersion;
    }
    else
    {
        heartbeat.language = "Unknown";
        heartbeat.editor = "Unknown";
    }

    return EnqueueHeartbeat(heartbeat);
}

bool WakaTimeClient::EnqueueHeartbeat(const HeartbeatData &heartbeat)
{
    if (!initialized)
    {
        WT_ERR("[WakaTimeClient] Not initialized, cannot enqueue heartbeat");
        return false;
    }

    // Pause Monitoring 전역 게이트: 모든 소스(파일/포커스)의 heartbeat를 단일 chokepoint에서 차단.
    if (g_monitoringPaused.load(std::memory_order_acquire))
    {
        return false;
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
                return false;
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

    return true;
}

bool WakaTimeClient::SendHeartbeatFromEvent(const FileChangeEvent &event)
{
    const bool isWrite = (event.action == FILE_ACTION_MODIFIED || event.action == FILE_ACTION_ADDED || event.action == FILE_ACTION_RENAMED_NEW_NAME);
    return SendHeartbeat(event.appId, event.filePath, event.projectName, "", isWrite);
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

