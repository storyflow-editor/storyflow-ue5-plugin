// Copyright 2026 StoryFlow. All Rights Reserved.

#include "StoryFlowEditor.h"
#include "StoryFlowRuntime.h"
#include "StoryFlowEditorSettings.h"
#include "Subsystems/StoryFlowEditorSubsystem.h"
#include "ToolMenus.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Colors/SColorBlock.h"
#include "Widgets/Layout/SSeparator.h"
#include "Styling/SlateStyleRegistry.h"
#include "Styling/SlateTypes.h"
#include "Styling/AppStyle.h"
#include "Brushes/SlateImageBrush.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Interfaces/IPluginManager.h"
#include "HAL/PlatformProcess.h"
#include "DesktopPlatformModule.h"
#include "Editor.h"

#define LOCTEXT_NAMESPACE "FStoryFlowEditorModule"

#define STORYFLOW_STYLE_NAME "StoryFlowEditorStyle"
#define STORYFLOW_VERSION "1.0.0"
#define STORYFLOW_URL_EDITOR_DOCS "https://storyflow-editor.com/docs"
#define STORYFLOW_URL_PLUGIN_DOCS "https://storyflow-editor.com/integrations/unreal-engine/docs"
#define STORYFLOW_URL_CHANGELOG "https://storyflow-editor.com/changelog"
#define STORYFLOW_URL_DISCORD "https://discord.com/invite/3mp5vyKRtN"

void FStoryFlowEditorModule::StartupModule()
{
	UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Editor module loaded"));

	// Create and register the style set
	StyleSet = MakeShareable(new FSlateStyleSet(STORYFLOW_STYLE_NAME));

	// Get the plugin's Resources directory for icons
	FString PluginBaseDir = IPluginManager::Get().FindPlugin(TEXT("StoryFlowPlugin"))->GetBaseDir();
	FString ResourcesDir = PluginBaseDir / TEXT("Resources");
	StyleSet->SetContentRoot(ResourcesDir);

	// Register the toolbar icons at their native sizes for crisp rendering
	StyleSet->Set("StoryFlow.ToolbarIcon", new FSlateImageBrush(ResourcesDir / TEXT("Icon40.png"), FVector2D(40.0f, 40.0f)));
	StyleSet->Set("StoryFlow.ToolbarIcon.Small", new FSlateImageBrush(ResourcesDir / TEXT("Icon20.png"), FVector2D(20.0f, 20.0f)));

	// Circle brush for status indicators (tinted at runtime via ColorAndOpacity)
	{
		auto* StatusDot = new FSlateRoundedBoxBrush(FLinearColor::White, 4.0f);
		StatusDot->ImageSize = FVector2D(8.0f, 8.0f);
		StyleSet->Set("StoryFlow.StatusDot", StatusDot);
	}

	// Register custom button styles for the unified combo toolbar button
	{
		const FLinearColor NormalBg(1.0f, 1.0f, 1.0f, 0.04f);
		const FLinearColor HoverBg(1.0f, 1.0f, 1.0f, 0.10f);
		const FLinearColor PressBg(1.0f, 1.0f, 1.0f, 0.06f);
		const float Radius = 4.0f;

		// Left button style (sync button - rounded left corners only)
		{
			FButtonStyle Style;
			Style.SetNormal(FSlateRoundedBoxBrush(NormalBg, Radius));
			Style.Normal.OutlineSettings.CornerRadii = FVector4(Radius, 0.0f, 0.0f, Radius);
			Style.SetHovered(FSlateRoundedBoxBrush(HoverBg, Radius));
			Style.Hovered.OutlineSettings.CornerRadii = FVector4(Radius, 0.0f, 0.0f, Radius);
			Style.SetPressed(FSlateRoundedBoxBrush(PressBg, Radius));
			Style.Pressed.OutlineSettings.CornerRadii = FVector4(Radius, 0.0f, 0.0f, Radius);
			Style.NormalPadding = FMargin(5.0f, 2.0f);
			Style.PressedPadding = FMargin(5.0f, 2.0f);
			StyleSet->Set("StoryFlow.SyncButton", Style);
		}

		// Right button style (dropdown arrow - rounded right corners only)
		{
			FButtonStyle Style;
			Style.SetNormal(FSlateRoundedBoxBrush(NormalBg, Radius));
			Style.Normal.OutlineSettings.CornerRadii = FVector4(0.0f, Radius, Radius, 0.0f);
			Style.SetHovered(FSlateRoundedBoxBrush(HoverBg, Radius));
			Style.Hovered.OutlineSettings.CornerRadii = FVector4(0.0f, Radius, Radius, 0.0f);
			Style.SetPressed(FSlateRoundedBoxBrush(PressBg, Radius));
			Style.Pressed.OutlineSettings.CornerRadii = FVector4(0.0f, Radius, Radius, 0.0f);
			Style.NormalPadding = FMargin(2.0f, 2.0f);
			Style.PressedPadding = FMargin(2.0f, 2.0f);
			StyleSet->Set("StoryFlow.DropdownButton", Style);
		}
	}

	FSlateStyleRegistry::RegisterSlateStyle(*StyleSet);

	// Register toolbar extension after ToolMenus is ready
	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FStoryFlowEditorModule::RegisterToolbarExtension));
}

