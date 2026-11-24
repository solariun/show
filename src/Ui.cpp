// ============================================================================
//  show - view pictures, play video and sound, convert media, all in the
//  terminal.  C++11, FFmpeg (native libraries) + Graphicx.
//  https://github.com/solariun/show
// ----------------------------------------------------------------------------
//  File:   src/Ui.cpp
//  Layer:  Terminal UI
//
//  What this file does
//  -------------------
//  Implementation of the shared UI helpers (see Ui.h).
//
//  How it fits in
//  --------------
//  fitLayout() asks gfx::fit() for the largest grid that fits in
//  (columns x rows - 1) and centers it.
//
//  Contributing
//  ------------
//  Strings here are ASCII only so truncation by byte count is exact.
// ============================================================================
#include "Ui.h"

#include <graphicx/terminal.h>

#include <algorithm>
#include <cstdio>

namespace show {

namespace {
void moveTo(std::string& out, int row, int column) {
    out += "\x1b[" + std::to_string(row) + ";" + std::to_string(column) + "H";
}
}  // namespace

Layout fitLayout(int width, int height) {
    gfx::terminal::Size s = gfx::terminal::size();
    Layout l;
    l.terminalColumns = s.columns;
    l.terminalRows = s.rows;
    gfx::fit(std::max(1, width), std::max(1, height), s.columns, std::max(1, s.rows - 1), l.columns, l.rows);
    l.originColumn = 1 + std::max(0, (s.columns - l.columns) / 2);
    l.originRow = 1 + std::max(0, (s.rows - 1 - l.rows) / 2);
    return l;
}

gfx::Options pictureOptions(const Options& options, const Layout& layout, gfx::Filter filter) {
    gfx::Options o;
    o.style = options.style;
    o.colorMode = options.colorMode;
    o.invert = options.invert;
    o.contrast = options.contrast;
    o.filter = filter;
    o.columns = layout.columns;
    o.rows = layout.rows;
    return o;
}

std::string statusBar(const std::string& text, const Layout& layout) {
    std::string out;
    moveTo(out, layout.terminalRows, 1);
    out += "\x1b[0m\x1b[2K\x1b[7m";
    std::string line = text;
    if (static_cast<int>(line.size()) < layout.terminalColumns) {
        line.append(static_cast<size_t>(layout.terminalColumns) - line.size(), ' ');
    }
    out += line.substr(0, static_cast<size_t>(layout.terminalColumns));
    out += "\x1b[0m";
    return out;
}

std::string infoBox(const std::vector<std::string>& lines, const Layout& layout) {
    size_t width = 0;
    for (size_t i = 0; i < lines.size(); ++i) width = std::max(width, lines[i].size());
    width = std::min(width, static_cast<size_t>(std::max(4, layout.terminalColumns - 4)));
    std::string out = "\x1b[0m";
    int row = std::max(1, layout.originRow);
    int col = std::max(1, layout.originColumn);
    for (size_t i = 0; i < lines.size() && row + static_cast<int>(i) < layout.terminalRows; ++i) {
        moveTo(out, row + static_cast<int>(i), col);
        std::string text = lines[i].substr(0, width);
        text.append(width - text.size(), ' ');
        out += "\x1b[1;38;5;231;48;5;236m " + text + " \x1b[0m";
    }
    return out;
}

std::string progressBar(double fraction, int width) {
    if (width < 3) return std::string();
    if (fraction < 0) fraction = 0;
    if (fraction > 1) fraction = 1;
    int inner = width - 2;
    int done = static_cast<int>(fraction * inner + 0.5);
    return "[" + std::string(static_cast<size_t>(done), '=') + std::string(static_cast<size_t>(inner - done), '-') + "]";
}

std::string styleLabel(const Options& options) {
    std::string s = gfx::styleName(options.style);
    if (options.style != gfx::Style::Ascii && options.colorMode == gfx::ColorMode::TrueColor) s += " 24-bit";
    if (options.contrast != 0) {
        char c[24];
        std::snprintf(c, sizeof(c), " contrast %+d%%", static_cast<int>(options.contrast * 100 + (options.contrast > 0 ? 0.5 : -0.5)));
        s += c;
    }
    return s;
}

std::string baseName(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool displayKey(int key, Options& options) {
    if (key == 's') {
        options.style = options.style == gfx::Style::Blocks
                            ? gfx::Style::AsciiColor
                            : (options.style == gfx::Style::AsciiColor ? gfx::Style::Ascii : gfx::Style::Blocks);
        return true;
    }
    if (key == '[' || key == ']') {  // contrast -5% / +5%, between -100% and +200%
        options.contrast += key == ']' ? 0.05 : -0.05;
        if (options.contrast < -1.0) options.contrast = -1.0;
        if (options.contrast > 2.0) options.contrast = 2.0;
        if (options.contrast > -0.001 && options.contrast < 0.001) options.contrast = 0;
        return true;
    }
    if (key == 'c') {
        options.colorMode = options.colorMode == gfx::ColorMode::Palette256 ? gfx::ColorMode::TrueColor
                                                                              : gfx::ColorMode::Palette256;
        return true;
    }
    return false;
}

}  // namespace show
