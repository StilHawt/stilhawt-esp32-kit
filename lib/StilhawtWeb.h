// ============================================================================
//  StilhawtWeb.h — mini interface web LOCALE pour tout device stilhawt-things (shared/)
//
//  Standalone-first : voir l'etat du device sur son IP, SANS le broker MQTT.
//  Sert :
//    GET  /        -> page HTML legere (auto-contenue, zero dependance externe) :
//                     identite/fonction + [mode d'emploi] + reseau + mesures metier
//                     (rendues auto depuis /state) [+ form config WiFi si active].
//    GET  /state   -> JSON : champs communs + champs metier (callback).
//    POST /wifi    -> ajoute/maj un AP  (ACTIF SEULEMENT si onWifiSave() a ete appele).
//
//  Usage (main.cpp) :
//    #include <WebServer.h>               // ⚠ AVANT StilhawtWeb.h — le LDF PlatformIO ne scanne PAS
//    #include "StilhawtWeb.h"                 //   shared/, donc <WebServer.h> doit etre vu dans main.cpp
//    StilhawtWeb web;
//    web.begin(DEVICE_ID, "kind", "desc", FW_VERSION,
//              [](String& j){ j += ",\"kg\":"; j += String(lastKg,2); });
//    web.setDoc("Monte sur la balance -> poids publie.\nTopics: state/heartbeat...");  // AUTO-DOC
//    web.onWifiSave([](const String& s, const String& p){ saveAP(s,p); });  // OPT-IN config
//    web.server().on("/creds", handleCreds);   // endpoints custom du device (facultatif)
//    // loop() : web.handle();
//
//  AUTO-DOC (setDoc) : le device DEPLOYE porte sa propre marche a suivre. On se connecte,
//  on lit "comment m'utiliser" — pas besoin de doc externe. Pour un device cable, y mettre
//  le wiring en texte (le SVG n'a pas de sens dans une page de diagnostic terminal-like).
//
//  Le callback state APPEND des paires ,"cle":valeur (sans accolades) — la page les affiche
//  automatiquement (un device de plus = zero HTML a ecrire).
//
//  SECURITE — config WiFi par le web : onWifiSave() est OPT-IN (pas dans la regle obligatoire,
//  qui reste lecture seule). Page LAN non authentifiee : OK LAN de confiance ; device expose/B2B
//  -> preuve d'autorisation dans le callback (cf water-meter /creds), a minima un mdp admin.
// ============================================================================
#pragma once
#include <WiFi.h>
#include <WebServer.h>
#include <functional>
#include "StilhawtWifi.h"    // pour attachWifi() : liste des AP connus + switch force depuis l'IHM

class StilhawtWeb {
public:
  using StateFn = std::function<void(String&)>;                       // append ,"k":v
  using SaveFn  = std::function<void(const String&, const String&)>; // (ssid, pass)

  void begin(const char* id, const char* kind, const char* desc,
             const char* fw, StateFn stateFn) {
    _id = id; _kind = kind; _desc = desc; _fw = fw; _fn = stateFn;
    _srv.on("/",      [this]() { _srv.send(200, "text/html; charset=utf-8", page()); });
    _srv.on("/state", [this]() {
      _srv.sendHeader("Access-Control-Allow-Origin", "*");   // le board stilhawt-things interroge /state
      _srv.send(200, "application/json", state()); });
    _srv.begin();
  }

  void setDoc(const String& d) { _doc = d; }     // mode d'emploi / wiring texte, affiche dans l'IHM
  WebServer& server() { return _srv; }           // endpoints custom du device (ex. /creds)

  // OPT-IN : active le formulaire + l'endpoint de config WiFi. Sans appel = lecture seule.
  void onWifiSave(SaveFn fn) {
    _saveFn = fn; _cfg = true;
    _srv.on("/wifi", [this]() {
      String s = _srv.arg("ssid"), p = _srv.arg("pass");
      if (s.length() && _saveFn) {
        _srv.send(200, "text/html; charset=utf-8",
                  "<meta charset=utf-8>AP enregistre, redemarrage… <a href=/>retour</a>");
        delay(300);
        _saveFn(s, p);            // le device ecrit en NVS + reboot
      } else {
        _srv.send(400, "text/plain", "ssid requis");
      }
    });
  }

