// ============================================================================
//  show - view pictures, play video and sound, convert media, all in the
//  terminal.  C++11, FFmpeg (native libraries) + Graphicx.
//  https://github.com/solariun/show
// ----------------------------------------------------------------------------
//  File:   src/ImageViewer.h
//  Layer:  Viewing
//
//  What this file does
//  -------------------
//  Declares ImageViewer, the full-screen picture viewer (zoom, pan, styles,
//  info box, slideshow), and loadPicture(), which decodes the first frame of
//  any file FFmpeg can read into a gfx::Image at full resolution.
//
//  How it fits in
//  --------------
//  main.cpp sends still pictures here and everything with a timeline to
//  Player.  Convert uses loadPicture() (and grabFrame() for a moment of a
//  video) to produce .txt / .png / .jpg files.
//
//  Terminal concepts
//  -----------------
//  The picture is resized by Graphicx with the Box filter (each character
//  cell averages all the pixels it covers), which keeps photos smooth even
//  when a 4000-pixel image becomes 150 characters.
//
//  Contributing
//  ------------
//  Zoom works on a crop of the full-resolution picture, so zooming in shows
//  real detail; keep the crop math in one place (cropView()).
// ============================================================================
#ifndef SHOW_IMAGEVIEWER_H
#define SHOW_IMAGEVIEWER_H

#include <graphicx/graphicx.h>
#include <graphicx/terminal.h>

#include <string>

#include "Options.h"
#include "Ui.h"

namespace show {

// Decodes the picture (or a video's frame at `seconds`, -1 = first frame)
// at full resolution with square pixels.  Throws MediaError.
gfx::Image loadPicture(const std::string& path, double seconds = -1);

class ImageViewer {
public:
    ImageViewer(gfx::terminal::Session& session, Options& options);

    // Shows `path` until the user moves on.  position / count: place in the
    // playlist, for the status bar.
    Outcome view(const std::string& path, int position, int count);
    const std::string& error() const { return error_; }

private:
    gfx::terminal::Session& session_;
    Options& options_;
    std::string error_;
};

}  // namespace show

#endif
