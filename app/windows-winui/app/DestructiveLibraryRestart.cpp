// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/winui/app/DestructiveLibraryRestart.h>

#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/compat/MoveOnlyFunction.h>

#include <exception>
#include <format>
#include <string_view>
#include <utility>

namespace ao::winui
{
  namespace
  {
    Error missingOperation(std::string_view const name)
    {
      return Error{.code = Error::Code::InvalidState,
                   .message = std::format("Destructive restart operation '{}' is missing", name)};
    }

    std::string_view missingOperationName(DestructiveLibraryRestartOperations const& operations)
    {
      if (!operations.prepareActiveGraph)
      {
        return "prepareActiveGraph";
      }

      if (!operations.releaseActiveGraph)
      {
        return "releaseActiveGraph";
      }

      if (!operations.launchSuccessor)
      {
        return "launchSuccessor";
      }

      return {};
    }

    void report(compat::MoveOnlyFunction<void(Error const&)>& reportFailure, Error const& error) noexcept
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

    void exitProcess(compat::MoveOnlyFunction<void()>& exitOperation) noexcept
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
    if (auto const missingName = missingOperationName(operations); !missingName.empty())
    {
      report(operations.reportLaunchFailure, missingOperation(missingName));
      exitProcess(operations.exitProcess);
      return DestructiveLibraryRestartOutcome::LaunchFailed;
    }

    auto preparedRes = Result<>{};

    try
    {
      preparedRes = operations.prepareActiveGraph();
    }
    catch (...)
    {
      AO_FATAL_EXCEPTION(std::current_exception(), "destructive restart active-graph preparation");
    }

    if (!preparedRes)
    {
      report(operations.reportPreparationFailure, preparedRes.error());
      return DestructiveLibraryRestartOutcome::PreparationFailed;
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
