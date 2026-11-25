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

#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace sw {

// Fetches subtitles from opensubtitles.com. All network work happens on a worker
// thread; the ui polls snapshot() and never blocks.
//
// Needs an api key (free, from opensubtitles.com/consumers) plus an account, since
// downloads are quota-limited per user. Credentials live in the app config.
class SubtitleDownloader {
    public:
        static constexpr const char *destination_dir = "sdmc:/SwitchWave/subs";

        enum class State {
            Idle,
            Searching,
            Ready,       // results available for the user to pick from
            Downloading,
            Done,
            Failed,
        };

        struct Result {
            std::int64_t file_id = 0;
            std::string release;
            std::string language;
            std::int64_t downloads = 0;
            bool matched_by_hash = false;
        };

        struct Snapshot {
            State state = State::Idle;
            std::string message;
            std::vector<Result> results;
            std::string downloaded_path;
        };

        struct Credentials {
            std::string api_key, username, password, languages;
        };

    public:
        SubtitleDownloader();
        ~SubtitleDownloader();

        SubtitleDownloader(const SubtitleDownloader &) = delete;
        SubtitleDownloader &operator=(const SubtitleDownloader &) = delete;

        // Queues a search for the currently playing file. False if already busy.
        bool search(std::string_view media_path, const Credentials &creds);

        // Queues a download of a previously returned result. False if already busy.
        bool download(std::size_t index);

        void reset();

        bool busy() const;
        Snapshot snapshot() const;

        // OSDb hash: filesize plus the 64-bit words of the first and last 64KiB.
        // Returns false for files below 128KiB, where the hash isn't meaningful.
        static bool compute_hash(const char *path, std::uint64_t &hash, std::uint64_t &size);

    private:
        enum class Job { None, Search, Download };

        void thread_fn(std::stop_token token);
        void do_search();
        void do_download(std::size_t index);

        // Fetches a bearer token, reusing the cached one when still held
        bool ensure_token(std::string &error);

    private:
        mutable std::mutex mutex;
        std::condition_variable condvar;

        Job job = Job::None;
        std::size_t job_index = 0;

        Credentials creds;
        std::string media_path;
        std::string token;

        State state = State::Idle;
        std::string message;
        std::vector<Result> results;
        std::string downloaded_path;

        std::jthread thread;
};

} // namespace sw
