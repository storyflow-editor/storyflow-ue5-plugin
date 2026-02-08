// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IWebSocket.h"

DECLARE_MULTICAST_DELEGATE_TwoParams(FOnStoryFlowMessageReceived, const FString& /* Type */, TSharedPtr<FJsonObject> /* Payload */);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnStoryFlowConnectionStateChanged, bool /* bConnected */);

/**
 * WebSocket client for connecting to StoryFlow Editor
 */
class STORYFLOWEDITOR_API FStoryFlowWebSocketClient : public TSharedFromThis<FStoryFlowWebSocketClient>
{
public:
	FStoryFlowWebSocketClient();
	~FStoryFlowWebSocketClient();

	/**
	 * Connect to StoryFlow Editor
	 * @param URL WebSocket URL (e.g., "ws://localhost:9000")
	 */
	void Connect(const FString& URL);

	/**
	 * Disconnect from StoryFlow Editor
	 */
	void Disconnect();

	/**
	 * Check if connected
	 */
	bool IsConnected() const;

	/**
	 * Send a message to StoryFlow Editor
	 * @param Type Message type
	 * @param Payload Message payload
	 */
	void SendMessage(const FString& Type, TSharedPtr<FJsonObject> Payload = nullptr);

	/**
	 * Request a full project sync
	 */
	void RequestSync();

	/**
	 * Send a ping to keep the connection alive
	 */
	void SendPing();

	// Events
	FOnStoryFlowMessageReceived OnMessageReceived;
	FOnStoryFlowConnectionStateChanged OnConnectionStateChanged;

private:
	/** Handle WebSocket connection */
	void HandleConnected();

	/** Handle WebSocket connection error */
	void HandleConnectionError(const FString& Error);

	/** Handle WebSocket close */
	void HandleClosed(int32 StatusCode, const FString& Reason, bool bWasClean);

	/** Handle incoming message */
	void HandleMessage(const FString& Message);

	/** Send handshake message */
	void SendHandshake();

	/** WebSocket instance */
	TSharedPtr<IWebSocket> WebSocket;

	/** Connection state */
	bool bIsConnected = false;

	/** URL for reconnection */
	FString LastURL;

	/** Reconnect attempt count */
	int32 ReconnectAttempts = 0;

	/** Max reconnect attempts */
	static constexpr int32 MaxReconnectAttempts = 5;

	/** Base reconnect delay in seconds */
	static constexpr float BaseReconnectDelay = 1.0f;
};
