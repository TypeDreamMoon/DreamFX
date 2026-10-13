#include "Generation/DreamFXPlanDigest.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::DreamFX::Editor
{
	namespace
	{
		TArray<TSharedPtr<FJsonValue>> ToJsonArray(const TArray<FString>& Strings)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			Values.Reserve(Strings.Num());
			for (const FString& String : Strings)
			{
				Values.Add(MakeShared<FJsonValueString>(String));
			}
			return Values;
		}

		TArray<FString> FromJsonArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
		{
			TArray<FString> Strings;
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (Object.IsValid() && Object->TryGetArrayField(Field, Values))
			{
				for (const TSharedPtr<FJsonValue>& Value : *Values)
				{
					Strings.Add(Value.IsValid() ? Value->AsString() : FString());
				}
			}
			return Strings;
		}

		FString GetString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
		{
			FString Value;
			if (Object.IsValid())
			{
				Object->TryGetStringField(Field, Value);
			}
			return Value;
		}

		TSharedPtr<FJsonObject> StackToJson(const FStackDigest& Stack)
		{
			TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("structure"), Stack.Structure);
			Object->SetStringField(TEXT("switches"), Stack.Switches);
			Object->SetArrayField(TEXT("nodes"), ToJsonArray(Stack.NodeNames));
			Object->SetArrayField(TEXT("bases"), ToJsonArray(Stack.ModuleBases));
			TSharedPtr<FJsonObject> Inputs = MakeShared<FJsonObject>();
			for (const TPair<FString, FString>& Input : Stack.Inputs)
			{
				Inputs->SetStringField(Input.Key, Input.Value);
			}
			Object->SetObjectField(TEXT("inputs"), Inputs);
			return Object;
		}

		FStackDigest StackFromJson(const TSharedPtr<FJsonObject>& Object)
		{
			FStackDigest Stack;
			Stack.Structure = GetString(Object, TEXT("structure"));
			Stack.Switches = GetString(Object, TEXT("switches"));
			Stack.NodeNames = FromJsonArray(Object, TEXT("nodes"));
			Stack.ModuleBases = FromJsonArray(Object, TEXT("bases"));
			const TSharedPtr<FJsonObject>* Inputs = nullptr;
			if (Object.IsValid() && Object->TryGetObjectField(TEXT("inputs"), Inputs))
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Input : (*Inputs)->Values)
				{
					Stack.Inputs.Add(Input.Key, Input.Value.IsValid() ? Input.Value->AsString() : FString());
				}
			}
			return Stack;
		}

		TSharedPtr<FJsonObject> StacksToJson(const TMap<FString, FStackDigest>& Stacks)
		{
			TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			for (const TPair<FString, FStackDigest>& Stack : Stacks)
			{
				Object->SetObjectField(Stack.Key, StackToJson(Stack.Value));
			}
			return Object;
		}

		TMap<FString, FStackDigest> StacksFromJson(const TSharedPtr<FJsonObject>& Parent, const TCHAR* Field)
		{
			TMap<FString, FStackDigest> Stacks;
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (Parent.IsValid() && Parent->TryGetObjectField(Field, Object))
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Stack : (*Object)->Values)
				{
					Stacks.Add(Stack.Key, StackFromJson(Stack.Value.IsValid() ? Stack.Value->AsObject() : nullptr));
				}
			}
			return Stacks;
		}

		void DiffInputs(const FStackDigest& Old, const FStackDigest& New, FStackChange& OutChange)
		{
			for (const TPair<FString, FString>& Input : New.Inputs)
			{
				const FString* Previous = Old.Inputs.Find(Input.Key);
				if (Previous == nullptr)
				{
					OutChange.Added.Add(Input.Key);
				}
				else if (*Previous != Input.Value)
				{
					OutChange.Changed.Add(Input.Key);
				}
			}
			for (const TPair<FString, FString>& Input : Old.Inputs)
			{
				if (!New.Inputs.Contains(Input.Key))
				{
					OutChange.Removed.Add(Input.Key);
				}
			}
			OutChange.Action = (OutChange.Added.Num() + OutChange.Changed.Num() + OutChange.Removed.Num()) > 0
				? EStackAction::Values
				: EStackAction::Keep;
		}

		bool SameStructure(const FStackDigest& Old, const FStackDigest& New)
		{
			return Old.Structure == New.Structure && Old.Switches == New.Switches
				&& Old.NodeNames.Num() == Old.ModuleBases.Num();
		}

		/**
		 * Node names are unique within one graph -- the system's, or one emitter's -- and the engine
		 * picks the suffix when a module is added. The full rebuild clears every main stack of the graph
		 * first and then adds stack by stack, so a module shared with a later stack is named before that
		 * stack's copy exists. Rebuilding one stack alone happens while the other stacks still hold
		 * their nodes, and a shared module name can then land on a different suffix. Only a module name
		 * that appears in more than one stack can do that.
		 */
		bool SharesModuleWithOtherStacks(const FString& StackKey, const TMap<FString, FStackDigest>& OldStacks,
			const TMap<FString, FStackDigest>& NewStacks, FString& OutShared)
		{
			TSet<FString> Mine;
			for (const TMap<FString, FStackDigest>* Stacks : { &OldStacks, &NewStacks })
			{
				if (const FStackDigest* Stack = Stacks->Find(StackKey))
				{
					Mine.Append(Stack->ModuleBases);
				}
			}
			for (const TMap<FString, FStackDigest>* Stacks : { &OldStacks, &NewStacks })
			{
				for (const TPair<FString, FStackDigest>& Other : *Stacks)
				{
					if (Other.Key == StackKey)
					{
						continue;
					}
					for (const FString& Base : Other.Value.ModuleBases)
					{
						if (Mine.Contains(Base))
						{
							OutShared = FString::Printf(TEXT("'%s' is used by both %s and %s"), *Base, *StackKey, *Other.Key);
							return true;
						}
					}
				}
			}
			return false;
		}

		/** Decides one set of stacks that share a graph. Returns false if the whole graph must be rebuilt. */
		bool DecideStacks(const TMap<FString, FStackDigest>& OldStacks, const TMap<FString, FStackDigest>& NewStacks,
			bool bUndeclaredStacksKeepContents, TMap<FString, FStackChange>& OutChanges, FString& OutWhyWhole)
		{
			TSet<FString> Keys;
			for (const TPair<FString, FStackDigest>& Stack : OldStacks) { Keys.Add(Stack.Key); }
			for (const TPair<FString, FStackDigest>& Stack : NewStacks) { Keys.Add(Stack.Key); }

			TArray<FString> Rebuilt;
			for (const FString& Key : Keys)
			{
				const FStackDigest* Old = OldStacks.Find(Key);
				const FStackDigest* New = NewStacks.Find(Key);
				FStackChange Change;
				if (Old == nullptr)
				{
					Change.Action = EStackAction::Rebuild;
				}
				else if (New == nullptr)
				{
					// The full rebuild leaves an undeclared system stack alone and empties an undeclared
					// emitter stack, and the incremental build has to do the same.
					Change.Action = bUndeclaredStacksKeepContents ? EStackAction::Keep : EStackAction::Clear;
				}
				else if (!SameStructure(*Old, *New))
				{
					Change.Action = EStackAction::Rebuild;
				}
				else
				{
					DiffInputs(*Old, *New, Change);
				}

				if (Change.Action == EStackAction::Rebuild || Change.Action == EStackAction::Clear)
				{
					Rebuilt.Add(Key);
				}
				if (Change.Action != EStackAction::Keep)
				{
					OutChanges.Add(Key, MoveTemp(Change));
				}
			}

			for (const FString& Key : Rebuilt)
			{
				if (SharesModuleWithOtherStacks(Key, OldStacks, NewStacks, OutWhyWhole))
				{
					return false;
				}
			}
			return true;
		}

		const TCHAR* LexAction(EStackAction Action)
		{
			switch (Action)
			{
			case EStackAction::Values:  return TEXT("values");
			case EStackAction::Rebuild: return TEXT("rebuild");
			case EStackAction::Clear:   return TEXT("clear");
			default:                    return TEXT("keep");
			}
		}

		void DescribeStacks(const TMap<FString, FStackChange>& Stacks, const FString& Prefix, TArray<FString>& OutParts)
		{
			for (const TPair<FString, FStackChange>& Stack : Stacks)
			{
				// Script names are usage enums spelled out; the stack is all a reader needs.
				FString Name = Stack.Key;
				Name.RemoveFromStart(TEXT("ENiagaraScriptUsage::"));
				if (Stack.Value.Action == EStackAction::Values)
				{
					OutParts.Add(FString::Printf(TEXT("%s%s values(+%d ~%d -%d)"), *Prefix, *Name,
						Stack.Value.Added.Num(), Stack.Value.Changed.Num(), Stack.Value.Removed.Num()));
				}
				else
				{
					OutParts.Add(FString::Printf(TEXT("%s%s %s"), *Prefix, *Name, LexAction(Stack.Value.Action)));
				}
			}
		}
	}

	FString MakeEventStackKey(int32 HandlerIndex, FName ScriptName)
	{
		return FString::Printf(TEXT("event%d|%s"), HandlerIndex, *ScriptName.ToString());
	}

	FString MakeStageStackKey(int32 StageIndex, FName ScriptName)
	{
		return FString::Printf(TEXT("stage%d|%s"), StageIndex, *ScriptName.ToString());
	}

	bool FEmitterChange::HasStructuralStackChange() const
	{
		for (const TPair<FString, FStackChange>& Stack : Stacks)
		{
			if (Stack.Value.Action == EStackAction::Rebuild || Stack.Value.Action == EStackAction::Clear)
			{
				return true;
			}
		}
		return false;
	}

	FString FPlanDigest::ToJson() const
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetNumberField(TEXT("v"), Version);
		Root->SetStringField(TEXT("generator"), GeneratorVersion);
		Root->SetStringField(TEXT("backend"), Backend);
		Root->SetStringField(TEXT("facts"), FactsHash);
		Root->SetStringField(TEXT("systemProperties"), SystemProperties);
		Root->SetStringField(TEXT("userVariables"), UserVariables);
		Root->SetBoolField(TEXT("selfReferences"), bHasSelfReferences);
		Root->SetObjectField(TEXT("systemStacks"), StacksToJson(SystemStacks));
		Root->SetArrayField(TEXT("emitterOrder"), ToJsonArray(EmitterOrder));

		TSharedPtr<FJsonObject> EmitterObjects = MakeShared<FJsonObject>();
		for (const TPair<FString, FEmitterDigest>& Entry : Emitters)
		{
			const FEmitterDigest& Emitter = Entry.Value;
			TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("parent"), Emitter.Parent);
			Object->SetStringField(TEXT("properties"), Emitter.Properties);
			Object->SetStringField(TEXT("simTarget"), Emitter.SimTarget);
			Object->SetStringField(TEXT("renderers"), Emitter.Renderers);
			Object->SetNumberField(TEXT("rendererCount"), Emitter.RendererCount);
			Object->SetStringField(TEXT("eventHandlers"), Emitter.EventHandlers);
			Object->SetStringField(TEXT("simulationStages"), Emitter.SimulationStages);
			Object->SetStringField(TEXT("parameterDefaults"), Emitter.ParameterDefaults);
			Object->SetObjectField(TEXT("stacks"), StacksToJson(Emitter.Stacks));
			TSharedPtr<FJsonObject> Aux = MakeShared<FJsonObject>();
			for (const TPair<FString, TArray<FString>>& Names : Emitter.AuxNodeNames)
			{
				Aux->SetArrayField(Names.Key, ToJsonArray(Names.Value));
			}
			Object->SetObjectField(TEXT("aux"), Aux);
			EmitterObjects->SetObjectField(Entry.Key, Object);
		}
		Root->SetObjectField(TEXT("emitters"), EmitterObjects);

		FString Json;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
		FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
		return Json;
	}

	bool FPlanDigest::FromJson(const FString& Json, FPlanDigest& OutDigest, FString& OutError)
	{
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			OutError = TEXT("the stored plan digest is not valid JSON");
			return false;
		}

		OutDigest = FPlanDigest();
		double Version = 0.0;
		Root->TryGetNumberField(TEXT("v"), Version);
		OutDigest.Version = static_cast<int32>(Version);
		OutDigest.GeneratorVersion = GetString(Root, TEXT("generator"));
		OutDigest.Backend = GetString(Root, TEXT("backend"));
		OutDigest.FactsHash = GetString(Root, TEXT("facts"));
		OutDigest.SystemProperties = GetString(Root, TEXT("systemProperties"));
		OutDigest.UserVariables = GetString(Root, TEXT("userVariables"));
		Root->TryGetBoolField(TEXT("selfReferences"), OutDigest.bHasSelfReferences);
		OutDigest.SystemStacks = StacksFromJson(Root, TEXT("systemStacks"));
		OutDigest.EmitterOrder = FromJsonArray(Root, TEXT("emitterOrder"));

		const TSharedPtr<FJsonObject>* EmitterObjects = nullptr;
		if (Root->TryGetObjectField(TEXT("emitters"), EmitterObjects))
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : (*EmitterObjects)->Values)
			{
				const TSharedPtr<FJsonObject> Object = Entry.Value.IsValid() ? Entry.Value->AsObject() : nullptr;
				FEmitterDigest& Emitter = OutDigest.Emitters.Add(Entry.Key);
				Emitter.Parent = GetString(Object, TEXT("parent"));
				Emitter.Properties = GetString(Object, TEXT("properties"));
				Emitter.SimTarget = GetString(Object, TEXT("simTarget"));
				Emitter.Renderers = GetString(Object, TEXT("renderers"));
				double RendererCount = 0.0;
				if (Object.IsValid()) { Object->TryGetNumberField(TEXT("rendererCount"), RendererCount); }
				Emitter.RendererCount = static_cast<int32>(RendererCount);
				Emitter.EventHandlers = GetString(Object, TEXT("eventHandlers"));
				Emitter.SimulationStages = GetString(Object, TEXT("simulationStages"));
				Emitter.ParameterDefaults = GetString(Object, TEXT("parameterDefaults"));
				Emitter.Stacks = StacksFromJson(Object, TEXT("stacks"));
				const TSharedPtr<FJsonObject>* Aux = nullptr;
				if (Object.IsValid() && Object->TryGetObjectField(TEXT("aux"), Aux))
				{
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Names : (*Aux)->Values)
					{
						Emitter.AuxNodeNames.Add(Names.Key, FromJsonArray(*Aux, *Names.Key));
					}
				}
			}
		}
		return true;
	}

	bool DecideIncrementalScope(const FPlanDigest& Old, const FPlanDigest& New, FIncrementalScope& OutScope,
		FString& OutWhyNot)
	{
		OutScope = FIncrementalScope();
		OutScope.Old = &Old;
		OutScope.bUserVariables = Old.UserVariables != New.UserVariables;
		OutScope.bSystemProperties = Old.SystemProperties != New.SystemProperties;

		// Surviving emitters keep their handles in the order the asset already has them, whatever
		// order the source lists them in, and new ones are appended -- in both build paths. A
		// reordering is still refused, because nothing proves it is inert for every consumer of the
		// handle order, and it is rare enough that a full rebuild costs nothing that matters.
		TArray<FString> OldCommon;
		TArray<FString> NewCommon;
		for (const FString& Name : Old.EmitterOrder)
		{
			if (New.Emitters.Contains(Name)) { OldCommon.Add(Name); }
			else { OutScope.RemovedEmitters.Add(Name); }
		}
		for (const FString& Name : New.EmitterOrder)
		{
			if (Old.Emitters.Contains(Name)) { NewCommon.Add(Name); }
		}
		if (OldCommon != NewCommon)
		{
			OutWhyNot = TEXT("the emitters were reordered");
			return false;
		}

		FString WhyWhole;
		if (!DecideStacks(Old.SystemStacks, New.SystemStacks, /*bUndeclaredStacksKeepContents=*/true,
			OutScope.SystemStacks, WhyWhole))
		{
			// Both system stacks share one graph, so rebuilding all of the declared ones is the same
			// sequence the full rebuild runs: clear them all, then add stack by stack.
			OutScope.SystemStacks.Reset();
			for (const TPair<FString, FStackDigest>& Stack : New.SystemStacks)
			{
				OutScope.SystemStacks.Add(Stack.Key).Action = EStackAction::Rebuild;
			}
		}

		bool bAnyEmitterSetOrRendererChange = OutScope.RemovedEmitters.Num() > 0;

		for (const FString& Name : New.EmitterOrder)
		{
			const FEmitterDigest& NewEmitter = New.Emitters.FindChecked(Name);
			const FEmitterDigest* OldEmitter = Old.Emitters.Find(Name);
			FEmitterChange Change;

			if (OldEmitter == nullptr)
			{
				Change.Action = EEmitterAction::Add;
				bAnyEmitterSetOrRendererChange = true;
				OutScope.Emitters.Add(Name, MoveTemp(Change));
				continue;
			}

			const bool bHasParent = !NewEmitter.Parent.IsEmpty();
			const bool bParentChanged = OldEmitter->Parent != NewEmitter.Parent;
			const bool bSimTargetChanged = OldEmitter->SimTarget != NewEmitter.SimTarget;

			// An emitter built from a parent restores the parent's baseline before its stacks are
			// rewritten, and that baseline is what an undeclared stack falls back to. The incremental
			// path has no way to restore one stack of it, so any structural change there is whole-emitter.
			bool bWhole = bParentChanged || bSimTargetChanged
				|| (bHasParent && OldEmitter->RendererCount > 0 && NewEmitter.RendererCount == 0);

			if (!bWhole)
			{
				if (!DecideStacks(OldEmitter->Stacks, NewEmitter.Stacks, /*bUndeclaredStacksKeepContents=*/false,
					Change.Stacks, WhyWhole))
				{
					bWhole = true;
				}
				else if (bHasParent && Change.HasStructuralStackChange())
				{
					bWhole = true;
				}
			}

			if (bWhole)
			{
				Change = FEmitterChange();
				Change.Action = EEmitterAction::Rebuild;
				bAnyEmitterSetOrRendererChange = true;
				OutScope.Emitters.Add(Name, MoveTemp(Change));
				continue;
			}

			Change.bProperties = OldEmitter->Properties != NewEmitter.Properties;
			Change.bRenderers = OldEmitter->Renderers != NewEmitter.Renderers;
			Change.bEventHandlers = OldEmitter->EventHandlers != NewEmitter.EventHandlers;
			Change.bSimulationStages = OldEmitter->SimulationStages != NewEmitter.SimulationStages;
			Change.bParameterDefaults = OldEmitter->ParameterDefaults != NewEmitter.ParameterDefaults;
			bAnyEmitterSetOrRendererChange |= Change.bRenderers;

			const bool bTouched = Change.bProperties || Change.bRenderers || Change.bEventHandlers
				|| Change.bSimulationStages || Change.bParameterDefaults || Change.Stacks.Num() > 0;
			if (bTouched)
			{
				Change.Action = EEmitterAction::Partial;
				OutScope.Emitters.Add(Name, MoveTemp(Change));
			}
		}

		// A self-reference names an emitter handle or a renderer by position and is resolved once
		// every emitter and renderer exists. When either set changes, every such reference in the
		// system has to be resolved again -- including ones in stacks this build would not touch.
		if ((Old.bHasSelfReferences || New.bHasSelfReferences) && bAnyEmitterSetOrRendererChange)
		{
			OutWhyNot = TEXT("an emitter or renderer that data interfaces refer to by position changed");
			return false;
		}

		return true;
	}

	FString FIncrementalScope::Describe() const
	{
		TArray<FString> Parts;
		if (bUserVariables) { Parts.Add(TEXT("user parameters")); }
		if (bSystemProperties) { Parts.Add(TEXT("system settings")); }
		DescribeStacks(SystemStacks, TEXT("system "), Parts);
		for (const FString& Name : RemovedEmitters)
		{
			Parts.Add(FString::Printf(TEXT("remove %s"), *Name));
		}
		for (const TPair<FString, FEmitterChange>& Emitter : Emitters)
		{
			const FEmitterChange& Change = Emitter.Value;
			if (Change.Action == EEmitterAction::Add) { Parts.Add(FString::Printf(TEXT("add %s"), *Emitter.Key)); continue; }
			if (Change.Action == EEmitterAction::Rebuild) { Parts.Add(FString::Printf(TEXT("rebuild %s"), *Emitter.Key)); continue; }
			if (Change.bProperties) { Parts.Add(Emitter.Key + TEXT(" settings")); }
			if (Change.bRenderers) { Parts.Add(Emitter.Key + TEXT(" renderers")); }
			if (Change.bEventHandlers) { Parts.Add(Emitter.Key + TEXT(" event handlers")); }
			if (Change.bSimulationStages) { Parts.Add(Emitter.Key + TEXT(" simulation stages")); }
			if (Change.bParameterDefaults) { Parts.Add(Emitter.Key + TEXT(" parameter defaults")); }
			DescribeStacks(Change.Stacks, Emitter.Key + TEXT(" "), Parts);
		}
		return Parts.Num() > 0 ? FString::Join(Parts, TEXT(", ")) : TEXT("nothing changed");
	}
}
