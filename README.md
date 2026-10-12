# ps5-exfat2ampr

![Testé et fonctionnel sur PlayStation 5](docs/tested-on-ps5.svg)

✅ **Testé et fonctionnel sur PlayStation 5** par l'auteur, V-10 (firmware 12.xx & 13.xx, kstuff, ShadowMountPlus 1.7).

Payload PS5 (jailbreak + chargeur d'ELF) qui convertit **sur la console** un jeu
en pack **AMPR LZ4** (format d'ampr_emu / Lazy_AMPR) et le dépose dans le dossier
`homebrew` du support de votre choix : SSD interne, SSD M.2 étendu ou USB. Aucun PC
dans la boucle : on envoie l'ELF, on ouvre la page web depuis un téléphone, on
choisit le jeu et la destination, on lance. La conversion peut être mise en pause,
survit à un arrêt de la console et reprend à l'octet près.

```
/mnt/usb0/homebrew/PPSA01234      (dump dossier sur clé exFAT)
/mnt/shadowmnt/PPSA01234_xxxx     (image .exfat montée par ShadowMountPlus)
        │
        ▼  exfat2ampr.elf (sur la PS5)
        │   scan → profil → ampr_emu.index → LZ4 par blocs 64 KiB, dédoublonnés → ampr_assets-NNN.pak
        │   + ampr_assets.index (+ .crc, .runtime) + fichiers loose + fakelib/libSceAmpr.sprx
        │   → vérification de chaque bloc
        ▼
/data/homebrew/PPSA01234     ou   /mnt/ext0/homebrew/PPSA01234   ou   /mnt/usb0/homebrew/PPSA01234
```

## Utilisation

1. Envoyer `exfat2ampr.elf` à la console (port 9021 : etaHEN, elfldr, websrv…).
   Une notification affiche l'adresse, par exemple `http://192.168.1.20:6972/`.
2. Ouvrir cette adresse depuis un téléphone ou un PC du même réseau. L'interface
   existe en français, anglais, espagnol, italien, russe et arabe.
3. **Source** : dump dossier sur clé USB (`homebrew/<jeu>`), image montée, ou chemin
   manuel. Une image `.exfat` non montée propose un bouton « Monter via
   ShadowMountPlus » (inscription dans `manual.lst`).
4. **Destination** : supports détectés automatiquement avec l'espace libre. Les
   supports non inscriptibles sont grisés ; la conversion refuse de démarrer si
   l'espace est manifestement insuffisant.
5. **Options** : niveau LZ4 (9 HC par défaut), threads, vérification (recommandée),
   dédoublonnage, nettoyage des restes de dump, remplacement, suppression de la source.
6. **Convertir** ajoute à la file d'attente ; plusieurs jeux s'enchaînent seuls.
7. **Progression** : barre, octets traités, débit, temps écoulé et restant, journal.
   **Pause** arrête proprement ; **Annuler** supprime le dossier en cours.
8. **Conversions interrompues** : tout dossier laissé en pause, ou interrompu par un
   arrêt, un mode repos ou un disque plein, apparaît ici avec **Reprendre** ou
   **Abandonner**.
9. **Profils par jeu** : `<TITLE_ID>.toml` déposés dans `USB:\homebrew\profiles\`
   (importés au démarrage) ou collés dans la page. Un sous-ensemble du format
   d'ampr_pack est compris : `[[rule]]` avec `action` (`compress`, `store`, `loose`),
   `include`, `exclude`, et `[pack] compression_level`. La dernière règle gagne. Les
   profils de Lazy_AMPR (`toml_profiles/`) se chargent tels quels.

Mode automatique : `exfat2ampr.elf --auto` met en file tout jeu trouvé dans
`/mnt/usb*/homebrew/` qui n'est pas encore dans `/data/homebrew`.

## Application sur l'écran d'accueil (PPSA99272)

La même chose à la manette, sans téléphone ni PC. Prérequis : kstuff à jour, un
chargeur d'ELF sur le port 9021 (elfldr, ou etaHEN : depuis la 1.7beta5,
ShadowMountPlus conseille de ne plus utiliser etaHEN) et ShadowMountPlus 1.7 ou plus
récent.

1. Copier `PPSA99272.ffpkg` dans `homebrew\` à la racine d'une clé USB exFAT, ou par
   FTP (serveur d'etaHEN, ou payload FTP sans etaHEN) dans `/data/homebrew/` (l'app
   reste alors disponible sans clé). En secours :
   le contenu de `PPSA99272-folder.zip` dans `/data/homebrew/PPSA99272/`.
2. ShadowMountPlus détecte l'image (une trentaine de secondes) et ajoute
   **exFAT2AMPR** à l'écran d'accueil.
3. Au lancement, l'app envoie elle-même son moteur au chargeur d'ELF (port 9021) :
   plus besoin d'envoyer `exfat2ampr.elf` à la main. Si un ancien moteur a été lancé
   à la main depuis le dernier démarrage, redémarrer la console d'abord.

Commandes : ↑↓ choisir, □ cocher plusieurs jeux, ✕ continuer (options →
destination → vérification → lancement), L1/R1 onglets (Progression, Interrompues,
Réglages), △ pause/reprise, ◯ retour. La conversion continue app fermée ou en jeu.
Une conversion en erreur n'est jamais bloquée : ✕ la relance (ou la reprend depuis
son point de reprise), □ l'efface. Chaque erreur est expliquée dans la langue choisie,
avec ce qu'il faut faire.

Version : affichée à côté du titre (par exemple `v1.137`), sur l'écran de démarrage, dans
Réglages et sur la page web. Le nombre augmente à chaque nouvelle version ; c'est aussi la
version que la console indique pour l'app (`01.000.137`). Si Réglages affiche un moteur
d'une autre version (en orange), redémarrer la console puis relancer l'app.

Jeux trouvés : dossiers `homebrew` de la console, des disques USB et du SSD M.2, dossiers
`etaHEN/games`, racine des disques (et un niveau de sous-dossier, par exemple
`<disque>/<nom de la release>/<jeu>`), plus tout ce que gère ShadowMountPlus. Chaque jeu indique
où il se trouve : stockage interne, USB n ou SSD M.2. Son image vient du jeu lui-même, de l'image
montée, de ShadowMountPlus ou de la copie que la console garde de chaque titre installé
(`/user/appmeta`) ; à défaut, une vignette est dessinée à partir du titre.

Langues de l'app : français, anglais, espagnol, italien, russe, portugais (Brésil),
chinois simplifié, thaï et vietnamien. Celle de la console est choisie au premier
lancement, puis Réglages → Langue. Les titres de jeux en chinois, japonais ou coréen
s'affichent aussi (caractères courants : GB2312, Big5 niveau 1, kana, kanji niveau 1,
hangul KS X 1001).

Images `.exfat` / `.ffpkg` gérées par ShadowMountPlus : SMP ne monte aucune image
tant qu'un jeu ou une application est ouvert, exFAT2AMPR compris (réponse 409
`EBUSY`, raison `game_active`). Une conversion d'image lancée depuis l'app attend
donc, sans erreur (« En attente de ShadowMountPlus », nouvel essai toutes les 5 à
20 s), et démarre seule dès que l'application est fermée : △ la ferme depuis
l'onglet Progression. Avec `persistent_image_mounts=1`, une image déjà montée est lue
directement, sans rien demander à SMP. Si le fakelib du jeu est « Désactivé » dans
SMP, il est réglé sur « Émulateurs » après la conversion : c'est le plus petit mode
qui charge encore `libSceAmpr.sprx` (sinon le jeu converti ne démarrerait pas).

Finitions : l'image de fond du jeu sélectionné (`sce_sys/pic0.png`, sinon sa jaquette
floutée) derrière des panneaux de verre fumé, animations (sélection qui glisse,
écrans qui coulissent, jaquettes en fondu, barre de progression vivante) et sons de
navigation synthétisés. **Réglages → Sons de navigation / Animations** les coupent
(les animations coupées soulagent le processeur si besoin).

Mises à jour : fermer l'app et remplacer le fichier `.ffpkg` (mise à jour
automatique prévue).

## Pause et reprise

- Le packer écrit ses tampons (enregistrements, blocs, CRC, chaînes, table de
  dédoublonnage) en mode ajout dans `<destination>/.e2a-state/` et un `state.bin`
  atomique toutes les dix secondes, à chaque fin de fichier et à la pause.
- Les volumes `.pak` sont retronqués à la position du dernier point de reprise ;
  les fichiers loose sont copiés via un temporaire puis renommés ; la phase de
  vérification redémarre du début si elle est interrompue.
- À la reprise, la source est rescannée et comparée (nombre, tailles, dates) ;
  si elle a changé, la reprise est refusée.
- Un disque plein met la conversion en pause au lieu d'échouer : libérez de la
  place, puis reprenez.

## Compilation et validation

GitHub Actions fait deux choses à chaque push :

1. **validate** : compile une version Linux (`make host`), génère un jeu
   synthétique, lance une conversion avec pause forcée à 48 Mio, la reprend, puis
   fait vérifier le résultat par les outils Python de référence de drakmor/ampr_emu
   (`ampr_pack.py verify --root`, reconstruction byte à byte).
2. **build** : compile `exfat2ampr.elf` avec le ps5-payload-sdk et publie
   l'artefact ; un tag `vX.Y.Z` crée une release.

En local (Linux ou WSL) :

```bash
sudo apt-get install bash clang-18 lld-18 wget unzip xxd curl
wget https://github.com/ps5-payload-dev/sdk/releases/latest/download/ps5-payload-sdk.zip
sudo unzip -d /opt ps5-payload-sdk.zip
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
make deps && make
PS5_HOST=192.168.1.20 make test
```

```bash
make host && python3 tests/make_game.py testgame && ./exfat2ampr-host testgame homebrew
```

## Ce que fait le packer (port C de `ampr_pack.py`)

- Un seul groupe, layout `mixed`, blocs 64 KiB, page d'E/S 64 KiB, alignement 64 o.
- Lecture en avance sur un thread, compression LZ4 (fast ≤ 4, HC 5 à 12) sur N threads.
- Un bloc reste RAW si le gain est inférieur à 64 o / 1 %, ou à 8 KiB / 12,5 % quand il
  n'économise aucune page d'E/S. `store` force RAW (fichiers déjà compressés mais à
  garder seekables).
- Dédoublonnage : blocs identiques (SHA-256 tronqué + taille) partagés, flag `SHARED`.
- Auto-loose : fichiers ≥ 64 Mio dont les premiers blocs gagnent moins de 5 %.
- Toujours loose : `eboot.bin`, `.prx/.sprx/.elf/.self`, `sce_sys/`, `sce_module/`,
  `fakelib/`, `system/`, `mods/`, `save/`, `trophy2/`, `uds/`, `_DUPLEX_/`, vidéos,
  audio compressé, `.json/.xml/.utoc/.bin/.dat`, archives.
- Formats écrits : AMPRIDX3, AMPRPAK4, AMPRDAT3, AMPRCRC1, AMPRCFG1.
- Émulateur : la copie tenue à jour par ShadowMountPlus
  (`/data/shadowmount/emus/libSceAmpr.sprx`) est préférée si elle a un en-tête PRX
  valide ; sinon la 0.4.2.1 embarquée. La page affiche laquelle est utilisée et son
  SHA-256.

## Optimisation par traces (section 7 de la page)

Port C de `ampr_pack_profile.py` (drakmor/ampr_emu) : au lieu de deviner, on mesure comment le jeu lit ses fichiers.

1. **Activer le mode trace** sur un jeu de `homebrew` (converti ou non) : l'émulateur debug
   `libSceAmpr.sprx-0.4.2.1-test-debug-pack` est placé dans `<jeu>/fakelib2`. ShadowMountPlus monte `fakelib2`
   à la place de `fakelib` et ne le remplace jamais lors de la mise à jour des émulateurs. Les autres bibliothèques
   de `fakelib` y sont recopiées. Un `fakelib2` qui n'a pas été créé par exfat2ampr (backport) n'est jamais modifié.
2. **Jouer** : démarrage, chargement d'une sauvegarde, plusieurs zones, puis quitter le jeu normalement.
   Le jeu écrit `/app0/ampr_commands.bin` (journal `AMPRCMD1`).
3. **Collecter** : le journal et son `ampr_emu.index` sont copiés dans `/data/exfat2ampr/traces/<TITLE_ID>/<n>/`.
   L'émulateur vide le journal à chaque lancement : collecter après chaque session.
4. **Générer le profil** : toutes les traces sont fusionnées. Pour chaque fichier lu, on choisit :
   - la taille de bloc (16 KiB à 1 MiB) ;
   - le layout (streaming, random ou mixed) ;
   - le drapeau hot ;
   - et la section `[runtime]` : cache décodé obtenu par simulation LRU, cache physique, workers et réserve.

   Le profil est écrit dans `/data/exfat2ampr/profiles/<TITLE_ID>.toml` et s'applique à la prochaine conversion de ce
   jeu, il faut donc **garder la source**.
5. **Désactiver le mode trace** ensuite : l'émulateur debug est plus lent.

Les règles générées utilisent `action = "keep"`. Elles ne règlent que la géométrie des fichiers déjà packés et ne
font jamais packer un fichier que la liste de sécurité garde en clair. La CI compare le résultat à
`ampr_pack_profile.py` sur un journal synthétique validé par `parse_ampr_command_log.py`.

## Plugins des jeux (pgo_stub, dlc_emu)

Le moteur embarque les modules de remplacement de drakmor : `libScePlayGo.sprx`
([pgo_stub](https://github.com/drakmor/pgo_stub) : PlayGo voit tous les morceaux installés) et
`libSceAppContent.sprx`, `libSceGameUpdate.sprx`, `libSceNpEntitlementAccess.sprx`
([dlc_emu](https://github.com/drakmor/dlc_emu) : les DLC listés dans le `dlc_emu.ini` du jeu). Versions
épinglées par le Makefile et vérifiées par SHA-256 (`tools/plugins.sha256`) ; `src/plugins.c` connaît
l'empreinte de chaque version publiée, donc sait dire si un fichier d'un jeu est à jour, ancien (remplacé,
l'ancienne copie reste en `.old`) ou inconnu (laissé tel quel).

- À chaque conversion, la destination reçoit ce dont le jeu a besoin : PlayGo quand le jeu l'importe, les
  modules DLC quand le jeu a un `dlc_emu.ini` (sans ce fichier ils cacheraient les vrais DLC : jamais ajoutés
  seuls).
- Au démarrage puis toutes les 6 heures (et quand un support est branché), chaque dossier de jeu des racines
  connues est vérifié, sauf un jeu en cours d'exécution.
- Le dossier d'émulateurs de ShadowMountPlus (`/data/shadowmount/emus`, `update_emulators=1`) reçoit les mêmes
  fichiers : les jeux montés depuis une image suivent aussi.
- App : menu du jeu › « Plugins : … » (un jeu), Réglages › Plugins des jeux (tous) ; page web : état par jeu et
  bouton « Mettre à jour les plugins » ; API : `GET /api/plugins[?path=]`, `POST /api/plugins/refresh [path=]`.

Nouveau, pas encore validé sur console.

## Limites

- Non testé sur console par l'auteur de ce dépôt : la CI valide le format avec les
  outils de référence, pas l'exécution sur PS5. Première exécution à faire sur un
  petit jeu, journal dans `/data/exfat2ampr/exfat2ampr.log`.
- Le payload ne monte pas lui-même les images : il s'appuie sur ShadowMountPlus.
- Le mode repos interrompt la conversion ; elle reprend ensuite, mais désactivez-le
  pour une conversion d'un seul trait.
- L'émulateur AMPR est en bêta ; si son format évolue (AMPRPAK5…), le packer devra
  être mis à jour.

## Crédits

- **V-10** : auteur d'exFAT2AMPR (moteur, app PS5, page web).
- **DLPSGAME** : le format LZ4 des jeux PS5.
- drakmor : l'émulateur AMPR (ampr_emu) et ShadowMountPlus.
- Yann Collet : l'algorithme de compression LZ4.

## Licence

exFAT2AMPR - Copyright (C) 2026 V-10. Logiciel libre sous GNU GPL version 3 ou ultérieure (texte complet :
`COPYING`), avec des conditions supplémentaires de l'article 7 (fichier `NOTICE`) :
- toute copie ou version modifiée garde ce copyright et le crédit « Créé par V-10 » ;
- une version modifiée se présente comme modifiée et ne se fait pas passer pour l'originale ;
- le nom « exFAT2AMPR » et son logo ne sont pas cédés : une version modifiée diffusée porte un autre nom et une
  autre icône.

Retirer le nom de l'auteur ou diffuser le programme sous un autre nom d'auteur met fin aux droits accordés par
la licence (article 8).

GPL-3.0-or-later. Formats et heuristiques d'après drakmor/ampr_emu (GPL-3.0),
LZ4 (BSD-2), ps5-payload-sdk (GPL-3.0). Polices de l'app : DejaVu Sans (licence
Bitstream Vera / DejaVu), Noto Sans Thai et Noto Sans CJK (SIL Open Font License 1.1).
