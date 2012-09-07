#!/bin/bash

#OUT=../../out/target/product/maguro
if [ "$OUT" = "" ]; then
    echo OUT variable not set, please lunch your android env
    exit 1
fi

# clean libs
pushd `dirname $0`
pushd $OUT
find -name "libnamavcodec*" -o -name "libnamavdevice*" -o -name "libnamavfilter*" -o -name "libnamavformat*" -o -name "libnamavutil*" -o -name "libnampostproc*" -o -name "libnamswresample*" -o -name "libnamswscale*" -o -name "libnamcmdutils*" | xargs rm -rf
popd;popd
