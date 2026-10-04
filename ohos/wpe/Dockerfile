FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build pkg-config \
      python3 python3-pip python3-setuptools \
      ruby perl gperf bison flex \
      autoconf automake libtool m4 make \
      curl ca-certificates xz-utils unzip git patch file \
      libglib2.0-dev libglib2.0-dev-bin gettext \
      sudo less vim-tiny \
 && rm -rf /var/lib/apt/lists/*

RUN pip3 install --break-system-packages --no-cache-dir -U meson

ENV NDK=/mnt/ohos/OpenHarmony-6.1-Release/prebuilts/ohos-sdk/linux/26.0.0/native \
    PREFIX=/mnt/ohos/wpe/out \
    TARGET=x86_64-linux-ohos

ENV SYSROOT=$NDK/sysroot
ENV CC="$NDK/llvm/bin/clang --target=$TARGET --sysroot=$SYSROOT" \
    CXX="$NDK/llvm/bin/clang++ --target=$TARGET --sysroot=$SYSROOT" \
    AR=$NDK/llvm/bin/llvm-ar \
    RANLIB=$NDK/llvm/bin/llvm-ranlib \
    STRIP=$NDK/llvm/bin/llvm-strip \
    CFLAGS="-O2 -fPIC -I$PREFIX/include" \
    CXXFLAGS="-O2 -fPIC -I$PREFIX/include" \
    LDFLAGS="-L$PREFIX/lib" \
    PKG_CONFIG_SYSROOT_DIR=/ \
    PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig \
    PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig \
    CROSS=/mnt/ohos/wpe/ohos-x86_64.cross

WORKDIR /mnt/ohos/wpe
