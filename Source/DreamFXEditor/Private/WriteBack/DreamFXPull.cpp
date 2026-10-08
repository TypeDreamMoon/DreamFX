#include "WriteBack/DreamFXPull.h"

#include "Adapter/DreamFXNiagaraAdapter.h"
#include "DreamFXModule.h"
#include "DreamFXParser.h"
#include "Generation/DreamFXValueLowering.h"
#include "Schema/DreamFXModuleLibrary.h"
#include "SourceFiles/DreamFXPaths.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"

namespace UE::DreamFX::Editor
{
	namespace
	{
		/**
		 * One value out of a rapid-iteration store.
		 *
		 * Bytes rather than a typed value because that is what the store holds and what the comparison
		 * has to be made in: a `float 1.0` and a `float 1` and a `float 1.0000001` are three answers
		 * the comparison between two assets has always given in bytes (`DescribeSystemFacts` dumps
		 * them as hex for exactly this reason).
		 */
		struct FStoredValue
		{
			FNiagaraTypeDefinition Type;
			TArray<uint8> Bytes;

			/** A data interface or object reference: stored, but not a value this language spells. */
			bool bObject = false;

			/** The script it was read from, for the report when a name is stored more than once. */
			FString Script;
		};

		/**
		 * One node of a stack as the TEXT describes it.
		 *
		 * A run of consecutive assignments is one node, not n, because that is what it becomes: L2 folds
		 * them into a single Set Parameters module. The grouping is not a guess about where the fold
		 * breaks -- the asset says which of its nodes is a Set Parameters module, and a grouping that
		 * disagrees with it fails the correspondence check below.
		 */
		struct FTextNode
		{
			bool bSetParameters = false;
			/** The module call, for a node that is one. */
			const FStatement* Call = nullptr;
			FSourceLocation Location;
		};

		/** One literal to rewrite, in the coordinates of the file's own text. */
		struct FEdit
		{
			int32 Start = 0;
			int32 End = 0;
			FString Old;
			FString New;
			FString Address;
			FSourceLocation Location;
		};

		/** The six stacks pull addresses. The event and stage stacks are read through focus slices. */
		bool IsFixedStack(EStackKind Kind)
		{
			return Kind != EStackKind::EventHandler && Kind != EStackKind::SimulationStage;
		}

		/** Reads one parameter store into the index, keeping every copy of a name. */
		void CollectStore(const FString& ScriptLabel, const FNiagaraParameterStore& Store,
			TMap<FString, TArray<FStoredValue>>& Out)
		{
			const TArrayView<const FNiagaraVariableWithOffset> Variables = Store.ReadParameterVariables();
			const TArray<uint8>& Data = Store.GetParameterDataArray();

			for (const FNiagaraVariableWithOffset& Variable : Variables)
			{
				FStoredValue Value;
				Value.Type = Variable.GetType();
				Value.Script = ScriptLabel;

				if (Variable.IsDataInterface() || Variable.IsUObject()
					|| Variable.Offset < 0 || Variable.Offset + Variable.GetSizeInBytes() > Data.Num())
				{
					Value.bObject = true;
				}
				else
				{
					Value.Bytes.Append(Data.GetData() + Variable.Offset, Variable.GetSizeInBytes());
				}

				Out.FindOrAdd(Variable.GetName().ToString()).Add(MoveTemp(Value));
			}
		}

