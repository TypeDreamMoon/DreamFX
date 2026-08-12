#include "Diff/DreamFXAssetFacts.h"

#include "Adapter/DreamFXNiagaraAdapter.h"
#include "NiagaraDataInterface.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraSystem.h"
#include "UObject/UObjectHash.h"

namespace UE::DreamFX::Editor
{
	namespace
	{
	/**
	 * Identity noise scrubbed out of a fact, so two assets that differ only in who they are compare
	 * equal: 32-hex guids, `_123` autoname suffixes (subobject names, node names), and references to
	 * the asset's own package. Everything else -- values, real names, resolution state -- survives.
	 */
	FString ScrubIdentity(const FString& Text, const FString& SelfPackage)
	{
		FString Out;
		Out.Reserve(Text.Len());

		int32 Index = 0;
		while (Index < Text.Len())
		{
			if (!SelfPackage.IsEmpty() && FCString::Strncmp(&Text[Index], *SelfPackage, SelfPackage.Len()) == 0)
			{
				Out += TEXT("<self>");
				Index += SelfPackage.Len();
				continue;
			}

			const TCHAR Character = Text[Index];

			auto IsHex = [](TCHAR C) { return FChar::IsDigit(C) || (C >= 'A' && C <= 'F'); };
			// Only a MAXIMAL run of exactly 32 hex characters is a guid. FLT_MAX prints as 39
			// decimal digits, whose 32-character tail this used to eat.
			if (IsHex(Character) && (Index == 0 || !IsHex(Text[Index - 1])))
			{
				int32 Run = Index;
				while (Run < Text.Len() && IsHex(Text[Run]))
				{
					++Run;
				}
				if (Run - Index == 32)
				{
					Out += TEXT("<guid>");
					Index = Run;
					continue;
				}
			}

			if (Character == TEXT('_') && Index + 1 < Text.Len() && FChar::IsDigit(Text[Index + 1]))
			{
				int32 Run = Index + 1;
				while (Run < Text.Len() && FChar::IsDigit(Text[Run]))
				{
					++Run;
				}
				if (Run >= Text.Len() || !FChar::IsAlpha(Text[Run]))
				{
					Out += TEXT("~");
					Index = Run;
					continue;
				}
			}

			Out.AppendChar(Character);
			++Index;
		}
		return Out;
	}

