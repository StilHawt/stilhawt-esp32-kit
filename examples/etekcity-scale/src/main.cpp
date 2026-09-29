// ============================================================================
//  etekcity-scale — firmware stilhawt-things (ESP32 WROOM, headless)
//  Configuration : src/zdt_device.h (nom, topics, broker).
//
//  Pont BLE -> MQTT pour une balance a impedance Etekcity FIT-8S en salle de bain.
//  La balance est BROADCAST-ONLY : elle DIFFUSE poids + impedance dans son
//  advertisement (manufacturer data, company id 0x06D0). On ecoute en PASSIF
//  (les donnees sont dans l'advert, pas besoin de connexion GATT) et on publie.
//  La composition corporelle (masse grasse, age metabolique) est calculee
//  DOWNSTREAM (par qui lit le topic MQTT) : ici on ne fait que poids + impedance BRUTS.
//
//  Decodage (manufacturer data COMPLETE, company id LE inclus -> payload + 2) :
//    [0..1]   company id LE (D0 06 = 0x06D0)
//    [12..14] poids en grammes, LE 24 bits   (0x0148B6 = 84150 -> 84.15 kg)
//    [15..16] impedance en ohms, LE 16 bits  (0x020E   = 526)
//    [17]     flag "impedance prete" (1 = mesuree)
//  Protocole reverse-engineere 17/08/2026 valide
//  <0,5 % vs l'app VeSync officielle.
//
//  NB coexistence : scan BLE PASSIF (leger) + WiFi/MQTT partagent la radio 2.4 GHz.
//  Le controleur ESP-IDF gere la coexistence ; le scan passif ne sature pas le WiFi.
//
//  Compile par PlatformIO (examples/etekcity-scale/platformio.ini), a flasher sur un ESP32 WROOM.
// ============================================================================
#include <WiFi.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <WiFiMulti.h>
#include <math.h>
#include <time.h>           // NTP + horodatage du journal de pesees
#include <ArduinoOTA.h>     // DOIT preceder StilhawtOta.h (le LDF ne scanne pas shared/)
#include "StilhawtOta.h"
#include <WebServer.h>      // DOIT preceder StilhawtWeb.h (le LDF ne scanne pas shared/ — meme piege que StilhawtOta)
#include <DNSServer.h>      // idem, AVANT StilhawtWifi.h (portail captif)
#include "StilhawtWeb.h"        // mini interface web locale (shared/) — standalone-first
#include "StilhawtWifi.h"       // WiFi non-bloquant + LED d'etat + portail captif (shared/)
#include "zdt_device.h"     // ZTD-GENERE : ZDT_DEVICE_ID, ZDT_MQTT_HOST/PORT/TLS
#include <NimBLEDevice.h>
#if ZDT_MQTT_TLS
#include <WiFiClientSecure.h>
#include "mqtt_ca.h"
#endif

static const char* DEVICE_ID = ZDT_DEVICE_ID;

// ─── BASCULE DE TOPICS : DEUX CRENEAUX (0.4.x), BASE NEUVE SEULE en 0.4.4 ────
// Close le 15/09/2026 : plus aucun lecteur de `stilhawt-things/…`, `base_legacy` est sorti du
// DSL. Le mecanisme reste, pour une prochaine migration.
// Les topics etaient ecrits "stilhawt-things/%s/..." au snprintf, alors que
// `ZDT_TOPIC_BASE` etait DEJA genere depuis le DSL. La declaration existait et ne liait
// rien : renommer dans le DSL n'avait aucun effet sur la puce. On consomme desormais la
// declaration, et la MIGRATION devient une donnee du DSL (`mqtt.base_legacy`), pas un
// patch dans le code.
//
// Tant que `base_legacy` est declare, on publie sur les DEUX bases et on s'abonne aux
// DEUX : a aucun instant le device n'est injoignable, et les consommateurs basculent a
// leur rythme. On retire la ligne du DSL — et on reflashe — quand ils ont tous bascule.
#ifdef ZDT_TOPIC_BASE_LEGACY
static const char* const TOPIC_BASES[] = { ZDT_TOPIC_BASE, ZDT_TOPIC_BASE_LEGACY };
#  define ZDT_LWT_BASE ZDT_TOPIC_BASE_LEGACY
#else
static const char* const TOPIC_BASES[] = { ZDT_TOPIC_BASE };
#  define ZDT_LWT_BASE ZDT_TOPIC_BASE
#endif
static const uint8_t N_BASES = sizeof(TOPIC_BASES) / sizeof(TOPIC_BASES[0]);

