# ESP-NOW 01 — préparation matérielle S3 / C3 / C6

Statut : expérimental. La préparation initiale était logicielle ; les premiers
essais S3/C3 sont décrits dans la section de résultats ci-dessous.
Référence logicielle initiale : `4a43ac00a48e961abcaab13e0e123e77f94effd8`.
Les modifications de préparation doivent être enregistrées avec les preuves locales
avant chaque essai. Aucun flash n'est lancé par les scripts de test du dépôt.
Universal GNSS n'est pas nécessaire.

## 1. Identifier avant toute ouverture du port

Exécuter sur l'hôte Linux, depuis la racine du dépôt, avec ESP-IDF **5.5.3** activé.
Débrancher/rebrancher une seule carte à la fois et observer :

```bash
ls -l /dev/serial/by-id/
udevadm monitor --udev --subsystem-match=tty
```

Dans un autre terminal, définir les chemins persistants réellement observés :

```bash
export BASE_PORT='/dev/serial/by-id/IDENTIFIANT_S3_OBSERVE'
export ROVER_PORT='/dev/serial/by-id/IDENTIFIANT_C3_OBSERVE'
readlink -f "$BASE_PORT"
readlink -f "$ROVER_PORT"
udevadm info --query=property --name="$BASE_PORT"
udevadm info --query=property --name="$ROVER_PORT"
fuser -v "$BASE_PORT" "$ROVER_PORT"
```

Ne pas se fier à ttyACM0/1 : ces numéros changent. Fermer les moniteurs série et
vérifier que ModemManager ne sonde pas ces ports pendant les essais. L'identifiant
USB n'est pas une preuve suffisante de la MAC Wi-Fi ni du modèle de carte Ethernet.
Confirmer visuellement la référence S3 Waveshare, le W5500, la mémoire et le câblage.
C3 : USB natif GPIO18 D− / GPIO19 D+ ; C6 : GPIO12 D− / GPIO13 D+.
Vérifier le schéma du SuperMini réel, pas seulement son nom commercial.
Référence : [USB Serial/JTAG C6 ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c6/api-guides/usb-serial-jtag-console.html).

Après identification explicite de la carte et du port seulement, les commandes
suivantes interrogent la ROM sans écrire la flash. Elles peuvent réinitialiser
la carte et interrompre le firmware courant. Répéter avec le port rover validé :

```bash
python -m esptool --chip auto --port "$BASE_PORT" chip_id
python -m esptool --chip esp32s3 --port "$BASE_PORT" read_mac
python -m esptool --chip esp32s3 --port "$BASE_PORT" flash_id
python -m esptool --chip esp32s3 --port "$BASE_PORT" get_security_info
```

Exiger esp32s3 pour BASE, esp32c3 pour C3, esp32c6 pour C6. Arrêter si le type,
la mémoire, Secure Boot/chiffrement ou le port contredit les hypothèses.
Ne modifier aucun eFuse. Conserver les identités et MAC réelles dans des fichiers
privés ignorés sous `build/espnow01/`, jamais dans la documentation versionnée.
Le code courant ne remplace pas la MAC Wi-Fi STA ; utiliser la MAC usine appropriée
indiquée par esptool et confirmer l'identité lors de l'essai radio. Ne pas déduire
une MAC depuis le seul numéro USB.

## 2. Audit des premiers builds conservés (avant campagne)

| Paramètre | BASE S3 | ROVER C3 | ROVER C6 préparé |
|---|---|---|---|
| SDK | 5.5.3 | 5.5.3 | 5.5.3 |
| Rôle | Waveshare Ethernet BASE | minimal USB | même minimal USB |
| Mémoire du build | 16 Mo flash / PSRAM octale 8 Mo | flash 2 Mo, sans PSRAM | flash 2 Mo, sans PSRAM |
| Canal | 6 | 6 | 6 |
| Pair compilé | fictif `02:00:00:00:00:02` | fictif `02:00:00:00:00:01` | fictif `02:00:00:00:00:01` |
| Application | 0x20000 (ota_0) | 0x10000 (factory) | 0x10000 (factory) |
| Table | 0x8000, personnalisée | 0x8000, single-app | 0x8000, single-app |
| Console / logs | actifs, diagnostic BASE | désactivés | désactivés |

