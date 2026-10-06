#!/bin/bash
# Сборка самого WPE WebKit. Запускается ВНУТРИ контейнера: ./wpe.sh engine
# Предполагается, что build-deps.sh уже отработал.

set -euo pipefail

WPE=/mnt/ohos/wpe
SRC=$WPE/src
BLD=$WPE/build
ROOT=/system
STAGE=$WPE/stage/root
SYSDIR=$STAGE$ROOT

V_WPE=2.54.0

say() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
die() { printf '\n\033[1;31mОШИБКА: %s\033[0m\n' "$*" >&2; exit 1; }

[ -f "$WPE/ohos.toolchain.cmake" ] || die "сначала ./wpe.sh deps"
[ -f "$SYSDIR/lib64/pkgconfig/libsoup-3.0.pc" ] || die "сначала ./wpe.sh deps"

# Поиск описаний — только в нашем промежуточном корне. Без этого CMake
# подхватывает описания машины сборки (например, её собственную glib)
# и получает чужие версии и пути.
export PKG_CONFIG_SYSROOT_DIR="$STAGE"
export PKG_CONFIG_LIBDIR="$SYSDIR/lib64/pkgconfig"
export PKG_CONFIG_PATH="$SYSDIR/lib64/pkgconfig"

mkdir -p "$SRC" "$BLD"
if [ ! -d "$SRC/wpewebkit-$V_WPE" ]; then
    say "исходники WPE WebKit $V_WPE"
    [ -f "$SRC/wpewebkit-$V_WPE.tar.xz" ] || \
        curl -fL --retry 3 -o "$SRC/wpewebkit-$V_WPE.tar.xz" \
            "https://wpewebkit.org/releases/wpewebkit-$V_WPE.tar.xz"
    tar -C "$SRC" -xf "$SRC/wpewebkit-$V_WPE.tar.xz"
fi

# ==============================================================================
#  Правки к исходникам
# ==============================================================================
# В patches/ лежат исправления, которые нужны нам и которых нет в выпуске.
# Накладываются заново при каждом запуске; уже наложенные пропускаются.

WKSRC=$SRC/wpewebkit-$V_WPE
if [ -d "$WPE/patches" ]; then
    say "правки к исходникам"
    for p in "$WPE"/patches/*.patch; do
        [ -e "$p" ] || continue
        name=$(basename "$p")
        if patch -p1 -d "$WKSRC" --dry-run --forward --silent < "$p" >/dev/null 2>&1; then
            patch -p1 -d "$WKSRC" --forward --silent < "$p" >/dev/null
            echo "    наложена:     $name"
        elif patch -p1 -d "$WKSRC" --dry-run --reverse --silent < "$p" >/dev/null 2>&1; then
            echo "    уже на месте: $name"
        else
            die "правка не ложится: $name"
        fi
    done
fi

# ==============================================================================
#  Настройка
# ==============================================================================
# Общий замысел: собрать движок, умеющий показывать страницы, и ничего сверх
# того. Остальное добавляется позже по одному.

if [ ! -f "$BLD/wpe/build.ninja" ]; then
say "настройка"
cmake -S "$SRC/wpewebkit-$V_WPE" -B "$BLD/wpe" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$WPE/ohos.toolchain.cmake" \
    -DCMAKE_INSTALL_PREFIX="$ROOT" \
    -DCMAKE_INSTALL_LIBDIR=lib64 \
    -DCMAKE_BUILD_TYPE=Release \
    -DPORT=WPE \
    \
    `# --- Площадки вывода ---------------------------------------------------` \
    `# WPEPlatform — новый способ вывода; под него мы будем писать свою` \
    `# площадку поверх поверхности OpenHarmony. Пока включена безэкранная:` \
    `# она даёт работающий движок, рисующий в память.` \
    -DENABLE_WPE_PLATFORM=ON \
    -DENABLE_WPE_PLATFORM_HEADLESS=ON \
    -DENABLE_WPE_PLATFORM_DRM=OFF \
    -DENABLE_WPE_PLATFORM_WAYLAND=OFF \
    -DENABLE_WPE_LEGACY_API=OFF \
    -DENABLE_WPE_QT_API=OFF \
    \
    `# --- Звук и видео ------------------------------------------------------` \
    `# Отключены целиком: это убирает GStreamer, самую тяжёлую зависимость.` \
    `# Воспроизведение сделаем через средства OpenHarmony.` \
    -DENABLE_VIDEO=OFF \
    -DENABLE_WEB_AUDIO=OFF \
    -DENABLE_MEDIA_STREAM=OFF \
    -DENABLE_WEB_RTC=OFF \
    -DENABLE_WEB_CODECS=OFF \
    -DUSE_GSTREAMER=OFF \
    \
    `# --- Разборщики изображений сверх обычных ------------------------------` \
    -DUSE_JPEGXL=OFF \
    -DUSE_AVIF=OFF \
    -DUSE_LCMS=OFF \
    \
    `# --- Текстовое ---------------------------------------------------------` \
    -DENABLE_XSLT=OFF \
    -DUSE_WOFF2=OFF \
    -DUSE_LIBHYPHEN=OFF \
    \
    `# --- Проверка правописания, речь, доступность --------------------------` \
    -DENABLE_SPELLCHECK=OFF \
    -DENABLE_SPEECH_SYNTHESIS=OFF \
    -DUSE_FLITE=OFF \
    -DUSE_ATK=OFF \
    \
    `# --- Прямой доступ к видеоустройству -----------------------------------` \
    `# Выводим через поверхность OpenHarmony, не через DRM напрямую.` \
    -DUSE_LIBDRM=OFF \
    -DUSE_GBM=OFF \
    \
    `# --- Прочее ------------------------------------------------------------` \
    -DENABLE_BUBBLEWRAP_SANDBOX=OFF \
    -DENABLE_GAMEPAD=OFF \
    -DENABLE_WEBDRIVER=OFF \
    -DENABLE_MINIBROWSER=OFF \
    -DENABLE_INTROSPECTION=OFF \
    -DENABLE_DOCUMENTATION=OFF \
    -DENABLE_JOURNALD_LOG=OFF \
    -DUSE_LIBBACKTRACE=OFF \
    -DUSE_SYSPROF_CAPTURE=OFF \
    -DDEVELOPER_MODE=OFF
fi

# ==============================================================================
#  Сборка
# ==============================================================================
# Около восьми тысяч единиц перевода. На обычной машине — час-полтора.
# При первом заходе WebKit иногда спотыкается о собственную гонку: заголовок
# ещё копируется, а предварительно скомпилированный блок уже собирают.
# Поэтому один повтор делаем сами.

say "сборка (журнал: $WPE/build-engine.log)"
for attempt in 1 2; do
    ninja -C "$BLD/wpe" 2>&1 | tee "$WPE/build-engine.log" | \
        grep -E --line-buffered "^\[[0-9]+/|FAILED" || true
    grep -q "FAILED" "$WPE/build-engine.log" || break
    [ "$attempt" = 2 ] && \
        die "сборка не прошла; посмотрите: grep -n 'error:' -A6 $WPE/build-engine.log | head -60"
    say "повтор после сбоя"
done

say "установка в промежуточный корень"
DESTDIR="$STAGE" ninja -C "$BLD/wpe" install >/dev/null

say "готово"
ls -1 "$SYSDIR/lib64" | grep -i wpe | sed 's/^/    /'
ls -1 "$SYSDIR/libexec/wpe-webkit-2.0" 2>/dev/null | sed 's/^/    /' || true
echo
echo "Дальше: ./wpe.sh deploy  (перенести в настоящий /system)"
