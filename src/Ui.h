// ============================================================================
//  show - view pictures, play video and sound, convert media, all in the
//  terminal.  C++11, FFmpeg (native libraries) + Graphicx.
//  https://github.com/solariun/show
// ----------------------------------------------------------------------------
//  File:   src/Ui.h
//  Layer:  Terminal UI
//
//  What this file does
//  -------------------
//  Small pieces shared by every full-screen mode (player, picture viewer,
//  movie player, camera):
//    Layout / fitLayout()   where the picture goes: the largest grid that
//                           fits the terminal (keeping proportions), centered,
//                           with the last row kept for the status bar
//    statusBar()            the reverse-video bottom line
//    infoBox()              a box of text lines drawn over the picture (i key)
//    progressBar()          "[=====-----]"
//    displayKey()           the keys every mode shares: s (style), c (colors)
//    Outcome                what the user asked for when a mode ends
//
//  How it fits in
//  --------------
//  Pictures are drawn by gfx::Screen (Graphicx); these helpers add the text
//  around it.  Everything returns escape sequences as strings; the caller
//  writes them with gfx::terminal::Session::write() in one block.
//
//  Terminal concepts
//  -----------------
//  CSI row;col H moves the cursor, CSI 2 K erases a line, CSI 7 m is reverse
//  video, CSI 0 m resets.  Character cells are ~1:2, so a picture of w x h
//  pixels needs w columns and h/2 rows at full size.
//
//  Contributing
//  ------------
//  Keep the look consistent across modes: change it here, not in each mode.
// ============================================================================
#ifndef SHOW_UI_H
#define SHOW_UI_H

#include <graphicx/graphicx.h>

#include <string>
#include <vector>

#include "Options.h"

namespace show {

// How a full-screen mode ended.
enum class Outcome { Next, Previous, Quit };

// Placement of the picture on the terminal.
struct Layout {
    int columns, rows;               // picture size in cells
    int originRow, originColumn;     // 1-based position of its top-left cell
    int terminalColumns, terminalRows;
    int pixelWidth() const { return columns; }
    int pixelHeight() const { return rows * 2; }
};

// Largest centered grid for a picture of width x height (square pixels),
// leaving the last terminal row free for the status bar.  zoom > 1 makes
// the grid larger than the terminal is not allowed: the grid is clamped.
Layout fitLayout(int width, int height);

// Graphicx options for drawing into `layout` with the user's display
// settings.
gfx::Options pictureOptions(const Options& options, const Layout& layout, gfx::Filter filter);

// The bottom line: `text` in reverse video, truncated to the terminal width.
std::string statusBar(const std::string& text, const Layout& layout);
// Lines in a framed box at the top-left of the picture (info overlay).
std::string infoBox(const std::vector<std::string>& lines, const Layout& layout);
// "[==========----------]" of `width` characters for fraction 0..1.
std::string progressBar(double fraction, int width);
// "blocks", "ascii-color 24-bit", ...
std::string styleLabel(const Options& options);
// File name without the directory.
std::string baseName(const std::string& path);

// s: next style, c: palette <-> truecolor, [ / ]: contrast -5% / +5%,
// < / >: brightness -5% / +5%.
// Returns true when the key was one of them (the caller then repaints
// everything).
bool displayKey(int key, Options& options);

}  // namespace show

#endif
