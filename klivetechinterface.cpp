#include "klivetechinterface.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <esp_arduino_version.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <mbedtls/base64.h>
#include <mbedtls/md.h>
#include <Update.h>

KliveTech *KliveTech::activeInstance = nullptr;
const uint8_t KliveTech::BroadcastAddress[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
const char *KliveTech::StartCommand = "{startComm}";
const char *KliveTech::EndCommand = "{endComm}";

namespace
{
String MacToID(const uint8_t address[6])
{
    char value[13];
    snprintf(
        value,
        sizeof(value),
        "%02X%02X%02X%02X%02X%02X",
        address[0],
        address[1],
        address[2],
        address[3],
        address[4],
        address[5]);
    return String(value);
}

bool IDToMac(const String &value, uint8_t address[6])
{
    if (value.length() != 12)
    {
        return false;
    }

    for (size_t index = 0; index < 6; ++index)
    {
        char pair[3] = {value[index * 2], value[index * 2 + 1], '\0'};
        char *end = nullptr;
        long parsed = strtol(pair, &end, 16);
        if (end == pair || *end != '\0' || parsed < 0 || parsed > 255)
        {
            return false;
        }
        address[index] = static_cast<uint8_t>(parsed);
    }
    return true;
}

bool AddressesEqual(const uint8_t first[6], const uint8_t second[6])
{
    return memcmp(first, second, 6) == 0;
}

bool EncodeBase64(const uint8_t *data, size_t length, String &encoded)
{
    if (data == nullptr || length == 0)
    {
        return false;
    }

    const size_t capacity = (((length + 2) / 3) * 4) + 1;
    std::vector<unsigned char> buffer(capacity);
    size_t encodedLength = 0;
    if (mbedtls_base64_encode(
            buffer.data(),
            buffer.size(),
            &encodedLength,
            data,
            length) != 0 ||
        encodedLength == 0 || encodedLength >= buffer.size())
    {
        return false;
    }

    buffer[encodedLength] = '\0';
    encoded = String(reinterpret_cast<const char *>(buffer.data()));
    return encoded.length() == encodedLength;
}

bool CalculateSha256(const uint8_t *data, size_t length, String &digestText)
{
    if (data == nullptr || length == 0)
    {
        return false;
    }

    const mbedtls_md_info_t *hashInfo = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (hashInfo == nullptr)
    {
        return false;
    }

    unsigned char digest[32] = {0};
    if (mbedtls_md(hashInfo, data, length, digest) != 0)
    {
        return false;
    }

    char text[65];
    for (size_t index = 0; index < sizeof(digest); ++index)
    {
        snprintf(text + (index * 2), 3, "%02x", digest[index]);
    }
    text[64] = '\0';
    digestText = String(text);
    return true;
}

bool IsStreamEventName(const char *eventName)
{
    return eventName != nullptr &&
           (strcmp(eventName, "StreamManifest") == 0 ||
            strcmp(eventName, "StreamSample") == 0 ||
            strcmp(eventName, "StreamFrame") == 0);
}

bool IsValidMimeType(const char *mimeType)
{
    if (mimeType == nullptr || mimeType[0] == '\0')
    {
        return false;
    }

    bool sawSlash = false;
    bool componentHasCharacter = false;
    for (const char *current = mimeType; *current != '\0'; ++current)
    {
        const char character = *current;
        if (character == '/')
        {
            if (sawSlash || !componentHasCharacter || current[1] == '\0')
            {
                return false;
            }
            sawSlash = true;
            componentHasCharacter = false;
            continue;
        }
        if (!((character >= 'A' && character <= 'Z') ||
              (character >= 'a' && character <= 'z') ||
              (character >= '0' && character <= '9') ||
              character == '.' || character == '+' || character == '-'))
        {
            return false;
        }
        componentHasCharacter = true;
    }
    return sawSlash && componentHasCharacter;
}
} // namespace

bool KliveTech::CreateKliveTechGadget(const char *name)
{
    if (mode != DeviceMode::Uninitialized || name == nullptr || name[0] == '\0')
    {
        return false;
    }
    if (activeInstance != nullptr && activeInstance != this)
    {
        Serial.println("KliveTech supports one active instance per ESP32.");
        return false;
    }

    Serial.begin(115200);
    activeInstance = this;
    mode = DeviceMode::Gadget;
    gadgetName = String(name);
    deviceID = LocalDeviceID();
    actionMutex = xSemaphoreCreateMutex();
    streamableMutex = xSemaphoreCreateMutex();
    streamMessageQueue = xQueueCreate(StreamMessageQueueLength, sizeof(String *));
    char sessionText[33];
    snprintf(
        sessionText,
        sizeof(sessionText),
        "%08lx%08lx%08lx%08lx",
        static_cast<unsigned long>(esp_random()),
        static_cast<unsigned long>(esp_random()),
        static_cast<unsigned long>(esp_random()),
        static_cast<unsigned long>(esp_random()));
    streamSessionID = String(sessionText);
    mbedtls_md_init(&firmwareHashContext);
    if (actionMutex == nullptr || streamableMutex == nullptr || streamMessageQueue == nullptr)
    {
        mode = DeviceMode::Uninitialized;
        activeInstance = nullptr;
        return false;
    }

    if (!StartBluetooth())
    {
        mode = DeviceMode::Uninitialized;
        activeInstance = nullptr;
        return false;
    }

    if (!relayNetworkKey.isEmpty() && !StartRelayRadio())
    {
        Serial.println("KliveTech relay discovery failed; Bluetooth fallback remains available.");
    }

    if (!StartCallLoop())
    {
        mode = DeviceMode::Uninitialized;
        activeInstance = nullptr;
        return false;
    }

    Serial.printf("KliveTech gadget '%s' created with device ID %s.\n", gadgetName.c_str(), deviceID.c_str());
    return true;
}

bool KliveTech::CreateKliveTechHub(const char *name, const KliveTechHubConfig &config)
{
    if (mode != DeviceMode::Uninitialized || name == nullptr || name[0] == '\0' ||
        config.wifiSSID == nullptr || config.wifiSSID[0] == '\0' ||
        config.omnipotentHost == nullptr || config.omnipotentHost[0] == '\0' ||
        config.accessToken == nullptr || config.accessToken[0] == '\0' ||
        config.relayNetworkKey == nullptr || config.relayNetworkKey[0] == '\0' ||
        (config.useTLS && (config.caCertificate == nullptr || config.caCertificate[0] == '\0')))
    {
        Serial.println("KliveTech hub configuration is incomplete. TLS hubs require a CA certificate.");
        return false;
    }
    if (activeInstance != nullptr && activeInstance != this)
    {
        Serial.println("KliveTech supports one active instance per ESP32.");
        return false;
    }

    Serial.begin(115200);
    activeInstance = this;
    mode = DeviceMode::Hub;
    gadgetName = String(name);
    deviceID = LocalDeviceID();
    wifiSSID = String(config.wifiSSID);
    wifiPassword = String(config.wifiPassword == nullptr ? "" : config.wifiPassword);
    omnipotentHost = String(config.omnipotentHost);
    omnipotentPort = config.omnipotentPort;
    omnipotentPath = String(config.omnipotentPath == nullptr ? "/klivetech/hub/connect" : config.omnipotentPath);
    accessToken = String(config.accessToken);
    relayNetworkKey = String(config.relayNetworkKey);
    caCertificate = String(config.caCertificate == nullptr ? "" : config.caCertificate);
    useTLS = config.useTLS;
    actionMutex = xSemaphoreCreateMutex();
    streamableMutex = xSemaphoreCreateMutex();
    streamMessageQueue = xQueueCreate(StreamMessageQueueLength, sizeof(String *));
    char sessionText[33];
    snprintf(
        sessionText,
        sizeof(sessionText),
        "%08lx%08lx%08lx%08lx",
        static_cast<unsigned long>(esp_random()),
        static_cast<unsigned long>(esp_random()),
        static_cast<unsigned long>(esp_random()),
        static_cast<unsigned long>(esp_random()));
    streamSessionID = String(sessionText);
    mbedtls_md_init(&firmwareHashContext);
    if (actionMutex == nullptr || streamableMutex == nullptr || streamMessageQueue == nullptr)
    {
        mode = DeviceMode::Uninitialized;
        activeInstance = nullptr;
        return false;
    }

    if (!ConnectHubToWifi() || !StartRelayRadio())
    {
        mode = DeviceMode::Uninitialized;
        activeInstance = nullptr;
        return false;
    }

    if (!ConfigureWebSocket())
    {
        mode = DeviceMode::Uninitialized;
        activeInstance = nullptr;
        return false;
    }
    if (!StartCallLoop())
    {
        mode = DeviceMode::Uninitialized;
        activeInstance = nullptr;
        return false;
    }

    Serial.printf("KliveTech hub '%s' created with device ID %s.\n", gadgetName.c_str(), deviceID.c_str());
    return true;
}

bool KliveTech::CreateKliveTechHub(
    const char *name,
    const char *wifiSSIDValue,
    const char *wifiPasswordValue,
    const char *omnipotentHostValue,
    const char *accessTokenValue,
    const char *relayNetworkKeyValue,
    const char *caCertificateValue,
    uint16_t omnipotentPortValue,
    bool useTLSValue)
{
    KliveTechHubConfig config;
    config.wifiSSID = wifiSSIDValue;
    config.wifiPassword = wifiPasswordValue;
    config.omnipotentHost = omnipotentHostValue;
    config.omnipotentPort = omnipotentPortValue;
    config.accessToken = accessTokenValue;
    config.relayNetworkKey = relayNetworkKeyValue;
    config.caCertificate = caCertificateValue;
    config.useTLS = useTLSValue;
    return CreateKliveTechHub(name, config);
}

bool KliveTech::SetRelayNetworkKey(const char *key)
{
    if (mode != DeviceMode::Uninitialized || key == nullptr || key[0] == '\0')
    {
        return false;
    }
    relayNetworkKey = String(key);
    return true;
}

bool KliveTech::StartBluetooth()
{
    bluetoothStarted = SerialBT.begin(gadgetName.c_str());
    if (!bluetoothStarted)
    {
        Serial.println("KliveTech could not initialize Bluetooth.");
    }
    return bluetoothStarted;
}

bool KliveTech::ConnectHubToWifi()
{
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());

    const unsigned long started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < 30000)
    {
        delay(100);
    }

    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.printf("KliveTech hub could not connect to Wi-Fi '%s'.\n", wifiSSID.c_str());
        return false;
    }

    currentRelayChannel = WiFi.channel();
    Serial.printf("KliveTech hub connected to Wi-Fi on channel %u.\n", currentRelayChannel);
    return true;
}

