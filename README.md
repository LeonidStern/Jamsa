# Jamsa

Jam online from Ableton Live, or any other VST3 host. Jamsa is a [NINJAM](https://www.cockos.com/ninjam/) client as a VST3 plug-in, with a NINJAM server built in. One player clicks **Host**, the others join, and everyone plays in time, one interval apart.

## Download

| System | Download |
|---|---|
| Windows 10 and 11 (64-bit) | [Jamsa-vst3-windows-x64.zip](https://github.com/LeonidStern/Jamsa/releases/download/jamsa-latest/Jamsa-vst3-windows-x64.zip) |
| macOS 12 and later (Apple silicon and Intel) | [Jamsa-vst3-macos.dmg](https://github.com/LeonidStern/Jamsa/releases/download/jamsa-latest/Jamsa-vst3-macos.dmg) |
| Linux (x86-64) | [Jamsa-vst3-linux-x86_64.zip](https://github.com/LeonidStern/Jamsa/releases/download/jamsa-latest/Jamsa-vst3-linux-x86_64.zip) |

These are the newest builds from `main`, in the [jamsa-latest release](https://github.com/LeonidStern/Jamsa/releases/tag/jamsa-latest). CI builds and tests them on every push.

## Install

Jamsa is one `Jamsa.vst3`. On Windows that's a single file; on macOS and Linux it's a bundle folder. If you still have the earlier `NINJAM.vst3`, delete it: Jamsa replaces it, and sets made with it open with Jamsa.

**Windows**
1. Unzip the download.
2. Copy `Jamsa.vst3` to `C:\Program Files\Common Files\VST3`.
3. In Ableton, open Settings (Preferences in Live 11), then Plug-Ins. Turn on *Use VST3 Plug-In System Folders* and click *Rescan*.

**macOS**
1. Open `Jamsa-vst3-macos.dmg`.
2. Drag `Jamsa.vst3` onto the *VST3 Plug-Ins* folder next to it. That folder is `/Library/Audio/Plug-Ins/VST3`, where most plug-in installers put theirs; if it isn't there yet, make it in Finder first.
3. The build isn't notarized by Apple, so macOS blocks it until you remove the download flag. In Terminal, run this once:
   ```
   xattr -dr com.apple.quarantine /Library/Audio/Plug-Ins/VST3/Jamsa.vst3
   ```
4. In Ableton, open Settings (Preferences in Live 11), then Plug-Ins. Turn on *Use VST3 Plug-In System Folders* and click *Rescan*.

**Linux**
1. Unzip the download into `~/.vst3`, so you have `~/.vst3/Jamsa.vst3`.
2. Rescan plug-ins in your host (Bitwig Studio, REAPER, Ardour and others).

The editor uses X11. It needs the usual desktop libraries: xcb, cairo, pango and fontconfig.

## Play

1. Put Jamsa on an audio track with your instrument or microphone as the input, and set monitoring to In.
2. **To host**, click **Host** in the bottom bar, and use **Copy** to send players the WAN address.
3. **To join**, type the address under Server and click **Connect**.
4. These settings belong to the room, so changing one changes it for everyone:
   - **BPM**: drag it, or double-click and type.
   - **BPI**: beats per interval, 2 to 64. A room starts at 4.
   - **Codec**: Opus, or lossless FLAC, which needs several times the bandwidth.
   - **Opus bitrate**: 64 to 510 kbps. A new Jamsa starts at 510, the best Opus sounds.
5. **Sync** keeps your DAW and the room in time, in any VST3 host. When you change your DAW's tempo, you vote the room to it, and while it plays, intervals start on its bars. If your DAW has Ableton Link, turn Link on there too: then the room's tempo moves your DAW's, whoever votes. In Live that's the Link button at the top left (if it isn't there, turn on *Show Link Toggle* in Live's Link settings).
6. You can also type room commands in the chat: `!vote bpm 120`, `!vote bpi 16`, `!codec opus` or `!codec flac`, and `!bitrate 128`.

Hosting tips:
- Jamsa asks your router to forward its port (UPnP or NAT-PMP). If the router doesn't allow that, forward TCP port 2049 to your computer yourself (Jamsa tries ports 2049 to 2058). The WAN address still shows: Jamsa asks a "what's my IP" service (api.ipify.org) for it. Jamsa can't see a port you forwarded yourself, so it says *Forward port* until the first player joins from the internet, then *Port open*.
- The host's connection sends every player everyone else's audio. With 4 players at 510 kbps, that's about 4.6 Mbit/s of upload; at 128 kbps, about 1.2 Mbit/s.
- The room closes when the host closes Ableton. For a room that stays up, run the server on a machine that's always on (see below).

## Build from source

```
cmake -S ninjam/vst3 -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target Jamsa
```

CMake fetches the VST3 SDK. The plug-in ends up in `build/VST3/Release/Jamsa.vst3`. On Linux, install the packages listed in [the CI workflow](.github/workflows/ninjam-vst3.yml) first. Add `-DNINJAM_VST3_TESTS=ON` to build the tests, then run them with `ctest --test-dir build`.

## Run a server

The NINJAM server is in [ninjam/server](ninjam/server):
- Build it with `make -C ninjam/server`.
- Start it with `ninjam/server/ninjamsrv ninjam/server/example.cfg`.
- To run it in Docker, see [ninjam/server/Dockerfile](ninjam/server/Dockerfile).

[example.cfg](ninjam/server/example.cfg) is a commented example. These settings control audio:
- `AllowCodecs` and `RequireCodec` set which codecs the server allows.
- `OpusBitrate` sets the room's Opus bitrate.

## About NINJAM

This repository is a fork of [NINJAM](https://www.cockos.com/ninjam/) by Cockos. The original clients in `ninjam/` are old and probably need some attention to work. The REAPER ReaNINJAM plug-in is in `jmde/fx/reaninjam/` and needs the VST2 SDK to build.

Jamsa and NINJAM are free software under the GNU General Public License, version 2 or later (see [LICENSE](LICENSE)).
