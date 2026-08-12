#include "DreamFXParser.h"

#include "DreamFXLexer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace UE::DreamFX
{
	namespace
	{
		/**
		 * Recursive-descent parser over the token stream.
		 *
		 * Recovery model: a syntax error inside a block skips forward to the next ';' or the block's
		 * own '}', then keeps going. One bad statement therefore costs one diagnostic, not the rest of
		 * the file -- which matters because the whole point of a text pipeline is that the author sees
		 * every problem in one build.
		 */
		class FParserImpl
		{
		public:
			FParserImpl(const FString& InSource, FDiagnosticSink& InDiagnostics)
				: Lexer(InSource, InDiagnostics)
				, Diagnostics(InDiagnostics)
			{
			}

			bool ParseDocument(FDocument& OutDocument);

		private:
			// --- token helpers ---------------------------------------------------------------
			bool Expect(const TCHAR* Symbol);
			bool ExpectIdentifier(FString& OutText);
			void SkipToStatementEnd();
			void SkipBalancedBlock();
			void ErrorAtCurrent(const TCHAR* Code, const FString& Message);

			// --- sections --------------------------------------------------------------------
			bool ParseHeaderArguments(FDocument& OutDocument);
			bool ParseSystemBody(FDocument& OutDocument);
			bool ParseEmitterBody(FEmitter& OutEmitter, bool bAllowRenderers);
			bool ParseModuleBody(FDocument& OutDocument);

			bool ParseSettingsBlock(TArray<FPropertyEntry>& OutProperties);
			bool ParseParameterBlock(TArray<FParameterDecl>& OutParameters);
			bool ParseStackBlock(FStack& OutStack);
			bool ParseEventHandlerArguments(FEventHandlerSpec& OutSpec);
			bool ParseSimulationStageArguments(FSimulationStageSpec& OutSpec);
			bool ParseEmitterDeclaration(FEmitter& OutEmitter);
			bool ParseRendererDeclaration(FRenderer& OutRenderer);

			bool ParseStatement(FStack& OutStack, TArray<FString>& RegionStack);
			bool ParseAttributeList(TArray<FAttribute>& OutAttributes);
			bool ParseArgumentList(TArray<FNamedArgument>& OutArguments, TArray<FValuePtr>& OutPositional);

			/** Reads `A/B.C` style dotted-or-slashed paths used for module names and parameter targets. */
			bool ParseQualifiedName(FString& OutName, FSourceLocation& OutLocation);
			bool ParseTypeName(FString& OutTypeName, FString& OutInnerTypeName);

			// --- values ----------------------------------------------------------------------
			FValuePtr ParseValue();
			FValuePtr ParseAdditive();
			FValuePtr ParseMultiplicative();
			FValuePtr ParseUnary();
			FValuePtr ParsePrimary();
			FValuePtr ParseCurveLiteral(const FSourceLocation& Location);
			bool ParseSignedNumber(float& OutNumber);

			FLexer Lexer;
			FDiagnosticSink& Diagnostics;
			EDocumentKind DocumentKind = EDocumentKind::System;
		};

		void FParserImpl::ErrorAtCurrent(const TCHAR* Code, const FString& Message)
		{
			Diagnostics.Error(Code, Lexer.Peek().Location, Message);
		}

		bool FParserImpl::Expect(const TCHAR* Symbol)
		{
			if (Lexer.TryConsumeSymbol(Symbol))
			{
				return true;
			}

			const FToken& Token = Lexer.Peek();
			Diagnostics.Error(TEXT("DFX2001"), Token.Location,
				FString::Printf(TEXT("Expected '%s' but found %s."), Symbol,
					Token.IsEnd() ? TEXT("end of file") : *FString::Printf(TEXT("'%s'"), *Token.Text)));
			return false;
		}

		bool FParserImpl::ExpectIdentifier(FString& OutText)
		{
			const FToken& Token = Lexer.Peek();
			if (Token.Kind != ETokenKind::Identifier)
			{
				Diagnostics.Error(TEXT("DFX2002"), Token.Location,
					FString::Printf(TEXT("Expected a name but found %s."),
						Token.IsEnd() ? TEXT("end of file") : *FString::Printf(TEXT("'%s'"), *Token.Text)));
				return false;
			}
			OutText = Lexer.Next().Text;
			return true;
		}

		void FParserImpl::SkipToStatementEnd()
		{
			int32 Depth = 0;
			while (!Lexer.Peek().IsEnd())
			{
				const FToken& Token = Lexer.Peek();
				if (Token.IsSymbol(TEXT("{")) || Token.IsSymbol(TEXT("(")) || Token.IsSymbol(TEXT("[")))
				{
					++Depth;
				}
				else if (Token.IsSymbol(TEXT(")")) || Token.IsSymbol(TEXT("]")))
				{
					--Depth;
				}
				else if (Token.IsSymbol(TEXT("}")))
				{
					if (Depth == 0)
					{
						// Leave the closing brace for the enclosing block to consume.
						return;
					}
					--Depth;
				}
				else if (Token.IsSymbol(TEXT(";")) && Depth <= 0)
				{
					Lexer.Next();
					return;
				}
				Lexer.Next();
			}
		}

		void FParserImpl::SkipBalancedBlock()
		{
			if (!Lexer.Peek().IsSymbol(TEXT("{")))
			{
				return;
			}
			int32 Depth = 0;
			while (!Lexer.Peek().IsEnd())
			{
				const FToken Token = Lexer.Next();
				if (Token.IsSymbol(TEXT("{")))
				{
					++Depth;
				}
				else if (Token.IsSymbol(TEXT("}")))
				{
					if (--Depth == 0)
					{
						return;
					}
				}
			}
		}

		bool FParserImpl::ParseQualifiedName(FString& OutName, FSourceLocation& OutLocation)
		{
			OutLocation = Lexer.Peek().Location;

			FString Name;
			if (Lexer.Peek().IsSymbol(TEXT("/")))
			{
				Lexer.Next();
				Name = TEXT("/");
			}

			FString Segment;
			if (!ExpectIdentifier(Segment))
			{
				return false;
			}
			Name += Segment;

			// Statement position has no arithmetic, so '/' here is unambiguously a path separator.
			while (Lexer.Peek().IsSymbol(TEXT(".")) || Lexer.Peek().IsSymbol(TEXT("/")))
			{
				const FString Separator = Lexer.Peek().Text;
				if (Lexer.Peek(1).Kind != ETokenKind::Identifier)
				{
					break;
				}
				Lexer.Next();
				Name += Separator;
				Name += Lexer.Next().Text;
			}

			OutName = Name;
			return true;
		}

		bool FParserImpl::ParseTypeName(FString& OutTypeName, FString& OutInnerTypeName)
		{
			if (!ExpectIdentifier(OutTypeName))
			{
				return false;
			}

			// `DI<SkeletalMesh>` -- the only generic form in the language.
			if (Lexer.Peek().IsSymbol(TEXT("<")))
			{
				Lexer.Next();
				if (!ExpectIdentifier(OutInnerTypeName))
				{
					return false;
				}
				if (!Expect(TEXT(">")))
				{
					return false;
				}
			}
			return true;
		}

		// -------------------------------------------------------------------------------------
		// Values
		// -------------------------------------------------------------------------------------

		FValuePtr FParserImpl::ParseValue()
		{
			return ParseAdditive();
		}

		FValuePtr FParserImpl::ParseAdditive()
		{
			FValuePtr Left = ParseMultiplicative();
			while (Left.IsValid() && (Lexer.Peek().IsSymbol(TEXT("+")) || Lexer.Peek().IsSymbol(TEXT("-"))))
			{
				const FToken Operator = Lexer.Next();
				FValuePtr Right = ParseMultiplicative();
				if (!Right.IsValid())
				{
					return nullptr;
				}
				FValuePtr Node = FValue::Make(EValueKind::Binary, Operator.Location);
				Node->Text = Operator.Text;
				Node->Left = Left;
				Node->Right = Right;
				Left = Node;
			}
			return Left;
		}

		FValuePtr FParserImpl::ParseMultiplicative()
		{
			FValuePtr Left = ParseUnary();
			while (Left.IsValid()
				&& (Lexer.Peek().IsSymbol(TEXT("*")) || Lexer.Peek().IsSymbol(TEXT("/")) || Lexer.Peek().IsSymbol(TEXT("%"))))
			{
				const FToken Operator = Lexer.Next();
				FValuePtr Right = ParseUnary();
				if (!Right.IsValid())
				{
					return nullptr;
				}
				FValuePtr Node = FValue::Make(EValueKind::Binary, Operator.Location);
				Node->Text = Operator.Text;
				Node->Left = Left;
				Node->Right = Right;
				Left = Node;
			}
			return Left;
		}

		FValuePtr FParserImpl::ParseUnary()
		{
			if (Lexer.Peek().IsSymbol(TEXT("-")))
			{
				const FToken Operator = Lexer.Next();
				FValuePtr Operand = ParseUnary();
				if (!Operand.IsValid())
				{
					return nullptr;
				}

				// Fold `-3.5` into a single literal so that negative defaults compare equal to the
				// schema defaults they mirror, instead of showing up as an expression tree.
				if (Operand->Kind == EValueKind::Number)
				{
					Operand->Number = -Operand->Number;
					Operand->Location = Operator.Location;
					return Operand;
				}

				FValuePtr Node = FValue::Make(EValueKind::Negate, Operator.Location);
				Node->Left = Operand;
				return Node;
			}

			if (Lexer.Peek().IsSymbol(TEXT("+")))
			{
				Lexer.Next();
				return ParseUnary();
			}

			return ParsePrimary();
		}

		FValuePtr FParserImpl::ParseCurveLiteral(const FSourceLocation& Location)
		{
			FValuePtr Node = FValue::Make(EValueKind::Curve, Location);
			if (!Expect(TEXT("{")))
			{
				return nullptr;
			}

			while (!Lexer.Peek().IsEnd() && !Lexer.Peek().IsSymbol(TEXT("}")))
			{
				FCurveKey Key;
				Key.Location = Lexer.Peek().Location;

				if (!ParseSignedNumber(Key.Time))
				{
					SkipToStatementEnd();
					continue;
				}
				if (!Expect(TEXT("->")))
				{
					SkipToStatementEnd();
					continue;
				}
				if (!ParseSignedNumber(Key.Value))
				{
					SkipToStatementEnd();
					continue;
				}

				if (Lexer.Peek().IsSymbol(TEXT("[")))
				{
					TArray<FAttribute> Attributes;
					if (!ParseAttributeList(Attributes))
					{
						SkipToStatementEnd();
						continue;
					}
					for (const FAttribute& Attribute : Attributes)
					{
						const bool bHasNumber = Attribute.Value.IsValid() && Attribute.Value->Kind == EValueKind::Number;
						if (Attribute.Key == TEXT("Interp"))
						{
							Key.Interpolation = Attribute.Value.IsValid() ? Attribute.Value->Text : FString();
						}
						else if (Attribute.Key == TEXT("Arrive"))
						{
							Key.bHasArrive = bHasNumber;
							Key.ArriveTangent = bHasNumber ? static_cast<float>(Attribute.Value->Number) : 0.0f;
						}
						else if (Attribute.Key == TEXT("Leave"))
						{
							Key.bHasLeave = bHasNumber;
							Key.LeaveTangent = bHasNumber ? static_cast<float>(Attribute.Value->Number) : 0.0f;
						}
						else
						{
							Diagnostics.Error(TEXT("DFX2020"), Attribute.Location,
								FString::Printf(TEXT("Unknown curve key attribute '%s'. Expected Interp, Arrive or Leave."),
									*Attribute.Key));
						}
					}
				}

				Node->CurveKeys.Add(MoveTemp(Key));
				Lexer.TryConsumeSymbol(TEXT(";"));
			}

			if (!Expect(TEXT("}")))
			{
				return nullptr;
			}
			return Node;
		}

		bool FParserImpl::ParseSignedNumber(float& OutNumber)
		{
			bool bNegate = false;
			if (Lexer.Peek().IsSymbol(TEXT("-")))
			{
				Lexer.Next();
				bNegate = true;
			}
			const FToken& Token = Lexer.Peek();
			if (Token.Kind != ETokenKind::Number)
			{
				Diagnostics.Error(TEXT("DFX2003"), Token.Location,
					FString::Printf(TEXT("Expected a number but found '%s'."), *Token.Text));
				return false;
			}
			OutNumber = static_cast<float>(Lexer.Next().Number) * (bNegate ? -1.0f : 1.0f);
			return true;
		}

		FValuePtr FParserImpl::ParsePrimary()
		{
			const FToken& Token = Lexer.Peek();
			const FSourceLocation Location = Token.Location;

			switch (Token.Kind)
			{
			case ETokenKind::Number:
			{
				const FToken Number = Lexer.Next();
				return FValue::MakeNumber(Number.Number, Number.bIsIntegerLiteral, Location);
			}

			case ETokenKind::String:
				return FValue::MakeString(Lexer.Next().Text, Location);

			case ETokenKind::Identifier:
			{
				if (Token.Text == TEXT("true") || Token.Text == TEXT("false"))
				{
					return FValue::MakeBool(Lexer.Next().Text == TEXT("true"), Location);
				}

				if (Token.Text == TEXT("hlsl") && Lexer.Peek(1).IsSymbol(TEXT("{")))
				{
					Lexer.Next();
					FValuePtr Node = FValue::Make(EValueKind::Hlsl, Location);
					FSourceLocation BlockLocation;
					if (!Lexer.ReadRawBlock(Node->Text, BlockLocation))
					{
						return nullptr;
					}
					return Node;
				}

				if (Token.Text == TEXT("curve") && Lexer.Peek(1).IsSymbol(TEXT("{")))
				{
					Lexer.Next();
					return ParseCurveLiteral(Location);
				}

				FString Name;
				FSourceLocation NameLocation;
				if (!ParseQualifiedName(Name, NameLocation))
				{
					return nullptr;
				}

				// `MakeFloatFromLinearColor@1.0(...)` -- the dynamic input answer to a module's version
				// pin. Read before the '(' for the same reason it is written there: the version is
				// part of naming the thing being called, not one of its arguments.
				FString VersionPin;
				if (Lexer.Peek().IsSymbol(TEXT("@")))
				{
					Lexer.Next();
					const FToken Version = Lexer.Next();
					if (Version.Kind != ETokenKind::Identifier && Version.Kind != ETokenKind::Number)
					{
						Diagnostics.Error(TEXT("DFX2007"), Version.Location,
							TEXT("Expected a version after '@'."));
						return nullptr;
					}
					VersionPin = Version.Text;
				}

				if (Lexer.Peek().IsSymbol(TEXT("(")))
				{
					FValuePtr Node = FValue::Make(EValueKind::Call, Location);
					Node->Text = Name;
					Node->VersionPin = VersionPin;
					if (!ParseArgumentList(Node->Arguments, Node->Elements))
					{
						return nullptr;
					}
					return Node;
				}

				if (!VersionPin.IsEmpty())
				{
					Diagnostics.Error(TEXT("DFX2007"), Location,
						FString::Printf(TEXT("'%s@%s' is not a call. A version pin only means something on a dynamic input call, e.g. %s@%s(...)."),
							*Name, *VersionPin, *Name, *VersionPin));
					return nullptr;
				}

				return FValue::MakeName(Name, Location);
			}

			case ETokenKind::Symbol:
			{
				if (Token.IsSymbol(TEXT("(")))
				{
					Lexer.Next();
					FValuePtr First = ParseValue();
					if (!First.IsValid())
					{
						return nullptr;
					}

					// '(' is a grouping paren until a comma proves it was a vector literal.
					if (!Lexer.Peek().IsSymbol(TEXT(",")))
					{
						if (!Expect(TEXT(")")))
						{
							return nullptr;
						}
						return First;
					}

					FValuePtr Node = FValue::Make(EValueKind::Vector, Location);
					Node->Elements.Add(First);
					while (Lexer.TryConsumeSymbol(TEXT(",")))
					{
						FValuePtr Component = ParseValue();
						if (!Component.IsValid())
						{
							return nullptr;
						}
						Node->Elements.Add(Component);
					}
					if (!Expect(TEXT(")")))
					{
						return nullptr;
					}
					return Node;
				}

				if (Token.IsSymbol(TEXT("[")))
				{
					Lexer.Next();
					FValuePtr Node = FValue::Make(EValueKind::Array, Location);
					if (!Lexer.Peek().IsSymbol(TEXT("]")))
					{
						do
						{
							FValuePtr Element = ParseValue();
							if (!Element.IsValid())
							{
								return nullptr;
							}
							Node->Elements.Add(Element);
						}
						while (Lexer.TryConsumeSymbol(TEXT(",")));
					}
					if (!Expect(TEXT("]")))
					{
						return nullptr;
					}
					return Node;
				}

				if (Token.IsSymbol(TEXT("/")))
				{
					// An absolute asset path used as a value, e.g. a fully qualified module name.
					FString Name;
					FSourceLocation NameLocation;
					if (!ParseQualifiedName(Name, NameLocation))
					{
						return nullptr;
					}
					if (Lexer.Peek().IsSymbol(TEXT("(")))
					{
						FValuePtr Node = FValue::Make(EValueKind::Call, Location);
						Node->Text = Name;
						if (!ParseArgumentList(Node->Arguments, Node->Elements))
						{
							return nullptr;
						}
						return Node;
					}
					return FValue::MakeName(Name, Location);
				}
				break;
			}

			default:
				break;
			}

			Diagnostics.Error(TEXT("DFX2004"), Location,
				FString::Printf(TEXT("Expected a value but found %s."),
					Token.IsEnd() ? TEXT("end of file") : *FString::Printf(TEXT("'%s'"), *Token.Text)));
			return nullptr;
		}

		bool FParserImpl::ParseArgumentList(TArray<FNamedArgument>& OutArguments, TArray<FValuePtr>& OutPositional)
		{
			if (!Expect(TEXT("(")))
			{
				return false;
			}

			if (Lexer.TryConsumeSymbol(TEXT(")")))
			{
				return true;
			}

			do
			{
				const FToken& Token = Lexer.Peek();

				// `Name = Value` is a named argument; anything else is positional. `==` is not an
				// assignment, so it must not be mistaken for one.
				if (Token.Kind == ETokenKind::Identifier && Lexer.Peek(1).IsSymbol(TEXT("=")))
				{
					FNamedArgument Argument;
					Argument.Location = Token.Location;
					Argument.Name = Lexer.Next().Text;
					Lexer.Next(); // '='
					Argument.Value = ParseValue();
					if (!Argument.Value.IsValid())
					{
						return false;
					}
					OutArguments.Add(MoveTemp(Argument));
				}
				else
				{
					FValuePtr Value = ParseValue();
					if (!Value.IsValid())
					{
						return false;
					}
					OutPositional.Add(Value);
				}
			}
			while (Lexer.TryConsumeSymbol(TEXT(",")));

			return Expect(TEXT(")"));
		}

		bool FParserImpl::ParseAttributeList(TArray<FAttribute>& OutAttributes)
		{
			if (!Expect(TEXT("[")))
			{
				return false;
			}

			if (Lexer.TryConsumeSymbol(TEXT("]")))
			{
				return true;
			}

			while (true)
			{
				FAttribute Attribute;
				Attribute.Location = Lexer.Peek().Location;
				if (!ExpectIdentifier(Attribute.Key))
				{
					return false;
				}

				if (Lexer.TryConsumeSymbol(TEXT("=")))
				{
					Attribute.Value = ParseValue();
					if (!Attribute.Value.IsValid())
					{
						return false;
					}
				}

				OutAttributes.Add(MoveTemp(Attribute));

				// Both ';' and ',' separate attributes: the family syntax uses ';', but ',' reads
				// naturally enough that rejecting it would only generate noise.
				if (Lexer.TryConsumeSymbol(TEXT(";")) || Lexer.TryConsumeSymbol(TEXT(",")))
				{
					if (Lexer.Peek().IsSymbol(TEXT("]")))
					{
						break;
					}
					continue;
				}
				break;
			}

			return Expect(TEXT("]"));
		}

		// -------------------------------------------------------------------------------------
		// Blocks
		// -------------------------------------------------------------------------------------

		bool FParserImpl::ParseSettingsBlock(TArray<FPropertyEntry>& OutProperties)
		{
			if (!Expect(TEXT("=")) || !Expect(TEXT("{")))
			{
				return false;
			}

			while (!Lexer.Peek().IsEnd() && !Lexer.Peek().IsSymbol(TEXT("}")))
			{
				FPropertyEntry Property;
				Property.Location = Lexer.Peek().Location;
				if (!ExpectIdentifier(Property.Name))
				{
					SkipToStatementEnd();
					continue;
				}
				if (!Expect(TEXT("=")))
				{
					SkipToStatementEnd();
					continue;
				}
				Property.Value = ParseValue();
				if (!Property.Value.IsValid())
				{
					SkipToStatementEnd();
					continue;
				}
				Lexer.TryConsumeSymbol(TEXT(";"));
				OutProperties.Add(MoveTemp(Property));
			}

			return Expect(TEXT("}"));
		}

		bool FParserImpl::ParseParameterBlock(TArray<FParameterDecl>& OutParameters)
		{
			if (!Expect(TEXT("=")) || !Expect(TEXT("{")))
			{
				return false;
			}

			while (!Lexer.Peek().IsEnd() && !Lexer.Peek().IsSymbol(TEXT("}")))
			{
				FParameterDecl Parameter;
				Parameter.Location = Lexer.Peek().Location;

				if (!ParseTypeName(Parameter.TypeName, Parameter.InnerTypeName))
				{
					SkipToStatementEnd();
					continue;
				}
				if (!ExpectIdentifier(Parameter.Name))
				{
					SkipToStatementEnd();
					continue;
				}
				if (Lexer.TryConsumeSymbol(TEXT("=")))
				{
					Parameter.DefaultValue = ParseValue();
					if (!Parameter.DefaultValue.IsValid())
					{
						SkipToStatementEnd();
						continue;
					}
				}
				if (Lexer.Peek().IsSymbol(TEXT("[")) && !ParseAttributeList(Parameter.Attributes))
				{
					SkipToStatementEnd();
					continue;
				}
				Lexer.TryConsumeSymbol(TEXT(";"));
				OutParameters.Add(MoveTemp(Parameter));
			}

			return Expect(TEXT("}"));
		}

		bool FParserImpl::ParseStatement(FStack& OutStack, TArray<FString>& RegionStack)
		{
			const FToken& Token = Lexer.Peek();

			// `#Region "label"` / `#EndRegion`. v1 keeps these as text only (L5).
			if (Token.IsSymbol(TEXT("#")))
			{
				Lexer.Next();
				FString Directive;
				if (!ExpectIdentifier(Directive))
				{
					SkipToStatementEnd();
					return false;
				}
				if (Directive == TEXT("Region"))
				{
					FString Label;
					if (Lexer.Peek().Kind == ETokenKind::String)
					{
						Label = Lexer.Next().Text;
					}
					RegionStack.Push(Label);
				}
				else if (Directive == TEXT("EndRegion"))
				{
					if (RegionStack.Num() == 0)
					{
						Diagnostics.Error(TEXT("DFX2005"), Token.Location,
							TEXT("'#EndRegion' has no matching '#Region'."));
					}
					else
					{
						RegionStack.Pop();
					}
				}
				else
				{
					Diagnostics.Error(TEXT("DFX2006"), Token.Location,
						FString::Printf(TEXT("Unknown directive '#%s'. Expected Region or EndRegion."), *Directive));
				}
				return true;
			}

			FStatement Statement;
			Statement.Location = Token.Location;
			Statement.Region = RegionStack.Num() > 0 ? RegionStack.Last() : FString();

			// `disabled GravityForce(...)`. Consumed before the type rule below, which would otherwise
			// read `disabled` as the type of a declaration. Requiring a name after it keeps a
			// parameter that happens to be called `disabled` working: `disabled = false;` is an
			// assignment, and only `disabled <name>` is the prefix.
			//
			// The name may also start with '/', because a module can be written as a full asset path --
			// and one always is when it has no short name to resolve by. Without the second case the
			// prefix was not consumed at all and `disabled /Game/FX/X()` parsed as a single module
			// named `disabled/Game/FX/X`, which then failed to resolve (plan-v5 R3).
			const FSourceLocation DisabledLocation = Token.Location;
			if (Token.IsIdentifier(TEXT("disabled"))
				&& (Lexer.Peek(1).Kind == ETokenKind::Identifier || Lexer.Peek(1).IsSymbol(TEXT("/"))))
			{
				Lexer.Next();
				Statement.bDisabled = true;
				Statement.Location = Lexer.Peek().Location;
			}

			// Peek() hands back a reference into the lexer's queue, which Next() above has just
			// reshuffled, so re-read rather than reusing `Token` from here on.
			const FToken& Head = Lexer.Peek();

			// `float Particles.X = ...` -- a type followed by a name. Unambiguous inside a stack: a
			// module call is followed by '(' and a bare assignment by '.' or '=', never by another
			// identifier. `DI<X> Name` is picked up by the '<' too.
			if (Head.Kind == ETokenKind::Identifier
				&& (Lexer.Peek(1).Kind == ETokenKind::Identifier || Lexer.Peek(1).IsSymbol(TEXT("<"))))
			{
				if (!ParseTypeName(Statement.TypeName, Statement.InnerTypeName))
				{
					SkipToStatementEnd();
					return false;
				}
			}

			FSourceLocation NameLocation;
			if (!ParseQualifiedName(Statement.Name, NameLocation))
			{
				SkipToStatementEnd();
				return false;
			}

			// R7 version pin: `AddVelocityInCone@2(...)`. Parsed now, honoured once module versioning
			// lands; recorded on the statement either way so the provenance stamp can see it.
			if (Lexer.Peek().IsSymbol(TEXT("@")))
			{
				Lexer.Next();
				const FToken Version = Lexer.Next();
				if (Version.Kind != ETokenKind::Identifier && Version.Kind != ETokenKind::Number)
				{
					Diagnostics.Error(TEXT("DFX2007"), Version.Location,
						TEXT("Expected a version after '@'."));
					SkipToStatementEnd();
					return false;
				}
				Statement.VersionPin = Version.Text;
			}

			if (Lexer.Peek().IsSymbol(TEXT("(")))
			{
				Statement.Kind = EStatementKind::ModuleCall;
				TArray<FValuePtr> Positional;
				if (!ParseArgumentList(Statement.Arguments, Positional))
				{
					SkipToStatementEnd();
					return false;
				}
				if (Positional.Num() > 0)
				{
					Diagnostics.Error(TEXT("DFX2008"), Positional[0]->Location,
						FString::Printf(TEXT("Module '%s' was given a positional argument. Module inputs must be written as 'Name = Value'."),
							*Statement.Name));
					SkipToStatementEnd();
					return false;
				}

				// Optional node name: `Grid3D_ResampleFloat() as Grid3D_ResampleFloat003;`. The name
				// is what `Output.<node>.<value>` links resolve against, so a rebuild has to land
				// the node on it rather than on whatever the add counter says next.
				if (Lexer.Peek().IsIdentifier(TEXT("as")))
				{
					Lexer.Next();
					if (!ExpectIdentifier(Statement.InstanceName))
					{
						SkipToStatementEnd();
						return false;
					}
				}
			}
			else if (Lexer.Peek().IsSymbol(TEXT("=")))
			{
				Lexer.Next();
				Statement.Kind = EStatementKind::Assignment;
				Statement.Value = ParseValue();
				if (!Statement.Value.IsValid())
				{
					SkipToStatementEnd();
					return false;
				}
			}
			else
			{
				ErrorAtCurrent(TEXT("DFX2009"),
					FString::Printf(TEXT("Expected '(' to call module '%s' or '=' to assign to it."), *Statement.Name));
				SkipToStatementEnd();
				return false;
			}

			if (!Statement.TypeName.IsEmpty() && Statement.Kind != EStatementKind::Assignment)
			{
				Diagnostics.Error(TEXT("DFX2023"), Statement.Location,
					FString::Printf(TEXT("'%s' is a module call, so it cannot be given a type. Types are written only on assignments."),
						*Statement.Name));
				return false;
			}

			if (Statement.bDisabled && Statement.Kind != EStatementKind::ModuleCall)
			{
				// An assignment has nothing to disable -- it is written into the stack's own Set
				// Parameters module, and turning that off would silently drop every other assignment
				// beside it.
				Diagnostics.Error(TEXT("DFX2024"), DisabledLocation,
					FString::Printf(TEXT("'disabled' can only prefix a module call, and '%s' is an assignment."),
						*Statement.Name));
				return false;
			}

			Lexer.TryConsumeSymbol(TEXT(";"));
			OutStack.Statements.Add(MoveTemp(Statement));
			return true;
		}

		bool FParserImpl::ParseStackBlock(FStack& OutStack)
		{
			if (!Expect(TEXT("=")) || !Expect(TEXT("{")))
			{
				return false;
			}

			TArray<FString> RegionStack;
			while (!Lexer.Peek().IsEnd() && !Lexer.Peek().IsSymbol(TEXT("}")))
			{
				const int32 BeforeOffset = Lexer.Peek().Offset;
				ParseStatement(OutStack, RegionStack);

				// Guarantee forward progress: a recovery path that consumed nothing would spin here.
				if (Lexer.Peek().Offset == BeforeOffset && !Lexer.Peek().IsEnd())
				{
					Lexer.Next();
				}
			}

			if (RegionStack.Num() > 0)
			{
				Diagnostics.Warning(TEXT("DFX2010"), OutStack.Location,
					FString::Printf(TEXT("'#Region \"%s\"' was never closed with '#EndRegion'."), *RegionStack.Last()));
			}

			return Expect(TEXT("}"));
		}

		bool FParserImpl::ParseRendererDeclaration(FRenderer& OutRenderer)
		{
			OutRenderer.Location = Lexer.Peek().Location;
			if (!ExpectIdentifier(OutRenderer.TypeName))
			{
				return false;
			}
			if (Lexer.Peek().Kind == ETokenKind::Identifier)
			{
				OutRenderer.Name = Lexer.Next().Text;
			}
			if (!Expect(TEXT("{")))
			{
				return false;
			}

			while (!Lexer.Peek().IsEnd() && !Lexer.Peek().IsSymbol(TEXT("}")))
			{
				const FToken& Token = Lexer.Peek();
				const int32 BeforeOffset = Token.Offset;

				if (Token.IsIdentifier(TEXT("Bind")))
				{
					FRendererBinding Binding;
					Binding.Location = Token.Location;
					Lexer.Next();
					if (!ExpectIdentifier(Binding.PropertyName) || !Expect(TEXT("->")))
					{
						SkipToStatementEnd();
						continue;
					}
					FSourceLocation TargetLocation;
					if (!ParseQualifiedName(Binding.Target, TargetLocation))
					{
						SkipToStatementEnd();
						continue;
					}
					Lexer.TryConsumeSymbol(TEXT(";"));
					OutRenderer.Bindings.Add(MoveTemp(Binding));
				}
				else if (Token.IsIdentifier(TEXT("MaterialParam")))
				{
					FPropertyEntry Parameter;
					Parameter.Location = Token.Location;
					Lexer.Next();
					if (!ExpectIdentifier(Parameter.Name) || !Expect(TEXT("=")))
					{
						SkipToStatementEnd();
						continue;
					}
					Parameter.Value = ParseValue();
					if (!Parameter.Value.IsValid())
					{
						SkipToStatementEnd();
						continue;
					}
					Lexer.TryConsumeSymbol(TEXT(";"));
					OutRenderer.MaterialParameters.Add(MoveTemp(Parameter));
				}
				else
				{
					FPropertyEntry Property;
					Property.Location = Token.Location;
					if (!ExpectIdentifier(Property.Name) || !Expect(TEXT("=")))
					{
						SkipToStatementEnd();
						continue;
					}
					Property.Value = ParseValue();
					if (!Property.Value.IsValid())
					{
						SkipToStatementEnd();
						continue;
					}
					Lexer.TryConsumeSymbol(TEXT(";"));
					OutRenderer.Properties.Add(MoveTemp(Property));
				}

				if (Lexer.Peek().Offset == BeforeOffset && !Lexer.Peek().IsEnd())
				{
					Lexer.Next();
				}
			}

			return Expect(TEXT("}"));
		}

		bool FParserImpl::ParseEmitterBody(FEmitter& OutEmitter, bool bAllowRenderers)
		{
			if (!Expect(TEXT("{")))
			{
				return false;
			}

			while (!Lexer.Peek().IsEnd() && !Lexer.Peek().IsSymbol(TEXT("}")))
			{
				const FToken& Token = Lexer.Peek();
				const int32 BeforeOffset = Token.Offset;

				if (Token.Kind != ETokenKind::Identifier)
				{
					ErrorAtCurrent(TEXT("DFX2011"),
						FString::Printf(TEXT("Unexpected '%s' inside an emitter body."), *Token.Text));
					SkipToStatementEnd();
					continue;
				}

				const FString Keyword = Token.Text;

				if (Keyword == TEXT("Settings"))
				{
					Lexer.Next();
					ParseSettingsBlock(OutEmitter.Settings);
				}
				else if (Keyword == TEXT("Defaults"))
				{
					// Reuses the stack block wholesale: a default is an assignment, and giving it its
					// own statement grammar would only mean two places to keep in step.
					Lexer.Next();
					FStack DefaultsBlock;
					DefaultsBlock.Location = Token.Location;
					if (ParseStackBlock(DefaultsBlock))
					{
						for (FStatement& Statement : DefaultsBlock.Statements)
						{
							if (Statement.Kind != EStatementKind::Assignment)
							{
								Diagnostics.Error(TEXT("DFX2016"), Statement.Location,
									TEXT("A Defaults block holds assignments only: a default says what "
									     "reading a parameter produces when nothing set it, so there is "
									     "nothing for a module call to mean here."));
								continue;
							}
							OutEmitter.Defaults.Add(MoveTemp(Statement));
						}
					}
				}
				else if (Keyword == TEXT("Stage"))
				{
					// A simulation stage: a named particle stack that runs after the update, plus the
					// stage properties (iteration source, DI binding, count, enabled) in the same
					// parenthesised attribute form OnEvent uses. The arguments are optional because,
					// unlike an event handler, a bare stage is meaningful: an enabled
					// particles-iteration stage that runs once, the engine's own default.
					const FSourceLocation Location = Token.Location;
					Lexer.Next();
					FStack Stack;
					Stack.Kind = EStackKind::SimulationStage;
					Stack.Location = Location;
					if (ExpectIdentifier(Stack.Name))
					{
						Stack.Stage.Name = Stack.Name;
						const bool bArgumentsOk = !Lexer.Peek().IsSymbol(TEXT("("))
							|| ParseSimulationStageArguments(Stack.Stage);
						if (bArgumentsOk && ParseStackBlock(Stack))
						{
							OutEmitter.Stacks.Add(MoveTemp(Stack));
						}
					}
				}
				else if (Keyword == TEXT("OnEvent"))
				{
					// The reserved spelling, grown up: the bare `OnEvent name = {}` form never carried
					// the properties an event handler is made of (which emitter's events, which event,
					// how to run), so the arguments moved into the same parenthesised attribute form
					// the document headers already use.
					const FSourceLocation Location = Token.Location;
					Lexer.Next();
					FStack Stack;
					Stack.Kind = EStackKind::EventHandler;
					Stack.Location = Location;
					if (ParseEventHandlerArguments(Stack.Handler) && ParseStackBlock(Stack))
					{
						OutEmitter.Stacks.Add(MoveTemp(Stack));
					}
				}
				else
				{
					EStackKind StackKind;
					if (ParseStackKind(Keyword, StackKind))
					{
						if (IsSystemScopeStack(StackKind))
						{
							Diagnostics.Error(TEXT("DFX2013"), Token.Location,
								FString::Printf(TEXT("'%s' is a system-scope stack and must be written at the top level of a .dfs, not inside an emitter."),
									*Keyword));
							Lexer.Next();
							FStack Discarded;
							Discarded.Kind = StackKind;
							Discarded.Location = Token.Location;
							ParseStackBlock(Discarded);
							continue;
						}
						FStack Stack;
						Stack.Kind = StackKind;
						Stack.Location = Token.Location;
						Lexer.Next();
						if (ParseStackBlock(Stack))
						{
							OutEmitter.Stacks.Add(MoveTemp(Stack));
						}
					}
					else if (bAllowRenderers && (Lexer.Peek(1).Kind == ETokenKind::Identifier || Lexer.Peek(1).IsSymbol(TEXT("{"))))
					{
						FRenderer Renderer;
						if (ParseRendererDeclaration(Renderer))
						{
							OutEmitter.Renderers.Add(MoveTemp(Renderer));
						}
					}
					else
					{
						ErrorAtCurrent(TEXT("DFX2014"),
							FString::Printf(TEXT("Unknown emitter section '%s'. Expected Settings, Defaults, EmitterSpawn, EmitterUpdate, ParticleSpawn, ParticleUpdate, Stage, OnEvent or a renderer declaration."),
								*Keyword));
						Lexer.Next();
						if (Lexer.Peek().IsSymbol(TEXT("=")))
						{
							Lexer.Next();
						}
						if (Lexer.Peek().IsSymbol(TEXT("{")))
						{
							SkipBalancedBlock();
						}
						else
						{
							SkipToStatementEnd();
						}
					}
				}

				if (Lexer.Peek().Offset == BeforeOffset && !Lexer.Peek().IsEnd())
				{
					Lexer.Next();
				}
			}

			return Expect(TEXT("}"));
		}

		/**
		 * The parenthesised arguments of an `OnEvent(...)` block, in the header-attribute style.
		 *
		 * Source and Mode are identifiers (an emitter name; an EScriptExecutionMode entry), Event is
		 * a string or an identifier, the numbers are integers and the flags are true/false. Source
		 * and Event are required -- a handler that does not say whose events it wants is not a
		 * handler -- and validation of Source against the declared emitters happens at build time,
		 * where the final emitter list exists.
		 */
		bool FParserImpl::ParseEventHandlerArguments(FEventHandlerSpec& OutSpec)
		{
			if (!Expect(TEXT("(")))
			{
				return false;
			}

			do
			{
				FString Key;
				const FSourceLocation KeyLocation = Lexer.Peek().Location;
				if (!ExpectIdentifier(Key) || !Expect(TEXT("=")))
				{
					return false;
				}

				const FToken& ValueToken = Lexer.Peek();

				auto ReadIdentifier = [this](FString& Out)
				{
					return ExpectIdentifier(Out);
				};
				auto ReadInteger = [this, &ValueToken](int32& Out)
				{
					if (ValueToken.Kind != ETokenKind::Number)
					{
						return false;
					}
					Out = static_cast<int32>(Lexer.Next().Number);
					return true;
				};
				auto ReadBool = [this, &ValueToken](bool& Out)
				{
					if (ValueToken.Kind == ETokenKind::Identifier
						&& (ValueToken.Text == TEXT("true") || ValueToken.Text == TEXT("false")))
					{
						Out = Lexer.Next().Text == TEXT("true");
						return true;
					}
					return false;
				};

				bool bOk = true;
				if (Key == TEXT("Source"))
				{
					bOk = ReadIdentifier(OutSpec.Source);
				}
				else if (Key == TEXT("Event"))
				{
					if (ValueToken.Kind == ETokenKind::String || ValueToken.Kind == ETokenKind::Identifier)
					{
						OutSpec.Event = Lexer.Next().Text;
					}
					else
					{
						bOk = false;
					}
				}
				else if (Key == TEXT("Mode"))
				{
					bOk = ReadIdentifier(OutSpec.Mode);
				}
				else if (Key == TEXT("SpawnNumber"))
				{
					bOk = ReadInteger(OutSpec.SpawnNumber);
				}
				else if (Key == TEXT("MaxEventsPerFrame"))
				{
					int32 Value = 0;
					bOk = ReadInteger(Value);
					OutSpec.MaxEventsPerFrame = Value;
				}
				else if (Key == TEXT("MinSpawnNumber"))
				{
					int32 Value = 0;
					bOk = ReadInteger(Value);
					OutSpec.MinSpawnNumber = Value;
				}
				else if (Key == TEXT("UpdateAttributeInitialValues"))
				{
					bool Value = true;
					bOk = ReadBool(Value);
					OutSpec.UpdateAttributeInitialValues = Value;
				}
				else if (Key == TEXT("RandomSpawnNumber"))
				{
					bool Value = false;
					bOk = ReadBool(Value);
					OutSpec.RandomSpawnNumber = Value;
				}
				else
				{
					Diagnostics.Error(TEXT("DFX2025"), KeyLocation,
						FString::Printf(TEXT("Unknown OnEvent argument '%s'. Expected Source, Event, Mode, SpawnNumber, MaxEventsPerFrame, UpdateAttributeInitialValues, RandomSpawnNumber or MinSpawnNumber."),
							*Key));
					return false;
				}

				if (!bOk)
				{
					Diagnostics.Error(TEXT("DFX2025"), ValueToken.Location,
						FString::Printf(TEXT("OnEvent argument '%s' has the wrong shape: Source and Mode are identifiers, Event is a name or string, the numbers are integers and the flags are true/false."),
							*Key));
					return false;
				}
			}
			while (Lexer.TryConsumeSymbol(TEXT(",")));

			if (!Expect(TEXT(")")))
			{
				return false;
			}

			if (OutSpec.Source.IsEmpty() || OutSpec.Event.IsEmpty())
			{
				Diagnostics.Error(TEXT("DFX2025"), Lexer.Peek().Location,
					TEXT("OnEvent needs at least Source (the emitter whose events to receive) and Event (the event's name)."));
				return false;
			}
			return true;
		}

		/**
		 * The parenthesised arguments of a `Stage name(...)` block, in the OnEvent attribute style.
		 *
		 * Iteration is an identifier naming an ENiagaraIterationSource entry (Particles,
		 * DataInterface, DirectSet). DataInterface is the bound grid parameter -- a dotted name like
		 * Emitter.PressureGrid, written as a string or as bare identifiers -- and saying it already
		 * says the iteration is DataInterface, so Iteration may then be omitted. NumIterations is an
		 * integer, Enabled is true/false. Whether the DataInterface parameter actually exists is a
		 * build-time question, answered where the emitter's parameters do.
		 */
		bool FParserImpl::ParseSimulationStageArguments(FSimulationStageSpec& OutSpec)
		{
			if (!Expect(TEXT("(")))
			{
				return false;
			}

			// `Stage X() = {}` reads as all defaults, same as no parentheses at all.
			if (Lexer.Peek().IsSymbol(TEXT(")")))
			{
				Lexer.Next();
				return true;
			}

			do
			{
				FString Key;
				const FSourceLocation KeyLocation = Lexer.Peek().Location;
				if (!ExpectIdentifier(Key) || !Expect(TEXT("=")))
				{
					return false;
				}

				const FToken& ValueToken = Lexer.Peek();

				// A dotted parameter name: "Emitter.PressureGrid" as one string, or as the same
				// identifier-dot-identifier sequence an assignment's left side would be. Peeked
				// fresh rather than through ValueToken -- the DataInterface form may consume a
				// DI<...> type first, after which that reference describes a consumed token.
				auto ReadDottedName = [this](FString& Out)
				{
					const FToken& NameToken = Lexer.Peek();
					if (NameToken.Kind == ETokenKind::String)
					{
						Out = Lexer.Next().Text;
						return true;
					}
					if (NameToken.Kind != ETokenKind::Identifier)
					{
						return false;
					}
					Out = Lexer.Next().Text;
					while (Lexer.TryConsumeSymbol(TEXT(".")))
					{
						FString Part;
						if (!ExpectIdentifier(Part))
						{
							return false;
						}
						Out += TEXT(".") + Part;
					}
					return true;
				};

				bool bOk = true;
				if (Key == TEXT("Iteration"))
				{
					bOk = ExpectIdentifier(OutSpec.Iteration);
				}
				else if (Key == TEXT("ExecuteBehavior"))
				{
					bOk = ExpectIdentifier(OutSpec.ExecuteBehavior);
				}
				else if (Key == TEXT("DataInterface"))
				{
					bOk = ReadDottedName(OutSpec.DataInterface);
				}
				else if (Key == TEXT("NumIterations"))
				{
					if (ValueToken.Kind == ETokenKind::Number)
					{
						OutSpec.NumIterations = static_cast<int32>(Lexer.Next().Number);
					}
					else
					{
						bOk = false;
					}
				}
				else if (Key == TEXT("Enabled"))
				{
					if (ValueToken.Kind == ETokenKind::Identifier
						&& (ValueToken.Text == TEXT("true") || ValueToken.Text == TEXT("false")))
					{
						OutSpec.Enabled = Lexer.Next().Text == TEXT("true");
					}
					else
					{
						bOk = false;
					}
				}
				else
				{
					Diagnostics.Error(TEXT("DFX2026"), KeyLocation,
						FString::Printf(TEXT("Unknown Stage argument '%s'. Expected Iteration, DataInterface, NumIterations, ExecuteBehavior or Enabled."),
							*Key));
					return false;
				}

				if (!bOk)
				{
					Diagnostics.Error(TEXT("DFX2026"), ValueToken.Location,
						FString::Printf(TEXT("Stage argument '%s' has the wrong shape: Iteration and ExecuteBehavior are identifiers, DataInterface is a dotted parameter name, NumIterations is an integer and Enabled is true/false."),
							*Key));
					return false;
				}
			}
			while (Lexer.TryConsumeSymbol(TEXT(",")));

			return Expect(TEXT(")"));
		}

		bool FParserImpl::ParseEmitterDeclaration(FEmitter& OutEmitter)
		{
			OutEmitter.Location = Lexer.Peek().Location;
			Lexer.Next(); // 'Emitter'

			if (!ExpectIdentifier(OutEmitter.Name))
			{
				return false;
			}

			if (Lexer.Peek().IsIdentifier(TEXT("from")))
			{
				OutEmitter.FromLocation = Lexer.Peek().Location;
				Lexer.Next();
				const FToken& PathToken = Lexer.Peek();
				if (PathToken.Kind != ETokenKind::String)
				{
					Diagnostics.Error(TEXT("DFX2015"), PathToken.Location,
						TEXT("'from' must be followed by a quoted path to a .dfe source file."));
					return false;
				}
				OutEmitter.FromPath = Lexer.Next().Text;
			}

			return ParseEmitterBody(OutEmitter, /*bAllowRenderers=*/true);
		}

		bool FParserImpl::ParseSystemBody(FDocument& OutDocument)
		{
			if (!Expect(TEXT("{")))
			{
				return false;
			}

			while (!Lexer.Peek().IsEnd() && !Lexer.Peek().IsSymbol(TEXT("}")))
			{
				const FToken& Token = Lexer.Peek();
				const int32 BeforeOffset = Token.Offset;

				if (Token.Kind != ETokenKind::Identifier)
				{
					ErrorAtCurrent(TEXT("DFX2011"),
						FString::Printf(TEXT("Unexpected '%s' inside a system body."), *Token.Text));
					SkipToStatementEnd();
					continue;
				}

				const FString Keyword = Token.Text;

				if (Keyword == TEXT("Settings"))
				{
					Lexer.Next();
					ParseSettingsBlock(OutDocument.Settings);
				}
				else if (Keyword == TEXT("Properties"))
				{
					Lexer.Next();
					ParseParameterBlock(OutDocument.Parameters);
				}
				else if (Keyword == TEXT("Emitter"))
				{
					FEmitter Emitter;
					if (ParseEmitterDeclaration(Emitter))
					{
						OutDocument.Emitters.Add(MoveTemp(Emitter));
					}
				}
				else
				{
					EStackKind StackKind;
					if (ParseStackKind(Keyword, StackKind) && IsSystemScopeStack(StackKind))
					{
						FStack Stack;
						Stack.Kind = StackKind;
						Stack.Location = Token.Location;
						Lexer.Next();
						if (ParseStackBlock(Stack))
						{
							OutDocument.Stacks.Add(MoveTemp(Stack));
						}
					}
					else
					{
						ErrorAtCurrent(TEXT("DFX2016"),
							FString::Printf(TEXT("Unknown system section '%s'. Expected Settings, Properties, SystemSpawn, SystemUpdate or Emitter."),
								*Keyword));
						Lexer.Next();
						if (Lexer.Peek().IsSymbol(TEXT("=")))
						{
							Lexer.Next();
						}
						if (Lexer.Peek().IsSymbol(TEXT("{")))
						{
							SkipBalancedBlock();
						}
						else
						{
							SkipToStatementEnd();
						}
					}
				}

				if (Lexer.Peek().Offset == BeforeOffset && !Lexer.Peek().IsEnd())
				{
					Lexer.Next();
				}
			}

			return Expect(TEXT("}"));
		}

		bool FParserImpl::ParseModuleBody(FDocument& OutDocument)
		{
			if (!Expect(TEXT("{")))
			{
				return false;
			}

			bool bSeenBody = false;

			while (!Lexer.Peek().IsEnd() && !Lexer.Peek().IsSymbol(TEXT("}")))
			{
				const FToken& Token = Lexer.Peek();
				const int32 BeforeOffset = Token.Offset;

				if (Token.Kind != ETokenKind::Identifier)
				{
					ErrorAtCurrent(TEXT("DFX2011"),
						FString::Printf(TEXT("Unexpected '%s' inside a module body."), *Token.Text));
					SkipToStatementEnd();
					continue;
				}

				const FString Keyword = Token.Text;

				if (Keyword == TEXT("Settings"))
				{
					Lexer.Next();
					ParseSettingsBlock(OutDocument.Settings);
				}
				else if (Keyword == TEXT("Inputs"))
				{
					Lexer.Next();
					ParseParameterBlock(OutDocument.Parameters);
				}
				else if (Keyword == TEXT("Body"))
				{
					Lexer.Next();
					if (Expect(TEXT("=")))
					{
						// Raw text: a module body is HLSL, and tokenising it would mangle it.
						if (Lexer.ReadRawBlock(OutDocument.Body, OutDocument.BodyLocation))
						{
							bSeenBody = true;
						}
					}
				}
				else
				{
					ErrorAtCurrent(TEXT("DFX2017"),
						FString::Printf(TEXT("Unknown module section '%s'. Expected Settings, Inputs or Body."), *Keyword));
					Lexer.Next();
					if (Lexer.Peek().IsSymbol(TEXT("=")))
					{
						Lexer.Next();
					}
					if (Lexer.Peek().IsSymbol(TEXT("{")))
					{
						SkipBalancedBlock();
					}
					else
					{
						SkipToStatementEnd();
					}
				}

				if (Lexer.Peek().Offset == BeforeOffset && !Lexer.Peek().IsEnd())
				{
					Lexer.Next();
				}
			}

			if (!bSeenBody)
			{
				Diagnostics.Error(TEXT("DFX2018"), OutDocument.HeaderLocation,
					TEXT("A Module or DynamicInput must declare a 'Body = { }' block."));
			}

			return Expect(TEXT("}"));
		}

		bool FParserImpl::ParseHeaderArguments(FDocument& OutDocument)
		{
			if (!Expect(TEXT("(")))
			{
				return false;
			}

			bool bSeenName = false;
			do
			{
				FString Key;
				const FSourceLocation KeyLocation = Lexer.Peek().Location;
				if (!ExpectIdentifier(Key) || !Expect(TEXT("=")))
				{
					return false;
				}

				const FToken& ValueToken = Lexer.Peek();
				if (ValueToken.Kind != ETokenKind::String)
				{
					Diagnostics.Error(TEXT("DFX2019"), ValueToken.Location,
						FString::Printf(TEXT("Header argument '%s' must be a quoted string."), *Key));
					return false;
				}
				const FString Value = Lexer.Next().Text;

				if (Key == TEXT("Name"))
				{
					OutDocument.Name = Value;
					bSeenName = true;
				}
				else if (Key == TEXT("Root"))
				{
					OutDocument.Root = Value;
				}
				else
				{
					Diagnostics.Error(TEXT("DFX2019"), KeyLocation,
						FString::Printf(TEXT("Unknown header argument '%s'. Expected Name or Root."), *Key));
				}
			}
			while (Lexer.TryConsumeSymbol(TEXT(",")));

			if (!bSeenName)
			{
				Diagnostics.Error(TEXT("DFX2019"), OutDocument.HeaderLocation,
					TEXT("Missing required header argument 'Name'."));
			}

			return Expect(TEXT(")"));
		}

		bool FParserImpl::ParseDocument(FDocument& OutDocument)
		{
			const FToken& Token = Lexer.Peek();
			OutDocument.HeaderLocation = Token.Location;

			if (Token.Kind != ETokenKind::Identifier)
			{
				ErrorAtCurrent(TEXT("DFX2000"),
					TEXT("A DreamFX source file must start with System, Emitter, Module or DynamicInput."));
				return false;
			}

			const FString Keyword = Lexer.Next().Text;
			if (Keyword == TEXT("System"))
			{
				DocumentKind = EDocumentKind::System;
			}
			else if (Keyword == TEXT("Emitter"))
			{
				DocumentKind = EDocumentKind::Emitter;
			}
			else if (Keyword == TEXT("Module"))
			{
				DocumentKind = EDocumentKind::Module;
			}
			else if (Keyword == TEXT("DynamicInput"))
			{
				DocumentKind = EDocumentKind::DynamicInput;
			}
			else
			{
				Diagnostics.Error(TEXT("DFX2000"), Token.Location,
					FString::Printf(TEXT("Unknown document type '%s'. Expected System, Emitter, Module or DynamicInput."),
						*Keyword));
				return false;
			}

			// The document kind is fixed by the first keyword, but a declared kind that contradicts the
			// file extension is a mistake worth catching -- the build tool routes by extension.
			// Compared per-extension rather than per-kind because .dfm legitimately carries either
			// Module or DynamicInput, and only the declaration keyword tells them apart.
			const FString ExpectedExtension = FParser::ExtensionForDocumentKind(OutDocument.Kind);
			const FString DeclaredExtension = FParser::ExtensionForDocumentKind(DocumentKind);
			if (ExpectedExtension != DeclaredExtension)
			{
				Diagnostics.Error(TEXT("DFX2021"), Token.Location,
					FString::Printf(TEXT("File declares '%s' but its extension is '%s'. Rename the file to '%s' or change the declaration."),
						*Keyword, *ExpectedExtension, *DeclaredExtension));
			}
			OutDocument.Kind = DocumentKind;

			if (!ParseHeaderArguments(OutDocument))
			{
				return false;
			}

			switch (DocumentKind)
			{
			case EDocumentKind::System:
				ParseSystemBody(OutDocument);
				break;
			case EDocumentKind::Emitter:
				OutDocument.EmitterDefinition.Name = FPaths::GetCleanFilename(OutDocument.Name);
				OutDocument.EmitterDefinition.Location = OutDocument.HeaderLocation;
				ParseEmitterBody(OutDocument.EmitterDefinition, /*bAllowRenderers=*/true);
				break;
			case EDocumentKind::Module:
			case EDocumentKind::DynamicInput:
				ParseModuleBody(OutDocument);
				break;
			}

			if (!Lexer.Peek().IsEnd())
			{
				Diagnostics.Error(TEXT("DFX2022"), Lexer.Peek().Location,
					FString::Printf(TEXT("Unexpected '%s' after the end of the document. A DreamFX file declares exactly one top-level object."),
						*Lexer.Peek().Text));
			}

			return !Diagnostics.HasErrors();
		}
	}

	// -----------------------------------------------------------------------------------------

	bool FParser::ParseText(const FString& SourceText, const FString& SourceFilePath,
		FDocument& OutDocument, FDiagnosticSink& Diagnostics)
	{
		Diagnostics.SetFile(SourceFilePath);
		OutDocument.SourceFilePath = SourceFilePath;
		OutDocument.SourceHash = HashSourceText(SourceText);

		EDocumentKind Kind;
		if (DocumentKindFromExtension(FPaths::GetExtension(SourceFilePath), Kind))
		{
			OutDocument.Kind = Kind;
		}

		FParserImpl Impl(SourceText, Diagnostics);
		const bool bParsed = Impl.ParseDocument(OutDocument);

		// Stamped here rather than threaded through every ParseStackBlock call: the parser has no
		// business knowing about paths, and a merged emitter needs this to report the right file.
		auto StampStacks = [&OutDocument](TArray<FStack>& Stacks)
		{
			for (FStack& Stack : Stacks)
			{
				Stack.SourceFile = OutDocument.SourceFilePath;
			}
		};

		StampStacks(OutDocument.Stacks);
		StampStacks(OutDocument.EmitterDefinition.Stacks);
		for (FEmitter& Emitter : OutDocument.Emitters)
		{
			StampStacks(Emitter.Stacks);
		}

		return bParsed && !Diagnostics.HasErrors();
	}

	bool FParser::ParseFile(const FString& FilePath, FDocument& OutDocument, FDiagnosticSink& Diagnostics)
	{
		Diagnostics.SetFile(FilePath);

		FString SourceText;
		if (!FFileHelper::LoadFileToString(SourceText, *FilePath))
		{
			Diagnostics.Error(TEXT("DFX1000"), FSourceLocation(),
				FString::Printf(TEXT("Could not read source file '%s'."), *FilePath));
			return false;
		}

		return ParseText(SourceText, FilePath, OutDocument, Diagnostics);
	}

	bool FParser::DocumentKindFromExtension(const FString& Extension, EDocumentKind& OutKind)
	{
		const FString Normalized = Extension.StartsWith(TEXT(".")) ? Extension.RightChop(1) : Extension;
		if (Normalized.Equals(TEXT("dfs"), ESearchCase::IgnoreCase))
		{
			OutKind = EDocumentKind::System;
			return true;
		}
		if (Normalized.Equals(TEXT("dfe"), ESearchCase::IgnoreCase))
		{
			OutKind = EDocumentKind::Emitter;
			return true;
		}
		if (Normalized.Equals(TEXT("dfm"), ESearchCase::IgnoreCase))
		{
			// A .dfm holds either a Module or a DynamicInput; the declaration keyword decides which,
			// so the extension only narrows it this far.
			OutKind = EDocumentKind::Module;
			return true;
		}
		return false;
	}

	const TCHAR* FParser::ExtensionForDocumentKind(EDocumentKind Kind)
	{
		switch (Kind)
		{
		case EDocumentKind::System:  return TEXT(".dfs");
		case EDocumentKind::Emitter: return TEXT(".dfe");
		default:                     return TEXT(".dfm");
		}
	}
}
