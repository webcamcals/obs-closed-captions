# Closed Caption Capture for OBS

<img width="2470" height="1167" alt="image" src="https://github.com/user-attachments/assets/87efacc7-4d3f-4397-b8ac-234cb8e78819" />

This plugin for OBS Studio decodes closed captions in a digitized analog video signal, and lets you display them in an OBS Text Source and/or write the closed captions to a txt file.

It works by adding a new source, **Video Capture Device (with Closed Captions)**, which works like the built-in **Video Capture Device** source, but also reads analog **closed captions** (EIA-608 / "line 21") from capture devices that expose a DirectShow `CC` pin (tested with the [Hauppauge USB-Live2](https://hauppauge.store/product/610/). Not a sponsor, 'Tubes.)\*.

There are two things that can be done with the closed captions:
- Text Source: A text source of your choice is updated directly with captions that would be currently showing on a TV; when there are no captions on-screen, it's empty.
- Write to text file: You write the captions to a text file of your choice in one of two ways: by overwriting the file with only the currently-showing captions (if any), or by appending each new line of captions to the bottom of a file. Because OBS only checks text files for updates once every second or so, the Text Source direct update method above is preferred for showing closed captions in real-time in your layout.

\* Note that if you are running your analog signal through something like an RCA->HDMI converter and feeding that into a capture card, you probably aren't getting the closed caption signal. Sorry. You probably need something that exists specifically to digitize an analog video signal.

Requires OBS 33+

## Install

1. Download the latest build
2. Unzip it into `C:\ProgramData\obs-studio\plugins\` so you end up with `C:\ProgramData\obs-studio\plugins\obs-closed-captions\obs-closed-captions.dll`.
3. Restart OBS
4. Add a **Text source** for displaying closed captions
5. Add the source **Video Capture Device (with Closed Captions)**

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

## Closed Captions?
I recommend [Technology Connections' video explaining how Closed Captions work](https://www.youtube.com/watch?v=6SL6zs2bDks). You can expect to find closed captions in professionally-released movies, and amateur recordings of broadcast TV. Some DVDs have closed captions in addition to digital subtitles, but not all. Laser Discs probably have them, too, if you swing that way.

## AI Disclosure

Claude wrote all the code... I don't know anything about that stuff. But a human wrote all of the user-facing text, so at least there's that.

## Credits

Based on OBS Studio's `win-dshow` plugin and `libdshowcapture` (Lain Bailey and OBS contributors). Licensed under the GPL v2 or later.
