#include "Generation/DreamFXModuleGenerator.h"

#include "Adapter/DreamFXNiagaraAdapter.h"
#include "Algo/AnyOf.h"
#include "Algo/Count.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Generation/DreamFXProvenance.h"
#include "Generation/DreamFXValueLowering.h"
#include "Misc/PackageName.h"
#include "NiagaraConstants.h"
#include "NiagaraScript.h"
#include "NiagaraTypes.h"
#include "SourceFiles/DreamFXPaths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

// Every header here is public on both a stock engine and MoonEngine. The two parameter map node
// classes are not, and neither are the five declarations this generator used to call directly; both
// now live behind FGraphSurgeon, which is what lets one copy of the generation code serve both.
#include "EdGraph/EdGraphSchema.h"
#include "Generation/DreamFXGraphSurgeon.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeCustomHlsl.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraScriptSource.h"
#include "NiagaraScriptVariable.h"

namespace UE::DreamFX::Editor
{
	namespace
	{
		/** The graph backend, chosen once: direct on MoonEngine, reflection on a stock engine. */
		FGraphSurgeon* GetSurgeon(FString& OutUnavailableReason)
		{
			static FString CachedReason;
			static TUniquePtr<FGraphSurgeon> Surgeon = FGraphSurgeon::Create(CachedReason);
			OutUnavailableReason = CachedReason;
			return Surgeon.Get();
		}

		/**
		 * The `Usage = ...` spellings a .dfm may declare, and the script usage each maps to.
		 *
		 * These are the six stack names the rest of the language already uses (L1), so an author never
		 * has to learn a second vocabulary for "where can this module go".
		 */
		struct FUsageMapping
		{
			const TCHAR* Text;
			ENiagaraScriptUsage Usage;
		};

		const FUsageMapping UsageMappings[] =
		{
			{ TEXT("SystemSpawn"),    ENiagaraScriptUsage::SystemSpawnScript },
			{ TEXT("SystemUpdate"),   ENiagaraScriptUsage::SystemUpdateScript },
			{ TEXT("EmitterSpawn"),   ENiagaraScriptUsage::EmitterSpawnScript },
			{ TEXT("EmitterUpdate"),  ENiagaraScriptUsage::EmitterUpdateScript },
			{ TEXT("ParticleSpawn"),  ENiagaraScriptUsage::ParticleSpawnScript },
			{ TEXT("ParticleUpdate"), ENiagaraScriptUsage::ParticleUpdateScript },
		};

		bool ParseUsageToken(const FString& Text, ENiagaraScriptUsage& OutUsage)
		{
			for (const FUsageMapping& Mapping : UsageMappings)
			{
				if (Text.Equals(Mapping.Text, ESearchCase::IgnoreCase))
				{
					OutUsage = Mapping.Usage;
					return true;
				}
			}
			return false;
		}

		FString ListUsageTokens()
		{
			TArray<FString> Names;
			for (const FUsageMapping& Mapping : UsageMappings)
			{
				Names.Add(Mapping.Text);
			}
			return FString::Join(Names, TEXT(", "));
		}

		/** One resolved `Inputs = {}` entry. */
		struct FModuleInput
		{
			FString Name;
			FNiagaraTypeDefinition Type;
			FInputValue Default;
			FString Description;
			bool bAdvanced = false;
			bool bStaticSwitchRequested = false;
			FSourceLocation Location;
		};

		bool IsIdentifierStart(TCHAR Character)
		{
			return FChar::IsAlpha(Character) || Character == TEXT('_');
		}

		bool IsIdentifierBody(TCHAR Character)
		{
			return FChar::IsAlnum(Character) || Character == TEXT('_');
		}

		/**
		 * Strips a `Module.` prefix from references to this module's own inputs.
		 *
		 * Inside the custom HLSL node an input is a *pin*, and the body refers to it by the pin's bare
		 * name -- writing `Module.SpinRate` would resolve against the node's own scope and produce a
		 * member of a structure nothing writes. Bare is therefore the only spelling that works, and
		 * bare is also what the plan's sample writes, since inside a module the namespace is implied.
		 *
		 * Authors who reach for the qualified form anyway are not wrong about the language, so it is
		 * accepted and normalised rather than rejected. Every other namespace (`Particles.`, `Engine.`,
		 * `User.`, ...) is left exactly as written: those are read at script scope and must keep their
		 * prefix.
		 *
		 * Comments and string literals are skipped, because a name inside them is prose, not code.
		 */
		FString NormalizeModuleInputReferences(const FString& Body, const TSet<FString>& InputNames)
		{
			FString Result;
			Result.Reserve(Body.Len() + InputNames.Num() * 8);

			const int32 Length = Body.Len();
			int32 Index = 0;
			TCHAR PreviousSignificant = TEXT('\0');

			while (Index < Length)
			{
				const TCHAR Character = Body[Index];

				// Line comment.
				if (Character == TEXT('/') && Index + 1 < Length && Body[Index + 1] == TEXT('/'))
				{
					while (Index < Length && Body[Index] != TEXT('\n'))
					{
						Result.AppendChar(Body[Index++]);
					}
					continue;
				}

				// Block comment.
				if (Character == TEXT('/') && Index + 1 < Length && Body[Index + 1] == TEXT('*'))
				{
					Result.AppendChar(Body[Index++]);
					Result.AppendChar(Body[Index++]);
					while (Index < Length && !(Body[Index] == TEXT('*') && Index + 1 < Length && Body[Index + 1] == TEXT('/')))
					{
						Result.AppendChar(Body[Index++]);
					}
					continue;
				}

				// String literal.
				if (Character == TEXT('"'))
				{
					Result.AppendChar(Body[Index++]);
					while (Index < Length && Body[Index] != TEXT('"'))
					{
						if (Body[Index] == TEXT('\\') && Index + 1 < Length)
						{
							Result.AppendChar(Body[Index++]);
						}
						Result.AppendChar(Body[Index++]);
					}
					if (Index < Length) { Result.AppendChar(Body[Index++]); }
					PreviousSignificant = TEXT('"');
					continue;
				}

				if (IsIdentifierStart(Character))
				{
					const int32 Start = Index;
					while (Index < Length && IsIdentifierBody(Body[Index]))
					{
						++Index;
					}
					FString Identifier = Body.Mid(Start, Index - Start);

					// `Module.` followed by one of our own inputs: drop the prefix. Only a head
					// identifier can start a namespace, so `Foo.Module.Bar` is left alone.
					if (PreviousSignificant != TEXT('.')
						&& Identifier.Equals(TEXT("Module"), ESearchCase::CaseSensitive)
						&& Index + 1 < Length && Body[Index] == TEXT('.'))
					{
						int32 Probe = Index + 1;
						while (Probe < Length && IsIdentifierBody(Body[Probe]))
						{
							++Probe;
						}
						const FString Member = Body.Mid(Index + 1, Probe - Index - 1);
						if (InputNames.Contains(Member))
						{
							Result.Append(Member);
							Index = Probe;
							PreviousSignificant = Member.IsEmpty() ? TEXT('.') : Member[Member.Len() - 1];
							continue;
						}
					}

					Result.Append(Identifier);
					PreviousSignificant = Identifier[Identifier.Len() - 1];
					continue;
				}

				if (!FChar::IsWhitespace(Character))
				{
					PreviousSignificant = Character;
				}
				Result.AppendChar(Character);
				++Index;
			}

			return Result;
		}

