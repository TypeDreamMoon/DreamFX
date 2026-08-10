#include "DreamFXNiagaraAdapter.h"

#include "DreamFXModule.h"

#include "EdGraphSchema_Niagara.h"
#include "NiagaraCommon.h"
#include "NiagaraDataInterface.h"
#include "NiagaraDataInterfaceCurveBase.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraEmitterFactoryNew.h"
#include "NiagaraExternalSystemEditorUtilities.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "NiagaraVariant.h"
#include "UpgradeNiagaraScriptResults.h"

#include "Dom/JsonObject.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformMemory.h"
#include "JsonObjectConverter.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectIterator.h"

namespace UE::DreamFX::Editor
{
	namespace
	{
		/** Drains an edit context's error list into the adapter's plain-string error channel. */
		bool Drain(FNiagaraExternalEditContext& Context, TArray<FString>& OutErrors)
		{
			const bool bHadErrors = Context.HasErrors();
			for (const FText& Error : Context.Errors)
			{
				OutErrors.Add(Error.ToString());
			}
			return !bHadErrors;
		}

		/**
		 * The contexts open scopes are sharing, by system.
		 *
		 * Empty except inside FNiagaraAdapter::FReadScope or FWriteScope, so nothing changes for any
		 * caller that has not asked for the sharing.
		 */
		TMap<UNiagaraSystem*, TSharedPtr<FNiagaraExternalEditContext>> GSharedContexts;

		/**
		 * Systems with an open FWriteScope.
		 *
		 * Separate from the map above because a write scope outlives the contexts it hands out: a
		 * structural operation drops the shared context and the next call builds the epoch's new one,
		 * so "sharing is enabled" and "a context exists right now" are two different facts.
		 */
		TSet<UNiagaraSystem*> GWriteScopedSystems;

		/** Systems with an open FReadScope, for the both-at-once guard. */
		TSet<UNiagaraSystem*> GReadScopedSystems;

		/** False restores the pre-P1 behaviour: one context per call. See SetWriteScopeEnabled. */
		bool GWriteScopeEnabled = true;

		/** True restores the pre-P3 behaviour: every structural mutator drops the context. */
		bool GRebuildContextOnStructural = false;

		/** True restores the pre-switch-refresh behaviour: every static-switch write drops the context. */
		bool GRebuildContextOnSwitch = false;

		/** False restores the per-add engine stack refresh instead of one batch refresh per stack. */
		bool GBatchAddRefresh = true;

		/**
		 * Wall time and call count per operation, for `-LogCmds="LogDreamFX Verbose"`.
		 *
		 * The counters below say how many times something happened; these say what it cost. Added
		 * after the first two attempts to explain a slow build from log timestamps both reached the
		 * wrong conclusion -- the only readings that have survived contact are the ones taken with a
		 * clock around the thing being blamed.
		 */
		TMap<FString, double> GOpSeconds;
		TMap<FString, int32> GOpCounts;

		/** Charges the enclosing scope to one operation label. */
		struct FOpTimer
		{
			explicit FOpTimer(const TCHAR* InLabel)
				: Label(InLabel), Start(FPlatformTime::Seconds()) {}

			~FOpTimer()
			{
				GOpSeconds.FindOrAdd(Label) += FPlatformTime::Seconds() - Start;
				++GOpCounts.FindOrAdd(Label);
			}

			FOpTimer(const FOpTimer&) = delete;
			FOpTimer& operator=(const FOpTimer&) = delete;

			FString Label;
			double Start;
		};

		/** One-shot counters for the build's summary line (plan-v6 P0). */
		struct FAdapterStats
		{
			int32 ContextsBuilt = 0;
			int32 StructuralCalls = 0;
			int32 ValueCalls = 0;
			int32 EpochBoundaries = 0;

			/** Of the structural calls, the ones that were a static switch write. */
			int32 StaticSwitchCalls = 0;

			/** Of the structural calls, the ones that kept the context because the engine refreshed. */
			int32 RefreshedInPlaceCalls = 0;
		};
		FAdapterStats GStats;

		/** The shared context if this system has one open, otherwise a private one for this call. */
		class FEditContext
		{
		public:
			explicit FEditContext(UNiagaraSystem* System)
			{
				if (TSharedPtr<FNiagaraExternalEditContext>* Shared = GSharedContexts.Find(System))
				{
					if (Shared->IsValid())
					{
						Context = Shared->Get();

						// Errors accumulate on a context and Drain does not clear them. A shared one
						// has to start each call clean, or the first failure makes every later call
						// report it too -- and return false.
						Context->Errors.Reset();
						return;
					}
				}

				++GStats.ContextsBuilt;

				// Charged separately from whatever operation happens to trigger it: building a context
				// is the same cost wherever it lands, and attributing it to the caller that unluckily
				// crossed an epoch boundary would smear it across every label.
				FOpTimer Timer(TEXT("~context build (view model)"));

				// Inside a write scope the epoch's context is built on first use rather than at the
				// boundary, so a structural operation that is never followed by a write costs nothing.
				if (GWriteScopedSystems.Contains(System))
				{
					TSharedPtr<FNiagaraExternalEditContext> Fresh = MakeShared<FNiagaraExternalEditContext>(System);
					Context = Fresh.Get();
					GSharedContexts.Add(System, MoveTemp(Fresh));
					return;
				}

				Owned = MakeUnique<FNiagaraExternalEditContext>(System);
				Context = Owned.Get();
			}

			FNiagaraExternalEditContext& Get() { return *Context; }

		private:
			TUniquePtr<FNiagaraExternalEditContext> Owned;
			FNiagaraExternalEditContext* Context = nullptr;
		};

		/**
		 * Drops the shared context, so the next FEditContext on this system builds a fresh view model.
		 *
		 * Only a write scope has epochs. Guarding on it keeps a structural call made while a read scope
		 * happens to be open -- the decompiler builds a host system for a standalone emitter this way --
		 * from quietly dropping that scope's context and costing it the sharing.
		 */
		void DropSharedContext(UNiagaraSystem* System)
		{
			if (System != nullptr && GWriteScopedSystems.Contains(System)
				&& GSharedContexts.Remove(System) > 0)
			{
				++GStats.EpochBoundaries;
			}
		}

		/** Whether the engine refreshed what this operation invalidated, before it returned. */
		enum class EStructuralKind
		{
			/**
			 * It did not, or not observably. The context is dropped.
			 *
			 * The default, and where an operation belongs until someone has read the engine's
			 * implementation and found the refresh. Guessing here is not free: assuming a whole build's
			 * worth of operations refreshed in place produced a system whose data channel reads
			 * compiled to a single namespace entry.
			 */
			Unrefreshed,

			/**
			 * It did, synchronously, on the group it changed -- so the context still describes the
			 * system accurately and dropping it only pays to rebuild the other emitters.
			 *
			 * Only for operations where that call is visible in the engine source. Today:
			 * AddModule and AddSetParametersModule call ScriptItem->RefreshChildren(), RemoveModule
			 * calls it on the script it removed from, and AddEmitter goes as far as RefreshAll().
			 *
			 * The cached data these operations invalidate is covered too, by the view model itself:
			 * EmitterScriptGraphChanged drops that emitter's stack module data on any graph edit, and
			 * SystemScriptGraphChanged empties the whole map.
			 */
			RefreshedInPlace,
		};

		/**
		 * Records a structural operation, and drops the context unless the engine already refreshed.
		 *
		 * A refresh the engine *defers* does not count as refreshed: a commandlet never pumps the tick
		 * that would run it. That is also why the deliberate drop stayed -- see EndStructuralEpoch.
		 *
		 * Cheap and idempotent outside a write scope, which is what lets every structural mutator call
		 * it unconditionally instead of asking first.
		 */
		void EndEpoch(UNiagaraSystem* System, EStructuralKind Kind = EStructuralKind::Unrefreshed)
		{
			++GStats.StructuralCalls;

			if (Kind == EStructuralKind::Unrefreshed || GRebuildContextOnStructural)
			{
				DropSharedContext(System);
			}
			else
			{
				++GStats.RefreshedInPlaceCalls;
			}
		}

		/**
		 * Ends the epoch when the enclosing call returns, by whichever of its paths.
		 *
		 * Declare it *before* the FEditContext it accompanies. Destruction runs in reverse, so the
		 * context holder is released first and this then drops the shared context -- rather than
		 * destroying the context out from under a holder that still points at it.
		 */
		struct FEpochGuard
		{
			explicit FEpochGuard(UNiagaraSystem* InSystem, EStructuralKind InKind = EStructuralKind::Unrefreshed)
				: System(InSystem), Kind(InKind) {}
			~FEpochGuard() { EndEpoch(System, Kind); }

			FEpochGuard(const FEpochGuard&) = delete;
			FEpochGuard& operator=(const FEpochGuard&) = delete;

			UNiagaraSystem* System = nullptr;
			EStructuralKind Kind = EStructuralKind::Unrefreshed;
		};

		/**
		 * Rewrites the `{"refPath": "..."}` spelling of an object reference to the bare path string,
		 * throughout a property JSON blob.
		 *
		 * UE 5.8.1 replaced the external edit API's property importer: 5.8.0 called
		 * `UToolsetLibrary::SetObjectProperties(..., EBypassContainerCheck::Yes)`, 5.8.1 calls
		 * `FJsonObjectConverter::JsonObjectToUStruct` directly. The exporter was not changed with it, so
		 * the engine now emits object references in a shape its own importer rejects, and every property
		 * that holds one fails to apply -- a mesh renderer's `Meshes`, an audio player's `SoundToPlay`.
		 * The failure is per-object and total: one bad reference fails the whole blob.
		 *
		 * Measured rather than guessed: `[]` imports, `[{"scale":{...}}]` imports, and
		 * `[{"mesh":{"refPath":"..."}}]` does not while `[{"mesh":"..."}]` does.
		 *
		 * This runs on the way into the engine and not on the way out, so the exported text keeps the
		 * engine's own spelling. When Epic fixes the importer this function is the only thing to delete.
		 */
		TSharedPtr<FJsonValue> RewriteObjectReferences(const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value.IsValid())
			{
				return Value;
			}

			if (Value->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject> Object = Value->AsObject();

				// Exactly one field, named refPath, holding a string: that shape is the exporter's, not
				// something a Niagara property struct declares, so there is nothing else it could be.
				if (Object->Values.Num() == 1)
				{
					if (const TSharedPtr<FJsonValue>* RefPath = Object->Values.Find(TEXT("refPath")))
					{
						if ((*RefPath)->Type == EJson::String)
						{
							return MakeShared<FJsonValueString>((*RefPath)->AsString());
						}
					}
				}

				const TSharedRef<FJsonObject> Rewritten = MakeShared<FJsonObject>();
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
				{
					Rewritten->SetField(Field.Key, RewriteObjectReferences(Field.Value));
				}
				return MakeShared<FJsonValueObject>(Rewritten);
			}

			if (Value->Type == EJson::Array)
			{
				TArray<TSharedPtr<FJsonValue>> Rewritten;
				for (const TSharedPtr<FJsonValue>& Element : Value->AsArray())
				{
					Rewritten.Add(RewriteObjectReferences(Element));
				}
				return MakeShared<FJsonValueArray>(Rewritten);
			}

			return Value;
		}

		/** The blob as the engine's importer can read it. Returns the input unchanged if it will not parse. */
		FString NormalizeObjectReferences(const FString& PropertiesJson)
		{
			if (PropertiesJson.IsEmpty())
			{
				return PropertiesJson;
			}

			TSharedPtr<FJsonObject> Parsed;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(PropertiesJson);
			if (!FJsonSerializer::Deserialize(Reader, Parsed) || !Parsed.IsValid())
			{
				// Not our JSON to fix. Let the engine report whatever it makes of it.
				return PropertiesJson;
			}

			const TSharedPtr<FJsonValue> Rewritten =
				RewriteObjectReferences(MakeShared<FJsonValueObject>(Parsed));

			FString Out;
			const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
			FJsonSerializer::Serialize(Rewritten->AsObject().ToSharedRef(), Writer);
			return Out;
		}

		FNiagaraExt_StackItemReference ToReference(const FStackAddress& Address)
		{
			FNiagaraExt_StackItemReference Reference(Address.System, Address.EmitterName, Address.ScriptName, Address.ModuleName);
			Reference.RendererIndex = Address.RendererIndex;
			Reference.InputNameStack = Address.InputNameStack;
			return Reference;
		}

		/** Fills a stack input value struct from the adapter's neutral representation. */
		bool ToStackInputValue(const FInputValue& Value, FNiagaraExt_StackInputValue& OutValue, TArray<FString>& OutErrors)
		{
			switch (Value.Mode)
			{
			case EInputValueMode::Literal:
				if (Value.LiteralStruct == nullptr || Value.LiteralBytes.Num() == 0)
				{
					OutErrors.Add(TEXT("Internal error: literal input value has no struct or no data."));
					return false;
				}
				OutValue.InitializeAs(Value.LiteralStruct, Value.LiteralBytes.GetData());
				return true;

			case EInputValueMode::Enum:
			{
				FNiagaraExt_StackInputData_Enum& Data = OutValue.InitializeAs<FNiagaraExt_StackInputData_Enum>();
				Data.Enum = Value.EnumType;
				Data.EnumName = Value.EnumEntryName;
				return true;
			}

			case EInputValueMode::Linked:
			{
				FNiagaraExt_StackInputData_Linked& Data = OutValue.InitializeAs<FNiagaraExt_StackInputData_Linked>();
				Data.LinkedVariable.Name = Value.LinkedVariable.GetName();
				Data.LinkedVariable.Type = Value.LinkedVariable.GetType();
				return true;
			}

			case EInputValueMode::Hlsl:
			{
				FNiagaraExt_StackInputData_HlslExpression& Data = OutValue.InitializeAs<FNiagaraExt_StackInputData_HlslExpression>();
				Data.HlslExpression = Value.HlslExpression;
				return true;
			}

			case EInputValueMode::DynamicInput:
			{
				FNiagaraExt_StackInputData_DynamicInput& Data = OutValue.InitializeAs<FNiagaraExt_StackInputData_DynamicInput>();
				Data.DynamicInputAsset = Value.DynamicInputAsset;
				return true;
			}

			case EInputValueMode::DataInterface:
			{
				FNiagaraExt_StackInputData_DataInterface& Data = OutValue.InitializeAs<FNiagaraExt_StackInputData_DataInterface>();
				Data.PropertyValues = NormalizeObjectReferences(Value.DataInterfaceJson);
				return true;
			}

			default:
				OutErrors.Add(TEXT("Internal error: attempted to write an unset input value."));
				return false;
			}
		}

		/** Inverse of ToStackInputValue, for the decompiler. */
		void FromStackInputValue(const FNiagaraExt_StackInputValue& In, FInputValue& OutValue)
		{
			if (const FNiagaraExt_StackInputData_Enum* Data = In.GetPtr<FNiagaraExt_StackInputData_Enum>())
			{
				OutValue = FInputValue::MakeEnum(Data->Enum, Data->EnumName);
				return;
			}
			if (const FNiagaraExt_StackInputData_Linked* Data = In.GetPtr<FNiagaraExt_StackInputData_Linked>())
			{
				OutValue = FInputValue::MakeLinked(FNiagaraVariableBase(Data->LinkedVariable.Type, Data->LinkedVariable.Name));
				return;
			}
			if (const FNiagaraExt_StackInputData_HlslExpression* Data = In.GetPtr<FNiagaraExt_StackInputData_HlslExpression>())
			{
				OutValue = FInputValue::MakeHlsl(Data->HlslExpression);
				return;
			}
			if (const FNiagaraExt_StackInputData_DynamicInput* Data = In.GetPtr<FNiagaraExt_StackInputData_DynamicInput>())
			{
				OutValue = FInputValue::MakeDynamicInput(Data->DynamicInputAsset);
				return;
			}
			if (const FNiagaraExt_StackInputData_DataInterface* Data = In.GetPtr<FNiagaraExt_StackInputData_DataInterface>())
			{
				OutValue = FInputValue::MakeDataInterface(nullptr, Data->PropertyValues);
				return;
			}
			if (In.GetPtr<FNiagaraExt_StackInputData_Unsupported>() != nullptr)
			{
				OutValue = FInputValue();
				return;
			}
			if (In.IsValid())
			{
				OutValue = FInputValue::MakeLiteral(In.GetScriptStruct(), In.GetMemory());
				return;
			}
			OutValue = FInputValue();
		}

