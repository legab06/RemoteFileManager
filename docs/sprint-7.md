# Sprint 7 — Système de fichiers local et arbre de navigation

Date : 11 août 2026
Version : `0.8.0`

## Objectif

Le Sprint 7 rend les répertoires locaux navigables dans les mêmes panneaux que les
répertoires SFTP et remplace la liste plate de Places par une arborescence organisée
par machine. La machine locale, ses emplacements et ses volumes restent séparés des
profils SSH. Une seule session SSH peut toujours être active.

Le périmètre privilégie une navigation fiable. Les mutations locales et les copies
entre sources sont volontairement différées ; une combinaison non prise en charge
est désactivée avant d'atteindre le backend SSH.

## Architecture des emplacements

`BrowserLocation` est la petite abstraction commune introduite dans `rfm_core`. Elle
associe trois informations : une source (`Local` ou `Ssh`), un identifiant de machine
et un chemin. `FileBrowserPane` conserve désormais cette identité complète, y compris
dans Back et Forward. Deux chemins textuellement identiques, par exemple `/tmp`, ne
sont donc pas confondus s'ils appartiennent à deux machines différentes.

Cette évolution ne crée pas de grand `FileSystemBackend` virtuel. Le chemin réseau
existant reste orchestré par `MainWindow` et `SshSession`, tandis que la lecture locale
est portée par `LocalFileSystemWorker`. Le type historique `RemoteEntry` est encore
réutilisé pour présenter les métadonnées communes (nom, taille, date, dossier et lien)
afin de ne pas perturber les opérations SSH. La provenance n'est jamais déduite de ce
type : elle vient toujours de `BrowserLocation`.

La découverte du stockage suit la même séparation : `Storage.hpp` porte le modèle,
le parsing Linux et les règles métier communes, tandis que `LocalFileSystem` collecte
les données natives et `SshSession` collecte les données du serveur via la session
SFTP existante. Les collecteurs ne choisissent donc pas chacun leur propre définition
de `Internal`, `External`, `Network` ou `Unknown`.

## Navigation locale

`LocalFileSystem` utilise les API Qt portables `QDir`, `QFileInfo` et `QStorageInfo`.
Son enrichissement Linux isolé lit en plus sysfs et utilise `stat()` pour identifier
un périphérique bloc ; les autres plateformes conservent le fallback Qt. Il ne lance
aucune commande système et ne suppose ni forme de répertoire personnel, ni emplacement
de montage. Les résultats sont triés
avec les dossiers en premier et incluent les entrées cachées, comme le navigateur
distant.

`LocalFileSystemWorker` vit dans un `QThread` distinct. Une activation dans l'arbre,
un changement de dossier, Parent, Back, Forward ou Refresh émet une requête identifiée.
`MainWindow` n'applique que la réponse encore attendue par le panneau. Une réponse
ancienne ne peut donc pas écraser une navigation plus récente. Les erreurs indiquent
le chemin local concerné et ne modifient pas la session SSH.

## Modèle d'arborescence et chargement paresseux

`NavigationTree` encapsule un `QTreeWidget`, donc les indicateurs, l'expansion, le
repli et la sélection restent ceux de Qt et du thème système. Ses deux racines sont :

```text
This Computer
 ├── Home
 ├── Documents
 ├── Downloads
 └── Volumes
Servers
 ├── profil A
 └── profil B
```

Documents et Downloads ne sont ajoutés que lorsque `QStandardPaths` fournit un chemin
distinct. Chaque dossier potentiellement développable commence avec un unique enfant
fictif. Son expansion retire ce marqueur et demande seulement le niveau immédiatement
inférieur. Aucun parcours récursif n'a lieu au démarrage ni pendant une expansion.
Une erreur laisse le nœud réessayable après repli et nouvelle expansion.

Les chargements locaux passent par le worker local. Ceux du serveur actif réutilisent
la file sérialisée et le thread de `SshSession`. Chaque requête distante d'arbre porte
la génération de connexion et l'identifiant du profil ; une réponse d'une ancienne
session est ignorée. L'expansion ne change pas le panneau actif et ne perturbe donc pas
la logique de focus.

## Machines et volumes

Les volumes locaux prêts, valides et actuellement montés sont obtenus avec
`QStorageInfo::mountedVolumes()` dans le worker. Sur un serveur Linux, `/proc/self/mountinfo`
et l'ascendance `/sys/dev/block` sont lus en SFTP dans le thread SSH, sans commande
shell ni composant serveur. `StorageVolume` conserve le nom d'affichage, le label du
filesystem, le modèle matériel, la racine navigable, le périphérique, le type de
filesystem, la capacité, l'état lecture seule et trois notions volontairement
distinctes :

- `StorageKind` décrit la catégorie principale : `System`, `Internal`, `External`,
  `Network` ou `Unknown` ;
