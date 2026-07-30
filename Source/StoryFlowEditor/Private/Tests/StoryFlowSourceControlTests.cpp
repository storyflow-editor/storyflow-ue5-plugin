// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Data/StoryFlowScriptAsset.h"
#include "Import/StoryFlowImporter.h"
#include "SourceControl/StoryFlowSourceControlUtils.h"
#include "EditorAssetLibrary.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"

/**
 * Tests the importer's revision-control contract via the test seam in
 * StoryFlowSourceControl: EnsureWritable runs before every save attempt and
 * a refusal skips the save (import continues), MarkForAdd runs after a save
 * that created a new file and never for overwrites or skipped saves.
 *
 * Run via: Session Frontend > Automation > "StoryFlow.SourceControl", or
 *   UnrealEditor-Cmd.exe StoryFlow.uproject -ExecCmds="Automation RunTests StoryFlow.SourceControl" -TestExit="Automation Test Queue Empty" -unattended -nullrhi
 */

namespace StoryFlowSourceControlTestHelpers
{
	const TCHAR* TestRoot = TEXT("/Game/StoryFlowSourceControlTests");

	TSharedPtr<FJsonObject> ParseJson(const FString& Json)
	{
		TSharedPtr<FJsonObject> JsonObject;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		FJsonSerializer::Deserialize(Reader, JsonObject);
		return JsonObject;
	}

	TSharedPtr<FJsonObject> ScriptV1()
	{
		return ParseJson(TEXT(R"JSON({"startNode":"0","nodes":{"0":{"type":"start","id":"0"}}})JSON"));
	}

	TSharedPtr<FJsonObject> ScriptV2()
	{
		return ParseJson(TEXT(R"JSON({"startNode":"0","nodes":{"0":{"type":"start","id":"0"},"1":{"type":"end","id":"1"}}})JSON"));
	}

