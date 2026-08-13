# Sprint 8 — Opérations sur les volumes

Date : 12 août 2026

## Correctifs v0.8.1

La release v0.8.1 complète le pipeline de stockage sans modifier ses frontières :

- les montages Btrfs dont `mountinfo` fournit un `major:minor` virtuel `0:*` remontent
  désormais de leur source bloc `/dev/...` vers `/sys/class/block`, puis parcourent
  l'ascendance `/sys/dev/block` existante ; un transport USB porté par le disque parent
  reste donc classé `External` avant et après montage ;
- `overlay` et `binfmt_misc` rejoignent le filtre explicite des pseudo-filesystems,
  après résolution des overmounts et avec l'exception maintenue pour la racine `/` ;
- un déplacement distant conserve le rename SFTP rapide. Si son échec générique est
  confirmé comme une frontière de filesystem par `statvfs` sur les deux répertoires
  parents et si la source n'est pas un point de montage, le worker coopératif réserve
  atomiquement un répertoire temporaire, copie côté serveur avec préservation des
  métadonnées, promeut l'élément par rename SFTP et ne supprime la source qu'après le
  nettoyage du temporaire. Erreurs, annulations et échecs de promotion déclenchent eux
  aussi ce nettoyage ; s'il échoue, le diagnostic conserve l'erreur initiale et le chemin
  temporaire. Une preuve ambiguë ou un changement de point de montage refuse toute
  suppression récursive. Cette dernière vérification couvre la racine et tous les mountpoints
  descendants, bind mounts compris, avant tout `rm`; `--one-file-system` complète sans remplacer
  ce refus préalable. Le code de retour du `cp -a` de staging est transmis explicitement avant
  EOF afin qu'une notification SSH `exit-status` tardive ne transforme pas une copie courte
  réussie — notamment celle d'un lien symbolique — en faux échec.

Les tests utilisent des fixtures `mountinfo`/sysfs et des doubles de backend. Ils couvrent
notamment fichiers, dossiers et liens symboliques, montages sources ou descendants, bind mounts,
faux préfixes de chemins, collision tardive du temporaire, erreurs de copie/promotion/nettoyage et
annulation ; ils ne nécessitent ni disque Btrfs/USB ni serveur SSH réel.

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
système, le point de montage sélectionné, les points de montage observés pour ce
périphérique dans le même instantané et la catégorie du volume. Le nom d'affichage,
le label et le modèle matériel ne servent jamais d'identifiant d'opération.

`VolumeOperationResult` conserve l'identité de la requête, l'opération et le
périphérique, ainsi qu'une erreur structurée : succès, opération non supportée,
authentification requise ou échouée, permission refusée, périphérique introuvable,
volume occupé, outil indisponible ou autre erreur système. Pour une authentification
distante, il porte aussi un jeton de défi opaque propre à la session. Un diagnostic
borné peut être conservé sans journalisation automatique.

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

Lorsqu'il est présent, `udisksctl` est préféré pour le montage et pour le démontage
d'un périphérique qui ne possède qu'un point de montage connu :

```text
udisksctl mount -b <device>
udisksctl unmount -b <device>
```

Cette voie laisse udisks2 appliquer ses règles et son éventuelle autorisation système
pour les volumes utilisateur. Sur un Linux sans udisks2, le service cherche
respectivement `mount` ou `umount` et invoque :

```text
mount -- <device>
umount -- <mountpoint>
```

Lorsqu'un périphérique possède plusieurs attachments, `udisksctl unmount -b` serait
ambigu car son interface accepte un objet ou un block-device, pas un mountpoint. Le
service choisit donc `umount -- <mountpoint sélectionné>` même si `udisksctl` est
installé. Il ne passe jamais `--all-targets`, `-a`, `--recursive`, `--lazy` ou
`--force`. La même règle interdit toute commande basée sur le device lorsque celui-ci
est également observé sur `/`.

