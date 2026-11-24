// ============================================================================
//  show - view pictures, play video and sound, convert media, all in the
//  terminal.  C++11, FFmpeg (native libraries) + Graphicx.
//  https://github.com/solariun/show
// ----------------------------------------------------------------------------
//  File:   src/Player.h
//  Layer:  Playback
//
//  What this file does
//  -------------------
//  Declares Player, which plays anything with a timeline in the terminal:
//  videos (with sound), music (album art or a live spectrum), animated
//  GIF / WebP, network streams, and live cameras.
//
//  How it fits in
//  --------------
//  main.cpp creates one Player for the whole session and calls play() for
//  each playlist entry (or camera() once).  Inside play() three threads
//  cooperate:
//
//    reader  (Player.cpp, readLoop)  demux packets -> decode -> scale video
//            frames to exactly the terminal grid -> VideoFrame queue;
//            audio -> resample -> AudioChunk queue.  Performs seeks.
//    audio   (audioLoop)  AudioChunk -> volume -> AudioOutput::write(),
//            which blocks at playback speed; updates the Clock.
//    main    (play's loop)  shows each VideoFrame when the Clock reaches its
//            time (late frames are dropped), handles keys, draws the
//            status bar and the info box.
//
//  Clock: with sound, time = position of the sound already handed to the
//  device (sound is never interrupted, the picture follows it); without
//  sound, time = wall clock since the first frame.
//
//  Contributing
//  ------------
//  * Every queue item carries the "serial" of the seek generation it
//    belongs to; after a seek, items of older serials are thrown away.
//    Keep that rule when adding queues.
//  * Never block the main thread: it must keep reading keys.
// ============================================================================
#ifndef SHOW_PLAYER_H
#define SHOW_PLAYER_H

#include <graphicx/terminal.h>

#include <string>

#include "Options.h"
#include "Ui.h"

namespace show {

class Player {
public:
    Player(gfx::terminal::Session& session, Options& options);

    // Plays one file or URL.  position / count = place in the playlist (for
    // the status bar).  Returns what the user chose (next, previous, quit);
    // a file that ends returns Next (or plays again with --loop).
    Outcome play(const std::string& path, int position, int count);

    // Live camera (format: avfoundation / v4l2, device: "0", "/dev/video0").
    Outcome camera(const std::string& format, const std::string& device);

    // Error text after play() failed to open a file (shown by main).
    const std::string& error() const { return error_; }

private:
    gfx::terminal::Session& session_;
    Options& options_;
    std::string error_;
};

}  // namespace show

#endif