		/**
		 * Every rapid-iteration value of the system, keyed by full parameter name.
		 *
		 * Every script of every emitter plus the two system ones, into ONE map: a constant does not
		 * necessarily live in the script that declares the module. `EmitterState` is declared in an
		 * emitter's EmitterUpdate stack and its constants are stored in the SYSTEM update script
		 * (measured: `ri system-update Constants.Atlas2D_Mesh.EmitterState.Loop Delay`), so a lookup
		 * that followed the stack's script would miss the very inputs this exists to write back.
		 */
		void CollectRapidIteration(UNiagaraSystem* System, TMap<FString, TArray<FStoredValue>>& Out)
		{
			if (UNiagaraScript* Script = System->GetSystemSpawnScript())
			{
				CollectStore(TEXT("system spawn"), Script->RapidIterationParameters, Out);
			}
			if (UNiagaraScript* Script = System->GetSystemUpdateScript())
			{
				CollectStore(TEXT("system update"), Script->RapidIterationParameters, Out);
			}

			for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
			{
				const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
				if (Data == nullptr)
				{
					continue;
				}

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
					CollectStore(FString::Printf(TEXT("%s %s"), *Handle.GetName().ToString(), *Usage),
						Script->RapidIterationParameters, Out);
				}
			}
		}

		/**
		 * The one value the asset holds for a parameter name, or null when it holds none.
		 *
		 * A name can sit in more than one store -- an interpolated spawn script keeps a copy of the
		 * spawn stack's constants -- so every copy has to agree. Disagreement is refused rather than
		 * resolved by picking a script: which copy the engine reads is not something this can tell, and
		 * a wrong pick writes a number the asset does not actually hold.
		 */
		const FStoredValue* FindStored(const TMap<FString, TArray<FStoredValue>>& ValuesByName,
			const FString& ParameterName, FString& OutWhy)
		{
			const TArray<FStoredValue>* Found = ValuesByName.Find(ParameterName);
			if (Found == nullptr || Found->Num() == 0)
			{
				return nullptr;
			}

			const FStoredValue* First = &(*Found)[0];
			for (int32 Copy = 1; Copy < Found->Num(); ++Copy)
			{
				const FStoredValue& Other = (*Found)[Copy];
				if (Other.bObject != First->bObject || Other.Type != First->Type || Other.Bytes != First->Bytes)
				{
					OutWhy = FString::Printf(
						TEXT("the asset stores it in two scripts with different values ('%s' and '%s'), so which one the engine reads is not something pull can tell"),
						*First->Script, *Other.Script);
					return nullptr;
				}
			}
			return First;
		}

		/**
		 * The stored bytes as the value mode the language would write them in.
		 *
		 * An enum input is one case with two spellings: the store holds the entry INDEX, the language
		 * writes the entry NAME, and comparing them has to happen in the mode `Lower` produces or every
		 * enum input would look like a difference forever.
		 */
		bool MakeStoredInputValue(const FStoredValue& Stored, FInputValue& Out, FString& OutWhy)
		{
			if (Stored.bObject)
			{
				OutWhy = TEXT("the asset holds a data interface or object reference there, and pull writes literals only");
				return false;
			}

			if (Stored.Type.IsEnum())
			{
				UEnum* Enum = Stored.Type.GetEnum();
				if (Enum == nullptr || Stored.Bytes.Num() < static_cast<int32>(sizeof(int32)))
				{
					OutWhy = TEXT("the stored enum value is not an entry of a known enum");
					return false;
				}

				int32 Index = 0;
				FMemory::Memcpy(&Index, Stored.Bytes.GetData(), sizeof(int32));
				if (Index < 0 || Index >= Enum->NumEnums() - 1)
				{
					OutWhy = FString::Printf(TEXT("the stored enum index %d is not an entry of '%s'"),
						Index, *Enum->GetName());
					return false;
				}

				Out = FInputValue::MakeEnum(Enum, Enum->GetNameByIndex(Index));
				return true;
			}

			const UScriptStruct* Struct = Stored.Type.GetScriptStruct();
			if (Struct == nullptr || Stored.Bytes.Num() < Struct->GetStructureSize())
			{
				OutWhy = FString::Printf(TEXT("the stored '%s' value cannot be read as a literal"),
					*Stored.Type.GetName());
				return false;
			}

			Out = FInputValue::MakeLiteral(Struct, Stored.Bytes.GetData());
			return true;
		}

