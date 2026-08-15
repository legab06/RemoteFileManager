<p align="center">
  <img src="assets/branding/png/logo-512.png"
       alt="RemoteFileManager"
       width="220">
</p>

# RemoteFileManager

RemoteFileManager est un gestionnaire de fichiers graphique natif pour administrer les fichiers d’un serveur distant via SSH/SFTP. L’objectif est de retrouver une ergonomie proche de Dolphin sans monter le serveur avec SSHFS et sans installer de logiciel supplémentaire côté serveur.

> État actuel : **Sprint 8 — gestion des volumes locaux et distants,
> version 0.8.1**. Cette release corrective fiabilise la classification des disques
> USB Btrfs, les déplacements distants entre filesystems et le filtrage des montages
> techniques.

## Principes

- client C++20 léger avec interface Qt Widgets ;
- prototype Linux en premier, architecture compatible Windows et macOS ;
- serveur SSH/SFTP standard, sans agent propriétaire ;
- opérations distantes exécutées côté serveur quand c’est pertinent ;
- sécurité explicite : vérification de la clé d’hôte et aucun secret en clair ;
- interface inspirée d’un navigateur de fichiers natif ;
- navigation locale multiplateforme fondée sur Qt, sans commande système ni SSHFS ;
- panneaux à source explicite, permettant notamment un affichage scindé local/SSH.

## Fonctionnalités disponibles

- navigation dans les répertoires locaux et dans les serveurs distants par SSH/SFTP ;
- un ou deux panneaux optionnels, chacun pouvant afficher une source locale ou la
  session SSH active ;
- arbre Places regroupant emplacements locaux, profils serveur enregistrés, connexion
  active, volumes et périphériques amovibles ;
- enregistrement de profils de connexion sans mot de passe persistant ;
- création de dossiers, renommage, déplacement, copie et suppression sur le serveur,
  avec copies entre chemins distants exécutées côté serveur ;
- envoi et téléchargement de fichiers ou dossiers entre le client et le serveur, avec
  progression, pause, reprise et annulation ;
- sous Linux, détection et actualisation explicite des volumes locaux et des volumes du
  serveur connecté, y compris les périphériques non montés lorsque `lsblk` est
  disponible ;
- montage et démontage des volumes locaux et distants Linux, avec protection de la
  racine et des volumes système et ciblage du seul point de montage sélectionné pour
  les périphériques attachés plusieurs fois ;
- utilisation de UDisks et de son autorisation Polkit lorsque nécessaire, sans
  `sudo`, `su` ou `pkexec` intégré ; le mot de passe d’autorisation distante est
  transféré sans copie partagée, effacé après usage et n’est jamais enregistré.

Une seule session SSH peut être active à la fois. Les mutations de fichiers locales,
les copies directes entre panneaux local/SSH, les sessions SSH simultanées et les
serveurs distants non Linux pour la gestion des volumes ne sont pas encore pris en
charge.

## Dépendances

- CMake 3.25 ou supérieur ;
- Ninja ;
- compilateur compatible C++20 ;
- Qt 6.4 ou supérieur (`Core`, `Widgets`, `Test`) ;
- libssh 0.10 ou supérieur.

### Arch Linux

```bash
sudo pacman -S --needed base-devel cmake ninja qt6-base libssh
```

### Ubuntu / Debian

```bash
sudo apt install build-essential cmake ninja-build qt6-base-dev libssh-dev
```

## Compiler et tester

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Puis lancer :

```bash
./build/debug/src/remote-file-manager
```

Le preset de production se construit avec :

```bash
cmake --preset release
cmake --build --preset release
```

## Organisation

```text
include/remotefilemanager/   API C++ interne
src/app/                     interface Qt Widgets
src/core/                    modèles et logique métier
src/ssh/                     intégration SSH/SFTP
tests/                       tests Qt Test
cmake/                       modules de construction
docs/                        architecture, sécurité et sprints
```

Les choix structurants sont détaillés dans
[l’ADR de la stack](docs/adr/0001-technical-stack.md), et les opérations sur les
volumes livrées au Sprint 8 dans [docs/sprint-8.md](docs/sprint-8.md). La navigation
locale introduite au Sprint 7 reste décrite dans [docs/sprint-7.md](docs/sprint-7.md),
et les profils serveur dans [docs/sprint-6.md](docs/sprint-6.md).

## Licence

La licence du futur dépôt public n’est pas encore choisie. En l’absence de fichier `LICENSE`, le code reste sous droits exclusifs de son auteur.
