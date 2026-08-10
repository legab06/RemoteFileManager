# RemoteFileManager

RemoteFileManager est un gestionnaire de fichiers graphique natif pour administrer les fichiers d’un serveur distant via SSH/SFTP. L’objectif est de retrouver une ergonomie proche de Dolphin sans monter le serveur avec SSHFS et sans installer de logiciel supplémentaire côté serveur.

> État actuel : **Sprint 4 — navigation scindée et opérations entre panneaux,
> version 0.5.0**. La navigation SSH/SFTP réelle, la copie et le déplacement
> distants, les transferts et l’historique persistant ont été validés. L’évaluation
> visuelle approfondie des thèmes clair et sombre est différée à un environnement
> natif hors WSL et ne bloque pas cette version.

## Principes

- client C++20 léger avec interface Qt Widgets ;
- prototype Linux en premier, architecture compatible Windows et macOS ;
- serveur SSH/SFTP standard, sans agent propriétaire ;
- opérations distantes exécutées côté serveur quand c’est pertinent ;
- sécurité explicite : vérification de la clé d’hôte et aucun secret en clair ;
- interface inspirée d’un navigateur de fichiers natif.

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
[l’ADR de la stack](docs/adr/0001-technical-stack.md), et les livrables de la
version 0.5.0 dans [docs/sprint-4.md](docs/sprint-4.md).

## Licence

La licence du futur dépôt public n’est pas encore choisie. En l’absence de fichier `LICENSE`, le code reste sous droits exclusifs de son auteur.
