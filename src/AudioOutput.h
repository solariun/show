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
//  the master clock that video follows.  playing() tells which moment of
//  the media is being heard right now (what was sent minus what is still
//  queued in the device), which the Player's clock uses for lip-sync.
//
//  Why fixed-size packets
//  ----------------------
//  FFmpeg's AudioToolbox output keeps only TWO buffers, each exactly the
//  size of the last packet, and re-allocates a buffer whenever the packet
//  size changes.  Writing decoded chunks as they come (sizes vary, small
//  leftovers of a few ms) leaves almost nothing queued at times: the device
//  runs dry and the sound crackles.  So samples are collected in pending_
//  and always sent as packets of kPacketFrames (85 ms): two buffers then
//  always hold ~170 ms, a comfortable margin, and nothing is re-allocated.
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

#include <chrono>
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

    // Sound is sent to the device in packets of this many sample frames.
    static const size_t kPacketFrames = 4096;  // 85 ms at 48 kHz

    // Queues `frames` sample frames (2 int16 each: left, right) at `volume`
    // (0 = silent, 1 = unchanged, up to 2).  `pts` is the media time of the
    // first sample (seconds).  Full packets go to the device; this blocks
    // while the device buffers are full.  False if the device failed.
    bool write(const std::int16_t* samples, size_t frames, double volume, double pts);
    // Sends what is still pending, padded with silence (end of the media).
    bool finish();
    // Drops pending samples (after a seek); sound already in the device
    // still plays.
    void discard();

    // Media time heard right now, if any sound was sent: the end of what
    // was sent minus what is still queued in the device.
    bool playing(double& pts) const;
    // Same as data: end of sent sound (seconds) and when the device queue
    // runs dry.  For a clock that interpolates between writes.
    void state(double& sentEnd, std::chrono::steady_clock::time_point& drainsAt) const;
    bool hasSent() const { return sent_; }
    // Times the device ran out of sound while playing (each one is an
    // audible click); 0 when playback is clean.
    long underruns() const { return underruns_; }

private:
    bool sendPacket(const std::int16_t* data);
    AudioOutput(const AudioOutput&);
    AudioOutput& operator=(const AudioOutput&);
    bool openDevice(const std::string& device);

    AVFormatContext* context_;
    std::int64_t pts_;  // sample frames sent so far (packet timestamps)
    std::string name_;
    std::string error_;
    std::vector<std::int16_t> pending_;  // volume-adjusted samples not sent yet
    double pendingPts_;                  // media time of pending_[0]
    double sentEnd_;                     // media time right after the last sent sample
    bool sent_;                          // anything sent since open / reset
    long underruns_;
    // When the device will have played everything sent (model, see .cpp).
    std::chrono::steady_clock::time_point drainsAt_;
};

}  // namespace show

#endif
