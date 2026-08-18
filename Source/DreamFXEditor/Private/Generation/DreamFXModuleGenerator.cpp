#include "Generation/DreamFXModuleGenerator.h"

#include "Adapter/DreamFXNiagaraAdapter.h"
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

			FString Sanitized() const { return FullName.Replace(TEXT("."), TEXT("_")); }
		};

		bool IsAssignmentOperatorAt(const FString& Text, int32 Index)
		{
			// `=` but not `==`, or one of the compound forms. `>=` / `<=` / `!=` end in `=` too, so the
			// character before matters as much as the one after.
			while (Index < Text.Len() && FChar::IsWhitespace(Text[Index]))
			{
				++Index;
			}
			if (Index >= Text.Len())
			{
				return false;
			}

			const TCHAR Character = Text[Index];
			if (Character == TEXT('='))
			{
				return Index + 1 >= Text.Len() || Text[Index + 1] != TEXT('=');
			}
			if (Character == TEXT('+') || Character == TEXT('-') || Character == TEXT('*') || Character == TEXT('/'))
			{
				return Index + 1 < Text.Len() && Text[Index + 1] == TEXT('=');
			}
			return false;
		}

		/** True when the assignment at Index also reads the target: `+=` and friends, but not `=`. */
		bool IsCompoundAssignmentAt(const FString& Text, int32 Index)
		{
			while (Index < Text.Len() && FChar::IsWhitespace(Text[Index]))
			{
				++Index;
			}
			return Index < Text.Len() && Text[Index] != TEXT('=');
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

		/**
		 * Rewrites every `Particles.*` reference in the body to a pin name, collecting what has to be
		 * wired up on either side.
		 *
		 * Two things make this more than a search and replace.
		 *
		 * *Which prefix is the attribute.* `Particles.Color.rgb` is a swizzle of `Particles.Color`,
		 * while `Particles.Moon.SparkSeed` is one attribute with a dotted name. Nothing in the text
		 * separates them, so the rule is longest-known-prefix: the longest dotted prefix that is either a
		 * common Niagara attribute or one the body declared a type for wins. Nothing matching is an
		 * error that says to write the type, rather than a guess that compiles into the wrong shape.
		 *
		 * *Read against write.* An attribute that is only ever assigned needs no read pin, and adding one
		 * would make the module claim a dependency it does not have. An attribute reached by `+=`, or
		 * read anywhere else in the body, needs both -- and both roles resolve to the single write pin
		 * after a seeding assignment, so `+=`, repeated writes and conditional writes all behave the way
		 * they read.
		 */
		bool BindParticleAttributes(const FString& Body, FDiagnosticSink& Diagnostics,
			const FSourceLocation& BodyLocation, TArray<FAttributeBinding>& OutBindings, FString& OutHlsl)
		{
			// Pass one: `float Particles.Moon.SparkSeed = ...` declares a type for an attribute the
			// engine has never heard of. The type token is DreamFX's, not HLSL's, so it is consumed here.
			TMap<FString, FNiagaraTypeDefinition> DeclaredTypes;
			FString Working;
			Working.Reserve(Body.Len());

			{
				int32 Index = 0;
				const int32 Length = Body.Len();
				bool bAtStatementStart = true;

				while (Index < Length)
				{
					const TCHAR Character = Body[Index];

					if (bAtStatementStart && IsIdentifierStart(Character))
					{
						const int32 TypeStart = Index;
						int32 Probe = Index;
						while (Probe < Length && IsIdentifierBody(Body[Probe]))
						{
							++Probe;
						}
						const FString TypeToken = Body.Mid(TypeStart, Probe - TypeStart);

						int32 NameStart = Probe;
						while (NameStart < Length && (Body[NameStart] == TEXT(' ') || Body[NameStart] == TEXT('\t')))
						{
							++NameStart;
						}

						if (NameStart > Probe && NameStart < Length
							&& Body.Mid(NameStart).StartsWith(TEXT("Particles."), ESearchCase::CaseSensitive))
						{
							int32 NameEnd = NameStart;
							while (NameEnd < Length && (IsIdentifierBody(Body[NameEnd]) || Body[NameEnd] == TEXT('.')))
							{
								++NameEnd;
							}
							const FString FullName = Body.Mid(NameStart, NameEnd - NameStart);

							FParameterDecl Declaration;
							Declaration.TypeName = TypeToken;
							Declaration.Name = FullName;
							Declaration.Location = BodyLocation;

							FNiagaraTypeDefinition Type;
							bool bIsDataInterface = false;
							if (!FValueLowering::ResolveDeclaredType(Declaration, Diagnostics, Type, bIsDataInterface)
								|| bIsDataInterface)
							{
								Diagnostics.Error(TEXT("DFX3045"), BodyLocation,
									FString::Printf(TEXT("'%s' is not a type a particle attribute can have."), *TypeToken));
								return false;
							}

							DeclaredTypes.Add(FullName, Type);
							Index = NameStart; // Drop the type token; the rest of the statement stands.
							bAtStatementStart = false;
							continue;
						}

						Working.Append(TypeToken);
						Index = Probe;
						bAtStatementStart = false;
						continue;
					}

					if (Character == TEXT(';') || Character == TEXT('{') || Character == TEXT('}'))
					{
						bAtStatementStart = true;
					}
					else if (!FChar::IsWhitespace(Character))
					{
						bAtStatementStart = false;
					}

					Working.AppendChar(Character);
					++Index;
				}
			}

			// Pass two: find the references, decide read against write, and rewrite.
			TMap<FString, int32> BindingIndices;
			FString Result;
			Result.Reserve(Working.Len());

			int32 Index = 0;
			const int32 Length = Working.Len();
			TCHAR PreviousSignificant = TEXT('\0');

			while (Index < Length)
			{
				const TCHAR Character = Working[Index];

				if (Character == TEXT('/') && Index + 1 < Length && Working[Index + 1] == TEXT('/'))
				{
					while (Index < Length && Working[Index] != TEXT('\n'))
					{
						Result.AppendChar(Working[Index++]);
					}
					continue;
				}
				if (Character == TEXT('/') && Index + 1 < Length && Working[Index + 1] == TEXT('*'))
				{
					Result.AppendChar(Working[Index++]);
					Result.AppendChar(Working[Index++]);
					while (Index < Length && !(Working[Index] == TEXT('*') && Index + 1 < Length && Working[Index + 1] == TEXT('/')))
					{
						Result.AppendChar(Working[Index++]);
					}
					continue;
				}

				if (PreviousSignificant != TEXT('.') && IsIdentifierStart(Character)
					&& Working.Mid(Index).StartsWith(TEXT("Particles."), ESearchCase::CaseSensitive))
				{
					int32 ChainEnd = Index;
					while (ChainEnd < Length && (IsIdentifierBody(Working[ChainEnd]) || Working[ChainEnd] == TEXT('.')))
					{
						++ChainEnd;
					}
					const FString Chain = Working.Mid(Index, ChainEnd - Index);

					// Longest known prefix wins, so a swizzle stays a swizzle.
					FString AttributeName;
					FNiagaraTypeDefinition AttributeType;
					for (int32 Cut = Chain.Len(); Cut > 0; --Cut)
					{
						if (Cut < Chain.Len() && Chain[Cut] != TEXT('.'))
						{
							continue;
						}
						const FString Candidate = Chain.Left(Cut);
						if (const FNiagaraTypeDefinition* Declared = DeclaredTypes.Find(Candidate))
						{
							AttributeName = Candidate;
							AttributeType = *Declared;
							break;
						}
						const FNiagaraTypeDefinition Known = FindKnownAttributeType(Candidate);
						if (Known.IsValid())
						{
							AttributeName = Candidate;
							AttributeType = Known;
							break;
						}
					}

					if (AttributeName.IsEmpty())
					{
						Diagnostics.Error(TEXT("DFX3046"), BodyLocation,
							FString::Printf(TEXT("'%s' is not a particle attribute DreamFX knows the type of. Write the type at its first use in the body -- `float %s = ...;` -- the way a .dfs declares a new attribute."),
								*Chain, *Chain));
						return false;
					}

					const int32 Suffix = ChainEnd - Index - AttributeName.Len();
					const bool bIsAssignmentTarget = Suffix == 0 && IsAssignmentOperatorAt(Working, ChainEnd);
					const bool bCompound = bIsAssignmentTarget && IsCompoundAssignmentAt(Working, ChainEnd);

					int32& BindingIndex = BindingIndices.FindOrAdd(AttributeName, INDEX_NONE);
					if (BindingIndex == INDEX_NONE)
					{
						BindingIndex = OutBindings.Num();
						FAttributeBinding Binding;
						Binding.FullName = AttributeName;
						Binding.Type = AttributeType;
						OutBindings.Add(MoveTemp(Binding));
					}

					FAttributeBinding& Binding = OutBindings[BindingIndex];
					Binding.bWritten |= bIsAssignmentTarget;
					Binding.bRead |= !bIsAssignmentTarget || bCompound;

					// Placeholder: which pin a reference resolves to depends on whether the attribute is
					// written *anywhere*, which is not known until the whole body has been read.
					Result.Append(FString::Printf(TEXT("\x1b%d\x1b"), BindingIndex));
					Result.Append(Working.Mid(Index + AttributeName.Len(), Suffix));
					Index = ChainEnd;
					PreviousSignificant = TEXT(')');
					continue;
				}

				if (IsIdentifierStart(Character))
				{
					const int32 Start = Index;
					while (Index < Length && IsIdentifierBody(Working[Index]))
					{
						++Index;
					}
					Result.Append(Working.Mid(Start, Index - Start));
					PreviousSignificant = Working[Index - 1];
					continue;
				}

				if (!FChar::IsWhitespace(Character))
				{
					PreviousSignificant = Character;
				}
				Result.AppendChar(Character);
				++Index;
			}

			for (int32 BindingIndex = 0; BindingIndex < OutBindings.Num(); ++BindingIndex)
			{
				Result.ReplaceInline(*FString::Printf(TEXT("\x1b%d\x1b"), BindingIndex),
					*OutBindings[BindingIndex].BodyName(), ESearchCase::CaseSensitive);
			}

			// Seed each written attribute's output from its input, so the body's reads see the value the
			// stack handed in and every later write accumulates onto it.
			FString Prologue;
			for (const FAttributeBinding& Binding : OutBindings)
			{
				if (Binding.bWritten && Binding.bRead)
				{
					Prologue.Append(FString::Printf(TEXT("%s = %s;\n"), *Binding.WritePin(), *Binding.ReadPin()));
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
		if (!BindParticleAttributes(Hlsl, Diagnostics, Document.BodyLocation, Attributes, Hlsl))
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

			if (FProvenance::IsUpToDate(Script, Document.SourceHash) && !Options.bForce)
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
		Stamp.SourceFullPath = Document.SourceFilePath;
		Stamp.SourceHash = Document.SourceHash;
		Stamp.GeneratorVersion = FProvenance::GetGeneratorVersion();

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
