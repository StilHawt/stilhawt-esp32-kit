// ============================================================================
//  water-meter — firmware stilhawt-things (TTGO T-Display)
//  Configuration : src/zdt_device.h (nom, topics, broker).
//
//  Compteur a sortie DOUBLE, 3 fils : black=common(GND), red=OUT1, green=OUT2.
//   - OUT1 (GPIO27) = pulse VOLUME, K=1L -> 1 impulsion = 1 litre  (certain)
//   - OUT2 (GPIO25) = 2e sortie a CARACTERISER : le firmware la LOGGE et
//     echantillonne son niveau AU front de OUT1. Interpretation empirique :
//       * niveau OUT2 stable (toujours HIGH ou LOW) au front OUT1 => quadrature
//         = 2 reeds dephases => SENS du flux (avant/arriere).
//       * OUT2 ~= inverse permanent de OUT1 => contacts NO/NC (ignorer OUT2).
//       * OUT2 statique en marche, bascule sur aimant/reflux => tamper.
//     Tant que le role n'est pas prouve, OUT2 n'AGIT PAS sur le comptage.
//
//  Persistance NVS (jour + 30 j), NTP (rollover minuit FR), 2 ecrans sprite,
//  bouton GPIO35, OTA obligatoire (StilhawtOta), wifi/MQTT (broker absent = degrade).
// ============================================================================
#include <WiFi.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <WebServer.h>     // (LDF) requis AVANT StilhawtWeb.h  — le LDF PlatformIO ne scanne pas shared/
#include <DNSServer.h>     // (LDF) requis AVANT StilhawtWifi.h — portail captif de secours
#include <time.h>
#include <ArduinoOTA.h>     // DOIT preceder StilhawtOta.h (le LDF ne scanne pas shared/)
#include "StilhawtOta.h"
#include "StilhawtWifi.h"       // WiFi non-bloquant + LED etat + multi-AP + portail captif + double-blink data
#include "StilhawtWeb.h"        // mini-IHM locale (identite + doc/wiring + /state) + liste AP + switch force
#include "zdt_device.h"     // ZTD-GENERE : ZDT_DEVICE_ID, ZDT_TOPIC_BASE, ZDT_MQTT_HOST/PORT/TLS
#if ZDT_MQTT_TLS
#include <WiFiClientSecure.h>
#include "mqtt_ca.h"        // CA publique du broker -> verification serveur (pas de MITM)
#endif

#ifndef PIN_OUT1
#define PIN_OUT1 27
#endif
#ifndef PIN_OUT2
#define PIN_OUT2 25
#endif
#ifndef BTN_SWITCH
#define BTN_SWITCH 35
#endif
#ifndef PULSE_WEIGHT_L
#define PULSE_WEIGHT_L 1        // K=1L : 1 impulsion = 1 litre
#endif
// Hors garde -DPULSE_WEIGHT_L (sinon sauté) : rejet de plausibilité OUT1.
#define GLITCH_MIN_GAP_MS 700   // K=1L -> gap réel mini ~1000 ms (60 L/min) ; < 700 ms = bruit/EMI ligne, rejeté

static const char* DEVICE_ID = ZDT_DEVICE_ID;   // depuis zdt_device.h (genere du DSL)
static const char* NTP_TZ    = "CET-1CEST,M3.5.0,M10.5.0/3";   // Europe/Paris
static const int   HIST_DAYS = 30;

// ── etat ────────────────────────────────────────────────────────────────────
TFT_eSPI tft;
TFT_eSprite spr = TFT_eSprite(&tft);
bool spr_ok = false;
Preferences nvs;
#if ZDT_MQTT_TLS
WiFiClientSecure net;
#else
WiFiClient net;
#endif
PubSubClient mqtt(net);

