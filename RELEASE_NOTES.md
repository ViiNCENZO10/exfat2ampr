## exFAT2AMPR — convert your PS5 games to AMPR LZ4, right on the console

![Tested and working on PlayStation 5](https://raw.githubusercontent.com/exFAT2AMPR/exfat2ampr/main/docs/tested-on-ps5.svg)

Made by **V-10** · LZ4 format: **DLPSGAME** · AMPR emulator: drakmor/ampr_emu · free software (GPL-3.0)

Pick a game with the controller: the app compresses it on the PS5 and puts it where you want (internal SSD, M.2 SSD
or USB drive). Smaller games, every block checked, interrupted conversions resume, the original is kept.

### New in this version

- **Start-up check**: once a game is converted, the app checks what would stop it from starting (AMPR emulator
  missing or overridden, fakelib2, backport, fakelib disabled, eboot.bin not executable), fixes what it can and
  tells you the rest on the Progress screen. Repair (game menu, OPTIONS) runs the same check.
- **Time estimate per LZ4 level**: the check screen shows how long the conversion should take, and at level 10 to
  12 how long level 9 would take. The speed is learned on your console after each conversion. Level 10 to 12 kept
  from an older version is set back to 9 once (you can choose it again).
- **Start and close the app** (△ on the check screen): for an image, ShadowMountPlus only mounts it once no app
  is open; this starts the conversion and closes the app, which finds it again when reopened.
- **Every cover shows**: the console refused the app medium memory blocks and most covers stayed blank; the app
  now keeps its own memory, taken from the console's direct memory.
- **Each game's own music**: rest on a game and its home-screen music plays (the `snd0.at9` the game or the
  console keeps), cross-fading from one game to the next; silent for games without one. Settings › Game music.

### Earlier

- **Safer conversions** (1.95): an already converted game cannot be converted again; a source with links or unreadable files is refused; each uncompressed file gets a SHA-256 checked at the end; damaged resume data is refused. Details in `docs/HARDENING.md`.
- **Repair**, **Set aside**, original game protected, full destination shown, colour themes.

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

- **Test de démarrage** : une fois le jeu converti, l'app vérifie ce qui l'empêcherait de démarrer (émulateur AMPR
  absent ou remplacé, fakelib2, backport, fakelib désactivé, eboot.bin non exécutable), corrige ce qu'elle peut et
  vous dit le reste sur l'écran Progression. Réparer (menu du jeu, OPTIONS) fait le même test.
- **Durée estimée par niveau LZ4** : l'écran de vérification indique la durée prévue et, aux niveaux 10 à 12, celle
  du niveau 9. La vitesse est apprise sur votre console après chaque conversion. Un niveau 10 à 12 gardé d'une
  ancienne version est ramené une fois à 9 (vous pouvez le remettre).
- **Lancer et quitter l'app** (△ sur l'écran de vérification) : ShadowMountPlus ne monte une image que lorsqu'aucune
  app n'est ouverte ; la conversion démarre et l'app se ferme, puis la retrouve à sa réouverture.
- **Toutes les jaquettes s'affichent** : la console refusait à l'app des blocs mémoire moyens et la plupart des
  jaquettes restaient vides ; l'app gère maintenant sa propre mémoire, prise dans la mémoire directe de la console.
- **La musique de chaque jeu** : arrêtez-vous sur un jeu et sa musique d'accueil joue (le `snd0.at9` que le jeu
  ou la console garde), en fondu d'un jeu à l'autre ; silence pour les jeux qui n'en ont pas. Réglages › Musique
  des jeux.

**Avant** : conversions plus sûres (1.95), Réparer, Mettre de côté, jeu d'origine protégé, dossier de destination
affiché, thèmes de couleurs.

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
