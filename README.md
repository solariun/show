# show

Pictures, video **with sound**, music, animations and live cameras, right in
your terminal. show also converts media to terminal text, PNG, JPEG and
playable "terminal movies".

show is written in **C++11** and links the **FFmpeg libraries natively**
(libavformat, libavcodec, libswscale, libswresample, libavdevice), so it
plays anything FFmpeg can read. It runs on **macOS and Linux**. The pictures
are drawn by [Graphicx](https://github.com/solariun/Graphicx), in three
styles:

| `-s` | Style | Looks like |
|------|-------|------------|
| `1` or `full` | half blocks | `▀` with a foreground and a background color: 2 pixels per character (default) |
| `2` or `ascii` | plain ASCII | ` .:-=+*#%@` by brightness, no colors at all |
| `3` or `ascii-color` | colored ASCII | the same characters in the picture's colors, no background |

Add `-t` / `--truecolor` for 24-bit color, and `--contrast 10%` (or
`--contrast -10%`) to adjust contrast.

## Install

```bash
git clone --recursive https://github.com/solariun/show.git
cd show
./env.sh
```

`env.sh` does everything:
1. It installs the dependencies. On macOS it uses Homebrew (`ffmpeg`,
   `pkg-config`, `git`). On Linux it uses apt, dnf, pacman, zypper or apk to
   install a C++ compiler, make, pkg-config, git and the FFmpeg development
   libraries.
2. It fetches the Graphicx submodule.
3. It builds `./show`.

Options:
- `./env.sh --install` also copies show to `/usr/local/bin`.
- `./env.sh --no-build` only prepares the environment.

Afterwards a plain `make` rebuilds.

## Use

```bash
show photo.jpg                     # picture viewer (zoom, pan)
show movie.mp4                     # video with sound
show music.mp3                     # album art + live spectrum
show animation.gif                 # animations loop
show ~/Pictures                    # a whole folder as a playlist
show a.jpg b.mp4 c.mp3 --shuffle --loop
show https://example.com/clip.mp4  # anything FFmpeg can open, streams too
show camera                        # live camera (macOS: 0, Linux: /dev/video0)
show info movie.mp4                # container, streams, codecs, tags
show devices                       # audio outputs and cameras
```

### Keys

| While playing | |
|---|---|
| space | pause / resume |
| ← / → | back / forward 5 s |
| ↓ / ↑ | back / forward 30 s |
| 0 … 9 | jump to 0% … 90% |
| + / − | volume |
| m | mute |
| s | next style (full → ascii-color → ascii) |
| c | 256 colors ↔ 24-bit |
| [ / ] | contrast −5% / +5% |
| i | information box |
| l | loop on / off |
| n / p | next / previous file |
| q or Esc | quit |

**While viewing pictures:**
- `+` / `−` zoom in or out (the zoom shows real detail from the full
  picture).
- Arrows move around, and `0` fits the picture back.
- `s`, `c`, `[` / `]` and `i` work as in the player.
- `n`, space or Enter go to the next file, `p` to the previous one, and `q`
  quits.
- `--slideshow 5` moves to the next picture every 5 s.

### Options

```
display:   -s 1|2|3|full|ascii|ascii-color   -t/--truecolor   --invert   --contrast P
playback:  --no-audio  --mute  --volume 0-200  --loop  --start SEC  --shuffle
           --slideshow SEC  --audio-device audiotoolbox|pulse|alsa
camera:    --camera-size WxH  --camera-fps N  --camera-format avfoundation|v4l2
```

## Convert

```bash
show convert photo.jpg -o photo.txt -w 100            # picture -> terminal text
show convert photo.jpg -o photo.txt -s 2              # plain ASCII art
show convert movie.mp4 --at 60 -o frame.png           # the frame at 1:00 -> PNG
show convert movie.mp4 --at 60 -o frame.jpg -w 640    # ... JPEG, 640 pixels wide
show convert movie.mp4 -o movie.txt -s 3 -w 80        # video -> terminal movie
show convert movie.mp4 -o clip.txt --duration 10 --fps 12
show movie.txt                                        # play it back (or: cat movie.txt)
```

A **terminal movie** is plain terminal output, so it works with any terminal
and with `cat`:
- **Header:** `ESC ] 9999 ; show-movie fps=N ESC \` gives the frame rate.
  Terminals ignore it.
- **Frames:** each frame holds only the characters that changed, wrapped in
  `CSI ?2026h … CSI ?2026l`.

show also plays the movies recorded by
[TBoyEmu](https://github.com/solariun/TBoyEmu) with its R key.

## How it works

```
                 ┌───────────── reader thread ─────────────┐
 file / URL ──►  │ libavformat → libavcodec → libswscale ──┼──► video frames ──┐
 camera          │                         → libswresample ┼──► sound chunks ─┐│
                 └─────────────────────────────────────────┘                  ││
                         audio thread: volume → libavdevice audio output ◄────┘│
                         (AudioToolbox / PulseAudio / ALSA) → the clock        │
                         main thread: show each frame when the clock reaches ◄─┘
                         it (late ones are dropped), keys, status bar
                         → Graphicx gfx::Screen → only changed cells → terminal
```

- **Clock:** sound is the master clock. The audio device accepts samples at
  playback speed, so video follows the sound already handed to it. Without
  sound, the wall clock drives playback.
- **Seeking:** seeks are frame-accurate. Frames before the target are decoded
  but not shown, and a "serial" number throws away everything from before the
  seek.
- **Scaling:** frames are scaled by FFmpeg to exactly the character grid
  that fits the terminal. A window resize takes effect at once.

| File | Role |
|------|------|
| `src/Media.*` | FFmpeg wrappers: MediaInput, Decoder, VideoScaler, AudioResampler |
| `src/AudioOutput.*` | native sound through libavdevice |
| `src/Player.*` | video, music, GIF and camera playback (3 threads) |
| `src/ImageViewer.*` | picture viewer, `loadPicture()` |
| `src/Commands.*` | info, convert, devices, terminal movies |
| `src/Ui.*` | layout, status bar, info box, shared keys |
| `src/Options.*` | command line |
| `external/Graphicx` | terminal drawing library (git submodule) |

Every source file starts with a header that explains what it does and how it
fits in.

## Notes

- **Sound:** show uses FFmpeg's own native outputs: AudioToolbox on macOS,
  PulseAudio or ALSA on Linux. `show devices` lists what your FFmpeg build
  has. Without one, video plays silently.
- **Camera:** on macOS, allow your terminal app under *System Settings →
  Privacy & Security → Camera*. On Linux, you need read access to
  `/dev/video*` (the `video` group).
- **Speed:** full-color blocks in a very large window can be a lot of text
  per frame. If frames drop (the status bar shows `drop N`), use a smaller
  window or an ASCII style.
- **Terminals:** terminals with synchronized output (kitty, iTerm2, WezTerm,
  Ghostty and others) never show a half-drawn frame.

## Contributing

- **Code:** C++11 only. The build uses `-std=c++11 -Wall -Wextra -Wpedantic`
  and must stay warning-free.
- **Ownership:** FFmpeg objects are owned by the wrappers in `Media.*` (RAII).
- **Testing:** try a video with sound, a music file, a GIF, a picture,
  seeking while paused, and resizing the terminal.

## License

MIT, see [LICENSE](LICENSE). FFmpeg is LGPL/GPL, depending on how your copy
was built; show links it dynamically.
