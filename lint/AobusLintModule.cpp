#include "ControlBlockSpacingCheck.h"
#include "clang-tidy/ClangTidyModule.h"
#include "clang-tidy/ClangTidyModuleRegistry.h"

namespace clang::tidy::readability {

class AobusLintModule : public ClangTidyModule {
public:
  void addCheckFactories(ClangTidyCheckFactories &CheckFactories) override {
    CheckFactories.registerCheck<ControlBlockSpacingCheck>(
        "readability-control-block-spacing");
  }
};

static ClangTidyModuleRegistry::Add<AobusLintModule>
    X("aobus-lint-module", "Adds Aobus custom checks.");

} // namespace clang::tidy::readability

volatile int AobusLintModuleAnchorSource = 0;