// ── CRENEAUX DE TOPICS (bascule RNM : deux bases en 0.6.4, base neuve seule en 0.6.5) ──
// Un renommage deplace les DEUX moities d'un contrat : le publieur ICI, et des lecteurs
// ailleurs. Basculer d'un coup ne casse rien VISIBLEMENT — le contrat se TAIT au lieu
// d'echouer. D'ou la transition a deux creneaux (0.6.4), close en 0.6.5 une fois le dernier
// lecteur de `stilhawt-things/…` bascule (11/09/2026) : `base_legacy` est sorti du DSL, le
// testament passe sur la base neuve. Le mecanisme reste, pour une prochaine migration.
#ifdef ZDT_TOPIC_BASE_LEGACY
static const char* const TOPIC_BASES[] = { ZDT_TOPIC_BASE, ZDT_TOPIC_BASE_LEGACY };
// Le testament (LWT) ne se DOUBLE pas : une connexion MQTT n'en porte qu'UN. On le laisse sur
// la base LEGACY, celle que les consommateurs actuels ecoutent — c'est la detection de mort,
// on ne la deplace qu'une fois les lecteurs passes de l'autre cote.
#  define ZDT_LWT_BASE ZDT_TOPIC_BASE_LEGACY
#else
static const char* const TOPIC_BASES[] = { ZDT_TOPIC_BASE };
#  define ZDT_LWT_BASE ZDT_TOPIC_BASE
#endif
static const uint8_t N_BASES = sizeof(TOPIC_BASES) / sizeof(TOPIC_BASES[0]);

// Publie la meme charge sur chaque creneau. `suffixe` = `state`, `manifest`, ...
static void pubAll(const char* suffixe, const uint8_t* charge, size_t n, bool retenu) {
  char t[80];
  for (uint8_t i = 0; i < N_BASES; i++) {
    snprintf(t, sizeof(t), "%s/%s", TOPIC_BASES[i], suffixe);
    mqtt.publish(t, charge, n, retenu);
  }
}
static void pubAll(const char* suffixe, const char* texte, bool retenu) {
  pubAll(suffixe, (const uint8_t*)texte, strlen(texte), retenu);
}
StilhawtWeb  web;                  // mini-IHM locale (:80) : http://<ip>/ + /state (local-first, sans broker)
StilhawtWifi zwifi;               // WiFi : multi-AP + LED d'etat + portail captif + double-blink "data recue"
bool     g_services = false;   // services reseau (OTA/IHM/NTP) demarres UNE FOIS le WiFi connecte
uint32_t lastDataMs = 0;       // dernier litre compte -> flash ecran "donnee recue"
static const char* DOC =       // mode d'emploi + wiring affiches dans l'IHM (self-doc, regle StilhawtWeb)
  "Compteur d'eau (reed K=1L). Cablage :\n"
  "  GPIO27 = OUT1 (fil ROUGE) = volume, 1 impulsion = 1 litre\n"
  "  GPIO25 = OUT2 (fil VERT)  = 2e sortie (direction/tamper, a caracteriser)\n"
  "  black (common)            -> GND\n"
  "Provisioning serie 115200 : WIFI <ssid> <pass> | MQTT <user> <pass>\n"
  "Rotation mot de passe MQTT : GET /creds?old=<actuel>&new=<nouveau>\n"
  "Topics MQTT : stilhawt-things/water-meter/{state,heartbeat,status,rotation}";

volatile uint32_t g_pulses  = 0;        // OUT1 : increments ISR (1 = 1 litre)
volatile uint32_t g_out2    = 0;        // OUT2 : compteur brut (caracterisation)
volatile uint32_t g_lastO1  = 0, g_lastO2 = 0;
volatile uint32_t g_lastGapMs = 0;      // intervalle entre 2 litres OUT1 -> débit inter-impulsion
volatile uint8_t  g_o2AtO1  = 2;        // niveau OUT2 echantillonne au front OUT1 (2=n/a)
volatile uint32_t g_glitches = 0;       // impulsions OUT1 REJETEES (gap < GLITCH_MIN_GAP_MS) : diag bruit/EMI

