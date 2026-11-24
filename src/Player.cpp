// ============================================================================
//  show - view pictures, play video and sound, convert media, all in the
//  terminal.  C++11, FFmpeg (native libraries) + Graphicx.
//  https://github.com/solariun/show
// ----------------------------------------------------------------------------
//  File:   src/Player.cpp
//  Layer:  Playback
//
//  What this file does
//  -------------------
//  Implementation of Player (see Player.h): the three playback threads, the
//  clock, seeking, the audio visualizer and the key handling.
//
//  How it fits in
//  --------------
//    Queue<T>    small thread-safe queue (mutex + condition variable)
//    Clock       playback time: driven by sound when there is sound
//    Shared      everything the threads share (flags, queues, clock)
//    readLoop    reader thread: packets -> frames -> queues, seeks
//    audioLoop   audio thread: chunks -> AudioOutput, updates the clock
//    Player::play / camera   the main thread: display, keys, status
//
//  Synchronization rules:
//  * The reader never waits forever on a full queue: it gives up when a
//    seek or stop is requested, and lets a queue grow past its limit when
//    the *other* queue is empty (otherwise video waiting for sound and sound
//    waiting for the reader could block each other).
//  * After a seek, every item with an older serial is discarded by whoever
//    takes it out of a queue.
//
//  Contributing
//  ------------
//  Any new shared field goes in Shared and is either std::atomic or
//  protected by Shared::mutex.  Test changes with a video with sound, a
//  music file, a GIF, seeking while paused, and resizing the terminal.
// ============================================================================
#include "Player.h"

#include <graphicx/graphicx.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "AudioOutput.h"
#include "Media.h"

namespace show {

namespace {

typedef std::chrono::steady_clock SteadyClock;

double secondsSince(SteadyClock::time_point t) {
    return std::chrono::duration<double>(SteadyClock::now() - t).count();
}

// ------------------------------------------------------------------ Queue --

// Thread-safe FIFO of shared pointers.  Capacity is enforced by the reader
// (see Shared::pushVideo / pushAudio), not here, so the reader can decide.
template <typename T>
class Queue {
public:
    void push(const std::shared_ptr<T>& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        items_.push_back(item);
        ready_.notify_one();
    }
    // Waits up to timeoutMs for an item.
    std::shared_ptr<T> pop(int timeoutMs) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (items_.empty()) ready_.wait_for(lock, std::chrono::milliseconds(timeoutMs));
        if (items_.empty()) return std::shared_ptr<T>();
        std::shared_ptr<T> item = items_.front();
        items_.pop_front();
        return item;
    }
    std::shared_ptr<T> peek() {
        std::lock_guard<std::mutex> lock(mutex_);
        return items_.empty() ? std::shared_ptr<T>() : items_.front();
    }
    std::shared_ptr<T> peekSecond() {
        std::lock_guard<std::mutex> lock(mutex_);
        return items_.size() < 2 ? std::shared_ptr<T>() : items_[1];
    }
    void popFront() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!items_.empty()) items_.pop_front();
    }
    // Live sources: keep only the newest `keep` items.
    void trim(size_t keep) {
        std::lock_guard<std::mutex> lock(mutex_);
        while (items_.size() > keep) items_.pop_front();
    }
    size_t size() {
        std::lock_guard<std::mutex> lock(mutex_);
        return items_.size();
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        items_.clear();
    }

private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::shared_ptr<T> > items_;
};

struct VideoFrame {
    double pts;   // seconds from the start
    int serial;   // seek generation
    gfx::Image image;
};

struct AudioChunk {
    double pts;
    int serial;
    std::vector<std::int16_t> samples;  // 48 kHz stereo interleaved
};

// ------------------------------------------------------------------ Clock --

// Playback time in seconds.
// * With sound: what the speakers play right now, from AudioOutput's model
//   (end of the sound sent minus what is still queued in the device).  So
//   the picture follows the sound you hear, including the device delay.
// * Without sound: the wall clock since an anchor (the first frame shown).
class Clock {
public:
    Clock() : audio_(false), anchored_(false), haveAudio_(false), base_(0), sentEnd_(0), paused_(false), pausedAt_(0) {}

