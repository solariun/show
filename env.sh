#!/bin/sh
# ============================================================================
#  show - pictures, video and sound in the terminal (FFmpeg + Graphicx).
#  https://github.com/solariun/show
# ----------------------------------------------------------------------------
#  env.sh - installs everything show needs, prepares the repositories and
#  builds it.  Safe to run again at any time.
#
#    ./env.sh             install dependencies, fetch Graphicx, build ./show
#    ./env.sh --no-build  only install dependencies and fetch Graphicx
#    ./env.sh --install   also copy show to /usr/local/bin (asks for sudo)
#
#  What it installs
#    macOS  (Homebrew)  ffmpeg, pkg-config, git
#    Linux  (apt, dnf, pacman, zypper or apk)  a C++ compiler, make,
#           pkg-config, git and the FFmpeg development libraries
#           (libavformat, libavcodec, libavutil, libswscale, libswresample,
#           libavdevice)
#  Then it fetches the Graphicx library (git submodule external/Graphicx)
#  and runs make.
# ============================================================================
set -e

BUILD=1
INSTALL=0
for arg in "$@"; do
    case "$arg" in
        --no-build) BUILD=0 ;;
        --install) INSTALL=1 ;;
        -h|--help) sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $arg (see --help)"; exit 2 ;;
    esac
done

cd "$(dirname "$0")"
step() { printf '\n\033[1;34m==> %s\033[0m\n' "$1"; }
fail() { printf '\n\033[1;31merror:\033[0m %s\n' "$1"; exit 1; }

# Run as root when needed (Linux package managers).
SUDO=""
if [ "$(id -u)" -ne 0 ] && command -v sudo >/dev/null 2>&1; then SUDO="sudo"; fi

# ---------------------------------------------------------------- packages --
OS="$(uname -s)"
step "Installing dependencies for $OS"
if [ "$OS" = "Darwin" ]; then
    if ! command -v brew >/dev/null 2>&1; then
        fail "Homebrew is needed on macOS. Install it from https://brew.sh and run ./env.sh again."
    fi
    xcode-select -p >/dev/null 2>&1 || { echo "Installing the Xcode command line tools (a window opens)..."; xcode-select --install || true; }
    for pkg in ffmpeg pkg-config git; do
        if brew list --versions "$pkg" >/dev/null 2>&1; then
            echo "$pkg: already installed ($(brew list --versions "$pkg"))"
        else
            brew install "$pkg"
        fi
    done
elif [ "$OS" = "Linux" ]; then
    if command -v apt-get >/dev/null 2>&1; then
        $SUDO apt-get update
        $SUDO apt-get install -y build-essential pkg-config git \
            libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev libavdevice-dev
    elif command -v dnf >/dev/null 2>&1; then
        # Fedora ships "ffmpeg-free"; RPM Fusion provides the full "ffmpeg".
        $SUDO dnf install -y gcc-c++ make pkgconf-pkg-config git \
            || fail "dnf could not install the compiler tools"
        $SUDO dnf install -y ffmpeg-devel || $SUDO dnf install -y ffmpeg-free-devel \
            || fail "install the FFmpeg development package (ffmpeg-devel from RPM Fusion, or ffmpeg-free-devel)"
    elif command -v pacman >/dev/null 2>&1; then
        $SUDO pacman -S --needed --noconfirm base-devel pkgconf git ffmpeg
    elif command -v zypper >/dev/null 2>&1; then
        $SUDO zypper --non-interactive install gcc-c++ make pkg-config git \
            ffmpeg-devel || $SUDO zypper --non-interactive install gcc-c++ make pkg-config git \
            libavformat-devel libavcodec-devel libavutil-devel libswscale-devel libswresample-devel libavdevice-devel \
            || fail "install the FFmpeg development packages (openSUSE: enable the Packman repository)"
    elif command -v apk >/dev/null 2>&1; then
        $SUDO apk add build-base pkgconf git ffmpeg-dev
    else
        fail "unknown Linux package manager: install a C++ compiler, make, pkg-config, git and the FFmpeg development libraries"
    fi
else
    fail "unsupported system $OS (show runs on macOS and Linux)"
fi

# ------------------------------------------------------------ repositories --
step "Preparing the repositories (Graphicx)"
if [ -d .git ] || git rev-parse --git-dir >/dev/null 2>&1; then
    git submodule sync --recursive
    git submodule update --init --recursive
else
    # Downloaded as an archive (no git history): fetch Graphicx directly.
    if [ ! -f external/Graphicx/src/graphicx.cpp ]; then
        rm -rf external/Graphicx
        git clone --depth 1 https://github.com/solariun/Graphicx.git external/Graphicx
    fi
fi
[ -f external/Graphicx/src/graphicx.cpp ] || fail "Graphicx could not be fetched"
echo "Graphicx: $(git -C external/Graphicx log -1 --format='%h %s' 2>/dev/null || echo ready)"

# --------------------------------------------------------------- checks --
step "Checking FFmpeg"
if command -v pkg-config >/dev/null 2>&1 && \
   pkg-config --exists libavformat libavcodec libavutil libswscale libswresample libavdevice; then
    echo "FFmpeg libraries: libavformat $(pkg-config --modversion libavformat), libavcodec $(pkg-config --modversion libavcodec)"
elif [ "$OS" = "Darwin" ] && [ -f "$(brew --prefix)/include/libavformat/avformat.h" ]; then
    echo "FFmpeg libraries: found in $(brew --prefix)"
else
    fail "the FFmpeg development libraries were not found"
fi

# ---------------------------------------------------------------- build --
if [ "$BUILD" -eq 1 ]; then
    step "Building show"
    JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
    make -j"$JOBS"
    ./show --version
    if [ "$INSTALL" -eq 1 ]; then
        step "Installing to /usr/local/bin"
        if [ -w /usr/local/bin ]; then make install; else $SUDO make install; fi
    fi
    step "Ready"
    echo "Try:  ./show --help"
    echo "      ./show photo.jpg        ./show movie.mp4        ./show music.mp3"
    echo "      ./show camera           ./show info movie.mp4   ./show convert photo.jpg -o photo.txt"
else
    step "Ready (not built: run make)"
fi