uint32_t total_l    = 0;                // litres cumules
uint32_t dayStart_l = 0;                // total_l au debut du jour courant
int32_t  hist[HIST_DAYS];               // litres/jour (index 0 = hier)
int      hist_n     = 0;
long     dayKey     = -1;
uint32_t out2_total = 0;                // fronts OUT2 cumules (info)
uint8_t  out2_phase = 2;                // dernier niveau OUT2 au front OUT1 (0/1/2)
uint32_t rateWinL   = 0, rateWinMs = 0;
float    lpm        = 0.0f;
uint32_t boots      = 0;                // nb de demarrages (NVS) : un reboot = <=60 s d'impulsions perdues
int      screen     = 0;                // 0 = jour, 1 = historique
char     mqttUser[40] = {0};    // provisionne en serie (secret, jamais dans le firmware)
char     mqttPass[64] = {0};    // mot de passe ACTIF (celui qui a deja fonctionne)
char     mqttPassNew[64] = {0}; // CANDIDAT de rotation, recu par `cmd` — cf rotation ci-dessous
uint8_t  rotTries = 0;          // tentatives sur le candidat avant abandon

// ── ISR OUT1 : REJET DE PLAUSIBILITE (pas un simple debounce). K=1L -> même à 60 L/min = 1 pulse/s
// (gap 1000 ms). Tout gap < GLITCH_MIN_GAP_MS est physiquement impossible pour de l'eau -> rejeté et
// compté (03/09 : train fantôme ~15 Hz sur la ligne rouge = 750-1000 L/min comptés, cadran immobile).
void IRAM_ATTR onOut1() {
  uint32_t now = millis();
  uint32_t gap = now - g_lastO1;
  if (g_lastO1 && gap < GLITCH_MIN_GAP_MS) { g_glitches++; return; }   // bruit/EMI -> rejeté (diag g_glitches)
  if (g_lastO1) g_lastGapMs = gap;      // 1 litre / gap -> debit continu (pas de quantif fenetre)
  g_lastO1 = now;
  g_o2AtO1 = digitalRead(PIN_OUT2);     // phase : niveau OUT2 a l'instant du front OUT1
  g_pulses += PULSE_WEIGHT_L;
}
void IRAM_ATTR onOut2() {
  uint32_t now = millis();
  if (now - g_lastO2 < 40) return;
  g_lastO2 = now;
  g_out2 += 1;
}

// ── persistance NVS ─────────────────────────────────────────────────────────
void saveState() {
  nvs.begin("watermeter", false);
  nvs.putULong("total", total_l);
  nvs.putULong("dstart", dayStart_l);
  nvs.putLong("daykey", dayKey);
  nvs.putInt("histn", hist_n);
  nvs.putULong("boots", boots);
  nvs.putBytes("hist", hist, sizeof(hist));
  nvs.end();
}
void loadState() {
  nvs.begin("watermeter", true);
  total_l    = nvs.getULong("total", 0);
  dayStart_l = nvs.getULong("dstart", 0);
  dayKey     = nvs.getLong("daykey", -1);
  hist_n     = nvs.getInt("histn", 0);
  boots      = nvs.getULong("boots", 0);
  if (nvs.getBytes("hist", hist, sizeof(hist)) != sizeof(hist)) { memset(hist, 0, sizeof(hist)); hist_n = 0; }
  nvs.end();
}

long currentDayKey() {
  time_t t = time(nullptr);
  if (t < 1700000000) return -1;            // NTP pas encore synchro
  struct tm lt; localtime_r(&t, &lt);
  return (long)(lt.tm_year) * 366 + lt.tm_yday;
}
void rolloverIfNeeded() {
  long k = currentDayKey();
  if (k < 0) return;
  if (dayKey < 0) { dayKey = k; dayStart_l = total_l; return; }
  if (k != dayKey) {
    int32_t dayTotal = (int32_t)(total_l - dayStart_l);
    for (int i = HIST_DAYS - 1; i > 0; --i) hist[i] = hist[i - 1];
    hist[0] = dayTotal;
    if (hist_n < HIST_DAYS) hist_n++;
    dayStart_l = total_l; dayKey = k;
    saveState();
  }
}