		/** The source spelling of a value the asset holds, or false with a reason it has none. */
		bool RenderStoredInputValue(const FInputValue& Value, const FStoredValue& Stored, FString& Out,
			FString& OutWhy)
		{
			if (Value.Mode == EInputValueMode::Enum)
			{
				Out = FValueLowering::EnumEntryToSourceToken(Value.EnumType, Value.EnumEntryName);
				if (Out.IsEmpty())
				{
					OutWhy = TEXT("the stored enum entry has no spelling this language reads back");
					return false;
				}
				return true;
			}

			if (!FValueLowering::LiteralToSource(Value, Stored.Type, Out))
			{
				OutWhy = FString::Printf(TEXT("the stored '%s' value has no literal spelling"),
					*Stored.Type.GetName());
				return false;
			}
			return true;
		}

		/** The first message a scratch sink collected, for a refusal that has to say why. */
		FString FirstMessage(const FDiagnosticSink& Sink)
		{
			const TArray<FDiagnostic>& Diagnostics = Sink.GetDiagnostics();
			return Diagnostics.Num() > 0 ? Diagnostics[0].Message : FString();
		}

		/** How a value mode reads in a message. The enum's own names are internal ("Literal"). */
		FString LexInputValueMode(EInputValueMode Mode)
		{
			switch (Mode)
			{
			case EInputValueMode::Linked:        return TEXT("a linked parameter");
			case EInputValueMode::Hlsl:          return TEXT("an inline hlsl expression");
			case EInputValueMode::DynamicInput:  return TEXT("a dynamic input");
			case EInputValueMode::DataInterface: return TEXT("a data interface configuration");
			case EInputValueMode::ObjectAsset:   return TEXT("an asset reference");
			case EInputValueMode::Unset:         return TEXT("nothing");
			default:                             return TEXT("a value mode pull does not rewrite");
			}
		}

		/** Where a rewritten file's backup goes: the same tree, under Saved, so nothing in DFX moves. */
		FString BackupPathFor(const FString& FilePath)
		{
			const FString Full = FPaths::ConvertRelativePathToFull(FilePath);
			FString Relative = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());

