#!/bin/sh
# Запуск сборки. Выполняется на хозяйской машине.
#
#   ./wpe.sh            — собрать образ (если нужно) и войти в оболочку
#   ./wpe.sh build      — образ, зависимости и движок подряд
#   ./wpe.sh image      — только пересобрать образ
#   ./wpe.sh deps       — только зависимости
#   ./wpe.sh engine     — только движок
#   ./wpe.sh platform   — площадка вывода для OpenHarmony
#   ./wpe.sh deploy     — перенести собранное в настоящий /system (нужен sudo)
#
# Всё собирается так, будто будет жить в /system, но кладётся в stage/root.
# Перенос в настоящий /system — отдельным шагом, deploy.

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
    engine) run bash "$WPE_DIR/build-engine.sh" ;;
    platform) run bash "$WPE_DIR/build-platform.sh" ;;
    deploy) sh "$WPE_DIR/deploy.sh" ;;
    build)  build_image
            run bash "$WPE_DIR/build-deps.sh"
            run bash "$WPE_DIR/build-engine.sh" ;;
    *)      echo "неизвестная команда: $1"; exit 1 ;;
esac
