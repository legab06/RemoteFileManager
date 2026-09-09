# Fondation des onglets globaux

La pile centrale contient `HomePage` et `WorkspaceTabs`. Ce dernier possède un
`QTabWidget` avec un unique onglet `Tab 1`, contenant le `PaneWorkspace` existant.
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

Les IDs sont alloués dans `PaneWorkspace.cpp` par un compteur commun au processus,
sur le thread graphique comme toute création de QWidget. Zéro reste invalide.
Les IDs ne sont jamais réutilisés, même après destruction, afin de ne pas associer
un résultat asynchrone ancien à un nouveau panneau. L'épuisement du compteur arrête
l'application avant tout débordement.

L'ouverture locale ou distante affiche le conteneur d'onglets. La déconnexion
conserve les panneaux locaux ; sans panneau local, elle revient à HomePage.
Une seule session SSH reste active pour toute l'application.

Ce lot n'expose aucune création, fermeture, duplication ou persistance d'onglets,
ni bouton + ou raccourci associé. L'ajout réel de workspaces et leur cycle de vie
seront traités dans un lot suivant, avec le raccordement des signaux de chaque
nouveau workspace et la politique de rafraîchissement des onglets inactifs.

Les tests dédiés couvrent l'état initial, les recherches invalides, le split,
les signaux et l'unicité des IDs entre workspaces et après destruction. Un test
MainWindow injecte un second workspace uniquement comme fixture pour vérifier
le ciblage dynamique des actions, sans exposer cette possibilité dans le produit.
