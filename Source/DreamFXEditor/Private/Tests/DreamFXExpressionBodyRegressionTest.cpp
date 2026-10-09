#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "DreamFXParser.h"
#include "EdGraph/EdGraphPin.h"
#include "Generation/DreamFXExpressions.h"
#include "Generation/DreamFXGenerator.h"
#include "Generation/DreamFXGraphSurgeon.h"
#include "Generation/DreamFXModuleGenerator.h"
#include "Generation/DreamFXValueLowering.h"
#include "NiagaraEmitter.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeCustomHlsl.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraScriptVariable.h"
#include "NiagaraSystem.h"
#include "UObject/GCObjectScopeGuard.h"
#include "UObject/UnrealType.h"

namespace UE::DreamFX::Editor::ExpressionBodyTests
{
	bool Parse(FAutomationTestBase& Test, const FString& Text, FDocument& Document)
	{
		FDiagnosticSink Diagnostics;
		if (!FParser::ParseText(Text, Text.StartsWith(TEXT("System")) ? TEXT("ExpressionBody.dfs") : TEXT("ExpressionBody.dfm"), Document, Diagnostics))
		{
			Test.AddError(Diagnostics.FormatAll()); return false;
		}
		return true;
	}

	FValuePtr Value(FAutomationTestBase& Test, const FString& Expression)
	{
		FDocument Document;
		return Parse(Test, TEXT("Module(Name=\"Value\",Root=\"Game\"){Inputs={int Probe=") + Expression + TEXT(";}Body={}}"), Document)
			? Document.Parameters[0].DefaultValue : nullptr;
	}

