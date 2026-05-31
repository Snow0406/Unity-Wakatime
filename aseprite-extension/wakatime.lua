-- Creative WakaTime - Aseprite extension
--
-- Aseprite의 편집/저장 활동을 감지해, WakaTime API로 직접 보내지 않고
-- 로컬 이벤트 파일(%APPDATA%/creative-wakatime/events/*.json)을 생성한다.
-- Creative WakaTime 트레이 앱(C++)이 그 폴더를 감시해 heartbeat로 변환한다.
--
-- 이벤트:
--   sitechange   -> 활성 sprite 전환 등 편집 활동 (is_write = false)
--   aftercommand -> SaveFile / SaveFileAs / SaveFileCopyAs 이후 (is_write = true)
--
-- Aseprite Lua sandbox에서는 rename이 제공되지 않으므로 .json에 직접 쓴다.
-- C++ 소비자는 빈/미완성 JSON을 삭제하지 않고 다음 이벤트나 시작 스캔에서 재시도한다.

local EDIT_DEBOUNCE_SECONDS = 120 -- 동일 파일 편집 heartbeat 최소 간격(2분)

-- 저장 명령으로 간주할 command 이름 집합
local SAVE_COMMANDS = {
    SaveFile = true,
    SaveFileAs = true,
    SaveFileCopyAs = true,
}

local lastSentByEntity = {}   -- entity -> 마지막 전송 os.time()
local writeCounter = 0        -- 같은 초 내 파일명 충돌 방지
local listenerKeys = {}       -- 해제할 리스너 키 보관

-- JSON 문자열 값 이스케이프
local function jsonEscape(s)
    s = s:gsub('\\', '\\\\')
    s = s:gsub('"', '\\"')
    s = s:gsub('\n', '\\n')
    s = s:gsub('\r', '\\r')
    s = s:gsub('\t', '\\t')
    return s
end

-- 이벤트 폴더 경로 반환 (없으면 생성). 실패 시 nil.
local function getEventsDir()
    local appData = os.getenv("APPDATA")
    if not appData or appData == "" then
        return nil
    end

    local dir = app.fs.joinPath(appData, "creative-wakatime", "events")
    if not app.fs.isDirectory(dir) then
        app.fs.makeAllDirectories(dir)
    end
    if not app.fs.isDirectory(dir) then
        return nil
    end

    return dir
end

-- HeartbeatData 한 건을 이벤트 파일로 기록
local function writeEvent(entity, project, isWrite)
    local dir = getEventsDir()
    if not dir then
        return
    end

    -- 경로를 forward slash로 정규화 (대시보드 표기 일관성)
    local normEntity = entity:gsub('\\', '/')

    writeCounter = writeCounter + 1
    local fname = "aseprite-" .. tostring(os.time()) .. "-" .. tostring(writeCounter) .. ".json"
    local finalPath = app.fs.joinPath(dir, fname)

    local json = "{"
        .. '"source":"aseprite",'
        .. '"entity":"' .. jsonEscape(normEntity) .. '",'
        .. '"project":"' .. jsonEscape(project) .. '",'
        .. '"language":"Aseprite",'
        .. '"editor":"Aseprite",'
        .. '"is_write":' .. (isWrite and "true" or "false") .. ','
        .. '"time":' .. tostring(os.time())
        .. "}"

    local f = io.open(finalPath, "w")
    if not f then
        return
    end
    f:write(json)
    f:close()
end

-- 현재 활성 sprite의 파일 경로/프로젝트명을 구해 이벤트 기록
local function emit(isWrite)
    local sprite = app.sprite or app.activeSprite
    if not sprite then
        return -- 열린 sprite 없음
    end

    local entity = sprite.filename
    if not entity or entity == "" then
        return -- 미저장 새 파일 (filename 없음)
    end

    -- project = 파일의 부모 폴더 basename
    local parentPath = app.fs.filePath(entity)
    local project = app.fs.fileName(parentPath)
    if not project or project == "" then
        project = "Aseprite"
    end

    local now = os.time()

    if not isWrite then
        -- 편집 활동: entity별 2분 debounce (C++ 측 debounce와 이중 안전)
        local last = lastSentByEntity[entity]
        if last and (now - last) < EDIT_DEBOUNCE_SECONDS then
            return
        end
    end

    lastSentByEntity[entity] = now
    writeEvent(entity, project, isWrite)
end

local function onActivity()
    emit(false)
end

local function onAfterCommand(ev)
    if ev and ev.name and SAVE_COMMANDS[ev.name] then
        emit(true)
    end
end

function init(plugin)
    -- 활성 sprite/layer/frame 전환 = 편집 활동
    listenerKeys[#listenerKeys + 1] = app.events:on('sitechange', onActivity)
    -- 저장 명령 이후
    listenerKeys[#listenerKeys + 1] = app.events:on('aftercommand', onAfterCommand)
end

function exit(plugin)
    for _, key in ipairs(listenerKeys) do
        app.events:off(key)
    end
    listenerKeys = {}
end
