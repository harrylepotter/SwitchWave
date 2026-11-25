# <img src="https://github.com/user-attachments/assets/b81b9503-948e-4cba-b0a1-f5f809588aad" width="48"> SwitchWave
A hardware-accelerated media player for the Nintendo Switch, built on mpv and FFmpeg.

## Features
- Custom hardware acceleration backend for FFmpeg, with dynamic frequency scaling. The following codecs can be decoded:
    - MPEG1/2/4
    - VC1
    - H.264/AVC (10+ bit not supported by hardware)
    - H.265/HEVC (12+ bit not supported by hardware)
    - VP8
    - VP9 (10+ bit not supported by hardware)
- Custom graphics backend for mpv using [deko3d](https://github.com/devkitPro/deko3d), supporting:
    - Playback at 4k60fps
    - Direct rendering (faster software decoding)
    - Custom post-processing shaders
- Custom audio backend for mpv using native Nintendo APIs, supporting layouts up to 5.1 surround
- Network playback through HTTP/S, Samba, NFS or SFTP
- External drive support using [libusbhsfs](https://github.com/DarkMatterCore/libusbhsfs)
- Rich and responsive user interface, even under load

## What this fork adds
This fork of [averne/SwitchWave](https://github.com/averne/SwitchWave) builds on the exFAT/GPT USB fixes from [crux161's fork](https://github.com/crux161/SwitchWave) and adds the following:

### Web remote control
- SwitchWave runs a small HTTP server on port **8080**, which starts with the app.
- The console's address (e.g. `http://192.168.0.99:8080`) is shown in the footer of the Explorer, so you can read it without opening any menus.
- Open the address in a browser on any phone or computer on the same network to get an on-screen remote with:
    - the d-pad, A, B, X and Y
    - L, R, ZL and ZR
    - Plus
    - Play/Pause, Volume −/+ and Screenshot
- Remote presses are sent to the app as if they came from a controller, so they work everywhere in the UI. Joy-Cons and other controllers keep working alongside it.

### Copy files to the SD card
- Files on USB drives or network shares can be copied to `sdmc:/SwitchWave` with the **Copy to SD card** button in the Explorer's file info panel.
- A progress bar shows while the file copies, and you can cancel the copy.
- Free space is checked before the copy starts. If a file with the same name is already there, the copy gets a new name instead of overwriting it.

### Subtitle downloads (opensubtitles.com)
- **Search opensubtitles.com** in the player's Subtitles tab finds subtitles for the file that's playing. It looks up the file's hash first, then falls back to searching by filename, and loads the subtitle you pick straight into playback.
- Before using it, enter your credentials under **Settings → Subtitles (opensubtitles.com)**:
    - an API key, which is free from [opensubtitles.com/consumers](https://www.opensubtitles.com/consumers)
    - your account username and password, because opensubtitles.com counts download quotas per user
    - your preferred languages (default `en`)
- The credentials are stored only in the app's settings file on your SD card. None are bundled with the app.

### Volume boost
- The volume slider now goes up to **200%**, using mpv's software amplification. Above about 150%, already-loud sources may clip. To change the limit, set `volume-max` in `mpv.conf`.

### USB drive compatibility
- Every patch in [`misc/libusbhsfs/`](misc/libusbhsfs/) is applied to libusbhsfs at build time. They add:
    - a relaxed, bounds-checked GPT volume probe, so exFAT partitions on GPT-formatted drives (e.g. NVMe enclosures) mount correctly
    - a separate scratch buffer for probing GPT partitions, so probing one partition can't overwrite the data being read for the next
    - tolerance for USB bridges (e.g. some Realtek NVMe enclosures) that falsely report "medium not present" to the optional Prevent/Allow Medium Removal command

### Developer conveniences
- `deploy.sh` sends a fresh build to the console over the network using hbmenu's netloader (press Y in hbmenu first), so there's no need to swap SD cards:
  ```sh
  SWITCH_IP=192.168.0.99 ./deploy.sh
  ```
- json-c is now linked explicitly, because portlibs ships no pkg-config file for it.

## Screenshots

<p float="center">
    <img src="https://github.com/user-attachments/assets/09aed446-148a-4276-8b07-336890c224a3" width="413" />
    <img src="https://github.com/user-attachments/assets/6e354511-47bc-4898-881c-348d5a9e6fbc" width="413" />
    <img src="https://github.com/user-attachments/assets/b86eb7c6-4229-46c6-8709-86d1a6ee8eed" width="413" />
    <img src="https://github.com/user-attachments/assets/70f4be3e-fa1e-434a-b76c-4fb6b671f80e" width="413" />
</p>

## Setup
- Download the [latest release](https://github.com/averne/SwitchWave/releases/latest), and extract it to the root of your sd card (be careful to merge and not overwrite folders)
- Network shares can be configured through the app, as can mpv settings via the built-in editor (refer to the [manual](https://mpv.io/manual/master/))
- Most relevant runtime parameters can be dynamically adjusted during playback through the menu, or failing that, the console ([manual](https://mpv.io/manual/master/#console))

## Building

### Docker (recommended)
```sh
./build-docker.sh
```
This builds the toolchain image and compiles everything automatically. Output will be found in `build/`.

To build with GIMP 3 instead of the default GIMP 2:
```sh
GIMP_VERSION=3 ./build-docker.sh
```

### Manual
- Set up a [devkitpro](https://devkitpro.org/wiki/devkitPro_pacman) environment for Switch homebrew development.
- Install the following packages: `switch-bzip2`, `switch-dav1d`, `switch-freetype`, `switch-glm`, `switch-harfbuzz`, `switch-libarchive`, `switch-libass`, `switch-libfribidi`, `switch-libjpeg-turbo`, `switch-libpng`, `switch-libwebp`, `switch-curl`, `switch-libssh2`, `switch-mbedtls`, `switch-ntfs-3g` and `switch-lwext4`. In addition, the following build dependencies are required: `switch-pkg-config`, `dkp-meson-scripts`, `dkp-toolchain-vars`, and [GIMP](https://www.gimp.org/) (2 or 3).
- Compile and install a GPL build of [libusbhsfs](https://github.com/DarkMatterCore/libusbhsfs).
- Compile and install [libsmb2](misc/libsmb2/) and [libnfs](misc/libnfs/).
- Configure, compile and install FFmpeg: `make configure-ffmpeg && make build-ffmpeg -j$(nproc)`.
- Configure, compile and install libuam: `make configure-uam && make build-uam`.
- Configure, compile and install mpv: `make configure-mpv && make build-mpv`.
- Finally, compile the main application and build the release distribution: `make dist -j$(nproc)`.
- Output will be found in `build/`.

## Credits
- [Behemoth](https://github.com/HookedBehemoth) for the screenshot button overriding method.
