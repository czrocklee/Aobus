# AppKit shell composition

The AppKit application delegate coordinates the native presentation and library session for the macOS development slice. Closing the main window hides it without releasing the library session, after any editor close has been accepted and sheets have settled. On Quit or a different-root switch, the editor and sheets settle before MediaPlayer admission closes and presentation owners detach. See the [AppKit lifecycle](../session-lifecycle.md#appkit) for teardown ordering and the [desktop library lifecycle](../desktop-library-lifecycle.md) for release before a successor starts. [Track Properties and Credits](appkit-track-properties.md) describes the editor's draft and save boundary.

## Opening and scanning

When creating a session on startup restore or explicit library opening (including `--library`), AppKit starts an initial scan if the [database marker](../desktop-library-lifecycle.md#invariants) is absent; the marker alone does not trigger a scan. An empty or incompletely indexed database with the marker requires **Rescan Library**. Reselecting the current library retains the session without checking the marker or starting a scan, and does not cancel an in-flight scan. An accepted different-root open retires the old session and cancels its scan.

## Rescan admission

The File menu checks the session's scanning flag when it validates **Rescan Library**. While a scan is active, the item is disabled and a direct request neither posts a busy notice nor queues another scan.

On completion, the session clears the flag and calls the library invalidation callback before posting the result to the notification feed. The callback schedules a later redraw; it is not a signal that the result has already been delivered.

## Playback slider

[`PlaybackBar.mm`](../../../app/macos-appkit/PlaybackBar.mm) uses the [shared seek interaction](../playback/README.md#position-and-keyboard-seeking). Pointer value changes preview against the starting occurrence and duration. Releasing a still-current gesture submits one final seek only if there was a value change. If playback is replaced, becomes unseekable, or another seek changes the position revision during the drag, the thumb follows the new position even while paused. The native cell may keep tracking, but its remaining actions cannot seek or move the thumb off that position. The session also checks the revision at dispatch, in case the change has not been presented yet.

Unmodified Left and Right on the focused slider queue relative steps against the presented occurrence and use live elapsed at execution. Other discrete value actions submit absolute seeks.

When the track has an unknown duration, the slider's accessibility value gives elapsed time and says the duration is unknown instead of using `--:--`.

The `presentation` scenario's [playback checks](../../../test/macos-appkit/AppKitPlaybackScenario.mm) cover repeated previews followed by one final seek, replacement including same-track replay, and relative key steps. They use generated AppKit events and native target/action callbacks, not physical input. They also inspect the unknown-duration accessibility value, not VoiceOver delivery.

## Text completion

Search fields, supported metadata fields, Credits name and role fields, and the List-expression editor use runtime completion providers. The [native adapter](../../../app/macos-appkit/EntryCompletionAdapter.mm) builds its popover views on first display. It applies a candidate's UTF-8 replacement span as an exact UTF-16 field-editor range; the display label is not insertion text.

Tab accepts the candidate; Return accepts only in search fields. Editor Return keeps its ordinary behavior. Up/Down and page commands navigate candidates. Escape dismisses the popover; caret and selection commands dismiss it without taking over native editing. A pointer or other non-command caret move and application deactivation also dismiss it.

The adapter refuses acceptance for marked input, disabled or read-only fields, text that cannot convert to UTF-8, a live selection, or stale source text or caret. If the source is stale, Tab or Return dismisses the popover and otherwise keeps its ordinary field behavior. A native text-change notification publishes the accepted metadata, expression, or quick filter. Each presentation owner detaches its adapter before the borrowed field or completion service retires.

The [authoring scenario](../../../test/macos-appkit/AppKitAuthoringScenario.mm) covers exact non-BMP span conversion, invalid spans, stale-source Tab fallthrough, caret and deactivation dismissal, editor acceptance, and teardown. The [desktop scenario](../../../test/macos-appkit/AppKitDesktopScenario.mm) covers Quick Filter and explicit expression completion in both shell modes. These exercise programmatic native editing and selector commands, not physical keyboard or VoiceOver delivery.

## List membership feedback

For a membership reply carrying an authoring status, `LibraryEditorModel` posts the shared `listMembershipEditNotification` summary to the feed. UIModel maps Applied and NoOp to transient Info, Busy to transient Warning, and Stale and Unavailable to history Error. An operation error stays inline instead of posting a completion notification. Neither model retirement nor reentrant shutdown may post a late completion. UIModel tests own the status mapping; the AppKit authoring scenario observes feed posts and durable tags for add, remove, and no-op, and checks failure suppression and both retirement paths.

## Now Playing and remote commands

The session-lifetime [`MediaPlayerAdapter`](../../../app/macos-appkit/MediaPlayerAdapter.mm) owns its MediaPlayer registration tokens, playback snapshot and command-availability subscriptions, and current artwork request. It registers Play, Pause, Play/Pause, Stop, Previous, Next, and Change Playback Position. It enables each from the current command-availability snapshot and disables all other commands: native commands default to enabled even without a handler.

Native callbacks use revocable admission without retaining the session. They synchronously return `NoActionableNowPlayingItem` for a registered command that is not currently available. `Success` means delivery was queued, not that playback changed.

Main-thread delivery checks live transport and availability, including Next's current subject. It counts duplicate Play while active and duplicate Pause while paused as handled without moving the playback clock anchor. Remote Play and Play/Pause follow the [shared target policy](../playback/application-commit.md#primary-play) and use the same selection callback as the window controls. The callback uses the live focused selection rather than rendered rows or an admission-time target. If there is no eligible target or the start is rejected, delivery settles without being handled. For a selection start, a handled delivery means immediate start admission, not audio startup. Selection-only availability changes update native admission without republishing Now Playing.

Change Playback Position accepts only a finite, nonnegative, in-range position and captures the current occurrence. Main-thread delivery rejects a replacement occurrence, including same-track replay; otherwise it submits a guarded final seek. The [runtime queue](../playback/application-commit.md) checks the occurrence and position again at execution. A backlog cannot retarget a seek; valid seeks for one occurrence keep FIFO order. Once handed off, the seek belongs to the runtime and may finish after the adapter retires.

Now Playing publishes title, artist, album, duration, playback state, rate, elapsed position, and matching decoded artwork. A transport, position-revision, or duration change establishes a new elapsed clock anchor. Metadata-only and artwork-only updates republish from the existing anchor; asynchronous artwork must still match the current resource.

Now Playing clears when there is no track, or when transport is Idle without a track held for deferred resume. Clearing succession does not hide a deferred track. Retirement also clears Now Playing, fences deliveries not yet handed off, unregisters and disables all seven commands, and cancels observation and artwork work. The [media scenario](../../../test/macos-appkit/AppKitMediaScenario.mm) checks command admission separately from unhandled delivery after selection clearing, filtering, or an injected start refusal. It also checks guarded seeks, clock preservation, and retirement. See [macOS development](../../development/macos.md#appkit-smoke-scenarios) for the distinction between native scenario and OS-surface evidence.
