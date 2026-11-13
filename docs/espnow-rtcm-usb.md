# ESP-NOW 01 — RTCM vers USB (expérimental)

Chemin visé : caster NTRIP → BASE ESP32-S3 Ethernet → ESP-NOW → ESP32-C3/C6
ROVER → USB Serial/JTAG natif → Linux. Universal GNSS n'est pas porté sur ESP32.
Un seul couple, deux MAC STA explicitement configurées, un canal fixe commun.
Pas de WebUI nouvelle, appairage automatique, relais, LR ou nouveau mode radio.

## Audit et intégration

Le client récepteur existe dans `main/interface/ntrip_client.c` : requête GET,
réception du socket, puis `uart_write`. Les slots actuels de `ntrip_runtime.c`
émettent des requêtes SOURCE vers un caster : ce ne sont pas des récepteurs.
Le client GET n'était pas appelé par le démarrage courant. Le build BASE opt-in
appelle désormais son initialisation, qui respecte toujours `ntr_cli_active`.

Le tap ESP-NOW reçoit aussi le premier bloc de réponse, pouvant déjà contenir
du RTCM. Il retire les en-têtes HTTP (CRLF CRLF) ou la ligne ICY (CRLF), même
répartis sur plusieurs lectures, avant de présenter le corps au parseur.
Le comportement UART et tous les fichiers des composants LoRa restent inchangés.
Le client existant conserve ses limites : réponse initiale devant contenir la
ligne de statut complète, TCP non TLS, pas de décodage HTTP chunked. Choisir un
caster renvoyant un flux RTCM brut HTTP/ICY compatible. Les services exigeant
GGA nécessitent les données GGA du GNSS BASE existant ; aucun GGA n'est inventé.

`components/espnow_rtcm` réutilise exactement `rtcm_filter` (parseur, CRC24Q) et
`lora_transport` (fragmentation, réassemblage, CRC16), sans modifier ces composants.
Les paquets de 220 octets incluent les 13 octets d'en-tête/CRC existants :
207 octets utiles, au plus cinq fragments pour une trame RTCM de 1 029 octets.
Chaque trame réassemblée est revalidée par CRC24Q. Le CRC n'est pas une authentification.

La file bornée contient huit messages complets et n'écrase jamais le plus ancien.
La BASE parse les données dans le client NTRIP et copie une trame complète dans
la file sans attente radio. Un worker distinct fragmente/envoie, en attendant
chaque callback MAC. Un timeout de callback arrête les envois jusqu'au redémarrage,
pour qu'un callback tardif ne puisse pas acquitter le paquet suivant.
Un succès MAC n'est pas un acquittement applicatif du ROVER.

Côté ROVER, le callback Wi-Fi vérifie la MAC source et copie le paquet dans une
file bornée ; seul le worker réassemble et écrit sur USB. Un réassemblage incomplet
expire après 500 ms. Les anciens numéros de trame/relectures sont rejetés dans cette
fenêtre, et le bouclage 16 bits est accepté. Après expiration, la synchronisation
peut repartir, notamment après redémarrage BASE ; il n'y a pas d'identité de session
persistante ni de protection cryptographique anti-rejeu. Les messages ayant attendu
plus d'une seconde dans la file sont abandonnés. Une perte peut supprimer une
trame complète ; elle ne doit pas produire une trame RTCM partiellement réassemblée.

L'option BASE est désactivée par défaut et limitée au profil Waveshare S3 Ethernet
BASE audité. Lorsqu'elle est activée, le fallback Wi-Fi est supprimé : Ethernet
est obligatoire, le Wi-Fi STA non associé appartient au transport ESP-NOW.
Ne pas utiliser les commandes de reconfiguration Wi-Fi de l'ancienne interface
BASE pendant cet essai. La chaîne LoRa historique et ses profils ne sont pas
remplacés par cette option.

## Configurer et compiler, sans flash

ESP-IDF **v5.5.3** est la référence de compilation. Employer des répertoires de
build et SDKCONFIG séparés ; ne pas réutiliser le SDKCONFIG du firmware historique.
Le ROVER minimal est un projet distinct dans `firmware/espnow_rover` : aucun
`receiver`, Web, NTRIP, UART GNSS ou pilote radio LoRa applicatif n'y est lié.
Il utilise le contrôleur USB Serial/JTAG **natif** du C3/C6 ; une prise USB raccordée
uniquement à un pont UART externe ne transporte pas ce flux. Vérifier le câblage
de la carte avant toute qualification matérielle.