- `removable` indique que le média peut être retiré du lecteur ;
- `ejectable` prépare une capacité d'éjection explicitement détectée.

Un SSD USB peut ainsi être `External` avec `removable == false`. Inversement, le
caractère amovible ne suffit jamais à lui seul à imposer la catégorie `External`.
`ejectable` reste actuellement faux lorsque la plateforme ne fournit pas une preuve
fiable ; aucune des trois valeurs ne déclenche une opération sur le périphérique.

Le nom affiché suit une règle commune et indépendante de la navigation : label du
filesystem, puis modèle matériel, puis dernier composant du périphérique bloc, puis
point de montage. En cas de noms identiques entre frères, seul le texte visible reçoit
un suffixe court issu du périphérique ou du point de montage ; `rootPath` reste toujours
la destination réelle. Le tooltip reprend
le nom humain puis, lorsqu'ils existent, le périphérique, le point de montage, le type
de filesystem, le modèle et la taille. Les valeurs absentes, `Unknown` et les tailles
nulles sont omises. Toutes les métadonnées sont normalisées puis converties depuis du
texte brut afin qu'aucun serveur ne puisse injecter du rich text ou de fausses lignes.

La classification applique un ordre conservateur. La racine système est `System`, un
filesystem réseau connu est `Network`, une couche bloc virtuelle reste `Unknown`, un
transport externe reconnu est `External`, et un autre périphérique bloc physique n'est
`Internal` que si toute l'ascendance sysfs a été parcourue avec succès. Une topologie
absente, interrompue ou limitée reste `Unknown`. `System`, `Internal`,
`Network` et `Unknown` sont présentés sous la branche `Volumes` de leur machine. Les
seuls objets `External` sont placés sous la branche `External devices` de cette même
machine. Cette seconde branche n'est créée que lorsqu'elle contient au moins un
périphérique.

Les racines sont normalisées, résolues lorsqu'une forme canonique est disponible et
dédupliquées dans la découverte puis défensivement dans l'arbre. Une racine ne peut
donc pas apparaître simultanément dans Volumes et External devices. Tous les objets
restent de simples emplacements paresseux : leur activation ouvre un `BrowserLocation`
portant explicitement la source locale ou l'identité du serveur concerné.

### Enrichissement Linux

Sous Linux, `QStorageInfo` reste la source commune. Pour un `device` qui désigne
réellement un périphérique bloc, la couche locale utilise `stat()` afin d'obtenir son
couple majeur/mineur, résout `/sys/dev/block/<major>:<minor>`, puis parcourt ses parents
sysfs en lecture seule. Un ancêtre sur les bus USB, FireWire ou Thunderbolt marque le
transport externe même lorsque le média est déclaré fixe. Pour MMC, le média doit en
plus être marqué amovible, ce qui évite de classer automatiquement un eMMC interne
comme périphérique externe. Le champ noyau `removable` est collecté séparément.
Le label local vient de `QStorageInfo::name()`. Le modèle est lu en remontant sysfs,
dans `model` ou `device/model`, ce qui permet à une partition d'hériter du modèle de
son disque parent.

Le parcours ne s'arrête pas sur les sous-systèmes intermédiaires `block` ou `scsi` et
n'emploie aucune profondeur fixe. Il remonte jusqu'à la racine sysfs, avec un ensemble
de chemins visités pour prévenir une boucle, collecte toute la chaîne des liens
`subsystem`, puis décide seulement après cette collecte. Un niveau sans lien
`subsystem` ajoute une preuve vide mais ne termine jamais le parcours. Une chaîne telle
que `block → block → [sans subsystem] → scsi → scsi → scsi → usb` constitue donc
explicitement une preuve de transport externe, y compris pour une partition enfant.
Les lectures déterminantes distinguent un attribut légitimement absent d'une permission
refusée ou d'une erreur d'I/O. Ces erreurs laissent le parcours continuer afin de ne pas
perdre une preuve USB accessible plus haut, mais empêchent `topologyComplete` de devenir
vrai.

Les périphériques virtuels (`loop`, device mapper, RAID logiciel et autres chemins
`/sys/devices/virtual`) restent `Unknown`, sauf lorsque le volume est la racine système.
Cette prudence évite d'inventer une origine physique à travers une couche virtuelle.
Les partitions conservent chacune leur point de montage navigable et héritent de la
topologie du disque parent. Aucun nom `/dev/sdX` n'est interprété comme heuristique.

### Fallbacks multiplateformes

Windows et macOS continuent de fournir toutes les métadonnées communes via
`QStorageInfo`. Dans cette passe, aucune API native supplémentaire non testable n'est
introduite : la racine de l'application peut être classée `System`, les filesystems
réseau reconnus `Network`, et les autres volumes restent `Unknown`. Le point
d'extension spécifique à la plateforme est isolé dans `LocalFileSystem.cpp` ; ni
`NavigationTree` ni `BrowserLocation` ne connaissent l'OS.

