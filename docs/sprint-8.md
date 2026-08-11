# Sprint 8 — Opérations sur les volumes

Date : 11 août 2026

## Socle des opérations

Le Sprint introduit le socle métier du montage et du démontage des volumes locaux et
distants sous Linux. La détection reste indépendante des opérations système :
`StorageVolume` décrit un instantané observé, `LocalFileSystem` l'énumère et
`VolumeService` exécute une intention explicite sur l'identifiant système du volume.

Une opération réussie ne modifie donc jamais artificiellement l'instantané courant.
Le branchement UI demande donc un nouveau refresh des volumes et affiche uniquement
le nouvel état observé par le système.

## Architecture retenue

`VolumeService` est le contrat indépendant de la plateforme et de l'interface. Il
reçoit une `VolumeOperationRequest`, comprenant un identifiant de requête, l'opération
et un `VolumeOperationTarget`. La cible distingue explicitement le périphérique
système, le point de montage observé et la catégorie du volume. Le nom d'affichage,
le label et le modèle matériel ne servent jamais d'identifiant d'opération.

`VolumeOperationResult` conserve l'identité de la requête, l'opération et le
périphérique, ainsi qu'une erreur structurée : succès, opération non supportée,
permission refusée, périphérique introuvable, volume occupé, outil indisponible ou
autre erreur système. Un diagnostic borné peut être conservé sans journalisation
automatique.

`LocalLinuxVolumeService` fournit la première implémentation. L'exécution de processus
est isolée derrière `VolumeCommandRunner`, ce qui rend la sélection de stratégie et
les arguments entièrement testables sans monter de volume. `VolumeOperationWorker`
est le petit adaptateur `QObject` exécuté sur un `QThread` dédié. Le service est
synchrone par contrat ; tout appel provenant de l'UI passe par ce worker et son signal
de résultat pour ne jamais bloquer le thread graphique ni les lectures locales.

`RemoteLinuxVolumeService` réutilise les mêmes requêtes, résultats, protections et
erreurs, mais produit un ensemble fermé de commandes destiné à la session SSH. Il
n'est volontairement pas exécuté par le worker local synchrone : `SshSession` reste
l'unique propriétaire de libssh et pilote son canal de commande par étapes dans son
thread existant. Les implémentations macOS et Windows restent hors périmètre.

## Stratégie Linux locale

Lorsqu'il est présent, `udisksctl` est préféré :

```text
udisksctl mount -b <device>
udisksctl unmount -b <device>
```

Cette voie laisse udisks2 appliquer ses règles et son éventuelle autorisation système
pour les volumes utilisateur. Sur un Linux sans udisks2, le service cherche
respectivement `mount` ou `umount` et invoque :

```text
mount -- <device>
umount -- <device>
```

Ce fallback n'élève jamais les privilèges. Il réussit uniquement si la configuration
du système, notamment `/etc/fstab`, et les droits de l'utilisateur autorisent déjà
l'opération. RemoteFileManager ne modifie pas `fstab`, ne demande pas de mot de passe
sudo et n'écrit rien sur un périphérique bloc.

Les programmes et les arguments sont transmis séparément à `QProcess`. Aucun shell,
`sh -c` ou texte de commande concaténé n'est utilisé. Les espaces et caractères
spéciaux d'un chemin de périphérique restent donc dans un argument unique ; les
métadonnées de présentation ne sont pas transmises. Une limite de temps empêche aussi
un processus système de rester bloqué indéfiniment.

## Protections et erreurs

- le démontage de `/` et de tout volume classé `System` est rejeté avant toute
  exécution ;
- seules les identités nettoyées situées sous `/dev/` sont acceptées ;
- un verrou logique refuse une deuxième opération simultanée sur le même périphérique ;
- les diagnostics usuels de permission, volume occupé et périphérique disparu sont
  convertis en erreurs structurées ;
- aucun secret n'est collecté, fourni sur l'entrée standard ou journalisé ;
- aucune opération de formatage, partitionnement, réparation, éjection ou écriture
  directe sur bloc n'est présente.

