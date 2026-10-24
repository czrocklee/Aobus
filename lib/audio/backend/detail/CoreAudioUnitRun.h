// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <AudioToolbox/AudioOutputUnit.h>
#include <AudioToolbox/AudioUnit.h>
#include <MacTypes.h>

#include <functional>

namespace ao::audio::backend::detail
{
  // Native control operations default to the corresponding SDK calls.
  struct CoreAudioUnitRunOperations final
  {
    std::function<decltype(::AudioOutputUnitStart)> start{::AudioOutputUnitStart};
    std::function<decltype(::AudioOutputUnitStop)> stop{::AudioOutputUnitStop};
  };

  // Tracks a successful start until confirmed stop or unit detachment, without
  // owning the AudioUnit or proving its current physical running state. The caller
  // passes the same unit and serializes all operations and queries with nativeMutex.
  // A failed stop preserves its raw status and blocks activation/reset; no operation
  // retries automatically. The owner detaches this record only when revoking the
  // unit and remains responsible for native cleanup and disposal.
  class CoreAudioUnitRun final
  {
  public:
    // Requires both bindings to be nonempty. Does not invoke them or contain
    // exceptions thrown by an injected callable.
    explicit CoreAudioUnitRun(CoreAudioUnitRunOperations operations = {});

    // Requires canStart() before the bound start call. noErr marks started.
    // Any other status is returned unchanged and does not mark started.
    // Does not stop, retry, or substitute a previous stop status.
    ::OSStatus start(::AudioUnit unit);

    // Returns noErr without a native call when the unit is not started.
    // Otherwise calls the bound stop once, including after a previous failure,
    // saves that status, and clears started only when the status is noErr.
    ::OSStatus stop(::AudioUnit unit);

    // True after a successful start until a confirmed stop or detach.
    bool isStarted() const noexcept;

    // True when the saved stop status is not noErr.
    bool hasUnconfirmedStop() const noexcept;

    // Raw status saved by the last stop call, or noErr when none has failed.
    ::OSStatus lastStopStatus() const noexcept;

    // False only while a native stop remains unconfirmed.
    bool canStart() const noexcept;

    // False while a successful start has not been confirmed stopped.
    bool canReset() const noexcept;

    // Drops started and stop status at unit detachment or revocation.
    // Does not call the SDK, dispose the unit, or recover it.
    void detach() noexcept;

  private:
    CoreAudioUnitRunOperations _operations;
    bool _started = false;
    ::OSStatus _stopStatus = ::noErr;
  };
} // namespace ao::audio::backend::detail
