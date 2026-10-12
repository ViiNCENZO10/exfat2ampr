## exFAT2AMPR — convert PS5 game dumps to AMPR LZ4, right on the console

![Tested and working on PlayStation 5](https://raw.githubusercontent.com/exFAT2AMPR/exfat2ampr/main/docs/tested-on-ps5.svg)

Made by **V-10** · LZ4 format: **DLPSGAME** · AMPR emulator: drakmor/ampr_emu · free software (GPL-3.0)

✅ **Tested and working on a PlayStation 5** by the author (firmware 12.xx & 13.xx, kstuff, ShadowMountPlus 1.7).

### What it does

- Finds every game and app on the console — internal storage, USB drives, M.2 SSD, `homebrew` and
  `etaHEN/games` folders, the drives' top level, and everything ShadowMountPlus manages — and shows where
  each one is kept, with its cover.
- Converts exFAT dumps (folders or images) to AMPR LZ4 packs on the PS5 itself: checks first (free space,
  AMPR compatibility, copies already converted), verifies every block, keeps the originals.
- Runs in the background, slows down while you play, resumes after an interruption.
- **Optimize this game**: measures how a game reads its files while you play and builds a profile for its
  next conversion (faster loading, fewer stutters).
- **Speed test** of each storage, the fastest one recommended, slow ones flagged.
- **Automatic updates**: the app tells you when a new version is out and installs it (the previous package
  is kept as `.old`). The console's HTTP library does not manage the redirect of a GitHub release download:
  the app now follows it by hand and, if that still fails, fetches the package in pieces from a plain address
  (the `updates` branch). Being validated on a console with this version.
- **Game plugins kept up to date**: drakmor's PlayGo stub (pgo_stub) and DLC modules (dlc_emu) are embedded.
  Every conversion gets what the game needs, every game folder is checked at start-up and from the app (the
  game's menu, or Settings › Game plugins for all of them), older releases are replaced (the old copy stays as
  `.old`), and ShadowMountPlus' emulator folder receives the same files, so the games it mounts follow too.
- **Faster conversions, packs within a fraction of a percent of the previous ones**: a fast LZ4 probe skips
  the slow pass on incompressible data, a faster CRC, the verification on several threads; the emulator's memory budget is checked before
  the cache settings are written (a large cache no longer ends up smaller than the standard one on a big
  game).
- **Advice on every option**: "Recommended" or "Less recommended" next to each setting (in the app, with the
  reason in one line); the defaults are the recommended values.
- **Big games (250 GB and more)**: the number of blocks is estimated before packing and the biggest files
  get bigger blocks when needed, so the emulator's manifest and caches stay within its memory (never more
  than 16 million blocks); incompressible files stay loose from 16 MB; the deduplication table is bounded
  (the pack stays valid, memory stays flat); every 16 GB volume is read back and checked while the next one
  fills, so a bad disk is caught within minutes and the final check only covers the rest; an interrupted
  verification resumes where it stopped; a passing write error on a USB drive is retried before pausing.
- **Faster on compressed images (new in this version, not yet measured on a console)**: a game mounted from a
  `.ffpfsc` image is decoded by the console on the thread that reads it, and one reader waited on it between two
  blocks; the packer now reads every batch with four threads, in 24 MB batches instead of 6, and writes each
  batch in one piece instead of one write per 64 KB block. Loose files (the incompressible ones) are copied the
  same way. The engine's log shows what each stage sustains (`pack stages: read … compress … write …`) every two
  minutes, so a slow source is told apart from a slow destination.
- **The app's own log, readable from anywhere**: what the app did (screens, engine answers, every step of an
  update download with the HTTP library's codes, errors shown on screen) is kept in Settings › Log, sent to the
  engine as it happens (it stays there after the app closes, and in `/data/exfat2ampr/app.log`), shown on the
  web page under the engine's log, and served to tools at `/api/applog?since=N` (the engine's log at
  `/api/log?since=N`), so a failure can be diagnosed from a PC without the TV.
