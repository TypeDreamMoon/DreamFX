#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "DreamFXParser.h"
#include "Generation/DreamFXGenerator.h"
#include "Generation/DreamFXEmitterMerge.h"
#include "Generation/DreamFXSystemInheritance.h"
#include "HAL/FileManager.h"
#include "Lint/DreamFXLint.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NiagaraEmitter.h"
#include "NiagaraSystem.h"
#include "SourceFiles/DreamFXPaths.h"
#include "UObject/GCObjectScopeGuard.h"
#include "Workspace/DreamFXSourceDependencies.h"

namespace UE::DreamFX::Editor::InheritanceTests
{
	struct FSourceFiles
	{
		FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT(".dreamfx-test-tmp")
			/ FGuid::NewGuid().ToString(EGuidFormats::Digits));
		TArray<FString> Files;
		TArray<FString> Directories;

		FString Write(const FString& Relative, const FString& Text)
		{
			const FString File = Directory / Relative;
			FString Parent = FPaths::GetPath(File);
			for (FString Candidate = Parent; FPaths::IsUnderDirectory(Candidate, Directory) || FPaths::IsSamePath(Candidate, Directory);
				Candidate = FPaths::GetPath(Candidate))
			{
				Directories.AddUnique(Candidate);
				if (FPaths::IsSamePath(Candidate, Directory)) { break; }
			}
			IFileManager::Get().MakeDirectory(*Parent, true);
			if (!FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) { return FString(); }
			Files.AddUnique(File);
			return File;
		}

