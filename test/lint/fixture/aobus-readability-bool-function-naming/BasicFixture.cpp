// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "Framework.inc"
#include "GeneratedProjection.g.h"
#include <ao/Error.h>

// This check only distinguishes direct bool from a wrapped return type.
namespace ao::async
{
  template<typename T>
  class Task;
}

#define DECLARE_BOOL(Name) bool Name();

// POSITIVE
DECLARE_BOOL(macroPredicate)

// NEGATIVE
DECLARE_BOOL(isMacroPredicate)

// NEGATIVE
bool isReady();
// NEGATIVE
bool hasItems();
// NEGATIVE
bool canOpen();
// NEGATIVE
bool shouldRetry();
// NEGATIVE
bool supportsSeeking();
// NEGATIVE
bool needsRefresh();
// NEGATIVE
bool matchesTrack();
// NEGATIVE
bool acceptsValue();
// NEGATIVE
bool tryOpen();

// NEGATIVE
bool empty();
// NEGATIVE
bool contains(int value);
// NEGATIVE
bool containsValue(int value);
// NEGATIVE
bool startsWithText();
// NEGATIVE
bool endsWithText();
// NEGATIVE
bool compare_exchange_strong();
// NEGATIVE
bool compare_exchange_weak();

// POSITIVE
bool ready();
// POSITIVE
bool handleEvent();
// POSITIVE
bool open();
// POSITIVE
bool filter();
// POSITIVE
bool history();
// POSITIVE
bool MoveNext();

// POSITIVE
bool on_save();

// POSITIVE
bool property_state();

// NEGATIVE
int isCount();

// NEGATIVE
ao::async::Task<bool> readyLater();

// NEGATIVE
ao::Result<bool> readyResult();

// POSITIVE
auto deducedReady()
{
  return true;
}

template<typename T>
// POSITIVE
auto templateReady(T const& value)
{
  return true;
}

template<typename T>
// NEGATIVE
auto isTemplateReady(T const& value)
{
  return true;
}

template<typename T>
class TemplatePredicate
{
public:
  // POSITIVE
  auto evaluate(T const& value) { return true; }

  // NEGATIVE
  auto isValid(T const& value) { return true; }
};

template<typename T>
// NEGATIVE
auto passThroughValue(T value)
{
  return value;
}

void instantiateBoolTemplates()
{
  [[maybe_unused]] bool intReady = templateReady(1);
  [[maybe_unused]] bool doubleReady = templateReady(2.0);
  [[maybe_unused]] bool compliantReady = isTemplateReady(1);
  [[maybe_unused]] bool intEvaluation = TemplatePredicate<int>{}.evaluate(1);
  [[maybe_unused]] bool doubleEvaluation = TemplatePredicate<double>{}.evaluate(2.0);
  [[maybe_unused]] bool valid = TemplatePredicate<int>{}.isValid(1);
  [[maybe_unused]] bool forwardedBool = passThroughValue(true);
  [[maybe_unused]] int forwardedInt = passThroughValue(1);
}

// NEGATIVE
auto predicateLambda = [] { return true; };

class ProjectBase
{
public:
  // POSITIVE
  virtual bool process() const = 0;
};

class ProjectDerived final : public ProjectBase
{
public:
  // POSITIVE
  bool process() const override { return true; }
};

class ForeignDerived : public BoolFramework
{
public:
  // NEGATIVE
  bool process() const override { return true; }

  // POSITIVE
  bool on_save() const { return true; }

  // POSITIVE
  bool rebuildCache() const { return true; }
};

class TransitiveForeignOverride final : public ForeignDerived
{
public:
  // NEGATIVE
  bool process() const override { return true; }
};

class Visitor final : public clang::RecursiveASTVisitor<Visitor>
{
public:
  // NEGATIVE
  bool VisitDecl(clang::Decl* declaration) { return declaration != nullptr; }

  // NEGATIVE
  bool TraverseDecl(clang::Decl* declaration) { return declaration != nullptr; }

  // NEGATIVE
  bool WalkUpFromDecl(clang::Decl* declaration) { return declaration != nullptr; }

