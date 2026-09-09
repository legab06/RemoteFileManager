# Onglets globaux — lots 1 et 2

La pile centrale contient `HomePage` et `WorkspaceTabs`. Ce dernier possède un
`QTabWidget` démarrant avec un onglet `Tab 1`, contenant un `PaneWorkspace`.
Chaque workspace conserve son splitter, ses panneaux et son panneau actif.

`WorkspaceTabs` expose le workspace actif, les panneaux et leurs IDs, et relaie
les signaux d'activation et de visibilité. `pane()` et `paneIds()` recherchent dans
tous les workspaces ; `visiblePaneIds()` et `otherVisiblePane()` concernent le
workspace actif. La navigation, les transferts et les opérations restent dans
`MainWindow`. Split view, Reset file view et Switch pane résolvent le workspace
actif au déclenchement ; les autres actions utilisent son panneau actif.
La réinitialisation conserve la portée globale des préférences de colonnes et de
tri déjà implémentée par `FileBrowserPane`, même si son point d'entrée est le
workspace actif.

Le bouton `+` en haut à droite de la barre crée et active un workspace neuf :
un seul panneau, aucun emplacement ni historique hérité. L'utilisateur choisit
son emplacement dans Places. Les titres `Tab N` utilisent un compteur croissant.
Les boutons de fermeture sont masqués lorsque le dernier onglet reste seul ;
l'API refuse également sa fermeture. Aucune fermeture ne déconnecte SSH.

Chaque panneau nouvellement créé est raccordé via `paneAdded` et reçoit le choix
actuel de Show hidden files. Les préférences de colonnes, tri et disposition
restent globales. Le split, les historiques, sélections et panneau actif restent
propres à chaque workspace. Changer d'onglet synchronise les actions sans
modifier le split d'un autre onglet.

`openPaneIds()` parcourt les panneaux non masqués par le split dans tous les
onglets. Les rafraîchissements automatiques, les suites d'opérations et les replis
après démontage l'utilisent pour maintenir les onglets inactifs à jour. Un listing
reçu en arrière-plan alimente son panneau sans changer l'onglet actif.

La fermeture retire le workspace du conteneur, notifie `paneRemoved` pour chacun
de ses panneaux et détruit le workspace avec ses enfants Qt. MainWindow retire
les listings, comptages, générations, sources attendues, états busy, préflights
et rafraîchissements programmés associés. Le clipboard est vidé si son panneau
source disparaît. Les IDs ne sont jamais recyclés.

Une requête SSH déjà exécutée garde uniquement son identifiant de sérialisation
jusqu'à sa réponse : son résultat ou erreur est ignoré puis le listing suivant
est lancé. Les résultats locaux et comptages tardifs n'ont plus de contexte et
sont ignorés. Les opérations lancées conservent leurs chemins et leur suivi
global ; seuls leurs IDs de panneaux sont détachés. Elles peuvent se terminer
dans OperationPanel et rafraîchir les destinations encore ouvertes.

Les IDs sont alloués dans `PaneWorkspace.cpp` par un compteur commun au processus,
sur le thread graphique comme toute création de QWidget. Zéro reste invalide.
Les IDs ne sont jamais réutilisés, même après destruction, afin de ne pas associer
un résultat asynchrone ancien à un nouveau panneau. L'épuisement du compteur arrête
l'application avant tout débordement.

L'ouverture locale ou distante affiche le conteneur d'onglets. La déconnexion
conserve les panneaux locaux ; sans panneau local, elle revient à HomePage.
Une seule session SSH reste active pour toute l'application.

La duplication, persistance, restauration, les titres dynamiques, le renommage,
le réordonnancement, les menus et raccourcis d'onglets restent hors périmètre.
Il n'existe aucun pool de connexions ni seconde session SSH.

Les tests dédiés couvrent l'état initial, les recherches invalides, le split,
les signaux, la création/fermeture et l'unicité des IDs après destruction.
Les tests MainWindow vérifient les actions, Places, les préférences des nouveaux
panneaux, les résultats locaux en arrière-plan et après fermeture, la reprise
de la file SSH après succès/erreur tardifs et la poursuite d'une copie locale.

## Validation manuelle

1. Dans Tab 1, ouvrir Local A puis activer Split et ouvrir Local B dans l'autre
   panneau. Sélectionner des fichiers et naviguer dans quelques sous-dossiers.
2. Activer Show hidden files, cliquer `+` : Tab 2 doit être vide, non scindé et
   sans historique. Ouvrir Local C, contenant un fichier caché, depuis Places.
3. Connecter un serveur SSH de test une seule fois. Dans Tab 2, garder Local C
   dans un panneau et ouvrir un chemin de ce serveur dans le second.
4. Alterner Tab 1/Tab 2 : vérifier chemins, sélections, panneau actif et Back /
   Forward. Désactiver Split dans Tab 2 ; Tab 1 doit conserver son split, et la
   coche de l'action doit suivre l'onglet. Réactiver Split dans Tab 2.
5. Ouvrir un autre chemin du même serveur dans un troisième onglet : aucune
   reconnexion ni saisie de mot de passe ne doit être demandée.
6. Dans Tab 1, utiliser Copy to other pane entre A et B. Lancer une copie assez
   longue, fermer Tab 1, puis vérifier sa progression et sa fin dans OperationPanel.
7. Cliquer `+` après fermeture : nouvel onglet vide, titre suivant, aucun historique
   restauré. Fermer les onglets jusqu'au dernier : son bouton de fermeture disparaît.
8. Pendant un listing lent local ou SSH, changer d'onglet puis fermer l'onglet
   demandeur : aucune erreur parasite, aucun blocage de navigation ultérieure.
9. Déconnecter SSH avec plusieurs onglets locaux/distants : les emplacements
   locaux restent utilisables et tous les panneaux SSH sont vidés. Sans aucun
   panneau local, HomePage globale réapparaît.
