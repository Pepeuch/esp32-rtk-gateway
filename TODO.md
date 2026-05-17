## TODO stabilité firmware

Note:
- mise à jour selon l'état du repo au 2026-05-17
- seules les tâches prouvées dans le code sont cochées
- les tests matériel restent volontairement ouverts
- l'init LoRa ne provoque plus de boucle WDT si le radio init échoue; le boot continue sans LoRa
- des `BOOT_STEP(...)` existent déjà dans `main.c`, mais pas encore de diagnostic persistant complet

- [ ] Ajouter mode "safe boot"
  - désactiver LoRa si crash précédent
  - désactiver WebUI avancée si heap critique
  - démarrage minimal Ethernet + dashboard

- [ ] Ajouter watchdog diagnostics
  - dernière étape boot
  - dernier module initialisé
  - raison reset persistante
  - compteur crash boot

- [ ] Ajouter "factory minimal profile"
  - Ethernet
  - dashboard
  - GNSS simple
  - sans LoRa
  - sans MQTT
  - sans OTA

## TODO WebUI/API audit

- [x] Générer docs/webui_audit.md
- [x] Cartographier :
  - pages HTML
  - JS utilisés
  - endpoints API
  - endpoints réellement implémentés
- [x] Identifier :
  - legacy incarvr6
  - code mort
  - endpoints manquants
  - limites hardcodées
  - blocs dupliqués
- [x] Ajouter validation automatique assets/API

## TODO — LoRa RTK roles

### Base mode
- [x] Add LoRa regional profiles
- [x] Add SX126x radio layer
- [x] Add RTCM3 filter
- [x] Add RTCM LoRa fragmentation
- [x] Wire GNSS UART -> RTCM filter -> LoRa TX pipeline

### Rover mode
- [x] Add build-time ROVER role
- [x] Add LoRa RX -> RTCM reassembly -> GNSS UART output pipeline
- [ ] Test on ESP32-C3 hardware
- [ ] Split rover firmware into a smaller dedicated repo if needed
- [ ] Add minimal sdkconfig defaults for ESP32-C3 rover
- [ ] Remove unused dependencies from rover build completely
- [ ] Add ACK/retry or loss statistics
- [ ] Add duty-cycle manager
- [ ] Add RTCM profile selection per region

## TODO IRAM / mémoire

- [ ] Retirer `IRAM_ATTR` de `lora_dio1_isr`
- [ ] Rebuild et vérifier `idf.py size`
- [ ] Boot test complet :
  - [x] W5500 Ethernet
  - [x] DHCP
  - [x] WebUI
  - [ ] LoRa init
  - [ ] RTCM pipeline
- [x] Garder W5500 ISR inchangé pour l’instant
- [ ] Reporter l’optimisation Kconfig IRAM à plus tard
- [ ] Tester plus tard, une option à la fois :
  - [ ] `CONFIG_RINGBUF_PLACE_FUNCTIONS_INTO_FLASH`
  - [ ] `CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH`
  - [ ] `CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH`
- [ ] Comparer avant/après avec :
  - [ ] `idf.py size`
  - [ ] `idf.py size-components`
- [ ] Ne pas toucher aux options WiFi/flash/log IRAM tant que le firmware n’est pas stabilisé en usage réel