    void reset(double t, bool audioDriven) {
        std::lock_guard<std::mutex> lock(mutex_);
        audio_ = audioDriven;
        anchored_ = false;
        haveAudio_ = false;
        base_ = t;
        if (paused_) pausedAt_ = t;
    }
    // Wall clock mode: time starts at t now.
    void anchor(double t) {
        std::lock_guard<std::mutex> lock(mutex_);
        anchored_ = true;
        base_ = t;
        wall_ = SteadyClock::now();
    }
    // Sound mode: the audio thread reports after every packet.
    void audioState(double sentEnd, SteadyClock::time_point drainsAt) {
        std::lock_guard<std::mutex> lock(mutex_);
        haveAudio_ = true;
        sentEnd_ = sentEnd;
        drainsAt_ = drainsAt;
    }
    // The sound ended (or failed): continue on the wall clock.
    void detachAudio() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!audio_) return;
        double t = getLocked();
        audio_ = false;
        anchored_ = true;
        base_ = t;
        wall_ = SteadyClock::now();
    }
    double get() {
        std::lock_guard<std::mutex> lock(mutex_);
        return getLocked();
    }
    bool anchored() {
        std::lock_guard<std::mutex> lock(mutex_);
        return audio_ ? haveAudio_ : anchored_;
    }
    bool audioDriven() {
        std::lock_guard<std::mutex> lock(mutex_);
        return audio_;
    }
    void pause() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (paused_) return;
        pausedAt_ = getLocked();
        paused_ = true;
    }
    void resume() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!paused_) return;
        paused_ = false;
        base_ = pausedAt_;
        wall_ = SteadyClock::now();
        anchored_ = true;
    }

private:
    double getLocked() const {
        if (paused_) return pausedAt_;
        if (audio_) {
            if (!haveAudio_) return base_;
            double queued = std::chrono::duration<double>(drainsAt_ - SteadyClock::now()).count();
            double t = sentEnd_ - (queued > 0 ? queued : 0);
            return t > base_ ? t : base_;  // after a seek, older sound may still be playing
        }
        if (!anchored_) return base_;
        return base_ + secondsSince(wall_);
    }

    std::mutex mutex_;
    bool audio_, anchored_, haveAudio_;
    double base_;
    SteadyClock::time_point wall_;
    double sentEnd_;
    SteadyClock::time_point drainsAt_;
    bool paused_;
    double pausedAt_;
};

// ------------------------------------------------------------- Visualizer --

// Recent sound for the spectrum display + the bar levels it shows.
class Visualizer {
public:
    Visualizer() : samples_(4096, 0) {}

    void feed(const std::vector<std::int16_t>& stereo) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i + 1 < stereo.size(); i += 2) {
            samples_.push_back(static_cast<float>(stereo[i] + stereo[i + 1]) / 65536.0f);
        }
        if (samples_.size() > 4096) samples_.erase(samples_.begin(), samples_.end() - 4096);
    }

    // Updates and returns `bands` levels (0..1), log-spaced 40 Hz - 16 kHz.
    const std::vector<float>& levels(int bands) {
        std::vector<float> window;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            window.assign(samples_.end() - 2048, samples_.end());
        }
        if (static_cast<int>(levels_.size()) != bands) levels_.assign(static_cast<size_t>(bands), 0.0f);
        const double rate = AudioResampler::kRate;
        const size_t n = window.size();
        for (int b = 0; b < bands; ++b) {
            // Goertzel algorithm: the energy of one frequency, O(n).
            double freq = 40.0 * std::pow(16000.0 / 40.0, (b + 0.5) / bands);
            double w = 2.0 * 3.14159265358979 * freq / rate;
            double coeff = 2.0 * std::cos(w), s1 = 0, s2 = 0;
            for (size_t i = 0; i < n; ++i) {
                double hann = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979 * i / (n - 1));
                double s = window[i] * hann + coeff * s1 - s2;
                s2 = s1;
                s1 = s;
            }
            double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
            double db = 10.0 * std::log10(power / n + 1e-12);  // about -120 .. +20
            float level = static_cast<float>((db + 70.0) / 70.0);
            if (level < 0) level = 0;
            if (level > 1) level = 1;
            float& current = levels_[static_cast<size_t>(b)];
            current = level > current ? level : current * 0.85f;  // fast attack, slow decay
        }
        return levels_;
    }

