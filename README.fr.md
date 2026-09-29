# stilhawt-esp32-kit

[English](README.md) · **Français**

**Un petit socle ESP32 et deux firmwares qui s'en servent** : une balance de salle de bain lue en
Bluetooth, et un compteur d'eau à impulsions qui distingue un vrai débit d'une impulsion fantôme.

Tout se compile avec [PlatformIO](https://platformio.org). Aucun service en ligne n'est requis : les
appareils publient en MQTT vers **votre** broker, et chacun sert une petite page web locale.

## Le socle (`lib/`)

| En-tête | Ce qu'il apporte |
|---|---|
| `StilhawtWifi.h` | WiFi non bloquant, plusieurs réseaux mémorisés, **portail captif** de secours quand aucun ne répond, LED d'état |
| `StilhawtOta.h` | mise à jour **par le réseau** (OTA) — **sans mot de passe** aujourd'hui (voir « Sécurité ») |
| `StilhawtWeb.h` | une page web locale : identité de l'appareil, `/state` en JSON, liste des réseaux — et des formulaires qui **modifient** la configuration (WiFi, MQTT, calibrage), **sans authentification** |
| `mqtt_ca.h` | un emplacement pour l'autorité de certification de votre broker — **pas encore lue** par les firmwares (voir « Sécurité ») |

## Les deux exemples

**`examples/water-meter`** — TTGO T-Display, compteur d'eau à **double sortie, trois fils** :
noir = commun (GND), rouge = OUT1 sur **GPIO27** (1 impulsion = 1 litre, c'est elle qui compte),
vert = OUT2 sur **GPIO25** (seconde sortie dont le rôle reste à caractériser : sens du débit, contacts
inversés ou détection de fraude — elle est journalisée et n'agit pas sur le comptage).
- Compte le jour et les 30 derniers jours, garde les totaux en mémoire non volatile, se cale sur NTP.
- **Rejette les impulsions impossibles** : même à 60 L/min, deux litres sont espacés d'au moins une
  seconde. Un écart de moins de 700 ms n'est pas de l'eau mais du bruit électrique (une masse qui
  lâche suffit). Ces impulsions sont refusées **et comptées** (`glitches` dans l'état publié) : une
  « micro-fuite » la nuit se lit alors pour ce qu'elle est.
- Deux écrans, un bouton, la page web locale, MQTT.

**`examples/etekcity-scale`** — ESP32 (WROOM), sans écran : pont Bluetooth → MQTT pour une balance
à impédance Etekcity FIT-8S.
- La balance **diffuse** son poids et son impédance dans ses annonces Bluetooth ; l'ESP32 les écoute
  en passif, sans appairage, et les publie. Aucun calcul de composition corporelle ici : les valeurs
  brutes seulement.
- Le format des annonces est décrit en tête de `src/main.cpp`.

## Démarrer

Il faut Python 3 et `git`.

```bash
git clone https://github.com/StilHawt/stilhawt-esp32-kit
cd stilhawt-esp32-kit
pip install platformio
```

Puis, pour chaque exemple :

1. **Configurez** `examples/<exemple>/src/zdt_device.h` : le nom de l'appareil, la base de ses topics
   MQTT, l'adresse et le port de votre broker. TLS est désactivé par défaut. `ZDT_MQTT_TLS 1` (et le
   port 8883) **chiffre** la connexion, mais le broker n'est **pas vérifié** (voir « Sécurité »). La
   valeur doit être exactement `0` ou `1` : toute autre valeur compile et désactive le TLS.
2. **Compilez** : `pio run -d examples/water-meter` (ou `examples/etekcity-scale`).
3. **Flashez** la carte branchée en USB : `pio run -d examples/water-meter -t upload`.
4. **Donnez-lui le WiFi et le compte MQTT** par le port série (115200 bauds), jamais dans le code :
   `WIFI <ssid> <mot de passe>` puis `MQTT <utilisateur> <mot de passe>`. Sans réseau connu,
   l'appareil ouvre son portail captif. **Le broker doit avoir un compte** : sans compte, le firmware
   n'essaie pas de se connecter, donc un broker anonyme n'est pas utilisable aujourd'hui.

Les mises à jour suivantes peuvent passer par le réseau (OTA) : `pio run -d examples/water-meter -t
upload --upload-port <adresse de l'appareil>`. **La balance** fait exception : Bluetooth et WiFi se
partagent la radio, son OTA n'est ouverte que 120 secondes après avoir reçu `{"ota":true}` sur son
topic `cmd` MQTT (elle redémarre pour l'ouvrir) — il faut donc un broker qui fonctionne.

## Sécurité — à lire avant de brancher

Ces firmwares sont faits pour un **réseau de confiance** (votre réseau domestique), pas pour un réseau
ouvert. Trois limites connues, dites telles quelles :

- **TLS sans vérification du serveur.** Avec `ZDT_MQTT_TLS 1`, la connexion est chiffrée, mais le
  firmware appelle `setInsecure()` : il ne vérifie pas l'identité du broker, et `lib/mqtt_ca.h`
  n'est pas lu. Un intermédiaire sur le chemin pourrait se faire passer pour votre broker.
- **OTA sans mot de passe.** Toute machine du même réseau peut reflasher l'appareil (port 3232).
- **Page web sans authentification.** Les formulaires de la page locale changent le WiFi, le compte
  MQTT ou le calibrage, sans mot de passe.

Ces trois points sont les prochains chantiers de ce dépôt.

## État

Les deux exemples sont **compilés** à chaque version, depuis ce dépôt seul, dans un conteneur neuf.
Le **compteur d'eau** a aussi **tourné dans le simulateur Wokwi** depuis ce dépôt (broker public de
test, TLS coupé) : WiFi donné par le port série, 3 vrais litres et 2 impulsions impossibles →
`total_l=3`, `glitches=2` dans l'état publié. Non simulés : la balance (Bluetooth), l'écran, l'OTA et
la page web. Pas encore rejoué sur une carte physique depuis ce dépôt. Les commentaires du code sont
en français.

## Licence

Apache License 2.0 — voir [LICENSE](LICENSE). Copyright 2026 StilHawt.