	/** Installs overrides and restores the real behavior when leaving scope. */
	struct FScopedOverrides
	{
		explicit FScopedOverrides(const StoryFlowSourceControl::FTestOverrides& Overrides)
		{
			StoryFlowSourceControl::SetTestOverrides(Overrides);
		}
		~FScopedOverrides()
		{
			StoryFlowSourceControl::SetTestOverrides(TOptional<StoryFlowSourceControl::FTestOverrides>());
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowSourceControlSeamContractTest,
	"StoryFlow.SourceControl.SeamContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowSourceControlSeamContractTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowSourceControlTestHelpers;

	// Clean slate (a crashed prior run leaves assets behind)
	UEditorAssetLibrary::DeleteDirectory(TestRoot);

	int32 EnsureWritableCalls = 0;
	TArray<FString> MarkedForAdd;

	StoryFlowSourceControl::FTestOverrides Overrides;
	Overrides.EnsureWritable = [&EnsureWritableCalls](const FString&, FString&) { ++EnsureWritableCalls; return true; };
	Overrides.MarkForAdd = [&MarkedForAdd](const FString& Filename, FString&) { MarkedForAdd.Add(Filename); return true; };
	FScopedOverrides Scope(Overrides);

	// Fresh asset: EnsureWritable consulted, new file marked for add
	UStoryFlowScriptAsset* First = UStoryFlowImporter::ImportScriptFromJson(ScriptV1(), TEXT("sc/contract"), TestRoot);
	if (!TestNotNull(TEXT("initial import succeeds"), First))
	{
		return false;
	}
	TestEqual(TEXT("fresh save consults EnsureWritable once"), EnsureWritableCalls, 1);
	if (TestEqual(TEXT("fresh save marks the new file for add once"), MarkedForAdd.Num(), 1))
	{
		TestTrue(TEXT("marked file is the script's .uasset"), MarkedForAdd[0].EndsWith(TEXT("contract.uasset")));
	}

	// Unchanged re-import: save skipped, seam untouched
	UStoryFlowImporter::ImportScriptFromJson(ScriptV1(), TEXT("sc/contract"), TestRoot);
	TestEqual(TEXT("skipped save does not consult EnsureWritable"), EnsureWritableCalls, 1);
	TestEqual(TEXT("skipped save does not mark for add"), MarkedForAdd.Num(), 1);

	// Changed re-import over an existing file: checkout consulted, no new add
	UStoryFlowImporter::ImportScriptFromJson(ScriptV2(), TEXT("sc/contract"), TestRoot);
	TestEqual(TEXT("overwrite consults EnsureWritable again"), EnsureWritableCalls, 2);
	TestEqual(TEXT("overwrite of an existing file is not marked for add"), MarkedForAdd.Num(), 1);

	UEditorAssetLibrary::DeleteDirectory(TestRoot);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowSourceControlCheckoutFailureTest,
	"StoryFlow.SourceControl.CheckoutFailureSkipsSave",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowSourceControlCheckoutFailureTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowSourceControlTestHelpers;

	// Clean slate
	UEditorAssetLibrary::DeleteDirectory(TestRoot);

	// One scope covering every import below, so none of them can reach the real
	// provider on a machine with revision control enabled (that would check out
	// or p4-add these throwaway test assets). The refusal is toggled through
	// bRefuseCheckout rather than by installing a second FScopedOverrides:
	// nesting would not work, because leaving the inner scope restores an unset
	// optional and so wipes the outer one.
	bool bRefuseCheckout = false;
	int32 MarkForAddCalls = 0;

	StoryFlowSourceControl::FTestOverrides Overrides;
	Overrides.EnsureWritable = [&bRefuseCheckout](const FString&, FString& OutError)
	{
		if (bRefuseCheckout)
		{
			OutError = TEXT("checked out by TestUser");
			return false;
		}
		return true;
	};
	Overrides.MarkForAdd = [&MarkForAddCalls](const FString&, FString&) { ++MarkForAddCalls; return true; };
	FScopedOverrides Scope(Overrides);

	// Import normally first so the file exists on disk
	UStoryFlowScriptAsset* First = UStoryFlowImporter::ImportScriptFromJson(ScriptV1(), TEXT("sc/lockedfile"), TestRoot);
	if (!TestNotNull(TEXT("initial import succeeds"), First))
	{
		return false;
	}
	TestEqual(TEXT("creating the file marks it for add once"), MarkForAddCalls, 1);

	FString PackageFileName;
	if (!TestTrue(TEXT("package maps to a filename"),
		FPackageName::TryConvertLongPackageNameToFilename(First->GetOutermost()->GetName(), PackageFileName, FPackageName::GetAssetPackageExtension())))
	{
		return false;
	}
	// Read the baseline explicitly, so a byte comparison below cannot pass by
	// comparing two empty arrays from failed reads.
	TArray<uint8> BytesBefore;
	if (!TestTrue(TEXT("saved package reads back from disk"), FFileHelper::LoadFileToArray(BytesBefore, *PackageFileName)))
	{
		return false;
	}
	if (!TestTrue(TEXT("saved package is not empty"), BytesBefore.Num() > 0))
	{
		return false;
	}

	// A refused checkout must be reported once, skip the write and keep going
	AddExpectedError(TEXT("checked out by TestUser"), EAutomationExpectedErrorFlags::Contains, 1);
	bRefuseCheckout = true;
	UStoryFlowScriptAsset* Refused = UStoryFlowImporter::ImportScriptFromJson(ScriptV2(), TEXT("sc/lockedfile"), TestRoot);
	TestNotNull(TEXT("import survives a refused checkout"), Refused);
	TArray<uint8> BytesAfter;
	TestTrue(TEXT("package still reads back after a refused checkout"), FFileHelper::LoadFileToArray(BytesAfter, *PackageFileName));
	TestTrue(TEXT("refused checkout leaves the file untouched"), BytesBefore == BytesAfter);
	TestEqual(TEXT("refused checkout does not mark for add"), MarkForAddCalls, 1);

	// With the refusal gone, the same source must save (failed save cleared the hash)
	bRefuseCheckout = false;
	UStoryFlowScriptAsset* Retried = UStoryFlowImporter::ImportScriptFromJson(ScriptV2(), TEXT("sc/lockedfile"), TestRoot);
	TestNotNull(TEXT("retry import succeeds"), Retried);
	TArray<uint8> BytesRetried;
	TestTrue(TEXT("package reads back after the retry"), FFileHelper::LoadFileToArray(BytesRetried, *PackageFileName));
	TestTrue(TEXT("retry actually rewrote the file"), BytesBefore != BytesRetried);
	TestEqual(TEXT("rewriting an existing file does not mark for add"), MarkForAddCalls, 1);

	UEditorAssetLibrary::DeleteDirectory(TestRoot);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
