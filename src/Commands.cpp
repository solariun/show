// ============================================================================
//  show - view pictures, play video and sound, convert media, all in the
//  terminal.  C++11, FFmpeg (native libraries) + Graphicx.
//  https://github.com/solariun/show
// ----------------------------------------------------------------------------
//  File:   src/Commands.cpp
//  Layer:  Commands
//
//  What this file does
//  -------------------
//  Implementation of info, convert, devices and the movie player (see
//  Commands.h).
//
//  How it fits in
//  --------------
//  * info reads everything from the AVFormatContext after
//    avformat_find_stream_info() (done by MediaInput).
//  * convert to .png / .jpg encodes with FFmpeg's png / mjpeg encoders; to
//    .txt it renders with gfx::render() (one picture) or records every frame
//    with a gfx::Screen (movie), the same way TBoyEmu records movies.
//  * devices asks libavdevice for its outputs and, where supported, the
//    input sources (cameras).
//
//  Contributing
//  ------------
//  Keep convert deterministic (no terminal needed): it must work in scripts
//  and pipelines.
// ============================================================================
#include "Commands.h"

extern "C" {
#include <libavdevice/avdevice.h>
#include <libavutil/imgutils.h>
}

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <thread>
#include <vector>

#include "AudioOutput.h"
#include "ImageViewer.h"
#include "Media.h"

