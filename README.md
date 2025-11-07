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
