// The certificate authority of YOUR MQTT broker — used only when ZDT_MQTT_TLS is 1.
// Paste its PEM between the markers (a CA certificate is public, not a secret). With it, the ESP32
// verifies the server it talks to: no man in the middle.
#pragma once
static const char* MQTT_CA_PEM = R"CA(
-----BEGIN CERTIFICATE-----
(paste your broker's CA certificate here)
-----END CERTIFICATE-----
)CA";
