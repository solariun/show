// ============================================================================
//  show - view pictures, play video and sound, convert media, all in the
//  terminal.  C++11, FFmpeg (native libraries) + Graphicx.
//  https://github.com/solariun/show
// ----------------------------------------------------------------------------
//  File:   src/AudioOutput.h
//  Layer:  Media (FFmpeg)
//
//  What this file does
//  -------------------
//  Plays sound on the computer's speakers through FFmpeg's own native audio
//  output devices (libavdevice), so show needs no other audio library:
//    macOS  "audiotoolbox"  (Apple AudioToolbox / AudioQueue)
//    Linux  "pulse"         (PulseAudio / PipeWire), else "alsa"
//  It accepts 48 kHz stereo 16-bit samples (what AudioResampler produces).
//
//  How it fits in
//  --------------
//  The Player's audio thread owns one and calls write() with each chunk of
//  decoded sound.  write() blocks while the device buffer is full, so the
//  audio thread runs exactly at playback speed: that is what makes sound
//  the master clock that video follows.
//
//  FFmpeg notes
//  ------------
//  An output device is used like a muxer: one PCM stream, a header, then
//  packets of raw samples (av_write_frame), then a trailer.  The stream must
//  say its sample format (codecpar->format) or AudioToolbox refuses it.
//
//  Contributing
//  ------------
//  Volume is applied to the samples before writing (portable; device volume
//  controls differ per platform).
// ============================================================================
#ifndef SHOW_AUDIOOUTPUT_H
#define SHOW_AUDIOOUTPUT_H

#include <cstdint>
#include <string>
#include <vector>

struct AVFormatContext;

namespace show {

class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();

    // Names of the audio output devices this FFmpeg build provides.
    static std::vector<std::string> devices();

    // Opens `device` ("" = the platform default from the list above).
    // Returns false (with error() set) if no device works.
    bool open(const std::string& device = std::string());
    void close();
    bool isOpen() const { return context_ != 0; }
    const std::string& name() const { return name_; }
    const std::string& error() const { return error_; }

    // Plays `frames` sample frames (2 int16 each: left, right) at `volume`
    // (0 = silent, 1 = unchanged, up to 2).  Blocks while the device buffer
    // is full.  False if the device failed.
    bool write(const std::int16_t* samples, size_t frames, double volume);

private:
    AudioOutput(const AudioOutput&);
    AudioOutput& operator=(const AudioOutput&);
    bool openDevice(const std::string& device);

    AVFormatContext* context_;
    std::int64_t pts_;  // sample frames written so far
    std::string name_;
    std::string error_;
    std::vector<std::int16_t> scaled_;  // volume-adjusted copy, reused
};

}  // namespace show

#endif
