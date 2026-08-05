// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/winui/app/DestructiveLibraryRestart.h>

#include <ao/Error.h>

#include <exception>
#include <expected>
#include <format>
#include <functional>
#include <string_view>
#include <tuple>

namespace ao::winui
{
  namespace
  {
    Error missingOperation(std::string_view const name)
    {
      return Error{.code = Error::Code::InvalidState,
                   .message = std::format("Destructive restart operation '{}' is missing", name)};
    }

    Error exceptionError(std::string_view const what, std::exception const& error)
    {
      return Error{
        .code = Error::Code::InitFailed, .message = std::format("{} threw an exception: {}", what, error.what())};
    }

    Error unknownExceptionError(std::string_view const what)
    {
      return Error{.code = Error::Code::InitFailed, .message = std::format("{} threw an unknown exception", what)};
    }

    Result<> guarded(std::move_only_function<Result<>()>& operation, std::string_view const what)
    {
      try
      {
        return operation();
      }
      catch (std::exception const& error)
      {
        return std::unexpected{exceptionError(what, error)};
      }
      catch (...)
      {
        return std::unexpected{unknownExceptionError(what)};
      }
    }

    Result<> guarded(std::move_only_function<void()>& operation, std::string_view const what)
    {
      try
      {
        operation();
        return {};
      }
      catch (std::exception const& error)
      {
        return std::unexpected{exceptionError(what, error)};
      }
      catch (...)
      {
        return std::unexpected{unknownExceptionError(what)};
      }
    }

    void report(std::move_only_function<void(Error const&) noexcept>& reportFailure, Error const& error) noexcept
    {
      if (!reportFailure)
      {
        return;
      }

      reportFailure(error);
    }

    void exitProcess(std::move_only_function<void() noexcept>& exitOperation) noexcept
    {
      if (!exitOperation)
      {
        return;
      }

      exitOperation();
    }
  } // namespace

  DestructiveLibraryRestartOutcome executeDestructiveLibraryRestart(
    DestructiveLibraryRestartOperations operations) noexcept
  {
    if (!operations.releaseActiveGraph || !operations.launchSuccessor)
    {
      report(operations.reportLaunchFailure,
             missingOperation(!operations.releaseActiveGraph ? "releaseActiveGraph" : "launchSuccessor"));
      exitProcess(operations.exitProcess);
      return DestructiveLibraryRestartOutcome::LaunchFailed;
    }

    // Deliberately discarded: a failed release must not cost the successor.
    std::ignore = guarded(operations.releaseActiveGraph, "Active graph release");

    auto const launchedRes = guarded(operations.launchSuccessor, "Successor process launch");

    if (!launchedRes)
    {
      report(operations.reportLaunchFailure, launchedRes.error());
    }

    exitProcess(operations.exitProcess);
    return launchedRes ? DestructiveLibraryRestartOutcome::Launched : DestructiveLibraryRestartOutcome::LaunchFailed;
  }
} // namespace ao::winui
