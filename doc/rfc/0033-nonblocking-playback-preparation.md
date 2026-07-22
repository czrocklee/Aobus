---
id: rfc.0033.nonblocking-playback-preparation
type: rfc
status: draft
domain: playback
summary: Proposes opening and preparing a playback candidate without blocking the runtime callback executor.
depends-on: none
---
# RFC 0033: Non-blocking playback preparation

## Problem

`PlaybackCommands::startFromView` currently reaches `Engine::stagePlayback` synchronously on the runtime callback executor.
That path opens the decoder, reads stream metadata, may reopen for negotiated format, seeks, constructs the streaming source, and performs initial preparation before returning.

The implementation explicitly publishes a preparing event before this call because the audio preflight can freeze the main thread.
A slow file, network filesystem, decoder, or backend therefore blocks playback controls and unrelated UI work.

The current prepare/commit split already keeps the previous playback state intact on failure.
The remaining defect is execution placement.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: None.

## Goals

- Perform decoder open, metadata read, negotiation, seek, and source preparation off the callback executor.
- Keep the previous sequence and transport current until prepared audio is accepted.
- Cancel or reject preparation after newer start, stop, output-route change, or shutdown.
- Return preparation failures through the existing playback failure/reporting path.
- Keep Engine adoption and application snapshot commit on their existing owners.

## Non-goals

- Change public playback command or result types.
- Add automatic retries.
- Move steady-state decode or render ownership.
- Change playback-session persistence.
- Make backend activation asynchronous unless profiling proves it remains a separate blocking step.

## Proposed design

### Runtime admission

`PlaybackService` validates the view/track request and prepares the succession candidate on the callback executor.
It then records one private pending start containing that candidate, a stop source, and the current command/route generations.

Launching worker preparation ends the command turn; it does not keep a service commit or command-queue slot open.
Later stop, replacement start, next, previous, output-route change, clear, restore, or shutdown invalidates and cancels that pending start before executing its own behavior.

### Worker preparation and adoption

Split audio start into worker-safe preparation and Engine adoption:

1. On the callback executor, capture the playback request, source context, selected route values, and the existing Engine start-context generation.
2. On the async worker pool, create/open the decoder, inspect metadata, negotiate the source format, seek, and construct an uninstalled streaming source.
3. Return the owned prepared value to the callback executor.
4. Reject it if command, route, playback, or shutdown generation no longer matches.
5. Ask Engine to adopt the prepared source and perform its existing synchronized commit.
6. Install succession/transport state and publish one settled `PlaybackService` snapshot.

The worker receives immutable route/device values and the decoder factory capability needed for preparation.
It does not access `PlaybackService`, succession, frontend objects, or mutable Engine state.

Completion re-enters `PlaybackService` as one executor turn and either discards the stale candidate or performs the existing silent transport/succession install inside one logical commit.
The old playback remains current while preparation is pending; unrelated commands keep their current meaning against that old state.

`startFromView` returns the existing immediate validation/admission `Result<>`.
Once worker work is admitted, later failure uses the current track/route failure notification path and leaves the previous playback current.

One stop source and the existing playback/start-context generations provide cancellation and stale-result rejection.

## Alternatives

### Keep synchronous preparation

This preserves immediate decoder errors but allows external I/O and decoder work to freeze the application-control thread.

### Move all of `Engine::stagePlayback` to a worker

That would move Engine mutation and control locking to an arbitrary worker and blur its current adoption authority.
Only isolated source preparation moves.

## Compatibility and migration

Public command types and successful playback behavior stay unchanged.
A decoder failure that was formerly returned synchronously after admission becomes an asynchronous playback failure.

## Validation

- A controlled blocking decoder does not block callback-executor heartbeat, stop, output selection, or unrelated UI commands.
- Stop and replacement positioning commands execute while an older preparation worker is still blocked.
- Preparation failure leaves the previous sequence, transport subject, and audio generation current.
- Newer start, stop, output change, and shutdown each reject an older prepared value.
- Prepared decoder/source destruction occurs off the callback executor when cancellation would otherwise block.
- Engine adoption revalidates route and start-context generation before changing playback.
- Shutdown joins preparation work before decoder factory, Engine, library, or callback targets are destroyed.
- Existing start, gapless, failure, session, and frontend behavior tests remain valid.

## Promotion plan

Update playback and runtime-execution architectures with worker preparation and callback-executor adoption.
Update the playback application-commit and audio-execution specifications with admission, failure timing, cancellation, and teardown behavior.
