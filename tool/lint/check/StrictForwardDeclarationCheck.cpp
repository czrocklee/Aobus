// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "StrictForwardDeclarationCheck.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclBase.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Type.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/SourceManager.h>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

using clang::ast_matchers::MatchFinder;

namespace clang::tidy::aobus
{
  namespace
  {
    CXXRecordDecl const* getRecordDeclFromType(QualType qt);

    CXXRecordDecl const* handleSmartPtrType(QualType const& pointee)
    {
      if (auto const* tst = pointee->getAs<TemplateSpecializationType>(); tst != nullptr)
      {
        for (std::uint32_t i = 0; i < tst->template_arguments().size(); ++i)
        {
          if (auto const& argument = tst->template_arguments()[i]; argument.getKind() == TemplateArgument::Type)
          {
            if (auto const* crd = getRecordDeclFromType(argument.getAsType()); crd != nullptr)
            {
              return dyn_cast<CXXRecordDecl>(crd->getCanonicalDecl());
            }
          }
        }
      }

      if (auto const* rt = pointee->getAs<RecordType>(); rt != nullptr)
      {
        if (auto const* ctsd = dyn_cast<ClassTemplateSpecializationDecl>(rt->getDecl()); ctsd != nullptr)
        {
          auto const& args = ctsd->getTemplateArgs();

          for (std::uint32_t i = 0; i < args.size(); ++i)
          {
            if (args[i].getKind() == TemplateArgument::Type)
            {
              if (auto const* crd = getRecordDeclFromType(args[i].getAsType()); crd != nullptr)
              {
                return dyn_cast<CXXRecordDecl>(crd->getCanonicalDecl());
              }
            }
          }
        }
      }

      return nullptr;
    }

    CXXRecordDecl const* getRecordDeclFromType(QualType qt)
    {
      if (qt.isNull())
      {
        return nullptr;
      }

      auto pointee = qt.getNonReferenceType();

      if (auto const* pt = pointee->getAs<PointerType>(); pt != nullptr)
      {
        pointee = pt->getPointeeType();
      }

      std::string const typeName = pointee.getAsString();
      bool const isSmartPtr = typeName.contains("unique_ptr") || typeName.contains("shared_ptr") ||
                              typeName.contains("weak_ptr") || typeName.contains("RefPtr");

      if (isSmartPtr)
      {
        if (auto const* res = handleSmartPtrType(pointee); res != nullptr)
        {
          return res;
        }
      }

      if (auto const* rt = pointee->getAs<RecordType>(); rt != nullptr)
      {
        if (auto const* crd = dyn_cast<CXXRecordDecl>(rt->getDecl()); crd != nullptr)
        {
          return dyn_cast<CXXRecordDecl>(crd->getCanonicalDecl());
        }
      }

      return nullptr;
    }

    // RecursiveASTVisitor customization points intentionally shadow its CRTP defaults.
    struct FwdDeclVisitor : public RecursiveASTVisitor<FwdDeclVisitor>
    {
      std::set<CXXRecordDecl const*> strongRefs;
      std::set<FileID> strongFileIDs;
      std::vector<DeclaratorDecl const*> weakRefs;
      SourceManager* sm{};

      bool isHeader(SourceLocation loc) const
      {
        if (loc.isInvalid())
        {
          return false;
        }

        SourceLocation const spellLoc = sm->getSpellingLoc(loc);

        if (sm->isInSystemHeader(spellLoc))
        {
          return false;
        }

        StringRef const fileName = sm->getFilename(spellLoc);
        return fileName.ends_with(".h") || fileName.ends_with(".hpp");
      }

      // Record the file of a non-forward-declarable declaration as a strong dependency.
      void recordStrongFileID(SourceLocation loc)
      {
        if (loc.isInvalid())
        {
          return;
        }

        if (SourceLocation const spellLoc = sm->getSpellingLoc(loc); sm->isInSystemHeader(spellLoc))
        {
          return;
        }

        strongFileIDs.insert(sm->getFileID(sm->getExpansionLoc(loc)));
      }