L'instantané de l'interface n'autorise jamais à lui seul une commande par device. Il
conserve sans les assainir tous les siblings observés, y compris une entrée invalide ou
dupliquée, afin qu'une incohérence rende l'opération moins permissive. Juste avant tout
démontage, le worker exécute un `lsblk` JSON ciblé sur le device déjà validé et reparse
strictement `PATH,MOUNTPOINTS`. Une erreur de lecture, une réponse ambiguë, un doublon
ou la disparition du mountpoint sélectionné arrête l'opération sans commande
destructive. Seul un état courant composé exactement du mountpoint sélectionné permet
encore `udisksctl unmount -b`; plusieurs attachments courants imposent `umount`.

Ce fallback n'élève jamais les privilèges. Il réussit uniquement si la configuration
du système, notamment `/etc/fstab`, et les droits de l'utilisateur autorisent déjà
l'opération. RemoteFileManager ne modifie pas `fstab`, ne demande pas de mot de passe
sudo et n'écrit rien sur un périphérique bloc.

Le mountpoint doit être absolu, déjà normalisé, différent de `/`, sans NUL ni caractère
de contrôle, et présent parmi les attachments du device dans l'instantané. Les
programmes et les arguments sont transmis séparément à `QProcess`. Aucun shell,
`sh -c` ou texte de commande concaténé n'est utilisé. Les espaces et caractères
spéciaux d'un chemin de périphérique restent donc dans un argument unique ; les
métadonnées de présentation ne sont pas transmises. Une limite de temps empêche aussi
un processus système de rester bloqué indéfiniment.

L'attente locale est découpée en tranches de 20 ms et consulte entre chaque tranche
un indicateur d'annulation atomique. À la fermeture de RFM, le thread graphique peut
donc demander directement l'annulation sans dépendre de la boucle d'événements du
worker occupé. Seul le `QProcess` possédé par l'opération est terminé, puis tué après
un délai de grâce borné si nécessaire. Le signal de résultat est déconnecté avant
l'attente du thread, les verrous de device sont libérés par le retour normal du
service et la fermeture ne reste pas suspendue au timeout de 60 secondes.

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

Dans l'arbre distant, une entrée montée est identifiée par son device et son point de
montage normalisé. Deux attachments de `/dev/sda2` vers des chemins différents restent
donc navigables séparément, tandis qu'un doublon exact est fusionné. Une entrée non
montée reste identifiée par le seul device. Cette identité de présentation ne change
pas le verrou des opérations : montage et démontage restent sérialisés par machine et
par device, et leur état busy apparaît sur toutes les entrées du même bloc.

Les capacités `lsblk`, `udisksctl`, `mount` et `umount` sont détectées par une commande
fixe lors du premier besoin. Elles sont mémorisées uniquement dans l'état de la
session courante. `SshSession::Impl::reset()` efface ce cache, les commandes en attente
et les devices occupés à la déconnexion ; un autre serveur est toujours sondé à
nouveau.

Le montage et le démontage à attachment unique privilégient respectivement :

```text
udisksctl mount -b /dev/... --no-user-interaction
udisksctl unmount -b /dev/... --no-user-interaction
```

L'option `--no-user-interaction` interdit au premier canal SSH d'attendre un dialogue
Polkit. Une autorisation déjà accordée continue donc de fonctionner sans popup. Un
refus définitif, notamment `NotAuthorized`, devient `PermissionDenied`. Seule une
réponse indiquant qu'une autorisation peut être obtenue, notamment
`NotAuthorizedCanObtain`, devient `AuthenticationRequired` et ouvre une boîte Qt
propre à la fenêtre RFM. Les autres erreurs de permission ne demandent jamais le mot
de passe inutilement.

