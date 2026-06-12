#pragma once

#include "globals.h"
#include <winhttp.h>    // Windows HTTP API
#include <queue>
#include <unordered_map>
#include <condition_variable>

constexpr size_t kMaxBatchSize = 10;

#pragma comment(lib, "winhttp.lib") // WinHTTP 라이브러리 링크

/**
 * WakaTime Heartbeat 데이터 구조체
 */
struct HeartbeatData {
    std::string entity;             // 파일/프로젝트 경로
    std::string type;               // "file"
    std::string category;           // "coding"
    std::string project;            // 프로젝트 이름
    std::string language;           // WakaTime language
    std::string editor;             // "Unity 2022.3" 등
    std::string operating_system;   // "Windows"
    int64_t time;                   // Unix timestamp
    bool is_write;                  // 파일 수정 여부
    int retryCount;                 // 전송 실패 재시도 횟수

    HeartbeatData() :
        type("file"),
        category("coding"),
        operating_system("Windows"),
        time(0),
        is_write(false),
        retryCount(0) {}
};

struct BulkSendResult {
    bool transportError = false;
    int httpStatusCode = 0;
    bool parseError = false;
    std::vector<int> perItemStatus;
};

/**
 * WakaTime API와 통신하여 heartbeat 데이터를 전송
 */
class WakaTimeClient {
private:
    std::string apiKey;           // WakaTime API 키
    std::string userAgent;        // User-Agent 헤더
    std::string machineName;      // 현재 머신 이름
    
    // HTTP 세션 관리
    HINTERNET hSession;           // WinHTTP 세션 핸들
    HINTERNET hConnect;           // WinHTTP 연결 핸들 (heartbeat 간 재사용, keep-alive)
    bool initialized;             // 초기화 상태

    // 비동기 전송 관리
    mutable std::mutex queueMutex;              // 큐 접근 동기화
    std::condition_variable queueCv;            // 큐 대기/통지 (busy-poll 제거)
    std::queue<HeartbeatData> heartbeatQueue;   // 전송 대기 큐
    // entity+project별 마지막 적재 시각. Unity/Aseprite 파일이 번갈아 들어와도
    // 서로의 debounce를 무효화하지 않도록 컨텍스트별로 분리한다.
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> lastQueuedByEntity;
    std::thread senderThread;                   // 백그라운드 전송 스레드
    std::atomic<bool> shouldStop;               // 스레드 종료 플래그
    
    // 통계
    std::atomic<int> totalSent;   // 총 전송 횟수
    std::atomic<int> totalFailed; // 총 실패 횟수
    
    /**
     * WinHTTP 세션 초기화
     * @return 성공하면 true
     */
    bool InitializeHttpSession();
    
    /**
     * WinHTTP 세션 정리
     */
    void CleanupHttpSession();
    
    /**
     * HeartbeatData를 JSON 문자열로 변환
     * @param heartbeat 변환할 heartbeat 데이터
     * @return JSON 문자열
     */
    std::string HeartbeatToJson(const HeartbeatData& heartbeat);
    
    /**
     * 문자열을 Base64로 인코딩 (Basic Authentication용)
     * @param input 인코딩할 문자열
     * @return Base64 인코딩된 문자열
     */
    std::string Base64Encode(const std::string& input);
    
    /**
     * JSON 문자열에서 특수문자 이스케이프 처리
     * @param str 처리할 문자열
     * @return 이스케이프 처리된 문자열
     */
    std::string EscapeJsonString(const std::string& str);
    
    /**
     * 현재 머신 이름 가져오기
     * @return 머신 이름
     */
    std::string GetMachineName();

    /**
     * Unix timestamp 생성 (현재 시간)
     * @return Unix timestamp (초 단위)
     */
    int64_t GetUnixTimestamp();
    
