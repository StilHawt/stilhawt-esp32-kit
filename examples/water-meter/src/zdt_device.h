// Configuration of THIS device — edit before building.
#pragma once
#define ZDT_DEVICE_ID  "water-meter"            // the name it announces (mDNS, web page, MQTT)
#define ZDT_TOPIC_BASE "home/water-meter"       // MQTT topics: <base>/state, <base>/status, ...
#define ZDT_MQTT_HOST  "mqtt.local"             // your broker (host name or IP)
#define ZDT_MQTT_PORT  1883                     // 8883 with TLS
#define ZDT_MQTT_TLS   0                        // 1 = verify the broker with lib/mqtt_ca.h
