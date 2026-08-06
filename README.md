# KliveTech Ecosystem

KliveTech lets an ESP32 expose named actions and typed live data streams to Omnipotent. A gadget can communicate directly over Bluetooth or automatically join a nearby KliveTech hub. The hub connects to Wi-Fi and relays commands, Streamables, media frames, and responses between all of its gadgets and Omnipotent over an authenticated WebSocket.

## Architecture

```text
Omnipotent API (WSS)
        |
        | authenticated hub connection
        v
KliveTech Hub (Wi-Fi + ESP-NOW)
        |
        +---- nearby gadget
        +---- nearby gadget
        +---- nearby gadget
```

- Direct gadgets retain the original framed Bluetooth protocol as a fallback.
- Hub discovery and local relay use ESP-NOW. A gadget scans Wi-Fi channels until it sees a hub with the same relay network key.
- Relay packets use a truncated HMAC-SHA256 tag and support fragmented JSON messages up to 8 KiB.
- The hub uses WSS with CA validation and a separate Omnipotent access token.
- Omnipotent treats relayed gadgets like direct gadgets, so actions, Streamables, media frames, and over-the-air firmware updates use the same API for both connection types.

## Dependencies

- An ESP32 with Classic Bluetooth support for ordinary gadgets
- ArduinoJson 7 or later
- [Links2004 WebSockets](https://github.com/Links2004/arduinoWebSockets) 2.4.1 or later
- Arduino-ESP32 with ESP-NOW support

Installing the library through Arduino will install the declared ArduinoJson and WebSockets dependencies.

The combined radio stacks require the ESP32 **Minimal SPIFFS (Large APPS with OTA)** partition scheme. In Arduino IDE select **Tools → Partition Scheme → Minimal SPIFFS**. This provides two 1.88 MiB application slots, so OTA remains available; the default 1.25 MiB application slot is too small.

## Create a normal relay-capable gadget

```cpp
#include <klivetechinterface.h>

KliveTech kliveTech;
constexpr int LIGHT_PIN = 2;

void setup() {
  kliveTech.SetRelayNetworkKey("a-long-random-key-shared-with-the-hub");
  kliveTech.CreateKliveTechGadget("KliveTech Workshop Light");
  kliveTech.CreateActionWithBoolParam(
    "Set light",
    [](bool on) { digitalWrite(LIGHT_PIN, on); },
    "Whether the light should be on");
}

void loop() {}
```

Once the relay key is configured, no hub address or Wi-Fi credentials are needed on a normal gadget. It discovers the hub, registers its stable ESP32 Bluetooth MAC as its device ID, sends a heartbeat every five seconds, and reconnects automatically.

## Designate a gadget as the hub

```cpp
KliveTechHubConfig config;
config.wifiSSID = "ssid";
config.wifiPassword = "password";
config.omnipotentHost = "klive.dev";
config.omnipotentPort = 443;
config.accessToken = "value from KliveTechHubAccessToken";
config.relayNetworkKey = "a-long-random-key-shared-with-the-gadgets";
config.caCertificate = ROOT_CA_PEM;
config.useTLS = true;

kliveTech.CreateKliveTechHub("KliveTech Kitchen Hub", config);
```

The hub itself can register actions. Omnipotent lists it with `isHub: true`, `connectionType: "Hub"`, and `connectedGadgetCount`. Gadgets behind it are listed with `connectionType: "Relayed"` and `connectedViaHubID`.

Helper functions:

- `IsHub()` — whether this device was configured as the hub.
- `IsOmnipotentConnected()` — whether the hub's WSS uplink is active.
- `GetConnectedGadgetCount()` — number of live gadgets registered with this hub.
- `IsConnectedToHub()` — whether a normal gadget currently sees a hub.
- `GetDeviceID()` — stable 12-character device identifier.

See [`examples/Hub/Hub.ino`](examples/Hub/Hub.ino), [`examples/RelayedGadget/RelayedGadget.ino`](examples/RelayedGadget/RelayedGadget.ino), and [`examples/Streamables/Streamables.ino`](examples/Streamables/Streamables.ino) for complete sketches.

## Omnipotent setup

1. Start Omnipotent once. KliveTech creates a sensitive `KliveTechHubAccessToken` global setting containing a random 256-bit value.
2. Copy that value into the hub sketch's `accessToken` field.
3. Choose a separate long random relay network key and put it in the hub and every gadget which may join it.
4. Install the root CA which signs the HTTPS certificate exposed by Omnipotent. TLS mode intentionally refuses to start without a CA certificate.
5. Flash the hub, then the gadgets. The WebSocket endpoint is `wss://<host>/klivetech/hub/connect`.

For isolated local development only, `useTLS` may be set to `false` with Omnipotent's HTTP port. Do not expose that configuration to the internet.

## Streamables

A Streamable is a typed value or binary frame declared by the gadget. Omnipotent learns the manifest automatically and updates its copy whenever the gadget publishes. The same sketch works over direct Bluetooth and through a hub; application code does not select a transport.

Register Streamables after successfully creating the gadget or hub:

```cpp
kliveTech.CreateNumberStreamable(
    "temperature",
    []() { return readTemperatureC(); },
    1000,
    KliveTechStreamMode::Periodic);

kliveTech.CreateBoolStreamable(
    "door_open",
    []() { return digitalRead(DOOR_PIN) == HIGH; },
    250,
    KliveTechStreamMode::OnChange);

kliveTech.CreateJsonStreamable(
    "status",
    []() { return String("{}"); },
    1000,
    KliveTechStreamMode::Manual);

kliveTech.CreateBinaryStreamable("camera_preview", "image/jpeg");
```

`KliveTechStreamMode` controls automatic publication:

- `Periodic` calls the getter and publishes its value on every configured interval.
- `OnChange` checks on the configured interval and publishes only when the serialized value changes.
- `Manual` does not poll the getter. Call the matching `Publish...Streamable` function when a value is ready.

The typed registration functions are `CreateIntegerStreamable`, `CreateNumberStreamable`, `CreateBoolStreamable`, `CreateStringStreamable`, and `CreateJsonStreamable`. Each has a corresponding manual publication function. A scalar or JSON publication can register a missing manual Streamable automatically, but that first call returns `false` while `CallLoop` announces the new definition; retry the value later. Explicit registration is recommended so the manifest is ready before the first value. A binary publication may be accepted into its pending slot while its definition is still dirty because `CallLoop` sends the manifest before its first chunk. `CreateBinaryStreamable` is always frame-oriented and uses `PublishBinaryStreamable`:

```cpp
JsonDocument status;
status["uptimeMs"] = millis();
status["healthy"] = true;
String json;
serializeJson(status, json);
kliveTech.PublishJsonStreamable("status", json);

// `jpegData` is one complete JPEG frame produced by a camera or encoder.
kliveTech.PublishBinaryStreamable("camera_preview", jpegData, jpegLength);
```

Getters execute on KliveTech's background task. They must finish quickly, must not wait for network activity, and must be safe to run concurrently with `loop()`. Protect multi-field application state with an appropriate critical section or mutex. Publication calls return `false` when the stream ID or value is invalid, the stream is unavailable, or bounded transport capacity cannot accept the publication. A `true` result means the value was accepted locally, not that Omnipotent has acknowledged delivery. Treat `false` as backpressure: retain or coalesce the latest state and retry later, rather than spinning in a tight retry loop.

Omnipotent can enable or disable an individual Streamable and adjust its interval through the Streamable configuration operation. The library enforces its local minimum interval even when a smaller value is requested. `GetStreamableCount()` returns the number registered on the gadget. `GetDroppedStreamMessageCount()` includes invalid or rejected publications, queue overflow, and stale telemetry coalesced during reconnect recovery, and is useful for health monitoring.

### Streamable wire behavior

Protocol version 1 uses three device events:

- `StreamManifest` contains `VERSION`, a per-boot `SESSIONID`, the manifest `REVISION`, and the `STREAMABLES` definitions. The manifest is announced again periodically so Omnipotent recovers automatically after a reconnect.
- `StreamSample` carries one typed scalar or JSON value. The serialized JSON value is base64-encoded before relay transport so arbitrary strings cannot interfere with framing.
- `StreamFrame` splits one binary publication into ordered chunks and includes both per-chunk and whole-frame SHA-256 metadata. Omnipotent exposes a frame only after all chunks arrive and every checksum matches.

A hub wraps device events in a WebSocket envelope with `Type: "stream"` and supplies the source device ID from its registered radio peer. Gadgets do not get to claim another gadget's identity in a Streamable payload.

After a transport reconnect, the library discards or coalesces stale queued telemetry, reannounces the manifest, and forces current automatic values to publish again. An accepted but incomplete binary frame restarts from chunk zero. This favors a coherent current state over treating Streamables as a durable event log.

### Omnipotent Streamables API

All Streamables HTTP and WebSocket routes require an authenticated profile with the `Klives` permission:

- `GET /klivetech/streamables?gadgetID=...` returns the discovered catalog. Omit `gadgetID` to list Streamables from every gadget.
- `GET /klivetech/streamables/history?gadgetID=...&streamID=...&limit=100&afterSequence=...` returns bounded scalar history. `limit` defaults to 100 and `afterSequence` is optional.
- `GET /klivetech/streamables/latest?gadgetID=...&streamID=...` returns the latest completed binary frame as raw bytes with its registered MIME type. Sequence, session, SHA-256, and receipt time are returned in `X-KliveTech-*` headers.
- `POST /klivetech/streamables/control` enables or disables a Streamable and can adjust its effective publication interval:

```json
{
  "gadgetID": "AABBCCDDEEFF",
  "streamID": "temperature",
  "enabled": true,
  "intervalMs": 1000
}
```

- `WS /klivetech/streamables/live?gadgetID=...&streamID=...` sends a catalog snapshot followed by live updates. `gadgetID` is required and `streamID` is optional. Live binary-frame messages contain metadata only; fetch the actual frame bytes from `/klivetech/streamables/latest`.

### Streamable limits and backpressure

- A gadget may register at most 32 Streamables. The complete manifest must also fit the universal 8 KiB device-message limit, so many definitions with unusually long IDs or MIME types may reach the aggregate byte limit before the count limit. Registration rejects the definition which would exceed either limit.
- Stream IDs are case-insensitively unique, contain at most 48 characters, start with an ASCII letter or digit, and may then contain letters, digits, `.`, `_`, or `-`.
- Automatic polling intervals have a 25 ms minimum. Omnipotent may negotiate a slower interval.
- A serialized scalar or JSON value may be at most 4 KiB.
- One binary frame may be at most 512 KiB and is transported in 1 KiB raw chunks. Base64 and JSON framing increase the radio bytes sent.
- Binary MIME types may contain at most 96 characters; use registered media types such as `image/jpeg` or `application/octet-stream`.
- The local outbound Streamable queue holds eight messages. It is deliberately bounded so an unavailable hub or Omnipotent instance cannot consume all ESP32 memory.
- Only one binary frame is staged by the gadget at a time. The library copies the complete frame into ESP32 heap before returning `true`, so choose frame sizes which leave enough RAM for the application and radio stacks. Retry later if another frame is still pending.
- The underlying ESP-NOW relay accepts logical JSON messages up to 8 KiB, using 184-byte authenticated radio fragments. A missing fragment causes that logical message to be discarded; live publishers must tolerate dropped samples or frames.
- Binary publication is a sequence of independent frames, not an unbounded byte socket. Repeated JPEG frames can provide a low-rate camera preview, but ESP-NOW is not suitable for high-bitrate or lossless video. Reduce JPEG resolution, quality, and frame rate, and always honor a `false` publication result.
- Commands, OTA traffic, and hub connectivity share the same radios. Sustained media publication should leave capacity for control traffic.

### Streamable security

WSS protects traffic between a hub and Omnipotent only when TLS is enabled with a trusted CA certificate. The current ESP-NOW relay authenticates packets with a truncated HMAC-SHA256 tag, but its peer payloads are **not encrypted**. Nearby radio observers may therefore read Streamable values and image frames, and every gadget holding the shared relay key belongs to the same trust boundary. Do not send sensitive telemetry, camera images, credentials, tokens, or personal data over the relay unless the deployment adds an encrypted radio layer. Never include Wi-Fi passwords, the Omnipotent access token, or the relay network key in a Streamable.

For a runnable example covering periodic, on-change, manual JSON, and JPEG publication without a camera or codec dependency, open [`examples/Streamables/Streamables.ino`](examples/Streamables/Streamables.ino).

## Over-the-air updates

Every gadget and hub created by this library accepts KliveTech's staged firmware update operations. The new image is written to the inactive OTA partition and is not selected for boot until every byte arrives and its SHA-256 checksum matches. On success, the gadget acknowledges completion and restarts. An interrupted, cancelled, oversized, or corrupt update leaves the running image bootable.

Omnipotent owns the compilation workflow:

1. Install `arduino-cli`, the ESP32 board core, ArduinoJson, and WebSockets on the Omnipotent host.
2. Drop a sketch folder containing its `.ino`, `.h`, `.cpp`, and other source files into the firmware inbox returned by `GET /klivetech/firmware/config` (normally `SavedData/KliveTechHub/FirmwareInbox`). As required by Arduino, the primary sketch must be a top-level `.ino` whose name matches the folder (for example, `WorkshopLight/WorkshopLight.ino`).
3. Start an update with `POST /klivetech/firmware/update`:

```json
{
  "gadgetID": "AABBCCDDEEFF",
  "project": "WorkshopLight"
}
```

Omnipotent automatically compiles the folder, locates the application image, calculates its SHA-256 digest, and transfers it in order. A gadget connected through a hub requires no different route or request. Updating the hub itself briefly disconnects every gadget relayed through that hub while it restarts.

The firmware routes require the `Klives` permission:

- `GET /klivetech/firmware/config` - inbox path and compiler defaults.
- `GET /klivetech/firmware/projects` - discover and validate dropped-in folders.
- `POST /klivetech/firmware/compile` - compile without deploying; body: `{ "project": "WorkshopLight" }`.
- `POST /klivetech/firmware/update` - compile and deploy to a gadget.
- `GET /klivetech/firmware/jobs?jobID=...` - inspect compiler output and transfer progress.
- `POST /klivetech/firmware/jobs/cancel` - cancel a queued, compiling, or uploading job; body: `{ "jobID": "..." }`.

Optional Omnipotent settings are `KliveTechArduinoCliPath`, `KliveTechFirmwareFqbn` (default `esp32:esp32:esp32`), `KliveTechFirmwarePartitionScheme` (default `min_spiffs`), and `KliveTechLibraryPath`. Leave the library path empty when KliveTech is already installed in Arduino's library search path.

## Security and limits

- The Omnipotent token and relay network key serve different purposes and should not be reused.
- Secrets are never included in gadget presence or inventory messages.
- One KliveTech instance is supported per ESP32.
- A hub supports up to 16 live gadgets and accepts inventories of at most 64 entries on the backend.
- The relay payload limit is 8 KiB; Omnipotent additionally caps WebSocket messages at 64 KiB.
- Streamable queues and media frames are bounded as described above; publication is best-effort and must not be used as a lossless event log.
- Firmware project names are direct inbox children only; projects containing links/junctions or exceeding the source limits are rejected. Compilation can execute Arduino build hooks, so only trusted administrators should place code in the inbox.
- Only one firmware update may target a gadget at a time. Omnipotent retains the latest 100 completed job records in memory and preserves build artifacts for inspection.
