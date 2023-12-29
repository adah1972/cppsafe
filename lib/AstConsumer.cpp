#include "cppsafe/AstConsumer.h"
#include "cppsafe/lifetime/Lifetime.h"

#include <clang/AST/Type.h>
#include <clang/Basic/DiagnosticIDs.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Sema/Overload.h>
#include <gsl/assert>

#include <array>
#include <cassert>
#include <set>

using namespace clang;

namespace clang::lifetime {

enum LifetimeDiag {
    warn_pset_of_global,
    warn_deref_nullptr,
    warn_assign_nullptr,
    warn_deref_dangling,
    warn_dangling,
    warn_null,
    warn_wrong_pset,
    warn_non_static_throw,
    warn_lifetime_pointer_arithmetic,
    warn_lifetime_unsafe_cast,
    warn_unsupported_expression,

    warn_lifetime_category,
    warn_lifetime_filtered,
    // debug
    warn_pset,
    warn_deref_unknown,
    warn_lifetime_type_category,

    note_never_initialized,
    note_pointee_left_scope,
    note_temporary_destroyed,
    note_pointer_arithmetic,
    note_dereferenced,
    note_null_here,
    note_null_reason_parameter,
    note_null_reason_default_construct,
    note_null_reason_compared_to_null,
    note_forbidden_cast,
    note_modified,
    note_deleted,
    note_assigned,
    note_here,
};

const std::array Warnings{
    LifetimeDiag::warn_deref_dangling,
    LifetimeDiag::warn_deref_nullptr,
    LifetimeDiag::warn_assign_nullptr,
    LifetimeDiag::warn_null,
    LifetimeDiag::warn_dangling,
};

const std::array Notes{
    LifetimeDiag::note_never_initialized,
    LifetimeDiag::note_temporary_destroyed,
    LifetimeDiag::note_dereferenced,
    LifetimeDiag::note_forbidden_cast,
    LifetimeDiag::note_modified,
    LifetimeDiag::note_deleted,
    LifetimeDiag::note_assigned,
    LifetimeDiag::note_null_reason_parameter,
    LifetimeDiag::note_null_reason_default_construct,
    LifetimeDiag::note_null_reason_compared_to_null,
};

class Reporter : public LifetimeReporterBase {
    Sema& S;
    bool FilterWarnings;
    std::set<SourceLocation> WarningLocs;
    bool IgnoreCurrentWarning = false;
    std::map<LifetimeDiag, unsigned int> WarningIds;