BASE : nvs 0x9000/24K, otadata 0xf000/8K, phy 0x11000/4K,
ota_0 0x20000/3M, ota_1 0x320000/3M, www 0x620000/9M,
coredump 0xf20000/256K. Rovers : nvs 0x9000/24K, phy 0xf000/4K,
factory 0x10000/1M. Les rovers compilés tiennent dans cette partition.
Le profil BASE active les composants LoRa historiques, mais RTK_LORA_TX_ENABLED
est désactivé dans cette configuration ; aucun composant historique n'est modifié.
**Ces binaires à MAC fictives ne sont pas prêts pour l'essai réel.**

## 3. Recompiler pour les paires physiques

Suivre le guide [de compilation](espnow-rtcm-usb.md) pour le S3. Placer la MAC STA
réelle du rover choisi dans `peer-base.defaults`, celle du S3 dans
`peer-rover.defaults`, avec le même canal autorisé (1 à 11).
Pour remplacer C3 par C6, reconstruire aussi BASE avec la nouvelle MAC du rover.
Ne pas faire fonctionner les deux rovers comme s'ils étaient simultanément appairés.

Le projet et les defaults rover sont communs. La cible est sélectionnée uniquement
par la commande ; ne jamais réutiliser un SDKCONFIG ou build C3 pour C6 :

```bash
# TARGET vaut esp32c3, puis esp32c6 dans un deuxième essai.
TARGET=esp32c6
idf.py -C firmware/espnow_rover -B "$PWD/build/espnow01/$TARGET-build" \
  -D IDF_TARGET="$TARGET" \
  -D SDKCONFIG="$PWD/build/espnow01/$TARGET.config" \
  -D "SDKCONFIG_DEFAULTS=$PWD/firmware/espnow_rover/sdkconfig.defaults;$PWD/build/espnow01/peer-rover.defaults" build
```

Contrôler dans le SDKCONFIG effectif : cible, ROVER=y, BASE désactivé,
MAC réelle du S3, canal, taille flash compatible, consoles primaire/secondaire NONE,
logs applicatifs/max/bootloader à 0, panic silent reboot. La BASE doit être le profil
Ethernet S3 attendu, avec MAC rover correcte. Inspecter `flasher_args.json`,
`project_description.json`, les versions SDK, le diff source et les SHA256 des
binaires. `python -m esptool image_info <application.bin>` doit confirmer la cible.

## 4. Partitions, sauvegarde puis flash contrôlé

**Commandes à préparer, pas à exécuter sans validation des ports et du matériel.**
Créer un dossier privé de preuves (`umask 077`). Sauvegarder la table existante :

```bash
mkdir -p build/espnow01/hardware
python -m esptool --chip esp32c3 --port "$ROVER_PORT" read_flash \
  0x8000 0x1000 build/espnow01/hardware/c3-existing-table.bin
python "$IDF_PATH/components/partition_table/gen_esp32part.py" \
  build/espnow01/hardware/c3-existing-table.bin
python "$IDF_PATH/components/partition_table/gen_esp32part.py" \
  build/espnow01/esp32c3-build/partition_table/partition-table.bin
```

Adapter le chip et les noms pour S3/C6. Comparer les tables décodées (le dump brut
contient aussi le remplissage du secteur). Sauvegarder la flash existante avec
`read_flash 0 <taille_physique_en_octets> <sauvegarde.bin>` après `flash_id`.
Ces sauvegardes peuvent contenir les identifiants NTRIP : ne pas les versionner
ni les publier. Relever la partition active et l'état OTA du S3 ; ne pas supposer
que ota_0 est active. Conserver NVS et son contenu.