Il n'existe aucune catégorie globale de volumes. Le sprint n'ajoute ni mount, ni
umount, ni eject, ni safely remove, ni partitionnement, ni formatage. La découverte
distante actuelle est un collecteur Linux isolé ; un autre OS distant pourra fournir
un collecteur différent sans modifier le modèle ou les règles de classification.
Le collecteur Linux distant associe les liens de `/dev/disk/by-label` aux périphériques
de `mountinfo`, lit le modèle dans la même ascendance sysfs et demande la capacité avec
`statvfs` SFTP. Chacune de ces métadonnées est optionnelle et possède les fallbacks du
modèle commun. `mountinfo` est lu par fragments : une étape du scanner effectue au plus
un `sftp_read`, puis rend la main au worker avant le fragment suivant. Le handle est
fermé et le snapshot abandonné lors d'une annulation, d'une déconnexion ou du shutdown.
Le parseur conserve l'identifiant, le parent, la racine et les champs optionnels d'un
montage. Une pile d'overmounts est résolue par les relations `parentId` plutôt que par
l'ordre numérique réutilisable des IDs. Une sous-racine démontrable d'un filesystem
bloc ordinaire peut être regroupée comme alias ; des racines Btrfs distinctes, deux
attaches de même racine et les autres cas ambigus restent navigables.

## Rafraîchissement du stockage

L'en-tête Storage de Places expose une action `view-refresh`. L'ouverture initiale et
le clic utilisent tous deux `MainWindow::refreshStorage()`. Une passe renouvelle les
volumes locaux puis, lorsqu'une session est active, les volumes distants sans
reconnexion ni navigation. Chaque source reste affichée sous sa machine et le contenu
du panneau ouvert est remplacé en place, sans duplication.

Les deux collectes s'exécutent dans leurs workers existants. Une requête déjà en cours
n'est pas réémise ; l'action est réactivée quand les réponses attendues sont revenues.
Sans serveur connecté, seule la collecte locale est déclenchée et aucune erreur SSH
n'est produite. `NavigationTree` synchronise uniquement les enfants de `Volumes` et
`External devices`, en réutilisant les nœuds dont le `rootPath` est stable. L'expansion,
les enfants déjà chargés, la sélection et le panneau actif restent inchangés.
Une métadonnée optionnelle inaccessible produit un résultat conservateur ; une vraie
perte de connexion abandonne au contraire tout le snapshot et suit la gestion normale
de perte de session.

## Profils et serveur actif

`ServerProfileStore` reste l'unique store. `MainWindow` charge une seule collection et
la transmet à Home et à `NavigationTree`; l'ancien `QListWidget` de Places a été
supprimé. Add, Edit, Remove, Connect, Disconnect et la connexion rapide utilisent les
mêmes workflows qu'au Sprint 6.

Un profil hors ligne n'a aucun enfant navigable et ne suggère donc pas que ses fichiers
sont accessibles. L'identité d'une session active est distincte de l'identité éventuelle
du profil sauvegardé. Une connexion rapide non enregistrée possède donc son propre nœud
transitoire avec Home, `/` et ses volumes, sans être persistée. Si le profil associé est
édité ou supprimé pendant la connexion, il redevient un profil hors ligne et la machine
active reste représentée séparément avec les réglages réels de la session. Seule la
machine correspondant exactement à la session reçoit « Connected », une graisse
renforcée, une icône d'état et des dossiers développables.

## Interaction arbre et panneaux

L'activation d'un emplacement navigable ouvre sa `BrowserLocation` dans le panneau
actif. Le champ de chemin affiche une URL `file:` pour le local et une URL `sftp:`
portant l'identité du serveur pour le distant. Chaque panneau conserve sa propre
source, son propre chemin et son propre historique.

En affichage scindé, un panneau peut être local pendant que l'autre reste distant.
Le design n'assigne aucun côté à une source. Les opérations distantes, le drag and drop
interne, le presse-papiers distant, Upload et Download ne sont activés que pour un
panneau SSH de la session courante. Copy/Move vers l'autre panneau exigent en plus que
la destination appartienne explicitement à cette même session. Une destination locale
ne peut donc jamais être transmise par erreur à `SshSession`.

À la déconnexion, seuls les panneaux SSH sont purgés. Un panneau local et son historique
restent utilisables, et le workspace reste visible s'il contient encore une source
locale. La session distante, ses requêtes, transferts et contextes sont toujours
nettoyés comme auparavant.

## Compatibilité multiplateforme

