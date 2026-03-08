// Copyright 2026 StoryFlow. All Rights Reserved.

#include "WebSocket/StoryFlowWebSocketClient.h"
#include "StoryFlowRuntime.h"
#include "WebSocketsModule.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/EngineVersion.h"

FStoryFlowWebSocketClient::FStoryFlowWebSocketClient()
{
}

FStoryFlowWebSocketClient::~FStoryFlowWebSocketClient()
{
	Disconnect();
}

void FStoryFlowWebSocketClient::Connect(const FString& URL)
{
	LastURL = URL;
	ReconnectAttempts = 0;

	// Disconnect if already connected
	if (WebSocket.IsValid())
	{
		Disconnect();
	}

	// Create WebSocket
	WebSocket = FWebSocketsModule::Get().CreateWebSocket(URL, TEXT("ws"));
	if (!WebSocket.IsValid())
	{
		UE_LOG(LogStoryFlow, Error, TEXT("StoryFlow: Failed to create WebSocket"));
		return;
	}

	// Bind events (delegates are fresh since we create a new WebSocket each time via Disconnect())
	WebSocket->OnConnected().AddRaw(this, &FStoryFlowWebSocketClient::HandleConnected);
	WebSocket->OnConnectionError().AddRaw(this, &FStoryFlowWebSocketClient::HandleConnectionError);
	WebSocket->OnClosed().AddRaw(this, &FStoryFlowWebSocketClient::HandleClosed);
	WebSocket->OnMessage().AddRaw(this, &FStoryFlowWebSocketClient::HandleMessage);

	// Connect
	WebSocket->Connect();

	UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Connecting to %s"), *URL);
}

void FStoryFlowWebSocketClient::Disconnect()
{
	if (WebSocket.IsValid())
	{
		WebSocket->Close();
		WebSocket.Reset();
	}

	bIsConnected = false;
}

bool FStoryFlowWebSocketClient::IsConnected() const
{
	return bIsConnected && WebSocket.IsValid() && WebSocket->IsConnected();
}

void FStoryFlowWebSocketClient::SendMessage(const FString& Type, const TSharedPtr<FJsonObject>& Payload)
{
	if (!IsConnected())
	{
		return;
	}

	// Build message
	TSharedPtr<FJsonObject> Message = MakeShared<FJsonObject>();
	Message->SetStringField(TEXT("type"), Type);

	if (Payload.IsValid())
	{
		Message->SetObjectField(TEXT("payload"), Payload);
	}
	else
	{
		Message->SetObjectField(TEXT("payload"), MakeShared<FJsonObject>());
	}

	// Serialize to string
	FString MessageString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&MessageString);
	FJsonSerializer::Serialize(Message.ToSharedRef(), Writer);

	// Send
	WebSocket->Send(MessageString);
}

void FStoryFlowWebSocketClient::RequestSync()
{
	SendMessage(TEXT("request-sync"));
}

void FStoryFlowWebSocketClient::SendPing()
{
	SendMessage(TEXT("ping"));
}

void FStoryFlowWebSocketClient::HandleConnected()
{
	UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Connected to editor"));

	bIsConnected = true;
	ReconnectAttempts = 0;

	// Send handshake
	SendHandshake();

	// Broadcast connection state
	OnConnectionStateChanged.Broadcast(true);
}

void FStoryFlowWebSocketClient::HandleConnectionError(const FString& Error)
{
	UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Connection error: %s"), *Error);

	bIsConnected = false;
	OnConnectionStateChanged.Broadcast(false);

	// Notify that manual reconnection may be needed
	if (ReconnectAttempts < MaxReconnectAttempts && !LastURL.IsEmpty())
	{
		ReconnectAttempts++;
		UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Connection lost (attempt %d/%d). Use Connect() to reconnect."),
			   ReconnectAttempts, MaxReconnectAttempts);
	}
}

void FStoryFlowWebSocketClient::HandleClosed(int32 StatusCode, const FString& Reason, bool bWasClean)
{
	UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Connection closed (code: %d, reason: %s, clean: %s)"),
		   StatusCode, *Reason, bWasClean ? TEXT("yes") : TEXT("no"));

	bIsConnected = false;
	OnConnectionStateChanged.Broadcast(false);
}

void FStoryFlowWebSocketClient::HandleMessage(const FString& Message)
{
	// Parse JSON
	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Message);

	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Failed to parse message: %s"), *Message);
		return;
	}

	// Extract type and payload
	FString Type;
	if (!JsonObject->TryGetStringField(TEXT("type"), Type))
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Message missing 'type' field: %s"), *Message);
		return;
	}
	TSharedPtr<FJsonObject> Payload;

	if (JsonObject->HasField(TEXT("payload")))
	{
		Payload = JsonObject->GetObjectField(TEXT("payload"));
	}

	// Handle pong
	if (Type == TEXT("pong"))
	{
		// Keep-alive response, ignore
		return;
	}

	// Broadcast message
	OnMessageReceived.Broadcast(Type, Payload);
}

void FStoryFlowWebSocketClient::SendHandshake()
{
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("engine"), TEXT("unreal"));
	Payload->SetStringField(TEXT("version"), FEngineVersion::Current().ToString());
	Payload->SetStringField(TEXT("pluginVersion"), TEXT("1.0.3"));

	SendMessage(TEXT("connect"), Payload);
}
