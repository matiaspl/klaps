# OBS 32 asynchronous-source A/V startup timing

## Summary

In OBS 32.1.2, changing an active source's Audio Sync Offset from `0 ms` to a nonzero value and back to
`0 ms` can permanently change the measured A/V offset of an `OBS_SOURCE_ASYNC_VIDEO` source. The change is
visible in the mixed program output; it is not an analyzer-only artifact.

The immediate cause is a clock-authority handoff inside libobs:

1. Audio can establish `source->timing_adjust` and populate the source audio deque before the first asynchronous
   video frame is rendered.
2. Rendering the first asynchronous video frame replaces `timing_adjust` with a video-derived value.
3. Differences smaller than the 70 ms audio timestamp smoothing threshold are treated as continuous audio, so
   later packets are appended and the existing deque remains anchored to the earlier adjustment.
4. Returning Audio Sync Offset from a nonzero value to zero forces one timestamp-based deque placement. This
   re-anchors the audio to the current video-derived clock and produces a persistent measurement step.

The recommended minimal fix is to discard audio buffered before the first asynchronous video timing anchor. The
next audio packet is then placed using the video-derived adjustment. This belongs in the generic libobs async A/V
path rather than in the FFmpeg Media Source or the analyzer plugin.

## Observed behavior

The original observation used a Media Source containing H.264 video and PCM audio:

- Initial measurements varied between approximately `-23 ms` and `-40 ms` (audio early).
- Opening Advanced Audio Properties, changing Audio Sync Offset to `1 ms`, and returning it to `0 ms` produced a
  stable measurement near `+8.3 ms` (audio late).
- Browser Source did not show the same persistent transition.

A controlled OBS 32.1.2 harness run used a generated 30 FPS H.264 MOV with two B-frames, 48 kHz PCM audio, and
matching zero stream start times. One run produced four stable pre-nudge measurements of `-28.729 ms`.

An instrumented run changed the source offset after two markers:

```text
before: -41.770 ms
before: -41.770 ms
offset: 1 ms
offset: 0 ms
after:  +16.667 ms
after:  +16.667 ms
after:  +16.668 ms
after:  +16.668 ms
```

The exact post-nudge value depends on compositor phase and OBS audio buffering. The important result is that the
source's actual mixed-output timing moves and remains at the new position after the configured offset returns to
zero.

## OBS 32 source walkthrough

Line numbers below refer to the OBS Studio `32.1.2` tag.

### Media playback already preserves the file PTS relationship

The FFmpeg playback layer does not normalize audio and video independently:

- `mp_media_prepare_frames()` decodes until the initial enabled streams have frames ready in
  `shared/media-playback/media-playback/media.c:264`.
- `mp_media_reset()` selects the minimum initial presentation timestamp as the shared `start_ts` in
  `media.c:551-587`.
- Audio and video both use the mapping
  `base_ts + frame_pts - start_ts + play_sys_ts - base_sys_ts` in `media.c:350-367` and `media.c:453-454`.
- Decoded video uses FFmpeg's `best_effort_timestamp`, converted through the stream time base, in
  `shared/media-playback/media-playback/decode.c:386-407`.

Consequently, B-frame decode reordering can delay wall-clock availability of the first video frame, but it does
not by itself become a presentation timestamp offset. Adding another file probe would duplicate work already done
by media-playback and would not correct the later libobs deque anchoring.

### Audio may establish timing before asynchronous video renders

`source_output_audio_data()` in `libobs/obs-source.c:1575-1654` handles incoming source audio. If timing has not
yet been established, `reset_audio_timing()` sets:

```c
source->timing_adjust = os_time - timestamp;
```

The audio is then placed into `source->audio_input_buf` using that adjustment.

For asynchronous video, `obs_source_update_async_video()` later performs this on each selected render frame in
`libobs/obs-source.c:2514-2535`:

```c
source->timing_adjust = obs->video.video_time - frame->timestamp;
source->timing_set = true;
```

The first assignment can therefore replace an audio-derived mapping after audio is already buffered.

### Timestamp smoothing retains the old deque anchor

libobs uses `TS_SMOOTHING_THRESHOLD = 70000000` ns. When the next adjusted audio timestamp differs by less than
70 ms from `next_audio_sys_ts_min`, the packet is considered continuous and `push_back` is selected.

`source_output_audio_push_back()` appends samples but does not use the packet's absolute timestamp. Thus a
30-60 ms change in `timing_adjust` can be visible in packet timestamps without repositioning audio already in the
deque. The observed transitions all fall within this smoothing window.

### Returning from a nonzero sync offset forces placement

Advanced Audio Properties only calls `obs_source_set_sync_offset()` from
`frontend/components/OBSAdvAudioCtrl.cpp:500-519`; it does not restart or seek the source.

The significant behavior is in `source_output_audio_data()`:

```c
if (source->last_sync_offset != sync_offset) {
	if (source->last_sync_offset)
		push_back = false;
	source->last_sync_offset = sync_offset;
}
```

The `0 -> 1 ms` transition records a nonzero `last_sync_offset` but does not necessarily stop append mode. The
`1 ms -> 0` transition sets `push_back = false`, selecting `source_output_audio_place()`. Placement uses the
current absolute timestamp relative to `source->audio_ts`, so subsequent audio becomes anchored to the current
video-derived adjustment.

This explains why a nominally reversible UI change can leave the source at a different zero-offset timing.

