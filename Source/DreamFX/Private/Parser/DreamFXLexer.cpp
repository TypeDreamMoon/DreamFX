#include "DreamFXLexer.h"

namespace UE::DreamFX
{
	namespace
	{
		bool IsIdentifierStart(TCHAR Character)
		{
			return FChar::IsAlpha(Character) || Character == TEXT('_');
		}

		bool IsIdentifierBody(TCHAR Character)
		{
			return FChar::IsAlnum(Character) || Character == TEXT('_');
		}

		/**
		 * Two-character operators must be matched before their single-character prefixes, otherwise
		 * `->` lexes as `-` followed by `>` and every curve key becomes a syntax error.
		 */
		const TCHAR* const TwoCharSymbols[] =
		{
			TEXT("->"), TEXT("+="), TEXT("-="), TEXT("*="), TEXT("/="),
			TEXT("=="), TEXT("!="), TEXT("<="), TEXT(">="), TEXT("&&"), TEXT("||"), TEXT("::"),
		};

		const TCHAR SingleCharSymbols[] =
		{
			TEXT('{'), TEXT('}'), TEXT('('), TEXT(')'), TEXT('['), TEXT(']'),
			TEXT('='), TEXT(';'), TEXT(','), TEXT('.'), TEXT('+'), TEXT('-'),
			TEXT('*'), TEXT('/'), TEXT('<'), TEXT('>'), TEXT(':'), TEXT('#'),
			TEXT('@'), TEXT('?'), TEXT('!'), TEXT('&'), TEXT('|'), TEXT('%'),
		};
	}

	FLexer::FLexer(const FString& InSource, FDiagnosticSink& InDiagnostics)
		: Source(InSource)
		, Diagnostics(InDiagnostics)
	{
	}

	TCHAR FLexer::Current() const
	{
		return Position < Source.Len() ? Source[Position] : TEXT('\0');
	}

	TCHAR FLexer::Lookahead(int32 Ahead) const
	{
		const int32 Index = Position + Ahead;
		return Index < Source.Len() ? Source[Index] : TEXT('\0');
	}

	void FLexer::Advance()
	{
		if (Position >= Source.Len())
		{
			return;
		}

		// Treat a bare \r, a bare \n and the \r\n pair as exactly one line break so column numbers
		// stay right on files with mixed line endings.
		const TCHAR Character = Source[Position];
		if (Character == TEXT('\r'))
		{
			if (Lookahead(1) == TEXT('\n'))
			{
				++Position;
			}
			++Position;
			++Line;
			Column = 1;
			return;
		}

		if (Character == TEXT('\n'))
		{
			++Position;
			++Line;
			Column = 1;
			return;
		}

		++Position;
		++Column;
	}

	void FLexer::SkipTriviaAndComments()
	{
		while (!AtEnd())
		{
			const TCHAR Character = Current();

			if (FChar::IsWhitespace(Character))
			{
				Advance();
				continue;
			}

			if (Character == TEXT('/') && Lookahead(1) == TEXT('/'))
			{
				while (!AtEnd() && Current() != TEXT('\n') && Current() != TEXT('\r'))
				{
					Advance();
				}
				continue;
			}

			if (Character == TEXT('/') && Lookahead(1) == TEXT('*'))
			{
				const FSourceLocation Start = CurrentLocation();
				Advance();
				Advance();
				bool bClosed = false;
				while (!AtEnd())
				{
					if (Current() == TEXT('*') && Lookahead(1) == TEXT('/'))
					{
						Advance();
						Advance();
						bClosed = true;
						break;
					}
					Advance();
				}
				if (!bClosed)
				{
					Diagnostics.Error(TEXT("DFX1002"), Start, TEXT("Unterminated block comment."));
				}
				continue;
			}

			break;
		}
	}

