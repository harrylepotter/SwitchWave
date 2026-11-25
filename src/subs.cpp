// Copyright (c) 2024 averne <averne381@gmail.com>
//
// This file is part of SwitchWave.
//
// SwitchWave is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// SwitchWave is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with SwitchWave.  If not, see <http://www.gnu.org/licenses/>.

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>

#include <sys/stat.h>

#include <curl/curl.h>
#include <json-c/json.h>

#include "subs.hpp"

namespace sw {

namespace {

constexpr const char *api_base   = "https://api.opensubtitles.com/api/v1";
constexpr const char *user_agent = "SwitchWave v1.1.1";

constexpr std::size_t hash_chunk_size = 64 * 1024;

std::size_t write_cb(char *ptr, std::size_t size, std::size_t nmemb, void *user) {
    auto total = size * nmemb;
    static_cast<std::string *>(user)->append(ptr, total);
    return total;
}

struct Response {
    long status = 0;
    std::string body;
    std::string error;
};

// One-shot https request. `body` empty means GET, otherwise POST.
Response http_request(const std::string &url, const std::string &body,
        const std::string &api_key, const std::string &token) {
    Response out;

    auto *curl = ::curl_easy_init();
    if (!curl) {
        out.error = "failed to initialize curl";
        return out;
    }

    curl_slist *headers = nullptr;
    headers = ::curl_slist_append(headers, "Accept: application/json");
    headers = ::curl_slist_append(headers, "Content-Type: application/json");

    if (!api_key.empty()) {
        auto h = "Api-Key: " + api_key;
        headers = ::curl_slist_append(headers, h.c_str());
    }

    if (!token.empty()) {
        auto h = "Authorization: Bearer " + token;
        headers = ::curl_slist_append(headers, h.c_str());
    }

    ::curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    ::curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
    ::curl_easy_setopt(curl, CURLOPT_USERAGENT,      user_agent);
    ::curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_cb);
    ::curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &out.body);
    ::curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    ::curl_easy_setopt(curl, CURLOPT_TIMEOUT,        30L);
    ::curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);

    if (!body.empty()) {
        ::curl_easy_setopt(curl, CURLOPT_POST,       1L);
        ::curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    }

    auto rc = ::curl_easy_perform(curl);
    if (rc != CURLE_OK)
        out.error = ::curl_easy_strerror(rc);
    else
        ::curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out.status);

    ::curl_slist_free_all(headers);
    ::curl_easy_cleanup(curl);

    return out;
}

// Pulls "message"/"error" out of an api error body, so the ui can show something useful
std::string api_error_message(const std::string &body, long status) {
    std::string detail;

    if (auto *root = ::json_tokener_parse(body.c_str())) {
        json_object *field = nullptr;
        if (::json_object_object_get_ex(root, "message", &field) ||
                ::json_object_object_get_ex(root, "error", &field)) {
            if (auto *str = ::json_object_get_string(field))
                detail = str;
        }
        ::json_object_put(root);
    }

    if (detail.empty())
        return "http " + std::to_string(status);

    return detail + " (http " + std::to_string(status) + ")";
}

std::string basename_no_ext(std::string_view path) {
    auto slash = path.rfind('/');
    auto name  = (slash != std::string_view::npos) ? path.substr(slash + 1) : path;

    auto dot = name.rfind('.');
    return std::string((dot != std::string_view::npos) ? name.substr(0, dot) : name);
}

} // namespace

