#!/bin/bash
# Сборка расшифровщика картинки для OpenHarmony.
# Запускается ВНУТРИ контейнера.
#
# Устроено так же, как приёмник звука: отдельное дополнение к GStreamer,
# исходники GStreamer не трогаются, готовое кладётся туда, где его ищет
# GStreamer — /system/lib64/gstreamer-1.0/
#
# decodebin перебирает расшифровщики по весу. Наш — единственный, кто умеет
# H.264 в этой сборке, но вес всё равно поднят: если когда-нибудь появится
# второй, выбирать надо системный, а не посторонний.
#
# Опора на стороне системы: служба расшифровки (av_codec_service, номер
# 3011) плюс библиотека libfcodec.z.so, которую та подгружает на ходу.
# Без них элемент соберётся, но при работе скажет, что расшифровщик
# H.264 не создался. Проверить можно пробой: /mnt/ohos/wpe/avcodecprobe

set -euo pipefail

WPE=/mnt/ohos/wpe
STAGE=$WPE/stage/root
SYSDIR=$STAGE/system
NDK=/mnt/ohos/OpenHarmony-6.1-Release/prebuilts/ohos-sdk/linux/26.0.0/native
TARGET=x86_64-linux-ohos

say() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
die() { printf '\n\033[1;31mОШИБКА: %s\033[0m\n' "$*" >&2; exit 1; }

[ -f "$WPE/ohavdec.c" ] || die "нет $WPE/ohavdec.c"
[ -f "$SYSDIR/lib64/pkgconfig/gstreamer-video-1.0.pc" ] || \
    die "нет gst-plugins-base; сначала ./wpe.sh deps"
[ -f "$NDK/sysroot/usr/include/multimedia/player_framework/native_avcodec_videodecoder.h" ] || \
    die "в наборе для разработчиков нет заголовков службы расшифровки"

export PKG_CONFIG_SYSROOT_DIR=$STAGE
export PKG_CONFIG_LIBDIR=$SYSDIR/lib64/pkgconfig
export PKG_CONFIG_PATH=$SYSDIR/lib64/pkgconfig

OUT=$SYSDIR/lib64/gstreamer-1.0
mkdir -p "$OUT"

say "сборка расшифровщика картинки"
clang-18 \
    --target=$TARGET --sysroot=$NDK/sysroot \
    -resource-dir=$WPE/clang18-rt -B$WPE/clang18-b \
    -I$WPE/shim -O2 -fPIC -shared \
    -o "$OUT/libgstohavdec.so" "$WPE/ohavdec.c" \
    $(pkg-config --cflags gstreamer-1.0 gstreamer-base-1.0 gstreamer-video-1.0) \
    -I"$NDK/sysroot/usr/include" \
    -fuse-ld=lld-18 --unwindlib=none \
    $(pkg-config --libs gstreamer-1.0 gstreamer-base-1.0 gstreamer-video-1.0) \
    -L"$NDK/sysroot/usr/lib/$TARGET" \
    -lnative_media_vdec -lnative_media_codecbase -lnative_media_core

say "готово"
ls -la "$OUT" | grep -i ohavdec
echo
echo "Перенести на место: ./wpe.sh deploy"
echo
echo "Проверить, что GStreamer его видит (на устройстве, после переноса):"
echo "    GST_PLUGIN_PATH=/system/lib64/gstreamer-1.0 gst-inspect-1.0 ohavdec_h264"
