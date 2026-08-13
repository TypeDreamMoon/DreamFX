#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Workspace/DreamFXWorkspaceService.h"

/**
 * The generated `.code-workspace` is JSON nothing validates.
 *
 * It is written by a hand-rolled TJsonWriter sequence, opened by another program, and read by nobody
 * on this side -- so a mismatched WriteObjectEnd produces a file VSCode silently declines to treat as
 * a workspace, with no error anywhere in the engine. That is the failure this exists to catch: the
 * assertions below are deliberately about *shape*, not about which folders a given machine has.
 *
 * The extension id is asserted as a literal because it is one: it is what the marketplace resolves,
 * and a typo in it is a recommendation for an extension that does not exist.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamFXWorkspaceJsonTest,
	"DreamFX.Workspace.Json",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamFXWorkspaceJsonTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamFX::Editor;

	const FString WorkspaceDirectory = FPaths::GetPath(FDreamFXWorkspaceService::GetWorkspaceFilePath());
	const FString Json = FDreamFXWorkspaceService::BuildWorkspaceJson(WorkspaceDirectory);

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		AddError(FString::Printf(TEXT("The workspace file is not valid JSON:\n%s"), *Json));
		return false;
	}

	// -- folders ---------------------------------------------------------------------------------

	const TArray<TSharedPtr<FJsonValue>>* Folders = nullptr;
	if (!Root->TryGetArrayField(TEXT("folders"), Folders) || Folders->Num() == 0)
	{
		AddError(TEXT("The workspace has no folders, so VSCode would open an empty window."));
		return false;
	}

	// The project root is always first and always "." -- a stable folder identity is what keeps
	// VSCode's per-folder state attached across a rewrite.
	const TSharedPtr<FJsonObject>* First = nullptr;
	if (!(*Folders)[0]->TryGetObject(First))
	{
		AddError(TEXT("The first workspace folder is not an object."));
		return false;
	}
	TestEqual(TEXT("first folder path"), (*First)->GetStringField(TEXT("path")), FString(TEXT(".")));

	// -- file associations -----------------------------------------------------------------------

	const TSharedPtr<FJsonObject>* Settings = nullptr;
	const TSharedPtr<FJsonObject>* Associations = nullptr;
	if (!Root->TryGetObjectField(TEXT("settings"), Settings)
		|| !(*Settings)->TryGetObjectField(TEXT("files.associations"), Associations))
	{
		AddError(TEXT("The workspace has no files.associations, so the sources open as plain text."));
		return false;
	}

	for (const TCHAR* Pattern : { TEXT("*.dfs"), TEXT("*.dfe"), TEXT("*.dfm") })
	{
		FString LanguageId;
		if (!(*Associations)->TryGetStringField(Pattern, LanguageId))
		{
			AddError(FString::Printf(TEXT("No association for '%s'."), Pattern));
			continue;
		}
		// Not a free choice: this id is what the DreamFXLang extension registers.
		TestEqual(FString::Printf(TEXT("language id for %s"), Pattern), LanguageId, FString(TEXT("dreamfxlang")));
	}

	// -- extension recommendation ----------------------------------------------------------------

	const TSharedPtr<FJsonObject>* Extensions = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Recommendations = nullptr;
	if (!Root->TryGetObjectField(TEXT("extensions"), Extensions)
		|| !(*Extensions)->TryGetArrayField(TEXT("recommendations"), Recommendations))
	{
		AddError(TEXT("The workspace recommends no extensions."));
		return false;
	}

	TArray<FString> Recommended;
	for (const TSharedPtr<FJsonValue>& Value : *Recommendations)
	{
		Recommended.Add(Value->AsString());
	}
	TestTrue(TEXT("recommends the DreamFXLang extension"),
		Recommended.Contains(TEXT("typedreammoon.dreamfxlang-language-support")));

	return true;
}

#endif
