---
id: user.play-music
---
# Play music in the Linux GTK application

## Outcome

Playback starts from the selected track in the Linux GTK application and follows the current list projection through next, previous, shuffle, and repeat commands.

For other frontends, see [Use the Windows desktop](use-windows-desktop.md) or [Use the terminal frontend](use-tui.md).

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
The interpretation below also applies to Windows and TUI quality surfaces; their controls for opening them differ.

Read three separate questions:

1. **Source fidelity:** is the encoded source known to be lossless or lossy? A lossy source can still be delivered without further signal changes.
2. **Pipeline changes:** does the observed path resample, change channels or precision, apply software gain, or mix another source? Inspect the findings to locate the change rather than treating every different PCM representation as a loss: padding and proven lossless conversions preserve the signal.
3. **Verification:** does Aobus have format evidence for the complete path to the reported output sink? Incomplete verification means missing evidence, not proof of degradation. A lossless source alone does not verify the output path.

Hardware volume, when the backend can identify it as such, does not lower the digital-path rating. Software attenuation changes digital samples; software amplification adds a clipping-risk warning, not proof that clipping occurred. A reported volume change whose hardware/software location is unknown is treated conservatively as an intervention.

The headline summarizes delivery rather than simply repeating the source's rating. If it reports incomplete evidence, inspect the available path and output route; do not assume changing the music file will supply missing backend evidence. Treat every conclusion as evidence about the current route, not as metadata stored on the track or a guarantee about audible quality. Track and output changes can change the conclusion.

## Verify the result

- The now-playing track matches the row you activated.
- Next and previous remain anchored to the playback source even if the visible list later changes.
- The elapsed position, volume, output device, and quality surface update when their corresponding runtime state changes.

If a track or output route cannot start, read the activity/notification diagnostic before retrying or changing devices.

## Related documents

- [GTK keymap reference](../reference/shell/keymap.md)
- [Playback cursor specification](../system/playback/cursor.md)
- [Volume-control specification](../system/presentation/volume-control.md)
- [Audio-quality architecture](../system/playback/quality.md)
- [Quality-surface reference](../system/playback/quality-values.md)
