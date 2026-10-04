# Образ для перекрёстной сборки WPE WebKit под OpenHarmony (x86_64, musl).
#
# Внутри нет ничего от OpenHarmony: пакет для разработчиков и дерево исходников
# подключаются при запуске как внешний каталог /mnt/ohos. Образ содержит только
# средства, работающие на машине сборки.

FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive

# --- Системы сборки -----------------------------------------------------------
# cmake и ninja нужны для WebKit, libjpeg-turbo и самой libc++.
# make и autotools — для libffi, pcre2, openssl, libxml2, libtasn1 и прочих,
# которые до сих пор живут на обычном configure.
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential make cmake ninja-build pkg-config \
      autoconf automake libtool m4 \
 && rm -rf /var/lib/apt/lists/*

# --- Перекрёстный компилятор --------------------------------------------------
# Берём clang 18 из Ubuntu, а не clang 15 из пакета OpenHarmony.
# Причина: WebKit 2.54 требует C++23 — std::expected, std::ranges, послабление
# с 'typename'. В пятнадцатой версии и в её libc++ этого нет.
# При этом clang 18 уже знает цель x86_64-linux-ohos (поддержка OpenHarmony
# принята в LLVM начиная с 17-й версии), поэтому ничего собирать из исходников
# не приходится — кроме стандартной библиотеки C++, её строит build-deps.sh.
RUN apt-get update && apt-get install -y --no-install-recommends \
      clang-18 lld-18 llvm-18 \
 && rm -rf /var/lib/apt/lists/*

# --- Meson --------------------------------------------------------------------
# Ставим из pip: в репозитории Ubuntu версия старее, чем ожидают glib и libsoup.
RUN apt-get update && apt-get install -y --no-install-recommends \
      python3 python3-pip python3-setuptools \
 && rm -rf /var/lib/apt/lists/* \
 && pip3 install --break-system-packages --no-cache-dir -U meson

# --- Порождение исходников ----------------------------------------------------
# WebKit пишет часть своего кода сам: привязки к JavaScript (ruby и perl),
# таблицы имён (gperf), разбор грамматик (bison, flex), чистка заголовков
# (unifdef). Без любого из них сборка остановится на полпути.
RUN apt-get update && apt-get install -y --no-install-recommends \
      ruby perl gperf bison flex unifdef \
 && rm -rf /var/lib/apt/lists/*

# --- glib ДЛЯ МАШИНЫ СБОРКИ ---------------------------------------------------
# Это не та glib, что пойдёт на устройство. Нужны её утилиты —
# glib-compile-resources, glib-mkenums, glib-genmarshal — они запускаются
# во время сборки libsoup и WebKit и должны работать здесь, а не на устройстве.
# Собственная glib под OpenHarmony собирается отдельно, скриптом build-deps.sh.
RUN apt-get update && apt-get install -y --no-install-recommends \
      libglib2.0-dev libglib2.0-dev-bin gettext \
 && rm -rf /var/lib/apt/lists/*

# --- Вспомогательное ----------------------------------------------------------
RUN apt-get update && apt-get install -y --no-install-recommends \
      curl ca-certificates xz-utils unzip git patch file less vim-tiny \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /mnt/ohos/wpe
