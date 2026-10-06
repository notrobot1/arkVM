#!/bin/bash
# Сборка всех зависимостей WPE WebKit под OpenHarmony (x86_64, musl).
# Запускается ВНУТРИ контейнера: ./wpe.sh deps
#
# Всё собирается так, будто будет жить в /system на устройстве, но кладётся
# в промежуточный каталог stage/root. Оттуда ./wpe.sh deploy переносит это
# в настоящий /system. Благодаря такому порядку все пути, вшитые в библиотеки
# при сборке (где искать дополнения gio, ресурсы, настройки шрифтов,
# вспомогательные процессы), сразу правильные.
#
# Каждая библиотека отмечается меткой в stamps/, поэтому повторный запуск
# продолжает с места остановки. Чтобы пересобрать что-то одно:
#     rm /mnt/ohos/wpe/stamps/glib && ./wpe.sh deps

set -euo pipefail

# ==============================================================================
#  Пути и версии
# ==============================================================================

TREE=/mnt/ohos/OpenHarmony-6.1-Release           # дерево исходников OpenHarmony
NDK=$TREE/prebuilts/ohos-sdk/linux/26.0.0/native # пакет для разработчиков
SYSROOT=$NDK/sysroot                             # заголовки и заглушки системы
TARGET=x86_64-linux-ohos

WPE=/mnt/ohos/wpe
SRC=$WPE/src              # распакованные исходники
BLD=$WPE/build            # каталоги сборки
LOGS=$WPE/logs            # журналы по одному на библиотеку
STAMPS=$WPE/stamps        # метки о том, что уже собрано
CXX18=$WPE/libcxx18       # наша стандартная библиотека C++ (не в /system!)
SHIM=$WPE/shim            # подкладки под изъяны заголовков
RTDIR=$WPE/clang18-rt     # подставной каталог времени исполнения для clang 18
BDIR=$WPE/clang18-b       # пустые опорные объектные файлы

ROOT=/system              # куда всё встанет НА УСТРОЙСТВЕ
STAGE=$WPE/stage/root     # промежуточный корень
SYSDIR=$STAGE$ROOT        # .../stage/root/system

V_LLVM=18.1.8
V_FFI=3.4.6
V_PCRE2=10.44
V_GLIB=2.78.6
V_XKB=1.7.0
V_EPOXY=1.5.10
V_WEBP=1.4.0
V_TASN1=4.19.0
V_GPGERR=1.50
V_GCRYPT=1.10.3
V_SQLITE=3460100
V_OPENSSL=3.0.15
V_NGHTTP2=1.61.0
V_PSL=0.21.5
V_SOUP=3.4.4
V_GNET=2.78.1
V_PNG=1.6.43
V_JPEG=3.0.3
V_XML2=2.12.7
V_EXPAT=2.6.2
V_FREETYPE=2.13.2
V_FONTCONFIG=2.15.0
V_HARFBUZZ=8.5.0
V_ICU=74.2

JOBS=$(nproc)

# ==============================================================================
#  Вспомогательное
# ==============================================================================