  // NEGATIVE
  bool dataTraverseStmtPre(clang::Stmt* statement) { return statement != nullptr; }

  // NEGATIVE
  bool dataTraverseStmtPost(clang::Stmt* statement) { return statement != nullptr; }

  // POSITIVE
  bool VisitCache(int value) { return value != 0; }

  // POSITIVE
  bool TraverseCache(int value) { return value != 0; }

  // POSITIVE
  bool rebuildCache() const { return true; }
};

template<typename T>
class TemplateVisitor final : public clang::RecursiveASTVisitor<TemplateVisitor<T>>
{
public:
  // NEGATIVE
  bool VisitDecl(clang::Decl* declaration) { return declaration != nullptr; }

  // NEGATIVE
  bool TraverseDecl(clang::Decl* declaration) { return declaration != nullptr; }

  // NEGATIVE
  bool WalkUpFromDecl(clang::Decl* declaration) { return declaration != nullptr; }

  // NEGATIVE
  bool dataTraverseStmtPre(clang::Stmt* statement) { return statement != nullptr; }

  // NEGATIVE
  bool dataTraverseStmtPost(clang::Stmt* statement) { return statement != nullptr; }

  // POSITIVE
  bool VisitDecl(int value) { return value != 0; }

  // POSITIVE
  bool rebuildCache() const { return true; }
};

template<typename T>
class UninstantiatedVisitor final : public clang::RecursiveASTVisitor<UninstantiatedVisitor<T>>
{
public:
  // NEGATIVE
  bool VisitDecl(clang::Decl* declaration) { return declaration != nullptr; }

  // NEGATIVE
  bool dataTraverseStmtPre(clang::Stmt* statement) { return statement != nullptr; }

  // POSITIVE
  bool VisitCache(int value) { return value != 0; }
};

void instantiateVisitors()
{
  [[maybe_unused]] auto first = TemplateVisitor<int>{};
  [[maybe_unused]] auto second = TemplateVisitor<double>{};
}

class Projected final : public GeneratedProjection<Projected>
{
public:
  // POSITIVE
  bool Enabled() const { return true; }

  // NEGATIVE
  bool IsGroupHeader() const { return true; }

  // POSITIVE
  bool Sortable() const { return true; }
};

class GtkBindingShape final
{
public:
  // POSITIVE
  bool on_event() const { return true; }
};

using Iterator = winrt::Windows::Foundation::Collections::IIterator<int>;
using VectorView = winrt::Windows::Foundation::Collections::IVectorView<int>;

class WinrtIterator final : public winrt::implements<WinrtIterator, Iterator>
{
public:
  // NEGATIVE
  bool HasCurrent() const { return true; }

  // NEGATIVE
  bool MoveNext() { return true; }

  // POSITIVE
  bool MoveNext(int count) { return count != 0; }

  // POSITIVE
  bool rebuildCache() const { return true; }
};

struct UnrelatedElement
{};

class WinrtVectorView final : public winrt::implements<WinrtVectorView, VectorView>
{
public:
  // NEGATIVE
  bool IndexOf(int value, unsigned int& index) const { return value == static_cast<int>(index); }

  // POSITIVE
  bool IndexOf(int value, int& index) const { return value == index; }

  // POSITIVE
  bool IndexOf(UnrelatedElement, unsigned int&) const { return true; }

  // POSITIVE
  bool IndexOf(UnrelatedElement const&, unsigned int&) { return true; }
};

using ElementView = winrt::Windows::Foundation::Collections::IVectorView<UnrelatedElement>;

class WinrtElementView final : public winrt::implements<WinrtElementView, Iterator, ElementView>
{
public:
  // NEGATIVE
  bool IndexOf(UnrelatedElement const&, unsigned int&) const { return true; }

  // POSITIVE
  bool IndexOf(int, unsigned int&) const { return true; }
};

class WinrtLookalike final : public winrt::implements<WinrtLookalike>
{
public:
  // POSITIVE
  bool MoveNext() { return true; }
};
