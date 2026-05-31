#pragma once

#include "globals.h"

class WakaTimeClient;
struct HeartbeatData;

/**
 * 외부(Aseprite 등) 이벤트 inbox(.json)를 소비해 WakaTimeClient로 heartbeat를 보낸다.
 *
 * Lua extension이 `%APPDATA%/creative-wakatime/events/`에 `.json` 이벤트 파일을 쓰면
 * FileWatcher의 inbox 콜백 또는 시작 시 스캔으로 받아 파싱 → EnqueueHeartbeat → 파일 삭제한다.
 *
 * 외부 라이브러리 없이 고정 스키마 키만 추출하는 경량 파서를 사용한다(직렬화도 수동).
 */
namespace InboxBridge
{
    /**
     * inbox에서 감지된 단일 .json 파일을 처리한다.
     * 성공/파싱 실패 모두 파일을 삭제해 무한 재시도를 차단한다.
     * (읽기 자체 실패 = 아직 안 풀린 lock 등은 남겨두고 다음 기회에 재시도)
     * @param jsonPath 이벤트 파일 전체 경로
     * @param client heartbeat를 적재할 클라이언트
     * @param onHeartbeat 처리 성공 시 생성된 heartbeat를 전달받는 선택 콜백
     */
    void ProcessFile(const std::string& jsonPath, WakaTimeClient* client,
                     const std::function<void(const HeartbeatData&)>& onHeartbeat = {});

    /**
     * 앱 시작 시 events/ 폴더에 쌓인 잔여 .json을 1회 스캔·소비한다.
     * ReadDirectoryChangesW는 기존 파일에 이벤트를 주지 않으므로 필수.
     * @param eventsDir 이벤트 폴더 경로
     * @param client heartbeat를 적재할 클라이언트
     * @param onHeartbeat 처리 성공 시 생성된 heartbeat를 전달받는 선택 콜백
     * @return 처리한 파일 수
     */
    int InitialScan(const std::string& eventsDir, WakaTimeClient* client,
                    const std::function<void(const HeartbeatData&)>& onHeartbeat = {});
}
