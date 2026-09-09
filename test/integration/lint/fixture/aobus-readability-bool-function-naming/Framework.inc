// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma clang system_header

class BoolFramework
{
public:
  virtual bool process() const = 0;
};

namespace clang
{
  class Decl
  {};

  class Stmt
  {};

  template<typename Derived>
  class RecursiveASTVisitor
  {
  public:
    bool VisitDecl(Decl* declaration);
    bool TraverseDecl(Decl* declaration);
    bool WalkUpFromDecl(Decl* declaration);
    bool dataTraverseStmtPre(Stmt* statement);
    bool dataTraverseStmtPost(Stmt* statement);
  };
} // namespace clang

namespace winrt
{
  namespace Windows::Foundation::Collections
  {
    template<typename T>
    class IIterator
    {};

    template<typename T>
    class IVectorView
    {};
  } // namespace Windows::Foundation::Collections

  template<typename Derived, typename... Interfaces>
  class implements
  {};
} // namespace winrt