		/**
		 * One `Particles.*` attribute the body touches.
		 *
		 * These cannot be read straight out of the body the way `Engine.` and `User.` can. During a
		 * standalone module compile the translator asks GetValidNamespacesForReading with a usage bitmask
		 * of zero, so `Particles.` is not on the list of namespaces it will resolve, and the token comes
		 * out as a field access on a structure that has no such member. Reading and writing through
		 * parameter map pins is what plan 3.3 called for anyway; the compile error is just what makes it
		 * non-optional.
		 */
		struct FAttributeBinding
		{
			/** As written: `Particles.SpriteRotation`. */
			FString FullName;
			FNiagaraTypeDefinition Type;
			bool bRead = false;
			bool bWritten = false;

			/** Pin names. Distinct prefixes because the translator renames input pins to `In_<name>`
			 *  and output pins to `Out_<name>` by whole-token match -- one name for both directions
			 *  would have the input pass rewrite the tokens the output pass is looking for. */
			FString ReadPin() const { return TEXT("Read_") + Sanitized(); }
			FString WritePin() const { return TEXT("Write_") + Sanitized(); }

			/** What the body refers to after rewriting. */
			FString BodyName() const { return bWritten ? WritePin() : ReadPin(); }

			FString Symbol;
			FString Sanitized() const { return Symbol; }
		};

		struct FBodyToken
		{
			FString Text;
			int32 Start = 0;
			int32 End = 0;
			bool bIdentifier = false;
		};

		/** Comments are trivia, strings are opaque, and operators are longest-match tokens. */
		TArray<FBodyToken> TokenizeBody(const FString& Body)
		{
			TArray<FBodyToken> Tokens;
			for (int32 Index = 0; Index < Body.Len();)
			{
				if (FChar::IsWhitespace(Body[Index])) { ++Index; continue; }
				if (Body.Mid(Index, 2) == TEXT("//"))
				{
					while (Index < Body.Len() && Body[Index] != TEXT('\n')) { ++Index; }
					continue;
				}
				if (Body.Mid(Index, 2) == TEXT("/*"))
				{
					Index += 2;
					while (Index < Body.Len() && Body.Mid(Index, 2) != TEXT("*/")) { ++Index; }
					Index = FMath::Min(Index + 2, Body.Len());
					continue;
				}
				FBodyToken Token;
				Token.Start = Index;
				Token.bIdentifier = IsIdentifierStart(Body[Index]);
				if (Body[Index] == TEXT('#'))
				{
					// Directive contents are not executable statements; a semicolon in a #define
					// must not change the following statement's control-flow classification.
					while (Index < Body.Len())
					{
						if (Body[Index] == TEXT('\n'))
						{
							int32 Previous = Index - 1;
							if (Previous >= 0 && Body[Previous] == TEXT('\r')) { --Previous; }
							if (Previous < 0 || Body[Previous] != TEXT('\\')) { break; }
						}
						++Index;
					}
				}
				else if (Token.bIdentifier)
				{
					while (Index < Body.Len() && IsIdentifierBody(Body[Index])) { ++Index; }
				}
				else if (Body[Index] == TEXT('"') || Body[Index] == TEXT('\''))
				{
					const TCHAR Quote = Body[Index++];
					while (Index < Body.Len())
					{
						if (Body[Index] == TEXT('\\')) { Index = FMath::Min(Index + 2, Body.Len()); }
						else if (Body[Index++] == Quote) { break; }
					}
				}
				else
				{
					int32 Width = 1;
					for (const TCHAR* Operator : { TEXT("<<="), TEXT(">>="), TEXT("++"), TEXT("--"),
						TEXT("+="), TEXT("-="), TEXT("*="), TEXT("/="), TEXT("%="), TEXT("&="), TEXT("|="),
						TEXT("^="), TEXT("=="), TEXT("!="), TEXT("<="), TEXT(">="), TEXT("&&"), TEXT("||"), TEXT("<<"), TEXT(">>") })
					{
						const int32 CandidateWidth = FCString::Strlen(Operator);
						if (Body.Mid(Index, CandidateWidth) == Operator) { Width = CandidateWidth; break; }
					}
					Index += Width;
				}
				Token.End = Index;
				Token.Text = Body.Mid(Token.Start, Token.End - Token.Start);
				Tokens.Add(MoveTemp(Token));
			}
			return Tokens;
		}

		bool IsWriteOperator(const FString& Operator)
		{
			return Operator == TEXT("=") || Operator == TEXT("++") || Operator == TEXT("--")
				|| Operator == TEXT("+=") || Operator == TEXT("-=") || Operator == TEXT("*=")
				|| Operator == TEXT("/=") || Operator == TEXT("%=") || Operator == TEXT("&=")
				|| Operator == TEXT("|=") || Operator == TEXT("^=") || Operator == TEXT("<<=") || Operator == TEXT(">>=");
		}

		TArray<bool> FindUnconditionalStatementStarts(const TArray<FBodyToken>& Tokens)
		{
			TArray<bool> Starts;
			int32 Braces = 0, Parentheses = 0;
			bool bStart = true;
			bool bMayHaveExited = false;
			bool bHasConditionalPreprocessing = false;
			for (const FBodyToken& Token : Tokens)
			{
				if (Token.Text.StartsWith(TEXT("#")))
				{
					FString Directive = Token.Text.Mid(1).TrimStart();
					int32 End = 0;
					while (End < Directive.Len() && IsIdentifierBody(Directive[End])) { ++End; }
					Directive.LeftInline(End);
					// Removing a conditional branch can change which statement a preceding unbraced
					// `if` or loop controls, even after #endif. Do not infer definite writes beyond it.
					bHasConditionalPreprocessing |= Directive == TEXT("if") || Directive == TEXT("ifdef") || Directive == TEXT("ifndef");
					Starts.Add(false); bStart = true; continue;
				}
				Starts.Add(bStart && Braces == 0 && Parentheses == 0 && !bHasConditionalPreprocessing && !bMayHaveExited);
				bMayHaveExited |= Token.Text == TEXT("return") || Token.Text == TEXT("discard");
				bStart = false;
				if (Token.Text == TEXT("{")) { ++Braces; }
				if (Token.Text == TEXT("}")) { --Braces; bStart = Braces == 0; }
				if (Token.Text == TEXT("(")) { ++Parentheses; }
				if (Token.Text == TEXT(")")) { --Parentheses; }
				if (Token.Text == TEXT(";") && Parentheses == 0) { bStart = true; }
			}
			return Starts;
		}
		/** The declared type of a common particle attribute, or an invalid type if it is not one. */
		FNiagaraTypeDefinition FindKnownAttributeType(const FString& FullName)
		{
			const FName Name(*FullName);
			for (const FNiagaraVariable& Attribute : FNiagaraConstants::GetCommonParticleAttributes())
			{
				if (Attribute.GetName() == Name)
				{
					return Attribute.GetType();
				}
			}
			return FNiagaraTypeDefinition();
		}

