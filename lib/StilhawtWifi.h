// ============================================================================
//  StilhawtWifi.h — gestion WiFi stilhawt-things (shared/) : non-bloquant + LED d'etat + portail captif
//  + VERIFICATION D'UPLINK (association != connectivite) + bascule vers un AP qui a une vraie
//  liaison + journal de connexion (RSSI).
//
//  Resout l'angle mort du device HEADLESS deploye : sans signal, on ne sait pas s'il
//  cherche / est connecte / a echoue, et sans portail on ne peut pas le reconfigurer
//  sans cable. Signaletique LED (comme les projets pre-IA du user) + secours captif.
//
//  INVARIANT (26/08, user) : un AP n'est BON que si son UPLINK est PROUVE. Un CPL/repeteur
//  (TP-Link WPA4220...) peut ASSOCIER en WiFi tout en ayant son backhaul mort -> "connecte
//  sans internet". WiFiMulti choisit par RSSI SEUL -> il s'accroche au plus fort (le plus proche),
//  pas au plus JOIGNABLE. Ici : apres association on teste un joint WAN (TCP) ; si KO on blackliste
//  cet AP et on bascule vers le suivant (quitte a plus faible mais qui MARCHE), on re-teste
//  periodiquement (le CPL est capricieux), et on PREFERE l'AP prouve au reconnect (NVS, survit reboot).
//
//  Etats + LED (GPIO par defaut 2, onboard) :
//    CONNECTING : clignotement RAPIDE (~150ms)   — cherche/connecte un AP connu
//    CONNECTED + uplink OK   : LED FIXE           — WiFi STA up ET joint WAN prouve
//    CONNECTED + PAS d'uplink: clignotement MOYEN (~400ms) — associe mais sans internet (degrade)
//    PORTAL     : clignotement LENT (~800ms)      — aucun AP joignable -> AP de config
//
//  Usage (main.cpp) :
//    #include <WebServer.h>            // AVANT (LDF ne scanne pas shared/)
//    #include <DNSServer.h>
//    #include "StilhawtWifi.h"
//    StilhawtWifi zwifi;
//    setup() : zwifi.begin(DEVICE_ID);            // non bloquant
//    loop()  : zwifi.handle();                     // pilote LED + etat + portail + uplink + reconnect
//              if (zwifi.connected() && !started) { started=true; /* OTA + StilhawtWeb */ }
//    IHM     : zwifi.connLogJson()  -> /connlog ; zwifi.hasUplink()/uplinkStr() -> etat expose
//
//  NVS : namespace "stilhawt_wifi", cle "n" + ssid0..N/pass0..N (compatible saveAP() / provision.py)
//        + "good" = dernier SSID a uplink prouve (prefere au boot/reconnect).
// ============================================================================
#pragma once
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <time.h>

class StilhawtWifi {
public:
  enum State  { CONNECTING, CONNECTED, PORTAL };
  enum Uplink { UP_UNKNOWN, UP_OK, UP_NONE };

  // wifiOnly=true : device SANS BLE -> on peut couper le power-save WiFi (entree joignable, cf plus bas).
  // ⚠ NE JAMAIS mettre wifiOnly=true sur un device BLE : le power-save WiFi OFF est INCOMPATIBLE avec la
  // COEXISTENCE WiFi+BT de l'ESP32 (le coex a besoin des creneaux modem-sleep pour partager la radio) ->
  // abort() dans coex_core_enable a l'init NimBLE (vecu 26/08 sur etekcity, boot-loop). Defaut = false = sur.
  void begin(const char* id, int ledPin = 2, uint32_t timeoutMs = 20000, bool wifiOnly = false) {
    _id = id; _led = ledPin; _timeout = timeoutMs;
    pinMode(_led, OUTPUT);
    WiFi.mode(WIFI_STA); WiFi.setHostname(_id);
    // Power-save OFF (WiFi-only seulement) : sinon l'ESP dort entre les beacons -> parle en SORTIE (MQTT)
    // mais devient INJOIGNABLE en ENTREE (HTTP :80, OTA :3232 : SYN/invites tombent). Un device WebServer/OTA
    // est sur secteur (regle StilhawtWeb). Sur un device BLE, la coex EXIGE le power-save -> on ne le coupe pas
    // (l'entree reste alors intermittente ; l'update passe par la serie). Cf gotcha coex ci-dessus.
    if (wifiOnly) WiFi.setSleep(false);
    int n = loadAPs();
    _loadGood();                          // AP prouve memorise -> on l'essaie EN PREMIER
    _t0 = millis();
    _state = CONNECTING;
    if (n == 0) _timeout = 2500;          // aucun AP connu -> bascule vite en portail
    else if (_lastGood.length() && _apPass(_lastGood, _forcedPass)) {
      _forced = _lastGood;                // prefere l'AP a uplink prouve (meme plus faible)
      _beginForced();
      Serial.printf("[wifi] %d AP connus ; prefere l'AP prouve '%s'\n", n, _lastGood.c_str());
      return;
    }
    Serial.printf("[wifi] %d AP connus, connexion (par RSSI)...\n", n);
  }