namespace show {

namespace {

std::string lower(std::string s) {
    for (size_t i = 0; i < s.size(); ++i) s[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
    return s;
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string tagList(AVDictionary* tags, const char* indent) {
    std::string out;
    const AVDictionaryEntry* e = 0;
    while ((e = av_dict_iterate(tags, e)) != 0) {
        out += std::string(indent) + e->key + ": " + e->value + "\n";
    }
    return out;
}

// gfx::Image -> PNG or JPEG file bytes, encoded by FFmpeg.
bool writePictureFile(const gfx::Image& picture, const std::string& path, std::string& error) {
    bool jpeg = endsWith(lower(path), ".jpg") || endsWith(lower(path), ".jpeg");
    const AVCodec* codec = avcodec_find_encoder(jpeg ? AV_CODEC_ID_MJPEG : AV_CODEC_ID_PNG);
    if (!codec) {
        error = jpeg ? "this FFmpeg has no JPEG encoder" : "this FFmpeg has no PNG encoder";
        return false;
    }
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    ctx->width = picture.width();
    ctx->height = picture.height();
    ctx->time_base.num = 1;
    ctx->time_base.den = 25;
    ctx->pix_fmt = jpeg ? AV_PIX_FMT_YUVJ420P : AV_PIX_FMT_RGB24;
    if (jpeg) {
        ctx->color_range = AVCOL_RANGE_JPEG;
        ctx->flags |= AV_CODEC_FLAG_QSCALE;
        ctx->global_quality = FF_QP2LAMBDA * 3;  // high quality
    }
    int rc = avcodec_open2(ctx, codec, 0);
    if (rc < 0) {
        error = "cannot open encoder: " + ffmpegError(rc);
        avcodec_free_context(&ctx);
        return false;
    }
    // RGB24 frame from the gfx::Image, converted to the encoder's format.
    AVFrame* rgb = av_frame_alloc();
    rgb->format = AV_PIX_FMT_RGB24;
    rgb->width = picture.width();
    rgb->height = picture.height();
    av_frame_get_buffer(rgb, 0);
    for (int y = 0; y < picture.height(); ++y) {
        const gfx::Rgb* in = picture.row(y);
        std::uint8_t* out = rgb->data[0] + static_cast<size_t>(y) * rgb->linesize[0];
        for (int x = 0; x < picture.width(); ++x) {
            out[3 * x] = in[x].r;
            out[3 * x + 1] = in[x].g;
            out[3 * x + 2] = in[x].b;
        }
    }
    AVFrame* frame = rgb;
    AVFrame* converted = 0;
    if (ctx->pix_fmt != AV_PIX_FMT_RGB24) {
        converted = av_frame_alloc();
        converted->format = ctx->pix_fmt;
        converted->width = ctx->width;
        converted->height = ctx->height;
        av_frame_get_buffer(converted, 0);
        SwsContext* sws = sws_getContext(ctx->width, ctx->height, AV_PIX_FMT_RGB24, ctx->width, ctx->height,
                                         ctx->pix_fmt, SWS_BICUBIC, 0, 0, 0);
        sws_scale(sws, rgb->data, rgb->linesize, 0, ctx->height, converted->data, converted->linesize);
        sws_freeContext(sws);
        frame = converted;
    }
    frame->pts = 0;
    AVPacket* packet = av_packet_alloc();
    bool ok = avcodec_send_frame(ctx, frame) >= 0 && avcodec_send_frame(ctx, 0) >= 0 &&
              avcodec_receive_packet(ctx, packet) >= 0;
    if (ok) {
        std::ofstream file(path.c_str(), std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(packet->data), packet->size);
        ok = static_cast<bool>(file);
        if (!ok) error = "cannot write " + path;
    } else {
        error = "encoding failed";
    }
    av_packet_free(&packet);
    av_frame_free(&converted);
    av_frame_free(&rgb);
    avcodec_free_context(&ctx);
    return ok;
}

gfx::Options textOptions(const Options& options) {
    gfx::Options o;
    o.style = options.style;
    o.colorMode = options.colorMode;
    o.invert = options.invert;
    o.contrast = options.contrast;
    o.brightness = options.brightness;
    o.columns = options.width > 0 ? options.width : 80;
    return o;
}

// Video -> .txt movie.
int convertMovie(const Options& options, const std::string& in, const std::string& outPath) {
    MediaInput input(in);
    if (input.videoStream() < 0) {
        std::fprintf(stderr, "error: %s has no video to make a movie from\n", in.c_str());
        return 1;
    }
    Decoder decoder(input, input.videoStream());
    int width = 0, height = 0;
    input.displaySize(width, height);
    gfx::Options o = textOptions(options);
    int columns, rows;
    gfx::gridSize(width, height, o, columns, rows);
    o.columns = columns;
    o.rows = rows;
    o.filter = gfx::Filter::Nearest;  // frames are scaled to the grid by FFmpeg
    double fps = options.fps > 0 ? options.fps : (input.frameRate() > 0 ? input.frameRate() : 25.0);
    double step = 1.0 / fps;

    std::ofstream out(outPath.c_str(), std::ios::binary | std::ios::trunc);
    if (!out) {
        std::fprintf(stderr, "error: cannot write %s\n", outPath.c_str());
        return 1;
    }
    char header[64];
    std::snprintf(header, sizeof(header), "\x1b]9999;show-movie fps=%.6g\x1b\\", fps);
    out << header << "\x1b[0m\x1b[2J";

    gfx::Screen screen(o);
    screen.setOrigin(1, 1);
    VideoScaler scaler;
    gfx::Image picture;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    double nextOut = 0, lastT = -1;
    long written = 0;
    bool ended = false, done = false;
    std::string text;
    while (!done && !ended) {
        if (av_read_frame(input.context(), packet) < 0) {
            decoder.send(0);
            ended = true;
        } else if (packet->stream_index == input.videoStream()) {
            decoder.send(packet);
        }
        av_packet_unref(packet);
        while (decoder.receive(frame)) {
            double t = input.seconds(input.videoStream(), frame->best_effort_timestamp);
            if (t < 0) t = lastT < 0 ? 0 : lastT + step;
            lastT = t;
            if (options.duration > 0 && t >= options.duration) {
                done = true;
                av_frame_unref(frame);
                break;
            }
            if (t + 1e-6 >= nextOut) {
                scaler.scale(frame, columns, rows * 2, picture);
                // Lower frame rate: skip frames; higher: repeat this one.
                while (nextOut <= t + 1e-6) {
                    text.clear();
                    screen.draw(picture, text);
                    out.write(text.data(), static_cast<std::streamsize>(text.size()));
                    nextOut += step;
                    ++written;
                }
                if (written % 25 == 0) std::fprintf(stderr, "\r%s  %ld frames", formatTime(t).c_str(), written);
            }
            av_frame_unref(frame);
        }
    }
    out << "\x1b[0m\x1b[" << (rows + 1) << ";1H\n";
    av_frame_free(&frame);
    av_packet_free(&packet);
    std::fprintf(stderr, "\r%s: %ld frames at %.3g fps, %dx%d characters, %s\n", outPath.c_str(), written, fps,
                 columns, rows, gfx::styleName(options.style));
    std::fprintf(stderr, "play it with: show \"%s\"   (or cat it)\n", outPath.c_str());
    return out ? 0 : 1;
}

}  // namespace

// ------------------------------------------------------------------ info --

int runInfo(const Options& options) {
    int status = 0;
    for (size_t i = 0; i < options.inputs.size(); ++i) {
        try {
            MediaInput input(options.inputs[i]);
            AVFormatContext* ic = input.context();
            std::printf("%s\n", options.inputs[i].c_str());
            std::printf("  container: %s (%s)\n", ic->iformat->long_name ? ic->iformat->long_name : "?", ic->iformat->name);
            if (input.duration() > 0) std::printf("  duration:  %s (%.3f s)\n", formatTime(input.duration()).c_str(), input.duration());
            if (ic->bit_rate > 0) std::printf("  bit rate:  %lld kb/s\n", static_cast<long long>(ic->bit_rate / 1000));
            if (ic->pb) {
                int64_t size = avio_size(ic->pb);
                if (size > 0) std::printf("  size:      %.2f MB\n", size / 1048576.0);
            }
            if (ic->nb_chapters) std::printf("  chapters:  %u\n", ic->nb_chapters);
            std::string tags = tagList(ic->metadata, "    ");
            if (!tags.empty()) std::printf("  tags:\n%s", tags.c_str());
            for (unsigned k = 0; k < ic->nb_streams; ++k) {
                AVStream* st = ic->streams[k];
                AVCodecParameters* p = st->codecpar;
                const AVCodecDescriptor* d = avcodec_descriptor_get(p->codec_id);
                std::string codec = d ? std::string(d->name) + " (" + (d->long_name ? d->long_name : "") + ")" : "unknown";
                const char* type = av_get_media_type_string(p->codec_type);
                std::printf("  stream %u: %s, %s", k, type ? type : "data", codec.c_str());
                if (p->codec_type == AVMEDIA_TYPE_VIDEO) {
                    AVRational fr = av_guess_frame_rate(ic, st, 0);
                    std::printf(", %dx%d", p->width, p->height);
                    if (fr.num > 0 && fr.den > 0 && !(st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
                        std::printf(", %.3f fps", av_q2d(fr));
                    }
                    const char* pix = av_get_pix_fmt_name(static_cast<AVPixelFormat>(p->format));
                    if (pix) std::printf(", %s", pix);
                } else if (p->codec_type == AVMEDIA_TYPE_AUDIO) {
                    char layout[64] = "";
                    av_channel_layout_describe(&p->ch_layout, layout, sizeof(layout));
                    std::printf(", %d Hz, %s", p->sample_rate, layout);
                    const char* fmt = av_get_sample_fmt_name(static_cast<AVSampleFormat>(p->format));
                    if (fmt) std::printf(", %s", fmt);
                }
                if (p->bit_rate > 0) std::printf(", %lld kb/s", static_cast<long long>(p->bit_rate / 1000));
                if (st->disposition & AV_DISPOSITION_ATTACHED_PIC) std::printf(" (cover art)");
                if (st->disposition & AV_DISPOSITION_DEFAULT) std::printf(" (default)");
                std::printf("\n");
                std::string streamTags = tagList(st->metadata, "      ");
                if (!streamTags.empty()) std::printf("%s", streamTags.c_str());
            }
            std::printf("  shown as: %s\n", input.isStillImage() ? "picture (viewer)" : "timeline (player)");
        } catch (const MediaError& e) {
            std::fprintf(stderr, "error: %s\n", e.what());
            status = 1;
        }
    }
    return status;
}

// --------------------------------------------------------------- convert --

int runConvert(const Options& options) {
    const std::string in = options.inputs[0];
    const std::string outPath = options.output;
    std::string ext = lower(outPath);
    bool toPicture = endsWith(ext, ".png") || endsWith(ext, ".jpg") || endsWith(ext, ".jpeg");
    bool toText = endsWith(ext, ".txt") || endsWith(ext, ".ans") || endsWith(ext, ".ansi");
    if (!toPicture && !toText) {
        std::fprintf(stderr, "error: the output must end in .txt, .png or .jpg\n");
        return 2;
    }
    try {
        bool still = false;
        {
            MediaInput probe(in);
            still = probe.isStillImage() || probe.videoStream() < 0;
        }
        if (toText && !still && options.at < 0) return convertMovie(options, in, outPath);

        gfx::Image picture = loadPicture(in, options.at);
        if (toPicture) {
            if (options.width > 0 && options.width != picture.width()) {
                // Resize with Graphicx's sampler (Box) by rendering into an image.
                int h = std::max(1, static_cast<int>(static_cast<long long>(picture.height()) * options.width / picture.width()));
                gfx::Image resized(options.width, h);
                for (int y = 0; y < h; ++y) {
                    for (int x = 0; x < options.width; ++x) {
                        resized.set(x, y, picture.get(static_cast<int>(static_cast<long long>(x) * picture.width() / options.width),
                                                      static_cast<int>(static_cast<long long>(y) * picture.height() / h)));
                    }
                }
                picture = resized;
            }
            if (options.contrast != 0 || options.brightness != 0) {
                for (int y = 0; y < picture.height(); ++y) {
                    gfx::Rgb* row = picture.row(y);
                    for (int x = 0; x < picture.width(); ++x) {
                        if (options.contrast != 0) row[x] = gfx::adjustContrast(row[x], options.contrast);
                        if (options.brightness != 0) row[x] = gfx::adjustBrightness(row[x], options.brightness);
                    }
                }
            }
            std::string error;
            if (!writePictureFile(picture, outPath, error)) {
                std::fprintf(stderr, "error: %s\n", error.c_str());
                return 1;
            }
            std::fprintf(stderr, "%s: %dx%d picture\n", outPath.c_str(), picture.width(), picture.height());
            return 0;
        }
        gfx::Options o = textOptions(options);
        std::string text = gfx::render(picture, o);
        std::ofstream file(outPath.c_str(), std::ios::binary | std::ios::trunc);
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!file) {
            std::fprintf(stderr, "error: cannot write %s\n", outPath.c_str());
            return 1;
        }
        int columns, rows;
        gfx::gridSize(picture.width(), picture.height(), o, columns, rows);
        std::fprintf(stderr, "%s: %dx%d characters, %s, %zu bytes\n", outPath.c_str(), columns, rows,
                     gfx::styleName(o.style), text.size());
        return 0;
    } catch (const MediaError& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}

// --------------------------------------------------------------- devices --

int runDevices() {
    initFFmpeg();
    std::printf("audio outputs (--audio-device):\n");
    std::vector<std::string> outs = AudioOutput::devices();
    for (size_t i = 0; i < outs.size(); ++i) std::printf("  %s\n", outs[i].c_str());
    if (outs.empty()) std::printf("  (none in this FFmpeg build: video plays without sound)\n");

    std::printf("camera inputs (show camera [DEVICE] --camera-format F):\n");
    const AVInputFormat* f = 0;
    while ((f = av_input_video_device_next(f)) != 0) {
        std::string name = f->name;
        if (name == "lavfi") continue;  // filter graphs, not a device
        std::printf("  %s", name.c_str());
        AVDeviceInfoList* list = 0;
        if (avdevice_list_input_sources(f, 0, 0, &list) >= 0 && list) {
            std::printf("\n");
            for (int k = 0; k < list->nb_devices; ++k) {
                std::printf("    %s  %s\n", list->devices[k]->device_name,
                            list->devices[k]->device_description ? list->devices[k]->device_description : "");
            }
            avdevice_free_list_devices(&list);
        } else {
            std::string format, device;
            defaultCamera(format, device);
            std::printf("  (try: show camera %s)\n", format == name ? device.c_str() : "<device>");
        }
    }
    return 0;
}

// ----------------------------------------------------------------- movie --

bool isMovieFile(const std::string& path) {
    std::string ext = lower(path);
    if (!endsWith(ext, ".txt") && !endsWith(ext, ".ans") && !endsWith(ext, ".ansi")) return false;
    std::ifstream in(path.c_str(), std::ios::binary);
    char head[16] = {0};
    in.read(head, sizeof(head));
    return head[0] == '\x1b';
}

Outcome playMovie(gfx::terminal::Session& session, const std::string& path, Options& options) {
    std::ifstream in(path.c_str(), std::ios::binary);
    std::string movie((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    double fps = 59.7275;  // TBoyEmu movies have no header
    size_t at = movie.find("show-movie fps=");
    if (at != std::string::npos && at < 64) fps = std::atof(movie.c_str() + at + 15);
    if (fps <= 0) fps = 30;

    std::vector<std::pair<size_t, size_t> > frames;  // offset, length
    const std::string marker = "\x1b[?2026l";
    size_t start = 0;
    for (size_t pos = movie.find(marker); pos != std::string::npos; pos = movie.find(marker, start)) {
        frames.push_back(std::make_pair(start, pos + marker.size() - start));
        start = pos + marker.size();
    }
    if (frames.empty()) {  // a single picture saved as text: just print it
        session.write("\x1b[0m\x1b[2J\x1b[H" + movie);
        for (;;) {
            int key = session.readKey(100);
            if (key == 'q' || key == gfx::terminal::KeyEscape) return Outcome::Quit;
            if (key == 'p') return Outcome::Previous;
            if (key != gfx::terminal::KeyNone) return Outcome::Next;
            if (gfx::terminal::Session::interrupted()) return Outcome::Quit;
        }
    }

    typedef std::chrono::steady_clock Clock;
    const Clock::duration step = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / fps));
    for (;;) {
        Clock::time_point next = Clock::now();
        session.write("\x1b[0m\x1b[2J");
        for (size_t i = 0; i < frames.size(); ++i) {
            session.write(movie.substr(frames[i].first, frames[i].second));
            next += step;
            for (;;) {
                long ms = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(next - Clock::now()).count());
                int key = session.readKey(ms > 0 ? static_cast<int>(ms) : 0);
                if (key == 'q' || key == gfx::terminal::KeyEscape) return Outcome::Quit;
                if (key == 'n') return Outcome::Next;
                if (key == 'p') return Outcome::Previous;
                if (key == 'l') options.loop = !options.loop;
                if (key == ' ') {  // pause until space again
                    Clock::time_point pausedAt = Clock::now();
                    while (session.readKey(100) != ' ') {
                        if (gfx::terminal::Session::interrupted()) return Outcome::Quit;
                    }
                    next += Clock::now() - pausedAt;
                }
                if (gfx::terminal::Session::interrupted()) return Outcome::Quit;
                if (ms <= 0) break;
            }
        }
        if (!options.loop) return Outcome::Next;
    }
}

}  // namespace show