bool KliveTech::StartRelayRadio()
{
    if (relayNetworkKey.isEmpty())
    {
        return false;
    }

    if (mode == DeviceMode::Gadget)
    {
        WiFi.mode(WIFI_STA);
        WiFi.disconnect(false, false);
        currentRelayChannel = 1;
        esp_wifi_set_channel(currentRelayChannel, WIFI_SECOND_CHAN_NONE);
    }

    if (esp_now_init() != ESP_OK)
    {
        Serial.println("KliveTech could not initialize ESP-NOW.");
        return false;
    }

    relayReceiveQueue = xQueueCreate(16, sizeof(QueuedRelayPacket));
    if (relayReceiveQueue == nullptr)
    {
        esp_now_deinit();
        return false;
    }

    if (esp_now_register_recv_cb(EspNowReceive) != ESP_OK)
    {
        vQueueDelete(relayReceiveQueue);
        relayReceiveQueue = nullptr;
        esp_now_deinit();
        return false;
    }

    relayRadioStarted = true;
    EnsureBroadcastPeer();
    return true;
}

bool KliveTech::StartCallLoop()
{
    BaseType_t created = xTaskCreate(
        [](void *parameter)
        {
            static_cast<KliveTech *>(parameter)->CallLoop();
        },
        "KliveTech Loop",
        12288,
        this,
        2,
        &callLoopTask);
    return created == pdPASS;
}

bool KliveTech::ConfigureWebSocket()
{
    webSocket = new WebSocketsClient();
    if (webSocket == nullptr)
    {
        return false;
    }
    webSocket->onEvent(WebSocketEvent);
    webSocket->setReconnectInterval(5000);
    webSocket->enableHeartbeat(15000, 3000, 2);
    if (useTLS)
    {
        webSocket->beginSslWithCA(
            omnipotentHost.c_str(),
            omnipotentPort,
            omnipotentPath.c_str(),
            caCertificate.c_str(),
            "klivetech-v2");
    }
    else
    {
        webSocket->begin(
            omnipotentHost.c_str(),
            omnipotentPort,
            omnipotentPath.c_str(),
            "klivetech-v2");
    }
    return true;
}

void KliveTech::CallLoop()
{
    for (;;)
    {
        PollBluetooth();
        PollRelayQueue();

        if (mode == DeviceMode::Hub && webSocket != nullptr && WiFi.status() == WL_CONNECTED)
        {
            webSocket->loop();
        }

        MaintainConnections();
        PollStreamMessageQueue();
        MaintainStreamables();
        if (firmwareRestartAt != 0 &&
            static_cast<int32_t>(millis() - firmwareRestartAt) >= 0)
        {
            ESP.restart();
        }
        delay(5);
    }
}

void KliveTech::MaintainConnections()
{
    const unsigned long now = millis();
    ClearStaleAssemblies();

    if (!relayRadioStarted)
    {
        return;
    }

    if (mode == DeviceMode::Hub)
    {
        if (WiFi.status() != WL_CONNECTED)
        {
            omnipotentConnected = false;
            if (now - lastChannelHop >= 5000)
            {
                lastChannelHop = now;
                WiFi.reconnect();
            }
            return;
        }

        currentRelayChannel = WiFi.channel();
        if (now - lastHubBeacon >= HubBeaconIntervalMs)
        {
            lastHubBeacon = now;
            SendBeacon();
        }
        RemoveStalePeers();
        return;
    }

    if (connectedToHub && now - lastHubSeen > PeerTimeoutMs)
    {
        connectedToHub = false;
        memset(currentHubAddress, 0, sizeof(currentHubAddress));
    }

    if (connectedToHub)
    {
        if (now - lastRegistration >= LeafRegistrationIntervalMs)
        {
            SendRegistration();
        }
    }
    else if (now - lastChannelHop >= 300)
    {
        HopRelayChannel();
    }
}

void KliveTech::PollBluetooth()
{
    if (!bluetoothStarted)
    {
        return;
    }

    while (SerialBT.available() > 0)
    {
        int value = SerialBT.read();
        if (value >= 0)
        {
            bluetoothBuffer += static_cast<char>(value);
        }
        if (bluetoothBuffer.length() > MaximumRelayMessageSize + 64)
        {
            bluetoothBuffer = String();
            break;
        }
    }

    while (true)
    {
        String frame = ExtractNextBluetoothFrame();
        if (frame.isEmpty())
        {
            break;
        }
        ProcessCommand(frame, ResponseTransport::Bluetooth);
    }
}

String KliveTech::ExtractNextBluetoothFrame()
{
    int start = bluetoothBuffer.indexOf(StartCommand);
    if (start < 0)
    {
        if (bluetoothBuffer.length() > strlen(StartCommand))
        {
            bluetoothBuffer.remove(0, bluetoothBuffer.length() - strlen(StartCommand));
        }
        return String();
    }

    int payloadStart = start + strlen(StartCommand);
    int end = bluetoothBuffer.indexOf(EndCommand, payloadStart);
    if (end < 0)
    {
        if (start > 0)
        {
            bluetoothBuffer.remove(0, start);
        }
        return String();
    }

    String frame = bluetoothBuffer.substring(payloadStart, end);
    bluetoothBuffer.remove(0, end + strlen(EndCommand));
    return frame;
}

void KliveTech::PollRelayQueue()
{
    if (relayReceiveQueue == nullptr)
    {
        return;
    }

    QueuedRelayPacket queuedPacket{};
    while (xQueueReceive(relayReceiveQueue, &queuedPacket, 0) == pdTRUE)
    {
        ProcessRelayPacket(queuedPacket);
    }
}

void KliveTech::ProcessRelayPacket(const QueuedRelayPacket &queuedPacket)
{
    const RelayPacket &packet = queuedPacket.packet;
    if (!VerifyRelayPacket(packet))
    {
        return;
    }

    RelayPacketKind kind = static_cast<RelayPacketKind>(packet.kind);
    if (kind == RelayPacketKind::Beacon && mode == DeviceMode::Gadget)
    {
        if (connectedToHub && !AddressesEqual(currentHubAddress, queuedPacket.sourceAddress))
        {
            return;
        }

        bool newlyConnected = !connectedToHub;
        memcpy(currentHubAddress, queuedPacket.sourceAddress, sizeof(currentHubAddress));
        connectedToHub = true;
        lastHubSeen = millis();
        AddEspNowPeer(currentHubAddress);
        if (newlyConnected)
        {
            SendRegistration();
        }
        return;
    }

    if (kind == RelayPacketKind::Registration && mode == DeviceMode::Hub)
    {
        UpsertRelayPeer(queuedPacket.sourceAddress, packet);
        return;
    }

    if (kind != RelayPacketKind::Data || packet.fragmentCount == 0 ||
        packet.fragmentIndex >= packet.fragmentCount || packet.payloadLength > RelayPayloadSize ||
        static_cast<size_t>(packet.fragmentCount) * RelayPayloadSize > MaximumRelayMessageSize + RelayPayloadSize)
    {
        return;
    }

    if (mode == DeviceMode::Gadget && !AddressesEqual(currentHubAddress, queuedPacket.sourceAddress))
    {
        return;
    }

    RelayAssembly *assembly = FindOrCreateAssembly(queuedPacket.sourceAddress, packet);
    if (assembly == nullptr)
    {
        return;
    }

    if (assembly->fragments[packet.fragmentIndex].isEmpty())
    {
        String fragment;
        fragment.reserve(packet.payloadLength);
        for (size_t index = 0; index < packet.payloadLength; ++index)
        {
            fragment += static_cast<char>(packet.payload[index]);
        }
        assembly->fragments[packet.fragmentIndex] = fragment;
        ++assembly->receivedFragments;
    }
    assembly->lastUpdated = millis();

    if (assembly->receivedFragments != assembly->fragments.size())
    {
        return;
    }

    String completed;
    for (const String &fragment : assembly->fragments)
    {
        completed += fragment;
        if (completed.length() > MaximumRelayMessageSize)
        {
            completed = String();
            break;
        }
    }
    uint8_t sourceAddress[6];
    memcpy(sourceAddress, assembly->sourceAddress, sizeof(sourceAddress));
    size_t assemblyIndex = static_cast<size_t>(assembly - relayAssemblies.data());
    relayAssemblies.erase(relayAssemblies.begin() + assemblyIndex);

    if (!completed.isEmpty())
    {
        ProcessCompletedRelayMessage(sourceAddress, completed);
    }
}

void KliveTech::ProcessCompletedRelayMessage(const uint8_t sourceAddress[6], const String &message)
{
    if (mode == DeviceMode::Hub)
    {
        RelayPeer *peer = FindRelayPeerByRadioAddress(sourceAddress);
        if (peer == nullptr)
        {
            return;
        }
        peer->lastSeen = millis();

        JsonDocument eventDocument;
        if (!deserializeJson(eventDocument, message) &&
            IsStreamEventName(eventDocument["EVENT"] | ""))
        {
            SendHubStreamEvent(peer->deviceID, message);
            return;
        }

        SendHubResponse(peer->deviceID, message);
        return;
    }

    lastHubSeen = millis();
    ProcessCommand(message, ResponseTransport::Relay, currentHubAddress, deviceID);
}

