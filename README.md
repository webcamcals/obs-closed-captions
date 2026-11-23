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
- **Caption input**: *Raw VBI* (the plugin decodes line 21 itself, like a TV; recommended) or
  *CC pin* (the capture chip's own decoder, which can miss letters on VHS).
- **Caption channel**: CC1 (almost always) or CC2.
- **Write captions to text file**: point an OBS Text source's "Read from file" at this file.
- **Text file contents**: *Current caption* (exactly what a TV would show right now; empty when
  nothing is shown) or *Transcript* (each finished caption line is appended; pairs well with the
  Text source's Chatlog Mode).
- **Also update this text source directly**: instant updates (a Text source reading a file only
  checks it about once a second).
- **Clear captions after…**: clears the text if no caption data arrives for this long.
- **Log raw caption data**: troubleshooting; writes every caption byte pair to the OBS log.

## Credits

Based on OBS Studio's `win-dshow` plugin and `libdshowcapture` (Lain Bailey and OBS
contributors). Licensed under the GPL v2 or later.
