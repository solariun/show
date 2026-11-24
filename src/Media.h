// ============================================================================
//  show - view pictures, play video and sound, convert media, all in the
//  terminal.  C++11, FFmpeg (native libraries) + Graphicx.
//  https://github.com/solariun/show
// ----------------------------------------------------------------------------
//  File:   src/Media.h
//  Layer:  Media (FFmpeg)
//
//  What this file does
//  -------------------
//  A thin, safe C++ layer over the FFmpeg C libraries, used by every part of
//  show:
//    MediaInput      opens a file, URL or capture device (libavformat /
//                    libavdevice) and finds its video, audio and cover streams
//    Decoder         decodes one stream (libavcodec, send/receive API)
//    VideoScaler     decoded frame -> gfx::Image of any size (libswscale)
//    AudioResampler  decoded audio -> 48 kHz stereo 16-bit samples
//                    (libswresample), the format AudioOutput plays
//    helpers         error text, time formatting, device names
//
//  How it fits in
//  --------------
//  Player, ImageViewer, Convert and Info all start from a MediaInput.  Every
//  FFmpeg object is owned by exactly one of these classes and freed in its
//  destructor (RAII), so callers never call av_*_free themselves.
//
//  FFmpeg notes
//  ------------
//  * Timestamps: each stream has its own time base (a fraction of a
//    second); frame times are converted to seconds with seconds().
//  * Decoding follows the modern API: avcodec_send_packet() then
//    avcodec_receive_frame() until EAGAIN; sending nullptr drains the
//    decoder at the end of the stream.
//  * Works with FFmpeg 6 and newer (AVChannelLayout API).
//
//  Contributing
//  ------------
//  Keep FFmpeg types out of the other headers where possible; add a small
//  wrapper here instead.  C++11 only.
// ============================================================================
#ifndef SHOW_MEDIA_H
#define SHOW_MEDIA_H

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <graphicx/graphicx.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace show {

// Thrown when media cannot be opened or decoded; what() is readable.
class MediaError : public std::runtime_error {
public:
    explicit MediaError(const std::string& message) : std::runtime_error(message) {}
};

// FFmpeg error code -> text ("No such file or directory", ...).
std::string ffmpegError(int code);
// 75.5 -> "01:15", 3725 -> "1:02:05".
std::string formatTime(double seconds);
// One-time FFmpeg setup (devices, network, quiet logging).  Safe to call
// many times.
void initFFmpeg();

// Default capture device for this platform: macOS AVFoundation camera 0,
// Linux V4L2 /dev/video0.
void defaultCamera(std::string& format, std::string& device);

// An opened input: file, URL (http, https, rtsp, ...) or capture device.
class MediaInput {
public:
    // path: file name or URL.  format: force an input format / device
    // ("avfoundation", "v4l2", ...), empty = detect.  options: "key=value"
    // pairs for the demuxer/device (e.g. "framerate=30").
    explicit MediaInput(const std::string& path, const std::string& format = std::string(),
                        const std::vector<std::string>& options = std::vector<std::string>());
    ~MediaInput();

    AVFormatContext* context() const { return context_; }
    const std::string& path() const { return path_; }

    // Stream indexes, -1 if absent.  cover = an attached picture (album art).
    int videoStream() const { return video_; }
    int audioStream() const { return audio_; }
    int coverStream() const { return cover_; }

    double duration() const;    // seconds, 0 if unknown (live / streams)
    double startTime() const;   // seconds of the first timestamp (subtract it)
    bool isLive() const { return live_; }
    // A single picture (JPEG, PNG, WebP, ...): one frame, no audio.
    bool isStillImage() const;
    // Video size with the sample aspect ratio applied (square pixels).
    void displaySize(int& width, int& height) const;
    double frameRate() const;   // frames per second of the video stream, 0 if unknown

    // Seconds of a timestamp in stream `index`'s time base.
    double seconds(int index, int64_t timestamp) const;

private:
    MediaInput(const MediaInput&);
    MediaInput& operator=(const MediaInput&);

    std::string path_;
    AVFormatContext* context_;
    int video_, audio_, cover_;
    bool live_;
};

// Decodes one stream of a MediaInput.
class Decoder {
public:
    Decoder(const MediaInput& input, int streamIndex);
    ~Decoder();

    AVCodecContext* context() const { return context_; }
    int stream() const { return stream_; }

    // Feeds a packet (nullptr = end of stream, drain).  False on a decode error
    // that should just be skipped (corrupt packet).
    bool send(const AVPacket* packet);
    // Next decoded frame; false when the decoder needs more input (or is done).
    bool receive(AVFrame* frame);
    // Forget buffered state (after a seek).
    void flush();

private:
    Decoder(const Decoder&);
    Decoder& operator=(const Decoder&);
    AVCodecContext* context_;
    int stream_;
};

// Converts decoded frames to gfx::Image (RGB24) of a chosen size.
class VideoScaler {
public:
    VideoScaler();
    ~VideoScaler();
    // Scales `frame` to width x height pixels into `out` (resized as needed).
    // Uses an area-averaging filter: smooth when shrinking, sharp enough when
    // enlarging.
    void scale(const AVFrame* frame, int width, int height, gfx::Image& out);

private:
    VideoScaler(const VideoScaler&);
    VideoScaler& operator=(const VideoScaler&);
    SwsContext* context_;
    std::vector<std::uint8_t> buffer_;
};

// Converts decoded audio of any format to 48 kHz, stereo, signed 16-bit
// interleaved samples.
class AudioResampler {
public:
    static const int kRate = 48000;
    static const int kChannels = 2;

    AudioResampler();
    ~AudioResampler();
    // Appends the converted samples of `frame` to `out` (2 values per sample
    // frame: left, right).
    void convert(const AVFrame* frame, std::vector<std::int16_t>& out);
    void reset();  // after a seek

private:
    AudioResampler(const AudioResampler&);
    AudioResampler& operator=(const AudioResampler&);
    SwrContext* context_;
    int inRate_, inFormat_;
    AVChannelLayout inLayout_;
};

}  // namespace show

#endif