    /**
     * 실제 HTTP POST 요청 수행
     * @param jsonData 전송할 JSON 데이터
     * @param heartbeat User-Agent 구성을 위한 heartbeat (editor 식별용)
     * @return 성공하면 true
     */
    bool SendHttpRequest(const std::string& jsonData, const HeartbeatData& heartbeat);

    std::string HeartbeatsToJsonArray(const std::vector<HeartbeatData>& heartbeats);
    BulkSendResult SendBulkHttpRequest(const std::string& jsonData, const std::vector<HeartbeatData>& batch);
    BulkSendResult ParseBulkResponse(const std::string& responseBody);

    /**
     * heartbeat의 editor에 맞춰 WakaTime User-Agent 문자열을 구성한다.
     * 형식: creative-wakatime/{ver} (Windows) {editor} creative-wakatime/{ver}
     * @param heartbeat editor 정보를 담은 heartbeat
     * @return User-Agent 문자열
     */
    std::string BuildUserAgent(const HeartbeatData& heartbeat) const;
    
    /**
     * 백그라운드 전송 스레드 함수
     * 큐에서 heartbeat을 꺼내서 순차적으로 전송
     */
    void SenderThreadFunction();
    
    /**
     * API 키 파일에서 로드
     * @return 성공하면 true
     */
    bool LoadApiKeyFromFile();
    
    /**
     * API 키 파일에 저장
     * @param key 저장할 API 키
     * @return 성공하면 true
     */
    bool SaveApiKeyToFile(const std::string& key);

public:
    WakaTimeClient();
    ~WakaTimeClient();

    /**
     * 초기화 상태 확인
     * @return 초기화되었으면 true
     */
    bool IsInitialized() const { return initialized; }
    
    /**
     * WakaTime 클라이언트 초기화
     * @param providedApiKey WakaTime API 키 (선택사항, 파일에서 로드 가능)
     * @return 성공하면 true
     */
    bool Initialize(const std::string& providedApiKey = "");

    /**
     * 현재 설정으로 재초기화 시도
     * @param newApiKey 새로운 API Key
     * @return 성공하면 true
     */
    bool ReInitialize(const std::string& newApiKey);

    /**
     * API 키 반환 (마스킹된 버전)
     * @return 마스킹된 API 키 (예: "waka_1234****")
     */
    std::string GetMaskedApiKey() const;
    
    /**
     * Heartbeat 전송 (비동기). appId로 앱 정의를 조회해 language/editor를 채운다.
     * @param appId AppRegistry 정의 id
     * @param entity 파일 또는 프로젝트 경로
     * @param project 프로젝트 이름
     * @param editorVersion 에디터 버전 (없으면 빈 값)
     * @param isWrite 파일 수정 여부
     */
    bool SendHeartbeat(const std::string& appId, const std::string& entity, const std::string& project,
                       const std::string& editorVersion, bool isWrite = true);
    
    /**
     * FileChangeEvent에서 자동으로 Heartbeat 생성해서 전송
     * @param event 파일 변경 이벤트
     */
    bool SendHeartbeatFromEvent(const FileChangeEvent& event);

    /**
     * 외부에서 완성된 HeartbeatData를 전송 큐에 적재 (비동기).
     * 모든 heartbeat 소스(파일 감시/포커스 추적)가 공유하는 단일 진입점이며,
     * Pause 전역 게이트와 entity+project별 debounce를 여기서 적용한다.
     * @param heartbeat 적재할 heartbeat 데이터
     */
    bool EnqueueHeartbeat(const HeartbeatData& heartbeat);
    
    /**
     * 전송 큐에 대기 중인 heartbeat 수
     * @return 대기 중인 항목 수
     */
    size_t GetQueueSize() const;

    /**
     * 통계 정보 반환
     * @param sent 총 전송 횟수 (출력)
     * @param failed 총 실패 횟수 (출력)
     */
    void GetStats(int &sent, int &failed) const;

    /**
     * 모든 대기 중인 heartbeat 즉시 전송 (동기)
     * 프로그램 종료 시 사용
     */
    void FlushQueue();
};