// ── creds MQTT (le WiFi est gere par StilhawtWifi : multi-AP, non-bloquant, LED, portail) ──
// Les identifiants WiFi legacy (cles ssid/pass ecrites par les fw <=0.5) sont migres
// automatiquement vers le schema multi-AP par StilhawtWifi.loadAPs() au begin() -> pas de perte a l'OTA.
void setupCreds() {
  nvs.begin("watermeter", true);
  nvs.getString("mqtt_user", mqttUser, sizeof(mqttUser));
  nvs.getString("mqtt_pass", mqttPass, sizeof(mqttPass));
  nvs.getString("mqtt_pass_new", mqttPassNew, sizeof(mqttPassNew));   // candidat de rotation en cours
  nvs.end();
#if ZDT_MQTT_TLS
  net.setInsecure();   // (TLS coupe dans le DSL : ce bloc est inactif) transport chiffre, verif serveur off
#endif
}
void publishManifest() {
  if (!mqtt.connected()) return;
  StaticJsonDocument<256> d;
  d["id"] = DEVICE_ID; d["kind"] = "water-meter";
  d["fw"] = String(FW_NAME) + " " + FW_VERSION;
  d["ip"] = WiFi.localIP().toString();
  JsonArray ch = d.createNestedArray("channels"); ch.add("water_liters");
  char buf[256]; size_t n = serializeJson(d, buf);
  pubAll("manifest", (uint8_t*)buf, n, true);
}
void publishState() {
  if (!mqtt.connected()) return;
  StaticJsonDocument<384> d;               // +glitches : 256 tronquait deja (cf mqtt.setBufferSize 384)
  d["today_l"] = total_l - dayStart_l;
  d["total_l"] = total_l;
  d["lpm"]     = lpm;
  // Signes VITAUX embarques dans /state : mqtt2prom est abonne a `stilhawt-things/+/state`, donc
  // les exposer ICI ne coute AUCUN changement d'infra. `uptime_s` change a chaque publication
  // meme quand zero litre ne coule : c'est le seul signal qui distingue « maison calme »
  // de « device mort » (sans lui, un reed debranche reste invisible ~24 h).
  d["uptime_s"] = (uint32_t)(millis() / 1000);
  d["boots"]    = boots;                    // +1 = reboot -> jusqu'a 60 s d'impulsions perdues (NVS)
  d["rssi"]     = WiFi.RSSI();              // degradation wifi AVANT la coupure
  d["out2_pulses"] = out2_total;            // aux : caracterisation OUT2
  d["glitches"]    = g_glitches;            // impulsions OUT1 rejetees (diag bruit/EMI ligne)
  if (out2_phase != 2) d["out2_at_pulse"] = out2_phase;
  char buf[256]; size_t n = serializeJson(d, buf);
  pubAll("state", (uint8_t*)buf, n, false);
}
// Battement dedie (cadence `liveness` du DSL). Distinct de /state : un consommateur qui ne veut
// que la VIE du device s'abonne ici sans parser la mesure.
void publishHeartbeat() {
  if (!mqtt.connected()) return;
  StaticJsonDocument<160> d;
  d["uptime_s"] = (uint32_t)(millis() / 1000);
  d["boots"]    = boots;
  d["rssi"]     = WiFi.RSSI();
  d["fw"]       = FW_VERSION;
  char buf[160]; size_t n = serializeJson(d, buf);
  pubAll("heartbeat", (uint8_t*)buf, n, false);
}
// ── ROTATION DU MOT DE PASSE MQTT, PAR OTA + COMMANDE (fw 0.4.0) ─────────────
// Le probleme : les identifiants vivent en NVS, ecrits par le port SERIE. Faire tourner le
// secret semblait donc exiger un cable — alors que l'OTA existe precisement pour ne plus en
// dependre. Il ne manquait pas un acces physique : il manquait un firmware qui sache le faire.
//
// DEUX EMPLACEMENTS, jamais un seul. `mqtt_pass` = le mot de passe qui a DEJA fonctionne ;
// `mqtt_pass_new` = un CANDIDAT recu par commande. Au reconnect on tente le candidat ; s'il
// passe, il est PROMU (et l'ancien oublie) ; s'il echoue 3 fois, il est ABANDONNE et le device
// repart avec l'ancien. Consequence : un mauvais mot de passe pousse par erreur ne peut pas
// rendre le compteur muet — le seul scenario qui justifiait de sortir un cable.
void saveCreds() {
  nvs.begin("watermeter", false);
  nvs.putString("mqtt_pass", mqttPass);
  if (mqttPassNew[0]) nvs.putString("mqtt_pass_new", mqttPassNew);
  else                nvs.remove("mqtt_pass_new");
  nvs.end();
}