		/**
		 * Inverse of ToVariableValue.
		 *
		 * Separate from FromStackInputValue because the two carry different payload structs even
		 * though both descend from FNiagaraExt_InstancedValue: a stack input can be a link, an HLSL
		 * expression or a dynamic input chain, and a variable value never is.
		 */
		void FromVariableValue(const FNiagaraExt_VariableValue& In, FInputValue& OutValue)
		{
			if (const FNiagaraExt_VariableValue_Enum* Data = In.GetPtr<FNiagaraExt_VariableValue_Enum>())
			{
				OutValue = FInputValue::MakeEnum(Data->Enum, Data->EnumName);
				return;
			}
			if (const FNiagaraExt_VariableValue_DataInterface* Data = In.GetPtr<FNiagaraExt_VariableValue_DataInterface>())
			{
				OutValue = FInputValue::MakeDataInterface(nullptr, FString());
				return;
			}
			if (const FNiagaraExt_VariableValue_Object* Data = In.GetPtr<FNiagaraExt_VariableValue_Object>())
			{
				// The reference IS the value here, unlike a data interface. Returning an unset value
				// -- which this did -- exported the parameter as a bare declaration, and the rebuild
				// left an empty slot: the _LevelUpSpawn systems came back without the texture their
				// "LEVEL UP" text is drawn from.
				OutValue = FInputValue::MakeObjectAsset(Data->Object);
				return;
			}
			if (In.IsValid())
			{
				OutValue = FInputValue::MakeLiteral(In.GetScriptStruct(), In.GetMemory());
				return;
			}
			OutValue = FInputValue();
		}

