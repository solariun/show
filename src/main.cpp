// ============================================================================
//  show - view pictures, play video and sound, convert media, all in the
//  terminal.  C++11, FFmpeg (native libraries) + Graphicx.
//  https://github.com/solariun/show
// ----------------------------------------------------------------------------
//  File:   src/main.cpp
//  Layer:  Application
//
//  What this file does
//  -------------------
//  Entry point: parses the command line and runs the chosen command.  For
//  viewing it builds the playlist (folders expand to the media files inside
//  them) and sends every entry to the right mode:
//    picture             -> ImageViewer (zoom, pan, slideshow)
//    .txt terminal movie -> playMovie (Commands.cpp)
//    anything else       -> Player (video, music, GIF, streams)
//
//  How it fits in
//  --------------
//  src/
//    main.cpp           this file
//    Options.*          command line
//    Media.*            FFmpeg: input, decoder, scaler, resampler
//    AudioOutput.*      native sound through libavdevice
//    Player.*           video / music / camera playback (3 threads)
//    ImageViewer.*      pictures
//    Commands.*         info, convert, devices, movie playback
//    Ui.*               layout, status bar, shared keys
//  external/Graphicx    the terminal drawing library (git submodule)
//
//  Contributing
//  ------------
//  The terminal Session must exist only while drawing: commands that print
//  (info, convert, devices) run without it, and errors are printed after it
//  is gone so they stay visible.
// ============================================================================
#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "Commands.h"
#include "ImageViewer.h"
#include "Media.h"
#include "Options.h"
#include "Player.h"

namespace {

const char kVersion[] = "1.0.0";

bool isDirectory(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string lower(std::string s) {
    for (size_t i = 0; i < s.size(); ++i) s[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
    return s;
}

// File name extensions show picks up when a folder is given.
bool looksLikeMedia(const std::string& name) {
    static const char* const kExtensions[] = {
        "jpg", "jpeg", "png", "webp", "bmp", "gif", "tif", "tiff", "avif", "heic", "jxl", "tga", "ppm", "pgm",
        "mp4", "m4v", "mkv", "webm", "mov", "avi", "mpg", "mpeg", "ts", "flv", "wmv", "3gp", "ogv",
        "mp3", "m4a", "aac", "flac", "wav", "ogg", "opus", "wma", "aiff", "txt"};
    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = lower(name.substr(dot + 1));
    for (size_t i = 0; i < sizeof(kExtensions) / sizeof(kExtensions[0]); ++i) {
        if (ext == kExtensions[i]) return true;
    }
    return false;
}

void addFolder(const std::string& folder, std::vector<std::string>& list) {
    DIR* dir = opendir(folder.c_str());
    if (!dir) return;
    std::vector<std::string> names;
    while (dirent* entry = readdir(dir)) {
        std::string name = entry->d_name;
        if (name.empty() || name[0] == '.') continue;
        std::string path = folder + "/" + name;
        if (!isDirectory(path) && looksLikeMedia(name)) names.push_back(path);
    }
    closedir(dir);
    std::sort(names.begin(), names.end());
    list.insert(list.end(), names.begin(), names.end());
}

bool isPicture(const std::string& path) {
    try {
        show::MediaInput input(path);
        return input.isStillImage();
    } catch (const show::MediaError&) {
        return false;  // the player reports the error
    }
}

int view(show::Options& options) {
    std::vector<std::string> playlist;
    for (size_t i = 0; i < options.inputs.size(); ++i) {
        if (isDirectory(options.inputs[i])) {
            addFolder(options.inputs[i], playlist);
        } else {
            playlist.push_back(options.inputs[i]);
        }
    }
    if (playlist.empty()) {
        std::fprintf(stderr, "error: no media files found\n");
        return 1;
    }
    if (options.shuffle) {
        std::mt19937 random(std::random_device{}());
        std::shuffle(playlist.begin(), playlist.end(), random);
    }
    if (!gfx::terminal::isTerminal()) {
        std::fprintf(stderr, "error: show needs a terminal (use `show convert` in scripts)\n");
        return 2;
    }

    std::vector<std::string> errors;
    {
        gfx::terminal::Session session;
        show::Player player(session, options);
        show::ImageViewer viewer(session, options);
        int index = 0;
        const int count = static_cast<int>(playlist.size());
        while (index >= 0 && index < count) {
            const std::string& path = playlist[static_cast<size_t>(index)];
            show::Outcome outcome;
            std::string error;
            if (show::isMovieFile(path)) {
                outcome = show::playMovie(session, path, options);
            } else if (isPicture(path)) {
                outcome = viewer.view(path, index + 1, count);
                error = viewer.error();
            } else {
                outcome = player.play(path, index + 1, count);
                error = player.error();
            }
            if (!error.empty()) errors.push_back(error);
            if (outcome == show::Outcome::Quit) break;
            index += outcome == show::Outcome::Previous ? -1 : 1;
            if (index < 0) index = 0;
            if (index >= count && options.loop && errors.size() < playlist.size()) index = 0;
        }
    }
    for (size_t i = 0; i < errors.size(); ++i) std::fprintf(stderr, "error: %s\n", errors[i].c_str());
    return errors.empty() ? 0 : 1;
}

int camera(show::Options& options) {
    std::string format, device;
    show::defaultCamera(format, device);
    if (!options.cameraFormat.empty()) format = options.cameraFormat;
    if (!options.inputs.empty()) device = options.inputs[0];
    if (!gfx::terminal::isTerminal()) {
        std::fprintf(stderr, "error: show needs a terminal\n");
        return 2;
    }
    std::string error;
    {
        gfx::terminal::Session session;
        show::Player player(session, options);
        player.camera(format, device);
        error = player.error();
    }
    if (!error.empty()) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
#if defined(__APPLE__)
        std::fprintf(stderr, "(macOS: allow your terminal app under System Settings > Privacy & Security > Camera)\n");
#endif
        return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    show::Options options;
    std::string problem = options.parse(argc, argv);
    if (options.help) {
        std::printf("%s", show::Options::usage().c_str());
        return 0;
    }
    if (options.version) {
        std::printf("show %s (FFmpeg %s)\n", kVersion, av_version_info());
        return 0;
    }
    if (!problem.empty()) {
        std::fprintf(stderr, "error: %s\n\n%s", problem.c_str(), show::Options::usage().c_str());
        return 2;
    }
    show::initFFmpeg();
    switch (options.command) {
        case show::Command::Info: return show::runInfo(options);
        case show::Command::Convert: return show::runConvert(options);
        case show::Command::Devices: return show::runDevices();
        case show::Command::Camera: return camera(options);
        default: return view(options);
    }
}