	FToken FLexer::LexToken()
	{
		SkipTriviaAndComments();

		FToken Token;
		Token.Location = CurrentLocation();
		Token.Offset = Position;

		if (AtEnd())
		{
			Token.Kind = ETokenKind::End;
			return Token;
		}

		const TCHAR Character = Current();

		if (IsIdentifierStart(Character))
		{
			const int32 Start = Position;
			while (!AtEnd() && IsIdentifierBody(Current()))
			{
				Advance();
			}
			Token.Kind = ETokenKind::Identifier;
			Token.Text = Source.Mid(Start, Position - Start);
			return Token;
		}

		// A number never starts with '-' here: unary minus is an operator, so that `A-1` does not
		// silently lex as `A` followed by the literal `-1` and lose the subtraction.
		if (FChar::IsDigit(Character) || (Character == TEXT('.') && FChar::IsDigit(Lookahead(1))))
		{
			const int32 Start = Position;
			bool bSeenDot = false;
			bool bSeenExponent = false;

			while (!AtEnd())
			{
				const TCHAR Digit = Current();
				if (FChar::IsDigit(Digit))
				{
					Advance();
				}
				else if (Digit == TEXT('.') && !bSeenDot && !bSeenExponent)
				{
					bSeenDot = true;
					Advance();
				}
				else if ((Digit == TEXT('e') || Digit == TEXT('E')) && !bSeenExponent
					&& (FChar::IsDigit(Lookahead(1))
						|| ((Lookahead(1) == TEXT('+') || Lookahead(1) == TEXT('-')) && FChar::IsDigit(Lookahead(2)))))
				{
					bSeenExponent = true;
					Advance();
					if (Current() == TEXT('+') || Current() == TEXT('-'))
					{
						Advance();
					}
				}
				else
				{
					break;
				}
			}

			// A trailing 'f' is tolerated so pasted HLSL constants do not need editing, but it makes
			// the literal a float regardless of whether a decimal point was written.
			bool bFloatSuffix = false;
			if (!AtEnd() && (Current() == TEXT('f') || Current() == TEXT('F')) && !IsIdentifierBody(Lookahead(1)))
			{
				bFloatSuffix = true;
				Advance();
			}

			const FString Digits = Source.Mid(Start, Position - Start - (bFloatSuffix ? 1 : 0));
			Token.Kind = ETokenKind::Number;
			Token.Text = Digits;
			Token.Number = FCString::Atod(*Digits);
			Token.bIsIntegerLiteral = !bSeenDot && !bSeenExponent && !bFloatSuffix;
			return Token;
		}

		if (Character == TEXT('"'))
		{
			const FSourceLocation Start = CurrentLocation();
			Advance();

			FString Text;
			bool bClosed = false;
			while (!AtEnd())
			{
				const TCHAR StringCharacter = Current();
				if (StringCharacter == TEXT('\\'))
				{
					Advance();
					if (AtEnd())
					{
						break;
					}
					const TCHAR Escaped = Current();
					switch (Escaped)
					{
					case TEXT('n'): Text.AppendChar(TEXT('\n')); break;
					case TEXT('r'): Text.AppendChar(TEXT('\r')); break;
					case TEXT('t'): Text.AppendChar(TEXT('\t')); break;
					default:        Text.AppendChar(Escaped);    break;
					}
					Advance();
					continue;
				}

				if (StringCharacter == TEXT('"'))
				{
					Advance();
					bClosed = true;
					break;
				}

				if (StringCharacter == TEXT('\n') || StringCharacter == TEXT('\r'))
				{
					break;
				}

				Text.AppendChar(StringCharacter);
				Advance();
			}

			if (!bClosed)
			{
				Diagnostics.Error(TEXT("DFX1001"), Start, TEXT("Unterminated string literal."));
			}

			Token.Kind = ETokenKind::String;
			Token.Text = Text;
			return Token;
		}

		for (const TCHAR* Symbol : TwoCharSymbols)
		{
			if (Character == Symbol[0] && Lookahead(1) == Symbol[1])
			{
				Advance();
				Advance();
				Token.Kind = ETokenKind::Symbol;
				Token.Text = Symbol;
				return Token;
			}
		}

		for (const TCHAR Symbol : SingleCharSymbols)
		{
			if (Character == Symbol)
			{
				Advance();
				Token.Kind = ETokenKind::Symbol;
				Token.Text = FString::Chr(Symbol);
				return Token;
			}
		}

		Diagnostics.Error(TEXT("DFX1003"), Token.Location,
			FString::Printf(TEXT("Unexpected character '%c' (U+%04X)."), Character, static_cast<int32>(Character)));
		Advance();
		return LexToken();
	}

