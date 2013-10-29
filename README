================================================================================
Stagefright Plugins for Android
================================================================================

1. Enhance the Android multimedia framework providing additional Plugins for
   user interaction
2. FFmpeg provides demuxers and av codecs;
3. FFmpegExtractor is a extractor plugin.
4. libstagefright_soft_ffmpegvdec plugin is video decoder
5. libstagefright_soft_ffmpegadec plugin is audio decoder

================================================================================
Why is named nam[NamExtractor]?
================================================================================

Namtso, or Lake Nam, is one of the three holy lakes in Tibet Autonomous Region
and should not be missed by any traveler to Tibet. In Tibetan, Namtso means
"Heavenly Lake." It is famous for its high altitude and imposing scenery.

I used to travel there in 2011 Sep.

================================================================================
Requirements
================================================================================

1. android_frameworks_native:
https://github.com/omxcodec/android_frameworks_native.git
branch: cm_maguro-10.1

2. android_frameworks_av:
https://github.com/omxcodec/android_frameworks_av.git
branch: cm_maguro-10.1

3. android_external_ffmpeg
git@github.com:omxcodec/android_external_ffmpeg
branch: cm_maguro-10.1

================================================================================
How to build
================================================================================

1. Get the Source
   in your "android/external" folder, run:
   stagefright-plugins(branch: master):
       git clone git@github.com:omxcodec/stagefright-plugins.git stagefright-plugins
   ffmpeg(branch: cm_maguro-10.1):
       git clone git@github.com:omxcodec/android_external_ffmpeg.git ffmpeg -b cm_maguro-10.1

   android_frameworks_native and android_frameworks_av:
       you should merge my android_frameworks_native(branch: cm_maguro-10.1) and
   android_frameworks_av(branch: cm_maguro-10.1) code. once you do, you should pay attention to
   the "USES_NAM" flag, it is only in the "android/frameworks/native" and "android/frameworks/av"
   directories.
 
2. Compile
   add USES_NAM flag to COMMON_GLOBAL_CFLAGS in your android build system(e.g. my device: maguro)
   vi vendor/samsung/maguro/BoardConfigVendor.mk and add this line to it
       COMMON_GLOBAL_CFLAGS += -DUSES_NAM
   then compile your android source tree again!

3. Install
   adb root
   adb remount
   adb sync // sync your android build system to your phone or pad
   reboot   // reboot your phone or pad

4. Run
   get test media files:
       wget http://movies.apple.com/media/us/ipad/2012/tv-spots/apple-ipad-this_good-us-20120307_848x480.mov
       wget http://movies.apple.com/media/us/ipad/2012/80ba527a-1a34-4f70-aae8-14f87ab76eea/tours/apple-ipad-feature-us-20120307_848x480.mp4
       wget http://ftp.kw.bbc.co.uk/hevc/hm-10.0-anchors/bitstreams/i_main/BQMall_832x480_60_qp22.bin

   let us suppose your media files locate at "/sdcard/Movies/" folder.
   one console window, you should run:
       adb logcat -c  //clears (flushes) the entire log and exits.
       adb logcat
   and other cosole window, you should run:
   test FFmpegExtractor plugins:
       adb shell am start -a android.intent.action.VIEW -d file:///mnt/sdcard/Movies/apple-ipad-this_good-us-20120307_848x480.mov -t video/*
       adb shell am start -a android.intent.action.VIEW -d file:///mnt/sdcard/Movies/apple-ipad-feature-us-20120307_848x480.mp4 -t video/*
   test SoftFFmpegVideo decoder plugin:
       cd android/external/stagefright-plugins/tools folder, run:
       adb root
       ./install // install my scripts
       adb root && adb shell set-vdec-sw1 // let omxcodec choose software decoder
       adb shell am start -a android.intent.action.VIEW -d file:///mnt/sdcard/Movies/apple-ipad-this_good-us-20120307_848x480.mov -t video/*
       adb shell am start -a android.intent.action.VIEW -d file:///mnt/sdcard/Movies/apple-ipad-feature-us-20120307_848x480.mp4 -t video/*

   test HEVC(H.265) decoder:
       cd android/external/stagefright-plugins/tools folder, run:
       adb root
       ./install // install my scripts
       adb root && adb shell set-vdec-drop // disable drop video frames
	   adb shell am start -a android.intent.action.VIEW -d file:///mnt/sdcard/Movies/BQMall_832x480_60_qp22.bin -t video/*

   run it and enjoy!

================================================================================
Features
================================================================================
Input formats:
    MP4 / MOV / 3GP
    TS / PS
    AVI
    ASF / WMV / WMA
    Matroska (MKV)
    Real(RM,RMVB)
    WAV
    FLV
    SWF
    APE
    DTS
    FLAC
    WAV
    OGG
    Raw HEVC(H.265) bitstreams

Video decoders:
    MPEG-1/2
    MPEG-4
    H.263
    H.264 / MPEG-4 AVC
    WMV 1/2
    WMV 3 / WMV-9 / VC-1
    RV (Real Video)
    VP8
    FLV1
    DIVX
    HEVC(H.265)

Audio decoder:
    MP2 (MPEG Layer 2)
    MP3 (MPEG Layer 3)
    AAC (MPEG-4 part3)
    AC3
    WMA 1/2
    WMA 3
    RA (Real Audio)
    APE
    DTS
    FLAC
    VORBIS
    
================================================================================
Known issues
================================================================================
1. some .mov movies do not av resync when seeking
2. some video stream ended while seeking
3. more video and audio codecs codec to be integrated
4. more formats to be integrated

If you need help with the library, or just want to discuss nam related issues, 
you can contact me: Michael Chen (omxcodec@gmail.com)
