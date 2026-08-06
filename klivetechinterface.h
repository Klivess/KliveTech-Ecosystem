#ifndef KLIVETECHINTERFACE_H
#define KLIVETECHINTERFACE_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <BluetoothSerial.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <esp_arduino_version.h>
#include <esp_now.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <mbedtls/md.h>
#include <atomic>
#include <functional>
#include <vector>

enum ActionParameterType
{
    Integer,
    StringParameter,
    Bool,
    None
};

enum OperationNumber
{
    ExecuteAction,
    GetActions,
    Ping,
    BeginFirmwareUpdate,
    FirmwareUpdateChunk,
    CompleteFirmwareUpdate,
    AbortFirmwareUpdate,
    GetStreamables,
    ConfigureStreamable
};

enum class KliveTechStreamMode : uint8_t
{
    Periodic,
    OnChange,
    Manual
};

struct KliveTechHubConfig
{
    const char *wifiSSID = nullptr;
    const char *wifiPassword = nullptr;
    const char *omnipotentHost = nullptr;
    uint16_t omnipotentPort = 443;
    const char *omnipotentPath = "/klivetech/hub/connect";
    const char *accessToken = nullptr;
    const char *relayNetworkKey = nullptr;
    const char *caCertificate = nullptr;
    bool useTLS = true;
};

class KliveTech
{
public:
    BluetoothSerial SerialBT;

    // Creates a normal gadget. When SetRelayNetworkKey has been called first,
    // it also discovers and joins a nearby KliveTech hub automatically.
    bool CreateKliveTechGadget(const char *name);

    // Creates a gadget which is also a Wi-Fi/ESP-NOW relay hub.
    bool CreateKliveTechHub(const char *name, const KliveTechHubConfig &config);

    // Convenience overload for sketches which do not need a custom path/port.
    bool CreateKliveTechHub(
        const char *name,
        const char *wifiSSID,
        const char *wifiPassword,
        const char *omnipotentHost,
        const char *accessToken,
        const char *relayNetworkKey,
        const char *caCertificate = nullptr,
        uint16_t omnipotentPort = 443,
        bool useTLS = true);

    // Call this before CreateKliveTechGadget. Gadgets with the same non-empty
    // key discover each other without exposing commands to unrelated devices.
    bool SetRelayNetworkKey(const char *relayNetworkKey);

    void CallLoop();

    void CreateActionWithIntegerParam(const char *actionName, std::function<void(int)> function, const char *paramDescription);
    void CreateActionWithStringParam(const char *actionName, std::function<void(const char *)> function, const char *paramDescription);
    void CreateActionWithBoolParam(const char *actionName, std::function<void(bool)> function, const char *paramDescription);
    void CreateActionWithNoParam(const char *actionName, std::function<void()> function);

    // Streamable getters run on KliveTech's background task. Keep them quick,
    // non-blocking, and safe to call concurrently with the sketch loop.
    bool CreateIntegerStreamable(
        const char *streamID,
        std::function<int64_t()> getter,
        unsigned long updateIntervalMs = 1000,
        KliveTechStreamMode mode = KliveTechStreamMode::OnChange);
    bool CreateNumberStreamable(
        const char *streamID,
        std::function<double()> getter,
        unsigned long updateIntervalMs = 1000,
        KliveTechStreamMode mode = KliveTechStreamMode::OnChange);
    bool CreateBoolStreamable(
        const char *streamID,
        std::function<bool()> getter,
        unsigned long updateIntervalMs = 250,
        KliveTechStreamMode mode = KliveTechStreamMode::OnChange);
    bool CreateStringStreamable(
        const char *streamID,
        std::function<String()> getter,
        unsigned long updateIntervalMs = 1000,
        KliveTechStreamMode mode = KliveTechStreamMode::OnChange);
    bool CreateJsonStreamable(
        const char *streamID,
        std::function<String()> jsonGetter,
        unsigned long updateIntervalMs = 1000,
        KliveTechStreamMode mode = KliveTechStreamMode::OnChange);

