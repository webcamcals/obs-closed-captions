# Closed Caption Capture for OBS

<img width="1280" height="640" alt="image" src="https://github.com/user-attachments/assets/a9f06b4b-3795-422e-b5a7-b7a735927c6d" />

This plugin for OBS Studio decodes closed captions in a digitized analog video signal, and lets you display them in an OBS Text Source and/or write the closed captions to a txt file.

<img width="2470" height="1167" alt="image" src="https://github.com/user-attachments/assets/87efacc7-4d3f-4397-b8ac-234cb8e78819" />


It works by adding a new source, **Video Capture Device (with Closed Captions)**, which works like the built-in **Video Capture Device** source, but also reads analog **closed captions** (EIA-608 / "line 21") from capture devices that expose a DirectShow `CC` pin\* (tested with the [Hauppauge USB-Live2](https://hauppauge.store/product/610/). Not a sponsor, youtube).

There are two things that can be done with the closed captions:
- **Text Source**: A text source of your choice is updated directly with captions that would be currently showing on a TV; when there are no captions on-screen, it's empty.
- **Write to text file**: You write the captions to a text file of your choice in one of two ways: by overwriting the file with only the currently-showing captions (if any), or by appending each new line of captions to the bottom of a file. Because OBS only checks text files for updates once every second or so, the Text Source direct update method above is preferred for showing closed captions in real-time in your layout.

\* Note that if you are running your analog signal through something like an RCA->HDMI converter and feeding that into a capture card, you probably aren't getting the closed caption signal. Sorry. You probably need something that exists specifically to digitize an analog video signal.

## Install

1. Download zip, close OBS
2. Extract and copy the `obs-closed-captions` folder into `C:\ProgramData\obs-studio\plugins\`, so you end up with `C:\ProgramData\obs-studio\plugins\obs-closed-captions\obs-closed-captions.dll`.
3. Start OBS
4. Add a **Text source** for displaying closed captions
5. Add the source **Video Capture Device (with Closed Captions)**, and point it at your text source for caption display
6. Set up your **Text source** however you want it. I recommend enabling `Use Custom Text Extents` in the properties to format and align the text where you want it in a rectangle of your preferred size.

Requires **OBS Studio 33+**.

## Settings (in the source's properties, under "Closed Captions")
<img width="448" height="361" alt="image" src="https://github.com/user-attachments/assets/0d9e7ca3-2925-4ffb-a458-533c6cf62e4b" />

In addition to the standard Video Source options, the **Video Capture Device (with Closed Captions)** source has a Closed Captions settings pane, with the following options:
- **Capture closed captions**: on/off.
- **Caption input**: *Raw VBI* decodes captions from the analog video signal the same way a CRT does; this is recommended for accuracy. *CC pin* relies on the closed caption signal provided by your capture device's driver, which omitted a lot of characters in my testing.
- **Caption channel**: CC1 (almost always) or CC2.
- **Text source for closed captions**: Select a text source for the plugin to use so it can update changes in captions instantly. This is preferred for displaying captions in real-time.
- **Clear captions after…**: clears the text if no caption data arrives for this long.
- **Write captions to text file**: on/off.
- **File**: text file to write captions in.
- **Text file contents**: *Overwrite* or *Append*. Overwrite replaces the entire document with only the text that is currently being shown on-screen, and empties it when there is none. Append writes each new line to the bottom row of the text file, which can be useful for archival purposes. If you are displaying a text file in OBS in Append mode, I recommend enabling ‘Chatlog Mode’ in the Text Source Properties window to set how many of the most recent lines to show.
- **Troubleshooting**: keeps more detailed logs for troubleshooting purposes.


<img width="2545" height="1204" alt="image" src="https://github.com/user-attachments/assets/f9393e97-aaf7-43ef-8393-187d0766e66d" />

## About Closed Captions
I recommend [Technology Connections' video explaining how Closed Captions work](https://www.youtube.com/watch?v=6SL6zs2bDks). You can expect to find closed captions in professionally-released movies, and amateur recordings of broadcast TV. Some DVDs have closed captions in addition to digital subtitles, but not all. Laser Discs probably have them, too, if you swing that way.

## Scene Collection Converter (Optional)

<img width="713" height="635" alt="image" src="https://github.com/user-attachments/assets/4146b9a8-cc9f-4b2d-873b-a1f798ad3aad" />

In addition to the `.zip` file containing the plugin, there is also a file called `convert-scene-collection.html`.

This plugin had to be designed as a separate kind of source that accesses the audio/video capture device while *also* reading the closed captions, because devices can't be shared between two sources. Alas, OBS does not let you ‘swap’ a source of one type for a source of another while keeping everything intact. I personally already had a very complicated setup for analog video source in OBS: many many filters for video and audio, some of which were controlled by my StreamDeck device and Streamer.bot Actions, associated with Twitch Viewer Point Rewards, etc. So I've provided this tool for people like me, for whom it would be a massive pain to remake the source from nothing and update all of the references.

If that applies to you, you can download this HTML file and open it in your browser. You can point it to your OBS scene collection `.json` file, and it will make it easy to swap sources of your choice to (and from) the new type of Video Capture Device source with Closed Captions, preserving everything else you had set up with that source, and all references to it. If you don't have a complicated setup for your video capture device source, don't worry about it.

## AI Disclosure

Claude wrote all the code... I don't know anything about that stuff. But a human wrote all of the user-facing text, so at least there's that.

## Credits

Based on OBS Studio's `win-dshow` plugin and `libdshowcapture` (Lain Bailey and OBS contributors). Licensed under the GPL v2 or later. Tested on [Popcorn Classics](https://www.oncinematimeline.com/popcorn-classics); list provided by Gregg Turkington at the Victorville Film Archive.

<img width="790" height="732" alt="image" src="https://github.com/user-attachments/assets/d0dfcc4c-0120-45e9-98e4-965284a89ca6" />