Les chemins locaux sont normalisés avec Qt et affichés avec `QUrl::fromLocalFile`.
`QStandardPaths` fournit les emplacements personnels et `QStorageInfo` les volumes.
Aucune commande Linux (`ls`, `find`, `mount`, `findmnt`, `df`, `lsblk`) ni aucun chemin
de montage ou nom de périphérique comme `/home`, `/mnt` ou `/dev/sdX` n'est codé dans
le produit. Le collecteur distant Linux lit seulement les interfaces noyau génériques
`/proc/self/mountinfo`, `/sys/dev/block` et leurs parents via SFTP. Les chemins SFTP
conservent leur normalisation POSIX propre, séparée de la logique locale.

## Tests

La suite ajoute des fixtures `QTemporaryDir` pour vérifier la lecture immédiate d'un
répertoire local, les métadonnées, l'absence de récursion et les erreurs. Les volumes
sont testés par invariants portables sans dépendre d'un disque précis de la CI. Des
preuves de classification synthétiques couvrent `Internal`, `External`, `System` et le
fallback `Unknown`, dont un stockage externe non amovible et un stockage amovible non
classé arbitrairement externe.
Des tests de régression imposent notamment les chaînes `block → scsi → usb` et
`block → block → [sans subsystem] → scsi → scsi → scsi → usb`; elles doivent produire
`StorageKind::External` sans interpréter le nom du device. Le parseur de mountinfo, une
partition USB, plusieurs niveaux sans subsystem et le cas `removable == false` sont
également couverts. Des fixtures couvrent aussi les champs optionnels, les échappements,
les bind mounts, les sous-racines, les overmounts à IDs réutilisés, les attaches Btrfs
distinctes et les lignes invalides. Les doubles du scanner imposent une seule lecture
de `mountinfo` par étape, des fragments courts, l'annulation en cours de lecture et le
rejet exact d'un fichier dépassant 1 Mio. Les doubles sysfs distinguent absence légitime,
permission refusée, erreur d'I/O et disparition d'un nœud.
La priorité label/modèle/device/montage et la conservation de la classification USB
après enrichissement sont testées séparément.

Les tests de `NavigationTree` couvrent les deux machines racines, l'appartenance des
volumes à la machine locale, la synchronisation des profils, le marqueur actif, le
serveur hors ligne sans enfants, le placeholder paresseux, l'expansion, le repli et le
chargement d'un seul niveau. Ils vérifient aussi le placement interne/externe, l'absence
de branche externe vide, la déduplication entre catégories et la navigation vers la
racine d'un périphérique externe injecté. Ils vérifient maintenant aussi le remplacement
des volumes distants, l'absence de mélange local/distant et la navigation distante.
Les tests de panneau couvrent l'identité
locale et l'historique Back/Forward/Refresh. Les tests de fenêtre couvrent l'ouverture
locale asynchrone sans SSH, un split local/SSH, la désactivation des opérations
incompatibles, la conservation du panneau local après déconnexion, le Refresh sans
serveur, le déclenchement distant et la coalescence des clics.
Ils couvrent aussi le tooltip complet, partiel et hostile, les collisions de noms, la
navigation indépendante du texte visible et la mise à jour ciblée après Refresh. Des
doubles du collecteur distant vérifient les limites, l'annulation, la perte de connexion
sans snapshot partiel et la terminaison bornée. Un scénario de fenêtre couvre la
connexion transitoire complète et le rejet d'un résultat Storage de A après connexion à B.

## Limites et prochaines évolutions

- Les créations, renommages, déplacements et suppressions locales sont différés.
- Les transferts local-local et les copies directes entre panneaux local/SSH ne sont
  pas encore exposés ; Upload et Download conservent leurs dialogues du Sprint 6.
- Le refresh périodique reste réservé aux panneaux SSH ; un panneau local dispose du
  Refresh manuel et n'utilise pas encore `QFileSystemWatcher`.
- L'état développé/replié reste stable pendant l'utilisation, mais n'est pas persisté
  entre deux lancements.
- Le collecteur de volumes distant est actuellement propre aux serveurs Linux qui
  exposent `/proc` et `/sys` via SFTP ; un serveur chrooté ou un autre OS reste sans
  enrichissement distant. Les limites de sécurité peuvent tronquer volontairement la
  découverte d'un serveur exceptionnellement volumineux ; ce cas est signalé ou classé
  `Unknown` plutôt que de monopoliser le worker.
- La détection externe enrichie est actuellement propre à Linux. Les volumes derrière
  device mapper ou une topologie sysfs ambiguë restent volontairement `Unknown` ; une
  API native Windows/macOS pourra compléter le point d'extension ultérieurement.
- `ejectable` reste conservateur et aucune action Mount, Unmount, Eject ou Safely Remove
  n'est exposée.
- Il n'existe toujours qu'une session SSH et aucun transfert serveur-à-serveur.

Les évolutions suivantes pourront généraliser les sélections et opérations sans
affaiblir l'identité de source désormais portée par chaque panneau.
