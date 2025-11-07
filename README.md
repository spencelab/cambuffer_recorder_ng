# cambuffer_recorder_ng

Note broader develop notes here:
https://github.com/aspence/spencelab/wiki/Ubuntu-22-Jammy-ROS-2-Humble-Testing-and-Mac-Multipass-Development

## Running and testing:

```
open terminal
ros2 launch cambuffer_recorder_ng fakecam.launch.py backend:=xiapi
open another terminal
ros2 lifecycle set /fakecam_node configure
ros2 lifecycle set /fakecam_node activate
ros2 lifecycle set /fakecam_node deacctivate
ros2 lifecycle set /fakecam_node shutdown
can ctrl-c the ros2 launch.
makes mpg.
```

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