## Découverte des volumes non montés

`QStorageInfo::mountedVolumes()` reste la source de vérité pour les montages actifs et
leurs véritables points de montage. Sous Linux, cet instantané est désormais complété
par une lecture JSON de `lsblk`, lancée sans shell et sans privilège avec une liste de
colonnes fixe. Cette source fournit le device, son parent, son type, le filesystem, le
label, la taille, le point de montage éventuel, le transport et les indicateurs RO/RM.

Le parser JSON et la fusion des block devices sont désormais communs au local et au
distant. Les deux sources locales sont fusionnées dans
`LocalFileSystem::makeStorageSnapshot`. Une
entrée déjà montée gagne toujours face à l'entrée bloc correspondante. Un objet non
monté n'est ajouté que s'il possède un filesystem directement navigable. `swap`, les
conteneurs LUKS, les membres LVM/RAID/ZFS, `zram`, les loop devices et les blocs sans
filesystem sont exclus. Lorsqu'un disque parent et une partition enfant portent tous
deux une signature navigable, la partition est conservée et le parent redondant est
écarté.

`StorageVolume::mounted` représente explicitement l'état. `rootPath` reste uniquement
le point de montage réellement observé et n'est jamais détourné en indicateur. La
classification existante est réutilisée et enrichie avec la topologie sysfs ; la
pipeline mountinfo distante reste inchangée : résolution des overmounts, filtrage des
pseudo-filesystems sauf `/`, puis déduplication et classification.

Si `lsblk` est absent, trop ancien ou échoue, les volumes montés continuent d'être
affichés via Qt ou `mountinfo` distant, mais les périphériques non montés ne peuvent
pas être découverts.

## Backend Linux distant

La découverte distante conserve le scanner SFTP coopératif de
`/proc/self/mountinfo`, notamment sa résolution des overmounts, son filtrage et son
enrichissement sysfs. Avant ce scan, `SshSession` exécute, si disponible, un `lsblk`
JSON à colonnes fixes. Sa sortie passe par le même parser que la source locale, puis
les périphériques non montés sont fusionnés avec les montages observés. Une panne ou
une absence de `lsblk` n'invalide donc ni la session ni les volumes déjà montés.

Les capacités `lsblk`, `udisksctl`, `mount` et `umount` sont détectées par une commande
fixe lors du premier besoin. Elles sont mémorisées uniquement dans l'état de la
session courante. `SshSession::Impl::reset()` efface ce cache, les commandes en attente
et les devices occupés à la déconnexion ; un autre serveur est toujours sondé à
nouveau.

Le montage et le démontage privilégient respectivement :

```text
udisksctl mount -b /dev/... --no-user-interaction
udisksctl unmount -b /dev/... --no-user-interaction
```

L'option `--no-user-interaction` interdit au canal SSH d'attendre un dialogue Polkit.
Une autorisation déjà accordée continue de fonctionner ; une authentification requise
ou refusée devient immédiatement une erreur `PermissionDenied` présentée sans stderr
brut. Si `udisksctl` est disponible mais refuse l'opération, aucune autre stratégie
n'est lancée automatiquement. `mount -- /dev/...` ou `umount -- /dev/...` est choisi
uniquement lorsque `udisksctl` est absent et que l'outil correspondant a été détecté.

Le device doit être un chemin Linux normalisé sous `/dev/` composé exclusivement de
caractères sûrs. Labels, modèles et textes UI ne sont jamais interpolés. Il n'existe
aucune API de commande libre côté UI, et aucune commande ne contient `sudo`, `su`,
mot de passe ou modification de `fstab`. Les canaux sont lus de façon bornée et
coopérative avec un délai maximal de 60 secondes ; à expiration, seul le canal de
commande est fermé. Leurs délais, pertes de connexion et codes de sortie sont
convertis en résultats structurés sans afficher directement
stderr à l'utilisateur.

