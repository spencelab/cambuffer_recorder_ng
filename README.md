# cambuffer_recorder_ng

Note broader develop notes here:
https://github.com/aspence/spencelab/wiki/Ubuntu-22-Jammy-ROS-2-Humble-Testing-and-Mac-Multipass-Development

## Running and testing:

```
open terminal
ros2 run cambuffer_recorder_ng cambuffer_recorder_ng   --ros-args   -p backend:=fake   -p width:=640   -p height:=480   -p fps:=30   -p output_path:=/tmp/fakecam_test.mp4
open another terminal
ros2 lifecycle set /cambuffer_recorder_ng configure
ros2 lifecycle set /cambuffer_recorder_ng activate
wait awhile, check the movie in /tmp is increasing in size...
ros2 lifecycle set /cambuffer_recorder_ng deactivate
ros2 lifecycle set /cambuffer_recorder_ng shutdown
can ctrl-c the ros2 launch.
makes mpg at the target dir - open and look for rainbows!
```

ximea backend should work if built with xiapi installed. test acquires.

Has issues with sizes going into ffmpeg etc might need to be 704 tall multiple of 16. Lots of testing of bandwidth and processing here:

`/home/spencelab/ros2_ws/src/cambuffer_recorder_ng/src`

With `xi_grab**` etc. Binaries. This one for example xi_grab_debayer_ffmpeg_stream.cpp is killer and saves at 250fps into an mp4 at half res that looks good just needs white balance and gamma! Files tiny! decimated!



## Parameters
Ok what parameters do we need to utilize for Ximea looking at the old code:

* width
* height
* 

### these all args passed to camera_init...

```
  // prepare struct
  ximeaState xiState;
  xiState.xiHandle = NULL;
  xiState.image_width = image_width;
  xiState.image_height = image_height;
  xiState.pixel_depth = pixel_depth;
  xiState.exposure_time = exposure_time;
  xiState.trig_state = trig_state;
  xiState.param_val = param_val;
  xiState.timeout = timeout;
  xiState.colorproc = colorproc;
  xiState.compress = compress;
  xiState.context_valid = false;
```

DEFAULT BAYER APPEARS TO BE GBRG!!!!

      BayerPattern pattern = BayerPattern::GBRG; // default

### Video mode comparisons:

Need to make a table, but

1. You can do full 2048x700x7 raw bayer into binaries at apparnetly up to 170fps, likely 125 is good margin. Need to test reliability with hardware trigger, might need a RAM buffer for it to be solid. Find out! test binary: xi_raw_rolling.
2. 


### Speed tests:

Even the ancient M73 with 128gb 2.5inch SSD can stream 2048x700x8 bayer directly to SSD though there maybe some blips to take care of with buffers etc.

It averages 170FPS doing that as fast as it can - so 100Hz ok. 

