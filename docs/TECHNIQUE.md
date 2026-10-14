# exFAT2AMPR : détails techniques

La présentation et l'installation sont dans le [README](../README.md). Cette page décrit le fonctionnement interne.

## Principe

```
/mnt/usb0/homebrew/PPSA01234      (dump dossier sur clé exFAT)
/mnt/shadowmnt/PPSA01234_xxxx     (image montée par ShadowMountPlus)
        │
        ▼  moteur exfat2ampr (sur la PS5)
        │   scan → profil → ampr_emu.index → LZ4 par blocs de 64 KiB, dédoublonnés → ampr_assets-NNN.pak
        │   + ampr_assets.index (+ .crc, .runtime) + fichiers laissés tels quels + fakelib/libSceAmpr.sprx
        │   → vérification de chaque bloc
        ▼
/data/homebrew/PPSA01234     ou   /mnt/ext0/homebrew/PPSA01234   ou   /mnt/usb0/homebrew/PPSA01234
```

Le format est celui de l'émulateur AMPR de drakmor (ampr_emu / Lazy_AMPR) : AMPRIDX3, AMPRPAK4, AMPRDAT3,
AMPRCRC1, AMPRCFG1.

## Le moteur sans l'app

1. Envoyer `exfat2ampr.elf` au chargeur d'ELF (port 9021). Une notification donne l'adresse de la page web,
   par exemple `http://192.168.1.20:6972/`.
2. Ouvrir cette adresse depuis un téléphone ou un PC du même réseau, s'appairer (code à 6 chiffres ou QR code).
3. Choisir la source (dossier, image montée ou chemin manuel ; une image non montée propose « Monter via
   ShadowMountPlus »), la destination (supports détectés, espace libre, supports non inscriptibles grisés), les
   options, puis **Convertir**. Plusieurs jeux s'enchaînent dans une file.

Mode automatique : `exfat2ampr.elf --auto` met en file tout jeu de `/mnt/usb*/homebrew/` qui n'est pas encore dans
`/data/homebrew`.

## Jeux trouvés

Dossiers `homebrew` de la console, des disques USB et du SSD M.2, dossiers `etaHEN/games`, racine des disques (et un
niveau de sous-dossier, par exemple `<disque>/<nom de la release>/<jeu>`), plus tout ce que gère ShadowMountPlus.
L'image d'un jeu vient du jeu lui-même, de l'image montée, de ShadowMountPlus ou de `/user/appmeta` ; à défaut, une
vignette est dessinée à partir du titre. Les jaquettes trouvées sont gardées dans `/data/exfat2ampr/covers`.

## Avant la conversion

- **Compatibilité AMPR** : `eboot.bin` et `sce_module/` doivent importer `libSceAmpr`. Absent : conversion
  refusée. Illisible (exécutable chiffré) : avertissement.
- **Espace** : taille finale estimée à partir des blocs du jeu (échantillonnés et compressés comme le packer le
  fait) ; la conversion refuse de démarrer si l'espace manque nettement.
- **Destination existante** : « Écraser » ne remplace qu'un jeu converti, une conversion inachevée ou un dossier
  vide, jamais un autre dossier (l'original, un autre dump).
- **Backports** : un `backports/<TITLE_ID>/fakelib/libSceAmpr.sprx` remplacerait l'émulateur ; il est signalé.

## Images gérées par ShadowMountPlus

ShadowMountPlus ne monte aucune image tant qu'un jeu ou une application est ouvert, exFAT2AMPR compris (réponse
409 `EBUSY`, raison `game_active`). Une conversion d'image attend donc, sans erreur (nouvel essai toutes les 5 à
20 s), et démarre dès que l'application est fermée (△ la ferme depuis l'onglet Progression). Avec
`persistent_image_mounts=1`, une image déjà montée est lue directement. Si le fakelib du jeu est « Désactivé »
dans ShadowMountPlus, il est réglé sur « Émulateurs » après la conversion : le plus petit mode qui charge encore
`libSceAmpr.sprx`.

## Pause et reprise