		/**
		 * Removes HLSL comments, keeping the line structure.
		 *
		 * Only the dynamic input reduction below uses this. A module's body is emitted verbatim and
		 * its comments are wanted there; a dynamic input's body is *rewritten*, and every step of that
		 * rewrite reads the text directly, so a comment is not trivia to it the way it is to a reader.
		 *
		 * String-aware because `"http://x"` is not a comment, and newline-preserving because dropping
		 * the line breaks of a block comment would join the lines around it into one.
		 */
		FString StripHlslComments(const FString& Text)
		{
			FString Result;
			Result.Reserve(Text.Len());

			int32 Index = 0;
			while (Index < Text.Len())
			{
				const TCHAR Character = Text[Index];

				if (Character == TEXT('"'))
				{
					Result.AppendChar(Character);
					++Index;
					while (Index < Text.Len())
					{
						const TCHAR StringCharacter = Text[Index];
						Result.AppendChar(StringCharacter);
						++Index;
						if (StringCharacter == TEXT('\\') && Index < Text.Len())
						{
							Result.AppendChar(Text[Index]);
							++Index;
							continue;
						}
						if (StringCharacter == TEXT('"'))
						{
							break;
						}
					}
					continue;
				}

				if (Character == TEXT('/') && Index + 1 < Text.Len() && Text[Index + 1] == TEXT('/'))
				{
					while (Index < Text.Len() && Text[Index] != TEXT('\n') && Text[Index] != TEXT('\r'))
					{
						++Index;
					}
					continue;
				}

				if (Character == TEXT('/') && Index + 1 < Text.Len() && Text[Index + 1] == TEXT('*'))
				{
					Result.AppendChar(TEXT(' ')); // `return/*comment*/X` still has two tokens.
					Index += 2;
					while (Index + 1 < Text.Len() && !(Text[Index] == TEXT('*') && Text[Index + 1] == TEXT('/')))
					{
						if (Text[Index] == TEXT('\n'))
						{
							Result.AppendChar(TEXT('\n'));
						}
						++Index;
					}
					Index = FMath::Min(Index + 2, Text.Len());
					continue;
				}

				Result.AppendChar(Character);
				++Index;
			}

			return Result;
		}

		/**
		 * Reduces a dynamic input body to the single expression the translator expects.
		 *
		 * A custom HLSL node whose ScriptUsage is DynamicInput has its whole body wrapped as
		 * `Out_X = (type)( body );` by ProcessCustomHlsl, so anything with statements in it produces
		 * invalid HLSL rather than a compile error that names the real problem. Detecting it here means
		 * the author gets DFX3037 pointing at the body instead of a translator error pointing at
		 * generated code.
		 *
		 * Modules are not restricted this way -- their bodies are emitted verbatim, which is exactly the
		 * multi-statement capability DFX4030 has been pointing at all along.
		 *
		 * Comments come off first, and that is not tidiness. All three steps below read the text
		 * directly: a `//` line in front of the return defeats the StartsWith test, so the `return`
		 * survives into `Out_X = (type)( return ... );` -- invalid HLSL, which the translator rejects
		 * with an *empty* message, so the author gets DFX6006 naming neither a line nor a reason. A
		 * `;` inside a comment trips the multi-statement test the other way, refusing a body that is
		 * a perfectly good single expression. Both were live until 2026-08-13, and the first was found
		 * by building a hand-written template rather than by reading this function.
		 */
		bool ReduceDynamicInputBody(const FString& Body, FString& OutExpression, FString& OutError)
		{
			FString Trimmed = StripHlslComments(Body).TrimStartAndEnd();

			if (Trimmed.StartsWith(TEXT("return"), ESearchCase::CaseSensitive)
				&& (Trimmed.Len() == 6 || !IsIdentifierBody(Trimmed[6])))
			{
				Trimmed = Trimmed.Mid(6).TrimStartAndEnd();
			}

			while (Trimmed.EndsWith(TEXT(";")))
			{
				Trimmed.LeftChopInline(1);
				Trimmed.TrimEndInline();
			}

			if (Trimmed.IsEmpty())
			{
				OutError = TEXT("The Body has no expression in it -- a DynamicInput computes a value, so there has to be something to compute.");
				return false;
			}

			// A semicolon left in the middle means more than one statement. Parenthesised expressions
			// cannot contain one, so this is a reliable test without parsing HLSL.
			if (Trimmed.Contains(TEXT(";")))
			{
				OutError = TEXT("A DynamicInput body has to be a single expression -- the Niagara translator wraps it as 'Output = (Type)( <body> );', so statements before the return cannot be expressed. Write the logic as a Module instead, or fold it into one expression.");
				return false;
			}

			OutExpression = Trimmed;
			return true;
		}