void KliveTech::ProcessCommand(
    const String &command,
    ResponseTransport responseTransport,
    const uint8_t *relayDestination,
    const String &sourceDeviceID)
{
    JsonDocument request;
    DeserializationError requestError = deserializeJson(request, command);
    if (requestError)
    {
        return;
    }

    int id = request["ID"] | 0;
    int operation = request["OP"] | -1;
    bool responseExpected = request["RESPEXPECT"] | true;
    if (id <= 0)
    {
        return;
    }

    JsonDocument responseData;
    JsonObject responseObject = responseData.to<JsonObject>();
    int status = 200;

    if (operation == GetActions)
    {
        JsonArray actions = responseObject["Actions"].to<JsonArray>();
        if (actionMutex != nullptr)
        {
            xSemaphoreTake(actionMutex, portMAX_DELAY);
        }
        for (const RegisteredAction &registeredAction : possibleActions)
        {
            JsonObject action = actions.add<JsonObject>();
            action["Name"] = registeredAction.name;
            action["ParamDescription"] = registeredAction.paramDescription;
            action["Type"] = static_cast<int>(registeredAction.type);
        }
        if (actionMutex != nullptr)
        {
            xSemaphoreGive(actionMutex);
        }
    }
    else if (operation == ExecuteAction)
    {
        JsonDocument actionRequest;
        JsonVariantConst requestData = request["DATA"];
        bool actionParseFailed = false;
        if (requestData.is<const char *>())
        {
            DeserializationError actionError = deserializeJson(actionRequest, requestData.as<const char *>());
            actionParseFailed = static_cast<bool>(actionError);
        }
        else
        {
            actionRequest.set(requestData);
        }

        const char *actionName = actionRequest["ActionName"] | "";
        RegisteredAction selectedAction;
        bool actionFound = false;
        if (!actionParseFailed)
        {
            if (actionMutex != nullptr)
            {
                xSemaphoreTake(actionMutex, portMAX_DELAY);
            }
            for (const RegisteredAction &candidate : possibleActions)
            {
                if (candidate.name.equals(actionName))
                {
                    selectedAction = candidate;
                    actionFound = true;
                    break;
                }
            }
            if (actionMutex != nullptr)
            {
                xSemaphoreGive(actionMutex);
            }
        }

        if (!actionFound)
        {
            status = 404;
            responseObject["Error"] = "Unknown action";
        }
        else
        {
            ActionInvocation *invocation = new ActionInvocation();
            if (invocation == nullptr)
            {
                status = 503;
                responseObject["Error"] = "Out of memory";
            }
            else
            {
                invocation->action = selectedAction;
                invocation->integerValue = actionRequest["Param"] | 0;
                invocation->boolValue = actionRequest["Param"] | false;
                invocation->stringValue = actionRequest["Param"].as<String>();
                BaseType_t taskCreated = xTaskCreate(
                    RunActionTask,
                    "KliveTech Action",
                    4096,
                    invocation,
                    1,
                    nullptr);
                if (taskCreated != pdPASS)
                {
                    delete invocation;
                    status = 503;
                    responseObject["Error"] = "Could not start action";
                }
            }
        }
    }
    else if (operation == BeginFirmwareUpdate)
    {
        status = BeginFirmwareUpdateFromRequest(request["DATA"], responseObject);
    }
    else if (operation == FirmwareUpdateChunk)
    {
        status = WriteFirmwareUpdateChunk(request["DATA"], responseObject);
    }
    else if (operation == CompleteFirmwareUpdate)
    {
        status = CompleteFirmwareUpdateFromRequest(responseObject);
    }
    else if (operation == AbortFirmwareUpdate)
    {
        status = AbortFirmwareUpdateFromRequest(responseObject);
    }
    else if (operation == GetStreamables)
    {
        String manifest = BuildStreamManifest();
        JsonDocument manifestDocument;
        if (manifest.isEmpty() || deserializeJson(manifestDocument, manifest))
        {
            status = 500;
            responseObject["Error"] = "Could not build the Streamables manifest";
        }
        else
        {
            responseObject.set(manifestDocument.as<JsonVariantConst>());

            ResetStreamTransportState(true);
            EnqueueStreamMessage(new (std::nothrow) String(manifest), 0);
        }
    }
    else if (operation == ConfigureStreamable)
    {
        status = ConfigureStreamableFromRequest(request["DATA"], responseObject);
    }
    else if (operation != Ping)
    {
        status = 400;
        responseObject["Error"] = "Unknown operation";
    }

    String response = BuildResponse(id, status, responseData.as<JsonVariantConst>(), false);
    if (responseExpected || status != 200)
    {
        SendResponse(response, responseTransport, relayDestination, sourceDeviceID);
    }
}

bool KliveTech::ParseOperationData(JsonVariantConst requestData, JsonDocument &parsed) const
{
    if (requestData.is<const char *>())
    {
        const char *serialized = requestData.as<const char *>();
        return serialized != nullptr && !deserializeJson(parsed, serialized);
    }
    parsed.set(requestData);
    return !parsed.isNull();
}

bool KliveTech::IsValidSha256(const String &value)
{
    if (value.length() != 64)
    {
        return false;
    }
    for (size_t index = 0; index < value.length(); ++index)
    {
        char character = value[index];
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f') ||
              (character >= 'A' && character <= 'F')))
        {
            return false;
        }
    }
    return true;
}

void KliveTech::ClearFirmwareUpdate(bool abortWriter)
{
    if (abortWriter && Update.isRunning())
    {
        Update.abort();
    }
    if (firmwareHashInitialized)
    {
        mbedtls_md_free(&firmwareHashContext);
        mbedtls_md_init(&firmwareHashContext);
        firmwareHashInitialized = false;
    }
    firmwareUpdateActive = false;
    firmwareUpdateSize = 0;
    firmwareBytesWritten = 0;
    expectedFirmwareSha256 = String();
}

int KliveTech::BeginFirmwareUpdateFromRequest(JsonVariantConst requestData, JsonObject response)
{
    JsonDocument parsed;
    if (!ParseOperationData(requestData, parsed))
    {
        response["Error"] = "Invalid firmware update request";
        return 400;
    }

    uint64_t requestedSize = parsed["Size"] | 0ULL;
    String requestedHash = parsed["Sha256"] | "";
    requestedHash.toLowerCase();
    if (requestedSize == 0 || requestedSize > static_cast<uint64_t>(SIZE_MAX) ||
        !IsValidSha256(requestedHash))
    {
        response["Error"] = "Size and a 64-character SHA-256 digest are required";
        return 400;
    }

    ClearFirmwareUpdate(true);
    if (!Update.begin(static_cast<size_t>(requestedSize), U_FLASH))
    {
        response["Error"] = Update.errorString();
        return 507;
    }

    const mbedtls_md_info_t *hashInfo = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (hashInfo == nullptr)
    {
        ClearFirmwareUpdate(true);
        response["Error"] = "Could not initialize SHA-256 verification";
        return 500;
    }
    firmwareHashInitialized = true;
    if (mbedtls_md_setup(&firmwareHashContext, hashInfo, 0) != 0)
    {
        ClearFirmwareUpdate(true);
        response["Error"] = "Could not initialize SHA-256 verification";
        return 500;
    }
    if (mbedtls_md_starts(&firmwareHashContext) != 0)
    {
        ClearFirmwareUpdate(true);
        response["Error"] = "Could not initialize SHA-256 verification";
        return 500;
    }

    firmwareUpdateActive = true;
    firmwareUpdateSize = static_cast<size_t>(requestedSize);
    firmwareBytesWritten = 0;
    expectedFirmwareSha256 = requestedHash;
    response["Accepted"] = true;
    response["Size"] = firmwareUpdateSize;
    return 200;
}

int KliveTech::WriteFirmwareUpdateChunk(JsonVariantConst requestData, JsonObject response)
{
    if (!firmwareUpdateActive)
    {
        response["Error"] = "No firmware update is active";
        return 409;
    }

    JsonDocument parsed;
    if (!ParseOperationData(requestData, parsed))
    {
        response["Error"] = "Invalid firmware chunk request";
        return 400;
    }

    uint64_t requestedOffset = parsed["Offset"] | UINT64_MAX;
    const char *encoded = parsed["Data"] | nullptr;
    if (requestedOffset != firmwareBytesWritten || encoded == nullptr)
    {
        response["Error"] = "Firmware chunk offset is not the next expected offset";
        response["NextOffset"] = firmwareBytesWritten;
        return 409;
    }

    size_t encodedLength = strlen(encoded);
    size_t decodedCapacity = ((encodedLength + 3) / 4) * 3;
    if (encodedLength == 0 || decodedCapacity > MaximumFirmwareChunkSize + 2)
    {
        response["Error"] = "Firmware chunk is empty or too large";
        return 413;
    }

    std::vector<uint8_t> decoded(decodedCapacity);
    size_t decodedLength = 0;
    int decodeResult = mbedtls_base64_decode(
        decoded.data(),
        decoded.size(),
        &decodedLength,
        reinterpret_cast<const unsigned char *>(encoded),
        encodedLength);
    if (decodeResult != 0 || decodedLength == 0 || decodedLength > MaximumFirmwareChunkSize ||
        decodedLength > firmwareUpdateSize - firmwareBytesWritten)
    {
        response["Error"] = "Firmware chunk contains invalid base64 data or exceeds the image size";
        return 400;
    }

    if (Update.write(decoded.data(), decodedLength) != decodedLength ||
        mbedtls_md_update(&firmwareHashContext, decoded.data(), decodedLength) != 0)
    {
        String updateError = Update.errorString();
        ClearFirmwareUpdate(true);
        response["Error"] = updateError;
        return 500;
    }

    firmwareBytesWritten += decodedLength;
    response["NextOffset"] = firmwareBytesWritten;
    response["Size"] = firmwareUpdateSize;
    return 200;
}

int KliveTech::CompleteFirmwareUpdateFromRequest(JsonObject response)
{
    if (!firmwareUpdateActive)
    {
        response["Error"] = "No firmware update is active";
        return 409;
    }
    if (firmwareBytesWritten != firmwareUpdateSize || !Update.isFinished())
    {
        response["Error"] = "Firmware image is incomplete";
        response["NextOffset"] = firmwareBytesWritten;
        return 409;
    }

    unsigned char digest[32];
    if (mbedtls_md_finish(&firmwareHashContext, digest) != 0)
    {
        ClearFirmwareUpdate(true);
        response["Error"] = "Could not finish SHA-256 verification";
        return 500;
    }

    char digestText[65];
    for (size_t index = 0; index < sizeof(digest); ++index)
    {
        snprintf(digestText + (index * 2), 3, "%02x", digest[index]);
    }
    digestText[64] = '\0';
    if (!expectedFirmwareSha256.equals(digestText))
    {
        ClearFirmwareUpdate(true);
        response["Error"] = "Firmware SHA-256 verification failed";
        return 422;
    }

    size_t completedSize = firmwareUpdateSize;
    String completedHash = expectedFirmwareSha256;
    if (!Update.end(false))
    {
        String updateError = Update.errorString();
        ClearFirmwareUpdate(true);
        response["Error"] = updateError;
        return 500;
    }

    ClearFirmwareUpdate(false);
    response["Size"] = completedSize;
    response["Sha256"] = completedHash;
    response["Rebooting"] = true;
    firmwareRestartAt = millis() + 1500;
    return 200;
}

int KliveTech::AbortFirmwareUpdateFromRequest(JsonObject response)
{
    bool wasActive = firmwareUpdateActive;
    ClearFirmwareUpdate(true);
    response["Aborted"] = wasActive;
    return 200;
}