void FStoryFlowEditorModule::ShutdownModule()
{
	UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Editor module unloaded"));

	UnregisterToolbarExtension();

	// Unregister the style set
	if (StyleSet.IsValid())
	{
		FSlateStyleRegistry::UnRegisterSlateStyle(*StyleSet);
		StyleSet.Reset();
	}
}

static UStoryFlowEditorSubsystem* GetEditorSubsystem()
{
	if (GEditor)
	{
		return GEditor->GetEditorSubsystem<UStoryFlowEditorSubsystem>();
	}
	return nullptr;
}

void FStoryFlowEditorModule::RegisterToolbarExtension()
{
	UToolMenu* ToolbarMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar.PlayToolBar");
	if (!ToolbarMenu)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Could not find LevelEditor toolbar to extend"));
		return;
	}

	FToolMenuSection& Section = ToolbarMenu->FindOrAddSection("StoryFlow");

	Section.AddEntry(FToolMenuEntry::InitWidget(
		"StoryFlowMenu",
		SNew(SHorizontalBox)
		// Main button (Connect when disconnected, Sync when connected)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(0)
		[
			SNew(SButton)
			.ButtonStyle(StyleSet.Get(), "StoryFlow.SyncButton")
			.ToolTipText(LOCTEXT("StoryFlowBtn_Tooltip", "Connect to or sync with StoryFlow Editor"))
			.OnClicked_Lambda([]() -> FReply
			{
				UStoryFlowEditorSubsystem* Subsystem = GetEditorSubsystem();
				if (Subsystem)
				{
					if (Subsystem->IsConnected())
					{
						Subsystem->RequestSync();
					}
					else
					{
						UStoryFlowEditorSettings* Settings = GetMutableDefault<UStoryFlowEditorSettings>();
						Subsystem->ConnectToStoryFlow(TEXT("localhost"), Settings->Port);
					}
				}
				return FReply::Handled();
			})
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 4.0f, 0.0f)
				[
					SNew(SImage)
					.Image(FSlateIcon(STORYFLOW_STYLE_NAME, "StoryFlow.ToolbarIcon.Small").GetIcon())
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text_Lambda([]() -> FText
					{
						UStoryFlowEditorSubsystem* Subsystem = GetEditorSubsystem();
						bool bConnected = Subsystem && Subsystem->IsConnected();
						return bConnected
							? LOCTEXT("StoryFlowBtn_Sync", "Sync")
							: LOCTEXT("StoryFlowBtn_Connect", "Connect");
					})
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(4.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SImage)
					.Image(StyleSet->GetBrush("StoryFlow.StatusDot"))
					.ColorAndOpacity(FLinearColor::Green)
					.Visibility_Lambda([]() -> EVisibility
					{
						UStoryFlowEditorSubsystem* Subsystem = GetEditorSubsystem();
						bool bConnected = Subsystem && Subsystem->IsConnected();
						return bConnected ? EVisibility::Visible : EVisibility::Collapsed;
					})
				]
			]
		]
		// Dropdown arrow button (rounded right corners)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(0)
		[
			SNew(SComboButton)
			.ButtonStyle(&StyleSet->GetWidgetStyle<FButtonStyle>("StoryFlow.DropdownButton"))
			.HasDownArrow(true)
			.ContentPadding(FMargin(1.0f, 0.0f))
			.ToolTipText(LOCTEXT("StoryFlowSettings_Tooltip", "StoryFlow settings"))
			.OnGetMenuContent(FOnGetContent::CreateRaw(this, &FStoryFlowEditorModule::GenerateToolbarMenu))
		],
		FText::GetEmpty()
	));

	UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Toolbar extension registered"));
}