say()  { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m    %s\033[0m\n' "$*"; }
die()  { printf '\n\033[1;31mОШИБКА: %s\033[0m\n' "$*" >&2; exit 1; }

done_already() { [ -f "$STAMPS/$1" ]; }
mark_done()    { mkdir -p "$STAMPS"; touch "$STAMPS/$1"; }

# Выполнить шаг, записывая вывод в журнал; при неудаче показать хвост.
run_logged() {
    local name="$1"; shift
    if ! "$@" >>"$LOGS/$name.log" 2>&1; then
        warn "не удалось: $name   (журнал: $LOGS/$name.log)"
        echo
        tail -40 "$LOGS/$name.log"
        die "шаг «$name» не прошёл"
    fi
}

# Скачать и распаковать, если ещё не распаковано.
fetch() {
    local dir="$1" file="$2" url="$3"
    [ -d "$SRC/$dir" ] && return 0
    [ -f "$SRC/$file" ] || curl -fL --retry 3 -o "$SRC/$file" "$url"
    tar -C "$SRC" -xf "$SRC/$file"
    [ -d "$SRC/$dir" ] || die "после распаковки нет каталога $SRC/$dir"
}

# Сборка через обычный configure.
autotools() {
    local name="$1" srcdir="$2"; shift 2
    done_already "$name" && { warn "$name уже собран"; return 0; }
    say "$name"
    rm -rf "$BLD/$name"; mkdir -p "$BLD/$name" "$LOGS"; : > "$LOGS/$name.log"
    cd "$BLD/$name"
    run_logged "$name" "$SRC/$srcdir/configure" \
        --host=x86_64-unknown-linux-musl --prefix="$ROOT" --libdir="$ROOT/lib64" "$@"
    run_logged "$name" make -j"$JOBS"
    ( export DESTDIR="$STAGE"; run_logged "$name" make install )
    mark_done "$name"
}

# Сборка через Meson.
meson_build() {
    local name="$1" srcdir="$2"; shift 2
    done_already "$name" && { warn "$name уже собран"; return 0; }
    say "$name"
    rm -rf "$BLD/$name"; mkdir -p "$LOGS"; : > "$LOGS/$name.log"
    run_logged "$name" meson setup "$BLD/$name" "$SRC/$srcdir" \
        --cross-file "$WPE/ohos.cross" --buildtype release "$@"
    ( export DESTDIR="$STAGE"; run_logged "$name" ninja -C "$BLD/$name" install )
    mark_done "$name"
}

# Сборка через CMake.
cmake_build() {
    local name="$1" srcdir="$2"; shift 2
    done_already "$name" && { warn "$name уже собран"; return 0; }
    say "$name"
    rm -rf "$BLD/$name"; mkdir -p "$LOGS"; : > "$LOGS/$name.log"
    run_logged "$name" cmake -S "$SRC/$srcdir" -B "$BLD/$name" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$WPE/ohos.toolchain.cmake" \
        -DCMAKE_INSTALL_PREFIX="$ROOT" -DCMAKE_INSTALL_LIBDIR=lib64 \
        -DCMAKE_BUILD_TYPE=Release "$@"
    ( export DESTDIR="$STAGE"; run_logged "$name" ninja -C "$BLD/$name" install )
    mark_done "$name"
}

mkdir -p "$SRC" "$BLD" "$LOGS" "$STAMPS" "$SYSDIR/lib64/pkgconfig" "$SYSDIR/include"
[ -d "$NDK" ] || die "нет пакета для разработчиков: $NDK"

# ==============================================================================
#  Шаг 0. Подкладки, опорные файлы, описания перекрёстной сборки
# ==============================================================================

say "подготовка среды"

# --- Подкладка под заголовки ядра ---------------------------------------------
# В пакете OpenHarmony заголовки ядра взяты из набора для Android: там
# linux/socket.h объявляет sockaddr_storage, который musl уже объявил в
# sys/socket.h. Любой исходник, включающий оба, не собирается.
mkdir -p "$SHIM/linux"
cat > "$SHIM/linux/socket.h" <<'EOF'
#ifndef _UAPI_LINUX_SOCKET_H
#define _UAPI_LINUX_SOCKET_H
#define _K_SS_MAXSIZE 128
typedef unsigned short __kernel_sa_family_t;
#include <sys/socket.h>
#endif
EOF

# --- Опорные файлы для clang 18 -----------------------------------------------
# Штатный clang ожидает crtbeginS.o и crtendS.o. В OpenHarmony их нет: musl
# запускает начальные действия через .init_array, и содержать им нечего.
mkdir -p "$BDIR"
: > /tmp/empty.c
for f in crtbeginS.o crtendS.o crtbegin.o crtend.o; do
    [ -f "$BDIR/$f" ] || clang-18 --target=$TARGET -c /tmp/empty.c -o "$BDIR/$f"
done

# Подставной каталог времени исполнения: заголовки от clang 18,
# а libclang_rt.builtins.a — из пакета OpenHarmony (она на C и совместима).
mkdir -p "$RTDIR/lib/$TARGET"
ln -sfn /usr/lib/llvm-18/lib/clang/18/include "$RTDIR/include"
cp -n "$NDK/llvm/lib/clang/15.0.4/lib/$TARGET/libclang_rt.builtins.a" \
      "$RTDIR/lib/$TARGET/" 2>/dev/null || true

# --- Общие наборы доводов -----------------------------------------------------
OH="--target=$TARGET --sysroot=$SYSROOT -resource-dir=$RTDIR -B$BDIR"
# Каталога $NDK/llvm/lib/$TARGET здесь намеренно НЕТ: там лежит libc++_shared.so
# от OpenHarmony (пятнадцатой версии), и сборщик связей хватал бы её.
OHLINK="-fuse-ld=lld-18 --unwindlib=none -L$SYSDIR/lib64"
CFLAGS_COMMON="-O2 -fPIC -I$SHIM -I$SYSDIR/include"
# -nostdinc++ / -nostdlib++ отрезают стандартную библиотеку C++ от OpenHarmony
# целиком. Довод -stdlib=libc++ использовать нельзя: для этой цели он
# подставляет именно их libc++_shared.so.
CXXSTD="-nostdinc++ -isystem $CXX18/include/c++/v1"
CXXLINK="-nostdlib++ -L$CXX18/lib -Wl,-rpath-link,$CXX18/lib"
CXXLIBS="-lc++ -lc++abi -lunwind"

# --- Описание перекрёстной сборки для CMake -----------------------------------
cat > "$WPE/ohos-bootstrap.toolchain.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_LIBRARY_ARCHITECTURE $TARGET)
set(CMAKE_C_COMPILER   /usr/bin/clang-18)
set(CMAKE_CXX_COMPILER /usr/bin/clang++-18)
set(CMAKE_ASM_COMPILER /usr/bin/clang-18)
set(CMAKE_AR     /usr/bin/llvm-ar-18     CACHE FILEPATH "")
set(CMAKE_RANLIB /usr/bin/llvm-ranlib-18 CACHE FILEPATH "")
set(CMAKE_NM     /usr/bin/llvm-nm-18     CACHE FILEPATH "")
set(CMAKE_C_FLAGS_INIT   "$OH")
set(CMAKE_CXX_FLAGS_INIT "$OH")
set(CMAKE_ASM_FLAGS_INIT "$OH")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "$OHLINK")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "$OHLINK")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "$OHLINK")
set(CMAKE_FIND_ROOT_PATH $SYSDIR $SYSROOT)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
EOF

