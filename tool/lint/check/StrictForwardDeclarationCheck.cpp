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
#include <clang/AST/TypeLoc.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/FileEntry.h>
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

    TemplateDecl const* getTemplateDeclFromType(QualType qt)
    {
      if (qt.isNull())
      {
        return nullptr;
      }

      auto const type = qt.getNonReferenceType();

      if (auto const* specialization = type->getAs<TemplateSpecializationType>(); specialization != nullptr)
      {
        return specialization->getTemplateName().getAsTemplateDecl();
      }

      if (auto const* record = type->getAs<RecordType>(); record != nullptr)
      {
        if (auto const* specialization = dyn_cast<ClassTemplateSpecializationDecl>(record->getDecl());
            specialization != nullptr)
        {
          return specialization->getSpecializedTemplate();
        }
      }

      return nullptr;
    }

    bool hasTemplateArgumentsByValue(QualType qt)
    {
      auto const* templateDecl = getTemplateDeclFromType(qt);

      if (templateDecl == nullptr)
      {
        return false;
      }

      auto const name = templateDecl->getQualifiedNameAsString();
      return name == "std::optional" || name == "std::pair" || name == "std::variant";
    }

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

      auto const* record = pointee->getAs<RecordType>();

      if (record == nullptr && pointee->getAs<TemplateSpecializationType>() == nullptr)
      {
        return nullptr;
      }

      if (std::string const typeName = pointee.getAsString();
          typeName.contains("unique_ptr") || typeName.contains("shared_ptr") || typeName.contains("weak_ptr") ||
          typeName.contains("RefPtr"))
      {
        if (auto const* res = handleSmartPtrType(pointee); res != nullptr)
        {
          return res;
        }
      }

      if (record != nullptr)
      {
        if (auto const* crd = dyn_cast<CXXRecordDecl>(record->getDecl()); crd != nullptr)
        {
          return dyn_cast<CXXRecordDecl>(crd->getCanonicalDecl());
        }
      }

      return nullptr;
    }

    bool isHeaderName(StringRef name)
    {
      return name.ends_with(".h") || name.ends_with(".hpp");
    }

    // A diagnostic needs both a header spelling and a main-file expansion. A
    // plain implementation file can only supply that combination through a
    // macro. Check source provenance, not the shapes seen by MatchFinder: it
    // can skip a system declaration that encloses user-written parameters.
    bool canHaveWeakRefs(SourceManager const& sm)
    {
      // The native clang-tidy frontend can install an idle ASTReader. Actual
      // serialized inputs reserve source-location entries before the TU match.
      if (sm.loaded_sloc_entry_size() != 0 || isHeaderName(sm.getFilename(sm.getLocForStartOfFile(sm.getMainFileID()))))
      {
        return true;
      }

      for (std::uint32_t i = 1; i < sm.local_sloc_entry_size(); ++i)
      {
        if (auto const& entry = sm.getLocalSLocEntry(i); entry.isFile())
        {
          // isInMainFile can also accept a detached file with no includer.
          auto const& file = entry.getFile();

          if (auto const& original = file.getContentCache().OrigEntry;
              file.getIncludeLoc().isInvalid() && original && isHeaderName(original->getName()))
          {
            return true;
          }
        }
        else
        {
          // Nested expansions lead to an outer entry at a physical location.
          auto const start = entry.getExpansion().getExpansionLocStart();

          if (start.isValid() && start.isFileID() && sm.isInMainFile(start))
          {
            return true;
          }
        }
      }

      return false;
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

        return isHeaderName(sm->getFilename(spellLoc));
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
        auto const* crd = getRecordDeclFromType(qt);

        if (crd != nullptr)
        {
          strongRefs.insert(crd);
        }

        // These wrappers embed their type arguments in their value
        // representation, so a by-value wrapper use also requires those
        // argument definitions.
        if (hasTemplateArgumentsByValue(qt))
        {
          auto const recordArgument = [this](TemplateArgument const& argument)
          {
            if (argument.getKind() != TemplateArgument::Type)
            {
              return;
            }

            if (auto const argumentType = argument.getAsType();
                !argumentType->isPointerType() && !argumentType->isReferenceType())
            {
              recordStrongType(argumentType);
            }
          };

          auto const type = qt.getNonReferenceType();

          if (auto const* typeSpecialization = type->getAs<TemplateSpecializationType>(); typeSpecialization != nullptr)
          {
            for (auto const& argument : typeSpecialization->template_arguments())
            {
              recordArgument(argument);
            }
          }
          else if (auto const* record = type->getAs<RecordType>(); record != nullptr)
          {
            if (auto const* recordSpecialization = dyn_cast<ClassTemplateSpecializationDecl>(record->getDecl());
                recordSpecialization != nullptr)
            {
              for (auto const& argument : recordSpecialization->getTemplateArgs().asArray())
              {
                recordArgument(argument);
              }
            }
          }
        }

        if (crd != nullptr)
        {
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

      void recordTypeAliasDependency(SourceLocation useLoc, SourceLocation declLoc)
      {
        // Alias names cannot be forward-declared. Only uses in the selected file
        // require their provider; a provider's own alias use must not retain it.
        if (useLoc.isValid() && sm->isInMainFile(sm->getExpansionLoc(useLoc)))
        {
          recordStrongFileID(declLoc);
        }
      }

      bool VisitTypedefTypeLoc(TypedefTypeLoc typeLoc)
      {
        recordTypeAliasDependency(typeLoc.getBeginLoc(), typeLoc.getTypePtr()->getDecl()->getLocation());
        return true;
      }

      bool VisitTemplateSpecializationTypeLoc(TemplateSpecializationTypeLoc typeLoc)
      {
        if (auto const* alias =
              dyn_cast_or_null<TypeAliasTemplateDecl>(typeLoc.getTypePtr()->getTemplateName().getAsTemplateDecl());
            alias != nullptr)
        {
          recordTypeAliasDependency(typeLoc.getBeginLoc(), alias->getLocation());
        }

        return true;
      }

      void recordWeakRef(DeclaratorDecl const* decl)
      {
        if (sm->isInMainFile(sm->getExpansionLoc(decl->getLocation())))
        {
          weakRefs.push_back(decl);
        }
      }

      bool VisitFieldDecl(FieldDecl* decl)
      {
        if (!isHeader(decl->getLocation()))
        {
          return true;
        }

        QualType const qt = decl->getType();

        if (qt->isPointerType() || qt->isReferenceType())
        {
          recordWeakRef(decl);
          return true;
        }

        if (auto const name = qt.getAsString();
            name.contains("unique_ptr") || name.contains("shared_ptr") || name.contains("RefPtr"))
        {
          recordWeakRef(decl);
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
          recordWeakRef(decl);
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

      bool tryFindSameFileDeclarationAndDefinition(CXXRecordDecl const* crd,
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
        QualType const qt = weak->getType();

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

          if (strongRefs.contains(crd))
          {
            return;
          }

          bool hasDeclInSameFile = false;
          FileID const refFile =
            result.SourceManager->getFileID(result.SourceManager->getExpansionLoc(weak->getLocation()));
          auto weakDefFile = FileID{};

          hasDeclInSameFile = tryFindSameFileDeclarationAndDefinition(crd, refFile, weakDefFile, *result.SourceManager);

          bool const sharesFileWithStrongDep = weakDefFile.isValid() && strongFileIDs.contains(weakDefFile);

          if (!hasDeclInSameFile && !sharesFileWithStrongDep)
          {
            checker->diag(weak->getLocation(), "use forward declaration instead of including the header for %1 '%0'")
              << crd->getNameAsString() << crd->getKindName();
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
    if (!canHaveWeakRefs(*result.SourceManager))
    {
      return;
    }

    auto visitor = FwdDeclVisitor{};
    visitor.sm = result.SourceManager;
    visitor.TraverseAST(*result.Context);

    if (visitor.weakRefs.empty())
    {
      return;
    }

    visitor.populateStrongFileIDs();

    for (auto const* weak : visitor.weakRefs)
    {
      visitor.processWeakRef(weak, this, result);
    }
  }
} // namespace clang::tidy::aobus