	FModuleGenerateResult Module(FAutomationTestBase& Test, const FString& Body, const FString& Inputs = FString(),
		bool bDynamic = false, bool bExpected = true, FDiagnosticSink* FailureDiagnostics = nullptr)
	{
		FDocument Document;
		const FString Name = TEXT("DreamFXAutomation/M_Body_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		if (!Parse(Test, FString::Printf(TEXT("%s(Name=\"%s\",Root=\"Game\"){Settings={Usage=%s;%s}Inputs={%s}Body={%s}}"),
			bDynamic ? TEXT("DynamicInput") : TEXT("Module"), *Name,
			bDynamic ? TEXT("DynamicInput") : TEXT("[ParticleSpawn,ParticleUpdate]"),
			bDynamic ? TEXT("Output=float;") : TEXT(""), *Inputs, *Body), Document)) { return {}; }
		FGenerateOptions Options; Options.bSave = false;
		FDiagnosticSink Diagnostics;
		FModuleGenerateResult Result = FModuleGenerator::Generate(Document, Options, Diagnostics);
		if (Result.bSucceeded != bExpected) { Test.AddError(Diagnostics.FormatAll()); }
		Test.TestEqual(TEXT("module generation result"), Result.bSucceeded, bExpected);
		if (FailureDiagnostics) { *FailureDiagnostics = MoveTemp(Diagnostics); }
		return Result;
	}

	const UNiagaraNodeCustomHlsl* Hlsl(FAutomationTestBase& Test, UNiagaraScript* Script)
	{
		const UNiagaraScriptSource* Source = Script ? Cast<UNiagaraScriptSource>(Script->GetLatestSource()) : nullptr;
		if (!Test.TestNotNull(TEXT("module graph source"), Source) || !Source->NodeGraph) { return nullptr; }
		for (const UEdGraphNode* Node : Source->NodeGraph->Nodes)
		{
			if (const auto* Custom = Cast<UNiagaraNodeCustomHlsl>(Node)) { return Custom; }
		}
		Test.AddError(TEXT("Missing custom HLSL node")); return nullptr;
	}

	FString Body(const UNiagaraNodeCustomHlsl* Hlsl)
	{
		const FStrProperty* Property = FindFProperty<FStrProperty>(Hlsl->GetClass(), TEXT("CustomHlsl"));
		return Property ? Property->GetPropertyValue_InContainer(Hlsl) : FString();
	}

	const UEdGraphPin* AttributePin(const UNiagaraNodeCustomHlsl* Hlsl, EEdGraphPinDirection Direction, const TCHAR* Attribute)
	{
		for (const UEdGraphPin* Pin : Hlsl->Pins)
		{
			if (Pin->Direction != Direction) { continue; }
			for (const UEdGraphPin* Linked : Pin->LinkedTo)
			{
				if (Linked && Linked->PinName == FName(Attribute)) { return Pin; }
			}
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXExpressionPrecisionAndCast,
	"DreamFX.Regression.Expressions.PrecisionAndIntCast",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXExpressionPrecisionAndCast::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::ExpressionBodyTests;
	for (double Number : { 1e-7, -1e-9, 1.2345678901234567, 1e20, 1.0 })
	{
		FValue Literal; Literal.Kind = EValueKind::Number; Literal.Number = Number;
		FDiagnosticSink Diagnostics;
		FString Rendered;
		TestTrue(TEXT("number renders"), FExpressions::Render(Literal, FNiagaraTypeDefinition::GetFloatDef(), TEXT("Probe"), Diagnostics, Rendered));
		TestEqual(TEXT("HLSL literal preserves source double precision"), FCString::Atod(*Rendered), Number);
		TestTrue(TEXT("number remains a floating literal"), Rendered.Contains(TEXT(".")) || Rendered.Contains(TEXT("e")) || Rendered.Contains(TEXT("E")));
	}
	{
		// Exact is not enough: the literal lands in generated HLSL and in every export of it.
		FValue Tenth; Tenth.Kind = EValueKind::Number; Tenth.Number = 0.1;
		FDiagnosticSink TenthDiagnostics;
		FString TenthText;
		TestTrue(TEXT("short decimal renders"), FExpressions::Render(Tenth, FNiagaraTypeDefinition::GetFloatDef(), TEXT("Probe"), TenthDiagnostics, TenthText));
		TestEqual(TEXT("short decimal keeps its shortest exact spelling"), TenthText, FString(TEXT("0.1")));
	}
	FDiagnosticSink Diagnostics;
	FValuePtr Cast = Value(*this, TEXT("int(-1.9)"));
	if (!Cast) { return false; }
	FNiagaraTypeDefinition Type;
	TestTrue(TEXT("cast has inferable int type"), FValueLowering::InferType(*Cast, TEXT("Probe"), Diagnostics, Type));
	TestTrue(TEXT("cast infers int"), Type == FNiagaraTypeDefinition::GetIntDef());
	FInputValue Literal;
	TestTrue(TEXT("literal cast is accepted as a default"), FValueLowering::Lower(*Cast, Type, TEXT("Probe"), Diagnostics, Literal));
	int32 Integer = 0;
	if (TestEqual(TEXT("int literal byte size"), Literal.LiteralBytes.Num(), int32(sizeof(Integer))))
	{
		FMemory::Memcpy(&Integer, Literal.LiteralBytes.GetData(), sizeof(Integer));
		TestEqual(TEXT("explicit narrowing truncates toward zero"), Integer, -1);
	}
	Cast = Value(*this, TEXT("int(User.Gain * 2.0)"));
	FString Rendered;
	if (!Cast) { return false; }
	TestTrue(TEXT("runtime cast takes the builtin path"), FExpressions::RequiresHlslLowering(*Cast));
	TestTrue(TEXT("runtime cast renders"), FExpressions::Render(*Cast, Type, TEXT("Probe"), Diagnostics, Rendered));
	TestTrue(TEXT("runtime conversion remains in HLSL"), Rendered.StartsWith(TEXT("int(")) && Rendered.Contains(TEXT("User.Gain")));
	for (const TCHAR* Invalid : { TEXT("int(2147483648.0)"), TEXT("int(-2147483649.0)"), TEXT("int(User.Gain)") })
	{
		Cast = Value(*this, Invalid); if (!Cast) { return false; }
		Diagnostics.Reset();
		TestFalse(TEXT("invalid or runtime defaults rejected"), FValueLowering::Lower(*Cast, Type, TEXT("Probe"), Diagnostics, Literal));
		TestTrue(TEXT("cast default rejection is actionable"), Diagnostics.FormatAll().Contains(TEXT("DFX4044")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXBodyLexicalBindings,
	"DreamFX.Regression.Generation.BodyLexicalBindings",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXBodyLexicalBindings::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::ExpressionBodyTests;
	const auto Generated = Module(*this, TEXT(R"(
        // float Particles.CommentOnly = 500.0;
        /* declaration separated by trivia */ float/* gap */Particles.A.B=11.0;
        float
        Particles.A_B=17.0;
        { float Write_Particles_SpriteRotation=3.0; }
        if(Apply){Particles.SpriteRotation=Particles.A.B+Particles.A_B;}
    )"), TEXT("bool Apply=false; float Write_Particles_A_B=0.0;"));
	if (!Generated.bSucceeded || !Generated.Script) { return false; }
	FGCObjectScopeGuard Guard(Generated.Script);
	const auto* Custom = Hlsl(*this, Generated.Script); if (!Custom) { return false; }
	const auto* A = AttributePin(Custom, EGPD_Output, TEXT("Particles.A.B"));
	const auto* B = AttributePin(Custom, EGPD_Output, TEXT("Particles.A_B"));
	if (!TestNotNull(TEXT("dotted custom output"), A) || !TestNotNull(TEXT("underscore custom output"), B)) { return false; }
	TestNotEqual(TEXT("distinct attributes get distinct HLSL symbols"), A->PinName, B->PinName);
	TestNotEqual(TEXT("generated symbol avoids a module input"), A->PinName, FName(TEXT("Write_Particles_A_B")));
	// The engine's pin uniquing (direct backend) renames a pin whose name differs from an existing one
	// only by an FName number; the body must still name the pins that actually exist.
	for (const UEdGraphPin* Pin : { A, B })
	{
		TestTrue(FString::Printf(TEXT("body names created pin '%s'"), *Pin->PinName.ToString()),
			Body(Custom).Contains(Pin->PinName.ToString()));
	}
	TestNull(TEXT("new custom attribute does not read uninitialized parameter map data"), AttributePin(Custom, EGPD_Input, TEXT("Particles.A.B")));
	TestNull(TEXT("comment declaration is not a real attribute"), AttributePin(Custom, EGPD_Output, TEXT("Particles.CommentOnly")));
	const auto* Read = AttributePin(Custom, EGPD_Input, TEXT("Particles.SpriteRotation"));
	const auto* Write = AttributePin(Custom, EGPD_Output, TEXT("Particles.SpriteRotation"));
	if (!TestNotNull(TEXT("conditional whole write reads old value"), Read) || !TestNotNull(TEXT("conditional write output"), Write)) { return false; }
	TestNotEqual(TEXT("generated symbol avoids a block local"), Write->PinName, FName(TEXT("Write_Particles_SpriteRotation")));
	TestTrue(TEXT("old value seeds the output before branching"), Body(Custom).Contains(Write->PinName.ToString() + TEXT(" = ") + Read->PinName.ToString() + TEXT(";")));

	// Compiling a consuming system catches duplicate signature names and uninitialized out pins.
	FDocument System;
	if (!Parse(*this, FString::Printf(TEXT("System(Name=\"DreamFXAutomation/NS_Body_%s\",Root=\"Game\"){Emitter E {ParticleSpawn={Particles.SpriteRotation=0.75; `%s`(Apply=false);} SpriteRenderer R {}}}"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits), *Generated.AssetPath), System)) { return false; }
	FGenerateOptions Options; Options.bSave = false;
	FDiagnosticSink Diagnostics;
	const FGenerateResult Compiled = FGenerator::Generate(System, Options, Diagnostics);
	if (!TestTrue(TEXT("conditional and colliding attributes compile into a consumer VM"), Compiled.bSucceeded)) { AddError(Diagnostics.FormatAll()); return false; }
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXBodyWriteOperators,
	"DreamFX.Regression.Generation.BodyWriteOperators",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXBodyWriteOperators::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX::Editor::ExpressionBodyTests;
	for (const TCHAR* Source : { TEXT("Particles.MeshIndex++;"), TEXT("++Particles.MeshIndex;"),
		TEXT("Particles.MeshIndex--;"), TEXT("--(Particles.MeshIndex);"), TEXT("(Particles.MeshIndex) += 1;"),
		TEXT("Particles.MeshIndex &= 3;"), TEXT("Particles.MeshIndex |= 3;"),
		TEXT("Particles.MeshIndex ^= 3;"), TEXT("Particles.MeshIndex <<= 1;"), TEXT("Particles.MeshIndex >>= 1;") })
	{
		const auto Generated = Module(*this, Source);
		if (!Generated.bSucceeded) { return false; }
		const auto* Custom = Hlsl(*this, Generated.Script); if (!Custom) { return false; }
		TestNotNull(FString::Printf(TEXT("%s reads old value"), Source), AttributePin(Custom, EGPD_Input, TEXT("Particles.MeshIndex")));
		TestNotNull(FString::Printf(TEXT("%s writes back"), Source), AttributePin(Custom, EGPD_Output, TEXT("Particles.MeshIndex")));
	}
	const auto Indexed = Module(*this, TEXT("Particles.Color[0] /* assignment trivia */ += 0.5;"));
	const auto* Custom = Hlsl(*this, Indexed.Script); if (!Custom) { return false; }
	TestNotNull(TEXT("indexed write reads untouched components"), AttributePin(Custom, EGPD_Input, TEXT("Particles.Color")));
	TestNotNull(TEXT("indexed write writes full attribute"), AttributePin(Custom, EGPD_Output, TEXT("Particles.Color")));
	const auto Remainder = Module(*this, TEXT("Particles.SpriteRotation %= 3.0;"));
	Custom = Hlsl(*this, Remainder.Script); if (!Custom) { return false; }
	TestNotNull(TEXT("remainder assignment reads old value"), AttributePin(Custom, EGPD_Input, TEXT("Particles.SpriteRotation")));
	TestNotNull(TEXT("remainder assignment writes back"), AttributePin(Custom, EGPD_Output, TEXT("Particles.SpriteRotation")));
	const auto Comparison = Module(*this, TEXT("bool Same=Particles.MeshIndex==1; bool Different=Particles.MeshIndex!=2; bool Less=Particles.MeshIndex<=3; bool More=Particles.MeshIndex>=4;"));
	Custom = Hlsl(*this, Comparison.Script); if (!Custom) { return false; }
	TestNull(TEXT("comparison operators never become writes"), AttributePin(Custom, EGPD_Output, TEXT("Particles.MeshIndex")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXBodyReturnsAndInitialization,
	"DreamFX.Regression.Generation.BodyReturnsAndInitialization",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXBodyReturnsAndInitialization::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor::ExpressionBodyTests;
	for (const TCHAR* Source : { TEXT("return Particles.SpriteRotation;"), TEXT("return/* separator */Particles.SpriteRotation;"),
		TEXT("// comment\nParticles.SpriteRotation;") })
	{
		const auto Generated = Module(*this, Source, FString(), true);
		if (!Generated.bSucceeded) { return false; }
		const auto* Custom = Hlsl(*this, Generated.Script); if (!Custom) { return false; }
		TestNotNull(TEXT("return expression retains its parameter read"), AttributePin(Custom, EGPD_Input, TEXT("Particles.SpriteRotation")));
		TestFalse(TEXT("return keyword stripped before Niagara expression wrapping"), Body(Custom).Contains(TEXT("return")));
	}
	for (const TCHAR* Invalid : { TEXT("if(Apply){float Particles.New=1.0;}"), TEXT("float Particles.New=Particles.New+1.0;"),
		TEXT("if(Apply){return;} float Particles.New=1.0;"), TEXT("float Particles.New=1.0; int Particles.New=2;") })
	{
		FDiagnosticSink Diagnostics;
		Module(*this, Invalid, TEXT("bool Apply=false;"), false, false, &Diagnostics);
		TestTrue(TEXT("unsafe initialization is diagnosed before graph mutation"), Diagnostics.FormatAll().Contains(TEXT("DFX3057")));
	}
	const auto Imported = Module(*this, TEXT("float Particles.Existing; if(Apply){Particles.Existing=1.0;}"), TEXT("bool Apply=false;"));
	const auto* Custom = Hlsl(*this, Imported.Script); if (!Custom) { return false; }
	TestNotNull(TEXT("typed incoming custom attribute keeps its old value"), AttributePin(Custom, EGPD_Input, TEXT("Particles.Existing")));
	const auto Initialized = Module(*this, TEXT("float Particles.New=1.0; if(Apply){Particles.New+=2.0;}"), TEXT("bool Apply=false;"));
	Custom = Hlsl(*this, Initialized.Script); if (!Custom) { return false; }
	TestNull(TEXT("initialized custom attribute does not need incoming data"), AttributePin(Custom, EGPD_Input, TEXT("Particles.New")));
	const auto Preprocessed = Module(*this, TEXT("\n#if 0\nfloat Unused=0;\nParticles.SpriteRotation=30.0;\n#endif\n"));
	Custom = Hlsl(*this, Preprocessed.Script); if (!Custom) { return false; }
	TestNotNull(TEXT("preprocessor-conditional assignment retains incoming value"), AttributePin(Custom, EGPD_Input, TEXT("Particles.SpriteRotation")));
	TestTrue(TEXT("preprocessor-conditional output is initialized outside the directive"), Body(Custom).StartsWith(TEXT("Write_Particles_SpriteRotation = Read_Particles_SpriteRotation;")));
	const auto Unbraced = Module(*this, TEXT("if(Apply)\n#if 0\nParticles.Color=float4(1,1,1,1);\n#endif\nParticles.SpriteRotation=30.0;"), TEXT("bool Apply=false;"));
	Custom = Hlsl(*this, Unbraced.Script); if (!Custom) { return false; }
	TestNotNull(TEXT("endif does not make an unbraced conditional assignment unconditional"), AttributePin(Custom, EGPD_Input, TEXT("Particles.SpriteRotation")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXExpressionVmRegression,
	"DreamFX.Regression.Expressions.VmCastsAndPrecision",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXExpressionVmRegression::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::ExpressionBodyTests;
	FDocument Document;
	if (!Parse(*this, FString::Printf(TEXT(R"(System(Name="DreamFXAutomation/NS_Expressions_%s",Root="Game") {
        Properties={float Gain=1.0; int Count=int(-1.9);}
        Emitter E {
            Defaults={int Particles.DefaultCount=int(2.9);}
            ParticleSpawn={
                Particles.CastResult=int(User.Gain*2.0);
                float Particles.SpriteRotation=User.Gain*1e-7;
                int Particles.LiteralCount=int(3.9);
            }
            SpriteRenderer R {}
        }
    })"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)), Document)) { return false; }
	FGenerateOptions Options; Options.bSave = false;
	FDiagnosticSink Diagnostics;
	const FGenerateResult Result = FGenerator::Generate(Document, Options, Diagnostics);
	if (!TestTrue(TEXT("system casts and defaults compile"), Result.bSucceeded) || !Result.System)
	{
		AddError(Diagnostics.FormatAll()); return false;
	}
	const FVersionedNiagaraEmitterData* Emitter = Result.System->GetEmitterHandles()[0].GetEmitterData();
	if (!TestNotNull(TEXT("generated emitter data"), Emitter) || !Emitter->SpawnScriptProps.Script) { return false; }
	const FNiagaraVMExecutableData& VM = Emitter->SpawnScriptProps.Script->GetVMExecutableData();
	TestTrue(TEXT("runtime casts have executable VM bytecode"), VM.HasByteCode());
	bool bFoundCoefficient = false;
	TArray<FString> FloatConstants;
	for (const FNiagaraVariable& Parameter : VM.InternalParameters.Parameters)
	{
		if (Parameter.GetType() == FNiagaraTypeDefinition::GetFloatDef() && Parameter.IsDataAllocated())
		{
			const float Number = Parameter.GetValue<float>();
			bFoundCoefficient |= Number == 1e-7f;
			FloatConstants.Add(FString::Printf(TEXT("%s=%.17g"), *Parameter.GetName().ToString(), static_cast<double>(Number)));
		}
	}
	if (!bFoundCoefficient) { AddInfo(TEXT("Compiled VM float constants: ") + FString::Join(FloatConstants, TEXT(", "))); }
	TestTrue(TEXT("compiled coefficient remains nonzero at float precision"), bFoundCoefficient);
	const auto InputDefault = Module(*this, TEXT("Particles.MeshIndex=Count;"), TEXT("int Count=int(1.9);"));
	TestTrue(TEXT("module input defaults accept a checked literal cast"), InputDefault.bSucceeded);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXGeneratedMapPinDefaults,
	"DreamFX.Regression.Generation.MapPinDefaults",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXGeneratedMapPinDefaults::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::ExpressionBodyTests;
	const auto Generated = Module(*this, TEXT("Particles.SpriteRotation += Rate;"), TEXT("float Rate=90.0;"));
	if (!Generated.bSucceeded || !Generated.Script) { return false; }
	FGCObjectScopeGuard Guard(Generated.Script);
	auto* Source = Cast<UNiagaraScriptSource>(Generated.Script->GetLatestSource());
	if (!TestNotNull(TEXT("module source"), Source) || !Source->NodeGraph) { return false; }
	UNiagaraGraph* Graph = Source->NodeGraph;
	TestNotNull(TEXT("particle attribute metadata is present before reload"), Graph->GetScriptVariable(FName(TEXT("Particles.SpriteRotation"))));
	int32 MapGetCount = 0;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node->GetClass()->GetFName() != FName(TEXT("NiagaraNodeParameterMapGet"))) { continue; }
		++MapGetCount;
		FMapProperty* Mapping = FindFProperty<FMapProperty>(Node->GetClass(), TEXT("PinOutputToPinDefaultPersistentId"));
		if (!TestNotNull(TEXT("default pin GUID mapping"), Mapping)) { return false; }
		FScriptMapHelper Ids(Mapping, Mapping->ContainerPtrToValuePtr<void>(Node));
		int32 Outputs = 0;
		int32 Inputs = 0;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->Direction == EGPD_Input) { ++Inputs; continue; }
			if (Pin->PinType.PinSubCategory == FName(TEXT("DynamicAddPin"))) { continue; }
			++Outputs;
			TestEqual(TEXT("map output has parameter category"), Pin->PinType.PinSubCategory, FName(TEXT("ParameterPin")));
			const uint8* Id = Ids.FindValueFromHash(&Pin->PersistentGuid);
			if (!TestNotNull(TEXT("output has a default pin mapping"), Id)) { return false; }
			const UEdGraphPin* Default = nullptr;
			for (const UEdGraphPin* Candidate : Node->Pins)
			{
				if (Candidate->Direction == EGPD_Input && Candidate->PersistentGuid == *reinterpret_cast<const FGuid*>(Id))
				{
					Default = Candidate; break;
				}
			}
			if (!TestNotNull(TEXT("mapped default pin exists"), Default)) { return false; }
			if (Pin->PinName == FName(TEXT("Module.Rate")))
			{
				TestEqual(TEXT("declared input default is on the actual graph pin"), FCString::Atod(*Default->DefaultValue), 90.0);
			}
			else
			{
				TestTrue(TEXT("existing particle attribute has no fallback value"), Default->bHidden);
			}
		}
		TestEqual(TEXT("exactly one default pin per read, plus the map input"), Inputs, Outputs + 1);
		TestEqual(TEXT("no extra default GUID entries"), Ids.Num(), Outputs);
	}
	TestEqual(TEXT("one map get node"), MapGetCount, 1);
	FNiagaraVMExecutableDataId Before;
	Generated.Script->ComputeVMCompilationId(Before, FGuid());
	FString Error;
	TUniquePtr<FGraphSurgeon> Surgeon = FGraphSurgeon::Create(Error);
	if (!TestTrue(TEXT("graph backend available"), Surgeon.IsValid())) { return false; }
	TestTrue(TEXT("finalizing existing complete pins succeeds"), Surgeon->FinalizeParameterMapPins(*Graph, Error));
	FNiagaraVMExecutableDataId After;
	Generated.Script->ComputeVMCompilationId(After, FGuid());
	FString BeforeKey, AfterKey;
	Before.AppendKeyString(BeforeKey);
	After.AppendKeyString(AfterKey);
	TestEqual(TEXT("completing an already normalized graph leaves its compile key unchanged"), AfterKey, BeforeKey);
	return true;
}

#endif