bool SubtitleDownloader::compute_hash(const char *path, std::uint64_t &hash, std::uint64_t &size) {
    auto *fp = std::fopen(path, "rb");
    if (!fp)
        return false;

    std::fseek(fp, 0, SEEK_END);
    auto file_size = std::uint64_t(std::ftell(fp));

    // Below two chunks the hash isn't defined, and matching would be meaningless
    if (file_size < 2 * hash_chunk_size) {
        std::fclose(fp);
        return false;
    }

    auto sum_chunk = [fp](long offset, std::uint64_t &acc) {
        auto words = std::make_unique<std::uint64_t[]>(hash_chunk_size / sizeof(std::uint64_t));

        std::fseek(fp, offset, SEEK_SET);
        if (std::fread(words.get(), 1, hash_chunk_size, fp) != hash_chunk_size)
            return false;

        for (std::size_t i = 0; i < hash_chunk_size / sizeof(std::uint64_t); ++i)
            acc += words[i];

        return true;
    };

    auto acc = file_size;
    auto ok  = sum_chunk(0, acc) && sum_chunk(long(file_size - hash_chunk_size), acc);

    std::fclose(fp);

    if (!ok)
        return false;

    hash = acc, size = file_size;
    return true;
}

SubtitleDownloader::SubtitleDownloader() {
    this->thread = std::jthread(&SubtitleDownloader::thread_fn, this);
}

SubtitleDownloader::~SubtitleDownloader() {
    this->thread.request_stop();
    this->condvar.notify_all();
    if (this->thread.joinable())
        this->thread.join();
}

bool SubtitleDownloader::search(std::string_view media_path, const Credentials &credentials) {
    auto lk = std::scoped_lock(this->mutex);
    if (this->job != Job::None || this->state == State::Searching || this->state == State::Downloading)
        return false;

    if (credentials.api_key.empty()) {
        this->state   = State::Failed;
        this->message = "No API key set. Add one in Settings.";
        return false;
    }

    this->creds      = credentials;
    this->media_path = media_path;
    this->job        = Job::Search;
    this->state      = State::Searching;
    this->message    = "Searching...";
    this->results.clear();
    this->downloaded_path.clear();

    this->condvar.notify_one();
    return true;
}

bool SubtitleDownloader::download(std::size_t index) {
    auto lk = std::scoped_lock(this->mutex);
    if (this->job != Job::None || index >= this->results.size())
        return false;

    this->job       = Job::Download;
    this->job_index = index;
    this->state     = State::Downloading;
    this->message   = "Downloading...";

    this->condvar.notify_one();
    return true;
}

void SubtitleDownloader::reset() {
    auto lk = std::scoped_lock(this->mutex);
    if (this->job != Job::None)
        return;

    this->state = State::Idle;
    this->message.clear();
    this->results.clear();
    this->downloaded_path.clear();
}

bool SubtitleDownloader::busy() const {
    auto lk = std::scoped_lock(this->mutex);
    return this->job != Job::None || this->state == State::Searching || this->state == State::Downloading;
}

SubtitleDownloader::Snapshot SubtitleDownloader::snapshot() const {
    auto lk = std::scoped_lock(this->mutex);
    return Snapshot{ this->state, this->message, this->results, this->downloaded_path };
}

bool SubtitleDownloader::ensure_token(std::string &error) {
    std::string api_key, username, password;

    {
        auto lk = std::scoped_lock(this->mutex);
        if (!this->token.empty())
            return true;
        api_key = this->creds.api_key, username = this->creds.username, password = this->creds.password;
    }

    if (username.empty() || password.empty()) {
        error = "Downloads need an opensubtitles.com login. Add one in Settings.";
        return false;
    }

    auto *payload = ::json_object_new_object();
    ::json_object_object_add(payload, "username", ::json_object_new_string(username.c_str()));
    ::json_object_object_add(payload, "password", ::json_object_new_string(password.c_str()));
    auto body = std::string(::json_object_to_json_string(payload));
    ::json_object_put(payload);

    auto resp = http_request(std::string(api_base) + "/login", body, api_key, "");
    if (!resp.error.empty()) {
        error = resp.error;
        return false;
    }

    if (resp.status != 200) {
        error = "Login failed: " + api_error_message(resp.body, resp.status);
        return false;
    }

    std::string parsed;
    if (auto *root = ::json_tokener_parse(resp.body.c_str())) {
        json_object *field = nullptr;
        if (::json_object_object_get_ex(root, "token", &field)) {
            if (auto *str = ::json_object_get_string(field))
                parsed = str;
        }
        ::json_object_put(root);
    }

    if (parsed.empty()) {
        error = "Login response had no token";
        return false;
    }

    auto lk = std::scoped_lock(this->mutex);
    this->token = std::move(parsed);
    return true;
}