		~FSourceFiles()
		{
			for (const FString& File : Files) { IFileManager::Get().Delete(*File); }
			Directories.Sort([](const FString& A, const FString& B) { return A.Len() > B.Len(); });
			for (const FString& Dir : Directories) { IFileManager::Get().DeleteDirectory(*Dir, false, false); }
		}
	};

	bool Parse(FAutomationTestBase& Test, const FString& Text, const FString& File, FDocument& Out)
	{
		FDiagnosticSink Diagnostics;
		if (!FParser::ParseText(Text, File, Out, Diagnostics)) { Test.AddError(Diagnostics.FormatAll()); return false; }
		return true;
	}

	bool Flatten(FAutomationTestBase& Test, const FDocument& Child, FDocument& Out)
	{
		FDiagnosticSink Diagnostics;
		if (!ResolveSystemInheritance(Child, Out, Diagnostics)) { Test.AddError(Diagnostics.FormatAll()); return false; }
		return true;
	}

	const TCHAR* Stacks = TEXT(R"(
        EmitterUpdate = { EmitterState(); SpawnRate(SpawnRate=1.0); }
        ParticleSpawn = { SystemLocation(); }
        ParticleUpdate = { ParticleState(); SolveForcesAndVelocity(); }
        SpriteRenderer Core {}
    )");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXSystemInheritanceMergeTest,
	"DreamFX.Inheritance.SystemMerge", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXSystemInheritanceMergeTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::InheritanceTests;
	FSourceFiles Files;
	const FString BaseText = TEXT(R"(System(Name="Base", Root="Game") {
        Settings = { WarmupTime=0.5; FixedTickDelta=true; }
        Properties = { float Speed=1.0; Color Tint=(1,0,0,1); }
        SystemSpawn = { float System.Keep=1.0; }
        SystemUpdate = { float System.Replace=1.0; }
        Emitter E from "Emitters/Shared" {
            Settings = { LocalSpace=true; }
            Defaults = { float Particles.Size=1.0; float Particles.Keep=2.0; }
            Stage A = {} Stage B = {}
        }
        Emitter Kept {}
    })");
	const FString BaseFile = Files.Write(TEXT("Base/Base.dfs"), BaseText);
	const FString EmitterFile = Files.Write(TEXT("Base/Emitters/Shared.dfe"), TEXT("Emitter(Name=\"Shared\") {}"));
	const FString MiddleFile = Files.Write(TEXT("Middle/Middle.dfs"), TEXT(R"(System(Name="Middle", Parent="../Base/Base") {
        Properties={float Speed=2.0;}
        Emitter E { Stage B(NumIterations=2) = {} }
    })"));
	const FString ChildFile = Files.Directory / TEXT("Child/Child.dfs");
	const FString ChildText = TEXT(R"(System(Name="Child", Root="Plugin.DreamFX", Parent="../Middle/Middle.dfs") {
        Settings={WarmupTime=0.75;}
        Properties={float Speed=3.0; int Count=4;}
        SystemUpdate={float System.Replace=5.0;}
        Emitter E { Defaults={float Particles.Size=6.0;} Stage C={} }
        Emitter Added {}
    })");
	if (BaseFile.IsEmpty() || EmitterFile.IsEmpty() || MiddleFile.IsEmpty()) { AddError(TEXT("Could not write inheritance sources.")); return false; }
	FDocument Child, Flat;
	if (!Parse(*this, ChildText, ChildFile, Child) || !Flatten(*this, Child, Flat)) { return false; }
	TestEqual(TEXT("child name retained"), Flat.Name, Child.Name);
	TestEqual(TEXT("child root retained"), Flat.Root, Child.Root);
	TestEqual(TEXT("child source retained"), Flat.SourceFilePath, ChildFile);
	TestTrue(TEXT("flattened parent is cleared"), Flat.ParentPath.IsEmpty());
	if (!TestEqual(TEXT("settings merge"), Flat.Settings.Num(), 2)
		|| !TestNotNull(TEXT("merged warmup setting"), Flat.FindSetting(TEXT("WarmupTime")))
		|| !TestNotNull(TEXT("merged fixed tick setting"), Flat.FindSetting(TEXT("FixedTickDelta")))) { return false; }
	TestEqual(TEXT("child setting wins"), Flat.FindSetting(TEXT("WarmupTime"))->Value->Number, 0.75);
	TestEqual(TEXT("inherited setting origin"), Flat.FindSetting(TEXT("FixedTickDelta"))->SourceFile, BaseFile);
	if (!TestEqual(TEXT("parameter merge"), Flat.Parameters.Num(), 3)) { return false; }
	TestEqual(TEXT("grandchild parameter wins"), Flat.Parameters[0].DefaultValue->Number, 3.0);
	TestEqual(TEXT("untouched parameter origin"), Flat.Parameters[1].SourceFile, BaseFile);
	if (!TestEqual(TEXT("system stack count"), Flat.Stacks.Num(), 2)
		|| !TestNotNull(TEXT("inherited system spawn"), Flat.FindStack(EStackKind::SystemSpawn))
		|| !TestNotNull(TEXT("overridden system update"), Flat.FindStack(EStackKind::SystemUpdate))) { return false; }
	TestEqual(TEXT("inherited stack source"), Flat.FindStack(EStackKind::SystemSpawn)->SourceFile, BaseFile);
	TestEqual(TEXT("overridden stack source"), Flat.FindStack(EStackKind::SystemUpdate)->SourceFile, ChildFile);
	if (!TestEqual(TEXT("emitter merge"), Flat.Emitters.Num(), 3)) { return false; }
	const FEmitter& Emitter = Flat.Emitters[0];
	TestEqual(TEXT("inherited from stays relative to its defining parent"), Emitter.FromSourceFile, BaseFile);
	FString ResolvedEmitter, Error;
	TestTrue(TEXT("inherited emitter reference resolves"), FDreamFXPaths::ResolveSourceReference(
		Emitter.FromPath, Emitter.FromSourceFile, TEXT(".dfe"), ResolvedEmitter, Error));
	TestTrue(TEXT("inherited emitter uses parent directory"), FPaths::IsSamePath(ResolvedEmitter, EmitterFile));
	if (!TestEqual(TEXT("default merge keeps untouched value"), Emitter.Defaults.Num(), 2)) { return false; }
	TestEqual(TEXT("child default wins"), Emitter.Defaults[0].Value->Number, 6.0);
	TestEqual(TEXT("inherited default origin"), Emitter.Defaults[1].SourceFile, BaseFile);
	if (!TestEqual(TEXT("named stages remain distinct"), Emitter.Stacks.Num(), 3)
		|| !TestTrue(TEXT("middle stage carries override"), Emitter.Stacks[1].Stage.NumIterations.IsSet())) { return false; }
	TestEqual(TEXT("middle stage override survives grandchild"), Emitter.Stacks[1].Stage.NumIterations.GetValue(), 2);
	FDocument Again;
	if (!Flatten(*this, Flat, Again)) { return false; }
	TestEqual(TEXT("flattening twice does not hash parent twice"), Again.SourceHash, Flat.SourceHash);

	// The same authored tree under a different checkout directory has the same fingerprint.
	FSourceFiles Relocated;
	Relocated.Write(TEXT("Base/Base.dfs"), BaseText);
	Relocated.Write(TEXT("Middle/Middle.dfs"), TEXT(R"(System(Name="Middle", Parent="../Base/Base") {
        Properties={float Speed=2.0;}
        Emitter E { Stage B(NumIterations=2) = {} }
    })"));
	FDocument MovedChild, MovedFlat;
	if (!Parse(*this, ChildText, Relocated.Directory / TEXT("Child/Child.dfs"), MovedChild)
		|| !Flatten(*this, MovedChild, MovedFlat)) { return false; }
	TestEqual(TEXT("parent hash is checkout-independent"), MovedFlat.SourceHash, Flat.SourceHash);
	Files.Write(TEXT("Base/Base.dfs"), BaseText + TEXT("\n// parent edited\n"));
	FDocument Changed;
	if (!Flatten(*this, Child, Changed)) { return false; }
	TestNotEqual(TEXT("grandparent-only edit invalidates child hash"), Changed.SourceHash, Flat.SourceHash);

	FEmitter NativeBase, SourceOverride, NativeOverride, Merged;
	NativeBase.Name = TEXT("E");
	NativeBase.NativeParentPath = TEXT("/Game/NE_Base");
	NativeBase.NativeParentVersion = FGuid::NewGuid().ToString();
	SourceOverride.Name = TEXT("E");
	SourceOverride.FromPath = TEXT("Other.dfe");
	SourceOverride.FromSourceFile = ChildFile;
	FDiagnosticSink MergeDiagnostics;
	TestTrue(TEXT("source base can replace native base"), MergeEmitterDefinitions(NativeBase, SourceOverride, Merged, MergeDiagnostics));
	TestTrue(TEXT("source replacement clears native parent"), Merged.NativeParentPath.IsEmpty() && Merged.NativeParentVersion.IsEmpty());
	TestEqual(TEXT("replacement keeps new source context"), Merged.FromSourceFile, ChildFile);
	NativeOverride = NativeBase;
	NativeOverride.NativeParentPath = TEXT("/Game/NE_Other");
	TestTrue(TEXT("native base can replace source base"), MergeEmitterDefinitions(SourceOverride, NativeOverride, Merged, MergeDiagnostics));
	TestTrue(TEXT("native replacement clears source parent"), Merged.FromPath.IsEmpty() && Merged.FromSourceFile.IsEmpty());
	TestEqual(TEXT("native replacement selects new asset"), Merged.NativeParentPath, NativeOverride.NativeParentPath);

	const FString WarningBase = Files.Write(TEXT("Warnings.dfs"), TEXT("System(Name=\"Warnings\") {Emitter E {Settings={SimTarget=GPU;}}}"));
	FDocument WarningChild;
	if (!Parse(*this, TEXT("System(Name=\"WarningChild\",Parent=\"Warnings\") {}"), Files.Directory / TEXT("WarningChild.dfs"), WarningChild)) { return false; }
	FDiagnosticSink LintDiagnostics;
	FLint::Run(WarningChild, LintDiagnostics);
	TestTrue(TEXT("lint resolves parent and points to inherited setting"), LintDiagnostics.GetDiagnostics().ContainsByPredicate(
		[&WarningBase](const FDiagnostic& Diagnostic) { return Diagnostic.Code == TEXT("DFX7101") && Diagnostic.File == WarningBase; }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXSystemInheritanceErrorsTest,
	"DreamFX.Inheritance.SystemErrors", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXSystemInheritanceErrorsTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::InheritanceTests;
	for (const TCHAR* Text : { TEXT("Emitter(Name=\"E\", Parent=\"Base\") {}"),
		TEXT("Module(Name=\"M\", Parent=\"Base\") { Body={} }"),
		TEXT("System(Name=\"S\", Parent=\"\") {}"),
		TEXT("System(Name=\"S\", Parent=\"Base\", Parent=\"Other\") {}") })
	{
		FDocument Document;
		FDiagnosticSink Diagnostics;
		TestFalse(TEXT("invalid Parent header rejected"), FParser::ParseText(Text, FString(), Document, Diagnostics));
		TestTrue(TEXT("invalid Parent header explained"), Diagnostics.FormatAll().Contains(TEXT("DFX2019")));
	}
	FSourceFiles Files;
	const FString BaseFile = Files.Write(TEXT("Base.dfs"), TEXT("System(Name=\"Base\") { Properties={float Speed=1.0;} }"));
	if (BaseFile.IsEmpty()) { AddError(TEXT("Could not write parent test source.")); return false; }
	const FString ChildFile = Files.Directory / TEXT("Child.dfs");
	auto ExpectResolutionError = [&](const FString& Source, const TCHAR* Code)
	{
		FDocument Child, Flat;
		if (!Parse(*this, Source, ChildFile, Child)) { return; }
		FDiagnosticSink Diagnostics;
		TestFalse(TEXT("invalid inheritance fails"), ResolveSystemInheritance(Child, Flat, Diagnostics));
		TestTrue(TEXT("inheritance diagnostic code"), Diagnostics.FormatAll().Contains(Code));
	};
	ExpectResolutionError(TEXT("System(Name=\"Child\",Parent=\"Missing\") {}"), TEXT("DFX3050"));
	ExpectResolutionError(TEXT("System(Name=\"Child\",Parent=\"Base\") {Properties={int Speed=2;}}"), TEXT("DFX3053"));
	ExpectResolutionError(TEXT("System(Name=\"Child\",Parent=\"Base\") {Emitter E {} Emitter E {}}"), TEXT("DFX3054"));
	Files.Write(TEXT("Base.dfs"), TEXT("System("));
	ExpectResolutionError(TEXT("System(Name=\"Child\",Parent=\"Base\") {}"), TEXT("DFX3051"));
	Files.Write(TEXT("Base.dfs"), TEXT("System(Name=\"Base\",Parent=\"Child\") {}"));
	Files.Write(TEXT("Child.dfs"), TEXT("System(Name=\"Child\",Parent=\"Base\") {}"));
	ExpectResolutionError(TEXT("System(Name=\"Child\",Parent=\"Base\") {}"), TEXT("DFX3052"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXSystemInheritanceDependencyTest,
	"DreamFX.Inheritance.SourceDependencies", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXSystemInheritanceDependencyTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::InheritanceTests;
	FSourceFiles Files;
	const FString Shared = Files.Write(TEXT("Base/Shared.dfe"), TEXT("Emitter(Name=\"Shared\") {}"));
	const FString Base = Files.Write(TEXT("Base/Base.dfs"), TEXT("System(Name=\"Base\") { Emitter E from \"Shared\" {} }"));
	const FString Middle = Files.Write(TEXT("Child/Middle.dfs"), TEXT("System(Name=\"Middle\",Parent=\"../Base/Base\") {}"));
	const FString Leaf = Files.Write(TEXT("Leaf.dfs"), TEXT("System(Name=\"Leaf\",Parent=\"Child/Middle\") {}"));
	const FString Unrelated = Files.Write(TEXT("Unrelated.dfs"), TEXT("System(Name=\"Unrelated\") {}"));
	FSourceDependencyIndex Index;
	Index.Refresh({ Base, Middle, Leaf, Unrelated });
	TSet<FString> Dependents;
	Index.FindDependents({ Shared }, Dependents);
	TestEqual(TEXT("emitter changes reach all three generations"), Dependents.Num(), 3);
	TestTrue(TEXT("leaf invalidated transitively"), Dependents.Contains(Leaf));
	TestFalse(TEXT("unrelated source stays current"), Dependents.Contains(Unrelated));
	Dependents.Reset();
	Index.FindDependents({ Base }, Dependents);
	TestEqual(TEXT("parent changes reach descendants"), Dependents.Num(), 2);
	IFileManager::Get().Delete(*Base);
	Dependents.Reset();
	Index.FindDependents({ Base }, Dependents);
	TestTrue(TEXT("deleted parent still invalidates leaf through old graph"), Dependents.Contains(Leaf));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXSystemInheritanceBuildTest,
	"DreamFX.Inheritance.SystemBuild", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXSystemInheritanceBuildTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::InheritanceTests;
	FSourceFiles Files;
	const FString Name = TEXT("DreamFXTests/NS_Inherited_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Base = Files.Write(TEXT("Base/Base.dfs"), TEXT("System(Name=\"UnusedParent\") {Settings={FixedTickDeltaTime=0.02;} Emitter E from \"Shared\" {}}"));
	const FString Shared = Files.Write(TEXT("Base/Shared.dfe"), FString::Printf(TEXT("Emitter(Name=\"Shared\") {Settings={LocalSpace=false;} %s}"), Stacks));
	const FString Child = Files.Write(TEXT("Child/Child.dfs"), FString::Printf(TEXT("System(Name=\"%s\",Parent=\"../Base/Base\") {}"), *Name));
	if (Base.IsEmpty() || Shared.IsEmpty() || Child.IsEmpty()) { AddError(TEXT("Could not write build sources.")); return false; }
	auto Generate = [&](bool bVerify, bool bExpectedSuccess)
	{
		FGenerateOptions Options;
		Options.bSave = false;
		Options.bVerifyOnly = bVerify;
		FDiagnosticSink Diagnostics;
		const FGenerateResult Result = FGenerator::GenerateFromFile(Child, Options, Diagnostics);
		if (Result.bSucceeded != bExpectedSuccess) { AddError(Diagnostics.FormatAll()); }
		return Result;
	};
	const FGenerateResult Initial = Generate(false, true);
	if (!Initial.bSucceeded || !Initial.System) { return false; }
	FGCObjectScopeGuard Guard(Initial.System);
	if (!TestEqual(TEXT("inherited emitter built"), Initial.System->GetEmitterHandles().Num(), 1)) { return false; }
	TestTrue(TEXT("unchanged child skips"), Generate(false, true).bSkipped);
	Files.Write(TEXT("Base/Base.dfs"), TEXT("System(Name=\"UnusedParent\") {Settings={FixedTickDeltaTime=0.04;} Emitter E from \"Shared\" {}}"));
	TestTrue(TEXT("parent-only edit reports drift"), Generate(true, false).bDrifted);
	const FGenerateResult ParentChanged = Generate(false, true);
	if (!ParentChanged.bSucceeded || !ParentChanged.System) { return false; }
	TestFalse(TEXT("parent-only edit rebuilds"), ParentChanged.bSkipped);
	TestEqual(TEXT("parent setting applied"), ParentChanged.System->GetFixedTickDeltaTime(), 0.04f);
	Files.Write(TEXT("Base/Shared.dfe"), FString::Printf(TEXT("Emitter(Name=\"Shared\") {Settings={LocalSpace=true;} %s}"), Stacks));
	TestTrue(TEXT("inherited emitter dependency reports drift"), Generate(true, false).bDrifted);
	TestFalse(TEXT("verify leaves inherited emitter untouched"), Initial.System->GetEmitterHandles()[0].GetEmitterData()->bLocalSpace);
	const FGenerateResult EmitterChanged = Generate(false, true);
	if (!EmitterChanged.bSucceeded || !EmitterChanged.System) { return false; }
	TestFalse(TEXT("inherited emitter dependency rebuilds"), EmitterChanged.bSkipped);
	TestTrue(TEXT("inherited emitter edit applied"), EmitterChanged.System->GetEmitterHandles()[0].GetEmitterData()->bLocalSpace);
	return true;
}

#endif
