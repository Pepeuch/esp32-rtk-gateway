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
  - [ ] W5500 Ethernet
  - [ ] DHCP
  - [ ] WebUI
  - [ ] LoRa init
  - [ ] RTCM pipeline
- [ ] Garder W5500 ISR inchangé pour l’instant
- [ ] Reporter l’optimisation Kconfig IRAM à plus tard
- [ ] Tester plus tard, une option à la fois :
  - [ ] `CONFIG_RINGBUF_PLACE_FUNCTIONS_INTO_FLASH`
  - [ ] `CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH`
  - [ ] `CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH`
- [ ] Comparer avant/après avec :
  - [ ] `idf.py size`
  - [ ] `idf.py size-components`
- [ ] Ne pas toucher aux options WiFi/flash/log IRAM tant que le firmware n’est pas stabilisé en usage réel

## TODO réseau / WebUI

- [ ] Tester avec un autre câble Ethernet
- [ ] Tester sur un autre switch/port
- [ ] Vérifier vitesse/négociation du lien
- [ ] Garder DHCP timeout à 30 s pour le moment
- [ ] Vérifier les fichiers WebUI manquants (/c/*.js)
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
- [ ] Ajouter profil board `mammotion_esp32s3_rtk`
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