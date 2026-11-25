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

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include <switch.h>

namespace sw {

// A minimal http server that lets a phone on the same network drive the ui.
//
// Button presses are injected straight into the PadState after padUpdate(), rather
// than into ImGui: the player reads the raw pad (padGetButtonsDown) while the menus
// go through ImGui's gamepad nav, and patching the pad drives both at once.
class RemoteControl {
    public:
        static constexpr std::uint16_t default_port = 8080;

    public:
        RemoteControl() = default;
        ~RemoteControl() { this->finalize(); }

        RemoteControl(const RemoteControl &) = delete;
        RemoteControl &operator=(const RemoteControl &) = delete;

        // Binds the listening socket and spawns the server thread.
        // Returns 0 on success, otherwise errno.
        int initialize(std::uint16_t port = RemoteControl::default_port);

        void finalize();

        // Applies at most one queued input per frame, so quick taps stay distinct
        // presses instead of merging into one. Call right after padUpdate().
        void inject(PadState &pad);

        bool running() const {
            return this->is_running;
        }

        // "http://10.0.0.5:8080", or empty when the console has no address
        std::string url() const {
            auto lk = std::scoped_lock(this->mutex);
            return this->url_;
        }

        // One-line summary for the ui, never empty
        std::string status() const;

    private:
        // One frame worth of input. The right stick is deflected for that frame only,
        // which the player reads as a single volume step.
        struct Input {
            std::uint64_t buttons = 0;
            HidAnalogStickState rstick = {};

            bool empty() const {
                return !this->buttons && !this->rstick.x && !this->rstick.y;
            }
        };

        void thread_fn(std::stop_token token);
        void serve_client(int fd);

        // Maps a request path to an input, empty when unrecognised
        static Input input_for_path(std::string_view path);

    private:
        mutable std::mutex mutex;
        std::deque<Input> queue;
        std::string url_;

        int listen_fd = -1;
        std::uint16_t port = RemoteControl::default_port;
        bool is_running = false;

        std::jthread thread;
};

} // namespace sw
