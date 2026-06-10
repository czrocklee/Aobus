// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <cstdint>

// POSITIVE: FIX-TO: std::int32_t getForbiddenVal()
[[nodiscard]] std::int32_t getForbiddenVal()
{
  return 42;
}

// POSITIVE: FIX-TO: struct ForbiddenStruct
struct [[nodiscard]] ForbiddenStruct
{};

// POSITIVE: FIX-TO: class ForbiddenClass
class [[nodiscard]] ForbiddenClass
{};

// NEGATIVE
std::int32_t getConformingVal()
{
  return 42;
}

// NEGATIVE
struct ConformingStruct
{};

// --- RAII Tests ---

// POSITIVE: FIX-TO: class [[nodiscard]] MissingNodiscardRaii
class MissingNodiscardRaii
{
public:
  ~MissingNodiscardRaii() {}
  MissingNodiscardRaii(MissingNodiscardRaii const&) = delete;
};

// NEGATIVE
class [[nodiscard]] GoodRaii
{
public:
  ~GoodRaii() {}
  GoodRaii(GoodRaii const&) = delete;
};

// NEGATIVE
class NotActuallyRaii
{
public:
  ~NotActuallyRaii() {} // User-provided dtor
  // Copy ctor NOT deleted (implicit or defaulted)
};

// NEGATIVE
class AlsoNotRaii
{
public:
  // No user-provided dtor
  AlsoNotRaii(AlsoNotRaii const&) = delete;
};

// NEGATIVE
class SomeInternalImpl
{
public:
  ~SomeInternalImpl() {}
  SomeInternalImpl(SomeInternalImpl const&) = delete;
};

// NEGATIVE
class GlobalPlaybackService
{
public:
  ~GlobalPlaybackService() {}
  GlobalPlaybackService(GlobalPlaybackService const&) = delete;
};

// NEGATIVE
class MainWindow
{
public:
  ~MainWindow() {}
  MainWindow(MainWindow const&) = delete;
};

// POSITIVE: FIX-TO: class [[nodiscard]] SomeResourceToken
class SomeResourceToken
{
public:
  ~SomeResourceToken() {}
  SomeResourceToken(SomeResourceToken const&) = delete;
};

namespace ao::tag
{
  // POSITIVE: FIX-TO: class [[nodiscard]] TagFile
  class TagFile
  {
  public:
    ~TagFile() {}
    TagFile(TagFile const&) = delete;
  };
} // namespace ao::tag

// POSITIVE: FIX-TO: class [[nodiscard]] WriteTransaction
class WriteTransaction
{
public:
  ~WriteTransaction() {}
  WriteTransaction(WriteTransaction const&) = delete;
};

// POSITIVE: FIX-TO: [[deprecated]] std::int32_t testMultiAttr1()
[[nodiscard, deprecated]] std::int32_t testMultiAttr1()
{
  return 1;
}

// POSITIVE: FIX-TO: [[deprecated]] std::int32_t testMultiAttr2()
[[deprecated, nodiscard]] std::int32_t testMultiAttr2()
{
  return 2;
}
