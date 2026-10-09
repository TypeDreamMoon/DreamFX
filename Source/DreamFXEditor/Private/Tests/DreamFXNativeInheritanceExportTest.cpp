#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Adapter/DreamFXNiagaraAdapter.h"
#include "Decompiler/DreamFXDecompiler.h"
#include "DreamFXParser.h"
#include "Generation/DreamFXGenerator.h"
#include "NiagaraEmitter.h"
#include "NiagaraSystem.h"
#include "UObject/GCObjectScopeGuard.h"

// Share the native parent fixture with the generation regression, rather than using an
// engine content emitter whose modules and versions vary across supported installations.
namespace UE::DreamFX::Editor::NativeInheritanceRegression
{
	UNiagaraEmitter* MakeParent(FAutomationTestBase& Test, const FString& Suffix);
	bool HasAssignment(const FVersionedNiagaraEmitterData* Data, FName Target);
	FString SourceText(const FString& Name, const FString& ParentPath, const FString& Body, const FString& Version);
	FGenerateResult Build(FAutomationTestBase& Test, const FString& Source);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXNativeInheritanceExportRegression,
	"DreamFX.Regression.Inheritance.NativeEmitterExport",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXNativeInheritanceExportRegression::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::NativeInheritanceRegression;
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UNiagaraEmitter* Parent = MakeParent(*this, TEXT("Export_") + Suffix);
	if (!TestNotNull(TEXT("export fixture parent"), Parent)) { return false; }
	FGCObjectScopeGuard ParentGuard(Parent);
	const FGuid ParentVersion = Parent->GetExposedVersion().VersionGuid;
	const FGuid ParentChangeId = Parent->GetChangeId();
	const FString Name = TEXT("DreamFXAutomation/NS_ExportInheritance_") + Suffix;
	const FGenerateResult Initial = Build(*this,
		SourceText(Name, Parent->GetPathName(), FString(), ParentVersion.ToString()));
	if (!TestTrue(TEXT("inherited source generated"), Initial.bSucceeded)
		|| !TestNotNull(TEXT("inherited system"), Initial.System)) { return false; }
	FGCObjectScopeGuard SystemGuard(Initial.System);

	FDiagnosticSink ExportDiagnostics;
	FDecompileOptions ExportOptions;
	ExportOptions.bDecompiledNamespace = true;
	const FDecompileResult Unchanged = FDecompiler::Decompile(Initial.System, TEXT("Game"), ExportDiagnostics, ExportOptions);
	TestTrue(TEXT("inherited system exports"), Unchanged.bSucceeded);
	TestTrue(TEXT("native parent path is exported"), Unchanged.Source.Contains(TEXT("inherits \"") + Parent->GetPathName() + TEXT("\"")));
	TestTrue(TEXT("actual parent version is pinned"), Unchanged.Source.Contains(
		TEXT("version \"") + ParentVersion.ToString(EGuidFormats::DigitsWithHyphens) + TEXT("\"")));
	TestFalse(TEXT("unchanged inherited stack remains implicit"), Unchanged.Source.Contains(TEXT("ParticleSpawn =")));
	TestFalse(TEXT("unchanged inherited renderers remain implicit"), Unchanged.Source.Contains(TEXT("SpriteRenderer")));
	TestFalse(TEXT("unchanged inherited LocalSpace remains implicit"), Unchanged.Source.Contains(TEXT("LocalSpace =")));
	TestFalse(TEXT("native export no longer claims flattening"), Unchanged.Source.Contains(TEXT("flattened")));
	TestTrue(TEXT("identical supported inheritance has no coverage gaps"), Unchanged.UnsupportedFeatures.IsEmpty());

	FDocument ExportedDocument;
	FDiagnosticSink ParseDiagnostics;
	if (!TestTrue(TEXT("inherited export parses"), FParser::ParseText(Unchanged.Source,
		TEXT("NativeEmitterExport.dfs"), ExportedDocument, ParseDiagnostics)))
	{
		AddError(ParseDiagnostics.FormatAll()); return false;
	}
	if (TestEqual(TEXT("one exported emitter"), ExportedDocument.Emitters.Num(), 1))
	{
		TestEqual(TEXT("parsed native parent"), ExportedDocument.Emitters[0].NativeParentPath, Parent->GetPathName());
	}
	const FGenerateResult Rebuilt = Build(*this, Unchanged.Source);
	if (!TestTrue(TEXT("inherited export rebuilds"), Rebuilt.bSucceeded)) { return false; }
	FGCObjectScopeGuard RebuiltGuard(Rebuilt.System);
	const FVersionedNiagaraEmitterData* RebuiltData = Rebuilt.System->GetEmitterHandles()[0].GetEmitterData();
	TestTrue(TEXT("export rebuild preserves parent association"), RebuiltData->GetParent().Emitter == Parent);
	TestEqual(TEXT("export rebuild preserves exact parent version"), RebuiltData->GetParent().Version, ParentVersion);
	TestTrue(TEXT("export rebuild inherits omitted parent stack"), HasAssignment(RebuiltData, TEXT("Particles.ParentMarker")));