			FString Tail = Full;
			if (Full.StartsWith(Relative))
			{
				Tail = Full.RightChop(Relative.Len());
			}
			Tail.ReplaceInline(TEXT(":"), TEXT(""));
			return FPaths::ProjectSavedDir() / TEXT("DreamFX/Pull") / Tail;
		}
	}

	FPullResult FPuller::PullFile(const FString& FilePath, const FPullOptions& Options,
		FDiagnosticSink& Diagnostics)
	{
		FPullResult Result;
		Result.FilePath = FilePath;

		FString SourceText;
		if (!FFileHelper::LoadFileToString(SourceText, *FilePath))
		{
			Diagnostics.Error(TEXT("DFX1000"), FSourceLocation(),
				FString::Printf(TEXT("Could not read source file '%s'."), *FilePath));
			return Result;
		}

		FDocument Document;
		if (!FParser::ParseText(SourceText, FilePath, Document, Diagnostics))
		{
			// The parse errors are the answer: there is no AST to address literals through.
			return Result;
		}

		if (Document.Kind != EDocumentKind::System)
		{
			Diagnostics.Error(TEXT("DFX7108"), Document.HeaderLocation, FString::Printf(
				TEXT("pull reads a .dfs and the Niagara system it names: '%s' declares a %s document."),
				*FilePath, LexDocumentKind(Document.Kind)));
			return Result;
		}

		// The asset to read: the one the text names, resolved exactly the way a build resolves it, so
		// pull reads what this text writes unless the caller points it somewhere else.
		FString FullAssetPath;
		if (!Options.AssetOverride.IsEmpty())
		{
			FString Error;
			if (!FDreamFXPaths::ResolveAssetPath(Options.AssetOverride, Document.Root, FullAssetPath, Error))
			{
				Diagnostics.Error(TEXT("DFX7108"), Document.HeaderLocation,
					FString::Printf(TEXT("pull reads a .dfs and the Niagara system it names: %s"), *Error));
				return Result;
			}
		}
		else
		{
			FString MountPoint;
			FString Error;
			if (!FDreamFXPaths::ResolveRootMountPoint(Document.Root, MountPoint, Error))
			{
				Diagnostics.Error(TEXT("DFX7108"), Document.HeaderLocation,
					FString::Printf(TEXT("pull reads a .dfs and the Niagara system it names: %s"), *Error));
				return Result;
			}

			FString Relative = Document.Name;
			Relative.RemoveFromStart(TEXT("/"));
			FullAssetPath = MountPoint / Relative;
		}
		Result.AssetPath = FullAssetPath;

		UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *FDreamFXPaths::ToObjectPath(FullAssetPath));
		if (System == nullptr)
		{
			Diagnostics.Error(TEXT("DFX7108"), Document.HeaderLocation, FString::Printf(
				TEXT("pull reads a .dfs and the Niagara system it names: there is no Niagara system at '%s'. A file that has never been built names an asset that does not exist yet, and an export names its mirror -- -Asset=<path> reads a different one."),
				*FullAssetPath));
			return Result;
		}

		TMap<FString, TArray<FStoredValue>> Stored;
		CollectRapidIteration(System, Stored);

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

		// Every message this run produces, in order, so the -Apply report on disk is the same list.
		TArray<FString> ReportLines;
		auto Report = [&Diagnostics, &ReportLines](EDiagnosticSeverity Severity, const TCHAR* Code,
			const FSourceLocation& Location, const FString& Message)
		{
			Diagnostics.Add(Severity, Code, Location, Message);
			ReportLines.Add(Message);
		};

		TArray<FEdit> Edits;
		FNiagaraAdapter::FReadScope ReadScope(System);
		TArray<FString> Errors;

		/**
		 * One stack: match the text's nodes to the asset's one for one, then compare the arguments.
		 *
		 * The correspondence is verified and never assumed. Both sides are walked in execution order,
		 * a run of assignments has to land on a Set Parameters node exactly where the asset has one,
		 * and every module call's asset has to be the asset that node runs -- so a text that has drifted
		 * from its asset structurally fails here and nothing inside the stack is touched.
		 */
		auto ProcessStack = [&](FName EmitterName, const FStack& Stack, const FScriptStackInfo& AssetStack)
		{
			TArray<FTextNode> Nodes;
			for (const FStatement& Statement : Stack.Statements)
			{
				if (Statement.Kind == EStatementKind::Assignment)
				{
					if (Nodes.Num() > 0 && Nodes.Last().bSetParameters)
					{
						continue; // folded into the run above
					}
					FTextNode Node;
					Node.bSetParameters = true;
					Node.Location = Statement.Location;
					Nodes.Add(MoveTemp(Node));
					continue;
				}

				FTextNode Node;
				Node.Call = &Statement;
				Node.Location = Statement.Location;
				Nodes.Add(MoveTemp(Node));
			}

			const FString StackLabel = FString::Printf(TEXT("%s.%s"),
				EmitterName.IsNone() ? TEXT("System") : *EmitterName.ToString(), LexStackKind(Stack.Kind));

			auto Refuse = [&](const FString& Detail)
			{
				++Result.Unaddressable;
				Report(EDiagnosticSeverity::Warning, TEXT("DFX7109"), Stack.Location, FString::Printf(
					TEXT("pull: %s: %s. pull rewrites literals inside a structure both sides agree on, so nothing in this stack was addressed."),
					*StackLabel, *Detail));
			};

			if (Nodes.Num() != AssetStack.Modules.Num())
			{
				Refuse(FString::Printf(TEXT("the text declares %d node(s) and the asset's stack holds %d"),
					Nodes.Num(), AssetStack.Modules.Num()));
				return;
			}

			for (int32 Index = 0; Index < Nodes.Num(); ++Index)
			{
				const FTextNode& Node = Nodes[Index];
				const FModuleInfo& Module = AssetStack.Modules[Index];

				if (Node.bSetParameters != Module.bIsSetParameters)
				{
					Refuse(FString::Printf(
						TEXT("node %d is %s in the text and %s in the asset"),
						Index, Node.bSetParameters ? TEXT("a folded assignment run") : TEXT("a module call"),
						Module.bIsSetParameters ? TEXT("a Set Parameters module") : TEXT("an ordinary module")));
					return;
				}

				if (Node.bSetParameters)
				{
					continue;
				}

				FString ModuleError;
				UNiagaraScript* CallScript = Modules.FindModule(Node.Call->Name, ModuleError);
				if (CallScript == nullptr)
				{
					Refuse(FString::Printf(TEXT("node %d calls '%s', which does not resolve to a module asset: %s"),
						Index, *Node.Call->Name, *ModuleError));
					return;
				}
				if (Module.Script == nullptr || CallScript->GetPathName() != Module.Script->GetPathName())
				{
					Refuse(FString::Printf(TEXT("node %d calls '%s' but the asset's node %d runs '%s'"),
						Index, *CallScript->GetPathName(), Index,
						Module.Script != nullptr ? *Module.Script->GetPathName() : TEXT("(no script)")));
					return;
				}
				if (!Node.Call->InstanceName.IsEmpty()
					&& !Node.Call->InstanceName.Equals(Module.ModuleName.ToString(), ESearchCase::CaseSensitive))
				{
					Refuse(FString::Printf(
						TEXT("node %d is pinned to the name '%s' but the asset's node is called '%s'"),
						Index, *Node.Call->InstanceName, *Module.ModuleName.ToString()));
					return;
				}
			}

			// The correspondence holds. Now the values: every constant the asset stores under a node,
			// against the argument the text wrote for it.
			for (int32 Index = 0; Index < Nodes.Num(); ++Index)
			{
				const FTextNode& Node = Nodes[Index];
				if (Node.bSetParameters)
				{
					// A folded assignment writes a parameter, not a module input, and the text's own
					// kind of those is a different question from this one.
					continue;
				}

				const FModuleInfo& Module = AssetStack.Modules[Index];
				FString Prefix = TEXT("Constants.");
				if (!EmitterName.IsNone())
				{
					Prefix += EmitterName.ToString() + TEXT(".");
				}
				Prefix += Module.ModuleName.ToString() + TEXT(".");

				// The constants under this node, in the order the store lists them.
				TArray<TPair<FString, FString>> NodeStored; // identifier -> the input's real name
				for (const TPair<FString, TArray<FStoredValue>>& Entry : Stored)
				{
					if (!Entry.Key.StartsWith(Prefix, ESearchCase::CaseSensitive))
					{
						continue;
					}
					const FString InputName = Entry.Key.Mid(Prefix.Len());
					if (InputName.IsEmpty() || InputName.Contains(TEXT(".")))
					{
						continue; // a deeper name belongs to something below this node, not to an input
					}
					NodeStored.Emplace(NormalizeInputIdentifier(InputName), InputName);
				}

				// The store's own order is a hash map's, and a report whose lines move between runs
				// cannot be diffed against the last one.
				NodeStored.Sort([](const TPair<FString, FString>& Left, const TPair<FString, FString>& Right)
				{
					return Left.Value < Right.Value;
				});

				for (const TPair<FString, FString>& NodeEntry : NodeStored)
				{
					const FString& Identifier = NodeEntry.Key;
					const FString& InputName = NodeEntry.Value;
					const FString Address = FString::Printf(TEXT("%s.%s"),
						*Module.ModuleName.ToString(), *ToInputIdentifier(FName(*InputName)));

					auto CannotWrite = [&](const FString& Why)
					{
						++Result.Unwritable;
						Report(EDiagnosticSeverity::Info, TEXT("DFX7111"), Node.Call->Location,
							FString::Printf(TEXT("pull: %s.%s is declared but was not written back: %s"),
								*Module.ModuleName.ToString(), *Address, *Why));
					};

					// The argument the text wrote for it, matched the way the generator matches input
					// names. More than one match, on either side, is refused: `Scale RGB` and the inline
					// condition `ScaleRGB` are one identifier to this language.
					TArray<const FNamedArgument*> Arguments;
					for (const FNamedArgument& Argument : Node.Call->Arguments)
					{
						if (NormalizeInputIdentifier(Argument.Name) == Identifier)
						{
							Arguments.Add(&Argument);
						}
					}
					if (Arguments.Num() > 1)
					{
						CannotWrite(FString::Printf(
							TEXT("the call writes %d arguments that name this input"), Arguments.Num()));
						continue;
					}
					if (Arguments.Num() == 0)
					{
						++Result.Undeclared;
						Report(EDiagnosticSeverity::Info, TEXT("DFX7106"), Node.Call->Location, FString::Printf(
							TEXT("pull: '%s' holds a value for %s, which this text does not declare. pull rewrites the literals a call already has and never adds structure -- write the input into the text first, or let the build safety gate (DFX8017) handle what a rebuild would drop."),
							*Result.AssetPath, *Address));
						continue;
					}

					const FNamedArgument& Argument = *Arguments[0];
					if (!Argument.Value.IsValid())
					{
						CannotWrite(TEXT("the argument has no value"));
						continue;
					}

					FString Why;
					const FStoredValue* StoredValue = FindStored(Stored, Prefix + InputName, Why);
					if (StoredValue == nullptr)
					{
						CannotWrite(Why.IsEmpty() ? TEXT("the asset stores no value for it") : Why);
						continue;
					}

					FInputValue StoredInputValue;
					if (!MakeStoredInputValue(*StoredValue, StoredInputValue, Why))
					{
						CannotWrite(Why);
						continue;
					}

					FString NewText;
					if (!RenderStoredInputValue(StoredInputValue, *StoredValue, NewText, Why))
					{
						CannotWrite(Why);
						continue;
					}

					++Result.Compared;

					// What the text currently means, lowered the way a build would lower it. Comparing
					// values rather than characters is what keeps `0` and `0.0` from being a difference:
					// a text that already holds the asset's value must not be rewritten into its
					// spelling, or the second run would rewrite it again.
					FInputValue Existing;
					FDiagnosticSink Scratch;
					Scratch.SetFile(FilePath);
					if (!FValueLowering::Lower(*Argument.Value, StoredValue->Type, Address, Scratch, Existing))
					{
						const FString Message = FirstMessage(Scratch);
						CannotWrite(Message.IsEmpty()
							? TEXT("the text's value does not type-check against this input")
							: FString::Printf(TEXT("the text's value does not type-check against this input: %s"), *Message));
						continue;
					}

					if (Existing.Mode != EInputValueMode::Literal && Existing.Mode != EInputValueMode::Enum)
					{
						CannotWrite(FString::Printf(
							TEXT("the text writes it as %s, not as a literal, and changing the value MODE is an edit rather than a write-back"),
							*LexInputValueMode(Existing.Mode)));
						continue;
					}

					if (Existing.Equals(StoredInputValue))
					{
						continue; // the text already holds it
					}

					if (Argument.Value->StartOffset == INDEX_NONE
						|| Argument.Value->EndOffset <= Argument.Value->StartOffset
						|| Argument.Value->EndOffset > SourceText.Len())
					{
						CannotWrite(TEXT("the parser did not record a byte range for the written literal"));
						continue;
					}

					FEdit Edit;
					Edit.Start = Argument.Value->StartOffset;
					Edit.End = Argument.Value->EndOffset;
					Edit.Old = SourceText.Mid(Edit.Start, Edit.End - Edit.Start);
					Edit.New = NewText;
					Edit.Address = Address;
					Edit.Location = Argument.Value->Location;
					Edits.Add(MoveTemp(Edit));
					++Result.Changed;
				}
			}
		};

		// --- the system's own stacks ------------------------------------------------------------
		const FStackAddress SystemAddress(System);
		for (const FStack& Stack : Document.Stacks)
		{
			if (!IsFixedStack(Stack.Kind))
			{
				++Result.Unwritable;
				Report(EDiagnosticSeverity::Info, TEXT("DFX7111"), Stack.Location, FString::Printf(
					TEXT("pull: the %s stack is not addressed in v1 (an event or stage stack is read through a focus slice, which is a different mechanism); nothing in it was written back."),
					LexStackKind(Stack.Kind)));
				continue;
			}

			const FName ScriptName = FNiagaraAdapter::ScriptNameForStack(Stack.Kind);
			FScriptStackInfo Info;
			Errors.Reset();
			if (!FNiagaraAdapter::GetScriptStackInfo(SystemAddress.WithScript(ScriptName), Info, Errors))
			{
				++Result.Unaddressable;
				Report(EDiagnosticSeverity::Warning, TEXT("DFX7109"), Stack.Location, FString::Printf(
					TEXT("pull: the asset's %s could not be read (%s), so nothing in it was addressed."),
					*ScriptName.ToString(), *FString::Join(Errors, TEXT(" | "))));
				continue;
			}

			ProcessStack(NAME_None, Stack, Info);
		}

		// --- the emitters ----------------------------------------------------------------------
		FEmitterInfo EmitterInfo;
		for (const FEmitter& Emitter : Document.Emitters)
		{
			// `Emitter Foo` is a display name in the text and an FName key in the asset; the two are
			// the same string and different types.
			const FName EmitterName(*Emitter.Name);

			Errors.Reset();
			if (!FNiagaraAdapter::GetEmitterInfo(SystemAddress.WithEmitter(EmitterName), EmitterInfo, Errors))
			{
				++Result.Unaddressable;
				Report(EDiagnosticSeverity::Warning, TEXT("DFX7109"), Emitter.Location, FString::Printf(
					TEXT("pull: emitter '%s' could not be read from '%s' (%s), so nothing in it was addressed."),
					*Emitter.Name, *Result.AssetPath, *FString::Join(Errors, TEXT(" | "))));
				continue;
			}

			if (!Emitter.FromPath.IsEmpty())
			{
				// The asset's stack is the merged one, which is what the inline blocks below replace in
				// full -- so they are still addressable. Stacks this file does not declare are not.
				UE_LOG(LogDreamFX, Verbose,
					TEXT("pull: emitter '%s' is declared 'from' another file; only the stacks this file spells out are read."),
					*Emitter.Name);
			}

			for (const FStack& Stack : Emitter.Stacks)
			{
				if (!IsFixedStack(Stack.Kind))
				{
					++Result.Unwritable;
					Report(EDiagnosticSeverity::Info, TEXT("DFX7111"), Stack.Location, FString::Printf(
						TEXT("pull: the %s stack of emitter '%s' is not addressed in v1; nothing in it was written back."),
						LexStackKind(Stack.Kind), *Emitter.Name));
					continue;
				}

				const FName ScriptName = FNiagaraAdapter::ScriptNameForStack(Stack.Kind);
				const FScriptStackInfo* AssetStack = EmitterInfo.FindStack(ScriptName);
				if (AssetStack == nullptr)
				{
					++Result.Unaddressable;
					Report(EDiagnosticSeverity::Warning, TEXT("DFX7109"), Stack.Location, FString::Printf(
						TEXT("pull: emitter '%s' has no %s in the asset, so the stack this text declares there was not addressed."),
						*Emitter.Name, *ScriptName.ToString()));
					continue;
				}

				ProcessStack(EmitterName, Stack, *AssetStack);
			}
		}

		// --- report and, with -Apply, write ------------------------------------------------------
		if (Edits.Num() == 0)
		{
			Report(EDiagnosticSeverity::Info, TEXT("DFX7107"), Document.HeaderLocation, FString::Printf(
				TEXT("pull: '%s' already holds every value '%s' stores for the input(s) it declares (%d compared, %d not declared, %d not writable). No bytes written."),
				*FilePath, *Result.AssetPath, Result.Compared, Result.Undeclared, Result.Unwritable));
			Result.bSucceeded = true;
			return Result;
		}

		// Oldest first so the report reads down the file; the splice below sorts its own copy.
		Edits.Sort([](const FEdit& Left, const FEdit& Right) { return Left.Start < Right.Start; });

		for (const FEdit& Edit : Edits)
		{
			Report(EDiagnosticSeverity::Info, TEXT("DFX7105"), Edit.Location,
				FString::Printf(TEXT("pull: %s: %s -> %s"), *Edit.Address, *Edit.Old, *Edit.New));
		}

		if (!Options.bApply)
		{
			UE_LOG(LogDreamFX, Display,
				TEXT("pull: %d literal(s) would be written and nothing was: this is a dry run. Pass -Apply to write them into '%s'."),
				Edits.Num(), *FilePath);
			Result.bSucceeded = true;
			return Result;
		}

		// Descending, so each splice happens after every offset it could shift.
		FString NewSource = SourceText;
		Edits.Sort([](const FEdit& Left, const FEdit& Right) { return Left.Start > Right.Start; });
		for (const FEdit& Edit : Edits)
		{
			NewSource = NewSource.Left(Edit.Start) + Edit.New + NewSource.Mid(Edit.End);
		}

		// The rewrite is not trusted until the result parses. This is the one failure that has to be
		// impossible rather than unlikely: a text that no longer reads back is a source file nothing
		// can build, which is worse than the drift pull came to fix.
		{
			FDocument Checked;
			FDiagnosticSink Scratch;
			if (!FParser::ParseText(NewSource, FilePath, Checked, Scratch))
			{
				const FString Message = FirstMessage(Scratch);
				Diagnostics.Error(TEXT("DFX7110"), Document.HeaderLocation, FString::Printf(
					TEXT("pull: the rewritten text does not read back (%s), so '%s' was left untouched."),
					Message.IsEmpty() ? TEXT("parse failed") : *Message, *FilePath));
				return Result;
			}
		}

		// Back up before writing. The bytes that were there are the only record of what the editor
		// held, and they are not recoverable from the asset once a build has run.
		Result.BackupPath = BackupPathFor(FilePath);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Result.BackupPath), /*Tree=*/true);
		if (!FFileHelper::SaveStringToFile(SourceText, *Result.BackupPath))
		{
			Diagnostics.Error(TEXT("DFX7110"), Document.HeaderLocation, FString::Printf(
				TEXT("pull: the backup '%s' could not be written, so '%s' was left untouched."),
				*Result.BackupPath, *FilePath));
			return Result;
		}

		if (!FFileHelper::SaveStringToFile(NewSource, *FilePath))
		{
			Diagnostics.Error(TEXT("DFX7110"), Document.HeaderLocation, FString::Printf(
				TEXT("pull: '%s' could not be written; the bytes it held are in '%s'."),
				*FilePath, *Result.BackupPath));
			return Result;
		}
		Result.bWroteFile = true;

		UE_LOG(LogDreamFX, Display,
			TEXT("pull: wrote %d literal(s) into '%s'. The file it was is at '%s'."),
			Edits.Num(), *FilePath, *Result.BackupPath);

		// The report beside the backup: the log scrolls away and this is what says what changed.
		Result.ReportPath = FPaths::ChangeExtension(Result.BackupPath, TEXT("report.txt"));
		ReportLines.Insert(FString::Printf(TEXT("pull: %d literal(s) written into %s"), Edits.Num(), *FilePath), 0);
		FFileHelper::SaveStringArrayToFile(ReportLines, *Result.ReportPath);

		Result.bSucceeded = true;
		return Result;
	}
}
