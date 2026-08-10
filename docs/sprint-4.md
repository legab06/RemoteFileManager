# Sprint 4 — Navigation scindée et opérations entre panneaux

Date : 10 août 2026  
Version : `0.5.0`

## Objectif

Introduire un affichage optionnel à deux panneaux sans imposer le paradigme
« local à gauche, serveur à droite ». RemoteFileManager reste en priorité un
gestionnaire de fichiers distants : pendant ce sprint, chaque panneau représente
un emplacement de la connexion SSH/SFTP active.

Chaque panneau possède son propre chemin, sa sélection, son historique de
navigation et son état d’actualisation. Les copies et déplacements utilisent
naturellement l’autre panneau comme destination lorsqu’il est visible. Lorsque
les deux emplacements appartiennent à la même connexion, les opérations restent
exécutées côté serveur et les données ne transitent pas par le client.

L’architecture doit préparer des sources locales, des onglets et plusieurs
connexions sans les implémenter ni introduire d’abstraction générique prématurée.
Les besoins réels de chaque étape guident les extractions.

## Périmètre

- extraction du navigateur distant de `MainWindow` vers un panneau réutilisable ;
- affichage simple par défaut et affichage scindé optionnel ;
- chemin, sélection, historique arrière/avant, scroll et refresh indépendants ;
- identification visuelle et fonctionnelle du panneau actif ;
- copie et déplacement de la sélection active vers l’autre panneau ;
- dialogue de destination conservé lorsque l’autre panneau n’est pas disponible ;
- routage fiable des résultats asynchrones vers le panneau demandeur ;
- actualisation ciblée de tous les panneaux affectés ;
- évolution du panneau de transferts vers une vue d’opérations ;
- suivi des uploads, downloads, copies et déplacements ;
- historique persistant, supprimable individuellement ou complètement ;
- maintien intégral des transferts et opérations du Sprint 3.

L’historique persistant est la dernière étape du sprint. Il ne doit pas dégrader
la qualité du double panneau ou du gestionnaire d’opérations si le périmètre doit
être réévalué.

## Principes de conception

Un panneau représente une source et un emplacement. Sa position gauche ou droite
n’a aucune signification métier. Les actions ciblent toujours le panneau actif et
utilisent l’autre panneau visible comme destination seulement lorsque l’opération
est valide.

Le Sprint 4 conserve initialement une seule connexion SSH active et un seul
worker. Les deux panneaux distants partagent cette connexion. L’identité de la
connexion devra être rendue explicite au moment où plusieurs connexions seront
réellement introduites, sans construire dès maintenant cette infrastructure.

## Architecture des panneaux

`FileBrowserPane` contient la présentation et l’état propre d’un navigateur :

- emplacement courant et affichage du chemin ;
- table des entrées et sélection multiple ;
- navigation vers un enfant ou le parent ;
- restauration de sélection et de position de défilement ;
- interactions de refresh et de menu contextuel ;
- exposition de la sélection distante sous forme métier.

Le panneau ne possède aucune session SSH, ne manipule aucun type libssh et
n’effectue aucune opération réseau. Il émet des intentions et applique des
résultats fournis par `MainWindow`.

`MainWindow` reste responsable de la coque, de la connexion active, des actions
globales et du routage entre panneaux et worker. La logique d’affichage des
fichiers ne doit plus y être dupliquée.

`PaneWorkspace` compose désormais un ou deux `FileBrowserPane` dans un
`QSplitter`. Le premier panneau existe dès le démarrage et le second est créé au
premier affichage scindé. Lorsque le split est fermé, le panneau non actif est
simplement masqué : le panneau actif reste l’unique panneau visible, quelle que
soit sa position. Les identifiants et les états des deux panneaux sont ainsi
conservés lors d’une réouverture. Le composant ne connaît ni session SSH ni
opération distante.

Chaque panneau affiche sa propre barre de chemin. Le panneau actif est déterminé
par les interactions de focus ou de souris et signalé par une bordure utilisant
le rôle `QPalette::Highlight`, complétée par un accent sur la barre de chemin,
sans couleur codée en dur. Une faible proportion de `Highlight` est également
mélangée à `Window` et `Base` pour teinter discrètement le conteneur et la barre
de chemin actifs tout en conservant la palette normale de la table.

## Routage des listings

Le contrat de listing est corrélé par un identifiant stable attribué à chaque
demande. `MainWindow` conserve l’identifiant actif et la demande la plus récente
attendue par panneau. Le worker retourne cet identifiant avec le chemin, les
entrées ou une erreur localisée. Une nouvelle intention d’un panneau reçoit son
identifiant immédiatement et rend obsolète sa réponse précédente.

