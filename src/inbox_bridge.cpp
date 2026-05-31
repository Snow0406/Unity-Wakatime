#include "inbox_bridge.h"
#include "wakatime_client.h"

namespace
{
    // JSON 문자열 이스케이프 해제 (\" \\ \/ \n \r \t \b \f \uXXXX 일부 처리)
    std::string UnescapeJson(const std::string &raw)
    {
        std::string out;
        out.reserve(raw.size());

        for (size_t i = 0; i < raw.size(); ++i)
        {
            const char c = raw[i];
            if (c == '\\' && i + 1 < raw.size())
            {
                const char next = raw[++i];
                switch (next)
                {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u':
                        // \uXXXX: BMP 범위만 단순 처리 (ASCII면 그대로, 아니면 UTF-8 인코딩)
                        if (i + 4 < raw.size())
                        {
                            const std::string hex = raw.substr(i + 1, 4);
                            i += 4;
                            try
                            {
                                const unsigned int cp = std::stoul(hex, nullptr, 16);
                                if (cp < 0x80)
                                {
                                    out += static_cast<char>(cp);
                                }
                                else if (cp < 0x800)
                                {
                                    out += static_cast<char>(0xC0 | (cp >> 6));
                                    out += static_cast<char>(0x80 | (cp & 0x3F));
                                }
                                else
                                {
                                    out += static_cast<char>(0xE0 | (cp >> 12));
                                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                                    out += static_cast<char>(0x80 | (cp & 0x3F));
                                }
                            }
                            catch (...)
                            {
                                // 무시
                            }
                        }
                        break;
                    default: out += next; break;
                }
            }
            else
            {
                out += c;
            }
        }

        return out;
    }

    // 고정 스키마용 경량 추출: "key" : "value" 또는 "key" : value(숫자/불리언) 형태.
    // 문자열 값이면 isString=true. 키를 못 찾으면 false.
    bool ExtractRawValue(const std::string &json, const std::string &key,
                         std::string &outValue, bool &outIsString)
    {
        const std::string needle = "\"" + key + "\"";
        size_t pos = json.find(needle);
        if (pos == std::string::npos) return false;

        pos += needle.size();

        // ':' 찾기
        pos = json.find(':', pos);
        if (pos == std::string::npos) return false;
        ++pos;

        // 공백 skip
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                     json[pos] == '\n' || json[pos] == '\r'))
        {
            ++pos;
        }
        if (pos >= json.size()) return false;

        if (json[pos] == '"')
        {
            // 문자열 값: 닫는 따옴표(이스케이프 안 된)까지
            ++pos;
            const size_t start = pos;
            std::string raw;
            while (pos < json.size())
            {
                if (json[pos] == '\\' && pos + 1 < json.size())
                {
                    raw += json[pos];
                    raw += json[pos + 1];
                    pos += 2;
                    continue;
                }
                if (json[pos] == '"') break;
                raw += json[pos];
                ++pos;
            }
            (void)start;
            outValue = UnescapeJson(raw);
            outIsString = true;
            return true;
        }

        // 숫자/불리언/null: 구분자(, } 공백)까지
        const size_t start = pos;
        while (pos < json.size() && json[pos] != ',' && json[pos] != '}' &&
               json[pos] != ' ' && json[pos] != '\t' && json[pos] != '\n' && json[pos] != '\r')
        {
            ++pos;
        }
        outValue = json.substr(start, pos - start);
        outIsString = false;
        return true;
    }

    std::string GetString(const std::string &json, const std::string &key)
    {
        std::string value;
        bool isString = false;
        if (ExtractRawValue(json, key, value, isString))
        {
            return value;
        }
        return "";
    }

    // 파일 전체를 문자열로 읽기. 실패(열기 실패/빈 파일)면 false.
    bool ReadWholeFile(const std::string &path, std::string &out)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) return false;

        std::ostringstream ss;
        ss << file.rdbuf();
        out = ss.str();

        return !out.empty();
    }

    bool IsProbablyCompleteJson(const std::string &content)
    {
        const auto first = content.find_first_not_of(" \t\r\n");
        if (first == std::string::npos || content[first] != '{') return false;

        const auto last = content.find_last_not_of(" \t\r\n");
        return last != std::string::npos && content[last] == '}';
    }
}

