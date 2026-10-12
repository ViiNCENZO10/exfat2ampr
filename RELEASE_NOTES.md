## exFAT2AMPR — convert PS5 game dumps to AMPR LZ4, right on the console

Made by **V-10** · LZ4 format: **DLPSGAME** · AMPR emulator: drakmor/ampr_emu · free software (GPL-3.0)

✅ **Tested and working on a PlayStation 5** by the author (firmware 12.40, kstuff, ShadowMountPlus 1.7).

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
  is kept as `.old`).
- Nine languages: English, French, Spanish, Italian, Russian, Portuguese, Chinese, Thai, Vietnamese; the
  web page for a PC or a phone (pairing with a 6-digit code or a QR code) adds Arabic.

### Install

Requirements: a jailbroken PS5 with kstuff, an ELF loader on port 9021 (elfldr, etaHEN…), ShadowMountPlus
1.7 or newer.

1. Copy `PPSA99272.ffpkg` into `homebrew/` at the root of an exFAT USB drive, or by FTP into `/data/homebrew/`.
   Keep a single copy of it.
2. Restart the console, start the jailbreak; ShadowMountPlus adds **exFAT2AMPR** to the home screen.
3. Open it: the app starts its engine itself. `exfat2ampr.elf` is only needed to run the engine by hand.

Tested and working on a PlayStation 5 by the author (firmware 12.40, kstuff, ShadowMountPlus 1.7). Other
firmwares and setups: reports are welcome.

---

### En français

exFAT2AMPR convertit les dumps de jeux PS5 au format AMPR LZ4, directement sur la console. Créé par **V-10**,
format LZ4 : **DLPSGAME**. ✅ **Testé et fonctionnel sur PlayStation 5** par l'auteur (firmware 12.40, kstuff,
ShadowMountPlus 1.7). Installation : copier `PPSA99272.ffpkg` dans `homebrew/` (clé USB exFAT) ou dans
`/data/homebrew/` par FTP, en un seul exemplaire, redémarrer, lancer le jailbreak puis l'app. Les mises à jour
suivantes s'installent depuis l'app (Réglages › Mises à jour). La vidéo de présentation (voix anglaise,
sous-titres français) est jointe à la release.