void SubtitleDownloader::do_search() {
    std::string path, api_key, languages;

    {
        auto lk = std::scoped_lock(this->mutex);
        path = this->media_path, api_key = this->creds.api_key;
        languages = this->creds.languages.empty() ? "en" : this->creds.languages;
    }

    std::uint64_t hash = 0, size = 0;
    auto have_hash = SubtitleDownloader::compute_hash(path.c_str(), hash, size);

    auto parse_into = [](const std::string &body, std::vector<Result> &out, bool by_hash) {
        auto *root = ::json_tokener_parse(body.c_str());
        if (!root)
            return;

        json_object *data = nullptr;
        if (::json_object_object_get_ex(root, "data", &data) &&
                ::json_object_is_type(data, json_type_array)) {
            auto count = ::json_object_array_length(data);
            for (std::size_t i = 0; i < count && out.size() < 25; ++i) {
                auto *entry = ::json_object_array_get_idx(data, i);

                json_object *attrs = nullptr;
                if (!::json_object_object_get_ex(entry, "attributes", &attrs))
                    continue;

                Result r;
                r.matched_by_hash = by_hash;

                json_object *field = nullptr;
                if (::json_object_object_get_ex(attrs, "release", &field))
                    if (auto *s = ::json_object_get_string(field)) r.release = s;
                if (::json_object_object_get_ex(attrs, "language", &field))
                    if (auto *s = ::json_object_get_string(field)) r.language = s;
                if (::json_object_object_get_ex(attrs, "download_count", &field))
                    r.downloads = ::json_object_get_int64(field);

                json_object *files = nullptr;
                if (::json_object_object_get_ex(attrs, "files", &files) &&
                        ::json_object_is_type(files, json_type_array) &&
                        ::json_object_array_length(files) > 0) {
                    auto *f0 = ::json_object_array_get_idx(files, 0);
                    if (::json_object_object_get_ex(f0, "file_id", &field))
                        r.file_id = ::json_object_get_int64(field);
                }

                if (r.file_id) {
                    if (r.release.empty())
                        r.release = "(untitled)";
                    out.push_back(std::move(r));
                }
            }
        }

        ::json_object_put(root);
    };

    std::vector<Result> found;
    std::string error;

    if (have_hash) {
        char hash_hex[32];
        std::snprintf(hash_hex, sizeof(hash_hex), "%016llx", static_cast<unsigned long long>(hash));

        auto url = std::string(api_base) + "/subtitles?moviehash=" + hash_hex + "&languages=" + languages;
        auto resp = http_request(url, "", api_key, "");

        if (!resp.error.empty())
            error = resp.error;
        else if (resp.status != 200)
            error = api_error_message(resp.body, resp.status);
        else
            parse_into(resp.body, found, true);
    }

    // Fall back to the filename when the hash found nothing, which happens for
    // re-encodes and renamed releases the database has never seen
    if (found.empty() && error.empty()) {
        auto query = basename_no_ext(path);

        if (auto *escaped = ::curl_easy_escape(nullptr, query.c_str(), int(query.size()))) {
            auto url = std::string(api_base) + "/subtitles?query=" + escaped + "&languages=" + languages;
            ::curl_free(escaped);

            auto resp = http_request(url, "", api_key, "");
            if (!resp.error.empty())
                error = resp.error;
            else if (resp.status != 200)
                error = api_error_message(resp.body, resp.status);
            else
                parse_into(resp.body, found, false);
        }
    }

    auto lk = std::scoped_lock(this->mutex);

    if (!error.empty())
        this->state = State::Failed, this->message = error;
    else if (found.empty())
        this->state = State::Failed, this->message = "No subtitles found for this file";
    else
        this->state   = State::Ready,
        this->results = std::move(found),
        this->message = std::to_string(this->results.size()) + " result(s)";
}

