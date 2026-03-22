# StoryFlow for Unreal Engine

Runtime plugin for [StoryFlow Editor](https://storyflow-editor.com) - a visual node editor for creating interactive stories and dialogue systems.

## Features

- 160+ node types - dialogue, branching, variables, arrays, characters, audio, images
- Full Blueprint and C++ API for dialogue control
- Built-in dialogue widget with auto-fallback when no custom UI assigned
- Live text interpolation - `{varname}`, `{Character.Name}`
- RunScript / RunFlow - nested scripts with parameters, outputs, and exit flows
- ForEach loops across all array types
- Audio advance-on-end with optional skip, 3D spatial audio support
- Character variables with built-in Name/Image field support
- Save/Load with slot-based persistence
- WebSocket Live Sync with auto-reconnect

## Requirements

- Unreal Engine 5.3 - 5.7
- StoryFlow Editor (for creating and exporting projects)

## Installation

1. Download the plugin for your UE version from [Releases](https://github.com/storyflow-editor/storyflow-ue5-plugin/releases)
2. Copy the `StoryFlowPlugin` folder into your project's `Plugins/` directory
3. Enable the plugin in **Edit > Plugins** and restart the editor

## Quick Start

1. **Import your project** - go to **StoryFlow > Import Project**, select your exported `build/` folder
2. **Add a StoryFlowComponent** to any Actor
3. **Call `StartDialogue()`** from Blueprint or C++:

```cpp
UStoryFlowComponent* StoryFlow = FindComponentByClass<UStoryFlowComponent>();
StoryFlow->StartDialogue();
```

A built-in dialogue widget appears automatically. No manager setup needed.

## Live Sync

Connect to the StoryFlow Editor for real-time updates during development:

1. Open **StoryFlow > Live Sync** in the editor
2. Click **Connect** (default port: 9000)
3. Click **Sync** - or sync from the editor

Changes sync automatically when you save in the editor.

## Customizing the UI

The built-in widget is for prototyping. For production:

- **Create a custom widget** implementing dialogue display logic
- **Bind to delegates** - `OnDialogueUpdated`, `OnDialogueEnded`, etc.

```cpp
StoryFlow->OnDialogueUpdated.AddDynamic(this, &AMyActor::HandleDialogueUpdated);

void AMyActor::HandleDialogueUpdated(const FStoryFlowDialogueState& State)
{
    // State.Text, State.Options, State.Character, etc.
}
```

## Save & Load

```cpp
UStoryFlowSubsystem* Subsystem = GetGameInstance()->GetSubsystem<UStoryFlowSubsystem>();
Subsystem->SaveToSlot("Slot1");
Subsystem->LoadFromSlot("Slot1");
```

## Documentation

Full documentation at [storyflow-editor.com/integrations/unreal-engine](https://storyflow-editor.com/integrations/unreal-engine).

## Contributing

Contributions are welcome! Please read the guidelines below before submitting.

### Branch Structure

- **`main`** - latest stable release. This is what users install.
- **`dev`** - active development. All changes go here first.

### How to Contribute

1. Fork this repository
2. Create a feature branch from `dev` (`git checkout -b my-feature dev`)
3. Make your changes and commit
4. Open a Pull Request targeting the `dev` branch
5. We'll review and merge when ready

Please open an [issue](https://github.com/storyflow-editor/storyflow-ue5-plugin/issues) first for large changes so we can discuss the approach.

### Building from Source

Requires Node.js for the version update script and Unreal Engine installed locally.

```bash
# Update version across all source files
node update-version.js

# Build for all UE versions (from storyflow-unreal root)
build_plugin.bat
```

## Changelog

See the full version history at [storyflow-editor.com/integrations/unreal-engine/changelog](https://storyflow-editor.com/integrations/unreal-engine/changelog/).

## License

[MIT](LICENSE)
