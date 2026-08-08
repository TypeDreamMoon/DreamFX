#pragma once

#include "CoreMinimal.h"

namespace UE::DreamFX
{
	/**
	 * A position in a DreamFXLang source file. 1-based, matching what editors and MSVC-style
	 * diagnostics expect; a zero Line means "no position known" and formats without the (l,c) suffix.
	 */
	struct FSourceLocation
	{
		int32 Line = 0;
		int32 Column = 0;

		FSourceLocation() = default;
		FSourceLocation(int32 InLine, int32 InColumn) : Line(InLine), Column(InColumn) {}

		bool IsValid() const { return Line > 0; }
	};

	enum class EDiagnosticSeverity : uint8
	{
		Info,
		Warning,
		Error,
	};

	/**
	 * One compiler message. Code is a stable DFXnnnn identifier so tests and the diagnose skill can
	 * assert on the failure mode rather than on the (translatable, tweakable) message text.
	 *
	 * Code ranges:
	 *   DFX1xxx  lexical
	 *   DFX2xxx  syntax
	 *   DFX3xxx  name / asset resolution
	 *   DFX4xxx  type checking
	 *   DFX5xxx  lowering and asset generation
	 *   DFX6xxx  Niagara compile events mapped back onto source
	 *   DFX7xxx  drift verification and lint
	 */
	struct FDiagnostic
	{
		EDiagnosticSeverity Severity = EDiagnosticSeverity::Error;
		FString Code;
		FString Message;
		FString File;
		FSourceLocation Location;

		/** "F:/path/file.dfs(12,5): error DFX2003: message" -- clickable in every editor we care about. */
		DREAMFX_API FString Format() const;
	};

	/**
	 * Accumulates diagnostics for one compilation. Deliberately not a singleton: the generator runs a
	 * fresh sink per source file so a batch build can report per-file results without cross-talk.
	 */
	class DREAMFX_API FDiagnosticSink
	{
	public:
		void SetFile(const FString& InFile) { CurrentFile = InFile; }
		const FString& GetFile() const { return CurrentFile; }

		void Add(EDiagnosticSeverity Severity, const FString& Code, const FSourceLocation& Location, const FString& Message);

		void Error(const FString& Code, const FSourceLocation& Location, const FString& Message)
		{
			Add(EDiagnosticSeverity::Error, Code, Location, Message);
		}

		void Warning(const FString& Code, const FSourceLocation& Location, const FString& Message)
		{
			Add(EDiagnosticSeverity::Warning, Code, Location, Message);
		}

		void Info(const FString& Code, const FSourceLocation& Location, const FString& Message)
		{
			Add(EDiagnosticSeverity::Info, Code, Location, Message);
		}

		bool HasErrors() const { return ErrorCount > 0; }
		int32 NumErrors() const { return ErrorCount; }
		int32 NumWarnings() const { return WarningCount; }

		const TArray<FDiagnostic>& GetDiagnostics() const { return Diagnostics; }

		/** Merges another sink's messages in, preserving each message's own file attribution. */
		void Append(const FDiagnosticSink& Other);

		void Reset();

		FString FormatAll() const;

	private:
		TArray<FDiagnostic> Diagnostics;
		FString CurrentFile;
		int32 ErrorCount = 0;
		int32 WarningCount = 0;
	};

	/**
	 * Echoes a sink to LogDreamFX at each message's own severity.
	 *
	 * Shared rather than copied per caller: every entry point -- commandlet, watcher, menu command --
	 * has to log the same way, or the same failure reads as an error headlessly and as a Display line
	 * in the editor, and the log stops being usable as evidence.
	 */
	DREAMFX_API void LogDiagnostics(const FDiagnosticSink& Diagnostics);
}
