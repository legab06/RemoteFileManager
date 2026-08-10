# Sprint 5 — Manipulation directe et ergonomie clavier

Date : 10 août 2026  
Version : `0.6.0`

## Objectif

Le Sprint 5 ajoute des interactions directes de gestionnaire de fichiers aux
emplacements distants déjà ouverts dans RemoteFileManager : Drag & Drop interne,
clipboard logique et raccourcis clavier. Ces interactions restent des façades sur
les opérations distantes existantes. Une copie utilise toujours le backend serveur
fondé sur `cp` et un déplacement utilise toujours le renommage SFTP ; les données
ne transitent jamais par le client.

## Périmètre

- glisser un fichier, un dossier ou une sélection multiple/mixte entre panneaux ;
- déposer dans la zone vide du panneau ou sur un sous-dossier affiché ;
- choisir explicitement Copier, Déplacer ou Annuler après un drop valide ;
- mémoriser une intention Copy ou Move avec Ctrl+C/Ctrl+X et l'appliquer avec
  Ctrl+V dans le panneau actif ;
- atténuer les lignes coupées jusqu'au collage réussi, au remplacement ou à
  l'annulation du clipboard ;
- compléter les raccourcis usuels de manipulation et de navigation ;
- conserver le routage de refresh, la sélection après opération et le gestionnaire
  d'opérations du Sprint 4.

## Architecture

`InternalTransfer`, dans `rfm_core`, définit les données communes au clipboard et
au Drag & Drop : action Copy/Move, identité non secrète de connexion, génération
de session, panneau source et sélection distante figée. Il sérialise et valide le
format MIME interne sans dépendre des widgets ni de libssh.

`FileBrowserPane` produit le drag à partir de sa sélection au début du geste,
résout la destination sous le pointeur et présente les états visuels. Il émet une
intention de drop et ne déclenche aucune opération réseau.

`PaneWorkspace` conserve le modèle de panneau actif du Sprint 4 et fournit la
bascule de focus. `MainWindow` possède le clipboard de la fenêtre, valide le
contexte de session et présente le choix Copier/Déplacer/Annuler. La fonction
commune de lancement Copy/Move est utilisée par les commandes manuelles, les
commandes inter-panneaux, Ctrl+V et le DnD. Elle alimente les mêmes signaux worker,
le même `OperationProgress`, les mêmes contextes de refresh et le même historique.

`RemoteFileOperations` reste l'autorité métier dans le worker SSH. Les contrôles
de collision, chemin protégé, destination identique et récursion précèdent donc
toujours l'appel SFTP ou la copie serveur, même si l'UI a déjà rejeté un cas
évident.

## Drag & Drop interne

Le type MIME est
`application/x-remotefilemanager-internal-transfer-v1`. Son document JSON borné
contient uniquement : version, identifiant aléatoire de l'instance applicative,
hôte, port, génération de session, identifiant du panneau et chemins distants avec
leur nature fichier/dossier. Il ne contient ni utilisateur, ni mot de passe, ni
clé, ni commande SSH.

Un payload non reconnu, mal formé, provenant d'une autre instance ou d'une autre
génération de connexion est ignoré. La sélection encodée au démarrage du drag ne
change plus pendant le geste. Un drop sur un dossier choisit ce dossier ; un drop
sur un fichier ou dans la zone vide choisit le dossier actuellement affiché.

Après validation, un dialogue court affiche le nombre d'éléments et la destination
avec trois choix explicites : Copier, Déplacer et Annuler. Annuler ne crée ni appel
backend ni entrée dans le gestionnaire d'opérations.

## Clipboard logique

Le clipboard est strictement interne et reste en mémoire. Ctrl+C remplace son
contenu par une intention Copy ; Ctrl+X le remplace par une intention Move et
atténue les éléments du panneau source. Ctrl+V utilise toujours le dossier du
panneau actif comme destination et passe par le même lancement Copy/Move que le
DnD.

Une intention Copy est conservée après succès pour permettre plusieurs collages.
Une intention Move n'est vidée qu'après le succès complet du lot ; en cas de
résultat partiel, elle reste visible afin que l'échec ne soit pas masqué. Échap
annule une coupe en attente lorsque la fenêtre principale reçoit le raccourci.
Ctrl+C après Ctrl+X restaure immédiatement le rendu normal.

Le clipboard est vidé à la déconnexion, lors du remplacement de session et à la
fermeture. Il n'est jamais écrit sur disque et n'est jamais intégré au
presse-papiers système.

Le menu contextuel réutilise les mêmes `QAction` que les raccourcis : Copier et
Couper capturent la sélection dans ce clipboard, tandis que Coller applique son
contenu au dossier actuellement affiché par le panneau actif. Coller reste proposé
dans le menu d'une zone vide et ne prend jamais le sous-dossier cliqué comme
destination implicite. Il est désactivé si le clipboard est vide, expiré,
incompatible avec la connexion ou manifestement invalide pour le dossier affiché.

Copier vers… et Déplacer vers… restent des commandes distinctes. Elles ouvrent le
dialogue de chemin distant existant et lancent directement l'opération choisie,
sans alimenter le clipboard. En vue scindée, Copier/Déplacer vers l'autre panneau
restent également disponibles dans le groupe des destinations explicites.

## Raccourcis

