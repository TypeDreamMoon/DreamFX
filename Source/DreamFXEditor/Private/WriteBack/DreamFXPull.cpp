#include "WriteBack/DreamFXPull.h"

#include "Adapter/DreamFXNiagaraAdapter.h"
#include "DreamFXModule.h"
#include "DreamFXParser.h"
#include "Generation/DreamFXGenerator.h"
#include "Generation/DreamFXValueLowering.h"
#include "Schema/DreamFXModuleLibrary.h"
#include "SourceFiles/DreamFXPaths.h"
#include "WriteBack/DreamFXSourceValue.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::DreamFX::Editor
{
	namespace
	{
		/** Separates the two halves of a baseline key; no scope or key text can contain it. */
		const TCHAR* const BaselineSeparator = TEXT("\x1f");

		/** The alignment key of a folded run of assignments; no asset path can contain it. */
		const TCHAR* const SetParametersKey = TEXT("\x1fset");

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
			/** The folded run's statements, in file order, for a node that is one. */
			TArray<const FStatement*> Assignments;
			FSourceLocation Location;

			/** The module asset this node runs, as a path; the alignment key. */
			FString Key;

			/** The node's own extent in the file, for a structural edit. */
			int32 StartOffset = INDEX_NONE;
			int32 EndOffset = INDEX_NONE;
		};

		/** How one edit changes the file. */
		enum class EEditKind : uint8
		{
			/** Replace `[Start, End)` with `Text`: a literal, a binding target, a whole statement. */
			Replace,
			/** Insert `Text` at `Start`; `End == Start`. */
			Insert,
		};

		/** One change to the file, in the coordinates of the file's own text. */
		struct FEdit
		{
			EEditKind Kind = EEditKind::Replace;
			int32 Start = 0;
			int32 End = 0;
			FString Old;
			FString Text;
			FString Address;
			FSourceLocation Location;
		};

		/**
		 * Where every line starts, so a structural edit can talk about whole lines.
		 *
		 * A statement carries its own token extent, and an edit that removes one has to remove the line
		 * it sits on -- indentation, and the newline. Whether it may is a question about the rest of
		 * that line, and this is what can answer it.
		 */
		struct FSourceLines
		{
			explicit FSourceLines(const FString& InText) : Text(InText)
			{
				Starts.Add(0);
				for (int32 Index = 0; Index < Text.Len(); ++Index)
				{
					if (Text[Index] == TEXT('\n'))
					{
						Starts.Add(Index + 1);
					}
				}
			}

			/** The offset of the first character of the line holding Offset. */
			int32 LineStartOf(int32 Offset) const
			{
				int32 Low = 0;
				int32 High = Starts.Num() - 1;
				while (Low < High)
				{
					const int32 Mid = (Low + High + 1) / 2;
					if (Starts[Mid] <= Offset)
					{
						Low = Mid;
					}
					else
					{
						High = Mid - 1;
					}
				}
				return Starts[Low];
			}

			/** One past the newline that ends the line holding Offset, or the end of the text. */
			int32 LineEndOf(int32 Offset) const
			{
				int32 At = FMath::Clamp(Offset, 0, Text.Len());
				while (At < Text.Len() && Text[At] != TEXT('\n'))
				{
					++At;
				}
				return At < Text.Len() ? At + 1 : Text.Len();
			}

			/** The line's own ending, so an inserted line matches the file it joins. */
			FString NewlineAt(int32 Offset) const
			{
				const int32 LineStart = LineStartOf(Offset);
				const int32 LineEnd = LineEndOf(LineStart);
				if (LineEnd > LineStart && Text[LineEnd - 1] == TEXT('\n'))
				{
					return LineEnd >= 2 && Text[LineEnd - 2] == TEXT('\r') ? TEXT("\r\n") : TEXT("\n");
				}
				return TEXT("\n");
			}

			/** True when nothing but whitespace precedes Offset on its line. */
			bool StartsLine(int32 Offset) const
			{
				for (int32 At = LineStartOf(Offset); At < Offset; ++At)
				{
					if (Text[At] != TEXT(' ') && Text[At] != TEXT('\t'))
					{
						return false;
					}
				}
				return true;
			}

			/** True when nothing but whitespace follows Offset on its line -- no trailing comment. */
			bool EndsLine(int32 Offset) const
			{
				const int32 LineEnd = LineEndOf(Offset);
				for (int32 At = Offset; At < LineEnd; ++At)
				{
					if (Text[At] != TEXT(' ') && Text[At] != TEXT('\t')
						&& Text[At] != TEXT('\n') && Text[At] != TEXT('\r'))
					{
						return false;
					}
				}
				return true;
			}

			/** The indentation the line holding Offset carries; empty when it carries none. */
			FString IndentAt(int32 Offset) const
			{
				FString Indent;
				for (int32 At = LineStartOf(Offset); At < Offset; ++At)
				{
					if (Text[At] != TEXT(' ') && Text[At] != TEXT('\t'))
					{
						return FString();
					}
					Indent.AppendChar(Text[At]);
				}
				return Indent;
			}

			const FString& Text;

		private:
			TArray<int32> Starts;
		};

		/** The six stacks pull addresses. The event and stage stacks are read through focus slices. */
		bool IsFixedStack(EStackKind Kind)
		{
			return Kind != EStackKind::EventHandler && Kind != EStackKind::SimulationStage;
		}

		/**
		 * A stack's name without its enum qualification.
		 *
		 * The external edit API is not consistent about which spelling it uses: every ADDRESSING call
		 * takes the qualified enum name (`ENiagaraScriptUsage::ParticleSpawnScript`, which is what
		 * ScriptNameForStack answers and what the writes want), while the topology READ reports the
		 * short one (`ParticleSpawnScript`). Matching a read against a write therefore has to compare
		 * the unqualified halves, or every emitter stack looks absent from its own asset.
		 */
		FString ShortScriptName(const FString& Name)
		{
			int32 Colon = INDEX_NONE;
			return Name.FindLastChar(TEXT(':'), Colon) ? Name.RightChop(Colon + 1) : Name;
		}

		/** The asset's stack for one stack kind, matched on the unqualified name. */
		const FScriptStackInfo* FindAssetStack(const FEmitterInfo& Info, EStackKind Kind)
		{
			const FString Wanted = ShortScriptName(FNiagaraAdapter::ScriptNameForStack(Kind).ToString());
			for (const FScriptStackInfo& Candidate : Info.Stacks)
			{
				if (ShortScriptName(Candidate.ScriptName.ToString()) == Wanted)
				{
					return &Candidate;
				}
			}
			return nullptr;
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

		/**
		 * One stored value as the baseline spells it: the type, then hex bytes or a reference marker.
		 *
		 * Bytes rather than the rendered source, because this half of the baseline exists precisely so
		 * that two runs compare the SAME quantity the store holds -- the source spelling is a rendering
		 * of it, and comparing one rendering against another is a comparison of a spelling rule.
		 */
		FString BytesToBaseline(const FStoredValue& Stored)
		{
			if (Stored.bObject)
			{
				return FString::Printf(TEXT("obj:%s"), *Stored.Type.GetName());
			}

			FString Hex;
			Hex.Reserve(Stored.Bytes.Num() * 2);
			for (const uint8 Byte : Stored.Bytes)
			{
				Hex += FString::Printf(TEXT("%02X"), Byte);
			}
			return FString::Printf(TEXT("%s:%s"), *Stored.Type.GetName(), *Hex);
		}

		/**
		 * One JSON value as a baseline record: canonical, one line, and the same on every run.
		 *
		 * Not `FJsonValue::ToString` and not a serializer call for the scalars, because the record has
		 * to be comparable byte for byte across runs and across engine versions; `%.17g` is the exact
		 * round-trip form of a double, so a value that has not moved cannot read as moved.
		 */
		FString JsonToBaseline(const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value.IsValid() || Value->IsNull())
			{
				return TEXT("json:none");
			}

			switch (Value->Type)
			{
			case EJson::Number:  return FString::Printf(TEXT("num:%.17g"), Value->AsNumber());
			case EJson::Boolean: return Value->AsBool() ? TEXT("bool:true") : TEXT("bool:false");
			case EJson::String:  return FString::Printf(TEXT("str:%s"), *Value->AsString());
			default: break;
			}

			FString Json;
			const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
			const bool bSerialized = Value->Type == EJson::Object
				? FJsonSerializer::Serialize(Value->AsObject().ToSharedRef(), Writer)
				: FJsonSerializer::Serialize(Value->AsArray(), Writer);
			return bSerialized ? FString::Printf(TEXT("json:%s"), *Json) : TEXT("json:?");
		}

		/** A record's field in the baseline file: one line, no tabs, escaped where it must be. */
		FString EscapeBaselineField(const FString& Text)
		{
			FString Out = Text;
			Out.ReplaceInline(TEXT("\\"), TEXT("\\\\"), ESearchCase::CaseSensitive);
			Out.ReplaceInline(TEXT("\t"), TEXT("\\t"), ESearchCase::CaseSensitive);
			Out.ReplaceInline(TEXT("\r"), TEXT("\\r"), ESearchCase::CaseSensitive);
			Out.ReplaceInline(TEXT("\n"), TEXT("\\n"), ESearchCase::CaseSensitive);
			return Out;
		}

		void UnescapeBaselineField(FString& Text)
		{
			// One pass, left to right: a second pass over the result would turn the `\\n` that an
			// escaped backslash followed by an `n` produces back into a newline.
			FString Out;
			Out.Reserve(Text.Len());
			for (int32 Index = 0; Index < Text.Len(); ++Index)
			{
				if (Text[Index] == TEXT('\\') && Index + 1 < Text.Len())
				{
					const TCHAR Next = Text[++Index];
					Out.AppendChar(Next == TEXT('t') ? TEXT('\t')
						: Next == TEXT('r') ? TEXT('\r')
						: Next == TEXT('n') ? TEXT('\n')
						: Next);
					continue;
				}
				Out.AppendChar(Text[Index]);
			}
			Text = MoveTemp(Out);
		}

		/**
		 * True when the two JSON values mean the same thing to this language.
		 *
		 * Deliberately not `FJsonValue::CompareEqual`, and the difference is a whole class of false
		 * positives: the asset's JSON comes from a property converter that widened a `float` to a
		 * `double`, so a property holding `0.1f` reads back as `0.10000000149011612` while the text's
		 * `0.1` lowers to `0.1`. Compared exactly, every such property would read as changed on every
		 * run and pull would rewrite a line that already said what the asset holds -- which is the one
		 * thing a write-back with nothing to do may never do. The comparison is therefore made at the
		 * precision the language can write: exact for integers, `float` for everything else.
		 *
		 * The cost is stated rather than hidden: two doubles that differ below `float` precision are
		 * one value here, because they are one value to Niagara. No property this reads is a double.
		 */
		bool JsonValuesEquivalent(const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right)
		{
			if (!Left.IsValid() || !Right.IsValid())
			{
				return !Left.IsValid() && !Right.IsValid();
			}
			if (Left->Type != Right->Type)
			{
				return false;
			}

			switch (Left->Type)
			{
			case EJson::Number:
			{
				const double A = Left->AsNumber();
				const double B = Right->AsNumber();
				const bool bBothIntegral = FMath::IsNearlyEqual(A, FMath::RoundToDouble(A))
					&& FMath::IsNearlyEqual(B, FMath::RoundToDouble(B));
				return bBothIntegral
					? static_cast<int64>(FMath::RoundToDouble(A)) == static_cast<int64>(FMath::RoundToDouble(B))
					: static_cast<float>(A) == static_cast<float>(B);
			}

			case EJson::Boolean:
				return Left->AsBool() == Right->AsBool();

			case EJson::String:
				return Left->AsString() == Right->AsString();

			case EJson::Array:
			{
				const TArray<TSharedPtr<FJsonValue>>& A = Left->AsArray();
				const TArray<TSharedPtr<FJsonValue>>& B = Right->AsArray();
				if (A.Num() != B.Num())
				{
					return false;
				}
				for (int32 Index = 0; Index < A.Num(); ++Index)
				{
					if (!JsonValuesEquivalent(A[Index], B[Index]))
					{
						return false;
					}
				}
				return true;
			}

			case EJson::Object:
			{
				const TSharedPtr<FJsonObject>& A = Left->AsObject();
				const TSharedPtr<FJsonObject>& B = Right->AsObject();
				if (!A.IsValid() || !B.IsValid() || A->Values.Num() != B->Values.Num())
				{
					return false;
				}
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : A->Values)
				{
					if (!JsonValuesEquivalent(Entry.Value, B->TryGetField(Entry.Key)))
					{
						return false;
					}
				}
				return true;
			}

			default:
				return FJsonValue::CompareEqual(*Left, *Right);
			}
		}

		/** Parses a properties JSON. A failure is the caller's to report, never silently empty. */
		bool ParsePropertiesJson(const FString& Json, TSharedPtr<FJsonObject>& Out)
		{
			Out.Reset();
			if (Json.IsEmpty())
			{
				return false;
			}
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
			return FJsonSerializer::Deserialize(Reader, Out) && Out.IsValid();
		}

		/**
		 * One correspondence between a text's nodes and an asset stack's.
		 *
		 * `None` is the refusal: the two sides do not agree in a way that can be pinned down, so nothing
		 * inside the stack is addressed. `Aligned` carries one entry per ASSET node -- the index of the
		 * text node it corresponds to, or INDEX_NONE for a node the text does not have -- plus the
		 * number of pairs, which is what says whether the text has nodes the asset does not.
		 */
		struct FCorrespondence
		{
			enum class EKind : uint8 { None, Aligned };

			EKind Kind = EKind::None;
			/** Size == asset node count; entry < 0 means "the asset has this, the text does not". */
			TArray<int32> AssetToText;
			/** How many text nodes found a partner. */
			int32 Matched = 0;
			/** The reason, when Kind is None. */
			FString Why;
		};

		/**
		 * The unique alignment of two key sequences, or a refusal.
		 *
		 * The alignment is the longest common subsequence, and it is only accepted when it is the ONLY
		 * one: a match is forced when every common subsequence of the maximal length has to pass through
		 * it, which is the standard `prefix + 1 + suffix == total` test applied to the two dynamic
		 * programming tables. An alignment that is merely *an* LCS is an alignment somebody guessed --
		 * two instances of one module and a third removed give several -- and a structural edit made
		 * through a guess puts a module somewhere else in the execution order, which is a different
		 * effect rather than a differently written one.
		 *
		 * Cost is O(n*m) time and memory, which for a stack (the largest one measured in this project
		 * holds 43 module nodes) is nothing.
		 */
		FCorrespondence AlignNodes(const TArray<FString>& TextKeys, const TArray<FString>& AssetKeys)
		{
			FCorrespondence Result;

			const int32 N = TextKeys.Num();
			const int32 M = AssetKeys.Num();

			TArray<int32> Prefix;
			Prefix.SetNumZeroed((N + 1) * (M + 1));
			auto P = [&Prefix, M](int32 I, int32 J) -> int32& { return Prefix[I * (M + 1) + J]; };

			for (int32 I = 1; I <= N; ++I)
			{
				for (int32 J = 1; J <= M; ++J)
				{
					P(I, J) = TextKeys[I - 1] == AssetKeys[J - 1]
						? P(I - 1, J - 1) + 1
						: FMath::Max(P(I - 1, J), P(I, J - 1));
				}
			}
			const int32 Total = P(N, M);

			TArray<int32> Suffix;
			Suffix.SetNumZeroed((N + 1) * (M + 1));
			auto S = [&Suffix, M](int32 I, int32 J) -> int32& { return Suffix[I * (M + 1) + J]; };

			for (int32 I = N - 1; I >= 0; --I)
			{
				for (int32 J = M - 1; J >= 0; --J)
				{
					S(I, J) = TextKeys[I] == AssetKeys[J]
						? S(I + 1, J + 1) + 1
						: FMath::Max(S(I + 1, J), S(I, J + 1));
				}
			}

			TArray<TPair<int32, int32>> Forced;
			for (int32 I = 0; I < N; ++I)
			{
				for (int32 J = 0; J < M; ++J)
				{
					if (TextKeys[I] == AssetKeys[J] && P(I, J) + 1 + S(I + 1, J + 1) == Total)
					{
						Forced.Emplace(I, J);
					}
				}
			}

			// A unique alignment has exactly one forced match per pair, strictly increasing on both
			// sides. Anything else -- too many, too few, or out of order -- means more than one LCS.
			if (Forced.Num() != Total)
			{
				Result.Why = FString::Printf(
					TEXT("%d of the module calls can be paired up, and %d of those pairs can be located without ambiguity"),
					Total, Forced.Num());
				return Result;
			}
			for (int32 Index = 1; Index < Forced.Num(); ++Index)
			{
				if (Forced[Index].Key <= Forced[Index - 1].Key || Forced[Index].Value <= Forced[Index - 1].Value)
				{
					Result.Why = TEXT("there is more than one way to line the two lists up, so which statement is which module cannot be decided");
					return Result;
				}
			}

			Result.Kind = FCorrespondence::EKind::Aligned;
			Result.Matched = Total;
			Result.AssetToText.Init(INDEX_NONE, M);
			for (const TPair<int32, int32>& Match : Forced)
			{
				Result.AssetToText[Match.Value] = Match.Key;
			}
			return Result;
		}
	}

	bool FPullBaseline::Find(const FString& Scope, const FString& Key, FString& OutValue) const
	{
		const FString* Found = Records.Find(Scope + BaselineSeparator + Key);
		if (Found == nullptr)
		{
			return false;
		}
		OutValue = *Found;
		return true;
	}

	void FPullBaseline::Set(const FString& Scope, const FString& Key, const FString& Value)
	{
		Records.Add(Scope + BaselineSeparator + Key, Value);
	}

	bool FPullBaseline::IsDirty(const FString& Scope, const FString& Key, const FString& Now) const
	{
		FString Recorded;
		return !Find(Scope, Key, Recorded) || Recorded != Now;
	}

	bool FPullBaseline::Load(const FString& InPath, FString& OutWhy)
	{
		Path = InPath;
		bLoaded = false;
		Records.Reset();

		if (!FPaths::FileExists(InPath))
		{
			OutWhy = FString::Printf(TEXT("there is no baseline at '%s' yet"), *InPath);
			return true; // not an error: a first run has nothing to compare against
		}

		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *InPath))
		{
			OutWhy = FString::Printf(TEXT("'%s' could not be read"), *InPath);
			return false;
		}

		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, /*InCullEmpty=*/true);
		int32 LineNumber = 0;
		for (FString& Line : Lines)
		{
			++LineNumber;
			if (Line.StartsWith(TEXT("#")))
			{
				continue;
			}
			TArray<FString> Fields;
			Line.ParseIntoArray(Fields, TEXT("\t"));
			if (Fields.Num() != 3)
			{
				OutWhy = FString::Printf(TEXT("'%s' line %d is not a record"), *InPath, LineNumber);
				return false;
			}
			UnescapeBaselineField(Fields[1]);
			UnescapeBaselineField(Fields[2]);
			Records.Add(Fields[0] + BaselineSeparator + Fields[1], Fields[2]);
		}

		bLoaded = true;
		return true;
	}

	bool FPullBaseline::Save(FString& OutWhy) const
	{
		TArray<FString> Lines;
		Lines.Add(TEXT("# DreamFX pull baseline -- what the asset held at the last -Apply."));
		Lines.Add(TEXT("# scope<TAB>key<TAB>value. Delete this file to make the next pull compare every declared value."));

		// Sorted, because an unsorted file diffs as a change every time it is written, and being able to
		// diff two runs is the whole point of keeping it.
		TArray<FString> Keys;
		Records.GetKeys(Keys);
		Keys.Sort();
		for (const FString& Key : Keys)
		{
			int32 At = INDEX_NONE;
			if (!Key.FindChar(BaselineSeparator[0], At))
			{
				continue;
			}
			Lines.Add(FString::Printf(TEXT("%s\t%s\t%s"),
				*Key.Left(At), *EscapeBaselineField(Key.Mid(At + 1)), *EscapeBaselineField(Records[Key])));
		}

		if (Path.IsEmpty())
		{
			OutWhy = TEXT("the baseline has no path");
			return false;
		}
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree=*/true);
		if (!FFileHelper::SaveStringArrayToFile(Lines, *Path))
		{
			OutWhy = FString::Printf(TEXT("'%s' could not be written"), *Path);
			return false;
		}
		return true;
	}

	bool FPuller::PullText(const FString& SourceText, const FString& FilePath, UNiagaraSystem* SystemOverride,
		const FPullOptions& Options, FPullBaseline& Baseline, FDiagnosticSink& Diagnostics,
		FPullResult& Result, FString& OutNewText)
	{
		OutNewText = SourceText;
		Result.FilePath = FilePath;

		FDocument Document;
		if (!FParser::ParseText(SourceText, FilePath, Document, Diagnostics))
		{
			// The parse errors are the answer: there is no AST to address literals through.
			return false;
		}

		if (Document.Kind != EDocumentKind::System)
		{
			Diagnostics.Error(TEXT("DFX7108"), Document.HeaderLocation, FString::Printf(
				TEXT("pull reads a .dfs and the Niagara system it names: '%s' declares a %s document."),
				*FilePath, LexDocumentKind(Document.Kind)));
			return false;
		}

		// The asset to read: the one the text names, resolved exactly the way a build resolves it, so
		// pull reads what this text writes unless the caller points it somewhere else.
		UNiagaraSystem* System = SystemOverride;
		if (System == nullptr)
		{
			FString FullAssetPath;
			if (!Options.AssetOverride.IsEmpty())
			{
				FString Error;
				if (!FDreamFXPaths::ResolveAssetPath(Options.AssetOverride, Document.Root, FullAssetPath, Error))
				{
					Diagnostics.Error(TEXT("DFX7108"), Document.HeaderLocation,
						FString::Printf(TEXT("pull reads a .dfs and the Niagara system it names: %s"), *Error));
					return false;
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
					return false;
				}

				FString Relative = Document.Name;
				Relative.RemoveFromStart(TEXT("/"));
				FullAssetPath = MountPoint / Relative;
			}
			Result.AssetPath = FullAssetPath;

			System = LoadObject<UNiagaraSystem>(nullptr, *FDreamFXPaths::ToObjectPath(FullAssetPath));
			if (System == nullptr)
			{
				Diagnostics.Error(TEXT("DFX7108"), Document.HeaderLocation, FString::Printf(
					TEXT("pull reads a .dfs and the Niagara system it names: there is no Niagara system at '%s'. A file that has never been built names an asset that does not exist yet, and an export names its mirror -- -Asset=<path> reads a different one."),
					*FullAssetPath));
				return false;
			}
		}
		else
		{
			Result.AssetPath = System->GetPathName();
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

		const FSourceLines Lines(SourceText);

		// Every message this run produces, in order, so the report on disk is the same list.
		//
		// `Note` records a line and hands the same string back, which keeps every diagnostic a literal
		// `Diagnostics.<Severity>(TEXT("DFXnnnn"), ...)` with its message in the same statement:
		// `.skill/gen-diagnostics.ps1` finds codes by that spelling, and a code it cannot find is a code
		// that quietly stops being documented.
		TArray<FString>& ReportLines = Result.Report;
		auto Note = [&ReportLines](const FString& Message)
		{
			ReportLines.Add(Message);
			return Message;
		};

		TArray<FEdit> Edits;
		FNiagaraAdapter::FReadScope ReadScope(System);
		TArray<FString> Errors;

		// What this run SEES, gathered as it goes and handed back to the caller. Only addresses pull
		// actually considered are in it, which is the honest scope: a value the text does not declare is
		// not something pull has an opinion about.
		TMap<FString, FString>& Observed = Result.ObservedRecords;

		/**
		 * The dirty-set test every write goes through, so that "only what the user moved" is one rule
		 * in one place rather than a condition repeated at each of the four write paths.
		 *
		 * Every address pull COMPARES is recorded, dirty or not: a value it declines to write is still a
		 * value the asset holds now, and the next run has to be able to tell that it has not moved
		 * since. Without that, declining would mean "ask again next time", forever.
		 */
		auto Record = [&](const FString& Scope, const FString& Key, const FString& Now)
		{
			Observed.Add(Scope + BaselineSeparator + Key, Now);
		};

		auto ShouldWrite = [&](const FString& Scope, const FString& Key, const FString& Now,
			const FSourceLocation& Location, const FString& Address) -> bool
		{
			Record(Scope, Key, Now);
			if (!Options.bUseBaseline || Baseline.IsDirty(Scope, Key, Now))
			{
				return true;
			}

			++Result.Withheld;
			Diagnostics.Info(TEXT("DFX7115"), Location, Note(FString::Printf(
				TEXT("pull: %s differs from this text but the asset has not moved since the last apply, so the text's value stands."),
				*Address)));
			return false;
		};

		/** An edit that replaces the characters a literal already occupies. */
		auto AddReplacement = [&](const FValue& Value, const FString& NewText, const FString& Address) -> bool
		{
			if (Value.StartOffset == INDEX_NONE
				|| Value.EndOffset <= Value.StartOffset
				|| Value.EndOffset > SourceText.Len())
			{
				return false;
			}

			FEdit Edit;
			Edit.Kind = EEditKind::Replace;
			Edit.Start = Value.StartOffset;
			Edit.End = Value.EndOffset;
			Edit.Old = SourceText.Mid(Edit.Start, Edit.End - Edit.Start);
			Edit.Text = NewText;
			Edit.Address = Address;
			Edit.Location = Value.Location;
			Edits.Add(MoveTemp(Edit));
			++Result.Changed;
			return true;
		};

		/**
		 * Values inside one property block, matched by property name.
		 *
		 * Shared by a `Settings` block and a renderer's properties, because the only thing that differs
		 * between them is how a key becomes JSON and how the asset's value is spelled -- and both of
		 * those are the generator's and the decompiler's answers, not this function's.
		 *
		 * @param Scope         the address prefix the baseline files these values under.
		 * @param bSystemScope  which settings table the keys are looked up in; ignored for a renderer.
		 * @param PropertyClass for a renderer block; null for a Settings block.
		 */
		auto ProcessProperties = [&](const TArray<FPropertyEntry>& Properties, const FString& Scope,
			const FString& Label, bool bSystemScope, const UClass* PropertyClass,
			const FString& RawJson, const TSharedPtr<FJsonObject>& AssetJson)
		{
			for (const FPropertyEntry& Property : Properties)
			{
				const FString Address = FString::Printf(TEXT("%s.%s"), *Label, *Property.Name);

				auto CannotWrite = [&](const FSourceLocation& Location, const FString& Why)
				{
					++Result.Unwritable;
					Diagnostics.Info(TEXT("DFX7111"), Location, Note(FString::Printf(
						TEXT("pull: %s is declared but was not written back: %s"), *Address, *Why)));
				};

				if (!Property.Value.IsValid())
				{
					CannotWrite(Property.Location, TEXT("the property has no value"));
					continue;
				}

				// The asset's value at this key, through the same table the plan writes through: a
				// nested setting (`Platforms.QualityLevelMask`) is one int inside an object, and only
				// the table knows the path.
				const TSharedPtr<FJsonValue> StoredValue = PropertyClass != nullptr
					? (AssetJson.IsValid() ? AssetJson->TryGetField(Property.Name) : nullptr)
					: FindJsonPropertyByPath(AssetJson, Property.Name);

				if (!StoredValue.IsValid() || StoredValue->IsNull())
				{
					CannotWrite(Property.Location, TEXT("the asset holds nothing under that name"));
					continue;
				}

				// What the text means, in the asset's own terms. Comparing anything else -- the
				// characters, or a rendering of them -- reads a plugin-relative path or a float widened
				// by the property converter as a change that is not one.
				TSharedPtr<FJsonValue> TextValue;
				if (!LowerDeclaredPropertyToJson(Property, bSystemScope, Document.Root, PropertyClass, TextValue))
				{
					CannotWrite(Property.Location,
						TEXT("the value cannot be converted to the asset's own representation, so whether it says the same thing cannot be decided"));
					continue;
				}

				FString NewText;
				FString Why;
				const bool bRendered = PropertyClass != nullptr
					? RenderJsonPropertyAsSource(PropertyClass, Property.Name, StoredValue, NewText, Why)
					: RenderSettingSource(Property.Name, bSystemScope, RawJson, NewText, Why);
				if (!bRendered)
				{
					CannotWrite(Property.Location, Why.IsEmpty()
						? TEXT("the asset's value has no settled spelling in this language") : Why);
					continue;
				}

				++Result.Compared;
				const FString BaselineValue = JsonToBaseline(StoredValue);

				if (JsonValuesEquivalent(TextValue, StoredValue))
				{
					Record(Scope, Property.Name, BaselineValue);
					continue; // the text already says it
				}

				if (!ShouldWrite(Scope, Property.Name, BaselineValue, Property.Value->Location, Address))
				{
					continue;
				}

				if (!AddReplacement(*Property.Value, NewText, Address))
				{
					CannotWrite(Property.Value->Location, TEXT("the parser did not record a byte range for the written literal"));
				}
			}
		};

		// --- the system's own stacks ------------------------------------------------------------
		const FStackAddress SystemAddress(System);

		/**
		 * One stack: match the text's nodes to the asset's one for one, then compare the arguments.
		 *
		 * The correspondence is verified and never assumed. Both sides are walked in execution order,
		 * a run of assignments has to land on a Set Parameters node exactly where the asset has one,
		 * and every module call's asset has to be the asset that node runs -- so a text that has drifted
		 * from its asset structurally fails here and nothing inside the stack is touched. Under
		 * `-Structure` the correspondence is allowed to be non-trivial, but only when it is the UNIQUE
		 * alignment of the two node lists; anything less is still a refusal.
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
						FTextNode& Run = Nodes.Last();
						Run.Assignments.Add(&Statement);
						Run.EndOffset = Statement.EndOffset;
						continue;
					}
					FTextNode Node;
					Node.bSetParameters = true;
					Node.Location = Statement.Location;
					Node.Assignments.Add(&Statement);
					Node.StartOffset = Statement.StartOffset;
					Node.EndOffset = Statement.EndOffset;
					Nodes.Add(MoveTemp(Node));
					continue;
				}

				FTextNode Node;
				Node.Call = &Statement;
				Node.Location = Statement.Location;
				Node.StartOffset = Statement.StartOffset;
				Node.EndOffset = Statement.EndOffset;
				Nodes.Add(MoveTemp(Node));
			}

			const FString StackLabel = FString::Printf(TEXT("%s.%s"),
				EmitterName.IsNone() ? TEXT("System") : *EmitterName.ToString(), LexStackKind(Stack.Kind));

			auto Refuse = [&](const FString& Detail)
			{
				++Result.Unaddressable;
				Diagnostics.Warning(TEXT("DFX7109"), Stack.Location, Note(FString::Printf(
					TEXT("pull: %s: %s. pull rewrites literals inside a structure both sides agree on, so nothing in this stack was addressed."),
					*StackLabel, *Detail)));
			};

			// The alignment keys: the module asset a call runs, and one sentinel for a folded run --
			// two Set Parameters nodes are told apart by their entries, not by their position.
			for (FTextNode& Node : Nodes)
			{
				if (Node.bSetParameters)
				{
					Node.Key = SetParametersKey;
					continue;
				}

				FString ModuleError;
				UNiagaraScript* CallScript = Modules.FindModule(Node.Call->Name, ModuleError);
				if (CallScript == nullptr)
				{
					Refuse(FString::Printf(TEXT("the call to '%s' does not resolve to a module asset: %s"),
						*Node.Call->Name, *ModuleError));
					return;
				}
				Node.Key = CallScript->GetPathName();
			}

			TArray<FString> TextKeys;
			TextKeys.Reserve(Nodes.Num());
			for (const FTextNode& Node : Nodes)
			{
				TextKeys.Add(Node.Key);
			}

			TArray<FString> AssetKeys;
			AssetKeys.Reserve(AssetStack.Modules.Num());
			for (const FModuleInfo& Module : AssetStack.Modules)
			{
				AssetKeys.Add(Module.bIsSetParameters
					? FString(SetParametersKey)
					: (Module.Script != nullptr ? Module.Script->GetPathName() : FString()));
			}

			const FCorrespondence Correspondence = AlignNodes(TextKeys, AssetKeys);
			if (Correspondence.Kind != FCorrespondence::EKind::Aligned)
			{
				Refuse(FString::Printf(
					TEXT("the text declares %d node(s) (%s) and the asset's stack holds %d (%s), and %s"),
					Nodes.Num(), *FString::Join(TextKeys, TEXT(", ")),
					AssetStack.Modules.Num(), *FString::Join(AssetKeys, TEXT(", ")),
					*Correspondence.Why));
				return;
			}

			// The alignment is unique. Whether the two lists are the SAME is now just a count: the
			// matching is injective, so the text has a node the asset lacks exactly when fewer pairs
			// came out than the text has nodes, and the other way round for the asset.
			const bool bStructureNeeded = Correspondence.Matched != Nodes.Num()
				|| Correspondence.Matched != AssetStack.Modules.Num();

			if (bStructureNeeded && !Options.bStructure)
			{
				// The difference is located -- that is what makes the message worth printing -- and it
				// is not applied, because adding or removing a line moves modules in the execution
				// order and that is not something a write-back does behind a switch that is off.
				TArray<FString> Missing;
				TArray<FString> Extra;
				for (int32 Index = 0; Index < AssetStack.Modules.Num(); ++Index)
				{
					if (Correspondence.AssetToText[Index] == INDEX_NONE)
					{
						Missing.Add(AssetStack.Modules[Index].ModuleName.ToString());
					}
				}
				TSet<int32> Matched;
				for (const int32 Index : Correspondence.AssetToText)
				{
					if (Index != INDEX_NONE)
					{
						Matched.Add(Index);
					}
				}
				for (int32 Index = 0; Index < Nodes.Num(); ++Index)
				{
					if (!Matched.Contains(Index))
					{
						Extra.Add(Nodes[Index].bSetParameters
							? TEXT("<folding assignments>") : Nodes[Index].Call->Name);
					}
				}

				++Result.StructureRefused;
				Diagnostics.Warning(TEXT("DFX7113"), Stack.Location, Note(FString::Printf(
					TEXT("pull: %s: the asset has %s that the text does not, and the text has %s that the asset does not. The two lists line up uniquely, so -Structure writes that difference; it adds and removes lines, so it does not happen without the switch. Nothing in this stack was addressed."),
					*StackLabel,
					Missing.Num() > 0 ? *FString::Join(Missing, TEXT(", ")) : TEXT("no module"),
					Extra.Num() > 0 ? *FString::Join(Extra, TEXT(", ")) : TEXT("no statement"))));
				return;
			}

			// Node-by-node verification of what the alignment claims. The keys already say the module
			// assets agree; a pinned node name is the one thing they do not carry, and it is exactly
			// what a wrong alignment would get wrong.
			for (int32 Index = 0; Index < AssetStack.Modules.Num(); ++Index)
			{
				const int32 TextIndex = Correspondence.AssetToText[Index];
				if (TextIndex == INDEX_NONE)
				{
					continue;
				}

				const FTextNode& Node = Nodes[TextIndex];
				const FModuleInfo& Module = AssetStack.Modules[Index];

				if (Node.bSetParameters != Module.bIsSetParameters)
				{
					Refuse(FString::Printf(TEXT("node %d is %s in the text and %s in the asset"),
						Index, Node.bSetParameters ? TEXT("a folded assignment run") : TEXT("a module call"),
						Module.bIsSetParameters ? TEXT("a Set Parameters module") : TEXT("an ordinary module")));
					return;
				}
				if (Node.bSetParameters)
				{
					continue;
				}
				if (!Node.Call->InstanceName.IsEmpty()
					&& !Node.Call->InstanceName.Equals(Module.ModuleName.ToString(), ESearchCase::CaseSensitive))
				{
					Refuse(FString::Printf(
						TEXT("the text pins this node to the name '%s' but the asset's node is called '%s'"),
						*Node.Call->InstanceName, *Module.ModuleName.ToString()));
					return;
				}
			}

			// --- structure -----------------------------------------------------------------------
			if (bStructureNeeded)
			{
				TArray<FEdit> StructuralEdits;
				int32 AddedLocally = 0;
				int32 RemovedLocally = 0;
				bool bRefused = false;
				FString RefusalDetail;

				auto RefuseStructure = [&](const FString& Detail)
				{
					bRefused = true;
					RefusalDetail = Detail;
				};

				const FString StackPrefix = EmitterName.IsNone()
					? FString(TEXT("Constants."))
					: FString::Printf(TEXT("Constants.%s."), *EmitterName.ToString());

				// Where a new line goes: above the statement it precedes, or above the block's closing
				// brace when it is last. Both are line starts, because an inserted statement is a line.
				auto InsertionOffsetFor = [&](int32 AssetIndex) -> int32
				{
					for (int32 Next = AssetIndex + 1; Next < AssetStack.Modules.Num(); ++Next)
					{
						const int32 TextIndex = Correspondence.AssetToText[Next];
						if (TextIndex != INDEX_NONE && Nodes[TextIndex].StartOffset != INDEX_NONE)
						{
							return Lines.LineStartOf(Nodes[TextIndex].StartOffset);
						}
					}
					if (Stack.EndOffset == INDEX_NONE || Stack.EndOffset <= 0)
					{
						return INDEX_NONE;
					}
					return Lines.LineStartOf(Stack.EndOffset - 1);
				};

				// The indentation an inserted statement gets: the file's own, taken from the nearest
				// statement that is already there rather than invented. A stack with no statements at
				// all falls back to the document's other stacks, and only then to four spaces.
				auto IndentFor = [&](int32 AssetIndex) -> FString
				{
					for (int32 Next = AssetIndex + 1; Next < AssetStack.Modules.Num(); ++Next)
					{
						const int32 TextIndex = Correspondence.AssetToText[Next];
						if (TextIndex != INDEX_NONE && Nodes[TextIndex].StartOffset != INDEX_NONE)
						{
							return Lines.IndentAt(Nodes[TextIndex].StartOffset);
						}
					}
					for (int32 Previous = AssetIndex - 1; Previous >= 0; --Previous)
					{
						const int32 TextIndex = Correspondence.AssetToText[Previous];
						if (TextIndex != INDEX_NONE && Nodes[TextIndex].StartOffset != INDEX_NONE)
						{
							return Lines.IndentAt(Nodes[TextIndex].StartOffset);
						}
					}
					for (const FStack& Sibling : Document.Stacks)
					{
						for (const FStatement& Statement : Sibling.Statements)
						{
							if (Statement.StartOffset != INDEX_NONE && Lines.StartsLine(Statement.StartOffset))
							{
								return Lines.IndentAt(Statement.StartOffset);
							}
						}
					}
					return TEXT("    ");
				};

				// Removals first: they depend on nothing the insertions compute.
				{
					TSet<int32> Matched;
					for (const int32 Index : Correspondence.AssetToText)
					{
						if (Index != INDEX_NONE)
						{
							Matched.Add(Index);
						}
					}

					for (int32 Index = 0; Index < Nodes.Num() && !bRefused; ++Index)
					{
						if (Matched.Contains(Index))
						{
							continue;
						}

						const FTextNode& Node = Nodes[Index];
						const FString NodeLabel = Node.bSetParameters
							? FString(TEXT("<folding assignments>")) : Node.Call->Name;

						if (Node.StartOffset == INDEX_NONE || Node.EndOffset == INDEX_NONE
							|| Node.StartOffset > SourceText.Len() || Node.EndOffset > SourceText.Len())
						{
							RefuseStructure(FString::Printf(
								TEXT("the text declares '%s' and the asset's stack does not, but the parser recorded no byte range for it"),
								*NodeLabel));
							break;
						}

						const int32 LineStart = Lines.LineStartOf(Node.StartOffset);
						const int32 LineEnd = Lines.LineEndOf(FMath::Max(Node.StartOffset, Node.EndOffset - 1));

						// The statement has to own its lines, comment included. An edit that ate a
						// trailing comment would take something the asset never held.
						if (!Lines.StartsLine(Node.StartOffset) || !Lines.EndsLine(Node.EndOffset))
						{
							RefuseStructure(FString::Printf(
								TEXT("'%s' is not alone on its line, so removing it would take something else with it"),
								*NodeLabel));
							break;
						}

						FEdit Edit;
						Edit.Kind = EEditKind::Replace;
						Edit.Start = LineStart;
						Edit.End = LineEnd;
						Edit.Old = SourceText.Mid(LineStart, LineEnd - LineStart);
						Edit.Text = FString(); // a removal replaces the line with nothing
						Edit.Address = FString::Printf(TEXT("%s: -%s"), *StackLabel, *NodeLabel);
						Edit.Location = Node.Location;
						StructuralEdits.Add(MoveTemp(Edit));
						++RemovedLocally;
					}
				}

				// Insertions, in asset order, so the text ends up in execution order.
				for (int32 Index = 0; Index < AssetStack.Modules.Num() && !bRefused; ++Index)
				{
					if (Correspondence.AssetToText[Index] != INDEX_NONE)
					{
						continue;
					}

					const FModuleInfo& Module = AssetStack.Modules[Index];
					if (Module.bIsSetParameters)
					{
						// A Set Parameters module is the fold of a run of assignments, and rebuilding one
						// means writing that run -- a different edit from adding a module call, and one
						// with no fixture behind it yet. Refused by name rather than approximated: the
						// boundary of a fold decides which entries can see each other.
						RefuseStructure(FString::Printf(
							TEXT("the asset holds a Set Parameters module ('%s') the text does not declare, and pulling a run of assignments out of one is not something this writes"),
							*Module.ModuleName.ToString()));
						break;
					}

					const int32 InsertAt = InsertionOffsetFor(Index);
					if (InsertAt == INDEX_NONE)
					{
						RefuseStructure(TEXT("the parser did not record where this stack's block ends, so there is nowhere to add a line"));
						break;
					}

					// The module's name as the file would write it: the asset's short name when that
					// resolves back to the same asset, and the full content path when it does not.
					// Verified rather than assumed, because "FindModule answers the same script" is the
					// only thing that makes the name a name.
					const FString ShortName = Module.Script != nullptr ? Module.Script->GetName() : FString();
					FString ModuleName = ShortName;
					if (!ModuleName.IsEmpty())
					{
						FString ModuleError;
						UNiagaraScript* Resolved = Modules.FindModule(ModuleName, ModuleError);
						if (Resolved == nullptr || Module.Script == nullptr
							|| Resolved->GetPathName() != Module.Script->GetPathName())
						{
							ModuleName.Reset();
						}
					}
					if (ModuleName.IsEmpty() && Module.Script != nullptr)
					{
						ModuleName = Module.Script->GetOutermost()->GetName();
					}
					if (ModuleName.IsEmpty())
					{
						RefuseStructure(TEXT("the asset's node runs no module asset, so there is no call to write"));
						break;
					}

					// The version pin, when the module has one: it is what keeps the node on the version
					// the asset was built against instead of whatever the project has today.
					FString VersionPin;
					{
						FScriptVersion Version;
						Errors.Reset();
						const FStackAddress ModuleAddress = SystemAddress.WithEmitter(EmitterName)
							.WithScript(FNiagaraAdapter::ScriptNameForStack(Stack.Kind))
							.WithModule(Module.ModuleName);
						if (FNiagaraAdapter::GetModuleScriptVersion(ModuleAddress, Version, Errors) && Version.IsValid())
						{
							VersionPin = FString::Printf(TEXT("@%s"), *Version.ToLabel());
						}
					}

					// `as <name>` only when the node's name is not the module asset's: a build names a
					// fresh node after the module when the name is free, so writing the pin where it
					// agrees is noise, and omitting it where it disagrees is a node with the wrong name.
					const FString Alias = !ShortName.IsEmpty() && Module.ModuleName.ToString() != ShortName
						? FString::Printf(TEXT(" as %s"), *Module.ModuleName.ToString())
						: FString();

					const FString Prefix = StackPrefix + Module.ModuleName.ToString() + TEXT(".");

					TArray<FString> Arguments;
					for (const FInputInfo& Input : Module.Inputs)
					{
						FString Why;
						const FStoredValue* Value = FindStored(Stored, Prefix + Input.Name.ToString(), Why);
						if (Value == nullptr)
						{
							// No stored constant for this input: the module's own default is what the
							// asset holds there too, so there is no argument to write.
							continue;
						}

						FInputValue AsValue;
						FString Rendered;
						if (!MakeStoredInputValue(*Value, AsValue, Why)
							|| !RenderStoredInputValue(AsValue, *Value, Rendered, Why))
						{
							RefuseStructure(FString::Printf(TEXT("the node's input '%s' %s"),
								*Input.Name.ToString(),
								Why.IsEmpty() ? TEXT("cannot be written as a literal") : *Why));
							break;
						}
						Arguments.Add(FString::Printf(TEXT("%s = %s"), *ToInputIdentifier(Input.Name), *Rendered));
					}
					if (bRefused)
					{
						break;
					}

					const FString Indent = IndentFor(Index);
					const FString Newline = Lines.NewlineAt(InsertAt);
					const FString Statement = FString::Printf(TEXT("%s%s%s%s%s(%s);"),
						*Indent,
						Module.bEnabled ? TEXT("") : TEXT("disabled "),
						*ModuleName, *VersionPin, *Alias,
						*FString::Join(Arguments, TEXT(", ")));

					FEdit Edit;
					Edit.Kind = EEditKind::Insert;
					Edit.Start = InsertAt;
					Edit.End = InsertAt;
					Edit.Text = Statement + Newline;
					Edit.Address = FString::Printf(TEXT("%s: +%s"), *StackLabel, *ModuleName);
					Edit.Location = Stack.Location;
					StructuralEdits.Add(MoveTemp(Edit));
					++AddedLocally;
				}

				if (bRefused)
				{
					// A refusal anywhere in this stack's structure means the stack is left alone
					// entirely: the value writes below address nodes by their position in a list whose
					// shape is exactly what could not be decided.
					++Result.StructureRefused;
					Diagnostics.Warning(TEXT("DFX7113"), Stack.Location, Note(FString::Printf(
						TEXT("pull: %s: %s. A structural edit is only made where the whole of it is unambiguous, so this stack was left as it is."),
						*StackLabel, *RefusalDetail)));
					return;
				}

				for (const FEdit& Edit : StructuralEdits)
				{
					Diagnostics.Info(TEXT("DFX7112"), Edit.Location, Note(FString::Printf(
						TEXT("pull: %s: %s"), *Edit.Address, *Edit.Text.TrimEnd())));
				}

				Result.Added += AddedLocally;
				Result.Removed += RemovedLocally;
				Edits.Append(StructuralEdits);
			}

			// --- values --------------------------------------------------------------------------
			for (int32 Index = 0; Index < AssetStack.Modules.Num(); ++Index)
			{
				const int32 TextIndex = Correspondence.AssetToText[Index];
				if (TextIndex == INDEX_NONE)
				{
					continue; // a node the text does not have: an insertion, written above
				}

				const FTextNode& Node = Nodes[TextIndex];
				const FModuleInfo& Module = AssetStack.Modules[Index];
				FString Prefix = TEXT("Constants.");
				if (!EmitterName.IsNone())
				{
					Prefix += EmitterName.ToString() + TEXT(".");
				}
				Prefix += Module.ModuleName.ToString() + TEXT(".");

				if (Module.bIsSetParameters)
				{
					// A folded assignment writes a parameter, not a module input. Its constants are the
					// entries of the Set Parameters node, keyed by the parameter's qualified name --
					// which is exactly the name the statement wrote, so the match is by name, not by
					// position.
					for (const FStatement* Statement : Node.Assignments)
					{
						const FString Address = FString::Printf(TEXT("%s.%s"),
							*Module.ModuleName.ToString(), *Statement->Name);

						auto CannotWrite = [&](const FString& Why)
						{
							++Result.Unwritable;
							Diagnostics.Info(TEXT("DFX7111"), Statement->Location, Note(FString::Printf(
								TEXT("pull: %s is declared but was not written back: %s"), *Address, *Why)));
						};

						FString Why;
						const FStoredValue* Value = FindStored(Stored, Prefix + Statement->Name, Why);
						if (Value == nullptr)
						{
							// A linked or computed assignment has no constant of its own -- its value is
							// the link, and there is nothing stored to disagree with.
							UE_LOG(LogDreamFX, Verbose,
								TEXT("pull: '%s' has no stored constant under '%s'; nothing to compare."),
								*Address, *Prefix);
							continue;
						}

						if (!Statement->Value.IsValid())
						{
							CannotWrite(TEXT("the assignment has no value"));
							continue;
						}

						FInputValue StoredInput;
						if (!MakeStoredInputValue(*Value, StoredInput, Why))
						{
							CannotWrite(Why);
							continue;
						}

						FString NewText;
						if (!RenderStoredInputValue(StoredInput, *Value, NewText, Why))
						{
							CannotWrite(Why);
							continue;
						}

						++Result.Compared;

						FInputValue Existing;
						FDiagnosticSink Scratch;
						Scratch.SetFile(FilePath);
						if (!FValueLowering::Lower(*Statement->Value, Value->Type, Address, Scratch, Existing))
						{
							const FString Message = FirstMessage(Scratch);
							CannotWrite(Message.IsEmpty()
								? TEXT("the text's value does not type-check against this parameter")
								: FString::Printf(TEXT("the text's value does not type-check against this parameter: %s"), *Message));
							continue;
						}

						if (Existing.Mode != EInputValueMode::Literal && Existing.Mode != EInputValueMode::Enum)
						{
							// A link, a dynamic input, an hlsl block: the text is not claiming a value,
							// so there is no disagreement to report.
							continue;
						}

						const FString BaselineValue = BytesToBaseline(*Value);
						const FString StoreScope = TEXT("ri ") + Prefix;
						if (Existing.Equals(StoredInput))
						{
							Record(StoreScope, Statement->Name, BaselineValue);
							continue;
						}

						if (!ShouldWrite(StoreScope, Statement->Name, BaselineValue,
							Statement->Value->Location, Address))
						{
							continue;
						}

						if (!AddReplacement(*Statement->Value, NewText, Address))
						{
							CannotWrite(TEXT("the parser did not record a byte range for the written literal"));
						}
					}

					// The other half: an entry the asset holds that no statement declares.
					for (const TPair<FString, TArray<FStoredValue>>& Entry : Stored)
					{
						if (!Entry.Key.StartsWith(Prefix, ESearchCase::CaseSensitive))
						{
							continue;
						}
						const FString Name = Entry.Key.Mid(Prefix.Len());
						if (Name.IsEmpty())
						{
							continue;
						}
						const bool bDeclared = Node.Assignments.ContainsByPredicate(
							[&Name](const FStatement* Statement)
							{
								return Statement->Name.Equals(Name, ESearchCase::IgnoreCase);
							});
						if (bDeclared)
						{
							continue;
						}

						++Result.Undeclared;
						Diagnostics.Info(TEXT("DFX7106"), Node.Location, Note(FString::Printf(
							TEXT("pull: %s.%s is stored on the asset and not declared in this text -- skipped. pull adds no structure."),
							*Module.ModuleName.ToString(), *Name)));
					}
					continue;
				}

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
						Diagnostics.Info(TEXT("DFX7111"), Node.Call->Location, Note(FString::Printf(
							TEXT("pull: %s is declared but was not written back: %s"), *Address, *Why)));
					};

					// The argument the text wrote for it, matched the way the generator matches input
					// names: normalization drops spaces, hyphens and case, so `Scale RGB` and the inline
					// edit condition `ScaleRGB` that gates it are ONE identifier here.
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

					// And so are the live inputs. Which one the written value MEANS is decided by its
					// type, the way the generator decides it -- so an argument that means a different
					// input than this stored value's is that input's business, not drift, and says
					// nothing here. Without this, `ScaleRGB = true` (the bool edit condition) reads as a
					// bool written into `Scale RGB` (a Vector3) and every call to that module reports a
					// type error that has nothing to do with the asset.
					bool bMeansAnotherInput = false;
					if (Arguments.Num() == 1 && Arguments[0]->Value.IsValid())
					{
						TArray<const FInputInfo*> Candidates;
						for (const FInputInfo& Candidate : Module.Inputs)
						{
							if (NormalizeInputIdentifier(Candidate.Name.ToString()) == Identifier)
							{
								Candidates.Add(&Candidate);
							}
						}

						if (Candidates.Num() > 1)
						{
							TArray<const FInputInfo*> Accepting;
							for (const FInputInfo* Candidate : Candidates)
							{
								FInputValue ScratchValue;
								FDiagnosticSink ScratchSink;
								if (FValueLowering::Lower(*Arguments[0]->Value, Candidate->Type, Address,
									ScratchSink, ScratchValue))
								{
									Accepting.Add(Candidate);
								}
							}

							if (Accepting.Num() == 1)
							{
								bMeansAnotherInput = !Accepting[0]->Name.IsEqual(FName(*InputName));
							}
							else if (Accepting.Num() > 1)
							{
								CannotWrite(FString::Printf(
									TEXT("%d inputs share this name and more than one of them accepts the written value, so which one it means is not something pull can tell"),
									Candidates.Num()));
								continue;
							}
						}
					}

					if (bMeansAnotherInput)
					{
						UE_LOG(LogDreamFX, Verbose,
							TEXT("pull: '%s' writes a value for an input other than the stored '%s.%s'; nothing to write back for that one."),
							*Address, *Module.ModuleName.ToString(), *InputName);
						continue;
					}

					if (Arguments.Num() == 0)
					{
						++Result.Undeclared;
						// One short line per stored value, however many there are. A real asset
						// materialises a constant for every input of every module, so this list is long
						// by nature; the paragraph explaining what pull will not do about it is printed
						// once, in the summary, rather than ninety times here.
						Diagnostics.Info(TEXT("DFX7106"), Node.Call->Location, Note(FString::Printf(
							TEXT("pull: %s is stored on the asset and not declared in this text -- skipped. pull adds no structure."),
							*Address)));
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

					const FString BaselineValue = BytesToBaseline(*StoredValue);
					const FString StoreScope = TEXT("ri ") + Prefix;
					if (Existing.Equals(StoredInputValue))
					{
						Record(StoreScope, InputName, BaselineValue);
						continue; // the text already holds it
					}

					if (!ShouldWrite(StoreScope, InputName, BaselineValue,
						Argument.Value->Location, Address))
					{
						continue;
					}

					if (!AddReplacement(*Argument.Value, NewText, Address))
					{
						CannotWrite(TEXT("the parser did not record a byte range for the written literal"));
					}
				}
			}
		};

		for (const FStack& Stack : Document.Stacks)
		{
			if (!IsFixedStack(Stack.Kind))
			{
				++Result.Unwritable;
				Diagnostics.Info(TEXT("DFX7111"), Stack.Location, Note(FString::Printf(
					TEXT("pull: the %s stack is not addressed (an event or stage stack is read through a focus slice, which is a different mechanism); nothing in it was written back."),
					LexStackKind(Stack.Kind))));
				continue;
			}

			const FName ScriptName = FNiagaraAdapter::ScriptNameForStack(Stack.Kind);
			FScriptStackInfo Info;
			Errors.Reset();
			if (!FNiagaraAdapter::GetScriptStackInfo(SystemAddress.WithScript(ScriptName), Info, Errors))
			{
				++Result.Unaddressable;
				Diagnostics.Warning(TEXT("DFX7109"), Stack.Location, Note(FString::Printf(
					TEXT("pull: the asset's %s could not be read (%s), so nothing in it was addressed."),
					*ScriptName.ToString(), *FString::Join(Errors, TEXT(" | ")))));
				continue;
			}

			ProcessStack(NAME_None, Stack, Info);
		}

		// --- the system's own settings -----------------------------------------------------------
		{
			FString SystemJson;
			Errors.Reset();
			if (!FNiagaraAdapter::GetSystemProperties(System, SystemJson, Errors))
			{
				++Result.Unwritable;
				Diagnostics.Info(TEXT("DFX7111"), Document.HeaderLocation, Note(FString::Printf(
					TEXT("pull: the system's own settings could not be read (%s), so none of them was written back."),
					*FString::Join(Errors, TEXT(" | ")))));
			}
			else
			{
				TSharedPtr<FJsonObject> Json;
				if (ParsePropertiesJson(SystemJson, Json))
				{
					ProcessProperties(Document.Settings, TEXT("system"), TEXT("Settings"),
						/*bSystemScope=*/true, /*PropertyClass=*/nullptr, SystemJson, Json);
				}
			}
		}

		// --- the emitters ----------------------------------------------------------------------
		for (const FEmitter& Emitter : Document.Emitters)
		{
			// A FRESH struct per emitter, and that is not a style choice: GetEmitterInfo APPENDS its
			// four stacks to whatever the out-parameter already holds, so one instance reused across
			// emitters answers the second emitter with the FIRST one's stacks. Measured -- it made
			// `Fountain001.ParticleUpdate` compare against `Fountain`'s stack and refuse a structure
			// that was in fact identical to its own.
			FEmitterInfo EmitterInfo;

			// `Emitter Foo` is a display name in the text and an FName key in the asset; the two are
			// the same string and different types.
			const FName EmitterName(*Emitter.Name);

			Errors.Reset();
			if (!FNiagaraAdapter::GetEmitterInfo(SystemAddress.WithEmitter(EmitterName), EmitterInfo, Errors))
			{
				++Result.Unaddressable;
				Diagnostics.Warning(TEXT("DFX7109"), Emitter.Location, Note(FString::Printf(
					TEXT("pull: emitter '%s' could not be read from '%s' (%s), so nothing in it was addressed."),
					*Emitter.Name, *Result.AssetPath, *FString::Join(Errors, TEXT(" | ")))));
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

			const FStackAddress EmitterAddress = SystemAddress.WithEmitter(EmitterName);

			for (const FStack& Stack : Emitter.Stacks)
			{
				if (!IsFixedStack(Stack.Kind))
				{
					++Result.Unwritable;
					Diagnostics.Info(TEXT("DFX7111"), Stack.Location, Note(FString::Printf(
						TEXT("pull: the %s stack of emitter '%s' is not addressed; nothing in it was written back."),
						LexStackKind(Stack.Kind), *Emitter.Name)));
					continue;
				}

				const FName ScriptName = FNiagaraAdapter::ScriptNameForStack(Stack.Kind);
				const FScriptStackInfo* AssetStack = FindAssetStack(EmitterInfo, Stack.Kind);
				if (AssetStack == nullptr)
				{
					TArray<FString> Available;
					for (const FScriptStackInfo& Candidate : EmitterInfo.Stacks)
					{
						Available.Add(Candidate.ScriptName.ToString());
					}

					++Result.Unaddressable;
					Diagnostics.Warning(TEXT("DFX7109"), Stack.Location, Note(FString::Printf(
						TEXT("pull: emitter '%s' has no %s in the asset (it holds: %s), so the stack this text declares there was not addressed."),
						*Emitter.Name, *ShortScriptName(ScriptName.ToString()),
						Available.Num() > 0 ? *FString::Join(Available, TEXT(", ")) : TEXT("(no stacks)"))));
					continue;
				}

				ProcessStack(EmitterName, Stack, *AssetStack);
			}

			const FString EmitterLabel = FString::Printf(TEXT("emitter %s"), *Emitter.Name);

			// --- the emitter's settings ----------------------------------------------------------
			{
				FString EmitterJson;
				Errors.Reset();
				if (!FNiagaraAdapter::GetEmitterProperties(EmitterAddress, EmitterJson, Errors))
				{
					++Result.Unwritable;
					Diagnostics.Info(TEXT("DFX7111"), Emitter.Location, Note(FString::Printf(
						TEXT("pull: the settings of emitter '%s' could not be read (%s), so none of them was written back."),
						*Emitter.Name, *FString::Join(Errors, TEXT(" | ")))));
				}
				else
				{
					TSharedPtr<FJsonObject> Json;
					if (ParsePropertiesJson(EmitterJson, Json))
					{
						ProcessProperties(Emitter.Settings, EmitterLabel, TEXT("Settings"),
							/*bSystemScope=*/false, /*PropertyClass=*/nullptr, EmitterJson, Json);
					}
				}
			}

			// --- the emitter's `Defaults = { }` ---------------------------------------------------
			if (Emitter.Defaults.Num() > 0)
			{
				TArray<FParameterDefault> Defaults;
				Errors.Reset();
				if (!FNiagaraAdapter::GetParameterDefaults(EmitterAddress, Defaults, Errors))
				{
					++Result.Unwritable;
					Diagnostics.Info(TEXT("DFX7111"), Emitter.Location, Note(FString::Printf(
						TEXT("pull: the defaults of emitter '%s' could not be read (%s), so none of them was written back."),
						*Emitter.Name, *FString::Join(Errors, TEXT(" | ")))));
				}
				else
				{
					const FString DefaultsScope = EmitterLabel + TEXT(" Defaults");
					for (const FStatement& Statement : Emitter.Defaults)
					{
						const FString Address = FString::Printf(TEXT("Defaults.%s"), *Statement.Name);

						auto CannotWrite = [&](const FString& Why)
						{
							++Result.Unwritable;
							Diagnostics.Info(TEXT("DFX7111"), Statement.Location, Note(FString::Printf(
								TEXT("pull: %s is declared but was not written back: %s"), *Address, *Why)));
						};

						if (!Statement.Value.IsValid())
						{
							CannotWrite(TEXT("the default has no value"));
							continue;
						}

						const FParameterDefault* Found = Defaults.FindByPredicate(
							[&Statement](const FParameterDefault& Candidate)
							{
								return Candidate.Variable.GetName().ToString().Equals(Statement.Name, ESearchCase::IgnoreCase);
							});
						if (Found == nullptr)
						{
							CannotWrite(TEXT("the asset holds no default under that name"));
							continue;
						}

						FString NewText;
						if (Found->Mode == FParameterDefault::EMode::Binding)
						{
							NewText = Found->Binding.ToString();
						}
						else if (Found->Mode != FParameterDefault::EMode::Value)
						{
							CannotWrite(TEXT("the asset's default is of a kind this language cannot spell"));
							continue;
						}
						else if (Found->Value.Mode == EInputValueMode::Enum)
						{
							NewText = FValueLowering::EnumEntryToSourceToken(Found->Value.EnumType, Found->Value.EnumEntryName);
						}
						else if (!FValueLowering::LiteralToSource(Found->Value, Found->Variable.GetType(), NewText))
						{
							NewText.Reset();
						}

						if (NewText.IsEmpty())
						{
							CannotWrite(FString::Printf(TEXT("the stored '%s' default has no spelling in this language"),
								*Found->Variable.GetType().GetName()));
							continue;
						}

						++Result.Compared;

						// The text's own meaning, lowered against the type the asset holds. A default has
						// no module signature to infer from, so the type comes from the asset -- which is
						// also what makes `1` and `1.0` the same declaration rather than two.
						FInputValue Existing;
						FDiagnosticSink Scratch;
						Scratch.SetFile(FilePath);
						if (!FValueLowering::Lower(*Statement.Value, Found->Variable.GetType(), Address, Scratch, Existing))
						{
							const FString Message = FirstMessage(Scratch);
							CannotWrite(Message.IsEmpty()
								? TEXT("the text's value does not type-check against this default")
								: FString::Printf(TEXT("the text's value does not type-check against this default: %s"), *Message));
							continue;
						}

						const FString BaselineValue = FString::Printf(TEXT("default:%s:%s"),
							*Found->Variable.GetType().GetName(), *NewText);

						const bool bSame = Found->Mode == FParameterDefault::EMode::Binding
							? (Existing.Mode == EInputValueMode::Linked
								&& Existing.LinkedVariable.GetName().ToString() == Found->Binding.ToString())
							: Existing.Equals(Found->Value);

						if (bSame)
						{
							Record(DefaultsScope, Statement.Name, BaselineValue);
							continue;
						}

						if (!ShouldWrite(DefaultsScope, Statement.Name, BaselineValue,
							Statement.Value->Location, Address))
						{
							continue;
						}

						if (!AddReplacement(*Statement.Value, NewText, Address))
						{
							CannotWrite(TEXT("the parser did not record a byte range for the written literal"));
						}
					}
				}
			}

			// --- the renderers --------------------------------------------------------------------
			if (Emitter.Renderers.Num() > 0)
			{
				if (Emitter.Renderers.Num() != EmitterInfo.Renderers.Num())
				{
					++Result.Unaddressable;
					Diagnostics.Warning(TEXT("DFX7109"), Emitter.Location, Note(FString::Printf(
						TEXT("pull: emitter '%s' declares %d renderer(s) and the asset holds %d, so neither list's positions can be trusted and no renderer property was addressed."),
						*Emitter.Name, Emitter.Renderers.Num(), EmitterInfo.Renderers.Num())));
				}
				else
				{
					for (int32 Index = 0; Index < Emitter.Renderers.Num(); ++Index)
					{
						const FRenderer& Renderer = Emitter.Renderers[Index];
						const FRendererInfo& AssetRenderer = EmitterInfo.Renderers[Index];
						const FStackAddress RendererAddress = EmitterAddress.WithRenderer(Index);
						const FString RendererLabel = FString::Printf(TEXT("emitter %s renderer %d"), *Emitter.Name, Index);

						// The class is half of the address: a renderer is a position on an emitter, and
						// if the position holds a different class the text's properties are not its.
						const FString AssetTypeName = AssetRenderer.Class != nullptr
							? FNiagaraAdapter::RendererTypeNameForClass(AssetRenderer.Class) : FString();
						if (AssetRenderer.Class == nullptr
							|| !AssetTypeName.Equals(Renderer.TypeName, ESearchCase::IgnoreCase))
						{
							++Result.Unaddressable;
							Diagnostics.Warning(TEXT("DFX7109"), Renderer.Location, Note(FString::Printf(
								TEXT("pull: emitter '%s' renderer %d is a %s in this text and a %s in the asset, so none of its properties was addressed."),
								*Emitter.Name, Index, *Renderer.TypeName,
								AssetTypeName.IsEmpty() ? TEXT("(no renderer)") : *AssetTypeName)));
							continue;
						}

						FString RendererJson;
						Errors.Reset();
						if (!FNiagaraAdapter::GetRendererProperties(RendererAddress, RendererJson, Errors))
						{
							++Result.Unwritable;
							Diagnostics.Info(TEXT("DFX7111"), Renderer.Location, Note(FString::Printf(
								TEXT("pull: the properties of %s could not be read (%s), so none of them was written back."),
								*RendererLabel, *FString::Join(Errors, TEXT(" | ")))));
						}
						else
						{
							TSharedPtr<FJsonObject> Json;
							if (ParsePropertiesJson(RendererJson, Json))
							{
								ProcessProperties(Renderer.Properties, RendererLabel, Renderer.TypeName,
									/*bSystemScope=*/false, AssetRenderer.Class, RendererJson, Json);
							}
						}

						// `Bind X -> Y` is not a property assignment; the adapter reads it off the live
						// struct, so it is read the same way and written over the target name's own bytes.
						TArray<TPair<FString, FName>> Bindings;
						Errors.Reset();
						if (!FNiagaraAdapter::GetRendererBindings(RendererAddress, Bindings, Errors))
						{
							++Result.Unwritable;
							Diagnostics.Info(TEXT("DFX7111"), Renderer.Location, Note(FString::Printf(
								TEXT("pull: the bindings of %s could not be read (%s), so none of them was written back."),
								*RendererLabel, *FString::Join(Errors, TEXT(" | ")))));
						}
						else
						{
							const FString BindingScope = RendererLabel + TEXT(" bindings");
							for (const FRendererBinding& Binding : Renderer.Bindings)
							{
								const FString Address = FString::Printf(TEXT("Bind %s -> %s"),
									*Binding.PropertyName, *Binding.Target);

								auto CannotWrite = [&](const FString& Why)
								{
									++Result.Unwritable;
									Diagnostics.Info(TEXT("DFX7111"), Binding.Location, Note(FString::Printf(
										TEXT("pull: %s is declared but was not written back: %s"), *Address, *Why)));
								};

								const TPair<FString, FName>* Found = Bindings.FindByPredicate(
									[&Binding](const TPair<FString, FName>& Candidate)
									{
										return Candidate.Key.Equals(Binding.PropertyName, ESearchCase::IgnoreCase);
									});
								if (Found == nullptr)
								{
									CannotWrite(TEXT("the renderer has no binding by that name in the asset"));
									continue;
								}

								const FString StoredTarget = Found->Value.ToString();
								const FString BaselineValue = FString::Printf(TEXT("bind:%s"), *StoredTarget);
								++Result.Compared;

								if (StoredTarget == Binding.Target)
								{
									Record(BindingScope, Binding.PropertyName, BaselineValue);
									continue;
								}

								if (!ShouldWrite(BindingScope, Binding.PropertyName, BaselineValue,
									Binding.Location, Address))
								{
									continue;
								}

								if (Binding.TargetStartOffset == INDEX_NONE
									|| Binding.TargetEndOffset <= Binding.TargetStartOffset
									|| Binding.TargetEndOffset > SourceText.Len())
								{
									CannotWrite(TEXT("the parser did not record a byte range for the binding target"));
									continue;
								}

								FEdit Edit;
								Edit.Kind = EEditKind::Replace;
								Edit.Start = Binding.TargetStartOffset;
								Edit.End = Binding.TargetEndOffset;
								Edit.Old = SourceText.Mid(Edit.Start, Edit.End - Edit.Start);
								Edit.Text = StoredTarget;
								Edit.Address = Address;
								Edit.Location = Binding.Location;
								Edits.Add(MoveTemp(Edit));
								++Result.Changed;
							}
						}
					}
				}
			}
		}

		// --- report and, with -Apply, write ------------------------------------------------------
		if (Result.Undeclared > 0)
		{
			UE_LOG(LogDreamFX, Display,
				TEXT("pull: %d stored value(s) are not declared in this text, so nothing was written for them. pull rewrites what a text already declares and never adds structure on its own -- write them into the text (decompile -NoDefaults prints every input a module has), or let the build safety gate (DFX8017) decide which of them a rebuild would actually drop."),
				Result.Undeclared);
		}

		if (Edits.Num() == 0)
		{
			Diagnostics.Info(TEXT("DFX7107"), Document.HeaderLocation, Note(FString::Printf(
				TEXT("pull: '%s' already holds every value '%s' stores for the input(s) it declares (%d compared, %d not declared, %d not writable, %d withheld). No bytes written."),
				*FilePath, *Result.AssetPath, Result.Compared, Result.Undeclared, Result.Unwritable,
				Result.Withheld)));
			Result.bSucceeded = true;
			return true;
		}

		// Oldest first so the report reads down the file; the splice below sorts its own copy.
		Edits.Sort([](const FEdit& Left, const FEdit& Right)
		{
			return Left.Start != Right.Start ? Left.Start < Right.Start : Left.End < Right.End;
		});

		for (const FEdit& Edit : Edits)
		{
			// Structural edits already reported themselves (DFX7112); this list is the value changes,
			// which have no other line until they are written.
			if (Edit.Kind == EEditKind::Insert || Edit.Text.IsEmpty())
			{
				continue;
			}
			Diagnostics.Info(TEXT("DFX7105"), Edit.Location, Note(FString::Printf(
				TEXT("pull: %s: %s -> %s"), *Edit.Address, *Edit.Old.TrimEnd(), *Edit.Text)));
		}

		Result.Edits = Edits.Num();

		if (!Options.bApply)
		{
			UE_LOG(LogDreamFX, Display,
				TEXT("pull: %d edit(s) would be made and nothing was: this is a dry run. Pass -Apply to write them into '%s'."),
				Edits.Num(), *FilePath);
			Result.bSucceeded = true;
			return true;
		}

		// Descending, so each splice happens after every offset it could shift. Two edits cannot start
		// at the same offset: an insertion point is a line start and a replacement starts at a token,
		// and two insertions at one point were merged into one statement's worth of text.
		FString NewSource = SourceText;
		Edits.Sort([](const FEdit& Left, const FEdit& Right) { return Left.Start > Right.Start; });
		for (const FEdit& Edit : Edits)
		{
			NewSource = NewSource.Left(Edit.Start) + Edit.Text + NewSource.Mid(Edit.End);
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
				return false;
			}
		}

		OutNewText = NewSource;
		Result.bSucceeded = true;
		return true;
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

		// The baseline: what the asset held the last time this file was pulled into. Read here rather
		// than inside the core because it is a file, and the core is about values.
		FPullBaseline Baseline;
		Baseline.Path = FPaths::ChangeExtension(BackupPathFor(FilePath), TEXT("baseline.txt"));
		Result.BaselinePath = Baseline.Path;
		{
			FString Why;
			if (!Baseline.Load(Baseline.Path, Why))
			{
				Diagnostics.Warning(TEXT("DFX7114"), FSourceLocation(), FString::Printf(
					TEXT("pull: the baseline could not be used (%s), so every value this text declares was compared from scratch."),
					*Why));
			}
			else if (!Baseline.bLoaded)
			{
				Diagnostics.Info(TEXT("DFX7114"), FSourceLocation(), FString::Printf(
					TEXT("pull: %s, so this run compares every value the text declares. A following -Apply records one, and after that pull only writes what the asset has moved since."),
					*Why));
			}
		}
		Result.bBaselineUsed = Baseline.bLoaded && Options.bUseBaseline;

		FString NewSource;
		if (!PullText(SourceText, FilePath, nullptr, Options, Baseline, Diagnostics, Result, NewSource))
		{
			return Result;
		}

		if (Options.bApply && NewSource != SourceText)
		{
			// Back up before writing. The bytes that were there are the only record of what the editor
			// held, and they are not recoverable from the asset once a build has run.
			Result.BackupPath = BackupPathFor(FilePath);
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Result.BackupPath), /*Tree=*/true);
			if (!FFileHelper::SaveStringToFile(SourceText, *Result.BackupPath))
			{
				Diagnostics.Error(TEXT("DFX7110"), FSourceLocation(), FString::Printf(
					TEXT("pull: the backup '%s' could not be written, so '%s' was left untouched."),
					*Result.BackupPath, *FilePath));
				Result.bSucceeded = false;
				return Result;
			}

			if (!FFileHelper::SaveStringToFile(NewSource, *FilePath))
			{
				Diagnostics.Error(TEXT("DFX7110"), FSourceLocation(), FString::Printf(
					TEXT("pull: '%s' could not be written; the bytes it held are in '%s'."),
					*FilePath, *Result.BackupPath));
				Result.bSucceeded = false;
				return Result;
			}
			Result.bWroteFile = true;

			UE_LOG(LogDreamFX, Display,
				TEXT("pull: wrote %d edit(s) into '%s'. The file it was is at '%s'."),
				Result.Edits, *FilePath, *Result.BackupPath);
		}

		// The baseline records the asset as it stands, and only an apply records one: a dry run has
		// not brought the text into step with anything, so a baseline from it would tell the next run
		// that the very differences it just reported have already been dealt with.
		if (Options.bApply)
		{
			Baseline.Records = MoveTemp(Result.ObservedRecords);
			FString Why;
			if (!Baseline.Save(Why))
			{
				Diagnostics.Warning(TEXT("DFX7114"), FSourceLocation(), FString::Printf(
					TEXT("pull: %s. The values were still written; only the record of what the asset held is missing, so the next run compares everything again."),
					*Why));
			}
			else
			{
				Result.bBaselineWritten = true;
			}
		}

		// The log scrolls away; this is what says what changed. Written on a dry run too: the console
		// list is capped and a dry run is the mode that produces the list in the first place.
		const FString ReportPath = FPaths::ChangeExtension(BackupPathFor(FilePath), TEXT("report.txt"));
		{
			TArray<FString> Lines;
			Lines.Add(FString::Printf(TEXT("pull: %s -> %s (%s)"),
				*FilePath, *Result.AssetPath, Options.bApply ? TEXT("-Apply") : TEXT("dry run")));
			Lines.Append(Result.Report);

			IFileManager::Get().MakeDirectory(*FPaths::GetPath(ReportPath), /*Tree=*/true);
			if (FFileHelper::SaveStringArrayToFile(Lines, *ReportPath))
			{
				Result.ReportPath = ReportPath;
			}
		}

		return Result;
	}
}
