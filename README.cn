================================================================================
Stagefright插件
================================================================================

1. 基于Android多媒体框架提供额外的插件给用户集成
2. FFmpeg提供demux和音视频解码器
3. FFmpegExtractor属于Stagefright的一个demux(extractor)插件
4. libstagefright_soft_ffmpegvdec是一个视频解码器插件
5. libstagefright_soft_ffmpegadec是一个音频解码器插件
6. 任何FFmpegExtractor demux后的视频都优先选用视频硬件解码，充分发挥硬件特性

================================================================================
为何起名为nam[NamExtractor]?
================================================================================

纳木措是西藏三大圣湖之一，是每一个去西藏的旅行者不容错过的景点。在藏语中，
"纳木措"是“天上之湖”的意思。纳木错以其高海拔和令人窒息的美景而声名远扬。

我曾经于2011年9月到过此湖，于是便起此名

================================================================================
需要条件
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
如何编译
================================================================================

1. 获取源码
   进入你的Android源码目录 "android/external", 运行如下命令:
   stagefright-plugins(branch: master):
       git clone git@github.com:omxcodec/stagefright-plugins.git stagefright-plugins
   ffmpeg(branch: cm_maguro-10.1):
       git clone git@github.com:omxcodec/android_external_ffmpeg.git ffmpeg -b cm_maguro-10.1

   android_frameworks_native and android_frameworks_av:
       显然，你应该合并我的android_frameworks_native(branch: cm_maguro-10.1)和
   android_frameworks_av(branch: cm_maguro-10.1)的源代码。合并修改部分的代码比较少，
   而且非常有规律，我修改过的代码使用了"USES_NAM"这个宏，而且我修改过的代码仅仅在
   "android/frameworks/native"和"android/frameworks/av"两个目录。
 
2. 编译
   根据你的设备(比如说我的设备: maguro)，增加"USES_NAM" flag到Android全局编译参数"COMMON_GLOBAL_CFLAGS"中.
   编辑 vendor/samsung/maguro/BoardConfigVendor.mk 增加如下一行:
       COMMON_GLOBAL_CFLAGS += -DUSES_NAM
   然后请重新编译你的整个Android系统。

3. 安装
   adb root
   adb remount
   adb sync // 与手机或者平板同步系统，一般系统会选择更改的文件或库进行同步
   reboot   // 重启你的手机或者平板

4. 运行
   获取测试文件:
       wget http://movies.apple.com/media/us/ipad/2012/tv-spots/apple-ipad-this_good-us-20120307_848x480.mov
       wget http://movies.apple.com/media/us/ipad/2012/80ba527a-1a34-4f70-aae8-14f87ab76eea/tours/apple-ipad-feature-us-20120307_848x480.mp4
       wget http://ftp.kw.bbc.co.uk/hevc/hm-10.0-anchors/bitstreams/i_main/BQMall_832x480_60_qp22.bin

   假设你视频测试文件放在目录 "/sdcard/Movies/".
   开启一个linux终端，运行:
       adb logcat -c //清空先前所有的log
       adb logcat
   开启另外一个linux终端, 分情况运行:
   测试 FFmpegExtractor 插件:
       adb shell am start -a android.intent.action.VIEW -d file:///mnt/sdcard/Movies/apple-ipad-this_good-us-20120307_848x480.mov -t video/*
       adb shell am start -a android.intent.action.VIEW -d file:///mnt/sdcard/Movies/apple-ipad-feature-us-20120307_848x480.mp4 -t video/*
   测试 SoftFFmpegVideo 视频解码器插件:
       进入目录android/external/stagefright-plugins/tools, 运行:
       adb root
       ./install // 安装测试脚本
       adb root && adb shell set-vdec-sw1 // 让Stagefright优先选用视频软件解码器
       adb shell am start -a android.intent.action.VIEW -d file:///mnt/sdcard/Movies/apple-ipad-this_good-us-20120307_848x480.mov -t video/*
       adb shell am start -a android.intent.action.VIEW -d file:///mnt/sdcard/Movies/apple-ipad-feature-us-20120307_848x480.mp4 -t video/*
   测试 HEVC(H.265) 视频解码器:
       进入目录android/external/stagefright-plugins/tools, 运行:
       adb root
       ./install // 安装测试脚本
       adb root && adb shell set-vdec-drop0 // 禁止视频解码速度过慢时Stagefright做丢帧处理
       adb shell am start -a android.intent.action.VIEW -d file:///mnt/sdcard/Movies/BQMall_832x480_60_qp22.bin -t video/*

   希望你一切顺利

================================================================================
功能
================================================================================
输入格式:
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

视频解码器:
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

音频解码器:
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
存在问题
================================================================================
1. 一些mov文件，当seek后，音视频不能迅速同步
2. 一些文件正在seek时，退出程序
3. 需要集成更多的音视频编解码器
4. 需要集成更多的文件格式解析器

如果这个项目对您有帮助，或者您想讨论跟此相关相关的话题，请联系本人：
Michael Chen (omxcodec@gmail.com)