Pour plusieurs attachments d'un même device, le distant emploie à la place
`LC_ALL=C umount -- '<mountpoint sélectionné>'`. Le chemin est validé puis protégé par
le quoting POSIX à apostrophes déjà utilisé pour les commandes distantes ; espaces,
apostrophes et métacaractères restent des données. Aucun fallback global n'est permis.
La session SSH met d'abord en file un `lsblk` ciblé, puis construit la commande
destructive uniquement à partir de cette réponse fraîche. Cette revalidation est
répétée après l'éventuelle saisie Polkit, car la topologie peut avoir changé pendant
le dialogue. Si le device possède alors plusieurs attachments, la session abandonne
la voie interactive, efface immédiatement le `SecurePassword` devenu inutile et met
en file le seul `umount -- '<mountpoint sélectionné>'`, sans nouveau dialogue ni
fallback vers un démontage par device. La disparition du mountpoint ou une topologie
incohérente n'exécute aucune commande destructive.

Après validation de cette boîte, `SshSession` vérifie l'identifiant d'opération et le
jeton de défi. Si la revalidation conserve un seul attachment, elle ouvre un canal SSH
dédié avec PTY et transfère l'unique propriété du `SecurePassword`. Ce PTY est
strictement interne : il n'existe ni terminal graphique, ni shell libre, ni console
utilisateur. La commande interactive appartient toujours à l'ensemble fermé du
backend :

```text
LC_ALL=C udisksctl mount -b /dev/...
LC_ALL=C udisksctl unmount -b /dev/...
```

Elle ne contient volontairement plus `--no-user-interaction`. Une petite machine
d'état bornée reconnaît le prompt anglais `Password:`, la fin ou l'échec de
l'authentification, la fermeture du canal et les timeouts. Elle ignore seulement les
contrôles terminaux nécessaires à cette reconnaissance et n'émule pas un terminal.
Le mot de passe n'est écrit dans le PTY qu'après le prompt attendu. La sortie brute du
PTY, susceptible de contenir le dialogue Polkit, n'est jamais exposée à l'UI.

Le mot de passe est celui saisi explicitement pour cette seule autorisation Polkit ;
le mot de passe SSH initial n'est jamais réutilisé. `SecurePassword` encode directement
la vue UTF-16 dans une allocation privée, non copiable et non implicitement partagée,
sans `QByteArray` intermédiaire. Un événement Qt dédié reçoit cette allocation par
move et en transfère l'unique propriété au thread SSH, solution compatible avec le
minimum Qt 6.4. Les écritures PTY partielles conservent le buffer uniquement jusqu'au
dernier octet ; sa capacité entière est alors écrasée par des écritures volatiles. Le
même effacement est exécuté par `clear()` et le destructeur, donc aussi lors d'une
erreur, d'un timeout, d'une déconnexion, d'un reset ou de la destruction d'un événement
encore en attente. Le secret n'est ajouté ni au profil serveur, ni aux paramètres, ni
à l'historique, ni aux diagnostics ou logs.

Cette garantie couvre les allocations possédées par `SecurePassword`. Qt peut garder
des représentations internes du texte dans `QLineEdit`, l'input method ou ses
allocateurs, que l'application ne peut ni localiser ni certifier écrasées. RFM vide le
widget immédiatement, limite l'extraction à un seul `QString`, tente d'écraser cette
représentation après que le widget l'a libérée, et ne crée aucun autre `QString` ou
`QByteArray` du secret. La mémoire virtuelle, le swap et les copies réalisées hors du
contrôle du processus ne peuvent pas recevoir une garantie absolue. La persistance,
un keyring et une gestion avancée des secrets restent hors périmètre du Sprint 8.

Pendant la boîte, le verrou du device et l'état `Mounting…` ou `Unmounting…` restent
actifs. `Cancel` abandonne le défi, libère ce verrou et ne déclenche aucun refresh de
succès. Un mauvais mot de passe termine la tentative avec `AuthenticationFailed` ;
l'utilisateur peut ensuite relancer explicitement l'opération, sans boucle de retry
automatique. Le délai de 60 secondes couvre l'attente du prompt et la période après
son envoi. Une perte SSH ferme la boîte ou le canal et rend le résultat obsolète. Les
jetons ne sont pas réutilisés lors d'un reset : une réponse provenant d'une ancienne
boîte ne peut pas atteindre une nouvelle session.

