// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <iterator>
#include <source_location>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ao
{
  enum class FatalCategory : std::uint8_t
  {
    Expects,
    Ensures,
    Invariant,
    Fatal,
    RealtimeInvariant,
    UnhandledException,
  };

  struct FatalDiagnostic final
  {
    FatalCategory category;
    std::string_view condition;
    std::string_view context;
    std::source_location location;
    bool contextTruncated = false;
  };

  using FatalSink = bool (*)(FatalDiagnostic const& diagnostic);

  std::string_view fatalCategoryName(FatalCategory category) noexcept;

  bool registerFatalSink(FatalSink sink) noexcept;
  bool unregisterFatalSink(FatalSink sink) noexcept;

  [[noreturn]] void fatalFromException(std::exception_ptr exceptionPtr,
                                       std::string_view context,
                                       std::source_location location = std::source_location::current()) noexcept;

  namespace detail
  {
    constexpr std::size_t kFatalContextCapacity = 2048;

    enum class AuditedCatchReason : std::uint8_t
    {
      ExceptionClassifier,
      FatalSinkFallback,
      DiagnosticFallback,
      SafeCleanup,
      PreservePrimaryException,
      PlatformFallback,
    };

    enum class ExceptionCarrierReason : std::uint8_t
    {
      CancellationTransport,
      CommandBoundary,
      PrivateErrorTransport,
      ForeignCallbackAdapter,
    };

    constexpr void acknowledgeAuditedCatch(AuditedCatchReason /*reason*/) noexcept
    {
    }
    constexpr void acknowledgeExceptionCarrier(ExceptionCarrierReason /*reason*/) noexcept
    {
    }
    constexpr void acknowledgeRawFatalBackend() noexcept
    {
    }

    [[noreturn]] void abortFatalDiagnostic(FatalCategory category,
                                           std::string_view condition,
                                           std::string_view context,
                                           bool contextTruncated,
                                           std::source_location location,
                                           bool useApplicationSink) noexcept;

    [[noreturn]] void abortFatal(FatalCategory category,
                                 std::string_view condition,
                                 std::source_location location) noexcept;

    [[noreturn]] void abortFatal(FatalCategory category,
                                 std::string_view condition,
                                 std::source_location location,
                                 std::string_view context) noexcept;

    template<typename... Args>
      requires(sizeof...(Args) > 0)
    [[noreturn]] void abortFatal(FatalCategory category,
                                 std::string_view condition,
                                 std::source_location location,
                                 std::format_string<std::type_identity_t<Args>...> format,
                                 Args&&... args) noexcept
    {
      auto context = std::array<char, kFatalContextCapacity>{};

      try
      {
        auto const result = std::format_to_n(context.begin(), context.size(), format, std::forward<Args>(args)...);
        auto const formattedSize = static_cast<std::size_t>(result.size);
        auto const storedSize = formattedSize < context.size() ? formattedSize : context.size();
        abortFatalDiagnostic(category,
                             condition,
                             std::string_view{context.data(), storedSize},
                             formattedSize > context.size(),
                             location,
                             true);
      }
      catch (...)
      {
        abortFatalDiagnostic(category, condition, "Fatal diagnostic context evaluation failed", false, location, true);
      }
    }

    template<std::size_t Size>
    [[noreturn]] void abortRealtime(FatalCategory category,
                                    std::string_view condition,
                                    std::source_location location,
                                    // Literal extent is the realtime static-context contract.
                                    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
                                    char const (&context)[Size]) noexcept
    {
      static_assert(Size > 0);
      abortFatalDiagnostic(category, condition, std::string_view{std::data(context), Size - 1}, false, location, false);
    }

    [[noreturn]] void abortFatalEvaluation(FatalCategory category,
                                           std::string_view condition,
                                           std::source_location location) noexcept;

    [[noreturn]] void abortRealtimeEvaluation(FatalCategory category,
                                              std::string_view condition,
                                              std::source_location location) noexcept;
  } // namespace detail
} // namespace ao

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): the AST policy requires the exact marker macro expansion.
#define AO_AUDITED_CATCH(reason) ::ao::detail::acknowledgeAuditedCatch(::ao::detail::AuditedCatchReason::reason)

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): the AST policy requires the exact marker macro expansion.
#define AO_EXCEPTION_CARRIER(reason)                                                                                   \
  ::ao::detail::acknowledgeExceptionCarrier(::ao::detail::ExceptionCarrierReason::reason)

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): the AST policy requires the exact marker macro expansion.
#define AO_RAW_FATAL_BACKEND() ::ao::detail::acknowledgeRawFatalBackend()

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): contract macros preserve expression text and lazy evaluation.
#define AO_DETAIL_FATAL_CONTRACT_AT(category, location, condition, ...)                                                \
  do /* NOLINT(cppcoreguidelines-avoid-do-while) */                                                                    \
  {                                                                                                                    \
    try                                                                                                                \
    {                                                                                                                  \
      if (!(condition)) [[unlikely]]                                                                                   \
      {                                                                                                                \
        ::ao::detail::abortFatal(category, #condition, (location)__VA_OPT__(, ) __VA_ARGS__);                          \
      }                                                                                                                \
    }                                                                                                                  \
    catch (...)                                                                                                        \
    {                                                                                                                  \
      ::ao::detail::abortFatalEvaluation(category, #condition, (location));                                            \
    }                                                                                                                  \
  }                                                                                                                    \
  while (false)

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): contract macros preserve expression text and lazy evaluation.
#define AO_DETAIL_FATAL_CONTRACT(category, condition, ...)                                                             \
  AO_DETAIL_FATAL_CONTRACT_AT(category, std::source_location::current(), condition __VA_OPT__(, ) __VA_ARGS__)

#define AO_EXPECTS(condition, ...)                                                                                     \
  AO_DETAIL_FATAL_CONTRACT(::ao::FatalCategory::Expects, condition __VA_OPT__(, ) __VA_ARGS__)

#define AO_ENSURES(condition, ...)                                                                                     \
  AO_DETAIL_FATAL_CONTRACT(::ao::FatalCategory::Ensures, condition __VA_OPT__(, ) __VA_ARGS__)

#define AO_INVARIANT(condition, ...)                                                                                   \
  AO_DETAIL_FATAL_CONTRACT(::ao::FatalCategory::Invariant, condition __VA_OPT__(, ) __VA_ARGS__)

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): helper-owned conditions must preserve the caller's location.
#define AO_EXPECTS_AT(location, condition, ...)                                                                        \
  AO_DETAIL_FATAL_CONTRACT_AT(::ao::FatalCategory::Expects, location, condition __VA_OPT__(, ) __VA_ARGS__)

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): helper-owned fatal branches must preserve the caller's location.
#define AO_FATAL_AT(location, ...)                                                                                     \
  do /* NOLINT(cppcoreguidelines-avoid-do-while) */                                                                    \
  {                                                                                                                    \
    try                                                                                                                \
    {                                                                                                                  \
      ::ao::detail::abortFatal(::ao::FatalCategory::Fatal, {}, (location)__VA_OPT__(, ) __VA_ARGS__);                  \
    }                                                                                                                  \
    catch (...)                                                                                                        \
    {                                                                                                                  \
      ::ao::detail::abortFatalEvaluation(::ao::FatalCategory::Fatal, {}, (location));                                  \
    }                                                                                                                  \
  }                                                                                                                    \
  while (false)

