#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cctype>
#include <fstream>
#include <iostream>
#include <regex>
#include "account.hpp"
#include "media.hpp"

using namespace wf;

namespace {
void event(const Json& value) {
    std::cout << value.dump(-1, ' ', false, Json::error_handler_t::replace) << std::endl;
}

std::string getText(const Json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

std::string lower(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string packedMediaName(const Json& row, const std::string& type) {
    auto packed = lower(getText(row, "packed_info_data"));
    if (packed.empty() || packed.size() % 2 || packed.find_first_not_of("0123456789abcdef") != std::string::npos)
        return {};
    const auto nibble = [](char digit) { return digit <= '9' ? digit - '0' : digit - 'a' + 10; };
    std::string text;
    text.reserve(packed.size() / 2);
    for (size_t i = 0; i < packed.size(); i += 2) {
        const auto byte = nibble(packed[i]) * 16 + nibble(packed[i + 1]);
        text.push_back(byte >= 0x20 && byte <= 0x7e ? static_cast<char>(byte) : ' ');
    }
    static const std::regex image(R"re(([a-fA-F0-9]{8,})(?:\.t)?\.dat)re", std::regex::icase);
    static const std::regex video(R"re(([a-fA-F0-9]{8,})\.mp4)re", std::regex::icase);
    static const std::regex token(R"re(([a-fA-F0-9]{32})(?![a-fA-F0-9]))re");
    std::smatch match;
    if (std::regex_search(text, match, type == "image" ? image : video) || std::regex_search(text, match, token))
        return match[1];
    return {};
}

class MediaExport {
    fs::path accountDir;
    std::vector<std::string> types;
    unsigned char xorKey = 0;
    std::string aesKey;
    std::map<std::string, std::vector<fs::path>> files;

    void index(const fs::path& root) {
        if (!fs::is_directory(root)) return;
        for (const auto& item : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied)) {
            if (fs::is_symlink(item.symlink_status()) || !item.is_regular_file()) continue;
            files[lower(item.path().filename().u8string())].push_back(item.path());
        }
    }

public:
    MediaExport(const Json& request, const fs::path& sessionFile) :
        types(request.value("mediaTypes", std::vector<std::string>{})) {
        if (types.empty()) return;
        const auto& account = request.at("account");
        const auto configuredDir = getText(account, "accountDir");
        const auto sessionDir = sessionFile.parent_path();
        const auto storage = sessionDir.filename() == "session" ? sessionDir.parent_path() : sessionDir;
        accountDir = configuredDir.empty() ? storage.parent_path() : path(configuredDir);
        aesKey = getText(account, "imageAesKey");
        const auto key = account.value("imageXorKey", Json(0));
        int numericKey = 0;
        if (key.is_string()) {
            const auto text = key.get<std::string>();
            const bool hasHexPrefix = text.rfind("0x", 0) == 0 || text.rfind("0X", 0) == 0;
            size_t read = 0;
            numericKey = std::stoi(text, &read, hasHexPrefix ? 16 : 10);
            if (read != text.size()) throw std::runtime_error("Invalid image XOR key");
        } else {
            numericKey = key.get<int>();
        }
        if (numericKey < 0 || numericKey > 255) throw std::runtime_error("Invalid image XOR key");
        xorKey = static_cast<unsigned char>(numericKey);
        index(accountDir / "msg");
        index(accountDir / "media");
        const auto emojiDir = getText(request, "emojiCacheDir");
        if (!emojiDir.empty()) index(path(emojiDir));
    }

    void write(Json& row, const fs::path& directory, Json& failures, size_t rowIndex, const std::string& session) {
        const auto localType = row.value("local_type", int64_t{1});
        const auto content = getText(row, "message_content");
        std::string type, counter;
        switch (localType) {
        case 3: type = "image"; counter = "images_failed"; break;
        case 34: type = "voice"; counter = "voices_failed"; break;
        case 43: type = "video"; counter = "videos_failed"; break;
        case 47: type = "emoji"; counter = "emojis_failed"; break;
        case 49:
            if (content.find("<type>6</type>") == std::string::npos) return;
            [[fallthrough]];
        case 25769803825LL: type = "file"; counter = "files_failed"; break;
        default: return;
        }
        if (std::find(types.begin(), types.end(), type) == types.end()) return;
        row["media_type"] = type;
        try {
            auto name = getText(row, "media_path");
            if (name.empty() && type == "image") name = getText(row, "image_dat_name");
            if (name.empty() && type == "video") name = getText(row, "video_md5");
            if (name.empty() && (type == "image" || type == "video")) name = packedMediaName(row, type);
            if (name.empty()) {
                static const std::regex attribute(R"re(\b(?:rawmd5|md5)="([0-9a-fA-F]{32})")re", std::regex::icase);
                static const std::regex title(R"re(<title>([^<]+)</title>)re");
                std::smatch match;
                if (std::regex_search(content, match, type == "file" ? title : attribute)) name = match[1];
            }
            if (name.empty()) throw std::runtime_error("Message has no local media reference");
            name = lower(path(name).filename().u8string());
            auto found = files.find(name);
            if (found == files.end()) {
                for (const char* suffix : {".dat", "_hd.dat", ".t.dat", "_t.dat", ".mp4", ".jpg", ".png", ".gif"}) {
                    found = files.find(name + suffix);
                    if (found != files.end()) break;
                }
            }
            if (found == files.end()) throw std::runtime_error("Local media file is missing");
            if (found->second.size() != 1) throw std::runtime_error("Local media filename is ambiguous");
            const auto& source = found->second.front();
            const auto stem = sessionTable(session) + "-" + std::to_string(rowIndex);
            fs::create_directories(directory);
            fs::path target;
            if (lower(source.extension().u8string()) == ".dat") {
                const auto media = decrypt(source, xorKey, aesKey);
                target = directory / (stem + "." + media.ext);
                std::ofstream output(target, std::ios::binary);
                output.exceptions(std::ios::failbit | std::ios::badbit);
                output.write(reinterpret_cast<const char*>(media.data.data()), media.data.size());
                output.close();
            } else {
                target = directory / (stem + source.extension().u8string());
                fs::copy_file(source, target, fs::copy_options::overwrite_existing);
            }
            row["media_path"] = target.u8string();
            event({{"type", "created_file"}, {"path", target.u8string()}});
        } catch (const std::exception& error) {
            failures[counter] = failures.value(counter, 0) + 1;
            row.erase("media_path");
            row["media_error"] = error.what();
        }
    }
};

int run(const Json& request) {
    const auto& accountConfig = request.at("account");
    const auto sessionFile = path(accountConfig.at("sessionDb").get<std::string>());
    Account account(sessionFile, accountConfig.at("dbKey").get<std::string>());
    account.accountId = getText(accountConfig, "myAccountId");
    MediaExport media(request, sessionFile);
    const auto output = fs::absolute(path(request.at("outputDir").get<std::string>()));
    const auto exports = fs::absolute(path(request.value("exportsDir", (output / "exports").u8string())));
    const auto mediaDir = fs::absolute(path(request.value("mediaDir", (output / "media").u8string())));
    fs::create_directories(exports);
    const auto sessions = request.at("sessionIds").get<std::vector<std::string>>();
    event({{"type", "ready"}, {"total", sessions.size()}, {"exports_dir", exports.u8string()}});
    Json paths = Json::object(), manifests = Json::object(), errors = Json::array();
    for (const auto& session : sessions) {
        const auto target = exports / (sessionTable(session) + ".jsonl");
        auto temporary = target;
        temporary += ".partial";
        try {
            auto cursor = account.cursor(session, request.value("ascending", true));
            Json manifest = {{"path", target.u8string()}};
            std::ofstream file(temporary, std::ios::binary);
            file.exceptions(std::ios::failbit | std::ios::badbit);
            size_t rowIndex = 0;
            while (cursor.hasMore()) {
                auto row = cursor.next();
                media.write(row, mediaDir, manifest, ++rowIndex, session);
                file << row.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n';
            }
            file.close();
            fs::rename(temporary, target);
            manifest["rows"] = rowIndex;
            manifest["bytes"] = fs::file_size(target);
            paths[session] = target.u8string();
            manifests[session] = manifest;
            event({{"type", "created_file"}, {"session_id", session}, {"path", target.u8string()}});
            event({{"type", "progress"}, {"phase", "complete"}, {"session_id", session},
                {"current", paths.size() + errors.size()}, {"total", sessions.size()}});
        } catch (const std::exception& error) {
            std::error_code ignored;
            fs::remove(temporary, ignored);
            Json failure = {{"session_id", session}, {"error", error.what()}};
            errors.push_back(failure);
            failure["type"] = "session_error";
            event(failure);
        }
    }
    event({{"type", "result"}, {"success", errors.empty()}, {"success_count", paths.size()},
        {"fail_count", errors.size()}, {"failed_sessions", errors}, {"raw_session_output_paths", paths},
        {"raw_export_manifests", manifests}});
    return 0;
}
}

int main(int argc, char** argv) {
    if (argc != 2 || std::string_view(argv[1]) != "weflow-export") return 2;
    try {
        return run(Json::parse(std::cin));
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
