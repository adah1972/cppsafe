#include "cppsafe/lifetime/type/Aggregate.h"

#include "cppsafe/lifetime/Lifetime.h"
#include "cppsafe/lifetime/LifetimePset.h"
#include "cppsafe/lifetime/LifetimePsetBuilder.h"
#include "cppsafe/lifetime/LifetimeTypeCategory.h"
#include "cppsafe/util/assert.h"

#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/OperationKinds.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <llvm/ADT/STLFunctionalExtras.h>

namespace clang::lifetime {

namespace {

void expandAggregateImpl(const Variable& Base, const CXXRecordDecl* RD, SubVarPath& Path,
    llvm::function_ref<void(const Variable&, const SubVarPath&, TypeClassification)> Fn)
{
    for (const auto* FD : RD->fields()) {
        const auto Ty = FD->getType();
        const auto TC = classifyTypeCategory(Ty);

        Path.push_back(FD);

        Fn(Variable(Base, FD), Path, TC);

        if (TC.isAggregate()) {
            auto VV = Base;
            VV.addFieldRef(FD);
            expandAggregateImpl(VV, Ty->getAsCXXRecordDecl(), Path, Fn);
        }

        Path.pop_back();
    }
}

}

void expandAggregate(const Variable& Base, const CXXRecordDecl* RD,
    llvm::function_ref<void(const Variable&, const SubVarPath&, TypeClassification)> Fn)
{
    CPPSAFE_ASSERT(RD->hasDefinition());

    SubVarPath Path;
    Fn(Base, Path, classifyTypeCategory(RD->getTypeForDecl()));

    expandAggregateImpl(Base, RD, Path, Fn);
}

void handleAggregateDefaultInit(
    const Variable& Base, const CXXRecordDecl* RD, const CFGBlock* B, SourceRange Range, PSBuilder& Builder)
{
    expandAggregate(
        Base, RD, [&Builder, Range, B](const Variable& SubVar, const SubVarPath& Path, TypeClassification TC) {
            if (!TC.isPointer()) {
                return;
            }

            CPPSAFE_ASSERT(!Path.empty());

            const auto* FD = Path.back();
            const auto FDTy = FD->getType();
            const auto* Init = FD->getInClassInitializer();
            if (Init == nullptr) {
                // primitive types
                if (FDTy->isPointerType()) {
                    Builder.setVarPSet(SubVar, PSet::invalid(InvalidationReason::notInitialized(Range, B)));
                    return;
                }

                // record types
                if (isNullableType(FDTy)) {
                    Builder.setVarPSet(SubVar, PSet::null(NullReason::defaultConstructed(Range, B)));
                } else {
                    Builder.setVarPSet(SubVar, PSet::globalVar(false));
                }

                return;
            }

            if (isa<CXXNewExpr>(Init)) {
                Builder.getReporter().warnNakedNewDelete(Init->getSourceRange());
                Builder.setVarPSet(SubVar, PSet::globalVar());
                return;
            }
            if (const auto* Cast = dyn_cast<ImplicitCastExpr>(Init); Cast && Cast->getCastKind() == CK_NullToPointer) {
                Builder.setVarPSet(SubVar, PSet::null(NullReason::defaultConstructed(Range, B)));
                return;
            }

            // TODO
            Builder.setVarPSet(SubVar, {});
        });
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void handleAggregateInit(const Variable& Base, const CXXRecordDecl* RD, const InitListExpr* InitE, const CFGBlock* B,
    SourceRange Range, PSBuilder& Builder)
{
    if (InitE->getNumInits() == 0) {
        handleAggregateDefaultInit(Base, RD, B, Range, Builder);
        return;
    }

    unsigned Idx = 0;
    for (const auto* FD : (RD->fields())) {
        const auto Ty = FD->getType();
        const auto TC = classifyTypeCategory(Ty);
        auto VV = Base;
        VV.addFieldRef(FD);

        if (TC.isAggregate()) {
            const auto* FieldRD = Ty->getAsCXXRecordDecl();

            if (Idx < InitE->getNumInits()) {
                const auto* SubE = InitE->getInit(Idx);
                if (const auto* SubInitE = dyn_cast<InitListExpr>(SubE)) {
                    handleAggregateInit(VV, FieldRD, SubInitE, B, Range, Builder);
                } else {
                    Builder.forEachExprMember(SubE, [VV, &Builder](const SubVarPath& Path, const PSet& P) {
                        VV.chainFields(Path);
                        Builder.setVarPSet(VV, P);
                    });
                }
            } else {
                handleAggregateDefaultInit(VV, FieldRD, B, Range, Builder);
            }
        } else if (TC.isPointer()) {
            if (Idx < InitE->getNumInits() && !isa<ImplicitValueInitExpr>(InitE->getInit(Idx))) {
                auto PS = Builder.getPSet(InitE->getInit(Idx));
                // handle inner pointers: pset(b.a.z3) = ((*this).x)
                PS.transformVars([&Base](Variable V) {
                    if (const auto* T = V.asThis()) {
                        if (T == Base.getType()->getAsCXXRecordDecl()) {
                            const auto* FD = V.getField().value();
                            V = Base;
                            V.addFieldRef(FD);
                        }
                    }

                    return V;
                });
                Builder.setVarPSet(VV, PS);
            } else {
                if (isNullableType(Ty) || !Ty->isRecordType()) {
                    Builder.setVarPSet(VV, PSet::null(NullReason::defaultConstructed(Range, B)));
                } else {
                    Builder.setVarPSet(VV, PSet::globalVar());
                }
            }
        }

        ++Idx;
    }
}

void handleAggregateCopy(const Expr* LHS, const Expr* RHS, PSBuilder& Builder)
{
    const auto OtherPS = Builder.getPSet(RHS);
    CPPSAFE_ASSERT(OtherPS.vars().size() <= 1);

    // if PSet(RHS) = {}, use it as a placeholder to derive members
    const Variable Candidate(Builder.ignoreTransparentExprs(RHS));
    const auto* Other = OtherPS.vars().empty() ? &Candidate : &*OtherPS.vars().begin();
    const auto* RD = LHS->getType()->getAsCXXRecordDecl();
    const Variable Base(LHS);
    expandAggregate(Base, RD,
        [&Base, &Builder, Other, &OtherPS](const Variable& LhsSubVar, const SubVarPath& Path, TypeClassification TC) {
            const Variable RhsSubVar(Other->chainFields(Path));
            if (!TC.isPointer()) {
                return;
            }
            if (OtherPS.containsGlobal()) {
                Builder.setVarPSet(LhsSubVar, PSet::globalVar());
                return;
            }

            auto PS = Builder.getVarPSet(RhsSubVar);
            CPPSAFE_ASSERT(PS);

            // handle inner pointers: pset(b.a.z3) = ((*this).x)
            PS->transformVars([&Base](Variable V) {
                if (V.asExpr()) {
                    V = V.replaceExpr(Base);
                }

                return V;
            });

            Builder.setVarPSet(LhsSubVar, *PS);
        });
}

}
