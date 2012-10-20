#!/bin/bash

adb root
adb remount

for file in *; do
        if [ $(basename $file) == "install.sh" ]; then
            continue
        fi
	echo $file
        adb push $file /system/bin
done

adb shell sync

