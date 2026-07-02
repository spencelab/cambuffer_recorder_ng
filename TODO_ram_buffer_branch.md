# RAM circular buffer branch TODO

Status: WIP, not ready to merge.

What works so far:
- Fakecam RAM circular buffer dumps work at 250 Hz with 320x240 test config.
- XIMEA hardware-triggered RAM dumps produce correct frame counts, frame indices, camera timestamps, and camera frame numbers.
- post_trigger, mid_trigger, and pre_trigger modes have been tested.
- Anchor timestamp logic added for multi-PC alignment.
- pause_acquisition dump policy works and resumes acquisition.
- CBRRAW v2 with optional payload CRC added.
- Rolling mode should default CRC off for bandwidth.

Known issues / TODO:
1. New XIMEA RAM dumps transcode to tiny black MP4s.
   - Suspect XiCamera::grabPackedInto() direct .bp path is not actually filling the destination buffer.
   - Next step: disable direct .bp by default or make it opt-in, then retest raw_rolling_to_mp4.
   - Add a quick raw payload sanity check tool or dump first frame PNG/PGM.

2. Benchmark CRC overhead.
   - Confirm raw.payload_crc32_enabled param plumbing.
   - Compare RAM dump wall time with CRC on/off.

3. Reconfirm rolling mode still works.
   - Test existing raw rolling config.
   - Confirm payload_crc32=off for rolling unless explicitly enabled.
   - Audit and transcode a rolling .cbrraw.

4. Multi-PC behavior still needs test.
   - Use shared trigger_utc_ns from camera_control.
   - Confirm cameras select matching frame windows or metadata allows clean alignment.
