# Sprint 1 — Connexion SSH sécurisée et premier affichage SFTP

Date : 7 août 2026  
Version : `0.2.0`

## Livrables

- formulaire hôte, utilisateur et port avec validation ;
- authentification asynchrone : agent et clés OpenSSH en priorité, mot de passe facultatif en repli ;
- vérification systématique de `known_hosts` ;
- confirmation explicite de l’empreinte SHA-256 lors d’une première connexion ;
- refus sans contournement d’une clé d’hôte modifiée ;
- initialisation SFTP et affichage du répertoire de connexion ;
- navigation dans les dossiers, dossier parent et actualisation ;
- métadonnées de base : nom, taille et date de modification.

## Garanties de sécurité

Le mot de passe n’est ni sauvegardé ni journalisé. Il est transmis au worker réseau puis effacé dès la tentative d’authentification. Une nouvelle clé acceptée est confiée au fichier `known_hosts` géré par libssh. Une clé connue qui change bloque la connexion et ne peut pas être remplacée depuis l’application.

Les opérations libssh et SFTP sont confinées dans un thread dédié. L’interface ne manipule aucun type natif libssh et reste réactive pendant la connexion et les lectures distantes.

## Hors périmètre

- stockage des profils et intégration à un trousseau système ;
- authentification interactive keyboard-interactive et clé privée chiffrée avec demande de passphrase ;
- transferts, renommage, suppression et opérations distantes ;
- reconnexion automatique et annulation d’une connexion en cours.

## Durcissement ultérieur

Le dossier de connexion est maintenant canonicalisé par SFTP en chemin absolu, ce qui permet de
remonter jusqu'à `/`. L'activation d'un lien symbolique tente une ouverture de dossier : un lien de
dossier fonctionne et un lien cassé ou non-dossier produit une erreur de navigation locale sans
déconnecter la session.