int KliveTech::ConfigureStreamableFromRequest(JsonVariantConst requestData, JsonObject response)
{
    JsonDocument parsed;
    if (!ParseOperationData(requestData, parsed))
    {
        response["Error"] = "Invalid Streamable configuration";
        return 400;
    }

    String streamID = !parsed["StreamID"].isNull()
                          ? parsed["StreamID"].as<String>()
                          : parsed["STREAMID"].as<String>();
    if (!IsValidStreamID(streamID.c_str()))
    {
        response["Error"] = "A valid StreamID is required";
        return 400;
    }

    const bool hasEnabled = !parsed["Enabled"].isNull() || !parsed["ENABLED"].isNull();
    const bool enabled = !parsed["Enabled"].isNull()
                             ? parsed["Enabled"].as<bool>()
                             : parsed["ENABLED"].as<bool>();
    const bool hasInterval = !parsed["IntervalMs"].isNull() || !parsed["INTERVALMS"].isNull();
    uint64_t requestedInterval = !parsed["IntervalMs"].isNull()
                                     ? parsed["IntervalMs"].as<uint64_t>()
                                     : parsed["INTERVALMS"].as<uint64_t>();
    if (hasInterval && (requestedInterval < MinimumStreamIntervalMs || requestedInterval > UINT32_MAX))
    {
        response["Error"] = "IntervalMs is outside the supported range";
        return 400;
    }
    if (!hasEnabled && !hasInterval)
    {
        response["Error"] = "Enabled or IntervalMs is required";
        return 400;
    }

    String definition;
    uint32_t revision = 0;
    if (streamableMutex != nullptr)
    {
        xSemaphoreTake(streamableMutex, portMAX_DELAY);
    }
    RegisteredStreamable *streamable = FindStreamable(streamID);
    if (streamable == nullptr)
    {
        if (streamableMutex != nullptr)
        {
            xSemaphoreGive(streamableMutex);
        }
        response["Error"] = "Unknown Streamable";
        return 404;
    }

    if (hasEnabled)
    {
        streamable->enabled = enabled;
        if (enabled)
        {
            streamable->hasPublished = false;
            streamable->lastChecked = 0;
        }
    }
    if (hasInterval)
    {
        streamable->updateIntervalMs = static_cast<unsigned long>(requestedInterval);
        streamable->lastChecked = 0;
    }
    streamable->definitionDirty = true;
    ++streamManifestRevision;
    if (streamManifestRevision == 0)
    {
        streamManifestRevision = 1;
    }
    revision = streamManifestRevision;
    definition = BuildStreamDefinition(*streamable);

    if (streamableMutex != nullptr)
    {
        xSemaphoreGive(streamableMutex);
    }

    JsonDocument definitionDocument;
    if (deserializeJson(definitionDocument, definition))
    {
        response["Error"] = "Could not serialize the Streamable definition";
        return 500;
    }
    response["Revision"] = revision;
    response["Streamable"].set(definitionDocument.as<JsonVariantConst>());

    String manifest = BuildStreamManifest();
    EnqueueStreamMessage(new (std::nothrow) String(manifest), 0);
    return 200;
}

String KliveTech::BuildResponse(int id, int status, JsonVariantConst data, bool responseExpected) const
{
    JsonDocument response;
    response["ID"] = id;
    response["DATA"].set(data);
    response["STATUS"] = status;
    response["RESPEXPECT"] = responseExpected;
    String serialized;
    serializeJson(response, serialized);
    return serialized;
}

void KliveTech::SendResponse(
    const String &response,
    ResponseTransport responseTransport,
    const uint8_t *relayDestination,
    const String &sourceDeviceID)
{
    switch (responseTransport)
    {
    case ResponseTransport::Bluetooth:
        SerialBT.print(StartCommand);
        SerialBT.print(response);
        SerialBT.print(EndCommand);
        break;
    case ResponseTransport::Relay:
        if (relayDestination != nullptr)
        {
            SendRelayMessage(relayDestination, response);
        }
        break;
    case ResponseTransport::HubWebSocket:
        SendHubResponse(sourceDeviceID, response);
        break;
    }
}

void KliveTech::RunActionTask(void *parameter)
{
    ActionInvocation *invocation = static_cast<ActionInvocation *>(parameter);
    if (invocation != nullptr)
    {
        switch (invocation->action.type)
        {
        case Integer:
            if (invocation->action.intFunction)
                invocation->action.intFunction(invocation->integerValue);
            break;
        case StringParameter:
            if (invocation->action.stringFunction)
                invocation->action.stringFunction(invocation->stringValue.c_str());
            break;
        case Bool:
            if (invocation->action.boolFunction)
                invocation->action.boolFunction(invocation->boolValue);
            break;
        case None:
            if (invocation->action.noParamFunction)
                invocation->action.noParamFunction();
            break;
        }
        delete invocation;
    }
    vTaskDelete(nullptr);
}

bool KliveTech::SendRelayMessage(const uint8_t destination[6], const String &message)
{
    if (!relayRadioStarted || destination == nullptr || message.isEmpty() || message.length() > MaximumRelayMessageSize)
    {
        return false;
    }

    AddEspNowPeer(destination);
    uint32_t messageID = ++nextRelayMessageID;
    if (messageID == 0)
    {
        messageID = ++nextRelayMessageID;
    }
    uint16_t fragmentCount = static_cast<uint16_t>((message.length() + RelayPayloadSize - 1) / RelayPayloadSize);
    uint8_t senderID[6] = {0};
    IDToMac(deviceID, senderID);

    for (uint16_t fragmentIndex = 0; fragmentIndex < fragmentCount; ++fragmentIndex)
    {
        size_t offset = static_cast<size_t>(fragmentIndex) * RelayPayloadSize;
        size_t length = std::min(
            RelayPayloadSize,
            static_cast<size_t>(message.length()) - offset);
        RelayPacket packet{};
        packet.magic = RelayMagic;
        packet.version = RelayProtocolVersion;
        packet.kind = static_cast<uint8_t>(RelayPacketKind::Data);
        packet.payloadLength = static_cast<uint16_t>(length);
        packet.messageID = messageID;
        packet.fragmentIndex = fragmentIndex;
        packet.fragmentCount = fragmentCount;
        memcpy(packet.senderID, senderID, sizeof(packet.senderID));
        strncpy(packet.senderName, gadgetName.c_str(), sizeof(packet.senderName) - 1);
        memcpy(packet.payload, message.c_str() + offset, length);
        if (!SendRelayPacket(destination, packet))
        {
            return false;
        }
        delay(4);
    }
    return true;
}

bool KliveTech::SendRelayPacket(const uint8_t destination[6], RelayPacket &packet)
{
    packet.networkHash = RelayNetworkHash();
    SignRelayPacket(packet);
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        if (esp_now_send(destination, reinterpret_cast<const uint8_t *>(&packet), sizeof(packet)) == ESP_OK)
        {
            return true;
        }
        delay(5);
    }
    return false;
}

void KliveTech::SendBeacon()
{
    RelayPacket packet{};
    packet.magic = RelayMagic;
    packet.version = RelayProtocolVersion;
    packet.kind = static_cast<uint8_t>(RelayPacketKind::Beacon);
    packet.fragmentCount = 1;
    uint8_t senderID[6] = {0};
    IDToMac(deviceID, senderID);
    memcpy(packet.senderID, senderID, sizeof(packet.senderID));
    strncpy(packet.senderName, gadgetName.c_str(), sizeof(packet.senderName) - 1);
    EnsureBroadcastPeer();
    SendRelayPacket(BroadcastAddress, packet);
}

void KliveTech::SendRegistration()
{
    if (!connectedToHub)
    {
        return;
    }
    RelayPacket packet{};
    packet.magic = RelayMagic;
    packet.version = RelayProtocolVersion;
    packet.kind = static_cast<uint8_t>(RelayPacketKind::Registration);
    packet.fragmentCount = 1;
    uint8_t senderID[6] = {0};
    IDToMac(deviceID, senderID);
    memcpy(packet.senderID, senderID, sizeof(packet.senderID));
    strncpy(packet.senderName, gadgetName.c_str(), sizeof(packet.senderName) - 1);
    AddEspNowPeer(currentHubAddress);
    SendRelayPacket(currentHubAddress, packet);
    lastRegistration = millis();
}

void KliveTech::UpsertRelayPeer(const uint8_t radioAddress[6], const RelayPacket &packet)
{
    RelayPeer *peer = FindRelayPeerByRadioAddress(radioAddress);
    bool isNew = peer == nullptr;
    if (isNew)
    {
        if (relayPeers.size() >= MaximumRelayPeers)
        {
            return;
        }
        RelayPeer created{};
        memcpy(created.radioAddress, radioAddress, sizeof(created.radioAddress));
        relayPeers.push_back(created);
        peer = &relayPeers.back();
    }

    AddEspNowPeer(radioAddress);
    peer->deviceID = MacToID(packet.senderID);
    char safeName[sizeof(packet.senderName) + 1] = {0};
    memcpy(safeName, packet.senderName, sizeof(packet.senderName));
    peer->name = String(safeName);
    peer->lastSeen = millis();
    connectedGadgetCount = relayPeers.size();

    // Reaffirm presence on every registration so Omnipotent can recover a
    // gadget after a transient command/action-discovery failure.
    SendPresence(*peer, true);
}

void KliveTech::RemoveStalePeers()
{
    const unsigned long now = millis();
    for (size_t index = relayPeers.size(); index > 0; --index)
    {
        RelayPeer &peer = relayPeers[index - 1];
        if (now - peer.lastSeen <= PeerTimeoutMs)
        {
            continue;
        }
        SendPresence(peer, false);
        esp_now_del_peer(peer.radioAddress);
        relayPeers.erase(relayPeers.begin() + (index - 1));
    }
    connectedGadgetCount = relayPeers.size();
}

void KliveTech::SendHello()
{
    if (!omnipotentConnected)
    {
        return;
    }
    JsonDocument message;
    message["Type"] = "hello";
    message["Protocol"] = RelayProtocolVersion;
    message["HubId"] = deviceID;
    message["HubName"] = gadgetName;
    message["Token"] = accessToken;
    JsonObject hub = message["Hub"].to<JsonObject>();
    hub["DeviceId"] = deviceID;
    hub["Name"] = gadgetName;
    String serialized;
    serializeJson(message, serialized);
    webSocket->sendTXT(serialized);
}

void KliveTech::SendInventory()
{
    if (!omnipotentConnected)
    {
        return;
    }
    JsonDocument message;
    message["Type"] = "inventory";
    message["HubId"] = deviceID;
    JsonArray gadgets = message["Gadgets"].to<JsonArray>();
    for (const RelayPeer &peer : relayPeers)
    {
        JsonObject gadget = gadgets.add<JsonObject>();
        gadget["DeviceId"] = peer.deviceID;
        gadget["Name"] = peer.name;
    }
    String serialized;
    serializeJson(message, serialized);
    webSocket->sendTXT(serialized);
}