```
spencelab@ros2test:~/ros2_ws/src/cambuffer_recorder_ng/src$ ./xi_raw_rolling
RAW8 rolling capture: 2048x700 exp=2000us, frames=10000, roll≈2 GiB, prefix=xi_raw
xiAPI: ---- xiOpenDevice API:V4.27.30.00 started ----
xiAPI: XIMEA Camera API V4.27.30.00
xiAPI: Adding camera context: dwID=28773051  ptr=1EE76000 processID=000020DB
xiAPI: Create handles 1 Process 000020DB
xiAPI: xiOpenDevice - legacy SN used for identification 28773051
xiAPI: Enable sensor
xiAPI: Calib data: Freq 0030 BL 3FCC ADC 002B bData 2B
xiAPI: OK retrains 0
xiAPI: xiReadFileFFS Time needed to read file SensFPNCorrections :207ms
xiAPI: xiReadFileFFS Time needed to read file SensFPNCorrections :218ms
xiAPI: Sensor SetExposure freq=48MHz exp=0us regexp=x1
xiAPI: Time needed to read BPL:9ms
xiAPI: Successfully parsed BPL file, 126 total corrected pixels
xiAPI: Sensor SetExposure freq=48MHz exp=0us regexp=x1
xiAPI: AutoSetBandwidth measurement
xiAPI: CalculateResources : Context 1EE76000 ID 28773051 m_maxBytes=1024 m_maxBufferSize=1048576
xiAPI: PoolAllocUSB30: zerocopy not available
xiAPI: Failed to change thread scheduler, check user limit for realtime priority.
xiAPI: AutoSetBandwidth measured 3658Mbps. Safe margin 10% will be used.
xiAPI: Current bandwidth limit auto-set to 3292 Mbps (min:80Mbps,max:3658Mbps)
xiAPI: Sensor SetExposure freq=48MHz exp=16us regexp=x1
xiAPI: Sensor SetExposure freq=48MHz exp=16us regexp=x1
xiAPI: ---- Device opened. Model:MQ022CG-CM SN:28773051 FwF1: API:V4.27.30.00 ----
xiAPI: Sensor SetExposure freq=48MHz exp=16us regexp=x1
xiAPI: Sensor SetExposure freq=48MHz exp=2000us regexp=x2E4
xiAPI: XIA(78b0):xiGetParam (padding_x) Finished with ERROR: 100
XI cfg: width=2048 height=700 exposure(us)=2000 padding_x=0 data_format=5 (RAW8=5)
[roll] opened xi_raw_0000.xraw (w=2048,h=700,stride=2048)
xiAPI: Sensor SetExposure freq=48MHz exp=2000us regexp=x2E4
xiAPI: CalculateResources : Context 1EE76000 ID 28773051 m_maxBytes=1024 m_maxBufferSize=1048576
xiAPI: PoolAllocUSB30: zerocopy not available
xiAPI: StartVideoStream
xiAPI: Failed to change thread scheduler, check user limit for realtime priority.
xiAPI: WorkerThread is up
==== streaming... (Ctrl+C to stop) ====
Frame 0 grab:3.42858ms pack+write:0.91259ms
Frame 100 grab:2.69094ms pack+write:0.971539ms
Frame 200 grab:2.63613ms pack+write:0.966021ms
Frame 300 grab:2.26612ms pack+write:1.49608ms
Frame 400 grab:2.70797ms pack+write:0.915827ms
Frame 500 grab:0.861665ms pack+write:1.17048ms
Frame 600 grab:2.76385ms pack+write:0.966437ms
Frame 700 grab:2.12517ms pack+write:0.95259ms
Frame 800 grab:2.70944ms pack+write:0.897714ms
Frame 900 grab:2.80658ms pack+write:1.54047ms
Frame 1000 grab:0.362827ms pack+write:0.950625ms
Frame 1100 grab:0.494142ms pack+write:2.00901ms
Frame 1200 grab:0.422966ms pack+write:1.89611ms
Frame 1300 grab:0.587949ms pack+write:3.27001ms
Frame 1400 grab:0.609315ms pack+write:2.07645ms
[roll] opened xi_raw_0001.xraw (w=2048,h=700,stride=2048)
Frame 1500 grab:1.79895ms pack+write:1.46962ms
Frame 1600 grab:0.353007ms pack+write:18.5773ms
Frame 1700 grab:0.360278ms pack+write:1.07775ms
Frame 1800 grab:2.73856ms pack+write:0.93485ms
Frame 1900 grab:0.465663ms pack+write:2.39935ms
Frame 2000 grab:0.401485ms pack+write:2.14918ms
Frame 2100 grab:0.263171ms pack+write:9.48099ms
Frame 2200 grab:0.365486ms pack+write:1.55926ms
Frame 2300 grab:0.507661ms pack+write:10.9592ms
Frame 2400 grab:0.368046ms pack+write:1.36122ms
Frame 2500 grab:0.506519ms pack+write:2.0642ms
Frame 2600 grab:0.289877ms pack+write:0.74363ms
Frame 2700 grab:0.361991ms pack+write:0.986744ms
Frame 2800 grab:0.662658ms pack+write:12.3624ms
Frame 2900 grab:0.600219ms pack+write:3.88086ms
[roll] opened xi_raw_0002.xraw (w=2048,h=700,stride=2048)
Frame 3000 grab:0.794857ms pack+write:2.0294ms
Frame 3100 grab:0.640176ms pack+write:1.88759ms
Frame 3200 grab:0.593667ms pack+write:1.7887ms
Frame 3300 grab:0.272441ms pack+write:9.90781ms
Frame 3400 grab:0.352758ms pack+write:0.941733ms
Frame 3500 grab:0.448697ms pack+write:11.2048ms
Frame 3600 grab:0.251913ms pack+write:0.854614ms
Frame 3700 grab:0.386878ms pack+write:13.3075ms
Frame 3800 grab:0.750511ms pack+write:3.40163ms
Frame 3900 grab:0.434076ms pack+write:1.31113ms
Frame 4000 grab:0.255277ms pack+write:14.6485ms
Frame 4100 grab:2.6753ms pack+write:0.906984ms
Frame 4200 grab:0.386775ms pack+write:11.7407ms
Frame 4300 grab:1.00357ms pack+write:1.83074ms
Frame 4400 grab:0.402018ms pack+write:1.69325ms
[roll] opened xi_raw_0003.xraw (w=2048,h=700,stride=2048)
Frame 4500 grab:0.343802ms pack+write:10.9964ms
Frame 4600 grab:0.352693ms pack+write:1.06067ms
Frame 4700 grab:0.329641ms pack+write:0.780338ms
Frame 4800 grab:0.504363ms pack+write:1.66939ms
Frame 4900 grab:0.276877ms pack+write:9.87581ms
Frame 5000 grab:0.656326ms pack+write:1.86011ms
Frame 5100 grab:0.524367ms pack+write:2.50198ms
Frame 5200 grab:0.438595ms pack+write:10.3527ms
Frame 5300 grab:0.559425ms pack+write:1.56831ms
Frame 5400 grab:0.712774ms pack+write:4.02292ms
Frame 5500 grab:0.246885ms pack+write:0.892798ms
Frame 5600 grab:0.568933ms pack+write:1.67656ms
Frame 5700 grab:0.351533ms pack+write:14.9552ms
Frame 5800 grab:0.479685ms pack+write:2.27622ms
Frame 5900 grab:0.431026ms pack+write:11.7957ms
[roll] opened xi_raw_0004.xraw (w=2048,h=700,stride=2048)
Frame 6000 grab:0.322928ms pack+write:14.6796ms
Frame 6100 grab:0.418218ms pack+write:9.52571ms
Frame 6200 grab:0.73784ms pack+write:11.8582ms
Frame 6300 grab:0.47069ms pack+write:1.8253ms
Frame 6400 grab:0.413379ms pack+write:0.833657ms
Frame 6500 grab:0.321113ms pack+write:1.44487ms
Frame 6600 grab:0.587878ms pack+write:0.960254ms
Frame 6700 grab:0.619217ms pack+write:19.5397ms
Frame 6800 grab:0.660753ms pack+write:3.26361ms
Frame 6900 grab:0.357784ms pack+write:12.2614ms
Frame 7000 grab:0.406381ms pack+write:1.7234ms
Frame 7100 grab:0.552802ms pack+write:11.157ms
Frame 7200 grab:0.466254ms pack+write:1.92271ms
Frame 7300 grab:0.254077ms pack+write:9.79064ms
Frame 7400 grab:0.453249ms pack+write:2.45528ms
[roll] opened xi_raw_0005.xraw (w=2048,h=700,stride=2048)
Frame 7500 grab:0.402872ms pack+write:13.5668ms
Frame 7600 grab:0.434934ms pack+write:0.90031ms
Frame 7700 grab:0.529322ms pack+write:1.51418ms
Frame 7800 grab:0.537898ms pack+write:10.1492ms
Frame 7900 grab:0.38545ms pack+write:10.1224ms
Frame 8000 grab:0.44666ms pack+write:2.51818ms
Frame 8100 grab:0.496614ms pack+write:48.0708ms
Frame 8200 grab:0.313858ms pack+write:0.823582ms
Frame 8300 grab:0.46213ms pack+write:5.08575ms
Frame 8400 grab:0.753575ms pack+write:13.3455ms
Frame 8500 grab:0.69281ms pack+write:1.55625ms
Frame 8600 grab:0.523742ms pack+write:14.0353ms
Frame 8700 grab:0.367497ms pack+write:0.908207ms
Frame 8800 grab:0.362006ms pack+write:0.965209ms
Frame 8900 grab:0.343654ms pack+write:0.910125ms
[roll] opened xi_raw_0006.xraw (w=2048,h=700,stride=2048)
Frame 9000 grab:0.546278ms pack+write:2.51606ms
Frame 9100 grab:0.563064ms pack+write:1.83207ms
Frame 9200 grab:1.05402ms pack+write:3.51188ms
Frame 9300 grab:1.1442ms pack+write:13.2623ms
Frame 9400 grab:0.619036ms pack+write:11.9058ms
Frame 9500 grab:0.349979ms pack+write:14.9761ms
Frame 9600 grab:0.675334ms pack+write:3.6407ms
Frame 9700 grab:0.284405ms pack+write:9.77276ms
Frame 9800 grab:0.514147ms pack+write:3.2483ms
Frame 9900 grab:0.307706ms pack+write:0.876767ms
xiAPI: WorkerThread is down: ret 0
xiAPI: xiCloseDevice
xiAPI: DisableSensor
xiAPI: DisableDevice
xiAPI: deleting camera context: dwID=28773051, ptr=1ee76000 processID=000020DB

Frames: 10000  Averages -> grab 0.677736 ms, write 5.25046 ms, total 5.92819 ms  => 168.686 FPS
done.
```

