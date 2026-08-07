#include "DreamFXGenerator.h"

#include "Adapter/DreamFXNiagaraAdapter.h"
#include "DreamFXModule.h"
#include "DreamFXParser.h"
#include "DreamFXProvenance.h"
#include "DreamFXValueLowering.h"
#include "Lint/DreamFXLint.h"
#include "Schema/DreamFXModuleLibrary.h"
#include "SourceFiles/DreamFXPaths.h"

#include "Dom/JsonObject.h"
#include "Misc/Paths.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::DreamFX::Editor
{
	namespace
	{
		/** A module call resolved against its schema, with every input already lowered. */
		struct FPlannedInput
		{
			FName Name;
			FInputValue Value;
			FSourceLocation Location;
		};

		/** One entry of a folded Set Parameters module (L2). */
		struct FPlannedSetParameter
		{
			FName Name;
			FNiagaraTypeDefinition Type;
			FInputValue Value;
			FSourceLocation Location;
		};

		struct FPlannedModule
		{
			FString SourceName;
			UNiagaraScript* Asset = nullptr;
			TArray<FPlannedInput> Inputs;
			FSourceLocation Location;

			/** When true this is a Set Parameters module and Parameters holds its entries. */
			bool bIsSetParameters = false;
			TArray<FPlannedSetParameter> Parameters;
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
			TArray<FString> ModuleDependencies;
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
					OutJson = MakeShared<FJsonValueString>(Resolved);
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
		bool PlanSettings(const TArray<FProperty>& Settings, TArrayView<const FSettingMapping> Mappings,
			const FString& DefaultRoot, const TCHAR* ScopeLabel, FDiagnosticSink& Diagnostics, FString& OutJson)
		{
			bool bOk = true;
			TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();

			for (const FProperty& Setting : Settings)
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
		};

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

			Diagnostics.Error(TEXT("DFX4027"), Location,
				FString::Printf(TEXT("'%s' is %s, but '%s' is %s. Linking binds a parameter directly -- there is no conversion. Declare '%s' as %s, or drive the input another way."),
					*SourceName.ToString(), *FValueLowering::DescribeType(*SourceType),
					*DisplayName, *FValueLowering::DescribeType(TargetType),
					*SourceName.ToString(), *FValueLowering::DescribeType(TargetType)));
			return false;
		}

		bool PlanStack(const FStack& Stack, const FStackContext& Context,
			FDiagnosticSink& Diagnostics, FPlannedStack& OutStack, TArray<FString>& OutDependencies)
		{
			FModuleLibrary& Modules = *Context.Modules;
			const FString& DefaultRoot = Context.DefaultRoot;
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
					if (const FNiagaraTypeDefinition* Existing = Context.DeclaredAttributes
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
					if (!FValueLowering::Lower(*Statement.Value, TargetType, Statement.Name, Diagnostics, Parameter.Value))
					{
						bOk = false;
						continue;
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

				if (!Statement.VersionPin.IsEmpty())
				{
					Diagnostics.Warning(TEXT("DFX5091"), Statement.Location,
						FString::Printf(TEXT("Version pin '@%s' on module '%s' is parsed but not yet honoured; the latest version is used."),
							*Statement.VersionPin, *Statement.Name));
				}

				FString Error;
				UNiagaraScript* ModuleAsset = Modules.FindModule(Statement.Name, Error);
				if (ModuleAsset == nullptr)
				{
					Diagnostics.Error(TEXT("DFX3001"), Statement.Location, Error);
					bOk = false;
					continue;
				}

				// Stack-aware, not asset-level: inline edit conditions and static switches only exist on
				// a live module, and both are things authors write every day.
				const FModuleSchema* Schema = Modules.GetStackSchema(ModuleAsset, Stack.Kind, Error);
				if (Schema == nullptr)
				{
					Diagnostics.Error(TEXT("DFX3002"), Statement.Location,
						FString::Printf(TEXT("Could not read the input schema of module '%s': %s"), *Statement.Name, *Error));
					bOk = false;
					continue;
				}

				FPlannedModule Planned;
				Planned.SourceName = Statement.Name;
				Planned.Asset = ModuleAsset;
				Planned.Location = Statement.Location;
				OutDependencies.AddUnique(ModuleAsset->GetPathName());

				TSet<FName> Seen;
				for (const FNamedArgument& Argument : Statement.Arguments)
				{
					const FInputSchema* InputSchema = Schema->FindInputByIdentifier(Argument.Name);
					if (InputSchema == nullptr)
					{
						// This is the Phase 1 acceptance case: a mistyped input name must point at the
						// exact line and column of the argument, not at the module or the file.
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

					FPlannedInput PlannedInput;
					PlannedInput.Name = InputSchema->Name;
					PlannedInput.Location = Argument.Location;
					const FString DisplayName = DescribeInput(Statement.Name, Argument.Name);
					if (!FValueLowering::Lower(*Argument.Value, InputSchema->Type,
						DisplayName, Diagnostics, PlannedInput.Value))
					{
						bOk = false;
						continue;
					}

					if (!ValidateLinkedType(PlannedInput.Value, InputSchema->Type, Context,
						DisplayName, Argument.Location, Diagnostics))
					{
						bOk = false;
						continue;
					}

					Planned.Inputs.Add(MoveTemp(PlannedInput));
				}

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
				if (!FValueLowering::IsNamespacedName(Binding.Target))
				{
					Diagnostics.Error(TEXT("DFX4026"), Binding.Location,
						FString::Printf(TEXT("'Bind %s -> %s': the target must be a namespace-qualified parameter, e.g. Particles.SpriteSize."),
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
			for (const FProperty& Property : Renderer.Properties)
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
				Properties->SetField(Property.Name, Json);
			}

			OutRenderer.PropertiesJson = Properties->Values.Num() > 0 ? SerializeJsonObject(Properties) : FString();
			return bOk;
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

			for (const FStack& Stack : Document.Stacks)
			{
				FPlannedStack Planned;
				if (!PlanStack(Stack, StackContext, Diagnostics, Planned, OutPlan.ModuleDependencies))
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

				if (!Emitter.FromPath.IsEmpty())
				{
					Diagnostics.Error(TEXT("DFX5096"), Emitter.FromLocation,
						TEXT("External emitter references ('from \"...\"') are not available yet (planned for Phase 6)."));
					bOk = false;
					continue;
				}

				FPlannedEmitter Planned;
				Planned.Name = EmitterName;
				Planned.Location = Emitter.Location;

				if (!PlanSettings(Emitter.Settings, EmitterSettings, Document.Root, TEXT("emitter"),
					Diagnostics, Planned.PropertiesJson))
				{
					bOk = false;
				}

				// Attribute declarations are per emitter: Particles.* on one emitter is a different
				// parameter from the same spelling on another.
				TMap<FName, FNiagaraTypeDefinition> EmitterAttributes;
				FStackContext EmitterContext = StackContext;
				EmitterContext.DeclaredAttributes = &EmitterAttributes;

				for (const FStack& Stack : Emitter.Stacks)
				{
					if (Stack.Kind == EStackKind::SimulationStage || Stack.Kind == EStackKind::EventHandler)
					{
						// The parser already reported these as reserved; do not double-report.
						continue;
					}
					FPlannedStack PlannedStack;
					if (!PlanStack(Stack, EmitterContext, Diagnostics, PlannedStack, OutPlan.ModuleDependencies))
					{
						bOk = false;
					}
					Planned.Stacks.Add(MoveTemp(PlannedStack));
				}

				for (const FRenderer& Renderer : Emitter.Renderers)
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

		bool ApplyStack(const FStackAddress& OwnerAddress, const FPlannedStack& Stack,
			FDiagnosticSink& Diagnostics, TMap<FName, FSourceLocation>& OutModuleLocations)
		{
			bool bOk = true;
			FName PreviousModule = NAME_None;

			for (const FPlannedModule& Module : Stack.Modules)
			{
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
						if (Parameter.Value.Mode == EInputValueMode::Literal || Parameter.Value.Mode == EInputValueMode::Enum)
						{
							continue;
						}
						Errors.Reset();
						if (!FNiagaraAdapter::SetInput(ModuleAddress.WithInput(Parameter.Name), Parameter.Value, Errors))
						{
							ReportAdapterErrors(Errors, TEXT("DFX5025"), Parameter.Location, Diagnostics);
							bOk = false;
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
				for (const FPlannedInput& Input : Module.Inputs)
				{
					Errors.Reset();
					if (!FNiagaraAdapter::SetInput(ModuleAddress.WithInput(Input.Name), Input.Value, Errors))
					{
						ReportAdapterErrors(Errors, TEXT("DFX5021"), Input.Location, Diagnostics);
						bOk = false;
					}
				}
			}

			return bOk;
		}

		bool ApplyPlan(UNiagaraSystem* System, const FPlan& Plan, FDiagnosticSink& Diagnostics,
			TMap<FName, FSourceLocation>& OutModuleLocations)
		{
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
		if (const FProperty* ModulePaths = Document.FindSetting(TEXT("ModulePaths")))
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

		FCompileStateInfo CompileState;
		Errors.Reset();
		const bool bCompiled = FNiagaraAdapter::CompileAndWait(System, CompileState, Errors);
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
		Stamp.ModuleDependencies = Plan.ModuleDependencies;

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
