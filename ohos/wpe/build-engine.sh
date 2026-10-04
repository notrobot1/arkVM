#!/bin/bash
# Сборка самого WPE WebKit. Запускается ВНУТРИ контейнера: ./wpe.sh engine
# Предполагается, что build-deps.sh уже отработал.

set -euo pipefail

WPE=/mnt/ohos/wpe
SRC=$WPE/src
BLD=$WPE/build
PREFIX=$WPE/out
STAGE=$WPE/stage

V_WPE=2.54.0

say() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
die() { printf '\n\033[1;31mОШИБКА: %s\033[0m\n' "$*" >&2; exit 1; }

[ -f "$WPE/ohos.toolchain.cmake" ] || die "сначала ./wpe.sh deps"
[ -f "$PREFIX/lib/pkgconfig/libsoup-3.0.pc" ] || die "сначала ./wpe.sh deps"

mkdir -p "$SRC" "$BLD"
if [ ! -d "$SRC/wpewebkit-$V_WPE" ]; then
    say "исходники WPE WebKit $V_WPE"
    [ -f "$SRC/wpewebkit-$V_WPE.tar.xz" ] || \
        curl -fL --retry 3 -o "$SRC/wpewebkit-$V_WPE.tar.xz" \
            "https://wpewebkit.org/releases/wpewebkit-$V_WPE.tar.xz"
    tar -C "$SRC" -xf "$SRC/wpewebkit-$V_WPE.tar.xz"
fi

# ==============================================================================
#  Настройка
# ==============================================================================
# Ниже каждая группа отключений объяснена. Общий замысел: собрать движок,
# умеющий показывать страницы, и ничего сверх того. Всё остальное добавляется
# позже по одному, когда будет что показывать.

if [ ! -f "$BLD/wpe/build.ninja" ]; then
say "настройка"
cmake -S "$SRC/wpewebkit-$V_WPE" -B "$BLD/wpe" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$WPE/ohos.toolchain.cmake" \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_BUILD_TYPE=Release \
    -DPORT=WPE \
    \
    `# --- Площадки вывода ---------------------------------------------------` \
    `# WPEPlatform — новый способ вывода, он нам и нужен: под него мы будем` \
    `# писать свою площадку поверх поверхности OpenHarmony.` \
    `# Пока включена только безэкранная: она даёт работающий движок, который` \
    `# рисует в память, и позволяет проверить всё остальное до того, как` \
    `# появится вывод на экран.` \
    -DENABLE_WPE_PLATFORM=ON \
    -DENABLE_WPE_PLATFORM_HEADLESS=ON \
    -DENABLE_WPE_PLATFORM_DRM=OFF \
    -DENABLE_WPE_PLATFORM_WAYLAND=OFF \
    `# Прежний способ (libwpe) — отдельная библиотека и отдельный слой;` \
    `# на OpenHarmony он не нужен и только добавил бы зависимость.` \
    -DENABLE_WPE_LEGACY_API=OFF \
    -DENABLE_WPE_QT_API=OFF \
    \
    `# --- Звук и видео ------------------------------------------------------` \
    `# Отключены целиком. Это убирает GStreamer — самую тяжёлую зависимость` \
    `# из всех возможных. Воспроизведение будем приделывать отдельно, через` \
    `# собственные средства OpenHarmony, а не через GStreamer.` \
    -DENABLE_VIDEO=OFF \
    -DENABLE_WEB_AUDIO=OFF \
    -DENABLE_MEDIA_STREAM=OFF \
    -DENABLE_WEB_RTC=OFF \
    -DENABLE_WEB_CODECS=OFF \
    -DUSE_GSTREAMER=OFF \
    \
    `# --- Разборщики изображений сверх обычных ------------------------------` \
    `# PNG, JPEG и WebP включены (мы их собрали). Эти три — нет: они требуют` \
    `# ещё трёх библиотек ради форматов, которые встречаются редко.` \
    -DUSE_JPEGXL=OFF \
    -DUSE_AVIF=OFF \
    -DUSE_LCMS=OFF \
    \
    `# --- Текстовое -----------------------------------------------------------` \
    `# XSLT — преобразование разметки, почти вымерший способ; тянет libxslt.` \
    `# WOFF2 — ещё один вид сжатия шрифтов, обычных WOFF хватает.` \
    `# Переносы слов требуют libhyphen и словарей на каждый язык.` \
    -DENABLE_XSLT=OFF \
    -DUSE_WOFF2=OFF \
    -DUSE_LIBHYPHEN=OFF \
    \
    `# --- Проверка правописания и речь --------------------------------------` \
    `# Требуют enchant со словарями и flite соответственно.` \
    -DENABLE_SPELLCHECK=OFF \
    -DENABLE_SPEECH_SYNTHESIS=OFF \
    -DUSE_FLITE=OFF \
    \
    `# --- Доступность ---------------------------------------------------------` \
    `# ATK — настольный способ связи с чтецами экрана. В OpenHarmony свой,` \
    `# и соединять их придётся отдельной работой.` \
    -DUSE_ATK=OFF \
    \
    `# --- Прямой доступ к видеоустройству -------------------------------------` \
    `# Мы выводим через поверхность OpenHarmony, а не через DRM напрямую,` \
    `# поэтому libdrm и gbm не нужны. Следствие: отключается и отдельный` \
    `# процесс отрисовки (он опирается на gbm).` \
    -DUSE_LIBDRM=OFF \
    -DUSE_GBM=OFF \
    \
    `# --- Прочее --------------------------------------------------------------` \
    `# Песочница на bubblewrap опирается на возможности ядра и на отдельную` \
    `# утилиту; на OpenHarmony разграничение устроено своими средствами.` \
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

say "сборка (журнал: $WPE/build-engine.log)"
ninja -C "$BLD/wpe" 2>&1 | tee "$WPE/build-engine.log" | \
    grep -E --line-buffered "^\[[0-9]+/|FAILED" || true

grep -q "FAILED" "$WPE/build-engine.log" && \
    die "сборка не прошла; посмотрите: grep -n 'error:' -A6 $WPE/build-engine.log | head -60"

say "установка"
DESTDIR="$STAGE/wpe" ninja -C "$BLD/wpe" install >/dev/null
cp -a "$STAGE/wpe$PREFIX/." "$PREFIX/"

say "готово"
ls -1 "$PREFIX/lib" | grep -i wpe | sed 's/^/    /'
ls -1 "$PREFIX/libexec/wpe-webkit-2.0" 2>/dev/null | sed 's/^/    /' || true
