#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "DreamFXParser.h"
#include "Engine/Texture2D.h"
#include "Generation/DreamFXGenerator.h"
#include "Generation/DreamFXModuleGenerator.h"
#include "Generation/DreamFXValueLowering.h"
#include "NiagaraEmitter.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraSystem.h"
#include "Materials/MaterialInterface.h"
#include "UObject/GCObjectScopeGuard.h"

namespace UE::DreamFX::Editor::MaterialDefaultRegression
{
	static FString UniqueName(const TCHAR* Prefix)
	{
		return FString::Printf(TEXT("DreamFXAutomation/%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	}

	static bool HasCode(const FDiagnosticSink& Diagnostics, const TCHAR* Code)
	{
		return Diagnostics.GetDiagnostics().ContainsByPredicate([&](const FDiagnostic& Entry) { return Entry.Code == Code; });
	}

	static const TCHAR* MaterialPath = TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXMeshMaterialValidationTest,
	"DreamFX.Regression.Materials.MeshOverrides",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXMeshMaterialValidationTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::MaterialDefaultRegression;
	struct FCase { const TCHAR* Properties; bool bWarning; bool bEnabled; bool bHasMaterial; };
	const FCase Cases[] = {
		{ TEXT("OverrideMaterials=[\"/Engine/EngineMaterials/DefaultMaterial\"];"), true, false, true },
		{ TEXT("bOverrideMaterials=false; OverrideMaterials=[\"/Engine/EngineMaterials/DefaultMaterial\"];"), true, false, true },
		{ TEXT("OverrideMaterials=[\"/Engine/EngineMaterials/DefaultMaterial\"]; bOverrideMaterials=true;"), false, true, true },
		{ TEXT("OverrideMaterials=[];"), false, false, false },
		{ TEXT("bOverrideMaterials=false; overridematerials=[\"/Engine/EngineMaterials/DefaultMaterial\"]; boverridematerials=true;"), false, true, true },
	};
	for (const FCase& Case : Cases)
	{
		FDiagnosticSink Diagnostics;
		FDocument Document;
		const FString Source = FString::Printf(TEXT("System(Name=\"%s\",Root=\"Game\") { Emitter E { MeshRenderer Mesh { Meshes=[\"/Engine/BasicShapes/Cube\"]; %s } } }"),
			*UniqueName(TEXT("NS_MeshMaterial")), Case.Properties);
		if (!FParser::ParseText(Source, TEXT("MeshMaterial.dfs"), Document, Diagnostics))
		{
			AddError(Diagnostics.FormatAll()); return false;
		}
		FGenerateOptions Options; Options.bSave = false;
		const FGenerateResult Result = FGenerator::Generate(Document, Options, Diagnostics);
		if (!TestTrue(TEXT("mesh renderer generates"), Result.bSucceeded) || Result.System == nullptr)
		{
			AddError(Diagnostics.FormatAll()); return false;
		}
		FGCObjectScopeGuard Guard(Result.System);
		TestEqual(FString::Printf(TEXT("disabled override diagnostic for %s"), Case.Properties),
			HasCode(Diagnostics, TEXT("DFX7105")), Case.bWarning);
		const FVersionedNiagaraEmitterData* Data = Result.System->GetEmitterHandles()[0].GetEmitterData();
		const UNiagaraMeshRendererProperties* Mesh = Data != nullptr && Data->GetRenderers().Num() == 1
			? Cast<UNiagaraMeshRendererProperties>(Data->GetRenderers()[0]) : nullptr;
		if (!TestNotNull(TEXT("generated mesh renderer"), Mesh)) { return false; }
		TestEqual(TEXT("explicit override state is preserved"), Mesh->bOverrideMaterials != 0, Case.bEnabled);
		TestEqual(TEXT("material array is preserved"), Mesh->OverrideMaterials.Num(), Case.bHasMaterial ? 1 : 0);
		if (Case.bHasMaterial && Mesh->OverrideMaterials.Num() == 1)
		{
			const UMaterialInterface* Material = Mesh->OverrideMaterials[0].ExplicitMat;
			if (TestNotNull(TEXT("override material resolved"), Material))
			{
				TestEqual(TEXT("chosen material preserved"), Material->GetPathName(), FString(MaterialPath));
			}
		}
	}

	FDocument Invalid;
	FDiagnosticSink Diagnostics;
	const FString Source = FString::Printf(TEXT("System(Name=\"%s\",Root=\"Game\") { Emitter E { MeshRenderer Mesh { Material=\"/Engine/EngineMaterials/DefaultMaterial\"; } } }"),
		*UniqueName(TEXT("NS_BadMeshMaterial")));
	if (!FParser::ParseText(Source, TEXT("BadMeshMaterial.dfs"), Invalid, Diagnostics)) { AddError(Diagnostics.FormatAll()); return false; }
	FGenerateOptions Options; Options.bSave = false;
	const FGenerateResult Rejected = FGenerator::Generate(Invalid, Options, Diagnostics);
	TestFalse(TEXT("single Material on mesh is rejected"), Rejected.bSucceeded);
	TestTrue(TEXT("mesh error names supported override syntax"), HasCode(Diagnostics, TEXT("DFX3049"))
		&& Diagnostics.FormatAll().Contains(TEXT("bOverrideMaterials")));
	TestNull(TEXT("invalid material rejected before creating an asset"), Rejected.System);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXInstanceInputDefaultsTest,
	"DreamFX.Regression.Defaults.InstanceInputDefaults",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXInstanceInputDefaultsTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::MaterialDefaultRegression;
	for (const TCHAR* Declaration : {
		TEXT("DI<SkeletalMesh> Source = \"this is not an asset path\";"),
		TEXT("DI<SkeletalMesh> Source = 0;"),
		TEXT("Object<Actor> Source = \"this is not an asset path\";"),
		TEXT("Object<SceneComponent> Source = \"this is not an asset path\";") })
	{
		FDocument Document;
		FDiagnosticSink Diagnostics;
		const FString Source = FString::Printf(TEXT("Module(Name=\"%s\",Root=\"Game\") { Settings={Usage=ParticleUpdate;} Inputs={%s} Body={float Local=1.0;} }"),
			*UniqueName(TEXT("M_InstanceDefault")), Declaration);
		if (!FParser::ParseText(Source, TEXT("InstanceDefault.dfm"), Document, Diagnostics))
		{
			AddError(Diagnostics.FormatAll()); return false;
		}
		FGenerateOptions Options; Options.bSave = false;
		const FModuleGenerateResult Result = FModuleGenerator::Generate(Document, Options, Diagnostics);
		TestFalse(TEXT("instance default rejected"), Result.bSucceeded);
		TestTrue(TEXT("instance default has actionable type diagnostic"), HasCode(Diagnostics, TEXT("DFX4043")));
		TestFalse(TEXT("instance default never reported as missing asset"), HasCode(Diagnostics, TEXT("DFX4040")));
		TestNull(TEXT("invalid default rejected before asset creation"), Result.Script);
	}

	FDocument Bare;
	FDiagnosticSink Diagnostics;
	const FString BareSource = FString::Printf(TEXT("Module(Name=\"%s\",Root=\"Game\") { Settings={Usage=ParticleUpdate;} Inputs={DI<SkeletalMesh> Source;} Body={float Local=1.0;} }"),
		*UniqueName(TEXT("M_BareDI")));
	if (!FParser::ParseText(BareSource, TEXT("BareDI.dfm"), Bare, Diagnostics)) { AddError(Diagnostics.FormatAll()); return false; }
	FGenerateOptions Options; Options.bSave = false;
	if (!TestTrue(TEXT("DI input without default still generates"), FModuleGenerator::Generate(Bare, Options, Diagnostics).bSucceeded))
	{
		AddError(Diagnostics.FormatAll());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXAssetDefaultsPreservedTest,
	"DreamFX.Regression.Defaults.AssetReferences",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXAssetDefaultsPreservedTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::MaterialDefaultRegression;
	FValue Value;
	Value.Kind = EValueKind::String;
	Value.Text = MaterialPath;
	Value.Location = FSourceLocation(3, 10);
	FInputValue Lowered;
	FDiagnosticSink Diagnostics;
	if (!TestTrue(TEXT("ordinary material asset reference still lowers"), FValueLowering::Lower(
		Value, FNiagaraTypeDefinition::GetUMaterialDef(), TEXT("User.Material"), Diagnostics, Lowered)))
	{
		AddError(Diagnostics.FormatAll()); return false;
	}
	TestTrue(TEXT("asset reference keeps object mode"), Lowered.Mode == EInputValueMode::ObjectAsset);
	if (TestNotNull(TEXT("asset default resolved"), Lowered.ObjectAsset))
	{
		TestEqual(TEXT("asset default points to chosen asset"), Lowered.ObjectAsset->GetPathName(), Value.Text);
		TestTrue(TEXT("asset remains an asset"), Lowered.ObjectAsset->IsAsset());
	}
	// A broad Object<UObject> can name an asset, but must not smuggle a transient subobject in.
	UObject* Instance = NewObject<UTexture2D>(GetTransientPackage());
	FGCObjectScopeGuard Guard(Instance);
	Value.Text = Instance->GetPathName();
	Diagnostics.Reset();
	TestFalse(TEXT("generic object reference cannot store an instance default"), FValueLowering::Lower(
		Value, FNiagaraTypeDefinition::GetUObjectDef(), TEXT("User.Instance"), Diagnostics, Lowered));
	TestTrue(TEXT("instance path diagnosed as an instance"), HasCode(Diagnostics, TEXT("DFX4043")));
	return true;
}

#endif
