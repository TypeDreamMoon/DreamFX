#include "Bridge/DreamFXBridgeService.h"

#include "DreamFXDiagnostics.h"
#include "DreamFXModule.h"
#include "DreamFXParser.h"
#include "Generation/DreamFXGenerator.h"
#include "Schema/DreamFXIndexExport.h"
#include "SourceFiles/DreamFXPaths.h"
#include "UI/DreamFXAssetCommands.h"
#include "Workspace/DreamFXSourceWatcher.h"

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NiagaraEmitter.h"
#include "NiagaraSystem.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Editor.h"

namespace UE::DreamFX::Editor
{
	namespace
	{
		/** Bumped only when a change would make an older client misread a newer editor, or vice versa. */
		constexpr int32 BridgeProtocolVersion = 1;

		/**
		 * Poll interval. Also the worst-case latency a client sees on an idle editor.
		 *
		 * A quarter second is imperceptible next to the work every request triggers, and the poll
		 * itself is a directory listing of a folder that is empty almost always.
		 */
		constexpr float PollSeconds = 0.25f;

		/** How often the heartbeat is rewritten while idle. */
		constexpr double HeartbeatSeconds = 2.0;

		FTSTicker::FDelegateHandle GTickerHandle;
		double GLastHeartbeat = 0.0;
		bool GBusy = false;
		FString GBusyAction;
		FString GLastResult;

