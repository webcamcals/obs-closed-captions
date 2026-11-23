# Closed Caption Capture for OBS

An OBS Studio source for Windows that works like the built-in **Video Capture Device**,
but also reads analog **closed captions** (EIA-608 / "line 21") from capture devices that
expose a DirectShow `CC` pin, such as the Hauppauge USB-Live2.

It writes the caption text that is *currently on screen* to a text file (and can optionally
update an OBS Text source directly). When no caption is showing, the file is emptied.

## Install

1. Download the latest build from the repository's **Actions** tab (newest run → *Artifacts*).
2. Unzip it into `C:\ProgramData\obs-studio\plugins\` so you end up with
   `C:\ProgramData\obs-studio\plugins\obs-closed-captions\obs-closed-captions.dll`
   (the OBS 33+ plugin layout).
3. Restart OBS and add the source **Video Capture Device (with Closed Captions)**.

Requires **OBS Studio 33 or newer** (built against OBS 33.0.3).

## Settings (in the source's properties, under "Closed Captions")

- **Capture closed captions**: on/off.
- **Caption channel**: CC1 (almost always) or CC2.
- **Write captions to text file**: point an OBS Text source's "Read from file" at this file.
- **Also update this text source directly**: optional, instant updates without the file.
- **Clear captions after…**: clears the text if no caption data arrives for this long.
- **Log raw caption data**: troubleshooting; writes every caption byte pair to the OBS log.

## Credits

Based on OBS Studio's `win-dshow` plugin and `libdshowcapture` (Lain Bailey and OBS
contributors). Licensed under the GPL v2 or later.