How long can we record?
```
Nice!! Here are solid, back-of-the-envelope record times at 100 fps for the common variants we’ve been juggling. I used realistic usable capacities: ~238 GiB for “256 GB” drives and ~931 GiB for “1 TB”.

Data rate & max record time (100 fps)

RAW8 Bayer, 2048×704 (1 B/px) → ~137.5 MiB/s
• 256 GB drive: ~0.49 h (~29 min)
• 1 TB drive: ~1.93 h

RAW8 Bayer, 2048×1088 (1 B/px) → ~212.5 MiB/s
• 256 GB: ~0.32 h (~19 min)
• 1 TB: ~1.25 h

CFA half (RAW8 preserved), 1024×352 (1 B/px) → ~34.4 MiB/s
• 256 GB: ~1.97 h
• 1 TB: ~7.70 h

RGB24 half (debayered), 1024×352 (3 B/px) → ~103.1 MiB/s
• 256 GB: ~0.66 h (~40 min)
• 1 TB: ~2.57 h

Quick formula you can reuse:

bytes/frame = width × height × bytes_per_pixel

bytes/sec = bytes/frame × fps

hours = (capacity_gib × 1024³) / bytes/sec / 3600

Practical notes

Write bandwidth: SATA SSDs (300–500 MB/s sustained) and NVMe (>>1 GB/s) both easily handle all the above streams, including full 2048×1088 RAW8 @ 100 fps (~212 MiB/s).

Headroom: leave ~10–20% free space for filesystem + wear leveling; the times above are “best case” continuous.

Rolling size: 2–4 GB per chunk is a sweet spot—fast to index, quick to salvage if a chunk corrupts, and keeps your metadata index tables small.

If you want, I can spit out the same table for 120 fps or 250 fps, or plug in your exact ROI and target fps.
```

