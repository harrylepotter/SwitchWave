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

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "remote.hpp"

namespace sw {

namespace {

// Kept deliberately small and dependency-free: no external css/js, since the
// console has no route to the internet from the phone's point of view.
constexpr std::string_view index_page = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<meta name="theme-color" content="#14161a">
<title>SwitchWave Remote</title>
<style>
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html,body{margin:0;min-height:100%;background:#14161a;color:#e6e8ec;
  font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;
  overscroll-behavior:none;touch-action:manipulation}
body{display:flex;flex-direction:column;align-items:center;gap:14px;padding:16px 10px}
h1{font-size:13px;font-weight:600;letter-spacing:.16em;text-transform:uppercase;color:#7d8595;margin:0}
button{border:0;border-radius:14px;background:#242832;color:#e6e8ec;font-weight:600;
  display:flex;align-items:center;justify-content:center;cursor:pointer;user-select:none;
  transition:transform .06s,background .12s}
button:active{background:#e60012;transform:scale(.92)}
.shoulders{display:flex;gap:8px;width:100%;max-width:330px}
.shoulders button{flex:1;height:46px;font-size:14px}
.mid{display:flex;gap:14px;flex-wrap:wrap;justify-content:center}
.grid{display:grid;grid-template-columns:repeat(3,58px);grid-template-rows:repeat(3,58px);gap:8px}
.grid button{font-size:20px}
.u{grid-area:1/2}.l{grid-area:2/1}.r{grid-area:2/3}.d{grid-area:3/2}
.fx{grid-area:1/2}.fy{grid-area:2/1}.fa{grid-area:2/3}.fb{grid-area:3/2}
.face button{background:#2f3542;font-size:17px}
.sys{display:flex;gap:10px;width:100%;max-width:330px}
.sys button{flex:1;height:46px;font-size:15px}
h2{font-size:11px;font-weight:600;letter-spacing:.16em;text-transform:uppercase;color:#5c6473;margin:4px 0 -4px}
.legend{font-size:11px;color:#5c6473;line-height:1.7;text-align:center;max-width:330px}
.legend b{color:#8b93a3;font-weight:600}
#s{font-size:11px;color:#5c6473;height:13px;letter-spacing:.04em}
</style>
</head>
<body>
<h1>SwitchWave</h1>

<div class="shoulders">
  <button data-k="zl">ZL</button>
  <button data-k="l">L</button>
  <button data-k="r">R</button>
  <button data-k="zr">ZR</button>
</div>

<div class="mid">
  <div class="grid dpad">
    <button class="u" data-k="up">&#9650;</button>
    <button class="l" data-k="left">&#9664;</button>
    <button class="r" data-k="right">&#9654;</button>
    <button class="d" data-k="down">&#9660;</button>
  </div>
  <div class="grid face">
    <button class="fx" data-k="x">X</button>
    <button class="fy" data-k="y">Y</button>
    <button class="fa" data-k="a">A</button>
    <button class="fb" data-k="b">B</button>
  </div>
</div>

<div class="sys">
  <button data-k="minus">&ndash; Minus</button>
  <button data-k="plus">+ Plus</button>
</div>

<h2>Player</h2>

<div class="sys">
  <button data-k="chapter-prev">&#9664;&#9664; Chapter</button>
  <button data-k="playpause">Play / Pause</button>
  <button data-k="chapter-next">Chapter &#9654;&#9654;</button>
</div>

<div class="sys">
  <button data-k="vol-down">Vol &minus;</button>
  <button data-k="vol-up">Vol +</button>
  <button data-k="stickl">Screenshot</button>
</div>

<div class="legend">
  In the player: <b>Y</b> menu &amp; subtitles &middot; <b>+</b> exit video &middot;
  <b>A</b>/<b>X</b> play/pause &middot; <b>&#9664;&#9654;</b> seek bar &middot;
  <b>L</b>/<b>R</b> &plusmn;5s &middot; <b>ZL</b>/<b>ZR</b> &plusmn;60s &middot;
  <b>&ndash;</b> console
</div>

<div id="s"></div>

<script>
var s=document.getElementById('s'),t=0;
function send(k){
  fetch('/k/'+k,{method:'POST',keepalive:true})
    .then(function(r){ if(!r.ok) throw 0; s.textContent=k; })
    .catch(function(){ s.textContent='connection lost'; });
  clearTimeout(t); t=setTimeout(function(){s.textContent=''},700);
  if(navigator.vibrate) navigator.vibrate(12);
}
document.querySelectorAll('button').forEach(function(b){
  // pointerdown rather than click, so there is no 300ms tap delay
  b.addEventListener('pointerdown',function(e){e.preventDefault();send(b.dataset.k)});
  b.addEventListener('contextmenu',function(e){e.preventDefault()});
});
</script>
</body>
</html>)HTML";

bool send_all(int fd, std::string_view data) {
    while (!data.empty()) {
        auto sent = ::send(fd, data.data(), data.size(), 0);
        if (sent <= 0)
            return false;
        data.remove_prefix(sent);
    }
    return true;
}

} // namespace

RemoteControl::Input RemoteControl::input_for_path(std::string_view path) {
    if (!path.starts_with("/k/"))
        return {};

    auto key = path.substr(3);
    auto button = [](std::uint64_t b) { return Input{ .buttons = b }; };

    if (key == "up")     return button(HidNpadButton_Up);
    if (key == "down")   return button(HidNpadButton_Down);
    if (key == "left")   return button(HidNpadButton_Left);
    if (key == "right")  return button(HidNpadButton_Right);

    if (key == "a")      return button(HidNpadButton_A);
    if (key == "b")      return button(HidNpadButton_B);
    if (key == "x")      return button(HidNpadButton_X);
    if (key == "y")      return button(HidNpadButton_Y);

    if (key == "l")      return button(HidNpadButton_L);
    if (key == "r")      return button(HidNpadButton_R);
    if (key == "zl")     return button(HidNpadButton_ZL);
    if (key == "zr")     return button(HidNpadButton_ZR);

    if (key == "plus")   return button(HidNpadButton_Plus);
    if (key == "minus")  return button(HidNpadButton_Minus);

    if (key == "stickl") return button(HidNpadButton_StickL);
    if (key == "stickr") return button(HidNpadButton_StickR);

    // The player only skips chapters with up or down held, so send both in one frame
    if (key == "chapter-prev") return button(HidNpadButton_Up | HidNpadButton_ZL);
    if (key == "chapter-next") return button(HidNpadButton_Up | HidNpadButton_ZR);

    // The player maps right stick left/right to volume
    if (key == "vol-down") return Input{ .rstick = { .x = JOYSTICK_MIN, .y = 0 } };
    if (key == "vol-up")   return Input{ .rstick = { .x = JOYSTICK_MAX, .y = 0 } };

    // Kept so an older cached page keeps working
    if (key == "ok")        return button(HidNpadButton_A);
    if (key == "back")      return button(HidNpadButton_B);
    if (key == "playpause") return button(HidNpadButton_X);

    return {};
}

int RemoteControl::initialize(std::uint16_t port) {
    if (this->is_running)
        return 0;

    this->port = port;

    auto fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return errno;

    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        auto err = errno;
        ::close(fd);
        return err;
    }

    if (::listen(fd, 8) < 0) {
        auto err = errno;
        ::close(fd);
        return err;
    }

    this->listen_fd  = fd;
    this->is_running = true;

    {
        // gethostid() gives the console's address, and loops back when there is no link
        auto host = std::uint32_t(::gethostid());

        auto lk = std::scoped_lock(this->mutex);
        if (host && host != htonl(INADDR_LOOPBACK)) {
            in_addr in = {};
            in.s_addr = host;

            char buf[64];
            std::snprintf(buf, sizeof(buf), "http://%s:%u", ::inet_ntoa(in), unsigned(port));
            this->url_ = buf;
        } else {
            this->url_.clear();
        }
    }

    this->thread = std::jthread(&RemoteControl::thread_fn, this);

    return 0;
}

void RemoteControl::finalize() {
    if (!this->is_running)
        return;

    this->is_running = false;

    this->thread.request_stop();
    if (this->thread.joinable())
        this->thread.join();

    if (this->listen_fd >= 0)
        ::close(this->listen_fd), this->listen_fd = -1;

    auto lk = std::scoped_lock(this->mutex);
    this->url_.clear();
    this->queue.clear();
}

std::string RemoteControl::status() const {
    if (!this->is_running)
        return "Remote: off";

    auto lk = std::scoped_lock(this->mutex);
    if (this->url_.empty())
        return "Remote: no network address";

    return "Remote: " + this->url_;
}

void RemoteControl::inject(PadState &pad) {
    Input input;

    {
        auto lk = std::scoped_lock(this->mutex);
        if (this->queue.empty())
            return;

        // A button injected last frame is still in buttons_old, so injecting it again
        // would not read as a new press: let this frame be the release instead
        if (pad.buttons_old & this->queue.front().buttons)
            return;

        input = this->queue.front();
        this->queue.pop_front();
    }

    // buttons_old still lacks the bit, so padGetButtonsDown() reports a fresh press,
    // and the next padUpdate() rolls it into buttons_old to produce the release
    pad.buttons_cur |= input.buttons;

    // padUpdate() overwrites the sticks every frame, so this needs no reset
    if (input.rstick.x || input.rstick.y)
        pad.sticks[1] = input.rstick;
}

void RemoteControl::serve_client(int fd) {
    char buf[1024] = {};
    auto len = ::recv(fd, buf, sizeof(buf) - 1, 0);
    if (len <= 0)
        return;

    auto request = std::string_view(buf, len);

    // Only the request line matters here: "<METHOD> <PATH> HTTP/1.1"
    auto line_end = request.find("\r\n");
    auto line     = request.substr(0, (line_end != std::string_view::npos) ? line_end : request.size());

    auto first_space = line.find(' ');
    if (first_space == std::string_view::npos)
        return;

    auto rest         = line.substr(first_space + 1);
    auto second_space = rest.find(' ');
    auto path         = rest.substr(0, (second_space != std::string_view::npos) ? second_space : rest.size());

    // Strip any query string
    if (auto q = path.find('?'); q != std::string_view::npos)
        path = path.substr(0, q);

    if (path == "/" || path == "/index.html") {
        char header[192];
        auto header_len = std::snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Content-Length: %zu\r\n"
            "Cache-Control: no-store\r\n"
            "Connection: close\r\n\r\n",
            index_page.size());

        send_all(fd, std::string_view(header, header_len));
        send_all(fd, index_page);
        return;
    }

    if (auto input = RemoteControl::input_for_path(path); !input.empty()) {
        {
            auto lk = std::scoped_lock(this->mutex);
            // Bound the backlog, so a flood of taps can't queue minutes of input
            if (this->queue.size() < 16)
                this->queue.push_back(input);
        }

        send_all(fd, "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n");
        return;
    }

    send_all(fd, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
}

void RemoteControl::thread_fn(std::stop_token token) {
    while (!token.stop_requested()) {
        pollfd pfd = {};
        pfd.fd     = this->listen_fd;
        pfd.events = POLLIN;

        // Short timeout so shutdown doesn't wait on a connection that never arrives
        auto ready = ::poll(&pfd, 1, 200);
        if (ready <= 0)
            continue;

        auto fd = ::accept(this->listen_fd, nullptr, nullptr);
        if (fd < 0)
            continue;

        this->serve_client(fd);
        ::close(fd);
    }
}

} // namespace sw