namespace InboxBridge
{
    void ProcessFile(const std::string &jsonPath, WakaTimeClient *client,
                     const std::function<void(const HeartbeatData&)> &onHeartbeat)
    {
        if (client == nullptr) return;

        std::string content;
        if (!ReadWholeFile(jsonPath, content))
        {
            // 아직 안 풀린 lock이거나 빈 파일 → 삭제하지 않고 다음 기회에 재시도.
            WT_LOG("[InboxBridge] Skip (unreadable/empty): " << jsonPath);
            return;
        }

        if (!IsProbablyCompleteJson(content))
        {
            // Aseprite Lua는 .json에 직접 쓰므로 파일 쓰기 중 MODIFIED 이벤트가 먼저 올 수 있다.
            // 이 경우 삭제하지 않고 다음 MODIFIED/시작 스캔에서 재시도한다.
            WT_LOG("[InboxBridge] Skip (incomplete json): " << jsonPath);
            return;
        }

        // 필수 키 추출
        const std::string entity = GetString(content, "entity");
        if (entity.empty())
        {
            // 스키마 위반 → 무한 재시도 방지 위해 삭제
            WT_ERR("[InboxBridge] Invalid event (no entity), deleting: " << jsonPath);
            std::error_code ec;
            fs::remove(jsonPath, ec);
            return;
        }

        HeartbeatData hb;
        hb.entity = entity;
        hb.project = GetString(content, "project");
        hb.language = GetString(content, "language");
        hb.editor = GetString(content, "editor");
        if (hb.editor.empty()) hb.editor = "Aseprite";
        if (hb.language.empty()) hb.language = hb.editor;

        // is_write (불리언)
        std::string isWriteRaw;
        bool isStr = false;
        hb.is_write = false;
        if (ExtractRawValue(content, "is_write", isWriteRaw, isStr))
        {
            hb.is_write = (isWriteRaw == "true" || isWriteRaw == "1");
        }

        // time (Unix timestamp). 없거나 0이면 현재 시각.
        std::string timeRaw;
        hb.time = 0;
        if (ExtractRawValue(content, "time", timeRaw, isStr) && !timeRaw.empty())
        {
            try
            {
                hb.time = static_cast<int64_t>(std::stoll(timeRaw));
            }
            catch (...)
            {
                hb.time = 0;
            }
        }
        if (hb.time <= 0)
        {
            const auto now = std::chrono::system_clock::now().time_since_epoch();
            hb.time = std::chrono::duration_cast<std::chrono::seconds>(now).count();
        }

        WT_LOG("[InboxBridge] Heartbeat: " << hb.entity << " (" << hb.project
                << ", " << hb.editor << ", write=" << (hb.is_write ? "1" : "0") << ")");

        client->EnqueueHeartbeat(hb);
        if (onHeartbeat)
        {
            onHeartbeat(hb);
        }

        // 처리 성공 → 파일 삭제
        std::error_code ec;
        fs::remove(jsonPath, ec);
        if (ec)
        {
            WT_ERR("[InboxBridge] Failed to delete consumed event: " << jsonPath);
        }
    }

    int InitialScan(const std::string &eventsDir, WakaTimeClient *client,
                    const std::function<void(const HeartbeatData&)> &onHeartbeat)
    {
        if (client == nullptr || eventsDir.empty()) return 0;

        std::error_code ec;
        if (!fs::exists(eventsDir, ec)) return 0;

        int processed = 0;
        for (const auto &entry: fs::directory_iterator(eventsDir, ec))
        {
            if (ec) break;
            if (!entry.is_regular_file(ec)) continue;

            const auto &path = entry.path();
            std::string ext = path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](const unsigned char c) { return static_cast<char>(::tolower(c)); });
            if (ext != ".json") continue;

            std::string full = path.string();
            std::replace(full.begin(), full.end(), '\\', '/');
            ProcessFile(full, client, onHeartbeat);
            ++processed;
        }

        if (processed > 0)
        {
            WT_LOG("[InboxBridge] Initial scan processed " << processed << " event(s)");
        }

        return processed;
    }
}