private:
    std::mutex mutex_;
    std::vector<float> samples_;
    std::vector<float> levels_;
};

// ----------------------------------------------------------------- Shared --

struct Shared {
    Shared() : stop(false), eof(false), paused(false), serial(0), targetWidth(0), targetHeight(0),
               live(false), hasVideo(false), hasAudio(false), audioActive(false), volume(100), muted(false), underruns(0),
               seekRequested(false), seekTarget(0), lastAudioWrite(SteadyClock::now()) {}

    std::atomic<bool> stop;
    std::atomic<bool> eof;
    std::atomic<bool> paused;
    std::atomic<int> serial;        // seek generation
    std::atomic<int> targetWidth;   // video frames are scaled to this size
    std::atomic<int> targetHeight;
    bool live, hasVideo, hasAudio;
    std::atomic<bool> audioActive;  // the audio thread is playing sound
    std::atomic<int> volume;        // percent
    std::atomic<bool> muted;
    std::atomic<long> underruns;    // sound gaps (clicks), see AudioOutput

    std::mutex mutex;               // protects the fields below
    bool seekRequested;
    double seekTarget;
    std::string audioError;
    SteadyClock::time_point lastAudioWrite;
    gfx::Image cover;               // album art (audio files)

    Queue<VideoFrame> video;
    Queue<AudioChunk> audio;
    Clock clock;
    Visualizer visualizer;

    static const size_t kVideoCapacity = 8;
    static const size_t kAudioCapacity = 64;  // ~1.4 s of decoded sound
    static const size_t kAudioLowWater = 12;  // ~250 ms: below this, sound comes first

    // Push helpers: wait while the queue is full, unless the other queue is
    // empty (see the file header) or a seek / stop is pending.
    bool waitForRoom(bool forVideo) {
        for (;;) {
            if (stop) return false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (seekRequested) return false;
            }
            size_t own = forVideo ? video.size() : audio.size();
            size_t cap = forVideo ? kVideoCapacity : kAudioCapacity;
            // The other queue running low means its consumer is about to wait:
            // keep reading so it gets data (sound running dry = crackles).
            bool otherStarved = forVideo ? (hasAudio && audioActive && audio.size() < kAudioLowWater)
                                         : (hasVideo && video.size() == 0);
            if (own < cap || otherStarved) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
    }
};

// ------------------------------------------------------------ reader thread --