Si `udisksctl` est disponible mais refuse une opération non ambiguë, aucune autre
stratégie n'est lancée automatiquement. `mount -- /dev/...` est choisi uniquement
lorsque `udisksctl` est absent. `umount -- '<mountpoint>'` est choisi lorsque
`udisksctl` est absent ou lorsque le snapshot montre plusieurs attachments pour le
device, à condition que `umount` ait été détecté.

La voie ciblée `umount` ne traverse pas UDisks et ne bénéficie donc pas de son dialogue
Polkit. Si les droits ordinaires du compte SSH ne suffisent pas, l'opération retourne
`PermissionDenied`; RFM ne remplace jamais cette limite par `sudo`, `su`, `pkexec` ou
un stockage de credentials. La boîte d'authentification, le PTY, les retries explicites
et `SecurePassword` restent inchangés pour les opérations représentables sans ambiguïté
par UDisks.

Le device doit être un chemin Linux normalisé sous `/dev/` composé exclusivement de
caractères sûrs. Labels, modèles et textes UI ne sont jamais interpolés. Il n'existe
aucune API de commande libre côté UI, et aucune commande ne contient `sudo`, `su`,
mot de passe ou modification de `fstab`. Les canaux sont lus de façon bornée et
coopérative avec un délai maximal de 60 secondes ; à expiration, seul le canal de
commande est fermé. Leurs délais, pertes de connexion et codes de sortie sont
convertis en résultats structurés sans afficher directement
stderr à l'utilisateur.

Les lectures libssh restant non bloquantes, leur polling rend toujours la main à la
boucle d'événements Qt. Lorsqu'aucune donnée ni terminaison n'est disponible, un seul
prochain poll est armé à 10 ms ; lorsqu'une sortie vient d'être reçue, un poll à 0 ms
permet de la drainer sans latence perceptible. Une génération invalide les timers
anciens après reset. Le canal de commande ordinaire et le PTY Polkit utilisent la
même politique, tout en conservant le timeout mural de 60 secondes et la détection de
perte SSH à chaque poll.

À la frontière du PTY, les erreurs métier reconnues par la machine d'état
(`AuthenticationFailed`, `PermissionDenied`, `VolumeBusy` et `DeviceNotFound`) sont
transportées sous forme d'enum jusqu'au `VolumeOperationResult`. Elles ne sont ni
reformulées puis reparsées, ni marquées comme crash. Les erreurs libssh, d'écriture ou
de canal restent au contraire des erreurs techniques ; les timeouts et pertes de
connexion gardent leurs classifications dédiées.

Après l'envoi complet du secret, la machine d'état attend explicitement le résultat
d'authentification. Tout nouveau prompt `Password:` — y compris fragmenté entre
plusieurs lectures PTY — termine immédiatement la tentative avec
`AuthenticationFailed`; RFM n'attend pas le timeout et ne relance pas implicitement
le dialogue. Le premier prompt reste normal et un succès Polkit poursuit l'opération.

Sur un serveur headless, l'authentification interactive dépend de la capacité de
Polkit/udisks2 à fournir son dialogue dans le PTY SSH. Un compte auquel Polkit oppose
un refus définitif reçoit une erreur de permission sans popup. RemoteFileManager
n'utilise jamais `sudo`, `sudo -S`, `su`, une redirection de mot de passe, ni une
modification de Polkit, sudoers ou `fstab`. Sur un serveur minimal refusant
l'exécution de commandes SSH, la navigation SFTP et la découverte des montages
restent possibles, mais les volumes non montés et les opérations ne le sont pas.

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
et le refus d'opérations concurrentes sur un même périphérique. Ils vérifient aussi le
ciblage d'un seul attachment, les devices également attachés sur `/`, les mountpoints
avec espaces ou apostrophes, le rejet des chemins incohérents et l'absence de toute
option globale de démontage. La suite existante de
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