// ⚠ LE TESTAMENT (LWT) NE PEUT PAS ETRE DOUBLE : MQTT n'en accepte qu'UN par connexion.
// On le pose donc sur la base HERITEE tant qu'elle existe — ce sont les consommateurs
// actuels qui doivent voir le `offline`. Quand `base_legacy` disparait du DSL, le
// testament suit AUTOMATIQUEMENT la base neuve. La contrainte du protocole est traitee,
// pas contournee.
// (Les helpers `pubAll` vivent apres la declaration du client MQTT, plus bas.)
static const char* NTP_TZ    = "CET-1CEST,M3.5.0,M10.5.0/3";   // Europe/Paris — horodatage du journal

// ── decode advert ────────────────────────────────────────────────────────────
static const uint16_t CID        = 0x06D0;  // Etekcity / VeSync
static const int      OFF_WEIGHT = 12;      // payload[10] (+2 pour le company id)
static const int      OFF_IMP    = 15;      // payload[13]
static const int      OFF_READY  = 17;      // payload[15]
static const int      MFR_MIN    = 18;
static const uint32_t DEDUP_MS   = 20000;   // une pesee = un message

// ── etat ─────────────────────────────────────────────────────────────────────
#if ZDT_MQTT_TLS
WiFiClientSecure net;
#else
WiFiClient net;
#endif
PubSubClient mqtt(net);

// Publie le MEME message sur toutes les bases declarees (cf. bascule deux creneaux).
// Un seul point de formatage : plus aucun topic n'est ecrit en dur ailleurs.
static void pubAll(const char* suffixe, const uint8_t* p, size_t n, bool retain) {
  char t[80];
  for (uint8_t i = 0; i < N_BASES; i++) {
    snprintf(t, sizeof(t), "%s/%s", TOPIC_BASES[i], suffixe);
    mqtt.publish(t, p, n, retain);
  }
}

static void pubAll(const char* suffixe, const char* s, bool retain) {
  pubAll(suffixe, (const uint8_t*)s, strlen(s), retain);
}
Preferences  nvs;
StilhawtWifi     zwifi;                 // WiFi non-bloquant + LED d'etat (GPIO2) + portail captif (shared)
bool         g_services = false;    // OTA + IHM demarres UNE fois, quand le wifi passe connecte
StilhawtWeb      web;                   // interface web locale (regle stilhawt-things : obligatoire)
static const int MAX_AP = 4;        // nombre max de points d'acces memorises (schema NVS partage)
char     mqttUser[40] = {0};
char     mqttPass[64] = {0};
uint32_t boots = 0;

// mesure decodee par la tache NimBLE -> consommee par loop(). Garde portMUX (cross-task).
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
float    g_kg = 0.0f;
uint16_t g_imp = 0;
bool     g_pending = false;
volatile uint32_t g_adverts = 0;   // debug coexistence : compte TOUS les adverts BLE vus

float    lastPubKg = -1.0f;
uint16_t lastPubImp = 0;
uint32_t lastPubMs = 0;

// ── DUTY-CYCLE des radios (radio_schedule du DSL) ────────────────────────────
// NimBLE (~45 Ko) et mbedtls/TLS (~40 Ko) ne tiennent PAS ensemble sur ce WROOM (crash 0.3.6 = OOM).
// On alterne : phase LISTEN (BLE actif, MQTT deconnecte -> mbedtls non alloue) et phase PUBLISH
// (BLE DEINIT -> heap libre -> MQTT/TLS up -> vide le buffer -> MQTT down -> BLE re-init). Le WiFi
// reste UP (IHM/OTA joignables). Une pesee capturee est persistee en NVS (survit reboot/offline).
enum Phase { PH_LISTEN, PH_PUBLISH };
Phase    g_phase   = PH_LISTEN;
uint32_t g_phaseT0 = 0;
static const uint32_t LISTEN_MS  = 90000;   // ecoute avant une fenetre publish (borne par liveness<120s du DSL)
static const uint32_t PUBLISH_MS = 20000;   // budget max de la fenetre publish -> heartbeat toutes ~110s
NimBLEScan* g_bleScan = nullptr;