void FStoryFlowEditorModule::UnregisterToolbarExtension()
{
	UToolMenus::UnregisterOwner(this);
}

TSharedRef<SWidget> FStoryFlowEditorModule::GenerateToolbarMenu()
{
	const float ButtonPadding = 2.0f;
	const FMargin ContentPadding(8.0f, 4.0f);

	return SNew(SVerticalBox)
		// Version header
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(ContentPadding)
		[
			SNew(STextBlock)
			.Text(FText::FromString(FString::Printf(TEXT("StoryFlow v%s"), TEXT(STORYFLOW_VERSION))))
			.Font(FAppStyle::GetFontStyle("BoldFont"))
		]

		// Separator
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.0f, 2.0f)
		[
			SNew(SSeparator)
		]

		// Connection status row
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8.0f, 4.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				// Status dot (circle)
				SNew(SImage)
				.Image(StyleSet->GetBrush("StoryFlow.StatusDot"))
				.ColorAndOpacity_Lambda([]() -> FSlateColor
				{
					UStoryFlowEditorSubsystem* Subsystem = GetEditorSubsystem();
					bool bConnected = Subsystem && Subsystem->IsConnected();
					return bConnected ? FSlateColor(FLinearColor::Green) : FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f));
				})
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text_Lambda([]() -> FText
				{
					UStoryFlowEditorSubsystem* Subsystem = GetEditorSubsystem();
					bool bConnected = Subsystem && Subsystem->IsConnected();
					return bConnected
						? LOCTEXT("StoryFlowMenu_Connected", "Connected to Editor")
						: LOCTEXT("StoryFlowMenu_Disconnected", "Not Connected");
				})
				.ColorAndOpacity_Lambda([]() -> FSlateColor
				{
					UStoryFlowEditorSubsystem* Subsystem = GetEditorSubsystem();
					bool bConnected = Subsystem && Subsystem->IsConnected();
					return bConnected
						? FSlateColor(FLinearColor::Green)
						: FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f));
				})
			]
		]

		// Port row
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8.0f, 2.0f, 8.0f, 4.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("StoryFlowMenu_Port", "Port"))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(SEditableTextBox)
				.Text_Lambda([]() -> FText
				{
					return FText::AsNumber(GetMutableDefault<UStoryFlowEditorSettings>()->Port, &FNumberFormattingOptions::DefaultNoGrouping());
				})
				.OnTextCommitted_Lambda([](const FText& NewText, ETextCommit::Type CommitType)
				{
					int32 NewValue = FCString::Atoi(*NewText.ToString());
					int32 Clamped = FMath::Clamp(NewValue, 1024, 65535);
					UStoryFlowEditorSettings* Settings = GetMutableDefault<UStoryFlowEditorSettings>();
					Settings->Port = Clamped;
					Settings->SaveConfig();

					// Disconnect if currently connected so next Connect uses the new port
					UStoryFlowEditorSubsystem* Subsystem = GetEditorSubsystem();
					if (Subsystem && Subsystem->IsConnected())
					{
						Subsystem->Disconnect();
					}
				})
			]
		]

		// Connect / Disconnect button
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(ButtonPadding)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ContentPadding(ContentPadding)
			.OnClicked_Lambda([]()
			{
				UStoryFlowEditorSubsystem* Subsystem = GetEditorSubsystem();
				if (Subsystem)
				{
					if (Subsystem->IsConnected())
					{
						Subsystem->Disconnect();
					}
					else
					{
						UStoryFlowEditorSettings* Settings = GetMutableDefault<UStoryFlowEditorSettings>();
						Subsystem->ConnectToStoryFlow(TEXT("localhost"), Settings->Port);
					}
				}
				return FReply::Handled();
			})
			[
				SNew(STextBlock)
				.Text_Lambda([]() -> FText
				{
					UStoryFlowEditorSubsystem* Subsystem = GetEditorSubsystem();
					bool bConnected = Subsystem && Subsystem->IsConnected();
					return bConnected
						? LOCTEXT("StoryFlowMenu_Disconnect", "Disconnect")
						: LOCTEXT("StoryFlowMenu_Connect", "Connect to Editor");
				})
			]
		]

		// Import from Folder button
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(ButtonPadding)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ContentPadding(ContentPadding)
			.OnClicked_Lambda([]()
			{
				IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
				if (DesktopPlatform)
				{
					const void* ParentHandle = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);

					FString SelectedPath;
					bool bOpened = DesktopPlatform->OpenDirectoryDialog(
						ParentHandle,
						TEXT("Select StoryFlow Build Folder"),
						FPaths::ProjectDir(),
						SelectedPath
					);

					if (bOpened && !SelectedPath.IsEmpty())
					{
						UStoryFlowEditorSubsystem* Subsystem = GetEditorSubsystem();
						if (Subsystem)
						{
							Subsystem->ImportProject(SelectedPath);
						}
					}
				}
				return FReply::Handled();
			})
			[
				SNew(STextBlock)
				.Text(LOCTEXT("StoryFlowMenu_ImportFolder", "Import from Folder..."))
			]
		]

		// Separator
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.0f, 2.0f)
		[
			SNew(SSeparator)
		]

		// Editor Documentation button
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(ButtonPadding)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ContentPadding(ContentPadding)
			.OnClicked_Lambda([]()
			{
				FPlatformProcess::LaunchURL(TEXT(STORYFLOW_URL_EDITOR_DOCS), nullptr, nullptr);
				return FReply::Handled();
			})
			[
				SNew(STextBlock)
				.Text(LOCTEXT("StoryFlowMenu_EditorDocs", "Editor Documentation"))
			]
		]

		// Plugin Documentation button
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(ButtonPadding)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ContentPadding(ContentPadding)
			.OnClicked_Lambda([]()
			{
				FPlatformProcess::LaunchURL(TEXT(STORYFLOW_URL_PLUGIN_DOCS), nullptr, nullptr);
				return FReply::Handled();
			})
			[
				SNew(STextBlock)
				.Text(LOCTEXT("StoryFlowMenu_PluginDocs", "Plugin Documentation"))
			]
		]

		// Changelog button
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(ButtonPadding)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ContentPadding(ContentPadding)
			.OnClicked_Lambda([]()
			{
				FPlatformProcess::LaunchURL(TEXT(STORYFLOW_URL_CHANGELOG), nullptr, nullptr);
				return FReply::Handled();
			})
			[
				SNew(STextBlock)
				.Text(LOCTEXT("StoryFlowMenu_Changelog", "Changelog"))
			]
		]

		// Discord button
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(ButtonPadding)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ContentPadding(ContentPadding)
			.OnClicked_Lambda([]()
			{
				FPlatformProcess::LaunchURL(TEXT(STORYFLOW_URL_DISCORD), nullptr, nullptr);
				return FReply::Handled();
			})
			[
				SNew(STextBlock)
				.Text(LOCTEXT("StoryFlowMenu_Discord", "Discord"))
			]
		];
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FStoryFlowEditorModule, StoryFlowEditor)