- Nine languages: English, French, Spanish, Italian, Russian, Portuguese, Chinese, Thai, Vietnamese; the
  web page for a PC or a phone (pairing with a 6-digit code or a QR code) adds Arabic.

### Install

Requirements: a jailbroken PS5 with kstuff, an ELF loader on port 9021 (elfldr, etaHEN…), ShadowMountPlus
1.7 or newer.

1. Copy `PPSA99272.ffpkg` into `homebrew/` at the root of an exFAT USB drive, or by FTP into `/data/homebrew/`.
   Keep a single copy of it.
2. Restart the console, start the jailbreak; ShadowMountPlus adds **exFAT2AMPR** to the home screen.
3. Open it: the app starts its engine itself. `exfat2ampr.elf` is only needed to run the engine by hand.

Tested and working on a PlayStation 5 by the author (firmware 12.xx & 13.xx, kstuff, ShadowMountPlus 1.7).
Other firmwares and setups: reports are welcome. New in this version and not yet validated on a console: the
game plugin updates.

**Presentation video**: `exfat2ampr-presentation.mp4` — English voice, French subtitles.

---

### En français

exFAT2AMPR convertit les dumps de jeux PS5 au format AMPR LZ4, directement sur la console. Créé par **V-10**,
format LZ4 : **DLPSGAME**. ✅ **Testé et fonctionnel sur PlayStation 5** par l'auteur (firmware 12.xx & 13.xx,
kstuff, ShadowMountPlus 1.7). Installation : copier `PPSA99272.ffpkg` dans `homebrew/` (clé USB exFAT) ou dans
`/data/homebrew/` par FTP, en un seul exemplaire, redémarrer, lancer le jailbreak puis l'app. Les mises à jour
suivantes s'installent depuis l'app (Réglages › Mises à jour) : la redirection du téléchargement GitHub est
maintenant suivie par l'app elle-même, avec en secours le package en morceaux depuis une adresse directe
(en cours de validation sur console avec cette version). Les plugins pgo_stub (PlayGo) et dlc_emu (DLC)
de drakmor sont intégrés et maintenus à jour dans le dossier `fakelib` des jeux : à chaque conversion, au
démarrage, depuis le menu du jeu ou Réglages › Plugins des jeux (nouveau, pas encore validé sur console).
Conversions plus rapides à packs identiques (sonde LZ4 rapide, CRC plus rapide, vérification sur plusieurs
cœurs, budget mémoire de l'émulateur respecté), et chaque option indique « Recommandé » ou « Moins
recommandé » avec la raison. Gros jeux (250 Go et plus) : nombre de blocs estimé avant la conversion et
blocs agrandis sur les plus gros fichiers si nécessaire (jamais plus de 16 millions de blocs), fichiers
incompressibles laissés tels quels dès 16 Mo, table de déduplication bornée, chaque volume de 16 Go relu et
vérifié pendant que le suivant se remplit, vérification reprenable, erreurs d'écriture passagères retentées.
Plus rapide sur les images compressées (`.ffpfsc`, nouveau, pas encore mesuré sur console) : lecture par quatre
threads, lots de 24 Mo, une seule écriture par lot, copie des fichiers non compressés de la même façon ; le journal
du moteur indique toutes les deux minutes le débit de chaque étape (lecture, compression, écriture). Journal de
l'app (Réglages › Journal) : écrans visités, réponses du moteur, chaque étape d'un téléchargement de mise à jour
avec les codes de la bibliothèque HTTP, erreurs affichées ; il part au fur et à mesure vers le moteur (conservé
après la fermeture de l'app, dans `/data/exfat2ampr/app.log`), s'affiche sur la page web et se lit à distance
via `/api/applog?since=N`. La vidéo de présentation
(`exfat2ampr-presentation.mp4`, voix anglaise, sous-titres français) est jointe à la release.
