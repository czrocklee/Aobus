// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <cstddef>
#include <exception>
#include <new>
#include <stdexcept>

void mayThrow();

namespace ao::detail
{
  enum class AuditedCatchReason
  {
    DiagnosticFallback,
    FatalSinkFallback,
  };

  enum class ExceptionCarrierReason
  {
    CancellationTransport,
    PrivateErrorTransport,
  };

  constexpr void acknowledgeAuditedCatch(AuditedCatchReason /*reason*/) noexcept
  {
  }
  constexpr void acknowledgeExceptionCarrier(ExceptionCarrierReason /*reason*/) noexcept
  {
  }
} // namespace ao::detail

#define AO_AUDITED_CATCH(reason) ::ao::detail::acknowledgeAuditedCatch(::ao::detail::AuditedCatchReason::reason)

#define AO_EXCEPTION_CARRIER(reason)                                                                                   \
  ::ao::detail::acknowledgeExceptionCarrier(::ao::detail::ExceptionCarrierReason::reason)

void rawThrowIsRejected()
{
  // POSITIVE
  throw std::runtime_error{"this should fail"};
}

namespace ao::async
{
  [[noreturn]] void rethrowException(std::exception_ptr const& exceptionPtr);

  [[noreturn]] void throwOperationCancelled()
  {
    AO_EXCEPTION_CARRIER(CancellationTransport);
    // NEGATIVE
    throw std::runtime_error{"approved cancellation carrier"};
  }
}

namespace ao::audio::detail
{
  [[noreturn]] void throwDecoderError()
  {
    // POSITIVE
    throw std::runtime_error{"a formerly approved function name is not a whitelist"};
  }
}

namespace ao::lmdb
{
  [[noreturn]] void throwOnMutationError()
  {
    AO_EXCEPTION_CARRIER(PrivateErrorTransport);
    // NEGATIVE
    throw std::runtime_error{"approved transaction carrier"};
  }
}

namespace ao::yaml
{
  [[noreturn]] void throwBasicParseFailure()
  {
    AO_EXCEPTION_CARRIER(PrivateErrorTransport);
    // NEGATIVE
    throw std::runtime_error{"approved parser callback carrier"};
  }
}

namespace ao::gtk::platform
{
  [[noreturn]] void throwGioError()
  {
    AO_EXCEPTION_CARRIER(PrivateErrorTransport);
    // NEGATIVE
    throw std::runtime_error{"approved platform ABI carrier"};
  }
}

namespace ao
{
  [[noreturn]] void throwUnexpectedError()
  {
    AO_EXCEPTION_CARRIER(PrivateErrorTransport);
    // NEGATIVE
    throw std::runtime_error{"a locally marked helper does not depend on its name"};
  }
}

[[noreturn]] void directExceptionCarrierHelperIsRejected()
{
  ao::detail::acknowledgeExceptionCarrier(ao::detail::ExceptionCarrierReason::PrivateErrorTransport);
  // POSITIVE
  throw std::runtime_error{"the marker helper itself is not an exemption"};
}

[[noreturn]] void nestedExceptionCarrierMarkerIsRejected()
{
  if (true)
  {
    AO_EXCEPTION_CARRIER(PrivateErrorTransport);
  }

  // POSITIVE
  throw std::runtime_error{"the marker must be the carrier helper's first statement"};
}

void broadCatchThatContinuesIsRejected()
{
  try
  {
    mayThrow();
  }
  // POSITIVE
  catch (std::exception const&)
  {
  }
}

void badAllocCatchThatContinuesIsRejected()
{
  try
  {
    mayThrow();
  }
  // POSITIVE
  catch (std::bad_alloc const&)
  {
  }
}

namespace ao::rt
{
  class Log final
  {
  public:
    static bool submitFatal();
    static bool submitOrdinary();
  };

  bool Log::submitFatal()
  {
    try
    {
      mayThrow();
    }
    // NEGATIVE
    catch (...)
    {
      AO_AUDITED_CATCH(FatalSinkFallback);
      return false;
    }

    return true;
  }

  bool Log::submitOrdinary()
  {
    try
    {
      mayThrow();
    }
    // POSITIVE
    catch (...)
    {
      return false;
    }

    return true;
  }
} // namespace ao::rt

