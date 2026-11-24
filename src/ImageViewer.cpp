// ============================================================================
//  show - view pictures, play video and sound, convert media, all in the
//  terminal.  C++11, FFmpeg (native libraries) + Graphicx.
//  https://github.com/solariun/show
// ----------------------------------------------------------------------------
//  File:   src/ImageViewer.cpp
//  Layer:  Viewing
//
//  What this file does
//  -------------------
//  Implementation of loadPicture() and ImageViewer (see ImageViewer.h).
//
//  How it fits in
//  --------------
//  view() decodes the picture once, then redraws only when something
//  changes (a key, a resize): it crops the visible part at the current zoom
//  and lets a gfx::Screen draw it into the space fitLayout() chose.
//
//  Contributing
//  ------------
//  Zoom 1 = the whole picture fits; each + multiplies by 1.25 (max 32x).
// ============================================================================
#include "ImageViewer.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>

#include "Media.h"

namespace show {

gfx::Image loadPicture(const std::string& path, double seconds) {
    MediaInput input(path);
    int stream = input.videoStream() >= 0 ? input.videoStream() : input.coverStream();
    if (stream < 0) throw MediaError(path + ": no picture in this file");
    Decoder decoder(input, stream);
    if (seconds > 0 && stream == input.videoStream()) {
        int64_t ts = static_cast<int64_t>((seconds + input.startTime()) * AV_TIME_BASE);
        avformat_seek_file(input.context(), -1, INT64_MIN, ts, ts, 0);
    }
    int width = 0, height = 0;
    input.displaySize(width, height);

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    gfx::Image picture;
    bool got = false, ended = false;
    while (!got && !ended) {
        if (av_read_frame(input.context(), packet) < 0) {
            decoder.send(0);  // drain: the last frames may still be inside
            ended = true;
        } else if (packet->stream_index == stream) {
            decoder.send(packet);
        }
        av_packet_unref(packet);
        while (!got && decoder.receive(frame)) {
            double t = input.seconds(stream, frame->best_effort_timestamp);
            if (seconds <= 0 || t + 1e-3 >= seconds || ended) {
                VideoScaler scaler;
                scaler.scale(frame, width > 0 ? width : frame->width, height > 0 ? height : frame->height, picture);
                got = true;
            }
            av_frame_unref(frame);
        }
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
    if (!got) throw MediaError(path + ": cannot decode a picture");
    return picture;
}

namespace {

// The part of `picture` visible at `zoom` around (cx, cy), as a new image.
gfx::Image cropView(const gfx::Image& picture, double zoom, double& cx, double& cy) {
    int w = std::max(1, static_cast<int>(picture.width() / zoom));
    int h = std::max(1, static_cast<int>(picture.height() / zoom));
    // Keep the view inside the picture.
    cx = std::max(w / 2.0, std::min(picture.width() - w / 2.0, cx));
    cy = std::max(h / 2.0, std::min(picture.height() - h / 2.0, cy));
    int x0 = static_cast<int>(cx - w / 2.0), y0 = static_cast<int>(cy - h / 2.0);
    if (zoom <= 1.0) return picture;
    gfx::Image view(w, h);
    for (int y = 0; y < h; ++y) {
        const gfx::Rgb* in = picture.row(y0 + y) + x0;
        gfx::Rgb* out = view.row(y);
        for (int x = 0; x < w; ++x) out[x] = in[x];
    }
    return view;
}

}  // namespace

ImageViewer::ImageViewer(gfx::terminal::Session& session, Options& options) : session_(session), options_(options) {}

Outcome ImageViewer::view(const std::string& path, int position, int count) {
    error_.clear();
    gfx::Image picture;
    try {
        picture = loadPicture(path);
    } catch (const MediaError& e) {
        error_ = e.what();
        return Outcome::Next;
    }

    double zoom = 1.0;
    double cx = picture.width() / 2.0, cy = picture.height() / 2.0;
    bool showInfo = false, repaint = true;
    gfx::Screen screen;
    Layout layout;
    const std::string name = baseName(path);
    std::chrono::steady_clock::time_point shownAt = std::chrono::steady_clock::now();

    for (;;) {
        if (gfx::terminal::Session::interrupted()) return Outcome::Quit;
        if (gfx::terminal::Session::resized()) repaint = true;

        if (repaint) {
            layout = fitLayout(picture.width(), picture.height());
            gfx::Image visible = cropView(picture, zoom, cx, cy);
            screen.setOptions(pictureOptions(options_, layout, gfx::Filter::Box));
            screen.setOrigin(layout.originRow, layout.originColumn);
            screen.setSynchronized(false);
            std::string out = "\x1b[?2026h\x1b[0m\x1b[2J";
            screen.draw(visible, out);
            if (showInfo) {
                std::vector<std::string> lines;
                lines.push_back(name);
                char text[160];
                std::snprintf(text, sizeof(text), "%dx%d pixels, shown as %dx%d characters", picture.width(),
                              picture.height(), layout.columns, layout.rows);
                lines.push_back(text);
                std::snprintf(text, sizeof(text), "zoom %.0f%%, view centre %.0f,%.0f", zoom * 100, cx, cy);
                lines.push_back(text);
                lines.push_back("display: " + styleLabel(options_));
                out += infoBox(lines, layout);
            }
            char status[512];
            std::string where = count > 1 ? " " + std::to_string(position) + "/" + std::to_string(count) : "";
            std::snprintf(status, sizeof(status),
                          " %s %dx%d | zoom %.0f%% | %s%s | +/- zoom, arrows move, 0 fit, n/p next/prev, q quit",
                          name.c_str(), picture.width(), picture.height(), zoom * 100, styleLabel(options_).c_str(),
                          where.c_str());
            out += statusBar(status, layout);
            out += "\x1b[?2026l";
            session_.write(out);
            repaint = false;
        }

        int key = session_.readKey(50);
        if (key == gfx::terminal::KeyNone) {
            if (options_.slideshow > 0 &&
                std::chrono::duration<double>(std::chrono::steady_clock::now() - shownAt).count() >= options_.slideshow) {
                return Outcome::Next;
            }
            continue;
        }
        double stepX = picture.width() / zoom / 8.0, stepY = picture.height() / zoom / 8.0;
        switch (key) {
            case 'q':
            case gfx::terminal::KeyEscape: return Outcome::Quit;
            case 'n':
            case ' ':
            case gfx::terminal::KeyEnter: return Outcome::Next;
            case 'p': return Outcome::Previous;
            case '+':
            case '=': zoom = std::min(32.0, zoom * 1.25); break;
            case '-':
            case '_': zoom = std::max(1.0, zoom / 1.25); break;
            case '0':
                zoom = 1.0;
                cx = picture.width() / 2.0;
                cy = picture.height() / 2.0;
                break;
            case gfx::terminal::KeyLeft: cx -= stepX; break;
            case gfx::terminal::KeyRight: cx += stepX; break;
            case gfx::terminal::KeyUp: cy -= stepY; break;
            case gfx::terminal::KeyDown: cy += stepY; break;
            case 'i': showInfo = !showInfo; break;
            default:
                if (!displayKey(key, options_)) continue;
                break;
        }
        repaint = true;
    }
}

}  // namespace show
