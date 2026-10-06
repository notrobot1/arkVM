#!/bin/bash
# Сборка и запуск пробы.
#
#   ./wpe.sh shell          — войти в контейнер
#   bash smoke.sh build     — собрать (внутри контейнера)
#
#   bash smoke.sh run       — запустить (НА ХОЗЯЙСКОЙ МАШИНЕ: собранное
#                             предназначено для OpenHarmony, а она там)
#   bash smoke.sh run https://example.com
#
# После ./wpe.sh deploy библиотеки лежат в /system/lib64, и загрузчик находит
# их сам — никаких особых путей задавать не надо.

set -euo pipefail

WPE=/mnt/ohos/wpe
STAGE=$WPE/stage/root
SYSDIR=$STAGE/system
CXX18=$WPE/libcxx18
NDK=/mnt/ohos/OpenHarmony-6.1-Release/prebuilts/ohos-sdk/linux/26.0.0/native
TARGET=x86_64-linux-ohos

case "${1:-run}" in
build)
    # Описания — только из промежуточного корня, иначе подхватятся
    # библиотеки машины сборки.
    export PKG_CONFIG_SYSROOT_DIR=$STAGE
    export PKG_CONFIG_LIBDIR=$SYSDIR/lib64/pkgconfig
    export PKG_CONFIG_PATH=$SYSDIR/lib64/pkgconfig

    clang++-18 \
        --target=$TARGET --sysroot=$NDK/sysroot \
        -resource-dir=$WPE/clang18-rt -B$WPE/clang18-b \
        -nostdinc++ -isystem $CXX18/include/c++/v1 \
        -I$WPE/shim -O2 \
        -o "$WPE/smoke" "$WPE/smoke.c" \
        $(pkg-config --cflags --libs wpe-webkit-2.0 wpe-platform-2.0) \
        -I"$SYSDIR/include/wpe-webkit-2.0/wpe-platform" \
        -fuse-ld=lld-18 --unwindlib=none -nostdlib++ \
        -L"$CXX18/lib" -Wl,-rpath-link,"$CXX18/lib" -lc++ -lc++abi -lunwind
    echo "собрано: $WPE/smoke"
    ;;

run)
    shift || true
    exec "$WPE/smoke" "$@"
    ;;

*)
    echo "неизвестная команда: $1"; exit 1 ;;
esac
