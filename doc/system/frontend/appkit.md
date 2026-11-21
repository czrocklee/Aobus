# AppKit shell composition

The AppKit application delegate coordinates the native presentation and library session for the macOS development slice. Closing the main window hides it without releasing the library session, after any editor close has been accepted and sheets have settled. On Quit or a different-root switch, the editor and sheets settle before MediaPlayer admission closes and presentation owners detach. See the [AppKit lifecycle](../session-lifecycle.md#appkit) for teardown ordering and the [desktop library lifecycle](../desktop-library-lifecycle.md) for release before a successor starts. [Track Properties and Credits](appkit-track-properties.md) describes the editor's draft and save boundary.

## Playback slider

[`PlaybackBar.mm`](../../../app/macos-appkit/PlaybackBar.mm) uses shared `SeekInteraction` classification. Continuous pointer actions publish previews; ending the current tracking gesture delivers one final seek only if there was a value action. Keyboard and discrete actions commit directly.

The gesture retains its starting playback occurrence and duration. A replacement occurrence (including same-track replay) or loss of seekability retires it instead of retargeting the preview or final seek. The session also rejects an obsolete position revision. The `presentation` scenario's [playback checks](../../../test/macos-appkit/AppKitPlaybackScenario.mm) cover repeated previews during one gesture, one final commit, and replacement including same-track replay, using generated AppKit events and native target/action callbacks, not physical input.

## Text completion

Search fields, supported metadata fields, Credits name and role fields, and the List-expression editor use runtime completion providers. The [native adapter](../../../app/macos-appkit/EntryCompletionAdapter.mm) builds its popover views on first display. It applies a candidate's UTF-8 replacement span as an exact UTF-16 field-editor range; the display label is not insertion text.

Tab accepts the candidate; Return accepts only in search fields. Editor Return keeps its ordinary behavior. Up/Down and page commands navigate candidates. Escape dismisses the popover; caret and selection commands dismiss it without taking over native editing. A pointer or other non-command caret move and application deactivation also dismiss it.

The adapter refuses acceptance for marked input, disabled or read-only fields, text that cannot convert to UTF-8, a live selection, or stale source text or caret. If the source is stale, Tab or Return dismisses the popover and otherwise keeps its ordinary field behavior. A native text-change notification publishes the accepted metadata, expression, or quick filter. Each presentation owner detaches its adapter before the borrowed field or completion service retires.

The [authoring scenario](../../../test/macos-appkit/AppKitAuthoringScenario.mm) covers exact non-BMP span conversion, invalid spans, stale-source Tab fallthrough, caret and deactivation dismissal, editor acceptance, and teardown. The [desktop scenario](../../../test/macos-appkit/AppKitDesktopScenario.mm) covers Quick Filter and explicit expression completion in both shell modes. These exercise programmatic native editing and selector commands, not physical keyboard or VoiceOver delivery.

## List membership feedback

For a membership reply carrying an authoring status, `LibraryEditorModel` posts the shared `listMembershipEditNotification` summary to the feed. UIModel maps Applied and NoOp to transient Info, Busy to transient Warning, and Stale and Unavailable to history Error. An operation error stays inline instead of posting a completion notification. Neither model retirement nor reentrant shutdown may post a late completion. UIModel tests own the status mapping; the AppKit authoring scenario observes feed posts and durable tags for add, remove, and no-op, and checks failure suppression and both retirement paths.

## Now Playing and remote commands

The session-lifetime [`MediaPlayerAdapter`](../../../app/macos-appkit/MediaPlayerAdapter.mm) owns its MediaPlayer registration tokens, playback snapshot subscription, and current artwork request. It registers Play, Pause, Play/Pause, Stop, Previous, Next, and Change Playback Position. It enables each from the current command-availability snapshot and disables all other commands: native commands default to enabled even without a handler.

Native callbacks use revocable admission without retaining the session. Play, Pause, Play/Pause, Stop, Previous, and Next enqueue main-thread delivery, which resolves the current transport and action availability. Each admitted Next resolves the then-current subject and runs only if still available. Duplicate Play while active and duplicate Pause while paused succeed without moving the playback clock anchor.

Change Playback Position accepts only a finite, nonnegative, in-range position and captures the current occurrence. Main-thread delivery rejects a replacement occurrence, including same-track replay; otherwise it submits a guarded final seek. The [runtime queue](../playback/application-commit.md) checks the occurrence and position again at execution. A backlog cannot retarget a seek; valid seeks for one occurrence keep FIFO order. Once handed off, the seek belongs to the runtime and may finish after the adapter retires.

Now Playing publishes title, artist, album, duration, playback state, rate, elapsed position, and matching decoded artwork. A transport, position-revision, or duration change establishes a new elapsed clock anchor. Metadata-only and artwork-only updates republish from the existing anchor; asynchronous artwork must still match the current resource.

Idle state without a resumable subject clears the publication. Retirement also clears it, fences deliveries not yet handed off, unregisters and disables all seven commands, and cancels observation and artwork work. The [media scenario](../../../test/macos-appkit/AppKitMediaScenario.mm) protects command admission, guarded seeks, clock preservation, and retirement. See [macOS development](../../development/macos.md#appkit-smoke-scenarios) for the distinction between native scenario and OS-surface evidence.
