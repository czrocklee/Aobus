#include "check/BracedInitializationCheck.h"
#include "check/CApiGlobalQualificationCheck.h"
#include "check/ConcreteFinalCheck.h"
#include "check/ControlBlockSpacingCheck.h"
#include "check/ForbidNodiscardCheck.h"
#include "check/ForbidTrailingReturnCheck.h"
#include "check/IdentifierNamingExtensionsCheck.h"
#include "check/LambdaParamsCheck.h"
#include "check/LocalInitializationStyleCheck.h"
#include "check/MemberOrderCheck.h"
#include "check/OptionalNamingAndUsageCheck.h"
#include "check/StdCLibraryQualificationCheck.h"
#include "check/ThreadingPolicyCheck.h"
#include "check/UnusedSuppressionStyleCheck.h"
#include "check/UseIfInitStatementCheck.h"
#include "clang-tidy/ClangTidyModule.h"
#include "clang-tidy/ClangTidyModuleRegistry.h"

namespace clang::tidy::readability
{
  class AobusLintModule : public ClangTidyModule
  {
  public:
    void addCheckFactories(ClangTidyCheckFactories& checkFactories) override
    {
      checkFactories.registerCheck<CApiGlobalQualificationCheck>("aobus-readability-c-api-global-qualification");
      checkFactories.registerCheck<ConcreteFinalCheck>("aobus-modernize-concrete-final");
      checkFactories.registerCheck<ControlBlockSpacingCheck>("aobus-readability-control-block-spacing");
      checkFactories.registerCheck<ForbidNodiscardCheck>("aobus-modernize-forbid-nodiscard");
      checkFactories.registerCheck<ForbidTrailingReturnCheck>("aobus-modernize-forbid-trailing-return");
      checkFactories.registerCheck<IdentifierNamingExtensionsCheck>("aobus-readability-identifier-naming-extensions");
      checkFactories.registerCheck<LambdaParamsCheck>("aobus-modernize-lambda-params");
      checkFactories.registerCheck<LocalInitializationStyleCheck>("aobus-modernize-local-initialization-style");
      checkFactories.registerCheck<BracedInitializationCheck>("aobus-modernize-braced-initialization");
      checkFactories.registerCheck<MemberOrderCheck>("aobus-readability-member-order");
      checkFactories.registerCheck<OptionalNamingAndUsageCheck>("aobus-readability-optional-naming-and-usage");
      checkFactories.registerCheck<StdCLibraryQualificationCheck>("aobus-readability-std-c-library-qualification");
      checkFactories.registerCheck<ThreadingPolicyCheck>("aobus-threading-policy");
      checkFactories.registerCheck<UnusedSuppressionStyleCheck>("aobus-readability-unused-suppression-style");
      checkFactories.registerCheck<UseIfInitStatementCheck>("aobus-readability-use-if-init-statement");
    }
  };

  namespace
  {
    ClangTidyModuleRegistry::Add<AobusLintModule> const aobusLintModuleRegistration{"aobus-lint-module",
                                                                                    "Adds Aobus custom checks."};
  } // namespace
} // namespace clang::tidy::readability

namespace
{
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables,aobus-threading-policy)
  int volatile AobusLintModuleAnchorSource = 0;
} // namespace