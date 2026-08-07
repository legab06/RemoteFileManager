# Sprint 0 — Fondations

Date : 7 août 2026  
Version : `0.1.0`

## Objectif

Obtenir un projet propre, reproductible et assez stable pour commencer la connexion SSH sans mélanger dès le départ l’interface, la logique métier et le transport réseau.

## Livrables

- projet C++20 construit avec CMake et Ninja ;
- dépendances de base : Qt 6 Widgets et libssh ;
- séparation en bibliothèques `rfm_core` et `rfm_ui` ;
- premier modèle métier `ConnectionProfile` ;
- première fenêtre native façon navigateur de fichiers ;
- deux tests automatiques : modèle de connexion et coque de l’interface ;
- intégration continue Linux ;
- conventions de code, documentation d’architecture et modèle de sécurité ;
- presets Debug et Release utilisables sur Linux, Windows et macOS.

## Hors périmètre

- ouverture d’une session SSH réelle ;
- formulaire de connexion et stockage des profils ;
- validation interactive de la clé d’hôte ;
- navigation SFTP ;
- copie, déplacement, suppression et transfert ;
- onglets et écran scindé.

## Définition de terminé

- [x] le dépôt possède une structure claire et documentée ;
- [x] le cœur ne dépend pas des widgets de l’interface ;
- [x] la couche Qt ne manipule pas directement les structures natives de libssh ;
- [x] une fenêtre native se lance dans l’état « déconnecté » ;
- [x] les actions non encore disponibles sont désactivées ou annoncées comme futures ;
- [x] les tests de base et la CI sont présents ;
- [ ] la compilation et les tests passent sur une machine équipée de Qt/libssh ou dans la première exécution de la CI.

Le dernier point reste une validation d’environnement : il doit être coché avant de fermer définitivement le Sprint 0.

## Étape suivante

Le Sprint 1 commencera par une connexion SSH sûre : formulaire hôte/utilisateur/port, authentification, contrôle de la clé d’hôte, connexion asynchrone, puis affichage du premier répertoire SFTP.