cat > "$WPE/ohos.toolchain.cmake" <<EOF
include($WPE/ohos-bootstrap.toolchain.cmake)
set(CMAKE_C_FLAGS_INIT   "$OH $CFLAGS_COMMON")
set(CMAKE_CXX_FLAGS_INIT "$OH $CFLAGS_COMMON $CXXSTD")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "$OHLINK $CXXLINK")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "$OHLINK $CXXLINK")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "$OHLINK $CXXLINK")
set(CMAKE_PREFIX_PATH $SYSDIR)
# Сюда попадает всё, что должно стоять в самом хвосте каждой строки
# связывания. Через обычные доводы связывания не задать: WebKit их перетирает.
#   libc++/libc++abi/libunwind — наша стандартная библиотека C++;
#   libsharpyuv — часть libwebp, вынесенная отдельно с версии 1.3, но не
#     упомянутая в её описании;
#   libintl — заглушка вместо перевода сообщений, которую glib собирает сама
#     при -Dnls=disabled.
set(CMAKE_CXX_STANDARD_LIBRARIES "-L$CXX18/lib $CXXLIBS -L$SYSDIR/lib64 -lsharpyuv -lintl" CACHE STRING "")
EOF

# --- Описание перекрёстной сборки для Meson -----------------------------------
# sys_root указывает на промежуточный корень: описания из pkg-config содержат
# пути вида /system/..., и к ним надо приписать stage/root.
cat > "$WPE/ohos.cross" <<EOF
[binaries]
c = ['/usr/bin/clang-18', '--target=$TARGET', '--sysroot=$SYSROOT', '-resource-dir=$RTDIR', '-B$BDIR']
cpp = ['/usr/bin/clang++-18', '--target=$TARGET', '--sysroot=$SYSROOT', '-resource-dir=$RTDIR', '-B$BDIR']
ar = '/usr/bin/llvm-ar-18'
strip = '/usr/bin/llvm-strip-18'
ranlib = '/usr/bin/llvm-ranlib-18'
pkg-config = 'pkg-config'

