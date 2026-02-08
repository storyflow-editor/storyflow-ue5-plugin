// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Styling/SlateStyle.h"

class SWidget;

class FStoryFlowEditorModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	/** Registers the toolbar extension */
	void RegisterToolbarExtension();

	/** Unregisters the toolbar extension */
	void UnregisterToolbarExtension();

	/** Creates the StoryFlow dropdown menu content */
	TSharedRef<SWidget> GenerateToolbarMenu();

	/** Style set for StoryFlow icons */
	TSharedPtr<FSlateStyleSet> StyleSet;
};