		/** Rewrite attributes using token boundaries, retaining comments and ordinary HLSL locals. */
		bool BindParticleAttributes(const FString& Body, const TSet<FString>& InputNames, FDiagnosticSink& Diagnostics,
			const FSourceLocation& BodyLocation, TArray<FAttributeBinding>& OutBindings, FString& OutHlsl)
		{
			struct FDeclaration
			{
				FNiagaraTypeDefinition Type;
				bool bNewAttribute = false;
			};
			TMap<FString, FDeclaration> DeclaredTypes;
			FString Working = Body;
			TArray<FBodyToken> Tokens = TokenizeBody(Body);
			TArray<bool> StatementStarts = FindUnconditionalStatementStarts(Tokens);
			// Pin identity as Niagara judges it. UNiagaraNodeCustomHlsl::OnNewTypedPinAdded uniques a new
			// pin with FNiagaraUtilities::GetUniqueName, which compares names with their FName number
			// stripped: `Write_X_1` collides with `Write_X` and is renamed to `Write_X001`, leaving the
			// body naming a pin that does not exist. Compare the same way, so no name is ever renamed.
			auto PinIdentity = [](const FString& Name) { return FName(FName(*Name), 0); };
			TSet<FName> Symbols;
			for (const FString& Input : InputNames) { Symbols.Add(PinIdentity(Input)); }
			for (const FBodyToken& Token : Tokens)
			{
				if (Token.bIdentifier) { Symbols.Add(PinIdentity(Token.Text)); }
			}
			Symbols.Add(TEXT("Map")); Symbols.Add(TEXT("Output"));

			// Recognize actual type names, never control-flow keywords such as `return` or `else`.
			for (int32 Index = 0; Index + 3 < Tokens.Num(); ++Index)
			{
				const FBodyToken& TypeToken = Tokens[Index];
				if (!TypeToken.bIdentifier || Tokens[Index + 1].Text != TEXT("Particles") || Tokens[Index + 2].Text != TEXT(".")) { continue; }
				bool bTypeName = false;
				for (const TCHAR* Name : { TEXT("float"), TEXT("int"), TEXT("int32"), TEXT("bool"), TEXT("Vector2"), TEXT("Vec2"),
					TEXT("Vector"), TEXT("Vector3"), TEXT("Vec3"), TEXT("Vector4"), TEXT("Vec4"), TEXT("Color"), TEXT("LinearColor"), TEXT("Position"), TEXT("Quat") })
				{
					bTypeName |= TypeToken.Text.Equals(Name, ESearchCase::IgnoreCase);
				}
				if (!bTypeName) { continue; }
				FString Name = TEXT("Particles");
				int32 End = Index + 2;
				while (End + 1 < Tokens.Num() && Tokens[End].Text == TEXT(".") && Tokens[End + 1].bIdentifier)
				{
					Name += TEXT(".") + Tokens[End + 1].Text;
					End += 2;
				}
				FParameterDecl Declaration;
				Declaration.TypeName = TypeToken.Text;
				Declaration.Name = Name;
				Declaration.Location = BodyLocation;
				FDeclaration Resolved;
				bool bDataInterface = false;
				if (!FValueLowering::ResolveDeclaredType(Declaration, Diagnostics, Resolved.Type, bDataInterface)) { return false; }
				const FNiagaraTypeDefinition Known = FindKnownAttributeType(Name);
				const FDeclaration* Earlier = DeclaredTypes.Find(Name);
				if ((Known.IsValid() && Known != Resolved.Type) || (Earlier && Earlier->Type != Resolved.Type))
				{
					Diagnostics.Error(TEXT("DFX3057"), BodyLocation,
						FString::Printf(TEXT("Attribute '%s' is declared with conflicting types."), *Name));
					return false;
				}
				Resolved.bNewAttribute = !Known.IsValid() && !Earlier && End < Tokens.Num() && Tokens[End].Text == TEXT("=");
				if (Resolved.bNewAttribute && !StatementStarts[Index])
				{
					Diagnostics.Error(TEXT("DFX3057"), BodyLocation,
						FString::Printf(TEXT("New attribute '%s' needs an unconditional whole-value initializer before conditional writes. To read an attribute supplied by an earlier module, declare its type without an initializer first."), *Name));
					return false;
				}
				if (!Earlier) { DeclaredTypes.Add(Name, Resolved); }
				// Replace only the type token with whitespace, preserving offsets and comment separation.
				for (int32 Position = TypeToken.Start; Position < TypeToken.End; ++Position) { Working[Position] = TEXT(' '); }
			}

			Tokens = TokenizeBody(Working);
			StatementStarts = FindUnconditionalStatementStarts(Tokens);
			TArray<int32> Matching;
			Matching.Init(INDEX_NONE, Tokens.Num());
			TArray<int32> Open;
			for (int32 Index = 0; Index < Tokens.Num(); ++Index)
			{
				if (Tokens[Index].Text == TEXT("(") || Tokens[Index].Text == TEXT("[")) { Open.Add(Index); }
				else if ((Tokens[Index].Text == TEXT(")") || Tokens[Index].Text == TEXT("]")) && !Open.IsEmpty())
				{
					const int32 Start = Open.Pop(EAllowShrinking::No);
					Matching[Start] = Index; Matching[Index] = Start;
				}
			}
			TMap<FString, int32> BindingIndices;
			TArray<int32> InitializedAt;
			FString Result;
			int32 CopiedUntil = 0;
			for (int32 Index = 0; Index + 2 < Tokens.Num(); ++Index)
			{
				if (Tokens[Index].Text != TEXT("Particles") || Tokens[Index + 1].Text != TEXT(".")
					|| (Index > 0 && Tokens[Index - 1].Text == TEXT("."))) { continue; }
				FString Chain = TEXT("Particles");
				FString AttributeName;
				FNiagaraTypeDefinition Type;
				int32 AttributeEnd = INDEX_NONE;
				int32 ChainEnd = Index + 1;
				while (ChainEnd + 1 < Tokens.Num() && Tokens[ChainEnd].Text == TEXT(".") && Tokens[ChainEnd + 1].bIdentifier)
				{
					Chain += TEXT(".") + Tokens[ChainEnd + 1].Text;
					const FDeclaration* Declaration = DeclaredTypes.Find(Chain);
					const FNiagaraTypeDefinition Candidate = Declaration ? Declaration->Type : FindKnownAttributeType(Chain);
					if (Candidate.IsValid()) { AttributeName = Chain; Type = Candidate; AttributeEnd = ChainEnd + 2; }
					ChainEnd += 2;
				}
				if (AttributeEnd == INDEX_NONE)
				{
					Diagnostics.Error(TEXT("DFX3046"), BodyLocation,
						FString::Printf(TEXT("'%s' is not a particle attribute DreamFX knows the type of. Write its type at first use, for example `float %s = ...;`."), *Chain, *Chain));
					return false;
				}
				int32 LvalueEnd = ChainEnd;
				while (LvalueEnd < Tokens.Num())
				{
					if (Tokens[LvalueEnd].Text == TEXT("[") && Matching[LvalueEnd] != INDEX_NONE) { LvalueEnd = Matching[LvalueEnd] + 1; }
					else if (LvalueEnd + 1 < Tokens.Num() && Tokens[LvalueEnd].Text == TEXT(".") && Tokens[LvalueEnd + 1].bIdentifier) { LvalueEnd += 2; }
					else { break; }
				}
				const bool bPartial = LvalueEnd != AttributeEnd;
				int32 LvalueStart = Index;
				while (LvalueStart > 0 && Tokens[LvalueStart - 1].Text == TEXT("(") && Matching[LvalueStart - 1] == LvalueEnd)
				{
					--LvalueStart; ++LvalueEnd;
				}
				const FString Operator = LvalueEnd < Tokens.Num() ? Tokens[LvalueEnd].Text : FString();
				const bool bPrefix = LvalueStart > 0 && (Tokens[LvalueStart - 1].Text == TEXT("++") || Tokens[LvalueStart - 1].Text == TEXT("--"));
				const bool bWritten = bPrefix || IsWriteOperator(Operator);
				const bool bWholeAssignment = bWritten && !bPrefix && Operator == TEXT("=") && !bPartial;
				const bool bGuaranteed = bWholeAssignment && StatementStarts[LvalueStart];
				int32& BindingIndex = BindingIndices.FindOrAdd(AttributeName, INDEX_NONE);
				if (BindingIndex == INDEX_NONE)
				{
					BindingIndex = OutBindings.Num();
					FAttributeBinding Binding;
					Binding.FullName = AttributeName;
					Binding.Type = Type;
					const FString Base = AttributeName.Replace(TEXT("."), TEXT("_"));
					for (int32 Suffix = 0;; ++Suffix)
					{
						// `_v<N>`, never `_<N>`: a trailing `_<digits>` is an FName number, which is
						// exactly the part the engine's uniquing ignores.
						Binding.Symbol = Suffix == 0 ? Base : FString::Printf(TEXT("%s_v%d"), *Base, Suffix);
						const FString Read = Binding.ReadPin(), Write = Binding.WritePin();
						const FName Reserved[] = { PinIdentity(Read), PinIdentity(Write),
							PinIdentity(TEXT("In_") + Read), PinIdentity(TEXT("Out_") + Write) };
						if (!Algo::AnyOf(Reserved, [&Symbols](const FName& Name) { return Symbols.Contains(Name); }))
						{
							for (const FName& Name : Reserved) { Symbols.Add(Name); }
							break;
						}
					}
					OutBindings.Add(MoveTemp(Binding));
					InitializedAt.Add(INDEX_NONE);
				}
				FAttributeBinding& Binding = OutBindings[BindingIndex];
				const bool bAlreadyInitialized = InitializedAt[BindingIndex] != INDEX_NONE && Tokens[Index].Start >= InitializedAt[BindingIndex];
				Binding.bWritten |= bWritten;
				Binding.bRead |= !bAlreadyInitialized && (!bWholeAssignment || !bGuaranteed);
				if (bGuaranteed && InitializedAt[BindingIndex] == INDEX_NONE)
				{
					// Reads inside this initializer still need the incoming value; later statements do not.
					for (int32 End = LvalueEnd + 1; End < Tokens.Num(); ++End)
					{
						if (Tokens[End].Text == TEXT(";")) { InitializedAt[BindingIndex] = Tokens[End].End; break; }
					}
				}
				Result += Working.Mid(CopiedUntil, Tokens[Index].Start - CopiedUntil);
				Result += FString::Printf(TEXT("\x1b%d\x1b"), BindingIndex);
				CopiedUntil = Tokens[AttributeEnd - 1].End;
				Index = AttributeEnd - 1;
			}
			Result += Working.Mid(CopiedUntil);
			FString Prologue;
			for (int32 Index = 0; Index < OutBindings.Num(); ++Index)
			{
				const FAttributeBinding& Binding = OutBindings[Index];
				const FDeclaration* Declaration = DeclaredTypes.Find(Binding.FullName);
				if (Declaration && Declaration->bNewAttribute && Binding.bRead)
				{
					Diagnostics.Error(TEXT("DFX3057"), BodyLocation,
						FString::Printf(TEXT("New attribute '%s' is read before its initializer completes. Initialize it from other values, or declare its type without an initializer to read an existing attribute."), *Binding.FullName));
					return false;
				}
				Result.ReplaceInline(*FString::Printf(TEXT("\x1b%d\x1b"), Index), *Binding.BodyName(), ESearchCase::CaseSensitive);
				if (Binding.bWritten && Binding.bRead)
				{
					Prologue += FString::Printf(TEXT("%s = %s;\n"), *Binding.WritePin(), *Binding.ReadPin());
				}
			}
			OutHlsl = Prologue + Result;
			return true;
		}
		/** Where a .dfm's asset lives. Shared by both configurations, so the two agree on the path. */
		bool ResolveTargetPath(const FDocument& Document, FDiagnosticSink& Diagnostics,
			FString& OutFullAssetPath, FString& OutPackagePath, FString& OutAssetName)
		{
			FString MountPoint;
			FString RootError;
			if (!FDreamFXPaths::ResolveRootMountPoint(Document.Root, MountPoint, RootError))
			{
				Diagnostics.Error(TEXT("DFX5101"), Document.HeaderLocation, RootError);
				return false;
			}

			OutFullAssetPath = MountPoint / Document.Name;
			FDreamFXPaths::SplitPackagePath(OutFullAssetPath, OutPackagePath, OutAssetName);
			return true;
		}

