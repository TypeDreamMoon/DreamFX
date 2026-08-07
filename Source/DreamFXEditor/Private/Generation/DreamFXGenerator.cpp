#include "DreamFXGenerator.h"

#include "Adapter/DreamFXNiagaraAdapter.h"
#include "DreamFXModule.h"
#include "DreamFXParser.h"
#include "DreamFXProvenance.h"
#include "DreamFXValueLowering.h"
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

		struct FPlannedModule
		{
			FString SourceName;
			UNiagaraScript* Asset = nullptr;
			TArray<FPlannedInput> Inputs;
			FSourceLocation Location;
		};

		struct FPlannedStack
		{
			EStackKind Kind = EStackKind::ParticleUpdate;
			FName ScriptName;
			TArray<FPlannedModule> Modules;
			FSourceLocation Location;
		};

		struct FPlannedRenderer
		{
			UClass* Class = nullptr;
			FString PropertiesJson;
			FSourceLocation Location;
		};

		struct FPlannedEmitter
		{
			FName Name;
			TArray<FPlannedStack> Stacks;
			TArray<FPlannedRenderer> Renderers;
			FSourceLocation Location;
		};

		struct FPlan
		{
			FString PackagePath;
			FString AssetName;
			FString FullAssetPath;
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

		FString SerializeJsonObject(const TSharedRef<FJsonObject>& Object)
		{
			FString Result;
			const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Result);
			FJsonSerializer::Serialize(Object, Writer);
			return Result;
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

		bool PlanStack(const FStack& Stack, FModuleLibrary& Modules, const FString& DefaultRoot,
			FDiagnosticSink& Diagnostics, FPlannedStack& OutStack, TArray<FString>& OutDependencies)
		{
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
					Diagnostics.Error(TEXT("DFX5090"), Statement.Location,
						FString::Printf(TEXT("Parameter assignments ('%s = ...') are not available yet (planned for Phase 2)."),
							*Statement.Name));
					bOk = false;
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
					if (!FValueLowering::Lower(*Argument.Value, InputSchema->Type,
						DescribeInput(Statement.Name, Argument.Name), Diagnostics, PlannedInput.Value))
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

			if (Renderer.Bindings.Num() > 0)
			{
				Diagnostics.Error(TEXT("DFX5092"), Renderer.Bindings[0].Location,
					TEXT("'Bind' is not available yet (planned for Phase 2)."));
				bOk = false;
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

			// Settings and Properties are parsed but not yet applied. Reported rather than dropped:
			// silently ignoring a declaration the author wrote is exactly the drift the plan's
			// "text is the only truth" principle exists to prevent.
			if (Document.Settings.Num() > 0)
			{
				Diagnostics.Warning(TEXT("DFX5094"), Document.Settings[0].Location,
					TEXT("System 'Settings' are parsed but not yet applied (planned for Phase 2)."));
			}
			if (Document.Parameters.Num() > 0)
			{
				Diagnostics.Warning(TEXT("DFX5095"), Document.Parameters[0].Location,
					TEXT("System 'Properties' (user parameters) are parsed but not yet applied (planned for Phase 2)."));
			}

			for (const FStack& Stack : Document.Stacks)
			{
				FPlannedStack Planned;
				if (!PlanStack(Stack, Modules, Document.Root, Diagnostics, Planned, OutPlan.ModuleDependencies))
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

				if (Emitter.Settings.Num() > 0)
				{
					Diagnostics.Warning(TEXT("DFX5094"), Emitter.Settings[0].Location,
						TEXT("Emitter 'Settings' are parsed but not yet applied (planned for Phase 2)."));
				}

				for (const FStack& Stack : Emitter.Stacks)
				{
					if (Stack.Kind == EStackKind::SimulationStage || Stack.Kind == EStackKind::EventHandler)
					{
						// The parser already reported these as reserved; do not double-report.
						continue;
					}
					FPlannedStack PlannedStack;
					if (!PlanStack(Stack, Modules, Document.Root, Diagnostics, PlannedStack, OutPlan.ModuleDependencies))
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
					if (!Renderer.PropertiesJson.IsEmpty())
					{
						Errors.Reset();
						if (!FNiagaraAdapter::SetRendererProperties(EmitterAddress.WithRenderer(RendererIndex),
							Renderer.PropertiesJson, Errors))
						{
							ReportAdapterErrors(Errors, TEXT("DFX5023"), Renderer.Location, Diagnostics);
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