Une réponse obsolète n’est pas appliquée à un panneau ayant déjà demandé un autre
emplacement. Deux demandes portant sur le même chemin restent distinguables par
leur identifiant. Un échec d’ouverture de dossier est associé à la demande et au
chemin concernés sans fermer automatiquement une session SSH encore valide. Une
absence de session, une déconnexion SSH ou une erreur SFTP signalant une perte de
connexion restent des erreurs fatales et conservent le traitement existant.

Un seul listing réseau reste actif à la fois sur la session. Une petite FIFO dans
`MainWindow` associe chaque demande à l’identifiant de son panneau. Les demandes
automatiques déjà pendantes sont ignorées et les nouvelles intentions d’un même
panneau remplacent ses demandes encore en file, sans supprimer celles de l’autre
panneau. Les refreshs automatiques et événementiels sont coalescés par panneau.

Les chemins métier sont normalisés dès leur entrée dans un `FileBrowserPane`.
Ils conservent explicitement leur convention : soit absolus POSIX (préfixés par
`/`), soit relatifs au home SSH. Une opération inter-panneaux refuse de mélanger
ces deux conventions. L’URL affichée représente un chemin relatif sous `/~/` et
un chemin absolu directement sous l’autorité SFTP ; elle n’ajoute donc plus un
slash textuel devant un chemin qui en possède déjà un.

## Panneau actif et commandes

Le panneau actif est déterminé par le focus ou une interaction explicite et reste
identifié par un accent visuel accessible. Les actions de navigation, création,
renommage, suppression, upload et download agissent sur ce panneau.

En mode scindé, des commandes explicites copient ou déplacent la sélection du
panneau actif vers le dossier affiché par l’autre panneau. L’interface présentera
la source et la destination avant une opération sensible. Elle ne déduira jamais
la direction à partir des notions de gauche et de droite.

Le panneau source est capturé par son identifiant avant toute confirmation et le
panneau destination est résolu relativement à cet identifiant. Les deux chemins
sont ensuite figés pour l’opération. Lorsque les deux panneaux affichent le même
dossier, ces commandes sont désactivées afin de ne pas envoyer une opération dont
la source et la destination finales seraient réellement identiques.

Sans second panneau, les commandes existantes demandant un chemin distant
resteront disponibles afin de préserver le comportement du Sprint 3.

## Opérations entre panneaux

Pour deux panneaux de la même connexion distante :

- le déplacement utilise `sftp_rename` ;
- la copie utilise la primitive serveur existante fondée sur `cp` ;
- les collisions restent refusées ;
- les chemins sont normalisés et traités comme des données ;
- une copie ou un déplacement dans soi-même est refusé ;
- les résultats partiels restent associés à chaque élément ;
- aucun transfert client intermédiaire n’est introduit.

Après une copie, tous les panneaux affichant la destination sont actualisés.
Après un déplacement, les panneaux affichant la source ou la destination sont
actualisés. La sélection des éléments créés est restaurée dans la destination
lorsque cela est possible.

Une copie serveur ne fournit pas de progression précise et n’est pas annoncée
comme annulable pendant ce sprint. Elle s’exécute dans le worker et ne bloque
jamais le thread Qt. Une refonte générale des opérations distantes en jobs
incrémentaux reste hors périmètre.

## Gestionnaire d’opérations

La vue inférieure évolue de « Transferts » vers « Opérations ». Elle suit
principalement :

- upload ;
- download ;
- copie distante ;
- déplacement distant.

Les états communs sont « En attente », « En cours », « Terminé » et « Erreur ».
L’état « Annulé » est conservé pour les transferts afin de ne pas perdre le
comportement du Sprint 3. La progression, la vitesse, la pause, la reprise et
l’annulation restent disponibles uniquement lorsque le moteur sous-jacent les
prend en charge.

Création de dossier, renommage et suppression ne sont pas ajoutés à cette vue
principale. Leur résultat reste présenté dans le contexte de l’action afin de ne
pas encombrer l’historique.

Le modèle d’affichage commun `OperationProgress` ne remplace pas la FIFO et les
jobs du Sprint 3. Il agrège leurs événements avec ceux des copies et déplacements.
`TransferDirection` reste limité aux uploads et downloads ; un `OperationKind`
distinct décrit les quatre catégories présentées. `OperationState` adapte les
états détaillés des transferts et fournit les états communs aux opérations
distantes.

`OperationPanel`, qui remplace `TransferPanel`, ne reçoit que ce modèle commun.
Les transferts conservent leur barre de progression réelle, leur vitesse et leurs
commandes Pause/Reprise/Annuler. Les copies et déplacements affichent leurs
sources, leur destination et leur état sans pourcentage, vitesse ou commande
fictive. Un résultat partiel indique le nombre d’éléments terminés et détaille
les éléments en erreur.

## Historique persistant

