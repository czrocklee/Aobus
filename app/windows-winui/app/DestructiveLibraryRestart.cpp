// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/winui/app/DestructiveLibraryRestart.h>

#include <ao/Contract.h>
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
      catch (...)
      {
        AO_FATAL_EXCEPTION(std::current_exception(), "destructive restart failure reporting");
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
      catch (...)
      {
        AO_FATAL_EXCEPTION(std::current_exception(), "destructive restart process exit");
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

    auto releaseExceptionPtr = std::exception_ptr{};
    try
    {
      operations.releaseActiveGraph();
    }
    catch (...)
    {
      // A dying parent must still attempt its successor before diagnosing an
      // unexpected teardown escape.
      releaseExceptionPtr = std::current_exception();
    }

    auto launchedRes = Result<>{};
    try
    {
      launchedRes = operations.launchSuccessor();
    }
    catch (...)
    {
      AO_FATAL_EXCEPTION(std::current_exception(), "destructive restart successor launch");
    }

    if (!launchedRes)
    {
      report(operations.reportLaunchFailure, launchedRes.error());
    }

    if (releaseExceptionPtr)
    {
      AO_FATAL_EXCEPTION(std::move(releaseExceptionPtr), "destructive restart active-graph release");
    }

    exitProcess(operations.exitProcess);
    return launchedRes ? DestructiveLibraryRestartOutcome::Launched : DestructiveLibraryRestartOutcome::LaunchFailed;
  }
} // namespace ao::winui