  // Surcharge des cibles de test uplink (joint WAN) + cadence de re-verif. Defaut : DNS publics 53.
  void setUplinkProbe(IPAddress a, uint16_t pa, IPAddress b, uint16_t pb, uint32_t everyMs = 60000) {
    _probeA = a; _probePortA = pa; _probeB = b; _probePortB = pb; _probeEveryMs = everyMs;
  }

  void handle() {
    uint32_t now = millis();
    if (_state == CONNECTING) {
      bool up = (WiFi.status() == WL_CONNECTED);
      if (!up && !_forced.length()) up = (_multi.run() == WL_CONNECTED);  // switch force : on attend l'AP demande
      if (up) _verify(now);                                              // <-- association OK -> on VERIFIE l'uplink
      else if (now - _t0 > _timeout) {
        if (_forced.length()) { _forced = ""; _fallback = false; _t0 = now; }  // AP force injoignable -> relache (multi)
        else _startPortal();
      }
    } else if (_state == CONNECTED) {
      if (WiFi.status() != WL_CONNECTED) {                   // perte de lien -> reconnexion
        _state = CONNECTING; _t0 = now; _uplink = UP_UNKNOWN; _journal(EV_RECONNECT);
        if (_lastGood.length() && _apPass(_lastGood, _forcedPass)) { _forced = _lastGood; _beginForced(); }
        else if (_forced.length()) _beginForced();
      } else if (now - _lastProbe > _probeEveryMs) {         // re-verif uplink (CPL intermittent)
        _lastProbe = now;
        bool ok = _uplinkOk();
        if (ok && _uplink != UP_OK)  { _uplink = UP_OK;   _journal(EV_RECOVERED); }
        else if (!ok && _uplink != UP_NONE) {              // l'uplink est TOMBE alors qu'on etait bon
          _uplink = UP_NONE; _journal(EV_UPLINK_FAIL); _blacklist(WiFi.SSID(), now);
          _forceNextCandidate(now);                        // -> bascule vers un AP qui marche (s'il y en a)
        }
      }
    } else {  // PORTAL
      _dns.processNextRequest();
      _portal.handleClient();
    }
    _blink(now);
  }

  bool  connected() const { return _state == CONNECTED; }
  State state()     const { return _state; }
  bool  hasUplink() const { return _uplink == UP_OK; }
  const char* uplinkStr() const { return _uplink == UP_OK ? "ok" : (_uplink == UP_NONE ? "no-uplink" : "unknown"); }

  // A appeler DES QU'UNE DONNEE arrive (regle stilhawt-things) : 2 flashs rapides, non-bloquant.
  void notifyData() { _dataBlinks = 4; }

  // Liste des AP connus (NVS) + lequel est courant, pour la mini-IHM. JSON compact.
  String apListJson() {
    String out = "[";
    Preferences p; p.begin("stilhawt_wifi", true);
    int n = p.getInt("n", 0);
    String cur = WiFi.SSID();
    for (int i = 0; i < n && i < 8; i++) {
      char ks[8]; snprintf(ks, sizeof(ks), "ssid%d", i);
      String s = p.getString(ks, "");
      if (!s.length()) continue;
      if (out.length() > 1) out += ",";
      out += "{\"ssid\":\""; out += s; out += "\",\"cur\":"; out += (s == cur ? "true" : "false"); out += "}";
    }
    p.end();
    out += "]";
    return out;
  }

