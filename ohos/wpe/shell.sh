#!/bin/sh
exec docker run --rm -it \
    -v /mnt/ohos:/mnt/ohos \
    --user "$(id -u):$(id -g)" \
    -e HOME=/tmp \
    -w /mnt/ohos/wpe \
    wpe-build bash
