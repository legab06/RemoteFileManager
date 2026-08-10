# Sprint 3 — Transferts SFTP client–serveur

Date : 8 août 2026  
Version : `0.4.0`

## Objectif

Ajouter l’envoi et le téléchargement de fichiers et dossiers entre le poste local
et le serveur SFTP connecté. Les transferts ont une file FIFO en mémoire, une
progression, une vitesse, une pause, une reprise pendant la session et une
annulation coopérative. Le navigateur distant existant reste la vue principale.

## Périmètre

- upload et download de fichiers et dossiers récursifs ;
- un transfert actif et une file FIFO par session ;
- progression, vitesse, pause, reprise et annulation ;
- temporaire puis promotion vers le nom final ;
- refus par défaut des collisions et des liens symboliques récursifs ;
- tests sans serveur SSH externe.

## Architecture

Le worker SSH est l’unique propriétaire des sessions et handles libssh. Une
machine à états traite une action bornée (une entrée, un répertoire ou un bloc)
puis programme l’étape suivante dans l’event loop Qt. Les slots de contrôle et
de navigation sont donc traités entre deux étapes, sans accès concurrent à
libssh. Les types et règles métier restent dans `rfm_core`; le backend SFTP est
privé au transport.

`TransferDirectoryJob` réalise un transfert logique de dossier. Il commence par
une découverte incrémentale de l’arborescence afin d’établir une taille totale
stable, crée ensuite les dossiers destination un par un et délègue chaque fichier
à un `TransferFileJob`. Pour un download, le backend ouvre un handle de dossier
abstrait et une étape ne lit qu’une entrée distante. Les fichiers enfants restent
internes au job de dossier et ne deviennent pas des éléments de la FIFO globale.
Dans une `TransferRequest` de dossier, `destination` désigne le chemin final de la
racine à créer, et non son dossier parent.

La progression globale additionne les octets finalisés et ceux du fichier enfant
actif. `completedFiles`, `totalFiles` et `currentItem` permettent de contextualiser
un résultat partiel. La progression en octets ne décroît pas.

## Intégrité et collisions

Un upload crée dans le dossier distant un temporaire exclusif avec
`O_CREAT | O_EXCL`; un download utilise un temporaire local dans le dossier
cible. Le nom final est publié seulement après fermeture réussie. Aucun fichier
final n’est ouvert avec `O_TRUNC` ni supprimé pour faciliter une promotion.

libssh implémente SFTP v3. Selon `draft-ietf-secsh-filexfer-02`, un renommage
vers une destination existante est une erreur. L’API `sftp_rename` ne permet
toutefois pas de demander une garantie atomique portable face à un acteur
externe : un conflit de promotion est donc signalé, jamais contourné par un
écrasement.

Pour un dossier, la destination racine doit être absente, qu’il s’agisse d’un
upload ou d’un download. Le Sprint 3 ne fusionne donc pas une arborescence avec
un dossier existant. Chaque sous-dossier est créé explicitement et tout conflit
ou changement concurrent fait échouer le transfert sans suppression préalable.

## Récursivité, liens et annulation

Le parcours local emploie `QDirIterator` sans suivi des liens. Le parcours distant
utilise les métadonnées métier fournies par `RemoteTransferBackend`. Un lien
symbolique sélectionné ou rencontré provoque un échec contextualisé et sa cible
n’est jamais parcourue.

Chaque nom distant destiné au poste local est désormais validé selon les règles de la plateforme
cliente, puis le chemin construit est vérifié sous la racine choisie. Les espaces de début et de fin
restent intacts lorsqu'ils sont représentables ; un nom POSIX légal mais non représentable sous
Windows échoue clairement sans modifier le nom distant. Le backend conserve le type SFTP complet :
seuls les fichiers réguliers et dossiers sont transférés, avant toute ouverture susceptible de
bloquer sur une FIFO ou un autre nœud spécial. Les erreurs de stat et d'I/O conservent leur classe,
l'opération et le chemin concernés.

