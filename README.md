# AV Sync and Latency Dock probe plugin for OBS Studio

## Introduction

This is an OBS Studio plugin to measure the audio offset and video latency. 

It's based on solid grounds of Norihiro's [Audio Video Sync Dock](https://obsproject.com/forum/resources/audio-video-sync-dock.2028/). 

Programming work done by Codex.

## Motivation behind the fork

The v2 AV offset workflow for repeatable relative AV sync checks
through real displays, speakers, cameras, microphones, OBS media and browser sources, and
recordings. The most important improvements over the original design are:
- lower-flash video markers,
- extended QR Code vocabulary allowing glass-to-glass latency  measurements,
- new visual markers for naked-eye AV offset assessment (web generator-only),
- cleaner event timing,
- sample-centered chirp/tick audio detection designed for echo and reverberation tolerance,
- DTMF/CRC event identity,
- MOV reference assets that avoid AAC priming and B-frame reorder bias.

V2 differences and improvements: the original legacy pattern relies on
high-contrast full-frame quadrant flashes and a tone-coded audio marker whose
timing is tied to frame-sized pulses. The flashes can be uncomfortable or
unsuitable for photosensitive viewers, while display cadence, camera exposure,
encoder reordering, lossy audio priming, or a missed marker can bias the result.
Version 2 separates timing from identity: sparse checkerboard transitions define
the video instant, a matched-filter chirp/tick defines the audio fiducial, and
the later DTMF/CRC payload only pairs the correct events. It is less visually
intrusive and easier to capture, while still measuring relative AV offset rather
than true source-to-capture latency; use the web generator workflow when you
need glass-to-glass latency.

## How to use

Use the MOV clip workflow when you only need relative audio/video offset
inside a repeatable file-based test. 
When you need the full display-to-camera-to-OBS path latency use the web generator workflow.

### AV offset clip workflow

Use this workflow when you want to measure the relative offset between audio and
video after playing a test clip through an external device, capturing it back
with a camera and microphone.

1. Generate a low-flash AV offset clip:

   ```sh
   (cd tool && ./avoffsetgen.py --vr 30 --ar 48000 -o /tmp/av-offset-pattern.mp4)
   ```

   Or use one of the generated MOV files:

   | MOV file | Video frame rate | Supported display frame rate |
   | -------- | ----------------:| ---------------------------- |
   | [av-offset-pattern-6000.mov](https://matiaspl.github.io/obs-audio-video-sync-dock/av-offset-pattern-6000.mov) | 60 FPS | 30 FPS, 60 FPS, or 120 FPS (iPhone) |
   | [av-offset-pattern-5994.mov](https://matiaspl.github.io/obs-audio-video-sync-dock/av-offset-pattern-5994.mov) | 59.94 FPS | 29.97 FPS, 59.94 FPS, or 119.88 FPS |
   | [av-offset-pattern-5000.mov](https://matiaspl.github.io/obs-audio-video-sync-dock/av-offset-pattern-5000.mov) | 50 FPS | 25 FPS or 50 FPS (PAL) |
   | [av-offset-pattern-3000.mov](https://matiaspl.github.io/obs-audio-video-sync-dock/av-offset-pattern-3000.mov) | 30 FPS | 30 FPS or 60 FPS |
   | [av-offset-pattern-2997.mov](https://matiaspl.github.io/obs-audio-video-sync-dock/av-offset-pattern-2997.mov) | 29.97 FPS | 29.97 FPS or 59.94 FPS |
   | [av-offset-pattern-2400.mov](https://matiaspl.github.io/obs-audio-video-sync-dock/av-offset-pattern-2400.mov) | 24 FPS | 24 FPS or 48 FPS |
   | [av-offset-pattern-2398.mov](https://matiaspl.github.io/obs-audio-video-sync-dock/av-offset-pattern-2398.mov) | 23.98 FPS | 23.98 FPS (24 FPS NTSC) |

   The generated MOV files are the preferred reference assets. They use PCM s16
   mono audio and intra-only H.264 video without B-frames, so the timing markers
   are not shifted by AAC encoder priming or video frame reordering. MP4 files
   with AAC audio are easier to play in some environments, but the lossy audio
   path can add a fixed priming bias and normal H.264 GOPs can add reorder
   latency; use those only when you intentionally want to measure that playback
   or codec behavior.

2. Play the generated clip on the device under test.
3. Capture the device with the camera and microphone that OBS will use.
4. Open the Audio Video Sync dock, select `AV Offset Clip`, and start measuring.

To verify the reference file itself without OBS media-source scheduling in the
path, run:

```sh
(cd tool && ./verify_avoffset_file.py ../release/av-offset-pattern-media-source.mov)
```

When validating OBS behavior, compare the dock result against an actual OBS
recording made with the same source sync offsets, encoder, frame rate, and audio
buffering settings.

### Web generator workflow

For live source-to-capture latency tests, open the web-based NTP Cue Generator:

<[https://broadcast-ready.com/obs-audio-video-sync-dock/](https://matiaspl.github.io/obs-audio-video-sync-dock/)>

The web generator renders the same v2 visual marker sequence in the browser and
schedules matching acoustic packets against a shared wall-clock target time. Its
QR payload includes the target UTC timestamp, so apart from the the AV offset the dock 
can compare that source time with the capture timestamp and report `Glass-to-glass` latency. 
This is especially useful for measuring the latency of a capture-encode-decode chain.

### Legacy workflow

1. Play the video.
   Choose the appropriate video file for your player and OBS Studio configuration.

   | Video file | Video frame rate | Supported display frame rate |
   | ---------- | ----------------:| --------------------- |
   | [sync-pattern-6000.mp4](https://matiaspl.github.io/obs-audio-video-sync-dock/sync-pattern-6000.mp4) | 60 FPS    | 30 FPS, 60 FPS, or 120 FPS (iPhone) |
   | [sync-pattern-5994.mp4](https://matiaspl.github.io/obs-audio-video-sync-dock/sync-pattern-5994.mp4) | 59.94 FPS | 29.97 FPS, 59.94 FPS, or 119.88 FPS |
   | [sync-pattern-5000.mp4](https://matiaspl.github.io/obs-audio-video-sync-dock/sync-pattern-5000.mp4) | 50 FPS    | 25 FPS or 50 FPS (PAL) |
   | [sync-pattern-2400.mp4](https://matiaspl.github.io/obs-audio-video-sync-dock/sync-pattern-2400.mp4) | 24 FPS    | 24 FPS or 48 FPS |
   | [sync-pattern-2398.mp4](https://matiaspl.github.io/obs-audio-video-sync-dock/sync-pattern-2398.mp4) | 23.98 FPS | 23.98 FPS (24 FPS NTSC) |

   - Choose the video frame rate that is same as player's frame rate or twice of that. For example, if your player (or display) is 60 FPS or 30 FPS such as iPhone, choose 60 FPS. If your player is 59.94 FPS or 29.97 FPS, choose 59.94 FPS.
   - If there are multiple candidates, try to choose the same frame rate as OBS Studio or twice of that.

2. Use your camera to shoot the display playing the video so that the pattern appears on the program of OBS Studio.
3. Open the Audio Video Sync dock and start measuring using the legacy probe.
4. Check the latency and adjust it accordingly:
   - Positive latency indicates audio is lagged, video is early.
   - Negative latency indicates audio is early, video is lagged.
   - To adjust the audio latency, increase or decrease the Sync Offset in the Advanced Audio Properties dialog in OBS Studio.
   - To adjust the video latency, you have two options:
     - Add a "Video Delay (Async)" filter to Audio/Video Filters on your video source (recommended if your audio comes from a different device).
     - Add a "Render Delay" filter to Effect Filters on your video source (not recommended).

## Science behind the V2 pattern

The v2 patterns uses sparse checkerboard events for video timing and far-field
acoustic packets for audio timing and identity. The exact audio event is the
center of the packet's short tick inside the centered matched-filter marker.
DTMF uses the standard 4x3 keypad frequencies, an 8-bit event code, CRC8, and
60 ms symbol guards for identity. The measurement is relative AV offset only;
it does not claim true source-to-capture transmission latency.

The audio packet is designed to be more tolerant of echo and reverberation than
a single short tone burst or quadrature amplitude modulation in the audible 
audio spectrum range. The timing fiducial is an 80 ms up/down chirp with a
short Ricker/Mexican-hat-style tick at the center, and the analyzer finds it by
matched-filter correlation; this is the same general signal-processing family
used for delay estimation of known waveforms in noisy and multipath channels.
The DTMF section is deliberately after the timing marker and guard interval, so
it identifies the event without defining the event time. DTMF also uses the
well-established two-frequency voice-band code standardized for push-button
signalling by ITU-T Q.23/Q.24, where receiver design explicitly considers
speech simulation, echoes, and noise immunity. This should be read as a
robustness design, not a guarantee: strong unresolved early reflections can
still bias any acoustic time-of-arrival measurement, so difficult rooms should
be validated with repeat runs and microphone placement changes.

Related background: [matched-filter delay estimation](https://arxiv.org/abs/1101.2713),
[multipath delay-estimation bias](https://arxiv.org/abs/2012.05790),
[ITU-T Q.23](https://www.itu.int/rec/T-REC-Q.23/en), and
[ITU-T Q.24](https://www.itu.int/rec/T-REC-Q.24/en).


## Build flow
See [main.yml](.github/workflows/main.yml) for the exact build flow.