		/**
		 * Loads an already-generated module asset, fully, or reports nothing found.
		 *
		 * FullyLoad is not optional here for the same reason it is not optional in AcquireSystem (R9):
		 * a partially loaded package reaches SavePackage's ValidatePackage and takes the process down.
		 */
		UNiagaraScript* FindExistingScript(const FString& PackagePath, const FString& AssetName,
			const FDocument& Document, FDiagnosticSink& Diagnostics, bool& bOutExists)
		{
			bOutExists = false;

			const FString PackageName = PackagePath / AssetName;
			if (!FPackageName::DoesPackageExist(PackageName))
			{
				return nullptr;
			}
			bOutExists = true;

			UPackage* Package = LoadPackage(nullptr, *PackageName, LOAD_None);
			if (Package == nullptr)
			{
				Diagnostics.Error(TEXT("DFX5103"), Document.HeaderLocation,
					FString::Printf(TEXT("Package '%s' exists on disk but could not be loaded."), *PackageName));
				return nullptr;
			}
			Package->FullyLoad();

			UNiagaraScript* Script = FindObject<UNiagaraScript>(Package, *AssetName);
			if (Script == nullptr)
			{
				Diagnostics.Error(TEXT("DFX5104"), Document.HeaderLocation,
					FString::Printf(TEXT("Package '%s' exists but holds no Niagara script named '%s'. Refusing to overwrite it."),
						*PackageName, *AssetName));
			}
			return Script;
		}

		/** The -Verify half of the provenance contract, identical for systems and modules. */
		bool ReportStampDrift(const UNiagaraScript* Script, const FDocument& Document,
			const FString& FullAssetPath, FDiagnosticSink& Diagnostics)
		{
			FProvenanceStamp Stamp;
			if (!FProvenance::Read(Script, Stamp))
			{
				Diagnostics.Error(TEXT("DFX7001"), Document.HeaderLocation,
					FString::Printf(TEXT("Asset '%s' carries no DreamFX provenance stamp: it was never generated from this source, or it was created by hand."),
						*FullAssetPath));
				return true;
			}
			if (Stamp.SourceHash != Document.SourceHash)
			{
				Diagnostics.Error(TEXT("DFX7002"), Document.HeaderLocation,
					FString::Printf(TEXT("Asset '%s' is stale: it was generated from a different revision of this source. Run the DreamFX build."),
						*FullAssetPath));
				return true;
			}
			return false;
		}
	}

	bool FModuleGenerator::IsAvailable()
	{
		FString Reason;
		return GetSurgeon(Reason) != nullptr;
	}

	FString FModuleGenerator::DescribeUnavailability()
	{
		FString Reason;
		if (GetSurgeon(Reason) != nullptr)
		{
			return FString();
		}

		// Reason names the specific check that failed. Saying "unsupported engine" instead would leave
		// whoever hits this with nothing to act on, and the whole point of the self-check is that the
		// backend knows exactly which assumption broke.
		// Deliberately says nothing about what this engine exports. The backend can also be selected by
		// hand on an engine that exports everything, and claiming otherwise there would send whoever
		// reads this looking in the wrong place.
		return FString::Printf(
			TEXT("the graph backend that writes a .dfm could not confirm the engine shapes it depends on: %s. Generate the module on an engine where it can -- MoonEngine always qualifies -- and commit the asset; any engine loads, references and cooks it normally. Until then, use an inline hlsl { } expression or an existing dynamic input asset."),
			*Reason);
	}

	FModuleGenerateResult FModuleGenerator::CheckWithoutGenerating(const FDocument& Document,
		FDiagnosticSink& Diagnostics)
	{
		// plan-v2 W1, the connected requirement: on an engine that cannot generate, a .dfm is not simply
		// waved through. The asset that was committed alongside it still has to match the source, or the
		// build is quietly running an older module than the text describes. What changes is the remedy --
		// "regenerate on MoonEngine", not "run the build here", which would not work.
		FModuleGenerateResult Result;
		Diagnostics.SetFile(Document.SourceFilePath);

		FString FullAssetPath;
		FString PackagePath;
		FString AssetName;
		if (!ResolveTargetPath(Document, Diagnostics, FullAssetPath, PackagePath, AssetName))
		{
			return Result;
		}
		Result.AssetPath = FullAssetPath;

		bool bExists = false;
		UNiagaraScript* Script = FindExistingScript(PackagePath, AssetName, Document, Diagnostics, bExists);

		if (!bExists)
		{
			Diagnostics.Error(TEXT("DFX5100"), Document.HeaderLocation,
				FString::Printf(TEXT("'%s' is a %s with no generated asset at '%s', and %s"),
					*FPaths::GetCleanFilename(Document.SourceFilePath), LexDocumentKind(Document.Kind),
					*FullAssetPath, *DescribeUnavailability()));
			return Result;
		}

		if (Script == nullptr)
		{
			return Result; // DFX5103 / DFX5104 already reported.
		}

		Result.Script = Script;

		FProvenanceStamp Stamp;
		if (!FProvenance::Read(Script, Stamp) || Stamp.SourceHash != Document.SourceHash)
		{
			Diagnostics.Error(TEXT("DFX5107"), Document.HeaderLocation,
				FString::Printf(TEXT("'%s' no longer matches the module asset at '%s', and this build cannot regenerate it. Rebuild it where a graph backend runs and commit the updated asset; %s"),
					*FPaths::GetCleanFilename(Document.SourceFilePath), *FullAssetPath,
					*DescribeUnavailability()));
			Result.bDrifted = true;
			return Result;
		}

		Result.bSucceeded = true;
		Result.bSkipped = true;
		return Result;
	}

