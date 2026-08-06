#include <klivetechinterface.h>

constexpr char RELAY_NETWORK_KEY[] = "use-a-long-random-mesh-key";
constexpr int LIGHT_PIN = 2;

KliveTech kliveTech;

void setup()
{
    // This must match the hub. Discovery and reconnection are automatic.
    kliveTech.SetRelayNetworkKey(RELAY_NETWORK_KEY);
    kliveTech.CreateKliveTechGadget("KliveTech Workshop Light");

    kliveTech.CreateActionWithBoolParam(
        "Set light",
        [](bool enabled)
        {
            digitalWrite(LIGHT_PIN, enabled ? HIGH : LOW);
        },
        "Whether the light should be on");

    pinMode(LIGHT_PIN, OUTPUT);
}

void loop()
{
    // KliveTech runs communications and actions on background FreeRTOS tasks.
    // The gadget retains Bluetooth as a fallback when no hub is reachable.
    delay(1000);
}
