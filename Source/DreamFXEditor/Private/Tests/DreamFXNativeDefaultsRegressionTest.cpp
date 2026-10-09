#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Adapter/DreamFXNiagaraAdapter.h"
#include "Generation/DreamFXGenerator.h"
#include "NiagaraEmitter.h"
#include "NiagaraGraph.h"
#include "NiagaraScriptSource.h"
#include "NiagaraScriptVariable.h"
#include "NiagaraSystem.h"
#include "UObject/GCObjectScopeGuard.h"

namespace UE::DreamFX::Editor::NativeInheritanceRegression
{
	UNiagaraEmitter* MakeParent(FAutomationTestBase& Test, const FString& Suffix);
	FString SourceText(const FString& Name, const FString& ParentPath, const FString& Body, const FString& Version);
	FGenerateResult Build(FAutomationTestBase& Test, const FString& Source);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXNativeDefaultsRegression,
	"DreamFX.Regression.Inheritance.NativeDefaults",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXNativeDefaultsRegression::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::NativeInheritanceRegression;
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UNiagaraEmitter* Parent = MakeParent(*this, TEXT("Defaults_") + Suffix);
	if (!TestNotNull(TEXT("native default fixture parent"), Parent)) { return false; }
	FGCObjectScopeGuard ParentGuard(Parent);
	UNiagaraScriptSource* ParentSource = Cast<UNiagaraScriptSource>(Parent->GetLatestEmitterData()->GraphSource);
	if (!TestNotNull(TEXT("parent script source"), ParentSource)
		|| !TestNotNull(TEXT("parent graph"), ParentSource->NodeGraph.Get())) { return false; }
	UNiagaraGraph* ParentGraph = ParentSource->NodeGraph;
	const FName BoundName(TEXT("Particles.ParentBound"));
	const FName LiteralName(TEXT("Particles.ParentLiteral"));
	const FName BindingTarget(TEXT("Particles.ParentMarker"));
	const FNiagaraTypeDefinition FloatType = FNiagaraTypeDefinition::GetFloatDef();
	const float ParentLiteral = 6.25f;
	const FNiagaraVariable BoundVariable(FloatType, BoundName);
	const FNiagaraVariable LiteralVariable(FloatType, LiteralName);

	// Use the public metadata store to construct actual parent graph defaults without relying
	// on the stock engine's unexported AddParameter. ParentMarker is written by the fixture's
	// spawn stack, so a child update read through this binding is a valid simulation.
	UNiagaraScriptVariable* BoundDefault = NewObject<UNiagaraScriptVariable>(ParentGraph);
	BoundDefault->Init(BoundVariable, FNiagaraVariableMetaData());
	BoundDefault->DefaultMode = ENiagaraDefaultMode::Binding;
	BoundDefault->DefaultBinding.SetName(BindingTarget);
	ParentGraph->GetAllMetaData().Add(BoundVariable, BoundDefault);
	UNiagaraScriptVariable* LiteralDefault = NewObject<UNiagaraScriptVariable>(ParentGraph);
	LiteralDefault->Init(LiteralVariable, FNiagaraVariableMetaData());
	LiteralDefault->DefaultMode = ENiagaraDefaultMode::Value;
	LiteralDefault->SetDefaultValueData(reinterpret_cast<const uint8*>(&ParentLiteral));
	ParentGraph->GetAllMetaData().Add(LiteralVariable, LiteralDefault);
	const FNiagaraVariable RequiredVariable(FloatType, BindingTarget);
	UNiagaraScriptVariable* RequiredDefault = ParentGraph->GetScriptVariable(BindingTarget);
	if (RequiredDefault == nullptr)
	{
		RequiredDefault = NewObject<UNiagaraScriptVariable>(ParentGraph);
		RequiredDefault->Init(RequiredVariable, FNiagaraVariableMetaData());
		ParentGraph->GetAllMetaData().Add(RequiredVariable, RequiredDefault);
	}
	RequiredDefault->DefaultMode = ENiagaraDefaultMode::FailIfPreviouslyNotSet;
	ParentGraph->NotifyGraphChanged();
	const FGuid ParentChangeId = Parent->GetChangeId();
	const FString Version = Parent->GetExposedVersion().VersionGuid.ToString();
	const FString Name = TEXT("DreamFXAutomation/NS_NativeDefaults_") + Suffix;
	const FString Reads = TEXT("ParticleUpdate = { float Particles.BoundRead = Particles.ParentBound; ")
		TEXT("float Particles.LiteralRead = Particles.ParentLiteral; float Particles.RequiredRead = Particles.ParentMarker; }");
	const FString InheritedText = SourceText(Name, Parent->GetPathName(), Reads, Version);
	const FGenerateResult Inherited = Build(*this, InheritedText);
	if (!TestTrue(TEXT("child with inherited defaults compiled"), Inherited.bSucceeded)
		|| !TestNotNull(TEXT("child system"), Inherited.System)) { return false; }
	FGCObjectScopeGuard SystemGuard(Inherited.System);