// ── MODE OTA (fenetre pure-WiFi) ──────────────────────────────────────────────
// Piege coex : sur un device BLE+WiFi, WiFi.setSleep(false) crashe (coex_core_enable abort). Or l'entree
// (OTA :3232) N'EST fiable QUE sans power-save. Parade : la commande MQTT {"ota":true} ecrit un flag NVS
// + reboot ; au boot, si le flag est mis, on N'INITIALISE JAMAIS le BLE -> coex absente -> setSleep(false)
// devient sur -> fenetre OTA fiable -> flash -> reboot vers le mode normal (BLE repart neuf). Aucun
// teardown BT a chaud (le point fragile). One-shot : le flag est efface au boot (OTA rate -> normal ensuite).
bool     g_otaMode     = false;
uint32_t g_otaWindowT0 = 0;
uint32_t g_bootMs      = 0;
static const uint32_t OTA_WINDOW_MS   = 120000;   // apres connexion WiFi : temps laisse pour recevoir l'OTA
static const uint32_t OTA_MODE_MAX_MS = 180000;   // garde-fou absolu : jamais coince en mode OTA

// JOURNAL des pesees (offline_buffer.store=nvs, max:30) : ring NVS {ts, kg, imp}, HORODATE (NTP),
// purge par age (retention). Survit reboot/offline ; expose via HTTP /journal + etat MQTT (derniere).
// -> plus rien de perdu (duty-cycle/reboot), et l'utilisateur VOIT quoi a ete capte et quand.
struct Weigh { uint32_t ts; float kg; uint16_t imp; };
static const int      JMAX = 30;
static const uint32_t J_RETENTION_S = 30UL * 24 * 3600;   // 30 jours
Weigh g_jlog[JMAX];
int   g_jHead = 0, g_jCount = 0;
bool  g_bufHas = false;                          // une pesee EN ATTENTE de publication MQTT
Weigh g_bufLast = {0, 0, 0};                     // derniere pesee (pour l'etat MQTT)

void jLoad() {                                   // au boot : recharge le journal + le flag "en attente"
  nvs.begin("scale", true);
  g_jCount = nvs.getInt("jn", 0); g_jHead = nvs.getInt("jh", 0);
  if (nvs.getBytes("jlog", g_jlog, sizeof(g_jlog)) != sizeof(g_jlog)) {
    memset(g_jlog, 0, sizeof(g_jlog)); g_jCount = 0; g_jHead = 0;
  }
  g_bufHas = nvs.getBool("buf_has", false);
  nvs.end();
  if (g_jCount > 0) g_bufLast = g_jlog[(g_jHead - 1 + JMAX) % JMAX];
}
void jAppend(float kg, uint16_t imp) {           // ajoute une pesee au ring (horodatee) + persiste
  uint32_t ts = (uint32_t)time(nullptr); if (ts < 1700000000UL) ts = 0;   // 0 = heure inconnue (NTP pas synchro)
  Weigh w = {ts, kg, imp};
  g_jlog[g_jHead] = w; g_jHead = (g_jHead + 1) % JMAX; if (g_jCount < JMAX) g_jCount++;
  g_bufLast = w; g_bufHas = true;
  nvs.begin("scale", false);
  nvs.putBytes("jlog", g_jlog, sizeof(g_jlog));
  nvs.putInt("jn", g_jCount); nvs.putInt("jh", g_jHead); nvs.putBool("buf_has", true);
  nvs.end();
}
void jPublished() {                              // pesee publiee sur MQTT -> plus "en attente" (reste au journal)
  g_bufHas = false; nvs.begin("scale", false); nvs.putBool("buf_has", false); nvs.end();
}
String jJson() {                                 // journal du plus RECENT au plus ancien, purge > retention
  uint32_t now = (uint32_t)time(nullptr);
  String o = "["; int shown = 0;
  for (int i = 0; i < g_jCount; i++) {
    Weigh& w = g_jlog[(g_jHead - 1 - i + 2 * JMAX) % JMAX];
    if (w.ts && now > 1700000000UL && now - w.ts > J_RETENTION_S) continue;  // trop vieux -> purge
    if (shown++) o += ",";
    o += "{\"ts\":"; o += w.ts; o += ",\"kg\":"; o += String(w.kg, 2); o += ",\"imp\":"; o += w.imp; o += "}";
  }
  o += "]"; return o;
}

void bleStart();   // fwd
void bleStop();    // fwd