Si table, bootloader et sécurité sont compatibles ET l'application ciblée est
celle qui démarrera, privilégier **app-flash** seulement :

```bash
idf.py -C firmware/espnow_rover -B "$PWD/build/espnow01/esp32c3-build" \
  -D SDKCONFIG="$PWD/build/espnow01/esp32c3.config" -p "$ROVER_PORT" app-flash
```

Adapter exactement le build/cible validés. Ne pas exécuter ce raccourci sur une
carte vierge, avec une table incompatible ou un slot OTA actif inconnu.
Dans ces cas, arrêter et faire valider séparément le plan de provisionnement initial
(bootloader/table/application et sauvegarde), avec justification du changement de
partitions. Ne pas lancer `erase_flash`, `erase_region`, ni le `flash` complet par
habitude. Un flash applicatif efface quand même les secteurs de l'application.
Vérifier ensuite les octets écrits avec esptool `verify_flash` à l'offset prévu et
le binaire exact. Relever les SHA256 et la procédure de restauration avant l'essai.

## 5. Essai S3 → C3, puis S3 → C6

1. Deux cartes proches, antennes/câblage conformes, Ethernet BASE branché.
   Pas de GNSS/Pixhawk requis. Alimentations stables, pas d'autres pairs ESP-NOW.
2. Configurer le client GET NTRIP existant (`ntr_cli_active`, hôte/port/mountpoint)
   vers un caster synthétique sur le LAN Ethernet, puis redémarrer BASE.
   Pas de TLS/HTTP chunked ; réponse HTTP 200 ou ICY compatible, pas d'obligation GGA.
   Les slots SOURCE ne remplacent pas ce client récepteur. Ne pas utiliser de vrais
   identifiants pour cet essai. Aucun réseau extérieur n'est nécessaire.
3. Le caster doit avoir un flux fini conservé sous `expected.rtcm`, composé de
   100 trames distinctes CRC24Q valides. Inclure un numéro unique dans le payload.
   Faire d'abord un lot de petits payloads (32 octets), puis un lot de 1023 octets
   (1029 avec entête/CRC, cinq fragments radio). Ne pas confondre ces payloads
   synthétiques de transport avec des messages RTCM sémantiquement utilisables.
4. Préparer le lecteur USB AVANT l'écoute du caster ; répondre HTTP/1.1 puis
   envoyer immédiatement le corps. Le client a un timeout de lecture de 2 s.
   Émettre chaque trame entière à 5 Hz, fermer la connexion après le lot et ne pas
   rejouer le lot lors des reconnexions NTRIP. Conserver l'ordre et les octets exacts
   servis après l'entête. Exemple de génération (depuis la racine) :

```bash
PYTHONPATH=scripts python3 - <<'PY'
from pathlib import Path
from check_rtcm_usb import crc24q
size = 1023  # refaire avec 32 pour le lot simple
frames = []
for sequence in range(100):
    payload = sequence.to_bytes(4, 'big') + bytes(i % 256 for i in range(size - 4))
    body = bytes([0xd3, size >> 8, size & 255]) + payload
    frames.append(body + crc24q(body).to_bytes(3, 'big'))
Path('build/espnow01/hardware/expected.rtcm').write_bytes(b''.join(frames))
PY
```

Exemple de caster à lancer dans un terminal distinct (adresse LAN fictive à remplacer).
Il attend l'appui sur Entrée quand le lecteur est prêt, puis écoute et sert
une connexion une seule fois, sans attente entre l'en-tête et les trames. Le délai de lecture de la requête est borné :

