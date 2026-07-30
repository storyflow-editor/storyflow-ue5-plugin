// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Data/StoryFlowScriptAsset.h"
#include "Import/StoryFlowImporter.h"
#include "EditorAssetLibrary.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"

/**
 * Tests that re-importing a script whose source JSON is unchanged does not
 * rewrite the .uasset (and leaves the package clean), while changed source
 * still saves, and a failed save retries on the next import.
 *
 * The read-only trick makes save attempts observable without timestamps:
 * with the target file read-only, any save attempt logs the plugin's
 * "Clear the read-only flag" error exactly once. So over the whole test the
 * count of that error equals the number of save attempts.
 */

namespace StoryFlowSkipUnchangedTestHelpers
{
	const TCHAR* TestRoot = TEXT("/Game/StoryFlowSkipUnchangedTests");

	TSharedPtr<FJsonObject> ParseJson(const FString& Json)
	{
		TSharedPtr<FJsonObject> JsonObject;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		FJsonSerializer::Deserialize(Reader, JsonObject);
		return JsonObject;
	}

	TSharedPtr<FJsonObject> ScriptV1()
	{
		return ParseJson(TEXT(R"JSON(
		{
			"startNode": "0",
			"nodes": {
				"0": { "type": "start", "id": "0" }
			}
		}
		)JSON"));
	}

	TSharedPtr<FJsonObject> ScriptV2()
	{
		return ParseJson(TEXT(R"JSON(
		{
			"startNode": "0",
			"nodes": {
				"0": { "type": "start", "id": "0" },
				"1": { "type": "end", "id": "1" }
			}
		}
		)JSON"));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowImportSkipUnchangedScriptTest,
	"StoryFlow.Import.SkipUnchanged.Script",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowImportSkipUnchangedScriptTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowSkipUnchangedTestHelpers;

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	// Clear read-only leftovers from a previous run that died mid-test
	{
		FString LeftoverFileName;
		if (FPackageName::TryConvertLongPackageNameToFilename(FString(TestRoot) + TEXT("/skip/subject"), LeftoverFileName, FPackageName::GetAssetPackageExtension())
			&& FPaths::FileExists(LeftoverFileName))
		{
			PlatformFile.SetReadOnly(*LeftoverFileName, false);
		}
	}

	TSharedPtr<FJsonObject> V1 = ScriptV1();
	TSharedPtr<FJsonObject> V2 = ScriptV2();
	if (!TestTrue(TEXT("Fixture JSON parses"), V1.IsValid() && V2.IsValid()))
	{
		return false;
	}

	// Save attempts over the read-only file must total exactly 2:
	// one for the first V2 import, one for the V2 retry after its failed save.
	// The unchanged V1 re-import must contribute zero.
	AddExpectedError(TEXT("it is read only"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("Error saving"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("Clear the read-only flag"), EAutomationExpectedErrorFlags::Contains, 2);

	// Import V1 normally
	UStoryFlowScriptAsset* First = UStoryFlowImporter::ImportScriptFromJson(V1, TEXT("skip/subject"), TestRoot);
	if (!TestNotNull(TEXT("initial import succeeds"), First))
	{
		return false;
	}

	FString PackageFileName;
	if (!TestTrue(TEXT("package maps to a filename"),
		FPackageName::TryConvertLongPackageNameToFilename(First->GetOutermost()->GetName(), PackageFileName, FPackageName::GetAssetPackageExtension())))
	{
		return false;
	}
	if (!TestTrue(TEXT("initial import wrote the .uasset"), FPaths::FileExists(PackageFileName)))
	{
		return false;
	}

	PlatformFile.SetReadOnly(*PackageFileName, true);

	// Unchanged re-import: no save attempt, package left clean
	UStoryFlowScriptAsset* Unchanged = UStoryFlowImporter::ImportScriptFromJson(V1, TEXT("skip/subject"), TestRoot);
	if (TestNotNull(TEXT("unchanged re-import returns the asset"), Unchanged))
	{
		TestFalse(TEXT("unchanged re-import leaves the package clean"), Unchanged->GetOutermost()->IsDirty());
	}

	// Changed source must attempt the save (fails: read-only) ...
	UStoryFlowScriptAsset* Changed = UStoryFlowImporter::ImportScriptFromJson(V2, TEXT("skip/subject"), TestRoot);
	TestNotNull(TEXT("changed import survives the failed save"), Changed);

	// ... and a failed save must not poison the hash: importing V2 again
	// must retry the save, not skip it as "unchanged".
	UStoryFlowScriptAsset* Retry = UStoryFlowImporter::ImportScriptFromJson(V2, TEXT("skip/subject"), TestRoot);
	TestNotNull(TEXT("retry import survives the failed save"), Retry);

	// Cleanup
	PlatformFile.SetReadOnly(*PackageFileName, false);
	UEditorAssetLibrary::DeleteDirectory(TestRoot);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