Les tests d'authentification ajoutent la distinction entre `NotAuthorizedCanObtain`
et `NotAuthorized`, les commandes interactives mount/unmount sans option non
interactive ni élévation, le protocole Polkit fragmenté et avec contrôles ANSI, le
prompt avant envoi, le succès, le mauvais mot de passe et les timeouts avant/après
prompt. Les tests UI vérifient la modalité fenêtre, le champ masqué, le serveur et le
device, l'annulation sans refresh, la soumission unique du secret, le maintien de
l'état occupé et la fermeture sûre à la déconnexion. Aucun test ne contacte un
serveur ni ne manipule un vrai volume ou mot de passe.

Les limitations volontaires de cette étape sont :

- le choix ou la création d'un point de montage personnalisé n'est pas pris en charge ;
- les opérations de volumes distants ciblent uniquement des serveurs Linux ;
- le fallback de déplacement inter-filesystems dépend de `cp -a` et de `mountinfo` sur le
  serveur Linux ; la préservation des propriétaires, ACL, attributs étendus et capabilities
  reste limitée par le filesystem et les droits du compte SSH, et les liens physiques entre
  plusieurs éléments sélectionnés indépendamment ne peuvent pas être reconstruits ;
- un `lsblk` trop ancien ne comprenant pas `MOUNTPOINTS` retombe sur les seuls montages
  observés plutôt que d'essayer une commande moins déterministe ;
- le probe automatique léger reste fondé sur `mountinfo` : l'apparition d'un device
  distant non monté nécessite `Refresh storage`, tandis que tout succès de commande
  déclenche déjà un refresh complet ;
- l'éjection physique et l'annulation explicite restent hors périmètre.
- RFM ne conserve pas le secret Polkit et ne peut donc pas réauthentifier une seconde
  opération sans une nouvelle saisie ; keyring, cache de session et gestion avancée
  des secrets restent hors périmètre ;
- la compatibilité réelle du dialogue texte Polkit dans un PTY dépend de la pile
  udisks2/Polkit du serveur et doit être validée manuellement sur chaque famille de
  serveur prise en charge.

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

### Cas Polkit prévu sur `serveur-keur`

Cette procédure est documentaire et ne doit pas être lancée automatiquement dans
l'environnement de développement :

1. confirmer hors RFM que `/dev/sdc1` est démonté et qu'il s'agit bien du volume de
   test ;
2. vérifier que `LC_ALL=C udisksctl mount -b /dev/sdc1 --no-user-interaction` renvoie
   `NotAuthorizedCanObtain` ;
3. dans RFM, se connecter à `serveur-keur`, rafraîchir les volumes, sélectionner
   `/dev/sdc1`, puis cliquer sur `Mount` ;
4. vérifier que `Mounting…` reste affiché et que la boîte `Authentication required`
   indique le serveur, l'action mount et `/dev/sdc1`, avec un champ masqué ;
5. tester une fois `Cancel` : aucune seconde commande, aucun refresh de succès et
   retour immédiat de l'action `Mount` ;
6. relancer, saisir le mot de passe Polkit de test et valider avec Entrée ; vérifier
   qu'aucun terminal ni texte `Password:` n'apparaît ;
7. attendre le refresh et confirmer le mountpoint observé
   `/run/media/gabriel/CTA`, puis `Open` ;
8. répéter avec un mauvais mot de passe et vérifier l'erreur claire ainsi que la
   libération de l'état occupé ;
9. tester `Unmount` de la même façon si le serveur demande une authentification, puis
   vérifier le `SafetyFallback` des panneaux ouverts sous le mountpoint ;
10. couper enfin SSH pendant une boîte puis pendant une tentative PTY et vérifier
    l'absence de crash, de popup persistante et de secret réutilisé après reconnexion.

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
