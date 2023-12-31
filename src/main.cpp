#include "cppsafe/AstConsumer.h"
#include "cppsafe/Options.h"

#include <clang/Frontend/FrontendActions.h>
#include <clang/Tooling/CommonOptionsParser.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/WithColor.h>

using namespace clang::tooling;
using namespace llvm;
using namespace cppsafe;

static cl::desc desc(StringRef Description) { return { Description.ltrim() }; }

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static cl::OptionCategory CppSafeCategory("cppsafe options");

static const cl::opt<bool> WarnLifetimeMove(
    "Wlifetime-move", desc("check use after move"), cl::init(false), cl::cat(CppSafeCategory));

class LifetimeFrontendAction : public clang::ASTFrontendAction {
public:
    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance&, llvm::StringRef) override
    {
        const CppsafeOptions Options {
            .LifetimeMove = WarnLifetimeMove,
        };
        return std::make_unique<AstConsumer>(Options);
    }
};

int main(int argc, const char** argv)
{
    const cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);
    const cl::extrahelp CppSafeHelp(R"(
Lifetime checks:
    default: local pset check, enforce function preconditions
)");

    const llvm::InitLLVM _(argc, argv);

    auto OptionsParser = CommonOptionsParser::create(argc, argv, CppSafeCategory);
    if (!OptionsParser) {
        llvm::WithColor::error() << llvm::toString(OptionsParser.takeError());
        return 1;
    }

    ClangTool Tool(OptionsParser->getCompilations(), OptionsParser->getSourcePathList());
    return Tool.run(newFrontendActionFactory<LifetimeFrontendAction>().get());
}