  // Journal de connexion (RAM ring) : evenements horodates avec RSSI (dBm). Le device l'expose
  // (StilhawtWeb /connlog) et/ou le bufferise vers MQTT -> une "DB de connexion" downstream.
  String connLogJson() {
    static const char* EVN[] = {"connect", "uplink_fail", "switch", "portal", "recovered", "reconnect"};
    String out = "[";
    for (int k = 0; k < _cjCount; k++) {
      int i = (_cjHead - _cjCount + k + 2 * CJ_MAX) % CJ_MAX;
      const ConnEv& e = _cj[i];
      if (out.length() > 1) out += ",";
      out += "{\"ts\":"; out += (uint32_t)e.ts;
      out += ",\"ev\":\""; out += (e.ev < 6 ? EVN[e.ev] : "?"); out += "\"";
      out += ",\"ssid\":\""; out += e.ssid; out += "\"";
      out += ",\"rssi\":"; out += e.rssi; out += "}";
    }
    out += "]";
    return out;
  }

  // Force la connexion a un AP connu (override manuel depuis l'IHM). false si SSID inconnu.
  bool switchTo(const String& ssid) {
    if (!_apPass(ssid, _forcedPass)) return false;
    _forced = ssid;
    Serial.printf("[wifi] switch FORCE -> %s\n", ssid.c_str());
    _beginForced();
    return true;
  }

  // Ajoute/maj un AP dans la liste NVS (schema multi-AP). config serie + formulaire + portail.
  bool addAP(const String& ssid, const String& pass, bool reboot = true) {
    if (!ssid.length()) return false;
    Preferences p; p.begin("stilhawt_wifi", false);
    int n = p.getInt("n", 0), idx = -1;
    for (int i = 0; i < n && i < 8; i++) { char k[8]; snprintf(k, sizeof(k), "ssid%d", i);
      if (p.getString(k, "") == ssid) { idx = i; break; } }
    if (idx < 0 && n < 8) { idx = n; p.putInt("n", ++n); }
    if (idx >= 0) { char ks[8], kp[8]; snprintf(ks, sizeof(ks), "ssid%d", idx); snprintf(kp, sizeof(kp), "pass%d", idx);
      p.putString(ks, ssid); p.putString(kp, pass); }
    p.end();
    if (reboot) { delay(300); ESP.restart(); }
    return idx >= 0;
  }

private:
  enum Event : uint8_t { EV_CONNECT = 0, EV_UPLINK_FAIL, EV_SWITCH, EV_PORTAL, EV_RECOVERED, EV_RECONNECT };

  const char* _id = "";
  int _led = 2;
  uint32_t _timeout = 20000, _t0 = 0, _tled = 0;
  bool _ledOn = false;
  volatile int _dataBlinks = 0;
  State  _state  = CONNECTING;
  Uplink _uplink = UP_UNKNOWN;
  String _forced, _forcedPass;         // AP impose (switch manuel OU bascule fallback) ; "" = WiFiMulti libre
  String _lastGood;                    // dernier SSID a uplink prouve (prefere) ; persiste en NVS ("good")
  bool   _fallback = false;
  uint32_t _lastProbe = 0, _probeEveryMs = 60000;
  IPAddress _probeA{1, 1, 1, 1}; uint16_t _probePortA = 53;   // joint WAN #1 (Cloudflare DNS)
  IPAddress _probeB{8, 8, 8, 8}; uint16_t _probePortB = 53;   // joint WAN #2 (Google DNS) — un des deux suffit
  WiFiMulti  _multi;
  Preferences _nvs;
  WebServer  _portal{80};
  DNSServer  _dns;

  // Blacklist runtime : AP associe mais SANS uplink -> ecarte quelques minutes (CPL capricieux).
  static const int BL_MAX = 6;
  String   _blSsid[BL_MAX];
  uint32_t _blUntil[BL_MAX] = {0};
  uint32_t _blMs = 180000;             // 3 min avant de re-tenter un AP sans uplink

  // Journal de connexion (RAM ring).
  struct ConnEv { uint32_t ts; int16_t rssi; uint8_t ev; char ssid[24]; };
  static const int CJ_MAX = 24;
  ConnEv _cj[CJ_MAX]; int _cjHead = 0, _cjCount = 0;

  // ---- uplink : le joint WAN est-il joignable ? (TCP court, non l'association) ----
  bool _uplinkOk() {
    WiFiClient c;
    if (c.connect(_probeA, _probePortA, 2500)) { c.stop(); return true; }
    if (c.connect(_probeB, _probePortB, 2500)) { c.stop(); return true; }
    return false;
  }

