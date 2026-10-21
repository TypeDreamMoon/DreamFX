#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "DreamFXParser.h"
#include "Generation/DreamFXDynamicMesh.h"
#include "Generation/DreamFXGenerator.h"
#include "Generation/DreamFXSystemInheritance.h"
#include "HAL/FileManager.h"
#include "Lint/DreamFXLint.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "NiagaraDataInterface.h"
#include "NiagaraEmitter.h"
#include "NiagaraSystem.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/GCObjectScopeGuard.h"
#include "UObject/UnrealType.h"

namespace UE::DreamFX::Editor::DynamicMeshTests
{
	bool Parse(FAutomationTestBase& Test, const FString& Text, const FString& File, FDocument& Out)
	{
		FDiagnosticSink Diagnostics;
		if (!FParser::ParseText(Text, File, Out, Diagnostics))
		{
			Test.AddError(Diagnostics.FormatAll());
			return false;
		}
		return true;
	}

	bool Lower(FAutomationTestBase& Test, const FDocument& Document, FDynamicMeshLowering& Out, bool bResolveAssets = true)
	{
		FDynamicMeshLowerOptions Options;
		Options.bResolveAssets = bResolveAssets;
		FDiagnosticSink Diagnostics;
		if (!LowerDynamicMeshes(Document, Options, Out, Diagnostics))
		{
			Test.AddError(Diagnostics.FormatAll());
			return false;
		}
		return true;
	}

	const FStatement* FindCall(const FEmitter& Emitter, EStackKind Kind, const FString& Suffix)
	{
		const FStack* Stack = Emitter.FindStack(Kind);
		if (Stack == nullptr)
		{
			return nullptr;
		}
		return Stack->Statements.FindByPredicate([&](const FStatement& Statement)
			{ return Statement.Kind == EStatementKind::ModuleCall && Statement.Name.EndsWith(Suffix); });
	}

	double ArgumentNumber(const FStatement* Statement, const TCHAR* Name)
	{
		if (Statement == nullptr)
		{
			return -1.0;
		}
		const FNamedArgument* Argument = Statement->Arguments.FindByPredicate(
			[&](const FNamedArgument& Candidate) { return Candidate.Name == Name; });
		return Argument != nullptr && Argument->Value.IsValid() ? Argument->Value->Number : -1.0;
	}

	const TCHAR* Source = TEXT(R"(System(Name="DreamFXAutomation/NS_DynamicMeshLowering", Root="Game")
{
    Properties = { float Lift = 10.0; }

    Emitter First { Settings = { SimTarget = CPU; } }

    DynamicMesh Petals
    {
        Settings = {
            Material  = "/Engine/EngineDebugMaterials/VertexColorMaterial";
            TexCoords = 3;
            Normals   = Explicit;
            Bounds    = box(-10, -20, -30, 10, 20, 30);
            Visible   = 1.0;
        }
        Inputs = { float Lift = User.Lift; }
        Renderer = { SortOrderHint = 7; }
        Draw = { Particles.Color = (1.0, 1.0, 1.0, 0.5); }

        Patch A = Grid(5, 3, Copies = 2) { Position = float3(U, V, Lift); }
        Patch B = Tube(9, 4, Material = "/Engine/EngineMaterials/WorldGridMaterial", Flip = true) { }
        Patch C = Strip(6) { UV2 = float2(K, Rand); }
    }

    Emitter Last { Settings = { SimTarget = CPU; } }
}
)");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXDynamicMeshParseTest, "DreamFX.DynamicMesh.Parse",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXDynamicMeshParseTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor::DynamicMeshTests;

