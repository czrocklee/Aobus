#include "check/CApiGlobalQualificationCheck.h"
#include "check/ConcreteFinalCheck.h"
#include "check/ControlBlockSpacingCheck.h"
#include "check/ForbidNodiscardCheck.h"
#include "check/ForbidTrailingReturnCheck.h"
#include "check/IdentifierNamingExtensionsCheck.h"
#include "check/LambdaParamsCheck.h"
#include "check/LocalInitializationStyleCheck.h"
#include "check/MemberInitializerBracesCheck.h"
#include "check/OptionalNamingAndUsageCheck.h"
#include "check/StdCLibraryQualificationCheck.h"
#include "check/ThreadingPolicyCheck.h"
#include "check/UnusedSuppressionStyleCheck.h"
#include "clang-tidy/ClangTidyModule.h"
#include "clang-tidy/ClangTidyModuleRegistry.h"

namespace clang::tidy::readability {

class AobusLintModule : public ClangTidyModule {
public:
  void addCheckFactories(ClangTidyCheckFactories &CheckFactories) override {
    CheckFactories.registerCheck<CApiGlobalQualificationCheck>(
        "aobus-readability-c-api-global-qualification");
    CheckFactories.registerCheck<ConcreteFinalCheck>(
        "aobus-modernize-concrete-final");
    CheckFactories.registerCheck<ControlBlockSpacingCheck>(
        "aobus-readability-control-block-spacing");
    CheckFactories.registerCheck<ForbidNodiscardCheck>(
        "aobus-modernize-forbid-nodiscard");
    CheckFactories.registerCheck<ForbidTrailingReturnCheck>(
        "aobus-modernize-forbid-trailing-return");
    CheckFactories.registerCheck<IdentifierNamingExtensionsCheck>(
        "aobus-readability-identifier-naming-extensions");
    CheckFactories.registerCheck<LambdaParamsCheck>(
        "aobus-modernize-lambda-params");
    CheckFactories.registerCheck<LocalInitializationStyleCheck>(
        "aobus-modernize-local-initialization-style");
    CheckFactories.registerCheck<MemberInitializerBracesCheck>(
        "aobus-modernize-member-initializer-braces");
    CheckFactories.registerCheck<OptionalNamingAndUsageCheck>(
        "aobus-readability-optional-naming-and-usage");
    CheckFactories.registerCheck<StdCLibraryQualificationCheck>(
        "aobus-readability-std-c-library-qualification");
    CheckFactories.registerCheck<ThreadingPolicyCheck>(
        "aobus-threading-policy");
    CheckFactories.registerCheck<UnusedSuppressionStyleCheck>(
        "aobus-readability-unused-suppression-style");
  }
};

static ClangTidyModuleRegistry::Add<AobusLintModule>
    X("aobus-lint-module", "Adds Aobus custom checks.");

} // namespace clang::tidy::readability

volatile int AobusLintModuleAnchorSource = 0;