// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ThrowError.h"

#include "ReadFaultInjection.h"
#include "ResultError.h"
#include "TransactionFailure.h"
#include <ao/Contract.h>
#include <ao/Error.h>

#include <lmdb.h>

#include <cstdint>
#include <format>
#include <source_location>
#include <thread>

namespace ao::lmdb
{
  namespace
  {
    struct InjectedReadFailure final
    {
      std::int32_t code = MDB_SUCCESS;
      bool* consumed = nullptr;
    };

    InjectedReadFailure& injectedReadFailure() noexcept
    {
      thread_local auto state = InjectedReadFailure{};
      return state;
    }

    std::int32_t consumeInjectedReadFailure(std::int32_t const nativeCode) noexcept
    {
      auto& injection = injectedReadFailure();

      if (injection.code == MDB_SUCCESS)
      {
        return nativeCode;
      }

      auto const injectedCode = injection.code;
      auto* const consumed = injection.consumed;
      injection = {};
      *consumed = true;
      return nativeCode == MDB_SUCCESS && injectedCode != MDB_SUCCESS ? injectedCode : nativeCode;
    }
  } // namespace

  namespace detail
  {
    ReadFaultInjection::ReadFaultInjection(std::int32_t const code)
      : _ownerThreadId{std::this_thread::get_id()}
    {
      auto& injection = injectedReadFailure();
      AO_EXPECTS(code != MDB_SUCCESS, "Injected LMDB read failure must not be success");
      AO_EXPECTS(injection.code == MDB_SUCCESS, "An injected LMDB read failure is already pending");
      injection = {.code = code, .consumed = &_consumed};
    }

    ReadFaultInjection::~ReadFaultInjection()
    {
      AO_INVARIANT(std::this_thread::get_id() == _ownerThreadId, "LMDB read-fault injection left its owning thread");

      if (!_consumed)
      {
        auto& injection = injectedReadFailure();
        AO_INVARIANT(injection.consumed == &_consumed, "LMDB read-fault injection ownership was replaced");
        injection = {};
        AO_FATAL("Injected LMDB read failure was not consumed");
      }
    }
  } // namespace detail

  void failRead(char const* origin, std::int32_t const nativeCode, bool const transactionOwned)
  {
    auto const code = consumeInjectedReadFailure(nativeCode);

    if (code == MDB_SUCCESS)
    {
      return;
    }

    if (transactionOwned)
    {
      throwOnMutationError(origin, code);
    }

    AO_FATAL("{}: {}", origin, ::mdb_strerror(code));
  }

  [[noreturn]] void throwOnMutationError(char const* origin, std::int32_t code, std::source_location location)
  {
    AO_EXCEPTION_CARRIER(PrivateErrorTransport);
    throw detail::TransactionFailure{Error{.code = errorCodeFor(code),
                                           .message = std::format("{}: {}", origin, ::mdb_strerror(code)),
                                           .location = location}};
  }
} // namespace ao::lmdb
