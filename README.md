# stilhawt-esp32-kit

**Un petit socle ESP32 et deux firmwares qui s'en servent** : une balance de salle de bain lue en
Bluetooth, et un compteur d'eau à impulsions qui distingue un vrai débit d'une impulsion fantôme.

Tout se compile avec [PlatformIO](https://platformio.org). Aucun service en ligne n'est requis : les
appareils publient en MQTT vers **votre** broker, et chacun sert une petite page web locale.

## Le socle (`lib/`)

| En-tête | Ce qu'il apporte |
|---|---|
| `StilhawtWifi.h` | WiFi non bloquant, plusieurs réseaux mémorisés, **portail captif** de secours quand aucun ne répond, LED d'état |
| `StilhawtOta.h` | mise à jour **par le réseau** (OTA), avec un écran « OTA en cours » sur les cartes qui en ont un |
| `StilhawtWeb.h` | une page web locale : identité de l'appareil, `/state` en JSON, liste des réseaux |
| `mqtt_ca.h` | l'autorité de certification de **votre** broker, si vous activez TLS |

## Les deux exemples

**`examples/water-meter`** — TTGO T-Display, compteur à sortie reed (1 impulsion = 1 litre sur GPIO27).
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
   MQTT, l'adresse et le port de votre broker. TLS est désactivé par défaut ; pour l'activer, mettez
   `ZDT_MQTT_TLS` à 1, le port à 8883, et collez le certificat de votre autorité dans `lib/mqtt_ca.h`.
2. **Compilez** : `pio run -d examples/water-meter` (ou `examples/etekcity-scale`).
3. **Flashez** la carte branchée en USB : `pio run -d examples/water-meter -t upload`.
4. **Donnez-lui le WiFi et le compte MQTT** par le port série (115200 bauds), jamais dans le code :
   `WIFI <ssid> <mot de passe>` puis `MQTT <utilisateur> <mot de passe>`. Sans réseau connu,
   l'appareil ouvre son portail captif.

Les mises à jour suivantes peuvent passer par le réseau (OTA) une fois l'appareil connecté.

## Licence

Apache License 2.0 — voir [LICENSE](LICENSE). Copyright 2026 StilHawt.