// ── callback BLE : filtre company 0x06D0, decode, marque une mesure prete ─────
class ScaleCallbacks : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* dev) override {
    g_adverts++;                                      // debug : tout advert vu (diagnostic coex)
    if (!dev->haveManufacturerData()) return;
    std::string md = dev->getManufacturerData();
    if (md.size() < (size_t)MFR_MIN) return;
    const uint8_t* b = (const uint8_t*)md.data();
    uint16_t cid = b[0] | (b[1] << 8);
    if (cid != CID) return;
    uint32_t wg  = (uint32_t)b[OFF_WEIGHT] | ((uint32_t)b[OFF_WEIGHT + 1] << 8) | ((uint32_t)b[OFF_WEIGHT + 2] << 16);
    uint16_t imp = (uint16_t)b[OFF_IMP] | ((uint16_t)b[OFF_IMP + 1] << 8);
    uint8_t  ready = b[OFF_READY];
    if (wg == 0 || wg > 300000) return;              // garde-fou 0-300 kg
    zwifi.notifyData();                              // donnee balance recue -> double-blink LED (regle stilhawt-things)
    if (ready == 1 && imp > 0) {                      // mesure stabilisee uniquement
      portENTER_CRITICAL(&g_mux);
      g_kg = wg / 1000.0f; g_imp = imp; g_pending = true;
      portEXIT_CRITICAL(&g_mux);
    }
  }
};

// ── wifi multi-AP / mqtt (provisionnes en NVS ; console serie de secours) ─────
// Stockage : namespace "stilhawt_wifi", cle "n" = nombre d'AP, ssid0..N / pass0..N.

// WiFi (connexion NON-BLOQUANTE + LED d'etat + portail captif) = StilhawtWifi (shared/StilhawtWifi.h).
// La migration v0.1 + le chargement multi-AP (ssid0..) y sont faits ; le schema NVS reste
// partage avec saveAP() (serie/web). Ici on ne charge que les creds MQTT.
void setupCreds() {
  nvs.begin("scale", true);
  nvs.getString("mqtt_user", mqttUser, sizeof(mqttUser));
  nvs.getString("mqtt_pass", mqttPass, sizeof(mqttPass));
  nvs.end();
#if ZDT_MQTT_TLS
  net.setInsecure();
#endif
}

// Ajoute/maj un AP dans la liste NVS (multi). doReboot=true (web, single) applique tout de suite ;
// false (serie/batch) accumule sans reboot -> `REBOOT` applique une fois a la fin.
void saveAP(const String& s, const String& p, bool doReboot = true) {
  if (!s.length()) return;
  nvs.begin("stilhawt_wifi", false);
  int n = nvs.getInt("n", 0), idx = -1;
  for (int i = 0; i < n; i++) { char k[8]; snprintf(k, sizeof(k), "ssid%d", i);
    if (nvs.getString(k, "") == s) { idx = i; break; } }          // maj si SSID deja connu
  if (idx < 0 && n < MAX_AP) { idx = n; nvs.putInt("n", ++n); }
  if (idx >= 0) { char ks[8], kp[8];
    snprintf(ks, sizeof(ks), "ssid%d", idx); snprintf(kp, sizeof(kp), "pass%d", idx);
    nvs.putString(ks, s); nvs.putString(kp, p); nvs.end();
    Serial.printf("AP #%d '%s' enregistre (%d au total)%s\n", idx, s.c_str(), n,
                  doReboot ? ", reboot" : " — REBOOT pour appliquer");
    if (doReboot) ESP.restart(); }
  else { nvs.end(); Serial.printf("liste AP pleine (max %d)\n", MAX_AP); }
}

void publishManifest() {
  if (!mqtt.connected()) return;
  StaticJsonDocument<256> d;
  d["id"] = DEVICE_ID; d["kind"] = "etekcity-scale";
  d["fw"] = String(FW_NAME) + " " + FW_VERSION;
  d["ip"] = WiFi.localIP().toString();
  JsonArray ch = d.createNestedArray("channels"); ch.add("kg"); ch.add("imp");
  char buf[256]; size_t n = serializeJson(d, buf);
  pubAll("manifest", (const uint8_t*)buf, n, true);
}

// Topic `state` (retenu) : porte les mesures (si dispo) ET les signes vitaux. Publie
// periodiquement ET sur chaque pesee. mqtt2prometheus s'abonne a `+/state`.
void publishState() {
  if (!mqtt.connected()) return;
  StaticJsonDocument<256> d;
  if (lastPubKg > 0) { d["kg"] = roundf(lastPubKg * 100) / 100.0f; d["imp"] = lastPubImp; }
  d["uptime_s"] = (uint32_t)(millis() / 1000);
  d["boots"]    = boots;
  d["rssi"]     = WiFi.RSSI();
  d["ssid"]     = WiFi.SSID();          // QUEL AP (diagnostic coex/CPL a distance, sans HTTP)
  d["uplink"]   = zwifi.uplinkStr();    // uplink prouve ? (ok / no-uplink / unknown)
  char buf[256]; size_t n = serializeJson(d, buf);
  pubAll("state", (const uint8_t*)buf, n, true);
}