Créer deux fichiers privés non versionnés, par exemple sous `build/espnow01/` :

```text
# peer-base.defaults : MAC Wi-Fi STA réelle du ROVER
CONFIG_ESPNOW_RTCM_PEER_MAC="02:00:00:00:00:02"
CONFIG_ESPNOW_RTCM_CHANNEL=1
```

```text
# peer-rover.defaults : MAC Wi-Fi STA réelle de la BASE
CONFIG_ESPNOW_RTCM_PEER_MAC="02:00:00:00:00:01"
CONFIG_ESPNOW_RTCM_CHANNEL=1
```

Les MAC ci-dessus sont fictives, uniquement pour compilation/tests logiciels.
Les remplacer par les MAC STA des deux cartes. La MAC nulle bloque la compilation ;
les adresses multicast ou mal formées sont rejetées au démarrage. Aucun balayage
automatique, appairage ou changement de canal n'est effectué. Le transport de test
est non chiffré, avec filtrage MAC ; il ne protège pas contre une source usurpée.

Depuis la racine, composer les defaults du profil S3 habituel, puis ajouter l'option :

```bash
cat configs/templates/sdkconfig.common.defaults \
    configs/templates/sdkconfig.esp32s3.defaults \
    configs/templates/sdkconfig.flash_16mb.defaults \
    configs/templates/sdkconfig.psram_8mb.defaults \
    configs/templates/sdkconfig.board_waveshare_esp32s3_eth.defaults \
    configs/templates/sdkconfig.role_base.defaults \
    configs/templates/sdkconfig.eth_w5500.defaults \
    configs/templates/sdkconfig.gnss_unicore.defaults \
    configs/templates/sdkconfig.lora_sx1262.defaults \
    configs/templates/sdkconfig.lora_driver_sx126x.defaults \
    configs/templates/sdkconfig.region_eu868.defaults \
    configs/templates/sdkconfig.espnow_base.defaults \
    build/espnow01/peer-base.defaults > build/espnow01/base.defaults
idf.py -B build-espnow-base -D IDF_TARGET=esp32s3 \
    -D SDKCONFIG="$PWD/build/espnow01/base.config" \
    -D SDKCONFIG_DEFAULTS="$PWD/build/espnow01/base.defaults" build

idf.py -C firmware/espnow_rover -B "$PWD/build-espnow-rover" -D IDF_TARGET=esp32c3 \
    -D SDKCONFIG="$PWD/build/espnow01/rover.config" \
    -D "SDKCONFIG_DEFAULTS=$PWD/firmware/espnow_rover/sdkconfig.defaults;$PWD/build/espnow01/peer-rover.defaults" build
```

Ces commandes compilent seulement. Elles ne déclenchent pas de flash. Adapter le
profil mémoire à la carte réelle ; les defaults C3 minimaux utilisent la flash
IDF par défaut (2 Mo), qui n'identifie pas la capacité réelle d'une carte.
Les artefacts ne sont pas ajoutés à `docs/firmware` ni publiés.

Le client NTRIP BASE réutilise les clés NVS existantes `ntr_cli_active`,
`ntr_cli_host`, `ntr_cli_port`, `ntr_cli_mp`, `ntr_cli_user`, `ntr_cli_pass`,
accessibles par l'API de configuration existante `/api/config`.
Il faut configurer ces clés avant l'essai et redémarrer explicitement après
activation. Les identifiants restent dans la configuration locale, jamais dans
les defaults, commandes de test, logs ou commits. Aucun nouveau panneau n'est ajouté.

## Outil Linux et preuves logicielles

```bash
bash scripts/test_espnow_host.sh
python3 scripts/check_rtcm_usb.py /dev/serial/by-id/LE_ROVER \
    --frames 100 --timeout 30 --output build/espnow01/received.rtcm
```

