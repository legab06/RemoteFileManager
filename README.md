<p align="center">
  <img src="assets/branding/png/logo-512.png"
       alt="RemoteFileManager"
       width="220">
</p>

# RemoteFileManager

RemoteFileManager (RFM) est un gestionnaire de fichiers graphique natif pour parcourir et modifier les fichiers locaux et ceux d’un serveur distant via SSH/SFTP. Son interface Qt Widgets s’inspire de Dolphin, sans montage SSHFS ni agent propriétaire à installer côté serveur.

Ce logiciel a été entièrement vibe-codé pour répondre à un besoin que j'avais. Je ne suis pas un développeur ou un codeur, juste un amateur avec un besoin précis. 


## Principes

- client C++20 léger avec interface Qt Widgets ;
- prototype Linux en premier, architecture compatible, à terme, Windows et macOS ;
- serveur SSH/SFTP standard, sans agent propriétaire ;
- copies entre chemins du même serveur exécutées côté serveur ;
- sécurité explicite : vérification de la clé d’hôte et aucun mot de passe persistant ;
- interface inspirée d’un navigateur de fichiers natif ;
- navigation locale multiplateforme fondée sur Qt, sans commande système ni SSHFS ;
- panneaux à source explicite, permettant notamment un affichage scindé local/SSH, local/local ou ssh/ssh.

## Fonctionnalités disponibles

- navigation dans les répertoires locaux et dans les serveurs distants par SSH/SFTP ;
- un ou deux panneaux optionnels, chacun pouvant afficher une source locale ou la
  session SSH active ;
- arbre Places regroupant emplacements locaux, profils serveur enregistrés, connexion
  active, volumes et périphériques amovibles ;
- profils serveur enregistrés, modifiables et supprimables, accessibles depuis l’accueil
  et Places ; connexion directe à un profil ou nouvelle connexion avec sauvegarde optionnelle ;
- authentification par clé/agent SSH (chemin de clé privée optionnel, repli par mot de
  passe optionnel) ou par mot de passe seul, sans sauvegarde du mot de passe ;
- sélection multiple, glisser-déposer interne, Copy / Move / Rename / Delete / New folder
  en local et sur SSH ;
- « Copy to other pane » entre panneaux Local → Local, SSH → SSH sur la session active,
  Local → SSH et SSH → Local ; copie Local ↔ SSH également par glisser-déposer ;
- transferts de fichiers réguliers et dossiers par SFTP avec progression, pause, reprise
  et annulation ; les boutons Upload/Download ont été retirés du navigateur principal,
  mais ces termes restent utilisés par les moteurs et le panneau Operations ;
- propriétés des fichiers à partir des métadonnées du listing ; colonnes Name / Size /
  Type / Modified, types et icônes fondés sur les extensions via la base MIME Qt ;
- colonnes triables, déplaçables et redimensionnables, état de vue et d’en-tête conservé
  entre lancements, réinitialisable par View → Reset file view ;
- ouverture des fichiers locaux avec l’application par défaut du système ;
- sous Linux, détection et actualisation explicite des volumes locaux et des volumes du
  serveur connecté, y compris les périphériques non montés lorsque `lsblk` est
  disponible ;
- montage et démontage des volumes locaux et distants Linux, avec protection de la
  racine et des volumes système et ciblage du seul point de montage sélectionné pour
  les périphériques attachés plusieurs fois ;
- utilisation de UDisks et de son autorisation Polkit lorsque nécessaire, sans
  `sudo`, `su` ou `pkexec` intégré ; le mot de passe d’autorisation distante est
  transféré sans copie partagée, effacé après usage et n’est jamais enregistré.

## Limites actuelles

- une seule session SSH active : deux panneaux SSH partagent le même serveur ;
- pas de déplacement Local ↔ SSH ; le copier/coller interne reste limité à Local → Local
  ou SSH → SSH, contrairement à « Copy to other pane » et au glisser-déposer ;
- les transferts Local ↔ SSH ne prennent pas en charge les liens symboliques ni les
  nœuds spéciaux ;
- suppression définitive après confirmation, sans corbeille ni restauration intégrée ;
- la suppression locale refuse les arbres traversant un point de montage ou une frontière de volume afin d’éviter la suppression involontaire de données sur un autre filesystem. Les liens symboliques ne sont pas suivis. Les vérifications sont conservatrices et peuvent refuser une suppression si la sécurité ne peut pas être établie ;
- la suppression n’est pas transactionnelle : une erreur survenant pendant l’opération peut laisser une suppression partielle ;
- gestion des volumes distants réservée à Linux ; les opérations serveur utilisant
  `cp`, la suppression récursive protégée et le déplacement entre filesystems ont des
  dépendances système supplémentaires détaillées dans l’architecture ;
- les clés privées protégées par une phrase secrète doivent être chargées dans un agent
  SSH, en laissant vide le champ Private key ; RFM ne demande ni ne stocke cette phrase.

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

Voir [l’architecture](docs/architecture.md), le [modèle de sécurité](docs/security-model.md)
et [l’ADR de la stack](docs/adr/0001-technical-stack.md). Les documents
`docs/sprint-*.md` conservent l’historique des livraisons.

## Licence

Copyright © 2026 Gabriel Albaladejo.

RemoteFileManager est un logiciel libre distribué sous les termes de la
GNU General Public License version 3 ou, à votre choix, toute version ultérieure
(`GPL-3.0-or-later`).

Voir [LICENSE](LICENSE) pour le texte complet de la licence.