[properties]
sys_root = '$STAGE'
pkg_config_libdir = '$SYSDIR/lib64/pkgconfig'
needs_exe_wrapper = true

[built-in options]
prefix = '$ROOT'
libdir = 'lib64'
c_args = ['-O2', '-fPIC', '-I$SHIM', '-I$SYSDIR/include']
cpp_args = ['-O2', '-fPIC', '-I$SHIM', '-I$SYSDIR/include',
            '-nostdinc++', '-isystem', '$CXX18/include/c++/v1']
c_link_args = ['-fuse-ld=lld-18', '--unwindlib=none', '-L$SYSDIR/lib64',
               '-Wl,-rpath-link,$SYSROOT/usr/lib/$TARGET']
cpp_link_args = ['-fuse-ld=lld-18', '--unwindlib=none', '-nostdlib++',
                 '-L$SYSDIR/lib64', '-L$CXX18/lib',
                 '-Wl,-rpath-link,$SYSROOT/usr/lib/$TARGET', '-Wl,-rpath-link,$CXX18/lib',
                 '-lc++', '-lc++abi', '-lunwind']

[host_machine]
system = 'linux'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF

# --- Переменные для сборок на обычном configure -------------------------------
export CC="clang-18 $OH"
export CXX="clang++-18 $OH $CXXSTD"
export AR=llvm-ar-18
export RANLIB=llvm-ranlib-18
export STRIP=llvm-strip-18
export CFLAGS="$CFLAGS_COMMON"
export CXXFLAGS="$CFLAGS_COMMON"
export LDFLAGS="$OHLINK $CXXLINK $CXXLIBS -Wl,-rpath-link,$SYSROOT/usr/lib/$TARGET"
export PKG_CONFIG_SYSROOT_DIR="$STAGE"
export PKG_CONFIG_LIBDIR="$SYSDIR/lib64/pkgconfig"
export PKG_CONFIG_PATH="$SYSDIR/lib64/pkgconfig"

# ==============================================================================
#  Шаг 1. Стандартная библиотека C++ 18-й версии
# ==============================================================================
# Собирается НЕ в /system: там уже лежит libc++.so от OpenHarmony, и установка
# затёрла бы её. Нужные файлы перенесёт туда ./wpe.sh deploy, по именам,
# которые с системными не сталкиваются (libc++.so.1 против libc++.so).

if ! done_already libcxx18; then
    say "libc++ $V_LLVM"
    fetch "llvm-project-$V_LLVM.src" "llvm-project-$V_LLVM.src.tar.xz" \
        "https://github.com/llvm/llvm-project/releases/download/llvmorg-$V_LLVM/llvm-project-$V_LLVM.src.tar.xz"
    rm -rf "$BLD/libcxx18"; : > "$LOGS/libcxx18.log"
    run_logged libcxx18 \
    cmake -S "$SRC/llvm-project-$V_LLVM.src/runtimes" -B "$BLD/libcxx18" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$WPE/ohos-bootstrap.toolchain.cmake" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$CXX18" \
        -DLLVM_ENABLE_RUNTIMES="libunwind;libcxxabi;libcxx" \
        -DLIBCXX_CXX_ABI=libcxxabi \
        -DLIBCXX_HAS_MUSL_LIBC=ON \
        -DLIBCXX_ENABLE_SHARED=ON  -DLIBCXX_ENABLE_STATIC=ON \
        -DLIBCXXABI_ENABLE_SHARED=ON -DLIBCXXABI_ENABLE_STATIC=ON \
        -DLIBUNWIND_ENABLE_SHARED=ON -DLIBUNWIND_ENABLE_STATIC=ON \
        -DLIBCXXABI_USE_LLVM_UNWINDER=ON \
        -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXX_INCLUDE_BENCHMARKS=OFF \
        -DLIBCXXABI_INCLUDE_TESTS=OFF -DLIBUNWIND_INCLUDE_TESTS=OFF
    run_logged libcxx18 ninja -C "$BLD/libcxx18" install
    mark_done libcxx18
fi

# ==============================================================================
#  Шаг 2. Основание: libffi и pcre2 — без них не соберётся glib
# ==============================================================================

fetch "libffi-$V_FFI" "libffi-$V_FFI.tar.gz" \
    "https://github.com/libffi/libffi/releases/download/v$V_FFI/libffi-$V_FFI.tar.gz"
