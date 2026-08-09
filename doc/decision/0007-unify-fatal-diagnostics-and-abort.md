---
id: decision.0007.unify-fatal-diagnostics-and-abort
type: decision
status: accepted
domain: system
summary: Unifies caller contracts, postconditions, invariants, realtime faults, and terminal infrastructure failures behind one diagnostic abort facility.
---
# Decision 0007: unify fatal diagnostics and abort

## Context

The August 2026 error-handling review found overlapping non-recoverable channels across Aobus.
Raw GSL contracts, C assertions, general exceptions, direct `std::terminate()` or `std::abort()` calls, and application-local fatal helpers could describe the same programmer or infrastructure fault while producing different diagnostics and catch behavior.
GSL checking was already active in every build configuration, but C assertions disappeared under `NDEBUG`, and raw termination could bypass application logging and source context.

Aobus is a single-user desktop music application whose process owns one coherent live runtime graph.
After an internal protocol or mandatory post-commit infrastructure contract fails, continuing in-process is less trustworthy than terminating and rebuilding from durable state at the next start.
External input, persisted data before validation, devices, IO, and ordinary state conflicts still require recoverable channels where an owner can report or retry.

Cross-platform feasibility probes on Linux and Windows showed that a fresh subprocess can capture a stable emergency marker and distinguish abnormal termination under Debug, Release, AddressSanitizer, and ThreadSanitizer without replacing abort with a test-only throw.

## Decision

Aobus uses one Core-owned fatal facility for non-recoverable project failures.
It exposes distinct diagnostic categories for caller preconditions, callee postconditions, internal invariants, realtime invariants, and unconditional fatal infrastructure failures while ending every category through `std::abort()`.

The Core facility always has an emergency diagnostic path that does not depend on application logging.
The application may register one process-wide, non-throwing, bounded fatal sink after logging initialization.
That sink is diagnostic-only: it cannot recover state, acknowledge an operation, publish application state, or prevent abort.
It is unregistered only after fatal producers quiesce and before the logger is destroyed.

The realtime entry accepts only static context, performs no dynamic formatting, and never calls the application logging sink.
Ordinary entries may format bounded context and attempt the application sink after the emergency diagnostic.
Formatting or sink failure cannot prevent the final abort.

Fatal behavior is tested only in fresh subprocesses.
The production backend is not replaced with an exception or long-jump hook in tests.

Adoption is staged in independently reviewable changes.
Introducing the facility does not by itself claim that every legacy GSL contract, C assertion, general exception, or direct termination site has already migrated.

## Alternatives considered

### Keep raw GSL contracts and direct termination

Rejected because category, source context, application diagnostics, reentrancy handling, and the final primitive would remain call-site dependent.
It would also leave C assertions configuration-dependent.

### Throw a fatal exception to an application leaf

Rejected because a general catch can accidentally treat the fault as recoverable and continue with partially invalid state.
Exception unwinding remains available only for explicitly owned transport and adapter boundaries; it is not the fatal contract.

### Put the fatal facility in the application runtime

Rejected because Core libraries and realtime code cannot depend upward on application logging or runtime services.
The dependency points from the application adapter into the Core registration seam.

### Replace abort with a throwing test hook

Rejected because it tests different control flow, permits RAII unwind that production never performs, and cannot prove process diagnostics or sanitizer behavior.

## Consequences

- Fatal categories remain semantically distinct even though they share one final primitive.
- A fatal check never returns and does not promise stack unwinding or in-process recovery.
- Emergency diagnostics remain available before logging initialization and after sink removal.
- Application fatal logging is best effort and bounded; it cannot promise durable log delivery before process death.
- The realtime path has less diagnostic richness than the ordinary path in exchange for avoiding formatting and application synchronization.
- Sink registration, producer quiescence, and logger teardown become an explicit process-lifetime contract.
- Subprocess death tests add platform-specific process-launch support and sanitizer-aware status assertions.
- Legacy failure sites remain truthful only as they are migrated in later, independently coherent changes.

## Current authorities

- [Failure and reporting architecture](../architecture/failure-and-reporting.md)
- [Runtime execution architecture](../architecture/runtime-execution.md)
- [Outcome channel specification](../spec/failure/outcome-channel.md)
- [Fatal facility reference](../reference/failure/fatal.md)

## Supersession

Not superseded.
