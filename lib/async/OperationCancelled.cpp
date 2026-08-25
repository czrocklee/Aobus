// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/async/OperationCancelled.h>

#include <ao/Contract.h>
// Preload the GCC TSan fence guard before Asio; this header is used for its preprocessing effect.
// NOLINTNEXTLINE(misc-include-cleaner)
#include <ao/async/detail/BoostAsioTsanPrelude.h>

#include <boost/asio/error.hpp>
#include <boost/system/system_error.hpp>

#include <exception>
#include <stop_token>

namespace ao::async
{
  bool isOperationCancelled(std::exception const& exception) noexcept
  {
    if (dynamic_cast<OperationCancelled const*>(&exception) != nullptr)
    {
      return true;
    }

    auto const* const systemError = dynamic_cast<boost::system::system_error const*>(&exception);
    return systemError != nullptr && systemError->code() == boost::asio::error::operation_aborted;
  }

  bool isOperationCancelled(std::exception_ptr const& exceptionPtr) noexcept
  {
    if (!exceptionPtr)
    {
      return false;
    }

    try
    {
      std::rethrow_exception(exceptionPtr);
    }
    catch (std::exception const& exception)
    {
      AO_AUDITED_CATCH(ExceptionClassifier);
      return isOperationCancelled(exception);
    }
    catch (...)
    {
      AO_AUDITED_CATCH(ExceptionClassifier);
      return false;
    }
  }

  [[noreturn]] void throwOperationCancelled()
  {
    AO_EXCEPTION_CARRIER(CancellationTransport);
    throw OperationCancelled{};
  }

  [[noreturn]] void rethrowException(std::exception_ptr const& exceptionPtr)
  {
    AO_EXPECTS(exceptionPtr, "Cannot rethrow an empty exception");

    if (isOperationCancelled(exceptionPtr))
    {
      throwOperationCancelled();
    }

    std::rethrow_exception(exceptionPtr);
  }

  void throwIfStopRequested(std::stop_token const stopToken)
  {
    if (stopToken.stop_requested())
    {
      throwOperationCancelled();
    }
  }

  void rethrowIfOperationCancelled(std::exception const& exception)
  {
    if (isOperationCancelled(exception))
    {
      throwOperationCancelled();
    }
  }

  void rethrowIfOperationCancelled()
  {
    if (isOperationCancelled(std::current_exception()))
    {
      throwOperationCancelled();
    }
  }
} // namespace ao::async