	FModuleGenerateResult FModuleGenerator::Generate(const FDocument& Document, const FGenerateOptions& Options,
		FDiagnosticSink& Diagnostics)
	{
		FString SurgeonUnavailable;
		FGraphSurgeon* Surgeon = GetSurgeon(SurgeonUnavailable);
		if (Surgeon == nullptr)
		{
			// State 3. FGenerator already routes here through IsAvailable(), so reaching this is not
			// expected; it is here so that no path can arrive at the graph code without a backend.
			(void)Options;
			return CheckWithoutGenerating(Document, Diagnostics);
		}

		FModuleGenerateResult Result;
		Diagnostics.SetFile(Document.SourceFilePath);

		const bool bDynamicInput = Document.Kind == EDocumentKind::DynamicInput;

		// ---------------------------------------------------------------- plan (nothing mutates yet)
		//
		// Same ordering rule as the system generator (plan 4.5): every check that can fail runs against
		// an in-memory plan first, so a bad .dfm leaves the previous asset intact instead of empty.

		FString FullAssetPath;
		FString PackagePath;
		FString AssetName;
		if (!ResolveTargetPath(Document, Diagnostics, FullAssetPath, PackagePath, AssetName))
		{
			return Result;
		}
		Result.AssetPath = FullAssetPath;

		// -- usage ------------------------------------------------------------------------------
		ENiagaraScriptUsage ScriptUsage = bDynamicInput
			? ENiagaraScriptUsage::DynamicInput
			: ENiagaraScriptUsage::Module;

		int32 UsageBitmask = 0;
		const FPropertyEntry* UsageSetting = Document.FindSetting(TEXT("Usage"));
		if (UsageSetting != nullptr && UsageSetting->Value.IsValid())
		{
			TArray<const FValue*> UsageTokens;
			if (UsageSetting->Value->Kind == EValueKind::Array)
			{
				for (const FValuePtr& Element : UsageSetting->Value->Elements)
				{
					if (Element.IsValid())
					{
						UsageTokens.Add(Element.Get());
					}
				}
			}
			else
			{
				UsageTokens.Add(UsageSetting->Value.Get());
			}

			for (const FValue* Token : UsageTokens)
			{
				if (bDynamicInput && Token->Text.Equals(TEXT("DynamicInput"), ESearchCase::IgnoreCase))
				{
					continue; // Already covered by DFX3032; the bitmask comes from the stacks instead.
				}

				ENiagaraScriptUsage Parsed;
				if (!ParseUsageToken(Token->Text, Parsed))
				{
					Diagnostics.Error(TEXT("DFX3038"), UsageSetting->Location,
						FString::Printf(TEXT("'%s' is not a stack a module can be placed in. Use one of: %s."),
							*Token->Text, *ListUsageTokens()));
					return Result;
				}
				UsageBitmask |= 1 << static_cast<int32>(Parsed);
			}
		}

		if (UsageBitmask == 0)
		{
			// A DynamicInput with `Usage = DynamicInput` and nothing else still has to say which stacks
			// it may be evaluated in, or the stack UI will never offer it. Particle spawn and update are
			// the two that cover the overwhelming majority; anything else is opt-in through the array
			// form of Usage.
			UsageBitmask = (1 << static_cast<int32>(ENiagaraScriptUsage::ParticleSpawnScript))
				| (1 << static_cast<int32>(ENiagaraScriptUsage::ParticleUpdateScript));
		}

		// -- output type ------------------------------------------------------------------------
		FNiagaraTypeDefinition OutputType = FNiagaraTypeDefinition::GetParameterMapDef();
		if (bDynamicInput)
		{
			const FPropertyEntry* OutputSetting = Document.FindSetting(TEXT("Output"));
			if (OutputSetting == nullptr || !OutputSetting->Value.IsValid())
			{
				return Result; // DFX3031 already reported by the linter.
			}

			FParameterDecl OutputDeclaration;
			OutputDeclaration.TypeName = OutputSetting->Value->Text;
			OutputDeclaration.Name = TEXT("Output");
			OutputDeclaration.Location = OutputSetting->Location;

			bool bIsDataInterface = false;
			if (!FValueLowering::ResolveDeclaredType(OutputDeclaration, Diagnostics, OutputType, bIsDataInterface))
			{
				return Result;
			}
			if (bIsDataInterface)
			{
				Diagnostics.Error(TEXT("DFX3039"), OutputSetting->Location,
					TEXT("A DynamicInput cannot return a data interface; its Output must be a value type."));
				return Result;
			}
		}

		// -- inputs -----------------------------------------------------------------------------
		TArray<FModuleInput> Inputs;
		TSet<FString> InputNames;
		for (const FParameterDecl& Declaration : Document.Parameters)
		{
			FModuleInput Input;
			Input.Name = Declaration.Name;
			Input.Location = Declaration.Location;

			bool bIsDataInterface = false;
			if (!FValueLowering::ResolveDeclaredType(Declaration, Diagnostics, Input.Type, bIsDataInterface))
			{
				return Result;
			}

			if (Declaration.DefaultValue.IsValid())
			{
				const FString DisplayName = FString::Printf(TEXT("%s.%s"), *AssetName, *Declaration.Name);
				if (!FValueLowering::ValidateObjectDefaultType(Input.Type, DisplayName,
					Declaration.DefaultValue->Location, Diagnostics))
				{
					return Result;
				}
				if (!FValueLowering::Lower(*Declaration.DefaultValue, Input.Type, DisplayName, Diagnostics, Input.Default))
				{
					return Result;
				}
				if (Input.Default.Mode != EInputValueMode::Literal && Input.Default.Mode != EInputValueMode::Enum)
				{
					Diagnostics.Error(TEXT("DFX3044"), Declaration.Location,
						FString::Printf(TEXT("The default for input '%s' has to be a literal or an enum entry. A module input default is stored on the asset, so it cannot reference anything outside the module."),
							*Declaration.Name));
					return Result;
				}
			}

			if (const FAttribute* DescriptionAttribute = Declaration.FindAttribute(TEXT("Description")))
			{
				if (DescriptionAttribute->Value.IsValid())
				{
					Input.Description = DescriptionAttribute->Value->Text;
				}
			}
			Input.bAdvanced = Declaration.HasAttribute(TEXT("Advanced"));
			Input.bStaticSwitchRequested = Declaration.HasAttribute(TEXT("StaticSwitch"));

			InputNames.Add(Input.Name);
			Inputs.Add(MoveTemp(Input));
		}

		// -- body -------------------------------------------------------------------------------
		FString Hlsl = NormalizeModuleInputReferences(Document.Body, InputNames);

		TArray<FAttributeBinding> Attributes;
		if (!BindParticleAttributes(Hlsl, InputNames, Diagnostics, Document.BodyLocation, Attributes, Hlsl))
		{
			return Result;
		}

		if (bDynamicInput)
		{
			for (const FAttributeBinding& Binding : Attributes)
			{
				if (Binding.bWritten)
				{
					Diagnostics.Error(TEXT("DFX3047"), Document.BodyLocation,
						FString::Printf(TEXT("A DynamicInput computes a value; it cannot write '%s'. Move the write into a Module."),
							*Binding.FullName));
					return Result;
				}
			}

			FString Expression;
			FString BodyError;
			if (!ReduceDynamicInputBody(Hlsl, Expression, BodyError))
			{
				Diagnostics.Error(TEXT("DFX3037"), Document.BodyLocation, BodyError);
				return Result;
			}
			Hlsl = Expression;
		}

		// Tier one puts the whole body in one node, so a [StaticSwitch] input has no branch to gate --
		// it becomes an ordinary input the body reads. Saying so is the point: silently downgrading a
		// declared compile-time switch to a runtime value is the kind of difference that surfaces later
		// as a performance question nobody can source.
		for (const FModuleInput& Input : Inputs)
		{
			if (Input.bStaticSwitchRequested)
			{
				Diagnostics.Info(TEXT("DFX5102"), Input.Location,
					FString::Printf(TEXT("Input '%s' is marked [StaticSwitch]. Tier-one generation (plan 3.3) lowers the whole Body to a single custom HLSL node, which has no branch for a switch to select, so it is written as an ordinary input instead. The body reads it the same way; only the compile-time folding is lost."),
						*Input.Name));
			}
		}

		// ---------------------------------------------------------------- acquire (first mutation)

		const FString PackageName = PackagePath / AssetName;

		bool bExists = false;
		UNiagaraScript* Script = FindExistingScript(PackagePath, AssetName, Document, Diagnostics, bExists);
		if (bExists && Script == nullptr)
		{
			return Result; // DFX5103 / DFX5104 already reported.
		}

		if (Script != nullptr)
		{
			Result.Script = Script;

			if (Options.bVerifyOnly)
			{
				Result.bDrifted = ReportStampDrift(Script, Document, FullAssetPath, Diagnostics);
				Result.bSucceeded = !Result.bDrifted;
				return Result;
			}

			if (FProvenance::IsUpToDate(Script, Document.SourceHash)
				&& FProvenance::IsSourceLocationCurrent(Script, Document.SourceFilePath) && !Options.bForce)
			{
				Result.bSucceeded = true;
				Result.bSkipped = true;
				return Result;
			}
		}
		else
		{
			if (Options.bVerifyOnly)
			{
				Diagnostics.Error(TEXT("DFX7004"), Document.HeaderLocation,
					FString::Printf(TEXT("Asset '%s' does not exist: this source has never been built."), *FullAssetPath));
				Result.bDrifted = true;
				return Result;
			}

			UPackage* NewPackage = CreatePackage(*PackageName);
			if (NewPackage == nullptr)
			{
				Diagnostics.Error(TEXT("DFX5105"), Document.HeaderLocation,
					FString::Printf(TEXT("Could not create package '%s'."), *PackageName));
				return Result;
			}

			Script = NewObject<UNiagaraScript>(NewPackage, *AssetName,
				RF_Public | RF_Standalone | RF_Transactional);
			FAssetRegistryModule::AssetCreated(Script);
			Result.Script = Script;
		}

		UPackage* Package = Script->GetOutermost();

		// ---------------------------------------------------------------- build the graph

		Script->Usage = ScriptUsage;

		// Category, Description and the usage bitmask live on the versioned data, not on the script:
		// a versioned module can present different metadata per version. A brand-new script has no
		// version entry yet, and CheckVersionDataAvailable is what seeds the first one.
		Script->CheckVersionDataAvailable();
		if (FVersionedNiagaraScriptData* ScriptData = Script->GetLatestScriptData())
		{
			ScriptData->ModuleUsageBitmask = UsageBitmask;

			if (const FPropertyEntry* Category = Document.FindSetting(TEXT("Category")))
			{
				if (Category->Value.IsValid())
				{
					ScriptData->Category = FText::FromString(Category->Value->Text);
				}
			}
			if (const FPropertyEntry* Description = Document.FindSetting(TEXT("Description")))
			{
				if (Description->Value.IsValid())
				{
					ScriptData->Description = FText::FromString(Description->Value->Text);
				}
			}
		}

		// A fresh source every build. Rebuilding in place would mean diffing pins against declarations,
		// which is the incremental-edit problem plan 4.5 deliberately does not solve; the asset's object
		// path -- the thing systems actually reference -- is what stays stable.
		UNiagaraScriptSource* Source = NewObject<UNiagaraScriptSource>(Script, NAME_None, RF_Transactional);
		UNiagaraGraph* Graph = NewObject<UNiagaraGraph>(Source, NAME_None, RF_Transactional);
		Source->NodeGraph = Graph;

		FGraphNodeCreator<UNiagaraNodeInput> InputNodeCreator(*Graph);
		UNiagaraNodeInput* InputNode = InputNodeCreator.CreateNode();
		InputNode->Usage = ENiagaraInputNodeUsage::Parameter;
		InputNode->Input = FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("MapIn"));
		InputNode->NodePosX = -400;
		InputNode->NodePosY = 0;
		InputNodeCreator.Finalize();

