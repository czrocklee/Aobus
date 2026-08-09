// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <cstdlib>
#include <exception>

namespace ao::detail
{
  constexpr void acknowledgeRawFatalBackend() noexcept
  {
  }

  [[noreturn]] void abortFatal();
  [[noreturn]] void abortRealtime();
}

#define AO_RAW_FATAL_BACKEND() ::ao::detail::acknowledgeRawFatalBackend()
#define AO_FATAL() ::ao::detail::abortFatal()
#define AO_RT_FATAL_EXCEPTION_AT() ::ao::detail::abortRealtime()

namespace application
{
  struct Transaction final
  {
    void abort();

    void cancel()
    {
      // NEGATIVE
      abort();
    }
  };

  void abort();

  void stopCustomSubsystem()
  {
    // NEGATIVE
    abort();
  }
} // namespace application

void globalAbortIsRejected()
{
  // POSITIVE
  ::abort();
}

void stdAbortIsRejected()
{
  // POSITIVE
  std::abort();
}

void importedAbortIsRejected()
{
  using std::abort;
  // POSITIVE
  abort();
}

void terminateIsRejected()
{
  // POSITIVE
  std::terminate();
}

void importedTerminateIsRejected()
{
  using std::terminate;
  // POSITIVE
  terminate();
}

void quickExitIsRejected()
{
  // POSITIVE
  std::quick_exit(1);
}

void immediateExitIsRejected()
{
  // POSITIVE
  std::_Exit(1);
}

void directAoFatalBackendIsRejected()
{
  // POSITIVE
  ao::detail::abortFatal();
}

void directAoRealtimeBackendIsRejected()
{
  // POSITIVE
  ao::detail::abortRealtime();
}

void publicAoFatalMacroIsAccepted()
{
  // NEGATIVE
  AO_FATAL();
}

void publicAoRealtimeMacroIsAccepted()
{
  // NEGATIVE
  AO_RT_FATAL_EXCEPTION_AT();
}

auto rawFatalAddressIsRejected()
{
  // POSITIVE
  return &std::abort;
}

[[noreturn]] void markedFatalBackendIsAccepted()
{
  AO_RAW_FATAL_BACKEND();
  // NEGATIVE
  std::abort();
}

[[noreturn]] void directMarkerHelperIsRejected()
{
  ao::detail::acknowledgeRawFatalBackend();
  // POSITIVE
  std::abort();
}

[[noreturn]] void nestedMarkerIsRejected()
{
  if (true)
  {
    AO_RAW_FATAL_BACKEND();
  }

  // POSITIVE
  std::abort();
}
