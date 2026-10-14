# Tester exfat2ampr sur votre PS5

Prérequis : une PS5 jailbreakée avec un chargeur d'ELF à l'écoute sur le port 9021
(etaHEN, ps5-payload-elfldr, websrv…), ShadowMountPlus installé, et un PC ou un
téléphone sur le même réseau. Un compte GitHub suffit pour compiler : aucun
outil à installer sur le PC.

## 1. Obtenir l'ELF (compilation par GitHub Actions)

1. Créez un dépôt GitHub vide (privé ou public), par exemple `ps5-exfat2ampr`.
2. Poussez le contenu du dossier `ps5-exfat2ampr` dedans :

```bash
cd ps5-exfat2ampr
git init
git add .
git commit -m "exfat2ampr"
git branch -M main
git remote add origin https://github.com/<vous>/ps5-exfat2ampr.git
git push -u origin main
```

3. Ouvrez l'onglet **Actions** du dépôt. Deux jobs se lancent :
   - **validate** : compile la version Linux, convertit un jeu synthétique avec une
     pause forcée, reprend, puis fait vérifier le pack par les outils Python de
     référence d'ampr_emu. S'il est vert, le format produit est correct.
   - **build** : compile `exfat2ampr.elf` avec le ps5-payload-sdk.
4. Quand **build** est vert, cliquez dessus, puis sur l'artefact
   `exfat2ampr_<version>.zip` en bas de la page, et extrayez `exfat2ampr.elf`.

Si un job est rouge, copiez les lignes d'erreur du journal (la première ligne
`error:` suffit souvent) et envoyez-les-moi : je corrige, vous repoussez.

## 2. Envoyer l'ELF à la console

Au choix :

- **ps5-payload-sdk / prospero-deploy** (Linux ou WSL) :

```bash
PS5_HOST=192.168.1.20 make test
```

- **netcat / socat** depuis n'importe quel PC :

```bash
nc -q0 192.168.1.20 9021 < exfat2ampr.elf
```

- **etaHEN** : copiez `exfat2ampr.elf` sur une clé USB et chargez-le depuis le
  menu des payloads, ou via son chargeur réseau (port 9021).
- **Windows sans outils** : PowerShell

```powershell
$c = New-Object Net.Sockets.TcpClient("192.168.1.20", 9021); $s = $c.GetStream(); $b = [IO.File]::ReadAllBytes("exfat2ampr.elf"); $s.Write($b, 0, $b.Length); $s.Close(); $c.Close()
```

Une notification apparaît sur la console avec l'adresse de la page, par exemple
`http://192.168.1.20:6972/`. Si rien n'apparaît après dix secondes, le journal est
dans `/data/exfat2ampr/exfat2ampr.log` (lisible par FTP).

## 3. Premier test, avec un petit jeu

1. Copiez un **petit** jeu (moins de 5 Go) en dossier sur une clé USB exFAT, dans
   `homebrew\<TITLE_ID>\` (le dossier doit contenir `eboot.bin` et `sce_sys`).
   Gardez-en une copie : la source n'est jamais modifiée, mais on ne sait jamais.
2. Ouvrez `http://<ip>:6972/` sur votre téléphone.
3. Source : le jeu de la clé. Destination : stockage interne (`/data/homebrew`).
   Laissez les options par défaut (LZ4 9, 6 threads, vérification et dédoublonnage
   activés).
4. **Convertir**. Suivez la barre, le débit et le temps restant.
5. Pendant la conversion, testez **Pause** : la conversion s'arrête en quelques
   secondes et apparaît dans « Conversions interrompues ». **Reprendre** : elle
   repart au même pourcentage.
6. À la fin, la notification indique les tailles avant et après. Dans
   ShadowMountPlus, le jeu doit apparaître dans `/data/homebrew/<TITLE_ID>` et se
   lancer. Jouez quelques minutes : chargement d'un niveau, sauvegarde, reprise.

## 4. Ce qu'il faut me rapporter

- Le contenu de `/data/exfat2ampr/exfat2ampr.log` (FTP) après le test.
- Le fichier `/data/homebrew/<TITLE_ID>.exfat2ampr.json` (tailles, blocs, volumes).
- Si le jeu ne se lance pas : le journal de ShadowMountPlus
  (`/data/shadowmount/debug.log`) et, si présent, `/app0/apr_emu.log` dans le dossier
  du jeu.
- Si la page ne répond pas : la notification affichée et ce que donne `ping`.

## 5. Ensuite

- Un gros jeu, avec le mode repos désactivé.
- Un jeu depuis une image `.exfat` : laissez ShadowMountPlus la monter (ou bouton
  « Monter via ShadowMountPlus »), puis choisissez l'entrée `/mnt/shadowmnt/…`.
- Un profil de Lazy_AMPR (`toml_profiles/<jeu>.toml`) copié dans
  `USB:\homebrew\profiles\<TITLE_ID>.toml`, puis « Importer depuis USB ».
- Destination SSD M.2 ou clé USB, pour vérifier la détection des supports.


## Local hardening regression suite (modified version)

Run `make check` on Linux with GCC, make, Python 3.9+ and the LZ4 sources (downloaded by make).
This builds an inert host fixture, not a console payload, and never deploys anything.
Tests cover strict JSON, existing unit/manifest/update integrity cases, changing compression worker
counts, publication interruption at four stages, pause/resume, reconversion refusal, loose SHA-256
corruption and damaged checkpoint/manifest rejection. Temporary fixtures and logs stay in `build/`.
A successful host run is not a PS5/exFAT hardware or power-loss qualification.
