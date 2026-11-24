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
//  Packets are always kPacketFrames (4096 frames, 85 ms): see "Why
//  fixed-size packets" in AudioOutput.h.  The pending samples carry their
//  media time so the Player can tell what is being heard (playing()).
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
}  // namespace

AudioOutput::AudioOutput()
    : context_(0), pts_(0), pendingPts_(0), sentEnd_(0), sent_(false), underruns_(0),
      drainsAt_(std::chrono::steady_clock::now()) {}

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
    pending_.clear();
    sent_ = false;
    underruns_ = 0;
    drainsAt_ = std::chrono::steady_clock::now();
    error_.clear();
    return true;
}

void AudioOutput::close() {
    if (!context_) return;
    av_write_trailer(context_);
    avformat_free_context(context_);
    context_ = 0;
}

bool AudioOutput::write(const std::int16_t* samples, size_t frames, double volume, double pts) {
    if (!context_) return false;
    const size_t channels = AudioResampler::kChannels;
    if (pending_.empty()) pendingPts_ = pts;
    size_t start = pending_.size();
    pending_.resize(start + frames * channels);
    std::int16_t* out = &pending_[start];
    size_t values = frames * channels;
    if (volume <= 0) {
        std::memset(out, 0, values * sizeof(std::int16_t));
    } else if (volume == 1.0) {
        std::memcpy(out, samples, values * sizeof(std::int16_t));
    } else {
        for (size_t i = 0; i < values; ++i) {
            double v = samples[i] * volume;
            out[i] = static_cast<std::int16_t>(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
        }
    }
    // Send every complete packet; keep the rest for next time.
    const size_t packetValues = kPacketFrames * channels;
    size_t offset = 0;
    while (pending_.size() - offset >= packetValues) {
        if (!sendPacket(&pending_[offset])) return false;
        offset += packetValues;
        pendingPts_ += static_cast<double>(kPacketFrames) / AudioResampler::kRate;
    }
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<long>(offset));
    return true;
}

bool AudioOutput::finish() {
    if (!context_ || pending_.empty()) return true;
    const size_t packetValues = kPacketFrames * AudioResampler::kChannels;
    pending_.resize(packetValues, 0);  // pad with silence to a full packet
    bool ok = sendPacket(&pending_[0]);
    pending_.clear();
    return ok;
}

void AudioOutput::discard() { pending_.clear(); }

// Device model: every packet plays for kPacketFrames / rate seconds, one
// after the other.  When write blocks, a buffer has just finished, so the
// device now holds exactly two packets (the one starting and the new one):
// the model is re-synchronized to that.
bool AudioOutput::sendPacket(const std::int16_t* data) {
    const size_t values = kPacketFrames * AudioResampler::kChannels;
    const double packetSeconds = static_cast<double>(kPacketFrames) / AudioResampler::kRate;
    AVPacket* packet = av_packet_alloc();
    if (!packet || av_new_packet(packet, static_cast<int>(values * sizeof(std::int16_t))) < 0) {
        av_packet_free(&packet);
        return false;
    }
    std::memcpy(packet->data, data, values * sizeof(std::int16_t));
    packet->pts = packet->dts = pts_;
    packet->duration = static_cast<int64_t>(kPacketFrames);
    pts_ += static_cast<int64_t>(kPacketFrames);

    std::chrono::steady_clock::time_point before = std::chrono::steady_clock::now();
    // The device had already played everything: there was a gap (a click).
    if (sent_ && before > drainsAt_ + std::chrono::milliseconds(5)) ++underruns_;
    int rc = av_write_frame(context_, packet);
    av_packet_free(&packet);
    if (rc < 0) {
        error_ = name_ + ": " + ffmpegError(rc);
        return false;
    }
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    std::chrono::steady_clock::duration packetTime =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(packetSeconds));
    bool blocked = now - before > std::chrono::milliseconds(2);
    if (blocked) {
        drainsAt_ = now + packetTime * 2;
    } else {
        drainsAt_ = (drainsAt_ > now ? drainsAt_ : now) + packetTime;
    }
    sentEnd_ = pendingPts_ + packetSeconds;
    sent_ = true;
    return true;
}

bool AudioOutput::playing(double& pts) const {
    if (!sent_) return false;
    double queued = std::chrono::duration<double>(drainsAt_ - std::chrono::steady_clock::now()).count();
    pts = sentEnd_ - (queued > 0 ? queued : 0);
    return true;
}

void AudioOutput::state(double& sentEnd, std::chrono::steady_clock::time_point& drainsAt) const {
    sentEnd = sentEnd_;
    drainsAt = drainsAt_;
}

}  // namespace show
