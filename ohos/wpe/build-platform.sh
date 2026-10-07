#!/bin/bash
# Сборка площадки вывода для OpenHarmony.
# Запускается ВНУТРИ контейнера: ./wpe.sh platform
#
# Площадка — отдельная подключаемая часть, исходники WebKit не трогаются.
# Готовая кладётся туда, где её ищет движок: /system/lib64/wpe-platform-2.0/modules/

set -euo pipefail

WPE=/mnt/ohos/wpe
STAGE=$WPE/stage/root
SYSDIR=$STAGE/system
CXX18=$WPE/libcxx18
NDK=/mnt/ohos/OpenHarmony-6.1-Release/prebuilts/ohos-sdk/linux/26.0.0/native
TARGET=x86_64-linux-ohos

say() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
die() { printf '\n\033[1;31mОШИБКА: %s\033[0m\n' "$*" >&2; exit 1; }

[ -f "$WPE/WPEPlatformOHOS.cpp" ] || die "нет $WPE/WPEPlatformOHOS.cpp"
[ -f "$SYSDIR/lib64/pkgconfig/wpe-platform-2.0.pc" ] || die "сначала ./wpe.sh engine"

export PKG_CONFIG_SYSROOT_DIR=$STAGE
export PKG_CONFIG_LIBDIR=$SYSDIR/lib64/pkgconfig
export PKG_CONFIG_PATH=$SYSDIR/lib64/pkgconfig

OUT=$SYSDIR/lib64/wpe-platform-2.0/modules
mkdir -p "$OUT"

say "сборка площадки"
clang++-18 \
    --target=$TARGET --sysroot=$NDK/sysroot \
    -resource-dir=$WPE/clang18-rt -B$WPE/clang18-b \
    -nostdinc++ -isystem $CXX18/include/c++/v1 \
    -I$WPE/shim -O2 -fPIC -shared -std=c++17 \
    -o "$OUT/libWPEPlatformOHOS.so" "$WPE/WPEPlatformOHOS.cpp" \
    $(pkg-config --cflags wpe-platform-2.0 gio-2.0) \
    -I"$SYSDIR/include/wpe-webkit-2.0/wpe-platform" \
    -fuse-ld=lld-18 --unwindlib=none -nostdlib++ \
    $(pkg-config --libs wpe-platform-2.0 gio-2.0) \
    -L"$NDK/sysroot/usr/lib/$TARGET" -lnative_window -lEGL -lGLESv2 \
    -L"$CXX18/lib" -Wl,-rpath-link,"$CXX18/lib" -lc++ -lc++abi -lunwind

say "готово"
ls -la "$OUT"
echo
echo "Перенести на место: ./wpe.sh deploy"
