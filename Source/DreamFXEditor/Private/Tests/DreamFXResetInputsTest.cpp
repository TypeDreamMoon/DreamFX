#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Adapter/DreamFXNiagaraAdapter.h"
#include "Diff/DreamFXAssetFacts.h"
#include "DreamFXParser.h"
#include "DreamFXTypes.h"
#include "Generation/DreamFXGenerator.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "UObject/GCObjectScopeGuard.h"

/**
 * Resetting an input to its default (FNiagaraAdapter::ResetInputs).
 *
 * The claim worth testing is the one an incremental build relies on: after an input is reset, the system
 * is the system a build of the same source without that argument would have made. So the cases that can
 * say so build both -- the overridden source, reset, against the plain source, fresh -- and compare what
 * DescribeSystemFacts can see of each. The other half of the claim is that the comparison could have
 * failed, so each of them also checks that the two differ before the reset.
 */
namespace UE::DreamFX::Editor::ResetInputsTests
{
	using namespace UE::DreamFX;

	FString UniqueName(const TCHAR* Prefix)
	{
		return FString::Printf(TEXT("DreamFXAutomation/%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	}

	/** Builds `System(...) { Body }` into a throwaway in-memory asset. */
	UNiagaraSystem* BuildSystem(FAutomationTestBase& Test, const FString& Body)
	{
		FDocument Document;
		FDiagnosticSink Diagnostics;
		const FString Source = FString::Printf(TEXT("System(Name=\"%s\",Root=\"Game\") { %s }"),
			*UniqueName(TEXT("NS_Reset")), *Body);
		if (!FParser::ParseText(Source, TEXT("ResetInputs.dfs"), Document, Diagnostics))
		{
			Test.AddError(Diagnostics.FormatAll());
			return nullptr;
		}

		FGenerateOptions Options;
		Options.bSave = false;
		const FGenerateResult Result = FGenerator::Generate(Document, Options, Diagnostics);
		if (!Result.bSucceeded || Result.System == nullptr)
		{
			Test.AddError(FString::Printf(TEXT("Build failed:\n%s"), *Diagnostics.FormatAll()));
			return nullptr;
		}
		return Result.System;
	}

	/** One emitter holding the given statements, plus a user parameter for links to point at. */
	FString EmitterBody(const FString& EmitterUpdate, const FString& ParticleSpawn, const FString& ParticleUpdate)
	{
		return TEXT("Properties = { Vector Scale = (2, 2, 2); } Emitter E { ")
			TEXT("EmitterUpdate = { EmitterState(); ") + EmitterUpdate + TEXT(" } ")
			TEXT("ParticleSpawn = { SystemLocation(); ") + ParticleSpawn + TEXT(" } ")
			TEXT("ParticleUpdate = { ParticleState(); ") + ParticleUpdate + TEXT(" SolveForcesAndVelocity(); } ")
			TEXT("SpriteRenderer Core {} }");
	}

	/** The address of one stack's script, system scope when Emitter is empty. */
	FStackAddress StackOf(UNiagaraSystem* System, const TCHAR* Emitter, EStackKind Kind)
	{
		FStackAddress Address(System);
		if (Emitter != nullptr && *Emitter != 0)
		{
			Address = Address.WithEmitter(FName(Emitter));
		}
		return Address.WithScript(FNiagaraAdapter::ScriptNameForStack(Kind));
	}

	/**
	 * The address of an input on the module that was added from a given script asset.
	 *
	 * Found by asking the stack, not by guessing the node name the engine gave the module: a module's
	 * name carries a counter that depends on what else sits in the graph.
	 */
	FStackAddress InputOf(FAutomationTestBase& Test, UNiagaraSystem* System, const TCHAR* Emitter, EStackKind Kind,
		const TCHAR* ModuleAsset, std::initializer_list<const TCHAR*> InputPath)
	{
		const FStackAddress Stack = StackOf(System, Emitter, Kind);
		FScriptStackInfo Info;
		TArray<FString> Errors;
		if (!FNiagaraAdapter::GetScriptStackInfo(Stack, Info, Errors))
		{
			Test.AddError(FString::Join(Errors, TEXT(" | ")));
			return Stack;
		}

		for (const FModuleInfo& Module : Info.Modules)
		{
			if (Module.Script != nullptr && Module.Script->GetName() == ModuleAsset)
			{
				FStackAddress Address = Stack.WithModule(Module.ModuleName);
				for (const TCHAR* Input : InputPath)
				{
					Address = Address.WithInput(FName(Input));
				}
				return Address;
			}
		}
		Test.AddError(FString::Printf(TEXT("No module added from '%s' in the stack."), ModuleAsset));
		return Stack;
	}

	/** Compiles from the live graphs, the way a build's final step does, so the compiled facts are current. */
	bool Recompile(FAutomationTestBase& Test, UNiagaraSystem* System)
	{
		FNiagaraAdapter::InvalidateCachedCompileIds(System);
		FNiagaraAdapter::RequestCompileAsync(System, /*bForce=*/true);
		FCompileStateInfo State;
		TArray<FString> Errors;
		const bool bCompiled = FNiagaraAdapter::WaitAndCollect(System, /*bIncludingGpuShaders=*/false, State, Errors);
		if (!bCompiled || State.bHasErrors)
		{
			Test.AddError(FString::Printf(TEXT("Recompile failed: %s"), *FString::Join(Errors, TEXT(" | "))));
			return false;
		}
		return true;
	}

	/** The facts of each side that the other lacks, as a multiset difference. */
	void DifferingFacts(UNiagaraSystem* Left, UNiagaraSystem* Right, TArray<FString>& OutOnlyLeft, TArray<FString>& OutOnlyRight)
	{
		TArray<FString> LeftFacts;
		TArray<FString> RightFacts;
		DescribeSystemFacts(Left, LeftFacts);
		DescribeSystemFacts(Right, RightFacts);

		TMap<FString, int32> Counts;
		for (const FString& Fact : LeftFacts)
		{
			++Counts.FindOrAdd(Fact);
		}
		for (const FString& Fact : RightFacts)
		{
			int32& Count = Counts.FindOrAdd(Fact);
			if (Count > 0)
			{
				--Count;
			}
			else
			{
				OutOnlyRight.Add(Fact);
			}
		}
		for (const TPair<FString, int32>& Entry : Counts)
		{
			for (int32 Copy = 0; Copy < Entry.Value; ++Copy)
			{
				OutOnlyLeft.Add(Entry.Key);
			}
		}
	}

	void ExpectSameFacts(FAutomationTestBase& Test, const TCHAR* What, UNiagaraSystem* Reset, UNiagaraSystem* Fresh)
	{
		TArray<FString> OnlyReset;
		TArray<FString> OnlyFresh;
		DifferingFacts(Reset, Fresh, OnlyReset, OnlyFresh);

#if !DREAMFX_HAS_NIAGARA_FAST_EDIT
		// The stock engine exports no stale-constant cleanup, so the rapid-iteration constants of the
		// nodes a reset took away -- a dynamic input's, a hidden input's -- stay in the scripts' stores.
		// A freshly added module never had them. They are inert, and the full rebuild leaves the same
		// kind behind on this engine, so here the reset has to match the fresh build in everything
		// else: nothing only on the fresh side, and nothing extra but those constants.
		OnlyReset.RemoveAll([](const FString& Fact) { return Fact.StartsWith(TEXT("ri ")); });
#endif

		if (OnlyReset.IsEmpty() && OnlyFresh.IsEmpty())
		{
			return;
		}

		FString Report;
		for (int32 Index = 0; Index < FMath::Min(OnlyReset.Num(), 8); ++Index)
		{
			Report += FString::Printf(TEXT("\n  reset only | %s"), *OnlyReset[Index].Left(260));
		}
		for (int32 Index = 0; Index < FMath::Min(OnlyFresh.Num(), 8); ++Index)
		{
			Report += FString::Printf(TEXT("\n  fresh only | %s"), *OnlyFresh[Index].Left(260));
		}
		Test.AddError(FString::Printf(TEXT("%s: %d fact(s) only on the reset system, %d only on the fresh one.%s"),
			What, OnlyReset.Num(), OnlyFresh.Num(), *Report));
	}

	void ExpectDifferentFacts(FAutomationTestBase& Test, const TCHAR* What, UNiagaraSystem* Left, UNiagaraSystem* Right)
	{
		TArray<FString> OnlyLeft;
		TArray<FString> OnlyRight;
		DifferingFacts(Left, Right, OnlyLeft, OnlyRight);
		Test.TestTrue(FString::Printf(TEXT("%s: the overrides show in the facts before the reset, so comparing after it means something"), What),
			!OnlyLeft.IsEmpty() || !OnlyRight.IsEmpty());
	}

	bool ReadInput(FAutomationTestBase& Test, const FStackAddress& Address, FInputValue& OutValue)
	{
		TArray<FString> Errors;
		if (!FNiagaraAdapter::GetInput(Address, OutValue, Errors))
		{
			Test.AddError(FString::Printf(TEXT("Could not read an input: %s"), *FString::Join(Errors, TEXT(" | "))));
			return false;
		}
		return true;
	}

	/** Resets, reports a failure with its errors, and recompiles so the facts that follow are current. */
	bool ResetAndRecompile(FAutomationTestBase& Test, const TCHAR* What, UNiagaraSystem* System, TArrayView<const FStackAddress> Addresses)
	{
		TArray<FString> Errors;
		const bool bReset = FNiagaraAdapter::ResetInputs(System, Addresses, Errors);
		if (!bReset)
		{
			Test.AddError(FString::Printf(TEXT("%s: the reset failed: %s"), What, *FString::Join(Errors, TEXT(" | "))));
			return false;
		}
		Test.TestEqual(*FString::Printf(TEXT("%s: a successful reset reports nothing"), What), Errors.Num(), 0);
		return Recompile(Test, System);
	}

	/** Whether one reset call fails, with an error that says why. */
	void ExpectRefusal(FAutomationTestBase& Test, const TCHAR* What, UNiagaraSystem* System, const FStackAddress& Address, const TCHAR* ExpectedText)
	{
		TArray<FString> Errors;
		const bool bReset = FNiagaraAdapter::ResetInputs(System, MakeArrayView(&Address, 1), Errors);
		Test.TestFalse(*FString::Printf(TEXT("%s: refused"), What), bReset);
		const FString Joined = FString::Join(Errors, TEXT(" | "));
		Test.TestTrue(*FString::Printf(TEXT("%s: the error says why (wanted '%s', got '%s')"), What, ExpectedText, *Joined),
			Joined.Contains(ExpectedText));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXResetInputsFreshBuildTest,
	"DreamFX.Regression.Adapter.ResetInputsMatchFreshBuild",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXResetInputsFreshBuildTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::ResetInputsTests;

	// A literal (Gravity), a dynamic input with a chain (SpawnRate), a link over a default that is itself a
	// link (ApplyOwnerScaleToAttributes reads Engine.Owner.Scale until told otherwise), and a static switch
	// with the inputs it gates (the lifetime mode of InitializeParticle).
	const FString Overridden = EmitterBody(
		TEXT("SpawnRate(SpawnRate = RandomRangeFloat(Minimum = 2.0, Maximum = 6.0));"),
		TEXT("Spawn/Initialization/V2/InitializeParticle(LifetimeMode = Random, LifetimeMin = 0.25, LifetimeMax = 0.55);"),
		TEXT("GravityForce(Gravity = (0, 0, -100)); ApplyOwnerScaleToAttributes(OwnerScale = User.Scale);"));
	const FString Plain = EmitterBody(
		TEXT("SpawnRate();"),
		TEXT("Spawn/Initialization/V2/InitializeParticle();"),
		TEXT("GravityForce(); ApplyOwnerScaleToAttributes();"));

	UNiagaraSystem* Subject = BuildSystem(*this, Overridden);
	UNiagaraSystem* Reference = BuildSystem(*this, Plain);
	if (!TestNotNull(TEXT("overridden system"), Subject) || !TestNotNull(TEXT("plain system"), Reference)) { return false; }
	FGCObjectScopeGuard SubjectGuard(Subject);
	FGCObjectScopeGuard ReferenceGuard(Reference);
	ExpectDifferentFacts(*this, TEXT("overridden vs plain"), Subject, Reference);

	// The switch is listed first on purpose: the batch has to reset what it gates before it.
	const FStackAddress LifetimeMode = InputOf(*this, Subject, TEXT("E"), EStackKind::ParticleSpawn, TEXT("InitializeParticle"), { TEXT("Lifetime Mode") });
	const FStackAddress LifetimeMin = InputOf(*this, Subject, TEXT("E"), EStackKind::ParticleSpawn, TEXT("InitializeParticle"), { TEXT("Lifetime Min") });
	const FStackAddress LifetimeMax = InputOf(*this, Subject, TEXT("E"), EStackKind::ParticleSpawn, TEXT("InitializeParticle"), { TEXT("Lifetime Max") });
	const FStackAddress SpawnRate = InputOf(*this, Subject, TEXT("E"), EStackKind::EmitterUpdate, TEXT("SpawnRate"), { TEXT("SpawnRate") });
	const FStackAddress Gravity = InputOf(*this, Subject, TEXT("E"), EStackKind::ParticleUpdate, TEXT("GravityForce"), { TEXT("Gravity") });
	const FStackAddress OwnerScale = InputOf(*this, Subject, TEXT("E"), EStackKind::ParticleUpdate, TEXT("ApplyOwnerScaleToAttributes"), { TEXT("Owner Scale") });

	FInputValue Value;
	if (ReadInput(*this, OwnerScale, Value))
	{
		TestTrue(TEXT("Owner Scale is linked to the user parameter before the reset"),
			Value.Mode == EInputValueMode::Linked && Value.LinkedVariable.GetName() == TEXT("User.Scale"));
	}
	if (ReadInput(*this, SpawnRate, Value))
	{
		TestTrue(TEXT("SpawnRate is a dynamic input before the reset"), Value.Mode == EInputValueMode::DynamicInput);
	}

	const FStackAddress Batch[] = { LifetimeMode, LifetimeMin, LifetimeMax, SpawnRate, Gravity, OwnerScale };
	FNiagaraAdapter::ResetStats();
	TArray<FString> Errors;
	if (!TestTrue(TEXT("the mixed batch resets"), FNiagaraAdapter::ResetInputs(Subject, Batch, Errors)))
	{
		AddError(FString::Join(Errors, TEXT(" | ")));
		return false;
	}
	TestTrue(TEXT("six addresses cost one view model"), FNiagaraAdapter::ReportStats().Contains(TEXT("adapter: 1 view model(s) built")));
	if (!Recompile(*this, Subject)) { return false; }

	if (ReadInput(*this, OwnerScale, Value))
	{
		TestTrue(TEXT("Owner Scale is back on its default link"),
			Value.Mode == EInputValueMode::Linked && Value.LinkedVariable.GetName() == TEXT("Engine.Owner.Scale"));
	}
	if (ReadInput(*this, SpawnRate, Value))
	{
		TestTrue(TEXT("SpawnRate is no longer a dynamic input"), Value.Mode != EInputValueMode::DynamicInput);
	}
	ExpectSameFacts(*this, TEXT("mixed batch vs fresh build"), Subject, Reference);

	// A second reset of the same inputs finds them at their defaults and changes nothing.
	Errors.Reset();
	TestTrue(TEXT("resetting inputs that are already at their defaults succeeds"), FNiagaraAdapter::ResetInputs(Subject, Batch, Errors));
	TestEqual(TEXT("and reports nothing"), Errors.Num(), 0);
	if (Recompile(*this, Subject))
	{
		ExpectSameFacts(*this, TEXT("second reset vs fresh build"), Subject, Reference);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXResetInputsSystemStackTest,
	"DreamFX.Regression.Adapter.ResetInputsSystemStack",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXResetInputsSystemStackTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::ResetInputsTests;

	// The system stacks have no emitter to name, and SystemState gates its loop duration behind a switch.
	const FString Emitter = EmitterBody(TEXT(""), TEXT(""), TEXT(""));
	UNiagaraSystem* Subject = BuildSystem(*this,
		TEXT("SystemUpdate = { SystemState(LoopBehavior = Once, LoopDuration = 3.5); } ") + Emitter);
	UNiagaraSystem* Reference = BuildSystem(*this, TEXT("SystemUpdate = { SystemState(); } ") + Emitter);
	if (!TestNotNull(TEXT("overridden system"), Subject) || !TestNotNull(TEXT("plain system"), Reference)) { return false; }
	FGCObjectScopeGuard SubjectGuard(Subject);
	FGCObjectScopeGuard ReferenceGuard(Reference);
	ExpectDifferentFacts(*this, TEXT("system stack"), Subject, Reference);

	const FStackAddress Batch[] = {
		InputOf(*this, Subject, TEXT(""), EStackKind::SystemUpdate, TEXT("SystemState"), { TEXT("Loop Behavior") }),
		InputOf(*this, Subject, TEXT(""), EStackKind::SystemUpdate, TEXT("SystemState"), { TEXT("Loop Duration") }),
	};
	if (ResetAndRecompile(*this, TEXT("system stack"), Subject, Batch))
	{
		ExpectSameFacts(*this, TEXT("system stack vs fresh build"), Subject, Reference);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXResetInputsChainTest,
	"DreamFX.Regression.Adapter.ResetInputsDynamicChain",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXResetInputsChainTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::ResetInputsTests;

	const FString Chain = TEXT("SpawnRate(SpawnRate = RandomRangeFloat(Minimum = 2.0, Maximum = 6.0));");
	const FString ChainWithoutMinimum = TEXT("SpawnRate(SpawnRate = RandomRangeFloat(Maximum = 6.0));");
	const FString NoChain = TEXT("SpawnRate();");

	// One nested input on its own: the dynamic input stays, with one of its two inputs back at its default.
	{
		UNiagaraSystem* Subject = BuildSystem(*this, EmitterBody(Chain, TEXT(""), TEXT("")));
		UNiagaraSystem* Reference = BuildSystem(*this, EmitterBody(ChainWithoutMinimum, TEXT(""), TEXT("")));
		if (!TestNotNull(TEXT("chain system"), Subject) || !TestNotNull(TEXT("reference system"), Reference)) { return false; }
		FGCObjectScopeGuard SubjectGuard(Subject);
		FGCObjectScopeGuard ReferenceGuard(Reference);
		ExpectDifferentFacts(*this, TEXT("nested input"), Subject, Reference);

		const FStackAddress Minimum[] = {
			InputOf(*this, Subject, TEXT("E"), EStackKind::EmitterUpdate, TEXT("SpawnRate"), { TEXT("SpawnRate"), TEXT("Minimum") }) };
		if (ResetAndRecompile(*this, TEXT("nested input"), Subject, Minimum))
		{
			FInputValue Value;
			if (ReadInput(*this, Minimum[0].WithInputPath({ TEXT("SpawnRate") }), Value))
			{
				TestTrue(TEXT("the dynamic input survives a reset of one of its inputs"), Value.Mode == EInputValueMode::DynamicInput);
			}
			ExpectSameFacts(*this, TEXT("nested input vs fresh build"), Subject, Reference);
		}
	}

	// Both ends of a chain in one batch, in either order.
	for (const bool bParentFirst : { true, false })
	{
		UNiagaraSystem* Subject = BuildSystem(*this, EmitterBody(Chain, TEXT(""), TEXT("")));
		UNiagaraSystem* Reference = BuildSystem(*this, EmitterBody(NoChain, TEXT(""), TEXT("")));
		if (!TestNotNull(TEXT("chain system"), Subject) || !TestNotNull(TEXT("reference system"), Reference)) { return false; }
		FGCObjectScopeGuard SubjectGuard(Subject);
		FGCObjectScopeGuard ReferenceGuard(Reference);

		const FStackAddress Parent = InputOf(*this, Subject, TEXT("E"), EStackKind::EmitterUpdate, TEXT("SpawnRate"), { TEXT("SpawnRate") });
		const FStackAddress Child = InputOf(*this, Subject, TEXT("E"), EStackKind::EmitterUpdate, TEXT("SpawnRate"), { TEXT("SpawnRate"), TEXT("Minimum") });
		const FStackAddress Batch[] = { bParentFirst ? Parent : Child, bParentFirst ? Child : Parent };
		const TCHAR* What = bParentFirst ? TEXT("parent listed first") : TEXT("child listed first");
		if (ResetAndRecompile(*this, What, Subject, Batch))
		{
			ExpectSameFacts(*this, What, Subject, Reference);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXResetInputsContractTest,
	"DreamFX.Regression.Adapter.ResetInputsContract",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXResetInputsContractTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX;
	using namespace UE::DreamFX::Editor;
	using namespace UE::DreamFX::Editor::ResetInputsTests;

	UNiagaraSystem* Subject = BuildSystem(*this, EmitterBody(
		TEXT("SpawnRate(SpawnRate = RandomRangeFloat(Minimum = 2.0, Maximum = 6.0));"), TEXT(""),
		TEXT("GravityForce(Gravity = (0, 0, -100));")));
	if (!TestNotNull(TEXT("system"), Subject)) { return false; }
	FGCObjectScopeGuard SubjectGuard(Subject);

	const FStackAddress SpawnRate = InputOf(*this, Subject, TEXT("E"), EStackKind::EmitterUpdate, TEXT("SpawnRate"), { TEXT("SpawnRate") });
	const FStackAddress Gravity = InputOf(*this, Subject, TEXT("E"), EStackKind::ParticleUpdate, TEXT("GravityForce"), { TEXT("Gravity") });
	const FStackAddress Untouched = InputOf(*this, Subject, TEXT("E"), EStackKind::EmitterUpdate, TEXT("SpawnRate"), { TEXT("SpawnGroup") });

	TArray<FString> Before;
	DescribeSystemFacts(Subject, Before);

	// Nothing to reset is not a failure.
	{
		TArray<FString> Errors;
		TestTrue(TEXT("an input nobody overrode resets to itself"), FNiagaraAdapter::ResetInputs(Subject, MakeArrayView(&Untouched, 1), Errors));
		TestEqual(TEXT("with nothing to report"), Errors.Num(), 0);
		TestTrue(TEXT("an empty batch succeeds"), FNiagaraAdapter::ResetInputs(Subject, TArrayView<const FStackAddress>(), Errors));
		TArray<FString> After;
		DescribeSystemFacts(Subject, After);
		TestTrue(TEXT("and neither touched the system"), Before == After);
	}

	// Every way an address can fail to resolve, with a reason a person can act on.
	ExpectRefusal(*this, TEXT("unknown module"), Subject, Gravity.WithModule(TEXT("NoSuchModule")), TEXT("module 'NoSuchModule' was not found"));
	ExpectRefusal(*this, TEXT("unknown input"), Subject, Gravity.WithInputPath({ TEXT("NoSuchInput") }), TEXT("input 'NoSuchInput' was not found"));
	ExpectRefusal(*this, TEXT("a path through a plain input"), Subject, Gravity.WithInput(TEXT("Anything")), TEXT("is not a dynamic input"));
	ExpectRefusal(*this, TEXT("unknown stack"), Subject, Gravity.WithScript(TEXT("NotAScript")), TEXT("is not a script usage"));
	ExpectRefusal(*this, TEXT("unknown emitter"), Subject, Gravity.WithEmitter(TEXT("Ghost")), TEXT("emitter 'Ghost' was not found"));
	ExpectRefusal(*this, TEXT("a system stack named through an emitter"), Subject,
		Gravity.WithScript(FNiagaraAdapter::ScriptNameForStack(EStackKind::SystemUpdate)), TEXT("has no"));
	ExpectRefusal(*this, TEXT("event handler stack"), Subject,
		Gravity.WithScript(FNiagaraAdapter::ScriptNameForStack(EStackKind::EventHandler)), TEXT("not supported for this stack"));
	ExpectRefusal(*this, TEXT("simulation stage stack"), Subject,
		Gravity.WithScript(FNiagaraAdapter::ScriptNameForStack(EStackKind::SimulationStage)), TEXT("not supported for this stack"));
	ExpectRefusal(*this, TEXT("no input named"), Subject, Gravity.WithInputPath({}), TEXT("at least one input name"));
	{
		UNiagaraSystem* Other = BuildSystem(*this, EmitterBody(TEXT(""), TEXT(""), TEXT("GravityForce();")));
		if (TestNotNull(TEXT("second system"), Other))
		{
			FGCObjectScopeGuard OtherGuard(Other);
			FStackAddress Foreign = Gravity;
			Foreign.System = Other;
			ExpectRefusal(*this, TEXT("an address into another system"), Subject, Foreign, TEXT("different system"));
		}
	}
	{
		TArray<FString> Errors;
		TestFalse(TEXT("a null system is refused"), FNiagaraAdapter::ResetInputs(nullptr, MakeArrayView(&Gravity, 1), Errors));
		TestTrue(TEXT("with a reason"), Errors.Num() == 1);
	}

	// A bool that only gates another input is drawn beside it and has no stack input to reset.
	ExpectRefusal(*this, TEXT("inline edit condition"), Subject,
		InputOf(*this, Subject, TEXT("E"), EStackKind::EmitterUpdate, TEXT("SpawnRate"), { TEXT("Use Spawn Probability") }),
		TEXT("is an inline edit condition"));

	// One bad address anywhere in a batch resets none of it.
	{
		TArray<FString> Errors;
		const FStackAddress Batch[] = { SpawnRate, Gravity, Gravity.WithInputPath({ TEXT("NoSuchInput") }) };
		TestFalse(TEXT("a batch with a bad address is refused"), FNiagaraAdapter::ResetInputs(Subject, Batch, Errors));
		TestEqual(TEXT("and names exactly the bad one"), Errors.Num(), 1);
		TArray<FString> After;
		DescribeSystemFacts(Subject, After);
		TestTrue(TEXT("and left every good address in the batch alone"), Before == After);
	}

	// Inside a write scope the reset ends the epoch: a write after it must see the graph the reset left. The
	// dynamic input is the case that shows it -- the view model the scope was keeping still has SpawnRate
	// as a dynamic input with children, and a literal written through that picture has nowhere to go.
	{
		FNiagaraAdapter::FWriteScope WriteScope(Subject);
		const FNiagaraTypeDefinition Vec3 = FNiagaraTypeDefinition::GetVec3Def();
		const FNiagaraTypeDefinition Float = FNiagaraTypeDefinition::GetFloatDef();
		TArray<FString> Errors;

		// Builds the scope's shared context, so there is one to drop.
		const FVector3f Pull(0.f, 0.f, -50.f);
		TestTrue(TEXT("a write that opens the scope's shared context"),
			FNiagaraAdapter::SetInput(Gravity, FInputValue::MakeLiteral(Vec3.GetScriptStruct(), &Pull), Errors));

		FNiagaraAdapter::ResetStats();
		TestTrue(TEXT("reset inside the scope"), FNiagaraAdapter::ResetInputs(Subject, MakeArrayView(&SpawnRate, 1), Errors));
		TestTrue(TEXT("which dropped the context the scope was sharing"),
			FNiagaraAdapter::ReportStats().Contains(TEXT("1 epoch boundary(ies)")));
		const float Rate = 10.f;
		TestTrue(TEXT("a write after the reset"),
			FNiagaraAdapter::SetInput(SpawnRate, FInputValue::MakeLiteral(Float.GetScriptStruct(), &Rate), Errors));

		FInputValue Value;
		if (ReadInput(*this, SpawnRate, Value))
		{
			TestTrue(TEXT("lands as a literal on the reset graph"), Value.Mode == EInputValueMode::Literal
				&& Value.LiteralBytes.Num() == static_cast<int32>(sizeof(float))
				&& *reinterpret_cast<const float*>(Value.LiteralBytes.GetData()) == Rate);
		}
		TestEqual(TEXT("without a complaint along the way"), Errors.Num(), 0);

		// And the earlier write is still what it was.
		if (ReadInput(*this, Gravity, Value) && Value.Mode == EInputValueMode::Literal && Value.LiteralBytes.Num() == static_cast<int32>(sizeof(FVector3f)))
		{
			TestEqual(TEXT("the other input is untouched"), reinterpret_cast<const FVector3f*>(Value.LiteralBytes.GetData())->Z, -50.f);
		}
	}

	// A read scope holds a view model that a reset would leave describing the old graph.
	{
		FNiagaraAdapter::FReadScope ReadScope(Subject);
		TArray<FString> Errors;
		TestFalse(TEXT("a reset inside a read scope is refused"), FNiagaraAdapter::ResetInputs(Subject, MakeArrayView(&Gravity, 1), Errors));
		TestTrue(TEXT("and says why"), FString::Join(Errors, TEXT(" | ")).Contains(TEXT("read scope")));
	}
	return true;
}

#endif
