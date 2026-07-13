---
id: user.play-music
type: user-guide
status: current
domain: playback
summary: Starts a playback sequence, controls transport and volume, and selects an output device.
---
# Play music

## Outcome

Playback starts from the selected track and follows the current list projection through next, previous, shuffle, and repeat commands.

## Steps

### Start and control playback in GTK

1. Open a list or library view and select the track where playback should begin.
2. Press Enter or activate the row to start a sequence from that view.
3. Use the transport controls for play/pause, previous, next, shuffle, and repeat.
   The default application shortcuts include Ctrl+P for play/pause, Ctrl+Left and Ctrl+Right for previous and next, Ctrl+U for shuffle, and Ctrl+R for repeat.
4. Drag or click the seek control to change position.
5. Use the volume button for precision control, scroll over it for two-percentage-point changes, or middle-click it to toggle mute.
6. Open **Edit → Preferences... → Playback/Output** to choose an output device.

### Inspect playback quality

Open the quality or pipeline surface while a track is active.
It reports the decoded stream, processing path, output route, and findings used for the current quality conclusion.
Treat the conclusion as evidence about the active pipeline, not as a property stored on the track.

## Verify the result

- The now-playing track matches the row you activated.
- Next and previous remain anchored to the playback source even if the visible list later changes.
- The elapsed position, volume, output device, and quality surface update when their corresponding runtime state changes.

If a track or output route cannot start, read the activity/notification diagnostic before retrying or changing devices.

## Related documents

- [GTK keymap reference](../reference/shell/keymap.md)
- [Playback cursor specification](../spec/playback/cursor.md)
- [Volume-control specification](../spec/presentation/volume-control.md)
- [Audio-quality architecture](../architecture/audio-quality.md)
- [Quality-surface reference](../reference/playback/quality-surface.md)