  // Branche la gestion WiFi : ajoute a l'IHM la liste des AP connus + un bouton "utiliser"
  // par AP (switch force). Sans cet appel, la carte WiFi n'apparait pas.
  void attachWifi(StilhawtWifi* w) {
    _wifi = w;
    _srv.on("/wifi/list", [this]() {
      _srv.sendHeader("Access-Control-Allow-Origin", "*");
      _srv.send(200, "application/json", _wifi ? _wifi->apListJson() : "[]");
    });
    _srv.on("/wifi/switch", [this]() {                 // ?ssid=... -> impose cet AP
      String s = _srv.arg("ssid");
      bool ok = _wifi && s.length() && _wifi->switchTo(s);
      _srv.send(ok ? 200 : 400, "text/plain", ok ? "switch en cours" : "ssid inconnu");
    });
    // POST /wifi/bulk : pousse PLUSIEURS AP d'un coup (body = lignes "ssid<TAB>pass\n"), UN seul
    // reboot a la fin. Sans dep JSON (StilhawtWeb reste autonome). Idempotent (addAP maj en place).
    // Sert le provisioning systematique "tous les AP" depuis le vault (valeur jamais loguee cote device).
    _srv.on("/wifi/bulk", HTTP_POST, [this]() {
      if (!_wifi) { _srv.send(400, "text/plain", "no wifi"); return; }
      String body = _srv.hasArg("plain") ? _srv.arg("plain") : "";
      int added = 0, from = 0;
      while (from < (int)body.length()) {
        int nl = body.indexOf('\n', from); if (nl < 0) nl = body.length();
        String line = body.substring(from, nl); line.trim();
        int tab = line.indexOf('\t');
        if (tab > 0) { String s = line.substring(0, tab), p = line.substring(tab + 1);
          if (_wifi->addAP(s, p, false)) added++; }   // addAP SANS reboot -> on cumule
        from = nl + 1;
      }
      _srv.send(200, "application/json", String("{\"added\":") + added + "}");
      if (added) { delay(400); ESP.restart(); }        // UN seul reboot pour charger tous les AP
    });
  }

  // OPT-IN : POST /mqtt?user=..&pass=.. → callback device (écrit les creds MQTT en NVS + reboot).
  // Pour provisionner un device HEADLESS (SdB) sans câble : vault → POST, valeur jamais loguée côté device.
  void onMqttSave(SaveFn fn) {
    _mqttSaveFn = fn;
    _srv.on("/mqtt", [this]() {
      String u = _srv.arg("user"), p = _srv.arg("pass");
      if (u.length() && _mqttSaveFn) {
        _srv.send(200, "text/html; charset=utf-8", "<meta charset=utf-8>Creds MQTT enregistrees, redemarrage…");
        delay(300);
        _mqttSaveFn(u, p);           // le device écrit en NVS + reboot
      } else {
        _srv.send(400, "text/plain", "user requis");
      }
    });
  }

  void handle() { _srv.handleClient(); }

private:
  WebServer   _srv{80};
  StilhawtWifi*   _wifi   = nullptr;
  SaveFn      _mqttSaveFn = nullptr;
  const char* _id   = "";
  const char* _kind = "";
  const char* _desc = "";
  const char* _fw   = "";
  StateFn     _fn;
  SaveFn      _saveFn = nullptr;
  bool        _cfg    = false;
  String      _doc;

  String state() {
    String j = "{";
    j += "\"id\":\"";   j += _id;   j += "\"";
    j += ",\"kind\":\""; j += _kind; j += "\"";
    j += ",\"fw\":\"";  j += _fw;   j += "\"";
    j += ",\"ssid\":\""; j += WiFi.SSID(); j += "\"";
    j += ",\"ip\":\"";  j += WiFi.localIP().toString(); j += "\"";
    j += ",\"rssi\":";  j += String(WiFi.RSSI());
    j += ",\"uptime_s\":"; j += String((uint32_t)(millis() / 1000));
    if (_fn) _fn(j);
    j += "}";
    return j;
  }