  uint32_t _epoch() {
    time_t t = time(nullptr);
    return (t > 1600000000) ? (uint32_t)t : (uint32_t)(millis() / 1000);   // NTP si synchro, sinon uptime
  }

  void _journal(uint8_t ev) {
    ConnEv& e = _cj[_cjHead];
    e.ts = _epoch();
    e.rssi = (int16_t)WiFi.RSSI();
    e.ev = ev;
    String s = WiFi.SSID();
    strncpy(e.ssid, s.c_str(), sizeof(e.ssid) - 1); e.ssid[sizeof(e.ssid) - 1] = 0;
    _cjHead = (_cjHead + 1) % CJ_MAX;
    if (_cjCount < CJ_MAX) _cjCount++;
  }

  bool _blacklisted(const String& s, uint32_t now) {
    for (int i = 0; i < BL_MAX; i++) if (_blSsid[i] == s && now < _blUntil[i]) return true;
    return false;
  }
  void _blacklist(const String& s, uint32_t now) {
    int slot = -1;
    for (int i = 0; i < BL_MAX; i++) { if (_blSsid[i] == s) { slot = i; break; } if (slot < 0 && _blUntil[i] <= now) slot = i; }
    if (slot < 0) slot = 0;
    _blSsid[slot] = s; _blUntil[slot] = now + _blMs;
  }

  bool _apPass(const String& ssid, String& passOut) {
    Preferences p; p.begin("stilhawt_wifi", true);
    int n = p.getInt("n", 0); bool found = false;
    for (int i = 0; i < n && i < 8; i++) {
      char ks[8], kp[8]; snprintf(ks, sizeof(ks), "ssid%d", i); snprintf(kp, sizeof(kp), "pass%d", i);
      if (p.getString(ks, "") == ssid) { passOut = p.getString(kp, ""); found = true; break; }
    }
    p.end(); return found;
  }
  void _beginForced() {
    WiFi.disconnect();
    WiFi.begin(_forced.c_str(), _forcedPass.c_str());
    _state = CONNECTING; _t0 = millis();
  }

  void _loadGood() {
    Preferences p; p.begin("stilhawt_wifi", true);
    _lastGood = p.getString("good", "");
    p.end();
  }
  void _saveGood(const String& s) {
    if (s == _lastGood) return;
    _lastGood = s;
    Preferences p; p.begin("stilhawt_wifi", false);
    p.putString("good", s);
    p.end();
  }

  // Association OK -> on decide si l'AP a un UPLINK reel avant de committer CONNECTED.
  void _verify(uint32_t now) {
    if (_uplinkOk()) {
      _uplink = UP_OK;
      _saveGood(WiFi.SSID());            // AP prouve : prefere-le au prochain boot/reconnect
      _forced = ""; _fallback = false;   // uplink OK : plus de contrainte de bascule
      _lastProbe = now;
      _journal(EV_CONNECT);
      _commitConnected();
    } else {
      _uplink = UP_NONE;
      _journal(EV_UPLINK_FAIL);
      _blacklist(WiFi.SSID(), now);
      if (!_forceNextCandidate(now)) {   // aucun autre AP a tenter -> on RESTE associe mais DEGRADE
        _commitConnected();              // (LED moyenne + re-test periodique ; le CPL peut revenir)
      }
    }
  }

  // Choisit le prochain AP connu (hors courant, hors blackliste) le mieux capte au scan, et le force.
  bool _forceNextCandidate(uint32_t now) {
    Preferences p; p.begin("stilhawt_wifi", true);
    int n = p.getInt("n", 0);
    String cur = WiFi.SSID(), pick, pickPass; int best = -999;
    int found = WiFi.scanNetworks();     // bloquant ~2s, seulement sur echec uplink
    for (int i = 0; i < n && i < 8; i++) {
      char ks[8], kp[8]; snprintf(ks, sizeof(ks), "ssid%d", i); snprintf(kp, sizeof(kp), "pass%d", i);
      String s = p.getString(ks, "");
      if (!s.length() || s == cur || _blacklisted(s, now)) continue;
      int r = -999;
      for (int j = 0; j < found; j++) if (WiFi.SSID(j) == s && WiFi.RSSI(j) > r) r = WiFi.RSSI(j);
      if (r > best) { best = r; pick = s; pickPass = p.getString(kp, ""); }
    }
    WiFi.scanDelete();
    p.end();
    if (!pick.length()) return false;
    _forced = pick; _forcedPass = pickPass; _fallback = true;
    _journal(EV_SWITCH);
    _beginForced();
    Serial.printf("[wifi] uplink KO -> bascule vers '%s' (rssi %d)\n", pick.c_str(), best);
    return true;
  }

