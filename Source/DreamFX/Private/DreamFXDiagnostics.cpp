#include "DreamFXDiagnostics.h"

namespace UE::DreamFX
{
	namespace
	{
		const TCHAR* LexSeverity(EDiagnosticSeverity Severity)
		{
			switch (Severity)
			{
			case EDiagnosticSeverity::Error:   return TEXT("error");
			case EDiagnosticSeverity::Warning: return TEXT("warning");
			default:                           return TEXT("info");
			}
		}
	}

	FString FDiagnostic::Format() const
	{
		// MSVC-style so terminals, Rider and the CI log parser all turn it into a clickable link.
		const FString Position = Location.IsValid()
			? FString::Printf(TEXT("(%d,%d)"), Location.Line, Location.Column)
			: FString();

		return FString::Printf(TEXT("%s%s: %s %s: %s"),
			File.IsEmpty() ? TEXT("<source>") : *File, *Position,
			LexSeverity(Severity), *Code, *Message);
	}

	void FDiagnosticSink::Add(EDiagnosticSeverity Severity, const FString& Code,
		const FSourceLocation& Location, const FString& Message)
	{
		FDiagnostic Diagnostic;
		Diagnostic.Severity = Severity;
		Diagnostic.Code = Code;
		Diagnostic.Message = Message;
		Diagnostic.File = CurrentFile;
		Diagnostic.Location = Location;

		if (Severity == EDiagnosticSeverity::Error)
		{
			++ErrorCount;
		}
		else if (Severity == EDiagnosticSeverity::Warning)
		{
			++WarningCount;
		}

		Diagnostics.Add(MoveTemp(Diagnostic));
	}

	void FDiagnosticSink::Append(const FDiagnosticSink& Other)
	{
		for (const FDiagnostic& Diagnostic : Other.Diagnostics)
		{
			// Keep each message's own File: a batch build merges many files into one sink and the
			// attribution is the whole point of the report.
			if (Diagnostic.Severity == EDiagnosticSeverity::Error)
			{
				++ErrorCount;
			}
			else if (Diagnostic.Severity == EDiagnosticSeverity::Warning)
			{
				++WarningCount;
			}
			Diagnostics.Add(Diagnostic);
		}
	}

	void FDiagnosticSink::Reset()
	{
		Diagnostics.Reset();
		ErrorCount = 0;
		WarningCount = 0;
	}

	FString FDiagnosticSink::FormatAll() const
	{
		TArray<FString> Lines;
		Lines.Reserve(Diagnostics.Num());
		for (const FDiagnostic& Diagnostic : Diagnostics)
		{
			Lines.Add(Diagnostic.Format());
		}
		return FString::Join(Lines, TEXT("\n"));
	}
}