void KliveTech::SendPresence(const RelayPeer &peer, bool isConnected)
{
    if (!omnipotentConnected)
    {
        return;
    }
    JsonDocument message;
    message["Type"] = "presence";
    message["HubId"] = deviceID;
    message["DeviceId"] = peer.deviceID;
    message["Name"] = peer.name;
    message["Connected"] = isConnected;
    String serialized;
    serializeJson(message, serialized);
    webSocket->sendTXT(serialized);
}

void KliveTech::SendHubResponse(const String &sourceDeviceID, const String &response)
{
    if (!omnipotentConnected)
    {
        return;
    }

    JsonDocument responseDocument;
    if (deserializeJson(responseDocument, response))
    {
        return;
    }
    JsonDocument message;
    message["Type"] = "response";
    message["HubId"] = deviceID;
    message["DeviceId"] = sourceDeviceID;
    message["RequestId"] = responseDocument["ID"] | 0;
    message["Payload"].set(responseDocument.as<JsonVariantConst>());
    String serialized;
    serializeJson(message, serialized);
    webSocket->sendTXT(serialized);
}

bool KliveTech::SendHubStreamEvent(const String &sourceDeviceID, const String &event)
{
    if (!omnipotentConnected || webSocket == nullptr || event.isEmpty())
    {
        return false;
    }

    JsonDocument eventDocument;
    if (deserializeJson(eventDocument, event) ||
        !IsStreamEventName(eventDocument["EVENT"] | "") ||
        (eventDocument["VERSION"] | 0) != StreamProtocolVersion)
    {
        return false;
    }

    JsonDocument message;
    message["Type"] = "stream";
    message["HubId"] = deviceID;
    message["DeviceId"] = sourceDeviceID;
    message["Payload"].set(eventDocument.as<JsonVariantConst>());
    String serialized;
    serializeJson(message, serialized);
    return webSocket->sendTXT(serialized);
}

bool KliveTech::CanSendDeviceMessage()
{
    if (mode == DeviceMode::Hub)
    {
        return omnipotentConnected && webSocket != nullptr;
    }
    if (mode != DeviceMode::Gadget)
    {
        return false;
    }
    if (connectedToHub && relayRadioStarted)
    {
        return true;
    }
    return bluetoothStarted && SerialBT.hasClient();
}

bool KliveTech::SendDeviceMessage(const String &message)
{
    if (message.isEmpty() || message.length() > MaximumRelayMessageSize)
    {
        return false;
    }

    if (mode == DeviceMode::Hub)
    {
        return SendHubStreamEvent(deviceID, message);
    }
    if (mode != DeviceMode::Gadget)
    {
        return false;
    }
    if (connectedToHub && relayRadioStarted)
    {
        return SendRelayMessage(currentHubAddress, message);
    }
    if (!bluetoothStarted || !SerialBT.hasClient())
    {
        return false;
    }

    const size_t startWritten = SerialBT.print(StartCommand);
    const size_t messageWritten = SerialBT.print(message);
    const size_t endWritten = SerialBT.print(EndCommand);
    return startWritten == strlen(StartCommand) &&
           messageWritten == message.length() &&
           endWritten == strlen(EndCommand);
}

bool KliveTech::EnqueueStreamMessage(String *message, TickType_t waitTicks)
{
    if (message == nullptr)
    {
        ++droppedStreamMessageCount;
        return false;
    }
    if (streamMessageQueue == nullptr || message->isEmpty() ||
        message->length() > MaximumRelayMessageSize ||
        xQueueSend(streamMessageQueue, &message, waitTicks) != pdTRUE)
    {
        delete message;
        ++droppedStreamMessageCount;
        return false;
    }
    return true;
}

void KliveTech::PollStreamMessageQueue()
{
    // Commands and OTA are handled before this method on every CallLoop pass.
    // Keep queued telemetry intact while offline or while an OTA write is active.
    if (streamMessageQueue == nullptr)
    {
        return;
    }

    const bool transportAvailable = CanSendDeviceMessage();
    if (!transportAvailable)
    {
        streamTransportPreviouslyAvailable = false;
        return;
    }
    if (!streamTransportPreviouslyAvailable)
    {
        ResetStreamTransportState(true);
        streamTransportPreviouslyAvailable = true;
    }
    if (firmwareUpdateActive)
    {
        return;
    }

    String *message = nullptr;
    if (xQueuePeek(streamMessageQueue, &message, 0) != pdTRUE)
    {
        return;
    }
    if (message == nullptr)
    {
        xQueueReceive(streamMessageQueue, &message, 0);
        return;
    }
    if (!SendDeviceMessage(*message))
    {
        return;
    }

    String *sentMessage = nullptr;
    if (xQueueReceive(streamMessageQueue, &sentMessage, 0) == pdTRUE)
    {
        delete sentMessage;
    }
}

void KliveTech::ResetStreamTransportState(bool discardQueuedMessages)
{
    if (streamableMutex != nullptr)
    {
        xSemaphoreTake(streamableMutex, portMAX_DELAY);
    }
    if (discardQueuedMessages && streamMessageQueue != nullptr)
    {
        String *queuedMessage = nullptr;
        while (xQueueReceive(streamMessageQueue, &queuedMessage, 0) == pdTRUE)
        {
            delete queuedMessage;
            queuedMessage = nullptr;
            ++droppedStreamMessageCount;
        }
    }
    for (RegisteredStreamable &streamable : streamables)
    {
        streamable.definitionDirty = true;
        streamable.hasPublished = false;
        streamable.lastChecked = 0;
    }
    if (pendingBinaryFrame != nullptr)
    {
        pendingBinaryFrame->offset = 0;
        pendingBinaryFrame->chunkIndex = 0;
    }
    lastStreamManifest = 0;
    if (streamableMutex != nullptr)
    {
        xSemaphoreGive(streamableMutex);
    }
}

void KliveTech::MaintainStreamables()
{
    if (firmwareUpdateActive || streamMessageQueue == nullptr ||
        uxQueueSpacesAvailable(streamMessageQueue) == 0)
    {
        return;
    }

    const unsigned long now = millis();
    bool hasStreamables = false;
    bool manifestRequired = false;
    bool binaryFrameAwaitingDrain = false;
    uint32_t revision = 0;
    unsigned long previousManifest = 0;
    if (streamableMutex != nullptr)
    {
        xSemaphoreTake(streamableMutex, portMAX_DELAY);
    }
    hasStreamables = !streamables.empty();
    revision = streamManifestRevision;
    previousManifest = lastStreamManifest;
    binaryFrameAwaitingDrain = pendingBinaryFrame != nullptr &&
                               pendingBinaryFrame->offset >= pendingBinaryFrame->length;
    for (const RegisteredStreamable &streamable : streamables)
    {
        if (streamable.definitionDirty)
        {
            manifestRequired = true;
            break;
        }
    }
    if (streamableMutex != nullptr)
    {
        xSemaphoreGive(streamableMutex);
    }

    if (binaryFrameAwaitingDrain)
    {
        QueueNextBinaryStreamChunk();
        return;
    }

    manifestRequired = hasStreamables &&
                       (manifestRequired || previousManifest == 0 ||
                        now - previousManifest >= StreamDefinitionIntervalMs);
    if (manifestRequired)
    {
        String manifest = BuildStreamManifest();
        if (EnqueueStreamMessage(new (std::nothrow) String(manifest), 0))
        {
            if (streamableMutex != nullptr)
            {
                xSemaphoreTake(streamableMutex, portMAX_DELAY);
                lastStreamManifest = now;
                if (streamManifestRevision == revision)
                {
                    for (RegisteredStreamable &streamable : streamables)
                    {
                        streamable.definitionDirty = false;
                        streamable.lastDefinition = now;
                        if (streamable.mode == KliveTechStreamMode::OnChange)
                        {
                            // Periodically refresh state even when an earlier
                            // best-effort relay sample was lost.
                            streamable.hasPublished = false;
                            streamable.lastChecked = 0;
                        }
                    }
                }
                xSemaphoreGive(streamableMutex);
            }
        }
        return;
    }

    bool queued = false;
    if (preferBinaryStreamChunk)
    {
        queued = QueueNextBinaryStreamChunk();
        if (!queued)
        {
            queued = QueueNextStreamSample();
        }
    }
    else
    {
        queued = QueueNextStreamSample();
        if (!queued)
        {
            queued = QueueNextBinaryStreamChunk();
        }
    }
    if (queued)
    {
        preferBinaryStreamChunk = !preferBinaryStreamChunk;
    }
}

bool KliveTech::QueueNextStreamSample()
{
    if (streamMessageQueue == nullptr || uxQueueSpacesAvailable(streamMessageQueue) == 0)
    {
        return false;
    }

    const unsigned long now = millis();
    RegisteredStreamable selected;
    bool selectedStreamable = false;
    if (streamableMutex != nullptr)
    {
        xSemaphoreTake(streamableMutex, portMAX_DELAY);
    }
    const size_t count = streamables.size();
    for (size_t attempt = 0; attempt < count; ++attempt)
    {
        const size_t index = (nextStreamableIndex + attempt) % count;
        RegisteredStreamable &candidate = streamables[index];
        if (!candidate.enabled || candidate.mode == KliveTechStreamMode::Manual ||
            !candidate.jsonGetter ||
            (candidate.lastChecked != 0 && now - candidate.lastChecked < candidate.updateIntervalMs))
        {
            continue;
        }

        candidate.lastChecked = now;
        selected = candidate;
        nextStreamableIndex = (index + 1) % count;
        selectedStreamable = true;
        break;
    }
    if (streamableMutex != nullptr)
    {
        xSemaphoreGive(streamableMutex);
    }
    if (!selectedStreamable)
    {
        return false;
    }

    String serializedJson = selected.jsonGetter();
    JsonDocument validationDocument;
    if (serializedJson.isEmpty() || serializedJson.length() > MaximumStreamJsonValueSize ||
        deserializeJson(validationDocument, serializedJson))
    {
        ++droppedStreamMessageCount;
        return false;
    }

    bool queued = false;
    if (streamableMutex != nullptr)
    {
        xSemaphoreTake(streamableMutex, portMAX_DELAY);
    }
    RegisteredStreamable *current = FindStreamable(selected.streamID);
    if (current != nullptr && current->enabled && current->mode != KliveTechStreamMode::Manual)
    {
        const bool shouldPublish = current->mode == KliveTechStreamMode::Periodic ||
                                   !current->hasPublished ||
                                   !current->lastSerializedValue.equals(serializedJson);
        if (shouldPublish)
        {
            uint64_t sequence = ++current->nextSequence;
            if (sequence == 0)
            {
                sequence = ++current->nextSequence;
            }
            String message = BuildStreamSample(*current, sequence, serializedJson);
            queued = EnqueueStreamMessage(new (std::nothrow) String(message), 0);
            if (queued)
            {
                current->hasPublished = true;
                current->lastSerializedValue = serializedJson;
            }
        }
    }
    if (streamableMutex != nullptr)
    {
        xSemaphoreGive(streamableMutex);
    }
    return queued;
}