void locallyAuditedCatchIsAccepted()
{
  try
  {
    mayThrow();
  }
  // NEGATIVE
  catch (...)
  {
    AO_AUDITED_CATCH(DiagnosticFallback);
  }
}

void directAuditedCatchHelperIsRejected()
{
  try
  {
    mayThrow();
  }
  // POSITIVE
  catch (...)
  {
    ao::detail::acknowledgeAuditedCatch(ao::detail::AuditedCatchReason::DiagnosticFallback);
  }
}

void nestedAuditedCatchMarkerIsRejected()
{
  try
  {
    mayThrow();
  }
  // POSITIVE
  catch (...)
  {
    if (true)
    {
      AO_AUDITED_CATCH(DiagnosticFallback);
    }
  }
}

void exactAdapterCatchIsAccepted()
{
  try
  {
    mayThrow();
  }
  // NEGATIVE
  catch (std::runtime_error const&)
  {
  }
}

void cleanupAndRethrowIsAccepted()
{
  try
  {
    mayThrow();
  }
  // NEGATIVE
  catch (...)
  {
    throw;
  }
}

void currentExceptionOwnershipIsAccepted()
{
  std::exception_ptr deferredException;

  try
  {
    mayThrow();
  }
  // NEGATIVE
  catch (...)
  {
    deferredException = std::current_exception();
  }

  ao::async::rethrowException(deferredException);
}

std::exception_ptr currentExceptionLookalike();

void currentExceptionLookalikeIsRejected()
{
  std::exception_ptr deferredException;

  try
  {
    mayThrow();
  }
  // POSITIVE
  catch (...)
  {
    deferredException = currentExceptionLookalike();
  }
}

void nestedCurrentExceptionCaptureDoesNotTransferOuterOwnership()
{
  std::exception_ptr deferredException;

  try
  {
    mayThrow();
  }
  // POSITIVE
  catch (...)
  {
    auto captureLater = [&deferredException] { deferredException = std::current_exception(); };
    static_cast<void>(captureLater);
  }
}

namespace ao
{
  [[noreturn]] void fatalFromException();

  namespace detail
  {
    template<std::size_t Size>
    [[noreturn]] void abortRealtime(char const (&context)[Size]);
  }
} // namespace ao

void owningFatalCatchIsAccepted()
{
  try
  {
    mayThrow();
  }
  // NEGATIVE
  catch (...)
  {
    ao::fatalFromException();
  }
}

template<std::size_t Size>
void dependentFatalCatchIsAccepted(char const (&context)[Size])
{
  try
  {
    mayThrow();
  }
  // NEGATIVE
  catch (...)
  {
    ao::detail::abortRealtime(context);
  }
}

void stdExceptionCleanupAndRethrowIsAccepted()
{
  try
  {
    mayThrow();
  }
  // NEGATIVE
  catch (std::exception const&)
  {
    throw;
  }
}

void nestedCatchDoesNotTransferOuterOwnership()
{
  try
  {
    mayThrow();
  }
  // POSITIVE
  catch (...)
  {
    try
    {
      mayThrow();
    }
    catch (...)
    {
      throw;
    }
  }
}

void lambdaDoesNotTransferOuterOwnership()
{
  try
  {
    mayThrow();
  }
  // POSITIVE
  catch (...)
  {
    auto terminateLater = [] { ao::fatalFromException(); };
    static_cast<void>(terminateLater);
  }
}

void conditionalRethrowDoesNotTransferEveryPath(bool const shouldRethrow)
{
  try
  {
    mayThrow();
  }
  // POSITIVE
  catch (...)
  {
    if (shouldRethrow)
    {
      throw;
    }
  }
}

void allConditionalPathsTransferOrTerminate(bool const shouldRethrow)
{
  try
  {
    mayThrow();
  }
  // NEGATIVE
  catch (...)
  {
    if (shouldRethrow)
    {
      throw;
    }
    else
    {
      ao::fatalFromException();
    }
  }
}

void earlyReturnDoesNotReachLaterFatalOnEveryPath(bool const shouldReturn)
{
  try
  {
    mayThrow();
  }
  // POSITIVE
  catch (...)
  {
    if (shouldReturn)
    {
      return;
    }

    ao::fatalFromException();
  }
}

void declarationDefaultArgumentDoesNotCrashChecker(
  // POSITIVE
  int value = (throw std::runtime_error{"default argument carrier is not approved"}, 0));