void readLoop(Shared& s, MediaInput& input, double startAt) {
    std::unique_ptr<Decoder> videoDecoder, audioDecoder, coverDecoder;
    try {
        if (input.videoStream() >= 0) videoDecoder.reset(new Decoder(input, input.videoStream()));
        if (s.hasAudio) audioDecoder.reset(new Decoder(input, input.audioStream()));
        if (input.coverStream() >= 0 && input.videoStream() < 0) coverDecoder.reset(new Decoder(input, input.coverStream()));
    } catch (const MediaError& e) {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.audioError = e.what();
        s.eof = true;
        return;
    }
    VideoScaler scaler, coverScaler;
    AudioResampler resampler;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFormatContext* ic = input.context();
    int serial = s.serial;
    double skipBefore = startAt;
    double lastVideoPts = -1;
    double frameStep = input.frameRate() > 0 ? 1.0 / input.frameRate() : 0.04;

    if (startAt > 0) {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.seekRequested = true;
        s.seekTarget = startAt;
    }

    // Takes all frames out of a decoder and queues them.
    auto drainVideo = [&]() {
        while (videoDecoder->receive(frame)) {
            double t = input.seconds(input.videoStream(), frame->best_effort_timestamp);
            if (t < 0) t = lastVideoPts < 0 ? 0 : lastVideoPts + frameStep;
            lastVideoPts = t;
            if (t + 1e-4 < skipBefore) {  // before the seek target: not shown
                av_frame_unref(frame);
                continue;
            }
            int w = s.targetWidth, h = s.targetHeight;
            if (w > 0 && h > 0 && (s.live || s.waitForRoom(true))) {
                std::shared_ptr<VideoFrame> vf(new VideoFrame);
                vf->pts = t;
                vf->serial = serial;
                scaler.scale(frame, w, h, vf->image);
                s.video.push(vf);
                if (s.live) s.video.trim(2);  // a camera shows the newest picture
            }
            av_frame_unref(frame);
        }
    };
    auto drainAudio = [&]() {
        while (audioDecoder->receive(frame)) {
            double t = input.seconds(input.audioStream(), frame->best_effort_timestamp);
            double length = frame->sample_rate > 0 ? static_cast<double>(frame->nb_samples) / frame->sample_rate : 0;
            if (t >= 0 && t + length < skipBefore) {
                av_frame_unref(frame);
                continue;
            }
            std::shared_ptr<AudioChunk> chunk(new AudioChunk);
            chunk->pts = t < 0 ? 0 : t;
            chunk->serial = serial;
            try {
                resampler.convert(frame, chunk->samples);
            } catch (const MediaError&) {
                chunk->samples.clear();
            }
            av_frame_unref(frame);
            if (!chunk->samples.empty() && s.waitForRoom(false)) s.audio.push(chunk);
        }
    };

    while (!s.stop) {
        // Seek requested by the main thread (or --start)?
        bool doSeek = false;
        double target = 0;
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            if (s.seekRequested) {
                doSeek = true;
                target = s.seekTarget;
                s.seekRequested = false;
            }
        }
        if (doSeek) {
            int64_t ts = static_cast<int64_t>((target + input.startTime()) * AV_TIME_BASE);
            if (avformat_seek_file(ic, -1, INT64_MIN, ts, ts, 0) < 0) {
                avformat_seek_file(ic, -1, INT64_MIN, ts, INT64_MAX, 0);
            }
            if (videoDecoder) videoDecoder->flush();
            if (audioDecoder) audioDecoder->flush();
            resampler.reset();
            serial = s.serial;
            skipBefore = target;
            lastVideoPts = -1;
            s.eof = false;
            continue;
        }
        if (s.eof) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        int rc = av_read_frame(ic, packet);
        if (rc == AVERROR(EAGAIN)) {  // live devices: no frame ready yet
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        if (rc < 0) {  // end of file (or a read error): drain the decoders
            if (videoDecoder) {
                videoDecoder->send(0);
                drainVideo();
                videoDecoder->flush();
            }
            if (audioDecoder) {
                audioDecoder->send(0);
                drainAudio();
                audioDecoder->flush();
            }
            s.eof = true;
            continue;
        }
        if (videoDecoder && packet->stream_index == input.videoStream()) {
            videoDecoder->send(packet);
            drainVideo();
        } else if (audioDecoder && packet->stream_index == input.audioStream()) {
            audioDecoder->send(packet);
            drainAudio();
        } else if (coverDecoder && packet->stream_index == input.coverStream()) {
            coverDecoder->send(packet);
            coverDecoder->send(0);
            if (coverDecoder->receive(frame)) {
                gfx::Image art;
                coverScaler.scale(frame, frame->width, frame->height, art);
                std::lock_guard<std::mutex> lock(s.mutex);
                s.cover = art;
                av_frame_unref(frame);
            }
            coverDecoder.reset();  // one picture is enough
        }
        av_packet_unref(packet);
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
}

// ------------------------------------------------------------- audio thread --

void audioLoop(Shared& s, const std::string& device) {
    AudioOutput output;
    if (!output.open(device)) {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.audioError = "no sound: " + output.error();
        s.audioActive = false;
        s.clock.detachAudio();
        return;
    }
    int serial = s.serial;
    bool finished = false;
    while (!s.stop) {
        if (s.paused) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        std::shared_ptr<AudioChunk> chunk = s.audio.pop(20);
        if (!chunk) {
            // End of the media: send the last partial packet (padded).
            if (s.eof && !finished && s.audio.size() == 0) {
                finished = true;
                output.finish();
                double end;
                SteadyClock::time_point drainsAt;
                output.state(end, drainsAt);
                if (output.hasSent()) s.clock.audioState(end, drainsAt);
            }
            continue;
        }
        if (chunk->serial != s.serial) continue;
        if (chunk->serial != serial) {  // first sound after a seek
            serial = chunk->serial;
            output.discard();
        }
        finished = false;
        size_t frames = chunk->samples.size() / AudioResampler::kChannels;
        s.visualizer.feed(chunk->samples);
        double volume = s.muted ? 0.0 : s.volume / 100.0;
        if (!output.write(&chunk->samples[0], frames, volume, chunk->pts)) {
            std::lock_guard<std::mutex> lock(s.mutex);
            s.audioError = "sound stopped: " + output.error();
            s.audioActive = false;
            s.clock.detachAudio();
            return;
        }
        if (output.hasSent()) {
            double end;
            SteadyClock::time_point drainsAt;
            output.state(end, drainsAt);
            s.clock.audioState(end, drainsAt);
        }
        s.underruns = output.underruns();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.lastAudioWrite = SteadyClock::now();
    }
}

// --------------------------------------------------------------- drawing --

// Nearest-neighbour copy of `src` into the rectangle (x0, y0, w, h) of `dst`.
void blit(const gfx::Image& src, gfx::Image& dst, int x0, int y0, int w, int h) {
    if (src.empty() || w <= 0 || h <= 0) return;
    for (int y = 0; y < h; ++y) {
        int dy = y0 + y;
        if (dy < 0 || dy >= dst.height()) continue;
        const gfx::Rgb* row = src.row(static_cast<int>(static_cast<long long>(y) * src.height() / h));
        gfx::Rgb* out = dst.row(dy);
        for (int x = 0; x < w; ++x) {
            int dx = x0 + x;
            if (dx >= 0 && dx < dst.width()) out[dx] = row[static_cast<long long>(x) * src.width() / w];
        }
    }
}

// Music display: the album art (if any) above a spectrum analyzer.
void drawMusic(gfx::Image& canvas, const gfx::Image& cover, const std::vector<float>& levels) {
    const int w = canvas.width(), h = canvas.height();
    for (int y = 0; y < h; ++y) {
        gfx::Rgb* row = canvas.row(y);
        for (int x = 0; x < w; ++x) row[x] = gfx::rgb(10, 10, 18);
    }
    int barsHeight = cover.empty() ? h : h / 4;
    if (!cover.empty()) {
        // Fit the cover into the space above the bars (pixels are 1:1 here).
        int areaH = h - barsHeight;
        double scale = std::min(static_cast<double>(w) / cover.width(), static_cast<double>(areaH) / cover.height());
        int cw = static_cast<int>(cover.width() * scale), ch = static_cast<int>(cover.height() * scale);
        blit(cover, canvas, (w - cw) / 2, (areaH - ch) / 2, cw, ch);
    }
    int bands = static_cast<int>(levels.size());
    for (int x = 0; x < w; ++x) {
        float level = levels[static_cast<size_t>(x * bands / w)];
        int barTop = h - static_cast<int>(level * barsHeight);
        for (int y = barTop; y < h; ++y) {
            double v = static_cast<double>(h - y) / barsHeight;  // 0 bottom .. 1 top
            std::uint8_t r = static_cast<std::uint8_t>(v < 0.5 ? 510 * v : 255);
            std::uint8_t g = static_cast<std::uint8_t>(v < 0.5 ? 220 : 220 * (1.0 - v) * 2);
            canvas.set(x, y, gfx::rgb(r, g, 60));
        }
    }
}

}  // namespace

