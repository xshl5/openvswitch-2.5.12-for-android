#!/bin/bash

if [ ! -e  $ANDROID_NDK/ndk-build ]; then
	echo "ANDROID_NDK path not set or it invalid!\n"
	exit 1
fi

#ANDROID_NDK=/home/xshl5/disk_5/android_ndk/android-ndk-r15c
TOOLCHAIN_PATH=`pwd`/toolchain
APP_PLATFORM=android-24

echo "--------------------"
echo "[*] make NDK standalone toolchain"
echo "--------------------"
TOOLCHAIN_TOUCH="$TOOLCHAIN_PATH/touch"
if [ ! -f "$TOOLCHAIN_TOUCH" ]; then
    $ANDROID_NDK/build/tools/make-standalone-toolchain.sh \
        --install-dir=$TOOLCHAIN_PATH \
        --platform=$APP_PLATFORM \
        --toolchain=aarch64-linux-android-4.9 \
#        --system=linux-x86_64
    touch $TOOLCHAIN_TOUCH;
fi

SYSROOT=$TOOLCHAIN_PATH/sysroot
export PATH=$TOOLCHAIN_PATH/bin:$PATH

if [ ! -f Makefile ]; then
	CFLAGS="-O3 -pipe -fPIE -fPIC --sysroot=$SYSROOT" LDFLAGS="-pie" CC=aarch64-linux-android-gcc ./configure --host=aarch64-linux-android --with-sysroot=$SYSROOT prefix=`pwd`/build --disable-ssl --disable-libcapng
fi

make -j3
make install