autotools libffi "libffi-$V_FFI" --disable-shared --enable-static --disable-docs

fetch "pcre2-$V_PCRE2" "pcre2-$V_PCRE2.tar.gz" \
    "https://github.com/PCRE2Project/pcre2/releases/download/pcre2-$V_PCRE2/pcre2-$V_PCRE2.tar.gz"
autotools pcre2 "pcre2-$V_PCRE2" --disable-shared --enable-static --enable-unicode \
    --disable-pcre2grep-libz --disable-pcre2grep-libbz2 --disable-pcre2test-libreadline

# ==============================================================================
#  Шаг 3. glib — опора всего остального в семействе
# ==============================================================================

fetch "glib-$V_GLIB" "glib-$V_GLIB.tar.xz" \
    "https://download.gnome.org/sources/glib/${V_GLIB%.*}/glib-$V_GLIB.tar.xz"
meson_build glib "glib-$V_GLIB" \
    -Dtests=false -Dnls=disabled -Dlibmount=disabled -Dselinux=disabled \
    -Dxattr=false -Dglib_assert=false -Dglib_checks=false

# В описании glib записаны пути к её утилитам (glib-mkenums, glib-genmarshal
# и прочим). Но там сборки под OpenHarmony, а запускаться эти утилиты должны
# здесь. Убираем путь совсем, оставляя одни имена: тогда они ищутся по PATH
# и находятся системные. Абсолютный путь не годится — pkg-config припишет
# к нему промежуточный корень.
sed -i 's|\${bindir}/||g' \
    "$SYSDIR/lib64/pkgconfig/glib-2.0.pc" \
    "$SYSDIR/lib64/pkgconfig/gio-2.0.pc" \
    "$SYSDIR/lib64/pkgconfig/gobject-2.0.pc"

# ==============================================================================
#  Шаг 4. Разбор клавиатуры и позднее связывание с OpenGL ES
# ==============================================================================

fetch "libxkbcommon-$V_XKB" "libxkbcommon-$V_XKB.tar.xz" \
    "https://xkbcommon.org/download/libxkbcommon-$V_XKB.tar.xz"
meson_build xkbcommon "libxkbcommon-$V_XKB" \
    -Denable-x11=false -Denable-wayland=false -Denable-xkbregistry=false \
    -Denable-docs=false -Denable-tools=false

fetch "libepoxy-$V_EPOXY" "libepoxy-$V_EPOXY.tar.gz" \
    "https://github.com/anholt/libepoxy/archive/refs/tags/$V_EPOXY.tar.gz"
meson_build epoxy "libepoxy-$V_EPOXY" -Dglx=no -Dx11=false -Degl=yes -Dtests=false

# ==============================================================================
#  Шаг 5. Изображения (zlib НЕ собираем: она есть в пакете OpenHarmony)
# ==============================================================================

fetch "libpng-$V_PNG" "libpng-$V_PNG.tar.gz" \
    "https://download.sourceforge.net/libpng/libpng-$V_PNG.tar.gz"
autotools png "libpng-$V_PNG" --disable-shared --enable-static

fetch "libjpeg-turbo-$V_JPEG" "libjpeg-turbo-$V_JPEG.tar.gz" \
    "https://github.com/libjpeg-turbo/libjpeg-turbo/archive/refs/tags/$V_JPEG.tar.gz"
cmake_build jpeg "libjpeg-turbo-$V_JPEG" \
    -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=OFF

fetch "libwebp-$V_WEBP" "libwebp-$V_WEBP.tar.gz" \
    "https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-$V_WEBP.tar.gz"
autotools webp "libwebp-$V_WEBP" --disable-shared --enable-static \
    --enable-libwebpdemux --enable-libwebpmux \
    --disable-gl --disable-sdl --disable-png --disable-jpeg --disable-tiff --disable-gif

# ==============================================================================
#  Шаг 6. ICU
# ==============================================================================
# Переходник /system/lib64/libicu.so от OpenHarmony выставляет наружу лишь
# часть ICU: нет языковых соглашений, приведения регистра с учётом языка,
# поиска по тексту. WebKit всё это использует, поэтому собираем свой —
# версии 74.2, той же, что в дереве OpenHarmony.
#
# ICU особенная: сперва надо получить её собственные средства для машины
# сборки, и только потом строить библиотеку под цель. Отсюда два захода.