bool KliveTech::QueueNextBinaryStreamChunk()
{
    if (streamMessageQueue == nullptr || uxQueueSpacesAvailable(streamMessageQueue) == 0)
    {
        return false;
    }

    if (streamableMutex != nullptr)
    {
        xSemaphoreTake(streamableMutex, portMAX_DELAY);
    }
    PendingBinaryFrame *frame = pendingBinaryFrame;
    if (frame == nullptr || frame->data == nullptr)
    {
        if (streamableMutex != nullptr)
        {
            xSemaphoreGive(streamableMutex);
        }
        return false;
    }
    if (frame->offset >= frame->length)
    {
        // Keep the source frame until its final queued chunk has actually left
        // the queue. A reconnect can then drain stale chunks and restart the
        // same frame from offset zero instead of leaving an incomplete frame
        // at Omnipotent.
        const bool waitingForQueuedChunks = uxQueueMessagesWaiting(streamMessageQueue) > 0;
        if (!waitingForQueuedChunks)
        {
            delete[] frame->data;
            delete frame;
            pendingBinaryFrame = nullptr;
        }
        if (streamableMutex != nullptr)
        {
            xSemaphoreGive(streamableMutex);
        }
        return waitingForQueuedChunks;
    }

    const size_t chunkLength = std::min(StreamBinaryChunkSize, frame->length - frame->offset);
    String message = BuildStreamFrameChunk(*frame, chunkLength);
    if (message.isEmpty())
    {
        delete[] frame->data;
        delete frame;
        pendingBinaryFrame = nullptr;
        ++droppedStreamMessageCount;
        if (streamableMutex != nullptr)
        {
            xSemaphoreGive(streamableMutex);
        }
        return false;
    }

    const bool queued = EnqueueStreamMessage(new (std::nothrow) String(message), 0);
    if (queued)
    {
        frame->offset += chunkLength;
        ++frame->chunkIndex;
    }
    if (streamableMutex != nullptr)
    {
        xSemaphoreGive(streamableMutex);
    }
    return queued;
}

String KliveTech::BuildStreamDefinition(const RegisteredStreamable &streamable) const
{
    JsonDocument definition;
    definition["ID"] = streamable.streamID;
    definition["VALUETYPE"] = streamable.valueType;
    definition["MIMETYPE"] = streamable.mimeType;
    definition["MODE"] = StreamModeName(streamable.mode);
    definition["INTERVALMS"] = streamable.updateIntervalMs;
    definition["ENABLED"] = streamable.enabled;
    String serialized;
    serializeJson(definition, serialized);
    return serialized;
}

String KliveTech::BuildStreamManifest() const
{
    JsonDocument manifest;
    manifest["EVENT"] = "StreamManifest";
    manifest["VERSION"] = StreamProtocolVersion;
    manifest["SESSIONID"] = streamSessionID;

    if (streamableMutex != nullptr)
    {
        xSemaphoreTake(streamableMutex, portMAX_DELAY);
    }
    manifest["REVISION"] = streamManifestRevision;
    JsonArray definitions = manifest["STREAMABLES"].to<JsonArray>();
    for (const RegisteredStreamable &streamable : streamables)
    {
        String serializedDefinition = BuildStreamDefinition(streamable);
        JsonDocument definition;
        if (!deserializeJson(definition, serializedDefinition))
        {
            definitions.add(definition.as<JsonVariantConst>());
        }
    }
    if (streamableMutex != nullptr)
    {
        xSemaphoreGive(streamableMutex);
    }

    String serialized;
    serializeJson(manifest, serialized);
    if (serialized.length() > MaximumRelayMessageSize)
    {
        return String();
    }
    return serialized;
}

String KliveTech::BuildStreamSample(
    const RegisteredStreamable &streamable,
    uint64_t sequence,
    const String &serializedJson) const
{
    String encoded;
    if (!EncodeBase64(
            reinterpret_cast<const uint8_t *>(serializedJson.c_str()),
            serializedJson.length(),
            encoded))
    {
        return String();
    }

    JsonDocument sample;
    sample["EVENT"] = "StreamSample";
    sample["VERSION"] = StreamProtocolVersion;
    sample["SESSIONID"] = streamSessionID;
    sample["STREAMID"] = streamable.streamID;
    sample["SEQUENCE"] = sequence;
    sample["TIMESTAMPMS"] = millis();
    sample["ENCODING"] = "base64-json";
    sample["DATA"] = encoded;
    String serialized;
    serializeJson(sample, serialized);
    if (serialized.length() > MaximumRelayMessageSize)
    {
        return String();
    }
    return serialized;
}

String KliveTech::BuildStreamFrameChunk(
    const PendingBinaryFrame &frame,
    size_t chunkLength) const
{
    if (frame.data == nullptr || chunkLength == 0 ||
        frame.offset > frame.length || chunkLength > frame.length - frame.offset)
    {
        return String();
    }

    const uint8_t *chunk = frame.data + frame.offset;
    String encoded;
    String chunkSha256;
    if (!EncodeBase64(chunk, chunkLength, encoded) ||
        !CalculateSha256(chunk, chunkLength, chunkSha256))
    {
        return String();
    }

    JsonDocument message;
    message["EVENT"] = "StreamFrame";
    message["VERSION"] = StreamProtocolVersion;
    message["SESSIONID"] = streamSessionID;
    message["STREAMID"] = frame.streamID;
    message["SEQUENCE"] = frame.sequence;
    message["TIMESTAMPMS"] = frame.timestampMs;
    message["MIMETYPE"] = frame.mimeType;
    message["FRAMESIZE"] = frame.length;
    message["FRAMESHA256"] = frame.frameSha256;
    message["CHUNKINDEX"] = frame.chunkIndex;
    message["CHUNKCOUNT"] = frame.chunkCount;
    message["CHUNKOFFSET"] = frame.offset;
    message["CHUNKSIZE"] = chunkLength;
    message["CHUNKSHA256"] = chunkSha256;
    message["ENCODING"] = "base64";
    message["DATA"] = encoded;
    String serialized;
    serializeJson(message, serialized);
    if (serialized.length() > MaximumRelayMessageSize)
    {
        return String();
    }
    return serialized;
}

bool KliveTech::RegisterStreamable(
    const char *streamID,
    const char *valueType,
    const char *mimeType,
    KliveTechStreamMode mode,
    unsigned long updateIntervalMs,
    std::function<String()> getter)
{
    if (streamableMutex == nullptr || !IsValidStreamID(streamID) ||
        valueType == nullptr || valueType[0] == '\0' ||
        mimeType == nullptr || mimeType[0] == '\0' ||
        strlen(mimeType) > MaximumStreamMimeTypeLength || !IsValidMimeType(mimeType) ||
        strstr(mimeType, StartCommand) != nullptr || strstr(mimeType, EndCommand) != nullptr ||
        (mode != KliveTechStreamMode::Manual && !getter))
    {
        return false;
    }

    RegisteredStreamable streamable;
    streamable.streamID = String(streamID);
    streamable.valueType = String(valueType);
    streamable.mimeType = String(mimeType);
    streamable.mode = mode;
    streamable.updateIntervalMs = mode == KliveTechStreamMode::Manual
                                      ? 0
                                      : std::max(MinimumStreamIntervalMs, updateIntervalMs);
    streamable.jsonGetter = getter;

    xSemaphoreTake(streamableMutex, portMAX_DELAY);
    if (streamables.size() >= MaximumStreamables || FindStreamable(streamable.streamID) != nullptr)
    {
        xSemaphoreGive(streamableMutex);
        return false;
    }
    size_t manifestSize = StreamManifestEnvelopeAllowance + BuildStreamDefinition(streamable).length();
    for (const RegisteredStreamable &registered : streamables)
    {
        manifestSize += BuildStreamDefinition(registered).length() + 1;
    }
    if (manifestSize > MaximumRelayMessageSize)
    {
        xSemaphoreGive(streamableMutex);
        return false;
    }
    streamables.push_back(streamable);
    ++streamManifestRevision;
    if (streamManifestRevision == 0)
    {
        streamManifestRevision = 1;
    }
    xSemaphoreGive(streamableMutex);
    return true;
}

KliveTech::RegisteredStreamable *KliveTech::FindStreamable(const String &streamID)
{
    for (RegisteredStreamable &streamable : streamables)
    {
        if (streamable.streamID.equalsIgnoreCase(streamID))
        {
            return &streamable;
        }
    }
    return nullptr;
}

bool KliveTech::IsValidStreamID(const char *streamID)
{
    if (streamID == nullptr)
    {
        return false;
    }
    const size_t length = strlen(streamID);
    if (length == 0 || length > MaximumStreamIDLength ||
        !((streamID[0] >= 'A' && streamID[0] <= 'Z') ||
          (streamID[0] >= 'a' && streamID[0] <= 'z') ||
          (streamID[0] >= '0' && streamID[0] <= '9')))
    {
        return false;
    }
    for (size_t index = 1; index < length; ++index)
    {
        const char character = streamID[index];
        if (!((character >= 'A' && character <= 'Z') ||
              (character >= 'a' && character <= 'z') ||
              (character >= '0' && character <= '9') ||
              character == '.' || character == '_' || character == '-'))
        {
            return false;
        }
    }
    return true;
}

const char *KliveTech::StreamModeName(KliveTechStreamMode mode)
{
    switch (mode)
    {
    case KliveTechStreamMode::Periodic:
        return "periodic";
    case KliveTechStreamMode::OnChange:
        return "onChange";
    case KliveTechStreamMode::Manual:
        return "manual";
    }
    return "manual";
}

