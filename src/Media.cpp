// ============================================================================
//  show - view pictures, play video and sound, convert media, all in the
//  terminal.  C++11, FFmpeg (native libraries) + Graphicx.
//  https://github.com/solariun/show
// ----------------------------------------------------------------------------
//  File:   src/Media.cpp
//  Layer:  Media (FFmpeg)
//
//  What this file does
//  -------------------
//  Implementation of the FFmpeg wrappers declared in Media.h.
//
//  How it fits in
//  --------------
//  MediaInput picks the streams once (av_find_best_stream), Decoder opens
//  a codec for one of them with automatic multithreading, VideoScaler and
//  AudioResampler cache their libswscale / libswresample contexts and only
//  rebuild them when the input format or the requested size changes.
//
//  FFmpeg notes
//  ------------
//  * Still images are recognized by their demuxer ("image2", "*_pipe") or a
//    video stream that holds a single frame; animated GIF / WebP / APNG are
//    treated as video so they animate.
//  * The sample aspect ratio (SAR) matters for some DVDs and anamorphic
//    video: displaySize() returns the size as it should look.
//
//  Contributing
//  ------------
//  Every FFmpeg allocation here has its matching free in a destructor; keep
//  it that way (no naked av_*_alloc in other files).
// ============================================================================
#include "Media.h"

extern "C" {
#include <libavdevice/avdevice.h>
#include <libavutil/imgutils.h>
}

#include <cstdio>
#include <cstring>

