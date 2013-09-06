#!/bin/bash

adb root
adb remount

adb push media_codecs.xml /etc/

adb shell sync
adb shell reboot