void publishRotationAck(const char* etat) {
  pubAll("rotation", etat, true);   // retenu : l'operateur lit le resultat meme s'il arrive apres
}

// Commande entrante. SEUL champ accepte : `mqtt_pass`. Vocabulaire FERME — un device qui
// executerait ce qu'on lui envoie serait exactement l'agent sur-permissionne qu'on s'interdit.
void onCmd(char* topic, uint8_t* payload, unsigned int len) {
  StaticJsonDocument<160> d;
  if (deserializeJson(d, payload, len)) return;
  const char* np = d["mqtt_pass"];
  if (!np || !*np || strlen(np) >= sizeof(mqttPassNew)) return;
  if (!strcmp(np, mqttPass)) { publishRotationAck("inchange"); return; }   // idempotent
  strncpy(mqttPassNew, np, sizeof(mqttPassNew) - 1);
  rotTries = 0;
  saveCreds();
  publishRotationAck("candidat-recu");
  mqtt.disconnect();             // force le reconnect : c'est LUI qui valide le candidat
}

void mqttReconnect() {
  if (WiFi.status() != WL_CONNECTED || mqtt.connected()) return;
  if (mqttUser[0] == 0) return;             // pas de creds provisionnees -> on n'essaie pas
  mqtt.setServer(ZDT_MQTT_HOST, ZDT_MQTT_PORT);   // endpoint depuis le DSL (public TLS 8883)
  mqtt.setCallback(onCmd);
  // LWT (Last Will and Testament) : le BROKER publie `offline` retenu si l'ESP disparait sans
  // dire au revoir. C'est la norme IoT et la seule detection de mort INSTANTANEE — un poll de
  // fraicheur ne peut, par construction, que constater l'absence apres coup.
  char st[80]; snprintf(st, sizeof(st), "%s/status", ZDT_LWT_BASE);   // un seul testament possible
  const bool essaiCandidat = (mqttPassNew[0] != 0);
  const char* pass = essaiCandidat ? mqttPassNew : mqttPass;

  if (mqtt.connect(DEVICE_ID, mqttUser, pass, st, 1, true, "offline")) {
    if (essaiCandidat) {                    // le candidat a FONCTIONNE : on le promeut
      strncpy(mqttPass, mqttPassNew, sizeof(mqttPass) - 1);
      mqttPassNew[0] = 0; rotTries = 0;
      saveCreds();
      publishRotationAck("promu");
    }
    pubAll("status", "online", true);       // `online` sur les DEUX bases (le LWT, lui, est unique)
    // On souscrit `cmd` sur TOUTES les bases : une commande envoyee a l'ancienne adresse doit
    // continuer d'arriver, sinon la rotation du mot de passe deviendrait injoignable pendant
    // la transition — et c'est le seul moyen de rattraper ce compteur sans cable.
    char ct[80];
    for (uint8_t i = 0; i < N_BASES; i++) {
      snprintf(ct, sizeof(ct), "%s/cmd", TOPIC_BASES[i]);
      mqtt.subscribe(ct);                   // `subscribes: [cmd]` etait DECLARE au DSL sans etre fait
    }
    publishManifest();
    publishHeartbeat();                     // ne pas attendre 30 s pour prouver qu'on est la
  } else if (essaiCandidat && ++rotTries >= 3) {
    mqttPassNew[0] = 0; rotTries = 0;       // candidat abandonne : on repart avec l'ancien
    saveCreds();                            // (l'ack partira a la prochaine connexion reussie)
  }
}

// ── console serie : provisioning + caracterisation OUT2 ──────────────────────
void pumpSerial() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (line.startsWith("WIFI ")) {                 // WIFI <ssid> <pass> : ajoute/maj un AP (multi-AP)
        int sp = line.indexOf(' ', 5);
        if (sp > 0) { Serial.println("wifi ajoute (multi-AP), reboot");
          zwifi.addAP(line.substring(5, sp), line.substring(sp + 1), true); }  // schema ssid<idx>/pass<idx> + reboot
      } else if (line.startsWith("MQTT ")) {          // MQTT <user> <pass>  (host/port = DSL)
        String rest = line.substring(5); int sp = rest.indexOf(' ');
        if (sp > 0) { nvs.begin("watermeter", false);
          nvs.putString("mqtt_user", rest.substring(0, sp));
          nvs.putString("mqtt_pass", rest.substring(sp + 1)); nvs.end();
          rest.substring(0, sp).toCharArray(mqttUser, sizeof(mqttUser));
          rest.substring(sp + 1).toCharArray(mqttPass, sizeof(mqttPass));
          Serial.println("mqtt creds enregistrees"); }
      } else if (line == "RESET") { saveState(); Serial.println("state sauve"); }
      line = "";
    } else line += c;
  }
}