// ------------------------------------------------------------------ Player --

Player::Player(gfx::terminal::Session& session, Options& options) : session_(session), options_(options) {}

Outcome Player::camera(const std::string& format, const std::string& device) {
    // The camera goes through play() like any other source, as a live input
    // written "format:device" (frame rate and size options are added there).
    return play(format + ":" + device, 1, 1);
}

Outcome Player::play(const std::string& path, int position, int count) {
    error_.clear();
    // "format:device" is how camera() passes a capture device.
    std::string format, location = path;
    std::vector<std::string> deviceOptions;
    if (path.compare(0, 13, "avfoundation:") == 0 || path.compare(0, 5, "v4l2:") == 0 ||
        path.compare(0, 6, "dshow:") == 0) {
        size_t colon = path.find(':');
        format = path.substr(0, colon);
        location = path.substr(colon + 1);
        deviceOptions.push_back("framerate=" + std::to_string(options_.cameraFps));
        if (!options_.cameraSize.empty()) deviceOptions.push_back("video_size=" + options_.cameraSize);
    }

    std::unique_ptr<MediaInput> input;
    try {
        input.reset(new MediaInput(location, format, deviceOptions));
    } catch (const MediaError& e) {
        if (!format.empty() && !deviceOptions.empty()) {
            // Some cameras reject the frame rate / size: retry with defaults.
            try {
                input.reset(new MediaInput(location, format));
            } catch (const MediaError& e2) {
                error_ = e2.what();
                return Outcome::Next;
            }
        } else {
            error_ = e.what();
            return Outcome::Next;
        }
    }

    Shared s;
    s.live = input->isLive();
    s.hasVideo = input->videoStream() >= 0;
    s.hasAudio = options_.audio && input->audioStream() >= 0 && !s.live;
    s.audioActive = s.hasAudio;
    s.volume = static_cast<int>(options_.volume * 100 + 0.5);
    s.muted = options_.muted;
    const std::string name = s.live ? std::string("camera ") + location : baseName(path);
    const double duration = input->duration();
    const bool animation = s.hasVideo && input->audioStream() < 0 && duration > 0 && duration < 60 &&
                           std::string(input->context()->iformat->name) == "gif";
    const double startAt = (!s.live && options_.start > 0 && options_.start < duration) ? options_.start : 0;
    s.clock.reset(startAt, s.hasAudio);

    // Layout: the video's shape, or the whole terminal for music.
    int sourceW = 0, sourceH = 0;
    input->displaySize(sourceW, sourceH);
    gfx::terminal::Size term = gfx::terminal::size();
    Layout layout;
    auto relayout = [&]() {
        if (s.hasVideo) {
            layout = fitLayout(sourceW, sourceH);
        } else {
            term = gfx::terminal::size();
            layout = fitLayout(term.columns, std::max(2, (term.rows - 1) * 2));
        }
        s.targetWidth = layout.pixelWidth();
        s.targetHeight = layout.pixelHeight();
    };
    relayout();
    gfx::Screen screen;
    auto resetScreen = [&]() {
        screen.setOptions(pictureOptions(options_, layout, gfx::Filter::Box));
        screen.setOrigin(layout.originRow, layout.originColumn);
        screen.setSynchronized(false);
        session_.write("\x1b[0m\x1b[2J");
    };
    resetScreen();

    std::thread reader(readLoop, std::ref(s), std::ref(*input), startAt);
    std::thread audio;
    if (s.hasAudio) audio = std::thread(audioLoop, std::ref(s), options_.audioDevice);

    Outcome outcome = Outcome::Next;
    bool showInfo = false;
    long drawn = 0, dropped = 0;
    int dropStreak = 0;
    std::shared_ptr<VideoFrame> current;
    gfx::Image canvas;
    SteadyClock::time_point lastStatus = SteadyClock::now() - std::chrono::seconds(1);
    SteadyClock::time_point lastMusicFrame = lastStatus;
    SteadyClock::time_point messageUntil = SteadyClock::now();
    std::string message;
    bool repaint = true;

    auto requestSeek = [&](double target) {
        if (s.live || duration <= 0) return;
        if (target < 0) target = 0;
        if (target > duration - 0.25) target = std::max(0.0, duration - 0.25);
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            s.seekRequested = true;
            s.seekTarget = target;
            s.serial = s.serial + 1;
        }
        s.video.clear();
        s.audio.clear();
        s.clock.reset(target, s.hasAudio && s.audioActive);
        current.reset();
    };

    auto infoLines = [&]() {
        std::vector<std::string> lines;
        AVFormatContext* ic = input->context();
        lines.push_back(name);
        lines.push_back(std::string("container: ") + (ic->iformat->long_name ? ic->iformat->long_name : ic->iformat->name));
        if (s.hasVideo) {
            AVStream* v = ic->streams[input->videoStream()];
            char text[160];
            std::snprintf(text, sizeof(text), "video: %s %dx%d %.3g fps", avcodec_get_name(v->codecpar->codec_id),
                          v->codecpar->width, v->codecpar->height, input->frameRate());
            lines.push_back(text);
        }
        if (input->audioStream() >= 0) {
            AVStream* a = ic->streams[input->audioStream()];
            char text[160];
            std::snprintf(text, sizeof(text), "audio: %s %d Hz %d ch -> %s, gaps %ld",
                          avcodec_get_name(a->codecpar->codec_id), a->codecpar->sample_rate,
                          a->codecpar->ch_layout.nb_channels,
                          s.hasAudio ? (s.audioActive ? "playing" : "off") : "disabled", static_cast<long>(s.underruns));
            lines.push_back(text);
        }
        char grid[96];
        std::snprintf(grid, sizeof(grid), "terminal: %dx%d cells, picture %dx%d", layout.terminalColumns,
                      layout.terminalRows, layout.columns, layout.rows);
        lines.push_back(grid);
        lines.push_back(std::string("display: ") + styleLabel(options_));
        return lines;
    };

    for (;;) {
        if (gfx::terminal::Session::interrupted()) {
            outcome = Outcome::Quit;
            break;
        }
        if (gfx::terminal::Session::resized()) {
            relayout();
            resetScreen();
            repaint = true;
        }

        // ---- keys (wait a little when nothing else is due) ----
        int key = session_.readKey(5);
        if (key == 'q' || key == gfx::terminal::KeyEscape) {
            outcome = Outcome::Quit;
            break;
        }
        if (key == 'n') {
            outcome = Outcome::Next;
            break;
        }
        if (key == 'p') {
            outcome = Outcome::Previous;
            break;
        }
        if (key == ' ' && !s.live) {
            s.paused = !s.paused;
            if (s.paused) {
                s.clock.pause();
            } else {
                s.clock.resume();
            }
            repaint = true;
        }
        double now = s.clock.get();
        if (key == gfx::terminal::KeyRight) requestSeek(now + 5);
        if (key == gfx::terminal::KeyLeft) requestSeek(now - 5);
        if (key == gfx::terminal::KeyUp) requestSeek(now + 30);
        if (key == gfx::terminal::KeyDown) requestSeek(now - 30);
        if (key >= '0' && key <= '9') requestSeek(duration * (key - '0') / 10.0);
        if (key == '+' || key == '=') {
            s.volume = std::min(200, s.volume + 10);
            s.muted = false;
            options_.volume = s.volume / 100.0;
            options_.muted = false;
        }
        if (key == '-' || key == '_') {
            s.volume = std::max(0, s.volume - 10);
            options_.volume = s.volume / 100.0;
        }
        if (key == 'm') {
            s.muted = !s.muted;
            options_.muted = s.muted;
        }
        if (key == 'l') options_.loop = !options_.loop;
        if (key == 'i') {
            showInfo = !showInfo;
            if (!showInfo) resetScreen();
            repaint = true;
        }
        if (displayKey(key, options_)) {
            resetScreen();
            repaint = true;
        }
        if (key != gfx::terminal::KeyNone) lastStatus = SteadyClock::now() - std::chrono::seconds(1);

        // ---- the picture ----
        std::string out;
        if (s.hasVideo) {
            std::shared_ptr<VideoFrame> show;
            for (;;) {
                std::shared_ptr<VideoFrame> f = s.video.peek();
                if (!f) break;
                if (f->serial != s.serial) {
                    s.video.popFront();
                    continue;
                }
                if (s.live) {  // newest frame, right away
                    s.video.popFront();
                    show = f;
                    continue;
                }
                double t = s.clock.get();
                if (!s.clock.anchored() && !s.clock.audioDriven()) {
                    s.clock.anchor(f->pts);
                    t = f->pts;
                }
                // Not due yet (while paused, still show the first picture after
                // a seek so the new position is visible).
                if (f->pts > t + 0.002 && !(s.paused && !current)) break;
                s.video.popFront();
                std::shared_ptr<VideoFrame> next = s.video.peek();
                if (next && next->serial == s.serial && next->pts <= t && dropStreak < 8 && !s.paused) {
                    ++dropped;  // late: the next one is due already
                    ++dropStreak;
                    continue;
                }
                dropStreak = 0;
                show = f;
                break;
            }
            if (show) {
                current = show;
                repaint = true;
            }
            if (repaint && current) {
                screen.draw(current->image, out);
                ++drawn;
            }
        } else if (secondsSince(lastMusicFrame) > 0.04 || repaint) {
            lastMusicFrame = SteadyClock::now();
            if (canvas.width() != layout.pixelWidth() || canvas.height() != layout.pixelHeight()) {
                canvas.resize(layout.pixelWidth(), layout.pixelHeight());
            }
            gfx::Image cover;
            {
                std::lock_guard<std::mutex> lock(s.mutex);
                cover = s.cover;
            }
            drawMusic(canvas, cover, s.visualizer.levels(std::max(8, layout.columns / 2)));
            screen.draw(canvas, out);
            repaint = true;
        }

        // ---- overlays and status ----
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            if (!s.audioError.empty()) {
                message = s.audioError;
                messageUntil = SteadyClock::now() + std::chrono::seconds(4);
                s.audioError.clear();
            }
        }
        if (showInfo && repaint) {
            std::vector<std::string> lines = infoLines();
            out += infoBox(lines, layout);
            screen.invalidateCells(0, 0, static_cast<int>(lines.size()), layout.columns);
        }
        if (repaint || secondsSince(lastStatus) > 0.25) {
            lastStatus = SteadyClock::now();
            double t = s.clock.get();
            char text[512];
            std::string vol = s.muted ? std::string("muted") : std::to_string(static_cast<int>(s.volume)) + "%";
            std::string where = count > 1 ? " " + std::to_string(position) + "/" + std::to_string(count) : "";
            if (s.live) {
                std::snprintf(text, sizeof(text), " LIVE  %s | %s | q quit", name.c_str(), styleLabel(options_).c_str());
            } else {
                int barWidth = std::max(5, std::min(30, layout.terminalColumns / 6));
                std::snprintf(text, sizeof(text), " %s %s %s / %s %s | vol %s | %s%s | drop %ld%s%s",
                              s.paused ? "||" : ">", name.c_str(), formatTime(t).c_str(),
                              duration > 0 ? formatTime(duration).c_str() : "--:--",
                              progressBar(duration > 0 ? t / duration : 0, barWidth).c_str(), vol.c_str(),
                              styleLabel(options_).c_str(), options_.loop ? " loop" : "", dropped, where.c_str(),
                              SteadyClock::now() < messageUntil ? (" | " + message).c_str() : "");
            }
            out += statusBar(text, layout);
        }
        if (!out.empty()) session_.write("\x1b[?2026h" + out + "\x1b[?2026l");
        repaint = false;

        // ---- end of the file ----
        if (s.eof && s.video.size() == 0 && s.audio.size() == 0 && !s.paused) {
            bool soundDone = !s.hasAudio || !s.audioActive;
            if (!soundDone) {
                std::lock_guard<std::mutex> lock(s.mutex);
                soundDone = secondsSince(s.lastAudioWrite) > 0.5;  // the device still plays ~0.25 s
                if (soundDone) s.clock.detachAudio();
            }
            if (soundDone) {
                if (options_.loop || animation) {
                    requestSeek(0);
                    continue;
                }
                outcome = Outcome::Next;
                break;
            }
        }
        (void)drawn;
    }

    s.stop = true;
    reader.join();
    if (audio.joinable()) audio.join();
    session_.write("\x1b[0m\x1b[2J");
    return outcome;
}

}  // namespace show