	auto ReadDefaults = [this](UNiagaraSystem* System, TArray<FParameterDefault>& Out)
	{
		TArray<FString> Errors;
		const bool bRead = FNiagaraAdapter::GetParameterDefaults(
			FStackAddress(System).WithEmitter(TEXT("Child")), Out, Errors);
		TestTrue(TEXT("parameter defaults read successfully"), bRead);
		if (!bRead) { AddError(FString::Join(Errors, TEXT(" | "))); }
		return bRead;
	};
	auto CheckBinding = [this, BoundName, BindingTarget](const TArray<FParameterDefault>& Defaults)
	{
		const FParameterDefault* Bound = Defaults.FindByPredicate([BoundName](const FParameterDefault& Default)
			{ return Default.Variable.GetName() == BoundName; });
		if (TestNotNull(TEXT("inherited binding is readable"), Bound))
		{
			TestTrue(TEXT("child link does not replace Binding with Value"), Bound->Mode == FParameterDefault::EMode::Binding);
			TestEqual(TEXT("parent binding target is retained"), Bound->Binding, BindingTarget);
		}
	};
	TArray<FParameterDefault> Defaults;
	if (!ReadDefaults(Inherited.System, Defaults)) { return false; }
	CheckBinding(Defaults);
	TestFalse(TEXT("export reader still omits Fail defaults"), Defaults.ContainsByPredicate(
		[BindingTarget](const FParameterDefault& Default) { return Default.Variable.GetName() == BindingTarget; }));
	TArray<FParameterDefault> Snapshot;
	TArray<FString> SnapshotErrors;
	if (!TestTrue(TEXT("inheritance reader includes required defaults"), FNiagaraAdapter::GetParameterDefaults(
		FStackAddress(Inherited.System).WithEmitter(TEXT("Child")), Snapshot, SnapshotErrors, true))) { return false; }
	const FParameterDefault* Required = Snapshot.FindByPredicate([BindingTarget](const FParameterDefault& Default)
		{ return Default.Variable.GetName() == BindingTarget; });
	if (TestNotNull(TEXT("required parent default survives child link"), Required))
	{
		TestTrue(TEXT("child implied fallback cannot relax inherited Fail"), Required->Mode == FParameterDefault::EMode::Fail);
	}
	const FParameterDefault* Literal = Defaults.FindByPredicate([LiteralName](const FParameterDefault& Default)
		{ return Default.Variable.GetName() == LiteralName; });
	if (TestNotNull(TEXT("inherited literal is readable"), Literal))
	{
		TestTrue(TEXT("inherited literal mode"), Literal->Mode == FParameterDefault::EMode::Value);
		TestTrue(TEXT("stock reader preserves literal bytes"), Literal->Value.Equals(
			FInputValue::MakeLiteral(FloatType.GetScriptStruct(), &ParentLiteral)));
	}

	const FGenerateResult Overridden = Build(*this, SourceText(Name, Parent->GetPathName(),
		Reads + TEXT(" Defaults = { float Particles.ParentBound = 9.0; }"), Version));
	if (!TestTrue(TEXT("explicit child default compiled"), Overridden.bSucceeded)) { return false; }
	Defaults.Reset();
	if (!ReadDefaults(Overridden.System, Defaults)) { return false; }
	const FParameterDefault* Explicit = Defaults.FindByPredicate([BoundName](const FParameterDefault& Default)
		{ return Default.Variable.GetName() == BoundName; });
	if (TestNotNull(TEXT("explicit child default is readable"), Explicit))
	{
		const float ChildLiteral = 9.0f;
		TestTrue(TEXT("explicit child default replaces Binding"), Explicit->Mode == FParameterDefault::EMode::Value);
		TestTrue(TEXT("explicit child value wins over parent"), Explicit->Value.Equals(
			FInputValue::MakeLiteral(FloatType.GetScriptStruct(), &ChildLiteral)));
	}

	const FGenerateResult Restored = Build(*this, InheritedText);
	if (!TestTrue(TEXT("removing child default compiled"), Restored.bSucceeded)) { return false; }
	Defaults.Reset();
	if (!ReadDefaults(Restored.System, Defaults)) { return false; }
	CheckBinding(Defaults);
	TestTrue(TEXT("parent binding was never modified"), BoundDefault->DefaultMode == ENiagaraDefaultMode::Binding);
	TestEqual(TEXT("parent binding target was never modified"), BoundDefault->DefaultBinding.GetName(), BindingTarget);
	TestEqual(TEXT("generation does not change the parent revision"), Parent->GetChangeId(), ParentChangeId);
	return true;
}

#endif
