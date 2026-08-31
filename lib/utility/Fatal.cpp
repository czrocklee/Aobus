// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/Contract.h>

#ifdef _WIN32
#include <io.h>
#include <stdlib.h> // NOLINT(modernize-deprecated-headers) -- MSVC abort-control APIs require this header.
#else
#include <pthread.h>
#include <signal.h> // NOLINT(modernize-deprecated-headers) -- POSIX signal-set APIs require this header.
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <source_location>
#include <string_view>
#include <system_error>

// Apple SDKs declare these POSIX functions before defining function-like
// macros with the same names. Remove the macros so qualified calls resolve to
// the declared functions; #undef is a no-op on implementations without them.
#undef sigaddset
#undef sigemptyset

namespace ao
{
  namespace
  {
    constexpr std::size_t kEmergencyDiagnosticCapacity = 4096;

    template<std::size_t Capacity>
    class BoundedText final
    {
    public:
      void append(std::string_view value) noexcept
      {
        auto const remaining = Capacity - _size;
        auto const appendedSize = value.size() < remaining ? value.size() : remaining;

        if (appendedSize > 0)
        {
          // The explicit byte count bounds this copy; null termination is neither required nor inspected.
          // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
          std::memcpy(_text.data() + _size, value.data(), appendedSize);
          _size += appendedSize;
        }

        _truncated = _truncated || appendedSize < value.size();
      }

      void appendNumber(std::uint_least32_t value) noexcept
      {
        auto number = std::array<char, 16>{};
        auto const result = std::to_chars(number.data(), number.data() + number.size(), value);

        if (result.ec == std::errc{})
        {
          append(std::string_view{number.data(), static_cast<std::size_t>(result.ptr - number.data())});
        }
      }

      std::string_view view() const noexcept { return {_text.data(), _size}; }
      bool isTruncated() const noexcept { return _truncated; }

      void finishDiagnostic() noexcept
      {
        if (!_truncated && _size < Capacity)
        {
          append("\n");
          return;
        }

        _truncated = true;
        constexpr auto kSuffix = std::string_view{" diagnostic-truncated=true\n"};
        static_assert(kSuffix.size() <= Capacity);
        _size = std::min(_size, Capacity - kSuffix.size());
        std::memcpy(_text.data() + _size, kSuffix.data(), kSuffix.size());
        _size += kSuffix.size();
      }

    private:
      std::array<char, Capacity> _text{};
      std::size_t _size = 0;
      bool _truncated = false;
    };

    // Fatal and realtime entry require namespace-scope constant initialization without a guarded accessor.
    // NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
    constinit std::atomic fatalSink{FatalSink{nullptr}};
    constinit std::atomic_flag fatalInProgress = ATOMIC_FLAG_INIT;
    constinit thread_local bool fatalEntryActive = false;
    // NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

    static_assert(std::atomic<FatalSink>::is_always_lock_free);

    void writeEmergency(std::string_view text) noexcept
    {
#ifdef _WIN32
      [[maybe_unused]] auto const written = ::_write(2, text.data(), static_cast<std::uint32_t>(text.size()));
#else
#ifdef __APPLE__
      ::sigset_t blockedSignals = 0;
#else
      auto blockedSignals = ::sigset_t{};
#endif
      ::sigemptyset(&blockedSignals);
      ::sigaddset(&blockedSignals, SIGPIPE);
      [[maybe_unused]] auto const maskResult = ::pthread_sigmask(SIG_BLOCK, &blockedSignals, nullptr);
      [[maybe_unused]] auto const written = ::write(STDERR_FILENO, text.data(), text.size());
#endif
    }

    void writeEmergencyDiagnostic(FatalDiagnostic const& diagnostic) noexcept
    {
      auto output = BoundedText<kEmergencyDiagnosticCapacity>{};
      output.append("AOBUS_FATAL category=");
      output.append(fatalCategoryName(diagnostic.category));
      output.append(" source=");
      output.append(diagnostic.location.file_name());
      output.append(":");
      output.appendNumber(diagnostic.location.line());
      output.append(" function=");
      output.append(diagnostic.location.function_name());

      if (diagnostic.contextTruncated)
      {
        output.append(" context-truncated=true");
      }

      if (!diagnostic.condition.empty())
      {
        output.append(" condition=");
        output.append(diagnostic.condition);
      }

      if (!diagnostic.context.empty())
      {
        output.append(" context=");
        output.append(diagnostic.context);
      }

      output.finishDiagnostic();
      writeEmergency(output.view());
    }

    [[noreturn]] void terminateProcess() noexcept
    {
      AO_RAW_FATAL_BACKEND();
#ifdef _WIN32
      // Debug CRT abort() otherwise shows a message box and can wait for WER/JIT.
      ::_set_error_mode(_OUT_TO_STDERR);
      ::_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
      std::abort();
    }

    [[noreturn]] void abortImmediately(std::string_view marker) noexcept
    {
      writeEmergency(marker);
      terminateProcess();
    }

