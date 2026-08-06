#include <ArduinoJson.h>
#include <klivetechinterface.h>

KliveTech kliveTech;

// This is a valid 1 x 1 grayscale JPEG. It keeps the example independent of a
// camera driver or JPEG encoder. A camera sketch can pass its frame buffer to
// PublishBinaryStreamable in exactly the same way.
static const uint8_t exampleJpeg[] = {
    0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01,
    0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0xFF, 0xDB, 0x00, 0x43,
    0x00, 0x0A, 0x07, 0x07, 0x08, 0x07, 0x06, 0x0A, 0x08, 0x08, 0x08, 0x0B,
    0x0A, 0x0A, 0x0B, 0x0E, 0x18, 0x10, 0x0E, 0x0D, 0x0D, 0x0E, 0x1D, 0x15,
    0x16, 0x11, 0x18, 0x23, 0x1F, 0x25, 0x24, 0x22, 0x1F, 0x22, 0x21, 0x26,
    0x2B, 0x37, 0x2F, 0x26, 0x29, 0x34, 0x29, 0x21, 0x22, 0x30, 0x41, 0x31,
    0x34, 0x39, 0x3B, 0x3E, 0x3E, 0x3E, 0x25, 0x2E, 0x44, 0x49, 0x43, 0x3C,
    0x48, 0x37, 0x3D, 0x3E, 0x3B, 0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x11, 0x00, 0xFF, 0xC4, 0x00, 0x14, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x07, 0xFF, 0xC4, 0x00, 0x14, 0x10, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3F, 0x00,
    0x19, 0x7F, 0xFF, 0xD9};

volatile bool doorOpen = false;
bool gadgetStarted = false;
unsigned long lastDoorToggle = 0;
unsigned long lastStatusPublish = 0;
unsigned long lastPreviewPublish = 0;

double readSyntheticTemperature()
{
    // Replace this with a sensor read. Periodic mode publishes every second,
    // even when the returned value has not changed.
    return 20.0 + static_cast<double>((millis() / 1000) % 40) / 10.0;
}

void publishStatusJson()
{
    JsonDocument status;
    status["uptimeMs"] = millis();
    status["doorOpen"] = doorOpen;
    status["connectedToHub"] = kliveTech.IsConnectedToHub();
    status["droppedStreamMessages"] = kliveTech.GetDroppedStreamMessageCount();

    String serialized;
    serializeJson(status, serialized);
    if (!kliveTech.PublishJsonStreamable("status", serialized))
    {
        Serial.println("Status was not queued; it can be retried later.");
    }
}

void setup()
{
    Serial.begin(115200);

    kliveTech.SetRelayNetworkKey("replace-with-the-hub-relay-network-key");
    if (!kliveTech.CreateKliveTechGadget("KliveTech Streamables Demo"))
    {
        Serial.println("Could not start the KliveTech gadget.");
        return;
    }
    gadgetStarted = true;

    bool registered = true;
    registered &= kliveTech.CreateNumberStreamable(
        "temperature",
        readSyntheticTemperature,
        1000,
        KliveTechStreamMode::Periodic);

    registered &= kliveTech.CreateBoolStreamable(
        "door_open",
        []() { return doorOpen; },
        250,
        KliveTechStreamMode::OnChange);

    // Manual mode does not call the getter. PublishJsonStreamable supplies each
    // value when the application has a new JSON object ready.
    registered &= kliveTech.CreateJsonStreamable(
        "status",
        []() { return String("{}"); },
        1000,
        KliveTechStreamMode::Manual);

    registered &= kliveTech.CreateBinaryStreamable(
        "camera_preview",
        "image/jpeg");

    if (!registered)
    {
        Serial.println("One or more streamables could not be registered.");
    }
}

void loop()
{
    if (!gadgetStarted)
    {
        delay(1000);
        return;
    }

    const unsigned long now = millis();

    // Simulate a digital input changing. OnChange mode only publishes when the
    // getter's serialized value differs from its previous value.
    if (now - lastDoorToggle >= 7000)
    {
        lastDoorToggle = now;
        doorOpen = !doorOpen;
    }

    if (now - lastStatusPublish >= 5000)
    {
        lastStatusPublish = now;
        publishStatusJson();
    }

    if (now - lastPreviewPublish >= 30000)
    {
        lastPreviewPublish = now;
        if (!kliveTech.PublishBinaryStreamable(
                "camera_preview",
                exampleJpeg,
                sizeof(exampleJpeg)))
        {
            Serial.println("JPEG was rejected or another binary frame is still pending.");
        }
    }

    delay(10);
}