		FString BridgeDir()      { return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("DreamFX/Bridge"))); }
		FString RequestsDir()    { return FPaths::Combine(BridgeDir(), TEXT("Requests")); }
		FString ResponsesDir()   { return FPaths::Combine(BridgeDir(), TEXT("Responses")); }
		FString StatusPath()     { return FPaths::Combine(BridgeDir(), TEXT("status.json")); }
		FString DiagnosticsPath(){ return FPaths::Combine(BridgeDir(), TEXT("diagnostics.json")); }

		/**
		 * Writes a file the way a reader that is polling for it needs it written.
		 *
		 * The client watches for `Responses/<id>.json` to exist and then reads it. A plain write makes
		 * the file exist while it is still half a file, so the client sees truncated JSON -- rarely,
		 * and only under load, which is the worst way for a bug to behave. Writing beside the target
		 * and renaming makes appearing and being complete the same event.
		 */
		bool WriteFileAtomically(const FString& Path, const FString& Text)
		{
			const FString Temporary = Path + TEXT(".tmp");
			if (!FFileHelper::SaveStringToFile(Text, *Temporary, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				return false;
			}
			// Move rather than Copy: a rename within one volume is atomic, a copy is not.
			if (!IFileManager::Get().Move(*Path, *Temporary, /*bReplace=*/true))
			{
				IFileManager::Get().Delete(*Temporary);
				return false;
			}
			return true;
		}

		void WriteDiagnosticsArray(const TSharedRef<TJsonWriter<>>& Writer, const FDiagnosticSink& Diagnostics)
		{
			Writer->WriteArrayStart(TEXT("diagnostics"));
			for (const FDiagnostic& Diagnostic : Diagnostics.GetDiagnostics())
			{
				Writer->WriteObjectStart();
				// Each diagnostic keeps its own file: a .dfs that pulls in a broken .dfe fails at the
				// .dfe's position, and attributing it to the .dfs would send the author to the wrong
				// file. The CLI has always got this right; so must the bridge.
				Writer->WriteValue(TEXT("file"), Diagnostic.File);
				Writer->WriteValue(TEXT("line"), FMath::Max(1, Diagnostic.Location.Line));
				Writer->WriteValue(TEXT("column"), FMath::Max(1, Diagnostic.Location.Column));
				Writer->WriteValue(TEXT("severity"),
					Diagnostic.Severity == EDiagnosticSeverity::Error   ? TEXT("error")
					: Diagnostic.Severity == EDiagnosticSeverity::Warning ? TEXT("warning")
					: TEXT("info"));
				Writer->WriteValue(TEXT("code"), Diagnostic.Code);
				Writer->WriteValue(TEXT("message"), Diagnostic.Message);
				Writer->WriteObjectEnd();
			}
			Writer->WriteArrayEnd();
		}

		void PublishStatus()
		{
			FString Text;
			const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
			Writer->WriteObjectStart();
			Writer->WriteValue(TEXT("protocol"), BridgeProtocolVersion);
			Writer->WriteValue(TEXT("pid"), static_cast<int32>(FPlatformProcess::GetCurrentProcessId()));
			Writer->WriteValue(TEXT("project"), FApp::GetProjectName());
			Writer->WriteValue(TEXT("projectDir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
			Writer->WriteValue(TEXT("engineDir"), FPaths::ConvertRelativePathToFull(FPaths::EngineDir()));

			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("DreamFX"));
			Writer->WriteValue(TEXT("pluginVersion"),
				Plugin.IsValid() ? Plugin->GetDescriptor().VersionName : TEXT("unknown"));

			// A long build blocks the game thread, so the heartbeat stops during it. Saying *what* is
			// running lets a client tell "busy for 40 seconds" from "died 40 seconds ago" -- without
			// this the only safe read of a stale heartbeat is "dead", and every real build would look
			// like a crash.
			Writer->WriteValue(TEXT("busy"), GBusy);
			if (GBusy)
			{
				Writer->WriteValue(TEXT("busyAction"), GBusyAction);
			}
			if (!GLastResult.IsEmpty())
			{
				Writer->WriteValue(TEXT("lastResult"), GLastResult);
			}
			Writer->WriteValue(TEXT("heartbeatUtc"), FDateTime::UtcNow().ToIso8601());
			Writer->WriteObjectEnd();
			Writer->Close();

			WriteFileAtomically(StatusPath(), Text);
			GLastHeartbeat = FPlatformTime::Seconds();
		}

		UObject* LoadAsset(const FString& AssetPath)
		{
			if (AssetPath.IsEmpty())
			{
				return nullptr;
			}
			return FSoftObjectPath(AssetPath).TryLoad();
		}

		/** One request's outcome, before it is serialised. */
		struct FActionResult
		{
			bool bOk = false;
			FString Message;
			FDiagnosticSink Diagnostics;
		};

		FActionResult Fail(const FString& Message)
		{
			FActionResult Result;
			Result.bOk = false;
			Result.Message = Message;
			return Result;
		}

		FActionResult Succeed(const FString& Message)
		{
			FActionResult Result;
			Result.bOk = true;
			Result.Message = Message;
			return Result;
		}

		FActionResult RunGenerate(const FString& SourceFile, bool bVerifyOnly)
		{
			if (SourceFile.IsEmpty() || !FPaths::FileExists(SourceFile))
			{
				return Fail(FString::Printf(TEXT("No such source file: '%s'."), *SourceFile));
			}

			FGenerateOptions Options;
			Options.bSave = !bVerifyOnly;
			Options.bVerifyOnly = bVerifyOnly;
			// An explicit request, unlike a save: the caller asked for this specific file to be built
			// now, and "nothing happened, it was already current" is not a useful answer to that.
			Options.bForce = !bVerifyOnly;

			FActionResult Result;

			// A .dfe generates nothing of its own -- it is merged into its host by copy (R3) -- so
			// building one would report a failure for a file that is not broken. Parsing it still
			// catches a syntax error here rather than in whatever .dfs pulls it in.
			EDocumentKind Kind = EDocumentKind::System;
			FParser::DocumentKindFromExtension(FPaths::GetExtension(SourceFile), Kind);

			if (Kind == EDocumentKind::Emitter)
			{
				FDocument Document;
				Result.bOk = FParser::ParseFile(SourceFile, Document, Result.Diagnostics);
				Result.Message = Result.bOk
					? TEXT("Parsed. A .dfe generates no asset of its own; build the system that references it.")
					: TEXT("Parse failed.");
				return Result;
			}

			const FGenerateResult Generated = FGenerator::GenerateFromFile(SourceFile, Options, Result.Diagnostics);
			Result.bOk = Generated.bSucceeded;
			Result.Message = Generated.bSkipped ? TEXT("Already up to date.")
				: Generated.bDrifted ? TEXT("The asset has drifted from its source.")
				: Generated.bSucceeded ? (bVerifyOnly ? TEXT("Verified.") : TEXT("Built."))
				: TEXT("Failed.");
			return Result;
		}

		FActionResult Dispatch(const TSharedPtr<FJsonObject>& Request)
		{
			FString Action;
			if (!Request->TryGetStringField(TEXT("action"), Action) || Action.IsEmpty())
			{
				return Fail(TEXT("The request has no 'action'."));
			}

			FString SourceFile, AssetPath, Scope;
			Request->TryGetStringField(TEXT("sourceFile"), SourceFile);
			Request->TryGetStringField(TEXT("assetPath"), AssetPath);
			Request->TryGetStringField(TEXT("scope"), Scope);

			if (Action == TEXT("ping"))
			{
				return Succeed(TEXT("alive"));
			}

			if (Action == TEXT("build") || Action == TEXT("verify"))
			{
				const bool bVerifyOnly = Action == TEXT("verify");
				if (Scope == TEXT("all"))
				{
					// Through the watcher's queue rather than a loop here. The queue is what carries
					// the module-before-emitter-before-system ordering and the bulk-batch gate, and it
					// runs across ticks so the editor stays responsive -- which also means the result
					// is not knowable yet, and saying so is better than blocking the caller for
					// minutes or inventing an answer.
					const int32 Queued = FSourceWatcher::QueueAllSources(/*bAnnounceSuccess=*/true);
					return Succeed(FString::Printf(
						TEXT("Queued %d source file(s); the editor reports the result when the batch drains."), Queued));
				}
				return RunGenerate(SourceFile, bVerifyOnly);
			}

			if (Action == TEXT("decompile"))
			{
				UObject* Asset = LoadAsset(AssetPath);
				if (Asset == nullptr)
				{
					return Fail(FString::Printf(TEXT("Could not load '%s'."), *AssetPath));
				}
				if (UNiagaraSystem* System = Cast<UNiagaraSystem>(Asset))
				{
					FDreamFXCommands::ExportSystem(System);
					return Succeed(TEXT("Exported."));
				}
				if (UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Asset))
				{
					FDreamFXCommands::ExportEmitter(Emitter);
					return Succeed(TEXT("Exported."));
				}
				return Fail(TEXT("Only a Niagara system or emitter can be exported."));
			}

			if (Action == TEXT("adopt"))
			{
				UNiagaraSystem* System = Cast<UNiagaraSystem>(LoadAsset(AssetPath));
				if (System == nullptr)
				{
					return Fail(FString::Printf(TEXT("Could not load a Niagara system at '%s'."), *AssetPath));
				}
				// The confirmation dialog is skipped -- a request from another editor already is the
				// confirmation, and a modal nobody is looking at would hang the bridge. The refusals
				// behind it are not skipped: those are correctness gates, not prompts.
				FDreamFXCommands::AdoptSystem(System, /*bSkipConfirmation=*/true);
				return Succeed(TEXT("Adopt requested; the editor reports the outcome."));
			}

			if (Action == TEXT("openAsset"))
			{
				UObject* Asset = LoadAsset(AssetPath);
				if (Asset == nullptr)
				{
					return Fail(FString::Printf(TEXT("Could not load '%s'."), *AssetPath));
				}
				if (GEditor == nullptr)
				{
					return Fail(TEXT("No editor to open it in."));
				}
				GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Asset);
				return Succeed(FString::Printf(TEXT("Opened '%s'."), *AssetPath));
			}

			if (Action == TEXT("revealSource"))
			{
				UObject* Asset = LoadAsset(AssetPath);
				if (Asset == nullptr)
				{
					return Fail(FString::Printf(TEXT("Could not load '%s'."), *AssetPath));
				}
				FDreamFXCommands::OpenSource(Asset);
				return Succeed(TEXT("Opened the source recorded in the asset's provenance stamp."));
			}

			if (Action == TEXT("refreshIndex"))
			{
				FString Destination;
				FString Out;
				Request->TryGetStringField(TEXT("outputPath"), Out);

				// Never probes. Measured the hard way: probing walks the module graphs, and
				// `/Niagara/Modules/Masks/ConeMask` sends UNiagaraGraph::ReferencesStaticVariable
				// into unbounded recursion -- 81 frames deep and then EXCEPTION_STACK_OVERFLOW,
				// which is not catchable and takes the whole process with it. Serving that request
				// over the bridge killed this editor outright, along with whatever was unsaved in it.
				//
				// The resume machinery that makes `dfx index` survive this is not applicable here.
				// It works by recording the module about to be probed, dying, and being *restarted*
				// by dfx.ps1, which then quarantines it. There is no supervisor to restart an editor,
				// and a quarantine list from an earlier run is no guarantee either: a new engine or a
				// new content pack can add a landmine that no list knows about yet.
				//
				// So the split is by which process is allowed to die. The CLI's is expendable and
				// supervised; the author's editor is neither. The cheap half is served here and the
				// caller is told plainly where the other half lives.
				const int32 Code = FIndexExport::Run(Out, /*bSkipInputs=*/true,
					/*bRetryQuarantined=*/false, Destination);
				return Code == 0
					? Succeed(FString::Printf(
						TEXT("Module list written to '%s'. Input signatures were not probed: probing can crash this editor, so it is a `dfx index` job."),
						*Destination))
					: Fail(FString::Printf(TEXT("Index export failed (%d)."), Code));
			}

			// Never silent. A client that asked for something this build does not have needs to be
			// told so, not left waiting for a response that is never coming.
			return Fail(FString::Printf(
				TEXT("Unknown action '%s'. This build understands: ping, build, verify, decompile, adopt, openAsset, revealSource, refreshIndex."),
				*Action));
		}

		void RespondTo(const FString& RequestId, const FActionResult& Result, double DurationMs)
		{
			FString Text;
			const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
			Writer->WriteObjectStart();
			Writer->WriteValue(TEXT("protocol"), BridgeProtocolVersion);
			Writer->WriteValue(TEXT("requestId"), RequestId);
			Writer->WriteValue(TEXT("ok"), Result.bOk);
			Writer->WriteValue(TEXT("durationMs"), static_cast<int32>(DurationMs));
			Writer->WriteValue(TEXT("message"), Result.Message);
			WriteDiagnosticsArray(Writer, Result.Diagnostics);
			Writer->WriteObjectEnd();
			Writer->Close();

			WriteFileAtomically(FPaths::Combine(ResponsesDir(), RequestId + TEXT(".json")), Text);

			// Also published standalone, so a client that was not the one who asked -- or one that
			// reconnected after the response was consumed -- can still show what the last run found.
			FString DiagnosticsText;
			const TSharedRef<TJsonWriter<>> DiagnosticsWriter = TJsonWriterFactory<>::Create(&DiagnosticsText);
			DiagnosticsWriter->WriteObjectStart();
			DiagnosticsWriter->WriteValue(TEXT("protocol"), BridgeProtocolVersion);
			DiagnosticsWriter->WriteValue(TEXT("generatedUtc"), FDateTime::UtcNow().ToIso8601());
			WriteDiagnosticsArray(DiagnosticsWriter, Result.Diagnostics);
			DiagnosticsWriter->WriteObjectEnd();
			DiagnosticsWriter->Close();
			WriteFileAtomically(DiagnosticsPath(), DiagnosticsText);
		}

		void ServeOneRequest(const FString& RequestPath)
		{
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *RequestPath))
			{
				// Most likely still being written. Left alone; the next poll picks it up.
				return;
			}

			TSharedPtr<FJsonObject> Request;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
			const bool bParsed = FJsonSerializer::Deserialize(Reader, Request) && Request.IsValid();

			FString RequestId;
			if (bParsed)
			{
				Request->TryGetStringField(TEXT("requestId"), RequestId);
			}
			if (RequestId.IsEmpty())
			{
				// Fall back to the file name so even a malformed request gets an answer somewhere the
				// client can find it.
				RequestId = FPaths::GetBaseFilename(RequestPath);
			}

			// Deleted before it is served, never after. A request that crashes the editor would
			// otherwise be replayed on every start -- the same shape as the DirectoryWatcher rebuild
			// storm this project has already been bitten by once.
			IFileManager::Get().Delete(*RequestPath);

			const double StartedAt = FPlatformTime::Seconds();
			FActionResult Result;

			if (!bParsed)
			{
				Result = Fail(TEXT("The request is not valid JSON."));
			}
			else
			{
				int32 Protocol = 0;
				Request->TryGetNumberField(TEXT("protocol"), Protocol);
				if (Protocol != BridgeProtocolVersion)
				{
					Result = Fail(FString::Printf(
						TEXT("Protocol %d is not understood; this editor speaks %d. Update the DreamFXLang extension or the plugin so the two match."),
						Protocol, BridgeProtocolVersion));
				}
				else
				{
					FString Action;
					Request->TryGetStringField(TEXT("action"), Action);

					GBusy = true;
					GBusyAction = Action;
					PublishStatus();

					Result = Dispatch(Request);

					GBusy = false;
					GBusyAction.Reset();
				}
			}

			const double DurationMs = (FPlatformTime::Seconds() - StartedAt) * 1000.0;
			GLastResult = FString::Printf(TEXT("%s (%s)"),
				Result.bOk ? TEXT("ok") : TEXT("failed"), *Result.Message);

			RespondTo(RequestId, Result, DurationMs);
			PublishStatus();

			UE_LOG(LogDreamFX, Display, TEXT("Bridge: %s -> %s in %.0f ms."),
				*RequestId, Result.bOk ? TEXT("ok") : TEXT("failed"), DurationMs);
		}

		bool Tick(float /*DeltaTime*/)
		{
			TArray<FString> Requests;
			IFileManager::Get().FindFiles(Requests, *(RequestsDir() / TEXT("*.json")), /*Files=*/true, /*Directories=*/false);

			if (Requests.Num() > 0)
			{
				// Oldest first, so a client that fired two requests gets them in the order it sent
				// them. The names carry a timestamp precisely so this is a sort and not a guess.
				Requests.Sort();
				for (const FString& Name : Requests)
				{
					ServeOneRequest(FPaths::Combine(RequestsDir(), Name));
				}
			}
			else if (FPlatformTime::Seconds() - GLastHeartbeat > HeartbeatSeconds)
			{
				PublishStatus();
			}

			return true;
		}
	}

	FString FBridgeService::GetBridgeDirectory()
	{
		return BridgeDir();
	}

	void FBridgeService::Register()
	{
		IFileManager& Files = IFileManager::Get();
		Files.MakeDirectory(*RequestsDir(), /*Tree=*/true);
		Files.MakeDirectory(*ResponsesDir(), /*Tree=*/true);

		// Responses left by a previous session are answers nobody is waiting for any more, and a
		// client that reconnects and finds one would act on a stale result.
		TArray<FString> Stale;
		Files.FindFiles(Stale, *(ResponsesDir() / TEXT("*.json")), true, false);
		for (const FString& Name : Stale)
		{
			Files.Delete(*FPaths::Combine(ResponsesDir(), Name));
		}

		GBusy = false;
		GLastResult.Reset();
		PublishStatus();

		GTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateStatic(&Tick), PollSeconds);

		UE_LOG(LogDreamFX, Display, TEXT("Bridge listening in '%s'."), *BridgeDir());
	}

	void FBridgeService::Unregister()
	{
		if (GTickerHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(GTickerHandle);
			GTickerHandle.Reset();
		}

		// The heartbeat is how a client tells a running editor from a closed one, and a file that
		// simply stops being updated is indistinguishable from one whose editor hung. Deleting it says
		// "gone" in a way a timeout cannot: the client falls back to the CLI immediately instead of
		// waiting out its liveness window first.
		IFileManager::Get().Delete(*StatusPath());
	}
}