    bool enableIfNew(SourceRange Range)
    {
        auto I = WarningLocs.insert(Range.getBegin());
        IgnoreCurrentWarning = !I.second;
        return !IgnoreCurrentWarning;
    }

public:
    Reporter(Sema& S, bool FilterWarnings)
        : S(S)
        , FilterWarnings(FilterWarnings)
    {
        auto& E = S.getDiagnostics();

        WarningIds[LifetimeDiag::warn_pset_of_global] = E.getCustomDiagID(
            DiagnosticsEngine::Warning, "the pset of '%0' must be a subset of {(static), (null)}, but is {%1}");
        WarningIds[LifetimeDiag::warn_deref_nullptr]
            = E.getCustomDiagID(DiagnosticsEngine::Warning, "dereferencing a%select{| possibly}0 null pointer");
        WarningIds[LifetimeDiag::warn_assign_nullptr] = E.getCustomDiagID(
            DiagnosticsEngine::Warning, "assigning a%select{| possibly}0 null pointer to a non-null object");
        WarningIds[LifetimeDiag::warn_deref_dangling]
            = E.getCustomDiagID(DiagnosticsEngine::Warning, "dereferencing a%select{| possibly}0 dangling pointer");
        WarningIds[LifetimeDiag::warn_dangling] = E.getCustomDiagID(DiagnosticsEngine::Warning,
            "%select{passing|returning|returning}0 a%select{| possibly}2 dangling pointer%select{ as argument|| as "
            "output value '%1'}0");
        WarningIds[LifetimeDiag::warn_null] = E.getCustomDiagID(DiagnosticsEngine::Warning,
            "%select{passing|returning|returning}0 a%select{| possibly}2 null pointer%select{ as argument|| as output "
            "value '%1'}0 where a non-null pointer is expected");
        WarningIds[LifetimeDiag::warn_wrong_pset] = E.getCustomDiagID(DiagnosticsEngine::Warning,
            "%select{passing|returning|returning}0 a pointer%select{ as argument|| as output value '%1'}0 with "
            "points-to set %2 where points-to set %3 is expected");
        WarningIds[LifetimeDiag::warn_non_static_throw] = E.getCustomDiagID(DiagnosticsEngine::Warning,
            "throwing a pointer with points-to set %0 where points-to set ((static)) is expected");
        WarningIds[LifetimeDiag::warn_lifetime_pointer_arithmetic]
            = E.getCustomDiagID(DiagnosticsEngine::Warning, "pointer arithmetic disables lifetime analysis");
        WarningIds[LifetimeDiag::warn_lifetime_unsafe_cast]
            = E.getCustomDiagID(DiagnosticsEngine::Warning, "unsafe cast disables lifetime analysis");
        WarningIds[LifetimeDiag::warn_unsupported_expression]
            = E.getCustomDiagID(DiagnosticsEngine::Warning, "this pre/postcondition is not supported");
        WarningIds[LifetimeDiag::warn_lifetime_category]
            = E.getCustomDiagID(DiagnosticsEngine::Warning, "%0 could be annotate as %1");
        WarningIds[LifetimeDiag::warn_lifetime_filtered] = E.getCustomDiagID(
            DiagnosticsEngine::Warning, "TODO: this is just a placeholder, never actually emitted.");
        WarningIds[LifetimeDiag::warn_pset] = E.getCustomDiagID(DiagnosticsEngine::Warning, "pset(%0) = %1");
        WarningIds[LifetimeDiag::warn_deref_unknown]
            = E.getCustomDiagID(DiagnosticsEngine::Warning, "dereferencing a unknown pointer");
        WarningIds[LifetimeDiag::warn_lifetime_type_category] = E.getCustomDiagID(DiagnosticsEngine::Warning,
            "lifetime type category is %select{Owner|Pointer|Aggregate|Value}0 %select{|with pointee %2}1");

        WarningIds[LifetimeDiag::note_never_initialized]
            = E.getCustomDiagID(DiagnosticsEngine::Note, "it was never initialized here");
        WarningIds[LifetimeDiag::note_pointee_left_scope]
            = E.getCustomDiagID(DiagnosticsEngine::Note, "pointee '%0' left the scope here");
        WarningIds[LifetimeDiag::note_temporary_destroyed]
            = E.getCustomDiagID(DiagnosticsEngine::Note, "temporary was destroyed at the end of the full expression");
        WarningIds[LifetimeDiag::note_pointer_arithmetic]
            = E.getCustomDiagID(DiagnosticsEngine::Note, "pointer arithmetic is not allowed");
        WarningIds[LifetimeDiag::note_dereferenced] = E.getCustomDiagID(DiagnosticsEngine::Note, "was dereferenced here");
        WarningIds[LifetimeDiag::note_null_here] = E.getCustomDiagID(DiagnosticsEngine::Note, "became nullptr here");
        WarningIds[LifetimeDiag::note_null_reason_parameter] = E.getCustomDiagID(DiagnosticsEngine::Note,
            "the parameter is assumed to be potentially null. Consider using gsl::not_null<>, a reference instead of a "
            "pointer or an assert() to explicitly remove null");
        WarningIds[LifetimeDiag::note_null_reason_default_construct]
            = E.getCustomDiagID(DiagnosticsEngine::Note, "default-constructed pointers are assumed to be null");
        WarningIds[LifetimeDiag::note_null_reason_compared_to_null]
            = E.getCustomDiagID(DiagnosticsEngine::Note, "is compared to null here");
        WarningIds[LifetimeDiag::note_forbidden_cast]
            = E.getCustomDiagID(DiagnosticsEngine::Note, "used a forbidden cast here");
        WarningIds[LifetimeDiag::note_modified] = E.getCustomDiagID(DiagnosticsEngine::Note, "modified here");
        WarningIds[LifetimeDiag::note_deleted] = E.getCustomDiagID(DiagnosticsEngine::Note, "deleted here");
        WarningIds[LifetimeDiag::note_assigned] = E.getCustomDiagID(DiagnosticsEngine::Note, "assigned here");
        WarningIds[LifetimeDiag::note_here] = E.getCustomDiagID(DiagnosticsEngine::Note, "here");
    }

    bool shouldFilterWarnings() const final { return FilterWarnings; }