notes:
```

Streaming Calcs:

Can we stream 100 fps 2048x704 to disk in 8bpp?

In [1]: 2048*700*100
Out[1]: 143360000


In [1]: 2048*700*100
Out[1]: 143360000

In [2]: 2048*700*100/(1024*1024)
Out[2]: 136.71875


MiB/S

136 Megabytes per second.


bytes per second.

Sata sequential write bandwidth: call it 400MB/s. (450-650MB/s).

So we migth get away with it.

How long could we record for?

At most you might want to do like 5 or 10 minutes of treadmill or reaching. Well reaching you might want more.


Treadmill:

10 minutes:
10*60*136=81600 MB.

Yeah I mean that's an 82GB file. Would take awhile to transcode, but you could do it...

Need to handle dropped frames.

Let's say you do 5 minutes, that'd be better.

Try it, see if the system can handle it.
Capturing 300 frames using pattern GBRG
xiAPI: ---- xiOpenDevice API:V4.27.30.00 started ----
xiAPI: XIMEA Camera API V4.27.30.00
xiAPI: Adding camera context: dwID=28773051  ptr=3E676000 processID=00001328
xiAPI: Create handles 1 Process 00001328
xiAPI: xiOpenDevice - legacy SN used for identification 28773051
xiAPI: Enable sensor
xiAPI: Calib data: Freq 0030 BL 3FCC ADC 002B bData 2B
xiAPI: OK retrains 0
xiAPI: xiReadFileFFS Time needed to read file SensFPNCorrections :205ms
xiAPI: xiReadFileFFS Time needed to read file SensFPNCorrections :227ms
xiAPI: Sensor SetExposure freq=48MHz exp=0us regexp=x1
xiAPI: Time needed to read BPL:9ms
xiAPI: Successfully parsed BPL file, 126 total corrected pixels
xiAPI: Sensor SetExposure freq=48MHz exp=0us regexp=x1
xiAPI: AutoSetBandwidth measurement
xiAPI: CalculateResources : Context 3E676000 ID 28773051 m_maxBytes=1024 m_maxBufferSize=1048576
xiAPI: PoolAllocUSB30: zerocopy not available
xiAPI: Failed to change thread scheduler, check user limit for realtime priority.
xiAPI: AutoSetBandwidth measured 3626Mbps. Safe margin 10% will be used.
xiAPI: Current bandwidth limit auto-set to 3263 Mbps (min:80Mbps,max:3626Mbps)
xiAPI: Sensor SetExposure freq=48MHz exp=16us regexp=x1
xiAPI: Sensor SetExposure freq=48MHz exp=16us regexp=x1
xiAPI: ---- Device opened. Model:MQ022CG-CM SN:28773051 FwF1: API:V4.27.30.00 ----
xiAPI: Sensor SetExposure freq=48MHz exp=16us regexp=x1
xiAPI: Sensor SetExposure freq=48MHz exp=2000us regexp=x2E4
xiAPI: XIA(0b40):xiGetParam (padding_x) Finished with ERROR: 100
XI config: width=2048 height=704 exposure(us)=2000 padding_x=0 data_format=5 (expect RAW8=5)
==== Camera ROI and Debayer Config ====
Sensor ROI: 2048 x 704
DebayerHalf Output: 1024 x 352 (1081344 bytes per RGB frame)
=======================================
xiAPI: Sensor SetExposure freq=48MHz exp=2000us regexp=x2E4
xiAPI: CalculateResources : Context 3E676000 ID 28773051 m_maxBytes=1024 m_maxBufferSize=1048576
xiAPI: PoolAllocUSB30: zerocopy not available
xiAPI: StartVideoStream
xiAPI: Failed to change thread scheduler, check user limit for realtime priority.
xiAPI: WorkerThread is up
xiAPI: XIA(0b40):xiGetParam (padding_x) Finished with ERROR: 100
ffmpeg version 4.4.2-0ubuntu0.22.04.1 Copyright (c) 2000-2021 the FFmpeg developers
  built with gcc 11 (Ubuntu 11.2.0-19ubuntu1)
  configuration: --prefix=/usr --extra-version=0ubuntu0.22.04.1 --toolchain=hardened --libdir=/usr/lib/x86_64-linux-gnu --incdir=/usr/include/x86_64-linux-gnu --arch=amd64 --enable-gpl --disable-stripping --enable-gnutls --enable-ladspa --enable-libaom --enable-libass --enable-libbluray --enable-libbs2b --enable-libcaca --enable-libcdio --enable-libcodec2 --enable-libdav1d --enable-libflite --enable-libfontconfig --enable-libfreetype --enable-libfribidi --enable-libgme --enable-libgsm --enable-libjack --enable-libmp3lame --enable-libmysofa --enable-libopenjpeg --enable-libopenmpt --enable-libopus --enable-libpulse --enable-librabbitmq --enable-librubberband --enable-libshine --enable-libsnappy --enable-libsoxr --enable-libspeex --enable-libsrt --enable-libssh --enable-libtheora --enable-libtwolame --enable-libvidstab --enable-libvorbis --enable-libvpx --enable-libwebp --enable-libx265 --enable-libxml2 --enable-libxvid --enable-libzimg --enable-libzmq --enable-libzvbi --enable-lv2 --enable-omx --enable-openal --enable-opencl --enable-opengl --enable-sdl2 --enable-pocketsphinx --enable-librsvg --enable-libmfx --enable-libdc1394 --enable-libdrm --enable-libiec61883 --enable-chromaprint --enable-frei0r --enable-libx264 --enable-shared
  libavutil      56. 70.100 / 56. 70.100
  libavcodec     58.134.100 / 58.134.100
  libavformat    58. 76.100 / 58. 76.100
  libavdevice    58. 13.100 / 58. 13.100
  libavfilter     7.110.100 /  7.110.100
  libswscale      5.  9.100 /  5.  9.100
  libswresample   3.  9.100 /  3.  9.100
  libpostproc    55.  9.100 / 55.  9.100
Frame 0 grab:5.76355ms debayer:2.69285ms enc:117.357ms
Input #0, rawvideo, from 'pipe:0':
  Duration: N/A, start: 0.000000, bitrate: 865075 kb/s
  Stream #0:0: Video: rawvideo (BGR[24] / 0x18524742), bgr24, 1024x352, 865075 kb/s, 100 tbr, 100 tbn, 100 tbc
Stream mapping:
  Stream #0:0 -> #0:0 (rawvideo (native) -> h264 (libx264))
[libx264 @ 0x5f9321c42340] using cpu capabilities: MMX2 SSE2Fast SSSE3 SSE4.2 AVX FMA3 BMI2 AVX2
[libx264 @ 0x5f9321c42340] profile Constrained Baseline, level 3.2, 4:2:0, 8-bit
[libx264 @ 0x5f9321c42340] 264 - core 163 r3060 5db6aa6 - H.264/MPEG-4 AVC codec - Copyleft 2003-2021 - http://www.videolan.org/x264.html - options: cabac=0 ref=1 deblock=0:0:0 analyse=0:0 me=dia subme=0 psy=1 psy_rd=1.00:0.00 mixed_ref=0 me_range=16 chroma_me=1 trellis=0 8x8dct=0 cqm=0 deadzone=21,11 fast_pskip=1 chroma_qp_offset=0 threads=6 lookahead_threads=1 sliced_threads=0 nr=0 decimate=1 interlaced=0 bluray_compat=0 constrained_intra=0 bframes=0 weightp=0 keyint=250 keyint_min=25 scenecut=0 intra_refresh=0 rc=crf mbtree=0 crf=18.0 qcomp=0.60 qpmin=0 qpmax=69 qpstep=4 ip_ratio=1.40 aq=0
Output #0, mp4, to 'xi_stream_test.mp4':
  Metadata:
    encoder         : Lavf58.76.100
  Stream #0:0: Video: h264 (avc1 / 0x31637661), yuv420p(tv, progressive), 1024x352, q=2-31, 100 fps, 12800 tbn
    Metadata:
      encoder         : Lavc58.134.100 libx264
    Side data:
      cpb: bitrate max/min/avg: 0/0/0 buffer size: 0 vbv_delay: N/A
Frame 30 grab:0.384419ms debayer:2.8152ms enc:0.58171ms.00 bitrate=N/A speed=   0x    
Frame 60 grab:0.510787ms debayer:3.80202ms enc:0.485159ms
Frame 90 grab:0.392112ms debayer:2.85559ms enc:0.660239ms
Frame 120 grab:0.614718ms debayer:2.55987ms enc:0.431143ms9 bitrate=16945.7kbits/s speed=1.98x    
Frame 150 grab:0.418454ms debayer:2.90218ms enc:0.701742ms
Frame 180 grab:0.395861ms debayer:2.80464ms enc:0.666145ms
Frame 210 grab:0.400262ms debayer:2.9442ms enc:0.409413ms
Frame 240 grab:0.659385ms debayer:4.18805ms enc:0.790951ms0 bitrate=16976.5kbits/s speed=2.09x    
Frame 270 grab:0.474756ms debayer:2.90569ms enc:0.471305ms
xiAPI: WorkerThread is down: ret 0
xiAPI: xiCloseDevice
xiAPI: DisableSensor
xiAPI: DisableDevice
xiAPI: deleting camera context: dwID=28773051, ptr=3e676000 processID=00001328
frame=  300 fps=197 q=-1.0 Lsize=    6314kB time=00:00:02.99 bitrate=17297.7kbits/s speed=1.97x    
video:6312kB audio:0kB subtitle:0kB other streams:0kB global headers:0kB muxing overhead: 0.033065%
[libx264 @ 0x5f9321c42340] frame I:2     Avg QP:17.00  size:118556
[libx264 @ 0x5f9321c42340] frame P:298   Avg QP:22.37  size: 20890
[libx264 @ 0x5f9321c42340] mb I  I16..4: 100.0%  0.0%  0.0%
[libx264 @ 0x5f9321c42340] mb P  I16..4:  0.1%  0.0%  0.0%  P16..4: 48.2%  0.0%  0.0%  0.0%  0.0%    skip:51.8%
[libx264 @ 0x5f9321c42340] coded y,uvDC,uvAC intra: 81.1% 61.2% 55.5% inter: 40.0% 29.8% 23.7%
[libx264 @ 0x5f9321c42340] i16 v,h,dc,p: 38% 22% 26% 13%
[libx264 @ 0x5f9321c42340] i8c dc,h,v,p: 57% 13% 24%  6%
[libx264 @ 0x5f9321c42340] kb/s:17233.16

Averages: grab 0.582733ms, debayer 3.33605ms, encode 1.18146ms, total 5.10024ms → 196.069 FPS
Write the frame num on it. How many frames is that?

60,000 frames, not so bad.


```

