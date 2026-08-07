#include "DreamFXDecompiler.h"

#include "Adapter/DreamFXNiagaraAdapter.h"
#include "DreamFXModule.h"
#include "Generation/DreamFXValueLowering.h"
#include "Schema/DreamFXModuleLibrary.h"
#include "SourceFiles/DreamFXPaths.h"

#include "Dom/JsonObject.h"
#include "NiagaraDataInterfaceCurve.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UE::DreamFX::Editor
{
	namespace
	{
		/** Accumulates the output with an indent level, so the emitters read as nested blocks. */
		class FWriter
		{
		public:
			void Line(const FString& Text)
			{
				if (Text.IsEmpty())
				{
					Buffer += LINE_TERMINATOR;
					return;
				}
				Buffer += FString::ChrN(Indent * 4, TEXT(' ')) + Text + LINE_TERMINATOR;
			}

			void Blank() { Buffer += LINE_TERMINATOR; }
			void Push() { ++Indent; }
			void Pop() { Indent = FMath::Max(0, Indent - 1); }

			const FString& Get() const { return Buffer; }

		private:
			FString Buffer;
			int32 Indent = 0;
		};

		/**
		 * Turns an enum's display label into something the parser will read back.
		 *
		 * Labels carry both a prose suffix ("Complete (Let Particles Finish...)") and spaces
		 * ("Spawn Only"), neither of which is a DSL identifier. The parenthesis is dropped and the
		 * spaces are removed; the lookup normalises spaces away too, so the two sides agree.
		 */
		FString EnumLabelToIdentifier(const FString& DisplayName)
		{
			int32 ParenIndex;
			FString Label = DisplayName.FindChar(TEXT('('), ParenIndex)
				? DisplayName.Left(ParenIndex)
				: DisplayName;
			Label.TrimStartAndEndInline();
			Label.ReplaceInline(TEXT(" "), TEXT(""), ESearchCase::CaseSensitive);
			Label.ReplaceInline(TEXT("-"), TEXT(""), ESearchCase::CaseSensitive);
			return Label;
		}

		FString FormatFloat(float Value)
		{
			// SanitizeFloat keeps the shortest round-trippable form, which is what keeps a re-export
			// byte-identical to the one before it.
			return FString::SanitizeFloat(Value);
		}

		/** Renders a literal value back to source, given the type that says how to read its bytes. */
		bool LiteralToSource(const FInputValue& Value, const FNiagaraTypeDefinition& Type, FString& Out)
		{
			const UScriptStruct* Struct = Value.LiteralStruct;
			if (Struct == nullptr || Value.LiteralBytes.Num() == 0)
			{
				return false;
			}

			const uint8* Memory = Value.LiteralBytes.GetData();

			if (Struct == FNiagaraFloat::StaticStruct())
			{
				Out = FormatFloat(reinterpret_cast<const FNiagaraFloat*>(Memory)->Value);
				return true;
			}
			if (Struct == FNiagaraInt32::StaticStruct())
			{
				const int32 Integer = reinterpret_cast<const FNiagaraInt32*>(Memory)->Value;
				// An int-typed slot holding an enum is written as the enum entry, not a raw number.
				if (UEnum* Enum = Type.GetEnum())
				{
					const int32 Index = Enum->GetIndexByValue(Integer);
					if (Index != INDEX_NONE)
					{
						FString Label = Enum->GetDisplayNameTextByIndex(Index).ToString();
						Label = EnumLabelToIdentifier(Label);
						Out = Label.IsEmpty() ? Enum->GetNameStringByIndex(Index) : Label;
						return true;
					}
				}
				Out = FString::FromInt(Integer);
				return true;
			}
			if (Struct == FNiagaraBool::StaticStruct())
			{
				Out = reinterpret_cast<const FNiagaraBool*>(Memory)->GetValue() ? TEXT("true") : TEXT("false");
				return true;
			}

			// Everything else in the family is packed floats; the component count comes from the size.
			const int32 Components = Struct->GetStructureSize() / static_cast<int32>(sizeof(float));
			if (Components >= 2 && Components <= 4
				&& Struct->GetStructureSize() == Components * static_cast<int32>(sizeof(float)))
			{
				const float* Floats = reinterpret_cast<const float*>(Memory);
				TArray<FString> Parts;
				for (int32 Index = 0; Index < Components; ++Index)
				{
					Parts.Add(FormatFloat(Floats[Index]));
				}
				Out = FString::Printf(TEXT("(%s)"), *FString::Join(Parts, TEXT(", ")));
				return true;
			}

			return false;
		}

		/** Reads a curve data interface's JSON back into a `curve { }` literal. */
		bool CurveJsonToSource(const FString& Json, int32 IndentLevel, FString& Out)
		{
			TSharedPtr<FJsonObject> Root;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
			if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
			{
				return false;
			}

			// Multi-channel curve interfaces hold XCurve/YCurve/...; the scalar one holds Curve. Only
			// the first channel is exported, matching what the generator can express.
			const TSharedPtr<FJsonObject>* Curve = nullptr;
			for (const TCHAR* Field : { TEXT("Curve"), TEXT("XCurve") })
			{
				if (Root->TryGetObjectField(Field, Curve))
				{
					break;
				}
			}
			if (Curve == nullptr || !Curve->IsValid())
			{
				return false;
			}

			const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
			if (!(*Curve)->TryGetArrayField(TEXT("Keys"), Keys) || Keys == nullptr)
			{
				return false;
			}

			const FString Indent = FString::ChrN(IndentLevel * 4, TEXT(' '));
			const FString InnerIndent = FString::ChrN((IndentLevel + 1) * 4, TEXT(' '));

			FString Result = TEXT("curve {") LINE_TERMINATOR;
			for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
			{
				const TSharedPtr<FJsonObject> Key = KeyValue->AsObject();
				if (!Key.IsValid())
				{
					continue;
				}

				const double Time = Key->GetNumberField(TEXT("Time"));
				const double Value = Key->GetNumberField(TEXT("Value"));

				TArray<FString> Attributes;
				const FString InterpMode = Key->GetStringField(TEXT("InterpMode"));
				if (InterpMode == TEXT("RCIM_Linear"))        { Attributes.Add(TEXT("Interp=Linear")); }
				else if (InterpMode == TEXT("RCIM_Constant")) { Attributes.Add(TEXT("Interp=Constant")); }
				else if (InterpMode == TEXT("RCIM_Cubic"))    { Attributes.Add(TEXT("Interp=Cubic")); }

				// Tangents are only meaningful in User mode; exporting the auto-derived ones would
				// turn a re-import into a differently-shaped curve.
				if (Key->GetStringField(TEXT("TangentMode")) == TEXT("RCTM_User"))
				{
					Attributes.Add(FString::Printf(TEXT("Arrive=%s"),
						*FormatFloat(static_cast<float>(Key->GetNumberField(TEXT("ArriveTangent"))))));
					Attributes.Add(FString::Printf(TEXT("Leave=%s"),
						*FormatFloat(static_cast<float>(Key->GetNumberField(TEXT("LeaveTangent"))))));
				}

				Result += InnerIndent + FString::Printf(TEXT("%s -> %s"),
					*FormatFloat(static_cast<float>(Time)), *FormatFloat(static_cast<float>(Value)));
				if (Attributes.Num() > 0)
				{
					Result += FString::Printf(TEXT(" [ %s ]"), *FString::Join(Attributes, TEXT("; ")));
				}
				Result += TEXT(";") LINE_TERMINATOR;
			}
			Result += Indent + TEXT("}");

			Out = Result;
			return true;
		}

		struct FContext
		{
			UNiagaraSystem* System = nullptr;
			FModuleLibrary* Modules = nullptr;
			FString RootToken;
			FString RootMountPoint;
			TArray<FString>* Unsupported = nullptr;
		};

		/** Shortens an asset path that lives under the document's root. */
		FString RelativeAssetPath(const FContext& Context, const FString& PackagePath)
		{
			if (!Context.RootMountPoint.IsEmpty() && PackagePath.StartsWith(Context.RootMountPoint + TEXT("/")))
			{
				return PackagePath.RightChop(Context.RootMountPoint.Len() + 1);
			}
			return PackagePath;
		}

		FString ValueToSource(const FContext& Context, const FStackAddress& InputAddress,
			const FInputValue& Value, const FNiagaraTypeDefinition& Type, int32 IndentLevel);

		/** Rebuilds a dynamic input call, recursing into whatever hangs below it. */
		FString DynamicInputToSource(const FContext& Context, const FStackAddress& InputAddress,
			const FInputValue& Value, int32 IndentLevel)
		{
			const FString Name = Value.DynamicInputAsset
				? Context.Modules->GetUnambiguousName(Value.DynamicInputAsset, /*bDynamicInput=*/true)
				: TEXT("<unknown>");

			TArray<TPair<FName, bool>> Children;
			TArray<FString> Errors;
			FNiagaraAdapter::GetDynamicInputChildren(InputAddress, Children, Errors);

			FString SchemaError;
			const FModuleSchema* Schema = Value.DynamicInputAsset
				? Context.Modules->GetDynamicInputSchema(Value.DynamicInputAsset, SchemaError)
				: nullptr;

			TArray<FString> Arguments;
			for (const TPair<FName, bool>& Child : Children)
			{
				const FName ChildName = Child.Key;

				// A non-editable input is one SetStackInputData refuses. Exporting it would produce a
				// file that cannot be rebuilt, so it is dropped -- it is at its default anyway,
				// because nothing could have written it.
				if (!Child.Value)
				{
					continue;
				}

				// The live chain exposes more children than the asset schema does -- static switches
				// among them -- and the generator plans against the schema. Exporting a name the
				// importer cannot resolve would produce a file that does not rebuild, so those are
				// reported as a gap instead of written out.
				const FInputSchema* Found = Schema ? Schema->FindInput(ChildName) : nullptr;
				if (Found == nullptr)
				{
					if (Context.Unsupported != nullptr)
					{
						Context.Unsupported->AddUnique(TEXT("dynamic input static switch"));
					}
					continue;
				}

				const FStackAddress ChildAddress = InputAddress.WithInput(ChildName);

				FInputValue ChildValue;
				Errors.Reset();
				if (!FNiagaraAdapter::GetInput(ChildAddress, ChildValue, Errors) || !ChildValue.IsSet())
				{
					continue;
				}

				Arguments.Add(FString::Printf(TEXT("%s = %s"),
					*ToInputIdentifier(ChildName),
					*ValueToSource(Context, ChildAddress, ChildValue, Found->Type, IndentLevel + 1)));
			}

			if (Arguments.Num() == 0)
			{
				return FString::Printf(TEXT("%s()"), *Name);
			}

			// A short call stays on one line; a long one wraps, because a nested chain on a single
			// line is unreadable and this is the output a human is meant to edit.
			const FString OneLine = FString::Printf(TEXT("%s(%s)"), *Name, *FString::Join(Arguments, TEXT(", ")));
			if (OneLine.Len() + IndentLevel * 4 <= 100 && !OneLine.Contains(LINE_TERMINATOR))
			{
				return OneLine;
			}

			const FString Indent = FString::ChrN((IndentLevel + 1) * 4, TEXT(' '));
			const FString CloseIndent = FString::ChrN(IndentLevel * 4, TEXT(' '));
			return FString::Printf(TEXT("%s(") LINE_TERMINATOR TEXT("%s%s") LINE_TERMINATOR TEXT("%s)"),
				*Name, *Indent, *FString::Join(Arguments, *(FString(TEXT(",") LINE_TERMINATOR) + Indent)), *CloseIndent);
		}

		FString ValueToSource(const FContext& Context, const FStackAddress& InputAddress,
			const FInputValue& Value, const FNiagaraTypeDefinition& Type, int32 IndentLevel)
		{
			switch (Value.Mode)
			{
			case EInputValueMode::Literal:
			{
				FString Text;
				if (LiteralToSource(Value, Type, Text))
				{
					return Text;
				}
				return TEXT("/* unsupported literal */ 0");
			}

			case EInputValueMode::Enum:
			{
				// The internal name is meaningless on user-defined enums; the label is what is written.
				if (Value.EnumType != nullptr)
				{
					const int32 Index = Value.EnumType->GetIndexByName(Value.EnumEntryName);
					if (Index != INDEX_NONE)
					{
						FString Label = Value.EnumType->GetDisplayNameTextByIndex(Index).ToString();
						Label = EnumLabelToIdentifier(Label);
						if (!Label.IsEmpty())
						{
							return Label;
						}
						return Value.EnumType->GetNameStringByIndex(Index);
					}
				}
				return Value.EnumEntryName.ToString();
			}

			case EInputValueMode::Linked:
				return Value.LinkedVariable.GetName().ToString();

			case EInputValueMode::Hlsl:
				return FString::Printf(TEXT("hlsl { %s }"), *Value.HlslExpression);

			case EInputValueMode::DynamicInput:
				return DynamicInputToSource(Context, InputAddress, Value, IndentLevel);

			case EInputValueMode::DataInterface:
			{
				FString Curve;
				if (CurveJsonToSource(Value.DataInterfaceJson, IndentLevel, Curve))
				{
					return Curve;
				}
				if (Context.Unsupported != nullptr)
				{
					Context.Unsupported->AddUnique(TEXT("data interface input (non-curve)"));
				}
				return TEXT("/* data interface: configure at runtime */");
			}

			default:
				return TEXT("/* unsupported */");
			}
		}

		/**
		 * Emits `Key = Value;` lines for JSON fields that differ from a baseline blob.
		 * Only fields DreamFX knows how to write back are considered, so an export never contains a
		 * setting the compiler would reject.
		 */
		void WriteChangedSettings(FWriter& Writer, const FString& Json, const FString& DefaultsJson,
			TArrayView<const TPair<const TCHAR*, const TCHAR*>> Mappings, TArray<FString>& OutLines)
		{
			TSharedPtr<FJsonObject> Current;
			TSharedPtr<FJsonObject> Defaults;
			{
				const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
				FJsonSerializer::Deserialize(Reader, Current);
			}
			{
				const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(DefaultsJson);
				FJsonSerializer::Deserialize(Reader, Defaults);
			}
			if (!Current.IsValid())
			{
				return;
			}

			for (const TPair<const TCHAR*, const TCHAR*>& Mapping : Mappings)
			{
				const TSharedPtr<FJsonValue> Value = Current->TryGetField(Mapping.Value);
				if (!Value.IsValid() || Value->IsNull())
				{
					continue;
				}

				if (Defaults.IsValid())
				{
					const TSharedPtr<FJsonValue> Default = Defaults->TryGetField(Mapping.Value);
					if (Default.IsValid() && FJsonValue::CompareEqual(*Value, *Default))
					{
						continue;
					}
				}

				FString Text;
				switch (Value->Type)
				{
				case EJson::Boolean:
					Text = Value->AsBool() ? TEXT("true") : TEXT("false");
					break;
				case EJson::Number:
				{
					// JSON has one number type, so an int32 property comes back as 1337.0 and would
					// re-import as a float -- which L7 then rejects. Integral values print as integers.
					const double Number = Value->AsNumber();
					Text = FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number))
						? FString::Printf(TEXT("%lld"), static_cast<int64>(FMath::RoundToDouble(Number)))
						: FormatFloat(static_cast<float>(Number));
					break;
				}
				case EJson::String:
				{
					const FString Raw = Value->AsString();
					// Enumerator names come back in their C++ spelling; the DSL aliases read better.
					if (Raw == TEXT("CPUSim"))            { Text = TEXT("CPU"); }
					else if (Raw == TEXT("GPUComputeSim")) { Text = TEXT("GPU"); }
					else if (Raw == TEXT("FixedCount"))    { Text = TEXT("Fixed"); }
					else if (Raw.StartsWith(TEXT("/")))    { Text = FString::Printf(TEXT("\"%s\""), *Raw); }
					else                                   { Text = Raw; }
					break;
				}
				default:
					continue; // structs and arrays have no settled DSL spelling yet
				}

				OutLines.Add(FString::Printf(TEXT("%s = %s;"), Mapping.Key, *Text));
			}
			(void)Writer;
		}

		/**
		 * Emits every scalar renderer property that differs from a pristine renderer of the same class.
		 *
		 * Renderer property blocks are schema-driven (L8), so there is no name list to work from --
		 * the diff against the default is the whole mechanism. Structs and arrays are skipped and
		 * counted as gaps rather than guessed at, because a half-written struct would re-import wrong.
		 */
		void WriteChangedRendererProperties(const FString& Json, const FString& DefaultsJson,
			TArray<FString>& OutLines, TArray<FString>& OutGaps)
		{
			TSharedPtr<FJsonObject> Current;
			TSharedPtr<FJsonObject> Defaults;
			{
				const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
				FJsonSerializer::Deserialize(Reader, Current);
			}
			{
				const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(DefaultsJson);
				FJsonSerializer::Deserialize(Reader, Defaults);
			}
			if (!Current.IsValid())
			{
				return;
			}

			// Sorted so a re-export of the same asset is byte-identical; map iteration order is not.
			TArray<FString> Keys;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : Current->Values)
			{
				Keys.Add(Entry.Key);
			}
			Keys.Sort();

			for (const FString& Key : Keys)
			{
				const TSharedPtr<FJsonValue> Value = Current->TryGetField(Key);
				if (!Value.IsValid() || Value->IsNull())
				{
					continue;
				}

				if (Defaults.IsValid())
				{
					const TSharedPtr<FJsonValue> Default = Defaults->TryGetField(Key);
					if (Default.IsValid() && FJsonValue::CompareEqual(*Value, *Default))
					{
						continue;
					}
				}

				// Attribute bindings are a `Bind X -> Y` statement, not a property assignment. They are
				// emitted from the live struct further down, so skipping them here is not a gap.
				if (Key.EndsWith(TEXT("Binding"), ESearchCase::CaseSensitive))
				{
					continue;
				}

				switch (Value->Type)
				{
				case EJson::Boolean:
					OutLines.Add(FString::Printf(TEXT("%s = %s;"), *Key, Value->AsBool() ? TEXT("true") : TEXT("false")));
					break;
				case EJson::Number:
				{
					const double Number = Value->AsNumber();
					OutLines.Add(FString::Printf(TEXT("%s = %s;"), *Key,
						FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number))
							? *FString::Printf(TEXT("%lld"), static_cast<int64>(FMath::RoundToDouble(Number)))
							: *FormatFloat(static_cast<float>(Number))));
					break;
				}
				case EJson::String:
				{
					const FString Raw = Value->AsString();
					OutLines.Add(FString::Printf(TEXT("%s = %s;"), *Key,
						Raw.StartsWith(TEXT("/")) ? *FString::Printf(TEXT("\"%s\""), *Raw) : *Raw));
					break;
				}
				default:
					OutGaps.AddUnique(FString::Printf(TEXT("renderer property '%s' (structured value)"), *Key));
					break;
				}
			}
		}

		const TPair<const TCHAR*, const TCHAR*> EmitterSettingFields[] =
		{
			{ TEXT("SimTarget"),          TEXT("SimTarget") },
			{ TEXT("LocalSpace"),         TEXT("bLocalSpace") },
			{ TEXT("Determinism"),        TEXT("bDeterminism") },
			{ TEXT("RandomSeed"),         TEXT("RandomSeed") },
			{ TEXT("AllocationMode"),     TEXT("AllocationMode") },
			{ TEXT("PreAllocationCount"), TEXT("PreAllocationCount") },
			{ TEXT("CalculateBoundsMode"),TEXT("CalculateBoundsMode") },
		};

		const TPair<const TCHAR*, const TCHAR*> SystemSettingFields[] =
		{
			{ TEXT("WarmupTime"), TEXT("WarmupTime") },
		};
	}

	FDecompileResult FDecompiler::Decompile(UNiagaraSystem* System, const FString& RootToken,
		FDiagnosticSink& Diagnostics)
	{
		FDecompileResult Result;
		if (System == nullptr)
		{
			Diagnostics.Error(TEXT("DFX8000"), FSourceLocation(), TEXT("Cannot decompile a null system."));
			return Result;
		}

		FModuleLibrary Modules;

		FContext Context;
		Context.System = System;
		Context.Modules = &Modules;
		Context.RootToken = RootToken;
		Context.Unsupported = &Result.UnsupportedFeatures;
		{
			FString Error;
			FDreamFXPaths::ResolveRootMountPoint(RootToken, Context.RootMountPoint, Error);
		}

		FWriter Writer;
		TArray<FString> Errors;

		const FString PackagePath = System->GetOutermost()->GetName();
		Writer.Line(FString::Printf(TEXT("// Decompiled from %s by DreamFX."), *System->GetPathName()));
		Writer.Line(TEXT("// Inline arithmetic is not recovered: expressions come back as equivalent hlsl { } blocks."));
		Writer.Line(FString::Printf(TEXT("System(Name=\"%s\", Root=\"%s\")"),
			*RelativeAssetPath(Context, PackagePath), *RootToken));
		Writer.Line(TEXT("{"));
		Writer.Push();

		// --- system settings ---------------------------------------------------------------
		{
			FString Json;
			Errors.Reset();
			if (FNiagaraAdapter::GetSystemProperties(System, Json, Errors))
			{
				TArray<FString> Lines;
				WriteChangedSettings(Writer, Json, FString(), SystemSettingFields, Lines);
				if (Lines.Num() > 0)
				{
					Writer.Line(TEXT("Settings = {"));
					Writer.Push();
					for (const FString& Line : Lines)
					{
						Writer.Line(Line);
					}
					Writer.Pop();
					Writer.Line(TEXT("}"));
					Writer.Blank();
				}
			}
		}

		// --- user parameters ---------------------------------------------------------------
		{
			TArray<FUserVariableInfo> UserVariables;
			Errors.Reset();
			if (FNiagaraAdapter::GetUserVariables(System, UserVariables, Errors) && UserVariables.Num() > 0)
			{
				Writer.Line(TEXT("Properties = {"));
				Writer.Push();
				for (const FUserVariableInfo& Variable : UserVariables)
				{
					FString Name = Variable.Name.ToString();
					Name.RemoveFromStart(TEXT("User."), ESearchCase::IgnoreCase);

					// Defaults are not recoverable through the read API -- GetUserVariables reports
					// name, type and description, and no value. Declaring without a default is
					// legal and honest; the runtime default stays on the asset.
					FString Line = FString::Printf(TEXT("%s %s;"),
						*FValueLowering::DescribeDeclaredType(Variable.Type), *Name);
					if (!Variable.Description.IsEmpty())
					{
						Line = FString::Printf(TEXT("%s %s [ Description=\"%s\" ];"),
							*FValueLowering::DescribeDeclaredType(Variable.Type), *Name, *Variable.Description);
					}
					Writer.Line(Line);
				}
				Writer.Pop();
				Writer.Line(TEXT("}"));
				Writer.Blank();
			}
		}

		// --- emitters ------------------------------------------------------------------------
		TArray<FName> EmitterNames;
		Errors.Reset();
		if (!FNiagaraAdapter::GetEmitterNames(System, EmitterNames, Errors))
		{
			Diagnostics.Error(TEXT("DFX8001"), FSourceLocation(),
				FString::Printf(TEXT("Could not read emitters: %s"), *FString::Join(Errors, TEXT(" | "))));
			return Result;
		}

		const FStackAddress SystemAddress(System);

		for (FName EmitterName : EmitterNames)
		{
			const FStackAddress EmitterAddress = SystemAddress.WithEmitter(EmitterName);

			FEmitterInfo Info;
			Errors.Reset();
			if (!FNiagaraAdapter::GetEmitterInfo(EmitterAddress, Info, Errors))
			{
				Diagnostics.Warning(TEXT("DFX8002"), FSourceLocation(),
					FString::Printf(TEXT("Skipping emitter '%s': %s"),
						*EmitterName.ToString(), *FString::Join(Errors, TEXT(" | "))));
				continue;
			}

			Writer.Line(FString::Printf(TEXT("Emitter %s"), *EmitterName.ToString()));
			Writer.Line(TEXT("{"));
			Writer.Push();

			{
				FString Json;
				Errors.Reset();
				if (FNiagaraAdapter::GetEmitterProperties(EmitterAddress, Json, Errors))
				{
					FString DefaultsError;
					const FString* Defaults = Modules.GetEmitterDefaults(DefaultsError);

					TArray<FString> Lines;
					WriteChangedSettings(Writer, Json, Defaults ? *Defaults : FString(),
						EmitterSettingFields, Lines);
					if (Lines.Num() > 0)
					{
						Writer.Line(TEXT("Settings = {"));
						Writer.Push();
						for (const FString& Line : Lines)
						{
							Writer.Line(Line);
						}
						Writer.Pop();
						Writer.Line(TEXT("}"));
						Writer.Blank();
					}
				}
			}

			for (const FScriptStackInfo& Stack : Info.Stacks)
			{
				if (Stack.Modules.Num() == 0)
				{
					continue;
				}

				EStackKind StackKind;
				if (!FNiagaraAdapter::StackForScriptName(Stack.ScriptName, StackKind))
				{
					Result.UnsupportedFeatures.AddUnique(FString::Printf(TEXT("stack %s"), *Stack.ScriptName.ToString()));
					continue;
				}

				Writer.Line(FString::Printf(TEXT("%s = {"), LexStackKind(StackKind)));
				Writer.Push();

				for (const FModuleInfo& Module : Stack.Modules)
				{
					const FStackAddress ModuleAddress = EmitterAddress
						.WithScript(Stack.ScriptName).WithModule(Module.ModuleName);

					TArray<TTuple<FName, FInputValue>> Values;
					Errors.Reset();
					FNiagaraAdapter::GetModuleInputValues(ModuleAddress, Values, Errors);

					if (Module.bIsSetParameters)
					{
						// A Set Parameters module exports as the assignment block it came from,
						// which is what makes L2's folding rule symmetric.
						for (const TTuple<FName, FInputValue>& Entry : Values)
						{
							const FInputInfo* InputInfo = Module.FindInput(Entry.Get<0>());
							const FNiagaraTypeDefinition Type = InputInfo ? InputInfo->Type : FNiagaraTypeDefinition();

							// Only a literal lets the type be inferred on re-import. Anything else
							// needs the type written out, or the export will not compile.
							const bool bNeedsType = Entry.Get<1>().Mode != EInputValueMode::Literal
								&& Entry.Get<1>().Mode != EInputValueMode::Enum
								&& Type.IsValid();
							const FString Prefix = bNeedsType
								? FValueLowering::DescribeDeclaredType(Type) + TEXT(" ")
								: FString();

							Writer.Line(FString::Printf(TEXT("%s%s = %s;"),
								*Prefix, *Entry.Get<0>().ToString(),
								*ValueToSource(Context, ModuleAddress.WithInput(Entry.Get<0>()),
									Entry.Get<1>(), Type, 0)));
						}
						continue;
					}

					// R8: only inputs that differ from a pristine instance of the same module are
					// printed. Without this every module dumps its entire input list.
					FString DefaultsError;
					const TMap<FName, FInputValue>* Defaults = Module.Script
						? Modules.GetStackDefaults(Module.Script, StackKind, DefaultsError)
						: nullptr;

					TArray<FString> Arguments;
					for (const TTuple<FName, FInputValue>& Entry : Values)
					{
						if (!Entry.Get<1>().IsSet())
						{
							continue;
						}
						// Same rule as the chain children: an input the writer would refuse must not
						// appear in an export.
						if (const FInputInfo* Gate = Module.FindInput(Entry.Get<0>()))
						{
							if (!Gate->bVisible || !Gate->bEditable)
							{
								continue;
							}
						}
						if (Defaults != nullptr)
						{
							if (const FInputValue* Default = Defaults->Find(Entry.Get<0>()))
							{
								if (Default->Equals(Entry.Get<1>()))
								{
									continue;
								}
							}
						}

						const FInputInfo* InputInfo = Module.FindInput(Entry.Get<0>());
						const FNiagaraTypeDefinition Type = InputInfo ? InputInfo->Type : FNiagaraTypeDefinition();
						Arguments.Add(FString::Printf(TEXT("%s = %s"),
							*ToInputIdentifier(Entry.Get<0>()),
							*ValueToSource(Context, ModuleAddress.WithInput(Entry.Get<0>()),
								Entry.Get<1>(), Type, 1)));
					}

					// Not the bare asset name: a short name that matches two modules re-imports as an
					// ambiguity error, which would make every export of InitializeParticle unusable.
					const FString ModuleSourceName = Module.Script
						? Modules.GetUnambiguousName(Module.Script, /*bDynamicInput=*/false)
						: Module.ModuleName.ToString();

					if (Arguments.Num() == 0)
					{
						Writer.Line(FString::Printf(TEXT("%s();"), *ModuleSourceName));
					}
					else
					{
						const FString OneLine = FString::Printf(TEXT("%s(%s);"),
							*ModuleSourceName, *FString::Join(Arguments, TEXT(", ")));
						if (OneLine.Len() <= 100 && !OneLine.Contains(LINE_TERMINATOR))
						{
							Writer.Line(OneLine);
						}
						else
						{
							Writer.Line(FString::Printf(TEXT("%s("), *ModuleSourceName));
							Writer.Push();
							for (int32 Index = 0; Index < Arguments.Num(); ++Index)
							{
								Writer.Line(Arguments[Index] + (Index + 1 < Arguments.Num() ? TEXT(",") : TEXT("")));
							}
							Writer.Pop();
							Writer.Line(TEXT(");"));
						}
					}

					if (!Module.bEnabled)
					{
						Result.UnsupportedFeatures.AddUnique(TEXT("disabled module"));
					}
				}

				Writer.Pop();
				Writer.Line(TEXT("}"));
				Writer.Blank();
			}

			for (const FRendererInfo& Renderer : Info.Renderers)
			{
				const FString TypeName = FNiagaraAdapter::RendererTypeNameForClass(Renderer.Class);
				Writer.Line(FString::Printf(TEXT("%sRenderer"),
					*TypeName.Replace(TEXT("Renderer"), TEXT(""), ESearchCase::CaseSensitive)));
				Writer.Line(TEXT("{"));
				Writer.Push();

				TArray<FString> Lines;
				FString Json;
				Errors.Reset();
				if (FNiagaraAdapter::GetRendererProperties(EmitterAddress.WithRenderer(Renderer.Index), Json, Errors))
				{
					FString DefaultsError;
					const FString* Defaults = Modules.GetRendererDefaults(Renderer.Class, DefaultsError);
					if (Defaults == nullptr)
					{
						Result.UnsupportedFeatures.AddUnique(TEXT("renderer defaults unavailable"));
					}

					// Renderers have no fixed setting list, so the mapping table used for emitters does
					// not apply (L8: property blocks are schema-driven). Every scalar field that
					// differs from a freshly added renderer of the same class is emitted.
					WriteChangedRendererProperties(Json, Defaults ? *Defaults : FString(), Lines,
						Result.UnsupportedFeatures);
					for (const FString& Line : Lines)
					{
						Writer.Line(Line);
					}
				}

				// Bindings are read from the live struct rather than the JSON blob: the serialised
				// form carries derived caches, and the bindable variable is the only part that means
				// anything in source.
				TArray<TPair<FString, FName>> Bindings;
				Errors.Reset();
				if (FNiagaraAdapter::GetRendererBindings(EmitterAddress.WithRenderer(Renderer.Index), Bindings, Errors))
				{
					FString DefaultsError;
					TArray<TPair<FString, FName>> DefaultBindings;
					Modules.GetRendererBindingDefaults(Renderer.Class, DefaultBindings, DefaultsError);

					bool bWroteBlank = false;
					for (const TPair<FString, FName>& Binding : Bindings)
					{
						if (Binding.Value.IsNone())
						{
							continue;
						}
						const TPair<FString, FName>* Default = DefaultBindings.FindByPredicate(
							[&Binding](const TPair<FString, FName>& Candidate) { return Candidate.Key == Binding.Key; });
						if (Default != nullptr && Default->Value == Binding.Value)
						{
							continue;
						}
						if (!bWroteBlank && Lines.Num() > 0)
						{
							Writer.Blank();
							bWroteBlank = true;
						}
						Writer.Line(FString::Printf(TEXT("Bind %s -> %s;"), *Binding.Key, *Binding.Value.ToString()));
					}
				}

				Writer.Pop();
				Writer.Line(TEXT("}"));
			}

			Writer.Pop();
			Writer.Line(TEXT("}"));
			Writer.Blank();
		}

		Writer.Pop();
		Writer.Line(TEXT("}"));

		Result.Source = Writer.Get();
		Result.bSucceeded = true;
		return Result;
	}
}