### Browser Source takes a different video path

Media Source is registered with `OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO` in
`plugins/obs-ffmpeg/obs-ffmpeg-source.c:784-799`.

Browser Source is registered as synchronous `OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_CUSTOM_DRAW` in
`plugins/obs-browser/obs-browser-plugin.cpp:430-466`. Its video is drawn on the compositor timeline instead of
entering the asynchronous frame queue. CEF audio packets carry direct monotonic millisecond timestamps in
`plugins/obs-browser/browser-client.cpp:546-563`.

Browser Source therefore does not perform the same first-render replacement of an audio-established async-video
mapping.

### The analyzer reports post-compositor timing

The plugin analyzes encoder frames supplied by the OBS output pipeline. Video and audio timestamps come from the
encoder frame PTS in `src/sync-test-output.cpp:1504-1536`. A persistent change after the source nudge therefore
represents a change in the program output timeline, not a private Media Source PTS observed before mixing.

## Proposed solutions

### 1. Reset pre-anchor audio when the first async video frame renders

This is the recommended minimal solution.

When the first coupled asynchronous video frame establishes `timing_adjust`, clear audio buffered under the
previous adjustment while preserving `timing_set = true` and the new video-derived adjustment:

```c
const bool first_async_video_timing = !source->async_video_timing_set;

source->timing_adjust = obs->video.video_time - frame->timestamp;
source->timing_set = true;
source->async_video_timing_set = true;

if (first_async_video_timing) {
	pthread_mutex_lock(&source->audio_buf_mutex);
	reset_audio_data(source, 0);
	pthread_mutex_unlock(&source->audio_buf_mutex);
}
```

Reset `async_video_timing_set` when `obs_source_output_video(source, NULL)` clears the asynchronous video stream.
The next audio packet will see an empty deque and be placed using the video-derived mapping.

An explicit boolean is preferable for an upstream patch because zero is theoretically a valid frame timestamp.
A smaller experimental patch could reuse `async_last_rendered_ts == 0` as the first-frame discriminator and reset
that field when video is cleared.

Benefits:

- Central fix for coupled async A/V sources, including Media Source, VLC, DeckLink, AJA, and macOS AV capture.
- No codec, container, or B-frame special case.
- Runs once per async-video start or restart rather than responding to normal per-frame render jitter.
- Keeps video as the clock authority expected by the existing libobs timing design.

Tradeoff:

- Audio received before the first displayed video frame is discarded. This is normally only the short startup
  region currently attached to the wrong clock. The source may produce startup silence until the next audio
  packet is placed.

The reset must not set `timing_set = false`; doing so would allow the next audio packet to replace the new
video-derived mapping and recreate the original problem.

### 2. Hold audio until async video timing is established

Instead of accepting and then clearing early audio, libobs could hold or discard audio from a coupled async source
until its first video frame has established timing.

This makes clock ownership explicit and avoids temporary allocation of incorrectly anchored audio. It needs more
state and careful behavior for sources whose video is absent, delayed indefinitely, disabled, or intentionally
decoupled. It is therefore less suitable as the shortest corrective patch.

### 3. Establish one source-owned common clock mapping

For sources that guarantee audio and video timestamps share a clock domain, libobs could establish one mapping
from the first valid source PTS and never replace it merely because video renders later:

```text
OBS timestamp = source PTS + common adjustment
```

This is architecturally clean, but callback arrival order must not define presentation order. B-frame reorder,
decoder scheduling, and audio packet duration all affect arrival time. The source API would also need to state
whether its audio and video timestamps are comparable. That assumption is safe for a demuxed file but not
necessarily for every capture source.

If playback must know the minimum initial audio/video PTS before emitting either stream, it needs the first
decoded frame from each enabled stream rather than only container metadata. OBS Media Source already performs
that preparation, so no additional probing is required there.

### 4. Force timestamp placement whenever `timing_adjust` changes

This is not recommended. Async video updates `timing_adjust` as frames are selected for rendering. Re-placing
audio for every small adjustment would convert harmless scheduling jitter into deque movement, gaps, overwrites,
or audible discontinuities.

Likewise, lowering or disabling the 70 ms smoothing threshold globally would affect timestamp-jitter tolerance
for all audio sources and would be much broader than the startup problem.

## Validation plan

Test the selected patch against OBS 32.1.2 on macOS and at least one other supported platform:

1. Generate zero-offset PCM patterns with intra-only H.264, P-frame GOPs, and B-frames.
2. Run repeated cold starts and compare the first several markers across runs.
3. Apply `1 ms -> 0 ms` while playing and verify that returning to zero no longer causes a persistent step.
4. Verify positive and negative configured sync offsets, including single-step `0 -> 1 ms` changes.
5. Test looping, restart, seek, deactivate/reactivate, and `obs_source_output_video(source, NULL)` recovery.
6. Test Media Source, VLC, and representative capture sources that provide coupled asynchronous audio and video.
7. Confirm video-only async sources are unchanged.
8. Confirm async-unbuffered/async-decoupled sources retain their intended independent-clock behavior.
9. Check logs for audio lag, timestamp jump, and maximum-buffering resets.
10. Record program output and verify packet placement independently of the live analyzer.

Acceptance should be based on repeatable pre/post runs. A near-zero generated file does not imply a literal zero
measurement through every OBS path, but cold-start results and a zero-offset nudge must converge to the same
stable baseline.
