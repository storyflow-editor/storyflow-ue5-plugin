# StoryFlow Plugin for Unreal Engine

## Updating Version

Requires Node.js. From the plugin root directory:

```bash
node update-version.js
```

This updates the version string in all relevant files:

- `StoryFlowPlugin.uplugin` — `VersionName` and `Version` (integer, auto-incremented)
- `Source/StoryFlowEditor/Private/StoryFlowEditor.cpp` — `STORYFLOW_VERSION` define
- `Source/StoryFlowEditor/Private/WebSocket/StoryFlowWebSocketClient.cpp` — `pluginVersion` in WebSocket handshake

## Building Plugin

From the plugin root directory, run:

```
build_plugin.bat
```

Builds the plugin for UE 5.3–5.7 and outputs to `storyflow-unreal/Builds/`. Expects Unreal Engine installations at `C:\Program Files\Epic Games\UE_5.x`.
