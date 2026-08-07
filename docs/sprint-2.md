# Sprint 2 — Opérations distantes

Date : 7 août 2026  
Version : `0.3.0`

## Objectifs et périmètre

Le Sprint 2 transforme la navigation SFTP en première expérience de gestion de fichiers : création de dossiers, renommage, déplacement, copie distante et suppression de fichiers ou dossiers. La vue accepte la sélection multiple, propose des actions contextuelles et n’est actualisée qu’après le résultat réel de l’opération.

## Choix techniques

- Les règles métier sont regroupées dans un orchestrateur synchrone testable, exécuté uniquement par le worker SSH.
- Un backend abstrait sépare ces règles de libssh et permet des doubles de test sans serveur.
- `mkdir`, `rename`, `unlink`, `rmdir`, l’inspection et le parcours utilisent SFTP.
- Le déplacement repose sur le renommage SFTP, donc reste entièrement distant.
- Faute d’API de copie serveur dans SFTP/libssh, la copie appelle `cp` dans un canal SSH. La commande est construite dans un composant unique avec arguments échappés, mode sans écrasement `-n` et séparateur `--`; aucun glob n’est employé.
- Chaque lot possède un identifiant et retourne un résultat par élément afin de représenter les réussites partielles.

## Mesures de sécurité

- Validation des noms et normalisation POSIX indépendante du système client.
- Refus des chemins destructifs vides, `/`, `.`, `..` et de toute remontée hors de la racine relative.
- Vérification préalable des destinations : aucun écrasement silencieux.
- Suppression récursive par parcours SFTP sans suivre les liens symboliques et sans commande shell.
- Confirmation obligatoire, renforcée dès qu’un dossier doit être supprimé récursivement.
- Aucun chemin n’est concaténé directement dans une commande et aucune donnée sensible n’est journalisée.

## Critères d’acceptation

- [x] Créer un dossier avec validation locale et retour d’erreur contextualisé.
- [x] Renommer un unique élément sans écrasement et restaurer sa sélection après actualisation.
- [x] Déplacer ou copier une sélection vers un chemin distant explicite avec résultats individuels.
- [x] Supprimer fichiers et arborescences après confirmation, avec garde-fous et échecs partiels.
- [x] Exposer un menu contextuel cohérent, la sélection multiple et un état d’activité non bloquant.
- [x] Compiler sans avertissement et réussir tous les tests sans serveur SSH externe.

## Tests prévus

- validation et normalisation des chemins et noms ;
- échappement de la commande de copie ;
- création, renommage, déplacement, copie et suppression simple/récursive ;
- collisions, permissions refusées, fonction de copie absente et résultats partiels ;
- signal d’erreur du worker, sélection multiple et non-régression de la coque de connexion/navigation.

État final : les trois exécutables de test CTest réussissent. Les tests couvrent les règles de chemins, l’échappement shell, toutes les opérations, les collisions, les permissions, la copie non supportée, les suppressions récursives, les garde-fous, les résultats partiels, les signaux du worker et la sélection multiple. Ils ne nécessitent aucun serveur externe.

## Limites connues

- La copie distante nécessite un shell et une commande `cp` acceptant `-R`, `-n` et `--`. Il n’existe volontairement aucun repli faisant transiter les données par le client.
- Un déplacement SFTP entre systèmes de fichiers distants peut être refusé par le serveur ; aucun repli par copie puis suppression n’est tenté.
- Les collisions sont contrôlées avant l’opération et la copie emploie en plus le mode sans écrasement. Le protocole SFTP v3 ne fournit pas de renommage atomique « no-clobber » portable face à une modification concurrente externe.
- Les suppressions sont définitives : aucune corbeille ou récupération distante n’est encore disponible.
- Les opérations ne publient pas de progression et ne sont pas annulables dans ce sprint.
- La CI couvre les règles avec un double de backend ; création, copie, déplacement et suppression doivent encore être validés manuellement sur de vrais serveurs OpenSSH/SFTP.

## Report explicite au Sprint 3

Les transferts client–serveur et serveur–client, leur progression, pause, annulation et file d’attente restent hors périmètre. Les onglets, l’affichage scindé, SSHFS et tout composant serveur supplémentaire restent également reportés.