void KliveTech::HandleHubWebSocketText(const uint8_t *payload, size_t length)
{
    if (payload == nullptr || length == 0 || length > MaximumRelayMessageSize)
    {
        return;
    }
    JsonDocument message;
    if (deserializeJson(message, payload, length))
    {
        return;
    }
    const char *type = message["Type"] | "";
    if (strcmp(type, "command") != 0)
    {
        return;
    }

    String targetDeviceID = message["DeviceId"].as<String>();
    JsonVariantConst payloadValue = message["Payload"];
    String command;
    if (payloadValue.is<const char *>())
    {
        command = payloadValue.as<String>();
    }
    else
    {
        serializeJson(payloadValue, command);
    }

    if (targetDeviceID.equalsIgnoreCase(deviceID))
    {
        ProcessCommand(command, ResponseTransport::HubWebSocket, nullptr, deviceID);
        return;
    }

    RelayPeer *peer = FindRelayPeerByDeviceID(targetDeviceID);
    if (peer == nullptr || !SendRelayMessage(peer->radioAddress, command))
    {
        JsonDocument commandDocument;
        int requestID = 0;
        if (!deserializeJson(commandDocument, command))
        {
            requestID = commandDocument["ID"] | 0;
        }
        JsonDocument errorData;
        errorData["Error"] = "Relay gadget is unavailable";
        SendHubResponse(
            targetDeviceID,
            BuildResponse(requestID, 503, errorData.as<JsonVariantConst>(), false));
    }
}

void KliveTech::WebSocketEvent(WStype_t type, uint8_t *payload, size_t length)
{
    if (activeInstance == nullptr || activeInstance->mode != DeviceMode::Hub)
    {
        return;
    }
    if (type == WStype_CONNECTED)
    {
        activeInstance->omnipotentConnected = true;
        activeInstance->SendHello();
        activeInstance->SendInventory();
    }
    else if (type == WStype_DISCONNECTED)
    {
        activeInstance->omnipotentConnected = false;
    }
    else if (type == WStype_TEXT)
    {
        activeInstance->HandleHubWebSocketText(payload, length);
    }
}

void KliveTech::AddEspNowPeer(const uint8_t address[6])
{
    if (esp_now_is_peer_exist(address))
    {
        return;
    }
    esp_now_peer_info_t peer{};
    memcpy(peer.peer_addr, address, sizeof(peer.peer_addr));
    peer.channel = 0;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
}

void KliveTech::EnsureBroadcastPeer()
{
    AddEspNowPeer(BroadcastAddress);
}

void KliveTech::HopRelayChannel()
{
    currentRelayChannel = currentRelayChannel >= 13 ? 1 : currentRelayChannel + 1;
    esp_wifi_set_channel(currentRelayChannel, WIFI_SECOND_CHAN_NONE);
    lastChannelHop = millis();
}

KliveTech::RelayAssembly *KliveTech::FindOrCreateAssembly(
    const uint8_t sourceAddress[6],
    const RelayPacket &packet)
{
    for (RelayAssembly &assembly : relayAssemblies)
    {
        if (assembly.messageID == packet.messageID && AddressesEqual(assembly.sourceAddress, sourceAddress))
        {
            if (assembly.fragments.size() != packet.fragmentCount)
            {
                return nullptr;
            }
            return &assembly;
        }
    }

    if (relayAssemblies.size() >= 8)
    {
        ClearStaleAssemblies();
        if (relayAssemblies.size() >= 8)
        {
            relayAssemblies.erase(relayAssemblies.begin());
        }
    }
    RelayAssembly created{};
    memcpy(created.sourceAddress, sourceAddress, sizeof(created.sourceAddress));
    created.messageID = packet.messageID;
    created.lastUpdated = millis();
    created.fragments.resize(packet.fragmentCount);
    relayAssemblies.push_back(created);
    return &relayAssemblies.back();
}

void KliveTech::ClearStaleAssemblies()
{
    const unsigned long now = millis();
    relayAssemblies.erase(
        std::remove_if(
            relayAssemblies.begin(),
            relayAssemblies.end(),
            [now](const RelayAssembly &assembly)
            {
                return now - assembly.lastUpdated > 10000;
            }),
        relayAssemblies.end());
}

KliveTech::RelayPeer *KliveTech::FindRelayPeerByRadioAddress(const uint8_t address[6])
{
    for (RelayPeer &peer : relayPeers)
    {
        if (AddressesEqual(peer.radioAddress, address))
        {
            return &peer;
        }
    }
    return nullptr;
}

KliveTech::RelayPeer *KliveTech::FindRelayPeerByDeviceID(const String &id)
{
    for (RelayPeer &peer : relayPeers)
    {
        if (peer.deviceID.equalsIgnoreCase(id))
        {
            return &peer;
        }
    }
    return nullptr;
}

String KliveTech::LocalDeviceID() const
{
    uint8_t bluetoothAddress[6] = {0};
    if (esp_read_mac(bluetoothAddress, ESP_MAC_BT) != ESP_OK)
    {
        esp_read_mac(bluetoothAddress, ESP_MAC_WIFI_STA);
    }
    return MacToID(bluetoothAddress);
}

uint32_t KliveTech::RelayNetworkHash() const
{
    uint32_t hash = 2166136261u;
    for (size_t index = 0; index < relayNetworkKey.length(); ++index)
    {
        hash ^= static_cast<uint8_t>(relayNetworkKey[index]);
        hash *= 16777619u;
    }
    return hash;
}

void KliveTech::SignRelayPacket(RelayPacket &packet) const
{
    memset(packet.authenticationTag, 0, sizeof(packet.authenticationTag));
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == nullptr)
    {
        return;
    }
    uint8_t digest[32] = {0};
    if (mbedtls_md_hmac(
            info,
            reinterpret_cast<const unsigned char *>(relayNetworkKey.c_str()),
            relayNetworkKey.length(),
            reinterpret_cast<const unsigned char *>(&packet),
            sizeof(packet),
            digest) == 0)
    {
        memcpy(packet.authenticationTag, digest, sizeof(packet.authenticationTag));
    }
}

bool KliveTech::VerifyRelayPacket(const RelayPacket &packet) const
{
    if (packet.magic != RelayMagic || packet.version != RelayProtocolVersion ||
        packet.networkHash != RelayNetworkHash())
    {
        return false;
    }
    RelayPacket signedCopy = packet;
    uint8_t receivedTag[sizeof(packet.authenticationTag)];
    memcpy(receivedTag, packet.authenticationTag, sizeof(receivedTag));
    SignRelayPacket(signedCopy);
    uint8_t difference = 0;
    for (size_t index = 0; index < sizeof(receivedTag); ++index)
    {
        difference |= receivedTag[index] ^ signedCopy.authenticationTag[index];
    }
    return difference == 0;
}

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
void KliveTech::EspNowReceive(const esp_now_recv_info_t *info, const uint8_t *data, int length)
{
    if (activeInstance == nullptr || info == nullptr || data == nullptr ||
        length != static_cast<int>(sizeof(RelayPacket)) || activeInstance->relayReceiveQueue == nullptr)
    {
        return;
    }
    QueuedRelayPacket queued{};
    memcpy(queued.sourceAddress, info->src_addr, sizeof(queued.sourceAddress));
    memcpy(&queued.packet, data, sizeof(queued.packet));
    xQueueSend(activeInstance->relayReceiveQueue, &queued, 0);
}
#else
void KliveTech::EspNowReceive(const uint8_t *sourceAddress, const uint8_t *data, int length)
{
    if (activeInstance == nullptr || sourceAddress == nullptr || data == nullptr ||
        length != static_cast<int>(sizeof(RelayPacket)) || activeInstance->relayReceiveQueue == nullptr)
    {
        return;
    }
    QueuedRelayPacket queued{};
    memcpy(queued.sourceAddress, sourceAddress, sizeof(queued.sourceAddress));
    memcpy(&queued.packet, data, sizeof(queued.packet));
    xQueueSend(activeInstance->relayReceiveQueue, &queued, 0);
}
#endif

void KliveTech::CreateActionWithIntegerParam(
    const char *actionName,
    std::function<void(int)> function,
    const char *paramDescription)
{
    RegisteredAction action;
    action.type = Integer;
    action.name = String(actionName == nullptr ? "" : actionName);
    action.paramDescription = String(paramDescription == nullptr ? "" : paramDescription);
    action.intFunction = function;
    if (actionMutex != nullptr)
    {
        xSemaphoreTake(actionMutex, portMAX_DELAY);
    }
    possibleActions.push_back(action);
    if (actionMutex != nullptr)
    {
        xSemaphoreGive(actionMutex);
    }
}

void KliveTech::CreateActionWithStringParam(
    const char *actionName,
    std::function<void(const char *)> function,
    const char *paramDescription)
{
    RegisteredAction action;
    action.type = StringParameter;
    action.name = ::String(actionName == nullptr ? "" : actionName);
    action.paramDescription = ::String(paramDescription == nullptr ? "" : paramDescription);
    action.stringFunction = function;
    if (actionMutex != nullptr)
    {
        xSemaphoreTake(actionMutex, portMAX_DELAY);
    }
    possibleActions.push_back(action);
    if (actionMutex != nullptr)
    {
        xSemaphoreGive(actionMutex);
    }
}

void KliveTech::CreateActionWithBoolParam(
    const char *actionName,
    std::function<void(bool)> function,
    const char *paramDescription)
{
    RegisteredAction action;
    action.type = Bool;
    action.name = String(actionName == nullptr ? "" : actionName);
    action.paramDescription = String(paramDescription == nullptr ? "" : paramDescription);
    action.boolFunction = function;
    if (actionMutex != nullptr)
    {
        xSemaphoreTake(actionMutex, portMAX_DELAY);
    }
    possibleActions.push_back(action);
    if (actionMutex != nullptr)
    {
        xSemaphoreGive(actionMutex);
    }
}

void KliveTech::CreateActionWithNoParam(const char *actionName, std::function<void()> function)
{
    RegisteredAction action;
    action.type = None;
    action.name = String(actionName == nullptr ? "" : actionName);
    action.noParamFunction = function;
    if (actionMutex != nullptr)
    {
        xSemaphoreTake(actionMutex, portMAX_DELAY);
    }
    possibleActions.push_back(action);
    if (actionMutex != nullptr)
    {
        xSemaphoreGive(actionMutex);
    }
}

bool KliveTech::CreateIntegerStreamable(
    const char *streamID,
    std::function<int64_t()> getter,
    unsigned long updateIntervalMs,
    KliveTechStreamMode mode)
{
    std::function<String()> serializedGetter;
    if (getter)
    {
        serializedGetter = [getter]()
        {
            char value[32];
            snprintf(value, sizeof(value), "%lld", static_cast<long long>(getter()));
            return String(value);
        };
    }
    return RegisterStreamable(
        streamID,
        "integer",
        "application/json",
        mode,
        updateIntervalMs,
        serializedGetter);
}