	/** One fact per top-level reflected property, skipping transients and an explicit list. */
	void AppendPropertyFacts(const FString& Prefix, const void* Container, const UStruct* Struct,
		const TSet<FName>& Skip, const FString& SelfPackage, TArray<FString>& OutFacts)
	{
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			FProperty* Property = *It;
			if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_Deprecated)
				|| Skip.Contains(Property->GetFName()))
			{
				continue;
			}
			FString Value;
			Property->ExportText_InContainer(0, Value, Container, Container, nullptr, PPF_None);
			OutFacts.Add(FString::Printf(TEXT("%s %s = %s"),
				*Prefix, *Property->GetName(), *ScrubIdentity(Value, SelfPackage)));
		}
	}

	/**
	 * Rapid-iteration and exposed parameter stores, one fact per parameter with its value bytes.
	 *
	 * The module segment of a name has its trailing digits stripped ("Collision001" and "Collision"
	 * are one module authored twice under Niagara's uniquifier), which is what let this tool see
	 * that Down_Root's original DOES carry Advanced Aging Rate -- the claim "the original has no
	 * value at all" had been read off the lossy export.
	 */
	void AppendParameterStoreFacts(const FString& Prefix, FNiagaraParameterStore& Store,
		TArray<FString>& OutFacts)
	{
		TArrayView<const FNiagaraVariableWithOffset> Variables = Store.ReadParameterVariables();
		const TArray<uint8>& Data = Store.GetParameterDataArray();

		for (const FNiagaraVariableWithOffset& Variable : Variables)
		{
			// Scrubbed like every other fact, and BEFORE the module-segment trim below. A rapid
			// iteration name embeds the node that owns the input, and a Set Parameters node is named
			// after its guid -- so without this every such parameter is a permanent difference on
			// both sides. The order matters twice over: the trim strips trailing digits off the
			// module segment, which on a raw guid ending in digits eats part of the guid and leaves
			// a shorter string that then never matches the 32-character run this collapses.
			FString Name = ScrubIdentity(Variable.GetName().ToString(), FString());
			TArray<FString> Segments;
			Name.ParseIntoArray(Segments, TEXT("."));
			if (Segments.Num() >= 3)
			{
				FString& Module = Segments[2];
				while (Module.Len() > 0 && FChar::IsDigit(Module[Module.Len() - 1]))
				{
					Module.LeftChopInline(1, EAllowShrinking::No);
				}
				Name = FString::Join(Segments, TEXT("."));
			}

			FString Value;
			if (Variable.IsDataInterface() || Variable.IsUObject())
			{
				Value = TEXT("<object>");
			}
			else if (Variable.Offset >= 0 && Variable.Offset + Variable.GetSizeInBytes() <= Data.Num())
			{
				Value = BytesToHex(Data.GetData() + Variable.Offset, Variable.GetSizeInBytes());
			}
			else
			{
				Value = TEXT("<no data>");
			}

			OutFacts.Add(FString::Printf(TEXT("%s %s (%s) = %s"),
				*Prefix, *Name, *Variable.GetType().GetName(), *Value));
		}
	}

	/**
	 * The compiler's own view of a script: what the translator concluded, not what the author wrote.
	 *
	 * This is the channel that finally broke the 2026-08-12 mirror-no-smoke case after L1, L2, the
	 * asset facts above and L3 had all reported green on a fluid that rendered nothing. Every one of
	 * those looks at authored state; the loss was in what the translator MADE of it, and the tell was
	 * `SimulationStageMetaData.OutputDestinations` -- one side wrote to the grids, the other wrote to
	 * nothing. Bringing it in here turns that afternoon's manual comparison into a standing eye.
	 *
	 * Everything emitted is names and enums: headless-readable (the translation runs in a commandlet
	 * even though the RHI cannot compile the shader), session-independent, and downstream of every
	 * authored input, so a module that silently failed to land shows up as a missing consequence.
	 */
	void AppendCompiledFacts(const FString& Prefix, UNiagaraScript* Script, const FString& SelfPackage,
		TArray<FString>& OutFacts)
	{
		const FNiagaraVMExecutableData& Compiled = Script->GetVMExecutableData();

		// The stage table, one fact per stage. Reflected wholesale: the struct is all names, enums
		// and counts (its one cached int, ParticleIterationStateComponentIndex, is not a UPROPERTY
		// and so is skipped for free), and reflecting it means a field the engine adds in 5.9 joins
		// the comparison without anyone remembering to add it.
		//
		// The struct type is reached through the property that holds it rather than through
		// FSimulationStageMetaData::StaticStruct(): that accessor inlines a call into NiagaraShader,
		// which this module does not link, and adding a module dependency to read one type would be
		// a heavier bargain than the lookup. Anchoring on the owning array property also means the
		// element type is derived rather than named, so it cannot drift.
		static const UScriptStruct* StageStruct = []() -> const UScriptStruct*
		{
			const FArrayProperty* Array = CastField<FArrayProperty>(
				FNiagaraVMExecutableData::StaticStruct()->FindPropertyByName(TEXT("SimulationStageMetaData")));
			const FStructProperty* Element = Array != nullptr ? CastField<FStructProperty>(Array->Inner) : nullptr;
			return Element != nullptr ? Element->Struct : nullptr;
		}();

		int32 StageIndex = 0;
		for (const FSimulationStageMetaData& Stage : Compiled.SimulationStageMetaData)
		{
			if (StageStruct == nullptr)
			{
				break;
			}
			TArray<FString> StageFacts;
			AppendPropertyFacts(TEXT(""), &Stage, StageStruct, TSet<FName>(), SelfPackage, StageFacts);
			StageFacts.Sort();
			OutFacts.Add(FString::Printf(TEXT("%s stage %d { %s }"),
				*Prefix, StageIndex, *FString::Join(StageFacts, TEXT("; "))));
			++StageIndex;
		}

		// The data interface table the translator bound. Identity fields are left out on purpose:
		// UserPtrIdx is a table slot whose number depends on discovery order, and the placeholder
		// flag rides along only because a placeholder that survives to here is a broken binding.
		for (const FNiagaraScriptDataInterfaceCompileInfo& Info : Compiled.DataInterfaceInfo)
		{
			// Scrubbed like every other fact: a module input's data interface is named after the
			// node that holds it, and a SetVariables node's name carries its guid. Without this the
			// family reports "both sides have this interface under a different node id" -- which is
			// true of every rebuild and means nothing.
			const FString Name = ScrubIdentity(Info.Name.ToString(), SelfPackage);
			OutFacts.Add(FString::Printf(TEXT("%s di '%s' : %s read=%s write=%s placeholder=%s source=%s"),
				*Prefix, *Name, *Info.Type.GetName(),
				*ScrubIdentity(Info.RegisteredParameterMapRead.ToString(), SelfPackage),
				*ScrubIdentity(Info.RegisteredParameterMapWrite.ToString(), SelfPackage),
				Info.bIsPlaceholder ? TEXT("true") : TEXT("false"), *Info.SourceEmitterName));

			// Which functions the compiled script actually calls on that interface -- the difference
			// between a grid that is merely bound and a grid that is written.
			TArray<FString> Functions;
			for (const FNiagaraFunctionSignature& Signature : Info.RegisteredFunctions)
			{
				// The `*` is the write flag, which is the mechanism behind OutputDestinations above:
				// a stage is an output stage because a write function was called on that interface.
				Functions.Add(FString::Printf(TEXT("%s%s"),
					*Signature.Name.ToString(), Signature.bWriteFunction ? TEXT("*") : TEXT("")));
			}
			Functions.Sort();
			OutFacts.Add(FString::Printf(TEXT("%s di '%s' calls: %s"),
				*Prefix, *Name, *FString::Join(Functions, TEXT(", "))));
		}

		// The attribute set the script writes. A module that failed to land writes nothing, and this
		// line says so in one place regardless of which module it was.
		TArray<FString> Written;
		for (const FNiagaraVariableBase& Attribute : Compiled.AttributesWritten)
		{
			Written.Add(FString::Printf(TEXT("%s(%s)"),
				*ScrubIdentity(Attribute.GetName().ToString(), SelfPackage), *Attribute.GetType().GetName()));
		}
		Written.Sort();
		OutFacts.Add(FString::Printf(TEXT("%s writes: %s"), *Prefix, *FString::Join(Written, TEXT(", "))));
	}

	/** Every comparable fact about one system, unsorted; the caller compares as a multiset. */
	void DescribeSystem(UNiagaraSystem* System, TArray<FString>& OutFacts)
	{
		const FString SelfPackage = System->GetOutermost()->GetName();

		// Identity, wiring and editor bookkeeping. Graphs and scripts are deliberately absent as
		// objects -- their observable content arrives through the parameter stores here, the export
		// (L1) and the simulation (L3); their node soup is all identity.
		static const TSet<FName> EmitterDataSkip = {
			TEXT("GraphSource"), TEXT("SpawnScriptProps"), TEXT("UpdateScriptProps"),
			TEXT("EventHandlerScriptProps"), TEXT("SimulationStages"), TEXT("GPUComputeScript"),
			TEXT("EmitterSpawnScriptProps"), TEXT("EmitterUpdateScriptProps"),
			TEXT("ScratchPads"), TEXT("ParentScratchPads"),
			TEXT("VersionedParent"), TEXT("VersionedParentAtLastMerge"),
			TEXT("RendererProperties"), TEXT("MessageStore"), TEXT("Version"),
		};
		static const TSet<FName> RendererSkip = { TEXT("MessageStore"), TEXT("StackMessages") };

		if (UNiagaraScript* SystemSpawn = System->GetSystemSpawnScript())
		{
			AppendParameterStoreFacts(TEXT("ri system-spawn"), SystemSpawn->RapidIterationParameters, OutFacts);
			AppendCompiledFacts(TEXT("compiled system-spawn"), SystemSpawn, SelfPackage, OutFacts);
		}
		if (UNiagaraScript* SystemUpdate = System->GetSystemUpdateScript())
		{
			AppendParameterStoreFacts(TEXT("ri system-update"), SystemUpdate->RapidIterationParameters, OutFacts);
			AppendCompiledFacts(TEXT("compiled system-update"), SystemUpdate, SelfPackage, OutFacts);
		}
		AppendParameterStoreFacts(TEXT("user"), System->GetExposedParameters(), OutFacts);

		for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			const FString EmitterName = Handle.GetName().ToString();
			const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
			if (Data == nullptr)
			{
				OutFacts.Add(FString::Printf(TEXT("emitter %s has no data"), *EmitterName));
				continue;
			}

			OutFacts.Add(FString::Printf(TEXT("emitter %s enabled = %s"),
				*EmitterName, Handle.GetIsEnabled() ? TEXT("true") : TEXT("false")));

			// Parentage is a first-class fact, not a property to scrub: an inheriting original and
			// its flattened mirror SHOULD read differently here, and this line is where that shows.
			const FVersionedNiagaraEmitter Parent = Data->GetParent();
			OutFacts.Add(FString::Printf(TEXT("emitter %s parent = %s"),
				*EmitterName, Parent.Emitter != nullptr ? *Parent.Emitter->GetPathName() : TEXT("none")));

			// Event handlers as dedicated facts, source spoken by NAME. The generic property walk
			// used to SKIP EventHandlerScriptProps -- which is precisely how a mirror with no event
			// handlers at all sailed through this tool while its event-spawned emitters rendered
			// nothing (2026-08-11, Descend/Up). The raw struct would compare on the source handle
			// guid and the script object path, both identity; the name form compares on meaning.
			{
				TArray<FNiagaraAdapter::FEventHandlerSummary> Handlers;
				TArray<FString> HandlerErrors;
				FNiagaraAdapter::GetEmitterEventHandlers(
					FStackAddress(System).WithEmitter(Handle.GetName()), Handlers, HandlerErrors);
				for (const FNiagaraAdapter::FEventHandlerSummary& Handler : Handlers)
				{
					OutFacts.Add(FString::Printf(TEXT("emitter %s event handler: '%s' from '%s' (%s x%d)"),
						*EmitterName, *Handler.SourceEventName.ToString(), *Handler.SourceEmitterName,
						*Handler.ExecutionMode, Handler.SpawnNumber));
				}
			}

			// Simulation stages as dedicated facts, for the same reason as the event handlers above:
			// the raw property walk skips SimulationStages because the array itself is subobject
			// identity, which is exactly how a mirror with no stages at all would sail through this
			// tool while a Grid3D fluid renders nothing. One fact per stage -- class plus full
			// property text, the shape the data interfaces use below. The script object is identity
			// (its content arrives through the export and the simulation), the outer version guid too.
			//
			// Every binding-struct field is reduced to its NAMES (plus default bytes where the
			// binding carries a value). The raw struct text prints registered-type handles, and a
			// handle is serialization residue: on authored content it resolves to an unrelated type
			// in any later session (GroomRods' PressureGrid binding reads as Vector4f) while the
			// asset simulates fine, because the engine resolves every binding by name and never
			// reads the stored type back. Comparing handles would fail every rebuilt mirror against
			// noise the engine itself ignores. DataInterface was normalized first; EnabledBinding,
			// NumIterations and the dispatch element counts are the same family.
			{
				static const TSet<FName> StageSkipBase = { TEXT("Script"), TEXT("OuterEmitterVersion") };
				int32 StageIndex = 0;
				for (const UNiagaraSimulationStageBase* Stage : Data->GetSimulationStages())
				{
					if (Stage != nullptr)
					{
						TSet<FName> StageSkip = StageSkipBase;
						TArray<FString> StageFacts;

						for (TFieldIterator<FStructProperty> It(Stage->GetClass()); It; ++It)
						{
							const FString StructName = It->Struct->GetName();
							const bool bIsBinding =
								StructName.Contains(TEXT("NiagaraParameterBinding"))
								|| StructName.Contains(TEXT("NiagaraVariableAttributeBinding"))
								|| StructName == TEXT("NiagaraVariableDataInterfaceBinding");
							if (!bIsBinding)
							{
								continue;
							}
							StageSkip.Add(It->GetFName());

							const void* BindingPtr = It->ContainerPtrToValuePtr<void>(Stage);
							TArray<FString> Parts;
							for (TFieldIterator<FProperty> Inner(It->Struct); Inner; ++Inner)
							{
								// Derived caches carry no semantics of their own: the resolved
								// variable, the data set spelling and the exists/cached flags are
								// all recomputed from the root name -- and bBindingExistsOnSource
								// is the documented false-diff of the NE_C round.
								const FString InnerName = Inner->GetName();
								if (InnerName.StartsWith(TEXT("Cached"))
									|| InnerName.StartsWith(TEXT("bBindingExists"))
									|| InnerName.StartsWith(TEXT("bIsCached"))
									|| InnerName == TEXT("ParamMapVariable")
									|| InnerName == TEXT("DataSetName")
									|| InnerName == TEXT("DataSetVariable")
									// The RootName's variable-shaped twin, recomputed from it.
									|| InnerName == TEXT("RootVariable"))
								{
									continue;
								}

								if (const FStructProperty* Var = CastField<FStructProperty>(*Inner))
								{
									// FNiagaraVariable/-Base sub-fields: the name is the meaning;
									// the stored type handle is cross-session residue.
									if (Var->Struct->IsChildOf(FNiagaraVariableBase::StaticStruct()))
									{
										const FNiagaraVariableBase* Variable =
											Var->ContainerPtrToValuePtr<FNiagaraVariableBase>(BindingPtr);
										Parts.Add(FString::Printf(TEXT("%s=%s"), *Var->GetName(),
											*Variable->GetName().ToString()));
										continue;
									}
								}

								// Everything else in a binding struct is plain data (a root FName,
								// a source-mode enum, default-value bytes) and exports safely.
								FString ValueText;
								Inner->ExportText_InContainer(0, ValueText, BindingPtr, BindingPtr,
									nullptr, PPF_None);
								Parts.Add(FString::Printf(TEXT("%s=%s"), *InnerName, *ValueText));
							}
							StageFacts.Add(FString::Printf(TEXT("%s = bind(%s)"),
								*It->GetName(), *FString::Join(Parts, TEXT(","))));
						}

						AppendPropertyFacts(TEXT(""), Stage, Stage->GetClass(), StageSkip, SelfPackage, StageFacts);
						StageFacts.Sort();
						OutFacts.Add(FString::Printf(TEXT("emitter %s simulation stage %d:%s { %s }"),
							*EmitterName, StageIndex, *Stage->GetClass()->GetName(),
							*FString::Join(StageFacts, TEXT("; "))));
					}
					++StageIndex;
				}
			}

			AppendPropertyFacts(FString::Printf(TEXT("emitter %s"), *EmitterName),
				Data, FVersionedNiagaraEmitterData::StaticStruct(), EmitterDataSkip, SelfPackage, OutFacts);

			TArray<UNiagaraScript*> Scripts;
			Data->GetScripts(Scripts, /*bCompilableOnly=*/true);
			for (UNiagaraScript* Script : Scripts)
			{
				if (Script == nullptr)
				{
					continue;
				}
				const FString Usage = StaticEnum<ENiagaraScriptUsage>()->GetNameStringByValue(
					static_cast<int64>(Script->GetUsage()));
				AppendParameterStoreFacts(FString::Printf(TEXT("ri %s %s"), *EmitterName, *Usage),
					Script->RapidIterationParameters, OutFacts);
				AppendCompiledFacts(FString::Printf(TEXT("compiled %s %s"), *EmitterName, *Usage),
					Script, SelfPackage, OutFacts);
			}

			int32 RendererIndex = 0;
			Data->ForEachRenderer([&](UNiagaraRendererProperties* Renderer)
			{
				if (Renderer != nullptr)
				{
					AppendPropertyFacts(
						FString::Printf(TEXT("emitter %s renderer %d:%s"),
							*EmitterName, RendererIndex, *Renderer->GetClass()->GetName()),
						Renderer, Renderer->GetClass(), RendererSkip, SelfPackage, OutFacts);
				}
				++RendererIndex;
			});
		}

		// Data interfaces live as subobjects wherever a module input placed them. One fact per
		// interface, class plus full property text, so a DI whose configuration fails to round-trip
		// shows up as a one-in/one-out pair instead of hiding behind an object path.
		//
		// The search starts at the package because a system's emitters are outered to the package
		// rather than to the system, so walking the system alone would miss every per-emitter
		// interface. It is then filtered back down to THIS system: a package is allowed to hold more
		// than one, and the round-trip corpus builds a fixture and its rebuild into the same one --
		// where an unfiltered walk reports each side's interfaces to both sides and the comparison
		// fails on its own bookkeeping rather than on anything the exporter did.
		TSet<const UObject*> Roots;
		Roots.Add(System);
		for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			if (const UNiagaraEmitter* Emitter = Handle.GetInstance().Emitter)
			{
				Roots.Add(Emitter);
			}
		}
		auto BelongsToSystem = [&Roots](const UObject* Object)
		{
			for (const UObject* Outer = Object; Outer != nullptr; Outer = Outer->GetOuter())
			{
				if (Roots.Contains(Outer))
				{
					return true;
				}
			}
			return false;
		};

		TArray<UObject*> Inner;
		GetObjectsWithOuter(System->GetOutermost(), Inner, /*bIncludeNestedObjects=*/true);
		for (UObject* Object : Inner)
		{
			UNiagaraDataInterface* Interface = Cast<UNiagaraDataInterface>(Object);
			if (Interface == nullptr || !BelongsToSystem(Interface))
			{
				continue;
			}
			// The cooked LUT family is derived from Curve at save time; comparing it repeats the
			// curve comparison with extra baked noise.
			static const TSet<FName> InterfaceSkip = {
				TEXT("CurveCookedEditorCache"), TEXT("ShaderLUT"), TEXT("LUTMinTime"),
				TEXT("LUTMaxTime"), TEXT("LUTInvTimeRange"), TEXT("LUTNumSamplesMinusOne"),
			};
			TArray<FString> InterfaceFacts;
			AppendPropertyFacts(TEXT(""), Interface, Interface->GetClass(), InterfaceSkip, SelfPackage, InterfaceFacts);
			InterfaceFacts.Sort();
			OutFacts.Add(FString::Printf(TEXT("di %s { %s }"),
				*Interface->GetClass()->GetName(), *FString::Join(InterfaceFacts, TEXT("; "))));
		}
	}
	}

	void DescribeSystemFacts(UNiagaraSystem* System, TArray<FString>& OutFacts)
	{
		DescribeSystem(System, OutFacts);
	}
}