if ! done_already icu-host; then
    say "ICU $V_ICU — средства для машины сборки"
    fetch icu "icu4c-${V_ICU//./_}-src.tgz" \
        "https://github.com/unicode-org/icu/releases/download/release-${V_ICU//./-}/icu4c-${V_ICU//./_}-src.tgz"
    rm -rf "$BLD/icu-host"; mkdir -p "$BLD/icu-host"; : > "$LOGS/icu-host.log"
    cd "$BLD/icu-host"
    ( unset CC CXX AR RANLIB STRIP CFLAGS CXXFLAGS LDFLAGS \
            PKG_CONFIG_LIBDIR PKG_CONFIG_PATH PKG_CONFIG_SYSROOT_DIR
      run_logged icu-host "$SRC/icu/source/configure" \
          --disable-shared --enable-static --disable-tests --disable-samples \
          --disable-extras --disable-layoutex
      run_logged icu-host make -j"$JOBS" )
    mark_done icu-host
fi

if ! done_already icu; then
    say "ICU $V_ICU — под OpenHarmony"
    rm -rf "$BLD/icu"; mkdir -p "$BLD/icu"; : > "$LOGS/icu.log"
    cd "$BLD/icu"
    run_logged icu "$SRC/icu/source/configure" \
        --host=x86_64-unknown-linux-musl --prefix="$ROOT" --libdir="$ROOT/lib64" \
        --with-cross-build="$BLD/icu-host" \
        --enable-shared --disable-static \
        --disable-tests --disable-samples --disable-extras --disable-layoutex \
        --disable-tools
    run_logged icu make -j"$JOBS"
    ( export DESTDIR="$STAGE"; run_logged icu make install )
    mark_done icu
fi

# ==============================================================================
#  Шаг 7. Шрифты
# ==============================================================================
# freetype собираем без harfbuzz (иначе зависимость кольцевая), затем harfbuzz,
# который уже видит и freetype, и ICU, и glib.
# fontconfig нужен не самому WPE, а Skia — она ищет шрифты через него.

fetch "freetype-$V_FREETYPE" "freetype-$V_FREETYPE.tar.xz" \
    "https://download.savannah.gnu.org/releases/freetype/freetype-$V_FREETYPE.tar.xz"
meson_build freetype "freetype-$V_FREETYPE" \
    -Dharfbuzz=disabled -Dbrotli=disabled -Dpng=enabled -Dzlib=system

fetch "harfbuzz-$V_HARFBUZZ" "harfbuzz-$V_HARFBUZZ.tar.xz" \
    "https://github.com/harfbuzz/harfbuzz/releases/download/$V_HARFBUZZ/harfbuzz-$V_HARFBUZZ.tar.xz"
meson_build harfbuzz "harfbuzz-$V_HARFBUZZ" \
    -Dglib=enabled -Dicu=enabled -Dfreetype=enabled \
    -Dcairo=disabled -Dtests=disabled -Ddocs=disabled -Dintrospection=disabled

fetch "expat-$V_EXPAT" "expat-$V_EXPAT.tar.xz" \
    "https://github.com/libexpat/libexpat/releases/download/R_${V_EXPAT//./_}/expat-$V_EXPAT.tar.xz"
autotools expat "expat-$V_EXPAT" --disable-shared --enable-static \
    --without-examples --without-tests --without-docbook

fetch "fontconfig-$V_FONTCONFIG" "fontconfig-$V_FONTCONFIG.tar.xz" \
    "https://www.freedesktop.org/software/fontconfig/release/fontconfig-$V_FONTCONFIG.tar.xz"
meson_build fontconfig "fontconfig-$V_FONTCONFIG" \
    --sysconfdir /system/etc \
    -Ddoc=disabled -Dtests=disabled -Dtools=disabled -Dnls=disabled \
    -Dcache-build=disabled \
    -Dbaseconfig-dir=/system/etc/fonts \
    -Dtemplate-dir=/system/etc/fonts/conf.avail \
    -Dxml-dir=/system/etc/fonts