// Battement (cadence liveness du DSL) : signes vitaux, dead-man's switch cote flotte.
void publishHeartbeat() {
  if (!mqtt.connected()) return;
  StaticJsonDocument<160> d;
  d["uptime_s"] = (uint32_t)(millis() / 1000);
  d["boots"]    = boots;
  d["rssi"]     = WiFi.RSSI();
  d["fw"]       = FW_VERSION;
  char buf[160]; size_t n = serializeJson(d, buf);
  pubAll("heartbeat", (const uint8_t*)buf, n, false);
}

// Topic `connlog` (RETENU) : le journal de connexion de StilhawtWifi {ts,ssid,rssi,event} pousse sur MQTT.
// Sur un device BLE (coex), l'HTTP /connlog est peu fiable -> le canal MQTT (sortie) est la vraie voie
// pour amener l'historique de connexion vers une DB downstream, malgre la coex. Publie a chaque fenetre.
void publishConnlog() {
  if (!mqtt.connected()) return;
  String j = zwifi.connLogJson();
  if (j.length() + 32 > 1536) return;                // ne deborde pas le buffer MQTT (setBufferSize)
  pubAll("connlog", (const uint8_t*)j.c_str(), j.length(), true);
}

// Commande entrante. Vocabulaire FERME : {"identify":true} (trace serie) et {"ota":true} (fenetre OTA).
// Un device qui executerait ce qu'on lui envoie = l'agent sur-permissionne qu'on s'interdit : rien d'autre.
void onCmd(char* topic, uint8_t* payload, unsigned int len) {
  StaticJsonDocument<128> d;
  if (deserializeJson(d, payload, len)) return;
  if (d["identify"].is<bool>() && d["identify"].as<bool>())
    Serial.printf("[identify] je suis %s @ %s\n", DEVICE_ID, WiFi.localIP().toString().c_str());
  if (d["ota"].is<bool>() && d["ota"].as<bool>()) {
    // Reboot en MODE OTA : flag NVS + restart. Au boot, BLE non init -> setSleep(false) sur -> OTA fiable.
    Serial.println("[ota] demande recue -> reboot en mode OTA (BLE off, WiFi no-sleep, fenetre ~120s)");
    nvs.begin("scale", false); nvs.putBool("ota_mode", true); nvs.end();
    // EFFACE la cmd retenue sur TOUTES les bases -> pas de re-declenchement en boucle,
    // y compris depuis la base heritee tant qu'elle est declaree.
    pubAll("cmd", (const uint8_t*)"", 0, true);
    pubAll("status", "ota-mode", true); mqtt.loop(); delay(300);
    ESP.restart();
  }
}

void mqttReconnect() {
  if (WiFi.status() != WL_CONNECTED || mqtt.connected()) return;
  if (mqttUser[0] == 0) return;                     // pas de creds -> on n'essaie pas
  mqtt.setServer(ZDT_MQTT_HOST, ZDT_MQTT_PORT);
  mqtt.setCallback(onCmd);
  // LWT : le broker publie `offline` retenu si l'ESP disparait -> seule detection instantanee.
  // ⚠ UN SEUL testament possible par connexion (contrainte MQTT, pas un choix) : on le pose
  // sur la base HERITEE tant qu'elle est declaree — ce sont les consommateurs ACTUELS qui
  // doivent voir le `offline`. Le jour ou `base_legacy` sort du DSL, `ZDT_LWT_BASE` bascule
  // tout seul sur la base neuve : la transition n'a pas de geste manuel a oublier.
  char st[80]; snprintf(st, sizeof(st), "%s/status", ZDT_LWT_BASE);
  if (mqtt.connect(DEVICE_ID, mqttUser, mqttPass, st, 1, true, "offline")) {
    pubAll("status", "online", true);
    // On s'abonne aux cmd des DEUX bases : une commande envoyee sur l'ancienne comme sur
    // la neuve atteint le device. C'est ce qui rend la bascule non destructive — a aucun
    // instant on ne perd le canal de commande, donc jamais de retour physique.
    char ct[80];
    for (uint8_t i = 0; i < N_BASES; i++) {
      snprintf(ct, sizeof(ct), "%s/cmd", TOPIC_BASES[i]);
      mqtt.subscribe(ct);
    }
    publishManifest();
    publishHeartbeat();
  }
}