      // Record a by-value type as a strong dependency. Handles both class/struct
      // types (via strongRefs) and enum types (via strongFileIDs).
      void recordStrongType(QualType qt)
      {
        if (auto const* crd = getRecordDeclFromType(qt); crd != nullptr)
        {
          strongRefs.insert(crd);
          return;
        }

        // Enum types used by value can never be forward-declared;
        // record their definition file so we suppress warnings for
        // any forward-declarable class that shares the same header.
        if (auto const* et = qt.getNonReferenceType()->getAs<EnumType>(); et != nullptr)
        {
          recordStrongFileID(et->getDecl()->getLocation());
        }
      }

      bool shouldVisitTemplateInstantiations() const { return false; }

      bool VisitFieldDecl(FieldDecl* decl)
      {
        if (!isHeader(decl->getLocation()))
        {
          return true;
        }

        if (QualType const qt = decl->getType();
            qt->isPointerType() || qt->isReferenceType() || qt.getAsString().contains("unique_ptr") ||
            qt.getAsString().contains("shared_ptr") || qt.getAsString().contains("RefPtr"))
        {
          weakRefs.push_back(decl);
        }
        else
        {
          recordStrongType(qt);
        }

        return true;
      }

      bool VisitParmVarDecl(ParmVarDecl* decl)
      {
        if (!isHeader(decl->getLocation()))
        {
          return true;
        }

        if (QualType const qt = decl->getType(); qt->isPointerType() || qt->isReferenceType())
        {
          weakRefs.push_back(decl);
        }
        else
        {
          recordStrongType(qt);
        }

        return true;
      }

      bool VisitFunctionDecl(FunctionDecl* decl)
      {
        if (!isHeader(decl->getLocation()))
        {
          return true;
        }

        if (QualType const qt = decl->getReturnType(); !qt->isPointerType() && !qt->isReferenceType())
        {
          recordStrongType(qt);
        }

        return true;
      }

      bool VisitCXXRecordDecl(CXXRecordDecl* decl)
      {
        if (decl->hasDefinition())
        {
          for (auto const& base : decl->bases())
          {
            if (auto const* crd = getRecordDeclFromType(base.getType()); crd != nullptr)
            {
              strongRefs.insert(crd);
            }
          }
        }

        return true;
      }

      bool VisitMemberExpr(MemberExpr* expr)
      {
        if (auto const* md = dyn_cast<CXXMethodDecl>(expr->getMemberDecl()); md != nullptr)
        {
          if (auto const* crd = md->getParent(); crd != nullptr)
          {
            strongRefs.insert(dyn_cast<CXXRecordDecl>(crd->getCanonicalDecl()));
          }
        }
        else if (auto const* fd = dyn_cast<FieldDecl>(expr->getMemberDecl()); fd != nullptr)
        {
          if (auto const* crd = dyn_cast<CXXRecordDecl>(fd->getParent()); crd != nullptr)
          {
            strongRefs.insert(dyn_cast<CXXRecordDecl>(crd->getCanonicalDecl()));
          }
        }

        return true;
      }

      bool VisitDeclRefExpr(DeclRefExpr* expr)
      {
        if (auto const* md = dyn_cast<CXXMethodDecl>(expr->getDecl()); md != nullptr)
        {
          if (auto const* crd = md->getParent(); crd != nullptr)
          {
            strongRefs.insert(dyn_cast<CXXRecordDecl>(crd->getCanonicalDecl()));
          }
        }

        // Track enum constant references (e.g., Transport::Idle).
        // The enum definition file is a non-forward-declarable dependency.
        if (auto const* ecd = dyn_cast<EnumConstantDecl>(expr->getDecl()); ecd != nullptr)
        {
          if (auto const* ed = dyn_cast<EnumDecl>(ecd->getDeclContext()); ed != nullptr)
          {
            recordStrongFileID(ed->getLocation());
          }
        }

        return true;
      }

      bool VisitCXXConstructExpr(CXXConstructExpr* expr)
      {
        if (auto const* md = expr->getConstructor(); md != nullptr)
        {
          if (auto const* crd = md->getParent(); crd != nullptr)
          {
            strongRefs.insert(dyn_cast<CXXRecordDecl>(crd->getCanonicalDecl()));
          }
        }

        return true;
      }

      bool VisitCXXDeleteExpr(CXXDeleteExpr* expr)
      {
        if (auto const* crd = getRecordDeclFromType(expr->getDestroyedType()); crd != nullptr)
        {
          strongRefs.insert(crd);
        }

        return true;
      }