    // Binary streamables are manually published as independent frames. JPEG
    // camera frames should use image/jpeg; arbitrary bytes may use
    // application/octet-stream.
    bool CreateBinaryStreamable(
        const char *streamID,
        const char *mimeType = "application/octet-stream");
    bool PublishIntegerStreamable(const char *streamID, int64_t value);
    bool PublishNumberStreamable(const char *streamID, double value);
    bool PublishBoolStreamable(const char *streamID, bool value);
    bool PublishStringStreamable(const char *streamID, const String &value);
    bool PublishJsonStreamable(const char *streamID, const String &serializedJson);
    bool PublishBinaryStreamable(const char *streamID, const uint8_t *data, size_t length);
    size_t GetStreamableCount() const;
    size_t GetDroppedStreamMessageCount() const;

    bool IsHub() const;
    bool IsConnectedToHub() const;
    bool IsOmnipotentConnected() const;
    size_t GetConnectedGadgetCount() const;
    const char *GetDeviceID() const;

private:
    static constexpr uint32_t RelayMagic = 0x4B544832; // "KTH2"
    static constexpr uint8_t RelayProtocolVersion = 2;
    static constexpr size_t RelayPayloadSize = 184;
    static constexpr size_t MaximumRelayMessageSize = 8192;
    static constexpr size_t MaximumFirmwareChunkSize = 2048;
    static constexpr uint8_t StreamProtocolVersion = 1;
    static constexpr size_t MaximumStreamables = 32;
    static constexpr size_t MaximumStreamIDLength = 48;
    static constexpr size_t MaximumStreamJsonValueSize = 4096;
    static constexpr size_t StreamBinaryChunkSize = 1024;
    static constexpr size_t MaximumStreamBinaryFrameSize = 512 * 1024;
    static constexpr size_t MaximumStreamMimeTypeLength = 96;
    static constexpr size_t StreamManifestEnvelopeAllowance = 192;
    static constexpr size_t StreamMessageQueueLength = 8;
    static constexpr unsigned long MinimumStreamIntervalMs = 25;
    static constexpr unsigned long StreamDefinitionIntervalMs = 30000;
    static constexpr size_t MaximumRelayPeers = 16;
    static constexpr unsigned long HubBeaconIntervalMs = 2000;
    static constexpr unsigned long LeafRegistrationIntervalMs = 5000;
    static constexpr unsigned long PeerTimeoutMs = 15000;

    enum class DeviceMode : uint8_t
    {
        Uninitialized,
        Gadget,
        Hub
    };

    enum class RelayPacketKind : uint8_t
    {
        Beacon = 1,
        Registration = 2,
        Data = 3
    };

    enum class ResponseTransport : uint8_t
    {
        Bluetooth,
        Relay,
        HubWebSocket
    };

    struct RegisteredAction
    {
        ActionParameterType type = None;
        String name;
        String paramDescription;
        std::function<void(int)> intFunction;
        std::function<void(const char *)> stringFunction;
        std::function<void(bool)> boolFunction;
        std::function<void()> noParamFunction;
    };

    struct ActionInvocation
    {
        RegisteredAction action;
        int integerValue = 0;
        bool boolValue = false;
        String stringValue;
    };

    struct RegisteredStreamable
    {
        String streamID;
        String valueType;
        String mimeType;
        KliveTechStreamMode mode = KliveTechStreamMode::Manual;
        unsigned long updateIntervalMs = 0;
        unsigned long lastChecked = 0;
        unsigned long lastDefinition = 0;
        uint64_t nextSequence = 0;
        bool definitionDirty = true;
        bool enabled = true;
        bool hasPublished = false;
        String lastSerializedValue;
        std::function<String()> jsonGetter;
    };

    struct PendingBinaryFrame
    {
        String streamID;
        String mimeType;
        String frameSha256;
        uint8_t *data = nullptr;
        size_t length = 0;
        size_t offset = 0;
        size_t chunkIndex = 0;
        size_t chunkCount = 0;
        uint64_t sequence = 0;
        unsigned long timestampMs = 0;
    };

