#!/usr/bin/env node

const fs = require('fs')
const path = require('path')
const readline = require('readline')

const PLUGIN_ROOT = __dirname

const FILES_TO_UPDATE = {
	uplugin: path.join(PLUGIN_ROOT, 'StoryFlowPlugin.uplugin'),
	storyFlowEditor: path.join(PLUGIN_ROOT, 'Source/StoryFlowEditor/Private/StoryFlowEditor.cpp'),
	webSocketClient: path.join(PLUGIN_ROOT, 'Source/StoryFlowEditor/Private/WebSocket/StoryFlowWebSocketClient.cpp'),
}

const rl = readline.createInterface({
	input: process.stdin,
	output: process.stdout,
})

function isValidVersion(version) {
	return /^\d+\.\d+\.\d+$/.test(version)
}

function getCurrentVersion() {
	try {
		const uplugin = JSON.parse(fs.readFileSync(FILES_TO_UPDATE.uplugin, 'utf8'))
		return { versionName: uplugin.VersionName, version: uplugin.Version }
	} catch (error) {
		console.error('Error reading current version:', error.message)
		process.exit(1)
	}
}

function updateUplugin(newVersion, newVersionInt) {
	const content = JSON.parse(fs.readFileSync(FILES_TO_UPDATE.uplugin, 'utf8'))
	content.VersionName = newVersion
	content.Version = newVersionInt
	fs.writeFileSync(FILES_TO_UPDATE.uplugin, JSON.stringify(content, null, '\t') + '\n')
	console.log('  Updated StoryFlowPlugin.uplugin')
}

function updateStoryFlowEditor(oldVersion, newVersion) {
	let content = fs.readFileSync(FILES_TO_UPDATE.storyFlowEditor, 'utf8')
	const regex = new RegExp(`#define STORYFLOW_VERSION "${escapeRegex(oldVersion)}"`)
	if (!regex.test(content)) {
		console.error('  Could not find STORYFLOW_VERSION in StoryFlowEditor.cpp')
		return
	}
	content = content.replace(regex, `#define STORYFLOW_VERSION "${newVersion}"`)
	fs.writeFileSync(FILES_TO_UPDATE.storyFlowEditor, content)
	console.log('  Updated StoryFlowEditor.cpp')
}

function updateWebSocketClient(oldVersion, newVersion) {
	let content = fs.readFileSync(FILES_TO_UPDATE.webSocketClient, 'utf8')
	const regex = new RegExp(`TEXT\\("${escapeRegex(oldVersion)}"\\)`)
	if (!regex.test(content)) {
		console.error('  Could not find pluginVersion in StoryFlowWebSocketClient.cpp')
		return
	}
	content = content.replace(regex, `TEXT("${newVersion}")`)
	fs.writeFileSync(FILES_TO_UPDATE.webSocketClient, content)
	console.log('  Updated StoryFlowWebSocketClient.cpp')
}

function escapeRegex(str) {
	return str.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')
}

function verifyFiles() {
	const missing = Object.entries(FILES_TO_UPDATE)
		.filter(([, filePath]) => !fs.existsSync(filePath))
		.map(([, filePath]) => filePath)

	if (missing.length > 0) {
		console.error('Missing files:')
		missing.forEach((f) => console.error(`  ${f}`))
		process.exit(1)
	}
}

function main() {
	console.log('StoryFlow Plugin Version Updater\n')

	verifyFiles()

	const { versionName: currentVersion, version: currentVersionInt } = getCurrentVersion()
	console.log(`Current version: ${currentVersion} (internal: ${currentVersionInt})`)

	rl.question(`\nEnter new version (e.g. 1.0.2): `, (input) => {
		const newVersion = input.trim()

		if (!newVersion) {
			console.log('Version cannot be empty')
			rl.close()
			return
		}

		if (!isValidVersion(newVersion)) {
			console.log('Invalid version format. Use semantic versioning (e.g. 1.0.2)')
			rl.close()
			return
		}

		if (newVersion === currentVersion) {
			console.log('New version is the same as current version')
			rl.close()
			return
		}

		const newVersionInt = currentVersionInt + 1

		console.log(`\nUpdating: ${currentVersion} -> ${newVersion} (internal: ${currentVersionInt} -> ${newVersionInt})\n`)

		try {
			updateUplugin(newVersion, newVersionInt)
			updateStoryFlowEditor(currentVersion, newVersion)
			updateWebSocketClient(currentVersion, newVersion)

			console.log(`\nVersion updated to ${newVersion}`)
			console.log('\nFiles changed:')
			console.log('  - StoryFlowPlugin.uplugin')
			console.log('  - Source/StoryFlowEditor/Private/StoryFlowEditor.cpp')
			console.log('  - Source/StoryFlowEditor/Private/WebSocket/StoryFlowWebSocketClient.cpp')
		} catch (error) {
			console.error('\nFailed to update version:', error.message)
			process.exit(1)
		}

		rl.close()
	})
}

rl.on('close', () => process.exit(0))

main().catch((error) => {
	console.error('Unexpected error:', error)
	process.exit(1)
})
