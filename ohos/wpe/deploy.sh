#!/bin/bash
# Перенос собранного в настоящий /system. Запускается НА ХОЗЯЙСКОЙ МАШИНЕ
# (там, где работает система), через ./wpe.sh deploy. Нужен sudo.

set -euo pipefail

WPE=/mnt/ohos/wpe
STAGE=$WPE/stage/root
SYSDIR=$STAGE/system
CXX18=$WPE/libcxx18

say() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
die() { printf '\n\033[1;31mОШИБКА: %s\033[0m\n' "$*" >&2; exit 1; }

[ -d "$SYSDIR/lib64" ] || die "нечего переносить: нет $SYSDIR/lib64"
[ "$(id -u)" = 0 ] && die "запускайте без sudo — он вызовется сам где нужно"

say "библиотеки, ресурсы и вспомогательные процессы"
sudo cp -a "$SYSDIR/." /system/

# Стандартная библиотека C++ лежит отдельно: её нельзя было ставить в /system
# вместе с остальным, потому что установка затёрла бы libc++.so от OpenHarmony.
# Переносим только настоящие файлы, без указателей для сборщика связей.
say "стандартная библиотека C++"
sudo cp -a "$CXX18"/lib/libc++.so.1* "$CXX18"/lib/libc++abi.so.1* \
           "$CXX18"/lib/libunwind.so.1* /system/lib64/

# epoxy ищет видеобиблиотеки по именам с номером, а OpenHarmony называет их
# без него. Даём привычные имена.
say "привычные имена для EGL и GLES"
[ -e /system/lib64/libEGL.so.1 ]    || sudo ln -sf libEGL.so    /system/lib64/libEGL.so.1
[ -e /system/lib64/libGLESv2.so.2 ] || sudo ln -sf libGLESv2.so /system/lib64/libGLESv2.so.2

# Доверенные удостоверения. OpenSSL собран с расчётом на /system/etc/ssl.
if [ ! -e /system/etc/ssl/cert.pem ]; then
    say "доверенные удостоверения"
    sudo mkdir -p /system/etc/ssl
    sudo ln -sfn /etc/ssl/certs /system/etc/ssl/certs
    sudo ln -sfn /etc/ssl/certs/ca-certificates.crt /system/etc/ssl/cert.pem
fi

say "готово"
echo "движок:"
ls -1 /system/lib64 | grep -i wpe | sed 's/^/    /'
echo "вспомогательные процессы:"
ls -1 /system/libexec/wpe-webkit-2.0 2>/dev/null | sed 's/^/    /' || true
echo "настройки шрифтов:"
ls -1 /system/etc/fonts 2>/dev/null | head -3 | sed 's/^/    /' || true