Pause et reprise conservent la phase de parcours, les listes découvertes et le
fichier enfant actif. Une annulation demande d’abord au fichier enfant de nettoyer
son temporaire, ferme un éventuel handle de listing, puis termine le job de dossier
en `Cancelled`. Les fichiers déjà promus restent en place : il n’y a pas de rollback
global de leurs succès. L’erreur indique l’élément fautif et les compteurs indiquent
le nombre d’éléments déjà terminés.

## Interface

- « Envoyer… » propose séparément « Fichier(s)… » et « Dossier… », avec les
  sélecteurs natifs Qt. Chaque sélection est ajoutée séparément à la FIFO et sa
  destination est construite dans le dossier distant affiché ;
- « Télécharger… » agit sur toute la sélection distante et demande un dossier
  local. Chaque nom distant est ajouté à ce dossier sans navigateur local maison ;
- un dock inférieur `TransferPanel`, indexé par l’identifiant stable, affiche
  direction, source, destination, état traduit, progression, taille, vitesse,
  erreur et actions pause/reprise/annulation ;
- la colonne d’actions est dimensionnée sur les `sizeHint()` des boutons et reste
  en `ResizeToContents`. Source, destination, progression et erreur se partagent
  l’espace extensible ; une barre de défilement horizontale prend le relais aux
  largeurs très réduites, sans tronquer « Reprendre » ou « Annuler » ;
- une progression de taille encore inconnue est indéterminée. Les erreurs restent
  dans le panneau et n’ouvrent pas une succession de boîtes de dialogue ;
- les transferts ne positionnent pas le verrou court `m_busy` utilisé par les
  anciennes opérations : navigation, sélection et ajout à la FIFO restent
  disponibles entre deux étapes du worker. Seule l’ouverture d’une nouvelle
  connexion est désactivée tant qu’un transfert n’est pas terminal, car elle
  remplacerait la session propriétaire des jobs actifs.

Les actions de la barre d’outils utilisent les icônes de thème portables
`go-up` et `go-down`, avec `SP_ArrowUp` et `SP_ArrowDown` comme fallbacks Qt.
Le texte et les tooltips restent visibles pour ne pas dépendre de l’interprétation
des icônes.

Le navigateur distant est rafraîchi automatiquement toutes les 3 000 ms quand
la session est connectée. Un seul listing peut être actif : le polling est ignoré
pendant un listing ou une opération UI courte, tandis qu’une demande utilisateur
ou événementielle est différée et coalescée. La sélection et la position de
défilement sont restaurées lors d’un refresh du même dossier. Un debounce de
150 ms regroupe les fins d’upload et les opérations distantes réussies. Un
download ne déclenche pas de refresh distant.

À la fermeture, `MainWindow` demande au worker de vider la file et d’annuler le
job actif. Le thread SSH continue ses étapes de nettoyage avant de fermer les
sessions et de notifier l’interface, ce qui préserve l’ordre de destruction
job → backend → SFTP.

## Hors périmètre

- persistance ou reprise après redémarrage ;
- parallélisme, navigateur local, double panneau et glisser-déposer ;
- shell, synchronisation, checksum utilisateur et limitation de débit.

## Critères d’acceptation

- [ ] Upload, download, fichiers et dossiers récursifs fonctionnent.
- [ ] La file FIFO n’exécute qu’un transfert actif.
- [ ] Pause, reprise, annulation et progression ne bloquent pas Qt.
- [ ] Les collisions, erreurs et résidus temporaires sont explicitement signalés.
- [ ] Les liens symboliques ne sont jamais suivis.
- [ ] Les tests unitaires ne nécessitent pas de serveur externe.

L’implémentation et les tests automatisés couvrent ces critères. Ils restent
volontairement non cochés jusqu’à la validation manuelle sur un serveur SFTP réel.