// ── console serie : provisioning wifi/mqtt (secrets jamais dans le firmware) ──
void pumpSerial() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (line.startsWith("WIFI LIST")) {             // liste les AP memorises (sans mdp)
        nvs.begin("stilhawt_wifi", true);
        int n = nvs.getInt("n", 0);
        Serial.printf("%d AP(s) :\n", n);
        for (int i = 0; i < n; i++) { char k[8]; snprintf(k, sizeof(k), "ssid%d", i);
          Serial.printf("  #%d %s\n", i, nvs.getString(k, "").c_str()); }
        nvs.end();
      } else if (line == "WIFIOFF") {                 // test coexistence : radio 2.4G au BLE seul
        WiFi.disconnect(true); WiFi.mode(WIFI_OFF); Serial.println("wifi OFF -> BLE seul");
      } else if (line == "REBOOT") {                  // applique les AP provisionnes en batch
        Serial.println("reboot..."); delay(50); ESP.restart();
      } else if (line.startsWith("WIFI ")) {          // WIFI <ssid> <pass> : ajoute/maj un AP (batch, pas de reboot)
        int sp = line.indexOf(' ', 5);
        if (sp > 0) saveAP(line.substring(5, sp), line.substring(sp + 1), false);
      } else if (line.startsWith("MQTT ")) {          // MQTT <user> <pass>
        String rest = line.substring(5); int sp = rest.indexOf(' ');
        if (sp > 0) { nvs.begin("scale", false);
          nvs.putString("mqtt_user", rest.substring(0, sp));
          nvs.putString("mqtt_pass", rest.substring(sp + 1)); nvs.end();
          rest.substring(0, sp).toCharArray(mqttUser, sizeof(mqttUser));
          rest.substring(sp + 1).toCharArray(mqttPass, sizeof(mqttPass));
          Serial.println("mqtt creds enregistrees"); }
      }
      line = "";
    } else line += c;
  }
}

// Demarre les services reseau (OTA + IHM web) UNE fois le WiFi connecte (appele depuis loop).
void startServices() {
  configTzTime(NTP_TZ, "pool.ntp.org", "time.nist.gov");     // NTP -> horodatage des pesees du journal
  stilhawtOtaBegin(DEVICE_ID, FW_NAME, FW_VERSION, nullptr);      // headless -> Serial seul
  web.begin(DEVICE_ID, "etekcity-scale",
            "Balance Etekcity FIT-8S — ecoute BLE, publie MQTT. Composition : calculee en aval.",
            FW_VERSION, [](String& j) {
              j += ",\"uplink\":\""; j += zwifi.uplinkStr(); j += "\"";   // joint WAN prouve ? (association != internet)
              j += ",\"rssi\":"; j += WiFi.RSSI();                         // puissance du lien courant (dBm)
              if (lastPubKg > 0) { j += ",\"kg\":";  j += String(lastPubKg, 2);
                                   j += ",\"imp\":"; j += String(lastPubImp); }
            });
  web.onWifiSave([](const String& s, const String& p) { saveAP(s, p); });
  web.onMqttSave([](const String& u, const String& p) {      // POST /mqtt : provisioning creds sans cable
    nvs.begin("scale", false);
    nvs.putString("mqtt_user", u); nvs.putString("mqtt_pass", p);
    nvs.end();
    Serial.println("[mqtt] creds enregistrees (HTTP), reboot");
    delay(300); ESP.restart();
  });
  web.attachWifi(&zwifi);   // carte "Reseaux WiFi connus" + bouton "utiliser" (switch force) dans l'IHM
  web.server().on("/journal", []() {          // journal des pesees {ts,kg,imp}, plus RECENT d'abord (JSON)
    web.server().sendHeader("Access-Control-Allow-Origin", "*");
    web.server().send(200, "application/json", jJson());
  });
  web.server().on("/connlog", []() {          // journal de connexion {ts,ssid,rssi,ev} — uplink prouve/perdu/bascule
    web.server().sendHeader("Access-Control-Allow-Origin", "*");
    web.server().send(200, "application/json", zwifi.connLogJson());
  });
  web.setDoc(
    "Monte sur la balance et reste dessus jusqu'a stabilisation : poids + impedance publies\n"
    "sur MQTT (<topic de base>/state) et affiches ci-dessous. Composition : calculee en aval.\n"
    "\n"
    "LED : VITE = cherche le WiFi · FIXE = connecte AVEC internet · MOYEN = associe SANS uplink\n"
    "  (CPL/repeteur au backhaul mort : bascule auto vers un AP qui a une co) · LENT = portail captif\n"
    "  (portail : connecte-toi a l'AP 'etekcity-scale-setup' pour saisir un WiFi a portee).\n"
    "Diag connexion : /connlog (evenements horodates + RSSI).\n"
    "Serie 115200 : WIFI <ssid> <pass> | WIFI LIST | MQTT <user> <pass> | REBOOT\n"
    "OTA : etekcity-scale.local:3232.");
  Serial.println("[svc] OTA + IHM demarres");
}

