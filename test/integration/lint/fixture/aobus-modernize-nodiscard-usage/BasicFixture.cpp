// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <cstdint>
#include <future>
#include <memory>
#include <optional>

// POSITIVE
[[nodiscard]] std::int32_t getForbiddenVal()
{
  return 42;
}

// POSITIVE
struct [[nodiscard]] ForbiddenStruct
{};

// POSITIVE
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

// POSITIVE
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

// POSITIVE
class ScopedRegistration
{
public:
  ~ScopedRegistration() {}
  ScopedRegistration(ScopedRegistration const&) = delete;
};

// NEGATIVE
class Registration
{};

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

// POSITIVE
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

// POSITIVE
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

// POSITIVE
class UniqueOwnerSession
{
public:
  ~UniqueOwnerSession() = default;
  UniqueOwnerSession(UniqueOwnerSession const&) = delete;

private:
  std::unique_ptr<OwnedPayload> _payloadPtr;
};

template<typename T>
// POSITIVE
class TaskFuture
{
public:
  TaskFuture(TaskFuture const&) = delete;

private:
  std::future<std::optional<T>> _future;
};

// POSITIVE
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
class GlobalPlaybackTransport
{
public:
  ~GlobalPlaybackTransport() {}
  GlobalPlaybackTransport(GlobalPlaybackTransport const&) = delete;
};

// NEGATIVE
class MainWindow
{
public:
  ~MainWindow() {}
  MainWindow(MainWindow const&) = delete;
};

// POSITIVE
class SomeResourceToken
{
public:
  ~SomeResourceToken() {}
  SomeResourceToken(SomeResourceToken const&) = delete;
};

namespace ao::media::file
{
  // POSITIVE
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

// POSITIVE
class WriteTransaction
{
public:
  ~WriteTransaction() {}
  WriteTransaction(WriteTransaction const&) = delete;
};

// POSITIVE
[[nodiscard, deprecated]] std::int32_t testMultiAttr1()
{
  return 1;
}

// POSITIVE
[[deprecated, nodiscard]] std::int32_t testMultiAttr2()
{
  return 2;
}