# ==============================================================================
#  Шаг 8. Разбор разметки и база данных
# ==============================================================================

fetch "libxml2-$V_XML2" "libxml2-$V_XML2.tar.xz" \
    "https://download.gnome.org/sources/libxml2/${V_XML2%.*}/libxml2-$V_XML2.tar.xz"
autotools xml2 "libxml2-$V_XML2" --disable-shared --enable-static \
    --without-python --without-lzma --without-iconv --without-debug

fetch "sqlite-autoconf-$V_SQLITE" "sqlite-autoconf-$V_SQLITE.tar.gz" \
    "https://www.sqlite.org/2024/sqlite-autoconf-$V_SQLITE.tar.gz"
autotools sqlite "sqlite-autoconf-$V_SQLITE" --disable-shared --enable-static --disable-readline

# ==============================================================================
#  Шаг 9. Шифрование
# ==============================================================================
# gl_cv_have_weak=no — OpenHarmony выбросил из своей musl вызов pthread_cancel,
# а libgpg-error использует его как признак «потоки подключены».

fetch "libtasn1-$V_TASN1" "libtasn1-$V_TASN1.tar.gz" \
    "https://ftp.gnu.org/gnu/libtasn1/libtasn1-$V_TASN1.tar.gz"
autotools tasn1 "libtasn1-$V_TASN1" --disable-shared --enable-static \
    --disable-doc --disable-gtk-doc

if ! done_already gpgerror; then
    fetch "libgpg-error-$V_GPGERR" "libgpg-error-$V_GPGERR.tar.bz2" \
        "https://gnupg.org/ftp/gcrypt/libgpg-error/libgpg-error-$V_GPGERR.tar.bz2"
    export gl_cv_have_weak=no
    autotools gpgerror "libgpg-error-$V_GPGERR" --disable-shared --enable-static \
        --disable-nls --disable-doc --disable-tests --enable-threads=posix
fi

# libgcrypt ищет сведения о libgpg-error через сценарий gpg-error-config.
# Настоящий назвал бы пути на устройстве (/system/...), которых на машине
# сборки нет, поэтому подкладываем свой — с путями промежуточного корня.
if ! done_already gcrypt; then
    mkdir -p "$SYSDIR/bin"
    cat > "$SYSDIR/bin/gpg-error-config" <<EOF
#!/bin/sh
while [ \$# -gt 0 ]; do
    case "\$1" in
        --version) echo "$V_GPGERR" ;;
        --cflags)  echo "-I$SYSDIR/include" ;;
        --libs)    echo "-L$SYSDIR/lib64 -lgpg-error" ;;
    esac
    shift
done
EOF
    chmod +x "$SYSDIR/bin/gpg-error-config"

    fetch "libgcrypt-$V_GCRYPT" "libgcrypt-$V_GCRYPT.tar.bz2" \
        "https://gnupg.org/ftp/gcrypt/libgcrypt/libgcrypt-$V_GCRYPT.tar.bz2"
    export gl_cv_have_weak=no
    autotools gcrypt "libgcrypt-$V_GCRYPT" --disable-shared --enable-static \
        --disable-doc --disable-tests --with-libgpg-error-prefix="$SYSDIR"
fi

# OpenSSL собирается своим средством настройки.
# openssldir — где искать доверенные удостоверения уже на устройстве.
if ! done_already openssl; then
    say "openssl $V_OPENSSL"
    fetch "openssl-$V_OPENSSL" "openssl-$V_OPENSSL.tar.gz" \
        "https://www.openssl.org/source/openssl-$V_OPENSSL.tar.gz"
    rm -rf "$BLD/openssl"; mkdir -p "$BLD/openssl"; cd "$BLD/openssl"
    : > "$LOGS/openssl.log"
    run_logged openssl "$SRC/openssl-$V_OPENSSL/Configure" linux-x86_64 \
        --prefix="$ROOT" --libdir=lib64 --openssldir=/system/etc/ssl \
        no-shared no-tests no-asm \
        CC="clang-18" CFLAGS="$OH $CFLAGS_COMMON" AR="$AR" RANLIB="$RANLIB"
    run_logged openssl make -j"$JOBS"
    ( export DESTDIR="$STAGE"; run_logged openssl make install_sw )
    mark_done openssl
