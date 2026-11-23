---
id: user.play-music
---

# Play music in the Linux GTK application

## Outcome

Playback starts from the selected track in the Linux GTK application.
The view you start from supplies the eligible tracks: sequential playback follows the source order, while shuffle chooses from the same source without following that order.

For other frontends, see [Use the Windows desktop](use-windows-desktop.md) or [Use the terminal frontend](use-tui.md).

## Steps

### Start and control playback in GTK

1. Open a list or library view and select the track where playback should begin.
2. Press Enter or activate the row to start a sequence from that view.
3. Use the transport controls for play/pause, previous, next, shuffle, and repeat.
   Play in the window or from system media controls resumes a paused or restored track, even if another row is selected.
   When stopped with nothing to resume, Play starts the first selected track still visible in the current view, provided the output is available.
   The default application shortcuts include Ctrl+P for play/pause, Ctrl+Left and Ctrl+Right for previous and next, Ctrl+U for shuffle, and Ctrl+R for repeat.
4. Drag or click the seek slider, or focus it and press Left or Right without modifiers to move five seconds per press. Repeated presses add up.
   If a different track starts or the same track starts again during a drag, the slider and clock show the new playback position. Release and drag again to seek.
5. Use the volume button for precision control, scroll over it for two-percentage-point changes, or middle-click it to toggle mute.
6. Open **Edit → Preferences... → Playback/Output** to choose an output device.

### Reveal the current track in GTK

Press Ctrl+L to select the playing track in the List where playback started, even if the track also appears in the List you're browsing. If a quick filter hides it there, Aobus shows an unfiltered view without clearing your filter. If the source List was deleted or no longer contains the track, Aobus looks in **All Tracks** instead.
If reveal changes views, press the mouse Back button once to return to the view and filter you left.

### Choose what happens if a PipeWire device disappears

In the output chooser, select your named PipeWire device, such as a USB DAC. Leave **Let system use other outputs** off (the default) if you don't want audio to play through speakers when the DAC is absent at startup or disconnects during playback.

If you request an unavailable output while music is playing, Aobus holds the track and its position. Trying to play another track while the output is unavailable does not replace the held track. Stop discards the held track, but keeps the output choice. Reconnect the device or choose an available output, then press Play; making the output available does not itself resume the track. If a listed device fails to open, check the playback error and choose a working output before retrying.

If the chooser says the saved PipeWire choice is invalid, select the intended device again. This can happen after upgrading from a version that saved only a device number. Reconnecting alone cannot restore that choice.
If the device is simply unavailable, check that it is connected. If its name changed, select it again. Aobus does not guess a match from a device number or from a name shared by multiple devices.

Turn on **Let system use other outputs** to allow PipeWire to choose a substitute at startup or after disconnection. If your DAC returns while music is playing elsewhere, selecting its highlighted row again does not force the stream back: the row shows your preference, not the current speakers. To require the DAC, turn **Let system use other outputs** off.

With **Exclusive Mode**, Aobus still requests exclusive access: playback can fail if the substitute is busy, and other apps may be unable to use it while Aobus holds it.

Choose **System Default** to follow system routing without preferring a particular device. The rerouting switch is available only for named PipeWire outputs.

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
- Browsing another list does not switch the playback source. With shuffle on, Previous uses playback history when it does not restart the current track.
- The elapsed position, volume, output device, and quality surface update as playback and output state change.

If a track or output route cannot start, read the activity/notification diagnostic before retrying or changing devices.

## Related documents

- [GTK keymap reference](../reference/shell/keymap.md)
- [Playback cursor specification](../system/playback/cursor.md)
- [Volume-control specification](../system/presentation/volume-control.md)
- [Audio-quality architecture](../system/playback/quality.md)
- [Quality-surface reference](../system/playback/quality-values.md)
