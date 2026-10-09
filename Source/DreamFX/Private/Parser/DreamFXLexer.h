#pragma once

#include "CoreMinimal.h"
#include "DreamFXDiagnostics.h"

namespace UE::DreamFX
{
	enum class ETokenKind : uint8
	{
		End,
		Identifier,
		Number,
		String,
		/** Punctuation and operators. Text holds the exact spelling, including two-character forms. */
		Symbol,
	};

	struct FToken
	{
		ETokenKind Kind = ETokenKind::End;
		FString Text;
		FSourceLocation Location;

		/** Number payload; only meaningful when Kind == Number. */
		double Number = 0.0;
		bool bIsIntegerLiteral = false;

		/** Byte offset of the first character, so raw-block reads can rewind exactly. */
		int32 Offset = 0;

		/**
		 * Byte offset one past the token's last character -- so the token's own text is
		 * `Source.Mid(Offset, EndOffset - Offset)`.
		 *
		 * Not derivable from Text: a number's payload drops a trailing `f`, a string's is unescaped,
		 * and a back-quoted name's excludes its quotes.
		 */
		int32 EndOffset = 0;

		bool IsSymbol(const TCHAR* Expected) const { return Kind == ETokenKind::Symbol && Text == Expected; }
		bool IsIdentifier(const TCHAR* Expected) const { return Kind == ETokenKind::Identifier && Text == Expected; }
		bool IsEnd() const { return Kind == ETokenKind::End; }
	};

	/**
	 * Streaming tokenizer with arbitrary lookahead.
	 *
	 * Streaming rather than up-front tokenisation because two constructs -- `hlsl { }` and a module's
	 * `Body = { }` -- are raw text whose contents must never be tokenised. ReadRawBlock rewinds the
	 * scanner to the recorded offset of the pending `{` token and does a brace-balanced character
	 * scan from there, which is only possible when the scanner position is still authoritative.
	 */
	class FLexer
	{
	public:
		FLexer(const FString& InSource, FDiagnosticSink& InDiagnostics);

		const FToken& Peek(int32 Ahead = 0);
		FToken Next();

		/** Consumes the next token if it is the given symbol. */
		bool TryConsumeSymbol(const TCHAR* Symbol);
		/** Consumes the next token if it is the given identifier (case-sensitive). */
		bool TryConsumeIdentifier(const TCHAR* Identifier);

		/**
		 * Consumes a brace-balanced block starting at the pending `{` and returns its interior text
		 * verbatim. String literals and comments inside are skipped for the purpose of brace counting
		 * so an HLSL body containing `// }` or `"{"` still balances correctly.
		 */
		bool ReadRawBlock(FString& OutText, FSourceLocation& OutLocation);

		FSourceLocation CurrentLocation() const { return FSourceLocation(Line, Column); }

		/**
		 * Byte offset one past the last token `Next` handed out, or past the last raw block read.
		 *
		 * This is where a VALUE ends when the parser stops consuming it -- the lexer's next pending
		 * token starts later, past whatever trivia follows, and a caller rewriting only the value's own
		 * characters has to leave that trivia alone.
		 */
		int32 LastTokenEnd() const { return LastConsumedEnd; }

	private:
		void Fill(int32 Count);
		FToken LexToken();
		/** The scanner itself; LexToken wraps it to stamp every token's end offset. */
		FToken LexTokenImpl();
		void SkipTriviaAndComments();
		void Advance();
		TCHAR Current() const;
		TCHAR Lookahead(int32 Ahead) const;
		bool AtEnd() const { return Position >= Source.Len(); }

		const FString& Source;
		FDiagnosticSink& Diagnostics;

		int32 Position = 0;
		int32 Line = 1;
		int32 Column = 1;
		int32 LastConsumedEnd = 0;

		TArray<FToken> Queue;
	};
}