`OperationHistoryStore`, indépendant de l’UI, conserve les opérations terminales
dans `operation-history.json` sous `QStandardPaths::AppDataLocation`. Le document
JSON porte explicitement la version `1` et les identifiants ainsi que les compteurs
64 bits sont encodés comme chaînes décimales pour éviter toute perte de précision.
`QSaveFile` assure une écriture atomique.

Le schéma limite chaque entrée aux champs utiles : identifiant, type d’opération,
identité non secrète du serveur (hôte et port), sources, destination, état terminal,
erreur éventuelle, octets transférés, compteurs de résultat et date de terminaison
UTC. Il ne sérialise ni nom d’utilisateur, ni état interne du moteur, ni vitesse
instantanée, ni capacité d’action, ni donnée d’authentification ou commande SSH.
Le serveur est rappelé discrètement dans l’infobulle de la catégorie d’opération,
sans ajouter de colonne à la table.

Seuls les états `Completed`, `Failed` et `Cancelled` sont enregistrés et acceptés
au chargement. Un fichier absent ou vide représente un historique vide. Un JSON
corrompu, trop volumineux ou d’une version inconnue est ignoré sans empêcher le
démarrage. Les 200 opérations terminales les plus récentes sont conservées,
ordonnées par leur date de terminaison ; cette limite borne durablement le fichier.

`MainWindow` agrège les changements terminaux et diffère l’écriture de 300 ms afin
de regrouper les mises à jour rapprochées. Une écriture encore planifiée est
finalisée à la fermeture. `OperationPanel` reste sans accès disque : il expose une
suppression de la ligne terminale sélectionnée et un nettoyage de toutes les
lignes terminales. Ces commandes ne retirent jamais une opération active.

## Sécurité et fiabilité

- aucune opération réseau lente sur le thread UI ;
- aucun handle ou type libssh exposé aux panneaux ;
- vérification `known_hosts` inchangée ;
- aucune élévation de privilèges ;
- aucune concaténation naïve de chemin dans une commande ;
- contrôle explicite de la connexion avant une future opération multipanneau ;
- refus des collisions et des destinations invalides ;
- erreurs associées à l’opération, au serveur et aux chemins concernés ;
- aucune donnée d’authentification dans l’historique ;
- conservation du nettoyage coopératif des transferts à la fermeture.

## Tests

### Panneau de navigation

- affichage du chemin et des entrées ;
- sélection multiple ;
- navigation vers un enfant et le parent ;
- restauration de sélection et de scroll après refresh du même dossier ;
- émission des intentions de refresh et de menu contextuel ;
- absence de dépendance à `SshSession` ou libssh.

### Routage asynchrone

- résultat livré au panneau demandeur ;
- deux requêtes portant sur le même chemin ;
- réponse obsolète ignorée ;
- refreshs différés et coalescés ;
- erreur de listing localisée sans déconnexion globale.

### Opérations entre panneaux

- destination déduite de l’autre panneau ;
- dialogue manuel conservé en vue simple ;
- copie exécutée par le backend serveur ;
- déplacement exécuté par renommage SFTP ;
- refus d’une connexion différente ;
- collisions, chemin identique et destination dans la source ;
- résultats partiels ;
- refresh des panneaux source et destination.

### Gestionnaire et historique

- adaptation des états de transfert existants ;
- états des copies et déplacements ;
- conservation de pause, reprise et annulation ;
- suppression d’une entrée terminale ;
- nettoyage complet ;
- chargement et sauvegarde atomique ;
- sérialisation, restauration et affichage de l’identité serveur non secrète ;
- fichier absent, corrompu ou d’une version inconnue ;
- aucune persistance des opérations triviales ou de données sensibles.

### Non-régression Sprint 3

- upload et download de fichiers et dossiers ;
- FIFO et identifiants stables ;
- progression, vitesse, pause, reprise et annulation ;
- collisions et temporaires ;
- refresh après upload ;
- arrêt ordonné du worker et nettoyage des jobs.

Tous les tests automatisés restent indépendants d’un serveur SSH externe.

## Hors périmètre

- navigateur local ;
- transfert entre deux connexions SSH distinctes ;
- plusieurs sessions simultanées ;
- onglets ;
- serveurs enregistrés et restauration de sessions ;
- glisser-déposer ;
- écrasement ou fusion de dossiers ;
- progression fiable ou annulation de `cp` et `mv` côté serveur ;
- reprise d’un transfert après redémarrage ;
- unification complète de toutes les opérations dans une seule file d’exécution.

## Étapes d’implémentation

1. [x] Extraire le navigateur dans `FileBrowserPane` sans changer le comportement.
2. [x] Corréler les listings et distinguer erreurs localisées et erreurs de session.
3. [x] Introduire le split optionnel et le panneau actif sans abstraction de source
   inutilisée.