    struct __attribute__((packed)) RelayPacket
    {
        uint32_t magic;
        uint8_t version;
        uint8_t kind;
        uint16_t payloadLength;
        uint32_t messageID;
        uint16_t fragmentIndex;
        uint16_t fragmentCount;
        uint8_t senderID[6];
        char senderName[32];
        uint32_t networkHash;
        uint8_t authenticationTag[8];
        uint8_t payload[RelayPayloadSize];
    };

    static_assert(sizeof(RelayPacket) <= ESP_NOW_MAX_DATA_LEN, "KliveTech relay packets must fit in ESP-NOW v1 frames");

    struct QueuedRelayPacket
    {
        uint8_t sourceAddress[6];
        RelayPacket packet;
    };

    struct RelayPeer
    {
        uint8_t radioAddress[6];
        String deviceID;
        String name;
        unsigned long lastSeen = 0;
    };

    struct RelayAssembly
    {
        uint8_t sourceAddress[6];
        uint32_t messageID = 0;
        unsigned long lastUpdated = 0;
        std::vector<String> fragments;
        size_t receivedFragments = 0;
    };

    std::vector<RegisteredAction> possibleActions;
    std::vector<RegisteredStreamable> streamables;
    std::vector<RelayPeer> relayPeers;
    std::vector<RelayAssembly> relayAssemblies;

    DeviceMode mode = DeviceMode::Uninitialized;
    String gadgetName;
    String deviceID;
    String bluetoothBuffer;
    String relayNetworkKey;
    String wifiSSID;
    String wifiPassword;
    String omnipotentHost;
    String omnipotentPath = "/klivetech/hub/connect";
    String accessToken;
    String caCertificate;
    uint16_t omnipotentPort = 443;
    bool useTLS = true;
    bool relayRadioStarted = false;
    bool bluetoothStarted = false;
    volatile bool connectedToHub = false;
    volatile bool omnipotentConnected = false;
    volatile size_t connectedGadgetCount = 0;
    uint8_t currentHubAddress[6] = {0};
    uint8_t currentRelayChannel = 1;
    uint32_t nextRelayMessageID = 0;
    unsigned long lastHubBeacon = 0;
    unsigned long lastRegistration = 0;
    unsigned long lastHubSeen = 0;
    unsigned long lastChannelHop = 0;
    QueueHandle_t relayReceiveQueue = nullptr;
    QueueHandle_t streamMessageQueue = nullptr;
    SemaphoreHandle_t actionMutex = nullptr;
    SemaphoreHandle_t streamableMutex = nullptr;
    TaskHandle_t callLoopTask = nullptr;
    WebSocketsClient *webSocket = nullptr;
    bool firmwareUpdateActive = false;
    bool firmwareHashInitialized = false;
    size_t firmwareUpdateSize = 0;
    size_t firmwareBytesWritten = 0;
    String expectedFirmwareSha256;
    mbedtls_md_context_t firmwareHashContext = {};
    unsigned long firmwareRestartAt = 0;
    String streamSessionID;
    uint32_t streamManifestRevision = 1;
    size_t nextStreamableIndex = 0;
    unsigned long lastStreamManifest = 0;
    PendingBinaryFrame *pendingBinaryFrame = nullptr;
    std::atomic<size_t> droppedStreamMessageCount{0};
    bool preferBinaryStreamChunk = false;
    bool streamTransportPreviouslyAvailable = false;

    static KliveTech *activeInstance;
    static const uint8_t BroadcastAddress[6];
    static const char *StartCommand;
    static const char *EndCommand;