```bash
PYTHONPATH=scripts python3 - 192.0.2.10 2101 <<'PY'
import socket
import sys
import time
from check_rtcm_capture import load_frames
frames = load_frames('build/espnow01/hardware/expected.rtcm')
print('Lecteur USB pret apres capture du boot ? Entree pour ouvrir le caster.', flush=True)
with open('/dev/tty') as terminal:
    if not terminal.readline():
        raise RuntimeError('confirmation absente')
with socket.socket() as server:
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((sys.argv[1], int(sys.argv[2])))
    server.listen(1)
    server.settimeout(120)
    print('Attente du client NTRIP BASE : redemarrer BASE si necessaire.', flush=True)
    connection, _ = server.accept()
    with connection:
        connection.settimeout(30)
        request = bytearray()
        while b'\r\n\r\n' not in request:
            data = connection.recv(1024)
            if not data:
                raise RuntimeError('requete interrompue')
            request.extend(data)
            if len(request) > 8192:
                raise RuntimeError('requete trop longue')
        if not request.startswith(b'GET '):
            raise RuntimeError('GET attendu, pas SOURCE')
        connection.sendall(b'HTTP/1.1 200 OK\r\n\r\n')
        for frame in frames:
            connection.sendall(frame)
            time.sleep(0.2)
print('Lot termine, aucun rejeu automatique.', flush=True)
PY
```

Capturer d'abord la phase ROM séparément (les essais archivés gardent la même
ouverture série), puis armer le lecteur strict avant de confirmer l'écoute TCP.
Ne pas masquer du texte de boot dans une capture prétendument RTCM pure. Une fois
le caster prêt, redémarrer BASE si son retry NTRIP attend trop longtemps. En cas de reconnexion avant le corps,
recommencer l'essai, sans considérer la capture comme une mesure de pertes.

Le caster envoie `HTTP/1.1 200 OK\r\n\r\n` après lecture de la requête GET
complète, puis immédiatement les trames à la cadence indiquée. Le socket est lié
uniquement à l'adresse LAN de test ; le lecteur est déjà armé. Un serveur servant
le fichier avant ouverture du port USB ne fournit pas une mesure de pertes exploitable.

```bash
python3 scripts/check_rtcm_usb.py "$ROVER_PORT" --frames 100 --timeout 45 \
  --output build/espnow01/hardware/c3.received.rtcm \
  > build/espnow01/hardware/c3.stats.json \
  2> build/espnow01/hardware/c3.errors.log
python3 scripts/check_rtcm_capture.py build/espnow01/hardware/expected.rtcm \
  build/espnow01/hardware/c3.received.rtcm
```

Le lecteur restaure les attributs termios et ne journalise rien dans le fichier
binaire. Ne pas lancer IDF monitor, screen ou un deuxième lecteur sur ce port.
Il s'arrête au minimum demandé (peut inclure plusieurs trames du dernier read).
Si timeout, capture partielle conservée : comparer quand même, mais l'essai reste
FAIL. Le comparateur doit rendre 0 avec égalité exacte des octets et de l'ordre,
zéro perte, zéro ajout ; CRC24Q et absence de pollution obligatoires.
Une capture vide, tronquée, polluée ou corrompue est rejetée, sans resynchronisation
silencieuse. Répéter avec le C6 après identification, recompilation des deux pairs
et validation du flash ; utiliser des noms de captures distincts.

## 6. Compteurs, débit et essais négatifs

- Le bilan USB contient trames, octets validés, secondes monotones, octets/s,
  trames/s, octets incomplets restants. Le débit inclut l'attente initiale : noter
  le délai de synchronisation ; ce n'est pas un débit RF maximal.
- Le comparateur compte les occurrences manquantes et inattendues et vérifie
  séparément l'ordre. Des trames uniques évitent l'ambiguïté des doublons identiques.
  Un CRC valide seul ne révèle pas une trame entièrement perdue.
- En cas de CRC24Q incorrect, le lecteur échoue au premier défaut : il ne fournit
  pas un compteur exhaustif de toutes les erreurs. Compter les essais en échec
  et garder stderr ; ne pas annoncer zéro erreur après un arrêt prématuré.