4. [x] Ajouter les opérations vers l’autre panneau et les refreshs ciblés.
5. [x] Généraliser le panneau inférieur avec un modèle d’opérations commun.
6. [x] Ajouter l’historique persistant et ses commandes de nettoyage.
7. [x] Exécuter la compilation stricte, les tests et la validation manuelle SSH/SFTP.

## Critères d’acceptation

- [x] L’application démarre en vue simple et peut activer ou fermer la vue scindée.
- [x] Chaque panneau conserve indépendamment chemin, sélection, historique et refresh.
- [x] Le panneau actif et la destination d’une opération sont sans ambiguïté.
- [x] Copier ou déplacer vers l’autre panneau ne demande pas de chemin manuel.
- [x] Une copie ou un déplacement sur la même connexion reste exécuté côté serveur.
- [x] Les réponses asynchrones ne peuvent pas être appliquées au mauvais panneau.
- [x] Uploads, downloads et navigation du Sprint 3 ne régressent pas.
- [x] La vue d’opérations suit upload, download, copie et déplacement.
- [x] L’historique terminal est persistant et nettoyable.
- [x] Les opérations triviales n’encombrent pas l’historique principal.
- [x] Les tests automatisés ne nécessitent aucun serveur externe.
- [x] La compilation stricte et toute la suite CTest réussissent.

La validation manuelle sur un serveur SSH/SFTP standard est réussie : navigation,
vue simple et scindée, copie et déplacement distants côté serveur, upload/download,
gestionnaire d’opérations, persistance, suppression individuelle et nettoyage de
l’historique ont été vérifiés en conditions réelles.

## État d’avancement

Les étapes 1 à 6 sont implémentées. À l’issue de l’étape 6 :

- chaque `FileBrowserPane` conserve un historique arrière/avant indépendant,
  modifié seulement lorsqu’un listing attendu réussit ;
- Retour, Suivant, Parent et Refresh ciblent le panneau actif ;
- les actions explicites « Copy to other pane » et « Move to other pane » utilisent
  le dossier courant de l’autre panneau, après confirmation des sources, de la
  destination et du type d’opération ;
- en vue scindée, ces actions figurent en tête du menu contextuel ;
  les variantes manuelles « Copy to… » et « Move to… » restent accessibles sous
  « More… » afin d’éviter toute ambiguïté de ciblage ;
- les commandes manuelles « Copy to… » et « Move to… » restent disponibles ;
- la copie réutilise `RemoteFileOperations::copy` et son `cp` serveur, tandis que
  le déplacement conserve le renommage SFTP ;
- l’identité des panneaux et des dossiers source/destination est conservée pendant
  l’opération, y compris si le split est refermé ;
- les succès complets ou partiels déclenchent uniquement les refreshs distants
  pertinents, coalescés par panneau ; un download n’en déclenche aucun ;
- le dock inférieur et son composant sont renommés « Operations » et
  `OperationPanel` ;
- `OperationProgress` agrège les événements sans modifier `TransferQueue`,
  `TransferFileJob`, `TransferDirectoryJob`, leur FIFO ni `TransferDirection` ;
- uploads et downloads conservent progression, vitesse, pause, reprise,
  annulation ainsi que leurs phases détaillées ;
- copies et déplacements distants apparaissent dès leur démarrage puis passent à
  « Completed » ou « Failed », sans progression ou contrôle non pris en charge ;
- les résultats partiels indiquent le nombre d’éléments réussis et conservent le
  détail contextualisé des échecs ;
- création de dossier, renommage et suppression restent exclus de la vue ;
- les opérations `Completed`, `Failed` et `Cancelled` sont restaurées au prochain
  démarrage depuis le document JSON v1 écrit atomiquement dans le répertoire de
  données applicatives Qt ;
- chaque opération mémorise l’hôte et le port SSH auxquels elle se rapporte, sans
  nom d’utilisateur ni donnée d’authentification, et les affiche en infobulle ;
- les états actifs ne sont ni sauvegardés ni restaurés ;
- l’historique est limité aux 200 terminaisons les plus récentes et les écritures
  rapprochées sont regroupées ;
- la ligne terminale sélectionnée peut être supprimée et tout l’historique
  terminal peut être vidé sans retirer les opérations actives ;
- les fichiers absents, vides, corrompus, trop volumineux ou d’une version inconnue
  sont traités comme un historique vide ;
- les tests du stockage injectent `QTemporaryDir` et la suite UI utilise le mode
  de chemins de test de Qt, sans écrire dans le profil utilisateur réel.

L’appréciation visuelle approfondie des thèmes clair et sombre n’a pas pu être
réalisée correctement sous WSL. Elle est explicitement différée à un environnement
natif Linux, Windows ou macOS ; les tests de palette et de transitions restent
valides et ce contrôle visuel différé ne bloque pas la release 0.5.0.
