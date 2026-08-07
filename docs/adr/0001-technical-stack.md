# ADR 0001 — Stack technique du client

- Statut : accepté
- Date : 7 août 2026

## Contexte

RemoteFileManager doit proposer une interface native et légère, d’abord sous Linux puis sous Windows et macOS. Il doit fonctionner avec un serveur SSH standard, sans composant serveur à installer.

## Décision

- **C++20** pour le cœur et l’application ;
- **Qt 6 Widgets** pour une interface de bureau native et mature ;
- **CMake + Ninja** pour une construction reproductible et multiplateforme ;
- **libssh** pour SSH/SFTP ;
- **Qt Test + CTest** pour les tests ;
- bibliothèques séparées pour empêcher le couplage de l’interface au transport.

## Conséquences

Qt augmente la taille minimale du paquet mais réduit fortement le travail nécessaire pour obtenir une interface fiable sur trois systèmes. libssh est une dépendance cliente uniquement : le serveur conserve son OpenSSH/SFTP standard. Le prototype reste centré sur Linux, tandis que les choix de construction évitent une réécriture ultérieure.