// ── endpoint custom : rotation du mot de passe MQTT en LAN (voie de secours) ──
// POST/GET /creds?old=<actuel>&new=<nouveau>. L'ancien mdp = PREUVE d'autorisation (pas de secret
// en plus ; un inconnu du LAN ne peut rien ecrire sans le connaitre). La rotation par `cmd` a une
// fenetre de risque (broker deja tourne + commande ratee = device injoignable en MQTT) : le device
// reste VIVANT en HTTP local -> c'est par la qu'on le rattrape.
void handleCreds() {
  WebServer& s = web.server();
  String o = s.arg("old"), n = s.arg("new");
  if (o.length() == 0 || n.length() == 0 || n.length() >= sizeof(mqttPassNew)) {
    s.send(400, "application/json", "{\"ok\":false,\"err\":\"old+new requis\"}"); return;
  }
  if (o != String(mqttPass)) {                    // preuve d'autorisation
    s.send(403, "application/json", "{\"ok\":false,\"err\":\"ancien mot de passe incorrect\"}"); return;
  }
  n.toCharArray(mqttPassNew, sizeof(mqttPassNew));
  rotTries = 0; saveCreds();
  mqtt.disconnect();                              // force la validation du candidat
  s.send(200, "application/json", "{\"ok\":true,\"etat\":\"candidat-recu\"}");
}

// ── endpoint calibration LAN : recale l'index / reset le jour (voie de secours, local-first) ──
// GET /calib?today=avg       -> today_l = moyenne(hist) (jour courant recalé sur une journée type)
// GET /calib?today=<litres>  -> today_l = <litres>
// GET /calib?total=<litres>  -> total_l = <litres> (resync exact sur le cadran physique, today conservé)
// LAN de confiance (comme /creds) ; renvoie l'état recalé. Sert à réparer un index fauss par du bruit.
void handleCalib() {
  WebServer& s = web.server();
  bool did = false;
  if (s.hasArg("total")) {
    uint32_t m = (uint32_t) strtoul(s.arg("total").c_str(), nullptr, 10);
    uint32_t today = total_l - dayStart_l;          // conserve la conso du jour
    total_l = m; dayStart_l = (m > today) ? m - today : 0; did = true;
  }
  if (s.hasArg("today")) {
    String t = s.arg("today");
    uint32_t v = 0;
    if (t == "avg") {
      long sum = 0; int c = 0;
      for (int i = 0; i < hist_n; i++) if (hist[i] > 0) { sum += hist[i]; c++; }
      v = c ? (uint32_t)(sum / c) : 0;
    } else v = (uint32_t) strtoul(t.c_str(), nullptr, 10);
    dayStart_l = (total_l > v) ? total_l - v : 0; did = true;
  }
  if (!did) { s.send(400, "application/json", "{\"ok\":false,\"err\":\"today= ou total= requis\"}"); return; }
  saveState();
  char buf[128];
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"today_l\":%lu,\"total_l\":%lu,\"glitches\":%lu}",
           (unsigned long)(total_l - dayStart_l), (unsigned long)total_l, (unsigned long)g_glitches);
  s.send(200, "application/json", buf);
}

