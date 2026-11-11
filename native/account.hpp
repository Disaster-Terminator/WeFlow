#pragma once

#include <algorithm>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <bcrypt.h>
#include "json.hpp"
#include "sqlite3mc_amalgamation.h"

namespace wf {
namespace fs = std::filesystem;
using Json = nlohmann::json;

inline fs::path path(const std::string& utf8) { return fs::u8path(utf8); }

inline std::string hex(const unsigned char* bytes, size_t length) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(length * 2);
    for (size_t i = 0; i < length; ++i) {
        result.push_back(digits[bytes[i] >> 4]);
        result.push_back(digits[bytes[i] & 15]);
    }
    return result;
}

inline std::string sessionTable(const std::string& session) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_MD5_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Cannot initialize MD5");
    unsigned char digest[16];
    const auto status = BCryptHash(algorithm, nullptr, 0,
        reinterpret_cast<PUCHAR>(const_cast<char*>(session.data())),
        static_cast<ULONG>(session.size()), digest, sizeof(digest));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) throw std::runtime_error("Cannot calculate message table name");
    return "Msg_" + hex(digest, sizeof(digest));
}

class Query {
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt{nullptr, sqlite3_finalize};

public:
    Query(sqlite3* db, const std::string& sql, const std::string* parameter) {
        sqlite3_stmt* prepared = nullptr;
        const int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &prepared, nullptr);
        stmt.reset(prepared);
        if (rc != SQLITE_OK || !stmt || !sqlite3_stmt_readonly(stmt.get()))
            throw std::runtime_error("Cannot prepare read-only database query");
        if (parameter && sqlite3_bind_text(stmt.get(), 1, parameter->data(),
                static_cast<int>(parameter->size()), SQLITE_TRANSIENT) != SQLITE_OK)
            throw std::runtime_error("Cannot bind database query parameter");
    }

    Json next() {
        const auto step = sqlite3_step(stmt.get());
        if (step == SQLITE_DONE) {
            stmt.reset();
            return nullptr;
        }
        if (step != SQLITE_ROW) throw std::runtime_error("Cannot read database rows");
        Json row = Json::object();
        for (int i = 0; i < sqlite3_column_count(stmt.get()); ++i) {
            const std::string name = sqlite3_column_name(stmt.get(), i);
            switch (sqlite3_column_type(stmt.get(), i)) {
            case SQLITE_INTEGER:
                if (name == "server_id") row[name] = std::to_string(sqlite3_column_int64(stmt.get(), i));
                else row[name] = sqlite3_column_int64(stmt.get(), i);
                break;
            case SQLITE_FLOAT: row[name] = sqlite3_column_double(stmt.get(), i); break;
            case SQLITE_TEXT: {
                const auto data = sqlite3_column_text(stmt.get(), i);
                row[name] = std::string(reinterpret_cast<const char*>(data), sqlite3_column_bytes(stmt.get(), i));
                break;
            }
            case SQLITE_BLOB: {
                const auto data = static_cast<const unsigned char*>(sqlite3_column_blob(stmt.get(), i));
                row[name] = hex(data, sqlite3_column_bytes(stmt.get(), i));
                break;
            }
            default: row[name] = nullptr;
            }
        }
        return row;
    }
};

