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