| Raccourci | Action sur le panneau actif |
|---|---|
| Ctrl+C | Copier la sélection dans le clipboard interne |
| Ctrl+X | Couper la sélection |
| Ctrl+V | Coller dans le dossier affiché |
| Échap | Annuler une coupe en attente |
| F2 | Renommer l'élément sélectionné |
| Suppr | Supprimer avec la confirmation existante |
| Ctrl+A | Sélectionner toutes les lignes |
| F5 | Actualiser ce panneau |
| Alt+Gauche | Historique précédent |
| Alt+Droite | Historique suivant |
| Alt+Haut | Dossier parent |
| Ctrl+L | Focaliser et sélectionner la barre de chemin |
| F6 | Basculer vers l'autre panneau visible |

F6 a été retenu pour la bascule : il est portable, traditionnellement associé au
passage entre zones d'une fenêtre et laisse Tab à la navigation d'accessibilité
normale entre widgets. En vue simple, il ne fait rien.

## Validation des destinations

Les validations communes refusent un payload expiré ou étranger, une connexion
différente, un chemin vide ou remontant, un mélange absolu/relatif, une destination
finale identique à une source et un dossier déposé dans lui-même ou l'un de ses
descendants. Les validations du backend refusent en plus les collisions et tout
chemin protégé avant l'opération réelle. Aucun écrasement ou fusion implicite
n'est ajouté.

## Feedback visuel

Le drag affiche une vignette compacte indiquant le nombre d'éléments distants. Le
panneau valide reçoit une bordure issue de `QPalette::Highlight` et le sous-dossier
survolé reprend `Highlight`/`HighlightedText`. Une cible interdite utilise une
bordure de palette neutre et le curseur interdit. Tous ces états sont supprimés au
drop, à la sortie ou à l'annulation.

Les éléments coupés utilisent la couleur de texte désactivé de la palette et une
italique légère. Le rendu est recalculé après chaque listing, ce qui évite qu'un
refresh perde ou conserve à tort l'état de coupe. Aucune couleur claire ou sombre
n'est codée en dur.

## Sécurité et fiabilité

- aucune donnée d'authentification dans le clipboard ou le MIME ;
- aucun type ou handle libssh dans l'UI ;
- aucune opération lente sur le thread graphique ;
- identité de session et origine applicative vérifiées avant le choix du drop et
  avant le lancement ;
- chemins toujours traités par `RemotePath` et `RemoteFileOperations` ;
- copie distante sécurisée et renommage SFTP existants inchangés ;
- `known_hosts`, absence d'élévation et politique sans écrasement inchangés ;
- DnD et collage enregistrés comme Copy ou Move, sans catégorie artificielle.

## Tests

Une nouvelle suite `unit.internal_transfer` couvre la sérialisation bornée, le
contenu non secret, le remplacement du clipboard, les sessions incompatibles et
les destinations identiques, récursives ou de convention incompatible.

Les suites UI couvrent la création d'un drag multisélection, le rejet de données
externes, le drop sur zone vide et sous-dossier, l'atténuation de coupe après
refresh, le collage Copy/Move, l'effacement d'un Move réussi, Échap, la
déconnexion, les trois choix du drop et les raccourcis. Les tests métier existants
continuent de couvrir collisions, permissions, opérations partielles et appels
serveur. Aucun test ne requiert de serveur SSH externe.

## Décisions d'UX

Les fonctions de clipboard sont ajoutées au menu Édition et au menu contextuel ;
aucun nouveau bouton permanent n'encombre la fenêtre. Le menu contextuel sépare
visuellement Copier/Couper/Coller, les opérations « vers… », puis les autres
actions sur la sélection. Les actions Qt centralisées portent les raccourcis et
les deux menus déclenchent la même orchestration. Le chemin reste en lecture
seule : Ctrl+L le focalise et sélectionne son contenu sans introduire une nouvelle
sémantique d'édition pendant ce sprint.

## Limites et hors périmètre

Le Sprint 5 ne prend en charge qu'une seule connexion SSH active et les opérations
entre emplacements de cette connexion. La copie serveur dépend toujours de la
commande distante `cp` décrite au Sprint 2 ; le déplacement entre systèmes de
fichiers peut toujours être refusé par SFTP. Les copies/déplacements distants ne
fournissent pas de progression fine ni d'annulation après leur lancement.

Restent explicitement hors périmètre : navigateur local, onglets, multi-connexion,
transfert serveur A vers serveur B, presse-papiers système, persistance du
clipboard, écrasement/fusion, upload ou download par DnD et montage SSHFS.

Le Drag & Drop externe depuis Dolphin, Nautilus, Windows Explorer, Finder, le
bureau ou toute autre application sera traité ultérieurement avec la distinction
explicite des sources locales et les transferts local ↔ distant.

## Étapes d'implémentation

1. [x] Cartographier les panneaux, le worker et le routage Copy/Move du Sprint 4.
2. [x] Introduire le modèle testable d'intention interne et centraliser le lancement.
3. [x] Ajouter Ctrl+C/Ctrl+X/Ctrl+V et le rendu de coupe.
4. [x] Ajouter les raccourcis de manipulation, navigation et la bascule F6.
5. [x] Encoder et valider le MIME interne.
6. [x] Implémenter les cibles, le choix d'action et le feedback de drop.
7. [x] Étendre les tests indépendants d'un serveur.
8. [x] Documenter les décisions et passer la version à `0.6.0`.