    bool StartCallLoop();
    bool StartBluetooth();
    bool StartRelayRadio();
    bool ConnectHubToWifi();
    bool ConfigureWebSocket();
    void MaintainConnections();
    void MaintainStreamables();
    void PollStreamMessageQueue();
    void ResetStreamTransportState(bool discardQueuedMessages);
    bool QueueNextStreamSample();
    bool QueueNextBinaryStreamChunk();
    void PollBluetooth();
    void PollRelayQueue();
    void ProcessRelayPacket(const QueuedRelayPacket &queuedPacket);
    void ProcessCompletedRelayMessage(const uint8_t sourceAddress[6], const String &message);
    void ProcessCommand(
        const String &command,
        ResponseTransport responseTransport,
        const uint8_t *relayDestination = nullptr,
        const String &sourceDeviceID = String());
    int BeginFirmwareUpdateFromRequest(JsonVariantConst requestData, JsonObject response);
    int WriteFirmwareUpdateChunk(JsonVariantConst requestData, JsonObject response);
    int CompleteFirmwareUpdateFromRequest(JsonObject response);
    int AbortFirmwareUpdateFromRequest(JsonObject response);
    int ConfigureStreamableFromRequest(JsonVariantConst requestData, JsonObject response);
    bool ParseOperationData(JsonVariantConst requestData, JsonDocument &parsed) const;
    void ClearFirmwareUpdate(bool abortWriter);
    static bool IsValidSha256(const String &value);
    void SendResponse(
        const String &response,
        ResponseTransport responseTransport,
        const uint8_t *relayDestination,
        const String &sourceDeviceID);
    bool SendRelayMessage(const uint8_t destination[6], const String &message);
    bool SendRelayPacket(const uint8_t destination[6], RelayPacket &packet);
    void SendBeacon();
    void SendRegistration();
    void SendInventory();
    void SendPresence(const RelayPeer &peer, bool isConnected);
    void SendHello();
    void SendHubResponse(const String &sourceDeviceID, const String &response);
    bool SendHubStreamEvent(const String &sourceDeviceID, const String &event);
    bool SendDeviceMessage(const String &message);
    bool CanSendDeviceMessage();
    bool EnqueueStreamMessage(String *message, TickType_t waitTicks);
    bool RegisterStreamable(
        const char *streamID,
        const char *valueType,
        const char *mimeType,
        KliveTechStreamMode mode,
        unsigned long updateIntervalMs,
        std::function<String()> getter);
    bool PublishSerializedStreamable(
        const char *streamID,
        const String &serializedJson,
        const char *expectedValueType);
    String BuildStreamDefinition(const RegisteredStreamable &streamable) const;
    String BuildStreamManifest() const;
    String BuildStreamSample(
        const RegisteredStreamable &streamable,
        uint64_t sequence,
        const String &serializedJson) const;
    String BuildStreamFrameChunk(const PendingBinaryFrame &frame, size_t chunkLength) const;
    RegisteredStreamable *FindStreamable(const String &streamID);
    static bool IsValidStreamID(const char *streamID);
    static const char *StreamModeName(KliveTechStreamMode mode);
    void HandleHubWebSocketText(const uint8_t *payload, size_t length);
    void UpsertRelayPeer(const uint8_t radioAddress[6], const RelayPacket &packet);
    void RemoveStalePeers();
    void AddEspNowPeer(const uint8_t address[6]);
    void EnsureBroadcastPeer();
    void HopRelayChannel();
    void ClearStaleAssemblies();
    RelayAssembly *FindOrCreateAssembly(const uint8_t sourceAddress[6], const RelayPacket &packet);
    RelayPeer *FindRelayPeerByRadioAddress(const uint8_t address[6]);
    RelayPeer *FindRelayPeerByDeviceID(const String &id);
    String BuildResponse(int id, int status, JsonVariantConst data, bool responseExpected = false) const;
    String ExtractNextBluetoothFrame();
    String LocalDeviceID() const;
    uint32_t RelayNetworkHash() const;
    void SignRelayPacket(RelayPacket &packet) const;
    bool VerifyRelayPacket(const RelayPacket &packet) const;

    static void RunActionTask(void *parameter);
    static void WebSocketEvent(WStype_t type, uint8_t *payload, size_t length);
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    static void EspNowReceive(const esp_now_recv_info_t *info, const uint8_t *data, int length);
#else
    static void EspNowReceive(const uint8_t *sourceAddress, const uint8_t *data, int length);
#endif
};

#endif
