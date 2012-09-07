#/bin/bash

#OUT=../../../out/target/product/maguro
if [ "$OUT" = "" ]; then
    echo OUT variable not set, please lunch your android env
    exit 1
fi

find -name "*SDL**" | xargs rm -rf