	FDocument Document;
	if (!Parse(*this, Source, TEXT("NS_DynamicMeshLowering.dfs"), Document))
	{
		return false;
	}
	if (!TestEqual(TEXT("one block"), Document.DynamicMeshes.Num(), 1))
	{
		return false;
	}
	const FDynamicMesh& Mesh = Document.DynamicMeshes[0];
	TestEqual(TEXT("name"), Mesh.Name, FString(TEXT("Petals")));
	TestEqual(TEXT("written after the first emitter"), Mesh.EmitterIndex, 1);
	TestEqual(TEXT("settings"), Mesh.Settings.Num(), 5);
	TestEqual(TEXT("inputs"), Mesh.Inputs.Num(), 1);
	TestEqual(TEXT("renderer properties"), Mesh.RendererProperties.Num(), 1);
	TestEqual(TEXT("draw statements"), Mesh.Draw.Statements.Num(), 1);
	TestEqual(TEXT("draw is a particle update stack"), Mesh.Draw.Kind, EStackKind::ParticleUpdate);
	if (TestEqual(TEXT("patches"), Mesh.Patches.Num(), 3))
	{
		const FDynamicMeshPatch& A = Mesh.Patches[0];
		TestEqual(TEXT("patch name"), A.Name, FString(TEXT("A")));
		TestTrue(TEXT("topology is a call"), A.Topology.IsValid() && A.Topology->Kind == EValueKind::Call);
		TestEqual(TEXT("topology"), A.Topology->Text, FString(TEXT("Grid")));
		TestEqual(TEXT("counts are positional"), A.Topology->Elements.Num(), 2);
		TestEqual(TEXT("options are named"), A.Topology->Arguments.Num(), 1);
		TestTrue(TEXT("body is raw HLSL"), A.Body.Contains(TEXT("Position = float3(U, V, Lift);")));
		TestTrue(TEXT("body location is known"), A.BodyLocation.IsValid());
		TestTrue(TEXT("an empty body parses"), Mesh.Patches[1].Body.TrimStartAndEnd().IsEmpty());
	}
	TestEqual(TEXT("both emitters kept"), Document.Emitters.Num(), 2);
	TestEqual(TEXT("source file stamped"), Mesh.SourceFile, FString(TEXT("NS_DynamicMeshLowering.dfs")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXDynamicMeshLoweringTest, "DreamFX.DynamicMesh.Lowering",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXDynamicMeshLoweringTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::DynamicMeshTests;

	FDocument Document;
	FDynamicMeshLowering Lowering;
	if (!Parse(*this, Source, TEXT("NS_DynamicMeshLowering.dfs"), Document) || !Lower(*this, Document, Lowering))
	{
		return false;
	}
	const FDocument& System = Lowering.System;
	TestTrue(TEXT("the blocks are gone from the lowered system"), System.DynamicMeshes.IsEmpty());

	// -- emitters: where the block was written, writers then the drawing emitter ------------------
	TArray<FString> Names;
	for (const FEmitter& Emitter : System.Emitters)
	{
		Names.Add(Emitter.Name);
	}
	TestEqual(TEXT("emitter order"), FString::Join(Names, TEXT(",")),
		FString(TEXT("First,Petals_A,Petals_B,Petals_C,Petals,Last")));

	// Vertices: A 5x3x2 = 30, B 9x4 = 36, C 6x2 = 12. Triangles: A 2*4*2*2 = 32, B 2*8*3 = 48, C 2*5 = 10.
	// Sections: VertexColorMaterial (A, C) first, WorldGridMaterial (B) second, so the triangles run
	// A, C, B -- section by section, patch order inside each.
	const FEmitter* A = System.Emitters.FindByPredicate([](const FEmitter& E) { return E.Name == TEXT("Petals_A"); });
	const FEmitter* B = System.Emitters.FindByPredicate([](const FEmitter& E) { return E.Name == TEXT("Petals_B"); });
	const FEmitter* C = System.Emitters.FindByPredicate([](const FEmitter& E) { return E.Name == TEXT("Petals_C"); });
	if (A == nullptr || B == nullptr || C == nullptr)
	{
		AddError(TEXT("a writer emitter is missing"));
		return false;
	}
	const FStatement* TopologyA = FindCall(*A, EStackKind::ParticleSpawn, TEXT("/DM_GridTopology"));
	const FStatement* TopologyB = FindCall(*B, EStackKind::ParticleSpawn, TEXT("/DM_GridTopology"));
	const FStatement* TopologyC = FindCall(*C, EStackKind::ParticleSpawn, TEXT("/DM_GridTopology"));
	TestEqual(TEXT("A vertex base"), ArgumentNumber(TopologyA, TEXT("DM_VertexBase")), 0.0);
	TestEqual(TEXT("B vertex base"), ArgumentNumber(TopologyB, TEXT("DM_VertexBase")), 30.0);
	TestEqual(TEXT("C vertex base"), ArgumentNumber(TopologyC, TEXT("DM_VertexBase")), 66.0);
	TestEqual(TEXT("A triangle base"), ArgumentNumber(TopologyA, TEXT("DM_TriangleBase")), 0.0);
	TestEqual(TEXT("C triangle base (same section as A)"), ArgumentNumber(TopologyC, TEXT("DM_TriangleBase")), 32.0);
	TestEqual(TEXT("B triangle base (second section)"), ArgumentNumber(TopologyB, TEXT("DM_TriangleBase")), 42.0);
	TestEqual(TEXT("Strip has two rows"), ArgumentNumber(TopologyC, TEXT("DM_Rows")), 2.0);
	const FNamedArgument* Flip = TopologyB != nullptr ? TopologyB->Arguments.FindByPredicate(
		[](const FNamedArgument& Argument) { return Argument.Name == TEXT("DM_Flip"); }) : nullptr;
	TestTrue(TEXT("Flip reaches the topology module"), Flip != nullptr && Flip->Value.IsValid() && Flip->Value->bBool);

	const FPropertyEntry* Count = A->Settings.FindByPredicate([](const FPropertyEntry& P) { return P.Name == TEXT("PreAllocationCount"); });
	TestTrue(TEXT("one particle per vertex"), Count != nullptr && Count->Value->Number == 30.0);
	const FPropertyEntry* Target = A->Settings.FindByPredicate([](const FPropertyEntry& P) { return P.Name == TEXT("SimTarget"); });
	TestTrue(TEXT("writers are GPU"), Target != nullptr && Target->Value->Text == TEXT("GPU"));

	const FStatement* Vertex = FindCall(*A, EStackKind::ParticleUpdate, TEXT("/DM_Petals_A"));
	TestNotNull(TEXT("the vertex module runs every frame"), Vertex);
	TestNotNull(TEXT("and at spawn"), FindCall(*A, EStackKind::ParticleSpawn, TEXT("/DM_Petals_A")));
	if (Vertex != nullptr)
	{
		TestTrue(TEXT("inputs are passed through"), Vertex->Arguments.ContainsByPredicate(
			[](const FNamedArgument& Argument) { return Argument.Name == TEXT("Lift") && Argument.Value->Text == TEXT("User.Lift"); }));
	}

	const FEmitter* Draw = System.Emitters.FindByPredicate([](const FEmitter& E) { return E.Name == TEXT("Petals"); });
	if (TestNotNull(TEXT("drawing emitter"), Draw))
	{
		TestEqual(TEXT("one renderer"), Draw->Renderers.Num(), 1);
		const FStack* Update = Draw->FindStack(EStackKind::ParticleUpdate);
		TestTrue(TEXT("Visible and the Draw block reach the drawing particle"), Update != nullptr
			&& Update->Statements.Num() == 3
			&& Update->Statements[1].Name.EndsWith(TEXT("/DM_Visibility"))
			&& Update->Statements[2].Name == TEXT("Particles.Color"));
		if (Draw->Renderers.Num() == 1)
		{
			const FRenderer& Renderer = Draw->Renderers[0];
			TestEqual(TEXT("mesh renderer"), Renderer.TypeName, FString(TEXT("MeshRenderer")));
			TestTrue(TEXT("bound to the mesh"), Renderer.Properties.ContainsByPredicate([](const FPropertyEntry& P)
				{ return P.Name == TEXT("MeshBindings") && P.Value->Elements.Num() == 1 && P.Value->Elements[0]->Text == TEXT("User.Petals"); }));
			TestTrue(TEXT("Renderer properties pass through"), Renderer.Properties.ContainsByPredicate(
				[](const FPropertyEntry& P) { return P.Name == TEXT("SortOrderHint"); }));
		}
	}

	// -- the data interface ---------------------------------------------------------------------
	const FParameterDecl* MeshParameter = System.Parameters.FindByPredicate([](const FParameterDecl& P) { return P.Name == TEXT("Petals"); });
	if (!TestNotNull(TEXT("mesh user parameter"), MeshParameter))
	{
		return false;
	}
	TestEqual(TEXT("data interface type"), MeshParameter->InnerTypeName, FString(TEXT("DynamicMesh")));
	TSharedPtr<FJsonObject> Json;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(MeshParameter->DefaultValue->Text);
	if (!TestTrue(TEXT("configuration is JSON"), FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid()))
	{
		return false;
	}
	const int32 Stride = DynamicMeshTexCoordsNeedVertexStride() ? 3 : 1;
	TestEqual(TEXT("NumVertices (times the UV workaround stride)"), Json->GetNumberField(TEXT("NumVertices")), 78.0 * Stride);
	TestEqual(TEXT("NumTexCoords"), Json->GetNumberField(TEXT("NumTexCoords")), 3.0);
	TestTrue(TEXT("tangent basis for explicit normals"), Json->GetBoolField(TEXT("bHasTangentBasis")));
	TestTrue(TEXT("colours by default"), Json->GetBoolField(TEXT("bHasColors")));
	const TArray<TSharedPtr<FJsonValue>>& Sections = Json->GetArrayField(TEXT("Sections"));
	const TArray<TSharedPtr<FJsonValue>>& Materials = Json->GetArrayField(TEXT("Materials"));
	if (TestEqual(TEXT("two sections"), Sections.Num(), 2) && TestEqual(TEXT("two materials"), Materials.Num(), 2))
	{
		TestEqual(TEXT("section 0 triangles (A + C)"), Sections[0]->AsObject()->GetNumberField(TEXT("NumTriangles")), 42.0);
		TestEqual(TEXT("section 1 triangles (B)"), Sections[1]->AsObject()->GetNumberField(TEXT("NumTriangles")), 48.0);
		TestEqual(TEXT("section 0 material"), Materials[0]->AsObject()->GetStringField(TEXT("Material")),
			FString(TEXT("/Engine/EngineDebugMaterials/VertexColorMaterial.VertexColorMaterial")));
		TestEqual(TEXT("section 1 material"), Materials[1]->AsObject()->GetStringField(TEXT("Material")),
			FString(TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial")));
	}
	const TSharedPtr<FJsonObject> Bounds = Json->GetObjectField(TEXT("DefaultBounds"));
	TestEqual(TEXT("bounds come from the block"), Bounds->GetObjectField(TEXT("Min"))->GetNumberField(TEXT("Y")), -20.0);

	// -- modules ----------------------------------------------------------------------------------
	TArray<FString> ModuleNames;
	for (const FDocument& Module : Lowering.Modules)
	{
		ModuleNames.Add(FPaths::GetCleanFilename(Module.Name));
		TestEqual(TEXT("module provenance is the .dfs"), Module.SourceFilePath, FString(TEXT("NS_DynamicMeshLowering.dfs")));
	}
	TestEqual(TEXT("modules"), FString::Join(ModuleNames, TEXT(",")),
		FString(TEXT("DM_GridTopology,DM_Visibility,DM_Petals_A,DM_Petals_B,DM_Petals_C")));
	TestTrue(TEXT("modules live next to the system"), Lowering.Modules.Num() > 0
		&& Lowering.Modules[0].Name == TEXT("DreamFXAutomation/NS_DynamicMeshLowering_DynamicMesh/DM_GridTopology"));
	if (Lowering.ModuleTexts.Num() == 5)
	{
		const FString& PatchA = Lowering.ModuleTexts[2];
		TestTrue(TEXT("the author's body is in the module"), PatchA.Contains(TEXT("Position = float3(U, V, Lift);")));
		// Patch A's body is written on one line; it must not land inside the generated comment line.
		TArray<FString> Lines;
		PatchA.ParseIntoArrayLines(Lines);
		const FString* BodyLine = Lines.FindByPredicate([](const FString& Line) { return Line.Contains(TEXT("Position = float3(U, V, Lift);")); });
		TestTrue(TEXT("a one-line body is code, not comment"), BodyLine != nullptr && !BodyLine->Contains(TEXT("//")));
		TestTrue(TEXT("inputs become module inputs"), PatchA.Contains(TEXT("float Lift;")));
		TestTrue(TEXT("explicit normals are written"), PatchA.Contains(TEXT("SetVertexTangentBasis")));
		TestEqual(TEXT("UV writes use the workaround stride"), PatchA.Contains(TEXT("DM_Vertex * 3")), Stride == 3);
		TestTrue(TEXT("only the declared UV channels exist"), PatchA.Contains(TEXT("float2 UV2")) && !PatchA.Contains(TEXT("float2 UV3")));
		TestTrue(TEXT("Strip's patch reads its own vertex base"), Lowering.ModuleTexts[4].Contains(TEXT("int DM_Vertex = 66 +")));
	}

	// -- identity ---------------------------------------------------------------------------------
	TestNotEqual(TEXT("the lowered system's hash covers the lowering"), System.SourceHash, Document.SourceHash);
	FDynamicMeshLowering Again;
	if (Lower(*this, Document, Again))
	{
		TestEqual(TEXT("lowering is deterministic"), Again.System.SourceHash, System.SourceHash);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXDynamicMeshInheritanceTest, "DreamFX.DynamicMesh.Inheritance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXDynamicMeshInheritanceTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::DynamicMeshTests;

	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT(".dreamfx-test-tmp")
		/ FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString ParentFile = Directory / TEXT("Base.dfs");
	const FString ChildFile = Directory / TEXT("Child.dfs");
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Directory, false, true); };

	FFileHelper::SaveStringToFile(FString(TEXT(R"(System(Name="DreamFXAutomation/Base", Root="Game")
{
    Emitter A { Settings = { SimTarget = CPU; } }
    DynamicMesh Shared { Patch P = Grid(3, 3) { } }
    Emitter B { Settings = { SimTarget = CPU; } }
    DynamicMesh Kept { Patch P = Grid(3, 3) { } }
})")), *ParentFile);
	const FString ChildText = TEXT(R"(System(Name="DreamFXAutomation/Child", Root="Game", Parent="Base.dfs")
{
    Emitter C { Settings = { SimTarget = CPU; } }
    DynamicMesh Added { Patch P = Strip(4) { } }
    DynamicMesh Shared { Patch Q = Tube(5, 3) { } }
})");
	FFileHelper::SaveStringToFile(ChildText, *ChildFile);

	FDocument Child;
	if (!Parse(*this, ChildText, ChildFile, Child))
	{
		return false;
	}
	FDocument Flat;
	FDiagnosticSink Diagnostics;
	if (!ResolveSystemInheritance(Child, Flat, Diagnostics))
	{
		AddError(Diagnostics.FormatAll());
		return false;
	}
	if (!TestEqual(TEXT("three blocks"), Flat.DynamicMeshes.Num(), 3))
	{
		return false;
	}
	TestEqual(TEXT("replaced block keeps its name"), Flat.DynamicMeshes[0].Name, FString(TEXT("Shared")));
	TestEqual(TEXT("replaced block takes the child's patches"), Flat.DynamicMeshes[0].Patches[0].Name, FString(TEXT("Q")));
	TestEqual(TEXT("replaced block keeps the parent's place"), Flat.DynamicMeshes[0].EmitterIndex, 1);
	TestEqual(TEXT("untouched block"), Flat.DynamicMeshes[1].EmitterIndex, 2);
	TestEqual(TEXT("added block goes after the child's new emitter"), Flat.DynamicMeshes[2].EmitterIndex, 3);

	FDynamicMeshLowering Lowering;
	if (Lower(*this, Flat, Lowering))
	{
		TArray<FString> Names;
		for (const FEmitter& Emitter : Lowering.System.Emitters)
		{
			Names.Add(Emitter.Name);
		}
		TestEqual(TEXT("lowered order"), FString::Join(Names, TEXT(",")),
			FString(TEXT("A,Shared_Q,Shared,B,Kept_P,Kept,C,Added_P,Added")));
		TestTrue(TEXT("modules are named after the child system"),
			Lowering.Modules.Num() > 0 && Lowering.Modules[0].Name.StartsWith(TEXT("DreamFXAutomation/Child_DynamicMesh/")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXDynamicMeshLintTest, "DreamFX.DynamicMesh.Lint",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXDynamicMeshLintTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::DynamicMeshTests;

	FDocument Document;
	if (!Parse(*this, TEXT(R"(System(Name="DreamFXAutomation/NS_DynamicMeshLint", Root="Game")
{
    DynamicMesh Sheet
    {
        Settings = { Material = "/Game/Nowhere/M_Missing"; TexCoords = 12; }
        Patch Body = Grid(4, 4) { }
    }
})"), TEXT("NS_DynamicMeshLint.dfs"), Document))
	{
		return false;
	}

	// Lint answers from the text: the malformed setting is reported, the material is not looked up.
	FDiagnosticSink LintDiagnostics;
	FLint::Run(Document, LintDiagnostics);
	const FString Lint = LintDiagnostics.FormatAll();
	TestTrue(TEXT("lint reports the setting"), Lint.Contains(TEXT("DFX3060")));
	TestFalse(TEXT("lint does not load assets"), Lint.Contains(TEXT("DFX3061")));

	// A build looks the material up.
	FDynamicMeshLowering Lowering;
	FDiagnosticSink BuildDiagnostics;
	TestFalse(TEXT("the build refuses"), LowerDynamicMeshes(Document, FDynamicMeshLowerOptions(), Lowering, BuildDiagnostics));
	TestTrue(TEXT("the build reports the material"), BuildDiagnostics.FormatAll().Contains(TEXT("DFX3061")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXDynamicMeshTexCoordProbeTest, "DreamFX.DynamicMesh.TexCoordProbe",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXDynamicMeshTexCoordProbeTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX::Editor;

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION <= 8
	// Every 5.8 build this was written against addresses texture coordinates as Vertex*2 + TexCoord*2.
	// If this fails the engine was fixed: delete the workaround's expectation here, keep the probe.
	TestTrue(TEXT("UE 5.8's dynamic mesh needs the texture coordinate stride workaround"),
		DynamicMeshTexCoordsNeedVertexStride());
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXDynamicMeshBodyErrorTest, "DreamFX.DynamicMesh.BodyError",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXDynamicMeshBodyErrorTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::DynamicMeshTests;

	// The engine logs its own compile failure at error level, in the editor's language.
	AddExpectedError(TEXT("Vector VM|向量VM"), EAutomationExpectedErrorFlags::Contains, 0);

	const FString Name = FString::Printf(TEXT("DreamFXAutomation/NS_DynamicMeshBodyError_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FDocument Document;
	if (!Parse(*this, FString::Printf(TEXT(R"(System(Name="%s", Root="Game")
{
    DynamicMesh Sheet
    {
        Patch Body = Grid(9, 9) {
            Position = float3(U, V);
        }
    }
})"), *Name), TEXT("NS_DynamicMeshBodyError.dfs"), Document))
	{
		return false;
	}

	FGenerateOptions Options;
	Options.bSave = false;
	Options.bForce = true;
	FDiagnosticSink Diagnostics;
	const FGenerateResult Result = FGenerator::Generate(Document, Options, Diagnostics);
	TestFalse(TEXT("a body that is not HLSL fails the build"), Result.bSucceeded);

	const FDiagnostic* Error = Diagnostics.GetDiagnostics().FindByPredicate(
		[](const FDiagnostic& Diagnostic) { return Diagnostic.Code == TEXT("DFX6006"); });
	if (TestNotNull(TEXT("DFX6006"), Error))
	{
		// Line 5, the `{` that opens the body, in the .dfs -- not a line of the generated module.
		TestEqual(TEXT("reported at the patch body"), Error->Location.Line, 5);
		TestEqual(TEXT("in the .dfs"), Error->File, FString(TEXT("NS_DynamicMeshBodyError.dfs")));
		TestTrue(TEXT("names the generated module's text"), Error->Message.Contains(TEXT("DM_Sheet_Body")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXDynamicMeshBuildTest, "DreamFX.DynamicMesh.Build",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXDynamicMeshBuildTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::DynamicMeshTests;

	const FString Name = FString::Printf(TEXT("DreamFXAutomation/NS_DynamicMesh_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FDocument Document;
	if (!Parse(*this, FString::Printf(TEXT(R"(System(Name="%s", Root="Game")
{
    DynamicMesh Shell
    {
        Settings = { TexCoords = 2; Normals = Auto; Bounds = box(-200, -200, -200, 200, 200, 200); }
        Patch Ball = Sphere(17, 9, Copies = 2)
        {
            Position = Dir * (100.0 + 20.0 * K + 5.0 * sin(T * 3.0 + Theta * 4.0));
            UV1 = float2(K, Rand);
        }
    }
})"), *Name), TEXT("NS_DynamicMeshBuild.dfs"), Document))
	{
		return false;
	}

	FGenerateOptions Options;
	Options.bSave = false;
	Options.bForce = true;
	FDiagnosticSink Diagnostics;
	const FGenerateResult Result = FGenerator::Generate(Document, Options, Diagnostics);
	if (!TestTrue(TEXT("the system builds and compiles"), Result.bSucceeded && Result.System != nullptr))
	{
		AddError(Diagnostics.FormatAll());
		return false;
	}
	FGCObjectScopeGuard Guard(Result.System);

	// The mesh is a user parameter holding a configured dynamic mesh data interface.
	UNiagaraDataInterface* Mesh = nullptr;
	TArray<FNiagaraVariable> UserParameters;
	Result.System->GetExposedParameters().GetUserParameters(UserParameters);
	for (const FNiagaraVariable& Variable : UserParameters)
	{
		if (Variable.GetName().ToString().EndsWith(TEXT("Shell")) && Variable.IsDataInterface())
		{
			Mesh = Result.System->GetExposedParameters().GetDataInterface(Variable);
		}
	}
	if (!TestNotNull(TEXT("User.Shell"), Mesh))
	{
		return false;
	}
	TestEqual(TEXT("dynamic mesh data interface"), Mesh->GetClass()->GetName(), FString(TEXT("NiagaraDataInterfaceDynamicMesh")));
	const int32 Stride = DynamicMeshTexCoordsNeedVertexStride() ? 2 : 1;
	if (const FIntProperty* NumVertices = FindFProperty<FIntProperty>(Mesh->GetClass(), TEXT("NumVertices")))
	{
		TestEqual(TEXT("NumVertices"), NumVertices->GetPropertyValue_InContainer(Mesh), 17 * 9 * 2 * Stride);
	}
	if (const FBoolProperty* Tangents = FindFProperty<FBoolProperty>(Mesh->GetClass(), TEXT("bHasTangentBasis")))
	{
		TestTrue(TEXT("tangent basis allocated for Normals = Auto"), Tangents->GetPropertyValue_InContainer(Mesh));
	}

	TArray<FString> Emitters;
	for (const FNiagaraEmitterHandle& Handle : Result.System->GetEmitterHandles())
	{
		Emitters.Add(Handle.GetName().ToString());
		if (Handle.GetName() == TEXT("Shell_Ball"))
		{
			const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
			TestTrue(TEXT("the writer is a GPU emitter"), Data != nullptr && Data->SimTarget == ENiagaraSimTarget::GPUComputeSim);
		}
	}
	TestEqual(TEXT("emitters"), FString::Join(Emitters, TEXT(",")), FString(TEXT("Shell_Ball,Shell")));
	return true;
}

#endif
