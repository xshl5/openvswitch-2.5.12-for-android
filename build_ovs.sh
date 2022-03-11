#!/bin/bash

TOOLCHAIN_PATH=/home/xshl5/workspace/lxc/lxc-2.0.11/toolchain
SYSROOT=$TOOLCHAIN_PATH/sysroot
export PATH=$TOOLCHAIN_PATH/bin:$PATH

CFLAGS="-O3 -pipe -fPIE -fPIC --sysroot=$SYSROOT" LDFLAGS="-pie" CC=aarch64-linux-android-gcc ./configure --host=aarch64-linux-android --with-sysroot=$SYSROOT prefix=`pwd`/build --disable-ssl --disable-libcapng
make