    template<std::size_t Capacity>
    void appendExceptionContext(BoundedText<Capacity>& output,
                                std::string_view context,
                                std::string_view exceptionMessage) noexcept
    {
      if (!context.empty())
      {
        output.append(context);
        output.append(": ");
      }

      output.append(exceptionMessage);
    }
  } // namespace

  std::string_view fatalCategoryName(FatalCategory category) noexcept
  {
    switch (category)
    {
      case FatalCategory::Expects: return "expects";
      case FatalCategory::Ensures: return "ensures";
      case FatalCategory::Invariant: return "invariant";
      case FatalCategory::Fatal: return "fatal";
      case FatalCategory::RealtimeInvariant: return "realtime-invariant";
      case FatalCategory::UnhandledException: return "unhandled-exception";
    }

    return "unknown";
  }

  bool registerFatalSink(FatalSink sink) noexcept
  {
    if (sink == nullptr)
    {
      return false;
    }

    auto expected = FatalSink{nullptr};
    return fatalSink.compare_exchange_strong(expected, sink, std::memory_order_release, std::memory_order_relaxed);
  }

  bool unregisterFatalSink(FatalSink sink) noexcept
  {
    if (sink == nullptr)
    {
      return false;
    }

    auto expected = sink;
    return fatalSink.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);
  }

  namespace detail
  {
    [[noreturn]] void abortFatalDiagnostic(FatalCategory category,
                                           std::string_view condition,
                                           std::string_view context,
                                           bool contextTruncated,
                                           std::source_location location,
                                           bool useApplicationSink) noexcept
    {
      if (fatalEntryActive)
      {
        abortImmediately("AOBUS_FATAL category=recursive-fatal\n");
      }

      fatalEntryActive = true;

      auto const diagnostic = FatalDiagnostic{.category = category,
                                              .condition = condition,
                                              .context = context,
                                              .location = location,
                                              .contextTruncated = contextTruncated};
      writeEmergencyDiagnostic(diagnostic);

      if (fatalInProgress.test_and_set(std::memory_order_acq_rel))
      {
        abortImmediately("AOBUS_FATAL category=concurrent-fatal\n");
      }

      if (useApplicationSink)
      {
        if (auto const sink = fatalSink.load(std::memory_order_acquire); sink != nullptr)
        {
          bool accepted = false;

          try
          {
            accepted = sink(diagnostic);
          }
          catch (...)
          {
            AO_AUDITED_CATCH(FatalSinkFallback);
            writeEmergency("AOBUS_FATAL sink=exception\n");
          }

          if (!accepted)
          {
            writeEmergency("AOBUS_FATAL sink=rejected\n");
          }
        }
        else
        {
          writeEmergency("AOBUS_FATAL sink=unavailable\n");
        }
      }

      terminateProcess();
    }

    [[noreturn]] void abortFatal(FatalCategory category,
                                 std::string_view condition,
                                 std::source_location location) noexcept
    {
      abortFatalDiagnostic(category, condition, {}, false, location, true);
    }

    [[noreturn]] void abortFatal(FatalCategory category,
                                 std::string_view condition,
                                 std::source_location location,
                                 std::string_view context) noexcept
    {
      abortFatalDiagnostic(category, condition, context, false, location, true);
    }

    [[noreturn]] void abortFatalEvaluation(FatalCategory category,
                                           std::string_view condition,
                                           std::source_location location) noexcept
    {
      abortFatalDiagnostic(category, condition, "Fatal condition or context evaluation threw", false, location, true);
    }

    [[noreturn]] void abortRealtimeEvaluation(FatalCategory category,
                                              std::string_view condition,
                                              std::source_location location) noexcept
    {
      abortFatalDiagnostic(category, condition, "Realtime fatal condition evaluation threw", false, location, false);
    }
  } // namespace detail

  [[noreturn]] void fatalFromException(std::exception_ptr exceptionPtr,
                                       std::string_view context,
                                       std::source_location location) noexcept
  {
    auto diagnosticContext = BoundedText<detail::kFatalContextCapacity>{};

    if (!exceptionPtr)
    {
      appendExceptionContext(diagnosticContext, context, "Missing exception payload");
      detail::abortFatalDiagnostic(FatalCategory::UnhandledException,
                                   {},
                                   diagnosticContext.view(),
                                   diagnosticContext.isTruncated(),
                                   location,
                                   true);
    }

    try
    {
      std::rethrow_exception(exceptionPtr);
    }
    catch (std::exception const& exception)
    {
      appendExceptionContext(diagnosticContext, context, exception.what());
      detail::abortFatalDiagnostic(FatalCategory::UnhandledException,
                                   {},
                                   diagnosticContext.view(),
                                   diagnosticContext.isTruncated(),
                                   location,
                                   true);
    }
    catch (...)
    {
      appendExceptionContext(diagnosticContext, context, "Unknown exception");
      detail::abortFatalDiagnostic(FatalCategory::UnhandledException,
                                   {},
                                   diagnosticContext.view(),
                                   diagnosticContext.isTruncated(),
                                   location,
                                   true);
    }
  }
} // namespace ao
