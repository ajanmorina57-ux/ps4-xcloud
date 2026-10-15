#!/bin/bash
set -e

export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1

# Verify OpenOrbis Toolchain
if [ -z "$OO_PS4_TOOLCHAIN" ]; then
    export OO_PS4_TOOLCHAIN="$HOME/OpenOrbis"
fi

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
PKG_ROOT="$BUILD_DIR/package-root"
PACKAGE_DIR="$BUILD_DIR/package"
TOOLCHAIN_BIN="$OO_PS4_TOOLCHAIN/bin/linux"

# Ensure OpenSSL compatibility for PkgTool.Core on Fedora/Nobara
export LD_LIBRARY_PATH="$TOOLCHAIN_BIN/compat_lib:${LD_LIBRARY_PATH:-}"

echo "=== 1. Building ps4-xcloud binary ==="
cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DXCLOUD_CLIENT_ID="${XCLOUD_CLIENT_ID:-}"
ninja -C "$BUILD_DIR"

echo "=== 2. Preparing Required PS4 Homebrew PRX Modules ==="
mkdir -p "$PKG_ROOT/sce_sys/about" "$PKG_ROOT/sce_module" "$PACKAGE_DIR"
cp "$PROJECT_DIR/pkg_root/eboot.bin" "$PKG_ROOT/eboot.bin"
cp "$OO_PS4_TOOLCHAIN/src/modules/libc.prx" "$PKG_ROOT/sce_module/libc.prx"
cp "$OO_PS4_TOOLCHAIN/src/modules/libSceFios2.prx" "$PKG_ROOT/sce_module/libSceFios2.prx"
cp "$OO_PS4_TOOLCHAIN/src/modules/right.sprx" "$PKG_ROOT/sce_sys/about/right.sprx"
cp "$OO_PS4_TOOLCHAIN/samples/hello_world/sce_sys/icon0.png" "$PKG_ROOT/sce_sys/icon0.png"

# Recreate param.sfo with Homebrew standards
CONTENT_ID="IV0000-XCLD00001_00-XCLDPS4000000000"
TITLE_ID="XCLD00001"

rm -f "$PKG_ROOT/sce_sys/param.sfo"
echo "Creating param.sfo with PkgTool.Core..."
"$TOOLCHAIN_BIN/PkgTool.Core" sfo_new "$PKG_ROOT/sce_sys/param.sfo"
"$TOOLCHAIN_BIN/PkgTool.Core" sfo_setentry "$PKG_ROOT/sce_sys/param.sfo" APP_TYPE --type Integer --maxsize 4 --value 1
"$TOOLCHAIN_BIN/PkgTool.Core" sfo_setentry "$PKG_ROOT/sce_sys/param.sfo" APP_VER --type Utf8 --maxsize 8 --value "01.10"
"$TOOLCHAIN_BIN/PkgTool.Core" sfo_setentry "$PKG_ROOT/sce_sys/param.sfo" ATTRIBUTE --type Integer --maxsize 4 --value 0
"$TOOLCHAIN_BIN/PkgTool.Core" sfo_setentry "$PKG_ROOT/sce_sys/param.sfo" CATEGORY --type Utf8 --maxsize 4 --value "gd"
"$TOOLCHAIN_BIN/PkgTool.Core" sfo_setentry "$PKG_ROOT/sce_sys/param.sfo" CONTENT_ID --type Utf8 --maxsize 48 --value "$CONTENT_ID"
"$TOOLCHAIN_BIN/PkgTool.Core" sfo_setentry "$PKG_ROOT/sce_sys/param.sfo" DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
"$TOOLCHAIN_BIN/PkgTool.Core" sfo_setentry "$PKG_ROOT/sce_sys/param.sfo" SYSTEM_VER --type Integer --maxsize 4 --value 0
"$TOOLCHAIN_BIN/PkgTool.Core" sfo_setentry "$PKG_ROOT/sce_sys/param.sfo" TITLE --type Utf8 --maxsize 128 --value "Xbox Cloud Gaming"
"$TOOLCHAIN_BIN/PkgTool.Core" sfo_setentry "$PKG_ROOT/sce_sys/param.sfo" TITLE_ID --type Utf8 --maxsize 12 --value "$TITLE_ID"
"$TOOLCHAIN_BIN/PkgTool.Core" sfo_setentry "$PKG_ROOT/sce_sys/param.sfo" VERSION --type Utf8 --maxsize 8 --value "01.10"

echo "=== 3. Packaging into .pkg with OpenOrbis Tools ==="
cd "$PKG_ROOT"
FILES="eboot.bin sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png sce_module/libc.prx sce_module/libSceFios2.prx"

"$TOOLCHAIN_BIN/create-gp4" -out "$PKG_ROOT/project.gp4" --content-id="$CONTENT_ID" --files "$FILES"
"$TOOLCHAIN_BIN/PkgTool.Core" pkg_build "$PKG_ROOT/project.gp4" "$PACKAGE_DIR"

echo "=== Packaging Complete! ==="
ls -lh "$PACKAGE_DIR/$CONTENT_ID.pkg"