		/** Builds the FNiagaraVariant that FNiagaraExt_VariableValue::Set expects for a user variable. */
		bool ToVariableValue(const FInputValue& Value, const FNiagaraTypeDefinition& Type,
			FNiagaraExt_VariableValue& OutValue, TArray<FString>& OutErrors)
		{
			switch (Value.Mode)
			{
			case EInputValueMode::Literal:
			{
				if (Value.LiteralBytes.Num() == 0)
				{
					OutErrors.Add(TEXT("Internal error: literal default has no data."));
					return false;
				}
				const FNiagaraVariant Variant(Value.LiteralBytes.GetData(), Value.LiteralBytes.Num());
				OutValue.Set(Type, Variant);
				return true;
			}

			case EInputValueMode::Enum:
			{
				const int32 EnumValue = Value.EnumType ? static_cast<int32>(Value.EnumType->GetValueByName(Value.EnumEntryName)) : 0;
				const FNiagaraVariant Variant(&EnumValue, sizeof(int32));
				OutValue.Set(Type, Variant);
				return true;
			}

			case EInputValueMode::DataInterface:
			{
				// Declaration only: the DI instance is created by the engine and fed at runtime (3.5).
				FNiagaraVariant Variant;
				Variant.SetDataInterface(nullptr);
				OutValue.Set(Type, Variant);
				return true;
			}

			case EInputValueMode::ObjectAsset:
			{
				// A null asset is written as deliberately as a non-null one: the source said the slot
				// is empty, and leaving whatever the previous build put there would be a rebuild that
				// does not match its source.
				FNiagaraVariant Variant;
				Variant.SetUObject(Value.ObjectAsset);
				OutValue.Set(Type, Variant);
				return true;
			}

			default:
				OutErrors.Add(TEXT("Internal error: user variable defaults must be literals, enums or data interfaces."));
				return false;
			}
		}
	}

	// -------------------------------------------------------------------------------------------
	// FStackAddress
	// -------------------------------------------------------------------------------------------

	FStackAddress FStackAddress::WithEmitter(FName InEmitterName) const
	{
		FStackAddress Copy = *this;
		Copy.EmitterName = InEmitterName;
		return Copy;
	}

	FStackAddress FStackAddress::WithScript(FName InScriptName) const
	{
		FStackAddress Copy = *this;
		Copy.ScriptName = InScriptName;
		return Copy;
	}

	FStackAddress FStackAddress::WithModule(FName InModuleName) const
	{
		FStackAddress Copy = *this;
		Copy.ModuleName = InModuleName;
		return Copy;
	}

	FStackAddress FStackAddress::WithRenderer(int32 InRendererIndex) const
	{
		FStackAddress Copy = *this;
		Copy.RendererIndex = InRendererIndex;
		return Copy;
	}

	FStackAddress FStackAddress::WithInput(FName InInputName) const
	{
		FStackAddress Copy = *this;
		Copy.InputNameStack.Add(InInputName);
		return Copy;
	}

	FStackAddress FStackAddress::WithInputPath(const TArray<FName>& InPath) const
	{
		FStackAddress Copy = *this;
		Copy.InputNameStack = InPath;
		return Copy;
	}

	// -------------------------------------------------------------------------------------------
	// FInputValue
	// -------------------------------------------------------------------------------------------

	FInputValue FInputValue::MakeLiteral(const UScriptStruct* Struct, const void* SourceMemory)
	{
		FInputValue Value;
		if (Struct == nullptr || SourceMemory == nullptr)
		{
			return Value;
		}
		Value.Mode = EInputValueMode::Literal;
		Value.LiteralStruct = Struct;
		Value.LiteralBytes.SetNumUninitialized(Struct->GetStructureSize());
		FMemory::Memcpy(Value.LiteralBytes.GetData(), SourceMemory, Struct->GetStructureSize());
		return Value;
	}

	FInputValue FInputValue::MakeEnum(UEnum* Enum, FName EntryName)
	{
		FInputValue Value;
		Value.Mode = EInputValueMode::Enum;
		Value.EnumType = Enum;
		Value.EnumEntryName = EntryName;
		return Value;
	}

	FInputValue FInputValue::MakeLinked(const FNiagaraVariableBase& Variable)
	{
		FInputValue Value;
		Value.Mode = EInputValueMode::Linked;
		Value.LinkedVariable = Variable;
		return Value;
	}

	FInputValue FInputValue::MakeHlsl(const FString& Expression)
	{
		FInputValue Value;
		Value.Mode = EInputValueMode::Hlsl;
		Value.HlslExpression = Expression;
		return Value;
	}

	FInputValue FInputValue::MakeDynamicInput(UNiagaraScript* Asset)
	{
		FInputValue Value;
		Value.Mode = EInputValueMode::DynamicInput;
		Value.DynamicInputAsset = Asset;
		return Value;
	}

	FInputValue FInputValue::MakeDataInterface(UClass* Class, const FString& Json)
	{
		FInputValue Value;
		Value.Mode = EInputValueMode::DataInterface;
		Value.DataInterfaceClass = Class;
		Value.DataInterfaceJson = Json;
		return Value;
	}

	FInputValue FInputValue::MakeObjectAsset(UObject* Asset)
	{
		FInputValue Value;
		Value.Mode = EInputValueMode::ObjectAsset;
		Value.ObjectAsset = Asset;
		return Value;
	}

	bool FInputValue::Equals(const FInputValue& Other) const
	{
		if (Mode != Other.Mode)
		{
			return false;
		}

		switch (Mode)
		{
		case EInputValueMode::Literal:
			return LiteralStruct == Other.LiteralStruct && LiteralBytes == Other.LiteralBytes;
		case EInputValueMode::Enum:
			return EnumType == Other.EnumType && EnumEntryName == Other.EnumEntryName;
		case EInputValueMode::Linked:
			return LinkedVariable == Other.LinkedVariable;
		case EInputValueMode::Hlsl:
			return HlslExpression == Other.HlslExpression;
		case EInputValueMode::DynamicInput:
			return DynamicInputAsset == Other.DynamicInputAsset;
		case EInputValueMode::DataInterface:
			return DataInterfaceJson == Other.DataInterfaceJson;
		case EInputValueMode::ObjectAsset:
			return ObjectAsset == Other.ObjectAsset;
		default:
			return true;
		}
	}

	FString NormalizeInputIdentifier(const FString& Name)
	{
		FString Result = Name;
		Result.ReplaceInline(TEXT(" "), TEXT(""), ESearchCase::CaseSensitive);
		Result.ReplaceInline(TEXT("_"), TEXT(""), ESearchCase::CaseSensitive);
		// Hyphens come from enum display labels like "Non-Uniform"; a DSL identifier cannot hold one,
		// so both sides drop it.
		Result.ReplaceInline(TEXT("-"), TEXT(""), ESearchCase::CaseSensitive);
		Result.ToLowerInline();
		// Inline edit conditions come back namespace-qualified ("Module.WriteLifetime") while ordinary
		// inputs do not ("Lifetime"). Inside a module call the namespace is implied, so it is dropped
		// on both sides rather than forcing authors to know which inputs happen to be checkboxes.
		Result.RemoveFromStart(TEXT("module."), ESearchCase::CaseSensitive);
		return Result;
	}

	FString ToInputIdentifier(FName InputName)
	{
		FString Result = InputName.ToString();
		Result.ReplaceInline(TEXT(" "), TEXT(""), ESearchCase::CaseSensitive);
		Result.RemoveFromStart(TEXT("Module."), ESearchCase::IgnoreCase);
		return ToNameToken(Result);
	}

	FString ToNameToken(const FString& Name)
	{
		if (Name.IsEmpty())
		{
			return Name;
		}

		// The lexer's own rule, segment by segment: a dotted name is bare only when every segment is.
		auto IsBareSegment = [](const FString& Segment)
		{
			if (Segment.IsEmpty() || !(FChar::IsAlpha(Segment[0]) || Segment[0] == TEXT('_')))
			{
				return false;
			}
			for (const TCHAR Character : Segment)
			{
				if (!FChar::IsAlnum(Character) && Character != TEXT('_'))
				{
					return false;
				}
			}
			return true;
		};

		TArray<FString> Segments;
		Name.ParseIntoArray(Segments, TEXT("."), /*InCullEmpty=*/false);

		bool bBare = Segments.Num() > 0;
		for (const FString& Segment : Segments)
		{
			bBare = bBare && IsBareSegment(Segment);
		}

		if (bBare)
		{
			return Name;
		}

		// A name containing a back-quote has no representation at all; the escape would need an
		// escape. Nothing in Niagara produces one, and inventing a rule for it now would be a rule
		// nobody could check.
		return FString::Printf(TEXT("`%s`"), *Name);
	}

	const FInputSchema* FModuleSchema::FindInput(FName Name) const
	{
		return Inputs.FindByPredicate([Name](const FInputSchema& Input) { return Input.Name == Name; });
	}

	const FInputSchema* FModuleSchema::FindInputByIdentifier(const FString& Identifier) const
	{
		if (const FInputSchema* Exact = Inputs.FindByPredicate(
			[&Identifier](const FInputSchema& Input) { return Input.Name.ToString() == Identifier; }))
		{
			return Exact;
		}

		const FString Normalized = NormalizeInputIdentifier(Identifier);
		return Inputs.FindByPredicate([&Normalized](const FInputSchema& Input)
		{
			return NormalizeInputIdentifier(Input.Name.ToString()) == Normalized;
		});
	}

	void FModuleSchema::FindInputsByIdentifier(const FString& Identifier, TArray<const FInputSchema*>& OutMatches) const
	{
		// Exact first, then the rest of the normalised matches, so a caller that takes the first entry
		// gets what FindInputByIdentifier would have given it.
		for (const FInputSchema& Input : Inputs)
		{
			if (Input.Name.ToString() == Identifier)
			{
				OutMatches.Add(&Input);
			}
		}

		const FString Normalized = NormalizeInputIdentifier(Identifier);
		for (const FInputSchema& Input : Inputs)
		{
			if (Input.Name.ToString() != Identifier
				&& NormalizeInputIdentifier(Input.Name.ToString()) == Normalized)
			{
				OutMatches.Add(&Input);
			}
		}
	}

	const FInputInfo* FModuleInfo::FindInput(FName Name) const
	{
		return Inputs.FindByPredicate([Name](const FInputInfo& Input) { return Input.Name == Name; });
	}

	const FScriptStackInfo* FEmitterInfo::FindStack(FName ScriptName) const
	{
		return Stacks.FindByPredicate([ScriptName](const FScriptStackInfo& Stack) { return Stack.ScriptName == ScriptName; });
	}

	// -------------------------------------------------------------------------------------------
	// System lifecycle
	// -------------------------------------------------------------------------------------------

	UNiagaraSystem* FNiagaraAdapter::AcquireSystem(const FString& PackagePath, const FString& AssetName,
		bool& bOutCreated, TArray<FString>& OutErrors)
	{
		bOutCreated = false;
		const FString PackageName = PackagePath / AssetName;

		if (FPackageName::DoesPackageExist(PackageName))
		{
			// R9. CreateNiagaraSystem does no collision handling at all; a partially loaded package
			// later trips ValidatePackage's appError inside SavePackage and kills the process. Loading
			// fully first is what makes regeneration -- the normal case -- survivable.
			UPackage* Package = LoadPackage(nullptr, *PackageName, LOAD_None);
			if (Package == nullptr)
			{
				OutErrors.Add(FString::Printf(TEXT("Package '%s' exists on disk but could not be loaded."), *PackageName));
				return nullptr;
			}
			Package->FullyLoad();

			if (UNiagaraSystem* Existing = FindObject<UNiagaraSystem>(Package, *AssetName))
			{
				// Reusing the object, not recreating it, is what keeps the 4.5 identity contract:
				// every level actor, blueprint and sequencer reference survives regeneration.
				return Existing;
			}

			OutErrors.Add(FString::Printf(
				TEXT("Package '%s' exists but does not contain a Niagara System named '%s'. Refusing to overwrite it."),
				*PackageName, *AssetName));
			return nullptr;
		}

		FNiagaraExternalEditContext Context;
		UNiagaraSystem* System = UNiagaraExternalEditUtilities::CreateNiagaraSystem(AssetName, PackagePath, nullptr, Context);
		Drain(Context, OutErrors);

		if (System == nullptr)
		{
			OutErrors.Add(FString::Printf(TEXT("Failed to create Niagara System '%s' at '%s'."), *AssetName, *PackagePath));
			return nullptr;
		}

		bOutCreated = true;
		return System;
	}

	FNiagaraAdapter::FReadScope::FReadScope(UNiagaraSystem* InSystem)
		: System(InSystem)
	{
		// Nested scopes on one system are harmless and the inner one simply defers to the outer: only
		// the scope that created the context removes it.
		if (System != nullptr && !GSharedContexts.Contains(System))
		{
			checkf(!GWriteScopedSystems.Contains(System),
				TEXT("DreamFX: opening a read scope on a system that already has a write scope open. ")
				TEXT("TNiagaraViewModelManager refuses a second live view model for one system."));

			++GStats.ContextsBuilt;
			GSharedContexts.Add(System, MakeShared<FNiagaraExternalEditContext>(System));
			GReadScopedSystems.Add(System);
			bOwns = true;
		}
	}

	FNiagaraAdapter::FReadScope::~FReadScope()
	{
		if (bOwns)
		{
			GSharedContexts.Remove(System);
			GReadScopedSystems.Remove(System);
		}
	}

	FNiagaraAdapter::FWriteScope::FWriteScope(UNiagaraSystem* InSystem)
		: System(InSystem)
	{
		if (System != nullptr && GWriteScopeEnabled && !GWriteScopedSystems.Contains(System))
		{
			checkf(!GReadScopedSystems.Contains(System),
				TEXT("DreamFX: opening a write scope inside a read scope on the same system. The read ")
				TEXT("scope's view model would go stale at the first structural write."));

			// Only the enabling flag is set here. The epoch's context is built on first use, so a
			// scope that turns out to write nothing costs nothing.
			GWriteScopedSystems.Add(System);
			bOwns = true;
		}
	}

	FNiagaraAdapter::FWriteScope::~FWriteScope()
	{
		if (bOwns)
		{
			GSharedContexts.Remove(System);
			GWriteScopedSystems.Remove(System);
		}
	}

	void FNiagaraAdapter::EndStructuralEpoch(UNiagaraSystem* System)
	{
		// Counting here is what separates the callers who ask for this -- the generator's retry of a
		// refused write, and a switch write when -RebuildOnSwitch restores the old behaviour -- from
		// the Add/Remove operations.
		++GStats.StaticSwitchCalls;

		// Unconditional, unlike the automatic drop above. The caller is asking for a context that
		// reflects a change the engine did not refresh synchronously -- the inline edit conditions
		// that gate other inputs with no schema flag anyone can see. A fresh view model is the only
		// thing that sees them.
		DropSharedContext(System);
	}

	void FNiagaraAdapter::OnStaticSwitchWritten(UNiagaraSystem* System)
	{
		// Unconditional since switches moved to the pin route. The engine's external write path used
		// to refresh the owning module item synchronously, which let the epoch survive a switch; a pin
		// default written straight onto the node has no such courtesy -- nothing tells the live edit
		// context that a whole set of sibling inputs just came into existence. Every later call would
		// be reading a view model built before the switch.
		//
		// So the previous in-place path, and the -RebuildOnSwitch flag that A/B'd it, are both gone.
		// One rebuild per switch write is the price of not needing the engine patch at all.
		EndStructuralEpoch(System);
	}

	void FNiagaraAdapter::SetWriteScopeEnabled(bool bEnabled)
	{
		GWriteScopeEnabled = bEnabled;
	}

	void FNiagaraAdapter::SetRebuildContextOnStructural(bool bEnabled)
	{
		GRebuildContextOnStructural = bEnabled;
	}

	void FNiagaraAdapter::SetRebuildContextOnSwitch(bool bEnabled)
	{
		GRebuildContextOnSwitch = bEnabled;
	}

	void FNiagaraAdapter::SetBatchAddRefresh(bool bEnabled)
	{
		GBatchAddRefresh = bEnabled;
	}

	bool FNiagaraAdapter::IsBatchAddRefreshEnabled()
	{
		return GBatchAddRefresh;
	}

	void FNiagaraAdapter::ResetStats()
	{
		GStats = FAdapterStats();
		GOpSeconds.Reset();
		GOpCounts.Reset();
#if DREAMFX_HAS_NIAGARA_FAST_EDIT
		// The accumulator is part of the MoonEngine addition, so the probe that gates the fast-edit
		// calls has to gate the counters they feed too. Forcing the define off on MoonEngine does not
		// catch this: the declaration is still in the header there, so it compiles either way. Only a
		// build against an engine that genuinely lacks it fails, which is what found this.
		FNiagaraExternalEditStepStats::Reset();
#endif
	}

	void FNiagaraAdapter::ReportOperationTimings()
	{
		if (GOpSeconds.Num() == 0)
		{
			return;
		}

		TArray<TPair<FString, double>> Sorted;
		for (const TPair<FString, double>& Entry : GOpSeconds)
		{
			Sorted.Add(Entry);
		}
		Sorted.Sort([](const TPair<FString, double>& A, const TPair<FString, double>& B)
		{
			return A.Value > B.Value;
		});

		double Total = 0.0;
		for (const TPair<FString, double>& Entry : Sorted)
		{
			Total += Entry.Value;
		}

		UE_LOG(LogDreamFX, Display, TEXT("=== adapter time by operation (%.1f s accounted) ==="), Total);
		for (const TPair<FString, double>& Entry : Sorted)
		{
			const int32 Count = GOpCounts.FindRef(Entry.Key);
			UE_LOG(LogDreamFX, Display, TEXT("  %8.2f s  %5.1f%%  %6d x  %7.2f ms each  %s"),
				Entry.Value,
				Total > 0.0 ? 100.0 * Entry.Value / Total : 0.0,
				Count,
				Count > 0 ? (Entry.Value * 1000.0 / Count) : 0.0,
				*Entry.Key);
		}

		// The engine-side step accumulators (SETINPUT / SETLOCAL / INIT / REFRESHALL) — the
		// breakdown INSIDE the operations above, from the same run.
#if DREAMFX_HAS_NIAGARA_FAST_EDIT
		TArray<FString> EngineSteps;
		FNiagaraExternalEditStepStats::BuildReport(EngineSteps);
		if (EngineSteps.Num() > 0)
		{
			UE_LOG(LogDreamFX, Display, TEXT("=== engine external-edit steps ==="));
			for (const FString& Line : EngineSteps)
			{
				UE_LOG(LogDreamFX, Display, TEXT("  %s"), *Line);
			}
		}
#endif
	}

	FString FNiagaraAdapter::ReportStats()
	{
		return FString::Printf(
			TEXT("adapter: %d view model(s) built, %d structural (%d static switch, %d refreshed in place) ")
			TEXT("+ %d value call(s), %d epoch boundary(ies)"),
			GStats.ContextsBuilt, GStats.StructuralCalls, GStats.StaticSwitchCalls,
			GStats.RefreshedInPlaceCalls, GStats.ValueCalls, GStats.EpochBoundaries);
	}

	bool FNiagaraAdapter::SaveSystem(UNiagaraSystem* System, TArray<FString>& OutErrors)
	{
		if (System == nullptr)
		{
			OutErrors.Add(TEXT("Cannot save a null system."));
			return false;
		}

		UPackage* Package = System->GetOutermost();
		Package->MarkPackageDirty();

		const FString FileName = FPackageName::LongPackageNameToFilename(
			Package->GetName(), FPackageName::GetAssetPackageExtension());

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;

		if (!UPackage::SavePackage(Package, System, *FileName, SaveArgs))
		{
			OutErrors.Add(FString::Printf(TEXT("SavePackage failed for '%s'."), *FileName));
			return false;
		}
		return true;
	}

	// -------------------------------------------------------------------------------------------
	// Topology reads
	// -------------------------------------------------------------------------------------------

	bool FNiagaraAdapter::GetEmitterNames(UNiagaraSystem* System, TArray<FName>& OutNames, TArray<FString>& OutErrors)
	{
		if (System == nullptr)
		{
			OutErrors.Add(TEXT("Cannot read emitters from a null system."));
			return false;
		}

		FEditContext ContextHolder(System);
		FNiagaraExternalEditContext& Context = ContextHolder.Get();
		FNiagaraExt_SystemSummary Summary;
		UNiagaraExternalEditUtilities::GetSystemSummary(System, Summary, Context);

		for (const FNiagaraExt_EmitterSummary& Emitter : Summary.Emitters)
		{
			OutNames.Add(Emitter.EmitterName);
		}
		return Drain(Context, OutErrors);
	}

	namespace
	{
		FParameterDefault::EMode FromNiagaraDefaultMode(ENiagaraDefaultMode Mode)
		{
			switch (Mode)
			{
			case ENiagaraDefaultMode::Value:   return FParameterDefault::EMode::Value;
			case ENiagaraDefaultMode::Binding: return FParameterDefault::EMode::Binding;
			case ENiagaraDefaultMode::Custom:  return FParameterDefault::EMode::Custom;
			default:                           return FParameterDefault::EMode::Fail;
			}
		}

		ENiagaraDefaultMode ToNiagaraDefaultMode(FParameterDefault::EMode Mode)
		{
			switch (Mode)
			{
			case FParameterDefault::EMode::Value:   return ENiagaraDefaultMode::Value;
			case FParameterDefault::EMode::Binding: return ENiagaraDefaultMode::Binding;
			case FParameterDefault::EMode::Custom:  return ENiagaraDefaultMode::Custom;
			default:                                return ENiagaraDefaultMode::FailIfPreviouslyNotSet;
			}
		}
	}

	bool FNiagaraAdapter::ClearScriptStack(const FStackAddress& ScriptAddress, TArray<FString>& OutErrors)
	{
		FOpTimer OpTimer(TEXT("ClearScriptStack"));

#if DREAMFX_HAS_NIAGARA_FAST_EDIT
		// Same classification as RemoveModule: the engine refreshes the group it emptied before
		// returning, so the context still describes the system accurately.
		FEpochGuard Epoch(ScriptAddress.System, EStructuralKind::RefreshedInPlace);
		FEditContext ContextHolder(ScriptAddress.System);
		FNiagaraExternalEditContext& Context = ContextHolder.Get();
		UNiagaraExternalEditUtilities::ClearScriptStack(ToReference(ScriptAddress), Context);
		return Drain(Context, OutErrors);
#else
		// Stock engine: no single-call clear, so remove the modules one at a time. This is the exact
		// cost ClearScriptStack was added to remove -- the engine rebuilds the group after each
		// removal, n times to reach a state that is empty either way -- and it runs on four stacks of
		// every emitter of every asset. Correct, just slower.
		//
		// Back to front, because removing by name shifts nothing but removing by position would.
		FScriptStackInfo Info;
		if (!GetScriptStackInfo(ScriptAddress, Info, OutErrors))
		{
			return false;
		}

		bool bOk = true;
		for (int32 Index = Info.Modules.Num() - 1; Index >= 0; --Index)
		{
			bOk &= RemoveModule(ScriptAddress.WithModule(Info.Modules[Index].ModuleName), OutErrors);
		}
		return bOk;
#endif
	}

	bool FNiagaraAdapter::GetParameterDefaults(const FStackAddress& EmitterAddress,
		TArray<FParameterDefault>& OutDefaults, TArray<FString>& OutErrors)
	{
		FOpTimer OpTimer(TEXT("read: GetParameterDefaults"));

#if !DREAMFX_HAS_NIAGARA_FAST_EDIT
		// Stock engine: no reader for a graph parameter's default. Reporting none is honest rather
		// than merely convenient -- the export then carries no Defaults block, which matches what the
		// writer below can reproduce. An asset authored on MoonEngine WITH defaults round-trips
		// lossily here, and that is the documented limit of the stock-engine path.
		return true;
#else
		FEditContext ContextHolder(EmitterAddress.System);
		FNiagaraExternalEditContext& Context = ContextHolder.Get();

		TArray<FNiagaraExt_ParameterDefault> Defaults;
		UNiagaraExternalEditUtilities::GetEmitterParameterDefaults(ToReference(EmitterAddress), Defaults, Context);

		for (const FNiagaraExt_ParameterDefault& Entry : Defaults)
		{
			FParameterDefault& Out = OutDefaults.AddDefaulted_GetRef();
			Out.Variable = FNiagaraVariableBase(Entry.Variable.Type, Entry.Variable.Name);
			Out.Mode = FromNiagaraDefaultMode(Entry.Mode);
			Out.Binding = Entry.Binding;
			if (Out.Mode == FParameterDefault::EMode::Value)
			{
				FromVariableValue(Entry.Value, Out.Value);
			}
		}
		return Drain(Context, OutErrors);
#endif
	}

	bool FNiagaraAdapter::SetParameterDefault(const FStackAddress& EmitterAddress,
		const FParameterDefault& Default, TArray<FString>& OutErrors)
	{
		FOpTimer OpTimer(TEXT("SetParameterDefault"));

#if !DREAMFX_HAS_NIAGARA_FAST_EDIT
		// Stock engine: the only functional casualty of this path, and worth stating exactly.
		//
		// Writing a parameter's default is how the generator turns a graph parameter created by a
		// link write from FailIfPreviouslyNotSet into Value. Left on Fail, Niagara refuses to compile
		// a read of it -- the DFX6001 this project spent a day on. There is no stock API that reaches
		// UNiagaraScriptVariable::DefaultMode: UNiagaraGraph::AddParameter is the way in, and it
		// carries no export macro on a stock engine.
		//
		// A no-op rather than an error, because whether it actually bites depends on the source: an
		// asset whose reads are all of stock attributes never creates such a parameter. Sources that
		// do will fail at the Niagara compile with a clear message, which is a better place to find
		// out than a build-time refusal that stops assets that would have been fine.
		return true;
#else
		// Structural: this creates the graph parameter when it is not there, and changes which pins
		// the map-get node carries. Nothing in the engine refreshes for it, so the epoch ends.
		FEpochGuard Epoch(EmitterAddress.System);
		FEditContext ContextHolder(EmitterAddress.System);
		FNiagaraExternalEditContext& Context = ContextHolder.Get();

		FNiagaraExt_ParameterDefault Entry;
		Entry.Variable.Name = Default.Variable.GetName();
		Entry.Variable.Type = Default.Variable.GetType();
		Entry.Mode = ToNiagaraDefaultMode(Default.Mode);
		Entry.Binding = Default.Binding;
		if (Default.Mode == FParameterDefault::EMode::Value && Default.Value.IsSet()
			&& !ToVariableValue(Default.Value, Default.Variable.GetType(), Entry.Value, OutErrors))
		{
			return false;
		}

		UNiagaraExternalEditUtilities::SetEmitterParameterDefault(ToReference(EmitterAddress), Entry, Context);
		return Drain(Context, OutErrors);
#endif
	}

	bool FNiagaraAdapter::SetParameterDefaults(const FStackAddress& EmitterAddress,
		TArrayView<const FParameterDefault> Defaults, TArray<FString>& OutErrors)
	{
		if (Defaults.IsEmpty())
		{
			return true;
		}

#if !DREAMFX_HAS_NIAGARA_FAST_EDIT
		// See SetParameterDefault: no stock way to write a graph parameter's default mode.
		return true;
#else

		FOpTimer OpTimer(TEXT("SetParameterDefaults"));

		// One epoch and one context for the whole batch. Each write is still structural, but none of
		// these read the stack -- they address the emitter's graph, not an item in it -- so a view
		// model left stale between two of them describes nothing either of them looks at. The caller
		// writes one of these per parameter its links touched, which on a 24-emitter system is several
		// hundred, and paying a context rebuild for each was most of the cost of having them at all.
		FEpochGuard Epoch(EmitterAddress.System);
		FEditContext ContextHolder(EmitterAddress.System);
		FNiagaraExternalEditContext& Context = ContextHolder.Get();
		const FNiagaraExt_StackItemReference Reference = ToReference(EmitterAddress);

		bool bOk = true;
		for (const FParameterDefault& Default : Defaults)
		{
			FNiagaraExt_ParameterDefault Entry;
			Entry.Variable.Name = Default.Variable.GetName();
			Entry.Variable.Type = Default.Variable.GetType();
			Entry.Mode = ToNiagaraDefaultMode(Default.Mode);
			Entry.Binding = Default.Binding;
			if (Default.Mode == FParameterDefault::EMode::Value && Default.Value.IsSet()
				&& !ToVariableValue(Default.Value, Default.Variable.GetType(), Entry.Value, OutErrors))
			{
				bOk = false;
				continue;
			}

			UNiagaraExternalEditUtilities::SetEmitterParameterDefault(Reference, Entry, Context);
		}

		return Drain(Context, OutErrors) && bOk;
#endif
	}

	bool FNiagaraAdapter::CleanUpStaleParameters(const FStackAddress& EmitterAddress, TArray<FString>& OutErrors)
	{
		FOpTimer OpTimer(TEXT("CleanUpStaleParameters"));

#if !DREAMFX_HAS_NIAGARA_FAST_EDIT
		// Stock engine: hygiene only. What survives is a rapid-iteration parameter for a module the
		// rebuilt stack no longer has -- it costs a little memory and shows up in the editor's
		// parameter list, and it does not change what the system simulates.
		return true;
#else
		// Structural: it removes parameters from the scripts the stack view models describe, so a
		// context built before it no longer matches what is there.
		FEpochGuard Epoch(EmitterAddress.System);
		FEditContext ContextHolder(EmitterAddress.System);
		FNiagaraExternalEditContext& Context = ContextHolder.Get();
		UNiagaraExternalEditUtilities::CleanUpStaleEmitterParameters(ToReference(EmitterAddress), Context);
		return Drain(Context, OutErrors);
#endif
	}

	bool FNiagaraAdapter::GetEmitterInfo(const FStackAddress& EmitterAddress, FEmitterInfo& OutInfo, TArray<FString>& OutErrors)
	{
		FOpTimer OpTimer(TEXT("read: GetEmitterInfo"));
		FEditContext ContextHolder(EmitterAddress.System);
		FNiagaraExternalEditContext& Context = ContextHolder.Get();
		FNiagaraExt_EmitterTopology Topology;
		UNiagaraExternalEditUtilities::GetEmitterTopology(ToReference(EmitterAddress), Topology, Context);

		OutInfo.Name = Topology.EmitterName;
		OutInfo.bEnabled = Topology.bEnabled;

		const FNiagaraExt_ScriptStackTopology* Stacks[] =
		{
			&Topology.EmitterSpawnScript, &Topology.EmitterUpdateScript,
			&Topology.ParticleSpawnScript, &Topology.ParticleUpdateScript,
		};

		for (const FNiagaraExt_ScriptStackTopology* Stack : Stacks)
		{
			FScriptStackInfo StackInfo;
			StackInfo.ScriptName = Stack->ScriptName;
			for (const FNiagaraExt_ModuleTopology& Module : Stack->Modules)
			{
				FModuleInfo ModuleInfo;
				ModuleInfo.ModuleName = Module.ModuleName;
				ModuleInfo.bEnabled = Module.Enabled;
				ModuleInfo.Script = Module.ModuleScript;
				ModuleInfo.bIsSetParameters = Module.bIsSetParametersModule;
				for (const FNiagaraExt_StackInputTopology& Input : Module.Inputs)
				{
					FInputInfo InputInfo;
					InputInfo.Name = Input.Name;
					InputInfo.Type = Input.Type;
					InputInfo.bVisible = Input.bIsVisible;
					InputInfo.bEditable = Input.bIsEditable;
					InputInfo.bDynamic = Input.bIsDynamic;
					InputInfo.bStaticSwitch = Input.bIsStaticSwitch;
					ModuleInfo.Inputs.Add(MoveTemp(InputInfo));
				}
				StackInfo.Modules.Add(MoveTemp(ModuleInfo));
			}
			OutInfo.Stacks.Add(MoveTemp(StackInfo));
		}

		for (const FNiagaraExt_RendererRef& Renderer : Topology.Renderers)
		{
			FRendererInfo RendererInfo;
			RendererInfo.Index = Renderer.RendererIndex;
			RendererInfo.Class = Renderer.RendererClass.Get();
			OutInfo.Renderers.Add(RendererInfo);
		}

		return Drain(Context, OutErrors);
	}

	bool FNiagaraAdapter::GetScriptStackInfo(const FStackAddress& ScriptAddress, FScriptStackInfo& OutInfo, TArray<FString>& OutErrors)
	{
		FOpTimer OpTimer(TEXT("read: GetScriptStackInfo"));
		FEditContext ContextHolder(ScriptAddress.System);
		FNiagaraExternalEditContext& Context = ContextHolder.Get();
		FNiagaraExt_ScriptStackTopology Topology;
		UNiagaraExternalEditUtilities::GetScriptStackTopology(ToReference(ScriptAddress), Topology, Context);

		OutInfo.ScriptName = Topology.ScriptName;
		for (const FNiagaraExt_ModuleTopology& Module : Topology.Modules)
		{
			FModuleInfo ModuleInfo;
			ModuleInfo.ModuleName = Module.ModuleName;
			ModuleInfo.bEnabled = Module.Enabled;
			ModuleInfo.Script = Module.ModuleScript;
			ModuleInfo.bIsSetParameters = Module.bIsSetParametersModule;
			for (const FNiagaraExt_StackInputTopology& Input : Module.Inputs)
			{
				FInputInfo InputInfo;
				InputInfo.Name = Input.Name;
				InputInfo.Type = Input.Type;
				InputInfo.bVisible = Input.bIsVisible;
				InputInfo.bEditable = Input.bIsEditable;
				InputInfo.bDynamic = Input.bIsDynamic;
				InputInfo.bStaticSwitch = Input.bIsStaticSwitch;
				ModuleInfo.Inputs.Add(MoveTemp(InputInfo));
			}
			OutInfo.Modules.Add(MoveTemp(ModuleInfo));
		}

		return Drain(Context, OutErrors);
	}

	bool FNiagaraAdapter::GetModuleInfo(const FStackAddress& ModuleAddress, FModuleInfo& OutInfo, TArray<FString>& OutErrors)
	{
		FOpTimer OpTimer(TEXT("read: GetModuleInfo"));
		FEditContext ContextHolder(ModuleAddress.System);
		FNiagaraExternalEditContext& Context = ContextHolder.Get();
		FNiagaraExt_ModuleTopology Topology;
		UNiagaraExternalEditUtilities::GetModuleTopology(ToReference(ModuleAddress), Topology, Context);

		OutInfo.ModuleName = Topology.ModuleName;
		OutInfo.bEnabled = Topology.Enabled;
		OutInfo.Script = Topology.ModuleScript;
		OutInfo.bIsSetParameters = Topology.bIsSetParametersModule;
		for (const FNiagaraExt_StackInputTopology& Input : Topology.Inputs)
		{
			FInputInfo InputInfo;
			InputInfo.Name = Input.Name;
			InputInfo.Type = Input.Type;
			InputInfo.bVisible = Input.bIsVisible;
			InputInfo.bEditable = Input.bIsEditable;
			InputInfo.bDynamic = Input.bIsDynamic;
			InputInfo.bStaticSwitch = Input.bIsStaticSwitch;
			OutInfo.Inputs.Add(MoveTemp(InputInfo));
		}

		return Drain(Context, OutErrors);
	}

	bool FNiagaraAdapter::GetUserVariables(UNiagaraSystem* System, TArray<FUserVariableInfo>& OutVariables, TArray<FString>& OutErrors)
	{
		if (System == nullptr)
		{
			OutErrors.Add(TEXT("Cannot read user variables from a null system."));
			return false;
		}

		FEditContext ContextHolder(System);
		FNiagaraExternalEditContext& Context = ContextHolder.Get();
		FNiagaraExt_UserVariables Variables;
		UNiagaraExternalEditUtilities::GetUserVariables(System, Variables, Context);

		for (const FNiagaraExt_UserVariable& Variable : Variables.UserVariables)
		{
			FUserVariableInfo Info;
			// Names come back namespace-qualified ("User.SparkCount") even though the write took a
			// bare name. Callers must compare on the qualified form.
			Info.Name = Variable.Name;
			Info.Type = Variable.Type;
			Info.Description = Variable.Description.ToString();
			FromVariableValue(Variable.DefaultValue, Info.DefaultValue);
			OutVariables.Add(MoveTemp(Info));
		}

		return Drain(Context, OutErrors);
	}

	// -------------------------------------------------------------------------------------------
	// Structural writes
	// -------------------------------------------------------------------------------------------

	bool FNiagaraAdapter::AddUserVariable(UNiagaraSystem* System, FName Name, const FNiagaraTypeDefinition& Type,
		const FString& Description, const FInputValue& DefaultValue, TArray<FString>& OutErrors)
	{
		if (System == nullptr)
		{
			OutErrors.Add(TEXT("Cannot add a user variable to a null system."));
			return false;
		}

		FEpochGuard Epoch(System);
		FEditContext ContextHolder(System);
		FNiagaraExternalEditContext& Context = ContextHolder.Get();

		FNiagaraExt_UserVariable Variable;
		Variable.Name = Name;
		Variable.Type = Type;
		Variable.Description = FText::FromString(Description);

		if (DefaultValue.IsSet() && !ToVariableValue(DefaultValue, Type, Variable.DefaultValue, OutErrors))
		{
			return false;
		}

		UNiagaraExternalEditUtilities::AddUserVariable(System, Variable, Context);
		return Drain(Context, OutErrors);
	}

	bool FNiagaraAdapter::RemoveUserVariable(UNiagaraSystem* System, FName Name, const FNiagaraTypeDefinition& Type,
		TArray<FString>& OutErrors)
	{
		if (System == nullptr)
		{
			OutErrors.Add(TEXT("Cannot remove a user variable from a null system."));
			return false;
		}

		FEpochGuard Epoch(System);
		FEditContext ContextHolder(System);
		FNiagaraExternalEditContext& Context = ContextHolder.Get();
		FNiagaraExt_Variable Variable;
		Variable.Name = Name;
		Variable.Type = Type;
		UNiagaraExternalEditUtilities::RemoveUserVariable(System, Variable, Context);
		return Drain(Context, OutErrors);
	}

	bool FNiagaraAdapter::AddEmitter(UNiagaraSystem* System, FName EmitterName, TArray<FString>& OutErrors)
	{
		if (System == nullptr)
		{
			OutErrors.Add(TEXT("Cannot add an emitter to a null system."));
			return false;
		}

		// AddEmitter rejects a null template ("template emitter is null"), so there is no
		// create-from-nothing path. InitializeEmitter is the engine's own answer: it builds the four
		// script stacks. bAddDefaultModulesAndRenderers stays false so the DSL remains the sole
		// description of what the emitter contains.
		UNiagaraEmitter* Template = NewObject<UNiagaraEmitter>(
			GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UNiagaraEmitter::StaticClass(),
				TEXT("DreamFXTemplateEmitter")), RF_Transactional);
		UNiagaraEmitterFactoryNew::InitializeEmitter(Template, /*bAddDefaultModulesAndRenderers=*/false);

		return AddEmitterFromTemplate(System, Template, EmitterName, OutErrors);
	}

	bool FNiagaraAdapter::AddEmitterFromTemplate(UNiagaraSystem* System, UNiagaraEmitter* Template,
		FName EmitterName, TArray<FString>& OutErrors)
	{
		FOpTimer OpTimer(TEXT("AddEmitter"));
		if (System == nullptr)
		{
			OutErrors.Add(TEXT("Cannot add an emitter to a null system."));
			return false;
		}
		if (Template == nullptr)
		{
			OutErrors.Add(TEXT("Cannot add an emitter from a null template."));
			return false;
		}

		// UNiagaraExternalEditUtilities::AddEmitter ends with SystemViewModel->RefreshAll().
		FEpochGuard Epoch(System, EStructuralKind::RefreshedInPlace);
		FEditContext ContextHolder(System);
		FNiagaraExternalEditContext& Context = ContextHolder.Get();
		FNiagaraExt_EmitterTopology Topology;
		UNiagaraExternalEditUtilities::AddEmitter(Template, EmitterName, Topology, Context);

		if (!Drain(Context, OutErrors))
		{
			return false;
		}

		if (Topology.EmitterName != EmitterName)
		{
			// Niagara uniquifies clashing names. Silently accepting a renamed emitter would break R4's
			// stable-key rule and orphan every rapid iteration parameter, so this is fatal.
			OutErrors.Add(FString::Printf(
				TEXT("Emitter '%s' was renamed to '%s' by the system (name collision). Emitter names are stable keys and must be unique."),
				*EmitterName.ToString(), *Topology.EmitterName.ToString()));
			return false;
		}
		return true;
	}

	bool FNiagaraAdapter::RemoveEmitter(const FStackAddress& EmitterAddress, TArray<FString>& OutErrors)
	{
		FEpochGuard Epoch(EmitterAddress.System);
		FEditContext ContextHolder(EmitterAddress.System);
		FNiagaraExternalEditContext& Context = ContextHolder.Get();
		UNiagaraExternalEditUtilities::RemoveEmitter(ToReference(EmitterAddress), Context);
		return Drain(Context, OutErrors);
	}

	bool FNiagaraAdapter::RenameEmitter(UNiagaraSystem* System, FName OldName, FName NewName, TArray<FString>& OutErrors)
	{
		if (System == nullptr)
		{
			OutErrors.Add(TEXT("Cannot rename an emitter on a null system."));
			return false;
		}

		TArray<FString> Existing;
		FNiagaraEmitterHandle* Target = nullptr;
		for (FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			Existing.Add(Handle.GetName().ToString());
			if (Handle.GetName() == OldName)
			{
				Target = &Handle;
			}
			else if (Handle.GetName() == NewName)
			{
				OutErrors.Add(FString::Printf(
					TEXT("'%s' already has an emitter named '%s'. Emitter names are stable keys and must be unique."),
					*System->GetName(), *NewName.ToString()));
				return false;
			}
		}

		if (Target == nullptr)
		{
			OutErrors.Add(FString::Printf(TEXT("'%s' has no emitter named '%s'. It has: %s"),
				*System->GetName(), *OldName.ToString(), *FString::Join(Existing, TEXT(", "))));
			return false;
		}

		// An emitter name is the key every stack address is written in terms of, so a rename ages every
		// cached entry even though nothing here goes through an edit context.
		FEpochGuard Epoch(System);

		System->Modify();
		Target->SetName(NewName, *System);

		if (Target->GetName() != NewName)
		{
			OutErrors.Add(FString::Printf(TEXT("Rename produced '%s' instead of '%s'."),
				*Target->GetName().ToString(), *NewName.ToString()));
			return false;
		}
		return true;
	}

	bool FNiagaraAdapter::AddModule(const FStackAddress& StackAddress, UNiagaraScript* ModuleAsset,
		FName& OutModuleName, TArray<FString>& OutErrors, bool bDeferStackRefresh)
	{
		FOpTimer OpTimer(TEXT("AddModule"));
		if (ModuleAsset == nullptr)
		{
			OutErrors.Add(TEXT("Cannot add a null module asset."));
			return false;
		}

		// UNiagaraExternalEditUtilities::AddModule calls ScriptItem->RefreshChildren() -- unless the
		// caller deferred that to a batch-closing RefreshScriptStack.
		FEpochGuard Epoch(StackAddress.System, EStructuralKind::RefreshedInPlace);
		FEditContext ContextHolder(StackAddress.System);
		FNiagaraExternalEditContext& Context = ContextHolder.Get();
		FNiagaraExt_ModuleTopology Topology;

		// Only the name is read below, and filling the rest walks every input on the module to build
		// a topology nobody looks at -- 18 of this call's 62 ms. The generator learns a module's
		// inputs from the schema, not from what it just added.
#if DREAMFX_HAS_NIAGARA_FAST_EDIT
		UNiagaraExternalEditUtilities::AddModule(ToReference(StackAddress), ModuleAsset, Topology, Context,
			UNiagaraExternalEditUtilities::EModuleTopologyDetail::HeaderOnly, bDeferStackRefresh);
#else
		// Stock engine: neither knob exists. The topology comes back fully populated and the stack
		// refreshes on every add, so a stack of n modules pays n refreshes instead of one.
		UNiagaraExternalEditUtilities::AddModule(ToReference(StackAddress), ModuleAsset, Topology, Context);
#endif

		OutModuleName = Topology.ModuleName;
		if (!Drain(Context, OutErrors))
		{
			return false;
		}
		if (OutModuleName == NAME_None)
		{
			OutErrors.Add(FString::Printf(TEXT("AddModule returned no module name for '%s'."), *ModuleAsset->GetName()));
			return false;
		}
		return true;
	}

	bool FNiagaraAdapter::RefreshScriptStack(const FStackAddress& StackAddress, TArray<FString>& OutErrors)
	{
		FOpTimer OpTimer(TEXT("RefreshScriptStack"));

#if !DREAMFX_HAS_NIAGARA_FAST_EDIT
		// Stock engine: nothing to settle. This call exists to pay, once, the refresh that a batch of
		// deferred adds skipped -- and without bDeferStackRefresh every add already paid its own.
		return true;
#else
		// The refresh IS the in-place refresh a batch of deferred adds owes: after it the shared
		// context describes the stack again, so the epoch survives.
		FEpochGuard Epoch(StackAddress.System, EStructuralKind::RefreshedInPlace);
		FEditContext ContextHolder(StackAddress.System);
		FNiagaraExternalEditContext& Context = ContextHolder.Get();
		UNiagaraExternalEditUtilities::RefreshScriptStack(ToReference(StackAddress), Context);
		return Drain(Context, OutErrors);
#endif
	}

	bool FNiagaraAdapter::RemoveModule(const FStackAddress& ModuleAddress, TArray<FString>& OutErrors)
	{
		FOpTimer OpTimer(TEXT("RemoveModule"));

		// UNiagaraExternalEditUtilities::RemoveModule calls RefreshChildren() on the script it removed from.
		FEpochGuard Epoch(ModuleAddress.System, EStructuralKind::RefreshedInPlace);
		FEditContext ContextHolder(ModuleAddress.System);
		FNiagaraExternalEditContext& Context = ContextHolder.Get();
		UNiagaraExternalEditUtilities::RemoveModule(ToReference(ModuleAddress), Context);
		return Drain(Context, OutErrors);
	}

	bool FNiagaraAdapter::SetModuleEnabled(const FStackAddress& ModuleAddress, bool bEnabled, TArray<FString>& OutErrors)
	{
		FOpTimer OpTimer(TEXT("SetModuleEnabled"));
		FEpochGuard Epoch(ModuleAddress.System);
		FEditContext ContextHolder(ModuleAddress.System);
		FNiagaraExternalEditContext& Context = ContextHolder.Get();
		UNiagaraExternalEditUtilities::SetModuleEnabled(ToReference(ModuleAddress), bEnabled, Context);
		return Drain(Context, OutErrors);
	}

	bool FNiagaraAdapter::AddSetParametersModule(const FStackAddress& StackAddress,
		const TArray<TTuple<FName, FNiagaraTypeDefinition, FInputValue>>& Entries,
		FName& OutModuleName, TArray<FString>& OutErrors, bool bDeferStackRefresh)
	{
		FOpTimer OpTimer(TEXT("AddSetParametersModule"));
		TArray<FNiagaraExt_SetParameterEntry> Parameters;
		Parameters.Reserve(Entries.Num());

		for (const TTuple<FName, FNiagaraTypeDefinition, FInputValue>& Entry : Entries)
		{
			FNiagaraExt_SetParameterEntry Parameter;
			Parameter.Variable.Name = Entry.Get<0>();
			Parameter.Variable.Type = Entry.Get<1>();

			const FInputValue& Value = Entry.Get<2>();
			// Non-literal entry values (dynamic input, HLSL, linked) cannot ride along on the create
			// call -- FNiagaraExt_SetParameterEntry only carries a plain variable value. The module is
			// created with a placeholder and the real value is written afterwards through SetInput,
			// which addresses the entry as an input on the Set Parameters module.
			if (Value.Mode == EInputValueMode::Literal || Value.Mode == EInputValueMode::Enum)
			{
				if (!ToVariableValue(Value, Entry.Get<1>(), Parameter.DefaultValue, OutErrors))
				{
					return false;
				}
			}

			Parameters.Add(MoveTemp(Parameter));
		}

		// UNiagaraExternalEditUtilities::AddSetParametersModule calls ScriptItem->RefreshChildren()
		// -- unless the caller deferred that to a batch-closing RefreshScriptStack.
		FEpochGuard Epoch(StackAddress.System, EStructuralKind::RefreshedInPlace);
		FEditContext ContextHolder(StackAddress.System);
		FNiagaraExternalEditContext& Context = ContextHolder.Get();
		FNiagaraExt_ModuleTopology Topology;

		// Same as AddModule above: the name is all that is read.
#if DREAMFX_HAS_NIAGARA_FAST_EDIT
		UNiagaraExternalEditUtilities::AddSetParametersModule(ToReference(StackAddress), Parameters, Topology, Context,
			UNiagaraExternalEditUtilities::EModuleTopologyDetail::HeaderOnly, bDeferStackRefresh);
#else
		UNiagaraExternalEditUtilities::AddSetParametersModule(ToReference(StackAddress), Parameters, Topology, Context);
#endif

		OutModuleName = Topology.ModuleName;
		if (!Drain(Context, OutErrors))
		{
			return false;
		}
		if (OutModuleName == NAME_None)
		{
			OutErrors.Add(TEXT("AddSetParametersModule returned no module name."));
			return false;
		}
		return true;
	}

	bool FNiagaraAdapter::AddRenderer(const FStackAddress& EmitterAddress, UClass* RendererClass,
		int32& OutRendererIndex, TArray<FString>& OutErrors)
	{
		FOpTimer OpTimer(TEXT("AddRenderer"));
		if (RendererClass == nullptr)
		{
			OutErrors.Add(TEXT("Cannot add a renderer with no class."));
			return false;
		}

		FEpochGuard Epoch(EmitterAddress.System);
		FEditContext ContextHolder(EmitterAddress.System);
		FNiagaraExternalEditContext& Context = ContextHolder.Get();
		FNiagaraExt_RendererRef Ref;
		UNiagaraExternalEditUtilities::AddRenderer(ToReference(EmitterAddress), RendererClass, Ref, Context);

		OutRendererIndex = Ref.RendererIndex;
		if (!Drain(Context, OutErrors))
		{
			return false;
		}
		if (OutRendererIndex == INDEX_NONE)
		{
			OutErrors.Add(FString::Printf(TEXT("AddRenderer returned no index for '%s'."), *RendererClass->GetName()));
			return false;
		}
		return true;
	}

	bool FNiagaraAdapter::RemoveRenderer(const FStackAddress& RendererAddress, TArray<FString>& OutErrors)
	{
		FEpochGuard Epoch(RendererAddress.System);
		FEditContext ContextHolder(RendererAddress.System);
		FNiagaraExternalEditContext& Context = ContextHolder.Get();
		UNiagaraExternalEditUtilities::RemoveRenderer(ToReference(RendererAddress), Context);
		return Drain(Context, OutErrors);
	}

	// -------------------------------------------------------------------------------------------
	// Value writes
	// -------------------------------------------------------------------------------------------

	bool FNiagaraAdapter::SetInput(const FStackAddress& InputAddress, const FInputValue& Value, TArray<FString>& OutErrors)
	{
		FOpTimer OpTimer(TEXT("SetInput"));
		FNiagaraExt_StackInputValue StackValue;
		if (!ToStackInputValue(Value, StackValue, OutErrors))
		{
			return false;
		}

		// The classification the write scope turns on. A dynamic input or a data interface is written
		// by creating a node, which gives the input children a later write may address, so it ends the
		// epoch. A literal, enum, link or HLSL expression lands on an entry that already exists and
		// leaves the shape of the stack alone -- and those are the overwhelming majority, which is
		// what makes the sharing worth having.
		//
		// A static switch is the case this cannot see: it changes which *other* inputs are visible,
		// and only the caller knows from the module schema that an input is one. The generator calls
		// EndStructuralEpoch for it.
		//
		// Inline edit conditions are the case nobody can see from the schema -- `UseLoopDelay` on
		// EmitterState is a plain NiagaraBool with no flag of any kind, yet writing it is what reveals
		// `DelayFirstLoopOnly` below it, and sharing across that write got the next one refused as
		// "hidden by static-switch / conditional logic". Ending the epoch after every bool write does
		// fix that, and was tried: it also collapsed the sharing this scope exists for, because most
		// bools gate nothing. Measured on NS_LevelUp_Descend_Root, 2789 input writes still built 2188
		// contexts -- a 22% share rate, for a mechanism whose whole point is not rebuilding.
		//
		// The generator's retry pass covers it instead, and covers it better: a write refused for any
		// reason is retried once the module's other inputs have landed, against a deliberately fresh
		// context. So the common case shares, and the rare gated case pays for one rebuild at the end
		// rather than every bool paying up front.
		// The two structural modes are not equally structural, which is why they are split here rather
		// than tested with one ||:
		//
		//   * a dynamic input is written by UNiagaraStackFunctionInput::SetDynamicInput, which ends in
		//     RefreshChildren() -- the sub-inputs a later write addresses exist by the time this
		//     returns, so the context is still accurate;
		//   * a data interface is written by SetDataInterfaceValueExternal, which edits a *placeholder*
		//     owned by the system view model's FNiagaraPlaceholderDataInterfaceManager. That manager
		//     caches one placeholder per (emitter, function call, input) and syncs it to the override
		//     pin from its OnChanged handler. Keeping it alive across a whole build was tried, and
		//     produced a system whose data channel reads compiled to a single namespace entry.
		const bool bStructural = Value.Mode == EInputValueMode::DynamicInput
			|| Value.Mode == EInputValueMode::DataInterface;

		bool bResult = false;
		{
			FEditContext ContextHolder(InputAddress.System);
			FNiagaraExternalEditContext& Context = ContextHolder.Get();
			UNiagaraExternalEditUtilities::SetStackInputData(ToReference(InputAddress), StackValue, Context);
			bResult = Drain(Context, OutErrors);
		}

		// After the holder is gone, so the context is not destroyed while it still points at one.
		if (bStructural)
		{
			EndEpoch(InputAddress.System, Value.Mode == EInputValueMode::DynamicInput
				? EStructuralKind::RefreshedInPlace
				: EStructuralKind::Unrefreshed);
		}
		else
		{
			++GStats.ValueCalls;
		}
		return bResult;
	}

	bool FNiagaraAdapter::SetRendererProperties(const FStackAddress& RendererAddress, const FString& PropertiesJson,
		TArray<FString>& OutErrors)
	{
		FOpTimer OpTimer(TEXT("SetRendererProperties"));
		if (PropertiesJson.IsEmpty())
		{
			return true;
		}

		// A renderer is addressed by index and its properties add no stack entries, so this is a value
		// write: the epoch survives it.
		++GStats.ValueCalls;
		FEditContext ContextHolder(RendererAddress.System);
		FNiagaraExternalEditContext& Context = ContextHolder.Get();
		FNiagaraExt_RendererData Data;
		Data.PropertyValues = NormalizeObjectReferences(PropertiesJson);
		UNiagaraExternalEditUtilities::SetRendererData(ToReference(RendererAddress), Data, Context);
		return Drain(Context, OutErrors);
	}

	namespace
	{
		/** The renderer at RendererIndex on the addressed emitter, or null. */
		UNiagaraRendererProperties* ResolveRenderer(const FStackAddress& Address, FNiagaraEmitterHandle*& OutHandle)
		{
			OutHandle = nullptr;
			if (Address.System == nullptr)
			{
				return nullptr;
			}

			for (FNiagaraEmitterHandle& Handle : Address.System->GetEmitterHandles())
			{
				if (Handle.GetName() != Address.EmitterName)
				{
					continue;
				}
				OutHandle = &Handle;
				FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
				if (Data == nullptr)
				{
					return nullptr;
				}
				const TArray<UNiagaraRendererProperties*>& Renderers = Data->GetRenderers();
				return Renderers.IsValidIndex(Address.RendererIndex) ? Renderers[Address.RendererIndex] : nullptr;
			}
			return nullptr;
		}

		/** Finds the FNiagaraVariableAttributeBinding property a DSL binding name refers to. */
		FStructProperty* FindBindingProperty(const UClass* RendererClass, const FString& BindingName)
		{
			const FString WithSuffix = BindingName.EndsWith(TEXT("Binding"), ESearchCase::IgnoreCase)
				? BindingName
				: BindingName + TEXT("Binding");

			for (TFieldIterator<FStructProperty> It(RendererClass); It; ++It)
			{
				if (It->Struct != FNiagaraVariableAttributeBinding::StaticStruct())
				{
					continue;
				}
				const FString PropertyName = It->GetName();
				if (PropertyName.Equals(WithSuffix, ESearchCase::IgnoreCase)
					|| PropertyName.Equals(BindingName, ESearchCase::IgnoreCase))
				{
					return *It;
				}
			}
			return nullptr;
		}
	}

	bool FNiagaraAdapter::EnsureRendererMaterial(const FStackAddress& RendererAddress,
		FString& OutAppliedMaterial, bool& bOutStillMissing, TArray<FString>& OutErrors)
	{
		OutAppliedMaterial.Reset();
		bOutStillMissing = false;

		FNiagaraEmitterHandle* Handle = nullptr;
		UNiagaraRendererProperties* Renderer = ResolveRenderer(RendererAddress, Handle);
		if (Renderer == nullptr)
		{
			OutErrors.Add(FString::Printf(TEXT("Could not resolve renderer %d on emitter '%s'."),
				RendererAddress.RendererIndex, *RendererAddress.EmitterName.ToString()));
			return false;
		}

		FObjectProperty* MaterialProperty = FindFProperty<FObjectProperty>(Renderer->GetClass(), TEXT("Material"));
		if (MaterialProperty == nullptr)
		{
			// Mesh renderers take their materials from the mesh; light and component renderers have
			// none at all. Nothing to do, and nothing wrong.
			return true;
		}

		if (MaterialProperty->GetObjectPropertyValue_InContainer(Renderer) != nullptr)
		{
			return true;
		}

		// Mirrors the paths the engine itself assigns. Kept as a table rather than derived, because
		// the engine derives nothing either -- these are literals in NiagaraSystemViewModel.
		struct FDefaultMaterial
		{
			const TCHAR* ClassName;
			const TCHAR* AssetPath;
		};
		static const FDefaultMaterial Defaults[] =
		{
			{ TEXT("NiagaraSpriteRendererProperties"), TEXT("/Niagara/DefaultAssets/DefaultSpriteMaterial.DefaultSpriteMaterial") },
			{ TEXT("NiagaraRibbonRendererProperties"), TEXT("/Niagara/DefaultAssets/DefaultRIbbonMaterial.DefaultRIbbonMaterial") },
			{ TEXT("NiagaraDecalRendererProperties"),  TEXT("/Niagara/DefaultAssets/DefaultDecalMaterial.DefaultDecalMaterial") },
		};

		const FString ClassName = Renderer->GetClass()->GetName();
		for (const FDefaultMaterial& Default : Defaults)
		{
			if (ClassName != Default.ClassName)
			{
				continue;
			}

			UObject* Material = LoadObject<UObject>(nullptr, Default.AssetPath);
			if (Material == nullptr)
			{
				OutErrors.Add(FString::Printf(TEXT("Could not load the default material '%s'."), Default.AssetPath));
				bOutStillMissing = true;
				return false;
			}

			Renderer->Modify();
			MaterialProperty->SetObjectPropertyValue_InContainer(Renderer, Material);

			FPropertyChangedEvent PropertyChangedEvent(MaterialProperty, EPropertyChangeType::ValueSet);
			Renderer->PostEditChangeProperty(PropertyChangedEvent);

			OutAppliedMaterial = Default.AssetPath;
			return true;
		}

		bOutStillMissing = true;
		return true;
	}

	void FNiagaraAdapter::ListRendererBindings(const UClass* RendererClass, TArray<FString>& OutNames)
	{
		if (RendererClass == nullptr)
		{
			return;
		}
		for (TFieldIterator<FStructProperty> It(RendererClass); It; ++It)
		{
			if (It->Struct != FNiagaraVariableAttributeBinding::StaticStruct())
			{
				continue;
			}
			FString Name = It->GetName();
			Name.RemoveFromEnd(TEXT("Binding"), ESearchCase::CaseSensitive);
			OutNames.Add(Name);
		}
		OutNames.Sort();
	}

	bool FNiagaraAdapter::GetRendererBindings(const FStackAddress& RendererAddress,
		TArray<TPair<FString, FName>>& OutBindings, TArray<FString>& OutErrors)
	{
		FNiagaraEmitterHandle* Handle = nullptr;
		const UNiagaraRendererProperties* Renderer = ResolveRenderer(RendererAddress, Handle);
		if (Renderer == nullptr)
		{
			OutErrors.Add(FString::Printf(TEXT("Could not resolve renderer %d on emitter '%s'."),
				RendererAddress.RendererIndex, *RendererAddress.EmitterName.ToString()));
			return false;
		}

		for (TFieldIterator<FStructProperty> It(Renderer->GetClass()); It; ++It)
		{
			if (It->Struct != FNiagaraVariableAttributeBinding::StaticStruct())
			{
				continue;
			}

			const FNiagaraVariableAttributeBinding* Binding =
				It->ContainerPtrToValuePtr<FNiagaraVariableAttributeBinding>(Renderer);

			FString Name = It->GetName();
			Name.RemoveFromEnd(TEXT("Binding"), ESearchCase::CaseSensitive);
			OutBindings.Emplace(Name, Binding->GetParamMapBindableVariable().GetName());
		}

		return true;
	}

	bool FNiagaraAdapter::SetRendererBinding(const FStackAddress& RendererAddress, const FString& BindingName,
		FName TargetParameter, TArray<FString>& OutErrors)
	{
		FNiagaraEmitterHandle* Handle = nullptr;
		UNiagaraRendererProperties* Renderer = ResolveRenderer(RendererAddress, Handle);
		if (Renderer == nullptr || Handle == nullptr)
		{
			OutErrors.Add(FString::Printf(
				TEXT("Could not resolve renderer %d on emitter '%s'."),
				RendererAddress.RendererIndex, *RendererAddress.EmitterName.ToString()));
			return false;
		}

		FStructProperty* BindingProperty = FindBindingProperty(Renderer->GetClass(), BindingName);
		if (BindingProperty == nullptr)
		{
			TArray<FString> Available;
			ListRendererBindings(Renderer->GetClass(), Available);
			OutErrors.Add(FString::Printf(
				TEXT("%s has no attribute binding named '%s'. Available bindings: %s"),
				*Renderer->GetClass()->GetName(), *BindingName,
				Available.Num() > 0 ? *FString::Join(Available, TEXT(", ")) : TEXT("(none)")));
			return false;
		}

		Renderer->Modify();

		FNiagaraVariableAttributeBinding* Binding =
			BindingProperty->ContainerPtrToValuePtr<FNiagaraVariableAttributeBinding>(Renderer);
		Binding->SetValue(TargetParameter, Handle->GetInstance(), Renderer->GetCurrentSourceMode());

		// The renderer caches derived state off its bindings; without this the change is invisible to
		// the compiled system until something else happens to trigger a refresh.
		FPropertyChangedEvent PropertyChangedEvent(BindingProperty, EPropertyChangeType::ValueSet);
		Renderer->PostEditChangeProperty(PropertyChangedEvent);
		return true;
	}

	bool FNiagaraAdapter::SetEmitterProperties(const FStackAddress& EmitterAddress, const FString& PropertiesJson,
		TArray<FString>& OutErrors)
	{
		FOpTimer OpTimer(TEXT("SetEmitterProperties"));
		if (PropertiesJson.IsEmpty())
		{
			return true;
		}

		// Structural: SimTarget lives in here, and moving an emitter between CPU and GPU changes which
		// script stacks it has at all.
		FEpochGuard Epoch(EmitterAddress.System);
		FEditContext ContextHolder(EmitterAddress.System);
		FNiagaraExternalEditContext& Context = ContextHolder.Get();
		FNiagaraExt_EmitterData Data;
		Data.PropertyValues = NormalizeObjectReferences(PropertiesJson);
		UNiagaraExternalEditUtilities::SetEmitterData(ToReference(EmitterAddress), Data, Context);
		return Drain(Context, OutErrors);
	}

	bool FNiagaraAdapter::SetSystemProperties(UNiagaraSystem* System, const FString& PropertiesJson, TArray<FString>& OutErrors)
	{
		if (System == nullptr || PropertiesJson.IsEmpty())
		{
			return System != nullptr;
		}

		FEpochGuard Epoch(System);
		FEditContext ContextHolder(System);
		FNiagaraExternalEditContext& Context = ContextHolder.Get();
		FNiagaraExt_SystemData Data;
		Data.PropertyValues = NormalizeObjectReferences(PropertiesJson);
		UNiagaraExternalEditUtilities::SetSystemData(System, Data, Context);
		return Drain(Context, OutErrors);
	}

	// -------------------------------------------------------------------------------------------
	// Value reads
	// -------------------------------------------------------------------------------------------

	bool FNiagaraAdapter::GetInput(const FStackAddress& InputAddress, FInputValue& OutValue, TArray<FString>& OutErrors)
	{
		FEditContext ContextHolder(InputAddress.System);
		FNiagaraExternalEditContext& Context = ContextHolder.Get();
		FNiagaraExt_StackInputValue StackValue;
		UNiagaraExternalEditUtilities::GetStackInputData(ToReference(InputAddress), StackValue, Context);
		FromStackInputValue(StackValue, OutValue);
		return Drain(Context, OutErrors);
	}

	bool FNiagaraAdapter::GetRendererProperties(const FStackAddress& RendererAddress, FString& OutJson, TArray<FString>& OutErrors)
	{
		FEditContext ContextHolder(RendererAddress.System);
		FNiagaraExternalEditContext& Context = ContextHolder.Get();
		FNiagaraExt_RendererData Data;
		UNiagaraExternalEditUtilities::GetRendererData(ToReference(RendererAddress), Data, Context);
		OutJson = Data.PropertyValues;
		return Drain(Context, OutErrors);
	}

	bool FNiagaraAdapter::GetEmitterProperties(const FStackAddress& EmitterAddress, FString& OutJson, TArray<FString>& OutErrors)
	{
		FEditContext ContextHolder(EmitterAddress.System);
		FNiagaraExternalEditContext& Context = ContextHolder.Get();
		FNiagaraExt_EmitterData Data;
		UNiagaraExternalEditUtilities::GetEmitterData(ToReference(EmitterAddress), Data, Context);
		OutJson = Data.PropertyValues;
		return Drain(Context, OutErrors);
	}

	bool FNiagaraAdapter::GetSystemProperties(UNiagaraSystem* System, FString& OutJson, TArray<FString>& OutErrors)
	{
		if (System == nullptr)
		{
			OutErrors.Add(TEXT("Cannot read properties from a null system."));
			return false;
		}

		FEditContext ContextHolder(System);
		FNiagaraExternalEditContext& Context = ContextHolder.Get();
		FNiagaraExt_SystemData Data;
		UNiagaraExternalEditUtilities::GetSystemData(System, Data, Context);
		OutJson = Data.PropertyValues;
		return Drain(Context, OutErrors);
	}

	bool FNiagaraAdapter::GetModuleInputValues(const FStackAddress& ModuleAddress,
		TArray<TTuple<FName, FInputValue>>& OutValues, TArray<FString>& OutErrors)
	{
		FEditContext ContextHolder(ModuleAddress.System);
		FNiagaraExternalEditContext& Context = ContextHolder.Get();
		FNiagaraExt_ModuleInputValues Values;
		UNiagaraExternalEditUtilities::GetModuleInputValues(ToReference(ModuleAddress), Values, Context);

		for (const FNiagaraExt_StackInputValueEntry& Entry : Values.Inputs)
		{
			FInputValue Value;
			FromStackInputValue(Entry.Value, Value);
			OutValues.Emplace(Entry.Name, MoveTemp(Value));
		}

		return Drain(Context, OutErrors);
	}

	bool FNiagaraAdapter::GetDynamicInputChildren(const FStackAddress& InputAddress,
		TArray<FDynamicInputChild>& OutChildren, TArray<FString>& OutErrors)
	{
		FOpTimer OpTimer(TEXT("read: GetDynamicInputChildren"));
		FEditContext ContextHolder(InputAddress.System);
		FNiagaraExternalEditContext& Context = ContextHolder.Get();
		FNiagaraExt_DynamicInputChainRef ChainRef;
		UNiagaraExternalEditUtilities::GetDynamicInputChain(ToReference(InputAddress), ChainRef, Context);

		if (Context.HasErrors())
		{
			return Drain(Context, OutErrors);
		}

		const FNiagaraExt_DynamicInputChain& Chain = ChainRef.Get();
		for (const FNiagaraExt_DynamicInputChainRef& ChildRef : Chain.Inputs)
		{
			const FNiagaraExt_DynamicInputChain& Child = ChildRef.Get();

			FDynamicInputChild Out;
			Out.Name = Child.Name;
			Out.Type = Child.Type;
			Out.bVisible = Child.bIsVisible;
			Out.bEditable = Child.bIsEditable;
			Out.bStaticSwitch = Child.bIsStaticSwitch;
			OutChildren.Add(MoveTemp(Out));
		}
		return Drain(Context, OutErrors);
	}

	// -------------------------------------------------------------------------------------------
	// Schema
	// -------------------------------------------------------------------------------------------

	namespace
	{
		void CopySchema(const FNiagaraExt_ModuleSchema& In, FModuleSchema& Out)
		{
			for (const FNiagaraExt_StackInputSchema& Input : In.Inputs)
			{
				FInputSchema Schema;
				Schema.Name = Input.Name;
				Schema.Type = Input.Type;
				Schema.Category = Input.Category.ToString();
				Schema.Description = Input.MetaData.Description.ToString();
				Schema.bSupportsExpressions = Input.bSupportsExpressions;
				// FNiagaraExt_StackInputSchema carries no static-switch flag, and the one on
				// FNiagaraVariableMetaData is deprecated (the live flag moved to UNiagaraScriptVariable).
				// Only FNiagaraExt_StackInputTopology::bIsStaticSwitch reports it, which is why R5's
				// two-pass lowering must add the module first and read topology back, not pre-plan from
				// the schema alone.
				Schema.bIsStaticSwitch = false;
				Out.Inputs.Add(MoveTemp(Schema));
			}

			// The output is what says where a dynamic input may be plugged in, and therefore what type
			// of host the E4-1 probe has to build in order to see the live chain.
			for (const FNiagaraExt_Variable& Output : In.Outputs)
			{
				FInputSchema Schema;
				Schema.Name = Output.Name;
				Schema.Type = Output.Type;
				Out.Outputs.Add(MoveTemp(Schema));
			}
		}
	}

	bool FNiagaraAdapter::GetModuleSchema(const UNiagaraScript* ModuleAsset, FModuleSchema& OutSchema, TArray<FString>& OutErrors)
	{
		if (ModuleAsset == nullptr)
		{
			OutErrors.Add(TEXT("Cannot read the schema of a null module asset."));
			return false;
		}

		FNiagaraExternalEditContext Context;
		FNiagaraExt_ModuleSchema Schema;
		UNiagaraExternalEditUtilities::GetModuleSchema(ModuleAsset, Schema, Context);
		CopySchema(Schema, OutSchema);
		return Drain(Context, OutErrors);
	}

	bool FNiagaraAdapter::GetInputSchema(const FStackAddress& InputAddress, FInputSchema& OutSchema, TArray<FString>& OutErrors)
	{
		FOpTimer OpTimer(TEXT("read: GetInputSchema"));
		FEditContext ContextHolder(InputAddress.System);
		FNiagaraExternalEditContext& Context = ContextHolder.Get();
		FNiagaraExt_StackInputSchema Schema;
		UNiagaraExternalEditUtilities::GetStackInputSchema(ToReference(InputAddress), Schema, Context);

		OutSchema.Name = Schema.Name;
		OutSchema.Type = Schema.Type;
		OutSchema.Category = Schema.Category.ToString();
		OutSchema.Description = Schema.MetaData.Description.ToString();
		OutSchema.bSupportsExpressions = Schema.bSupportsExpressions;
		return Drain(Context, OutErrors);
	}

	bool FNiagaraAdapter::GetDynamicInputSchema(const UNiagaraScript* Asset, FModuleSchema& OutSchema, TArray<FString>& OutErrors)
	{
		if (Asset == nullptr)
		{
			OutErrors.Add(TEXT("Cannot read the schema of a null dynamic input asset."));
			return false;
		}

		FNiagaraExternalEditContext Context;
		FNiagaraExt_DynamicInputSchema Schema;
		UNiagaraExternalEditUtilities::GetDynamicInputSchema(Asset, Schema, Context);
		CopySchema(Schema, OutSchema);
		return Drain(Context, OutErrors);
	}

	void FNiagaraAdapter::GetAvailableDynamicInputs(const FNiagaraTypeDefinition& Type, TArray<UNiagaraScript*>& OutScripts)
	{
		FNiagaraExternalEditContext Context;
		UNiagaraExternalEditUtilities::GetAvailableDynamicInputs(Type, OutScripts, Context);
	}

	// -------------------------------------------------------------------------------------------
	// Compile + diagnostics
	// -------------------------------------------------------------------------------------------

	FNiagaraAdapter::FCompileSuppressionScope::FCompileSuppressionScope(UNiagaraSystem* InSystem)
		: System(InSystem)
	{
#if DREAMFX_HAS_NIAGARA_FAST_EDIT
		if (System != nullptr && !System->GetSuppressCompileRequests())
		{
			bOwns = true;
			System->SetSuppressCompileRequests(true);
			// The debt closes the guarded launch sites immediately -- RefreshAll and auto-compile ask
			// HasOutstandingCompilationRequests before compiling -- so the window starts quiet instead
			// of starting with one stray launch.
			System->DeferRequestCompile();
		}
#else
		// Stock engine: the scope becomes a no-op and every edit relaunches the system's compile.
		// RequestCompile aborts whatever is queued and starts again, so all but the last are thrown
		// away -- 260 of them, 8.3 s, on the worst system in this tree. Nothing is incorrect about it;
		// the work is simply done and discarded.
		(void)System;
#endif
	}

	FNiagaraAdapter::FCompileSuppressionScope::~FCompileSuppressionScope()
	{
#if DREAMFX_HAS_NIAGARA_FAST_EDIT
		if (bOwns)
		{
			System->SetSuppressCompileRequests(false);
		}
#endif
	}

	void FNiagaraAdapter::RequestCompileAsync(UNiagaraSystem* System)
	{
		if (System == nullptr)
		{
			return;
		}
		UE_LOG(LogDreamFX, Verbose, TEXT("PHASE RequestCompile begin '%s'"), *System->GetName());
		System->RequestCompile(/*bForce=*/false);
		UE_LOG(LogDreamFX, Verbose, TEXT("PHASE RequestCompile issued '%s'"), *System->GetName());
	}

	bool FNiagaraAdapter::PumpCompile(UNiagaraSystem* System)
	{
		if (System == nullptr)
		{
			return true;
		}
		// Completion is judged by the outstanding-work queries, not by this call's return value:
		// QueryCompileComplete returns false both mid-flight and when nothing was ever active.
		System->PollForCompilationComplete(/*bFlushRequestCompile=*/false);
		return !System->HasActiveCompilations() && !System->NeedsRequestCompile();
	}

	bool FNiagaraAdapter::WaitAndCollect(UNiagaraSystem* System, bool bIncludingGpuShaders,
		FCompileStateInfo& OutState, TArray<FString>& OutErrors)
	{
		FOpTimer OpTimer(TEXT("CompileAndWait"));
		if (System == nullptr)
		{
			OutErrors.Add(TEXT("Cannot compile a null system."));
			return false;
		}

		UE_LOG(LogDreamFX, Verbose, TEXT("PHASE WaitForCompilationComplete begin '%s'"), *System->GetName());
		System->WaitForCompilationComplete(bIncludingGpuShaders, /*bShowProgress=*/false);
		UE_LOG(LogDreamFX, Verbose, TEXT("PHASE WaitForCompilationComplete end '%s'"), *System->GetName());

		FNiagaraExternalEditContext Context(System);
		FNiagaraExt_SystemCompileState State;
		UNiagaraExternalEditUtilities::GetSystemCompileState(System, State, Context);

		const UEnum* StatusEnum = StaticEnum<ENiagaraExt_ScriptCompileStatus>();
		OutState.StatusName = StatusEnum
			? StatusEnum->GetNameStringByValue(static_cast<int64>(State.AggregateStatus))
			: FString::FromInt(static_cast<int32>(State.AggregateStatus));
		OutState.bHasErrors = State.bHasErrors;
		OutState.bHasWarnings = State.bHasWarnings;
		OutState.bIsStale = State.bIsStale;

		for (const FNiagaraExt_ScriptCompileInfo& Script : State.Scripts)
		{
			for (const FNiagaraExt_CompileEvent& Event : Script.CompileEvents)
			{
				FCompileEventInfo Info;
				Info.Severity = static_cast<int32>(Event.Severity);
				Info.Message = Event.Message;
				Info.ShortDescription = Event.ShortDescription;
				Info.EmitterName = Script.EmitterName;
				Info.ScriptName = Script.ScriptName;
				Info.bFromDependency = Event.bFromScriptDependency;
				OutState.Events.Add(MoveTemp(Info));
			}
		}

		const bool bStatusOk =
			State.AggregateStatus == ENiagaraExt_ScriptCompileStatus::UpToDate ||
			State.AggregateStatus == ENiagaraExt_ScriptCompileStatus::UpToDateWithWarnings ||
			State.AggregateStatus == ENiagaraExt_ScriptCompileStatus::ComputeUpToDateWithWarnings;

		Drain(Context, OutErrors);
		return bStatusOk && !State.bHasErrors;
	}

	bool FNiagaraAdapter::CompileAndWait(UNiagaraSystem* System, bool bIncludingGpuShaders,
		FCompileStateInfo& OutState, TArray<FString>& OutErrors)
	{
		if (System == nullptr)
		{
			OutErrors.Add(TEXT("Cannot compile a null system."));
			return false;
		}
		RequestCompileAsync(System);
		return WaitAndCollect(System, bIncludingGpuShaders, OutState, OutErrors);
	}

	bool FNiagaraAdapter::IsStackIssueReadingAvailable()
	{
		// GetStackIssues does NOT go through the data-only view model the rest of this API uses. It
		// calls FNiagaraExternalEditContext::GetDiagnosticsSystemViewModel, which deliberately builds a
		// *non*-data-only FNiagaraSystemViewModel so the stack-issue arrays are populated -- and that
		// path runs the full detail-customisation machinery, which constructs Slate widgets
		// (FNiagaraUserParameterBindingCustomization::CustomizeHeader -> SComboButton). With no Slate
		// application there is nothing to construct against and the process dies.
		//
		// So this one function is editor-only, unlike every other call in this file. Headless builds
		// fall back to compile events, which carry the errors that actually matter for a CI gate.
		return !IsRunningCommandlet() && FSlateApplication::IsInitialized();
	}

	bool FNiagaraAdapter::GetStackIssues(UNiagaraSystem* System, TArray<FStackIssueInfo>& OutIssues, TArray<FString>& OutErrors)
	{
		if (System == nullptr)
		{
			OutErrors.Add(TEXT("Cannot read stack issues from a null system."));
			return false;
		}

		if (!IsStackIssueReadingAvailable())
		{
			return false;
		}

		FNiagaraExternalEditContext Context(System);
		FNiagaraExt_StackIssues Issues;
		UNiagaraExternalEditUtilities::GetStackIssues(System, Issues, Context);

		for (const FNiagaraExt_StackIssue& Issue : Issues.Issues)
		{
			if (Issue.bIsDismissed)
			{
				continue;
			}
			FStackIssueInfo Info;
			Info.Severity = static_cast<int32>(Issue.Severity);
			Info.ShortDescription = Issue.ShortDescription;
			Info.LongDescription = Issue.LongDescription;
			Info.DisplayPath = Issue.StackDisplayPath;
			Info.EmitterName = Issue.Location.EmitterName;
			Info.ScriptName = Issue.Location.ScriptName;
			Info.ModuleName = Issue.Location.ModuleName;
			OutIssues.Add(MoveTemp(Info));
		}

		return Drain(Context, OutErrors);
	}

	// -------------------------------------------------------------------------------------------
	// Naming helpers
	// -------------------------------------------------------------------------------------------

	namespace
	{
		bool ScriptUsageForStack(EStackKind Kind, ENiagaraScriptUsage& OutUsage)
		{
			switch (Kind)
			{
			case EStackKind::SystemSpawn:    OutUsage = ENiagaraScriptUsage::SystemSpawnScript;    return true;
			case EStackKind::SystemUpdate:   OutUsage = ENiagaraScriptUsage::SystemUpdateScript;   return true;
			case EStackKind::EmitterSpawn:   OutUsage = ENiagaraScriptUsage::EmitterSpawnScript;   return true;
			case EStackKind::EmitterUpdate:  OutUsage = ENiagaraScriptUsage::EmitterUpdateScript;  return true;
			case EStackKind::ParticleSpawn:  OutUsage = ENiagaraScriptUsage::ParticleSpawnScript;  return true;
			case EStackKind::ParticleUpdate: OutUsage = ENiagaraScriptUsage::ParticleUpdateScript; return true;
			default: return false;
			}
		}
	}

	FString FScriptVersion::ToStampString() const
	{
		return FString::Printf(TEXT("%d.%d:%s"), Major, Minor, *Guid.ToString(EGuidFormats::Digits));
	}

	bool FScriptVersion::FromStampString(const FString& Text, FScriptVersion& OutVersion)
	{
		FString Numbers;
		FString GuidText;
		if (!Text.Split(TEXT(":"), &Numbers, &GuidText))
		{
			return false;
		}

		FString MajorText;
		FString MinorText;
		if (!Numbers.Split(TEXT("."), &MajorText, &MinorText))
		{
			return false;
		}

		OutVersion.Major = FCString::Atoi(*MajorText);
		OutVersion.Minor = FCString::Atoi(*MinorText);
		return FGuid::ParseExact(GuidText, EGuidFormats::Digits, OutVersion.Guid);
	}

	FScriptVersion FNiagaraAdapter::GetScriptVersion(const UNiagaraScript* Asset)
	{
		FScriptVersion Version;
		if (Asset == nullptr)
		{
			return Version;
		}

		const FNiagaraAssetVersion Exposed = Asset->GetExposedVersion();
		Version.Major = Exposed.MajorVersion;
		Version.Minor = Exposed.MinorVersion;
		Version.Guid = Exposed.VersionGuid;
		Version.bVersioningEnabled = Asset->IsVersioningEnabled();
		return Version;
	}

	namespace
	{
		/** The one graph an emitter's (or the system's) stacks are all built out of. */
		UNiagaraGraph* GraphForAddress(const FStackAddress& Address)
		{
			if (Address.System == nullptr)
			{
				return nullptr;
			}

			UNiagaraScriptSourceBase* SourceBase = nullptr;
			if (Address.EmitterName.IsNone())
			{
				// Both system-scope stacks live in the system script's source.
				if (UNiagaraScript* SystemScript = Address.System->GetSystemSpawnScript())
				{
					SourceBase = SystemScript->GetLatestSource();
				}
			}
			else
			{
				for (const FNiagaraEmitterHandle& Handle : Address.System->GetEmitterHandles())
				{
					if (Handle.GetName() != Address.EmitterName)
					{
						continue;
					}
					if (const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData())
					{
						SourceBase = Data->GraphSource;
					}
					break;
				}
			}

			UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(SourceBase);
			return Source != nullptr ? Source->NodeGraph : nullptr;
		}

		/**
		 * The function-call node a module reference addresses.
		 *
		 * Matched on GetFunctionName(), which is the same comparison the external edit API makes when
		 * it resolves a FNiagaraExt_StackItemReference -- so a name that addressed a module there
		 * addresses the same node here. Names are unique within a graph (Niagara appends 001, 002 to
		 * keep them so), which is why a whole-graph scan needs no stack ordering.
		 */
		UNiagaraNodeFunctionCall* FindModuleNode(const FStackAddress& ModuleAddress)
		{
			UNiagaraGraph* Graph = GraphForAddress(ModuleAddress);
			if (Graph == nullptr || ModuleAddress.ModuleName.IsNone())
			{
				return nullptr;
			}

			const FString Wanted = ModuleAddress.ModuleName.ToString();
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				UNiagaraNodeFunctionCall* Call = Cast<UNiagaraNodeFunctionCall>(Node);
				if (Call != nullptr && Call->GetFunctionName() == Wanted)
				{
					return Call;
				}
			}
			return nullptr;
		}

		FScriptVersion ToScriptVersion(const FNiagaraAssetVersion& Version, bool bVersioningEnabled)
		{
			FScriptVersion Out;
			Out.Major = Version.MajorVersion;
			Out.Minor = Version.MinorVersion;
			Out.Guid = Version.VersionGuid;
			Out.bVersioningEnabled = bVersioningEnabled;
			return Out;
		}
	}

	void FNiagaraAdapter::CollectIfHeavy()
	{
		constexpr uint64 CollectAboveBytes = 6ull * 1024 * 1024 * 1024;
		constexpr uint64 GrowthSinceLastCollectBytes = 2ull * 1024 * 1024 * 1024;

		// What the last collection left behind. The threshold alone is not a stopping condition: a
		// build of the four content packs sits at roughly 8 GB of live, reachable assets, so it is
		// permanently over any fixed threshold and collected on every single call -- a full purge per
		// module, which turned a rebuild into hours of `Compacting FUObjectHashTables`. Collecting
		// again only after the process has grown *since the last collection* is what makes this a
		// ceiling rather than a treadmill.
		static uint64 UsedAfterLastCollect = 0;

		const uint64 Used = FPlatformMemory::GetStats().UsedPhysical;
		if (Used <= CollectAboveBytes)
		{
			return;
		}
		if (UsedAfterLastCollect != 0 && Used < UsedAfterLastCollect + GrowthSinceLastCollectBytes)
		{
			return;
		}

		// What goes is exactly the view-model debris: the assets being read or written are reachable
		// from their packages, and the schema probe holds its own root.
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, /*bPerformFullPurge=*/true);
		UsedAfterLastCollect = FPlatformMemory::GetStats().UsedPhysical;
	}

	void FNiagaraAdapter::RefreshCurveLookupTables(UNiagaraSystem* System)
	{
		FOpTimer OpTimer(TEXT("RefreshCurveLookupTables"));
#if WITH_EDITORONLY_DATA
		if (System == nullptr)
		{
			return;
		}

		int32 Refreshed = 0;
		ForEachObjectWithOuter(System->GetOutermost(),
			[&Refreshed](UObject* Object)
			{
				if (UNiagaraDataInterfaceCurveBase* Curve = Cast<UNiagaraDataInterfaceCurveBase>(Object))
				{
					Curve->UpdateLUT();
					++Refreshed;
				}
			},
			EGetObjectsFlags::IncludeNestedObjects);

		if (Refreshed > 0)
		{
			UE_LOG(LogDreamFX, Verbose, TEXT("  rebaked %d curve lookup table(s) on '%s'"),
				Refreshed, *System->GetName());
		}
#endif
	}

	TArray<FScriptVersion> FNiagaraAdapter::GetAvailableScriptVersions(const UNiagaraScript* Asset)
	{
		TArray<FScriptVersion> Versions;
		if (Asset == nullptr)
		{
			return Versions;
		}

		const bool bVersioningEnabled = Asset->IsVersioningEnabled();
		for (const FNiagaraAssetVersion& Version : Asset->GetAllAvailableVersions())
		{
			Versions.Add(ToScriptVersion(Version, bVersioningEnabled));
		}
		return Versions;
	}

	bool FNiagaraAdapter::GetModuleScriptVersion(const FStackAddress& ModuleAddress, FScriptVersion& OutVersion,
		TArray<FString>& OutErrors)
	{
		UNiagaraNodeFunctionCall* Node = FindModuleNode(ModuleAddress);
		if (Node == nullptr)
		{
			OutErrors.Add(FString::Printf(TEXT("No module node named '%s' was found in the graph."),
				*ModuleAddress.ModuleName.ToString()));
			return false;
		}

		UNiagaraScript* Script = Node->FunctionScript;
		if (Script == nullptr)
		{
			OutErrors.Add(FString::Printf(TEXT("Module '%s' has no script asset."),
				*ModuleAddress.ModuleName.ToString()));
			return false;
		}

		// An asset that never opted into versioning has one implicit version and a node that carries
		// no guid for it. Reporting the exposed version keeps every caller on one code path.
		if (!Script->IsVersioningEnabled() || !Node->SelectedScriptVersion.IsValid())
		{
			OutVersion = GetScriptVersion(Script);
			return true;
		}

		const FVersionedNiagaraScriptData* Data = Script->GetScriptData(Node->SelectedScriptVersion);
		if (Data == nullptr)
		{
			OutErrors.Add(FString::Printf(
				TEXT("Module '%s' is bound to a script version that '%s' no longer offers."),
				*ModuleAddress.ModuleName.ToString(), *Script->GetName()));
			return false;
		}

		OutVersion = ToScriptVersion(Data->Version, /*bVersioningEnabled=*/true);
		return true;
	}

	bool FNiagaraAdapter::SetModuleScriptVersion(const FStackAddress& ModuleAddress, const FGuid& VersionGuid,
		TArray<FString>& OutErrors)
	{
		FOpTimer OpTimer(TEXT("SetModuleScriptVersion"));
		UNiagaraNodeFunctionCall* Node = FindModuleNode(ModuleAddress);
		if (Node == nullptr)
		{
			OutErrors.Add(FString::Printf(TEXT("No module node named '%s' was found in the graph."),
				*ModuleAddress.ModuleName.ToString()));
			return false;
		}

		if (Node->FunctionScript == nullptr || Node->FunctionScript->GetScriptData(VersionGuid) == nullptr)
		{
			OutErrors.Add(FString::Printf(
				TEXT("Module '%s' cannot be moved to that script version: its asset does not offer one with that identity."),
				*ModuleAddress.ModuleName.ToString()));
			return false;
		}

		if (Node->SelectedScriptVersion == VersionGuid)
		{
			return true;
		}

		// Structural, and about as structural as it gets: a version change is exactly a change to
		// which inputs the module has. Placed after the early-outs above so a no-op rebind does not
		// throw away a perfectly good epoch.
		FEpochGuard Epoch(ModuleAddress.System);

		// The graph-side sequence, not the stack-side one: FNiagaraFunctionCallNodeDetails::
		// SwitchToVersion is the same situation as this -- a version changed on the node rather than
		// through a stack view model -- and it skips the Python upgrade scripts for the same reason
		// DreamFX must ("we don't need to remap any inputs"): every input is written from source
		// immediately afterwards, so remapping produces work that is then overwritten.
		FNiagaraScriptVersionUpgradeContext UpgradeContext;
		UpgradeContext.bSkipPythonScript = true;

		Node->ChangeScriptVersion(VersionGuid, UpgradeContext, /*bShowNotesInStack=*/false);

		// Load-bearing, and the reason the first attempt at this reported a module with no static
		// switch selectors and two pins called `ScaleRGB`: ChangeScriptVersion records the choice and
		// drops override pins, but the node's own pin set is still the one it derived from the version
		// it had. Refreshing is what re-derives it, and until it runs every topology read describes a
		// module that is half one version and half the other.
		Node->RefreshFromExternalChanges();
		return true;
	}

	bool FNiagaraAdapter::SetStaticSwitchByPin(const FStackAddress& ModuleAddress, FName SwitchVariableName,
		const FInputValue& Value, bool& bOutNotASwitch, TArray<FString>& OutErrors)
	{
		FOpTimer OpTimer(TEXT("SetStaticSwitchByPin"));
		bOutNotASwitch = false;

		UNiagaraNodeFunctionCall* Node = FindModuleNode(ModuleAddress);
		const UEdGraphSchema_Niagara* Schema = GetDefault<UEdGraphSchema_Niagara>();
		UNiagaraGraph* CalledGraph = Node != nullptr ? Node->GetCalledGraph() : nullptr;
		if (Node == nullptr || Schema == nullptr || CalledGraph == nullptr)
		{
			// Not an error: every depth-one input asks this question, and plenty of them address
			// something that is not a module function call with a graph behind it.
			bOutNotASwitch = true;
			return false;
		}

		// UNiagaraNodeFunctionCall::FindStaticSwitchInputPin is public but unexported, so its two steps
		// are repeated here: the switch set comes from the *called* graph -- the pin's own type does not
		// carry the static flag, which is what made the first attempt find nothing -- and the pin on the
		// node is named after the variable.
		TArray<UEdGraphPin*> InputPins;
		Node->GetInputPins(InputPins);

		UEdGraphPin* SwitchPin = nullptr;
		FNiagaraTypeDefinition SwitchType;
		const TArray<FNiagaraVariable> SwitchVariables = CalledGraph->FindStaticSwitchInputs();
		for (const FNiagaraVariable& SwitchVariable : SwitchVariables)
		{
			if (!SwitchVariable.GetName().IsEqual(SwitchVariableName))
			{
				continue;
			}
			for (UEdGraphPin* Pin : InputPins)
			{
				if (Pin != nullptr && SwitchVariableName.IsEqual(Pin->GetFName()))
				{
					SwitchPin = Pin;
					SwitchType = SwitchVariable.GetType();
					break;
				}
			}
			break;
		}

		if (SwitchPin == nullptr)
		{
			// The module declares no switch by this name, which is the ordinary answer for an ordinary
			// input. The caller writes it the normal way.
			//
			// -DreamFXTraceSwitchLookup prints what the module actually declares against what was
			// asked for. It exists because an input can be static-typed to the engine and still be
			// absent from this list: a switch a module *propagates* from an inner call surfaces as an
			// input on the outer module with no pin on the outer node. Nothing short of printing both
			// sides distinguishes that from a plain name mismatch -- it is what identified the case.
			if (FParse::Param(FCommandLine::Get(), TEXT("DreamFXTraceSwitchLookup")))
			{
				TArray<FString> Declared;
				for (const FNiagaraVariable& SwitchVariable : SwitchVariables)
				{
					Declared.Add(SwitchVariable.GetName().ToString());
				}
				TArray<FString> PinNames;
				for (const UEdGraphPin* Pin : InputPins)
				{
					if (Pin != nullptr) { PinNames.Add(Pin->GetFName().ToString()); }
				}
				UE_LOG(LogDreamFX, Warning,
					TEXT("[switch-trace] module '%s' wanted '%s' | declares: %s | pins: %s"),
					*ModuleAddress.ModuleName.ToString(), *SwitchVariableName.ToString(),
					Declared.Num() > 0 ? *FString::Join(Declared, TEXT(", ")) : TEXT("(none)"),
					PinNames.Num() > 0 ? *FString::Join(PinNames, TEXT(", ")) : TEXT("(none)"));
			}
			bOutNotASwitch = true;
			return false;
		}

		FNiagaraVariable LocalValue(SwitchType, NAME_None);
		LocalValue.AllocateData();

		if (Value.Mode == EInputValueMode::Literal
			&& Value.LiteralBytes.Num() == LocalValue.GetSizeInBytes())
		{
			LocalValue.SetData(Value.LiteralBytes.GetData());
		}
		else if (Value.Mode == EInputValueMode::Enum && Value.EnumType != nullptr)
		{
			const int64 EntryValue = Value.EnumType->GetValueByName(Value.EnumEntryName);
			if (EntryValue == INDEX_NONE || LocalValue.GetSizeInBytes() != sizeof(int32))
			{
				OutErrors.Add(FString::Printf(TEXT("'%s' is not an entry of enum '%s'."),
					*Value.EnumEntryName.ToString(), *Value.EnumType->GetName()));
				return false;
			}
			const int32 Narrowed = static_cast<int32>(EntryValue);
			LocalValue.SetData(reinterpret_cast<const uint8*>(&Narrowed));
		}
		else
		{
			OutErrors.Add(FString::Printf(
				TEXT("A static switch takes a compile-time constant; '%s' was given a value this write path cannot encode."),
				*SwitchVariableName.ToString()));
			return false;
		}

		FString PinDefaultValue;
		if (!Schema->TryGetPinDefaultValueFromNiagaraVariable(LocalValue, PinDefaultValue))
		{
			OutErrors.Add(FString::Printf(TEXT("Niagara could not encode a %s value as a pin default for '%s'."),
				*SwitchType.GetName(), *SwitchVariableName.ToString()));
			return false;
		}

		SwitchPin->Modify();
		SwitchPin->DefaultValue = PinDefaultValue;
		Node->MarkNodeRequiresSynchronization(TEXT("DreamFX static switch write"), /*bRaiseGraphNeedsRecompile=*/true);
		return true;
	}

	bool FNiagaraAdapter::SetDynamicInputAtVersion(const FStackAddress& InputAddress, UNiagaraScript* DynamicInput,
		const FGuid& VersionGuid, TArray<FString>& OutErrors)
	{
		FOpTimer OpTimer(TEXT("SetDynamicInputAtVersion"));
		if (DynamicInput == nullptr)
		{
			OutErrors.Add(TEXT("Cannot write a null dynamic input."));
			return false;
		}

		UNiagaraGraph* Graph = GraphForAddress(InputAddress);
		if (Graph == nullptr || !VersionGuid.IsValid())
		{
			// No graph to inspect, or nothing to pin: an ordinary write, which is what an unversioned
			// dynamic input and a hand-written call both want.
			return SetInput(InputAddress, FInputValue::MakeDynamicInput(DynamicInput), OutErrors);
		}

		// The inner SetInput ends the epoch on its own (a dynamic input write creates a node), but this
		// function keeps mutating afterwards -- ChangeScriptVersion re-derives the new node's pins. The
		// guard covers that tail as well, so the epoch ends when the whole operation is finished
		// rather than in the middle of it.
		FEpochGuard Epoch(InputAddress.System);

		TSet<const UEdGraphNode*> Before;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			Before.Add(Node);
		}

		if (!SetInput(InputAddress, FInputValue::MakeDynamicInput(DynamicInput), OutErrors))
		{
			return false;
		}

		UNiagaraNodeFunctionCall* Created = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Before.Contains(Node))
			{
				continue;
			}
			UNiagaraNodeFunctionCall* Call = Cast<UNiagaraNodeFunctionCall>(Node);
			if (Call != nullptr && Call->FunctionScript == DynamicInput)
			{
				Created = Call;
				break;
			}
		}

		if (Created == nullptr)
		{
			// The write succeeded but nothing new calls this script, which happens when the input
			// already held this very dynamic input. Reported rather than silently ignored: the caller
			// asked for a version and did not get one.
			OutErrors.Add(FString::Printf(
				TEXT("Wrote dynamic input '%s', but no new call node appeared, so its script version could not be set."),
				*DynamicInput->GetName()));
			return false;
		}

		if (Created->FunctionScript->GetScriptData(VersionGuid) == nullptr)
		{
			OutErrors.Add(FString::Printf(
				TEXT("Dynamic input '%s' does not offer a script version with that identity."),
				*DynamicInput->GetName()));
			return false;
		}

		FNiagaraScriptVersionUpgradeContext UpgradeContext;
		UpgradeContext.bSkipPythonScript = true;
		Created->ChangeScriptVersion(VersionGuid, UpgradeContext, /*bShowNotesInStack=*/false);
		Created->RefreshFromExternalChanges();
		return true;
	}

	bool FNiagaraAdapter::GetDynamicInputScriptVersion(const FStackAddress& EmitterAddress,
		const UNiagaraScript* DynamicInput, FScriptVersion& OutVersion, TArray<FString>& OutErrors)
	{
		if (DynamicInput == nullptr)
		{
			OutErrors.Add(TEXT("Cannot read the version of a null dynamic input."));
			return false;
		}

		if (!DynamicInput->IsVersioningEnabled())
		{
			OutVersion = GetScriptVersion(DynamicInput);
			return true;
		}

		UNiagaraGraph* Graph = GraphForAddress(EmitterAddress);
		if (Graph == nullptr)
		{
			OutErrors.Add(TEXT("No graph to read dynamic input versions from."));
			return false;
		}

		bool bFound = false;
		FGuid Agreed;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			const UNiagaraNodeFunctionCall* Call = Cast<UNiagaraNodeFunctionCall>(Node);
			if (Call == nullptr || Call->FunctionScript.Get() != DynamicInput)
			{
				continue;
			}

			const FGuid Selected = Call->SelectedScriptVersion.IsValid()
				? Call->SelectedScriptVersion
				: DynamicInput->GetExposedVersion().VersionGuid;

			if (!bFound)
			{
				Agreed = Selected;
				bFound = true;
				continue;
			}
			if (Agreed != Selected)
			{
				OutErrors.Add(FString::Printf(
					TEXT("'%s' is called at more than one script version in this emitter, so no single version describes it."),
					*DynamicInput->GetName()));
				return false;
			}
		}

		if (!bFound)
		{
			OutErrors.Add(FString::Printf(TEXT("No call to '%s' was found in this emitter's graph."),
				*DynamicInput->GetName()));
			return false;
		}

		const FVersionedNiagaraScriptData* Data = DynamicInput->GetScriptData(Agreed);
		if (Data == nullptr)
		{
			OutErrors.Add(FString::Printf(TEXT("'%s' is bound to a script version it no longer offers."),
				*DynamicInput->GetName()));
			return false;
		}

		OutVersion = ToScriptVersion(Data->Version, /*bVersioningEnabled=*/true);
		return true;
	}

	FName FNiagaraAdapter::ScriptNameForStack(EStackKind Kind)
	{
		ENiagaraScriptUsage Usage;
		if (!ScriptUsageForStack(Kind, Usage))
		{
			return NAME_None;
		}

		// Reflection rather than a literal: the API resolves ScriptName through
		// StaticEnum<ENiagaraScriptUsage>()->GetValueByName, so deriving the spelling from the same
		// table means a rename upstream cannot silently desynchronise the two.
		const UEnum* UsageEnum = StaticEnum<ENiagaraScriptUsage>();
		return UsageEnum ? UsageEnum->GetNameByValue(static_cast<int64>(Usage)) : NAME_None;
	}

	bool FNiagaraAdapter::StackForScriptName(FName ScriptName, EStackKind& OutKind)
	{
		static const EStackKind AllKinds[] =
		{
			EStackKind::SystemSpawn, EStackKind::SystemUpdate,
			EStackKind::EmitterSpawn, EStackKind::EmitterUpdate,
			EStackKind::ParticleSpawn, EStackKind::ParticleUpdate,
		};

		// The API is not consistent about which spelling it uses: writes take the qualified name
		// ("ENiagaraScriptUsage::EmitterUpdateScript") while GetEmitterTopology reports the short one
		// ("EmitterUpdateScript"). Accepting both is the only way a read-then-write round trip works.
		FString Short = ScriptName.ToString();
		int32 ColonIndex;
		if (Short.FindLastChar(TEXT(':'), ColonIndex))
		{
			Short = Short.RightChop(ColonIndex + 1);
		}

		for (EStackKind Kind : AllKinds)
		{
			FString Candidate = ScriptNameForStack(Kind).ToString();
			int32 CandidateColon;
			if (Candidate.FindLastChar(TEXT(':'), CandidateColon))
			{
				Candidate = Candidate.RightChop(CandidateColon + 1);
			}
			if (Candidate == Short)
			{
				OutKind = Kind;
				return true;
			}
		}
		return false;
	}

	UClass* FNiagaraAdapter::FindRendererClass(const FString& TypeName)
	{
		// Discovered by reflection rather than a fixed table so renderers added by other plugins get
		// DSL support for free -- which is the whole point of L8's schema-driven property block.
		const FString Trimmed = TypeName.TrimStartAndEnd();
		const FString Candidate = FString::Printf(TEXT("Niagara%sProperties"), *Trimmed);

		for (TObjectIterator<UClass> ClassIterator; ClassIterator; ++ClassIterator)
		{
			UClass* Class = *ClassIterator;
			if (!Class->IsChildOf(UNiagaraRendererProperties::StaticClass())
				|| Class == UNiagaraRendererProperties::StaticClass()
				|| Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
			{
				continue;
			}

			const FString ClassName = Class->GetName();
			if (ClassName.Equals(Candidate, ESearchCase::IgnoreCase)
				|| ClassName.Equals(Trimmed, ESearchCase::IgnoreCase)
				|| ClassName.Equals(FString::Printf(TEXT("%sProperties"), *Trimmed), ESearchCase::IgnoreCase))
			{
				return Class;
			}
		}
		return nullptr;
	}

	FString FNiagaraAdapter::RendererTypeNameForClass(const UClass* Class)
	{
		if (Class == nullptr)
		{
			return FString();
		}

		FString Name = Class->GetName();
		Name.RemoveFromStart(TEXT("Niagara"), ESearchCase::CaseSensitive);
		Name.RemoveFromEnd(TEXT("Properties"), ESearchCase::CaseSensitive);
		return Name;
	}

	bool FNiagaraAdapter::GetArrayElementReferenceField(const UClass* RendererClass,
		const FString& JsonPropertyName, FString& OutReferenceField, FString& OutElementDefaultsJson,
		TArray<FString>& OutErrors)
	{
		if (RendererClass == nullptr)
		{
			OutErrors.Add(TEXT("Cannot inspect a property on a null renderer class."));
			return false;
		}

		// The blob's keys are UE's JSON spelling of the UPROPERTY name -- first character lowercased,
		// the rest untouched, which is why "LODMode" arrives as "lODMode". Comparing case-insensitively
		// is exact enough here because two UPROPERTYs on one struct cannot differ only by case.
		const FArrayProperty* ArrayProperty = nullptr;
		for (TFieldIterator<FProperty> It(RendererClass); It; ++It)
		{
			if (It->GetName().Equals(JsonPropertyName, ESearchCase::IgnoreCase))
			{
				ArrayProperty = CastField<FArrayProperty>(*It);
				break;
			}
		}

		if (ArrayProperty == nullptr)
		{
			OutErrors.Add(FString::Printf(TEXT("'%s' is not an array property on %s."),
				*JsonPropertyName, *RendererClass->GetName()));
			return false;
		}

		const FStructProperty* ElementProperty = CastField<FStructProperty>(ArrayProperty->Inner);
		if (ElementProperty == nullptr || ElementProperty->Struct == nullptr)
		{
			OutErrors.Add(FString::Printf(TEXT("'%s' is not an array of structs."), *JsonPropertyName));
			return false;
		}

		UScriptStruct* ElementStruct = ElementProperty->Struct;

		const FObjectPropertyBase* ReferenceProperty = nullptr;
		int32 ReferenceCount = 0;
		for (TFieldIterator<FProperty> It(ElementStruct); It; ++It)
		{
			if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(*It))
			{
				ReferenceProperty = ObjectProperty;
				++ReferenceCount;
			}
		}

		if (ReferenceCount != 1)
		{
			OutErrors.Add(FString::Printf(
				TEXT("'%s' elements (%s) carry %d asset reference field(s); exactly one is required to represent an element as a path."),
				*JsonPropertyName, *ElementStruct->GetName(), ReferenceCount));
			return false;
		}

		FString FieldName = ReferenceProperty->GetName();
		FieldName[0] = FChar::ToLower(FieldName[0]);
		OutReferenceField = FieldName;

		// A default-constructed element, serialised the same way the property blob is, so the
		// decompiler can tell "just a mesh" from "a mesh with a custom pivot" without a table of
		// per-field defaults.
		TArray<uint8> Storage;
		Storage.SetNumZeroed(ElementStruct->GetStructureSize());
		ElementStruct->InitializeStruct(Storage.GetData());

		const TSharedRef<FJsonObject> DefaultsObject = MakeShared<FJsonObject>();
		const bool bSerialized = FJsonObjectConverter::UStructToJsonObject(
			ElementStruct, Storage.GetData(), DefaultsObject, /*CheckFlags=*/0, /*SkipFlags=*/0);

		ElementStruct->DestroyStruct(Storage.GetData());

		if (!bSerialized)
		{
			OutErrors.Add(FString::Printf(TEXT("Could not serialise a default '%s' element."), *ElementStruct->GetName()));
			return false;
		}

		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutElementDefaultsJson);
		FJsonSerializer::Serialize(DefaultsObject, Writer);
		return true;
	}
}
