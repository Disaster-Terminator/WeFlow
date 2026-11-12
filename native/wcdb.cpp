#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include "account.hpp"
#include "read_features.hpp"

using namespace wf;

namespace {
struct Cursor {
    MessageCursor rows;
    size_t batchSize;
};

struct Connection {
    Account account;
    std::map<int64_t, Cursor> cursors;
};

std::mutex mutex;
std::map<int64_t, Connection> accounts;
int64_t nextHandle = 1;
size_t clients = 0;

template <class Action>
int call(Action action, int errorCode = -1) noexcept {
    try {
        std::lock_guard lock(mutex);
        action();
        return 0;
    } catch (...) {
        return errorCode;
    }
}

void writeJson(const Json& value, char** output) {
    const auto text = value.dump(-1, ' ', false, Json::error_handler_t::replace);
    auto* memory = static_cast<char*>(std::malloc(text.size() + 1));
    if (!memory) throw std::bad_alloc();
    std::memcpy(memory, text.c_str(), text.size() + 1);
    *output = memory;
}

Json page(MessageCursor& cursor, size_t count) {
    Json rows = Json::array();
    while (cursor.hasMore() && rows.size() < count) rows.push_back(cursor.next());
    return rows;
}
}

#define WCDB extern "C" __declspec(dllexport)
WCDB int wcdb_init() { return call([] { ++clients; }); }
WCDB int wcdb_shutdown() {
    return call([] {
        if (clients > 0 && --clients == 0) accounts.clear();
    });
}
WCDB void wcdb_free_string(void* ptr) { std::free(ptr); }

WCDB int wcdb_open_account(const char* file, const char* key, int64_t* handle) {
    if (!file || !key || !handle) return -1;
    return call([&] {
        accounts.emplace(nextHandle, Connection{Account(path(file), key), {}});
        *handle = nextHandle++;
    }, -4);
}

WCDB int wcdb_close_account(int64_t handle) {
    return call([&] { accounts.erase(handle); });
}

WCDB int wcdb_set_my_account_id(int64_t handle, const char* accountId) {
    if (!accountId) return -1;
    return call([&] { accounts.at(handle).account.accountId = accountId; });
}

WCDB int wcdb_get_sessions(int64_t handle, char** output) {
    if (!output) return -1;
    return call([&] { writeJson(accounts.at(handle).account.sessions(), output); });
}

WCDB int wcdb_get_messages(int64_t handle, const char* session, int limit, int offset, char** output) {
    if (!session || !output || limit < 0 || offset < 0) return -1;
    return call([&] {
        auto cursor = accounts.at(handle).account.cursor(session, false);
        for (int i = 0; i < offset && cursor.hasMore(); ++i) cursor.next();
        writeJson(page(cursor, limit), output);
    });
}

WCDB int wcdb_get_message_count(int64_t handle, const char* session, int* count) {
    if (!session || !count) return -1;
    return call([&] {
        const auto size = accounts.at(handle).account.count(session);
        if (size > std::numeric_limits<int>::max()) throw std::overflow_error("Message count");
        *count = static_cast<int>(size);
    });
}

WCDB int wcdb_open_message_cursor(int64_t handle, const char* session, int batchSize,
    int ascending, int begin, int end, int64_t* cursor) {
    if (!session || !cursor || batchSize <= 0) return -1;
    return call([&] {
        auto& connection = accounts.at(handle);
        connection.cursors.emplace(nextHandle, Cursor{
            connection.account.cursor(session, ascending != 0, begin, end), static_cast<size_t>(batchSize)
        });
        *cursor = nextHandle++;
    });
}

WCDB int wcdb_fetch_message_batch(int64_t handle, int64_t cursor, char** output, int* more) {
    if (!output || !more) return -1;
    return call([&] {
        auto& state = accounts.at(handle).cursors.at(cursor);
        const auto result = page(state.rows, state.batchSize);
        writeJson(result, output);
        *more = state.rows.hasMore();
    });
}

WCDB int wcdb_close_message_cursor(int64_t handle, int64_t cursor) {
    return call([&] { accounts.at(handle).cursors.erase(cursor); });
}

WCDB int wcdb_get_available_years(int64_t handle, const char* ids, char** output) {
    if (!ids || !output) return -1;
    return call([&] { writeJson(availableYears(accounts.at(handle).account, sessionIds(ids)), output); });
}

WCDB int wcdb_get_annual_report_stats(int64_t handle, const char* ids, int begin, int end, char** output) {
    if (!ids || !output) return -1;
    return call([&] { writeJson(annualStats(accounts.at(handle).account, sessionIds(ids), begin, end), output); });
}

WCDB int wcdb_scan_media_stream(int64_t handle, const char* ids, int type, int begin, int end,
    int limit, int offset, char** output, int* more) {
    if (!ids || !output || !more || type < 0 || type > 2 || limit <= 0 || offset < 0) return -1;
    return call([&] {
        int hasMore;
        auto rows = mediaPage(accounts.at(handle).account, sessionIds(ids), type, begin, end, limit, offset, hasMore);
        writeJson(rows, output);
        *more = hasMore;
    });
}

WCDB int wcdb_get_sns_timeline(int64_t handle, int limit, int offset, const char* users, const char* keyword,
    int begin, int end, char** output) {
    if (!users || !keyword || !output || limit <= 0 || offset < 0) return -1;
    return call([&] {
        const Json filters = {{"users", *users ? Json(sessionIds(users)) : Json::array()},
            {"keyword", keyword}, {"begin", begin}, {"end", end}, {"limit", limit}, {"offset", offset}};
        writeJson(snsTimeline(accounts.at(handle).account, filters.dump()), output);
    });
}

WCDB int wcdb_get_sns_usernames(int64_t handle, char** output) {
    if (!output) return -1;
    return call([&] {
        Json names = Json::array();
        for (const auto& row : accounts.at(handle).account.moments().query(
            "SELECT DISTINCT user_name FROM SnsTimeLine WHERE user_name <> '' ORDER BY user_name")) {
            names.push_back(row.at("user_name"));
        }
        writeJson(names, output);
    });
}

WCDB int wcdb_get_sns_export_stats(int64_t handle, const char* accountId, char** output) {
    if (!output) return -1;
    return call([&] {
        const std::string user = accountId ? accountId : "";
        auto query = accounts.at(handle).account.moments().prepare(
            "SELECT count(*) AS total_posts, count(DISTINCT NULLIF(user_name,'')) AS total_friends, "
            "CASE WHEN ?1 = '' THEN NULL ELSE count(CASE WHEN user_name = ?1 THEN 1 END) END AS my_posts "
            "FROM SnsTimeLine", &user);
        writeJson(query.next(), output);
    });
}