		FGraphNodeCreator<UNiagaraNodeOutput> OutputNodeCreator(*Graph);
		UNiagaraNodeOutput* OutputNode = OutputNodeCreator.CreateNode();
		OutputNode->SetUsage(ScriptUsage);
		OutputNode->Outputs.Add(FNiagaraVariable(OutputType, TEXT("Output")));
		OutputNode->NodePosX = 400;
		OutputNode->NodePosY = 0;
		OutputNodeCreator.Finalize();

		FGraphNodeCreator<UNiagaraNodeCustomHlsl> HlslNodeCreator(*Graph);
		UNiagaraNodeCustomHlsl* HlslNode = HlslNodeCreator.CreateNode();
		HlslNode->NodePosX = 0;
		HlslNode->NodePosY = 0;
		HlslNodeCreator.Finalize();

		if (bDynamicInput)
		{
			// Adds the map input and the typed output, and marks the node so the translator wraps the
			// body as an assignment to that output.
			Surgeon->InitAsDynamicInput(*HlslNode, OutputType);
		}
		else
		{
			// The map passes straight through. Both pins carry the same name on purpose: the translator
			// rewrites every parameter-map pin name to the map instance, so in and out resolve to the
			// same `Context.Map` and the body's namespaced reads and writes land on it.
			Surgeon->AddTypedPin(*HlslNode, EGPD_Input, FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("Map"));
			Surgeon->AddTypedPin(*HlslNode, EGPD_Output, FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("Map"));
		}

		Surgeon->SetCustomHlsl(*HlslNode, Hlsl);

		const UEdGraphSchema* Schema = Graph->GetSchema();
		if (Schema == nullptr || !Schema->TryCreateConnection(InputNode->GetOutputPin(0), HlslNode->GetInputPin(0)))
		{
			Diagnostics.Error(TEXT("DFX5106"), Document.HeaderLocation,
				TEXT("Could not wire the module graph. The Niagara schema rejected a parameter map connection."));
			return Result;
		}

		// ---------------------------------------------------------------- reads
		//
		// Everything the body reads off the parameter map at *script* scope arrives as a typed pin fed
		// from this map-get node: the module's own inputs, and any particle attribute it touches.
		//
		// Module inputs have to come this way because the translator aliases `Module.` to the enclosing
		// function call, and inside a custom HLSL node that call is the node itself -- `Module.Frequency`
		// in the body would resolve to `<node>.Frequency`, a member of a structure nothing writes.
		// Particle attributes have to come this way because a standalone module compile does not count
		// `Particles.` as a resolvable namespace at all. `Engine.`, `User.`, `System.` and `Emitter.` are
		// always resolvable and stay in the body untouched.
		const int32 ReadCount = Inputs.Num() + Algo::CountIf(Attributes,
			[](const FAttributeBinding& Binding) { return Binding.bRead; });

		UNiagaraNode* MapGetNode = nullptr;
		if (ReadCount > 0)
		{
			MapGetNode = Surgeon->CreateParameterMapGet(*Graph);
			if (MapGetNode == nullptr)
			{
				Diagnostics.Error(TEXT("DFX5106"), Document.HeaderLocation,
					TEXT("Could not wire the module graph. A parameter map get node could not be created."));
				return Result;
			}
			MapGetNode->NodePosX = -200;
			MapGetNode->NodePosY = 200;

			if (!Schema->TryCreateConnection(InputNode->GetOutputPin(0), MapGetNode->GetInputPin(0)))
			{
				Diagnostics.Error(TEXT("DFX5106"), Document.HeaderLocation,
					TEXT("Could not wire the module graph. The Niagara schema rejected the map-get connection."));
				return Result;
			}
		}

		auto WireRead = [&](const FNiagaraTypeDefinition& Type, const FName& MapName, const FName& PinName,
			const FSourceLocation& Location, const FString& What) -> bool
		{
			UEdGraphPin* ReadPin = Surgeon->AddTypedPin(*MapGetNode, EGPD_Output, Type, MapName);
			UEdGraphPin* FeedPin = Surgeon->AddTypedPin(*HlslNode, EGPD_Input, Type, PinName);

			if (ReadPin == nullptr || FeedPin == nullptr || !Schema->TryCreateConnection(ReadPin, FeedPin))
			{
				Diagnostics.Error(TEXT("DFX5106"), Location,
					FString::Printf(TEXT("Could not wire %s of type %s into the module body."),
						*What, *FValueLowering::DescribeType(Type)));
				return false;
			}
			return true;
		};

