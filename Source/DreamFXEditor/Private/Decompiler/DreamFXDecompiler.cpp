#include "DreamFXDecompiler.h"

#include "Adapter/DreamFXNiagaraAdapter.h"
#include "DreamFXModule.h"
#include "Generation/DreamFXValueLowering.h"
#include "Schema/DreamFXModuleLibrary.h"
#include "SourceFiles/DreamFXPaths.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "HAL/PlatformMemory.h"
#include "NiagaraDataInterfaceCurve.h"
#include "NiagaraEmitter.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/GCObjectScopeGuard.h"
#include "UObject/UObjectGlobals.h"

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

		// The export and the pin defaults the rebuild writes have to agree on how a float is spelled,
		// so there is one implementation and it lives next to the other shared spelling rules.
		FString FormatFloat(float Value)
		{
			return FormatFloatLossless(Value);
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
						const FString Token = FValueLowering::EnumEntryToSourceToken(Enum, Index);
						if (!Token.IsEmpty())
						{
							Out = Token;
							return true;
						}
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

		/**
		 * JSON text as a DSL string literal: compacted onto one line and escaped.
		 *
		 * Two characters need escaping and only two, because the lexer's default escape rule is "take
		 * the next character verbatim" -- so a quote needs one and a backslash needs one to stop it
		 * eating the quote that follows. Compacting is not cosmetic: the lexer ends a string at a
		 * newline, so a pretty-printed blob would not survive being read back.
		 */
		bool JsonTextToSourceString(const FString& JsonText, FString& OutLiteral)
		{
			// Re-serialised rather than passed through, so the same value always produces the same
			// bytes however the engine happened to format it -- which is what keeps a re-export of the
			// mirror identical to the export it came from.
			TSharedPtr<FJsonValue> Parsed;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
			if (!FJsonSerializer::Deserialize(Reader, Parsed) || !Parsed.IsValid())
			{
				return false;
			}

			FString Compact;
			const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Compact);

			if (Parsed->Type == EJson::Object)
			{
				if (!FJsonSerializer::Serialize(Parsed->AsObject().ToSharedRef(), Writer))
				{
					return false;
				}
			}
			else if (Parsed->Type == EJson::Array)
			{
				if (!FJsonSerializer::Serialize(Parsed->AsArray(), Writer))
				{
					return false;
				}
			}
			else
			{
				return false;
			}

			if (Compact.IsEmpty() || Compact.Contains(TEXT("\n")) || Compact.Contains(TEXT("\r")))
			{
				return false;
			}

			FString Escaped = Compact;
			Escaped.ReplaceInline(TEXT("\\"), TEXT("\\\\"), ESearchCase::CaseSensitive);
			Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""), ESearchCase::CaseSensitive);

			OutLiteral = FString::Printf(TEXT("\"%s\""), *Escaped);
			return true;
		}

		/**
		 * Limits on how far a dynamic input chain is expanded.
		 *
		 * A chain is read as a tree, one node per call, and nothing in the read API promises the walk
		 * terminates or stays small: a shared sub-chain reached down several paths is expanded once per
		 * path. On a third-party system in this project that turned into runaway expansion -- one core
		 * pinned, 38 GB resident and still climbing, no output. Refusing to expand past these limits
		 * turns that into a gap in the report, which is a bad export instead of a dead machine.
		 *
		 * Both are far above anything hand-authored: the deepest chain in this project's own content is
		 * 4, and a whole system expands a few hundred nodes.
		 */
		constexpr int32 MaxDynamicInputDepth = 24;
		constexpr int32 MaxDynamicInputExpansions = 20000;

		struct FContext
		{
			UNiagaraSystem* System = nullptr;
			FModuleLibrary* Modules = nullptr;
			FString RootToken;
			FString RootMountPoint;
			TArray<FString>* Unsupported = nullptr;

			/** Where lifted-out scratch pad scripts go; empty means "do not lift any out" (R3). */
			FString ExtractedScriptFolder;

			/** Diagnostic: print inputs even when they equal a pristine instance's. */
			bool bIncludeDefaultedInputs = false;

			/** Counts down across the whole document, not per chain: the blow-up is in the total. */
			mutable int32 ExpansionsRemaining = MaxDynamicInputExpansions;
		};

		/**
		 * The name to write for a module or dynamic input script, lifting it out of its owner first if
		 * that is the only way to have a name at all (plan-v5 R3).
		 *
		 * Returns empty when the script cannot be addressed and cannot be extracted either, which is
		 * route C: the caller drops the reference and records a gap, so the loss lands in the file
		 * header instead of in a path that resolves to a Niagara system.
		 */
		FString ScriptSourceName(const FContext& Context, UNiagaraScript* Script, bool bDynamicInput)
		{
			if (Script == nullptr)
			{
				return FString();
			}

			const FString Addressable = Context.Modules->FindAddressableName(Script, bDynamicInput);
			if (!Addressable.IsEmpty())
			{
				return Addressable;
			}

			if (Context.ExtractedScriptFolder.IsEmpty())
			{
				return FString();
			}

			// Prefixed with the owner, because two systems may each hold a scratch pad called
			// `ColorRamp` and they are not the same graph.
			const FString OwnerName = FPaths::GetBaseFilename(Script->GetOutermost()->GetName());
			const FString AssetName = FString::Printf(TEXT("%s_%s"), *OwnerName, *Script->GetName());

			FString Error;
			UNiagaraScript* Extracted = Context.Modules->MaterializeEmbeddedScript(
				Script, Context.ExtractedScriptFolder, AssetName, Error);
			if (Extracted == nullptr)
			{
				UE_LOG(LogDreamFX, Warning, TEXT("Could not extract embedded script '%s': %s"),
					*Script->GetName(), *Error);
				return FString();
			}

			// The full package path, deliberately, and not the shortest unambiguous name.
			//
			// A short name only resolves through the module index, and that index is built from the
			// *search paths* -- `/Niagara/Modules` and friends. An extracted script lands under
			// `Decompiled/`, which is not one of them, so the short name resolves in the process that
			// just wrote the file (which registered it) and in no other. The export would then build
			// only in the session that produced it, which is the worst kind of green.
			return Extracted->GetOutermost()->GetName();
		}

		/** Shortens an asset path that lives under the document's root. */
		FString RelativeAssetPath(const FContext& Context, const FString& PackagePath)
		{
			if (!Context.RootMountPoint.IsEmpty() && PackagePath.StartsWith(Context.RootMountPoint + TEXT("/")))
			{
				return PackagePath.RightChop(Context.RootMountPoint.Len() + 1);
			}
			return PackagePath;
		}

		/**
		 * The `Name="..."` the document header carries: the asset itself, or its mirror.
		 *
		 * A path that did not shorten is still absolute, which means the asset is not under this
		 * document's root at all. Rehoming that under `Decompiled/` would invent a location in the
		 * wrong mount point, so it is left alone and DFX8013 refuses the file if it is ever built.
		 */
		FString DocumentAssetName(const FContext& Context, const FString& PackagePath,
			const bool bDecompiledNamespace)
		{
			const FString Relative = RelativeAssetPath(Context, PackagePath);
			if (!bDecompiledNamespace || Relative.StartsWith(TEXT("/")))
			{
				return Relative;
			}
			return FDreamFXPaths::ToDecompiledNamespace(Relative);
		}

		FString ValueToSource(const FContext& Context, const FStackAddress& InputAddress,
			const FInputValue& Value, const FNiagaraTypeDefinition& Type, int32 IndentLevel);

		/**
		 * Rebuilds a dynamic input call, recursing into whatever hangs below it.
		 * (declaration order: defined below, called from ValueToSource)
		 *
		 * @param HostType  the type of the input this chain is plugged into, which is what the E4-1
		 *                  schema probe needs in order to see the chain's static switches.
		 */
		FString DynamicInputToSource(const FContext& Context, const FStackAddress& InputAddress,
			const FInputValue& Value, const FNiagaraTypeDefinition& HostType, int32 IndentLevel)
		{
			// A chain node whose script pointer is null has no name to write, and the placeholder this
			// used to emit -- `<unknown>()` -- is not a value the parser can read: it was the last
			// three syntax errors left in the four content packs (plan-v5 R2). An input with no source
			// form is dropped and recorded as a gap, the same rule the non-curve data interfaces
			// follow, so the loss shows up in the file header instead of as a file that will not parse.
			if (Value.DynamicInputAsset == nullptr)
			{
				if (Context.Unsupported != nullptr)
				{
					Context.Unsupported->AddUnique(TEXT("dynamic input whose script asset could not be resolved"));
				}
				UE_LOG(LogDreamFX, Warning,
					TEXT("A dynamic input at %s has no script asset; the assignment is omitted from the export."),
					*InputAddress.ModuleName.ToString());
				return FString();
			}

			// R3: a scratch pad dynamic input has no name until it is lifted out of the system.
			FString Name = ScriptSourceName(Context, Value.DynamicInputAsset, /*bDynamicInput=*/true);
			if (Name.IsEmpty())
			{
				if (Context.Unsupported != nullptr)
				{
					Context.Unsupported->AddUnique(
						TEXT("dynamic input stored inside the asset (scratch pad), which this export could not extract"));
				}
				return FString();
			}

			// R1b, the dynamic input half. Written only when it differs from the exposed version, for
			// the same reason a module's is: a pin on every call would be noise, and noise that breaks
			// on the next engine upgrade. The guid is kept because the schema below has to be read at
			// the same version -- that is what makes the child names and types the ones being written.
			FGuid LiveVersionGuid;
			{
				const FStackAddress EmitterAddress = FStackAddress(Context.System).WithEmitter(InputAddress.EmitterName);

				FScriptVersion LiveVersion;
				TArray<FString> VersionErrors;
				if (FNiagaraAdapter::GetDynamicInputScriptVersion(EmitterAddress, Value.DynamicInputAsset,
					LiveVersion, VersionErrors))
				{
					const FScriptVersion Exposed = FNiagaraAdapter::GetScriptVersion(Value.DynamicInputAsset);
					if (LiveVersion.bVersioningEnabled && LiveVersion.Guid.IsValid() && LiveVersion.Guid != Exposed.Guid)
					{
						LiveVersionGuid = LiveVersion.Guid;
						Name += FString::Printf(TEXT("@%s"), *LiveVersion.ToLabel());
					}
				}
				else if (Context.Unsupported != nullptr && Value.DynamicInputAsset->IsVersioningEnabled())
				{
					// Only reachable when one emitter calls the same dynamic input at two versions.
					// Recorded rather than guessed: picking one would silently retype half the calls.
					Context.Unsupported->AddUnique(
						TEXT("dynamic input called at more than one script version in the same emitter"));
				}
			}

			// The two runaway guards. Reported as gaps rather than swallowed: an export that silently
			// dropped half a chain would rebuild into a different effect and look like a DreamFX bug
			// somewhere else entirely.
			if (IndentLevel > MaxDynamicInputDepth || Context.ExpansionsRemaining <= 0)
			{
				const bool bTooDeep = IndentLevel > MaxDynamicInputDepth;
				if (Context.Unsupported != nullptr)
				{
					Context.Unsupported->AddUnique(bTooDeep
						? TEXT("dynamic input chain deeper than the expansion limit")
						: TEXT("dynamic input expansion budget exhausted"));
				}
				UE_LOG(LogDreamFX, Warning,
					TEXT("Stopped expanding dynamic input '%s': %s. The export names it with no arguments."),
					*Name, bTooDeep
						? TEXT("chain deeper than 24")
						: TEXT("more than 20000 nodes expanded in this document"));
				return FString::Printf(TEXT("%s()"), *Name);
			}
			--Context.ExpansionsRemaining;

			UE_LOG(LogDreamFX, Verbose, TEXT("    expanding dynamic input '%s' at depth %d (%d left)"),
				*Name, IndentLevel, Context.ExpansionsRemaining);

			TArray<FDynamicInputChild> Children;
			TArray<FString> Errors;
			FNiagaraAdapter::GetDynamicInputChildren(InputAddress, Children, Errors);

			UE_LOG(LogDreamFX, Verbose, TEXT("      '%s' has %d child(ren); probing its schema"),
				*Name, Children.Num());

			// The stack schema, which the generator now also plans against (plan-v3 E4-1). Both sides
			// have to agree on what names exist, or an export names an input the import cannot find.
			FString SchemaError;
			const FModuleSchema* Schema = Value.DynamicInputAsset
				? Context.Modules->GetDynamicInputStackSchema(Value.DynamicInputAsset, HostType,
					LiveVersionGuid, SchemaError)
				: nullptr;

			UE_LOG(LogDreamFX, Verbose, TEXT("      '%s' schema %s"), *Name,
				Schema != nullptr ? TEXT("read") : TEXT("unavailable"));

			// Two lists, joined switches-first (plan-v5 R1). The generator reorders on its own, so this
			// is not what makes a rebuild work -- it is what makes the file honest to read and to hand
			// edit: an argument list whose switch trails the input it reveals reads as though the order
			// did not matter, and someone will one day write a new call in that order.
			TArray<FString> SwitchArguments;
			TArray<FString> Arguments;
			for (const FDynamicInputChild& Child : Children)
			{
				const FName ChildName = Child.Name;

				// A non-editable input is one SetStackInputData refuses. Exporting it would produce a
				// file that cannot be rebuilt, so it is dropped -- it is at its default anyway,
				// because nothing could have written it.
				//
				// This applies to a static switch as much as to anything else, and exempting them was
				// tried and reverted: a switch sitting on an inactive branch of another switch is
				// itself hidden, and the export it produced was refused on rebuild with "该输入被静态
				// 开关/条件逻辑隐藏". Live visibility is the only thing that predicts a successful write.
				if (!Child.bVisible || !Child.bEditable)
				{
					continue;
				}

				// A name the importer cannot resolve would produce a file that does not rebuild, so
				// anything still missing from the schema is reported as a gap rather than written.
				const FInputSchema* Found = Schema ? Schema->FindInput(ChildName) : nullptr;
				if (Found == nullptr)
				{
					if (Context.Unsupported != nullptr)
					{
						Context.Unsupported->AddUnique(Child.bStaticSwitch
							? TEXT("dynamic input static switch")
							: TEXT("dynamic input child not in schema"));
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

				// The chain's own type wins over the schema's: on a static switch the live entry is
				// the authority on what the value's bytes mean.
				const FNiagaraTypeDefinition ChildType = Child.Type.IsValid() ? Child.Type : Found->Type;

				const FString ChildSource = ValueToSource(Context, ChildAddress, ChildValue, ChildType,
					IndentLevel + 1);
				if (ChildSource.IsEmpty())
				{
					continue; // no source form; the gap is already recorded in the header
				}

				(Child.bStaticSwitch ? SwitchArguments : Arguments).Add(
					FString::Printf(TEXT("%s = %s"), *ToInputIdentifier(ChildName), *ChildSource));
			}

			Arguments.Insert(SwitchArguments, 0);

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
				// The internal name is meaningless on user-defined enums and the label is what reads
				// best, but only a spelling the importer resolves back to this entry may be written.
				return FValueLowering::EnumEntryToSourceToken(Value.EnumType, Value.EnumEntryName);

			case EInputValueMode::Linked:
				return ToNameToken(Value.LinkedVariable.GetName().ToString());

			case EInputValueMode::Hlsl:
				return FString::Printf(TEXT("hlsl { %s }"), *Value.HlslExpression);

			case EInputValueMode::DynamicInput:
				// `Type` here is the type of the input the chain feeds, which is exactly the host the
				// schema probe has to reproduce.
				return DynamicInputToSource(Context, InputAddress, Value, Type, IndentLevel);

			case EInputValueMode::DataInterface:
			{
				FString Curve;
				if (CurveJsonToSource(Value.DataInterfaceJson, IndentLevel, Curve))
				{
					return Curve;
				}

				// plan-v5 R4 step 3. plan 3.5's v1 decision was "declare a data interface, do not
				// configure it", which on real content means exporting a mesh sampler or a grid with
				// none of its settings -- an effect that rebuilds and does nothing. The configuration
				// is JSON on both sides of the adapter already, so carrying it verbatim costs no
				// language surface and loses nothing; curves keep their readable `curve { }` form
				// because they have one.
				// ...with one exception, and it is not a small one: a configuration that points *back
				// into the system being read*.
				//
				// `V2/SubUVAnimation` binds itself to a specific sprite renderer, and the binding is a
				// reference to a subobject of the emitter it lives on:
				//   {"SpriteRenderer":{"refPath":"/Game/…/NS_X.NS_X:Black_3.NiagaraSpriteRendererProperties_2"}}
				// Carried verbatim, the mirror ends up holding a reference to the *original* asset's
				// private subobject, and SavePackage refuses that with `appError` -- which is not an
				// error DreamFX can report, it is the process dying. That is what stopped every full
				// build of the four packs partway through: 45 of these across the 11 _LevelUpSpawn
				// systems, and the first one reached takes the run down with it.
				//
				// Rewriting the path to the mirror's equivalent is not available either: the mirror's
				// subobjects are named independently (`Black_0` where the source has `Black_3`), so a
				// textual rewrite would produce a dangling reference instead of a crashing one. Until
				// intra-system references have a real representation (plan-v5 R4-3), this is a gap --
				// declared out loud in the file header, which is the whole point of having one.
				const FString OwnPackage = Context.System != nullptr
					? Context.System->GetOutermost()->GetName() : FString();
				if (!OwnPackage.IsEmpty() && Value.DataInterfaceJson.Contains(OwnPackage + TEXT(".")))
				{
					if (Context.Unsupported != nullptr)
					{
						Context.Unsupported->AddUnique(
							TEXT("data interface input referencing this system's own emitters or renderers"));
					}
					UE_LOG(LogDreamFX, Warning,
						TEXT("Dropping a data interface configuration on '%s': it references a subobject of the system being read, which a mirror cannot legally hold."),
						*InputAddress.ModuleName.ToString());
					return FString();
				}

				FString Blob;
				if (!Value.DataInterfaceJson.IsEmpty() && JsonTextToSourceString(Value.DataInterfaceJson, Blob))
				{
					return Blob;
				}

				if (Context.Unsupported != nullptr)
				{
					Context.Unsupported->AddUnique(TEXT("data interface input (non-curve)"));
				}

				// Empty means "no source form": the caller drops the whole assignment rather than
				// writing a half of one. This used to emit a comment in the value position, which is
				// not a value -- every export touching a data interface failed to parse, and the
				// language decision it was meant to express was stated only in a file that could not
				// be read back.
				return FString();
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
		/**
		 * Reads a dotted property path out of Object, or an invalid pointer if any segment is missing.
		 *
		 * The mirror of the generator's SetJsonFieldByPath: a setting whose property lives inside a
		 * struct, such as `Platforms.QualityLevelMask`, arrives as a nested object here too.
		 */
		TSharedPtr<FJsonValue> FindJsonValueByPath(const TSharedPtr<FJsonObject>& Object, const FString& Path)
		{
			if (!Object.IsValid())
			{
				return nullptr;
			}

			TArray<FString> Segments;
			Path.ParseIntoArray(Segments, TEXT("."));
			if (Segments.Num() <= 1)
			{
				return Object->TryGetField(Path);
			}

			TSharedPtr<FJsonObject> Current = Object;
			for (int32 Index = 0; Index < Segments.Num() - 1; ++Index)
			{
				const TSharedPtr<FJsonObject>* Child = nullptr;
				if (!Current->TryGetObjectField(Segments[Index], Child) || Child == nullptr || !Child->IsValid())
				{
					return nullptr;
				}
				Current = *Child;
			}
			return Current->TryGetField(Segments.Last());
		}

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
				const TSharedPtr<FJsonValue> Value = FindJsonValueByPath(Current, Mapping.Value);
				if (!Value.IsValid() || Value->IsNull())
				{
					continue;
				}

				if (Defaults.IsValid())
				{
					const TSharedPtr<FJsonValue> Default = FindJsonValueByPath(Defaults, Mapping.Value);
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
		/** "/Game/FX/M_X.M_X" -> "/Game/FX/M_X". The generator re-appends the object suffix on import. */
		FString ReferenceToPackagePath(const FString& ReferencePath)
		{
			FString PackagePath = ReferencePath;
			int32 Dot = INDEX_NONE;
			if (PackagePath.FindLastChar(TEXT('.'), Dot))
			{
				PackagePath.LeftInline(Dot);
			}
			return PackagePath;
		}

		/** The `{"refPath": "..."}` shape the external edit API round-trips object references through. */
		bool TryReadReferenceObject(const TSharedPtr<FJsonValue>& Value, FString& OutPackagePath)
		{
			FString ReferencePath;
			if (Value.IsValid() && Value->Type == EJson::Object && Value->AsObject().IsValid()
				&& Value->AsObject()->TryGetStringField(TEXT("refPath"), ReferencePath)
				&& ReferencePath.StartsWith(TEXT("/")))
			{
				OutPackagePath = ReferenceToPackagePath(ReferencePath);
				return true;
			}
			return false;
		}

		/**
		 * A JSON object that is really a number tuple, as the `(x, y)` literal the DSL already has.
		 *
		 * plan-v5 R4 step 1, and the largest single bucket in the coverage report: eight of the eleven
		 * "structured value" gaps were one thing wearing different names. `SubImageSize` (18 hits) is
		 * an FVector2D, `PivotInUVSpace` (7) is an FVector2D, `ColorAdd` (2) is an FLinearColor. The
		 * generator has always been able to write these -- `ValueToJson` emits exactly the X/Y and
		 * R/G/B/A shapes read here -- so the whole gap was the decompiler not recognising them on the
		 * way out.
		 *
		 * `SubImageSize` is the reason this is step 1 rather than step 4: losing it silently breaks
		 * every flipbook's sub-UV, which is a visible difference in the mirror that no amount of L1 or
		 * L2 green would have explained.
		 *
		 * Only all-numeric objects with exactly the expected key set convert. Anything else is still a
		 * gap, because a struct that half-matches is not a vector and guessing its remaining fields
		 * would be the silent-loss bug this whole pass exists to remove.
		 */
		bool TryWriteNumberTuple(const FString& PropertyName, const TSharedPtr<FJsonObject>& Object, FString& OutLiteral)
		{
			if (!Object.IsValid())
			{
				return false;
			}

			static const TCHAR* const Xy[]   = { TEXT("X"), TEXT("Y") };
			static const TCHAR* const Xyz[]  = { TEXT("X"), TEXT("Y"), TEXT("Z") };
			static const TCHAR* const Xyzw[] = { TEXT("X"), TEXT("Y"), TEXT("Z"), TEXT("W") };
			static const TCHAR* const Rgba[] = { TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A") };

			// A four-tuple in source carries no record of whether it meant XYZW or RGBA -- the text is
			// `(1, 0, 0, 1)` either way -- so the writer has to agree with how the reader will encode
			// it. The generator picks RGBA when the property name contains "Color" and XYZW otherwise
			// (ValueToJson), and a property whose JSON disagrees with that rule is left as a gap rather
			// than written as a tuple that would re-import into the wrong four fields.
			const bool bGeneratorWouldUseRgba = PropertyName.Contains(TEXT("Color"));

			const TCHAR* const* Names = nullptr;
			switch (Object->Values.Num())
			{
			case 2: Names = Xy; break;
			case 3: Names = Xyz; break;
			case 4:
				Names = Object->HasField(TEXT("R")) ? Rgba : Xyzw;
				if ((Names == Rgba) != bGeneratorWouldUseRgba)
				{
					return false;
				}
				break;
			default: return false;
			}

			const int32 Count = Object->Values.Num();
			TArray<FString> Parts;
			Parts.Reserve(Count);

			for (int32 Index = 0; Index < Count; ++Index)
			{
				const TSharedPtr<FJsonValue> Field = Object->TryGetField(Names[Index]);
				if (!Field.IsValid() || Field->Type != EJson::Number)
				{
					return false;
				}

				// Same integral rule the scalar properties use, so a value of 8 reads back as the int
				// literal it was and a re-export of the mirror is byte-identical.
				const double Number = Field->AsNumber();
				Parts.Add(FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number))
					? FString::Printf(TEXT("%lld"), static_cast<int64>(FMath::RoundToDouble(Number)))
					: FormatFloat(static_cast<float>(Number)));
			}

			OutLiteral = FString::Printf(TEXT("(%s)"), *FString::Join(Parts, TEXT(", ")));
			return true;
		}

		/**
		 * A structured value carried verbatim, as a quoted JSON string.
		 *
		 * plan-v5 R4 steps 2 and 4. `MaterialParameters` is an attribute-to-material-parameter binding
		 * table; `Meshes` elements carry a pivot and a scale beside the mesh; `Platforms` is a
		 * scalability filter; `UV0Settings` is a ribbon UV struct. Each is a different shape, none has
		 * a DSL spelling, and between them they are every remaining "structured value" gap in the four
		 * content packs.
		 *
		 * Designing a block syntax per struct is the readable answer and is what the plan asked for.
		 * This is not that -- it is the *lossless* answer, chosen because the alternative in front of
		 * us was continuing to drop them. A renderer property that survives as an opaque blob rebuilds
		 * into the same renderer; one that is recorded as a gap rebuilds into a different effect. When
		 * a struct earns real syntax it stops coming through here, and nothing else has to change:
		 * both sides key off "did a more specific rule already handle this".
		 *
		 * Deliberately compact (no pretty-printing) and single-line, because it has to fit a string
		 * literal, and the lexer stops a string at a newline.
		 */
		bool TryWriteJsonBlob(const TSharedPtr<FJsonValue>& Value, FString& OutLiteral)
		{
			if (!Value.IsValid() || (Value->Type != EJson::Object && Value->Type != EJson::Array))
			{
				return false;
			}

			FString Json;
			const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);

			const bool bSerialized = Value->Type == EJson::Object
				? FJsonSerializer::Serialize(Value->AsObject().ToSharedRef(), Writer)
				: FJsonSerializer::Serialize(Value->AsArray(), Writer);

			return bSerialized && JsonTextToSourceString(Json, OutLiteral);
		}

		/**
		 * An array of asset-carrying structs, as a plain array of paths.
		 *
		 * plan-v3 E4-3. `Meshes` and `OverrideMaterials` are arrays of structs whose only interesting
		 * field, in every case this project has, is the asset itself. Writing them as a path array is
		 * what makes a mesh renderer migrate at all -- but each element's remaining fields are compared
		 * against a default-constructed element first, so an entry with a custom pivot or scale is
		 * reported as a gap instead of being flattened away.
		 */
		bool TryWriteReferenceArray(const UClass* RendererClass, const FString& Key,
			const TArray<TSharedPtr<FJsonValue>>& Elements, TArray<FString>& OutLines)
		{
			if (Elements.Num() == 0)
			{
				return false;
			}

			FString ReferenceField;
			FString ElementDefaultsJson;
			TArray<FString> Errors;
			if (!FNiagaraAdapter::GetArrayElementReferenceField(
				RendererClass, Key, ReferenceField, ElementDefaultsJson, Errors))
			{
				return false;
			}

			TSharedPtr<FJsonObject> ElementDefaults;
			{
				const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ElementDefaultsJson);
				FJsonSerializer::Deserialize(Reader, ElementDefaults);
			}

			TArray<FString> Paths;
			bool bDroppedField = false;

			for (const TSharedPtr<FJsonValue>& Element : Elements)
			{
				if (!Element.IsValid() || Element->Type != EJson::Object || !Element->AsObject().IsValid())
				{
					return false;
				}

				const TSharedPtr<FJsonObject> Object = Element->AsObject();

				FString PackagePath;
				if (!TryReadReferenceObject(Object->TryGetField(ReferenceField), PackagePath))
				{
					// An element whose reference is unset is not representable as a path, and writing an
					// empty string would import as "no asset" on a different element index.
					return false;
				}
				Paths.Add(PackagePath);

				for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
				{
					if (Field.Key == ReferenceField || !Field.Value.IsValid())
					{
						continue;
					}
					const TSharedPtr<FJsonValue> Default = ElementDefaults.IsValid()
						? ElementDefaults->TryGetField(Field.Key) : nullptr;
					if (!Default.IsValid() || !FJsonValue::CompareEqual(*Field.Value, *Default))
					{
						bDroppedField = true;
					}
				}
			}

			// plan-v5 R4 step 4. The path array is the readable form and is exactly right when the
			// elements carry nothing but their asset -- which is the common case, and why it stays the
			// preferred one. When an element has a custom pivot or scale, the path array cannot say so,
			// and it used to be written anyway with the loss noted in the header. Refusing here hands
			// the property to the verbatim JSON rule instead: less readable, but the mesh renderer
			// rebuilds with the pivot the artist set.
			if (bDroppedField)
			{
				return false;
			}

			TArray<FString> Quoted;
			Quoted.Reserve(Paths.Num());
			for (const FString& Path : Paths)
			{
				Quoted.Add(FString::Printf(TEXT("\"%s\""), *Path));
			}
			OutLines.Add(FString::Printf(TEXT("%s = [%s];"), *Key, *FString::Join(Quoted, TEXT(", "))));
			return true;
		}

		void WriteChangedRendererProperties(const UClass* RendererClass, const FString& Json,
			const FString& DefaultsJson, TArray<FString>& OutLines, TArray<FString>& OutGaps)
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
				case EJson::Object:
				{
					// An asset reference comes back as {"refPath": "/Game/FX/M_X.M_X"} -- the shape the
					// external edit API's reference converter round-trips through. Reading it is what
					// makes the decompiler usable at all on real content: a coverage sweep of this
					// project found Material unexported on 17 of 20 systems, and a sprite renderer
					// without its material is not a migration, it is a white square.
					//
					// The trailing `.ShortName` is dropped because the generator re-appends it; the
					// source form is the package path.
					FString PackagePath;
					if (TryReadReferenceObject(Value, PackagePath))
					{
						OutLines.Add(FString::Printf(TEXT("%s = \"%s\";"), *Key, *PackagePath));
						break;
					}

					// R4 step 1: FVector2D / FLinearColor and friends, which the DSL writes as tuples.
					FString Tuple;
					if (TryWriteNumberTuple(Key, Value->AsObject(), Tuple))
					{
						OutLines.Add(FString::Printf(TEXT("%s = %s;"), *Key, *Tuple));
						break;
					}

					// R4 steps 2/4: everything else structured, carried verbatim rather than dropped.
					FString Blob;
					if (TryWriteJsonBlob(Value, Blob))
					{
						OutLines.Add(FString::Printf(TEXT("%s = %s;"), *Key, *Blob));
						break;
					}

					OutGaps.AddUnique(FString::Printf(TEXT("renderer property '%s' (structured value)"), *Key));
					break;
				}
				case EJson::Array:
				{
					if (TryWriteReferenceArray(RendererClass, Key, Value->AsArray(), OutLines))
					{
						break;
					}

					FString Blob;
					if (TryWriteJsonBlob(Value, Blob))
					{
						OutLines.Add(FString::Printf(TEXT("%s = %s;"), *Key, *Blob));
						break;
					}

					OutGaps.AddUnique(FString::Printf(TEXT("renderer property '%s' (structured value)"), *Key));
					break;
				}
				default:
					OutGaps.AddUnique(FString::Printf(TEXT("renderer property '%s' (structured value)"), *Key));
					break;
				}
			}
		}

		/**
		 * Must stay in step with `EmitterSettings` on the generator side -- the two tables are the two
		 * halves of one contract, and a setting present in only one of them is a silent loss.
		 *
		 * That is not hypothetical: the writer knew `InterpolatedSpawning` and `Enabled` and the reader
		 * did not, so a disabled emitter came back enabled and an interpolated one came back stepped,
		 * with nothing anywhere saying so. `RequiresPersistentIDs` was in neither, and Niagara refuses
		 * to compile a system that reads `Particles.ID` without it -- 8 of the 63 remaining rebuild
		 * errors were that one missing checkbox (plan-v5, item A).
		 *
		 * `FixedBounds` is deliberately absent: it is an FBox, and WriteChangedSettings only writes
		 * scalars. Listing it here would produce nothing and imply it round-trips.
		 */
		const TPair<const TCHAR*, const TCHAR*> EmitterSettingFields[] =
		{
			{ TEXT("SimTarget"),            TEXT("SimTarget") },
			{ TEXT("LocalSpace"),           TEXT("bLocalSpace") },
			{ TEXT("Determinism"),          TEXT("bDeterminism") },
			{ TEXT("RandomSeed"),           TEXT("RandomSeed") },
			{ TEXT("AllocationMode"),       TEXT("AllocationMode") },
			{ TEXT("PreAllocationCount"),   TEXT("PreAllocationCount") },
			{ TEXT("InterpolatedSpawning"), TEXT("InterpolatedSpawnMode") },
			{ TEXT("CalculateBoundsMode"),  TEXT("CalculateBoundsMode") },
			{ TEXT("RequiresPersistentIDs"),TEXT("bRequiresPersistentIDs") },
			{ TEXT("Enabled"),              TEXT("bIsEnabled") },
			// Found by diffing all 112 properties of one emitter against its mirror: 31 in the asset,
			// -1 in the export. Behaviourally the same while five quality levels exist, which is
			// exactly why it would never have shown up as a build failure.
			{ TEXT("QualityLevelMask"),     TEXT("Platforms.QualityLevelMask") },
		};

		const TPair<const TCHAR*, const TCHAR*> SystemSettingFields[] =
		{
			{ TEXT("WarmupTime"), TEXT("WarmupTime") },
		};

		/**
		 * Names the content roots this asset depends on that are not mounted in this process.
		 *
		 * A module whose script will not resolve is reported as "scratch pad or missing reference",
		 * which is true and was still misleading enough to send two rounds of diagnosis after a
		 * scratch-pad materialiser. The 17 unresolvable modules in NS_Spawn_Ninja_Root are
		 * `/NiagaraFluids/Modules/Grid3D/*`, and NiagaraFluids is simply not enabled in this project:
		 * nothing was embedded in the asset and nothing needs extracting, the plugin is off.
		 *
		 * Read from the asset registry rather than the linker, deliberately. The registry is built
		 * from on-disk headers, so it still knows a dependency whose package cannot be loaded --
		 * which is exactly the case being diagnosed -- and UNiagaraNodeFunctionCall's own
		 * FunctionScriptAssetObjectPath is Transient, so it is empty for anything loaded from disk.
		 */
		TArray<FString> FindUnmountedDependencyRoots(const UObject* Asset)
		{
			TArray<FString> Roots;
			if (Asset == nullptr)
			{
				return Roots;
			}

			const IAssetRegistry* Registry = IAssetRegistry::Get();
			if (Registry == nullptr)
			{
				return Roots;
			}

			const FString AssetPackageName = Asset->GetOutermost()->GetName();

			// The registry's discovery is asynchronous and a commandlet does not wait for it, so
			// asking straight away returns nothing and reads as "this asset depends on nothing".
			// Scanning the one directory is enough and costs a fraction of WaitForCompletion.
			FString AssetFileName;
			if (FPackageName::DoesPackageExist(AssetPackageName, &AssetFileName))
			{
				const_cast<IAssetRegistry*>(Registry)->ScanFilesSynchronous({ AssetFileName }, /*bForceRescan=*/false);
			}

			TArray<FName> Dependencies;
			Registry->GetDependencies(FName(*AssetPackageName), Dependencies,
				UE::AssetRegistry::EDependencyCategory::Package);

			for (const FName& Dependency : Dependencies)
			{
				const FString PackageName = Dependency.ToString();

				// Deliberately NOT guarded by IsValidLongPackageName: that validates the root against
				// the registered ones, so it rejects exactly the packages being looked for here and
				// the whole check silently found nothing.
				FString Root;
				FString Remainder;
				if (!PackageName.StartsWith(TEXT("/")) || !PackageName.RightChop(1).Split(TEXT("/"), &Root, &Remainder))
				{
					continue;
				}

				// A mount point of "" means no content root by that name is registered, which for a
				// plugin's content means the plugin is not enabled.
				if (!FPackageName::GetPackageMountPoint(PackageName).IsNone())
				{
					continue;
				}
				Roots.AddUnique(TEXT("/") + Root);
			}

			Roots.Sort();
			return Roots;
		}

		/**
		 * Adds one gap line naming the disabled content roots, when there is one to name.
		 *
		 * Deliberately only when the export already lost something: an asset that depends on an
		 * unmounted root but exported cleanly did not need it, and saying so would be noise.
		 */
		void NoteUnmountedDependencies(const UObject* Asset, TArray<FString>& UnsupportedFeatures)
		{
			if (UnsupportedFeatures.Num() == 0)
			{
				return;
			}

			const TArray<FString> Roots = FindUnmountedDependencyRoots(Asset);
			if (Roots.Num() == 0)
			{
				return;
			}

			UnsupportedFeatures.AddUnique(FString::Printf(
				TEXT("^ some of the above may be none of DreamFX's doing: this asset depends on %s, ")
				TEXT("which %s not mounted in this project. Enable the plugin that provides %s and export again."),
				*FString::Join(Roots, TEXT(", ")),
				Roots.Num() == 1 ? TEXT("is") : TEXT("are"),
				Roots.Num() == 1 ? TEXT("it") : TEXT("them")));
		}

		/**
		 * The comment block every export opens with, including what the export could not carry.
		 *
		 * plan-v3 E4-0. Until now an unrepresentable feature was a warning in a commandlet log, which
		 * is exactly the place nobody looks at again -- the file went on to be committed as if it were
		 * the whole asset. Writing the list into the file makes the loss survive into code review, and
		 * it is what E2's Export warning and Adopt refusal both read.
		 */
		FString FormatHeader(const FString& SourcePath, const TCHAR* Noun,
			const TArray<FString>& UnsupportedFeatures, const FString& MirrorPath)
		{
			const bool bMirror = !MirrorPath.IsEmpty();

			FString Header;
			Header += FString::Printf(TEXT("// Decompiled from %s by DreamFX.%s"),
				*SourcePath, LINE_TERMINATOR);
			Header += FString::Printf(
				TEXT("// Inline arithmetic is not recovered: expressions come back as equivalent hlsl { } blocks.%s"),
				LINE_TERMINATOR);

			// plan-v4 V1-6. What this file rebuilds used to be the asset it was read from, which is why
			// the whole Decompiled tree had to be kept out of the build. It now rebuilds a mirror, and
			// saying so is what tells a reader why editing here is safe -- and why the effect they see
			// in the level does not change until they point something at the mirror.
			if (bMirror)
			{
				Header += FString::Printf(
					TEXT("// Editing and saving this file rebuilds %s. The %s above is never modified.%s"),
					*MirrorPath, Noun, LINE_TERMINATOR);
			}

			if (UnsupportedFeatures.Num() == 0)
			{
				return Header;
			}

			// Sorted, not in discovery order: the same asset must produce the same bytes whichever
			// emitter happened to be walked first, or a re-export is a spurious diff.
			TArray<FString> Sorted = UnsupportedFeatures;
			Sorted.Sort();

			const FString NounText(Noun);
			const TCHAR* Article = (!NounText.IsEmpty() && FCString::Strchr(TEXT("aeiou"), FChar::ToLower(NounText[0])) != nullptr)
				? TEXT("an") : TEXT("a");

			Header += FString::Printf(TEXT("//%s"), LINE_TERMINATOR);
			Header += FString::Printf(
				TEXT("// NOT REPRESENTED IN THIS FILE -- %d feature(s) of the %s that DreamFXLang cannot%s"),
				Sorted.Num(), Noun, LINE_TERMINATOR);
			Header += bMirror
				? FString::Printf(TEXT("// express yet. The rebuilt mirror is %s %s WITHOUT them:%s"),
					Article, Noun, LINE_TERMINATOR)
				: FString::Printf(TEXT("// express yet. Rebuilding from this source produces %s %s WITHOUT them:%s"),
					Article, Noun, LINE_TERMINATOR);
			for (const FString& Feature : Sorted)
			{
				Header += FString::Printf(TEXT("//   - %s%s"), *Feature, LINE_TERMINATOR);
			}
			Header += FString::Printf(TEXT("//%s"), LINE_TERMINATOR);
			return Header;
		}

		/**
		 * A whole `Emitter ... { ... }` block, header line included.
		 *
		 * Split out so a .dfe export can reuse it verbatim (plan-v3 E2): a standalone emitter document
		 * differs from an emitter inside a system only in that header line, and keeping two writers for
		 * one body is how the two forms drift apart.
		 */
		/**
		 * @param bSystemScope  write only the stacks, with no wrapper block, no Settings and no
		 *                      renderers -- the shape the two system-scope stacks need.
		 *
		 * The system scope reuses this rather than getting a writer of its own because the module
		 * walk is the whole body of the function and a second copy of it would drift. It had no
		 * writer at all until plan-v5: `SystemSpawn` and `SystemUpdate` appeared 0 times across all
		 * 24 exports while `EmitterUpdate` appeared 199 times, so every Set Parameters at system
		 * scope was silently dropped -- which is why rebuilt systems failed to compile with
		 * "变量 Emitter.SubSize 在设置之前被读取": the module reading it survived, the one writing it
		 * did not. `EmitterAddress` is then the bare system address, which is exactly what
		 * GetScriptStackInfo wants.
		 */
		void WriteEmitterBlock(FWriter& Writer, const FString& HeaderLine, const FContext& Context,
			FModuleLibrary& Modules, const FStackAddress& EmitterAddress, const FEmitterInfo& Info,
			FDecompileResult& Result, FDiagnosticSink& Diagnostics, bool bSystemScope = false)
		{
			TArray<FString> Errors;

			if (!bSystemScope)
			{
				Writer.Line(HeaderLine);
				Writer.Line(TEXT("{"));
				Writer.Push();
			}

		if (!bSystemScope)
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

		// What a read of a parameter produces when nothing set it earlier in the stack. Written after
		// Settings and before the stacks because that is the order it applies in.
		{
			TArray<FParameterDefault> ParameterDefaults;
			Errors.Reset();
			if (FNiagaraAdapter::GetParameterDefaults(EmitterAddress, ParameterDefaults, Errors)
				&& ParameterDefaults.Num() > 0)
			{
				TArray<FString> Lines;
				for (const FParameterDefault& Default : ParameterDefaults)
				{
					const FString Name = ToNameToken(Default.Variable.GetName().ToString());
					const FString TypeName = FValueLowering::DescribeDeclaredType(Default.Variable.GetType());

					if (Default.Mode == FParameterDefault::EMode::Binding)
					{
						Lines.Add(FString::Printf(TEXT("%s %s = %s;"), *TypeName, *Name,
							*ToNameToken(Default.Binding.ToString())));
						continue;
					}
					if (Default.Mode != FParameterDefault::EMode::Value)
					{
						// Custom means a sub-graph computes the default, which has no text form.
						Result.UnsupportedFeatures.AddUnique(FString::Printf(
							TEXT("custom (sub-graph) default for parameter '%s'"),
							*Default.Variable.GetName().ToString()));
						continue;
					}

					const FString ValueSource = ValueToSource(Context,
						EmitterAddress, Default.Value, Default.Variable.GetType(), 0);
					if (ValueSource.IsEmpty())
					{
						Result.UnsupportedFeatures.AddUnique(FString::Printf(
							TEXT("default value for parameter '%s'"), *Default.Variable.GetName().ToString()));
						continue;
					}
					Lines.Add(FString::Printf(TEXT("%s %s = %s;"), *TypeName, *Name, *ValueSource));
				}

				if (Lines.Num() > 0)
				{
					Lines.Sort();
					Writer.Line(TEXT("Defaults = {"));
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
				// Tracing a walk that can stall: reading a stack costs a live probe per module and per
				// dynamic input, and when one of those does not come back there is nothing in the log
				// to say which asset, which stack or which module it was.
				// Run with -LogCmds="LogDreamFX Verbose".
				UE_LOG(LogDreamFX, Verbose, TEXT("  reading %s / %s"),
					*Stack.ScriptName.ToString(), *Module.ModuleName.ToString());

				// Between modules, not inside one: a collection here cannot run while a chain walk is
				// holding adapter results, and one module is a small enough step to keep the ceiling.
				FNiagaraAdapter::CollectIfHeavy();

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

						// The declared type is always written when it is known. It used to be omitted
						// for literals, on the theory that a literal lets the type be inferred on
						// re-import -- which is not true, because the interesting literals are
						// ambiguous. A four component tuple is a Vector4f, a LinearColor or a Quat; a
						// three component one is a Vector or a Position. `Emitter.ccC = (1.0, 0.43,
						// 0.3, 1.0)` came back as a Vector4f, was linked into HueShiftLinearColor's
						// LinearColor input, and the rebuild failed with DFX4027 on a link the
						// original asset makes happily.
						//
						// Inference could be taught which literals are ambiguous, but there is no
						// reason to: the real type is in hand right here, and writing it costs one
						// word and removes the whole class of bug.
						const FString Prefix = Type.IsValid()
							? FValueLowering::DescribeDeclaredType(Type) + TEXT(" ")
							: FString();

						const FString AssignedSource = ValueToSource(Context,
							ModuleAddress.WithInput(Entry.Get<0>()), Entry.Get<1>(), Type, 0);
						if (AssignedSource.IsEmpty())
						{
							// This used to `continue` on the claim that the gap was already in the
							// header. That is only true when ValueToSource recorded one, and a dropped
							// *assignment* is the one loss that cannot be allowed to go unrecorded:
							// every later read of the parameter becomes "read before set", which
							// Niagara refuses to compile, and the export gives no clue why. Record it
							// here so the claim is enforced rather than assumed.
							Result.UnsupportedFeatures.AddUnique(FString::Printf(
								TEXT("assignment to '%s' in %s (no source form for its value; anything "
								     "reading it later will not compile)"),
								*Entry.Get<0>().ToString(), *Module.ModuleName.ToString()));
							continue;
						}

						Writer.Line(FString::Printf(TEXT("%s%s = %s;"),
							*Prefix, *ToNameToken(Entry.Get<0>().ToString()), *AssignedSource));
					}
					continue;
				}

				// plan-v5 R1b. Which version this module is bound to, which is not necessarily the one
				// its asset now exposes: content outlives module revisions, and a revision renames and
				// retypes inputs. Everything below -- the defaults baseline, the `@` suffix on the call
				// -- hangs off this, because reading an authored module against a different version of
				// its own script produces a file that describes a module nobody has.
				FScriptVersion LiveVersion;
				bool bHasLiveVersion = false;
				if (Module.Script != nullptr)
				{
					Errors.Reset();
					bHasLiveVersion = FNiagaraAdapter::GetModuleScriptVersion(ModuleAddress, LiveVersion, Errors);
					if (!bHasLiveVersion)
					{
						UE_LOG(LogDreamFX, Warning, TEXT("Could not read the script version of module '%s': %s"),
							*Module.ModuleName.ToString(), *FString::Join(Errors, TEXT(" | ")));
					}
				}

				// A pin is written only when it says something: on an unversioned asset, or one whose
				// exposed version is the one in use, `@1.0` would be noise on every single call and
				// would turn the next engine upgrade into an error at every call site.
				const FScriptVersion ExposedVersion = FNiagaraAdapter::GetScriptVersion(Module.Script);
				const bool bVersionDrifted = bHasLiveVersion && LiveVersion.bVersioningEnabled
					&& LiveVersion.Guid.IsValid() && LiveVersion.Guid != ExposedVersion.Guid;

				// R8: only inputs that differ from a pristine instance of the same module are
				// printed. Without this every module dumps its entire input list.
				FString DefaultsError;
				const FGuid DefaultsVersion = bVersionDrifted ? LiveVersion.Guid : FGuid();
				const bool bWantDefaults = Module.Script && !Context.bIncludeDefaultedInputs;

				// Two baselines, and which one an input is judged against decides whether this export
				// can be rebuilt.
				//
				// An input's default can depend on a static switch -- EmitterState's LoopDuration is
				// 1.0 pristine and 5.0 once LoopBehavior is Once -- so a non-switch input has to be
				// compared against a module with this module's switches applied. Judged against the
				// pristine baseline, an authored 1.0 looks like the default, gets dropped, and the
				// rebuild (which does set LoopBehavior) produces 5.0. That was 9 of the 10 L1
				// mismatches.
				//
				// A switch, though, must keep the pristine baseline. Compared against a baseline that
				// already has it applied it always matches, so it would suppress itself; the export
				// would lose the switch, the rebuild would never set it, and every input it gates
				// would then not exist. That is the "no input named 'bUseMinDistance'" failure a
				// previous attempt at this produced, 3921 dropped lines and 36 broken assets.
				const TMap<FName, FInputValue>* PristineDefaults = bWantDefaults
					? Modules.GetStackDefaults(Module.Script, StackKind, DefaultsVersion, DefaultsError)
					: nullptr;

				TArray<TPair<FName, FInputValue>> SwitchValues;
				if (bWantDefaults)
				{
					for (const TTuple<FName, FInputValue>& Entry : Values)
					{
						const FInputInfo* Info = Module.FindInput(Entry.Get<0>());
						if (Info != nullptr && Info->bStaticSwitch && Entry.Get<1>().IsSet())
						{
							SwitchValues.Emplace(Entry.Get<0>(), Entry.Get<1>());
						}
					}
				}

				const TMap<FName, FInputValue>* SwitchedDefaults = PristineDefaults;
				if (bWantDefaults && SwitchValues.Num() > 0)
				{
					FString SwitchedError;
					if (const TMap<FName, FInputValue>* Probed = Modules.GetStackDefaultsForSwitches(
						Module.Script, StackKind, SwitchValues, DefaultsVersion, SwitchedError))
					{
						SwitchedDefaults = Probed;
					}
					else
					{
						// Falling back to the pristine baseline is the wrong answer, not a lesser one:
						// the note above says exactly what it costs -- an authored value that matches
						// the pristine default gets dropped, and the rebuild produces the switched one
						// instead. Silent until now. Measured at zero on the content packs, so this
						// says "not the cause of the current losses" rather than "cannot happen".
						UE_LOG(LogDreamFX, Warning,
							TEXT("Could not probe %s / %s with its %d switch(es) applied, so its inputs "
							     "are judged against the pristine defaults and any that match will be "
							     "dropped from the export: %s"),
							*Stack.ScriptName.ToString(), *Module.ModuleName.ToString(),
							SwitchValues.Num(), *SwitchedError);
					}
				}

				// Switches first; see the same split in DynamicInputToSource.
				TArray<FString> SwitchArguments;
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
					const FInputInfo* InputInfo = Module.FindInput(Entry.Get<0>());

					// See the two-baseline note above: switches against pristine, everything else
					// against a module carrying this module's switches.
					const bool bIsSwitchInput = InputInfo != nullptr && InputInfo->bStaticSwitch;

					// A static switch is always written, whatever it is worth.
					//
					// The rule everywhere else is "equal to the default, so the rebuild will produce it
					// anyway". For a switch that inference does not hold, because the two defaults are
					// not the same object: the one compared against here comes from probing a module,
					// and the one the rebuild lands on is whatever AddModule leaves on the node's pin.
					// They disagree, and nothing in the export says so.
					//
					// NS_Spawn_Ground_Root is the case that showed it. Its ParticleUpdate carries
					// SolveForcesAndVelocity(ManuallyEnableRotationalSolver = false), which matched the
					// probed default and was dropped; the rebuild then compiled the other branch, and
					// the interpolated spawn script failed with "Particles.MySize was read before being
					// set" -- a name that appears nowhere near this switch. Removing any one of the
					// four other inputs that export drops changes nothing; removing this one alone
					// reproduces the failure exactly.
					//
					// This does not need to know whether an author touched the value, which is what the
					// three earlier attempts tried to reconstruct and could not: a rebuilt asset keeps
					// no record of authorship. Presence is decided by what the input *is*, so the
					// mirror re-exports the same line and the round trip stays symmetric.
					//
					// Cost is ~290 lines per system in the worst case measured, against 508 non-switch
					// drops that stay dropped.
					if (const TMap<FName, FInputValue>* Defaults = bIsSwitchInput ? nullptr : SwitchedDefaults)
					{
						if (const FInputValue* Default = Defaults->Find(Entry.Get<0>()))
						{
							if (Default->Equals(Entry.Get<1>()))
							{
								// -DreamFXTraceSuppressed names every input this drops. The count is
								// not the interesting part -- most of these are genuinely untouched
								// inputs -- so it prints which baseline decided it, which is what
								// separates a correct drop from one judged against the wrong module.
								// "pristine-fallback" means no switch value was collected, so the
								// switched probe never ran; that is the case worth looking at first.
								if (FParse::Param(FCommandLine::Get(), TEXT("DreamFXTraceSuppressed")))
								{
									UE_LOG(LogDreamFX, Warning,
										TEXT("DFXTRACE-DROP %s / %s / %s (%s baseline)"),
										*Stack.ScriptName.ToString(), *Module.ModuleName.ToString(),
										*Entry.Get<0>().ToString(),
										SwitchedDefaults == PristineDefaults ? TEXT("pristine-fallback")
											: TEXT("switched"));
								}
								continue;
							}
						}
					}

					const FNiagaraTypeDefinition Type = InputInfo ? InputInfo->Type : FNiagaraTypeDefinition();

					const FString InputSource = ValueToSource(Context,
						ModuleAddress.WithInput(Entry.Get<0>()), Entry.Get<1>(), Type, 1);
					if (InputSource.IsEmpty())
					{
						continue; // no source form; the gap is already recorded in the header
					}

					const bool bIsSwitch = InputInfo != nullptr && InputInfo->bStaticSwitch;
					(bIsSwitch ? SwitchArguments : Arguments).Add(FString::Printf(TEXT("%s = %s"),
						*ToInputIdentifier(Entry.Get<0>()), *InputSource));
				}

				Arguments.Insert(SwitchArguments, 0);

				// Not the bare asset name: a short name that matches two modules re-imports as an
				// ambiguity error, which would make every export of InitializeParticle unusable.
				//
				// The `@version` suffix is new in plan-v5 R1b and is what plan-v2 W3 could not do.
				// It appears only on a module whose version is not the asset's exposed one, which in
				// the four content packs is five modules out of dozens -- and those five accounted for
				// 895 of the 1229 rebuild failures, because a rebuild silently used the newest version
				// and got a module with different input names and different input types.
				// R3: a scratch pad module is lifted out of the system first, or it has no name.
				//
				// A module with no script asset at all falls in here too, and used to be written as
				// its *stack* name -- `disabled SetFluidSourceAttributes()`. That name means something
				// to a reader and nothing to the importer, which reported it as DFX3001 "no module
				// named SetFluidSourceAttributes" on a file DreamFX had just written itself. A name
				// that cannot resolve is not a name; it is a gap, and it belongs in the header.
				FString ModuleSourceName = Module.Script
					? ScriptSourceName(Context, Module.Script, /*bDynamicInput=*/false)
					: FString();
				if (ModuleSourceName.IsEmpty())
				{
					Result.UnsupportedFeatures.AddUnique(FString::Printf(
						TEXT("module '%s' has no resolvable script asset (scratch pad or missing reference)"),
						*Module.ModuleName.ToString()));
					UE_LOG(LogDreamFX, Warning,
						TEXT("Module '%s' has no name an import could resolve; it is omitted from the export."),
						*Module.ModuleName.ToString());
					continue;
				}
				if (bVersionDrifted)
				{
					ModuleSourceName += FString::Printf(TEXT("@%s"), *LiveVersion.ToLabel());
				}

				// plan-v3 E4-2. The prefix rides on the call rather than on a separate statement so
				// a parked module keeps its arguments in the place they belong -- which is the whole
				// reason someone disables one instead of deleting it.
				const FString Prefix = Module.bEnabled ? FString() : FString(TEXT("disabled "));

				if (Arguments.Num() == 0)
				{
					Writer.Line(FString::Printf(TEXT("%s%s();"), *Prefix, *ModuleSourceName));
				}
				else
				{
					const FString OneLine = FString::Printf(TEXT("%s%s(%s);"),
						*Prefix, *ModuleSourceName, *FString::Join(Arguments, TEXT(", ")));
					if (OneLine.Len() <= 100 && !OneLine.Contains(LINE_TERMINATOR))
					{
						Writer.Line(OneLine);
					}
					else
					{
						Writer.Line(FString::Printf(TEXT("%s%s("), *Prefix, *ModuleSourceName));
						Writer.Push();
						for (int32 Index = 0; Index < Arguments.Num(); ++Index)
						{
							Writer.Line(Arguments[Index] + (Index + 1 < Arguments.Num() ? TEXT(",") : TEXT("")));
						}
						Writer.Pop();
						Writer.Line(TEXT(");"));
					}
				}
			}

			Writer.Pop();
			Writer.Line(TEXT("}"));
			Writer.Blank();
		}

		// A range-for over a ternary would bind a temporary array; the system scope simply has no
		// renderers to walk.
		for (const FRendererInfo& Renderer : Info.Renderers)
		{
			if (bSystemScope)
			{
				break;
			}
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
				WriteChangedRendererProperties(Renderer.Class, Json, Defaults ? *Defaults : FString(),
					Lines, Result.UnsupportedFeatures);
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
					Writer.Line(FString::Printf(TEXT("Bind %s -> %s;"),
						*ToNameToken(Binding.Key), *ToNameToken(Binding.Value.ToString())));
				}
			}

			Writer.Pop();
			Writer.Line(TEXT("}"));
			}

			if (!bSystemScope)
			{
				Writer.Pop();
				Writer.Line(TEXT("}"));
			}
		}
	}

	FDecompileResult FDecompiler::Decompile(UNiagaraSystem* System, const FString& RootToken,
		FDiagnosticSink& Diagnostics, const FDecompileOptions& Options)
	{
		FDecompileResult Result;
		if (System == nullptr)
		{
			Diagnostics.Error(TEXT("DFX8000"), FSourceLocation(), TEXT("Cannot decompile a null system."));
			return Result;
		}

		// The asset outlives every collection the walk triggers. Nothing else holds it: an asset the
		// commandlet loaded and is now reading is reachable from nowhere GC can see.
		FGCObjectScopeGuard SystemGuard(System);

		// One edit context for the whole read instead of one per adapter call. Sound here because
		// nothing below mutates this system -- the schema probe mutates a different one.
		FNiagaraAdapter::FReadScope ReadScope(System);

		FModuleLibrary Modules;

		FContext Context;
		Context.System = System;
		Context.Modules = &Modules;
		Context.RootToken = RootToken;
		Context.Unsupported = &Result.UnsupportedFeatures;
		Context.bIncludeDefaultedInputs = Options.bIncludeDefaultedInputs;
		{
			FString Error;
			FDreamFXPaths::ResolveRootMountPoint(RootToken, Context.RootMountPoint, Error);
		}

		FWriter Writer;
		TArray<FString> Errors;

		const FString PackagePath = System->GetOutermost()->GetName();
		const FString DocumentName = DocumentAssetName(Context, PackagePath, Options.bDecompiledNamespace);

		// R3. Beside the mirror the export rebuilds, under a Scripts/ folder: the extracted scripts
		// are products of this pipeline exactly as the mirror is, so they belong in the same namespace
		// and are deleted by the same sweep.
		if (Options.bMaterializeEmbeddedScripts && !Context.RootMountPoint.IsEmpty())
		{
			const FString MirrorRelative = FDreamFXPaths::ToDecompiledNamespace(
				RelativeAssetPath(Context, PackagePath));
			Context.ExtractedScriptFolder = Context.RootMountPoint / FPaths::GetPath(MirrorRelative) / TEXT("Scripts");
		}

		// The banner is composed after the body: the gap list is only complete once every stack,
		// renderer and binding has been walked, and a gap that only shows up in the log is a gap
		// nobody sees -- the file is what gets committed, reviewed and rebuilt from.
		Writer.Line(FString::Printf(TEXT("System(Name=\"%s\", Root=\"%s\")"),
			*DocumentName, *RootToken));
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
				// Sorted, because the order the API reports user variables in is not stable across a
				// rebuild: exporting a system, rebuilding from the export and exporting again produced
				// the same parameters in a different order, which breaks the idempotence the round-trip
				// contract rests on. Nothing is lost by sorting -- Group and SortPriority never reach
				// the asset (DFX5099), so declaration order carries no meaning to recover.
				UserVariables.Sort([](const FUserVariableInfo& Left, const FUserVariableInfo& Right)
				{
					return Left.Name.LexicalLess(Right.Name);
				});

				Writer.Line(TEXT("Properties = {"));
				Writer.Push();
				for (const FUserVariableInfo& Variable : UserVariables)
				{
					FString Name = Variable.Name.ToString();
					Name.RemoveFromStart(TEXT("User."), ESearchCase::IgnoreCase);
					Name = ToNameToken(Name);

					// The default is the whole value of a user parameter -- nothing in the stacks
					// assigns it, so a declaration without one is a parameter that reads as zero.
					// Dropping it is what made a rebuilt mirror simulate differently while exporting
					// to identical text: NS_SparkBurst's spawn rate is User.SparkRate, the original
					// carries 90.0, the mirror carried 0, and it emitted no particles at all. L1 could
					// not see it because the export it compares is the one missing the value, and L2
					// could not either because zero compiles.
					FString Line = FString::Printf(TEXT("%s %s"),
						*FValueLowering::DescribeDeclaredType(Variable.Type), *Name);

					FString DefaultText;
					if (Variable.DefaultValue.Mode == EInputValueMode::Literal
						&& LiteralToSource(Variable.DefaultValue, Variable.Type, DefaultText))
					{
						Line += FString::Printf(TEXT(" = %s"), *DefaultText);
					}
					else if (Variable.DefaultValue.Mode == EInputValueMode::Enum
						&& Variable.DefaultValue.EnumType != nullptr)
					{
						Line += FString::Printf(TEXT(" = %s"), *FValueLowering::EnumEntryToSourceToken(
							Variable.DefaultValue.EnumType, Variable.DefaultValue.EnumEntryName));
					}
					else if (Variable.DefaultValue.Mode == EInputValueMode::ObjectAsset
						&& Variable.DefaultValue.ObjectAsset != nullptr)
					{
						// The same spelling a renderer's Material already uses. An empty slot stays a
						// bare declaration, so "no asset" and "an asset we failed to record" do not
						// end up looking alike in the source.
						Line += FString::Printf(TEXT(" = \"%s\""),
							*Variable.DefaultValue.ObjectAsset->GetPathName());
					}
					// A data interface or object default has no inline spelling, so those still
					// declare bare -- the same state every user parameter used to be exported in.

					if (!Variable.Description.IsEmpty())
					{
						Line += FString::Printf(TEXT(" [ Description=\"%s\" ]"), *Variable.Description);
					}
					Writer.Line(Line + TEXT(";"));
				}
				Writer.Pop();
				Writer.Line(TEXT("}"));
				Writer.Blank();
			}
		}


		// --- system-scope stacks --------------------------------------------------------------
		//
		// These have no owning emitter, so GetEmitterInfo cannot reach them -- which is exactly why
		// GetScriptStackInfo exists, and why it had no caller until now. Everything an author writes
		// at system scope (Set Parameters feeding `Emitter.*`, spawn-rate drivers, system-level
		// modules) was absent from every export, and a mirror built from one failed to compile with
		// "read before set" on parameters whose only writer lived here.
		{
			const FStackAddress SystemScope(System);
			FEmitterInfo SystemInfo;

			for (EStackKind Kind : { EStackKind::SystemSpawn, EStackKind::SystemUpdate })
			{
				FScriptStackInfo StackInfo;
				Errors.Reset();
				if (FNiagaraAdapter::GetScriptStackInfo(
					SystemScope.WithScript(FNiagaraAdapter::ScriptNameForStack(Kind)), StackInfo, Errors))
				{
					SystemInfo.Stacks.Add(MoveTemp(StackInfo));
				}
				else
				{
					UE_LOG(LogDreamFX, Warning, TEXT("Could not read the %s stack: %s"),
						LexStackKind(Kind), *FString::Join(Errors, TEXT(" | ")));
				}
			}

			if (SystemInfo.Stacks.ContainsByPredicate(
				[](const FScriptStackInfo& Stack) { return Stack.Modules.Num() > 0; }))
			{
				WriteEmitterBlock(Writer, FString(), Context, Modules, SystemScope, SystemInfo,
					Result, Diagnostics, /*bSystemScope=*/true);
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
			UE_LOG(LogDreamFX, Verbose, TEXT("emitter %s"), *EmitterName.ToString());

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

			WriteEmitterBlock(Writer, FString::Printf(TEXT("Emitter %s"), *ToNameToken(EmitterName.ToString())),
				Context, Modules, EmitterAddress, Info, Result, Diagnostics);
			Writer.Blank();
		}

		Writer.Pop();
		Writer.Line(TEXT("}"));

		NoteUnmountedDependencies(System, Result.UnsupportedFeatures);

		Result.Source = FormatHeader(System->GetPathName(), TEXT("asset"), Result.UnsupportedFeatures,
			Options.bDecompiledNamespace ? Context.RootMountPoint / DocumentName : FString())
			+ Writer.Get();
		Result.bSucceeded = true;
		return Result;
	}

	FDecompileResult FDecompiler::DecompileEmitter(UNiagaraEmitter* Emitter, const FString& RootToken,
		FDiagnosticSink& Diagnostics, const FDecompileOptions& Options)
	{
		FDecompileResult Result;
		if (Emitter == nullptr)
		{
			Diagnostics.Error(TEXT("DFX8003"), FSourceLocation(), TEXT("Cannot decompile a null emitter."));
			return Result;
		}

		// /Temp is the engine's scratch mount: nothing under it is ever written to disk, so the host
		// leaves no trace even if the export fails partway through.
		TArray<FString> Errors;
		bool bCreated = false;
		UNiagaraSystem* Host = FNiagaraAdapter::AcquireSystem(
			TEXT("/Temp/DreamFX"), TEXT("DreamFXEmitterExportHost"), bCreated, Errors);
		if (Host == nullptr)
		{
			Diagnostics.Error(TEXT("DFX8004"), FSourceLocation(),
				FString::Printf(TEXT("Could not create a host system to read the emitter through: %s"),
					*FString::Join(Errors, TEXT(" | "))));
			return Result;
		}

		// A re-export in the same session would otherwise hit the previous copy's name.
		const FName EmitterName(*Emitter->GetName());
		{
			TArray<FName> Existing;
			TArray<FString> ReadErrors;
			if (FNiagaraAdapter::GetEmitterNames(Host, Existing, ReadErrors))
			{
				for (FName Name : Existing)
				{
					TArray<FString> RemoveErrors;
					FNiagaraAdapter::RemoveEmitter(FStackAddress(Host).WithEmitter(Name), RemoveErrors);
				}
			}
		}

		Errors.Reset();
		if (!FNiagaraAdapter::AddEmitterFromTemplate(Host, Emitter, EmitterName, Errors))
		{
			Diagnostics.Error(TEXT("DFX8005"), FSourceLocation(),
				FString::Printf(TEXT("Could not copy emitter '%s' into a host system: %s"),
					*EmitterName.ToString(), *FString::Join(Errors, TEXT(" | "))));
			return Result;
		}

		// Same reason as the system path: the host is transient and reachable from nowhere GC can see.
		FGCObjectScopeGuard HostGuard(Host);

		// Opened only now: the emitter was copied into the host above, and a shared context may not
		// span a mutation of the system it describes.
		FNiagaraAdapter::FReadScope ReadScope(Host);

		const FStackAddress EmitterAddress = FStackAddress(Host).WithEmitter(EmitterName);

		FEmitterInfo Info;
		Errors.Reset();
		if (!FNiagaraAdapter::GetEmitterInfo(EmitterAddress, Info, Errors))
		{
			Diagnostics.Error(TEXT("DFX8006"), FSourceLocation(),
				FString::Printf(TEXT("Could not read emitter '%s': %s"),
					*EmitterName.ToString(), *FString::Join(Errors, TEXT(" | "))));
			return Result;
		}

		FModuleLibrary Modules;

		FContext Context;
		Context.System = Host;
		Context.Modules = &Modules;
		Context.RootToken = RootToken;
		Context.Unsupported = &Result.UnsupportedFeatures;
		Context.bIncludeDefaultedInputs = Options.bIncludeDefaultedInputs;
		{
			FString Error;
			FDreamFXPaths::ResolveRootMountPoint(RootToken, Context.RootMountPoint, Error);
		}

		const FString PackagePath = Emitter->GetOutermost()->GetName();
		const FString DocumentName = DocumentAssetName(Context, PackagePath, Options.bDecompiledNamespace);

		// R3, same rule as the system path: a .dfe with an unextracted scratch pad does not rebuild.
		if (Options.bMaterializeEmbeddedScripts && !Context.RootMountPoint.IsEmpty())
		{
			const FString MirrorRelative = FDreamFXPaths::ToDecompiledNamespace(
				RelativeAssetPath(Context, PackagePath));
			Context.ExtractedScriptFolder = Context.RootMountPoint / FPaths::GetPath(MirrorRelative) / TEXT("Scripts");
		}

		FWriter Writer;
		WriteEmitterBlock(Writer,
			FString::Printf(TEXT("Emitter(Name=\"%s\", Root=\"%s\")"),
				*DocumentName, *RootToken),
			Context, Modules, EmitterAddress, Info, Result, Diagnostics);

		NoteUnmountedDependencies(Emitter, Result.UnsupportedFeatures);

		Result.Source = FormatHeader(Emitter->GetPathName(), TEXT("emitter"), Result.UnsupportedFeatures,
			Options.bDecompiledNamespace ? Context.RootMountPoint / DocumentName : FString())
			+ Writer.Get();
		Result.bSucceeded = true;
		return Result;
	}
}