	// False is the engine default, but differs from this parent. Comparing against a fresh
	// emitter would silently drop the authored override and re-enable local space on rebuild.
	const FGenerateResult Changed = Build(*this, SourceText(Name, Parent->GetPathName(),
		TEXT("Settings = { LocalSpace = false; } ParticleSpawn = {}"), ParentVersion.ToString()));
	if (!TestTrue(TEXT("child overrides generated"), Changed.bSucceeded)) { return false; }
	FDiagnosticSink ChangedDiagnostics;
	const FDecompileResult ChangedExport = FDecompiler::Decompile(Changed.System, TEXT("Game"), ChangedDiagnostics, ExportOptions);
	TestTrue(TEXT("setting equal to engine default still overrides parent"), ChangedExport.Source.Contains(TEXT("LocalSpace = false;")));
	TestTrue(TEXT("cleared inherited stack exports an explicit empty block"), ChangedExport.Source.Contains(TEXT("ParticleSpawn = {")));
	TestTrue(TEXT("whole-stack inheritance limit is disclosed"), ChangedDiagnostics.FormatAll().Contains(TEXT("DFX8014")));
	const FGenerateResult ChangedRebuilt = Build(*this, ChangedExport.Source);
	if (!TestTrue(TEXT("changed inherited export rebuilds"), ChangedRebuilt.bSucceeded)) { return false; }
	RebuiltData = ChangedRebuilt.System->GetEmitterHandles()[0].GetEmitterData();
	TestFalse(TEXT("parent-relative false survives rebuild"), RebuiltData->bLocalSpace);
	TestFalse(TEXT("explicit empty stack survives rebuild"), HasAssignment(RebuiltData, TEXT("Particles.ParentMarker")));

	// Model an asset with an inherited renderer removed without asking the stack editor to
	// delete an inherited entry, which some engine versions intentionally disallow in the UI.
	UNiagaraEmitter* ChangedEmitter = FNiagaraAdapter::GetEmitterInstance(
		FStackAddress(Changed.System).WithEmitter(TEXT("Child")));
	FVersionedNiagaraEmitterData* ChangedData = ChangedEmitter->GetLatestEmitterData();
	if (!TestEqual(TEXT("renderer deletion fixture starts with inherited renderer"), ChangedData->GetRenderers().Num(), 1)) { return false; }
	ChangedEmitter->RemoveRenderer(ChangedData->GetRenderers()[0], ChangedData->Version.VersionGuid);
	FDiagnosticSink RemovedDiagnostics;
	const FDecompileResult Removed = FDecompiler::Decompile(Changed.System, TEXT("Game"), RemovedDiagnostics, ExportOptions);
	TestTrue(TEXT("unrepresentable renderer deletion is a coverage gap"), Removed.UnsupportedFeatures.ContainsByPredicate(
		[](const FString& Gap) { return Gap.Contains(TEXT("removing all inherited renderers")); }));
	TestEqual(TEXT("export never mutates the native parent"), Parent->GetChangeId(), ParentChangeId);
	TestTrue(TEXT("parent setting remains authored"), Parent->GetLatestEmitterData()->bLocalSpace);
	TestTrue(TEXT("parent graph remains authored"), HasAssignment(Parent->GetLatestEmitterData(), TEXT("Particles.ParentMarker")));
	Parent->bIsInheritable = false;
	FDiagnosticSink InvalidParentDiagnostics;
	const FDecompileResult InvalidParent = FDecompiler::Decompile(Initial.System, TEXT("Game"), InvalidParentDiagnostics, ExportOptions);
	TestTrue(TEXT("a parent that no longer permits inheritance cannot export without a gap"), InvalidParent.UnsupportedFeatures.ContainsByPredicate(
		[](const FString& Gap) { return Gap.Contains(TEXT("not an inheritable standalone emitter asset")); }));
	Parent->bIsInheritable = true;
	return true;
}

#endif