namespace show {

// --------------------------------------------------------------- helpers ----

std::string ffmpegError(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(code, text, sizeof(text));
    return text;
}

std::string formatTime(double seconds) {
    if (seconds < 0 || seconds != seconds) seconds = 0;
    long total = static_cast<long>(seconds);
    char text[32];
    if (total >= 3600) {
        std::snprintf(text, sizeof(text), "%ld:%02ld:%02ld", total / 3600, (total / 60) % 60, total % 60);
    } else {
        std::snprintf(text, sizeof(text), "%02ld:%02ld", total / 60, total % 60);
    }
    return text;
}

void initFFmpeg() {
    static bool done = false;
    if (done) return;
    done = true;
    av_log_set_level(AV_LOG_QUIET);  // messages would draw over the picture
    avdevice_register_all();
    avformat_network_init();
}

void defaultCamera(std::string& format, std::string& device) {
#if defined(__APPLE__)
    format = "avfoundation";
    device = "0";  // first camera, no audio
#else
    format = "v4l2";
    device = "/dev/video0";
#endif
}

// ------------------------------------------------------------ MediaInput ----

MediaInput::MediaInput(const std::string& path, const std::string& format, const std::vector<std::string>& options)
    : path_(path), context_(0), video_(-1), audio_(-1), cover_(-1), live_(false) {
    initFFmpeg();
    const AVInputFormat* inputFormat = 0;
    if (!format.empty()) {
        inputFormat = av_find_input_format(format.c_str());
        if (!inputFormat) throw MediaError("unknown input format or device: " + format);
        live_ = true;  // forced formats are used for capture devices
    }
    AVDictionary* dict = 0;
    for (size_t i = 0; i < options.size(); ++i) {
        size_t eq = options[i].find('=');
        if (eq != std::string::npos) {
            av_dict_set(&dict, options[i].substr(0, eq).c_str(), options[i].substr(eq + 1).c_str(), 0);
        }
    }
    int rc = avformat_open_input(&context_, path.c_str(), inputFormat, &dict);
    av_dict_free(&dict);
    if (rc < 0) {
        context_ = 0;
        throw MediaError(path + ": " + ffmpegError(rc));
    }
    rc = avformat_find_stream_info(context_, 0);
    if (rc < 0) {
        avformat_close_input(&context_);
        throw MediaError(path + ": " + ffmpegError(rc));
    }

    // Album art is a video stream flagged "attached picture": keep it apart
    // so audio files are not mistaken for video.
    for (unsigned i = 0; i < context_->nb_streams; ++i) {
        if (context_->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC) {
            cover_ = static_cast<int>(i);
            break;
        }
    }
    int v = av_find_best_stream(context_, AVMEDIA_TYPE_VIDEO, -1, -1, 0, 0);
    if (v >= 0 && v != cover_ && !(context_->streams[v]->disposition & AV_DISPOSITION_ATTACHED_PIC)) video_ = v;
    int a = av_find_best_stream(context_, AVMEDIA_TYPE_AUDIO, -1, video_, 0, 0);
    if (a >= 0) audio_ = a;
    if (video_ < 0 && audio_ < 0 && cover_ < 0) {
        avformat_close_input(&context_);
        throw MediaError(path + ": no picture or sound found");
    }
    // Discard the streams we never read (subtitles, data, extra tracks).
    for (unsigned i = 0; i < context_->nb_streams; ++i) {
        int index = static_cast<int>(i);
        if (index != video_ && index != audio_ && index != cover_) context_->streams[i]->discard = AVDISCARD_ALL;
    }
}

MediaInput::~MediaInput() {
    if (context_) avformat_close_input(&context_);
}

double MediaInput::duration() const {
    if (context_->duration > 0) return context_->duration / static_cast<double>(AV_TIME_BASE);
    return 0;
}

double MediaInput::startTime() const {
    if (context_->start_time != AV_NOPTS_VALUE) return context_->start_time / static_cast<double>(AV_TIME_BASE);
    return 0;
}

bool MediaInput::isStillImage() const {
    if (video_ < 0 || audio_ >= 0) return false;
    std::string name = context_->iformat ? context_->iformat->name : "";
    if (name == "gif" || name == "apng") return false;  // animations
    AVStream* s = context_->streams[video_];
    if (name.find("_pipe") != std::string::npos || name == "image2") {
        // webp_pipe can be animated: more than one frame means animation.
        return s->nb_frames <= 1;
    }
    return s->nb_frames == 1;
}

void MediaInput::displaySize(int& width, int& height) const {
    width = height = 0;
    int index = video_ >= 0 ? video_ : cover_;
    if (index < 0) return;
    AVStream* s = context_->streams[index];
    width = s->codecpar->width;
    height = s->codecpar->height;
    AVRational sar = av_guess_sample_aspect_ratio(context_, s, 0);
    if (sar.num > 0 && sar.den > 0 && sar.num != sar.den) {
        width = static_cast<int>(static_cast<long long>(width) * sar.num / sar.den);
    }
}

double MediaInput::frameRate() const {
    if (video_ < 0) return 0;
    AVRational r = av_guess_frame_rate(context_, context_->streams[video_], 0);
    return (r.num > 0 && r.den > 0) ? av_q2d(r) : 0;
}

double MediaInput::seconds(int index, int64_t timestamp) const {
    if (timestamp == AV_NOPTS_VALUE) return -1;
    return timestamp * av_q2d(context_->streams[index]->time_base) - startTime();
}

// --------------------------------------------------------------- Decoder ----

Decoder::Decoder(const MediaInput& input, int streamIndex) : context_(0), stream_(streamIndex) {
    AVStream* s = input.context()->streams[streamIndex];
    const AVCodec* codec = avcodec_find_decoder(s->codecpar->codec_id);
    if (!codec) throw MediaError(std::string("no decoder for ") + avcodec_get_name(s->codecpar->codec_id));
    context_ = avcodec_alloc_context3(codec);
    if (!context_) throw MediaError("out of memory");
    avcodec_parameters_to_context(context_, s->codecpar);
    context_->pkt_timebase = s->time_base;
    context_->thread_count = 0;  // automatic: one thread per core
    int rc = avcodec_open2(context_, codec, 0);
    if (rc < 0) {
        avcodec_free_context(&context_);
        throw MediaError(std::string("cannot open decoder ") + codec->name + ": " + ffmpegError(rc));
    }
}

Decoder::~Decoder() { avcodec_free_context(&context_); }

bool Decoder::send(const AVPacket* packet) {
    int rc = avcodec_send_packet(context_, packet);
    return rc >= 0 || rc == AVERROR(EAGAIN) || rc == AVERROR_EOF;
}

bool Decoder::receive(AVFrame* frame) { return avcodec_receive_frame(context_, frame) >= 0; }

void Decoder::flush() { avcodec_flush_buffers(context_); }

// ----------------------------------------------------------- VideoScaler ----

VideoScaler::VideoScaler() : context_(0) {}

VideoScaler::~VideoScaler() { sws_freeContext(context_); }

void VideoScaler::scale(const AVFrame* frame, int width, int height, gfx::Image& out) {
    if (width <= 0 || height <= 0) return;
    context_ = sws_getCachedContext(context_, frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                                    width, height, AV_PIX_FMT_RGB24, SWS_AREA, 0, 0, 0);
    if (!context_) throw MediaError("cannot convert this picture format");
    buffer_.resize(static_cast<size_t>(width) * height * 3);
    std::uint8_t* dst[4] = {&buffer_[0], 0, 0, 0};
    int dstStride[4] = {width * 3, 0, 0, 0};
    sws_scale(context_, frame->data, frame->linesize, 0, frame->height, dst, dstStride);
    if (out.width() != width || out.height() != height) out.resize(width, height);
    const std::uint8_t* p = &buffer_[0];
    for (int y = 0; y < height; ++y) {
        gfx::Rgb* row = out.row(y);
        for (int x = 0; x < width; ++x, p += 3) row[x] = gfx::rgb(p[0], p[1], p[2]);
    }
}

// -------------------------------------------------------- AudioResampler ----

AudioResampler::AudioResampler() : context_(0), inRate_(0), inFormat_(-1) { std::memset(&inLayout_, 0, sizeof(inLayout_)); }

AudioResampler::~AudioResampler() {
    swr_free(&context_);
    av_channel_layout_uninit(&inLayout_);
}

void AudioResampler::reset() {
    swr_free(&context_);
    inRate_ = 0;
}

void AudioResampler::convert(const AVFrame* frame, std::vector<std::int16_t>& out) {
    // (Re)build the converter when the input format changes (rare, but
    // streams can switch layout mid-way).
    if (!context_ || frame->sample_rate != inRate_ || frame->format != inFormat_ ||
        av_channel_layout_compare(&frame->ch_layout, &inLayout_) != 0) {
        swr_free(&context_);
        av_channel_layout_uninit(&inLayout_);
        av_channel_layout_copy(&inLayout_, &frame->ch_layout);
        inRate_ = frame->sample_rate;
        inFormat_ = frame->format;
        AVChannelLayout stereo;
        av_channel_layout_default(&stereo, kChannels);
        int rc = swr_alloc_set_opts2(&context_, &stereo, AV_SAMPLE_FMT_S16, kRate, &inLayout_,
                                     static_cast<AVSampleFormat>(inFormat_), inRate_, 0, 0);
        av_channel_layout_uninit(&stereo);
        if (rc < 0 || swr_init(context_) < 0) {
            swr_free(&context_);
            throw MediaError("cannot convert this sound format");
        }
    }
    int maxOut = swr_get_out_samples(context_, frame->nb_samples);
    if (maxOut <= 0) return;
    size_t start = out.size();
    out.resize(start + static_cast<size_t>(maxOut) * kChannels);
    std::uint8_t* dst = reinterpret_cast<std::uint8_t*>(&out[start]);
    int got = swr_convert(context_, &dst, maxOut, const_cast<const std::uint8_t**>(frame->extended_data),
                          frame->nb_samples);
    out.resize(start + static_cast<size_t>(got > 0 ? got : 0) * kChannels);
}

}  // namespace show