- Le packer écrit ses tampons (enregistrements, blocs, CRC, chaînes, table de dédoublonnage) en mode ajout dans
  `<destination>/.e2a-state/`, et un `state.bin` atomique toutes les dix secondes, à chaque fin de fichier et à la
  pause.
- Les volumes `.pak` sont retronqués à la position du dernier point de reprise ; les fichiers laissés tels quels
  sont copiés via un temporaire puis renommés ; une vérification interrompue reprend où elle s'était arrêtée.
- À la reprise, la source est rescannée et comparée (nombre, tailles, dates) ; si elle a changé, la reprise est
  refusée.
- Un disque plein met la conversion en pause au lieu d'échouer.

## Ce que fait le packer (port C de `ampr_pack.py`)

- Un seul groupe, layout `mixed`, blocs de 64 KiB, page d'E/S de 64 KiB, alignement de 64 o.
- Lecture sur quatre threads par lots de 24 Mo, compression LZ4 (fast ≤ 4, HC de 5 à 12) sur N threads.
- Un bloc reste RAW si le gain est inférieur à 64 o / 1 %, ou à 8 KiB / 12,5 % quand il n'économise aucune page
  d'E/S. `store` force RAW.
- Dédoublonnage : blocs identiques (SHA-256 tronqué + taille) partagés, drapeau `SHARED`.
- Fichiers incompressibles laissés tels quels dès 16 Mo ; gros jeux : blocs agrandis si nécessaire (jamais plus de
  16 millions de blocs) ; chaque volume de 16 Go relu et vérifié pendant que le suivant se remplit.
- Toujours laissés tels quels : `eboot.bin`, `.prx/.sprx/.elf/.self`, `sce_sys/`, `sce_module/`, `fakelib/`,
  `system/`, `mods/`, `save/`, `trophy2/`, `uds/`, `_DUPLEX_/`, vidéos, audio compressé,
  `.json/.xml/.utoc/.bin/.dat`, archives, `.pak`.
- Émulateur : la copie tenue à jour par ShadowMountPlus (`/data/shadowmount/emus/libSceAmpr.sprx`) est préférée si
  elle a un en-tête PRX valide ; sinon la 0.4.2.1 embarquée.