## TODO NET
- [ ] Vérifier vitesse/négociation du lien
- [ ] Garder DHCP timeout à 30 s pour le moment
- [x] Vérifier les fichiers WebUI manquants (/c/*.js)
- [ ] Vérifier génération SPIFFS complète
- [ ] Audit mémoire WebUI/config.html
- [ ] MQTT runtime control
- [ ] LoRa TX demand-driven
- [ ] Runtime TX timeout automatique
- [ ] API WebUI enable/disable TX
- [ ] MQTT topics:
  - [ ] rtk/base/tx/set
  - [ ] rtk/base/tx/status
- [ ] Préparer futur protocole rover → base LoRa

## TODO OTA / update manager

- [ ] Ajouter canal firmware : stable / beta / nightly
- [ ] Publier manifests OTA par profil
- [ ] WebUI : vérifier mise à jour disponible
- [ ] Comparer version actuelle / version distante
- [ ] Vérifier compatibilité board/chip/flash/psram
- [ ] Télécharger OTA app depuis GitHub Pages/Releases
- [ ] Vérifier SHA256 avant flash
- [ ] Mettre à jour aussi la partition WebUI/SPIFFS
- [ ] Ajouter rollback si boot échoue
- [ ] Ajouter bouton “update now” dans WebUI
- [ ] Ne jamais activer auto-update sans confirmation utilisateur

## TODO Mammotion migration workflow

### Detection / onboarding
- [x] Ajouter profil board `mammotion_esp32s3_rtk`
- [ ] Détection automatique Mammotion via logs / UART / signatures
- [ ] Assistant WebUI “Mammotion migration”
- [ ] Étape obligatoire avant flash :
  - analyse logs originaux
  - extraction paramètres device
  - validation utilisateur

### Backup original state
- [ ] Générer un backup JSON côté client uniquement
- [ ] Ne jamais envoyer les secrets sur serveur/GitHub
- [ ] Export backup JSON téléchargeable
- [ ] Ajouter version/schema backup :
  - mammotion_rtk_backup_v1

### Données à sauvegarder
- [ ] Device name / serial
- [ ] Versions firmware :
  - ESP32
  - GNSS
  - LoRa
- [ ] Paramètres LoRa :
  - région
  - fréquence
  - netid
  - canal
  - sous-canal
- [ ] Paramètres GNSS :
  - modèle
  - baudrate
  - profils RTCM
- [ ] Paramètres cloud/MQTT (option utilisateur)
- [ ] MAC WiFi/BLE
- [ ] Paramètres OTA détectés
- [ ] URLs OTA détectées
- [ ] Manifest OTA si disponible

### Import / restore
- [ ] Import backup JSON après flash
- [ ] Restaurer automatiquement :
  - LoRa
  - GNSS
  - identité device
  - paramètres réseau compatibles
- [ ] Bouton “Restore original Mammotion config”
- [ ] Validation compatibilité schema/version

### Sécurité
- [ ] Masquer secrets dans UI
- [ ] Confirmation avant export credentials
- [ ] Option chiffrement backup JSON
- [ ] Ajouter checksum/signature backup

### OTA / reverse engineering helper
- [ ] Capture OTA helper
- [ ] Détection URLs firmware
- [ ] Détection manifest OTA
- [ ] Journal OTA local
- [ ] Export analyse OTA

# TODO — Mesures qualité GNSS

## 1. Logs bruts GNSS

- [ ] Ajouter un mode `GNSS_RAW_LOG_ENABLE`
- [ ] Logger les trames brutes du récepteur GNSS :
  - UBX RXM-RAWX / RXM-SFRBX pour u-blox
  - logs RAWOBS / RANGEB / équivalent pour Unicore
- [ ] Sauvegarder les logs sur :
  - carte SD
  - ou SPIFFS/LittleFS
  - ou stream TCP/MQTT vers Raspberry Pi
- [ ] Ajouter rotation automatique des fichiers :
  - `gnss_raw_YYYYMMDD_HHMMSS.bin`
  - limite taille fichier
  - limite espace disque

## 2. Mesure cycle slip

- [ ] Lire les indicateurs disponibles par satellite/fréquence :
  - `locktime`
  - `LLI`
  - `half-cycle valid`
  - `cycle slip flag`
  - `carrier phase valid`
- [ ] Compter les pertes de lock par :
  - constellation
  - satellite
  - fréquence
- [ ] Calculer :
  - `slips_total`
  - `slips_per_sat_hour`
  - `slips_per_frequency_hour`
- [ ] Publier un résumé toutes les 10 à 60 s.

## 3. Mesure latence

- [ ] Timestamp ESP32 à la réception UART/SPI du GNSS
- [ ] Lire le timestamp GNSS dans les messages NAV/PVT
- [ ] Calculer :
  - `latency_ms = esp_rx_time - gnss_fix_time`
- [ ] Mesurer séparément :
  - latence GNSS → ESP32
  - latence ESP32 → ROS/MQTT
  - âge RTCM/NTRIP
- [ ] Publier :
  - `latency_avg_ms`
  - `latency_max_ms`
  - `rtcm_age_ms`

## 4. Phase RMS / Code RMS

- [ ] Vérifier si le récepteur expose déjà les résidus :
  - phase residual RMS
  - code residual RMS
  - RTK residuals
- [ ] Si disponible, parser ces valeurs directement.
- [ ] Sinon :
  - logger les observations brutes
  - calculer RMS hors ESP32 en post-traitement avec RTKLIB/Python
- [ ] Ne pas surcharger l’ESP32 avec un vrai solveur RTK complet.

## 5. Métriques qualité à exposer

- [ ] `fix_type`
- [ ] `carrier_solution`
- [ ] `num_satellites`
- [ ] `cn0_avg`
- [ ] `cn0_min`
- [ ] `rtk_fix_ratio`
- [ ] `rtk_float_ratio`
- [ ] `single_ratio`
- [ ] `cycle_slip_rate`
- [ ] `latency_ms`
- [ ] `rtcm_age`
- [ ] `hdop/vdop/pdop`
- [ ] `hacc/vacc`
- [ ] `phase_rms`
- [ ] `code_rms`

## 6. Export des données

- [ ] Ajouter sortie JSON diagnostic :
  - série USB
  - TCP
  - MQTT
  - micro-ROS
- [ ] Ajouter topic ROS 2 futur :
  - `/gnss/quality`
  - `/gnss/raw_status`
- [ ] Ajouter fichier CSV léger :
  - timestamp
  - fix_type
  - sats
  - cn0
  - slips
  - latency
  - rtk_age
  - hacc
  - vacc

## 7. Dashboard WebUI

- [ ] Ajouter page “GNSS Quality”
- [ ] Afficher :
  - état RTK
  - satellites par constellation
  - C/N0 moyen
  - cycle slip rate
  - latence
  - âge RTCM
  - historique fix/float/single
- [ ] Ajouter bouton :
  - démarrer log brut
  - arrêter log
  - télécharger log
  - reset compteurs

## 8. Tests terrain

- [ ] Test antenne fixe 30 min
- [ ] Test antenne fixe 24 h
- [ ] Test dynamique sur tondeuse
- [ ] Comparer plusieurs antennes au même emplacement
- [ ] Générer un rapport par antenne :
  - fix ratio
  - slip rate
  - CN0 moyen
  - latence moyenne/max
  - stabilité position ENU