      void populateStrongFileIDs()
      {
        for (auto const* strongCrd : strongRefs)
        {
          for (auto const* redecl : strongCrd->redecls())
          {
            if (redecl->getLocation().isValid() && redecl->isThisDeclarationADefinition())
            {
              strongFileIDs.insert(sm->getFileID(sm->getExpansionLoc(redecl->getLocation())));
            }
          }
        }
      }

      bool findDefinitionAndCheckSameFile(CXXRecordDecl const* crd,
                                          FileID refFile,
                                          FileID& outWeakDefFile,
                                          SourceManager& sourceManager) const
      {
        bool inSame = false;

        for (auto const* redecl : crd->redecls())
        {
          if (redecl->getLocation().isValid())
          {
            FileID const declFile = sourceManager.getFileID(sourceManager.getExpansionLoc(redecl->getLocation()));

            if (declFile == refFile)
            {
              inSame = true;
            }

            if (redecl->isThisDeclarationADefinition())
            {
              outWeakDefFile = declFile;
            }
          }
        }

        return inSame;
      }

      void processWeakRef(DeclaratorDecl const* weak,
                          StrictForwardDeclarationCheck* checker,
                          MatchFinder::MatchResult const& result) const
      {
        auto qt = QualType{};

        if (auto const* fd = dyn_cast<FieldDecl>(weak); fd != nullptr)
        {
          qt = fd->getType();
        }
        else if (auto const* vd = dyn_cast<VarDecl>(weak); vd != nullptr)
        {
          qt = vd->getType();
        }
        else if (auto const* pd = dyn_cast<ParmVarDecl>(weak); pd != nullptr)
        {
          qt = pd->getType();
        }

        if (qt.isNull())
        {
          return;
        }

        auto pointee = qt;

        if (pointee->isReferenceType())
        {
          pointee = pointee.getNonReferenceType();
        }

        if (pointee->isPointerType())
        {
          pointee = pointee->getPointeeType();
        }

        if (pointee->getAs<TypedefType>() != nullptr)
        {
          return;
        }

        if (auto const* crd = getRecordDeclFromType(qt); crd != nullptr)
        {
          if (crd->getLocation().isValid() && result.SourceManager->isInSystemHeader(crd->getLocation()))
          {
            return;
          }

          if (!result.SourceManager->isInMainFile(result.SourceManager->getExpansionLoc(weak->getLocation())))
          {
            return;
          }

          if (strongRefs.contains(crd))
          {
            return;
          }

          bool hasDeclInSameFile = false;
          FileID const refFile =
            result.SourceManager->getFileID(result.SourceManager->getExpansionLoc(weak->getLocation()));
          auto weakDefFile = FileID{};

          hasDeclInSameFile = findDefinitionAndCheckSameFile(crd, refFile, weakDefFile, *result.SourceManager);

          bool const sharesFileWithStrongDep = weakDefFile.isValid() && strongFileIDs.contains(weakDefFile);

          if (!hasDeclInSameFile && !sharesFileWithStrongDep)
          {
            auto diagBuilder =
              checker->diag(weak->getLocation(), "use forward declaration instead of including the header for %1 '%0'")
              << crd->getNameAsString() << crd->getKindName();

            // A declaration spelled inside a macro cannot take the marker
            // insertion; the FixIt would edit the macro definition.
            if (!weak->getBeginLoc().isMacroID())
            {
              diagBuilder << FixItHint::CreateInsertion(weak->getBeginLoc(), "/* forward declare */ ");
            }
          }
        }
      }
    };
  } // namespace

  void StrictForwardDeclarationCheck::registerMatchers(MatchFinder* finder)
  {
    using namespace clang::ast_matchers;

    finder->addMatcher(translationUnitDecl().bind("tu"), this);
  }

  void StrictForwardDeclarationCheck::check(MatchFinder::MatchResult const& result)
  {
    auto visitor = FwdDeclVisitor{};
    visitor.sm = result.SourceManager;
    visitor.TraverseAST(*result.Context);

    visitor.populateStrongFileIDs();

    for (auto const* weak : visitor.weakRefs)
    {
      visitor.processWeakRef(weak, this, result);
    }
  }
} // namespace clang::tidy::aobus
