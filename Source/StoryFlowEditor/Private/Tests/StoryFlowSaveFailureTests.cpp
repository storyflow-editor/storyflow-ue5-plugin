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
 * Tests that a failed package save during import degrades to a logged error
 * instead of crashing the editor.
 *
 * UPackage::SavePackage called with default FSavePackageArgs routes save
 * failures to GError, the fatal-error output device, whose Serialize ignores
 * verbosity and raises (crash reported from Linux as "Error saving
 * '...characters_leonardo.uasset'" during sync, but the mechanism is identical
 * on every platform). The most common trigger is a read-only target .uasset:
 * FinalizeTempOutputFiles fails the save before moving any files.
 *
 * Run via: Session Frontend > Automation > "StoryFlow.Import", or
 *   UnrealEditor-Cmd.exe StoryFlow.uproject -ExecCmds="Automation RunTests StoryFlow.Import.SaveFailure" -TestExit="Automation Test Queue Empty" -unattended -nullrhi
 */

namespace StoryFlowSaveFailureTestHelpers
{
	const TCHAR* TestRoot = TEXT("/Game/StoryFlowSaveFailureTests");

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowImportSaveFailureNonFatalTest,
	"StoryFlow.Import.SaveFailureNonFatal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowImportSaveFailureNonFatalTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowSaveFailureTestHelpers;

	TSharedPtr<FJsonObject> JsonObject = MinimalScriptJson();
	if (!TestTrue(TEXT("Fixture JSON parses"), JsonObject.IsValid()))
	{
		return false;
	}

	// A previous run that died mid-test leaves the victim file behind still
	// read-only, which would make the first import below fail. Clear it.
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	{
		FString LeftoverFileName;
		if (FPackageName::TryConvertLongPackageNameToFilename(FString(TestRoot) + TEXT("/readonly/victim"), LeftoverFileName, FPackageName::GetAssetPackageExtension())
			&& FPaths::FileExists(LeftoverFileName))
		{
			PlatformFile.SetReadOnly(*LeftoverFileName, false);
		}
	}

	// First import saves the .uasset to disk normally
	UStoryFlowScriptAsset* Imported = UStoryFlowImporter::ImportScriptFromJson(JsonObject, TEXT("readonly/victim"), TestRoot);
	if (!TestNotNull(TEXT("initial import succeeds"), Imported))
	{
		return false;
	}

	FString PackageFileName;
	if (!TestTrue(TEXT("package maps to a filename"),
		FPackageName::TryConvertLongPackageNameToFilename(Imported->GetOutermost()->GetName(), PackageFileName, FPackageName::GetAssetPackageExtension())))
	{
		return false;
	}
	if (!TestTrue(TEXT("initial import wrote the .uasset to disk"), FPaths::FileExists(PackageFileName)))
	{
		return false;
	}

	// Make the target unwritable, the same condition as a checked-in or
	// permission-stripped asset at sync time
	if (!TestTrue(TEXT("target .uasset marked read-only"), PlatformFile.SetReadOnly(*PackageFileName, true)))
	{
		return false;
	}

	// The save failure is expected to be reported, not fatal. Note: in this
	// engine version an expected error must actually occur or the test fails,
	// so only messages this scenario always produces are listed.
	AddExpectedError(TEXT("it is read only"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("Error saving"), EAutomationExpectedErrorFlags::Contains, 0);
	// The plugin must diagnose the read-only file and name the remedy, exactly
	// once for the one failed save
	AddExpectedError(TEXT("Clear the read-only flag"), EAutomationExpectedErrorFlags::Contains, 1);

	// Re-import over the read-only file. Unfixed, this never returns: the
	// engine routes the save failure to GError and the process dies.
	UStoryFlowScriptAsset* Reimported = UStoryFlowImporter::ImportScriptFromJson(JsonObject, TEXT("readonly/victim"), TestRoot);
	TestNotNull(TEXT("import survives a failed save and still returns the asset"), Reimported);

	// Cleanup
	PlatformFile.SetReadOnly(*PackageFileName, false);
	UEditorAssetLibrary::DeleteDirectory(TestRoot);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