- `s_dropped` (file pleine/âge), `s_invalid` (échec radio/codec) et `s_usb_errors`
  existent dans le firmware mais sont internes, sans export opérateur. Aucun log
  n'est ajouté au flux USB. Un relevé éventuel au débogueur est intrusif et doit
  être distingué de la mesure de débit. Pas de compteur matériel exporté prétendu.
- Éloignement/masquage RF : vérifier qu'aucune trame CRC invalide n'est acceptée ;
  les pertes entières doivent apparaître dans la comparaison/timeout.
- Augmenter progressivement la cadence pour la saturation, noter débit et pertes ;
  ni livraison sans perte ni seuil de capacité ne sont présupposés.
- Débrancher USB, arrêter le lecteur puis reprendre, couper NTRIP, redémarrer BASE,
  débrancher Ethernet. Vérifier les limites documentées (USB partiel possible,
  timeout TX pouvant nécessiter reboot). Nouvelle capture pour chaque incarnation.
- Capturer aussi boot/reset à part ; les octets ROM restent à qualifier. Ne pas
  filtrer du bruit pour annoncer une capture RTCM pure.
- L'injection précise d'un fragment radio perdu/corrompu reste couverte en logiciel ;
  aucun mode d'injection RF n'a été ajouté. Ne pas présenter l'atténuation radio
  comme une preuve de perte d'un fragment précis.

Pour chaque essai : carte/révision, firmware+SHA256+config privée, MAC/canal,
partitions, OS/kernel, chemin USB, topology/alimentation/distance, source/cadence,
commande, durée, résultats attendus/observés et causes de FAIL. Préserver les deux
suppressions de manifestes préexistantes ; aucune preuve locale n'est à versionner.

## Premiers résultats S3/C3 — 13 novembre 2026

Campagne effectuée dans un working tree non commité basé sur `4a43ac0`.
La MAC de chaque puce a été lue par esptool ; pour ce firmware, le SDK dérive
la MAC STA directement de cette MAC usine, sans remplacement applicatif.
La réception radio confirme les pairs configurés. Identités réelles et defaults
privés restent dans le répertoire de preuves ignoré, pas dans ce guide.

Les flashs complets S3 16 Mo et C3 4 Mo ont été sauvegardés et comparés par
`verify_flash`. La première sauvegarde C3 différait après reboot sur un seul
secteur NVS ; une seconde sauvegarde complète sans redémarrage applicatif entre
lecture et comparaison a été vérifiée. Les deux copies sont conservées.
Les applications seules ont été écrites : S3 `ota_0` à `0x20000`, C3 `app0` à
`0x10000`. Les tables originales, bootloaders et métadonnées OTA ont été conservés.
Le build C3 reprend exactement sa table OTA/SPIFFS existante et sa flash de 4 Mo.
Aucun repartitionnement, effacement global ou flash C6 n'a été effectué.

Le canal de cette campagne est **1**, validé par le scan Ubiquiti du mainteneur.
Un scan NetworkManager depuis le conteneur a été refusé par AppArmor : aucune
protection n'a été contournée. Le S3 confirme au démarrage canal primaire 1,
secondaire 0, HT20 ; le C3 vérifie également le canal avant d'accepter les trames.
Le Wi-Fi STA ne s'associe pas à un point d'accès. Ethernet W5500 est conservé.
La fonctionnalité LoRa est désactivée uniquement dans les defaults privés de
cet essai ; ses composants ne sont pas modifiés.

Le contrôle du canal initialise son enum de sortie : le blob Wi-Fi IDF 5.5.3
écrit un seul octet dans le pointeur du canal secondaire. Une version provisoire
sans cette initialisation refusait à tort le démarrage ; elle n'a pas produit
une validation RTCM et ses traces sont conservées.

