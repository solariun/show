// ============================================================================
//  show - view pictures, play video and sound, convert media, all in the
//  terminal.  C++11, FFmpeg (native libraries) + Graphicx.
//  https://github.com/solariun/show
// ----------------------------------------------------------------------------
//  File:   src/Commands.h
//  Layer:  Commands
//
//  What this file does
//  -------------------
//  The non-interactive commands and the text-movie player:
//    runInfo()      `show info`: container, streams, codecs, tags, chapters
//    runConvert()   `show convert`: picture or video frame -> .txt / .png /
//                   .jpg; video -> .txt movie
//    runDevices()   `show devices`: cameras and audio outputs
//    playMovie()    plays a .txt movie (made by convert or by TBoyEmu's R key)
//    isMovieFile()  tells a terminal movie from other files
//
//  How it fits in
//  --------------
//  main.cpp dispatches here; they reuse Media (FFmpeg), loadPicture()
//  (ImageViewer) and Graphicx for the text output.
//
//  Movie file format
//  -----------------
//  Plain terminal output, so `cat` shows it too:
//    ESC ] 9999 ; show-movie fps=N ESC \   frame rate (terminals ignore it)
//    CSI 0m CSI 2J                          clear
//    per frame: CSI ?2026h <changed cells> CSI 0m CSI ?2026l
//  Files without the first line (TBoyEmu movies) play at 59.7275 fps.
//
//  Contributing
//  ------------
//  Commands print to stdout and errors to stderr and return an exit code.
// ============================================================================
#ifndef SHOW_COMMANDS_H
#define SHOW_COMMANDS_H

#include <graphicx/terminal.h>

#include <string>

#include "Options.h"
#include "Ui.h"

namespace show {

int runInfo(const Options& options);
int runConvert(const Options& options);
int runDevices();

bool isMovieFile(const std::string& path);
Outcome playMovie(gfx::terminal::Session& session, const std::string& path, Options& options);

}  // namespace show

#endif
