// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <cstdint>
#include <future>
#include <memory>
#include <optional>

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
class [[nodiscard]] ExplicitlyDefaultedOwner
{
public:
  ~ExplicitlyDefaultedOwner();
  ExplicitlyDefaultedOwner(ExplicitlyDefaultedOwner const&) = delete;
};

inline ExplicitlyDefaultedOwner::~ExplicitlyDefaultedOwner() = default;

class [[nodiscard]] ComposedGuard
{
public:
  ~ComposedGuard() {}
  ComposedGuard(ComposedGuard const&) = delete;
};

// NEGATIVE: a defaulted destructor still owns cleanup through an RAII member.
class [[nodiscard]] ComposedTempFile
{
public:
  ~ComposedTempFile() = default;
  ComposedTempFile(ComposedTempFile const&) = delete;

private:
  ComposedGuard _guard;
};

class [[nodiscard]] ReadTransaction
{
public:
  ~ReadTransaction() = default;
  ReadTransaction(ReadTransaction const&) = delete;

private:
  ComposedGuard _guard;
};

// NEGATIVE: an inherited RAII base still owns cleanup for the derived value.
class [[nodiscard]] AnnotatedWriteTransaction : public ReadTransaction
{
public:
  AnnotatedWriteTransaction(AnnotatedWriteTransaction const&) = delete;
};

// POSITIVE: FIX-TO: class [[nodiscard]] DerivedWriteTransaction : public ReadTransaction
class DerivedWriteTransaction : public ReadTransaction
{
public:
  DerivedWriteTransaction(DerivedWriteTransaction const&) = delete;
};

// NEGATIVE: an abstract lifecycle interface cannot itself be constructed as a discarded value.
class DecoderSession
{
public:
  virtual ~DecoderSession() = default;
  DecoderSession(DecoderSession const&) = delete;
};

// POSITIVE: FIX-TO: class [[nodiscard]] TestDecoderSession : public DecoderSession
class TestDecoderSession : public DecoderSession
{
public:
  TestDecoderSession(TestDecoderSession const&) = delete;
};

// NEGATIVE: a raw pointer observes an RAII object but does not own its cleanup.
class BorrowedPointerGuard
{
public:
  ~BorrowedPointerGuard() = default;
  BorrowedPointerGuard(BorrowedPointerGuard const&) = delete;

private:
  ComposedGuard* _guard = nullptr;
};

class OwnedPayload
{};

// POSITIVE: FIX-TO: class [[nodiscard]] UniqueOwnerSession
class UniqueOwnerSession
{
public:
  ~UniqueOwnerSession() = default;
  UniqueOwnerSession(UniqueOwnerSession const&) = delete;

private:
  std::unique_ptr<OwnedPayload> _payloadPtr;
};

template<typename T>
// POSITIVE: FIX-TO: class [[nodiscard]] TaskFuture
class TaskFuture
{
public:
  TaskFuture(TaskFuture const&) = delete;

private:
  std::future<std::optional<T>> _future;
};

// POSITIVE: FIX-TO: class [[nodiscard]] ExplicitlyDefaultedTransaction
class ExplicitlyDefaultedTransaction
{
public:
  ~ExplicitlyDefaultedTransaction();
  ExplicitlyDefaultedTransaction(ExplicitlyDefaultedTransaction const&) = delete;
};

inline ExplicitlyDefaultedTransaction::~ExplicitlyDefaultedTransaction() = default;

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

namespace ao::media::file
{
  // POSITIVE: FIX-TO: class [[nodiscard]] File
  class File
  {
  public:
    ~File() {}
    File(File const&) = delete;
  };
} // namespace ao::media::file

namespace ao
{
  // NEGATIVE
  template<typename T = void>
  class [[nodiscard]] Result
  {};
} // namespace ao

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
