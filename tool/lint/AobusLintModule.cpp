// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "check/AsyncCancellationGuardCheck.h"
#include "check/BracedInitializationCheck.h"
#include "check/CApiGlobalQualificationCheck.h"
#include "check/ChronoNamingConventionCheck.h"
#include "check/ConcreteFinalCheck.h"
#include "check/ControlBlockSpacingCheck.h"
#include "check/ForbidRawThrowCheck.h"
#include "check/ForbidTrailingReturnCheck.h"
#include "check/IdentifierNamingExtensionsCheck.h"
#include "check/ImplicitBoolConversionInInitCheck.h"
#include "check/IncludeConventionCheck.h"
#include "check/LambdaParamsCheck.h"
#include "check/LocalInitializationStyleCheck.h"
#include "check/MemberOrderCheck.h"
#include "check/NodiscardUsageCheck.h"
#include "check/OptionalNamingAndUsageCheck.h"
#include "check/PointerNamingConventionCheck.h"
#include "check/RedundantNamespaceQualificationCheck.h"
#include "check/RedundantUsingDirectiveCheck.h"
#include "check/SpdxLicenseHeaderCheck.h"
#include "check/StdCLibraryQualificationCheck.h"
#include "check/StrictForwardDeclarationCheck.h"
#include "check/ThreadingPolicyCheck.h"
#include "check/UnusedSuppressionStyleCheck.h"
#include "check/UseCtadCheck.h"
#include "check/UseEraseIfCheck.h"
#include "check/UseIfInitStatementCheck.h"
#include "check/UseRangesAnyOfCheck.h"
#include "check/UseRangesContainsCheck.h"
#include "check/UseRangesMinMaxCheck.h"
#include "check/UseRangesProjectionCheck.h"
#include "check/UseStartsWithCheck.h"
#include "check/UseStdNumbersCheck.h"
#include "check/UseStdToArrayCheck.h"

#include <clang-tidy/ClangTidyModule.h>
#include <llvm/Config/llvm-config.h>

#if LLVM_VERSION_MAJOR < 22
#include <clang-tidy/ClangTidyModuleRegistry.h>
#endif

namespace clang::tidy::readability
{
  namespace
  {
    class AobusLintModule final : public ClangTidyModule
    {
    public:
      void addCheckFactories(ClangTidyCheckFactories& checkFactories) override
      {
        checkFactories.registerCheck<CApiGlobalQualificationCheck>("aobus-readability-c-api-global-qualification");
        checkFactories.registerCheck<AsyncCancellationGuardCheck>("aobus-async-cancellation-guard");
        checkFactories.registerCheck<ConcreteFinalCheck>("aobus-modernize-concrete-final");
        checkFactories.registerCheck<ControlBlockSpacingCheck>("aobus-readability-control-block-spacing");
        checkFactories.registerCheck<modernize::NodiscardUsageCheck>("aobus-modernize-nodiscard-usage");
        checkFactories.registerCheck<ForbidTrailingReturnCheck>("aobus-modernize-forbid-trailing-return");
        checkFactories.registerCheck<ForbidRawThrowCheck>("aobus-readability-forbid-raw-throw");
        checkFactories.registerCheck<IdentifierNamingExtensionsCheck>("aobus-readability-identifier-naming-extensions");
        checkFactories.registerCheck<aobus::ImplicitBoolConversionInInitCheck>(
          "aobus-implicit-bool-conversion-in-init");
        checkFactories.registerCheck<LambdaParamsCheck>("aobus-modernize-lambda-params");
        checkFactories.registerCheck<PointerNamingConventionCheck>("aobus-readability-pointer-naming-convention");
        checkFactories.registerCheck<ChronoNamingConventionCheck>("aobus-readability-chrono-naming-convention");
        checkFactories.registerCheck<LocalInitializationStyleCheck>("aobus-modernize-local-initialization-style");
        checkFactories.registerCheck<BracedInitializationCheck>("aobus-modernize-braced-initialization");
        checkFactories.registerCheck<MemberOrderCheck>("aobus-readability-member-order");
        checkFactories.registerCheck<OptionalNamingAndUsageCheck>("aobus-readability-optional-naming-and-usage");
        checkFactories.registerCheck<RedundantNamespaceQualificationCheck>(
          "aobus-readability-redundant-namespace-qualification");
        checkFactories.registerCheck<RedundantUsingDirectiveCheck>("aobus-readability-redundant-using-directive");
        checkFactories.registerCheck<StdCLibraryQualificationCheck>("aobus-readability-std-c-library-qualification");
        checkFactories.registerCheck<ThreadingPolicyCheck>("aobus-threading-policy");
        checkFactories.registerCheck<UnusedSuppressionStyleCheck>("aobus-readability-unused-suppression-style");
        checkFactories.registerCheck<UseIfInitStatementCheck>("aobus-readability-use-if-init-statement");
        checkFactories.registerCheck<UseRangesContainsCheck>("aobus-modernize-use-ranges-contains");
        checkFactories.registerCheck<UseRangesProjectionCheck>("aobus-modernize-use-ranges-projection");
        checkFactories.registerCheck<modernize::UseRangesAnyOfCheck>("aobus-modernize-use-ranges-any-of");
        checkFactories.registerCheck<UseEraseIfCheck>("aobus-modernize-use-erase-if");
        checkFactories.registerCheck<UseRangesMinMaxCheck>("aobus-modernize-use-ranges-min-max");
        checkFactories.registerCheck<UseStartsWithCheck>("aobus-modernize-use-starts-with");
        checkFactories.registerCheck<UseStdNumbersCheck>("aobus-modernize-use-std-numbers");
        checkFactories.registerCheck<UseCtadCheck>("aobus-modernize-use-ctad");
        checkFactories.registerCheck<modernize::UseStdToArrayCheck>("aobus-modernize-use-std-to-array");
        checkFactories.registerCheck<IncludeConventionCheck>("aobus-include-convention");
        checkFactories.registerCheck<SpdxLicenseHeaderCheck>("aobus-license-header");
        checkFactories.registerCheck<aobus::StrictForwardDeclarationCheck>("aobus-strict-forward-declaration");
      }
    };

    // LLVM requires registration through a namespace-scope constructor.
    ClangTidyModuleRegistry::Add<AobusLintModule> const aobusLintModuleRegistration{"aobus-lint-module",
                                                                                    "Adds Aobus custom checks."};
  } // namespace
} // namespace clang::tidy::readability

namespace
{
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables,aobus-threading-policy)
  [[maybe_unused]] int volatile AobusLintModuleAnchorSource = 0;
} // namespace
