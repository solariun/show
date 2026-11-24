// ============================================================================
//  show - view pictures, play video and sound, convert media, all in the
//  terminal.  C++11, FFmpeg (native libraries) + Graphicx.
//  https://github.com/solariun/show
// ----------------------------------------------------------------------------
//  File:   src/Options.cpp
//  Layer:  Application
//
//  What this file does
//  -------------------
//  Implementation of Options (see Options.h): defaults, the parser and the
//  --help text.
//
//  How it fits in
//  --------------
//  The first word may be a command (camera, info, convert, devices); without
//  one, every argument that is not an option is a file or URL to show.
//
//  Contributing
//  ------------
//  Keep usage() and README.md in sync.
// ============================================================================
#include "Options.h"

#include <cstdlib>

namespace show {

Options::Options()
    : command(Command::View),
      style(gfx::Style::Blocks),
      colorMode(gfx::ColorMode::Palette256),
      invert(false),
      contrast(0),
      brightness(0),
      audio(true),
      muted(false),
      volume(1.0),
      loop(false),
      start(0),
      slideshow(0),
      shuffle(false),
      cameraFps(30),
      width(0),
      at(-1),
      fps(0),
      duration(0),
      help(false),
      version(false) {}

bool parseStyle(const std::string& name, gfx::Style& style) {
    if (name == "1" || name == "full") {
        style = gfx::Style::Blocks;
        return true;
    }
    if (name == "2") {
        style = gfx::Style::Ascii;
        return true;
    }
    if (name == "3") {
        style = gfx::Style::AsciiColor;
        return true;
    }
    return gfx::parseStyle(name, style);  // blocks, ascii, ascii-color, bw, ...
}

std::string Options::parse(int argc, char** argv) {
    int i = 1;
    if (argc > 1) {
        std::string first = argv[1];
        if (first == "camera") {
            command = Command::Camera;
            ++i;
        } else if (first == "info") {
            command = Command::Info;
            ++i;
        } else if (first == "convert") {
            command = Command::Convert;
            ++i;
        } else if (first == "devices") {
            command = Command::Devices;
            ++i;
        }
    }
    for (; i < argc; ++i) {
        std::string a = argv[i];
        bool hasValue = i + 1 < argc;
        if (a == "-h" || a == "--help") {
            help = true;
        } else if (a == "-V" || a == "--version") {
            version = true;
        } else if ((a == "-s" || a == "--style") && hasValue) {
            if (!show::parseStyle(argv[++i], style)) {
                return std::string("unknown style ") + argv[i] + " (full, ascii, ascii-color or 1, 2, 3)";
            }
        } else if (a == "-t" || a == "--truecolor") {
            colorMode = gfx::ColorMode::TrueColor;
        } else if (a == "--invert") {
            invert = true;
        } else if (a == "--contrast" && hasValue) {
            if (!gfx::parsePercent(argv[++i], contrast)) {
                return std::string("--contrast takes a percentage, e.g. 10% or -10%, not ") + argv[i];
            }
        } else if (a == "--brightness" && hasValue) {
            if (!gfx::parsePercent(argv[++i], brightness)) {
                return std::string("--brightness takes a percentage, e.g. 10% or -10%, not ") + argv[i];
            }
        } else if (a == "--no-audio") {
            audio = false;
        } else if (a == "--mute") {
            muted = true;
        } else if (a == "--volume" && hasValue) {
            volume = std::atof(argv[++i]) / 100.0;
            if (volume < 0) volume = 0;
            if (volume > 2) volume = 2;
        } else if (a == "--loop") {
            loop = true;
        } else if (a == "--start" && hasValue) {
            start = std::atof(argv[++i]);
        } else if (a == "--audio-device" && hasValue) {
            audioDevice = argv[++i];
        } else if (a == "--slideshow" && hasValue) {
            slideshow = std::atof(argv[++i]);
        } else if (a == "--shuffle") {
            shuffle = true;
        } else if (a == "--camera-format" && hasValue) {
            cameraFormat = argv[++i];
        } else if (a == "--camera-size" && hasValue) {
            cameraSize = argv[++i];
        } else if (a == "--camera-fps" && hasValue) {
            cameraFps = std::atoi(argv[++i]);
        } else if ((a == "-o" || a == "--output") && hasValue) {
            output = argv[++i];
        } else if ((a == "-w" || a == "--width") && hasValue) {
            width = std::atoi(argv[++i]);
        } else if (a == "--at" && hasValue) {
            at = std::atof(argv[++i]);
        } else if (a == "--fps" && hasValue) {
            fps = std::atof(argv[++i]);
        } else if (a == "--duration" && hasValue) {
            duration = std::atof(argv[++i]);
        } else if (a.size() > 1 && a[0] == '-') {
            return "unknown option " + a;
        } else {
            inputs.push_back(a);
        }
    }
    if (help || version) return std::string();
    if (command == Command::View && inputs.empty()) return "nothing to show: give a file or URL (see --help)";
    if ((command == Command::Info || command == Command::Convert) && inputs.empty()) return "missing input file";
    if (command == Command::Convert && output.empty()) return "convert needs -o OUTPUT";
    return std::string();
}

std::string Options::usage() {
    return "show - pictures, video and sound in the terminal (FFmpeg + Graphicx)\n"
           "\n"
           "usage:\n"
           "  show [options] FILE|URL...          view pictures, play video / music / animations,\n"
           "                                      play terminal movies (.txt)\n"
           "  show camera [DEVICE] [options]      live camera (macOS: 0, Linux: /dev/video0)\n"
           "  show info FILE|URL...               container, streams, codecs, tags\n"
           "  show convert INPUT -o OUTPUT [opts] picture/frame -> .txt / .png / .jpg,\n"
           "                                      video -> .txt movie (play it with show or cat)\n"
           "  show devices                        cameras and audio outputs available\n"
           "\n"
           "display:\n"
           "  -s, --style S         1 or full        colored half blocks (default)\n"
           "                        2 or ascii       plain characters, no colors\n"
           "                        3 or ascii-color colored characters, no background\n"
           "  -t, --truecolor       24-bit colors instead of the 256-color palette\n"
           "      --invert          ASCII styles on light terminal backgrounds\n"
           "      --contrast P      contrast change in percent, e.g. 10% or -10%\n"
           "      --brightness P    brightness change in percent, e.g. 10% or -10%\n"
           "\n"
           "playback:\n"
           "      --no-audio        no sound\n"
           "      --mute            start muted\n"
           "      --volume N        0-200 (%, default 100)\n"
           "      --loop            repeat (playlist or single file)\n"
           "      --start SECONDS   start position\n"
           "      --shuffle         random playlist order\n"
           "      --slideshow SEC   pictures: next one every SEC seconds\n"
           "      --audio-device D  audiotoolbox | pulse | alsa (default: automatic)\n"
           "\n"
           "camera:\n"
           "      --camera-size WxH  --camera-fps N  --camera-format F (avfoundation | v4l2)\n"
           "\n"
           "convert:\n"
           "  -o, --output FILE     .txt (terminal text or movie), .png, .jpg\n"
           "  -w, --width N         width in characters (default 80; pictures for .png/.jpg: pixels)\n"
           "      --at SECONDS      take one frame of a video\n"
           "      --fps N           movie frame rate (default: the video's)\n"
           "      --duration SEC    convert only this much\n"
           "\n"
           "keys while playing:\n"
           "  space pause   left/right -/+5 s   up/down +/-30 s   0-9 jump to 0%..90%\n"
           "  + / - volume   m mute   s style   c colors   [ / ] contrast   { / } brightness\n"
           "  i info   l loop\n"
           "  n / p next / previous file   q or Esc quit\n"
           "keys while viewing pictures:\n"
           "  + / - zoom   arrows move   0 fit   s style   c colors   [ / ] contrast   { / } brightness\n"
           "  i info\n"
           "  n / p (or space) next / previous   q or Esc quit\n";
}

}  // namespace show
