#!/usr/bin/env bash

# ==============================================================================
# Remote Mouse Build Script
# 
# Usage:
#   ./build.sh                - Default: Compile for X86 host
#   ./build.sh x86            - Compile for X86 host
#   ./build.sh aarch64        - Cross-compile for AArch64 using i.MX SDK
#   ./build.sh clean          - Clean build artifacts
#   ./build.sh aarch64 clean  - Clean build artifacts for AArch64 context
# ==============================================================================

set -e

# Default settings
DEFAULT_TOOLCHAIN_DIR="/opt/fsl-imx-xwayland/5.10-gatesgarth"
TOOLCHAIN_DIR="${TOOLCHAIN_PATH:-$DEFAULT_TOOLCHAIN_DIR}"
TARGET_ARCH="x86"
ACTION="build"
BUILD_SERVER=true
BUILD_CLIENT=true

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Color output helpers
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

header() {
    echo -e "${BLUE}==================================================${NC}"
    echo -e "${BLUE} $1${NC}"
    echo -e "${BLUE}==================================================${NC}"
}

show_help() {
    echo "Remote Mouse Build Script"
    echo ""
    echo "Usage: $0 [ARCH] [ACTION] [OPTIONS]"
    echo ""
    echo "Architectures (ARCH):"
    echo "  x86, host, x86_64     Compile for X86 host architecture (default)"
    echo "  aarch64, arm64, arm   Cross-compile for AArch64 (i.MX8MP)"
    echo ""
    echo "Actions (ACTION):"
    echo "  build                 Build target binaries (default)"
    echo "  clean                 Clean build artifacts"
    echo ""
    echo "Options:"
    echo "  --toolchain PATH      Custom SDK/Toolchain directory"
    echo "                        (default: /opt/fsl-imx-xwayland/5.10-gatesgarth/)"
    echo "  --server-only         Build server binary only"
    echo "  --client-only         Build client binary only"
    echo "  -h, --help            Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0                    # Default: Build for X86 host"
    echo "  $0 aarch64            # Cross-compile for AArch64"
    echo "  $0 clean              # Clean build artifacts"
    echo "  $0 aarch64 clean      # Clean build artifacts"
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        x86|host|x86_64)
            TARGET_ARCH="x86"
            shift
            ;;
        aarch64|arm64|arm|imx8mp)
            TARGET_ARCH="aarch64"
            shift
            ;;
        build)
            ACTION="build"
            shift
            ;;
        clean)
            ACTION="clean"
            shift
            ;;
        --toolchain)
            TOOLCHAIN_DIR="$2"
            shift 2
            ;;
        --server-only)
            BUILD_SERVER=true
            BUILD_CLIENT=false
            shift
            ;;
        --client-only)
            BUILD_SERVER=false
            BUILD_CLIENT=true
            shift
            ;;
        -h|--help)
            show_help
            exit 0
            ;;
        *)
            error "Unknown argument: $1"
            show_help
            exit 1
            ;;
    esac
done

clean_all() {
    info "Cleaning build output..."
    make -C "$SCRIPT_DIR" clean 2>/dev/null || true
    if [ -d "$SCRIPT_DIR/remote_mouse_client" ]; then
        cd "$SCRIPT_DIR/remote_mouse_client"
        make clean 2>/dev/null || true
        rm -f Makefile .qmake.stash remote_mouse_gui moc_* qrc_* *.o
        cd "$SCRIPT_DIR"
    fi
    info "Clean complete."
}

if [ "$ACTION" = "clean" ]; then
    clean_all
    exit 0
fi

# Prepare environment based on architecture
if [ "$TARGET_ARCH" = "aarch64" ]; then
    header "Target: AArch64 (Cross-Compilation)"
    info "Toolchain Directory: $TOOLCHAIN_DIR"
    
    if [ ! -d "$TOOLCHAIN_DIR" ]; then
        error "Toolchain directory '$TOOLCHAIN_DIR' does not exist."
        exit 1
    fi
    
    ENV_SETUP_SCRIPT=$(ls "$TOOLCHAIN_DIR"/environment-setup-* 2>/dev/null | head -n 1)
    if [ -z "$ENV_SETUP_SCRIPT" ] || [ ! -f "$ENV_SETUP_SCRIPT" ]; then
        error "No environment-setup-* script found in '$TOOLCHAIN_DIR'."
        exit 1
    fi
    
    info "Sourcing cross-compile environment: $ENV_SETUP_SCRIPT"
    source "$ENV_SETUP_SCRIPT"
else
    header "Target: X86 (Native Compilation)"
fi

# Always clean prior qmake Makefile artifacts to avoid architecture cross-contamination
if [ "$BUILD_CLIENT" = true ] && [ -d "$SCRIPT_DIR/remote_mouse_client" ]; then
    if [ -f "$SCRIPT_DIR/remote_mouse_client/Makefile" ]; then
        make -C "$SCRIPT_DIR/remote_mouse_client" clean 2>/dev/null || true
        rm -f "$SCRIPT_DIR/remote_mouse_client/Makefile" "$SCRIPT_DIR/remote_mouse_client/.qmake.stash"
    fi
fi

# Build Server
if [ "$BUILD_SERVER" = true ]; then
    info "Building remote_mouse_server..."
    cd "$SCRIPT_DIR"
    make clean
    make
    if [ -f "$SCRIPT_DIR/remote_mouse_server" ]; then
        info "Server build successful: $SCRIPT_DIR/remote_mouse_server"
        file "$SCRIPT_DIR/remote_mouse_server"
    else
        error "Server build failed!"
        exit 1
    fi
fi

# Build Client
if [ "$BUILD_CLIENT" = true ]; then
    if command -v qmake &> /dev/null; then
        info "Building remote_mouse_client..."
        cd "$SCRIPT_DIR/remote_mouse_client"
        qmake remote_mouse.pro
        make
        if [ -f "$SCRIPT_DIR/remote_mouse_client/remote_mouse_gui" ]; then
            info "Client build successful: $SCRIPT_DIR/remote_mouse_client/remote_mouse_gui"
            file "$SCRIPT_DIR/remote_mouse_client/remote_mouse_gui"
        else
            error "Client build failed!"
            exit 1
        fi
        cd "$SCRIPT_DIR"
    else
        warn "qmake command not found. Skipping client compilation."
    fi
fi

header "Build Finished Successfully!"