  int loadAPs() {
    _nvs.begin("stilhawt_wifi", false);
    if (_nvs.getInt("n", -1) < 0) {
      String ls = _nvs.getString("ssid", "");
      if (ls.length()) { _nvs.putString("ssid0", ls);
        _nvs.putString("pass0", _nvs.getString("pass", "")); _nvs.putInt("n", 1); }
      else _nvs.putInt("n", 0);
    }
    int n = _nvs.getInt("n", 0), added = 0;
    for (int i = 0; i < n && i < 8; i++) {
      char ks[8], kp[8]; snprintf(ks, sizeof(ks), "ssid%d", i); snprintf(kp, sizeof(kp), "pass%d", i);
      String s = _nvs.getString(ks, ""), p = _nvs.getString(kp, "");
      if (s.length()) { _multi.addAP(s.c_str(), p.c_str()); added++; }
    }
    _nvs.end();
    return added;
  }

  void _commitConnected() {
    _state = CONNECTED;
    _dns.stop(); _portal.stop();
    if (MDNS.begin(_id)) {}
    Serial.printf("[wifi] connecte a %s @ %s — uplink %s\n",
                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), uplinkStr());
  }

  void _startPortal() {
    _state = PORTAL;
    _journal(EV_PORTAL);
    WiFi.mode(WIFI_AP);
    String ap = String(_id) + "-setup";
    WiFi.softAP(ap.c_str());
    _dns.start(53, "*", WiFi.softAPIP());
    _portal.onNotFound([this]() { _page(); });
    _portal.on("/",     [this]() { _page(); });
    _portal.on("/save", [this]() { _save(); });
    _portal.begin();
    Serial.printf("[wifi] PORTAIL CAPTIF -> connecte-toi a l'AP '%s' (192.168.4.1)\n", ap.c_str());
  }

  void _page() {
    String o = F("<!doctype html><meta charset=utf-8>"
      "<meta name=viewport content='width=device-width,initial-scale=1'>"
      "<style>body{font:16px system-ui;background:#0e1116;color:#e6edf3;padding:18px;max-width:420px;margin:auto}"
      "input{width:100%;padding:11px;margin:7px 0;border-radius:9px;border:1px solid #273140;background:#161b22;color:#e6edf3;box-sizing:border-box}"
      "button{width:100%;padding:13px;border:0;border-radius:9px;background:#4dd08a;color:#06210f;font-weight:700;font-size:16px}"
      ".d{color:#8b98a5;font-size:13px}</style>");
    o += "<h2>"; o += _id; o += " — config WiFi</h2>";
    o += F("<p class=d>Aucun WiFi connu joignable. Entre un reseau a portee.</p>"
           "<form action=/save method=post>"
           "<input name=ssid placeholder='nom du WiFi (SSID)' autofocus>"
           "<input name=pass placeholder='mot de passe' type=password>"
           "<button>Enregistrer &amp; redemarrer</button></form>");
    _portal.send(200, "text/html; charset=utf-8", o);
  }

  void _save() {
    String s = _portal.arg("ssid"), p = _portal.arg("pass");
    if (!s.length()) { _portal.send(400, "text/plain", "ssid requis"); return; }
    _portal.send(200, "text/html; charset=utf-8", "<meta charset=utf-8>Enregistre, redemarrage...");
    addAP(s, p, true);
  }

  void _blink(uint32_t now) {
    if (_dataBlinks > 0) {
      if (now - _tled >= 70) { _tled = now; _ledOn = !_ledOn; digitalWrite(_led, _ledOn); _dataBlinks--; }
      return;
    }
    // CONNECTED+uplink = fixe ; CONNECTED sans uplink = moyen (400) ; PORTAL = lent (800) ; CONNECTING = rapide (150).
    uint32_t period;
    if (_state == CONNECTED)   period = (_uplink == UP_OK) ? 0 : 400;
    else if (_state == PORTAL) period = 800;
    else                       period = 150;
    if (period == 0) { digitalWrite(_led, HIGH); _ledOn = true; return; }
    if (now - _tled >= period) { _tled = now; _ledOn = !_ledOn; digitalWrite(_led, _ledOn); }
  }
};
