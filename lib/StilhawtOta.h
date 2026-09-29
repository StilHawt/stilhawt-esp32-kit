// stilhawt-things shared — OTA standard : récepteur + écran "OTA EN COURS"
// avec nom du firmware + version, JAMAIS effacé pendant le flash.
//
// Principe : tout firmware du kit embarque l'OTA (mise a jour sans cable).
// Extension actée user 2026-06-12 : "un bout de code dans le firmware de base
// pour indiquer OTA en cours, avec le nom du prog/version. Jamais effacé."
//
// Display-AGNOSTIQUE : le firmware passe un callback de dessin (ou rien →
// Serial seul, pour les leaves sans écran). Le "jamais effacé" tient à DEUX
// mécanismes : (1) pendant le transfert, ArduinoOTA.handle() bloque le loop
// (rien ne peut redessiner) ; (2) le garde stilhawtOtaActive() court-circuite les
// renders applicatifs sur les phases début/fin/erreur — à mettre en tête de
// loop() :
//
//   #include <StilhawtOta.h>
//   void setup() { ... if (wifi_ok) stilhawtOtaBegin("pin-probe", FW_NAME, FW_VERSION, otaDraw); }
//   void loop()  { stilhawtOtaHandle(); if (stilhawtOtaActive()) return;  ... renders ... }
//
// FW_NAME / FW_VERSION : à définir dans platformio.ini :
//   -DFW_NAME='"pin-probe"'  -DFW_VERSION='"1.2.0"'
// Upload : pio run -t upload --upload-port <hostname>.local   (espota :3232)
//
// ⚠ GOTCHA PlatformIO : le LDF ne scanne PAS les headers hors projet →
// le main.cpp consommateur DOIT inclure <ArduinoOTA.h> AVANT <StilhawtOta.h>
// (sinon "ArduinoOTA.h: No such file"). Et platformio.ini doit porter
// `-I../../../shared` dans build_flags.
#pragma once

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <functional>

// drawFn(phase, nom, version, pct) — pct ∈ [0..100], -1 = pas de barre (erreur)
using StilhawtOtaDraw = std::function<void(const char* phase, const char* name,
                                       const char* version, int pct)>;

// static (pas inline : C++17 requis, le core Arduino ESP32 est souvent en
// gnu++11) — chaque firmware = 1 TU, pas de duplication réelle
namespace stilhawt_ota_detail {
static bool active = false;
static StilhawtOtaDraw draw = nullptr;
static const char* name = "?";
static const char* version = "?";
static int last_pct = -1;
}  // namespace stilhawt_ota_detail

inline bool stilhawtOtaActive() { return stilhawt_ota_detail::active; }

inline void stilhawtOtaBegin(const char* hostname, const char* fw_name,
                         const char* fw_version, StilhawtOtaDraw draw_fn = nullptr) {
  using namespace stilhawt_ota_detail;
  draw = draw_fn;
  name = fw_name;
  version = fw_version;
  ArduinoOTA.setHostname(hostname);
  ArduinoOTA.onStart([]() {
    using namespace stilhawt_ota_detail;
    active = true;
    last_pct = -1;
    Serial.printf("[ota] START %s %s\n", name, version);
    if (draw) draw("OTA EN COURS", name, version, 0);
  });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
    using namespace stilhawt_ota_detail;
    int pct = t ? (int)((uint64_t)p * 100 / t) : 0;
    if (pct != last_pct) {     // redraw seulement quand le % change
      last_pct = pct;
      if (draw) draw("OTA EN COURS", name, version, pct);
    }
  });
  ArduinoOTA.onEnd([]() {
    using namespace stilhawt_ota_detail;
    Serial.println("[ota] OK, reboot");
    if (draw) draw("OTA OK - reboot", name, version, 100);
  });
  ArduinoOTA.onError([](ota_error_t e) {
    using namespace stilhawt_ota_detail;
    active = false;            // on rend l'écran à l'application
    Serial.printf("[ota] ERREUR %u\n", (unsigned)e);
    if (draw) draw("OTA ERREUR", name, version, -1);
  });
  ArduinoOTA.begin();
  Serial.printf("[ota] pret (%s.local:3232) — %s %s\n",
                hostname, fw_name, fw_version);
}

inline void stilhawtOtaHandle() { ArduinoOTA.handle(); }