fi

# ==============================================================================
#  Шаг 10. Сеть
# ==============================================================================
# В сборке WPE нет выбора между libsoup и curl — сетевая часть прибита
# к libsoup. Та тянет nghttp2 (HTTP/2) и libpsl (доменные суффиксы).

fetch "nghttp2-$V_NGHTTP2" "nghttp2-$V_NGHTTP2.tar.xz" \
    "https://github.com/nghttp2/nghttp2/releases/download/v$V_NGHTTP2/nghttp2-$V_NGHTTP2.tar.xz"
autotools nghttp2 "nghttp2-$V_NGHTTP2" --enable-lib-only --disable-shared --enable-static

fetch "libpsl-$V_PSL" "libpsl-$V_PSL.tar.gz" \
    "https://github.com/rockdaboot/libpsl/releases/download/$V_PSL/libpsl-$V_PSL.tar.gz"
meson_build psl "libpsl-$V_PSL" -Druntime=no -Dbuiltin=false -Dtests=false

# tls_check=false — проверка наличия готовой glib-networking. Мы её собираем
# следующей, так что порядок обратный, и эта настройка его расплетает.
fetch "libsoup-$V_SOUP" "libsoup-$V_SOUP.tar.xz" \
    "https://download.gnome.org/sources/libsoup/${V_SOUP%.*}/libsoup-$V_SOUP.tar.xz"
meson_build soup "libsoup-$V_SOUP" \
    -Dgssapi=disabled -Dntlm=disabled -Dbrotli=disabled -Dsysprof=disabled \
    -Dintrospection=disabled -Dvapi=disabled -Ddocs=disabled \
    -Dtests=false -Dtls_check=false

# glib-networking даёт libsoup защищённые соединения поверх OpenSSL.
# Установка споткнётся на gio-querymodules — он собран под OpenHarmony и здесь
# не запускается. Он лишь готовит список готовых дополнений; без списка gio
# просматривает каталог сам. Поэтому ошибку глотаем и проверяем по файлу.
if ! done_already gnet; then
    say "glib-networking $V_GNET"
    fetch "glib-networking-$V_GNET" "glib-networking-$V_GNET.tar.xz" \
        "https://download.gnome.org/sources/glib-networking/${V_GNET%.*}/glib-networking-$V_GNET.tar.xz"
    rm -rf "$BLD/gnet"; : > "$LOGS/gnet.log"
    run_logged gnet meson setup "$BLD/gnet" "$SRC/glib-networking-$V_GNET" \
        --cross-file "$WPE/ohos.cross" --buildtype release \
        -Dgnutls=disabled -Dopenssl=enabled -Dlibproxy=disabled \
        -Dgnome_proxy=disabled -Dinstalled_tests=false
    DESTDIR="$STAGE" ninja -C "$BLD/gnet" install >>"$LOGS/gnet.log" 2>&1 || true
    [ -f "$SYSDIR/lib64/gio/modules/libgioopenssl.so" ] \
        || die "glib-networking не поставила libgioopenssl.so"
    mark_done gnet
fi

# ==============================================================================
#  Уборка
# ==============================================================================
# Исполняемые файлы на складе собраны под OpenHarmony и на машине сборки не
# запускаются. Если оставить их в stage/system/bin, CMake при настройке WebKit
# выберет оттуда glib-compile-resources вместо системного. Убираем в сторону;
# на устройство их перенесёт deploy отдельно, если понадобятся.
if [ -d "$SYSDIR/bin" ] && [ -n "$(ls -A "$SYSDIR/bin" 2>/dev/null)" ]; then
    say "убираю исполняемые файлы из stage/system/bin"
    mkdir -p "$WPE/stage/bin-target"
    mv "$SYSDIR/bin/"* "$WPE/stage/bin-target/"
fi

# ==============================================================================
#  Итог
# ==============================================================================

say "готово"
echo
echo "Стандартная библиотека C++ ($CXX18/lib):"
ls -1 "$CXX18/lib" | sed 's/^/    /'
echo
echo "Описания для поиска ($SYSDIR/lib64/pkgconfig):"
ls -1 "$SYSDIR/lib64/pkgconfig" | sed 's/^/    /'
echo
echo "Дальше: ./wpe.sh engine"
