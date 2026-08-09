// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/fatal/FatalProbeScenario.h"

#include "app/runtime/playback/PreparedNextContract.h"
#include <ao/Contract.h>

#include <cstdint>
#include <cstdio>
#include <exception>
#include <semaphore>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace ao::test
{
  namespace
  {
    std::binary_semaphore& concurrentFatalEntered()
    {
      static auto semaphore = std::binary_semaphore{0};
      return semaphore;
    }

    template<typename T>
    [[noreturn]] void throwProbeValue(T&& value)
    {
      throw std::forward<T>(value);
    }

    bool acceptingSink(FatalDiagnostic const& /*diagnostic*/)
    {
      std::fputs("AOBUS_TEST sink=accepted\n", stderr);
      std::fflush(stderr);
      return true;
    }

    bool rejectingSink(FatalDiagnostic const& /*diagnostic*/)
    {
      return false;
    }

    bool throwingSink(FatalDiagnostic const& /*diagnostic*/)
    {
      throwProbeValue(std::runtime_error{"probe sink failure"});
    }

    bool recursiveSink(FatalDiagnostic const& /*diagnostic*/)
    {
      AO_FATAL("recursive sink entry");
    }

    bool blockingSink(FatalDiagnostic const& /*diagnostic*/)
    {
      concurrentFatalEntered().release();

      for (;;)
      {
        std::this_thread::yield();
      }
    }

    bool throwingCondition()
    {
      throwProbeValue(std::runtime_error{"probe condition failure"});
    }

    std::string_view throwingContext()
    {
      throwProbeValue(std::runtime_error{"probe context failure"});
    }

    void requireSinkRegistration(FatalSink sink)
    {
      auto const registered = registerFatalSink(sink);
      AO_INVARIANT(registered, "Fatal probe could not register its sink");
    }
  } // namespace

  std::int32_t runFatalProbeScenario(std::string_view scenario)
  {
    if (scenario == "expects")
    {
      std::int32_t evaluationCount = 0;
      AO_EXPECTS(++evaluationCount == 2, "evaluation count {}", evaluationCount);
    }

    if (scenario == "duplicate-prepared-next")
    {
      rt::detail::expectPreparedNextSlotAvailable(true);
    }

    if (scenario == "ensures")
    {
      AO_ENSURES(false, "postcondition probe");
    }

    if (scenario == "invariant")
    {
      AO_INVARIANT(false, "invariant probe");
    }

    if (scenario == "formatted-fatal")
    {
      AO_FATAL("formatted value {}", 42);
    }

    if (scenario == "realtime-invariant")
    {
      requireSinkRegistration(&acceptingSink);
      AO_RT_INVARIANT(false, "realtime probe");
    }

    if (scenario == "accepted-sink")
    {
      requireSinkRegistration(&acceptingSink);
      AO_FATAL("accepted sink probe");
    }

    if (scenario == "rejected-sink")
    {
      requireSinkRegistration(&rejectingSink);
      AO_FATAL("rejected sink probe");
    }

    if (scenario == "throwing-sink")
    {
      requireSinkRegistration(&throwingSink);
      AO_FATAL("throwing sink probe");
    }

    if (scenario == "recursive-sink")
    {
      requireSinkRegistration(&recursiveSink);
      AO_FATAL("recursive sink probe");
    }

    if (scenario == "unhandled-exception")
    {
      try
      {
        throwProbeValue(std::runtime_error{"probe exception"});
      }
      catch (...)
      {
        AO_FATAL_EXCEPTION(std::current_exception(), "probe root");
      }
    }

    if (scenario == "unhandled-unknown-exception")
    {
      try
      {
        throwProbeValue(42);
      }
      catch (...)
      {
        AO_FATAL_EXCEPTION(std::current_exception(), "unknown root");
      }
    }

    if (scenario == "missing-exception")
    {
      AO_FATAL_EXCEPTION(std::exception_ptr{}, "missing root");
    }

    if (scenario == "truncated-context")
    {
      auto const oversizedContext = std::string(detail::kFatalContextCapacity + 128, 'x');
      AO_FATAL("{}", oversizedContext);
    }

    if (scenario == "truncated-diagnostic")
    {
      auto const oversizedCondition = std::string(8192, 'x');
      detail::abortFatalDiagnostic(
        FatalCategory::Fatal, oversizedCondition, {}, false, std::source_location::current(), false);
    }

    if (scenario == "throwing-condition")
    {
      AO_EXPECTS(throwingCondition(), "unreachable context");
    }

    if (scenario == "throwing-context")
    {
      AO_FATAL(throwingContext());
    }

    if (scenario == "concurrent-entry")
    {
      requireSinkRegistration(&blockingSink);
      auto firstFatalThread = std::jthread{[] { AO_FATAL("first concurrent fatal"); }};
      concurrentFatalEntered().acquire();
      AO_FATAL("second concurrent fatal");
    }

    return 2;
  }
} // namespace ao::test