#define AO_FATAL(...) AO_FATAL_AT(std::source_location::current() __VA_OPT__(, ) __VA_ARGS__)

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): the macro preserves the owning catch call site.
#define AO_FATAL_EXCEPTION(exceptionPtr, context)                                                                      \
  do /* NOLINT(cppcoreguidelines-avoid-do-while) */                                                                    \
  {                                                                                                                    \
    try                                                                                                                \
    {                                                                                                                  \
      ::ao::fatalFromException((exceptionPtr), (context), std::source_location::current());                            \
    }                                                                                                                  \
    catch (...)                                                                                                        \
    {                                                                                                                  \
      ::ao::detail::abortFatalEvaluation(                                                                              \
        ::ao::FatalCategory::UnhandledException, {}, std::source_location::current());                                 \
    }                                                                                                                  \
  }                                                                                                                    \
  while (false)

#define AO_RT_INVARIANT(condition, staticContext)                                                                      \
  do /* NOLINT(cppcoreguidelines-avoid-do-while) */                                                                    \
  {                                                                                                                    \
    try                                                                                                                \
    {                                                                                                                  \
      if (!(condition)) [[unlikely]]                                                                                   \
      {                                                                                                                \
        ::ao::detail::abortRealtime(                                                                                   \
          ::ao::FatalCategory::RealtimeInvariant, #condition, std::source_location::current(), staticContext);         \
      }                                                                                                                \
    }                                                                                                                  \
    catch (...)                                                                                                        \
    {                                                                                                                  \
      ::ao::detail::abortRealtimeEvaluation(                                                                           \
        ::ao::FatalCategory::RealtimeInvariant, #condition, std::source_location::current());                          \
    }                                                                                                                  \
  }                                                                                                                    \
  while (false)

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): realtime callback helpers preserve their registration call site.
#define AO_RT_FATAL_EXCEPTION_AT(location, staticContext)                                                              \
  do /* NOLINT(cppcoreguidelines-avoid-do-while) */                                                                    \
  {                                                                                                                    \
    ::ao::detail::abortRealtime(::ao::FatalCategory::UnhandledException, {}, (location), staticContext);               \
  }                                                                                                                    \
  while (false)