void SubtitleDownloader::do_download(std::size_t index) {
    std::string error;
    if (!this->ensure_token(error)) {
        auto lk = std::scoped_lock(this->mutex);
        this->state = State::Failed, this->message = error;
        return;
    }

    std::string api_key, token, path;
    Result target;

    {
        auto lk = std::scoped_lock(this->mutex);
        if (index >= this->results.size()) {
            this->state = State::Failed, this->message = "Result no longer available";
            return;
        }
        target  = this->results[index];
        api_key = this->creds.api_key, token = this->token, path = this->media_path;
    }

    auto *payload = ::json_object_new_object();
    ::json_object_object_add(payload, "file_id", ::json_object_new_int64(target.file_id));
    auto body = std::string(::json_object_to_json_string(payload));
    ::json_object_put(payload);

    auto resp = http_request(std::string(api_base) + "/download", body, api_key, token);

    auto fail = [this](std::string msg) {
        auto lk = std::scoped_lock(this->mutex);
        this->state = State::Failed, this->message = std::move(msg);
    };

    if (!resp.error.empty())
        return fail(resp.error);

    if (resp.status != 200)
        return fail("Download request failed: " + api_error_message(resp.body, resp.status));

    std::string link, remote_name;
    if (auto *root = ::json_tokener_parse(resp.body.c_str())) {
        json_object *field = nullptr;
        if (::json_object_object_get_ex(root, "link", &field))
            if (auto *s = ::json_object_get_string(field)) link = s;
        if (::json_object_object_get_ex(root, "file_name", &field))
            if (auto *s = ::json_object_get_string(field)) remote_name = s;
        ::json_object_put(root);
    }

    if (link.empty())
        return fail("Download response had no link");

    // The link serves the subtitle itself, not json
    auto file = http_request(link, "", "", "");
    if (!file.error.empty())
        return fail(file.error);
    if (file.status != 200 || file.body.empty())
        return fail("Failed to fetch subtitle file (http " + std::to_string(file.status) + ")");

    // The usb drive is mounted read-only, so subtitles always land on the sd card
    ::mkdir("sdmc:/SwitchWave", 0777);
    if (::mkdir(SubtitleDownloader::destination_dir, 0777) != 0 && errno != EEXIST)
        return fail(std::string("Failed to create subtitle directory: ") + std::strerror(errno));

    auto extension = std::string(".srt");
    if (auto dot = remote_name.rfind('.'); dot != std::string::npos)
        extension = remote_name.substr(dot);

    auto out_path = std::string(SubtitleDownloader::destination_dir) + "/" +
        basename_no_ext(path) + "." + (target.language.empty() ? "und" : target.language) + extension;

    auto *fp = std::fopen(out_path.c_str(), "wb");
    if (!fp)
        return fail(std::string("Failed to open ") + out_path + ": " + std::strerror(errno));

    auto written = std::fwrite(file.body.data(), 1, file.body.size(), fp);
    auto flushed = std::fflush(fp) == 0;
    std::fclose(fp);

    if (written != file.body.size() || !flushed) {
        std::remove(out_path.c_str());
        return fail("Failed to write subtitle to the sd card");
    }

    auto lk = std::scoped_lock(this->mutex);
    this->state           = State::Done;
    this->downloaded_path = std::move(out_path);
    this->message         = "Downloaded";
}

void SubtitleDownloader::thread_fn(std::stop_token token) {
    while (!token.stop_requested()) {
        Job pending  = Job::None;
        std::size_t index = 0;

        {
            auto lk = std::unique_lock(this->mutex);
            this->condvar.wait(lk, [this, &token] { return this->job != Job::None || token.stop_requested(); });

            if (token.stop_requested())
                return;

            pending = this->job;
            index   = this->job_index;
        }

        if (pending == Job::Search)
            this->do_search();
        else if (pending == Job::Download)
            this->do_download(index);

        auto lk = std::scoped_lock(this->mutex);
        this->job = Job::None;
    }
}

} // namespace sw
