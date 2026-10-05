#!/bin/sh
# Запуск контейнера сборки. Выполняется на хозяйской машине.
#
#   ./wpe.sh            — собрать образ (если нужно) и войти в оболочку
#   ./wpe.sh build      — собрать образ и сразу запустить сборку зависимостей
#   ./wpe.sh deps       — только сборка зависимостей (образ уже есть)
#   ./wpe.sh engine     — сборка самого WPE WebKit
#   ./wpe.sh image      — только пересобрать образ
#
# Каталог /mnt/ohos подключается внутрь по тому же пути, поэтому все записи
# в наших заметках и в файлах настройки перекрёстной сборки совпадают с тем,
# что видно изнутри.

set -e

WPE_DIR=/mnt/ohos/wpe
IMAGE=wpe-build

build_image() {
    docker build -t "$IMAGE" "$WPE_DIR"
}

# --user — чтобы созданные файлы принадлежали вам, а не root.
# HOME=/tmp — внутри контейнера вашего домашнего каталога нет, а meson и pip
# норовят туда что-нибудь записать.
run() {
    docker run --rm -it \
        -v /mnt/ohos:/mnt/ohos \
        --user "$(id -u):$(id -g)" \
        -e HOME=/tmp \
        -w "$WPE_DIR" \
        "$IMAGE" "$@"
}

case "${1:-shell}" in
    image)  build_image ;;
    shell)  build_image; run bash ;;
    deps)   run bash "$WPE_DIR/build-deps.sh" ;;
    build)  build_image; run bash "$WPE_DIR/build-deps.sh" ;;
    engine) run bash "$WPE_DIR/build-engine.sh" ;;
    *)      echo "неизвестная команда: $1"; exit 1 ;;
esac