// Demarre le scan passif (phase LISTEN). La pile NimBLE est INITIALISEE UNE SEULE FOIS (au 1er appel) :
// on ne fait ENSUITE que start/stop du scan. wantDuplicates=true (balance = adresse stable, sinon vue
// une seule fois). NB : deinit/reinit NimBLE depuis la loop crashe (use-after-free callback, valide en
// serie 0.3.7) — et c'est INUTILE : heap ~134 Ko libre en LISTEN, mbedtls (~40 Ko) rentre largement.
// Le duty-cycle garde son sens (BLE radio SILENCIEUSE pendant la fenetre TLS -> moins de coex + batterie).
void bleStart() {
  if (!g_bleScan) {                                                     // init unique
    NimBLEDevice::init("");
    g_bleScan = NimBLEDevice::getScan();
    g_bleScan->setAdvertisedDeviceCallbacks(new ScaleCallbacks(), true);
    g_bleScan->setActiveScan(false);
    g_bleScan->setMaxResults(0);
    g_bleScan->setInterval(160); g_bleScan->setWindow(80);
  }
  g_bleScan->start(0, nullptr, false);                                  // (re)demarre le scan
}
// Arrete le scan (radio BLE au repos) SANS deinit -> pas de use-after-free, heap deja suffisant pour TLS.
void bleStop() {
  if (g_bleScan) g_bleScan->stop();
}

// ── setup / loop ─────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  nvs.begin("scale", false); boots = nvs.getULong("boots", 0) + 1;
  nvs.putULong("boots", boots); nvs.end();           // un boot-loop doit se voir

  // Flag MODE OTA (ecrit par la commande MQTT {"ota":true} avant reboot). ONE-SHOT : efface tout de suite
  // -> un OTA rate ne laisse pas le device coince (boot normal au suivant). Cf bloc "MODE OTA" plus haut.
  nvs.begin("scale", false);
  g_otaMode = nvs.getBool("ota_mode", false);
  if (g_otaMode) nvs.putBool("ota_mode", false);
  nvs.end();
  g_bootMs = millis();

  mqtt.setBufferSize(1536);          // +connlog : le journal de connexion (~1,3 Ko, 24 events) ne rentre pas dans 384
  setupCreds();
  // wifiOnly = mode OTA : coupe le power-save WiFi (entree fiable). SUR car en mode OTA le BLE n'est jamais
  // initialise -> pas de coex. En mode normal wifiOnly=false (coex protegee, cf gotcha StilhawtWifi).
  zwifi.begin(DEVICE_ID, 2, 20000, g_otaMode);

  // wantDuplicates=TRUE (dans bleStart) : sinon la balance (adresse stable) n'est vue QU'UNE fois — la 1re
  // frame (impedance=0), jamais la frame "prete". C'ETAIT LE BUG (prouve au POC : 1 fige -> 164/15s).
  jLoad();                                                       // recharge le journal de pesees (offline_buffer NVS)
  if (!g_otaMode) {                                              // BLE UNIQUEMENT en mode normal (jamais en OTA)
    bleStart();                                                  // phase LISTEN par defaut (TLS non alloue)
    g_phase = PH_LISTEN; g_phaseT0 = millis();
  }
  Serial.printf("[boot] %s v%s — %s — journal: %d pesee(s)%s\n", DEVICE_ID, FW_VERSION,
                g_otaMode ? "MODE OTA (BLE off, WiFi no-sleep, fenetre ~120s)" : "LISTEN (scan BLE passif)",
                g_jCount, g_bufHas ? ", 1 en attente de publication" : "");
}