// ── services reseau : demarres UNE FOIS le WiFi connecte (via StilhawtWifi, non-bloquant) ─
// Contrairement au fw <=0.5 (setupWifi bloquait 15 s au boot), ici setup() ne bloque plus :
// des que zwifi.connected(), on branche OTA + IHM + NTP.
void startServices() {
  configTzTime(NTP_TZ, "pool.ntp.org", "time.nist.gov");
  stilhawtOtaBegin(DEVICE_ID, FW_NAME, FW_VERSION, nullptr);   // TODO callback ecran (sig StilhawtOtaDraw)
  web.begin(DEVICE_ID, "water-meter",
            "Compteur d'eau interieur (reed K=1L) — TTGO T-Display", FW_VERSION,
            [](String& j) {                        // metriques metier appendees a /state (ssid/rssi deja fournis)
    j += ",\"today_l\":"; j += String((unsigned long)(total_l - dayStart_l));
    j += ",\"total_l\":"; j += String((unsigned long)total_l);
    j += ",\"lpm\":";     j += String(lpm, 1);
    j += ",\"boots\":";   j += String((unsigned long)boots);
    j += ",\"out2_pulses\":"; j += String((unsigned long)out2_total);
    j += ",\"glitches\":"; j += String((unsigned long)g_glitches);
  });
  web.setDoc(DOC);                        // mode d'emploi + wiring dans l'IHM (self-doc)
  web.attachWifi(&zwifi);                 // carte "Reseaux WiFi connus" + bouton "utiliser" (switch force)
  web.onWifiSave([](const String& s, const String& p) { zwifi.addAP(s, p, true); });  // config WiFi opt-in
  web.server().on("/creds", handleCreds); // voie de secours rotation (LAN, preuve = ancien mdp)
  web.server().on("/calib", handleCalib); // recalage index / reset jour (LAN, local-first)
  // Journal de connexion (ts/ssid/rssi/event) : StilhawtWifi le tenait DEJA en anneau RAM, rien ne
  // le lisait. Un journal qu'on ne peut pas consulter ne diagnostique rien — surtout ici, ou le
  // suspect est un CPL intermittent et ou le device n'est pas atteignable au cable.
  web.server().on("/connlog", []() {
    web.server().send(200, "application/json", zwifi.connLogJson());
  });
}

// ── ecran (sprite double-buffer obligatoire) ─────────────────────────────────
void render() {
  if (!spr_ok) return;
  spr.fillSprite(TFT_BLACK); spr.setTextDatum(TL_DATUM);
  if (screen == 0) {
    spr.setTextColor(TFT_CYAN);  spr.drawString("Conso du jour", 6, 4, 4);
    spr.setTextColor(TFT_WHITE);
    char l[24]; snprintf(l, sizeof(l), "%lu L", (unsigned long)(total_l - dayStart_l));
    spr.drawString(l, 6, 40, 6);
    char r[24]; snprintf(r, sizeof(r), "%.1f L/min", lpm);
    spr.setTextColor(TFT_GREENYELLOW); spr.drawString(r, 6, 96, 4);
    // info OUT2 (caracterisation) : niveau echantillonne au dernier front OUT1
    char o[20]; snprintf(o, sizeof(o), "o2:%s", out2_phase == 2 ? "-" : (out2_phase ? "H" : "L"));
    spr.setTextColor(TFT_DARKGREY); spr.drawString(o, 190, 100, 2);
  } else {
    spr.setTextColor(TFT_CYAN); spr.drawString("Jours precedents", 6, 4, 4);
    spr.setTextColor(TFT_WHITE);
    int rows = hist_n < 5 ? hist_n : 5;
    for (int i = 0; i < rows; ++i) {
      char l[32]; snprintf(l, sizeof(l), "J-%d   %ld L", i + 1, (long)hist[i]);
      spr.drawString(l, 6, 36 + i * 20, 2);
    }
    if (hist_n == 0) spr.drawString("(pas encore d'historique)", 6, 40, 2);
  }
  spr.setTextColor(TFT_DARKGREY);
  const char* ws = (zwifi.state() == StilhawtWifi::CONNECTED) ? "wifi ok"
                 : (zwifi.state() == StilhawtWifi::PORTAL)    ? "portail" : "wifi..";
  spr.drawString(ws, 150, 4, 2);
  if (millis() - lastDataMs < 600) spr.fillCircle(232, 9, 6, TFT_GREEN);  // flash visible "donnee recue"
  spr.pushSprite(0, 0);
}