Le caster synthétique doit répondre **HTTP/1.1 200 OK** (ou ICY compatible).
Le client historique refuse HTTP/1.0 et possède un timeout de lecture de 2 s.
Le lecteur C3 doit donc être prêt AVANT que le caster accepte la connexion et
émette sa réponse : ne pas attendre le boot du rover après l'en-tête HTTP.
Les essais de mise au point interrompus pour ces deux raisons sont conservés,
mais ne sont pas des mesures de pertes radio.

Premiers lots réussis : 20 trames de 38 octets puis 100 trames de 1029 octets
(cinq fragments par grande trame), à 5 Hz. Captures USB identiques aux références,
ordre conservé, zéro trame RTCM manquante ou ajoutée, CRC24Q valides et aucun octet
incomplet restant. Cela ne mesure ni les retransmissions MAC ni les doublons de
fragments supprimés par le réassemblage. Les compteurs internes ne sont pas exportés sur USB ; ils ont été lus par JTAG
après les essais, hors transfert. Aucune capacité maximale n'est déduite du débit nominal.

Chaque boot C3 observé a émis **269 octets ROM** avant l'application. Ils sont
capturés séparément, sans modifier le bootloader d'origine ni les eFuses.
Le test ouvre le port une seule fois, conserve ces octets de boot, puis arme le
parseur strict avant toute donnée du caster. Le flux applicatif mesuré ne contient
pas de logs. Un consommateur ouvrant le port pendant un reset doit néanmoins
traiter cette phase : une absence totale de texte au boot n'est PAS qualifiée.

Les détails de chaque essai, captures, SHA256, defaults, commandes de restauration
et limites de durée/reconnexion sont enregistrés sous
`build/espnow01/hardware-s3-c3/`. Les résultats du test prolongé et les essais
USB ultérieurs doivent être lus dans le rapport de campagne final, pas déduits
implicitement des deux lots courts ci-dessus. Il ne s'agit pas d'une preuve RTK
ni d'une qualification de portée, de stabilité longue durée ou du C6.

L'essai prolongé a livré 600/600 grandes trames en environ 120 s d'émission,
soit 617400 octets identiques. Après débranchement/rebranchement physique du C3,
vérification de son image et réinitialisation contrôlée, un nouveau lot de 20/20
grandes trames a livré 20580 octets identiques. Les quatre lots nominaux totalisent
740 trames, sans manque/ajout ni CRC24Q invalide dans les captures.
La reprise est prouvée avec ce protocole contrôlé entre lots ; aucune reprise
sans perte au milieu d'un flux interrompu n'est promise.

Une rafale volontaire de 200 grandes trames a livré 71 trames intactes et en a
perdu 129. La comparaison complète échoue donc comme prévu : ce lot n'est pas
un PASS de livraison sans perte. Le relevé RAM BASE après rafale indique 129
abandons `s_dropped` (file pleine ou attente supérieure à 1 s), `s_invalid=0`
et `s_tx_halted=false`. Les compteurs ROVER `s_dropped`, `s_invalid` et
`s_usb_errors` valent zéro. Les valeurs sont des instantanés JTAG après le lot,
pas des compteurs cumulés sur toute la campagne. Le compteur de doublons/pertes
radio bruts et les retransmissions MAC ne sont pas mesurés.

Les six paramètres du client NTRIP de test ont été restaurés ; tous les champs
de configuration exposés par l'API correspondent au snapshot avant essai. Les
applications expérimentales canal 1 restent en flash. Les sauvegardes complètes
et les commandes de restauration sont conservées ; aucune restauration complète
n'a été exécutée. Les relevés JTAG sont intrusifs et ont été suivis de
redémarrages de nettoyage. Le verdict est GO pour poursuivre la validation à débit
nominal, avec qualification de déploiement encore ouverte (boot ROM, charge,
pertes/reprises en flux, portée, sécurité et longue durée).
