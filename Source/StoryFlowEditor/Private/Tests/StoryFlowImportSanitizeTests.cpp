// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Data/StoryFlowScriptAsset.h"
#include "Import/StoryFlowImporter.h"
#include "EditorAssetLibrary.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"

/**
 * Tests that script file and folder names containing characters invalid in
 * Unreal package names (spaces, apostrophes, dots, ...) are sanitized during
 * import.
 *
 * Unsanitized names produce packages the loader and cooker can never resolve
 * (FPackageName rejects them, e.g. PackageNameSpacesNotAllowed), so the assets
 * work in the editor and PIE but silently drop out of packaged builds. Script
 * asset names came straight from user-authored StoryFlow file names, unlike
 * character/media assets which already went through NormalizeAssetPath.
 *
 * Run via: Session Frontend > Automation > "StoryFlow.Import", or
 *   UnrealEditor-Cmd.exe StoryFlow.uproject -ExecCmds="Automation RunTests StoryFlow.Import" -TestExit="Automation Test Queue Empty" -unattended -nullrhi
 */

namespace StoryFlowImportSanitizeTestHelpers
{
	const TCHAR* TestRoot = TEXT("/Game/StoryFlowSanitizeTests");

	TSharedPtr<FJsonObject> MinimalScriptJson()
	{
		const FString Json = TEXT(R"JSON(
		{
			"startNode": "0",
			"nodes": {
				"0": { "type": "start", "id": "0" }
			}
		}
		)JSON");

		TSharedPtr<FJsonObject> JsonObject;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		FJsonSerializer::Deserialize(Reader, JsonObject);
		return JsonObject;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowImportPackageNameSanitizeTest,
	"StoryFlow.Import.PackageNameSanitize",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowImportPackageNameSanitizeTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowImportSanitizeTestHelpers;

	TSharedPtr<FJsonObject> JsonObject = MinimalScriptJson();
	if (!TestTrue(TEXT("Fixture JSON parses"), JsonObject.IsValid()))
	{
		return false;
	}

	// Spaces in folder and file name (the user-reported case)
	UStoryFlowScriptAsset* Spaced = UStoryFlowImporter::ImportScriptFromJson(JsonObject, TEXT("act 1/chapter 1"), TestRoot);
	if (TestNotNull(TEXT("spaces: script imported"), Spaced))
	{
		const FString PackageName = Spaced->GetOutermost()->GetName();
		TestTrue(FString::Printf(TEXT("spaces: '%s' is a valid long package name"), *PackageName),
			FPackageName::IsValidLongPackageName(PackageName));
		TestEqual(TEXT("spaces: raw ScriptPath preserved for runtime lookup"),
			Spaced->ScriptPath, FString(TEXT("act 1/chapter 1")));
	}

	// Punctuation invalid in package names (apostrophe, dot in a folder segment)
	UStoryFlowScriptAsset* Punctuated = UStoryFlowImporter::ImportScriptFromJson(JsonObject, TEXT("bob's route v1.2/finale"), TestRoot);
	if (TestNotNull(TEXT("punctuation: script imported"), Punctuated))
	{
		const FString PackageName = Punctuated->GetOutermost()->GetName();
		TestTrue(FString::Printf(TEXT("punctuation: '%s' is a valid long package name"), *PackageName),
			FPackageName::IsValidLongPackageName(PackageName));
	}

	// Already-valid names must keep their exact package path, so existing
	// clean projects do not get duplicate assets on re-import
	UStoryFlowScriptAsset* Clean = UStoryFlowImporter::ImportScriptFromJson(JsonObject, TEXT("chapters/intro"), TestRoot);
	if (TestNotNull(TEXT("clean: script imported"), Clean))
	{
		TestEqual(TEXT("clean: package path unchanged"),
			Clean->GetOutermost()->GetName(), FString(TestRoot) + TEXT("/chapters/intro"));
	}

	// Distinct source names must stay distinct after sanitization
	UStoryFlowScriptAsset* Underscored = UStoryFlowImporter::ImportScriptFromJson(JsonObject, TEXT("act_1/chapter_1"), TestRoot);
	if (Spaced && TestNotNull(TEXT("collision: underscored variant imported"), Underscored))
	{
		TestNotEqual(TEXT("collision: 'act 1/chapter 1' and 'act_1/chapter_1' import to different packages"),
			Spaced->GetOutermost()->GetName(), Underscored->GetOutermost()->GetName());
	}

	UEditorAssetLibrary::DeleteDirectory(TestRoot);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
