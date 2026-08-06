#include <klivetechinterface.h>

// Use the value of Omnipotent's sensitive KliveTechHubAccessToken setting.
// Every nearby gadget must use the same RELAY_NETWORK_KEY.
constexpr char WIFI_SSID[] = "your-wifi";
constexpr char WIFI_PASSWORD[] = "your-wifi-password";
constexpr char OMNIPOTENT_TOKEN[] = "paste-KliveTechHubAccessToken-here";
constexpr char RELAY_NETWORK_KEY[] = "use-a-long-random-mesh-key";

// Root CA which signs the certificate served by your Omnipotent endpoint.
// Replace this placeholder with the PEM certificate for your deployment.
constexpr char ROOT_CA[] = R"PEM(
-----BEGIN CERTIFICATE-----
paste-root-ca-certificate-here
-----END CERTIFICATE-----
)PEM";

KliveTech kliveTech;

void setup()
{
    KliveTechHubConfig config;
    config.wifiSSID = WIFI_SSID;
    config.wifiPassword = WIFI_PASSWORD;
    config.omnipotentHost = "klive.dev";
    config.omnipotentPort = 443;
    config.accessToken = OMNIPOTENT_TOKEN;
    config.relayNetworkKey = RELAY_NETWORK_KEY;
    config.caCertificate = ROOT_CA;
    config.useTLS = true;

    if (!kliveTech.CreateKliveTechHub("KliveTech Kitchen Hub", config))
    {
        Serial.println("Could not start the KliveTech hub.");
    }

    // A hub can expose actions of its own just like any other gadget.
    kliveTech.CreateActionWithNoParam("Restart display", []()
    {
        Serial.println("Hub action executed");
    });
}

void loop()
{
    static unsigned long lastStatus = 0;
    if (millis() - lastStatus >= 5000)
    {
        lastStatus = millis();
        Serial.printf(
            "Omnipotent: %s, relayed gadgets: %u\n",
            kliveTech.IsOmnipotentConnected() ? "connected" : "offline",
            static_cast<unsigned>(kliveTech.GetConnectedGadgetCount()));
    }
    delay(50);
}
