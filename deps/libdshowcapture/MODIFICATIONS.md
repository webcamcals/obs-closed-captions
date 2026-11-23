# Modifications to libdshowcapture

This is a copy of [libdshowcapture](https://github.com/obsproject/libdshowcapture)
(LGPL 2.1 or later) at commit `8878638324393815512f802640b0d5ce940161f1`, the
version used by OBS Studio 33.0.3, including its `capture-device-support`
submodule. `CMakeLists.txt` in this folder is OBS Studio's wrapper for it.

It has been modified to capture closed caption data alongside video and audio:

- `dshowcapture.hpp`, `source/dshowcapture.cpp`: added `CaptionProc` /
  `Device::SetCaptionCallback` (the device's "CC" pin, EIA-608 byte pairs) and
  `VbiProc` / `Device::SetVbiCallback` (the device's raw "VBI" pin).
- `source/device.hpp`, `source/device.cpp`: connect the VBI pin (or, failing
  that, the CC pin) to capture filters after video and audio are connected;
  request extra buffers on those pins; if the graph fails to run with them
  connected, retry without them so video and audio still work.
- `source/capture-filter.hpp`, `source/capture-filter.cpp`: `PinCaptureInfo`
  can request a minimum number of buffers from the upstream pin.

The `tests` and `vs` folders of the original repository are not included.