# Speed Test Outputs:

### xi_grab_debayer_ffmpeg_stream
```
Capturing 300 frames using pattern GBRG
xiAPI: ---- xiOpenDevice API:V4.27.30.00 started ----
xiAPI: XIMEA Camera API V4.27.30.00
xiAPI: Adding camera context: dwID=28773051  ptr=3E676000 processID=00001328
xiAPI: Create handles 1 Process 00001328
xiAPI: xiOpenDevice - legacy SN used for identification 28773051
xiAPI: Enable sensor
xiAPI: Calib data: Freq 0030 BL 3FCC ADC 002B bData 2B
xiAPI: OK retrains 0
xiAPI: xiReadFileFFS Time needed to read file SensFPNCorrections :205ms
xiAPI: xiReadFileFFS Time needed to read file SensFPNCorrections :227ms
xiAPI: Sensor SetExposure freq=48MHz exp=0us regexp=x1
xiAPI: Time needed to read BPL:9ms
xiAPI: Successfully parsed BPL file, 126 total corrected pixels
xiAPI: Sensor SetExposure freq=48MHz exp=0us regexp=x1
xiAPI: AutoSetBandwidth measurement
xiAPI: CalculateResources : Context 3E676000 ID 28773051 m_maxBytes=1024 m_maxBufferSize=1048576
xiAPI: PoolAllocUSB30: zerocopy not available
xiAPI: Failed to change thread scheduler, check user limit for realtime priority.
xiAPI: AutoSetBandwidth measured 3626Mbps. Safe margin 10% will be used.
xiAPI: Current bandwidth limit auto-set to 3263 Mbps (min:80Mbps,max:3626Mbps)
xiAPI: Sensor SetExposure freq=48MHz exp=16us regexp=x1
xiAPI: Sensor SetExposure freq=48MHz exp=16us regexp=x1
xiAPI: ---- Device opened. Model:MQ022CG-CM SN:28773051 FwF1: API:V4.27.30.00 ----
xiAPI: Sensor SetExposure freq=48MHz exp=16us regexp=x1
xiAPI: Sensor SetExposure freq=48MHz exp=2000us regexp=x2E4
xiAPI: XIA(0b40):xiGetParam (padding_x) Finished with ERROR: 100
XI config: width=2048 height=704 exposure(us)=2000 padding_x=0 data_format=5 (expect RAW8=5)
==== Camera ROI and Debayer Config ====
Sensor ROI: 2048 x 704
DebayerHalf Output: 1024 x 352 (1081344 bytes per RGB frame)
=======================================
xiAPI: Sensor SetExposure freq=48MHz exp=2000us regexp=x2E4
xiAPI: CalculateResources : Context 3E676000 ID 28773051 m_maxBytes=1024 m_maxBufferSize=1048576
xiAPI: PoolAllocUSB30: zerocopy not available
xiAPI: StartVideoStream
xiAPI: Failed to change thread scheduler, check user limit for realtime priority.
xiAPI: WorkerThread is up
xiAPI: XIA(0b40):xiGetParam (padding_x) Finished with ERROR: 100
ffmpeg version 4.4.2-0ubuntu0.22.04.1 Copyright (c) 2000-2021 the FFmpeg developers
  built with gcc 11 (Ubuntu 11.2.0-19ubuntu1)
  configuration: --prefix=/usr --extra-version=0ubuntu0.22.04.1 --toolchain=hardened --libdir=/usr/lib/x86_64-linux-gnu --incdir=/usr/include/x86_64-linux-gnu --arch=amd64 --enable-gpl --disable-stripping --enable-gnutls --enable-ladspa --enable-libaom --enable-libass --enable-libbluray --enable-libbs2b --enable-libcaca --enable-libcdio --enable-libcodec2 --enable-libdav1d --enable-libflite --enable-libfontconfig --enable-libfreetype --enable-libfribidi --enable-libgme --enable-libgsm --enable-libjack --enable-libmp3lame --enable-libmysofa --enable-libopenjpeg --enable-libopenmpt --enable-libopus --enable-libpulse --enable-librabbitmq --enable-librubberband --enable-libshine --enable-libsnappy --enable-libsoxr --enable-libspeex --enable-libsrt --enable-libssh --enable-libtheora --enable-libtwolame --enable-libvidstab --enable-libvorbis --enable-libvpx --enable-libwebp --enable-libx265 --enable-libxml2 --enable-libxvid --enable-libzimg --enable-libzmq --enable-libzvbi --enable-lv2 --enable-omx --enable-openal --enable-opencl --enable-opengl --enable-sdl2 --enable-pocketsphinx --enable-librsvg --enable-libmfx --enable-libdc1394 --enable-libdrm --enable-libiec61883 --enable-chromaprint --enable-frei0r --enable-libx264 --enable-shared
  libavutil      56. 70.100 / 56. 70.100
  libavcodec     58.134.100 / 58.134.100
  libavformat    58. 76.100 / 58. 76.100
  libavdevice    58. 13.100 / 58. 13.100
  libavfilter     7.110.100 /  7.110.100
  libswscale      5.  9.100 /  5.  9.100
  libswresample   3.  9.100 /  3.  9.100
  libpostproc    55.  9.100 / 55.  9.100
Frame 0 grab:5.76355ms debayer:2.69285ms enc:117.357ms
Input #0, rawvideo, from 'pipe:0':
  Duration: N/A, start: 0.000000, bitrate: 865075 kb/s
  Stream #0:0: Video: rawvideo (BGR[24] / 0x18524742), bgr24, 1024x352, 865075 kb/s, 100 tbr, 100 tbn, 100 tbc
Stream mapping:
  Stream #0:0 -> #0:0 (rawvideo (native) -> h264 (libx264))
[libx264 @ 0x5f9321c42340] using cpu capabilities: MMX2 SSE2Fast SSSE3 SSE4.2 AVX FMA3 BMI2 AVX2
[libx264 @ 0x5f9321c42340] profile Constrained Baseline, level 3.2, 4:2:0, 8-bit
[libx264 @ 0x5f9321c42340] 264 - core 163 r3060 5db6aa6 - H.264/MPEG-4 AVC codec - Copyleft 2003-2021 - http://www.videolan.org/x264.html - options: cabac=0 ref=1 deblock=0:0:0 analyse=0:0 me=dia subme=0 psy=1 psy_rd=1.00:0.00 mixed_ref=0 me_range=16 chroma_me=1 trellis=0 8x8dct=0 cqm=0 deadzone=21,11 fast_pskip=1 chroma_qp_offset=0 threads=6 lookahead_threads=1 sliced_threads=0 nr=0 decimate=1 interlaced=0 bluray_compat=0 constrained_intra=0 bframes=0 weightp=0 keyint=250 keyint_min=25 scenecut=0 intra_refresh=0 rc=crf mbtree=0 crf=18.0 qcomp=0.60 qpmin=0 qpmax=69 qpstep=4 ip_ratio=1.40 aq=0
Output #0, mp4, to 'xi_stream_test.mp4':
  Metadata:
    encoder         : Lavf58.76.100
  Stream #0:0: Video: h264 (avc1 / 0x31637661), yuv420p(tv, progressive), 1024x352, q=2-31, 100 fps, 12800 tbn
    Metadata:
      encoder         : Lavc58.134.100 libx264
    Side data:
      cpb: bitrate max/min/avg: 0/0/0 buffer size: 0 vbv_delay: N/A
Frame 30 grab:0.384419ms debayer:2.8152ms enc:0.58171ms.00 bitrate=N/A speed=   0x    
Frame 60 grab:0.510787ms debayer:3.80202ms enc:0.485159ms
Frame 90 grab:0.392112ms debayer:2.85559ms enc:0.660239ms
Frame 120 grab:0.614718ms debayer:2.55987ms enc:0.431143ms9 bitrate=16945.7kbits/s speed=1.98x    
Frame 150 grab:0.418454ms debayer:2.90218ms enc:0.701742ms
Frame 180 grab:0.395861ms debayer:2.80464ms enc:0.666145ms
Frame 210 grab:0.400262ms debayer:2.9442ms enc:0.409413ms
Frame 240 grab:0.659385ms debayer:4.18805ms enc:0.790951ms0 bitrate=16976.5kbits/s speed=2.09x    
Frame 270 grab:0.474756ms debayer:2.90569ms enc:0.471305ms
xiAPI: WorkerThread is down: ret 0
xiAPI: xiCloseDevice
xiAPI: DisableSensor
xiAPI: DisableDevice
xiAPI: deleting camera context: dwID=28773051, ptr=3e676000 processID=00001328
frame=  300 fps=197 q=-1.0 Lsize=    6314kB time=00:00:02.99 bitrate=17297.7kbits/s speed=1.97x    
video:6312kB audio:0kB subtitle:0kB other streams:0kB global headers:0kB muxing overhead: 0.033065%
[libx264 @ 0x5f9321c42340] frame I:2     Avg QP:17.00  size:118556
[libx264 @ 0x5f9321c42340] frame P:298   Avg QP:22.37  size: 20890
[libx264 @ 0x5f9321c42340] mb I  I16..4: 100.0%  0.0%  0.0%
[libx264 @ 0x5f9321c42340] mb P  I16..4:  0.1%  0.0%  0.0%  P16..4: 48.2%  0.0%  0.0%  0.0%  0.0%    skip:51.8%
[libx264 @ 0x5f9321c42340] coded y,uvDC,uvAC intra: 81.1% 61.2% 55.5% inter: 40.0% 29.8% 23.7%
[libx264 @ 0x5f9321c42340] i16 v,h,dc,p: 38% 22% 26% 13%
[libx264 @ 0x5f9321c42340] i8c dc,h,v,p: 57% 13% 24%  6%
[libx264 @ 0x5f9321c42340] kb/s:17233.16

Averages: grab 0.582733ms, debayer 3.33605ms, encode 1.18146ms, total 5.10024ms → 196.069 FPS
```
