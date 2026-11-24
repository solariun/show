// ============================================================================
//  show - view pictures, play video and sound, convert media, all in the
//  terminal.  C++11, FFmpeg (native libraries) + Graphicx.
//  https://github.com/solariun/show
// ----------------------------------------------------------------------------
//  File:   src/Options.h
//  Layer:  Application
//
//  What this file does
//  -------------------
//  The command line: which command to run (view / play, camera, info,
//  convert, devices), the files, and every setting, parsed into one
//  Options value.  usage() is the --help text.
//
//  How it fits in
//  --------------
//  main.cpp parses once and passes the Options to the command.  The viewer
//  and player keep a reference and change some fields live (style, colors,
//  volume, mute, loop) so the choice carries over to the next file of a
//  playlist.
//
//  Contributing
//  ------------
//  A new option needs: a field with its default in the constructor, a
//  branch in parse(), a line in usage(), and a mention in README.md.
// ============================================================================
#ifndef SHOW_OPTIONS_H
#define SHOW_OPTIONS_H

#include <graphicx/graphicx.h>

#include <string>
#include <vector>

namespace show {

enum class Command { View, Camera, Info, Convert, Devices };

// -s values: "1" / "full" / "blocks" (half blocks), "2" / "ascii",
// "3" / "ascii-color".  False if the name is unknown.
bool parseStyle(const std::string& name, gfx::Style& style);

struct Options {
    Options();

    Command command;
    std::vector<std::string> inputs;  // files, URLs (or the camera device)

    // Display
    gfx::Style style;          // -s 1|full, 2|ascii, 3|ascii-color
    gfx::ColorMode colorMode;  // -t / --truecolor
    bool invert;               // --invert (ASCII on light backgrounds)
    double contrast;           // --contrast P%: 0.10 = +10%, -0.10 = -10% ([ and ] keys)

    // Playback
    bool audio;                // --no-audio turns sound off entirely
    bool muted;                // --mute (m key)
    double volume;             // --volume 0..200 (%), stored as 0..2
    bool loop;                 // --loop (l key)
    double start;              // --start SECONDS
    std::string audioDevice;   // --audio-device NAME
    double slideshow;          // --slideshow SECONDS for pictures, 0 = off
    bool shuffle;              // --shuffle the playlist

    // Camera
    std::string cameraFormat;  // --camera-format (default per platform)
    std::string cameraSize;    // --camera-size WxH (e.g. 1280x720)
    int cameraFps;             // --camera-fps N

    // Convert
    std::string output;        // -o FILE
    int width;                 // -w / --width N characters (0 = automatic)
    double at;                 // --at SECONDS: one frame of a video (-1 = not set)
    double fps;                // --fps N for video -> movie (0 = source rate)
    double duration;           // --duration SECONDS to convert (0 = all)

    bool help;
    bool version;

    // Returns an error message, or "" when the command line is valid.
    std::string parse(int argc, char** argv);
    static std::string usage();
};

}  // namespace show

#endif
