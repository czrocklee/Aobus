// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>

static void testUseIfInit(std::int32_t cond)
{
  // POSITIVE
  std::int32_t const localX = cond * 2;

  // FIX-TO: if (std::int32_t const localX = cond * 2; localX > 10)
  if (localX > 10)
  {
    std::cout << localX;
  }

  // POSITIVE
  std::int32_t const localSwitch = cond + 1;

  // FIX-TO: switch (std::int32_t const localSwitch = cond + 1; localSwitch)
  switch (localSwitch)
  {
    case 1: break;
    default: break;
  }

  // NEGATIVE
  std::int32_t const usedAfter = cond * 3;

  if (usedAfter > 0)
  {
    std::cout << usedAfter;
  }

  std::cout << usedAfter;

  // NEGATIVE
  constexpr std::int32_t kConstVar = 42;

  if (kConstVar > 0)
  {
    std::cout << kConstVar;
  }

  // NEGATIVE
  if (std::int32_t const localY = cond * 2; localY > 10)
  {
    std::cout << localY;
  }

  // POSITIVE
  if (std::int32_t const localImplicit = cond * 2)
  {
    std::cout << localImplicit;
  }

  // NEGATIVE
  if (std::int32_t const localExplicit = cond * 2; localExplicit)
  {
    std::cout << localExplicit;
  }
}

namespace ao::media::file
{
  class File
  {
  public:
    File() {}
    virtual ~File() {}
    bool isValid() const { return true; }
  };
} // namespace ao::media::file

class [[nodiscard]] MockTransaction
{
public:
  MockTransaction() {}
  ~MockTransaction() {}
  bool ok() const { return true; }
};

static void testRaiiProtection(std::int32_t cond)
{
  std::mutex m;

  // NEGATIVE - lock_guard is RAII
  std::lock_guard guard(m);
  if (cond > 0)
  {
    (void)guard;
  }

  // NEGATIVE - media File is whitelisted
  ao::media::file::File mediaFile;
  if (mediaFile.isValid())
  {
    std::cout << "tag";
  }

  // NEGATIVE - Transaction suffix is RAII
  MockTransaction txn;
  if (txn.ok())
  {
    std::cout << "txn";
  }

  // NEGATIVE - Wrapper unique_ptr<File> is protected
  std::unique_ptr<ao::media::file::File> mediaFilePtr;
  if (mediaFilePtr.operator->() != nullptr)
  {
    std::cout << "ptr";
  }

  // NEGATIVE - Wrapper optional<Transaction> is protected
  std::optional<MockTransaction> txnOpt;
  if (txnOpt.has_value())
  {
    std::cout << "opt";
  }

  // NEGATIVE - Already in init-statement (don't flag or touch)
  if (MockTransaction txn2; txn2.ok())
  {
    std::cout << "txn2";
  }
}
