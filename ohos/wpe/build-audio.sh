#!/bin/bash
# Сборка приёмника звука для OpenHarmony.
# Запускается ВНУТРИ контейнера: ./wpe.sh audio
#
# Приёмник — отдельное дополнение к GStreamer, исходники GStreamer не
# трогаются. Готовое кладётся туда, где его ищет GStreamer:
# /system/lib64/gstreamer-1.0/
#
# Движок просит приёмник звука по имени autoaudiosink, тот перебирает
# доступные по весу. У нашего вес выше, чем у alsasink, поэтому выберут
# его. alsasink при этом остаётся — как запасной и как мера для сравнения.

set -euo pipefail

WPE=/mnt/ohos/wpe
STAGE=$WPE/stage/root
SYSDIR=$STAGE/system
CXX18=$WPE/libcxx18
NDK=/mnt/ohos/OpenHarmony-6.1-Release/prebuilts/ohos-sdk/linux/26.0.0/native
TARGET=x86_64-linux-ohos

say() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
die() { printf '\n\033[1;31mОШИБКА: %s\033[0m\n' "$*" >&2; exit 1; }

[ -f "$WPE/ohaudiosink.c" ] || die "нет $WPE/ohaudiosink.c"
[ -f "$SYSDIR/lib64/pkgconfig/gstreamer-audio-1.0.pc" ] || \
    die "нет gst-plugins-base; сначала ./wpe.sh deps"

# Свод вызовов к звуку лежит в наборе для разработчиков, а сама библиотека
# в этом продукте не собирается — её надо собрать и установить отдельно:
#     ninja -C out/arkvm \
#       foundation/multimedia/audio_framework/frameworks/native/ohaudio:ohaudio
# и положить в /system/lib64 (и ссылкой в /system/lib64/ndk).
[ -f "$NDK/sysroot/usr/include/ohaudio/native_audiorenderer.h" ] || \
    die "в наборе для разработчиков нет заголовков ohaudio"

export PKG_CONFIG_SYSROOT_DIR=$STAGE
export PKG_CONFIG_LIBDIR=$SYSDIR/lib64/pkgconfig
export PKG_CONFIG_PATH=$SYSDIR/lib64/pkgconfig

OUT=$SYSDIR/lib64/gstreamer-1.0
mkdir -p "$OUT"

say "сборка приёмника звука"
# На C, а не на C++: GStreamer весь на C, и однодельная библиотека здесь
# не нужна вовсе — меньше поводов для столкновений имён.
clang-18 \
    --target=$TARGET --sysroot=$NDK/sysroot \
    -resource-dir=$WPE/clang18-rt -B$WPE/clang18-b \
    -I$WPE/shim -O2 -fPIC -shared \
    -o "$OUT/libgstohaudio.so" "$WPE/ohaudiosink.c" \
    $(pkg-config --cflags gstreamer-1.0 gstreamer-base-1.0 gstreamer-audio-1.0) \
    -I"$NDK/sysroot/usr/include" \
    -fuse-ld=lld-18 --unwindlib=none \
    $(pkg-config --libs gstreamer-1.0 gstreamer-base-1.0 gstreamer-audio-1.0) \
    -L"$NDK/sysroot/usr/lib/$TARGET" -lohaudio

say "готово"
ls -la "$OUT" | grep -i ohaudio
echo
echo "Перенести на место: ./wpe.sh deploy"
echo
echo "Проверить, что GStreamer его видит (на устройстве, после deploy):"
echo "    GST_PLUGIN_PATH=/system/lib64/gstreamer-1.0 gst-inspect-1.0 ohaudiosink"
