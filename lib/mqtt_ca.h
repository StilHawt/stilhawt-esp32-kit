// The certificate authority of YOUR MQTT broker — a placeholder.
// ⚠ Not read yet: the firmwares call setInsecure(), so with ZDT_MQTT_TLS 1 the connection is ENCRYPTED
// but the broker is NOT verified (see « Sécurité » in the README). Verifying it is the next change.
#pragma once
static const char* MQTT_CA_PEM = R"CA(
-----BEGIN CERTIFICATE-----
(paste your broker's CA certificate here)
-----END CERTIFICATE-----
)CA";
