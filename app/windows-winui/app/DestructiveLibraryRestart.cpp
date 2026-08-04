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

    void report(std::move_only_function<void(Error const&)>& reportFailure, Error const& error) noexcept
    {
      if (!reportFailure)
      {
        return;
      }

      try
      {
        reportFailure(error);
      }
      // NOLINTNEXTLINE(bugprone-empty-catch): Failure reporting is best-effort after the operation already failed.
      catch (...)
      {
        // The caller is already being told about a failure it cannot act on.
      }
    }

    void exitProcess(std::move_only_function<void()>& exitOperation) noexcept
    {
      if (!exitOperation)
      {
        return;
      }

      try
      {
        exitOperation();
      }
      // NOLINTNEXTLINE(bugprone-empty-catch): Process exit is the final teardown step and cannot propagate.
      catch (...)
      {
        // Exit is the last step either way.
      }
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

    auto const launched = guarded(operations.launchSuccessor, "Successor process launch");

    if (!launched)
    {
      report(operations.reportLaunchFailure, launched.error());
    }

    exitProcess(operations.exitProcess);
    return launched ? DestructiveLibraryRestartOutcome::Launched : DestructiveLibraryRestartOutcome::LaunchFailed;
  }
} // namespace ao::winui