class Database {
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db{nullptr, sqlite3_close};

public:
    Database(const fs::path& file, const std::string& key) {
        sqlite3* opened = nullptr;
        const auto result = sqlite3_open_v2(file.u8string().c_str(), &opened,
            SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
        db.reset(opened);
        if (result != SQLITE_OK) throw std::runtime_error("Cannot open database");
        sqlite3_db_config(db.get(), SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr);
        if (sqlite3_exec(db.get(), "SELECT count(*) FROM sqlite_schema", nullptr, nullptr, nullptr) == SQLITE_OK)
            return;

        if (key.size() != 64 || key.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
            throw std::runtime_error("Encrypted database requires a 32-byte hexadecimal key");
        const std::string rawKey = "x'" + key + "'";
        if (sqlite3_exec(db.get(), "PRAGMA cipher=sqlcipher; PRAGMA legacy=4", nullptr, nullptr, nullptr) != SQLITE_OK ||
            sqlite3_key(db.get(), rawKey.data(), static_cast<int>(rawKey.size())) != SQLITE_OK ||
            sqlite3_exec(db.get(), "SELECT count(*) FROM sqlite_schema", nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error("Cannot read SQLCipher database with supplied key");
    }

    Query prepare(const std::string& sql, const std::string* parameter = nullptr) const {
        return Query(db.get(), sql, parameter);
    }

    Json query(const std::string& sql, const std::string* parameter = nullptr) const {
        auto query = prepare(sql, parameter);
        Json rows = Json::array();
        for (auto row = query.next(); !row.is_null(); row = query.next()) rows.push_back(std::move(row));
        return rows;
    }

    bool hasTable(const std::string& name) const {
        return !query("SELECT name FROM sqlite_schema WHERE type='table' AND name=? COLLATE NOCASE", &name).empty();
    }
};

class MessageCursor {
    struct Source {
        Query query;
        Json row;
    };
    std::vector<Source> sources;
    std::string accountId;
    bool ascending;

    static auto order(const Json& row) {
        const auto number = [&row](const char* field) {
            const auto value = row.find(field);
            return value != row.end() && value->is_number_integer() ? value->get<int64_t>() : int64_t{0};
        };
        return std::tuple(number("create_time"), number("sort_seq"), number("local_id"));
    }

public:
    MessageCursor(std::string account, bool forward) : accountId(std::move(account)), ascending(forward) {}

    void add(Query query) {
        auto row = query.next();
        if (!row.is_null()) sources.push_back(Source{std::move(query), std::move(row)});
    }

    bool hasMore() const { return !sources.empty(); }

    Json next() {
        if (!hasMore()) return nullptr;
        const auto selected = std::min_element(sources.begin(), sources.end(), [&](const Source& left, const Source& right) {
            return ascending ? order(left.row) < order(right.row) : order(right.row) < order(left.row);
        });
        auto row = std::move(selected->row);
        selected->row = selected->query.next();
        if (selected->row.is_null()) sources.erase(selected);
        if (row.contains("sender_username") && row["sender_username"].is_string())
            row["computed_is_send"] = !accountId.empty() && row["sender_username"] == accountId ? 1 : 0;
        return row;
    }
};

class Account {
    Database sessionDb;
    std::vector<Database> shards;

public:
    std::string accountId;

    Account(const fs::path& sessionFile, const std::string& key)
        : sessionDb(sessionFile, key) {
        if (!sessionDb.hasTable("SessionTable")) throw std::runtime_error("SessionTable is missing");
        const auto sessionDir = sessionFile.parent_path();
        const auto storage = sessionDir.filename() == "session" ? sessionDir.parent_path() : sessionDir;
        const auto messageDir = storage / "message";
        if (!fs::is_directory(messageDir)) return;
        std::vector<fs::path> files;
        for (const auto& entry : fs::directory_iterator(messageDir)) {
            const auto stem = entry.path().stem().u8string();
            if (entry.is_regular_file() && entry.path().extension() == ".db" &&
                stem.rfind("message_", 0) == 0 && stem.size() > 8 &&
                stem.find_first_not_of("0123456789", 8) == std::string::npos)
                files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());
        for (const auto& file : files) shards.emplace_back(file, key);
    }

    Json sessions() const { return sessionDb.query("SELECT * FROM SessionTable"); }

    MessageCursor cursor(const std::string& session, bool ascending, int begin = 0, int end = 0) const {
        if (shards.empty()) throw std::runtime_error("No message database found");
        const auto table = sessionTable(session);
        MessageCursor result(accountId, ascending);
        for (const auto& shard : shards) {
            if (!shard.hasTable(table)) continue;
            const bool senderMap = shard.hasTable("Name2Id");
            const std::string from = " FROM " + table + " AS m";
            std::string sql = senderMap
                ? "SELECT m.*, n.user_name AS sender_username" + from + " LEFT JOIN Name2Id AS n ON m.real_sender_id=n.rowid"
                : "SELECT m.*" + from;
            sql += " WHERE 1=1";
            if (begin > 0) sql += " AND m.create_time >= " + std::to_string(begin);
            if (end > 0) sql += " AND m.create_time <= " + std::to_string(end);
            const std::string direction = ascending ? " ASC" : " DESC";
            sql += " ORDER BY m.create_time" + direction + ", m.sort_seq" + direction + ", m.local_id" + direction;
            result.add(shard.prepare(sql));
        }
        return result;
    }

    int64_t count(const std::string& session) const {
        if (shards.empty()) throw std::runtime_error("No message database found");
        const auto table = sessionTable(session);
        int64_t result = 0;
        for (const auto& shard : shards) {
            if (shard.hasTable(table))
                result += shard.prepare("SELECT count(*) AS total FROM " + table).next().at("total").get<int64_t>();
        }
        return result;
    }
};
}