// ── setup / loop ─────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  tft.init(); tft.setRotation(1); tft.fillScreen(TFT_BLACK);
  spr.setColorDepth(8); spr_ok = (spr.createSprite(240, 135) != nullptr);

  loadState();
  boots++; saveState();          // compte le demarrage TOUT DE SUITE : un boot-loop doit se voir
  mqtt.setBufferSize(384);       // /state a grossi (signes vitaux) : le defaut 256 tronquerait en silence
  pinMode(PIN_OUT1, INPUT_PULLUP);
  pinMode(PIN_OUT2, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_OUT1), onOut1, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_OUT2), onOut2, FALLING);
  pinMode(BTN_SWITCH, INPUT_PULLUP);

  setupCreds();                    // charge les creds MQTT (NVS) ; le WiFi est gere par StilhawtWifi
  zwifi.begin(DEVICE_ID, 2);       // NON-bloquant : LED d'etat (GPIO2) + multi-AP + portail captif de secours
  // OTA / IHM / NTP sont demarres dans loop() des que zwifi.connected() (cf startServices).
  rateWinL = total_l; rateWinMs = millis();
}

void loop() {
  stilhawtOtaHandle();
  if (stilhawtOtaActive()) return;

  zwifi.handle();                                     // LED d'etat + reconnexion + portail (non-bloquant)
  if (zwifi.connected() && !g_services) { g_services = true; startServices(); }  // OTA/IHM/NTP au 1er connect

  uint32_t now = millis();

  // 1) integrer OUT1 -> litres, et relever OUT2 (brut + phase)
  noInterrupts();
  uint32_t p = g_pulses; g_pulses = 0;
  uint32_t p2 = g_out2;  g_out2 = 0;
  uint8_t  ph = g_o2AtO1;
  interrupts();
  if (p)  { total_l += p; zwifi.notifyData(); lastDataMs = now; }  // donnee recue -> double-blink LED + flash ecran
  if (p2) out2_total += p2;
  if (ph != 2) out2_phase = ph;

  // 2) bascule quotidienne
  rolloverIfNeeded();

  // 3) debit INSTANTANE depuis l'intervalle inter-impulsion (resolution continue)
  {
    uint32_t gap, lastp;
    noInterrupts(); gap = g_lastGapMs; lastp = g_lastO1; interrupts();
    uint32_t sinceLast = now - lastp;
    float inst = 0.0f;
    if (gap > 0) {
      inst = 60000.0f / gap;                          // 1 L / intervalle -> L/min
      if (sinceLast > gap) inst = 60000.0f / sinceLast; // le flux ralentit -> le debit decroit
    }
    if (sinceLast > 30000) inst = 0.0f;               // idle > 30 s -> debit nul
    if (inst > 90.0f) inst = 90.0f;                   // plafond physique (garde-fou ; le rejet ISR borne deja ~86)
    lpm = inst;
  }

  // 4) bouton -> ecran
  static uint32_t tBtn = 0; static int last = HIGH;
  int b = digitalRead(BTN_SWITCH);
  if (b == LOW && last == HIGH && now - tBtn > 200) { screen ^= 1; tBtn = now; }
  last = b;

  // 5) rendu ~250 ms
  static uint32_t tR = 0; if (now - tR >= 250) { tR = now; render(); }

  // 6) mqtt (degrade) + state ~5 s + trace de caracterisation OUT2 sur serie
  mqtt.loop();
  static uint32_t tH = 0;
  if (now - tH >= 30000) { tH = now; publishHeartbeat(); }   // cadence `liveness` du DSL
  static uint32_t tM = 0;
  if (now - tM >= 5000) { tM = now; mqttReconnect(); publishState();
    Serial.printf("[carac] total=%luL out2=%lu o2@pulse=%s  (o2 stable au front OUT1 => quadrature=direction)\n",
                  (unsigned long)total_l, (unsigned long)out2_total,
                  out2_phase == 2 ? "-" : (out2_phase ? "H" : "L")); }

  // 7) sauvegarde NVS ~60 s
  static uint32_t tS = 0; if (now - tS >= 60000) { tS = now; saveState(); }

  if (g_services) web.handle();    // mini-IHM StilhawtWeb (dispo une fois le WiFi connecte)
  pumpSerial();
  delay(5);
}