	void FLexer::Fill(int32 Count)
	{
		while (Queue.Num() < Count)
		{
			FToken Token = LexToken();
			const bool bWasEnd = Token.Kind == ETokenKind::End;
			Queue.Add(MoveTemp(Token));
			if (bWasEnd)
			{
				// Never lex past the end marker; Peek beyond it keeps returning that same token.
				break;
			}
		}
	}

	const FToken& FLexer::Peek(int32 Ahead)
	{
		Fill(Ahead + 1);
		return Queue.IsValidIndex(Ahead) ? Queue[Ahead] : Queue.Last();
	}

	FToken FLexer::Next()
	{
		Fill(1);
		if (Queue[0].Kind == ETokenKind::End)
		{
			return Queue[0];
		}
		FToken Token = Queue[0];
		Queue.RemoveAt(0);
		return Token;
	}

	bool FLexer::TryConsumeSymbol(const TCHAR* Symbol)
	{
		if (Peek().IsSymbol(Symbol))
		{
			Next();
			return true;
		}
		return false;
	}

	bool FLexer::TryConsumeIdentifier(const TCHAR* Identifier)
	{
		if (Peek().IsIdentifier(Identifier))
		{
			Next();
			return true;
		}
		return false;
	}

	bool FLexer::ReadRawBlock(FString& OutText, FSourceLocation& OutLocation)
	{
		const FToken& Open = Peek();
		if (!Open.IsSymbol(TEXT("{")))
		{
			Diagnostics.Error(TEXT("DFX2001"), Open.Location,
				FString::Printf(TEXT("Expected '{' to open a raw block but found '%s'."), *Open.Text));
			return false;
		}

		// The queue holds tokens lexed past '{'. Rewinding the scanner to the '{' offset and dropping
		// the queue is what makes the raw scan authoritative -- everything after this point is read as
		// characters, not tokens.
		OutLocation = Open.Location;
		Position = Open.Offset;
		Line = Open.Location.Line;
		Column = Open.Location.Column;
		Queue.Reset();

		Advance(); // past '{'

		const int32 BodyStart = Position;
		int32 Depth = 1;

		while (!AtEnd())
		{
			const TCHAR Character = Current();

			if (Character == TEXT('/') && Lookahead(1) == TEXT('/'))
			{
				while (!AtEnd() && Current() != TEXT('\n') && Current() != TEXT('\r'))
				{
					Advance();
				}
				continue;
			}

			if (Character == TEXT('/') && Lookahead(1) == TEXT('*'))
			{
				Advance();
				Advance();
				while (!AtEnd() && !(Current() == TEXT('*') && Lookahead(1) == TEXT('/')))
				{
					Advance();
				}
				if (!AtEnd())
				{
					Advance();
					Advance();
				}
				continue;
			}

			if (Character == TEXT('"'))
			{
				Advance();
				while (!AtEnd() && Current() != TEXT('"'))
				{
					if (Current() == TEXT('\\'))
					{
						Advance();
					}
					Advance();
				}
				if (!AtEnd())
				{
					Advance();
				}
				continue;
			}

			if (Character == TEXT('{'))
			{
				++Depth;
				Advance();
				continue;
			}

			if (Character == TEXT('}'))
			{
				--Depth;
				if (Depth == 0)
				{
					OutText = Source.Mid(BodyStart, Position - BodyStart);
					Advance(); // past the closing '}'
					return true;
				}
				Advance();
				continue;
			}

			Advance();
		}

		Diagnostics.Error(TEXT("DFX1004"), OutLocation, TEXT("Unterminated raw block: missing '}'."));
		return false;
	}
}
