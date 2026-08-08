#include "DreamFXGenerator.h"

#include "Adapter/DreamFXNiagaraAdapter.h"
#include "DreamFXExpressions.h"
#include "DreamFXModule.h"
#include "DreamFXParser.h"
#include "DreamFXProvenance.h"
#include "DreamFXValueLowering.h"
#include "Generation/DreamFXModuleGenerator.h"
#include "Lint/DreamFXLint.h"
#include "Schema/DreamFXModuleLibrary.h"
#include "SourceFiles/DreamFXPaths.h"

#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::DreamFX::Editor
{
	namespace
	{
		/**
		 * Every module and dynamic input asset a build touched, with the version each exposed at the
		 * time (R7, plan-v2 W3).
		 *
		 * Collected during planning rather than read back afterwards, because the asset pointer is in
		 * hand exactly once -- at resolution -- and turning a path back into an asset later would mean
		 * loading it again to ask a question that was already answerable.
		 */
		struct FDependencySet
		{
			TArray<FString> Paths;
			/** Asset path -> "Major.Minor:Guid". */
			TMap<FString, FString> Versions;

			void Add(const UNiagaraScript* Asset)
			{
				if (Asset == nullptr)
				{
					return;
				}
				const FString Path = Asset->GetPathName();
				Paths.AddUnique(Path);
				Versions.Add(Path, FNiagaraAdapter::GetScriptVersion(Asset).ToStampString());
			}
		};

		/**
		 * One resolved write, addressed by its input-name path relative to the owning module.
		 *
		 * A path rather than a name because a dynamic input chain is just a deeper address:
		 * `SpriteSizeMin = ToonPulse(Frequency = RandomRangeFloat(Min = 1))` becomes three writes at
		 * [SpriteSizeMin], [SpriteSizeMin, Frequency] and [SpriteSizeMin, Frequency, Min]. Written in
		 * that order, each level exists before the next one addresses through it.
		 */
		struct FPlannedInput
		{
			TArray<FName> Path;
			FInputValue Value;
			FSourceLocation Location;

			/** For a dynamic input write, the script version its node is rebound to (R1b). */
			FGuid DynamicInputVersion;

			/**
			 * Whether this write is a static switch (plan-v6 P1).
			 *
			 * Recorded here because this is where it is known: the module schema is in hand while
			 * planning and gone by the time the write happens. Writing a switch changes which *other*
			 * inputs exist, so it ends the write scope's structural epoch -- the same reason the
			 * switch pass runs before the value pass a few lines below.
			 */
			bool bIsStaticSwitch = false;
		};

		/**
		 * Flags everything one argument planned as a static switch write.
		 *
		 * A range rather than a single entry because one argument can plan several writes, and the
		 * caller records the start index before planning so it does not have to know how many.
		 */
		void MarkStaticSwitch(TArray<FPlannedInput>& Inputs, int32 FirstIndex, bool bIsStaticSwitch)
		{
			if (!bIsStaticSwitch)
			{
				return;
			}
			for (int32 Index = FirstIndex; Index < Inputs.Num(); ++Index)
			{
				Inputs[Index].bIsStaticSwitch = true;
			}
		}

		/** One entry of a folded Set Parameters module (L2). */
		struct FPlannedSetParameter
		{
			FName Name;
			FNiagaraTypeDefinition Type;
			FInputValue Value;
			FSourceLocation Location;

			/** As FPlannedInput: the entry's own value may itself be a pinned dynamic input (R1b). */
			FGuid DynamicInputVersion;

			/** Writes below the entry itself: the dynamic input chain hanging off it, if any. */
			TArray<FPlannedInput> NestedInputs;
		};

		struct FPlannedModule
		{
			FString SourceName;
			UNiagaraScript* Asset = nullptr;
			TArray<FPlannedInput> Inputs;
			FSourceLocation Location;

			/** `Foo@1.2(...)`: the version the module is rebound to after it is added (R1b). */
			FGuid VersionGuid;

			/** When true this is a Set Parameters module and Parameters holds its entries. */
			bool bIsSetParameters = false;
			TArray<FPlannedSetParameter> Parameters;

			/** `disabled Foo(...)`: added to the stack, then switched off (plan-v3 E4-2). */
			bool bDisabled = false;
		};

		struct FPlannedStack
		{
			EStackKind Kind = EStackKind::ParticleUpdate;
			FName ScriptName;
			TArray<FPlannedModule> Modules;
			FSourceLocation Location;
		};

		struct FPlannedBinding
		{
			FString PropertyName;
			FName Target;
			FSourceLocation Location;
		};

		struct FPlannedRenderer
		{
			UClass* Class = nullptr;
			FString PropertiesJson;
			TArray<FPlannedBinding> Bindings;
			FSourceLocation Location;
		};

		struct FPlannedEmitter
		{
			FName Name;
			TArray<FPlannedStack> Stacks;
			TArray<FPlannedRenderer> Renderers;
			FString PropertiesJson;
			FSourceLocation Location;
		};

		struct FPlannedUserVariable
		{
			FName Name;
			FNiagaraTypeDefinition Type;
			FString Description;
			FInputValue DefaultValue;
			FSourceLocation Location;
		};

		struct FPlan
		{
			FString PackagePath;
			FString AssetName;
			FString FullAssetPath;
			FString SystemPropertiesJson;
			TArray<FPlannedUserVariable> UserVariables;
			TArray<FPlannedStack> SystemStacks;
			TArray<FPlannedEmitter> Emitters;
			FDependencySet Dependencies;
		};

		/** Converts a literal AST value into JSON for the object-property blob writers. */
		bool ValueToJson(const FValue& Value, const FString& DefaultRoot, const FString& PropertyName,
			FDiagnosticSink& Diagnostics, TSharedPtr<FJsonValue>& OutJson)
		{
			switch (Value.Kind)
			{
			case EValueKind::Number:
				OutJson = MakeShared<FJsonValueNumber>(Value.Number);
				return true;

			case EValueKind::Bool:
				OutJson = MakeShared<FJsonValueBoolean>(Value.bBool);
				return true;

			case EValueKind::Name:
				// Enum entries serialise as their name; the JSON-to-struct converter resolves them.
				OutJson = MakeShared<FJsonValueString>(Value.Text);
				return true;

			case EValueKind::String:
			{
				// plan-v5 R4. A quoted structured value is spliced in as JSON rather than written as a
				// JSON *string*, which is how `MaterialParameters` and the other struct-shaped renderer
				// properties survive a round trip (see TryWriteJsonBlob in the decompiler).
				//
				// Gated on the leading brace and on the parse succeeding, so an ordinary string
				// property is unaffected: no renderer property in the engine holds a string that both
				// starts with `{`/`[` and is valid JSON, and one that did would have been written by
				// this same pair of rules anyway.
				if (Value.Text.StartsWith(TEXT("{")) || Value.Text.StartsWith(TEXT("[")))
				{
					TSharedPtr<FJsonValue> Parsed;
					const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Value.Text);
					if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid())
					{
						OutJson = Parsed;
						return true;
					}
				}

				// A quoted string in a property block is an asset reference often enough that resolving
				// the DreamFX root prefix is the useful default; plain strings pass through unchanged
				// because resolution only rewrites references that carry a root or look like a path.
				FString Resolved;
				FString Error;
				if (Value.Text.Contains(TEXT(":")) || Value.Text.StartsWith(TEXT("/")))
				{
					if (!FDreamFXPaths::ResolveAssetPath(Value.Text, DefaultRoot, Resolved, Error))
					{
						Diagnostics.Error(TEXT("DFX3010"), Value.Location,
							FString::Printf(TEXT("Property '%s': %s"), *PropertyName, *Error));
						return false;
					}
					// The external edit API's JSON importer rejects bare package paths -- its reference
					// converter demands the Package.Object form. ResolveAssetPath already assumes the
					// asset shares the package's short name when it strips a suffix, so appending it
					// back is lossless.
					OutJson = MakeShared<FJsonValueString>(
						FString::Printf(TEXT("%s.%s"), *Resolved, *FPackageName::GetShortName(Resolved)));
					return true;
				}
				OutJson = MakeShared<FJsonValueString>(Value.Text);
				return true;
			}

			case EValueKind::Vector:
			{
				// Vector-shaped properties are structs with X/Y/Z/W (or R/G/B/A) members. Component
				// count picks the naming, matching how the engine serialises FVector2D / FLinearColor.
				static const TCHAR* const XyzwNames[] = { TEXT("X"), TEXT("Y"), TEXT("Z"), TEXT("W") };
				static const TCHAR* const RgbaNames[] = { TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A") };

				if (Value.Elements.Num() < 2 || Value.Elements.Num() > 4)
				{
					Diagnostics.Error(TEXT("DFX4002"), Value.Location,
						FString::Printf(TEXT("Property '%s': a vector literal must have 2 to 4 components."), *PropertyName));
					return false;
				}

				const bool bColor = Value.Elements.Num() == 4 && PropertyName.Contains(TEXT("Color"));
				const TCHAR* const* Names = bColor ? RgbaNames : XyzwNames;

				TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
				for (int32 Index = 0; Index < Value.Elements.Num(); ++Index)
				{
					const FValuePtr& Element = Value.Elements[Index];
					if (!Element.IsValid() || Element->Kind != EValueKind::Number)
					{
						Diagnostics.Error(TEXT("DFX4005"), Value.Location,
							FString::Printf(TEXT("Property '%s': component %d is not a number."), *PropertyName, Index));
						return false;
					}
					Object->SetNumberField(Names[Index], Element->Number);
				}
				OutJson = MakeShared<FJsonValueObject>(Object);
				return true;
			}

			case EValueKind::Array:
			{
				TArray<TSharedPtr<FJsonValue>> Elements;
				for (const FValuePtr& Element : Value.Elements)
				{
					TSharedPtr<FJsonValue> ElementJson;
					if (!Element.IsValid() || !ValueToJson(*Element, DefaultRoot, PropertyName, Diagnostics, ElementJson))
					{
						return false;
					}
					Elements.Add(ElementJson);
				}
				OutJson = MakeShared<FJsonValueArray>(Elements);
				return true;
			}

			default:
				Diagnostics.Error(TEXT("DFX4005"), Value.Location,
					FString::Printf(TEXT("Property '%s': only literal values are allowed in a property block."), *PropertyName));
				return false;
			}
		}

		/**
		 * One `Settings` key, and the asset property it drives.
		 *
		 * The DSL name is not always the property name: `LocalSpace` reads better than `bLocalSpace`,
		 * and `SimTarget = CPU` reads better than `SimTarget = CPUSim`. Rather than exposing the C++
		 * spelling, the mapping is explicit and the value aliases travel with it.
		 */
		struct FSettingMapping
		{
			const TCHAR* SourceName;
			const TCHAR* PropertyName;
			/** Pairs of {written, actual}, terminated by a null. Empty when no aliasing is needed. */
			const TCHAR* const* ValueAliases;
		};

		const TCHAR* const SimTargetAliases[] = { TEXT("CPU"), TEXT("CPUSim"), TEXT("GPU"), TEXT("GPUComputeSim"), nullptr };
		const TCHAR* const AllocationAliases[] = { TEXT("Fixed"), TEXT("FixedCount"), TEXT("Automatic"), TEXT("AutomaticEstimate"), TEXT("Manual"), TEXT("ManualEstimate"), nullptr };

		const FSettingMapping SystemSettings[] =
		{
			{ TEXT("EffectType"),  TEXT("EffectType"),  nullptr },
			{ TEXT("WarmupTime"),  TEXT("WarmupTime"),  nullptr },
			{ TEXT("FixedBounds"), TEXT("FixedBounds"), nullptr },
		};

		const FSettingMapping EmitterSettings[] =
		{
			{ TEXT("SimTarget"),            TEXT("SimTarget"),            SimTargetAliases },
			{ TEXT("LocalSpace"),           TEXT("bLocalSpace"),          nullptr },
			{ TEXT("Determinism"),          TEXT("bDeterminism"),         nullptr },
			{ TEXT("RandomSeed"),           TEXT("RandomSeed"),           nullptr },
			{ TEXT("AllocationMode"),       TEXT("AllocationMode"),       AllocationAliases },
			{ TEXT("PreAllocationCount"),   TEXT("PreAllocationCount"),   nullptr },
			{ TEXT("InterpolatedSpawning"), TEXT("InterpolatedSpawnMode"), nullptr },
			{ TEXT("CalculateBoundsMode"),  TEXT("CalculateBoundsMode"),  nullptr },
			{ TEXT("FixedBounds"),          TEXT("FixedBounds"),          nullptr },
			// Niagara refuses to compile a system that reads `Particles.ID` unless this is on, so an
			// export that omitted it produced a system that could not be built at all (plan-v5 item A).
			{ TEXT("RequiresPersistentIDs"),TEXT("bRequiresPersistentIDs"), nullptr },
			{ TEXT("Enabled"),              TEXT("bIsEnabled"),           nullptr },
		};

		/** `ModulePaths` configures DreamFX itself and never reaches the asset. */
		bool IsGeneratorOnlySetting(const FString& Name)
		{
			return Name.Equals(TEXT("ModulePaths"), ESearchCase::IgnoreCase);
		}

		FString ApplyValueAlias(const TCHAR* const* Aliases, const FString& Written)
		{
			if (Aliases == nullptr)
			{
				return Written;
			}
			for (int32 Index = 0; Aliases[Index] != nullptr; Index += 2)
			{
				if (Written.Equals(Aliases[Index], ESearchCase::IgnoreCase))
				{
					return Aliases[Index + 1];
				}
			}
			return Written;
		}

		/** `box(minX, minY, minZ, maxX, maxY, maxZ)` -> the JSON shape FBox serialises to. */
		bool BoxCallToJson(const FValue& Value, const FString& PropertyName, FDiagnosticSink& Diagnostics,
			TSharedPtr<FJsonValue>& OutJson)
		{
			if (Value.Elements.Num() != 6)
			{
				Diagnostics.Error(TEXT("DFX4023"), Value.Location,
					FString::Printf(TEXT("Property '%s': box() takes 6 numbers -- minX, minY, minZ, maxX, maxY, maxZ."),
						*PropertyName));
				return false;
			}

			double Components[6] = {};
			for (int32 Index = 0; Index < 6; ++Index)
			{
				const FValuePtr& Element = Value.Elements[Index];
				if (!Element.IsValid() || Element->Kind != EValueKind::Number)
				{
					Diagnostics.Error(TEXT("DFX4023"), Value.Location,
						FString::Printf(TEXT("Property '%s': box() argument %d is not a number."), *PropertyName, Index));
					return false;
				}
				Components[Index] = Element->Number;
			}

			auto MakePoint = [](double X, double Y, double Z)
			{
				TSharedRef<FJsonObject> Point = MakeShared<FJsonObject>();
				Point->SetNumberField(TEXT("X"), X);
				Point->SetNumberField(TEXT("Y"), Y);
				Point->SetNumberField(TEXT("Z"), Z);
				return Point;
			};

			TSharedRef<FJsonObject> Box = MakeShared<FJsonObject>();
			Box->SetObjectField(TEXT("Min"), MakePoint(Components[0], Components[1], Components[2]));
			Box->SetObjectField(TEXT("Max"), MakePoint(Components[3], Components[4], Components[5]));
			// FBox is ignored entirely unless IsValid is set; a box that reads as invalid silently does
			// nothing, which is the worst possible failure mode for a bounds setting.
			Box->SetNumberField(TEXT("IsValid"), 1);
			OutJson = MakeShared<FJsonValueObject>(Box);
			return true;
		}

		FString SerializeJsonObject(const TSharedRef<FJsonObject>& Object)
		{
			FString Result;
			const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Result);
			FJsonSerializer::Serialize(Object, Writer);
			return Result;
		}

		/** Translates a `Settings = { }` block into the JSON blob the property writers take. */
		bool PlanSettings(const TArray<FPropertyEntry>& Settings, TArrayView<const FSettingMapping> Mappings,
			const FString& DefaultRoot, const TCHAR* ScopeLabel, FDiagnosticSink& Diagnostics, FString& OutJson)
		{
			bool bOk = true;
			TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();

			for (const FPropertyEntry& Setting : Settings)
			{
				if (IsGeneratorOnlySetting(Setting.Name))
				{
					continue;
				}

				const FSettingMapping* Mapping = Mappings.FindByPredicate([&Setting](const FSettingMapping& Candidate)
				{
					return Setting.Name.Equals(Candidate.SourceName, ESearchCase::IgnoreCase);
				});

				if (Mapping == nullptr)
				{
					TArray<FString> Available;
					for (const FSettingMapping& Candidate : Mappings)
					{
						Available.Add(Candidate.SourceName);
					}
					Diagnostics.Error(TEXT("DFX3020"), Setting.Location,
						FString::Printf(TEXT("Unknown %s setting '%s'. Available settings: %s"),
							ScopeLabel, *Setting.Name, *FString::Join(Available, TEXT(", "))));
					bOk = false;
					continue;
				}

				if (!Setting.Value.IsValid())
				{
					continue;
				}

				TSharedPtr<FJsonValue> Json;
				if (Setting.Value->Kind == EValueKind::Call && Setting.Value->Text.Equals(TEXT("box"), ESearchCase::IgnoreCase))
				{
					if (!BoxCallToJson(*Setting.Value, Setting.Name, Diagnostics, Json))
					{
						bOk = false;
						continue;
					}
				}
				else if (!ValueToJson(*Setting.Value, DefaultRoot, Setting.Name, Diagnostics, Json))
				{
					bOk = false;
					continue;
				}

				// Enum-shaped settings are written as friendly words; map them to the enumerator name
				// the JSON reader expects.
				if (Mapping->ValueAliases != nullptr && Json.IsValid() && Json->Type == EJson::String)
				{
					Json = MakeShared<FJsonValueString>(ApplyValueAlias(Mapping->ValueAliases, Json->AsString()));
				}

				Properties->SetField(Mapping->PropertyName, Json);
			}

			OutJson = Properties->Values.Num() > 0 ? SerializeJsonObject(Properties) : FString();
			return bOk;
		}

		/** Builds "ModuleName.InputName" for diagnostics. */
		FString DescribeInput(const FString& ModuleName, const FString& InputName)
		{
			return FString::Printf(TEXT("%s.%s"), *ModuleName, *InputName);
		}

		/** Suggests the closest schema input name when an author mistypes one. */
		FString SuggestInputName(const FModuleSchema& Schema, const FString& Written)
		{
			FString Best;
			int32 BestScore = MAX_int32;
			for (const FInputSchema& Input : Schema.Inputs)
			{
				const FString Candidate = ToInputIdentifier(Input.Name);
				if (Candidate.Equals(Written, ESearchCase::IgnoreCase))
				{
					return Candidate;
				}
				// Cheap proxy for edit distance: shared prefix length against total length. Good enough
				// to catch case slips and single-character typos without dragging in a real metric.
				int32 Shared = 0;
				while (Shared < Candidate.Len() && Shared < Written.Len()
					&& FChar::ToLower(Candidate[Shared]) == FChar::ToLower(Written[Shared]))
				{
					++Shared;
				}
				const int32 Score = Candidate.Len() + Written.Len() - 2 * Shared;
				if (Shared >= 3 && Score < BestScore)
				{
					BestScore = Score;
					Best = Candidate;
				}
			}
			return Best;
		}

		/** Everything a stack needs from outside itself to be planned. */
		struct FStackContext
		{
			FModuleLibrary* Modules = nullptr;
			FString DefaultRoot;
			/** Declared user parameters, so `= User.Foo` can be typed without loading the asset. */
			const TMap<FName, FNiagaraTypeDefinition>* UserVariableTypes = nullptr;
			/** Custom attributes already declared by an earlier assignment (L2 "first write declares"). */
			TMap<FName, FNiagaraTypeDefinition>* DeclaredAttributes = nullptr;
			/** The package this build is writing, so a reference into a *different* one can be caught. */
			FString SystemPackageName;
		};

		/**
		 * Finds a `refPath` in a data-interface blob that names a private subobject of another asset.
		 *
		 * The `:` is what distinguishes a subobject reference (`/Game/X.X:Emitter_0.Renderer_2`) from an
		 * ordinary asset reference (`/Game/X.X`). The first is legal only inside the package that owns
		 * it; written into a different package, SavePackage aborts the process. The second is fine and
		 * must keep working -- meshes, materials and textures are all referenced that way.
		 */
		bool FindForeignSubobjectReference(const FString& Json, const FString& OwnPackageName, FString& OutOffender)
		{
			int32 Cursor = 0;
			const FString Needle(TEXT("\"refPath\""));
			while (true)
			{
				const int32 Key = Json.Find(Needle, ESearchCase::IgnoreCase, ESearchDir::FromStart, Cursor);
				if (Key == INDEX_NONE)
				{
					return false;
				}

				int32 Open = Json.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, Key + Needle.Len());
				if (Open == INDEX_NONE)
				{
					return false;
				}
				++Open;
				const int32 Close = Json.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, Open);
				if (Close == INDEX_NONE)
				{
					return false;
				}

				const FString Path = Json.Mid(Open, Close - Open);
				int32 Colon = INDEX_NONE;
				if (Path.FindChar(TEXT(':'), Colon))
				{
					const FString Package = Path.Left(Path.Find(TEXT("."), ESearchCase::CaseSensitive) == INDEX_NONE
						? Path.Len()
						: Path.Find(TEXT("."), ESearchCase::CaseSensitive));
					if (!OwnPackageName.IsEmpty() && Package != OwnPackageName)
					{
						OutOffender = Path;
						return true;
					}
				}

				Cursor = Close + 1;
			}
		}

		/**
		 * Types an assignment's right-hand side.
		 *
		 * A linked reference carries no type of its own, so it has to be looked up: user parameters
		 * come from the Properties block, and attributes this file already assigned come from the
		 * running declaration map. Anything else is refused rather than guessed -- a wrong guess here
		 * produces an effect that compiles and misbehaves.
		 */
		bool ResolveAssignmentType(const FValue& Value, const FString& TargetName, const FStackContext& Context,
			FDiagnosticSink& Diagnostics, FNiagaraTypeDefinition& OutType)
		{
			if (Value.Kind == EValueKind::Name && FValueLowering::IsNamespacedName(Value.Text))
			{
				const FName SourceName(*Value.Text);

				if (Context.UserVariableTypes != nullptr)
				{
					if (const FNiagaraTypeDefinition* Found = Context.UserVariableTypes->Find(SourceName))
					{
						OutType = *Found;
						return true;
					}
				}
				if (Context.DeclaredAttributes != nullptr)
				{
					if (const FNiagaraTypeDefinition* Found = Context.DeclaredAttributes->Find(SourceName))
					{
						OutType = *Found;
						return true;
					}
				}

				Diagnostics.Error(TEXT("DFX4024"), Value.Location,
					FString::Printf(TEXT("Cannot type '%s = %s': the type of '%s' is not known here. Declare it in Properties, or assign a literal first so the type is explicit."),
						*TargetName, *Value.Text, *Value.Text));
				return false;
			}

			return FValueLowering::InferType(Value, TargetName, Diagnostics, OutType);
		}

		/**
		 * Checks a linked parameter against the input it drives.
		 *
		 * A link binds a parameter straight to an input -- there is no conversion step, so the two
		 * types must actually match. Only links whose source type DreamFX already knows (a declared
		 * user parameter, or an attribute this file assigned) can be checked; engine attributes like
		 * Particles.Velocity are taken on trust and Niagara's own compile catches those.
		 */
		bool ValidateLinkedType(const FInputValue& Value, const FNiagaraTypeDefinition& TargetType,
			const FStackContext& Context, const FString& DisplayName, const FSourceLocation& Location,
			FDiagnosticSink& Diagnostics)
		{
			if (Value.Mode != EInputValueMode::Linked)
			{
				return true;
			}

			const FName SourceName = Value.LinkedVariable.GetName();
			const FNiagaraTypeDefinition* SourceType = nullptr;
			if (Context.UserVariableTypes != nullptr)
			{
				SourceType = Context.UserVariableTypes->Find(SourceName);
			}
			if (SourceType == nullptr && Context.DeclaredAttributes != nullptr)
			{
				SourceType = Context.DeclaredAttributes->Find(SourceName);
			}
			if (SourceType == nullptr || *SourceType == TargetType)
			{
				return true;
			}

			// This was briefly a warning for same-layout types, on the theory that Niagara links by
			// reinterpreting bytes and a Vector4f fed into a LinearColor input therefore builds. It
			// does not: the translator rejects it with "变量 Pillar001.ccC 之前已被定义，但其类型不同！
			// Vector 4 != 线性颜色", and the only thing the downgrade achieved was moving the failure
			// from a diagnostic that names the source line to a Niagara compile error that does not.
			// Same-size is not same-type here, so it stays an error (plan-v5 item C).
			Diagnostics.Error(TEXT("DFX4027"), Location,
				FString::Printf(TEXT("'%s' is %s, but '%s' is %s. Linking binds a parameter directly -- there is no conversion. Declare '%s' as %s, or drive the input another way."),
					*SourceName.ToString(), *FValueLowering::DescribeType(*SourceType),
					*DisplayName, *FValueLowering::DescribeType(TargetType),
					*SourceName.ToString(), *FValueLowering::DescribeType(TargetType)));
			return false;
		}

		/**
		 * The input a written identifier means, when more than one input answers to it.
		 *
		 * plan-v5 R2. Niagara names an inline edit condition after the input it gates -- `ScaleRGB`
		 * the checkbox next to `Scale RGB` the value -- and the DSL's space-insensitive matching makes
		 * those one name. Taking the first match wrote a colour into the checkbox and failed with the
		 * engine's own "值必须是NiagaraBool".
		 *
		 * The value decides. A boolean literal means the checkbox; anything else means the value. The
		 * test is the real lowering, run into a scratch sink, so it stays right for whatever pair of
		 * types collides next; values with no lowering of their own -- a dynamic input chain, an hlsl
		 * block -- fall back to "not the bool", which is what those can never be.
		 */
		const FInputSchema* ResolveInput(const FModuleSchema& Schema, const FNamedArgument& Argument)
		{
			TArray<const FInputSchema*> Candidates;
			Schema.FindInputsByIdentifier(Argument.Name, Candidates);

			if (Candidates.Num() <= 1 || !Argument.Value.IsValid())
			{
				return Candidates.Num() > 0 ? Candidates[0] : nullptr;
			}

			for (const FInputSchema* Candidate : Candidates)
			{
				FDiagnosticSink Scratch;
				FInputValue Lowered;
				if (FValueLowering::Lower(*Argument.Value, Candidate->Type, Argument.Name, Scratch, Lowered))
				{
					return Candidate;
				}
			}

			for (const FInputSchema* Candidate : Candidates)
			{
				if (Candidate->Type != FNiagaraTypeDefinition::GetBoolDef())
				{
					return Candidate;
				}
			}

			return Candidates[0];
		}

		/**
		 * Resolves one value into the flat list of writes it needs, recursing through dynamic input
		 * chains. Nothing is written here -- this is still the plan phase.
		 */
		bool PlanInputValue(const FValue& Value, const FNiagaraTypeDefinition& TargetType,
			const TArray<FName>& Path, const FString& DisplayName, const FStackContext& Context,
			FDiagnosticSink& Diagnostics, TArray<FPlannedInput>& OutInputs, FDependencySet& OutDependencies)
		{
			auto Emit = [&OutInputs, &Path, &Value](FInputValue&& InputValue)
			{
				FPlannedInput Planned;
				Planned.Path = Path;
				Planned.Value = MoveTemp(InputValue);
				Planned.Location = Value.Location;
				OutInputs.Add(MoveTemp(Planned));
			};

			if (Value.Kind == EValueKind::Hlsl)
			{
				FString Hlsl;
				if (!FExpressions::PrepareRawBlock(Value, DisplayName, Diagnostics, Hlsl))
				{
					return false;
				}
				Emit(FInputValue::MakeHlsl(Hlsl));
				return true;
			}

			if (Value.Kind == EValueKind::Curve)
			{
				FString Json;
				if (!FExpressions::RenderCurve(Value, TargetType, DisplayName, Diagnostics, Json))
				{
					return false;
				}
				Emit(FInputValue::MakeDataInterface(TargetType.GetClass(), Json));
				return true;
			}

			// plan-v5 R4 step 3, the read side of the decompiler's verbatim data interface blob. A
			// data-interface input given a quoted JSON object is configured with it; the same input
			// given a quoted anything-else still falls through to the normal lowering, where a string
			// on a non-asset input is the DFX4001 it has always been.
			if (Value.Kind == EValueKind::String && TargetType.IsDataInterface()
				&& Value.Text.StartsWith(TEXT("{")))
			{
				TSharedPtr<FJsonObject> Parsed;
				const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Value.Text);
				if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid())
				{
					// plan-v5 R3's acceptance guard, in the one channel that can still carry such a
					// reference. A `refPath` naming a *subobject* (the `:` is what makes it one) of some
					// other asset cannot legally live in this system: SavePackage rejects a reference to
					// another package's private object with `appError`, which kills the process instead
					// of reporting anything. The decompiler no longer writes these, but a hand-written
					// or older file still can, and a crash is not an acceptable way to find out.
					FString Offender;
					if (FindForeignSubobjectReference(Value.Text, Context.SystemPackageName, Offender))
					{
						Diagnostics.Error(TEXT("DFX3010"), Value.Location,
							FString::Printf(TEXT("Input '%s' configures a data interface with a reference to '%s', which is a private subobject of another asset. Saving a system that holds one aborts the editor rather than failing, so DreamFX refuses it here. Reference an asset, not a subobject inside one."),
								*DisplayName, *Offender));
						return false;
					}

					Emit(FInputValue::MakeDataInterface(TargetType.GetClass(), Value.Text));
					return true;
				}
			}

			// L6: arithmetic and builtin calls collapse into one HLSL expression rather than growing an
			// operator-node backend.
			if (FExpressions::RequiresHlslLowering(Value))
			{
				FString Hlsl;
				if (!FExpressions::Render(Value, TargetType, DisplayName, Diagnostics, Hlsl))
				{
					return false;
				}
				Emit(FInputValue::MakeHlsl(Hlsl));
				return true;
			}

			if (Value.Kind == EValueKind::Call)
			{
				FString Error;
				UNiagaraScript* DynamicInput = Context.Modules->FindDynamicInput(Value.Text, Error);
				if (DynamicInput == nullptr)
				{
					Diagnostics.Error(TEXT("DFX3006"), Value.Location,
						FString::Printf(TEXT("'%s' is neither an allowed inline function (%s) nor a dynamic input: %s"),
							*Value.Text, *FExpressions::ListBuiltins(), *Error));
					return false;
				}
				OutDependencies.Add(DynamicInput);

				// R1b, the dynamic input half. `MakeFloatFromLinearColor@1.0` and the same call with
				// no pin are different scripts as far as their inputs are concerned: the revision
				// moved `Channel` from one enum asset to another, so `Channel = R` is valid on one and
				// meaningless on the other.
				FGuid PinnedVersion;
				if (!Value.VersionPin.IsEmpty())
				{
					const TArray<FScriptVersion> Available = FNiagaraAdapter::GetAvailableScriptVersions(DynamicInput);
					const FScriptVersion* Match = Available.FindByPredicate(
						[&Value](const FScriptVersion& Candidate) { return Candidate.ToLabel() == Value.VersionPin; });

					if (Match == nullptr)
					{
						TArray<FString> Labels;
						for (const FScriptVersion& Candidate : Available)
						{
							Labels.AddUnique(Candidate.ToLabel());
						}
						Diagnostics.Error(TEXT("DFX3009"), Value.Location,
							FString::Printf(TEXT("Dynamic input '%s' is pinned to version %s, which its asset does not offer. Available version(s): %s."),
								*Value.Text, *Value.VersionPin,
								Labels.Num() > 0 ? *FString::Join(Labels, TEXT(", ")) : TEXT("(this asset does not use versioning)")));
						return false;
					}
					PinnedVersion = Match->Guid;
				}

				// The *stack* schema, not the asset one: a static switch on the dynamic input exists
				// only on a live chain, and planning against the asset schema is what made
				// `Absolute = true` a "no input named" error (plan-v3 E4-1). TargetType is what the
				// probe plugs the dynamic input into.
				const FModuleSchema* Schema = Context.Modules->GetDynamicInputStackSchema(
					DynamicInput, TargetType, PinnedVersion, Error);
				if (Schema == nullptr)
				{
					Diagnostics.Error(TEXT("DFX3007"), Value.Location,
						FString::Printf(TEXT("Could not read the input schema of dynamic input '%s': %s"),
							*Value.Text, *Error));
					return false;
				}

				if (Value.Elements.Num() > 0)
				{
					Diagnostics.Error(TEXT("DFX2008"), Value.Elements[0]->Location,
						FString::Printf(TEXT("Dynamic input '%s' was given a positional argument. Its inputs must be written as 'Name = Value'."),
							*Value.Text));
					return false;
				}

				// The chain node itself is written before its children, because addressing a child
				// means addressing *through* the node that owns it.
				{
					FPlannedInput Planned;
					Planned.Path = Path;
					Planned.Value = FInputValue::MakeDynamicInput(DynamicInput);
					Planned.DynamicInputVersion = PinnedVersion;
					Planned.Location = Value.Location;
					OutInputs.Add(MoveTemp(Planned));
				}

				bool bOk = true;
				TSet<FName> Seen;

				// Static switches first, source order preserved within each group.
				//
				// Not a style choice: a switch reshapes which of its siblings are part of the graph at
				// all, and SetStackInputData refuses a write to an input the current switch state hides
				// ("该输入被静态开关/条件逻辑隐藏"). Writing `EvaluationType` before the `RandomnessMode`
				// that reveals it fails on a chain that was perfectly valid when it was exported.
				// Ordering here rather than in the decompiler means a hand-written file is under no
				// obligation to know this (plan-v3 E4-1).
				auto PlanArguments = [&](bool bStaticSwitchPass)
				{
					for (const FNamedArgument& Argument : Value.Arguments)
					{
						const FInputSchema* InputSchema = ResolveInput(*Schema, Argument);
						if (InputSchema == nullptr)
						{
							if (bStaticSwitchPass)
							{
								continue; // reported once, on the second pass
							}
							TArray<FString> Available;
							for (const FInputSchema& Candidate : Schema->Inputs)
							{
								Available.Add(ToInputIdentifier(Candidate.Name));
							}
							Diagnostics.Error(TEXT("DFX3008"), Argument.Location,
								FString::Printf(TEXT("Dynamic input '%s' has no input named '%s'. Available inputs: %s"),
									*Value.Text, *Argument.Name,
									Available.Num() > 0 ? *FString::Join(Available, TEXT(", ")) : TEXT("(none)")));
							bOk = false;
							continue;
						}

						if (InputSchema->bIsStaticSwitch != bStaticSwitchPass)
						{
							continue;
						}

						if (Seen.Contains(InputSchema->Name))
						{
							Diagnostics.Error(TEXT("DFX4010"), Argument.Location,
								FString::Printf(TEXT("Input '%s' is set more than once on dynamic input '%s'."),
									*Argument.Name, *Value.Text));
							bOk = false;
							continue;
						}
						Seen.Add(InputSchema->Name);

						TArray<FName> ChildPath = Path;
						ChildPath.Add(InputSchema->Name);

						const int32 FirstPlanned = OutInputs.Num();

						if (!Argument.Value.IsValid()
							|| !PlanInputValue(*Argument.Value, InputSchema->Type, ChildPath,
								FString::Printf(TEXT("%s.%s"), *Value.Text, *Argument.Name),
								Context, Diagnostics, OutInputs, OutDependencies))
						{
							bOk = false;
						}

						MarkStaticSwitch(OutInputs, FirstPlanned, InputSchema->bIsStaticSwitch);
					}
				};

				PlanArguments(/*bStaticSwitchPass=*/true);
				PlanArguments(/*bStaticSwitchPass=*/false);
				return bOk;
			}

			FInputValue Lowered;
			if (!FValueLowering::Lower(Value, TargetType, DisplayName, Diagnostics, Lowered))
			{
				return false;
			}
			if (!ValidateLinkedType(Lowered, TargetType, Context, DisplayName, Value.Location, Diagnostics))
			{
				return false;
			}
			Emit(MoveTemp(Lowered));
			return true;
		}

		bool PlanStack(const FStack& Stack, const FStackContext& Context,
			FDiagnosticSink& Diagnostics, FPlannedStack& OutStack, FDependencySet& OutDependencies)
		{
			FModuleLibrary& Modules = *Context.Modules;
			const FString& DefaultRoot = Context.DefaultRoot;

			// A merged emitter's stacks can come from a different file than the document being built,
			// and a line number reported against the wrong path is worse than no line number.
			const FString PreviousFile = Diagnostics.GetFile();
			if (!Stack.SourceFile.IsEmpty() && Stack.SourceFile != PreviousFile)
			{
				Diagnostics.SetFile(Stack.SourceFile);
			}
			ON_SCOPE_EXIT { Diagnostics.SetFile(PreviousFile); };
			OutStack.Kind = Stack.Kind;
			OutStack.Location = Stack.Location;
			OutStack.ScriptName = FNiagaraAdapter::ScriptNameForStack(Stack.Kind);

			if (OutStack.ScriptName == NAME_None)
			{
				Diagnostics.Error(TEXT("DFX5001"), Stack.Location,
					FString::Printf(TEXT("Stack '%s' has no Niagara script usage mapping."), LexStackKind(Stack.Kind)));
				return false;
			}

			bool bOk = true;

			for (const FStatement& Statement : Stack.Statements)
			{
				if (Statement.Kind == EStatementKind::Assignment)
				{
					// L2: consecutive assignments fold into one Set Parameters module, and any module
					// call between them starts a new group. Appending to the trailing planned module
					// when it is already a Set Parameters module is exactly that rule.
					const FName TargetName(*Statement.Name);

					if (!FValueLowering::IsNamespacedName(Statement.Name))
					{
						Diagnostics.Error(TEXT("DFX4025"), Statement.Location,
							FString::Printf(TEXT("'%s' is not a valid assignment target. Parameter names are namespace-qualified, e.g. Particles.MyValue or Emitter.MyCounter."),
								*Statement.Name));
						bOk = false;
						continue;
					}

					if (!Statement.Value.IsValid())
					{
						Diagnostics.Error(TEXT("DFX4005"), Statement.Location,
							FString::Printf(TEXT("'%s' has no value."), *Statement.Name));
						bOk = false;
						continue;
					}

					FNiagaraTypeDefinition TargetType;
					if (!Statement.TypeName.IsEmpty())
					{
						// An explicit type wins over both the running declaration map and inference:
						// writing it is the author saying what this attribute is.
						FParameterDecl AsDeclaration;
						AsDeclaration.TypeName = Statement.TypeName;
						AsDeclaration.InnerTypeName = Statement.InnerTypeName;
						AsDeclaration.Name = Statement.Name;
						AsDeclaration.Location = Statement.Location;

						bool bIsDataInterface = false;
						if (!FValueLowering::ResolveDeclaredType(AsDeclaration, Diagnostics, TargetType, bIsDataInterface))
						{
							bOk = false;
							continue;
						}
					}
					else if (const FNiagaraTypeDefinition* Existing = Context.DeclaredAttributes
						? Context.DeclaredAttributes->Find(TargetName) : nullptr)
					{
						TargetType = *Existing;
					}
					else if (!ResolveAssignmentType(*Statement.Value, Statement.Name, Context, Diagnostics, TargetType))
					{
						bOk = false;
						continue;
					}

					FPlannedSetParameter Parameter;
					Parameter.Name = TargetName;
					Parameter.Type = TargetType;
					Parameter.Location = Statement.Location;

					// Planned as writes addressed from the Set Parameters module: the entry itself at
					// [Name], and anything a dynamic input chain hangs below it at deeper paths.
					TArray<FPlannedInput> Writes;
					if (!PlanInputValue(*Statement.Value, TargetType, { TargetName }, Statement.Name,
						Context, Diagnostics, Writes, OutDependencies))
					{
						bOk = false;
						continue;
					}

					// The first write is the entry's own value; it may be able to ride along on the
					// module create call. Everything deeper always has to be written afterwards.
					if (Writes.Num() > 0)
					{
						Parameter.Value = Writes[0].Value;
						Parameter.DynamicInputVersion = Writes[0].DynamicInputVersion;
						for (int32 Index = 1; Index < Writes.Num(); ++Index)
						{
							Parameter.NestedInputs.Add(Writes[Index]);
						}
					}

					if (Context.DeclaredAttributes != nullptr)
					{
						// First write declares. A later write with a different inferred type is caught
						// because the declared type wins and the value is checked against it.
						Context.DeclaredAttributes->FindOrAdd(TargetName) = TargetType;
					}

					if (OutStack.Modules.Num() > 0 && OutStack.Modules.Last().bIsSetParameters)
					{
						OutStack.Modules.Last().Parameters.Add(MoveTemp(Parameter));
					}
					else
					{
						FPlannedModule SetParameters;
						SetParameters.bIsSetParameters = true;
						SetParameters.SourceName = TEXT("Set Parameters");
						SetParameters.Location = Statement.Location;
						SetParameters.Parameters.Add(MoveTemp(Parameter));
						OutStack.Modules.Add(MoveTemp(SetParameters));
					}
					continue;
				}

				FString Error;
				UNiagaraScript* ModuleAsset = Modules.FindModule(Statement.Name, Error);
				if (ModuleAsset == nullptr)
				{
					Diagnostics.Error(TEXT("DFX3001"), Statement.Location, Error);
					bOk = false;
					continue;
				}

				// R7's `@version` pin, which as of R1b selects rather than merely checks. Resolved
				// before anything is read, because the version decides what the module's inputs even
				// are -- see the long note at the FPlannedModule assignment below.
				FGuid PinnedVersion;
				if (!Statement.VersionPin.IsEmpty())
				{
					const TArray<FScriptVersion> Available = FNiagaraAdapter::GetAvailableScriptVersions(ModuleAsset);

					const FScriptVersion* Match = Available.FindByPredicate(
						[&Statement](const FScriptVersion& Candidate) { return Candidate.ToLabel() == Statement.VersionPin; });

					if (Match == nullptr)
					{
						TArray<FString> Labels;
						for (const FScriptVersion& Candidate : Available)
						{
							Labels.AddUnique(Candidate.ToLabel());
						}
						Diagnostics.Error(TEXT("DFX3009"), Statement.Location,
							FString::Printf(TEXT("'%s' is pinned to version %s, which its module asset does not offer. Available version(s): %s. Drop the '@%s' to build against the exposed version."),
								*Statement.Name, *Statement.VersionPin,
								Labels.Num() > 0 ? *FString::Join(Labels, TEXT(", ")) : TEXT("(this asset does not use versioning)"),
								*Statement.VersionPin));
						bOk = false;
						continue;
					}

					PinnedVersion = Match->Guid;
				}

				// Stack-aware, not asset-level: inline edit conditions and static switches only exist on
				// a live module, and both are things authors write every day.
				const FModuleSchema* Schema = Modules.GetStackSchema(ModuleAsset, Stack.Kind,
					TArrayView<const TPair<FName, FInputValue>>(), PinnedVersion, Error);
				if (Schema == nullptr)
				{
					Diagnostics.Error(TEXT("DFX3002"), Statement.Location,
						FString::Printf(TEXT("Could not read the input schema of module '%s': %s"), *Statement.Name, *Error));
					bOk = false;
					continue;
				}

				// plan-v5 R1. A module's input list is a function of its static switches, so the schema
				// above -- read from a module whose switches sit at their defaults -- is only the
				// signature of the *unconfigured* module. `SpriteSizeMode = Uniform` is what makes
				// `UniformSpriteSize` exist at all, and checking that argument against the default
				// configuration rejected it as a typo.
				//
				// So the switches this call sets are collected first and the schema re-read with them
				// applied. A fixed point rather than one pass, because a switch can be revealed by
				// another switch: each round can only see the switches the previous round's
				// configuration exposed. Four rounds is far past anything in the engine's own modules,
				// where the deepest nesting is two.
				TArray<TPair<FName, FInputValue>> SwitchValues;
				bool bSchemaFailed = false;
				for (int32 Round = 0; Round < 4; ++Round)
				{
					TArray<TPair<FName, FInputValue>> Found;
					for (const FNamedArgument& Argument : Statement.Arguments)
					{
						const FInputSchema* InputSchema = ResolveInput(*Schema, Argument);
						if (InputSchema == nullptr || !InputSchema->bIsStaticSwitch || !Argument.Value.IsValid())
						{
							continue;
						}

						// Lowered into a scratch sink: a switch whose value does not type-check is not
						// reported here but by the real pass below, which knows the display name and
						// would otherwise say it twice.
						FDiagnosticSink Scratch;
						FInputValue Lowered;
						if (FValueLowering::Lower(*Argument.Value, InputSchema->Type,
							DescribeInput(Statement.Name, Argument.Name), Scratch, Lowered))
						{
							Found.Emplace(InputSchema->Name, MoveTemp(Lowered));
						}
					}

					if (Found.Num() == SwitchValues.Num())
					{
						break; // nothing new became visible; this is the configuration the source describes
					}

					SwitchValues = MoveTemp(Found);

					const FModuleSchema* Configured = Modules.GetStackSchema(ModuleAsset, Stack.Kind,
						SwitchValues, PinnedVersion, Error);
					if (Configured == nullptr)
					{
						Diagnostics.Error(TEXT("DFX3002"), Statement.Location,
							FString::Printf(TEXT("Could not read the input schema of module '%s' with its static switches applied: %s"),
								*Statement.Name, *Error));
						bSchemaFailed = true;
						break;
					}
					Schema = Configured;
				}

				// Checking the rest of the arguments against a schema that is known to describe the
				// wrong configuration would bury the real failure under a list of invented typos.
				if (bSchemaFailed)
				{
					bOk = false;
					continue;
				}

				// plan-v2 W3 concluded a `@version` pin could never do more than record, because the
				// external edit API has no version surface: AddModule takes a bare asset pointer and
				// hardcodes the newest version. That is still true of the external API -- what changed
				// is that the node it creates does have one, and rebinding it afterwards is a
				// supported, exported operation (see the R1b block in the adapter header). A pinned
				// module is added, moved to the pinned version, and only then written.
				//
				// Without this, a rebuild of any real content is a rebuild against different modules:
				// the four content packs sit on module versions two and three revisions behind the
				// engine's current assets, whose inputs have since been renamed and retyped.
				FPlannedModule Planned;
				Planned.SourceName = Statement.Name;
				Planned.Asset = ModuleAsset;
				Planned.Location = Statement.Location;
				Planned.bDisabled = Statement.bDisabled;
				Planned.VersionGuid = PinnedVersion;
				OutDependencies.Add(ModuleAsset);

				TSet<FName> Seen;

				// Static switches first, source order preserved within each group -- the same rule the
				// dynamic input chains above already follow, and for the same reason: a switch decides
				// which of its siblings are part of the graph at all, and SetStackInputData refuses a
				// write to an input the current switch state hides. Before R1 the module path wrote in
				// source order, so an export whose switch happened to come after the input it reveals
				// failed at write time even once the type check passed.
				auto PlanArguments = [&](bool bStaticSwitchPass)
				{
					for (const FNamedArgument& Argument : Statement.Arguments)
					{
						const FInputSchema* InputSchema = ResolveInput(*Schema, Argument);
						if (InputSchema == nullptr)
						{
							if (bStaticSwitchPass)
							{
								continue; // reported once, on the second pass
							}

							// This is the Phase 1 acceptance case: a mistyped input name must point at
							// the exact line and column of the argument, not at the module or the file.
							const FString Suggestion = SuggestInputName(*Schema, Argument.Name);
							TArray<FString> Available;
							for (const FInputSchema& Candidate : Schema->Inputs)
							{
								Available.Add(ToInputIdentifier(Candidate.Name));
							}
							Diagnostics.Error(TEXT("DFX3003"), Argument.Location,
								FString::Printf(TEXT("Module '%s' has no input named '%s'.%s Available inputs: %s"),
									*Statement.Name, *Argument.Name,
									Suggestion.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" Did you mean '%s'?"), *Suggestion),
									Available.Num() > 0 ? *FString::Join(Available, TEXT(", ")) : TEXT("(none)")));
							bOk = false;
							continue;
						}

						if (InputSchema->bIsStaticSwitch != bStaticSwitchPass)
						{
							continue;
						}

						// Keyed on the resolved Niagara name, not the written identifier, so that
						// `LoopDuration` and `Loop Duration` are recognised as the same input.
						if (Seen.Contains(InputSchema->Name))
						{
							Diagnostics.Error(TEXT("DFX4010"), Argument.Location,
								FString::Printf(TEXT("Input '%s' is set more than once on module '%s'."),
									*Argument.Name, *Statement.Name));
							bOk = false;
							continue;
						}
						Seen.Add(InputSchema->Name);

						if (!Argument.Value.IsValid())
						{
							Diagnostics.Error(TEXT("DFX4005"), Argument.Location,
								FString::Printf(TEXT("Input '%s' has no value."), *Argument.Name));
							bOk = false;
							continue;
						}

						const int32 FirstPlanned = Planned.Inputs.Num();

						if (!PlanInputValue(*Argument.Value, InputSchema->Type, { InputSchema->Name },
							DescribeInput(Statement.Name, Argument.Name), Context, Diagnostics,
							Planned.Inputs, OutDependencies))
						{
							bOk = false;
						}

						MarkStaticSwitch(Planned.Inputs, FirstPlanned, InputSchema->bIsStaticSwitch);
					}
				};

				PlanArguments(/*bStaticSwitchPass=*/true);
				PlanArguments(/*bStaticSwitchPass=*/false);

				OutStack.Modules.Add(MoveTemp(Planned));
			}

			return bOk;
		}

		bool PlanRenderer(const FRenderer& Renderer, const FString& DefaultRoot,
			FDiagnosticSink& Diagnostics, FPlannedRenderer& OutRenderer)
		{
			OutRenderer.Location = Renderer.Location;
			OutRenderer.Class = FNiagaraAdapter::FindRendererClass(Renderer.TypeName);
			if (OutRenderer.Class == nullptr)
			{
				Diagnostics.Error(TEXT("DFX3004"), Renderer.Location,
					FString::Printf(TEXT("Unknown renderer type '%s'. Expected one of SpriteRenderer, MeshRenderer, RibbonRenderer, LightRenderer, DecalRenderer, ComponentRenderer, VolumeRenderer, or any UNiagaraRendererProperties subclass."),
						*Renderer.TypeName));
				return false;
			}

			bool bOk = true;

			for (const FRendererBinding& Binding : Renderer.Bindings)
			{
				// A dot, not a namespace from the known set. Niagara aliases an emitter's own
				// parameters under the emitter's name -- `Grid3D_Gas_CONTROLS_Emitter001.Velocity` is
				// how a renderer on that emitter names its Velocity -- and an emitter name is content,
				// so no fixed list can ever contain it. Demanding a known namespace rejected 44
				// bindings Niagara had written itself. What the check is for is the unqualified
				// `Bind SpriteSize -> SpriteSize`, and requiring a dot still catches that.
				int32 DotIndex = INDEX_NONE;
				if (!Binding.Target.FindChar(TEXT('.'), DotIndex) || DotIndex == 0)
				{
					Diagnostics.Error(TEXT("DFX4026"), Binding.Location,
						FString::Printf(TEXT("'Bind %s -> %s': the target must be a qualified parameter -- either a namespace such as Particles.SpriteSize, or an emitter's own alias such as MyEmitter.Velocity."),
							*Binding.PropertyName, *Binding.Target));
					bOk = false;
					continue;
				}

				FPlannedBinding Planned;
				Planned.PropertyName = Binding.PropertyName;
				Planned.Target = FName(*Binding.Target);
				Planned.Location = Binding.Location;
				OutRenderer.Bindings.Add(MoveTemp(Planned));
			}

			if (Renderer.MaterialParameters.Num() > 0)
			{
				Diagnostics.Error(TEXT("DFX5093"), Renderer.MaterialParameters[0].Location,
					TEXT("'MaterialParam' is reserved syntax and is not implemented in v1 (plan section 7)."));
				bOk = false;
			}

			TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
			for (const FPropertyEntry& Property : Renderer.Properties)
			{
				if (!Property.Value.IsValid())
				{
					continue;
				}
				TSharedPtr<FJsonValue> Json;
				if (!ValueToJson(*Property.Value, DefaultRoot, Property.Name, Diagnostics, Json))
				{
					bOk = false;
					continue;
				}

				// plan-v3 E4-3. `Meshes = ["/Engine/BasicShapes/Cube"]` is an array of paths in source
				// and an array of element structs in the asset. Which field inside the element takes
				// the reference is read off the class, so this stays right for renderers that do not
				// exist yet.
				if (Json.IsValid() && Json->Type == EJson::Array)
				{
					FString ReferenceField;
					FString ElementDefaultsJson;
					TArray<FString> ReferenceErrors;
					if (FNiagaraAdapter::GetArrayElementReferenceField(
						OutRenderer.Class, Property.Name, ReferenceField, ElementDefaultsJson, ReferenceErrors))
					{
						TArray<TSharedPtr<FJsonValue>> Wrapped;
						bool bAllStrings = true;
						for (const TSharedPtr<FJsonValue>& Element : Json->AsArray())
						{
							if (!Element.IsValid() || Element->Type != EJson::String)
							{
								bAllStrings = false;
								break;
							}
							const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
							Object->SetField(ReferenceField, Element);
							Wrapped.Add(MakeShared<FJsonValueObject>(Object));
						}
						if (bAllStrings && Wrapped.Num() > 0)
						{
							Json = MakeShared<FJsonValueArray>(Wrapped);
						}
					}
				}

				Properties->SetField(Property.Name, Json);
			}

			OutRenderer.PropertiesJson = Properties->Values.Num() > 0 ? SerializeJsonObject(Properties) : FString();
			return bOk;
		}

		/**
		 * Copies a referenced emitter and lays the inline block over it.
		 *
		 * Merge granularity is deliberately coarse: a declared stack replaces the base's whole stack
		 * rather than merging module by module. Per-module merging needs a stable module identity the
		 * language does not have -- two calls to the same module in one stack are indistinguishable --
		 * and guessing would silently reorder someone's effect. Settings merge per key because those
		 * are unambiguously named.
		 */
		bool MergeEmitter(const FEmitter& Base, const FEmitter& Override, FEmitter& OutMerged,
			FDiagnosticSink& Diagnostics)
		{
			OutMerged = Base;
			OutMerged.Name = Override.Name;
			OutMerged.Location = Override.Location;

			for (const FPropertyEntry& Setting : Override.Settings)
			{
				FPropertyEntry* Existing = OutMerged.Settings.FindByPredicate([&Setting](const FPropertyEntry& Candidate)
				{
					return Candidate.Name.Equals(Setting.Name, ESearchCase::IgnoreCase);
				});
				if (Existing != nullptr)
				{
					*Existing = Setting;
				}
				else
				{
					OutMerged.Settings.Add(Setting);
				}
			}

			for (const FStack& Stack : Override.Stacks)
			{
				const int32 Index = OutMerged.Stacks.IndexOfByPredicate(
					[&Stack](const FStack& Candidate) { return Candidate.Kind == Stack.Kind; });
				if (Index != INDEX_NONE)
				{
					OutMerged.Stacks[Index] = Stack;
				}
				else
				{
					OutMerged.Stacks.Add(Stack);
				}
			}

			// All or nothing for renderers: they are addressed by declaration order, so overriding one
			// of several would mean silently reindexing the rest.
			if (Override.Renderers.Num() > 0)
			{
				OutMerged.Renderers = Override.Renderers;
			}

			(void)Diagnostics;
			return true;
		}

		/** 3.4: every User.* a referenced emitter reads must be declared by the host system. */
		bool CheckReferencedUserParameters(const FEmitter& Referenced,
			const TMap<FName, FNiagaraTypeDefinition>& UserVariableTypes,
			const FSourceLocation& FromLocation, const FString& ReferencedFile, FDiagnosticSink& Diagnostics)
		{
			TSet<FName> Missing;

			TFunction<void(const FValue&)> Visit = [&Visit, &UserVariableTypes, &Missing](const FValue& Value)
			{
				if (Value.Kind == EValueKind::Name && Value.Text.StartsWith(TEXT("User."), ESearchCase::CaseSensitive))
				{
					const FName Name(*Value.Text);
					if (!UserVariableTypes.Contains(Name))
					{
						Missing.Add(Name);
					}
				}
				for (const FValuePtr& Element : Value.Elements)
				{
					if (Element.IsValid()) { Visit(*Element); }
				}
				for (const FNamedArgument& Argument : Value.Arguments)
				{
					if (Argument.Value.IsValid()) { Visit(*Argument.Value); }
				}
				if (Value.Left.IsValid())  { Visit(*Value.Left); }
				if (Value.Right.IsValid()) { Visit(*Value.Right); }
			};

			for (const FStack& Stack : Referenced.Stacks)
			{
				for (const FStatement& Statement : Stack.Statements)
				{
					for (const FNamedArgument& Argument : Statement.Arguments)
					{
						if (Argument.Value.IsValid()) { Visit(*Argument.Value); }
					}
					if (Statement.Value.IsValid()) { Visit(*Statement.Value); }
				}
			}

			if (Missing.Num() == 0)
			{
				return true;
			}

			TArray<FString> Names;
			for (FName Name : Missing)
			{
				FString Bare = Name.ToString();
				Bare.RemoveFromStart(TEXT("User."), ESearchCase::CaseSensitive);
				Names.Add(Bare);
			}
			Names.Sort();

			Diagnostics.Error(TEXT("DFX3043"), FromLocation,
				FString::Printf(TEXT("'%s' reads user parameters this system does not declare: %s. Add them to the Properties block."),
					*FPaths::GetCleanFilename(ReferencedFile), *FString::Join(Names, TEXT(", "))));
			return false;
		}

		bool BuildPlan(const FDocument& Document, FModuleLibrary& Modules,
			FDiagnosticSink& Diagnostics, FPlan& OutPlan)
		{
			FString MountPoint;
			FString Error;
			if (!FDreamFXPaths::ResolveRootMountPoint(Document.Root, MountPoint, Error))
			{
				Diagnostics.Error(TEXT("DFX3000"), Document.HeaderLocation, Error);
				return false;
			}

			FString RelativeName = Document.Name;
			RelativeName.RemoveFromStart(TEXT("/"));
			OutPlan.FullAssetPath = MountPoint / RelativeName;
			FDreamFXPaths::SplitPackagePath(OutPlan.FullAssetPath, OutPlan.PackagePath, OutPlan.AssetName);

			if (OutPlan.AssetName.IsEmpty())
			{
				Diagnostics.Error(TEXT("DFX3000"), Document.HeaderLocation,
					FString::Printf(TEXT("Name=\"%s\" does not end in an asset name."), *Document.Name));
				return false;
			}

			bool bOk = true;

			if (!PlanSettings(Document.Settings, SystemSettings, Document.Root, TEXT("system"),
				Diagnostics, OutPlan.SystemPropertiesJson))
			{
				bOk = false;
			}

			// User parameters are planned before any stack, because a stack assignment reading
			// `User.Foo` needs its declared type to be resolvable.
			TMap<FName, FNiagaraTypeDefinition> UserVariableTypes;
			TSet<FName> DeclaredUserNames;
			bool bWarnedAboutMetadata = false;

			for (const FParameterDecl& Declaration : Document.Parameters)
			{
				FNiagaraTypeDefinition Type;
				bool bIsDataInterface = false;
				if (!FValueLowering::ResolveDeclaredType(Declaration, Diagnostics, Type, bIsDataInterface))
				{
					bOk = false;
					continue;
				}

				const FName QualifiedName(*FString::Printf(TEXT("User.%s"), *Declaration.Name));
				if (DeclaredUserNames.Contains(QualifiedName))
				{
					Diagnostics.Error(TEXT("DFX3021"), Declaration.Location,
						FString::Printf(TEXT("User parameter '%s' is declared more than once."), *Declaration.Name));
					bOk = false;
					continue;
				}
				DeclaredUserNames.Add(QualifiedName);

				FPlannedUserVariable Planned;
				// AddUserVariable takes the bare name and stores it qualified; the qualified form is
				// what a stack assignment writes and what a read-back reports, so both are kept.
				Planned.Name = FName(*Declaration.Name);
				Planned.Type = Type;
				Planned.Location = Declaration.Location;

				if (const FAttribute* Description = Declaration.FindAttribute(TEXT("Description")))
				{
					if (Description->Value.IsValid())
					{
						Planned.Description = Description->Value->Text;
					}
				}

				if (Declaration.DefaultValue.IsValid())
				{
					if (bIsDataInterface)
					{
						// Plan 3.5: v1 declares data interface parameters and lets a blueprint or
						// component feed them. Configuring the DI's own properties from text is
						// explicitly out of scope, so a default here would be quietly ignored.
						Diagnostics.Warning(TEXT("DFX5098"), Declaration.Location,
							FString::Printf(TEXT("Data interface parameter '%s' has a default value, which v1 does not apply. Feed it at runtime instead."),
								*Declaration.Name));
					}
					else if (!FValueLowering::Lower(*Declaration.DefaultValue, Type,
						FString::Printf(TEXT("User.%s"), *Declaration.Name), Diagnostics, Planned.DefaultValue))
					{
						bOk = false;
						continue;
					}
				}

				// Group / SortPriority have nowhere to go: FNiagaraExt_UserVariable carries only Name,
				// Type, DefaultValue and Description. Documented as the 2.5 fallback -- they stay in
				// text so the source keeps its intent, but they do not reach the asset.
				if (!bWarnedAboutMetadata
					&& (Declaration.HasAttribute(TEXT("Group")) || Declaration.HasAttribute(TEXT("SortPriority"))))
				{
					Diagnostics.Info(TEXT("DFX5099"), Declaration.Location,
						TEXT("[Group] and [SortPriority] are kept in source only: the external edit API's user variable struct has no metadata fields to write them to."));
					bWarnedAboutMetadata = true;
				}

				UserVariableTypes.Add(QualifiedName, Type);
				OutPlan.UserVariables.Add(MoveTemp(Planned));
			}

			TMap<FName, FNiagaraTypeDefinition> DeclaredAttributes;
			FStackContext StackContext;
			StackContext.Modules = &Modules;
			StackContext.DefaultRoot = Document.Root;
			StackContext.UserVariableTypes = &UserVariableTypes;
			StackContext.DeclaredAttributes = &DeclaredAttributes;
			StackContext.SystemPackageName = OutPlan.PackagePath / OutPlan.AssetName;

			for (const FStack& Stack : Document.Stacks)
			{
				FPlannedStack Planned;
				if (!PlanStack(Stack, StackContext, Diagnostics, Planned, OutPlan.Dependencies))
				{
					bOk = false;
				}
				OutPlan.SystemStacks.Add(MoveTemp(Planned));
			}

			TSet<FName> EmitterNames;
			for (const FEmitter& Emitter : Document.Emitters)
			{
				const FName EmitterName(*Emitter.Name);
				if (EmitterNames.Contains(EmitterName))
				{
					Diagnostics.Error(TEXT("DFX3005"), Emitter.Location,
						FString::Printf(TEXT("Emitter '%s' is declared more than once. Emitter names are stable keys and must be unique."),
							*Emitter.Name));
					bOk = false;
					continue;
				}
				EmitterNames.Add(EmitterName);

				// `from "..."` is copy, not inheritance (R3). The referenced .dfe supplies a base and
				// the inline block overrides it; nothing links the two afterwards, which is why the
				// decompiler always emits self-contained emitters.
				FEmitter Merged;
				const FEmitter* Source = &Emitter;
				if (!Emitter.FromPath.IsEmpty())
				{
					FString ReferencedFile;
					FString ReferenceError;
					if (!FDreamFXPaths::ResolveSourceReference(Emitter.FromPath, Document.SourceFilePath,
						TEXT(".dfe"), ReferencedFile, ReferenceError))
					{
						Diagnostics.Error(TEXT("DFX3040"), Emitter.FromLocation, ReferenceError);
						bOk = false;
						continue;
					}

					FDocument Referenced;
					FDiagnosticSink ReferencedDiagnostics;
					if (!FParser::ParseFile(ReferencedFile, Referenced, ReferencedDiagnostics))
					{
						// The referenced file's own diagnostics keep their own file attribution, so the
						// author is pointed at the line in the .dfe rather than at the `from`.
						Diagnostics.Append(ReferencedDiagnostics);
						Diagnostics.SetFile(Document.SourceFilePath);
						Diagnostics.Error(TEXT("DFX3041"), Emitter.FromLocation,
							FString::Printf(TEXT("'%s' could not be parsed; see the errors above."), *ReferencedFile));
						bOk = false;
						continue;
					}
					Diagnostics.Append(ReferencedDiagnostics);
					Diagnostics.SetFile(Document.SourceFilePath);

					if (Referenced.Kind != EDocumentKind::Emitter)
					{
						Diagnostics.Error(TEXT("DFX3042"), Emitter.FromLocation,
							FString::Printf(TEXT("'%s' declares a %s, but 'from' needs an Emitter document."),
								*ReferencedFile, LexDocumentKind(Referenced.Kind)));
						bOk = false;
						continue;
					}

					if (!MergeEmitter(Referenced.EmitterDefinition, Emitter, Merged, Diagnostics))
					{
						bOk = false;
						continue;
					}
					Source = &Merged;

					// 3.4: a .dfe may read User.*, and only the host declares those. Checking here means
					// the error points at the `from` line, which is where the fix belongs.
					if (!CheckReferencedUserParameters(Referenced.EmitterDefinition, UserVariableTypes,
						Emitter.FromLocation, ReferencedFile, Diagnostics))
					{
						bOk = false;
						continue;
					}
				}

				FPlannedEmitter Planned;
				Planned.Name = EmitterName;
				Planned.Location = Emitter.Location;

				if (!PlanSettings(Source->Settings, EmitterSettings, Document.Root, TEXT("emitter"),
					Diagnostics, Planned.PropertiesJson))
				{
					bOk = false;
				}

				// Attribute declarations are per emitter: Particles.* on one emitter is a different
				// parameter from the same spelling on another.
				TMap<FName, FNiagaraTypeDefinition> EmitterAttributes;
				FStackContext EmitterContext = StackContext;
				EmitterContext.DeclaredAttributes = &EmitterAttributes;

				for (const FStack& Stack : Source->Stacks)
				{
					if (Stack.Kind == EStackKind::SimulationStage || Stack.Kind == EStackKind::EventHandler)
					{
						// The parser already reported these as reserved; do not double-report.
						continue;
					}
					FPlannedStack PlannedStack;
					if (!PlanStack(Stack, EmitterContext, Diagnostics, PlannedStack, OutPlan.Dependencies))
					{
						bOk = false;
					}
					Planned.Stacks.Add(MoveTemp(PlannedStack));
				}

				for (const FRenderer& Renderer : Source->Renderers)
				{
					FPlannedRenderer PlannedRenderer;
					if (!PlanRenderer(Renderer, Document.Root, Diagnostics, PlannedRenderer))
					{
						bOk = false;
						continue;
					}
					Planned.Renderers.Add(MoveTemp(PlannedRenderer));
				}

				OutPlan.Emitters.Add(MoveTemp(Planned));
			}

			if (OutPlan.Emitters.Num() == 0 && bOk)
			{
				Diagnostics.Warning(TEXT("DFX5002"), Document.HeaderLocation,
					TEXT("This system declares no emitters, so it will produce nothing."));
			}

			return bOk;
		}

		void ReportAdapterErrors(const TArray<FString>& Errors, const FString& Code,
			const FSourceLocation& Location, FDiagnosticSink& Diagnostics)
		{
			for (const FString& Error : Errors)
			{
				Diagnostics.Error(Code, Location, Error);
			}
		}

		/** Removes every module from one script stack, so it can be rebuilt from the source order. */
		bool ClearStack(const FStackAddress& OwnerAddress, FName ScriptName, FDiagnosticSink& Diagnostics,
			const FSourceLocation& Location)
		{
			TArray<FString> Errors;
			FScriptStackInfo Info;
			if (!FNiagaraAdapter::GetScriptStackInfo(OwnerAddress.WithScript(ScriptName), Info, Errors))
			{
				ReportAdapterErrors(Errors, TEXT("DFX5010"), Location, Diagnostics);
				return false;
			}

			bool bOk = true;
			// Remove back to front: RemoveModule reindexes the stack, and walking forward while
			// mutating would skip every other module.
			for (int32 Index = Info.Modules.Num() - 1; Index >= 0; --Index)
			{
				Errors.Reset();
				const FStackAddress ModuleAddress = OwnerAddress
					.WithScript(ScriptName)
					.WithModule(Info.Modules[Index].ModuleName);
				if (!FNiagaraAdapter::RemoveModule(ModuleAddress, Errors))
				{
					ReportAdapterErrors(Errors, TEXT("DFX5011"), Location, Diagnostics);
					bOk = false;
				}
			}
			return bOk;
		}

		/** Empties an existing emitter so it can be rebuilt in place, keeping its handle GUID. */
		bool ClearEmitter(const FStackAddress& EmitterAddress, FDiagnosticSink& Diagnostics,
			const FSourceLocation& Location)
		{
			TArray<FString> Errors;
			FEmitterInfo Info;
			if (!FNiagaraAdapter::GetEmitterInfo(EmitterAddress, Info, Errors))
			{
				ReportAdapterErrors(Errors, TEXT("DFX5010"), Location, Diagnostics);
				return false;
			}

			bool bOk = true;

			for (const FScriptStackInfo& Stack : Info.Stacks)
			{
				bOk &= ClearStack(EmitterAddress, Stack.ScriptName, Diagnostics, Location);
			}

			for (int32 Index = Info.Renderers.Num() - 1; Index >= 0; --Index)
			{
				Errors.Reset();
				if (!FNiagaraAdapter::RemoveRenderer(EmitterAddress.WithRenderer(Info.Renderers[Index].Index), Errors))
				{
					ReportAdapterErrors(Errors, TEXT("DFX5012"), Location, Diagnostics);
					bOk = false;
				}
			}

			return bOk;
		}

		/**
		 * One planned write, taking the dynamic-input version route when the plan asked for one.
		 *
		 * A dynamic input is written by handing the adapter the script; the node the engine creates
		 * for it lands on the newest version, so a pinned chain has to be rebound immediately after
		 * (R1b). Every other value mode is an ordinary write.
		 */
		bool ApplyPlannedInput(const FStackAddress& InputAddress, const FPlannedInput& Input, TArray<FString>& OutErrors)
		{
			// Writing a static switch changes which other inputs are visible, so the shared edit
			// context stops describing this module the moment it lands. The adapter cannot tell -- a
			// switch write looks like any other literal from there -- so the epoch is ended here, where
			// the module schema said so at plan time. Nothing happens outside a write scope.
			ON_SCOPE_EXIT
			{
				if (Input.bIsStaticSwitch)
				{
					FNiagaraAdapter::EndStructuralEpoch(InputAddress.System);
				}
			};

			if (Input.Value.Mode == EInputValueMode::DynamicInput && Input.DynamicInputVersion.IsValid())
			{
				return FNiagaraAdapter::SetDynamicInputAtVersion(
					InputAddress, Input.Value.DynamicInputAsset, Input.DynamicInputVersion, OutErrors);
			}
			return FNiagaraAdapter::SetInput(InputAddress, Input.Value, OutErrors);
		}

		bool ApplyStack(const FStackAddress& OwnerAddress, const FPlannedStack& Stack,
			FDiagnosticSink& Diagnostics, TMap<FName, FSourceLocation>& OutModuleLocations)
		{
			bool bOk = true;
			FName PreviousModule = NAME_None;

			for (const FPlannedModule& Module : Stack.Modules)
			{
				// Between modules, not inside one: the view-model debris each adapter call leaves
				// behind is what grows the process, and one module is a small enough step to hold a
				// ceiling without collecting while a write is half done.
				FNiagaraAdapter::CollectIfHeavy();

				TArray<FString> Errors;
				// AddModule inserts after ModuleName when one is given, and appends otherwise. Chaining
				// each new module after the previous one makes declaration order the stack order without
				// depending on what an unset target index means.
				FStackAddress StackAddress = OwnerAddress.WithScript(Stack.ScriptName);
				if (PreviousModule != NAME_None)
				{
					StackAddress = StackAddress.WithModule(PreviousModule);
				}

				FName AddedName;

				if (Module.bIsSetParameters)
				{
					TArray<TTuple<FName, FNiagaraTypeDefinition, FInputValue>> Entries;
					Entries.Reserve(Module.Parameters.Num());
					for (const FPlannedSetParameter& Parameter : Module.Parameters)
					{
						Entries.Emplace(Parameter.Name, Parameter.Type, Parameter.Value);
					}

					if (!FNiagaraAdapter::AddSetParametersModule(StackAddress, Entries, AddedName, Errors))
					{
						ReportAdapterErrors(Errors, TEXT("DFX5024"), Module.Location, Diagnostics);
						bOk = false;
						continue;
					}
					PreviousModule = AddedName;
					OutModuleLocations.Add(AddedName, Module.Location);

					// Only literals and enums ride along on the create call; every other value mode has
					// to be written afterwards, addressing the entry as an input on the new module.
					const FStackAddress ModuleAddress = OwnerAddress.WithScript(Stack.ScriptName).WithModule(AddedName);
					for (const FPlannedSetParameter& Parameter : Module.Parameters)
					{
						if (Parameter.Value.Mode != EInputValueMode::Literal && Parameter.Value.Mode != EInputValueMode::Enum)
						{
							FPlannedInput AsInput;
							AsInput.Value = Parameter.Value;
							AsInput.DynamicInputVersion = Parameter.DynamicInputVersion;
							AsInput.Location = Parameter.Location;

							Errors.Reset();
							if (!ApplyPlannedInput(ModuleAddress.WithInput(Parameter.Name), AsInput, Errors))
							{
								ReportAdapterErrors(Errors, TEXT("DFX5025"), Parameter.Location, Diagnostics);
								bOk = false;
								continue;
							}
						}

						for (const FPlannedInput& Nested : Parameter.NestedInputs)
						{
							Errors.Reset();
							if (!ApplyPlannedInput(ModuleAddress.WithInputPath(Nested.Path), Nested, Errors))
							{
								ReportAdapterErrors(Errors, TEXT("DFX5025"), Nested.Location, Diagnostics);
								bOk = false;
							}
						}
					}
					continue;
				}

				if (!FNiagaraAdapter::AddModule(StackAddress, Module.Asset, AddedName, Errors))
				{
					ReportAdapterErrors(Errors, TEXT("DFX5020"), Module.Location, Diagnostics);
					bOk = false;
					continue;
				}
				PreviousModule = AddedName;
				OutModuleLocations.Add(AddedName, Module.Location);

				const FStackAddress ModuleAddress = OwnerAddress.WithScript(Stack.ScriptName).WithModule(AddedName);

				// R1b: before any input is written, because the version decides which inputs exist.
				// AddModule always lands on the asset's newest version, so a module the source pinned
				// to an older one is the wrong module until this runs.
				if (Module.VersionGuid.IsValid())
				{
					Errors.Reset();
					if (!FNiagaraAdapter::SetModuleScriptVersion(ModuleAddress, Module.VersionGuid, Errors))
					{
						ReportAdapterErrors(Errors, TEXT("DFX5022"), Module.Location, Diagnostics);
						bOk = false;
						continue;
					}
				}

				for (const FPlannedInput& Input : Module.Inputs)
				{
					Errors.Reset();
					if (!ApplyPlannedInput(ModuleAddress.WithInputPath(Input.Path), Input, Errors))
					{
						ReportAdapterErrors(Errors, TEXT("DFX5021"), Input.Location, Diagnostics);
						bOk = false;
					}
				}

				// Disabling comes last, after every input is written: the inputs are what makes a parked
				// module worth keeping, and Niagara has no reason to preserve them on a module that was
				// switched off before they arrived.
				if (Module.bDisabled)
				{
					Errors.Reset();
					if (!FNiagaraAdapter::SetModuleEnabled(ModuleAddress, /*bEnabled=*/false, Errors))
					{
						ReportAdapterErrors(Errors, TEXT("DFX5029"), Module.Location, Diagnostics);
						bOk = false;
					}
				}
			}

			return bOk;
		}

		bool ApplyPlan(UNiagaraSystem* System, const FPlan& Plan, FDiagnosticSink& Diagnostics,
			TMap<FName, FSourceLocation>& OutModuleLocations)
		{
			// plan-v6 P1. Every write below shares one edit context until something changes the shape
			// of the stack, at which point the next call builds a fresh one. Without this each write
			// built an entire system view model of its own, which made the cost of applying a plan
			// quadratic in its size -- the largest system in this project spent minutes on it.
			FNiagaraAdapter::FWriteScope WriteScope(System);

			const FStackAddress SystemAddress(System);
			TArray<FString> Errors;

			TArray<FName> ExistingEmitters;
			if (!FNiagaraAdapter::GetEmitterNames(System, ExistingEmitters, Errors))
			{
				ReportAdapterErrors(Errors, TEXT("DFX5013"), FSourceLocation(), Diagnostics);
				return false;
			}

			TSet<FName> DeclaredEmitters;
			for (const FPlannedEmitter& Emitter : Plan.Emitters)
			{
				DeclaredEmitters.Add(Emitter.Name);
			}

			// --- user parameters -------------------------------------------------------------
			// 4.5's identity contract: user variables are keyed by name so blueprint
			// SetNiagaraVariable* calls survive a rebuild. Existing ones are therefore only removed
			// when the source stopped declaring them; the rest are re-added, which updates in place.
			TArray<FUserVariableInfo> ExistingUserVariables;
			Errors.Reset();
			if (!FNiagaraAdapter::GetUserVariables(System, ExistingUserVariables, Errors))
			{
				ReportAdapterErrors(Errors, TEXT("DFX5016"), FSourceLocation(), Diagnostics);
				return false;
			}

			TSet<FName> DeclaredUserNames;
			for (const FPlannedUserVariable& Variable : Plan.UserVariables)
			{
				// Read-back reports the qualified form even though the write takes the bare name.
				DeclaredUserNames.Add(FName(*FString::Printf(TEXT("User.%s"), *Variable.Name.ToString())));
			}

			for (const FUserVariableInfo& Existing : ExistingUserVariables)
			{
				if (DeclaredUserNames.Contains(Existing.Name))
				{
					continue;
				}
				Errors.Reset();
				if (!FNiagaraAdapter::RemoveUserVariable(System, Existing.Name, Existing.Type, Errors))
				{
					ReportAdapterErrors(Errors, TEXT("DFX5017"), FSourceLocation(), Diagnostics);
					return false;
				}
			}

			for (const FPlannedUserVariable& Variable : Plan.UserVariables)
			{
				Errors.Reset();
				if (!FNiagaraAdapter::AddUserVariable(System, Variable.Name, Variable.Type,
					Variable.Description, Variable.DefaultValue, Errors))
				{
					ReportAdapterErrors(Errors, TEXT("DFX5018"), Variable.Location, Diagnostics);
					return false;
				}
			}

			if (!Plan.SystemPropertiesJson.IsEmpty())
			{
				Errors.Reset();
				if (!FNiagaraAdapter::SetSystemProperties(System, Plan.SystemPropertiesJson, Errors))
				{
					ReportAdapterErrors(Errors, TEXT("DFX5019"), FSourceLocation(), Diagnostics);
					return false;
				}
			}

			// Emitters that survive are cleared, not removed and re-added: reusing the handle preserves
			// its GUID, which is what keeps cook diffs and external references stable (plan 4.5).
			for (FName Existing : ExistingEmitters)
			{
				if (DeclaredEmitters.Contains(Existing))
				{
					if (!ClearEmitter(SystemAddress.WithEmitter(Existing), Diagnostics, FSourceLocation()))
					{
						return false;
					}
				}
				else
				{
					Errors.Reset();
					if (!FNiagaraAdapter::RemoveEmitter(SystemAddress.WithEmitter(Existing), Errors))
					{
						ReportAdapterErrors(Errors, TEXT("DFX5014"), FSourceLocation(), Diagnostics);
						return false;
					}
				}
			}

			// System-scope stacks live on the system itself, addressed with no emitter name. Unlike
			// emitters they always exist and are never added or removed -- only their contents change.
			//
			// Only *declared* stacks are cleared. A brand-new system arrives from CreateNiagaraSystem
			// with SystemState already in SystemUpdate; blanket-clearing would strip it and leave a
			// system that never runs, for every .dfs that does not spell the module out. Declaring a
			// stack is therefore the act of taking ownership of it. An undeclared stack that still holds
			// modules is reported, so the divergence is visible rather than silent.
			for (EStackKind Kind : { EStackKind::SystemSpawn, EStackKind::SystemUpdate })
			{
				const FName ScriptName = FNiagaraAdapter::ScriptNameForStack(Kind);
				const bool bDeclared = Plan.SystemStacks.ContainsByPredicate(
					[ScriptName](const FPlannedStack& Stack) { return Stack.ScriptName == ScriptName; });

				if (bDeclared)
				{
					if (!ClearStack(SystemAddress, ScriptName, Diagnostics, FSourceLocation()))
					{
						return false;
					}
					continue;
				}

				FScriptStackInfo Existing;
				Errors.Reset();
				if (FNiagaraAdapter::GetScriptStackInfo(SystemAddress.WithScript(ScriptName), Existing, Errors)
					&& Existing.Modules.Num() > 0)
				{
					TArray<FString> ModuleNames;
					for (const FModuleInfo& Module : Existing.Modules)
					{
						ModuleNames.Add(Module.ModuleName.ToString());
					}
					Diagnostics.Info(TEXT("DFX5003"), FSourceLocation(),
						FString::Printf(TEXT("'%s' is not declared in this source, so its existing modules are left as-is: %s"),
							LexStackKind(Kind), *FString::Join(ModuleNames, TEXT(", "))));
				}
			}

			for (const FPlannedStack& Stack : Plan.SystemStacks)
			{
				if (!ApplyStack(SystemAddress, Stack, Diagnostics, OutModuleLocations))
				{
					return false;
				}
			}

			for (const FPlannedEmitter& Emitter : Plan.Emitters)
			{
				if (!ExistingEmitters.Contains(Emitter.Name))
				{
					Errors.Reset();
					if (!FNiagaraAdapter::AddEmitter(System, Emitter.Name, Errors))
					{
						ReportAdapterErrors(Errors, TEXT("DFX5015"), Emitter.Location, Diagnostics);
						return false;
					}
				}

				const FStackAddress EmitterAddress = SystemAddress.WithEmitter(Emitter.Name);

				// Emitter settings go on before the stacks: SimTarget in particular changes which
				// modules and data interfaces are legal, so a GPU emitter must know it is one first.
				if (!Emitter.PropertiesJson.IsEmpty())
				{
					Errors.Reset();
					if (!FNiagaraAdapter::SetEmitterProperties(EmitterAddress, Emitter.PropertiesJson, Errors))
					{
						ReportAdapterErrors(Errors, TEXT("DFX5026"), Emitter.Location, Diagnostics);
						return false;
					}
				}

				for (const FPlannedStack& Stack : Emitter.Stacks)
				{
					if (!ApplyStack(EmitterAddress, Stack, Diagnostics, OutModuleLocations))
					{
						return false;
					}
				}

				for (const FPlannedRenderer& Renderer : Emitter.Renderers)
				{
					Errors.Reset();
					int32 RendererIndex = INDEX_NONE;
					if (!FNiagaraAdapter::AddRenderer(EmitterAddress, Renderer.Class, RendererIndex, Errors))
					{
						ReportAdapterErrors(Errors, TEXT("DFX5022"), Renderer.Location, Diagnostics);
						return false;
					}

					const FStackAddress RendererAddress = EmitterAddress.WithRenderer(RendererIndex);

					if (!Renderer.PropertiesJson.IsEmpty())
					{
						Errors.Reset();
						if (!FNiagaraAdapter::SetRendererProperties(RendererAddress, Renderer.PropertiesJson, Errors))
						{
							ReportAdapterErrors(Errors, TEXT("DFX5023"), Renderer.Location, Diagnostics);
							return false;
						}
					}

					// A renderer with no material draws nothing and reports nothing, so this cannot be
					// left to chance -- see EnsureRendererMaterial.
					{
						Errors.Reset();
						FString AppliedMaterial;
						bool bStillMissing = false;
						if (!FNiagaraAdapter::EnsureRendererMaterial(RendererAddress, AppliedMaterial, bStillMissing, Errors))
						{
							ReportAdapterErrors(Errors, TEXT("DFX5028"), Renderer.Location, Diagnostics);
							return false;
						}
						if (!AppliedMaterial.IsEmpty())
						{
							Diagnostics.Info(TEXT("DFX5004"), Renderer.Location,
								FString::Printf(TEXT("No Material was set, so the engine default was applied: %s. Write 'Material = \"...\";' to choose your own."),
									*AppliedMaterial));
						}
						else if (bStillMissing)
						{
							Diagnostics.Warning(TEXT("DFX7104"), Renderer.Location,
								TEXT("This renderer has no Material and DreamFX knows no default for its type, so it will not draw. Set 'Material = \"...\";'."));
						}
					}

					// Bindings come after the property blob: SourceMode is a plain property, and it
					// decides how a binding resolves.
					for (const FPlannedBinding& Binding : Renderer.Bindings)
					{
						Errors.Reset();
						if (!FNiagaraAdapter::SetRendererBinding(RendererAddress, Binding.PropertyName,
							Binding.Target, Errors))
						{
							ReportAdapterErrors(Errors, TEXT("DFX5027"), Binding.Location, Diagnostics);
							return false;
						}
					}
				}
			}

			return true;
		}

		/**
		 * Maps Niagara's own diagnostics back onto source positions.
		 *
		 * Compile events only carry emitter + script, so they land on the stack's opening line. Stack
		 * issues also carry a module name, which resolves to the exact statement.
		 */
		void ReportNiagaraDiagnostics(UNiagaraSystem* System, const FCompileStateInfo& CompileState,
			const FPlan& Plan, const TMap<FName, FSourceLocation>& ModuleLocations, FDiagnosticSink& Diagnostics)
		{
			auto FindStackLocation = [&Plan](FName EmitterName, FName ScriptName) -> FSourceLocation
			{
				const TArray<FPlannedStack>* Stacks = &Plan.SystemStacks;
				if (EmitterName != NAME_None)
				{
					const FPlannedEmitter* Emitter = Plan.Emitters.FindByPredicate(
						[EmitterName](const FPlannedEmitter& Candidate) { return Candidate.Name == EmitterName; });
					if (Emitter == nullptr)
					{
						return FSourceLocation();
					}
					Stacks = &Emitter->Stacks;
				}
				const FPlannedStack* Stack = Stacks->FindByPredicate(
					[ScriptName](const FPlannedStack& Candidate) { return Candidate.ScriptName == ScriptName; });
				return Stack ? Stack->Location : FSourceLocation();
			};

			for (const FCompileEventInfo& Event : CompileState.Events)
			{
				if (Event.Severity < 2)
				{
					continue; // Log / Display are noise in a build report.
				}

				const FSourceLocation Location = FindStackLocation(Event.EmitterName, Event.ScriptName);
				const FString Where = Event.EmitterName != NAME_None
					? FString::Printf(TEXT("%s / %s"), *Event.EmitterName.ToString(), *Event.ScriptName.ToString())
					: Event.ScriptName.ToString();
				const FString Message = FString::Printf(TEXT("Niagara compile (%s): %s%s"),
					*Where, *Event.Message,
					Event.bFromDependency ? TEXT(" [raised by a dependency script]") : TEXT(""));

				if (Event.Severity >= 3)
				{
					Diagnostics.Error(TEXT("DFX6001"), Location, Message);
				}
				else
				{
					Diagnostics.Warning(TEXT("DFX6002"), Location, Message);
				}
			}

			// Stack issues are an editor-only extra: reading them needs a Slate-backed view model, so a
			// headless build reports compile events alone. Those carry the errors that gate CI; stack
			// issues mostly add "this input is unset" style hints.
			if (!FNiagaraAdapter::IsStackIssueReadingAvailable())
			{
				return;
			}

			TArray<FStackIssueInfo> Issues;
			TArray<FString> Errors;
			FNiagaraAdapter::GetStackIssues(System, Issues, Errors);

			for (const FStackIssueInfo& Issue : Issues)
			{
				if (Issue.Severity > 1)
				{
					continue; // Info-level stack notes are not build output.
				}

				FSourceLocation Location;
				if (const FSourceLocation* ModuleLocation = ModuleLocations.Find(Issue.ModuleName))
				{
					Location = *ModuleLocation;
				}
				else
				{
					Location = FindStackLocation(Issue.EmitterName, Issue.ScriptName);
				}

				const FString Message = FString::Printf(TEXT("Niagara stack issue at %s: %s%s%s"),
					Issue.DisplayPath.IsEmpty() ? TEXT("<system>") : *Issue.DisplayPath,
					*Issue.ShortDescription,
					Issue.LongDescription.IsEmpty() ? TEXT("") : TEXT(" -- "),
					*Issue.LongDescription);

				if (Issue.Severity == 0)
				{
					Diagnostics.Error(TEXT("DFX6003"), Location, Message);
				}
				else
				{
					Diagnostics.Warning(TEXT("DFX6004"), Location, Message);
				}
			}
		}
	}

	namespace
	{
		/**
		 * plan-v4 V1-4. A source under the decompiled output directory may only build inside the
		 * `Decompiled/` namespace.
		 *
		 * This is what replaced excluding that whole tree from discovery. The exclusion was safe and
		 * silent in the worst way: editing an export and saving did nothing, reported nothing, and
		 * looked exactly like a broken watcher. Now the tree is ordinary source, and the one thing
		 * that must stay impossible -- an export overwriting the third-party asset it was read from --
		 * is impossible because of where its `Name=` points, checked here.
		 *
		 * It fires on exports written before plan-v4, whose `Name=` still names the original. Refusing
		 * them is the migration: re-export, and the file that replaces it is already correct.
		 */
		bool GuardDecompiledNamespace(const FDocument& Document, FDiagnosticSink& Diagnostics)
		{
			if (Document.SourceFilePath.IsEmpty() || !FDreamFXPaths::IsDecompiledExport(Document.SourceFilePath))
			{
				return true;
			}

			FString MountPoint;
			FString MountError;
			if (!FDreamFXPaths::ResolveRootMountPoint(Document.Root, MountPoint, MountError))
			{
				return true; // BuildPlan reports the unresolvable root itself, with the better message
			}

			FString Relative = Document.Name;
			Relative.RemoveFromStart(TEXT("/"));

			if (FDreamFXPaths::IsDecompiledNamespaceAsset(MountPoint / Relative))
			{
				return true;
			}

			Diagnostics.Error(TEXT("DFX8013"), Document.HeaderLocation, FString::Printf(
				TEXT("This file sits in the decompiled output directory but Name=\"%s\" builds '%s', outside the '%s/' namespace. That would overwrite the asset it was exported from. Re-export it, or move the file out of the decompiled tree to keep this name."),
				*Document.Name, *(MountPoint / Relative), FDreamFXPaths::DecompiledNamespace));
			return false;
		}
	}

	FGenerateResult FGenerator::GenerateFromFile(const FString& FilePath, const FGenerateOptions& Options,
		FDiagnosticSink& Diagnostics)
	{
		FDocument Document;
		if (!FParser::ParseFile(FilePath, Document, Diagnostics))
		{
			return FGenerateResult();
		}
		return Generate(Document, Options, Diagnostics);
	}

	FGenerateResult FGenerator::Generate(const FDocument& Document, const FGenerateOptions& Options,
		FDiagnosticSink& Diagnostics)
	{
		FGenerateResult Result;
		Diagnostics.SetFile(Document.SourceFilePath);

		if (!GuardDecompiledNamespace(Document, Diagnostics))
		{
			return Result;
		}

		if (Document.Kind == EDocumentKind::Module || Document.Kind == EDocumentKind::DynamicInput)
		{
			// Lint first: a .dfm that is internally inconsistent should say so whether or not this build
			// can generate one, and the generator below assumes the linter's invariants hold.
			FLint::Run(Document, Diagnostics);
			if (Diagnostics.HasErrors())
			{
				return Result;
			}

			// plan-v2 W1. Generation exists only where the engine exports a way to put HLSL on a custom
			// node -- MoonEngine, detected by the Build.cs probe. Everywhere else the committed asset is
			// still checked against its source; only the remedy differs.
			const FModuleGenerateResult ModuleResult = FModuleGenerator::IsAvailable()
				? FModuleGenerator::Generate(Document, Options, Diagnostics)
				: FModuleGenerator::CheckWithoutGenerating(Document, Diagnostics);
			Result.bSucceeded = ModuleResult.bSucceeded;
			Result.bSkipped = ModuleResult.bSkipped;
			Result.bDrifted = ModuleResult.bDrifted;
			Result.AssetPath = ModuleResult.AssetPath;
			return Result;
		}

		if (Document.Kind != EDocumentKind::System)
		{
			Diagnostics.Error(TEXT("DFX5097"), Document.HeaderLocation,
				FString::Printf(TEXT("Only System documents can be generated right now; this file declares a %s."),
					LexDocumentKind(Document.Kind)));
			return Result;
		}

		// Lint before planning: its findings are about the source, so they are worth having even when
		// the build goes on to fail for an unrelated reason.
		FLint::Run(Document, Diagnostics);

		FModuleLibrary Modules;
		if (const FPropertyEntry* ModulePaths = Document.FindSetting(TEXT("ModulePaths")))
		{
			TArray<FString> Paths;
			if (ModulePaths->Value.IsValid() && ModulePaths->Value->Kind == EValueKind::Array)
			{
				for (const FValuePtr& Element : ModulePaths->Value->Elements)
				{
					if (Element.IsValid() && Element->Kind == EValueKind::String)
					{
						Paths.Add(Element->Text);
					}
				}
			}
			if (Paths.Num() > 0)
			{
				Modules.SetSearchPaths(Paths);
			}
		}

		// Everything that can fail happens here, before the asset is touched (plan 4.5).
		FPlan Plan;
		if (!BuildPlan(Document, Modules, Diagnostics, Plan))
		{
			return Result;
		}

		Result.AssetPath = Plan.FullAssetPath;

		bool bCreated = false;
		TArray<FString> Errors;
		UNiagaraSystem* System = FNiagaraAdapter::AcquireSystem(Plan.PackagePath, Plan.AssetName, bCreated, Errors);
		if (System == nullptr)
		{
			ReportAdapterErrors(Errors, TEXT("DFX5000"), Document.HeaderLocation, Diagnostics);
			return Result;
		}
		Result.System = System;

		const bool bUpToDate = FProvenance::IsUpToDate(System, Document.SourceHash);

		if (Options.bVerifyOnly)
		{
			FProvenanceStamp Stamp;
			const bool bHasStamp = FProvenance::Read(System, Stamp);
			if (!bHasStamp)
			{
				Diagnostics.Error(TEXT("DFX7001"), Document.HeaderLocation,
					FString::Printf(TEXT("Asset '%s' carries no DreamFX provenance stamp: it was never generated from this source, or it was created by hand."),
						*Plan.FullAssetPath));
				Result.bDrifted = true;
			}
			else if (Stamp.SourceHash != Document.SourceHash)
			{
				Diagnostics.Error(TEXT("DFX7002"), Document.HeaderLocation,
					FString::Printf(TEXT("Asset '%s' is stale: it was generated from a different revision of this source. Run the DreamFX build."),
						*Plan.FullAssetPath));
				Result.bDrifted = true;
			}
			else if (Stamp.GeneratorVersion != FProvenance::GetGeneratorVersion())
			{
				Diagnostics.Warning(TEXT("DFX7003"), Document.HeaderLocation,
					FString::Printf(TEXT("Asset '%s' was generated by DreamFX %s; the current generator is %s. A rebuild is recommended."),
						*Plan.FullAssetPath, *Stamp.GeneratorVersion, FProvenance::GetGeneratorVersion()));
			}

			// R7. The stamp says which version of each module the asset was built against; the planner
			// above has just resolved the same modules as they are now. A difference means the text is
			// unchanged, the asset is unchanged, and the thing they both depend on moved -- the exact
			// failure that is otherwise undetectable, because nothing in the source or the asset says
			// anything about it.
			if (bHasStamp)
			{
				for (const TPair<FString, FString>& Current : Plan.Dependencies.Versions)
				{
					const FString* Recorded = Stamp.ModuleVersions.Find(Current.Key);
					if (Recorded == nullptr)
					{
						// A dependency the stamp never mentioned: either a source edit added it (which
						// the hash check above already caught) or the stamp predates version recording.
						continue;
					}
					if (*Recorded == Current.Value)
					{
						continue;
					}

					FScriptVersion Was;
					FScriptVersion Now;
					FScriptVersion::FromStampString(*Recorded, Was);
					FScriptVersion::FromStampString(Current.Value, Now);

					const FString Message = FString::Printf(
						TEXT("Module '%s' was version %s when '%s' was built and is version %s now. The source did not change, so the asset was built against a different module than the one this build would use. Rebuild to adopt it, or pin the module with '@%s' to have the mismatch reported at the call site."),
						*Current.Key, *Was.ToLabel(), *Plan.FullAssetPath, *Now.ToLabel(), *Was.ToLabel());

					if (Options.bStrictVersions)
					{
						Diagnostics.Error(TEXT("DFX7005"), Document.HeaderLocation, Message);
						Result.bDrifted = true;
					}
					else
					{
						Diagnostics.Warning(TEXT("DFX7005"), Document.HeaderLocation, Message);
					}
				}
			}

			Result.bSucceeded = !Result.bDrifted;
			return Result;
		}

		if (bUpToDate && !Options.bForce)
		{
			Result.bSucceeded = true;
			Result.bSkipped = true;
			return Result;
		}

		TMap<FName, FSourceLocation> ModuleLocations;
		if (!ApplyPlan(System, Plan, Diagnostics, ModuleLocations))
		{
			return Result;
		}

		// Before the compile, because the compile is the first thing that reads them. A curve written
		// through the data-interface JSON path has its keys but not the sample table those keys are
		// baked into, and nothing in that path bakes it (see RefreshCurveLookupTables).
		FNiagaraAdapter::RefreshCurveLookupTables(System);

		// plan-v2 W4. A GPU emitter's real work is the compute shader, and WaitForCompilationComplete
		// does not wait for it unless asked -- so without this a GPU system's build reports the VM
		// scripts' status and finishes while the shader is still compiling. The gate would pass on a
		// system whose shader had not been built, let alone succeeded.
		bool bHasGpuEmitter = false;
		for (const FEmitter& Emitter : Document.Emitters)
		{
			for (const FPropertyEntry& Setting : Emitter.Settings)
			{
				if (Setting.Name.Equals(TEXT("SimTarget"), ESearchCase::IgnoreCase)
					&& Setting.Value.IsValid()
					&& Setting.Value->Text.Equals(TEXT("GPU"), ESearchCase::IgnoreCase))
				{
					bHasGpuEmitter = true;
					break;
				}
			}
		}

		FCompileStateInfo CompileState;
		Errors.Reset();
		const bool bCompiled = FNiagaraAdapter::CompileAndWait(System, bHasGpuEmitter, CompileState, Errors);
		ReportAdapterErrors(Errors, TEXT("DFX6000"), Document.HeaderLocation, Diagnostics);
		ReportNiagaraDiagnostics(System, CompileState, Plan, ModuleLocations, Diagnostics);

		if (!bCompiled)
		{
			Diagnostics.Error(TEXT("DFX6005"), Document.HeaderLocation,
				FString::Printf(TEXT("Niagara compilation of '%s' did not succeed (status %s)."),
					*Plan.FullAssetPath, *CompileState.StatusName));
			return Result;
		}

		FProvenanceStamp Stamp;
		Stamp.SourceFullPath = Document.SourceFilePath;
		Stamp.SourceHash = Document.SourceHash;
		Stamp.GeneratorVersion = FProvenance::GetGeneratorVersion();
		Stamp.ModuleDependencies = Plan.Dependencies.Paths;
		Stamp.ModuleVersions = Plan.Dependencies.Versions;

		FSourceRoot OwningRoot;
		if (FDreamFXPaths::FindOwningRoot(Document.SourceFilePath, OwningRoot))
		{
			Stamp.SourceRelativePath = Document.SourceFilePath;
			FPaths::MakePathRelativeTo(Stamp.SourceRelativePath, *(OwningRoot.Directory / TEXT("")));
		}
		else
		{
			Stamp.SourceRelativePath = FPaths::GetCleanFilename(Document.SourceFilePath);
		}

		FProvenance::Write(System, Stamp);

		if (Options.bSave)
		{
			Errors.Reset();
			if (!FNiagaraAdapter::SaveSystem(System, Errors))
			{
				ReportAdapterErrors(Errors, TEXT("DFX5030"), Document.HeaderLocation, Diagnostics);
				return Result;
			}
		}

		Result.bSucceeded = !Diagnostics.HasErrors();
		return Result;
	}
}
