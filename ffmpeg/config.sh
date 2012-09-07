#!/bin/bash

# ARM_EABI_TOOLCHAIN
# ANDROID_BUILD_TOP
# ANDROID_TOOLCHAIN
# TARGET_ARCH_VARIANT=armv7-a-neon
# ANDROID_EABI_TOOLCHAIN
# TARGET_ARCH=arm

if [ "$ANDROID_TOOLCHAIN" = "" ]; then
    echo ANDROID_TOOLCHAIN variable not set, please lunch your android env
    exit 1
fi

ANDROID_LIBS=$ANDROID_BUILD_TOP/out/target/product/$CM_BUILD/obj/lib
CXX_INCLUDE=$ANDROID_BUILD_TOP/bionic/libstdc++/include

rm -rf build/ffmpeg
mkdir -p build/ffmpeg

DEST=build/ffmpeg

#FLAGS="--target-os=linux --cross-prefix=arm-linux-androideabi- --arch=arm --cpu=cortex-a9 "
FLAGS="--target-os=linux --cross-prefix=arm-eabi- --arch=arm"
FLAGS="$FLAGS --enable-version3 --enable-gpl --enable-nonfree --disable-stripping --disable-ffserver --disable-ffprobe \
	--enable-protocol=file --disable-avdevice --enable-avfilter --enable-cross-compile --disable-doc --enable-small --disable-ffplay \
	--disable-yasm --disable-armv5te"

EXTRA_CFLAGS="-I$ANDROID_BUILD_TOP/system/core/include/arch/linux-arm"
EXTRA_CFLAGS="$EXTRA_CFLAGS -I$ANDROID_BUILD_TOP/bionic/libc/include"
EXTRA_CFLAGS="$EXTRA_CFLAGS -I$ANDROID_BUILD_TOP/bionic/libc/arch-arm/include"
EXTRA_CFLAGS="$EXTRA_CFLAGS -I$ANDROID_BUILD_TOP/bionic/libc/kernel/common"
EXTRA_CFLAGS="$EXTRA_CFLAGS -I$ANDROID_BUILD_TOP/bionic/libc/kernel/arch-arm"
EXTRA_CFLAGS="$EXTRA_CFLAGS -I$ANDROID_BUILD_TOP/dalvik/libnativehelper/include/nativehelper"
EXTRA_CFLAGS="$EXTRA_CFLAGS -I$CXX_INCLUDE"
EXTRA_CFLAGS="$EXTRA_CFLAGS -I$ANDROID_BUILD_TOP/frameworks/base/native/include"
EXTRA_CFLAGS="$EXTRA_CFLAGS -I$ANDROID_BUILD_TOP/external/zlib"
EXTRA_CFLAGS="$EXTRA_CFLAGS -I$ANDROID_BUILD_TOP/hardware/libhardware/include"
EXTRA_CFLAGS="$EXTRA_CFLAGS -I$ANDROID_BUILD_TOP/frameworks/base/media/libstagefright"
EXTRA_CFLAGS="$EXTRA_CFLAGS -I$ANDROID_BUILD_TOP/frameworks/base/include/media/stagefright/openmax"
EXTRA_CFLAGS="$EXTRA_CFLAGS -I$ANDROID_BUILD_TOP/frameworks/base/include -I$ANDROID_BUILD_TOP/system/core/include"

EXTRA_CFLAGS="$EXTRA_CFLAGS -march=armv7-a -mfloat-abi=softfp -mfpu=neon"
EXTRA_CFLAGS="$EXTRA_CFLAGS -fPIC -DANDROID"

EXTRA_CXXFLAGS="-Wno-multichar -fno-exceptions -fno-rtti"

EXTRA_LDFLAGS="-Wl,--fix-cortex-a8 -L$ANDROID_LIBS -Wl,-rpath-link,$ANDROID_LIBS"
EXTRA_LDFLAGS="$EXTRA_LDFLAGS -nostdlib $ANDROID_LIBS/crtbegin_so.o $ANDROID_LIBS/crtend_so.o -lc -lm -ldl"
ABI="armeabi-v7a"
DEST="$DEST/$ABI"
FLAGS="$FLAGS --prefix=$DEST"

mkdir -p $DEST

echo $FLAGS --extra-cflags="$EXTRA_CFLAGS" --extra-ldflags="$EXTRA_LDFLAGS" --extra-cxxflags="$EXTRA_CXXFLAGS" > $DEST/info.txt
./configure $FLAGS --extra-cflags="$EXTRA_CFLAGS" --extra-ldflags="$EXTRA_LDFLAGS" --extra-cxxflags="$EXTRA_CXXFLAGS" | tee $DEST/configuration.txt
#[ $PIPESTATUS == 0 ] || exit 1

# remove restrict flag
if test -f config.h; then
sed -i 's/#define restrict restrict/#define restrict/g' config.h
sed -i 's/#define HAVE_PTHREADS 1/#undef HAVE_PTHREADS \n#define HAVE_PTHREADS 1/g' config.h
sed -i 's/#define HAVE_MALLOC_H 1/#undef HAVE_MALLOC_H \n#define HAVE_MALLOC_H 1/g' config.h
sed -i 's/#define ARCH_ARM 1/#undef ARCH_ARM \n#define ARCH_ARM 1/g' config.h
fi

if test -f version.h; then
echo "have version.h" > /dev/null
else
echo "#define FFMPEG_VERSION \"0.10.2\"" >> version.h
fi

[ $PIPESTATUS == 0 ] || exit 1