Le script de tests compile le vrai codec et les sources LoRa/RTCM existantes avec
ASan/UBSan. Il couvre les 1 024 tailles RTCM, l'ordre/reclassement de fragments,
pertes, CRC16/CRC24Q, expiration, reset NTRIP, saturation sans écrasement,
en-têtes HTTP/ICY fragmentés et corps coalescés, bouclage des séquences.
Onze tests Python vérifient le parseur et des pseudo-terminaux Linux. Deux relient
un caster TCP local synthétique au codec C réel, à une radio simulée et au lecteur
PTY, avec et sans perte de fragment. Ni la tâche FreeRTOS NTRIP ni les pilotes
radio/USB physiques ne sont exécutés par ce harnais hôte.

Le lecteur ouvre le port en mode brut, vérifie longueur/réservés/CRC24Q et refuse
tout octet étranger : il ne masque pas les logs de démarrage. Il écrit uniquement
les trames valides dans le fichier de capture, et son bilan JSON sur stdout.
`--frames` est un minimum ; un début de trame suivante à la fin d'une lecture est
signalé par `pending_bytes`, pas interprété comme une corruption. Une perte de
trame complète n'est détectable qu'avec un nombre/contenu attendu ou une mesure
de débit externe ; le CRC seul ne démontre pas une livraison sans perte.

## Limites matérielles et prochaine qualification

Les builds BASE S3, ROVER C3 et LoRa BASE/ROVER avec ESP-NOW désactivé sont des
preuves logicielles. Les premiers lots physiques S3/C3 avec caster synthétique local sont documentés
dans [la procédure matérielle](espnow-hardware-tests.md). La portée, les essais
longue durée, le crash et un caster GNSS réel restent à qualifier.
Console, logs applicatifs/bootloader et impression de panic sont désactivés côté
ROVER. Les messages ROM antérieurs à l'application ne sont pas garantis absents
sans essai sur la carte réelle ; aucun eFuse n'est modifié.

Une écriture USB partielle est reprise avec un délai total borné de 200 ms.
Un hôte absent, débranché ou bloqué peut tronquer le flux après les octets déjà
écrits : ce n'est pas un transport transactionnel. Le lecteur strict doit alors
signaler corruption/timeout ; aucune absence de perte lors d'un débranchement
n'est promise. Les compteurs de pertes restent locaux au firmware, sans nouvelle UI.

Sur deux cartes, vérifier successivement : MAC/canal, Ethernet obligatoire,
RTCM simple puis maximum fragmenté, capture USB et comparaison au flux source,
perte/corruption radio, saturation, USB sans lecteur puis reprise, boot/reset/crash
sans logs dans la capture, puis profils LoRa historiques. Les commandes de compilation ne flashent pas les cartes. Les flashs et essais
physiques autorisés séparément sont consignés dans la procédure matérielle.

## Consommation par Universal GNSS

L'audit en lecture seule d'UG trouve le transport série POSIX, les parseurs RTCM,
le writer de corrections et l'entrée ROS2 `RtcmFrame` du receiver. Le `ntrip_node`
actuel produit ce topic à partir du réseau ; le runtime natif alimente les
corrections depuis son client NTRIP. Aucun adaptateur autonome configuré pour
**lire un deuxième port série RTCM-only** et l'injecter comme source de corrections
n'a été identifié dans ces chemins actifs.

Une intégration ultérieure devra ouvrir le port du C3 comme source de corrections,
valider le RTCM, puis utiliser le topic RTCM/chemin d'injection existant vers le
vrai récepteur GNSS. Le C3 ne doit pas être configuré comme récepteur de position.
Il reste à définir ownership, fraîcheur, reconnexion et invalidation de cette
source distincte. Aucun fichier Universal GNSS n'est modifié dans ce lot.

La préparation S3/C3/C6, les contrôles de partitions et les captures Linux sont
décrits dans [la procédure matérielle](espnow-hardware-tests.md). La cible rover
est choisie par `-D IDF_TARGET=esp32c3` ou `esp32c6`, sans duplication du code.

Pour la campagne S3/C3, utiliser les defaults privés canal 1 et la table OTA
originale C3 en flash 4 Mo ; les anciens binaires fictifs 2 Mo ne conviennent pas.
Le firmware S3 de cette campagne désactive LoRa par configuration et conserve
Ethernet. Le contrôle du canal ne connecte pas le STA au point d'accès.
