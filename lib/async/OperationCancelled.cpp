// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/async/OperationCancelled.h>

#include <boost/asio/error.hpp>
#include <boost/system/system_error.hpp>

#include <exception>

namespace ao::async
{
  bool isOperationCancelled(std::exception const& exception)
  {
    if (dynamic_cast<OperationCancelled const*>(&exception) != nullptr)
    {
      return true;
    }

    auto const* const systemError = dynamic_cast<boost::system::system_error const*>(&exception);
    return systemError != nullptr && systemError->code() == boost::asio::error::operation_aborted;
  }

  [[noreturn]] void throwOperationCancelled()
  {
    throw OperationCancelled{};
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
    auto const exceptionPtr = std::current_exception();

    if (!exceptionPtr)
    {
      return;
    }

    try
    {
      std::rethrow_exception(exceptionPtr);
    }
    catch (std::exception const& e)
    {
      rethrowIfOperationCancelled(e);
    }
    catch (...)
    {
      // Non-std exceptions cannot be classified as OperationCancelled.
      return;
    }
  }
} // namespace ao::async