		for (const FModuleInput& Input : Inputs)
		{
			const FName QualifiedName(*FString::Printf(TEXT("Module.%s"), *Input.Name));
			if (!WireRead(Input.Type, QualifiedName, FName(*Input.Name), Input.Location,
				FString::Printf(TEXT("input '%s'"), *Input.Name)))
			{
				return Result;
			}

			// Defaults and tooltips. The pin is what makes the input exist; the script variable is what
			// gives it a value and a description in the stack.
			FNiagaraVariable Variable(Input.Type, QualifiedName);

			FNiagaraVariableMetaData MetaData;
			MetaData.Description = FText::FromString(Input.Description);
			MetaData.bAdvancedDisplay = Input.bAdvanced;
			MetaData.CreateNewGuid();

			UNiagaraScriptVariable* ScriptVariable = Surgeon->AddParameter(*Graph, Variable, MetaData);
			if (ScriptVariable == nullptr)
			{
				continue;
			}

			ScriptVariable->DefaultMode = ENiagaraDefaultMode::Value;

			if (Input.Default.Mode == EInputValueMode::Literal
				&& Input.Default.LiteralBytes.Num() == Input.Type.GetSize())
			{
				ScriptVariable->SetDefaultValueData(Input.Default.LiteralBytes.GetData());
			}
			else if (Input.Default.Mode == EInputValueMode::Enum && Input.Default.EnumType != nullptr)
			{
				const int32 EntryValue = static_cast<int32>(
					Input.Default.EnumType->GetValueByName(Input.Default.EnumEntryName));
				if (EntryValue != INDEX_NONE && Input.Type.GetSize() == sizeof(int32))
				{
					ScriptVariable->SetDefaultValueData(reinterpret_cast<const uint8*>(&EntryValue));
				}
			}
		}

		for (const FAttributeBinding& Binding : Attributes)
		{
			if (!Binding.bRead)
			{
				continue;
			}
			if (!WireRead(Binding.Type, FName(*Binding.FullName), FName(*Binding.ReadPin()),
				Document.BodyLocation, FString::Printf(TEXT("attribute '%s'"), *Binding.FullName)))
			{
				return Result;
			}
		}

		// ---------------------------------------------------------------- writes
		//
		// The map leaves the custom HLSL node, picks up every written attribute at a map-set node, and
		// goes to the output. With nothing written the node connects to the output directly.
		UEdGraphPin* MapOutPin = HlslNode->GetOutputPin(0);

		const bool bHasWrites = Attributes.ContainsByPredicate(
			[](const FAttributeBinding& Binding) { return Binding.bWritten; });

		if (bHasWrites)
		{
			UNiagaraNode* MapSetNode = Surgeon->CreateParameterMapSet(*Graph);
			if (MapSetNode == nullptr)
			{
				Diagnostics.Error(TEXT("DFX5106"), Document.HeaderLocation,
					TEXT("Could not wire the module graph. A parameter map set node could not be created."));
				return Result;
			}
			MapSetNode->NodePosX = 200;
			MapSetNode->NodePosY = 0;

			if (!Schema->TryCreateConnection(MapOutPin, MapSetNode->GetInputPin(0)))
			{
				Diagnostics.Error(TEXT("DFX5106"), Document.HeaderLocation,
					TEXT("Could not wire the module graph. The Niagara schema rejected the map-set connection."));
				return Result;
			}

			for (const FAttributeBinding& Binding : Attributes)
			{
				if (!Binding.bWritten)
				{
					continue;
				}

				UEdGraphPin* SourcePin = Surgeon->AddTypedPin(*HlslNode, EGPD_Output, Binding.Type, FName(*Binding.WritePin()));
				UEdGraphPin* TargetPin = Surgeon->AddTypedPin(*MapSetNode, EGPD_Input, Binding.Type, FName(*Binding.FullName));

				if (SourcePin == nullptr || TargetPin == nullptr || !Schema->TryCreateConnection(SourcePin, TargetPin))
				{
					Diagnostics.Error(TEXT("DFX5106"), Document.BodyLocation,
						FString::Printf(TEXT("Could not wire the write to '%s' of type %s out of the module body."),
							*Binding.FullName, *FValueLowering::DescribeType(Binding.Type)));
					return Result;
				}
			}

			MapOutPin = MapSetNode->GetOutputPin(0);
		}

		if (!Schema->TryCreateConnection(MapOutPin, OutputNode->GetInputPin(0)))
		{
			Diagnostics.Error(TEXT("DFX5106"), Document.HeaderLocation,
				TEXT("Could not wire the module graph. The Niagara schema rejected the output connection."));
			return Result;
		}

		FString FinalizeError;
		if (!Surgeon->FinalizeParameterMapPins(*Graph, FinalizeError))
		{
			Diagnostics.Error(TEXT("DFX5106"), Document.BodyLocation, FinalizeError);
			return Result;
		}
		Graph->NotifyGraphChanged();
		Script->SetLatestSource(Source);
		Script->RequestCompile(FGuid());

		// The compile is synchronous for a module script's VM, and its result is the only thing that
		// says whether the body is valid HLSL at all. Without this the gate would pass on a module that
		// fails to translate -- exactly the "compiles but does nothing" hole the CI step exists to close.
		if (Script->GetLastCompileStatus() == ENiagaraScriptCompileStatus::NCS_Error)
		{
			FString CompileError = Script->GetVMExecutableData().ErrorMsg;
			CompileError.ReplaceInline(TEXT("\r\n"), TEXT("\n"));

			// This message is multi-line, and that used to be invisible: dfx.ps1 kept only the lines
			// carrying a LogDreamFX prefix, which the second and later lines of a UE_LOG do not have,
			// so the diagnostic reached the terminal ending in a bare colon. The translator's own text
			// -- which names the generated line and the syntax error -- was there the whole time and
			// was dropped by the driver. Fixed in dfx.ps1, not here; noted here because a diagnostic
			// that says nothing is not a thing you go looking for in the *printer*.
			Diagnostics.Error(TEXT("DFX6006"), Document.BodyLocation,
				FString::Printf(TEXT("Niagara could not compile the body of '%s':\n%s"),
					*AssetName, *CompileError.TrimEnd()));
			return Result;
		}

		// ---------------------------------------------------------------- stamp and save

		FProvenanceStamp Stamp;
		FProvenance::SetSourceLocation(Stamp, Document.SourceFilePath);
		Stamp.SourceHash = Document.SourceHash;
		Stamp.GeneratorVersion = FProvenance::GetGeneratorVersion();

		FProvenance::Write(Script, Stamp);

		if (Options.bSave)
		{
			Package->MarkPackageDirty();

			const FString FileName = FPackageName::LongPackageNameToFilename(
				Package->GetName(), FPackageName::GetAssetPackageExtension());

			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
			SaveArgs.Error = GWarn; // default GError makes a failed save fatal

			if (!UPackage::SavePackage(Package, Script, *FileName, SaveArgs))
			{
				Diagnostics.Error(TEXT("DFX5030"), Document.HeaderLocation,
					FString::Printf(TEXT("SavePackage failed for '%s'."), *FileName));
				return Result;
			}
		}

		Result.bSucceeded = !Diagnostics.HasErrors();
		return Result;
	}
}