void loop() {
  zwifi.handle();                                             // WiFi state machine + LED + portail captif (TOUJOURS)
  if (zwifi.connected() && !g_services) { g_services = true; startServices(); }

  uint32_t now = millis();

  // OTA + IHM restent joignables EN CONTINU (WiFi always-on du radio_schedule) — independant de la phase.
  if (g_services) { stilhawtOtaHandle(); if (stilhawtOtaActive()) return; web.handle(); }

  // ── MODE OTA : fenetre pure-WiFi (BLE jamais init -> entree fiable). Pas de duty-cycle. Timeout -> reboot. ──
  if (g_otaMode) {
    if (g_services && g_otaWindowT0 == 0) {
      g_otaWindowT0 = now;
      Serial.printf("[ota] fenetre OUVERTE @ %s (~120s) — pousse l'OTA maintenant\n",
                    WiFi.localIP().toString().c_str());
    }
    bool windowExpired = g_otaWindowT0 && (now - g_otaWindowT0 > OTA_WINDOW_MS);
    if (windowExpired || now - g_bootMs > OTA_MODE_MAX_MS) {     // rien recu -> retour au mode normal
      Serial.println("[ota] fenetre expiree -> reboot mode normal");
      delay(200); ESP.restart();
    }
    return;                                                      // ne PAS entrer dans le duty-cycle/BLE
  }

  // Pesee decodee par la tache BLE (phase LISTEN) -> BUFFER NVS (offline_buffer). Persistee = jamais
  // perdue, meme si le WiFi est down ou si on reboote. Publiee a la prochaine fenetre PUBLISH.
  bool pend = false; float kg = 0; uint16_t imp = 0;
  portENTER_CRITICAL(&g_mux);
  pend = g_pending; kg = g_kg; imp = g_imp; g_pending = false;
  portEXIT_CRITICAL(&g_mux);
  if (pend) {
    bool changed = (fabsf(kg - lastPubKg) > 0.02f) || (imp != lastPubImp);
    if (changed || now - lastPubMs > DEDUP_MS) {
      lastPubKg = kg; lastPubImp = imp; lastPubMs = now;
      jAppend(kg, imp);                                       // au JOURNAL (horodate) -> publie a la prochaine fenetre
      Serial.printf("[scale] pesee %.2f kg / %u ohm -> journal (%d entrees)\n", kg, imp, g_jCount);
    }
  }

  // ── DUTY-CYCLE : BLE (LISTEN) et TLS (PUBLISH) ne coexistent JAMAIS (anti-OOM) ──
  if (g_phase == PH_LISTEN) {
    // Fenetre periodique (heartbeat + pesee bufferisee) toutes les LISTEN_MS, une fois le WiFi up.
    if (g_services && (now - g_phaseT0 > LISTEN_MS)) {
      Serial.printf("[phase] LISTEN -> PUBLISH (BLE deinit, heap libre %u -> ", ESP.getFreeHeap());
      bleStop();                                              // libere ~45 Ko pour mbedtls
      Serial.printf("%u)\n", ESP.getFreeHeap());
      g_phase = PH_PUBLISH; g_phaseT0 = now;
    }
  } else {                                                    // PH_PUBLISH : BLE OFF, MQTT/TLS actif
    mqtt.loop();
    if (!mqtt.connected()) mqttReconnect();                   // connecte (mqttReconnect publie manifest+heartbeat)
    if (mqtt.connected()) {
      // Fenetre de RECEPTION : draine les cmd entrantes (dont {"ota":true} RETENU, livre au subscribe)
      // AVANT de publier/couper. Sans ca, connect->publish->disconnect atomique = la cmd n'est jamais lue
      // (PubSubClient ne traite l'inbound que dans loop()). onCmd peut rebooter ICI (bascule mode OTA).
      for (uint32_t rx = millis(); millis() - rx < 1000; ) { mqtt.loop(); delay(10); }
      if (g_bufHas) { lastPubKg = g_bufLast.kg; lastPubImp = g_bufLast.imp; }
      publishState();                                         // derniere pesee (si en attente) + signes vitaux (retenu)
      publishHeartbeat();
      publishConnlog();                                       // journal de connexion {ts,ssid,rssi,event} -> DB downstream
      if (g_bufHas) { Serial.printf("[phase] publie %.2f kg -> journal a jour\n", g_bufLast.kg); jPublished(); }
      mqtt.disconnect();
      net.stop();                                             // libere mbedtls
      bleStart();                                             // BLE re-init -> retour ECOUTE
      g_phase = PH_LISTEN; g_phaseT0 = now;
      Serial.println("[phase] PUBLISH -> LISTEN (TLS down, BLE re-init)");
    } else if (now - g_phaseT0 > PUBLISH_MS) {                // connexion ratee dans le budget -> on relache
      net.stop(); bleStart(); g_phase = PH_LISTEN; g_phaseT0 = now;
      Serial.println("[phase] PUBLISH timeout (pas de MQTT) -> LISTEN");
    }
  }

  static uint32_t tDbg = 0;
  if (now - tDbg >= 15000) { tDbg = now;
    Serial.printf("[state] phase=%s adverts=%u buf=%d wifi=%s heapLibre=%u\n",
      g_phase == PH_LISTEN ? "LISTEN" : "PUBLISH", g_adverts, g_bufHas,
      zwifi.connected() ? "up" : "down", ESP.getFreeHeap()); }

  pumpSerial();
  delay(10);
}
