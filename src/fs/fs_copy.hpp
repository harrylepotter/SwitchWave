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

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <sys/stat.h>
#include <sys/statvfs.h>

#include "fs/fs_common.hpp"
#include "utils.hpp"

namespace sw::fs {

// Copies a single file onto the sd card from a background thread, so the ui stays
// responsive while multi-gigabyte transfers run. Only one copy is in flight at a time.
class FileCopier {
    public:
        static constexpr const char *destination_dir = "sdmc:/SwitchWave";

        enum class State {
            Idle,
            Running,
            Done,
            Failed,
            Cancelled,
        };

        struct Progress {
            State state = State::Idle;
            std::string filename;
            std::uint64_t copied = 0, total = 0;
            int error = 0;
        };

    public:
        FileCopier() {
            this->thread = std::jthread(&FileCopier::thread_fn, this);
        }

        ~FileCopier() {
            this->cancelled = true;
            this->thread.request_stop();
            this->condvar.notify_all();
            this->thread.join();
        }

        FileCopier(const FileCopier &) = delete;
        FileCopier &operator=(const FileCopier &) = delete;

        // Queues a copy of the file at source. Returns false if one is already in flight.
        bool start(std::string_view source) {
            auto lk = std::scoped_lock(this->mutex);
            if (this->pending || this->state == State::Running)
                return false;

            this->source    = source;
            this->filename  = Path::filename(source);
            this->state     = State::Running;
            this->error     = 0;
            this->pending   = true;
            this->cancelled = false;
            this->copied    = 0;
            this->total     = 0;

            this->condvar.notify_one();
            return true;
        }

        // Asks the worker to stop; the partial file is removed once it does.
        void cancel() {
            this->cancelled = true;
        }

        // Clears a finished result, so the ui can dismiss it.
        void reset() {
            auto lk = std::scoped_lock(this->mutex);
            if (this->state != State::Running && !this->pending)
                this->state = State::Idle;
        }

        bool busy() const {
            auto lk = std::scoped_lock(this->mutex);
            return this->pending || this->state == State::Running;
        }

        Progress progress() const {
            auto lk = std::scoped_lock(this->mutex);
            return Progress{ this->state, this->filename, this->copied.load(), this->total.load(), this->error };
        }

    private:
        // Picks a free name in the destination directory, so repeated copies don't clobber each other.
        static std::string unique_destination(std::string_view filename) {
            auto dir       = std::string(FileCopier::destination_dir) + "/";
            auto candidate = dir + std::string(filename);

            struct stat st;
            if (::stat(candidate.c_str(), &st) != 0)
                return candidate;

            auto dot  = filename.rfind('.');
            auto stem = (dot != std::string_view::npos) ? filename.substr(0, dot) : filename;
            auto ext  = (dot != std::string_view::npos) ? filename.substr(dot)    : std::string_view();

            for (int i = 1; i < 1000; ++i) {
                candidate = dir + std::string(stem) + " (" + std::to_string(i) + ")" + std::string(ext);
                if (::stat(candidate.c_str(), &st) != 0)
                    return candidate;
            }

            return candidate;
        }

        std::pair<State, int> do_copy(const std::string &source) {
            struct stat st;
            if (::stat(source.c_str(), &st) != 0)
                return { State::Failed, errno };

            this->total = std::uint64_t(st.st_size);

            if (::mkdir(FileCopier::destination_dir, 0777) != 0 && errno != EEXIST)
                return { State::Failed, errno };

            // Bail out early rather than filling the sd card and failing halfway through
            struct statvfs vfs;
            if (::statvfs("sdmc:/", &vfs) == 0) {
                auto available = std::uint64_t(vfs.f_bsize) * std::uint64_t(vfs.f_bavail);
                if (available < std::uint64_t(st.st_size))
                    return { State::Failed, ENOSPC };
            }

            auto destination = FileCopier::unique_destination(Path::filename(source));

            auto *src = std::fopen(source.c_str(), "rb");
            if (!src)
                return { State::Failed, errno };
            SW_SCOPEGUARD([src] { std::fclose(src); });

            auto *dst = std::fopen(destination.c_str(), "wb");
            if (!dst)
                return { State::Failed, errno };

            auto buffer = std::make_unique<std::uint8_t[]>(FileCopier::buffer_size);

            bool ok = true;
            int err = 0;

            while (true) {
                if (this->cancelled) {
                    ok = false;
                    break;
                }

                auto read = std::fread(buffer.get(), 1, FileCopier::buffer_size, src);
                if (read == 0) {
                    if (std::ferror(src))
                        ok = false, err = EIO;
                    break;
                }

                if (std::fwrite(buffer.get(), 1, read, dst) != read) {
                    ok = false, err = errno ? errno : EIO;
                    break;
                }

                this->copied += read;
            }

            // Flush before reporting success, so a full sd card surfaces here and not silently
            if (ok && std::fflush(dst) != 0)
                ok = false, err = errno ? errno : EIO;

            std::fclose(dst);

            if (!ok) {
                // Never leave a truncated file behind, it would look like a valid media file
                std::remove(destination.c_str());
                return { this->cancelled ? State::Cancelled : State::Failed, err };
            }

            return { State::Done, 0 };
        }

        void thread_fn(std::stop_token token) {
            while (!token.stop_requested()) {
                std::string source;

                {
                    auto lk = std::unique_lock(this->mutex);
                    this->condvar.wait(lk, [this, &token] { return this->pending || token.stop_requested(); });

                    if (token.stop_requested())
                        return;

                    this->pending = false;
                    source        = this->source;
                }

                auto [result, err] = this->do_copy(source);

                auto lk = std::scoped_lock(this->mutex);
                this->state = result, this->error = err;
            }
        }

    private:
        static constexpr std::size_t buffer_size = 1 << 20; // 1MiB

        mutable std::mutex mutex;
        std::condition_variable condvar;

        std::string source, filename;
        State state = State::Idle;
        int error   = 0;
        bool pending = false;

        std::atomic<bool> cancelled = false;
        std::atomic<std::uint64_t> copied = 0, total = 0;

        std::jthread thread;
};

} // namespace sw::fs
