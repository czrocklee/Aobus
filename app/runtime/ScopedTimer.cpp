// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/rt/ScopedTimer.h>

#include <ao/Contract.h>
#include <ao/rt/Log.h>

#include <chrono>

namespace ao::rt
{
  ScopedTimer::~ScopedTimer()
  {
    auto const end = Clock::now();
    auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - _start);

    try
    {
      APP_LOG_DEBUG("[perf] {} took {} ms", _label, elapsed.count());
    }
    catch (...)
    {
      AO_AUDITED_CATCH(DiagnosticFallback);
      // Best-effort logging from a destructor; must not propagate.
    }
  }
} // namespace ao::rt