- Index : table de hachage FNV-1a avec la base de `build_ampr_index.py` ; la CI compare chaque index à celui de
  l'outil de référence. Au démarrage, le moteur répare les dossiers convertis par d'anciennes versions (index
  reconstruit depuis le manifeste, bit d'exécution des modules) ; OPTIONS › Réparer fait de même sur un jeu.

## Profils par jeu

`<TITLE_ID>.toml` déposés dans `USB:\homebrew\profiles\` (importés au démarrage) ou collés dans la page web. Un
sous-ensemble du format d'ampr_pack est compris : `[[rule]]` avec `action` (`compress`, `store`, `loose`),
`include`, `exclude`, et `[pack] compression_level`. La dernière règle gagne. Les profils de Lazy_AMPR
(`toml_profiles/`) se chargent tels quels.

## Optimisation par traces (« Optimiser ce jeu »)

Port C de `ampr_pack_profile.py` : au lieu de deviner, on mesure comment le jeu lit ses fichiers.

1. **Activer le mode trace** sur un jeu non converti : l'émulateur debug est placé dans `<jeu>/fakelib2`, que
   ShadowMountPlus monte à la place de `fakelib`. Un `fakelib2` qui ne vient pas d'exFAT2AMPR (backport) n'est
   jamais modifié.
2. **Jouer** : démarrage, chargement d'une sauvegarde, plusieurs zones, puis quitter normalement. Le jeu écrit
   `/app0/ampr_commands.bin` (journal `AMPRCMD1`).
3. **Collecter** : le journal est copié dans `/data/exfat2ampr/traces/<TITLE_ID>/<n>/` (à faire après chaque
   session : l'émulateur le vide à chaque lancement).
4. **Générer le profil** : taille de bloc (16 KiB à 1 MiB), layout (streaming, random, mixed), drapeau hot et
   réglages `[runtime]` (cache décodé obtenu par simulation LRU, cache physique, workers, réserve), écrits dans
   `/data/exfat2ampr/profiles/<TITLE_ID>.toml` pour la prochaine conversion. Il faut donc garder la source.
5. **Désactiver le mode trace** ensuite : l'émulateur debug est plus lent.

Les règles générées (`action = "keep"`) ne règlent que la géométrie des fichiers déjà packés.

## Plugins des jeux (pgo_stub, dlc_emu)

Le moteur embarque `libScePlayGo.sprx` ([pgo_stub](https://github.com/drakmor/pgo_stub)) et `libSceAppContent.sprx`,
`libSceGameUpdate.sprx`, `libSceNpEntitlementAccess.sprx` ([dlc_emu](https://github.com/drakmor/dlc_emu)), versions
épinglées et vérifiées par SHA-256 (`tools/plugins.sha256`).

- Chaque conversion reçoit ce dont le jeu a besoin : PlayGo quand le jeu l'importe, les modules DLC quand il a un
  `dlc_emu.ini`.
- Au démarrage puis toutes les 6 heures, chaque dossier de jeu est vérifié (sauf un jeu en cours) ; une ancienne
  version est remplacée (l'ancienne copie reste en `.old`), un fichier inconnu est laissé tel quel.
- Le dossier d'émulateurs de ShadowMountPlus (`/data/shadowmount/emus`) reçoit les mêmes fichiers.
- API : `GET /api/plugins[?path=]`, `POST /api/plugins/refresh [path=]`.

## Journaux et diagnostic

- Moteur : `/data/exfat2ampr/exfat2ampr.log` (repart de zéro au-delà de 500 Mo, l'ancien gardé en `.old`), servi à
  `/api/log?since=N`.
- App : Réglages › Journal ; envoyé au moteur au fil de l'eau (`/data/exfat2ampr/app.log`), servi à
  `/api/applog?since=N` (avec le jeton d'appairage depuis un PC).
- Réglages › Diagnostic écrit `exfat2ampr-diagnostic-<date>.txt` dans le dossier homebrew et sur la première clé
  USB : versions, état, stockages, sources, réglages, plugins, dernières lignes des deux journaux.

## Version et mises à jour

La version (`v1.N`) est affichée à côté du titre, dans Réglages et sur la page web ; la console l'indique comme
`01.000.N`. Si Réglages affiche un moteur d'une autre version (en orange), redémarrer la console puis relancer
l'app. Réglages › Mises à jour télécharge la nouvelle version (l'ancien package est gardé en `.old`) ; si le
téléchargement GitHub échoue, le package est récupéré en morceaux depuis la branche `updates`.

## Compilation et validation

À chaque push, GitHub Actions :

1. **validate** : compile une version Linux (`make host`), génère un jeu synthétique, le convertit avec une pause
   forcée, reprend, puis fait vérifier le résultat par les outils Python de référence de drakmor/ampr_emu
   (reconstruction octet par octet, index comparé à `build_ampr_index.py`).
2. **build** : compile `exfat2ampr.elf` avec le ps5-payload-sdk, l'app et son package ; un tag crée une release.

En local (Linux ou WSL) :

```bash
sudo apt-get install bash clang-18 lld-18 wget unzip xxd curl
wget https://github.com/ps5-payload-dev/sdk/releases/latest/download/ps5-payload-sdk.zip
sudo unzip -d /opt ps5-payload-sdk.zip
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
make deps && make
```

```bash
make host && python3 tests/make_game.py testgame && ./exfat2ampr-host testgame homebrew
```

## Limites connues

- Certains jeux ne démarrent pas une fois convertis bien qu'ils importent `libSceAmpr` (observé sur deux jeux
  Unity) : garder l'original jusqu'au premier lancement réussi.
- Le moteur ne monte pas lui-même les images : il s'appuie sur ShadowMountPlus.
- Le mode repos interrompt la conversion (elle reprend ensuite) ; le désactiver pour une conversion d'un trait.
- Si le format de l'émulateur évolue (AMPRPAK5…), le packer devra suivre.