  String page() {
    String h = F("<!doctype html><meta charset=utf-8>"
      "<meta name=viewport content='width=device-width,initial-scale=1'>"
      "<style>body{font:15px system-ui,sans-serif;margin:0;background:#0e1116;color:#e6edf3}"
      ".w{max-width:520px;margin:0 auto;padding:18px}h1{font-size:19px;margin:0 0 2px}"
      ".d{color:#8b98a5;font-size:13px;margin:0 0 16px}.c{background:#161b22;border:1px solid #273140;"
      "border-radius:12px;padding:14px;margin-bottom:12px}.c h2{font-size:12px;letter-spacing:.05em;"
      "text-transform:uppercase;color:#8b98a5;margin:0 0 8px}.r{display:flex;justify-content:space-between;"
      "padding:6px 0;border-bottom:1px solid #1c232d}.r:last-child{border:none}.k{color:#8b98a5}"
      ".v{font-variant-numeric:tabular-nums}pre{white-space:pre-wrap;margin:0;font:13px/1.5 ui-monospace,monospace;color:#c9d4e0}"
      "input{flex:1;min-width:120px;padding:8px;border-radius:8px;border:1px solid #273140;background:#0e1116;color:#e6edf3}"
      "button{padding:8px 12px;border-radius:8px;border:0;background:#4dd08a;color:#06210f;font-weight:600}"
      "form{display:flex;gap:8px;flex-wrap:wrap}</style>");
    h += "<div class=w><h1>"; h += _id; h += "</h1><p class=d>"; h += _desc; h += "</p>";
    if (_doc.length()) { h += F("<div class=c><h2>Mode d'emploi</h2><pre>"); h += _doc; h += F("</pre></div>"); }
    h += F("<div class=c><h2>Mesures</h2><div id=m>...</div></div>"
           "<div class=c><h2>Reseau</h2><div id=n></div></div>");
    if (_wifi)
      h += F("<div class=c><h2>Reseaux WiFi connus</h2><div id=w class=d>...</div>"
             "<p class=d style='margin:8px 0 0'>\"utiliser\" impose l'AP (utile si le WiFi auto s'accroche a un signal faible).</p></div>");
    if (_cfg)
      h += F("<div class=c><h2>Config WiFi</h2>"
             "<form method=post action=/wifi>"
             "<input name=ssid placeholder=SSID>"
             "<input name=pass placeholder='mot de passe' type=password>"
             "<button>Ajouter</button></form>"
             "<p class=d style='margin:8px 0 0'>Ajoute un point d'acces (le device redemarre).</p></div>");
    h += F("<script>"
           "const NET=['ssid','ip','rssi','uptime_s'];"
           "const LBL={kg:'Poids (kg)',imp:'Impedance (ohm)',ssid:'WiFi',ip:'IP',rssi:'RSSI (dBm)',"
           "uptime_s:'Uptime (s)',fw:'Firmware',today_l:'Conso du jour (L)',total_l:'Index (L)',lpm:'Debit (L/min)'};"
           "async function u(){try{const s=await (await fetch('/state')).json();"
           "let m='',n='';for(const k in s){if(['id','kind','fw'].includes(k))continue;"
           "const row=`<div class=r><span class=k>${LBL[k]||k}</span><span class=v>${s[k]}</span></div>`;"
           "if(NET.includes(k))n+=row;else m+=row;}"
           "document.getElementById('m').innerHTML=m||'<div class=r><span class=k>en attente</span></div>';"
           "document.getElementById('n').innerHTML=n;}catch(e){}}"
           "u();setInterval(u,3000);"
           "</script>");
    if (_wifi)
      h += F("<script>"
             "async function wl(){const e=document.getElementById('w');if(!e)return;try{"
             "const a=await (await fetch('/wifi/list')).json();"
             "e.innerHTML=a.map(x=>`<div class=r><span class=k>${x.ssid}${x.cur?' <b style=color:#4dd08a>(actuel)</b>':''}</span>`+"
             "`<span>${x.cur?'':`<button onclick=\"sw('${x.ssid.replace(/'/g,'')}')\">utiliser</button>`}</span></div>`).join('')"
             "||'<div class=r><span class=k>aucun AP memorise</span></div>';}catch(e2){}}"
             "async function sw(s){if(!confirm('Forcer le WiFi vers '+s+' ?'))return;"
             "await fetch('/wifi/switch?ssid='+encodeURIComponent(s),{method:'POST'});"
             "document.getElementById('w').innerHTML='<div class=r><span class=k>switch en cours, reconnecte-toi a l IHM…</span></div>';}"
             "wl();setInterval(wl,5000);"
             "</script>");
    h += F("</div>");
    return h;
  }
};
