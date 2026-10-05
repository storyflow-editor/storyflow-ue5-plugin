// Copyright 2026 StoryFlow. All Rights Reserved.
#include "MetaHuman/StoryFlowMetaHumanDetails.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "IDetailCustomization.h"
#include "DetailLayoutBuilder.h"
#include "DetailCategoryBuilder.h"
#include "DetailWidgetRow.h"
#include "PropertyHandle.h"
#include "IPropertyUtilities.h"
#include "MetaHuman/StoryFlowMetaHumanBakeSubsystem.h"
#include "MetaHuman/StoryFlowMetaHumanLipsyncComponent.h"
#include "Data/StoryFlowProjectAsset.h"
#include "Sound/SoundWave.h"
#include "Editor.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/SBoxPanel.h"

class FStoryFlowMetaHumanDetails : public IDetailCustomization
{
	TWeakObjectPtr<UStoryFlowMetaHumanLipsyncComponent> Component;
	TArray<TSharedPtr<FString>> CharacterOptions;
	TMap<FString, FString> CharacterLabels;
	TArray<TSharedPtr<TWeakObjectPtr<USoundWave>>> VoiceOptions;
	TSharedPtr<TWeakObjectPtr<USoundWave>> SelectedVoice;
	UStoryFlowMetaHumanBakeSubsystem* Subsystem() const { return GEditor->GetEditorSubsystem<UStoryFlowMetaHumanBakeSubsystem>(); }
public:
	static TSharedRef<IDetailCustomization> MakeInstance() { return MakeShared<FStoryFlowMetaHumanDetails>(); }
	virtual void CustomizeDetails(IDetailLayoutBuilder& Builder) override
	{
		TArray<TWeakObjectPtr<UObject>> Objects;
		Builder.GetObjectsBeingCustomized(Objects);
		if (Objects.Num() != 1) return;
		Component = Cast<UStoryFlowMetaHumanLipsyncComponent>(Objects[0].Get());
		if (!Component.IsValid() || !GEditor || !Subsystem()) return;
		CharacterOptions.Reset(); VoiceOptions.Reset(); CharacterLabels.Reset(); SelectedVoice.Reset();
		Subsystem()->InvalidateStatusCache();
		auto& Category = Builder.EditCategory("StoryFlow Editor Lip Sync", FText::GetEmpty(), ECategoryPriority::Important);
		Category.AddCustomRow(FText::FromString("Status")).WholeRowContent()[
			SNew(STextBlock).AutoWrapText(true).Text_Lambda([this] { return FText::FromString(Component.IsValid() ? Subsystem()->GetStatus(Component.Get()).Message : TEXT("Select a character.")); })];
		const TWeakPtr<IPropertyUtilities> Utilities = Builder.GetPropertyUtilities();
		auto ProjectProperty = Builder.GetProperty("BakingProject");
		ProjectProperty->SetOnPropertyValueChanged(FSimpleDelegate::CreateLambda([this, Utilities] { Subsystem()->InvalidateStatusCache(); if (auto Pinned = Utilities.Pin()) Pinned->RequestRefresh(); }));
		Category.AddProperty(ProjectProperty);
		Builder.HideProperty("CharacterId");
		CharacterOptions.Add(MakeShared<FString>(TEXT("")));
		if (auto* Project = Subsystem()->ResolveProject(Component.Get()))
		{
			CharacterLabels = Subsystem()->GetCharacters(Project);
			TArray<FString> Keys; CharacterLabels.GetKeys(Keys); Keys.Sort([this](const FString& A, const FString& B) { return CharacterLabels[A] < CharacterLabels[B]; });
			for (const auto& Key : Keys) CharacterOptions.Add(MakeShared<FString>(Key));
			for (auto* Voice : Subsystem()->DiscoverVoices(Project, Component->CharacterId)) VoiceOptions.Add(MakeShared<TWeakObjectPtr<USoundWave>>(Voice));
			if (!VoiceOptions.IsEmpty()) SelectedVoice = VoiceOptions[0];
		}
		Category.AddCustomRow(FText::FromString("Character"))
		.NameContent()[SNew(STextBlock).Text(FText::FromString("Dialogue character"))]
		.ValueContent().MinDesiredWidth(260)[
			SNew(SComboBox<TSharedPtr<FString>>).OptionsSource(&CharacterOptions)
			.OnGenerateWidget_Lambda([this](TSharedPtr<FString> Item) { return SNew(STextBlock).Text(FText::FromString(Item->IsEmpty() ? TEXT("All speakers") : CharacterLabels.FindRef(*Item))); })
			.OnSelectionChanged_Lambda([this, Utilities](TSharedPtr<FString> Item, ESelectInfo::Type) {
				if (Item && Component.IsValid()) { Subsystem()->SelectCharacter(Component.Get(), *Item); if (auto Pinned = Utilities.Pin()) Pinned->RequestRefresh(); }
			})
			[SNew(STextBlock).Text_Lambda([this] { return FText::FromString(!Component.IsValid() || Component->CharacterId.IsEmpty() ? TEXT("All speakers") : CharacterLabels.FindRef(Component->CharacterId)); })]];
		Category.AddCustomRow(FText::FromString("Bake Dialogue Voices")).WholeRowContent()[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 3, 5, 3)[SNew(SButton).Text(FText::FromString("Bake Dialogue Voices"))
				.IsEnabled_Lambda([this] { return Component.IsValid() && Subsystem()->GetStatus(Component.Get()).bCanBake; })
				.OnClicked_Lambda([this] { Subsystem()->Bake(Component.Get()); return FReply::Handled(); })]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 3)[SNew(SButton).Text(FText::FromString("Cancel Bake"))
				.IsEnabled_Lambda([this] { return Subsystem()->IsBaking(); })
				.OnClicked_Lambda([this] { Subsystem()->CancelBake(); return FReply::Handled(); })]];
		Category.AddCustomRow(FText::FromString("Refresh" )).WholeRowContent()[SNew(SButton).Text(FText::FromString("Refresh Setup Status"))
			.OnClicked_Lambda([this, Utilities] { Subsystem()->InvalidateStatusCache(); if (auto Pinned = Utilities.Pin()) Pinned->RequestRefresh(); return FReply::Handled(); })];
		Category.AddCustomRow(FText::FromString("Progress")).WholeRowContent()[SNew(STextBlock).AutoWrapText(true)
			.Text_Lambda([this] { return FText::FromString(Subsystem()->GetProgressText()); })];
		Category.AddCustomRow(FText::FromString("Automatic bake")).WholeRowContent()[
			SNew(SCheckBox)
			.IsChecked_Lambda([this] { return Component.IsValid() && Component->BakeLibrary && Component->BakeLibrary->bAutoBakeAfterSync ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { Subsystem()->SetAutoBake(Component.Get(), State == ECheckBoxState::Checked); })
			[SNew(STextBlock).Text(FText::FromString("Bake new and changed voices after sync"))]];
		Category.AddCustomRow(FText::FromString("Preview voice"))
		.NameContent()[SNew(STextBlock).Text(FText::FromString("Preview voice"))]
		.ValueContent().MinDesiredWidth(260)[
			SNew(SComboBox<TSharedPtr<TWeakObjectPtr<USoundWave>>>).OptionsSource(&VoiceOptions).InitiallySelectedItem(SelectedVoice)
			.OnGenerateWidget_Lambda([](TSharedPtr<TWeakObjectPtr<USoundWave>> Item) { return SNew(STextBlock).Text(FText::FromString(GetNameSafe(Item ? Item->Get() : nullptr))); })
			.OnSelectionChanged_Lambda([this](TSharedPtr<TWeakObjectPtr<USoundWave>> Item, ESelectInfo::Type) { SelectedVoice = Item; })
			[SNew(STextBlock).Text_Lambda([this] { return FText::FromString(SelectedVoice ? GetNameSafe(SelectedVoice->Get()) : TEXT("No dialogue voices")); })]];
		Category.AddCustomRow(FText::FromString("Preview")).WholeRowContent()[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 3, 5, 3)[SNew(SButton).Text(FText::FromString("Preview on Character"))
				.IsEnabled_Lambda([this] { return SelectedVoice.IsValid() && SelectedVoice->IsValid() && !Subsystem()->IsBaking() && !GEditor->PlayWorld; })
				.OnClicked_Lambda([this] { Subsystem()->Preview(Component.Get(), SelectedVoice ? SelectedVoice->Get() : nullptr); return FReply::Handled(); })]
			+ SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(FText::FromString("Stop Preview"))
				.OnClicked_Lambda([this] { Subsystem()->StopPreview(); return FReply::Handled(); })]];
	}
};

void RegisterStoryFlowMetaHumanDetails()
{
	FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor").RegisterCustomClassLayout(
		"StoryFlowMetaHumanLipsyncComponent", FOnGetDetailCustomizationInstance::CreateStatic(&FStoryFlowMetaHumanDetails::MakeInstance));
}

void UnregisterStoryFlowMetaHumanDetails()
{
	if (auto* Properties = FModuleManager::GetModulePtr<FPropertyEditorModule>("PropertyEditor"))
		Properties->UnregisterCustomClassLayout("StoryFlowMetaHumanLipsyncComponent");
}
