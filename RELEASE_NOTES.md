## exFAT2AMPR — convert your PS5 games to AMPR LZ4, right on the console

![Tested and working on PlayStation 5](https://raw.githubusercontent.com/exFAT2AMPR/exfat2ampr/main/docs/tested-on-ps5.svg)

Made by **V-10** · LZ4 format: **DLPSGAME** · AMPR emulator: drakmor/ampr_emu · free software (GPL-3.0)

Pick a game with the controller: the app compresses it on the PS5 and puts it where you want (internal SSD, M.2 SSD
or USB drive). Smaller games, every block checked, interrupted conversions resume, the original is kept.

### New in this version

- **Safer conversions**: an already converted game cannot be converted again; a source with links or unreadable files is refused instead of giving an incomplete game; each file left uncompressed gets a SHA-256 checked at the end; a conversion interrupted while it was being finalised resumes without starting over; damaged resume data is refused. Details in `docs/HARDENING.md`.
- **Repair** (game menu, OPTIONS): fixes a game converted by an older version, in one press.
- **Set aside** (game menu): hides a duplicate image from ShadowMountPlus (renamed `.off`; rename it back to undo).
- **The original game is protected**: "Overwrite" never erases a folder that is not a converted game, and deleting
  a copy tells you first if it is the only one on the console.
- **The destination folder is shown in full** before you start.
- **Colour themes** (Settings › Theme), including a red-to-yellow gradient.

### Install

Requirements: jailbroken PS5 with kstuff, an ELF loader on port 9021 (elfldr, etaHEN…), ShadowMountPlus 1.7+.

1. Copy `PPSA99272.ffpkg` into `homebrew/` on an exFAT USB drive, or by FTP into `/data/homebrew/`. Keep one copy.
2. Restart the console and run the jailbreak: **exFAT2AMPR** appears on the home screen.
3. Open it. Next updates install from the app: Settings › Updates.

**Good to know**: keep the original until the converted game has started once; a few games (seen with Unity
games) do not run once converted. Problem? Settings › Diagnostic writes a file to attach to your report.

Tested and working by the author on firmware 12.xx and 13.xx. Presentation video attached
(`exfat2ampr-presentation.mp4`).

---

### En français

Choisissez un jeu à la manette : l'app le compresse sur la PS5 et le range où vous voulez (SSD interne, SSD M.2 ou
disque USB). Jeux plus légers, chaque bloc vérifié, conversions interrompues reprises, jeu d'origine gardé.

**Nouveautés**

- **Conversions plus sûres** : un jeu déjà converti ne peut plus être reconverti ; une source avec des liens ou des fichiers illisibles est refusée au lieu de donner un jeu incomplet ; chaque fichier non compressé reçoit une empreinte SHA-256 vérifiée à la fin ; une conversion interrompue pendant sa finalisation reprend sans tout refaire ; des données de reprise abîmées sont refusées.
- **Réparer** (menu du jeu, OPTIONS) : corrige en un clic un jeu converti par une ancienne version.
- **Mettre de côté** (menu du jeu) : cache une image en double à ShadowMountPlus (renommée `.off` ; renommez-la pour
  annuler).
- **Le jeu d'origine est protégé** : « Écraser » ne supprime jamais un dossier qui n'est pas un jeu converti, et la
  suppression d'une copie prévient si c'est la seule sur la console.
- **Le dossier de destination est affiché en entier** avant le lancement.
- **Thèmes de couleurs** (Réglages › Thème), dont un dégradé rouge et jaune.

**Installation** : PS5 jailbreakée avec kstuff, chargeur d'ELF sur le port 9021, ShadowMountPlus 1.7 ou plus.

1. Copiez `PPSA99272.ffpkg` dans `homebrew/` sur une clé USB exFAT, ou par FTP dans `/data/homebrew/`. Une seule
   copie.
2. Redémarrez la console et lancez le jailbreak : **exFAT2AMPR** apparaît sur l'écran d'accueil.
3. Ouvrez-la. Les mises à jour suivantes s'installent depuis l'app : Réglages › Mises à jour.

**Bon à savoir** : gardez le jeu d'origine tant que la version convertie n'a pas démarré une fois ; quelques jeux
(vu avec des jeux Unity) ne fonctionnent pas une fois convertis. Un problème ? Réglages › Diagnostic crée un fichier
à joindre à votre message.

Testé et fonctionnel par l'auteur sur firmware 12.xx et 13.xx. Vidéo de présentation jointe
(`exfat2ampr-presentation.mp4`, voix anglaise, sous-titres français).