    void warnPsetOfGlobal(SourceRange Range, StringRef VariableName, std::string ActualPset) final
    {
        if (enableIfNew(Range)) {
            S.Diag(Range.getBegin(), WarningIds[warn_pset_of_global]) << VariableName << ActualPset << Range;
        }
    }
    void warnNullDangling(WarnType T, SourceRange Range, ValueSource Source, StringRef ValueName, bool Possibly) final
    {
        assert(T == WarnType::Dangling || T == WarnType::Null);
        if (enableIfNew(Range)) {
            S.Diag(Range.getBegin(), WarningIds[(LifetimeDiag)Warnings.at((int)T)])
                << (int)Source << ValueName << Possibly << Range;
        }
    }
    void warn(WarnType T, SourceRange Range, bool Possibly) final
    {
        assert((unsigned)T < sizeof(Warnings) / sizeof(Warnings[0]));
        assert(T != WarnType::Dangling && T != WarnType::Null);
        if (enableIfNew(Range)) {
            S.Diag(Range.getBegin(), WarningIds[(LifetimeDiag)Warnings.at((int)T)]) << Possibly << Range;
        }
    }
    void warnNonStaticThrow(SourceRange Range, StringRef ThrownPset) final
    {
        if (enableIfNew(Range)) {
            S.Diag(Range.getBegin(), WarningIds[LifetimeDiag::warn_non_static_throw]) << ThrownPset << Range;
        }
    }
    void warnWrongPset(
        SourceRange Range, ValueSource Source, StringRef ValueName, StringRef RetPset, StringRef ExpectedPset) final
    {
        if (enableIfNew(Range)) {
            S.Diag(Range.getBegin(), WarningIds[LifetimeDiag::warn_wrong_pset])
                << (int)Source << ValueName << RetPset << ExpectedPset << Range;
        }
    }
    void warnPointerArithmetic(SourceRange Range) final
    {
        if (enableIfNew(Range)) {
            S.Diag(Range.getBegin(), WarningIds[LifetimeDiag::warn_lifetime_pointer_arithmetic]);
        }
    }
    void warnUnsafeCast(SourceRange Range) final
    {
        if (enableIfNew(Range)) {
            S.Diag(Range.getBegin(), WarningIds[LifetimeDiag::warn_lifetime_unsafe_cast]);
        }
    }

    void warnUnsupportedExpr(SourceRange Range) final
    {
        if (enableIfNew(Range)) {
            S.Diag(Range.getBegin(), WarningIds[LifetimeDiag::warn_unsupported_expression]) << Range;
        }
    }
    void notePointeeLeftScope(SourceRange Range, std::string Name) final
    {
        if (!IgnoreCurrentWarning) {
            S.Diag(Range.getBegin(), WarningIds[LifetimeDiag::note_pointee_left_scope]) << Name << Range;
        }
    }
    void note(NoteType T, SourceRange Range) final
    {
        assert((unsigned)T < sizeof(Notes) / sizeof(Notes[0]));
        if (!IgnoreCurrentWarning) {
            S.Diag(Range.getBegin(), WarningIds[(LifetimeDiag)Notes.at((int)T)]) << Range;
        }
    }
    void debugPset(SourceRange Range, StringRef Variable, std::string Pset) final
    {
        S.Diag(Range.getBegin(), WarningIds[LifetimeDiag::warn_pset]) << Variable << Pset << Range;
    }

    void debugTypeCategory(SourceRange Range, TypeCategory Category, StringRef Pointee) final
    {
        S.Diag(Range.getBegin(), WarningIds[LifetimeDiag::warn_lifetime_type_category])
            << (int)Category << !Pointee.empty() << Pointee;
    }
};

} // namespace clang::lifetime

namespace cppsafe {

void AstConsumer::run(const clang::FunctionDecl* Fn)
{
    Expects(Sema != nullptr);

    if (Sema->getSourceManager().isInSystemHeader(Fn->getBeginLoc())) {
        return;
    }

    auto IsConvertible = [this, Fn](QualType From, QualType To) {
        OpaqueValueExpr Expr(Fn->getBeginLoc(), From, clang::VK_PRValue);
        const ImplicitConversionSequence ICS
            = Sema->TryImplicitConversion(&Expr, To, /*SuppressUserConversions=*/false, Sema::AllowedExplicit::All,
                /*InOverloadResolution=*/false, /*CStyle=*/false,
                /*AllowObjCWritebackConversion=*/false);
        return !ICS.isFailure();
    };

    lifetime::Reporter Reporter(*Sema, false);
    lifetime::runAnalysis(Fn, Sema->getASTContext(), Reporter, IsConvertible);
}

} // namespace cppsafe
