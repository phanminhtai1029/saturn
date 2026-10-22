#!/bin/sh
# Install saturn on Linux or macOS.
#   Inside a clone with a C compiler: builds from source.
#   Otherwise (e.g. curl ... | sh):    downloads the latest release binary.
# Installs into $PREFIX/bin, default ~/.local/bin. No sudo needed.
set -e

REPO="phanminhtai1029/saturn"
PREFIX="${PREFIX:-$HOME/.local}"
BIN="$PREFIX/bin"
mkdir -p "$BIN"

src=""
if [ -f "$(dirname "$0")/../saturn.c" ]; then
    src="$(cd "$(dirname "$0")/.." && pwd)"
elif [ -f "./saturn.c" ]; then
    src="$(pwd)"
fi

if [ -n "$src" ] && command -v make >/dev/null 2>&1 && command -v cc >/dev/null 2>&1; then
    echo "Building saturn from source in $src"
    make -C "$src" install PREFIX="$PREFIX"
else
    os=$(uname -s)
    arch=$(uname -m)
    case "$os" in
        Linux)
            case "$arch" in
                x86_64|amd64) asset="saturn-linux-x86_64" ;;
                aarch64|arm64) asset="saturn-linux-arm64" ;;
                *) echo "No prebuilt binary for Linux/$arch; clone the repo and run 'make install'." >&2; exit 1 ;;
            esac ;;
        Darwin) asset="saturn-macos-universal" ;;
        *) echo "Unsupported system: $os (on Windows use install/install.ps1)." >&2; exit 1 ;;
    esac
    url="https://github.com/$REPO/releases/latest/download/$asset"
    echo "Downloading $url"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "$url" -o "$BIN/saturn"
    elif command -v wget >/dev/null 2>&1; then
        wget -qO "$BIN/saturn" "$url"
    else
        echo "Need curl or wget to download saturn." >&2; exit 1
    fi
    chmod 755 "$BIN/saturn"
fi

echo "Installed: $BIN/saturn"
case ":$PATH:" in
    *":$BIN:"*) echo "Run it with: saturn" ;;
    *)
        case "$(basename "${SHELL:-sh}")" in
            zsh)  rc="$HOME/.zshrc" ;;
            bash) rc="$HOME/.bashrc" ;;
            fish) rc="" ;;
            *)    rc="$HOME/.profile" ;;
        esac
        echo ""
        echo "$BIN is not on your PATH yet. Add it with:"
        if [ -z "$rc" ]; then
            echo "    fish_add_path $BIN"
        else
            echo "    echo 'export PATH=\"$BIN:\$PATH\"' >> $rc && . $rc"
        fi
        echo "or run it directly: $BIN/saturn" ;;
esac