Sur un serveur headless, un compte sans règle udisks/polkit ni entrée `fstab` adaptée
recevra normalement une erreur de permission. RemoteFileManager ne tente aucune
élévation. Sur un serveur minimal refusant l'exécution de commandes SSH, la navigation
SFTP et la découverte des montages restent possibles, mais les volumes non montés et
les opérations ne le sont pas.

## Intégration dans le panneau Volumes

Le panneau conserve une seule arborescence légère. Lorsqu'une ligne de volume local
est sélectionnée, une rangée compacte propose seulement les actions pertinentes :

- `Mount` pour un filesystem disponible mais non monté ;
- `Open` et `Unmount` pour un volume monté et démontable ;
- `Open` seul pour `/` et les volumes classés `System`.

Une ligne en cours d'opération affiche `Mounting…` ou `Unmounting…` et seules ses
actions sont désactivées. Les autres volumes et le reste de l'application restent
utilisables. L'identité asynchrone est le chemin de device copié dans la requête ;
aucun pointeur vers une entrée de modèle remplaçable n'est conservé.

En cas de succès, l'état occupé reste visible jusqu'à une nouvelle énumération locale.
Un instantané qui était déjà en vol avant la fin de la commande est ignoré et suivi
d'une seconde requête, afin de garantir l'ordre : commande, puis observation système,
puis mise à jour UI. Le point de montage n'est jamais inventé. `Open` utilise celui du
nouvel instantané et navigue dans le panneau de fichiers actif selon les conventions
existantes.

Les erreurs structurées sont traduites en messages utilisateur pour les permissions,
le volume occupé, le périphérique disparu, l'outillage absent et les autres erreurs
système. Le stderr technique n'est pas affiché. Un périphérique disparu déclenche en
plus un refresh afin de supprimer sa ligne obsolète.

Après un démontage local réussi, chaque panneau visible dont la source est `Local` et
dont le chemin courant est le mountpoint ou l'un de ses descendants est évacué vers le
home local avant le refresh des volumes. La comparaison normalise les chemins et exige
une frontière de composant : `/mnt/disk2` n'appartient pas à `/mnt/disk`. Un panneau
SSH ayant le même texte de chemin n'est jamais concerné. Si le home appartient lui-même
au volume démonté, `/` sert de dernier repli.

Cette navigation de sécurité ne place pas le chemin démonté dans Back. Elle retire des
historiques Back et Forward uniquement les destinations locales appartenant au volume,
conserve les autres destinations sûres et vide Forward lors de l'arrivée au repli. Un
échec de démontage ne modifie ni le panneau ni son historique.

Le même mécanisme s'applique au distant avec `RemotePath`. Après réussite seulement,
les panneaux SSH de la connexion concernée situés sur le mountpoint ou un descendant
sont envoyés vers le home distant mémorisé à la connexion, ou `/` si ce home est
indisponible ou appartient au volume. Les panneaux locaux, ceux d'une autre identité
SSH et les préfixes voisins restent intacts. Le nettoyage Back/Forward est limité à la
même source et au même identifiant de machine.

## Tests et limites actuelles

Les tests injectent un faux exécuteur et couvrent le refus de la racine et des volumes
système, le périphérique disparu, le mapping des erreurs, la priorité de `udisksctl`,
les fallbacks `mount`/`umount`, l'absence d'outil, la construction sûre des arguments
et le refus d'opérations concurrentes sur un même périphérique. La suite existante de
découverte locale continue de couvrir les overmounts, les bind mounts et le filtrage
des pseudo-filesystems, dont l'exception de la racine.

La seconde étape ajoute des fixtures `lsblk` sans accès matériel et des tests UI pour
les états monté/non monté, la suppression des parents et du swap, les actions
contextuelles, les états occupés, le refresh après succès, l'absence de mutation
optimiste, les messages d'erreur, la disparition d'un périphérique et l'ouverture à
partir du point de montage ré-observé.

