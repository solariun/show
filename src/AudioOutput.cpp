// ============================================================================
//  show - view pictures, play video and sound, convert media, all in the
//  terminal.  C++11, FFmpeg (native libraries) + Graphicx.
//  https://github.com/solariun/show
// ----------------------------------------------------------------------------
//  File:   src/AudioOutput.cpp
//  Layer:  Media (FFmpeg)
//
//  What this file does
//  -------------------
//  Implementation of AudioOutput (see AudioOutput.h): opens a libavdevice
//  audio output with one PCM 16-bit stereo 48 kHz stream and writes packets.
//
//  How it fits in
//  --------------
//  Packets are at most 1024 sample frames (21 ms): small enough for prompt
//  pause / seek reactions, large enough to keep overhead low.
//
//  FFmpeg notes
//  ------------
//  The device is chosen by name with av_guess_format(); devices are only
//  visible after avdevice_register_all() (done by initFFmpeg()).
//
//  Contributing
//  ------------
//  To add a device (e.g. "sndio" on BSD), append it to kPreferred.
// ============================================================================
#include "AudioOutput.h"

extern "C" {
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
}

#include <cstring>

#include "Media.h"

namespace show {

namespace {
const char* const kPreferred[] = {"audiotoolbox", "pulse", "alsa", "oss", "sndio"};
const size_t kPacketFrames = 1024;
}  // namespace

AudioOutput::AudioOutput() : context_(0), pts_(0) {}

AudioOutput::~AudioOutput() { close(); }

std::vector<std::string> AudioOutput::devices() {
    initFFmpeg();
    std::vector<std::string> names;
    const AVOutputFormat* f = 0;
    while ((f = av_output_audio_device_next(f)) != 0) names.push_back(f->name);
    return names;
}

bool AudioOutput::open(const std::string& device) {
    close();
    initFFmpeg();
    if (!device.empty()) return openDevice(device);
    std::vector<std::string> available = devices();
    for (size_t i = 0; i < sizeof(kPreferred) / sizeof(kPreferred[0]); ++i) {
        for (size_t k = 0; k < available.size(); ++k) {
            if (available[k] == kPreferred[i] && openDevice(available[k])) return true;
        }
    }
    if (error_.empty()) error_ = "no audio output device in this FFmpeg build";
    return false;
}

bool AudioOutput::openDevice(const std::string& device) {
    const AVOutputFormat* format = av_guess_format(device.c_str(), 0, 0);
    if (!format) {
        error_ = "unknown audio device " + device;
        return false;
    }
    int rc = avformat_alloc_output_context2(&context_, format, 0, "");
    if (rc < 0 || !context_) {
        error_ = device + ": " + ffmpegError(rc);
        context_ = 0;
        return false;
    }
    AVStream* stream = avformat_new_stream(context_, 0);
    stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    stream->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
    stream->codecpar->format = AV_SAMPLE_FMT_S16;
    stream->codecpar->bits_per_coded_sample = 16;
    stream->codecpar->sample_rate = AudioResampler::kRate;
    av_channel_layout_default(&stream->codecpar->ch_layout, AudioResampler::kChannels);
    stream->time_base.num = 1;
    stream->time_base.den = AudioResampler::kRate;
    rc = avformat_write_header(context_, 0);
    if (rc < 0) {
        error_ = device + ": " + ffmpegError(rc);
        avformat_free_context(context_);
        context_ = 0;
        return false;
    }
    name_ = device;
    pts_ = 0;
    error_.clear();
    return true;
}

void AudioOutput::close() {
    if (!context_) return;
    av_write_trailer(context_);
    avformat_free_context(context_);
    context_ = 0;
}

bool AudioOutput::write(const std::int16_t* samples, size_t frames, double volume) {
    if (!context_) return false;
    const int channels = AudioResampler::kChannels;
    while (frames > 0) {
        size_t n = frames < kPacketFrames ? frames : kPacketFrames;
        size_t values = n * channels;
        scaled_.resize(values);
        if (volume <= 0) {
            std::memset(&scaled_[0], 0, values * sizeof(std::int16_t));
        } else if (volume == 1.0) {
            std::memcpy(&scaled_[0], samples, values * sizeof(std::int16_t));
        } else {
            for (size_t i = 0; i < values; ++i) {
                double v = samples[i] * volume;
                scaled_[i] = static_cast<std::int16_t>(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
            }
        }
        AVPacket* packet = av_packet_alloc();
        if (!packet || av_new_packet(packet, static_cast<int>(values * sizeof(std::int16_t))) < 0) {
            av_packet_free(&packet);
            return false;
        }
        std::memcpy(packet->data, &scaled_[0], values * sizeof(std::int16_t));
        packet->pts = packet->dts = pts_;
        packet->duration = static_cast<int64_t>(n);
        pts_ += static_cast<int64_t>(n);
        int rc = av_write_frame(context_, packet);
        av_packet_free(&packet);
        if (rc < 0) {
            error_ = name_ + ": " + ffmpegError(rc);
            return false;
        }
        samples += values;
        frames -= n;
    }
    return true;
}

}  // namespace show
