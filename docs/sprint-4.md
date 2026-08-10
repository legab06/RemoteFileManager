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

Cette fonctionnalité constitue la dernière étape du Sprint 4. Elle n’est abordée
qu’après stabilisation du double panneau et du gestionnaire d’opérations.

Les opérations terminales seront persistées dans un format local versionné et
écrit atomiquement sous le répertoire de données applicatives fourni par Qt. Les
opérations actives ne seront jamais restaurées comme encore en cours après un
redémarrage.

L’historique :

- est borné par une politique de rétention documentée ;
- peut être nettoyé entrée par entrée ;
- peut être vidé complètement ;
- tolère un fichier absent, ancien ou corrompu ;
- ne contient aucun mot de passe, clé, jeton ou commande SSH ;
- limite les informations enregistrées aux données utiles à l’utilisateur.

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

1. Extraire le navigateur dans `FileBrowserPane` sans changer le comportement.
2. Corréler les listings et distinguer erreurs localisées et erreurs de session.
3. Introduire le split optionnel et le panneau actif sans abstraction de source
   inutilisée.
4. Ajouter les opérations vers l’autre panneau et les refreshs ciblés.
5. Généraliser le panneau inférieur avec un modèle d’opérations commun.
6. Ajouter l’historique persistant et ses commandes de nettoyage seulement si les
   étapes précédentes sont stabilisées.
7. Exécuter la compilation stricte, les tests et la validation manuelle SSH/SFTP.

## Critères d’acceptation

- [ ] L’application démarre en vue simple et peut activer ou fermer la vue scindée.
- [ ] Chaque panneau conserve indépendamment chemin, sélection, historique et refresh.
- [ ] Le panneau actif et la destination d’une opération sont sans ambiguïté.
- [ ] Copier ou déplacer vers l’autre panneau ne demande pas de chemin manuel.
- [ ] Une copie ou un déplacement sur la même connexion reste exécuté côté serveur.
- [ ] Les réponses asynchrones ne peuvent pas être appliquées au mauvais panneau.
- [ ] Uploads, downloads et navigation du Sprint 3 ne régressent pas.
- [x] La vue d’opérations suit upload, download, copie et déplacement.
- [ ] L’historique terminal est persistant et nettoyable.
- [ ] Les opérations triviales n’encombrent pas l’historique principal.
- [ ] Les tests automatisés ne nécessitent aucun serveur externe.
- [ ] La compilation stricte et toute la suite CTest réussissent.

Les critères dépendant d’un vrai serveur restent non cochés jusqu’à une validation
manuelle sur une installation SSH/SFTP standard.

## État d’avancement

Les étapes 1 à 5 sont implémentées. À l’issue de l’étape 5 :

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
- création de dossier, renommage et suppression restent exclus de la vue.

L’historique persistant de l’étape 6 n’est pas commencé. Les lignes du panneau ne
sont conservées que pendant l’exécution courante de l’application et aucune
commande de nettoyage n’est encore proposée.

Une nouvelle validation visuelle manuelle reste nécessaire sur les thèmes clairs
et sombres réellement ciblés ; les tests automatisés vérifient les palettes et les
transitions d’état, mais ne remplacent pas cette appréciation visuelle.