bool KliveTech::CreateNumberStreamable(
    const char *streamID,
    std::function<double()> getter,
    unsigned long updateIntervalMs,
    KliveTechStreamMode mode)
{
    std::function<String()> serializedGetter;
    if (getter)
    {
        serializedGetter = [getter]()
        {
            const double number = getter();
            if (!std::isfinite(number))
            {
                return String();
            }
            JsonDocument value;
            value.set(number);
            String serialized;
            serializeJson(value, serialized);
            return serialized;
        };
    }
    return RegisterStreamable(
        streamID,
        "number",
        "application/json",
        mode,
        updateIntervalMs,
        serializedGetter);
}

bool KliveTech::CreateBoolStreamable(
    const char *streamID,
    std::function<bool()> getter,
    unsigned long updateIntervalMs,
    KliveTechStreamMode mode)
{
    std::function<String()> serializedGetter;
    if (getter)
    {
        serializedGetter = [getter]()
        {
            return String(getter() ? "true" : "false");
        };
    }
    return RegisterStreamable(
        streamID,
        "boolean",
        "application/json",
        mode,
        updateIntervalMs,
        serializedGetter);
}

bool KliveTech::CreateStringStreamable(
    const char *streamID,
    std::function<String()> getter,
    unsigned long updateIntervalMs,
    KliveTechStreamMode mode)
{
    std::function<String()> serializedGetter;
    if (getter)
    {
        serializedGetter = [getter]()
        {
            JsonDocument value;
            value.set(getter());
            String serialized;
            serializeJson(value, serialized);
            return serialized;
        };
    }
    return RegisterStreamable(
        streamID,
        "string",
        "application/json",
        mode,
        updateIntervalMs,
        serializedGetter);
}

bool KliveTech::CreateJsonStreamable(
    const char *streamID,
    std::function<String()> jsonGetter,
    unsigned long updateIntervalMs,
    KliveTechStreamMode mode)
{
    return RegisterStreamable(
        streamID,
        "json",
        "application/json",
        mode,
        updateIntervalMs,
        jsonGetter);
}

bool KliveTech::CreateBinaryStreamable(const char *streamID, const char *mimeType)
{
    return RegisterStreamable(
        streamID,
        "binary",
        mimeType,
        KliveTechStreamMode::Manual,
        0,
        std::function<String()>());
}

bool KliveTech::PublishSerializedStreamable(
    const char *streamID,
    const String &serializedJson,
    const char *expectedValueType)
{
    if (!IsValidStreamID(streamID) || expectedValueType == nullptr ||
        serializedJson.isEmpty() || serializedJson.length() > MaximumStreamJsonValueSize)
    {
        ++droppedStreamMessageCount;
        return false;
    }
    JsonDocument validation;
    if (deserializeJson(validation, serializedJson))
    {
        ++droppedStreamMessageCount;
        return false;
    }

    bool exists = false;
    if (streamableMutex != nullptr)
    {
        xSemaphoreTake(streamableMutex, portMAX_DELAY);
        exists = FindStreamable(String(streamID)) != nullptr;
        xSemaphoreGive(streamableMutex);
    }
    if (!exists && !RegisterStreamable(
                       streamID,
                       expectedValueType,
                       "application/json",
                       KliveTechStreamMode::Manual,
                       0,
                       std::function<String()>()))
    {
        // Another publisher may have won the registration race. Validate the
        // resulting definition below before rejecting the sample.
        if (streamableMutex == nullptr)
        {
            ++droppedStreamMessageCount;
            return false;
        }
    }

    bool manifestRequired = false;
    xSemaphoreTake(streamableMutex, portMAX_DELAY);
    RegisteredStreamable *registered = FindStreamable(String(streamID));
    manifestRequired = registered != nullptr && registered->definitionDirty;
    xSemaphoreGive(streamableMutex);
    if (manifestRequired)
    {
        // CallLoop owns catalog scheduling. Returning false here prevents a
        // reconnect reset from draining the manifest between a publisher's
        // manifest and sample enqueue operations. The caller can retry after
        // CallLoop has announced the dirty definition.
        ++droppedStreamMessageCount;
        return false;
    }

    if (streamableMutex != nullptr)
    {
        xSemaphoreTake(streamableMutex, portMAX_DELAY);
    }
    RegisteredStreamable *streamable = FindStreamable(String(streamID));
    if (streamable == nullptr || !streamable->enabled ||
        !streamable->valueType.equals(expectedValueType) ||
        streamable->valueType.equals("binary") ||
        (streamable->mode == KliveTechStreamMode::Manual &&
         streamable->updateIntervalMs > 0 && streamable->lastChecked != 0 &&
         millis() - streamable->lastChecked < streamable->updateIntervalMs))
    {
        if (streamableMutex != nullptr)
        {
            xSemaphoreGive(streamableMutex);
        }
        ++droppedStreamMessageCount;
        return false;
    }

    uint64_t sequence = ++streamable->nextSequence;
    if (sequence == 0)
    {
        sequence = ++streamable->nextSequence;
    }
    String message = BuildStreamSample(*streamable, sequence, serializedJson);
    const bool queued = EnqueueStreamMessage(new (std::nothrow) String(message), 0);
    if (queued)
    {
        streamable->hasPublished = true;
        streamable->lastSerializedValue = serializedJson;
        streamable->lastChecked = millis();
    }
    if (streamableMutex != nullptr)
    {
        xSemaphoreGive(streamableMutex);
    }
    return queued;
}

bool KliveTech::PublishIntegerStreamable(const char *streamID, int64_t value)
{
    char serialized[32];
    snprintf(serialized, sizeof(serialized), "%lld", static_cast<long long>(value));
    return PublishSerializedStreamable(streamID, String(serialized), "integer");
}

bool KliveTech::PublishNumberStreamable(const char *streamID, double value)
{
    if (!std::isfinite(value))
    {
        ++droppedStreamMessageCount;
        return false;
    }
    JsonDocument document;
    document.set(value);
    String serialized;
    serializeJson(document, serialized);
    return PublishSerializedStreamable(streamID, serialized, "number");
}

bool KliveTech::PublishBoolStreamable(const char *streamID, bool value)
{
    return PublishSerializedStreamable(streamID, value ? String("true") : String("false"), "boolean");
}

bool KliveTech::PublishStringStreamable(const char *streamID, const String &value)
{
    JsonDocument document;
    document.set(value);
    String serialized;
    serializeJson(document, serialized);
    return PublishSerializedStreamable(streamID, serialized, "string");
}

bool KliveTech::PublishJsonStreamable(const char *streamID, const String &serializedJson)
{
    return PublishSerializedStreamable(streamID, serializedJson, "json");
}

bool KliveTech::PublishBinaryStreamable(const char *streamID, const uint8_t *data, size_t length)
{
    if (!IsValidStreamID(streamID) || data == nullptr || length == 0 ||
        length > MaximumStreamBinaryFrameSize || streamableMutex == nullptr)
    {
        ++droppedStreamMessageCount;
        return false;
    }

    bool exists = false;
    xSemaphoreTake(streamableMutex, portMAX_DELAY);
    exists = FindStreamable(String(streamID)) != nullptr;
    xSemaphoreGive(streamableMutex);
    if (!exists && !CreateBinaryStreamable(streamID))
    {
        xSemaphoreTake(streamableMutex, portMAX_DELAY);
        exists = FindStreamable(String(streamID)) != nullptr;
        xSemaphoreGive(streamableMutex);
        if (!exists)
        {
            ++droppedStreamMessageCount;
            return false;
        }
    }

    xSemaphoreTake(streamableMutex, portMAX_DELAY);
    RegisteredStreamable *availableStreamable = FindStreamable(String(streamID));
    const bool canAcceptFrame = availableStreamable != nullptr &&
                                availableStreamable->enabled &&
                                availableStreamable->valueType.equals("binary") &&
                                pendingBinaryFrame == nullptr &&
                                !(availableStreamable->updateIntervalMs > 0 &&
                                  availableStreamable->lastChecked != 0 &&
                                  millis() - availableStreamable->lastChecked <
                                      availableStreamable->updateIntervalMs);
    xSemaphoreGive(streamableMutex);
    if (!canAcceptFrame)
    {
        ++droppedStreamMessageCount;
        return false;
    }

    PendingBinaryFrame *frame = new (std::nothrow) PendingBinaryFrame();
    if (frame == nullptr)
    {
        ++droppedStreamMessageCount;
        return false;
    }
    frame->data = new (std::nothrow) uint8_t[length];
    if (frame->data == nullptr)
    {
        delete frame;
        ++droppedStreamMessageCount;
        return false;
    }
    memcpy(frame->data, data, length);
    frame->streamID = String(streamID);
    frame->length = length;
    frame->chunkCount = (length + StreamBinaryChunkSize - 1) / StreamBinaryChunkSize;
    frame->timestampMs = millis();
    if (!CalculateSha256(frame->data, frame->length, frame->frameSha256))
    {
        delete[] frame->data;
        delete frame;
        ++droppedStreamMessageCount;
        return false;
    }

    xSemaphoreTake(streamableMutex, portMAX_DELAY);
    RegisteredStreamable *streamable = FindStreamable(frame->streamID);
    if (streamable == nullptr || !streamable->enabled ||
        !streamable->valueType.equals("binary") || pendingBinaryFrame != nullptr ||
        (streamable->updateIntervalMs > 0 && streamable->lastChecked != 0 &&
         millis() - streamable->lastChecked < streamable->updateIntervalMs))
    {
        xSemaphoreGive(streamableMutex);
        delete[] frame->data;
        delete frame;
        ++droppedStreamMessageCount;
        return false;
    }
    frame->mimeType = streamable->mimeType;
    frame->sequence = ++streamable->nextSequence;
    if (frame->sequence == 0)
    {
        frame->sequence = ++streamable->nextSequence;
    }
    pendingBinaryFrame = frame;
    streamable->lastChecked = millis();
    xSemaphoreGive(streamableMutex);
    return true;
}

size_t KliveTech::GetStreamableCount() const
{
    if (streamableMutex != nullptr)
    {
        xSemaphoreTake(streamableMutex, portMAX_DELAY);
    }
    const size_t count = streamables.size();
    if (streamableMutex != nullptr)
    {
        xSemaphoreGive(streamableMutex);
    }
    return count;
}

size_t KliveTech::GetDroppedStreamMessageCount() const
{
    return droppedStreamMessageCount.load(std::memory_order_relaxed);
}

bool KliveTech::IsHub() const
{
    return mode == DeviceMode::Hub;
}

bool KliveTech::IsConnectedToHub() const
{
    return connectedToHub;
}

bool KliveTech::IsOmnipotentConnected() const
{
    return omnipotentConnected;
}

size_t KliveTech::GetConnectedGadgetCount() const
{
    return connectedGadgetCount;
}

const char *KliveTech::GetDeviceID() const
{
    return deviceID.c_str();
}