Les tests distants couvrent le parser partagé, le fallback `mountinfo`, les volumes
montés/non montés, parents/partitions, swap, loop et racine, le choix des outils, les
erreurs structurées, le rejet des devices hostiles, l'absence de sudo, le refresh sans
mutation optimiste, l'ouverture du mountpoint ré-observé, les pertes de session aux
frontières de l'opération, ainsi que le `SafetyFallback` multi-panneau et isolé par
machine. Ils utilisent exclusivement des sorties et résultats simulés.

Les limitations volontaires de cette étape sont :

- le choix ou la création d'un point de montage personnalisé n'est pas pris en charge ;
- les opérations de volumes distants ciblent uniquement des serveurs Linux ;
- un `lsblk` trop ancien ne comprenant pas `MOUNTPOINTS` retombe sur les seuls montages
  observés plutôt que d'essayer une commande moins déterministe ;
- le probe automatique léger reste fondé sur `mountinfo` : l'apparition d'un device
  distant non monté nécessite `Refresh storage`, tandis que tout succès de commande
  déclenche déjà un refresh complet ;
- l'éjection physique et l'annulation explicite restent hors périmètre.

## Validation manuelle sur un serveur Linux

1. se connecter à un compte SSH de test et vérifier que Home, `/` et les volumes
   montés restent navigables ;
2. comparer `Refresh storage` avec `lsblk -o
   NAME,PATH,PKNAME,TYPE,FSTYPE,LABEL,SIZE,MOUNTPOINTS,TRAN,MODEL,RO,RM` sur le serveur ;
3. sélectionner un device non monté et vérifier que `Mount` apparaît sous le serveur,
   jamais sous `This Computer` ;
4. cliquer sur `Mount`, observer `Mounting…`, attendre le refresh puis utiliser `Open`
   et comparer le chemin au mountpoint réellement retourné par `lsblk` ;
5. ouvrir ce volume dans un ou deux panneaux, y compris un sous-dossier, puis cliquer
   sur `Unmount` et vérifier leur retour au home distant sans possibilité de revenir
   dans le volume avec Back ;
6. conserver en parallèle un panneau local ou d'une autre connexion au même chemin
   textuel et vérifier qu'il ne bouge pas ;
7. provoquer un volume occupé et un refus de permission avec un compte non privilégié,
   puis vérifier les messages sans suggestion ni demande sudo ;
8. couper la connexion avant une opération, pendant celle-ci puis après la commande
   avant le refresh, et vérifier l'absence de mutation optimiste et d'état occupé
   persistant ;
9. sur un serveur sans `lsblk`, vérifier que les montages de `mountinfo` restent
   visibles ; sur un serveur sans outil de montage, vérifier l'erreur dédiée ;
10. confirmer qu'aucune commande de formatage, partitionnement, `sudo` ou modification
    de `fstab` n'est exécutée.

## Validation manuelle avec une clé USB

Cette procédure ne doit être exécutée que sur une clé de test dont les données sont
sauvegardées. Elle ne requiert aucune commande privilégiée dans RemoteFileManager :

1. lancer l'application et identifier précisément la clé avec `lsblk -o
   NAME,PATH,TRAN,RM,SIZE,FSTYPE,LABEL,MOUNTPOINTS` ;
2. si la partition est déjà montée, la démonter avec les outils habituels du bureau,
   puis cliquer sur `Refresh storage` dans RFM ;
3. sélectionner la partition `/dev/...` non montée dans `External devices` et vérifier
   que seule l'action `Mount` apparaît ;
4. cliquer sur `Mount`, vérifier l'état `Mounting…`, puis attendre le refresh
   automatique ;
5. vérifier que `Open` et `Unmount` apparaissent, cliquer sur `Open` et confirmer que
   le panneau actif navigue vers le point indiqué par `lsblk` ;
6. fermer tout fichier ou terminal utilisant la clé, cliquer sur `Unmount`, puis
   vérifier après refresh que la ligne redevient non montée ;
7. pour le cas occupé, ouvrir un terminal dans le point de montage, tenter `Unmount`
   et vérifier qu'un message compréhensible est affiché sans bloquer l'application ;
8. ne jamais retirer physiquement la clé avant que le système confirme le démontage.
