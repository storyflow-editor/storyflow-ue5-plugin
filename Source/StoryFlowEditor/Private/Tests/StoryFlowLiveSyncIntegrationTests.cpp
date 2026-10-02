// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "StoryFlowEngineContractFixtures.h"
#include "StoryFlowScopedWorld.h"
#include "Components/StoryFlowComponent.h"
#include "Components/TextBlock.h"
#include "Components/Button.h"
#include "Components/Image.h"
#include "Data/StoryFlowCharacterAsset.h"
#include "Data/StoryFlowDataAssetAsset.h"
#include "Data/StoryFlowProjectAsset.h"
#include "Data/StoryFlowScriptAsset.h"
#include "Data/StoryFlowSaveGame.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/Texture2D.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Sound/SoundWave.h"
#include "Subsystems/StoryFlowEditorSubsystem.h"
#include "Subsystems/StoryFlowSubsystem.h"
#include "Blueprint/WidgetTree.h"
#include "Slate/WidgetRenderer.h"
#include "UI/StoryFlowDialogueWidget.h"

namespace StoryFlowLiveSyncProbe
{
	const TCHAR* TestRoot = TEXT("/Game/StoryFlowLiveSyncProbe");

	TSharedPtr<FJsonObject> LoadJson(const FString& File)
	{
		FString Text;
		TSharedPtr<FJsonObject> Result;
		if (!FFileHelper::LoadFileToString(Text, *File)) { return nullptr; }
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Result);
		return Result;
	}

	EStoryFlowVariableType VariableType(const FString& Name)
	{
		if (Name == TEXT("boolean")) return EStoryFlowVariableType::Boolean;
		if (Name == TEXT("integer")) return EStoryFlowVariableType::Integer;
		if (Name == TEXT("float")) return EStoryFlowVariableType::Float;
		if (Name == TEXT("string")) return EStoryFlowVariableType::String;
		if (Name == TEXT("enum")) return EStoryFlowVariableType::Enum;
		if (Name == TEXT("image")) return EStoryFlowVariableType::Image;
		if (Name == TEXT("audio")) return EStoryFlowVariableType::Audio;
		if (Name == TEXT("character")) return EStoryFlowVariableType::Character;
		if (Name == TEXT("map")) return EStoryFlowVariableType::Map;
		return EStoryFlowVariableType::None;
	}

	bool CheckVariable(FAutomationTestBase& Test, const FString& Label, const FStoryFlowVariable* Actual, const TSharedPtr<FJsonObject>& Expected)
	{
		if (!Test.TestTrue(Label + TEXT(" exists"), Actual != nullptr && Expected.IsValid())) return false;
		bool bOk = true;
		bOk &= Test.TestEqual(Label + TEXT(" id"), Actual->Id, Expected->GetStringField(TEXT("id")));
		bOk &= Test.TestEqual(Label + TEXT(" name"), Actual->Name, Expected->GetStringField(TEXT("name")));
		bOk &= Test.TestTrue(Label + TEXT(" type"), Actual->Type == VariableType(Expected->GetStringField(TEXT("type"))));
		bool bArray = false;
		Expected->TryGetBoolField(TEXT("isArray"), bArray);
		bOk &= Test.TestTrue(Label + TEXT(" array flag"), Actual->bIsArray == bArray);
		if (Actual->Type == EStoryFlowVariableType::Map)
		{
			bOk &= Test.TestTrue(Label + TEXT(" map key type"), Actual->KeyType == VariableType(Expected->GetStringField(TEXT("keyType"))));
			bOk &= Test.TestTrue(Label + TEXT(" map value type"), Actual->ValueType == VariableType(Expected->GetStringField(TEXT("valueType"))));
		}
		const TArray<TSharedPtr<FJsonValue>>* EnumValues = nullptr;
		if (Expected->TryGetArrayField(TEXT("enumValues"), EnumValues))
		{
			bOk &= Test.TestEqual(Label + TEXT(" enum count"), Actual->EnumValues.Num(), EnumValues->Num());
			for (int32 I = 0; I < FMath::Min(Actual->EnumValues.Num(), EnumValues->Num()); ++I)
			{
				bOk &= Test.TestEqual(Label + TEXT(" enum item"), Actual->EnumValues[I], (*EnumValues)[I]->AsString());
			}
		}
		if (Expected->HasField(TEXT("value")))
		{
			bOk &= StoryFlowEngineContract::VariantMatchesJson(Test, Label + TEXT(" value"), Actual->Value, Expected->TryGetField(TEXT("value")));
		}
		return bOk;
	}

	bool CheckMedia(FAutomationTestBase& Test, const FString& Label, const TSharedPtr<FJsonObject>& Document,
		const TMap<FString, TSoftObjectPtr<UObject>>& Imported, int32 MinCount)
	{
		if (!Test.TestTrue(Label + TEXT(" has an asset registry"), Document->HasField(TEXT("assets")))) return false;
		bool bOk = true;
		const auto Assets = Document->GetObjectField(TEXT("assets"));
		bOk &= Test.TestTrue(Label + TEXT(" references media"), Assets->Values.Num() >= MinCount);
		for (const auto& Pair : Assets->Values)
		{
			const TSoftObjectPtr<UObject>* Media = Imported.Find(FString(Pair.Key));
			UObject* Object = Media ? Media->LoadSynchronous() : nullptr;
			const FString MediaLabel = Label + TEXT(" media ") + Pair.Key;
			bOk &= Test.TestNotNull(MediaLabel, Object);
			const FString Type = Pair.Value->AsObject()->GetStringField(TEXT("type"));
			if (Type == TEXT("image"))
			{
				const UTexture2D* Texture = Cast<UTexture2D>(Object);
				bOk &= Test.TestTrue(MediaLabel + TEXT(" has image pixels"), Texture && Texture->GetSizeX() > 0 && Texture->GetSizeY() > 0);
			}
			else if (Type == TEXT("audio"))
			{
				const USoundWave* Sound = Cast<USoundWave>(Object);
				bOk &= Test.TestTrue(MediaLabel + TEXT(" has decoded audio"), Sound && Sound->Duration > 0.0f);
			}
			else { bOk &= Test.TestTrue(MediaLabel + TEXT(" has a supported media type"), false); }
		}
		return bOk;
	}

	bool CheckImport(FAutomationTestBase& Test, UStoryFlowProjectAsset* Project, const FString& ExportDir,
		const FString& DialogueId, const FString& ExpectedLine, int32 ExpectedHp)
	{
		if (!Test.TestNotNull(TEXT("WebSocket sync installed project"), Project)) return false;
		const TSharedPtr<FJsonObject> Globals = LoadJson(FPaths::Combine(ExportDir, TEXT("global-variables.json")));
		const TSharedPtr<FJsonObject> Characters = LoadJson(FPaths::Combine(ExportDir, TEXT("characters.json")));
		const TSharedPtr<FJsonObject> Data = LoadJson(FPaths::Combine(ExportDir, TEXT("data-assets.json")));
		if (!Test.TestTrue(TEXT("exported JSON is readable"), Globals.IsValid() && Characters.IsValid() && Data.IsValid())) return false;
		bool bOk = true;
		const auto GlobalVars = Globals->GetObjectField(TEXT("variables"));
		bOk &= Test.TestEqual(TEXT("global count"), Project->GlobalVariables.Num(), GlobalVars->Values.Num());
		for (const auto& Pair : GlobalVars->Values)
		{
			const FString Key(Pair.Key);
			bOk &= CheckVariable(Test, TEXT("global ") + Key, Project->GlobalVariables.Find(Key), Pair.Value->AsObject());
		}
		bOk &= CheckMedia(Test, TEXT("global"), Globals, Project->ResolvedAssets, 3);

		const auto CharacterRecords = Characters->GetObjectField(TEXT("characters"));
		bOk &= Test.TestEqual(TEXT("character count"), Project->Characters.Num(), CharacterRecords->Values.Num());
		for (const auto& Pair : CharacterRecords->Values)
		{
			const FString Key = FString(Pair.Key).ToLower();
			UStoryFlowCharacterAsset* const* Found = Project->Characters.Find(Key);
			if (!Test.TestTrue(TEXT("character ") + Key, Found && *Found)) { bOk = false; continue; }
			const auto Expected = Pair.Value->AsObject();
			bOk &= Test.TestEqual(Key + TEXT(" path"), (*Found)->CharacterPath, Key);
			bOk &= Test.TestEqual(Key + TEXT(" name key"), (*Found)->Name, Expected->GetStringField(TEXT("name")));
			bOk &= Test.TestEqual(Key + TEXT(" portrait key"), (*Found)->Image, Expected->GetStringField(TEXT("image")));
			const auto Vars = Expected->GetObjectField(TEXT("variables"));
			bOk &= Test.TestEqual(Key + TEXT(" variable count"), (*Found)->Variables.Num(), Vars->Values.Num());
			for (const auto& Var : Vars->Values)
			{
				const auto Spec = Var.Value->AsObject();
				bOk &= CheckVariable(Test, Key + TEXT(" variable ") + Var.Key, (*Found)->Variables.Find(Spec->GetStringField(TEXT("name"))), Spec);
			}
			if (!(*Found)->Image.IsEmpty())
			{
				const auto* Portrait = (*Found)->ResolvedAssets.Find((*Found)->Image);
				bOk &= Test.TestTrue(Key + TEXT(" imported portrait"), Portrait && Portrait->LoadSynchronous() != nullptr);
			}
		}
		bOk &= CheckMedia(Test, TEXT("character"), Characters, Project->ResolvedAssets, 1);

		const auto DataRecords = Data->GetObjectField(TEXT("dataAssets"));
		bOk &= Test.TestEqual(TEXT("data asset count"), Project->DataAssets.Num(), DataRecords->Values.Num());
		for (const auto& Pair : DataRecords->Values)
		{
			const FString Key(Pair.Key);
			UStoryFlowDataAssetAsset* const* Found = Project->DataAssets.Find(Key);
			if (!Test.TestTrue(TEXT("data asset ") + Key, Found && *Found)) { bOk = false; continue; }
			const auto Spec = Pair.Value->AsObject();
			FString Parent;
			Spec->TryGetStringField(TEXT("parent"), Parent);
			bOk &= Test.TestEqual(Key + TEXT(" parent"), (*Found)->Parent, Parent);
			const auto Vars = Spec->GetArrayField(TEXT("variables"));
			bOk &= Test.TestEqual(Key + TEXT(" declaration count"), (*Found)->Variables.Num(), Vars.Num());
			for (int32 I = 0; I < FMath::Min((*Found)->Variables.Num(), Vars.Num()); ++I)
			{
				bOk &= CheckVariable(Test, Key + TEXT(" declaration"), &(*Found)->Variables[I], Vars[I]->AsObject());
			}
			const auto Overrides = Spec->GetObjectField(TEXT("overrides"));
			bOk &= Test.TestEqual(Key + TEXT(" override count"), (*Found)->Overrides.Num(), Overrides->Values.Num());
			for (const auto& Override : Overrides->Values)
			{
				const FStoryFlowVariant* Value = (*Found)->Overrides.Find(FString(Override.Key));
				bOk &= Test.TestTrue(Key + TEXT(" override exists"), Value != nullptr);
				if (Value) bOk &= StoryFlowEngineContract::VariantMatchesJson(Test, Key + TEXT(" override"), *Value, Override.Value);
			}
		}
		bOk &= CheckMedia(Test, TEXT("data asset"), Data, Project->ResolvedAssets, 3);
		StoryFlowDataAssets::FSeed Seed;
		StoryFlowDataAssets::FOverlay Overlay;
		StoryFlowDataAssets::BuildSeed(Project->DataAssets, Seed);
		FStoryFlowVariant Hp;
		bOk &= Test.TestTrue(TEXT("inherited child Health resolves"), StoryFlowDataAssets::TryResolve(Seed, Overlay,
			TEXT("da_20202020202020202020202020202020"), TEXT("30303030303030303030303030303030"), Hp));
		bOk &= Test.TestEqual(TEXT("inherited child Health"), Hp.GetInt(), ExpectedHp);

		UStoryFlowScriptAsset* Intro = Project->GetScriptByPath(TEXT("scripts/script_intro"));
		if (!Test.TestNotNull(TEXT("intro script"), Intro)) return false;
		const FStoryFlowNode* Dialogue = Intro->Nodes.Find(DialogueId);
		if (!Test.TestTrue(TEXT("intro dialogue"), Dialogue != nullptr)) return false;
		bOk &= Test.TestEqual(TEXT("intro text"), Intro->GetString(Dialogue->Data.Text), ExpectedLine);
		return bOk;
	}

	bool CheckRuntimeValues(FAutomationTestBase& Test, UStoryFlowComponent* Component, UStoryFlowProjectAsset* Project,
		const FString& ExportDir)
	{
		const TSharedPtr<FJsonObject> Globals = LoadJson(FPaths::Combine(ExportDir, TEXT("global-variables.json")));
		const TSharedPtr<FJsonObject> Characters = LoadJson(FPaths::Combine(ExportDir, TEXT("characters.json")));
		const TSharedPtr<FJsonObject> Data = LoadJson(FPaths::Combine(ExportDir, TEXT("data-assets.json")));
		if (!Test.TestTrue(TEXT("runtime comparison exports are readable"), Globals.IsValid() && Characters.IsValid() && Data.IsValid())) return false;
		const auto GlobalRecords = Globals->GetObjectField(TEXT("variables"));
		auto GlobalValue = [&GlobalRecords](const FString& Name) -> TSharedPtr<FJsonValue>
		{
			for (const auto& Pair : GlobalRecords->Values)
			{
				const auto Record = Pair.Value->AsObject();
				if (Record->GetStringField(TEXT("name")) == Name) return Record->TryGetField(TEXT("value"));
			}
			return nullptr;
		};
		bool bOk = true;
		bOk &= Test.TestEqual(TEXT("runtime PlayerHP"), Component->GetIntVariable(TEXT("PlayerHP"), true), 100);
		bOk &= Test.TestNearlyEqual(TEXT("runtime float global"), Component->GetFloatVariable(TEXT("SyncFloat"), true), 1.25f, 1.e-4f);
		bOk &= Test.TestEqual(TEXT("runtime enum global"), Component->GetEnumVariable(TEXT("SyncMode"), true), TEXT("Alert"));
		for (const TCHAR* Name : { TEXT("SyncImage"), TEXT("SyncAudio"), TEXT("SyncCharacter") })
		{
			const TSharedPtr<FJsonValue> Expected = GlobalValue(Name);
			bOk &= Test.TestTrue(FString(TEXT("exported global ")) + Name, Expected.IsValid());
			if (Expected) bOk &= Test.TestEqual(FString(TEXT("runtime global ")) + Name,
				Component->GetStringVariable(Name, true), Expected->AsString());
		}

		FString ElderId;
		for (const auto& Pair : Project->CharacterIdToPath)
		{
			if (Pair.Value.Contains(TEXT("npc_elder"))) { ElderId = Pair.Key; break; }
		}
		bOk &= Test.TestTrue(TEXT("runtime Elder id bridge"), !ElderId.IsEmpty());
		if (!ElderId.IsEmpty())
		{
			const FString ElderPath = Project->CharacterIdToPath.FindChecked(ElderId);
			const auto ElderRecord = Characters->GetObjectField(TEXT("characters"))->TryGetField(ElderPath);
			bOk &= Test.TestTrue(TEXT("exported Elder record"), ElderRecord.IsValid());
			if (ElderRecord)
			{
				const auto ElderVars = ElderRecord->AsObject()->GetObjectField(TEXT("variables"));
				for (const TCHAR* Name : { TEXT("SyncBadge"), TEXT("SyncAlly"), TEXT("SyncTrust") })
				{
					TSharedPtr<FJsonValue> Expected;
					for (const auto& Pair : ElderVars->Values)
					{
						const auto Record = Pair.Value->AsObject();
						if (Record->GetStringField(TEXT("name")) == Name) { Expected = Record->TryGetField(TEXT("value")); break; }
					}
					bOk &= StoryFlowEngineContract::VariantMatchesJson(Test, FString(TEXT("runtime Elder ")) + Name,
						Component->GetCharacterVariableById(ElderId, Name), Expected);
				}
			}
		}

		const auto DataRecords = Data->GetObjectField(TEXT("dataAssets"));
		const auto BaseRecord = DataRecords->GetObjectField(TEXT("da_10101010101010101010101010101010"));
		const auto ChildRecord = DataRecords->GetObjectField(TEXT("da_20202020202020202020202020202020"));
		UStoryFlowDataAssetAsset* const* Child = Project->DataAssets.Find(TEXT("da_20202020202020202020202020202020"));
		if (!Test.TestTrue(TEXT("runtime child Data Asset"), Child && *Child)) return false;
		const auto Overrides = ChildRecord->GetObjectField(TEXT("overrides"));
		for (const TSharedPtr<FJsonValue>& Item : BaseRecord->GetArrayField(TEXT("variables")))
		{
			const auto Declaration = Item->AsObject();
			const FString Id = Declaration->GetStringField(TEXT("id"));
			const FString Name = Declaration->GetStringField(TEXT("name"));
			bool bFound = false;
			const FStoryFlowVariant Value = Component->GetDataAssetVariantVariable(*Child, Name, bFound);
			bOk &= Test.TestTrue(TEXT("runtime child resolves ") + Name, bFound);
			if (bFound) bOk &= StoryFlowEngineContract::VariantMatchesJson(Test, TEXT("runtime child ") + Name, Value,
				Overrides->HasField(Id) ? Overrides->TryGetField(Id) : Declaration->TryGetField(TEXT("value")));
		}
		return bOk;
	}

	bool Choose(FAutomationTestBase& Test, UStoryFlowComponent* Component, const FString& Label)
	{
		for (const FStoryFlowDialogueOption& Option : Component->GetCurrentDialogue().Options)
		{
			if (Option.Text == Label) { Component->SelectOption(Option.Id); return true; }
		}
		Test.AddError(TEXT("Runtime option missing: ") + Label);
		return false;
	}

	void GatherWidgets(UUserWidget* Root, TArray<UWidget*>& Out)
	{
		if (!Root || !Root->WidgetTree) return;
		TArray<UWidget*> Own;
		Root->WidgetTree->GetAllWidgets(Own);
		for (UWidget* Child : Own)
		{
			Out.Add(Child);
			if (UUserWidget* Nested = Cast<UUserWidget>(Child)) GatherWidgets(Nested, Out);
		}
	}

	FString WidgetText(UUserWidget* Root)
	{
		TArray<UWidget*> Children;
		GatherWidgets(Root, Children);
		FString Text;
		for (UWidget* Child : Children)
		{
			if (UTextBlock* Label = Cast<UTextBlock>(Child)) Text += Label->GetText().ToString() + TEXT("\n");
		}
		return Text;
	}

	UButton* OptionButton(UUserWidget* Root, const FString& Label)
	{
		if (!Root || !Root->WidgetTree) return nullptr;
		TArray<UWidget*> Direct;
		Root->WidgetTree->GetAllWidgets(Direct);
		for (UWidget* Child : Direct)
		{
			UUserWidget* Nested = Cast<UUserWidget>(Child);
			if (!Nested || !Nested->WidgetTree) continue;
			TArray<UWidget*> Inner;
			Nested->WidgetTree->GetAllWidgets(Inner);
			bool bHasLabel = false;
			UButton* Button = nullptr;
			for (UWidget* Item : Inner)
			{
				if (UTextBlock* Text = Cast<UTextBlock>(Item)) bHasLabel |= Text->GetText().ToString() == Label;
				if (UButton* Found = Cast<UButton>(Item)) Button = Found;
			}
			if (bHasLabel && Button) return Button;
		}
		return nullptr;
	}

	bool CaptureWidget(FAutomationTestBase& Test, const TSharedRef<SWidget>& SlateWidget, const FString& Path)
	{
		FWidgetRenderer Renderer(true, true);
		UTextureRenderTarget2D* Frame = Renderer.DrawWidget(SlateWidget, FVector2D(1280, 720));
		TArray<FColor> Pixels;
		const bool bRead = Frame && Frame->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
		if (!Test.TestTrue(TEXT("UMG frame can be read"), bRead && Pixels.Num() == 1280 * 720)) return false;
		TSet<FColor> Colors;
		for (int32 I = 0; I < Pixels.Num(); I += 97) Colors.Add(Pixels[I]);
		return Test.TestTrue(TEXT("UMG frame contains rendered content"), Colors.Num() > 10)
			&& Test.TestTrue(TEXT("UMG frame saved"), FFileHelper::CreateBitmap(*Path, 1280, 720, Pixels.GetData()));
	}

	bool CheckSaveLoad(FAutomationTestBase& Test, UStoryFlowSubsystem* Runtime,
		UStoryFlowComponent* Component, UStoryFlowProjectAsset* Project, const FString& ResultDir)
	{
		const FString Slot = TEXT("StoryFlowLiveSyncSaveProbe");
		const FString ColdSlot = TEXT("StoryFlowLiveSyncColdProbe");
		const FString ChildId = TEXT("da_20202020202020202020202020202020");
		const FString BaseId = TEXT("da_10101010101010101010101010101010");
		const FString HpId = TEXT("30303030303030303030303030303030");
		const FString ImageId = TEXT("80808080808080808080808080808080");
		const FString CharacterId = TEXT("90909090909090909090909090909090");
		const FString AudioId = TEXT("abababababababababababababababab");
		const FString ImagesId = TEXT("bcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbc");
		const FString MapId = TEXT("cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd");
		UGameplayStatics::DeleteGameInSlot(Slot, 0);
		UGameplayStatics::DeleteGameInSlot(ColdSlot, 0);
		UStoryFlowDataAssetAsset* Child = Project->DataAssets.FindRef(ChildId);
		UStoryFlowDataAssetAsset* Base = Project->DataAssets.FindRef(BaseId);
		if (!Test.TestNotNull(TEXT("save probe imported child"), Child)) return false;
		if (!Test.TestNotNull(TEXT("save probe imported base"), Base)) return false;
		FString ElderId;
		for (const auto& Pair : Project->CharacterIdToPath)
		{
			if (Pair.Value.Contains(TEXT("npc_elder"))) { ElderId = Pair.Key; break; }
		}
		if (!Test.TestTrue(TEXT("save probe Elder id"), !ElderId.IsEmpty())) return false;
		const FString ElderPath = Project->CharacterIdToPath.FindChecked(ElderId);
		FStoryFlowCharacterDef* Elder = Runtime->GetRuntimeCharacters().Find(ElderPath);
		if (!Test.TestTrue(TEXT("save probe runtime Elder"), Elder != nullptr)) return false;
		const FString SmilingKey = Elder->Image;
		Elder->Name = TEXT("Saved Elder");
		Elder->bNameIsLiteral = true;
		FStoryFlowVariant FalseValue;
		FalseValue.SetBool(false);
		Component->SetCharacterVariableById(ElderId, TEXT("SyncTrust"), FalseValue);
		Component->SetIntVariable(TEXT("PlayerHP"), 73, true);
		bool bOk = true;
		bOk &= Test.TestTrue(TEXT("save writes Health"), Runtime->SetDataAssetIntVariable(Child, TEXT("Health"), 37));
		bOk &= Test.TestTrue(TEXT("save writes Title"), Runtime->SetDataAssetStringVariable(Child, TEXT("Title"), TEXT("Saved Child")));
		bOk &= Test.TestTrue(TEXT("save writes Enabled"), Runtime->SetDataAssetBoolVariable(Child, TEXT("Enabled"), true));
		bOk &= Test.TestTrue(TEXT("save writes Rate"), Runtime->SetDataAssetFloatVariable(Child, TEXT("Rate"), 6.25f));
		bOk &= Test.TestTrue(TEXT("save writes Mode"), Runtime->SetDataAssetEnumVariable(Child, TEXT("Mode"), TEXT("Closed")));
		bool bFound = false;
		const FStoryFlowVariant Images = Runtime->GetDataAssetVariantVariable(Child, TEXT("Portraits"), bFound);
		bOk &= Test.TestTrue(TEXT("save probe image array"), bFound && Images.GetArray().Num() > 0);
		const FStoryFlowVariant Map = Runtime->GetDataAssetVariantVariable(Child, TEXT("SpeakerByRole"), bFound);
		bOk &= Test.TestTrue(TEXT("save probe character map"), bFound && Map.GetMap().Num() > 0);
		if (!bOk) return false;
		TArray<FStoryFlowVariant> NewImages = Images.GetArray();
		const FStoryFlowVariant ExtraImage = NewImages[0];
		NewImages.Add(ExtraImage);
		TArray<FStoryFlowVariant> Keys;
		TArray<FStoryFlowVariant> Values;
		for (const FStoryFlowMapEntry& Entry : Map.GetMap()) { Keys.Add(Entry.Key); Values.Add(Entry.Value); }
		FStoryFlowVariant ExtraKey;
		ExtraKey.SetString(TEXT("alternate"));
		Keys.Add(ExtraKey);
		const FStoryFlowVariant ExtraValue = Values[0];
		Values.Add(ExtraValue);
		bOk &= Test.TestTrue(TEXT("save writes image array"), Runtime->SetDataAssetArrayVariable(Child, TEXT("Portraits"), NewImages));
		bOk &= Test.TestTrue(TEXT("save writes character map"), Runtime->SetDataAssetMapVariable(Child, TEXT("SpeakerByRole"), Keys, Values));
		if (!bOk) return false;
		const FString IconKey = Runtime->GetDataAssetStringVariable(Base, TEXT("Icon"), bFound);
		const FString SpeakerKey = Runtime->GetDataAssetStringVariable(Base, TEXT("Speaker"), bFound);
		const FString CurrentCue = Runtime->GetDataAssetStringVariable(Child, TEXT("Cue"), bFound);
		FString AudioKey;
		for (const auto& Pair : Project->ResolvedAssets)
		{
			if (Pair.Key != CurrentCue && Cast<USoundWave>(Pair.Value.LoadSynchronous())) { AudioKey = Pair.Key; break; }
		}
		bOk &= Test.TestTrue(TEXT("save probe alternate audio"), !AudioKey.IsEmpty());
		bOk &= Test.TestTrue(TEXT("save writes image reference"), Runtime->SetDataAssetStringVariable(Child, TEXT("Icon"), IconKey));
		bOk &= Test.TestTrue(TEXT("save writes character reference"), Runtime->SetDataAssetStringVariable(Child, TEXT("Speaker"), SpeakerKey));
		bOk &= Test.TestTrue(TEXT("save writes audio reference"), Runtime->SetDataAssetStringVariable(Child, TEXT("Cue"), AudioKey));
		if (!bOk) return false;
		Runtime->GetUsedOnceOnlyOptions().Add(TEXT("sync_probe.once"));
		if (!Test.TestTrue(TEXT("save imported game to slot"), Runtime->SaveToSlot(Slot, 0))) return false;
		auto ReadSlot = [&Slot]() -> TSharedPtr<FJsonObject>
		{
			UStoryFlowSaveGame* Saved = Cast<UStoryFlowSaveGame>(UGameplayStatics::LoadGameFromSlot(Slot, 0));
			TSharedPtr<FJsonObject> Parsed;
			if (Saved) FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Saved->SaveDataJson), Parsed);
			return Parsed;
		};
		const TSharedPtr<FJsonObject> First = ReadSlot();
		if (!Test.TestTrue(TEXT("slot contains JSON"), First.IsValid())) return false;
		bOk &= Test.TestEqual(TEXT("save format version"), First->GetStringField(TEXT("version")), TEXT("1"));
		const auto Globals = First->GetObjectField(TEXT("globalVariables"));
		bOk &= Test.TestEqual(TEXT("slot carries every imported global"), Globals->Values.Num(), Runtime->GetGlobalVariables().Num());
		bool bSavedHp = false;
		for (const auto& Pair : Globals->Values)
		{
			const auto Row = Pair.Value->AsObject();
			if (Row->GetStringField(TEXT("name")) == TEXT("PlayerHP")) bSavedHp = Row->GetNumberField(TEXT("value")) == 73;
		}
		bOk &= Test.TestTrue(TEXT("slot contains changed global"), bSavedHp);
		const auto Characters = First->GetObjectField(TEXT("characters"));
		bOk &= Test.TestEqual(TEXT("slot carries every imported character"), Characters->Values.Num(), Runtime->GetRuntimeCharacters().Num());
		const TSharedPtr<FJsonObject>* SavedElder = nullptr;
		bOk &= Test.TestTrue(TEXT("slot contains Elder"), Characters->TryGetObjectField(ElderPath, SavedElder));
		if (SavedElder)
		{
			bOk &= Test.TestEqual(TEXT("slot contains changed name"), (*SavedElder)->GetStringField(TEXT("name")), TEXT("Saved Elder"));
			bOk &= Test.TestEqual(TEXT("slot contains portrait"), (*SavedElder)->GetStringField(TEXT("image")), SmilingKey);
			bOk &= Test.TestTrue(TEXT("slot contains custom character variables"),
				(*SavedElder)->GetObjectField(TEXT("variables"))->Values.Num() >= 3);
		}
		bOk &= Test.TestTrue(TEXT("slot contains once-only choice"), First->GetArrayField(TEXT("usedOnceOnlyOptions")).ContainsByPredicate(
			[](const TSharedPtr<FJsonValue>& Value) { return Value->AsString() == TEXT("sync_probe.once"); }));
		const auto DataAssets = First->GetObjectField(TEXT("dataAssets"));
		const TSharedPtr<FJsonObject>* SavedChild = nullptr;
		bOk &= Test.TestTrue(TEXT("slot contains child overlay"), DataAssets->TryGetObjectField(ChildId, SavedChild));
		if (SavedChild)
		{
			bOk &= Test.TestEqual(TEXT("slot carries every Data Asset write shape"), (*SavedChild)->Values.Num(), 10);
			bOk &= Test.TestEqual(TEXT("slot contains Health"), (*SavedChild)->GetNumberField(HpId), 37.0);
			bOk &= Test.TestEqual(TEXT("slot contains image reference"), (*SavedChild)->GetStringField(ImageId), IconKey);
			bOk &= Test.TestEqual(TEXT("slot contains character reference"), (*SavedChild)->GetStringField(CharacterId), SpeakerKey);
			bOk &= Test.TestEqual(TEXT("slot contains audio reference"), (*SavedChild)->GetStringField(AudioId), AudioKey);
			bOk &= Test.TestTrue(TEXT("slot contains image array and character map"),
				(*SavedChild)->HasField(ImagesId) && (*SavedChild)->HasField(MapId));
		}
		if (!bOk) return false;
		Component->SetIntVariable(TEXT("PlayerHP"), 9, true);
		Runtime->ResetRuntimeCharacters();
		Runtime->ResetDataAssetOverlay();
		Runtime->GetUsedOnceOnlyOptions().Empty();
		Runtime->GetUsedOnceOnlyOptions().Add(TEXT("sync_probe.after_save"));
		bOk &= Test.TestEqual(TEXT("post-save global mutation landed"), Component->GetIntVariable(TEXT("PlayerHP"), true), 9);
		bOk &= Test.TestTrue(TEXT("post-save overlay reset landed"), Runtime->GetDataAssetIntVariable(Child, TEXT("Health"), bFound) != 37 && bFound);
		if (!bOk) return false;
		bOk &= Test.TestTrue(TEXT("load imported game from slot"), Runtime->LoadFromSlot(Slot, 0));
		Elder = Runtime->GetRuntimeCharacters().Find(ElderPath);
		bOk &= Test.TestEqual(TEXT("loaded global"), Component->GetIntVariable(TEXT("PlayerHP"), true), 73);
		bOk &= Test.TestTrue(TEXT("loaded Elder"), Elder && Elder->Name == TEXT("Saved Elder") && Elder->Image == SmilingKey && Elder->bNameIsLiteral);
		bOk &= Test.TestFalse(TEXT("loaded custom character boolean"), Component->GetCharacterVariableById(ElderId, TEXT("SyncTrust")).GetBool());
		bOk &= Test.TestEqual(TEXT("loaded Health"), Runtime->GetDataAssetIntVariable(Child, TEXT("Health"), bFound), 37);
		bOk &= Test.TestEqual(TEXT("loaded Title"), Runtime->GetDataAssetStringVariable(Child, TEXT("Title"), bFound), TEXT("Saved Child"));
		bOk &= Test.TestTrue(TEXT("loaded Enabled"), Runtime->GetDataAssetBoolVariable(Child, TEXT("Enabled"), bFound));
		bOk &= Test.TestNearlyEqual(TEXT("loaded Rate"), Runtime->GetDataAssetFloatVariable(Child, TEXT("Rate"), bFound), 6.25f);
		bOk &= Test.TestEqual(TEXT("loaded Mode"), Runtime->GetDataAssetEnumVariable(Child, TEXT("Mode"), bFound), TEXT("Closed"));
		bOk &= Test.TestEqual(TEXT("loaded image reference"), Runtime->GetDataAssetStringVariable(Child, TEXT("Icon"), bFound), IconKey);
		bOk &= Test.TestEqual(TEXT("loaded character reference"), Runtime->GetDataAssetStringVariable(Child, TEXT("Speaker"), bFound), SpeakerKey);
		bOk &= Test.TestEqual(TEXT("loaded audio reference"), Runtime->GetDataAssetStringVariable(Child, TEXT("Cue"), bFound), AudioKey);
		bOk &= Test.TestTrue(TEXT("loaded once-only set replaces later edits"),
			Runtime->GetUsedOnceOnlyOptions().Contains(TEXT("sync_probe.once")) &&
			!Runtime->GetUsedOnceOnlyOptions().Contains(TEXT("sync_probe.after_save")));
		bOk &= Test.TestTrue(TEXT("resave loaded state"), Runtime->SaveToSlot(Slot, 0));
		const TSharedPtr<FJsonObject> Second = ReadSlot();
		bOk &= Test.TestTrue(TEXT("resaved slot parses"), Second.IsValid());
		if (Second) bOk &= StoryFlowEngineContract::JsonEquals(Test, TEXT("full saved state"),
			MakeShared<FJsonValueObject>(Second), MakeShared<FJsonValueObject>(First));
		bOk &= Test.TestTrue(TEXT("prepare cold-process slot"), Runtime->SaveToSlot(ColdSlot, 0));
		FString ExpectedText;
		if (First) FJsonSerializer::Serialize(First.ToSharedRef(), TJsonWriterFactory<>::Create(&ExpectedText));
		bOk &= Test.TestTrue(TEXT("write cold-process expected envelope"),
			!ExpectedText.IsEmpty() && FFileHelper::SaveStringToFile(ExpectedText,
				*FPaths::Combine(ResultDir, TEXT("cold-expected.json"))));
		UGameplayStatics::DeleteGameInSlot(Slot, 0);
		return bOk;
	}

	bool CheckLegacyImport(FAutomationTestBase& Test, UStoryFlowProjectAsset* Project,
		const FString& ExportDir, const FString& DialogueId, const FString& ExpectedLine)
	{
		if (!Test.TestNotNull(TEXT("migrated WebSocket sync installed project"), Project)) return false;
		const TSharedPtr<FJsonObject> Globals = LoadJson(FPaths::Combine(ExportDir, TEXT("global-variables.json")));
		const TSharedPtr<FJsonObject> Characters = LoadJson(FPaths::Combine(ExportDir, TEXT("characters.json")));
		const TSharedPtr<FJsonObject> Index = LoadJson(FPaths::Combine(ExportDir, TEXT("character-index.json")));
		const TSharedPtr<FJsonObject> Data = LoadJson(FPaths::Combine(ExportDir, TEXT("data-assets.json")));
		if (!Test.TestTrue(TEXT("migrated export JSON is readable"),
			Globals.IsValid() && Characters.IsValid() && Index.IsValid() && Data.IsValid())) return false;
		bool bOk = true;
		const auto GlobalVars = Globals->GetObjectField(TEXT("variables"));
		bOk &= Test.TestEqual(TEXT("migrated global count"), Project->GlobalVariables.Num(), GlobalVars->Values.Num());
		for (const auto& Pair : GlobalVars->Values)
		{
			const FString Key(Pair.Key);
			bOk &= CheckVariable(Test, TEXT("migrated global ") + Key,
				Project->GlobalVariables.Find(Key), Pair.Value->AsObject());
		}
		bOk &= CheckMedia(Test, TEXT("migrated global"), Globals, Project->ResolvedAssets, 1);

		const auto CharacterRecords = Characters->GetObjectField(TEXT("characters"));
		bOk &= Test.TestEqual(TEXT("migrated character count"), Project->Characters.Num(), CharacterRecords->Values.Num());
		for (const auto& Pair : CharacterRecords->Values)
		{
			const FString Key = FString(Pair.Key).ToLower();
			UStoryFlowCharacterAsset* const* Found = Project->Characters.Find(Key);
			if (!Test.TestTrue(TEXT("migrated character ") + Key, Found && *Found)) { bOk = false; continue; }
			const auto Expected = Pair.Value->AsObject();
			bOk &= Test.TestEqual(Key + TEXT(" name key"), (*Found)->Name, Expected->GetStringField(TEXT("name")));
			bOk &= Test.TestEqual(Key + TEXT(" portrait key"), (*Found)->Image, Expected->GetStringField(TEXT("image")));
			const auto Variables = Expected->GetObjectField(TEXT("variables"));
			bOk &= Test.TestEqual(Key + TEXT(" variable count"), (*Found)->Variables.Num(), Variables->Values.Num());
			for (const auto& Variable : Variables->Values)
			{
				const auto Spec = Variable.Value->AsObject();
				bOk &= CheckVariable(Test, Key + TEXT(" variable ") + Variable.Key,
					(*Found)->Variables.Find(Spec->GetStringField(TEXT("name"))), Spec);
			}
			if (!(*Found)->Image.IsEmpty())
			{
				const auto* Portrait = (*Found)->ResolvedAssets.Find((*Found)->Image);
				bOk &= Test.TestTrue(Key + TEXT(" resolved portrait"), Portrait && Portrait->LoadSynchronous() != nullptr);
			}
		}
		bOk &= CheckMedia(Test, TEXT("migrated character"), Characters, Project->ResolvedAssets, 1);

		const auto Bridge = Index->GetObjectField(TEXT("characters"));
		bOk &= Test.TestEqual(TEXT("migrated character bridge count"), Project->CharacterIdToPath.Num(), Bridge->Values.Num());
		for (const auto& Pair : Bridge->Values)
		{
			const FString Key(Pair.Key);
			const FString* Actual = Project->CharacterIdToPath.Find(Key);
			bOk &= Test.TestTrue(TEXT("migrated character id ") + Key,
				Actual && *Actual == Pair.Value->AsString().ToLower() && Project->Characters.Contains(*Actual));
		}
		bOk &= Test.TestEqual(TEXT("frozen 1.7 data asset export is empty"),
			Data->GetObjectField(TEXT("dataAssets"))->Values.Num(), 0);
		bOk &= Test.TestEqual(TEXT("frozen 1.7 imported data assets are empty"), Project->DataAssets.Num(), 0);
		UStoryFlowScriptAsset* Intro = Project->GetScriptByPath(TEXT("scripts/script_intro"));
		if (!Test.TestNotNull(TEXT("migrated intro script"), Intro)) return false;
		const FStoryFlowNode* Dialogue = Intro->Nodes.Find(DialogueId);
		if (!Test.TestTrue(TEXT("migrated opening dialogue"), Dialogue != nullptr)) return false;
		bOk &= Test.TestEqual(TEXT("migrated opening text"), Intro->GetString(Dialogue->Data.Text), ExpectedLine);
		return bOk;
	}

	struct FColdLoad final : IAutomationLatentCommand
	{
		FAutomationTestBase* Test;
		FString ResultDir;
		FString OptionSource;

		FColdLoad(FAutomationTestBase* InTest, const FString& InResultDir, const FString& InOptionSource)
			: Test(InTest), ResultDir(InResultDir), OptionSource(InOptionSource) {}

		bool Update() override
		{
			const FString ColdSlot = TEXT("StoryFlowLiveSyncColdProbe");
			const FString AgainSlot = TEXT("StoryFlowLiveSyncColdProbeAgain");
			bool bOk = true;
			UStoryFlowProjectAsset* Project = LoadObject<UStoryFlowProjectAsset>(nullptr,
				TEXT("/Game/StoryFlowLiveSyncProbe/SF_Project.SF_Project"));
			if (!Test->TestNotNull(TEXT("fresh Unreal process loads imported project asset"), Project))
			{
				FFileHelper::SaveStringToFile(TEXT("cold_load=failed\nproject_asset=missing\n"),
					*FPaths::Combine(ResultDir, TEXT("cold-load.txt")));
				return true;
			}
			StoryFlowTestWorld::FScopedWorld World;
			if (!Test->TestTrue(TEXT("cold-load runtime world initialized"), World.Init()))
			{
				FFileHelper::SaveStringToFile(TEXT("cold_load=failed\nruntime_world=failed\n"),
					*FPaths::Combine(ResultDir, TEXT("cold-load.txt")));
				return true;
			}
			World.Subsystem->SetProject(Project);
			bOk &= Test->TestTrue(TEXT("fresh Unreal process loads cold slot"), World.Subsystem->LoadFromSlot(ColdSlot, 0));

			const TSharedPtr<FJsonObject> Expected = LoadJson(FPaths::Combine(ResultDir, TEXT("cold-expected.json")));
			bOk &= Test->TestTrue(TEXT("cold expected envelope parses"), Expected.IsValid());
			bOk &= Test->TestTrue(TEXT("fresh Unreal process resaves restored state"), World.Subsystem->SaveToSlot(AgainSlot, 0));
			UStoryFlowSaveGame* Resaved = Cast<UStoryFlowSaveGame>(UGameplayStatics::LoadGameFromSlot(AgainSlot, 0));
			TSharedPtr<FJsonObject> Actual;
			if (Resaved) FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Resaved->SaveDataJson), Actual);
			bOk &= Test->TestTrue(TEXT("cold resaved envelope parses"), Actual.IsValid());
			if (Expected && Actual) bOk &= StoryFlowEngineContract::JsonEquals(*Test, TEXT("cold full state"),
				MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueObject>(Expected));

			UClass* WidgetClass = LoadClass<UStoryFlowDialogueWidget>(nullptr,
				TEXT("/StoryFlowPlugin/Examples/WBP_Dialogue.WBP_Dialogue_C"));
			bOk &= Test->TestNotNull(TEXT("cold-load dialogue widget class"), WidgetClass);
			ULocalPlayer* LocalPlayer = NewObject<ULocalPlayer>(GEngine,
				GEngine->LocalPlayerClass ? GEngine->LocalPlayerClass.Get() : ULocalPlayer::StaticClass());
			APlayerController* Controller = World.World->SpawnActor<APlayerController>();
			bOk &= Test->TestTrue(TEXT("cold-load local player controller"), LocalPlayer && Controller);
			if (LocalPlayer && Controller)
			{
				Controller->Player = LocalPlayer;
				LocalPlayer->PlayerController = Controller;
				World.World->AddController(Controller);
			}
			World.Component->DialogueWidgetClass = WidgetClass;
			World.Component->bAutoAddWidgetToViewport = false;
			World.Component->StartDialogueWithScript(TEXT("scripts/script_intro"));
			UStoryFlowDialogueWidget* Widget = World.Component->GetDialogueWidget();
			bOk &= Test->TestNotNull(TEXT("cold-load dialogue created UMG"), Widget);
			UButton* Enter = Widget ? OptionButton(Widget, OptionSource) : nullptr;
			bOk &= Test->TestNotNull(TEXT("cold-load rendered intro option"), Enter);
			if (Enter) Enter->OnClicked.Broadcast();
			const FStoryFlowDialogueState Elder = World.Component->GetCurrentDialogue();
			bOk &= Test->TestEqual(TEXT("cold-load option enters Elder dialogue"), Elder.NodeId,
				TEXT("34a307bb13d44de7b0f996ca1431bd74"));
			bOk &= Test->TestEqual(TEXT("cold-load Elder name"), Elder.Character.Name, TEXT("Saved Elder"));
			bOk &= Test->TestTrue(TEXT("cold-load Elder portrait"), Elder.Character.Image &&
				Elder.Character.Image->GetName().Contains(TEXT("elder_smiling")));
			if (Widget)
			{
				bOk &= Test->TestTrue(TEXT("cold-load UMG shows restored Elder"), WidgetText(Widget).Contains(TEXT("Saved Elder")));
				bOk &= CaptureWidget(*Test, Widget->TakeWidget(), FPaths::Combine(ResultDir, TEXT("ui-cold-restored.bmp")));
			}
			World.Component->StopDialogue();
			UGameplayStatics::DeleteGameInSlot(ColdSlot, 0);
			UGameplayStatics::DeleteGameInSlot(AgainSlot, 0);
			FFileHelper::SaveStringToFile(bOk
				? TEXT("cold_load=passed\nfull_envelope=passed\nui_restore=passed\n")
				: TEXT("cold_load=failed\n"), *FPaths::Combine(ResultDir, TEXT("cold-load.txt")));
			return true;
		}
	};

	struct FWaitForDeletion final : IAutomationLatentCommand
	{
		FAutomationTestBase* Test;
		TWeakObjectPtr<UStoryFlowEditorSubsystem> Subsystem;
		FString ResultDir;
		FString BaseId;
		FString ChildId;
		FString TitleId;
		FString DialogueId;
		double Started = FPlatformTime::Seconds();

		FWaitForDeletion(FAutomationTestBase* InTest, UStoryFlowEditorSubsystem* InSubsystem,
			const FString& InResultDir, const FString& InBaseId, const FString& InChildId,
			const FString& InTitleId, const FString& InDialogueId)
			: Test(InTest), Subsystem(InSubsystem), ResultDir(InResultDir), BaseId(InBaseId),
			  ChildId(InChildId), TitleId(InTitleId), DialogueId(InDialogueId) {}

		bool Update() override
		{
			UStoryFlowEditorSubsystem* Live = Subsystem.Get();
			if (!Live)
			{
				Test->AddError(TEXT("StoryFlow editor subsystem disappeared during deletion resync"));
				return true;
			}
			if (FPlatformTime::Seconds() - Started > 240.0)
			{
				Test->AddError(TEXT("Timed out waiting for deleted StoryFlow data to leave the Unreal project"));
				FFileHelper::SaveStringToFile(TEXT("deletion resync timed out"),
					*FPaths::Combine(ResultDir, TEXT("failure.txt")));
				Live->Disconnect();
				return true;
			}

			UStoryFlowProjectAsset* Project = Live->GetProjectAsset();
			if (!Project) return false;
			bool bSyncGlobalsGone = true;
			for (const auto& Pair : Project->GlobalVariables)
			{
				bSyncGlobalsGone &= !Pair.Value.Name.StartsWith(TEXT("Sync"), ESearchCase::CaseSensitive);
			}
			UStoryFlowCharacterAsset* Elder = nullptr;
			for (const auto& Pair : Project->Characters)
			{
				if (Pair.Key.EndsWith(TEXT("npc_elder.sfc"), ESearchCase::IgnoreCase))
				{
					Elder = Pair.Value;
					break;
				}
			}
			bool bSyncCharacterFieldsGone = Elder != nullptr;
			if (Elder)
			{
				for (const auto& Pair : Elder->Variables)
				{
					bSyncCharacterFieldsGone &= !Pair.Value.Name.StartsWith(TEXT("Sync"), ESearchCase::CaseSensitive);
				}
			}
			UStoryFlowDataAssetAsset* Base = Project->DataAssets.FindRef(BaseId);
			bool bBaseDeclarationsGone = Base != nullptr;
			if (Base)
			{
				for (const FStoryFlowVariable& Variable : Base->Variables)
				{
					bBaseDeclarationsGone &= Variable.Id != TitleId &&
						Variable.Id != TEXT("80808080808080808080808080808080") &&
						Variable.Id != TEXT("abababababababababababababababab");
				}
			}
			const FStoryFlowStringTable* French = Project->LanguageStrings.Find(TEXT("fr"));
			const bool bLocalizationCorrect = French &&
				!French->Entries.Contains(TitleId + TEXT(".value")) &&
				French->Entries.Contains(DialogueId + TEXT(".text"));
			bool bStaleMediaGone = true;
			for (const auto& Pair : Project->ResolvedAssets)
			{
				bStaleMediaGone &= !Pair.Key.Contains(TEXT("syncprobeunique"), ESearchCase::IgnoreCase) &&
					!Pair.Value.ToSoftObjectPath().ToString().Contains(TEXT("syncprobeunique"), ESearchCase::IgnoreCase);
			}

			if (!bSyncGlobalsGone || !bSyncCharacterFieldsGone || Project->DataAssets.Contains(ChildId) ||
				!bBaseDeclarationsGone || !bLocalizationCorrect || !bStaleMediaGone)
			{
				return false;
			}

			FFileHelper::SaveStringToFile(TEXT("deletion_resync=passed\nstale_media=absent\n"),
				*FPaths::Combine(ResultDir, TEXT("deletion-resync.txt")));
			Live->Disconnect();
			return true;
		}
	};

	struct FWaitForLegacy final : IAutomationLatentCommand
	{
		FAutomationTestBase* Test;
		TWeakObjectPtr<UStoryFlowEditorSubsystem> Subsystem;
		FString ResultDir;
		FString ExportDir;
		FString DialogueId;
		FString DialogueText;
		FString OptionSource;
		FString ElderName;
		double Started = FPlatformTime::Seconds();

		FWaitForLegacy(FAutomationTestBase* InTest, UStoryFlowEditorSubsystem* InSubsystem,
			const FString& InResultDir, const FString& InExportDir, const FString& InDialogueId,
			const FString& InDialogueText, const FString& InOptionSource, const FString& InElderName)
			: Test(InTest), Subsystem(InSubsystem), ResultDir(InResultDir), ExportDir(InExportDir),
			  DialogueId(InDialogueId), DialogueText(InDialogueText), OptionSource(InOptionSource),
			  ElderName(InElderName) {}

		bool Update() override
		{
			UStoryFlowEditorSubsystem* Live = Subsystem.Get();
			if (!Live) { Test->AddError(TEXT("StoryFlow editor subsystem disappeared during migrated import")); return true; }
			if (FPlatformTime::Seconds() - Started > 480.0)
			{
				Test->AddError(TEXT("Timed out waiting for migrated 1.7 import"));
				FFileHelper::SaveStringToFile(TEXT("migrated import timed out"),
					*FPaths::Combine(ResultDir, TEXT("failure.txt")));
				Live->Disconnect();
				return true;
			}
			UStoryFlowProjectAsset* Project = Live->GetProjectAsset();
			// The same content root still contains the deletion-phase project when this
			// process starts. The frozen sample has no authored Data Assets and 63
			// globals, so wait for that replacement shape before running strict checks.
			if (!Project || Project->DataAssets.Num() != 0 || Project->GlobalVariables.Num() != 63) return false;
			bool bOk = CheckLegacyImport(*Test, Project, ExportDir, DialogueId, DialogueText);
			FString Diagnostics = FString::Printf(TEXT("import=%d\n"), bOk);

			StoryFlowTestWorld::FScopedWorld World;
			bOk &= Test->TestTrue(TEXT("migrated runtime world initialized"), World.Init());
			World.Subsystem->SetProject(Project);
			UClass* WidgetClass = LoadClass<UStoryFlowDialogueWidget>(nullptr,
				TEXT("/StoryFlowPlugin/Examples/WBP_Dialogue.WBP_Dialogue_C"));
			bOk &= Test->TestNotNull(TEXT("migrated dialogue widget class"), WidgetClass);
			ULocalPlayer* LocalPlayer = NewObject<ULocalPlayer>(GEngine,
				GEngine->LocalPlayerClass ? GEngine->LocalPlayerClass.Get() : ULocalPlayer::StaticClass());
			APlayerController* Controller = World.World->SpawnActor<APlayerController>();
			bOk &= Test->TestTrue(TEXT("migrated local player controller"), LocalPlayer && Controller);
			if (LocalPlayer && Controller)
			{
				Controller->Player = LocalPlayer;
				LocalPlayer->PlayerController = Controller;
				World.World->AddController(Controller);
			}
			World.Component->DialogueWidgetClass = WidgetClass;
			World.Component->bAutoAddWidgetToViewport = false;
			World.Component->StartDialogueWithScript(TEXT("scripts/script_intro"));
			const FStoryFlowDialogueState First = World.Component->GetCurrentDialogue();
			bOk &= Test->TestEqual(TEXT("migrated runtime opening node"), First.NodeId, DialogueId);
			bOk &= Test->TestEqual(TEXT("migrated runtime opening text"), First.Text, DialogueText);
			Diagnostics += FString::Printf(TEXT("runtime_node=%s\nruntime_text=%s\n"), *First.NodeId, *First.Text);
			UStoryFlowDialogueWidget* Widget = World.Component->GetDialogueWidget();
			bOk &= Test->TestNotNull(TEXT("migrated runtime created UMG"), Widget);
			if (Widget)
			{
				const FString Displayed = WidgetText(Widget);
				Diagnostics += TEXT("widget_text=") + Displayed + TEXT("\n");
				bOk &= Test->TestTrue(TEXT("migrated UMG displays opening story"),
					Displayed.Contains(DialogueText) && Displayed.Contains(OptionSource));
				bOk &= CaptureWidget(*Test, Widget->TakeWidget(),
					FPaths::Combine(ResultDir, TEXT("ui-legacy-intro.bmp")));
				UButton* Enter = OptionButton(Widget, OptionSource);
				bOk &= Test->TestNotNull(TEXT("migrated opening option has a UMG button"), Enter);
				if (Enter) Enter->OnClicked.Broadcast();
			}
			const FStoryFlowDialogueState Elder = World.Component->GetCurrentDialogue();
			Diagnostics += FString::Printf(TEXT("elder_node=%s\nelder_name=%s\nelder_image=%d\n"),
				*Elder.NodeId, *Elder.Character.Name, Elder.Character.Image != nullptr);
			bOk &= Test->TestEqual(TEXT("migrated Elder name reaches runtime"), Elder.Character.Name, ElderName);
			bOk &= Test->TestTrue(TEXT("migrated Elder portrait reaches runtime"), Elder.Character.Image != nullptr);
			FFileHelper::SaveStringToFile(bOk
				? TEXT("legacy_migration=passed\nvalue_parity=passed\nmedia=passed\nui_render=passed\n")
				: TEXT("legacy_migration=failed\n") + Diagnostics,
				*FPaths::Combine(ResultDir, TEXT("legacy-migration.txt")));
			Live->Disconnect();
			return true;
		}
	};

	struct FWaitForSync final : IAutomationLatentCommand
	{
		FAutomationTestBase* Test;
		TWeakObjectPtr<UStoryFlowEditorSubsystem> Subsystem;
		FString ResultDir;
		FString ExportDir;
		FString BaseId;
		FString DialogueId;
		FString FirstLine;
		FString SecondLine;
		FString FrenchLine;
		FString TitleId;
		FString OptionId;
		FString OptionSource;
		FString FrenchOption;
		FString ElderId;
		FString ElderName;
		FString FrenchElderName;
		FString BaseTitle;
		FString FrenchBaseTitle;
		int32 Stage = 0;
		double Started = FPlatformTime::Seconds();
		TUniquePtr<StoryFlowTestWorld::FScopedWorld> PlayWorld;
		TSharedPtr<SWidget> SlateWidget;
		bool bPlayback = true;
		bool bWaitingForFrenchElder = false;

		FWaitForSync(FAutomationTestBase* InTest, UStoryFlowEditorSubsystem* InSubsystem, const FString& InResultDir,
			const FString& InExportDir, const FString& InBaseId, const FString& InDialogueId, const FString& InFirstLine, const FString& InSecondLine,
			const FString& InFrenchLine, const FString& InTitleId, const FString& InOptionId, const FString& InOptionSource, const FString& InFrenchOption,
			const FString& InElderId, const FString& InElderName, const FString& InFrenchElderName,
			const FString& InBaseTitle, const FString& InFrenchBaseTitle)
			: Test(InTest), Subsystem(InSubsystem), ResultDir(InResultDir), ExportDir(InExportDir), BaseId(InBaseId), DialogueId(InDialogueId),
			  FirstLine(InFirstLine), SecondLine(InSecondLine), FrenchLine(InFrenchLine), TitleId(InTitleId), OptionId(InOptionId),
			  OptionSource(InOptionSource), FrenchOption(InFrenchOption), ElderId(InElderId), ElderName(InElderName),
			  FrenchElderName(InFrenchElderName), BaseTitle(InBaseTitle), FrenchBaseTitle(InFrenchBaseTitle) {}

		bool ContinuePlayback(UStoryFlowEditorSubsystem* Live)
		{
			if (!PlayWorld || !PlayWorld->Component) return true;
			PlayWorld->World->Tick(LEVELTICK_All, 0.016f);
			UStoryFlowDialogueWidget* Widget = PlayWorld->Component->GetDialogueWidget();
			if (!Widget)
			{
				Test->AddError(TEXT("dialogue widget disappeared during story walk"));
				FFileHelper::SaveStringToFile(TEXT("dialogue widget disappeared"), *FPaths::Combine(ResultDir, TEXT("failure.txt")));
				Live->Disconnect();
				return true;
			}
			// The sample Blueprint hides its panel for an animation when a button is
			// clicked. Wait for the next authored line to be in the actual widget tree.
			const FString Expected = Stage == 2 ? TEXT("An elderly man approaches you")
				: Stage == 3 ? TEXT("A cave troll has made its den")
				: Stage == 4 ? TEXT("Bless you!") : TEXT("The village square of Millhaven");
			const FString Displayed = WidgetText(Widget);
			if (!Displayed.Contains(Expected))
			{
				FWidgetRenderer TickRenderer(true, true);
				UTextureRenderTarget2D* TickFrame = TickRenderer.DrawWidget(SlateWidget.ToSharedRef(), FVector2D(640, 360));
				TArray<FColor> TickPixels;
				if (TickFrame) TickFrame->GameThread_GetRenderTargetResource()->ReadPixels(TickPixels);
				if (FPlatformTime::Seconds() - Started < 10.0) return false;
				const FString Failure = FString::Printf(TEXT("UMG never displayed '%s'; node=%s; constructed=%d; playing_animation=%d; correct_world=%d; widget text: %s"),
					*Expected, *PlayWorld->Component->GetCurrentDialogue().NodeId, Widget->IsConstructed(),
					Widget->IsAnyAnimationPlaying(), Widget->GetWorld() == PlayWorld->World, *Displayed);
				Test->AddError(Failure);
				FFileHelper::SaveStringToFile(Failure, *FPaths::Combine(ResultDir, TEXT("failure.txt")));
				Live->Disconnect();
				return true;
			}
			if (Stage == 2)
			{
				const FStoryFlowDialogueState Elder = PlayWorld->Component->GetCurrentDialogue();
				bPlayback &= Test->TestEqual(TEXT("UMG button entered elder dialogue"), Elder.NodeId,
					TEXT("34a307bb13d44de7b0f996ca1431bd74"));
				bPlayback &= Test->TestTrue(TEXT("elder line"), Elder.Text.Contains(Expected));
				bPlayback &= Test->TestEqual(TEXT("elder speaker"), Elder.Character.Name, ElderName);
				bPlayback &= Test->TestNotNull(TEXT("elder portrait resolved"), Elder.Character.Image.Get());
				bPlayback &= Test->TestTrue(TEXT("UMG shows elder speaker"), Displayed.Contains(ElderName));
				bPlayback &= Test->TestTrue(TEXT("UMG shows elder option"), Displayed.Contains(TEXT("What kind of help?")));
				TArray<UWidget*> ElderWidgets;
				GatherWidgets(Widget, ElderWidgets);
				bool bPortraitBound = false;
				for (UWidget* Child : ElderWidgets)
				{
				if (UImage* Image = Cast<UImage>(Child)) bPortraitBound |= Image->GetBrush().GetResourceObject() == Elder.Character.Image.Get();
				}
				bPlayback &= Test->TestTrue(TEXT("UMG portrait uses imported elder image"), bPortraitBound);
				bPlayback &= CaptureWidget(*Test, SlateWidget.ToSharedRef(), FPaths::Combine(ResultDir, TEXT("ui-elder.bmp")));
				UButton* Help = OptionButton(Widget, TEXT("What kind of help?"));
				bPlayback &= Test->TestNotNull(TEXT("elder help has a UMG button"), Help);
				if (Help) Help->OnClicked.Broadcast();
			}
			else if (Stage == 3)
			{
				const FStoryFlowDialogueState Quest = PlayWorld->Component->GetCurrentDialogue();
				bPlayback &= Test->TestEqual(TEXT("help reaches quest"), Quest.NodeId, TEXT("c7697227f48e4319ba97014adf12bb15"));
				UButton* Accept = OptionButton(Widget, TEXT("I'll do it. Where do I start?"));
				bPlayback &= Test->TestNotNull(TEXT("quest accept has a UMG button"), Accept);
				if (Accept) Accept->OnClicked.Broadcast();
			}
			else if (Stage == 4)
			{
				const FStoryFlowDialogueState Accepted = PlayWorld->Component->GetCurrentDialogue();
				bPlayback &= Test->TestEqual(TEXT("accept reaches answer"), Accepted.NodeId, TEXT("b91487d6449c4138900a3e34f32969e6"));
				bPlayback &= Test->TestTrue(TEXT("smiling portrait imported"), Accepted.Character.Image
					&& Accepted.Character.Image->GetName().Contains(TEXT("elder_smiling")));
				bPlayback &= CaptureWidget(*Test, SlateWidget.ToSharedRef(), FPaths::Combine(ResultDir, TEXT("ui-accepted.bmp")));
				UButton* Ready = OptionButton(Widget, TEXT("I'll get ready."));
				bPlayback &= Test->TestNotNull(TEXT("accepted line has a UMG continue button"), Ready);
				if (Ready) Ready->OnClicked.Broadcast();
			}
			else
			{
				const FStoryFlowDialogueState City = PlayWorld->Component->GetCurrentDialogue();
				bPlayback &= Test->TestEqual(TEXT("Run Script reaches city"), City.NodeId,
					TEXT("c1a0b0c0d0e0f0a0b0c0d0e0f0a0b0c0"));
				bPlayback &= Test->TestTrue(TEXT("city line"), City.Text.Contains(Expected));
				bPlayback &= CheckRuntimeValues(*Test, PlayWorld->Component, Live->GetProjectAsset(), ExportDir);

				PlayWorld->Component->StopDialogue();
				SlateWidget.Reset();
				PlayWorld->Component->StartDialogueWithScript(TEXT("scripts/script_intro"));
				bPlayback &= Choose(*Test, PlayWorld->Component, OptionSource);
				bPlayback &= Choose(*Test, PlayWorld->Component, TEXT("I'm just passing through."));
				bPlayback &= Test->TestTrue(TEXT("pass-through option ends dialogue"),
					!PlayWorld->Component->GetCurrentDialogue().bIsValid);

				PlayWorld->Component->StartDialogueWithScript(TEXT("scripts/script_intro"));
				bPlayback &= Choose(*Test, PlayWorld->Component, OptionSource);
				bPlayback &= Choose(*Test, PlayWorld->Component, TEXT("What kind of help?"));
				bPlayback &= Choose(*Test, PlayWorld->Component, TEXT("That sounds dangerous. I'll pass."));
				const FStoryFlowDialogueState Declined = PlayWorld->Component->GetCurrentDialogue();
				bPlayback &= Test->TestEqual(TEXT("decline reaches farewell"), Declined.NodeId,
					TEXT("bcac5c00572c40d9b1939816c5e97192"));
				bPlayback &= Test->TestTrue(TEXT("farewell line"), Declined.Text.Contains(TEXT("The elder nods slowly.")));
				PlayWorld->Component->StopDialogue();
				bPlayback &= CheckSaveLoad(*Test, PlayWorld->Subsystem, PlayWorld->Component, Live->GetProjectAsset(), ResultDir);
				PlayWorld->Component->StartDialogueWithScript(TEXT("scripts/script_intro"));
				bPlayback &= Choose(*Test, PlayWorld->Component, OptionSource);
				const FStoryFlowDialogueState RestoredElder = PlayWorld->Component->GetCurrentDialogue();
				bPlayback &= Test->TestEqual(TEXT("loaded Elder reaches the dialogue UI"), RestoredElder.NodeId,
					TEXT("34a307bb13d44de7b0f996ca1431bd74"));
				bPlayback &= Test->TestEqual(TEXT("loaded Elder name reaches dialogue"), RestoredElder.Character.Name,
					TEXT("Saved Elder"));
				bPlayback &= Test->TestTrue(TEXT("loaded Elder portrait reaches dialogue"), RestoredElder.Character.Image
					&& RestoredElder.Character.Image->GetName().Contains(TEXT("elder_smiling")));
				if (UStoryFlowDialogueWidget* RestoredWidget = PlayWorld->Component->GetDialogueWidget())
				{
					bPlayback &= Test->TestTrue(TEXT("loaded Elder name reaches UMG"), WidgetText(RestoredWidget).Contains(TEXT("Saved Elder")));
				}
				else { bPlayback &= Test->TestTrue(TEXT("loaded dialogue widget exists"), false); }
				PlayWorld->Component->StopDialogue();
				PlayWorld.Reset();
				FFileHelper::SaveStringToFile(bPlayback ? TEXT("playback=passed\ntyped_runtime=passed\nbranches=passed\nui_render=passed\nlocalization=passed\nsave_load=passed\n") : TEXT("playback=failed\n"),
					*FPaths::Combine(ResultDir, TEXT("playback.txt")));
				Live->Disconnect();
				return true;
			}
			++Stage;
			Started = FPlatformTime::Seconds();
			return false;
		}

		bool ContinueFrenchElder(UStoryFlowEditorSubsystem* Live)
		{
			if (!PlayWorld || !PlayWorld->Component) return true;
			PlayWorld->World->Tick(LEVELTICK_All, 0.016f);
			UStoryFlowDialogueWidget* Widget = PlayWorld->Component->GetDialogueWidget();
			if (!Widget)
			{
				Test->AddError(TEXT("French dialogue widget disappeared while its transition was running"));
				Live->Disconnect();
				return true;
			}
			const FString Displayed = WidgetText(Widget);
			if (!Displayed.Contains(FrenchElderName))
			{
				FWidgetRenderer TickRenderer(true, true);
				UTextureRenderTarget2D* TickFrame = SlateWidget.IsValid()
					? TickRenderer.DrawWidget(SlateWidget.ToSharedRef(), FVector2D(640, 360)) : nullptr;
				TArray<FColor> TickPixels;
				if (TickFrame) TickFrame->GameThread_GetRenderTargetResource()->ReadPixels(TickPixels);
				if (FPlatformTime::Seconds() - Started < 10.0) return false;
				const FString Failure = FString::Printf(TEXT("UMG never displayed localized Elder name '%s'; widget text: %s"),
					*FrenchElderName, *Displayed);
				Test->AddError(Failure);
				FFileHelper::SaveStringToFile(Failure, *FPaths::Combine(ResultDir, TEXT("failure.txt")));
				Live->Disconnect();
				return true;
			}

			bPlayback &= SlateWidget.IsValid() && CaptureWidget(*Test, SlateWidget.ToSharedRef(),
				FPaths::Combine(ResultDir, TEXT("ui-french-elder.bmp")));
			PlayWorld->Component->StopDialogue();
			bPlayback &= Test->TestFalse(TEXT("unknown Unreal language is refused"), PlayWorld->Subsystem->SetLanguage(TEXT("unknown")));
			bPlayback &= Test->TestEqual(TEXT("unknown language keeps French"), PlayWorld->Subsystem->GetLanguage(), TEXT("fr"));
			bPlayback &= Test->TestTrue(TEXT("Unreal runtime returns to source"), PlayWorld->Subsystem->SetLanguage(TEXT("en")));
			PlayWorld->Subsystem->ResetAllState();
			PlayWorld->Component->StartDialogueWithScript(TEXT("scripts/script_intro"));
			const FStoryFlowDialogueState State = PlayWorld->Component->GetCurrentDialogue();
			bPlayback &= Test->TestTrue(TEXT("intro dialogue active"), State.bIsValid);
			bPlayback &= Test->TestEqual(TEXT("runtime intro line"), State.Text, SecondLine);
			bPlayback &= Test->TestEqual(TEXT("runtime intro node"), State.NodeId, DialogueId);
			Widget = PlayWorld->Component->GetDialogueWidget();
			bPlayback &= Test->TestNotNull(TEXT("dialogue created the example UMG widget"), Widget);
			bPlayback &= Test->TestTrue(TEXT("example widget has a tree"), Widget && Widget->WidgetTree != nullptr);
			if (Widget && Widget->WidgetTree)
			{
				SlateWidget = Widget->TakeWidget();
				bPlayback &= Test->TestTrue(TEXT("example widget displays intro"), WidgetText(Widget).Contains(SecondLine));
				bPlayback &= Test->TestTrue(TEXT("example widget displays intro option"), WidgetText(Widget).Contains(OptionSource));
				bPlayback &= CaptureWidget(*Test, SlateWidget.ToSharedRef(), FPaths::Combine(ResultDir, TEXT("ui-intro.bmp")));
				bPlayback &= Test->TestTrue(TEXT("widget remains constructed after capture"), Widget->IsConstructed());
				UButton* Enter = OptionButton(Widget, OptionSource);
				bPlayback &= Test->TestNotNull(TEXT("intro option has a rendered UMG button"), Enter);
				if (Enter) Enter->OnClicked.Broadcast();
			}
			bWaitingForFrenchElder = false;
			Started = FPlatformTime::Seconds();
			return false;
		}

		bool Update() override
		{
			UStoryFlowEditorSubsystem* Live = Subsystem.Get();
			if (!Live) { Test->AddError(TEXT("StoryFlow editor subsystem disappeared")); return true; }
			if (bWaitingForFrenchElder) return ContinueFrenchElder(Live);
			if (Stage >= 2) return ContinuePlayback(Live);
			if (FPlatformTime::Seconds() - Started > 240.0)
			{
				Test->AddError(FString::Printf(TEXT("Timed out waiting for sync stage %d"), Stage + 1));
				FFileHelper::SaveStringToFile(TEXT("sync timed out"), *FPaths::Combine(ResultDir, TEXT("failure.txt")));
				Live->Disconnect();
				return true;
			}
			UStoryFlowProjectAsset* Project = Live->GetProjectAsset();
			if (!Project) return false;
			UStoryFlowScriptAsset* Intro = Project->GetScriptByPath(TEXT("scripts/script_intro"));
			const FStoryFlowNode* Dialogue = Intro ? Intro->Nodes.Find(DialogueId) : nullptr;
			const FString Line = Dialogue ? Intro->GetString(Dialogue->Data.Text) : FString();
			if (Line != (Stage == 0 ? FirstLine : SecondLine)) return false;
			const int32 Hp = Stage == 0 ? 10 : 21;
			if (!CheckImport(*Test, Project, ExportDir, DialogueId, Line, Hp))
			{
				FFileHelper::SaveStringToFile(TEXT("import parity failed; inspect Unreal automation log"), *FPaths::Combine(ResultDir, TEXT("failure.txt")));
				Live->Disconnect();
				return true;
			}
			const TSharedPtr<FJsonObject> Sidecar = LoadJson(FPaths::Combine(ExportDir, TEXT("localization.json")));
			const FStoryFlowStringTable* French = Project->LanguageStrings.Find(TEXT("fr"));
			const TSharedPtr<FJsonObject> Tables = Sidecar.IsValid() ? Sidecar->GetObjectField(TEXT("strings")) : nullptr;
			const TSharedPtr<FJsonObject> FrenchWire = Tables.IsValid() ? Tables->GetObjectField(TEXT("fr")) : nullptr;
			const TMap<FString, FString> ExpectedStrings = {
				{ DialogueId + TEXT(".text"), FrenchLine },
				{ OptionId, FrenchOption },
				{ ElderId + TEXT(".cf_name"), FrenchElderName },
				{ TitleId + TEXT(".value"), FrenchBaseTitle },
			};
			bool bStringsAgree = French != nullptr && FrenchWire.IsValid();
			for (const TPair<FString, FString>& Entry : ExpectedStrings)
			{
				const FString* Imported = French ? French->Entries.Find(Entry.Key) : nullptr;
				FString WireValue;
				bStringsAgree &= Imported && *Imported == Entry.Value && FrenchWire.IsValid() &&
					FrenchWire->TryGetStringField(Entry.Key, WireValue) && WireValue == Entry.Value;
			}
			if (!Test->TestTrue(TEXT("imported localization sidecar and multi-surface French table agree"),
				Sidecar.IsValid() && Project->bHasLocalization && Project->SourceLanguage == TEXT("en") &&
				Project->Languages.Num() == 1 && Project->Languages[0].Code == TEXT("fr") &&
				bStringsAgree))
			{
				FFileHelper::SaveStringToFile(TEXT("localization import parity failed"), *FPaths::Combine(ResultDir, TEXT("failure.txt")));
				Live->Disconnect();
				return true;
			}
			const FString Marker = FString::Printf(TEXT("value_parity=passed\nlocalization=passed\nchild_hp=%d\nintro=%s\n"), Hp, *Line);
			FFileHelper::SaveStringToFile(Marker, *FPaths::Combine(ResultDir, Stage == 0 ? TEXT("stage-1.txt") : TEXT("stage-2.txt")));
			if (Stage++ == 0) { Started = FPlatformTime::Seconds(); return false; }

			PlayWorld = MakeUnique<StoryFlowTestWorld::FScopedWorld>();
			if (!Test->TestTrue(TEXT("runtime world initialized"), PlayWorld->Init())) return true;
			PlayWorld->Subsystem->SetProject(Project);
			bPlayback &= Test->TestTrue(TEXT("French appears in the Unreal language roster"),
				PlayWorld->Subsystem->GetLanguages().ContainsByPredicate([](const FStoryFlowLanguage& Row) { return Row.Code == TEXT("fr"); }));
			UClass* WidgetClass = LoadClass<UStoryFlowDialogueWidget>(nullptr,
				TEXT("/StoryFlowPlugin/Examples/WBP_Dialogue.WBP_Dialogue_C"));
			if (!Test->TestNotNull(TEXT("plugin example dialogue widget class"), WidgetClass)) return true;
			ULocalPlayer* LocalPlayer = NewObject<ULocalPlayer>(GEngine,
				GEngine->LocalPlayerClass ? GEngine->LocalPlayerClass.Get() : ULocalPlayer::StaticClass());
			APlayerController* Controller = PlayWorld->World->SpawnActor<APlayerController>();
			if (!Test->TestTrue(TEXT("local player controller"), LocalPlayer && Controller)) return true;
			Controller->Player = LocalPlayer;
			LocalPlayer->PlayerController = Controller;
			PlayWorld->World->AddController(Controller);
			PlayWorld->Component->DialogueWidgetClass = WidgetClass;
			PlayWorld->Component->bAutoAddWidgetToViewport = false;
			bPlayback &= Test->TestTrue(TEXT("Unreal runtime accepts French"), PlayWorld->Subsystem->SetLanguage(TEXT("fr")));
			bPlayback &= Test->TestEqual(TEXT("active language is French"), PlayWorld->Subsystem->GetLanguage(), TEXT("fr"));
			UStoryFlowDataAssetAsset* BaseAsset = Project->DataAssets.FindRef(BaseId);
			bPlayback &= Test->TestNotNull(TEXT("localized base Data Asset imported"), BaseAsset);
			bool bFoundTitle = false;
			bPlayback &= Test->TestEqual(TEXT("authored Data Asset string localizes in French"),
				PlayWorld->Subsystem->GetDataAssetStringVariable(BaseAsset, TEXT("Title"), bFoundTitle), FrenchBaseTitle);
			bPlayback &= Test->TestTrue(TEXT("localized Data Asset Title found"), bFoundTitle);
			bPlayback &= Test->TestTrue(TEXT("write source-shaped Data Asset literal"),
				PlayWorld->Subsystem->SetDataAssetStringVariable(BaseAsset, TEXT("Title"), BaseTitle));
			bPlayback &= Test->TestTrue(TEXT("switch to source for literal provenance"), PlayWorld->Subsystem->SetLanguage(TEXT("en")));
			bPlayback &= Test->TestTrue(TEXT("switch back to French for literal provenance"), PlayWorld->Subsystem->SetLanguage(TEXT("fr")));
			bPlayback &= Test->TestEqual(TEXT("host-written source-shaped string stays literal"),
				PlayWorld->Subsystem->GetDataAssetStringVariable(BaseAsset, TEXT("Title"), bFoundTitle), BaseTitle);
			PlayWorld->Subsystem->ResetDataAssetOverlay();
			bPlayback &= Test->TestEqual(TEXT("overlay reset reveals authored French Data Asset string"),
				PlayWorld->Subsystem->GetDataAssetStringVariable(BaseAsset, TEXT("Title"), bFoundTitle), FrenchBaseTitle);
			PlayWorld->Component->StartDialogueWithScript(TEXT("scripts/script_intro"));
			const FStoryFlowDialogueState FrenchState = PlayWorld->Component->GetCurrentDialogue();
			bPlayback &= Test->TestEqual(TEXT("French intro resolves through the imported table"), FrenchState.Text, FrenchLine);
			bPlayback &= Test->TestEqual(TEXT("French intro option count"), FrenchState.Options.Num(), 1);
			if (FrenchState.Options.Num() == 1)
			{
				bPlayback &= Test->TestEqual(TEXT("French option keeps its id"), FrenchState.Options[0].Id, OptionId);
				bPlayback &= Test->TestEqual(TEXT("French option resolves through the imported table"), FrenchState.Options[0].Text, FrenchOption);
			}
			if (UStoryFlowDialogueWidget* FrenchWidget = PlayWorld->Component->GetDialogueWidget())
			{
				SlateWidget = FrenchWidget->TakeWidget();
				bPlayback &= Test->TestTrue(TEXT("example UMG widget displays French dialogue and option"),
					WidgetText(FrenchWidget).Contains(FrenchLine) && WidgetText(FrenchWidget).Contains(FrenchOption));
				bPlayback &= CaptureWidget(*Test, SlateWidget.ToSharedRef(), FPaths::Combine(ResultDir, TEXT("ui-french.bmp")));
				UButton* FrenchEnter = OptionButton(FrenchWidget, FrenchOption);
				bPlayback &= Test->TestNotNull(TEXT("translated option has a rendered UMG button"), FrenchEnter);
				if (FrenchEnter) FrenchEnter->OnClicked.Broadcast();
				const FStoryFlowDialogueState FrenchElder = PlayWorld->Component->GetCurrentDialogue();
				bPlayback &= Test->TestEqual(TEXT("translated option enters the Elder dialogue"), FrenchElder.NodeId,
					TEXT("34a307bb13d44de7b0f996ca1431bd74"));
				bPlayback &= Test->TestEqual(TEXT("authored Elder name localizes in French"), FrenchElder.Character.Name, FrenchElderName);
			}
			else bPlayback &= Test->TestTrue(TEXT("French dialogue created a UMG widget"), false);
			bWaitingForFrenchElder = true;
			Started = FPlatformTime::Seconds();
			return false;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowLiveSyncIntegrationTest, "StoryFlow.Integration.ExportSync",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowLiveSyncIntegrationTest::RunTest(const FString& Parameters)
{
	int32 ColdLoadMode = 0;
	FParse::Value(FCommandLine::Get(), TEXT("-sfColdLoad="), ColdLoadMode);
	const bool bColdLoad = ColdLoadMode == 1;
	int32 DeletionProbeMode = 0;
	FParse::Value(FCommandLine::Get(), TEXT("-sfDeletionProbe="), DeletionProbeMode);
	const bool bDeletionProbe = DeletionProbeMode == 1;
	int32 LegacyProbeMode = 0;
	FParse::Value(FCommandLine::Get(), TEXT("-sfLegacyProbe="), LegacyProbeMode);
	const bool bLegacyProbe = LegacyProbeMode == 1;
	FString ResultDir, ExportDir, BaseId, ChildId, DialogueId, FirstLine, SecondLine, FrenchLine, TitleId;
	FString OptionId, OptionSource, FrenchOption, ElderId, ElderName, FrenchElderName, BaseTitle, FrenchBaseTitle;
	int32 Port = 0;
	FParse::Value(FCommandLine::Get(), TEXT("-sfPort="), Port);
	FParse::Value(FCommandLine::Get(), TEXT("-sfResultDir="), ResultDir);
	FParse::Value(FCommandLine::Get(), TEXT("-sfExportDir="), ExportDir);
	FParse::Value(FCommandLine::Get(), TEXT("-sfBaseId="), BaseId);
	FParse::Value(FCommandLine::Get(), TEXT("-sfChildId="), ChildId);
	FParse::Value(FCommandLine::Get(), TEXT("-sfDialogueId="), DialogueId);
	FParse::Value(FCommandLine::Get(), TEXT("-sfTitleId="), TitleId);
	FParse::Value(FCommandLine::Get(), TEXT("-sfFirstLine="), FirstLine);
	FParse::Value(FCommandLine::Get(), TEXT("-sfSecondLine="), SecondLine);
	FParse::Value(FCommandLine::Get(), TEXT("-sfFrenchLine="), FrenchLine);
	FParse::Value(FCommandLine::Get(), TEXT("-sfOptionId="), OptionId);
	FParse::Value(FCommandLine::Get(), TEXT("-sfOptionSource="), OptionSource);
	FParse::Value(FCommandLine::Get(), TEXT("-sfFrenchOption="), FrenchOption);
	FParse::Value(FCommandLine::Get(), TEXT("-sfElderId="), ElderId);
	FParse::Value(FCommandLine::Get(), TEXT("-sfElderName="), ElderName);
	FParse::Value(FCommandLine::Get(), TEXT("-sfFrenchElderName="), FrenchElderName);
	FParse::Value(FCommandLine::Get(), TEXT("-sfBaseTitle="), BaseTitle);
	FParse::Value(FCommandLine::Get(), TEXT("-sfFrenchBaseTitle="), FrenchBaseTitle);
	if (bLegacyProbe)
	{
		FString LegacyLineFile;
		FParse::Value(FCommandLine::Get(), TEXT("-sfLegacyLineFile="), LegacyLineFile);
		if (!TestTrue(TEXT("legacy line sidecar supplied"), !LegacyLineFile.IsEmpty() &&
			FFileHelper::LoadFileToString(FirstLine, *LegacyLineFile))) return false;
	}
	if (!bLegacyProbe) FirstLine.ReplaceInline(TEXT("_"), TEXT(" "));
	SecondLine.ReplaceInline(TEXT("_"), TEXT(" "));
	FrenchLine.ReplaceInline(TEXT("_"), TEXT(" "));
	OptionSource.ReplaceInline(TEXT("_"), TEXT(" "));
	FrenchOption.ReplaceInline(TEXT("_"), TEXT(" "));
	ElderName.ReplaceInline(TEXT("_"), TEXT(" "));
	FrenchElderName.ReplaceInline(TEXT("_"), TEXT(" "));
	BaseTitle.ReplaceInline(TEXT("_"), TEXT(" "));
	FrenchBaseTitle.ReplaceInline(TEXT("_"), TEXT(" "));
	if (!TestTrue(TEXT("integration arguments supplied"), Port > 0 && !ResultDir.IsEmpty() && !ExportDir.IsEmpty()
		&& !BaseId.IsEmpty() && !ChildId.IsEmpty() && !DialogueId.IsEmpty() && !TitleId.IsEmpty() && !FirstLine.IsEmpty() && !SecondLine.IsEmpty()
		&& !FrenchLine.IsEmpty() && !OptionId.IsEmpty() && !OptionSource.IsEmpty() && !FrenchOption.IsEmpty()
		&& !ElderId.IsEmpty() && !ElderName.IsEmpty() && !FrenchElderName.IsEmpty()
		&& !BaseTitle.IsEmpty() && !FrenchBaseTitle.IsEmpty())) return false;
	IFileManager::Get().MakeDirectory(*ResultDir, true);
	if (bColdLoad)
	{
		ADD_LATENT_AUTOMATION_COMMAND(StoryFlowLiveSyncProbe::FColdLoad(this, ResultDir, OptionSource));
		return true;
	}
	UStoryFlowEditorSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UStoryFlowEditorSubsystem>() : nullptr;
	if (!TestNotNull(TEXT("StoryFlow editor subsystem"), Subsystem)) return false;
	if (bDeletionProbe)
	{
		Subsystem->SetContentPath(StoryFlowLiveSyncProbe::TestRoot);
		Subsystem->ConnectToStoryFlow(TEXT("127.0.0.1"), Port);
		ADD_LATENT_AUTOMATION_COMMAND(StoryFlowLiveSyncProbe::FWaitForDeletion(
			this, Subsystem, ResultDir, BaseId, ChildId, TitleId, DialogueId));
		return true;
	}
	if (bLegacyProbe)
	{
		Subsystem->SetContentPath(StoryFlowLiveSyncProbe::TestRoot);
		Subsystem->ConnectToStoryFlow(TEXT("127.0.0.1"), Port);
		ADD_LATENT_AUTOMATION_COMMAND(StoryFlowLiveSyncProbe::FWaitForLegacy(
			this, Subsystem, ResultDir, ExportDir, DialogueId, FirstLine, OptionSource, ElderName));
		return true;
	}
	UEditorAssetLibrary::DeleteDirectory(StoryFlowLiveSyncProbe::TestRoot);
	Subsystem->SetContentPath(StoryFlowLiveSyncProbe::TestRoot);
	Subsystem->ConnectToStoryFlow(TEXT("127.0.0.1"), Port);
	ADD_LATENT_AUTOMATION_COMMAND(StoryFlowLiveSyncProbe::FWaitForSync(this, Subsystem, ResultDir, ExportDir, BaseId,
		DialogueId, FirstLine, SecondLine, FrenchLine, TitleId, OptionId, OptionSource, FrenchOption,
		ElderId, ElderName, FrenchElderName, BaseTitle, FrenchBaseTitle));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
