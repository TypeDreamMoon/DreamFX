#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "DreamFXParser.h"
#include "Generation/DreamFXGenerator.h"
#include "Materials/Material.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterFactoryNew.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeAssignment.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraSystem.h"
#include "UObject/GCObjectScopeGuard.h"
#include "UObject/Package.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"

namespace UE::DreamFX::Editor::NativeInheritanceRegression
{
	UNiagaraEmitter* MakeParent(FAutomationTestBase& Test, const FString& Suffix)
	{
		const FString Name = TEXT("NE_Parent_") + Suffix;
		UPackage* Package = CreatePackage(*(TEXT("/Game/DreamFXAutomation/") + Name));
		UNiagaraEmitter* Parent = NewObject<UNiagaraEmitter>(Package, *Name,
			RF_Public | RF_Standalone | RF_Transactional);
		UNiagaraEmitterFactoryNew::InitializeEmitter(Parent, false);
		Parent->bIsInheritable = true;
		Parent->SetUniqueEmitterName(TEXT("NativeParent"));
		FVersionedNiagaraEmitterData* Data = Parent->GetLatestEmitterData();
		Data->bLocalSpace = true;
		UNiagaraSpriteRendererProperties* Sprite = NewObject<UNiagaraSpriteRendererProperties>(Parent);
		Sprite->Alignment = ENiagaraSpriteAlignment::VelocityAligned;
		Sprite->Material = UMaterial::GetDefaultMaterial(MD_Surface);
		Parent->AddRenderer(Sprite, Data->Version.VersionGuid);

		UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Data->GraphSource);
		if (!Test.TestNotNull(TEXT("native parent graph source"), Source)) { return nullptr; }
		UNiagaraNodeOutput* Output = Source->NodeGraph->FindEquivalentOutputNode(ENiagaraScriptUsage::ParticleSpawnScript);
		if (!Test.TestNotNull(TEXT("native parent spawn output"), Output)) { return nullptr; }
		const TArray<FNiagaraVariable> Targets = {
			FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("Particles.ParentMarker"))
		};
		const TArray<FString> Defaults = { TEXT("17.25") };
		if (!Test.TestNotNull(TEXT("native parent assignment"),
			FNiagaraStackGraphUtilities::AddParameterModuleToStack(Targets, *Output, INDEX_NONE, Defaults)))
		{
			return nullptr;
		}
		return Parent;
	}

	bool HasAssignment(const FVersionedNiagaraEmitterData* Data, FName Target)
	{
		const UNiagaraScriptSource* Source = Data == nullptr ? nullptr : Cast<UNiagaraScriptSource>(Data->GraphSource);
		if (Source == nullptr || Source->NodeGraph == nullptr) { return false; }
		for (const UEdGraphNode* Node : Source->NodeGraph->Nodes)
		{
			if (const UNiagaraNodeAssignment* Assignment = Cast<UNiagaraNodeAssignment>(Node))
			{
				if (Assignment->GetAssignmentTargets().ContainsByPredicate([Target](const FNiagaraVariable& Variable)
					{ return Variable.GetName() == Target; })) { return true; }
			}
		}
		return false;
	}

	FString SourceText(const FString& Name, const FString& ParentPath, const FString& Body,
		const FString& Version = FString())
	{
		FString Header = TEXT("Emitter Child");
		if (!ParentPath.IsEmpty())
		{
			Header += FString::Printf(TEXT(" inherits \"%s\""), *ParentPath);
			if (!Version.IsEmpty()) { Header += FString::Printf(TEXT(" version \"%s\""), *Version); }
		}
		return FString::Printf(TEXT("System(Name=\"%s\",Root=\"Game\") { %s { %s } }"), *Name, *Header, *Body);
	}

	FGenerateResult Build(FAutomationTestBase& Test, const FString& Source)
	{
		FDiagnosticSink Diagnostics;
		FDocument Document;
		if (!FParser::ParseText(Source, TEXT("NativeInheritance.dfs"), Document, Diagnostics))
		{
			Test.AddError(Diagnostics.FormatAll());
			return FGenerateResult();
		}
		FGenerateOptions Options;
		Options.bSave = false;
		// Do not force: a parent mutation must invalidate the dependency fingerprint itself.
		const FGenerateResult Result = FGenerator::Generate(Document, Options, Diagnostics);
		if (!Result.bSucceeded) { Test.AddError(Diagnostics.FormatAll()); }
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXNativeEmitterInheritanceRegression,
	"DreamFX.Regression.Inheritance.NativeEmitter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXNativeEmitterInheritanceRegression::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::NativeInheritanceRegression;
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UNiagaraEmitter* Parent = MakeParent(*this, Suffix);
	if (!TestNotNull(TEXT("standalone native parent"), Parent)) { return false; }
	FGCObjectScopeGuard ParentGuard(Parent);
	const FGuid ParentVersion = Parent->GetExposedVersion().VersionGuid;
	const FString Name = TEXT("DreamFXAutomation/NS_Inherits_") + Suffix;
	const FString OriginalText = SourceText(Name, Parent->GetPathName(), FString(), ParentVersion.ToString());
	const FGenerateResult Initial = Build(*this, OriginalText);
	if (!TestTrue(TEXT("native child compiled"), Initial.bSucceeded)
		|| !TestNotNull(TEXT("native child system"), Initial.System)) { return false; }
	FGCObjectScopeGuard SystemGuard(Initial.System);
	if (!TestEqual(TEXT("one inherited emitter"), Initial.System->GetEmitterHandles().Num(), 1)) { return false; }
	const FGuid HandleId = Initial.System->GetEmitterHandles()[0].GetId();
	auto CheckIdentity = [this, Parent, ParentVersion, HandleId](UNiagaraSystem* System)
	{
		const FNiagaraEmitterHandle& Handle = System->GetEmitterHandles()[0];
		const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
		TestEqual(TEXT("rebuild preserves emitter handle GUID"), Handle.GetId(), HandleId);
		TestEqual(TEXT("authored emitter name"), Handle.GetName(), FName(TEXT("Child")));
		if (TestNotNull(TEXT("child data"), Data))
		{
			TestTrue(TEXT("native parent reference is retained"), Data->GetParent().Emitter == Parent);
			TestEqual(TEXT("explicit parent version is retained"), Data->GetParent().Version, ParentVersion);
		}
	};
	CheckIdentity(Initial.System);
	const FVersionedNiagaraEmitterData* Data = Initial.System->GetEmitterHandles()[0].GetEmitterData();
	TestTrue(TEXT("omitted LocalSpace inherits true"), Data->bLocalSpace);
	TestEqual(TEXT("omitted renderer list inherits parent renderer"), Data->GetRenderers().Num(), 1);
	if (Data->GetRenderers().Num() == 1)
	{
		const UNiagaraSpriteRendererProperties* Sprite = Cast<UNiagaraSpriteRendererProperties>(Data->GetRenderers()[0]);
		if (TestNotNull(TEXT("inherited sprite"), Sprite))
		{
			TestEqual(TEXT("omitted renderer properties are preserved"), Sprite->Alignment, ENiagaraSpriteAlignment::VelocityAligned);
		}
	}
	TestTrue(TEXT("omitted stack retains native assignment"), HasAssignment(Data, TEXT("Particles.ParentMarker")));
	const FGenerateResult Unchanged = Build(*this, OriginalText);
	TestTrue(TEXT("unchanged native parent permits fingerprint skip"), Unchanged.bSkipped);

	const FGuid OldParentChangeId = Parent->GetChangeId();
	Parent->GetLatestEmitterData()->bLocalSpace = false;
	UNiagaraSpriteRendererProperties* SecondSprite = NewObject<UNiagaraSpriteRendererProperties>(Parent);
	SecondSprite->Material = UMaterial::GetDefaultMaterial(MD_Surface);
	Parent->AddRenderer(SecondSprite, ParentVersion);
	TestTrue(TEXT("fixture parent edit updates engine change ID"), Parent->GetChangeId() != OldParentChangeId);
	const FGenerateResult ChangedParent = Build(*this, OriginalText);
	if (!TestTrue(TEXT("changed parent rebuilt"), ChangedParent.bSucceeded)) { return false; }
	TestFalse(TEXT("parent fingerprint invalidates without Force"), ChangedParent.bSkipped);
	TestTrue(TEXT("same destination system is rebuilt"), ChangedParent.System == Initial.System);
	CheckIdentity(ChangedParent.System);
	Data = ChangedParent.System->GetEmitterHandles()[0].GetEmitterData();
	TestFalse(TEXT("updated parent LocalSpace is applied"), Data->bLocalSpace);
	TestEqual(TEXT("updated parent renderers are applied"), Data->GetRenderers().Num(), 2);
	TestTrue(TEXT("parent stack survives parent refresh"), HasAssignment(Data, TEXT("Particles.ParentMarker")));

	const FGenerateResult Overridden = Build(*this, SourceText(Name, Parent->GetPathName(), TEXT(
		"Settings={LocalSpace=true;Enabled=false;} "
		"ParticleSpawn={float Particles.ChildMarker=42.0;} "
		"SpriteRenderer Override { Alignment=Unaligned; }"), ParentVersion.ToString()));
	if (!TestTrue(TEXT("explicit native child overrides compiled"), Overridden.bSucceeded)) { return false; }
	CheckIdentity(Overridden.System);
	Data = Overridden.System->GetEmitterHandles()[0].GetEmitterData();
	TestTrue(TEXT("explicit LocalSpace overrides parent"), Data->bLocalSpace);
	TestFalse(TEXT("explicit Enabled disables handle"), Overridden.System->GetEmitterHandles()[0].GetIsEnabled());
	TestEqual(TEXT("explicit renderer replaces inherited renderer list"), Data->GetRenderers().Num(), 1);
	TestTrue(TEXT("explicit stack contains child assignment"), HasAssignment(Data, TEXT("Particles.ChildMarker")));
	TestFalse(TEXT("explicit stack replaces parent assignment"), HasAssignment(Data, TEXT("Particles.ParentMarker")));

	const FGenerateResult Restored = Build(*this, OriginalText);
	if (!TestTrue(TEXT("removing overrides compiled"), Restored.bSucceeded)) { return false; }
	CheckIdentity(Restored.System);
	Data = Restored.System->GetEmitterHandles()[0].GetEmitterData();
	TestTrue(TEXT("omitting Enabled restores fresh handle baseline"), Restored.System->GetEmitterHandles()[0].GetIsEnabled());
	TestFalse(TEXT("removing settings restores current parent"), Data->bLocalSpace);
	TestEqual(TEXT("removing renderer override restores parent list"), Data->GetRenderers().Num(), 2);
	TestTrue(TEXT("removing stack override restores parent stack"), HasAssignment(Data, TEXT("Particles.ParentMarker")));
	TestFalse(TEXT("removing stack override removes child assignment"), HasAssignment(Data, TEXT("Particles.ChildMarker")));

	const FGenerateResult EmptyStack = Build(*this,
		SourceText(Name, Parent->GetPathName(), TEXT("ParticleSpawn={}"), ParentVersion.ToString()));
	if (!TestTrue(TEXT("explicit empty inherited stack compiled"), EmptyStack.bSucceeded)) { return false; }
	TestFalse(TEXT("explicit empty stack clears inherited assignment"),
		HasAssignment(EmptyStack.System->GetEmitterHandles()[0].GetEmitterData(), TEXT("Particles.ParentMarker")));

	const FGenerateResult Detached = Build(*this, SourceText(Name, FString(), FString()));
	if (!TestTrue(TEXT("removing inherits compiled"), Detached.bSucceeded)) { return false; }
	Data = Detached.System->GetEmitterHandles()[0].GetEmitterData();
	TestEqual(TEXT("removing inherits preserves handle GUID"), Detached.System->GetEmitterHandles()[0].GetId(), HandleId);
	TestNull(TEXT("removing inherits removes engine parent"), Data->GetParent().Emitter.Get());
	TestEqual(TEXT("removing inherits clears inherited renderers"), Data->GetRenderers().Num(), 0);
	TestFalse(TEXT("removing inherits clears inherited stack"), HasAssignment(Data, TEXT("Particles.ParentMarker")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXNativeEmitterParentValidationRegression,
	"DreamFX.Regression.Inheritance.NativeParentValidation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXNativeEmitterParentValidationRegression::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::NativeInheritanceRegression;
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UNiagaraEmitter* Parent = MakeParent(*this, Suffix);
	if (!TestNotNull(TEXT("validation fixture parent"), Parent)) { return false; }
	FGCObjectScopeGuard ParentGuard(Parent);
	auto RejectBeforeAsset = [this, &Suffix](UNiagaraEmitter* SelectedParent,
		const FString& Case, const FString& Version, const FString& Code)
	{
		const FString ShortName = TEXT("NS_InvalidParent_") + Case + Suffix;
		const FString Name = TEXT("DreamFXAutomation/") + ShortName;
		FDocument Document;
		FDiagnosticSink Diagnostics;
		if (!TestTrue(TEXT("invalid semantic parent remains valid syntax"), FParser::ParseText(
			SourceText(Name, SelectedParent->GetPathName(), FString(), Version), TEXT("BadParent.dfs"), Document, Diagnostics)))
		{
			AddError(Diagnostics.FormatAll()); return;
		}
		FGenerateOptions Options;
		Options.bSave = false;
		const FGenerateResult Result = FGenerator::Generate(Document, Options, Diagnostics);
		TestFalse(TEXT("invalid native parent is rejected"), Result.bSucceeded);
		TestTrue(TEXT("invalid native parent explains rejection"), Diagnostics.FormatAll().Contains(Code));
		TestNull(TEXT("invalid parent cannot create destination asset"),
			FindObject<UNiagaraSystem>(nullptr, *(TEXT("/Game/") + Name + TEXT(".") + ShortName)));
	};
	Parent->bIsInheritable = false;
	RejectBeforeAsset(Parent, TEXT("Disabled"), FString(), TEXT("DFX3055"));
	Parent->bIsInheritable = true;
	RejectBeforeAsset(Parent, TEXT("Version"), FGuid::NewGuid().ToString(), TEXT("DFX3056"));

	const FString IntermediateName = TEXT("NE_Intermediate_") + Suffix;
	UPackage* IntermediatePackage = CreatePackage(*(TEXT("/Game/DreamFXAutomation/") + IntermediateName));
	UNiagaraEmitter* Intermediate = UNiagaraEmitter::CreateWithParentAndOwner(
		FVersionedNiagaraEmitter(Parent, Parent->GetExposedVersion().VersionGuid), IntermediatePackage,
		*IntermediateName, RF_Public | RF_Standalone | RF_Transactional);
	if (!TestNotNull(TEXT("intermediate native emitter"), Intermediate)) { return false; }
	FGCObjectScopeGuard IntermediateGuard(Intermediate);
	TestTrue(TEXT("intermediate starts synchronized with its parent"), Intermediate->IsSynchronizedWithParent());
	const FGuid AncestorChangeId = Parent->GetChangeId();
	Parent->GetLatestEmitterData()->bLocalSpace = false;
	UNiagaraSpriteRendererProperties* ExtraRenderer = NewObject<UNiagaraSpriteRendererProperties>(Parent);
	ExtraRenderer->Material = UMaterial::GetDefaultMaterial(MD_Surface);
	Parent->AddRenderer(ExtraRenderer, Parent->GetExposedVersion().VersionGuid);
	TestTrue(TEXT("ancestor change updates fingerprint"), Parent->GetChangeId() != AncestorChangeId);
	TestFalse(TEXT("intermediate now has unmerged ancestor changes"), Intermediate->IsSynchronizedWithParent());
	RejectBeforeAsset(Intermediate, TEXT("StaleAncestor"), FString(), TEXT("DFX3056"));
	TestFalse(TEXT("validation does not merge or mutate the parent asset"), Intermediate->IsSynchronizedWithParent());
	return true;
}

#endif
