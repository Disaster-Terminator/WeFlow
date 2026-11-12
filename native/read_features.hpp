#pragma once

#include <queue>
#include <set>
#include "account.hpp"

namespace wf {
inline std::vector<std::string> sessionIds(const char* encoded) {
    const auto ids = Json::parse(encoded).get<std::vector<std::string>>();
    std::vector<std::string> result;
    std::set<std::string> seen;
    for (const auto& id : ids) if (!id.empty() && seen.insert(id).second) result.push_back(id);
    return result;
}

inline Json availableYears(const Account& account, const std::vector<std::string>& sessions) {
    std::set<int, std::greater<int>> years;
    for (const auto& id : sessions) {
        for (const auto& row : account.dailyCounts(id))
            years.insert(std::stoi(row.at("day").get<std::string>().substr(0, 4)));
    }
    return years;
}

inline void increment(Json& object, const std::string& key, int64_t count) {
    object[key] = object.value(key, int64_t{0}) + count;
}

inline Json annualStats(const Account& account, const std::vector<std::string>& sessions, int begin, int end) {
    Json daily = Json::object(), bySession = Json::object();
    int64_t total = 0;
    for (const auto& id : sessions) {
        const auto rows = account.dailyCounts(id, begin, end);
        if (rows.empty()) continue;
        Json monthly = Json::object();
        int64_t sent = 0, received = 0;
        for (const auto& row : rows) {
            const auto day = row.at("day").get<std::string>();
            const auto count = row.at("total").get<int64_t>();
            increment(daily, day, count);
            increment(monthly, std::to_string(std::stoi(day.substr(5, 2))), count);
            (row.at("sent").get<int>() ? sent : received) += count;
        }
        total += sent + received;
        bySession[id] = {{"sent", sent}, {"received", received}, {"monthly", monthly}};
    }
    return {{"total", total}, {"sessions", bySession}, {"daily", daily}};
}

inline Json mediaPage(const Account& account, const std::vector<std::string>& sessions, int type,
    int begin, int end, int limit, int offset, int& more) {
    struct Source {
        MessageCursor cursor;
        Json row;
        std::string session;
    };
    std::vector<Source> sources;
    const auto order = [&sources](size_t index) {
        return std::tuple_cat(messageOrder(sources[index].row), std::tuple(index));
    };
    const auto earlier = [&order](size_t left, size_t right) { return order(left) < order(right); };
    std::priority_queue<size_t, std::vector<size_t>, decltype(earlier)> queue(earlier);
    for (const auto& id : sessions) {
        auto cursor = account.cursor(id, false, begin, end, type);
        if (!cursor.hasMore()) continue;
        auto row = cursor.next();
        sources.push_back({std::move(cursor), std::move(row), id});
        queue.push(sources.size() - 1);
    }
    Json page = Json::array();
    while (!queue.empty() && page.size() < static_cast<size_t>(limit)) {
        const auto index = queue.top();
        queue.pop();
        auto& source = sources[index];
        if (offset > 0) {
            --offset;
        } else {
            source.row["session_id"] = source.session;
            page.push_back(std::move(source.row));
        }
        if (source.cursor.hasMore()) {
            source.row = source.cursor.next();
            queue.push(index);
        }
    }
    more = !queue.empty();
    return page;
}

inline Json snsTimeline(Account& account, const std::string& filters) {
    // The timeline's timestamp is serialized inside the XML payload.
    return account.moments().query(R"sql(
        WITH options AS (SELECT ? AS value),
        posts AS (
          SELECT tid, user_name, CAST(content AS TEXT) AS xml
          FROM SnsTimeLine, options
          WHERE json_array_length(value,'$.users') = 0
             OR user_name IN (SELECT value FROM json_each(options.value,'$.users'))
        ),
        fields AS (
          SELECT tid, user_name, xml,
            CASE WHEN instr(xml,'<createTime>') > 0
                 THEN CAST(substr(xml,instr(xml,'<createTime>') + 12) AS INTEGER)
                 ELSE 0 END AS timestamp,
            CASE WHEN instr(xml,'<contentDesc>') > 0 AND instr(xml,'</contentDesc>') > 0
                 THEN substr(xml,instr(xml,'<contentDesc>') + 13,
                   instr(xml,'</contentDesc>') - instr(xml,'<contentDesc>') - 13)
                 ELSE '' END AS description
          FROM posts
        )
        SELECT CAST(tid AS TEXT) AS id, user_name AS username, xml AS rawXml,
               timestamp AS createTime, description AS contentDesc
        FROM fields, options
        WHERE (json_extract(value,'$.begin') = 0 OR timestamp >= json_extract(value,'$.begin'))
          AND (json_extract(value,'$.end') = 0 OR timestamp <= json_extract(value,'$.end'))
          AND (json_extract(value,'$.keyword') = '' OR instr(lower(description),lower(json_extract(value,'$.keyword'))) > 0)
        ORDER BY timestamp DESC, tid DESC
        LIMIT (SELECT json_extract(value,'$.limit') FROM options)
        OFFSET (SELECT json_extract(value,'$.offset') FROM options)
    )sql", &filters);
}
}
