#!/bin/bash
# Сборка и запуск пробы.
#
#   ./wpe.sh shell          — войти в контейнер
#   bash smoke.sh build     — собрать (внутри контейнера)
#
#   bash smoke.sh run       — запустить (НА ХОЗЯЙСКОЙ МАШИНЕ, не в контейнере:
#                             собранное предназначено для OpenHarmony, а она
#                             работает именно там)
#   bash smoke.sh run https://example.com

set -euo pipefail

WPE=/mnt/ohos/wpe
PREFIX=$WPE/out
CXX18=$WPE/libcxx18
NDK=/mnt/ohos/OpenHarmony-6.1-Release/prebuilts/ohos-sdk/linux/26.0.0/native
TARGET=x86_64-linux-ohos

case "${1:-run}" in
build)
    export PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig
    export PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig
    export PKG_CONFIG_SYSROOT_DIR=/

    # Доводы ровно те же, что у всей сборки: своя стандартная библиотека C++,
    # подкладка под заголовки ядра, пустые опорные файлы для clang 18.
    clang++-18 \
        --target=$TARGET --sysroot=$NDK/sysroot \
        -resource-dir=$WPE/clang18-rt -B$WPE/clang18-b \
        -nostdinc++ -isystem $CXX18/include/c++/v1 \
        -I$WPE/shim -O2 \
        -o "$WPE/smoke" "$WPE/smoke.c" \
        $(pkg-config --cflags --libs wpe-webkit-2.0 wpe-platform-2.0) \
        -I"$PREFIX/include/wpe-webkit-2.0/wpe-platform" \
        -fuse-ld=lld-18 --unwindlib=none -nostdlib++ \
        -L"$CXX18/lib" -Wl,-rpath-link,"$CXX18/lib" -lc++ -lc++abi -lunwind
    echo "собрано: $WPE/smoke"
    ;;

run)
    shift || true
    # Библиотеки лежат не там, где их ищет загрузчик по умолчанию.
    export LD_LIBRARY_PATH="$PREFIX/lib:$CXX18/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    # Настройки шрифтов остались в промежуточном каталоге: при сборке мы
    # задали им место на устройстве (/system/etc/fonts), а туда ещё не клали.
    export FONTCONFIG_PATH="$WPE/stage/fontconfig/system/etc/fonts"
    # Движок многословен; при неполадке это единственный способ понять, где встал.
    export WEBKIT_DEBUG="${WEBKIT_DEBUG:-}"
    exec "$WPE/smoke" "$@"
    ;;

*)
    echo "неизвестная команда: $1"; exit 1 ;;
esac
