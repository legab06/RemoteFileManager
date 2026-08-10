# Sprint 6 — Profils de serveurs et connexion rapide

Date : 10 août 2026
Version : `0.7.0`

## Objectif

Le Sprint 6 permet d'enregistrer plusieurs serveurs SSH et de relancer rapidement
une connexion à partir du dock Places. Il corrige également le cycle du dialogue
de connexion : cliquer sur Connect lance désormais une intention asynchrone sans
fermer la fenêtre. Seul le signal `SshSession::connected` confirme et ferme le
dialogue.

Le transport reste un SSH/SFTP standard fondé sur libssh, sans agent ni composant
à installer sur le serveur. Une seule session SSH active reste prise en charge à
la fois.

## Architecture

`ConnectionProfile`, dans `rfm_core`, porte les réglages non secrets communs à une
tentative et à un profil enregistré : identifiant stable, nom d'affichage, hôte,
utilisateur, port et autorisation explicite du fallback par mot de passe. Un profil
sans nom utilise toujours `effectiveDisplayName()`.

`ServerProfileStore`, également dans `rfm_core`, possède seul le format persistant.
Il ne dépend d'aucun widget et accepte un répertoire alternatif pour les tests.
`ServerProfileDialog` édite uniquement les champs persistables. `ConnectionDialog`
ajoute le secret temporaire nécessaire à une tentative, mais ne connaît pas le
store.

`HomePage` est un widget Qt Widgets purement présentatif. Il reçoit la collection
de `ConnectionProfile` déjà chargée, affiche les serveurs et émet uniquement les
intentions `connectProfileRequested(id)` et `newConnectionRequested()`. Il ne
connaît ni le store, ni libssh, ni la session active.

`MainWindow` orchestre ces composants : il charge la collection, expose le CRUD
dans Places, préremplit le dialogue pour une connexion rapide et relaie les
intentions vers le worker `SshSession`. Cette collection n'impose aucune limite au
nombre de profils et prépare ainsi le routage futur de plusieurs connexions sans
l'implémenter.

Une connexion manuelle peut demander l'enregistrement du serveur. Cette demande
reste une intention en mémoire dans `ConnectionDialog` : `MainWindow` ne crée
l'identifiant stable et n'appelle le store qu'après le véritable signal
`SshSession::connected`. Un échec ne modifie donc jamais le fichier.

La zone centrale de `MainWindow` est un `QStackedWidget` qui conserve deux pages
vivantes : `HomePage` et `PaneWorkspace`. L'accueil est sélectionné au démarrage ;
le navigateur distant ne devient visible qu'après `SshSession::connected`. La
déconnexion ou une perte de connexion purge les chemins, contenus, historiques et
contextes distants des panneaux avant de revenir à l'accueil.

## Format et persistance

Le fichier `server-profiles.json` est placé dans le répertoire applicatif retourné
par `QStandardPaths::AppDataLocation`. Son écriture utilise `QSaveFile`, donc le
remplacement est atomique. Le schéma actuel est versionné :

```json
{
  "version": 1,
  "servers": [
    {
      "id": "identifiant-stable",
      "name": "Serveur maison",
      "host": "192.0.2.10",
      "username": "alice",
      "port": 22,
      "allowPasswordFallback": true
    }
  ]
}
```

Un fichier absent représente une collection vide. Un fichier illisible, vide,
trop volumineux, mal formé ou d'une version inconnue produit une erreur propre et
n'est jamais écrasé par une opération CRUD. Les entrées invalides et les
identifiants dupliqués sont ignorés au chargement ; une sauvegarde refuse en
revanche toute collection invalide ou dupliquée.

## Sécurité

Le schéma ne possède aucun champ de mot de passe, passphrase, clé privée, token,
credential ou commande. Le store ne peut donc pas sérialiser le secret temporaire
du dialogue. Le mot de passe saisi reste en mémoire pour permettre un nouvel essai
après erreur, puis il est effacé du champ lorsque la connexion réussit. Le worker
ne tente l'authentification par mot de passe que si
`allowPasswordFallback` est explicitement activé et efface sa copie après
l'authentification.

La vérification stricte de `known_hosts` est inchangée. Une clé nouvelle exige
toujours une confirmation d'empreinte séparée ; une clé modifiée bloque toujours
la connexion. Pendant cette confirmation, le dialogue principal reste en état
Connecting.

## Dialogue de connexion

Le dialogue possède trois états explicites : Idle, Connecting et Error.

- Idle valide les champs et autorise Connect ou Cancel.
- Connecting désactive les champs, Connect, Cancel et la fermeture, affiche une
  barre de progression indéterminée et nomme l'hôte contacté.
- Error conserve toutes les valeurs, y compris le mot de passe en mémoire,
  réactive le formulaire et affiche l'erreur SSH dans le dialogue.

Pour une nouvelle connexion manuelle, l'option « Save this server for future
connections » est visible et décochée par défaut. Si elle est cochée, le profil
non secret est enregistré uniquement après succès. La connexion rapide d'un
profil existant masque cette option afin de ne pas créer une seconde logique de
sauvegarde. Avant toute création, l'hôte, l'utilisateur et le port sont comparés
aux profils chargés ; une correspondance existante est réutilisée sans doublon.

Le bouton Connect émet `connectionRequested` mais n'appelle jamais `accept()`.
`MainWindow` ferme le dialogue avec `connectionSucceeded()` uniquement après le
signal réel du worker. Un refus de clé d'hôte ou toute erreur libssh suit le même
retour vers Error. Les erreurs d'une session déjà établie conservent le traitement
global existant.

## Interface et CRUD

Le dock Places contient une section Servers avec les profils enregistrés et les
actions Connect, Add, Edit et Remove. La suppression demande une confirmation
courte. Un double-clic ou Connect ouvre le dialogue prérempli. Les réglages peuvent
y être modifiés pour la tentative courante sans modifier le profil ; seule l'action
Edit sauvegarde ces changements. Aucun secret n'est récupéré automatiquement.

Home présente la même collection issue de `MainWindow`, avec le nom et l'identité
de chaque serveur, un bouton Connect et une action New connection. Les changements
Add/Edit/Remove rafraîchissent Home et Places par une méthode commune ; il n'existe
ni second store ni second modèle métier. Les deux intentions réutilisent les
workflows de connexion existants dans `MainWindow`.

Dans Places, le bouton principal dépend du profil sélectionné. Hors connexion, il
affiche Connect. Pour le profil actif, il affiche Disconnect et déclenche la même
action `disconnectionRequested` que le menu et la barre d'outils. Un autre profil
sélectionné pendant la session conserve le libellé Connect mais reste désactivé
avec une explication. Le serveur actif porte le suffixe textuel « Connected » et
une graisse renforcée, afin que l'état ne dépende pas uniquement d'une couleur ni
de la sélection courante.

L'action partagée Disconnect est disponible dans le menu File, la barre d'outils
et le menu contextuel du serveur actif dans Places. Elle émet uniquement
`disconnectionRequested`, déjà relié à `SshSession::disconnectFromHost()`. Elle est
désactivée hors connexion ainsi que pendant une opération ou un transfert actif.
Après `SshSession::disconnected`, les refresh périodiques et différés sont arrêtés,
les requêtes de navigation en attente et les contextes de transfert sont invalidés,
les panneaux distants deviennent inactifs et le profil enregistré reste intact.

## Tests

Les tests du store couvrent le fichier absent, plusieurs profils, le round-trip,
le JSON malformé, les versions inconnues, les entrées invalides, les doublons, le
remplacement, la suppression, le rejet d'un profil invalide, l'écriture dans un
répertoire temporaire et l'absence de champ de secret.

Les tests du dialogue couvrent la validation, le clic Connect non fermant, l'état
Connecting, le verrouillage du formulaire, le retour après erreur, la conservation
des champs et du secret temporaire, l'absence du secret dans l'état affiché et la
fermeture uniquement sur succès, ainsi que l'option d'enregistrement décochée par
défaut. Les tests de `MainWindow` couvrent le cycle échec-correction-nouvelle
tentative-succès, le préremplissage depuis le store, la sauvegarde seulement après
succès, l'absence de doublon et de secret sérialisé, puis le cycle complet de
déconnexion et l'arrêt du refresh.

Les tests de Home couvrent la présentation des profils et les intentions de
connexion rapide et manuelle. Les tests de la fenêtre vérifient également la
bascule Home/Workspace, la purge du navigateur après déconnexion, la synchronisation
du CRUD entre Home et Places, le bouton Connect/Disconnect, le marqueur Connected
et le refus d'une seconde connexion en mode mono-session.

## Limites et suites différées

Ce sprint n'ajoute ni onglets, ni sessions SSH simultanées, ni copie entre deux
serveurs. Il n'ajoute aucun coffre-fort système, stockage de mot de passe, import
complet de `~/.ssh/config`, ProxyJump, SSHFS ou synchronisation des profils. Une
annulation réseau fiable n'étant pas disponible dans l'architecture actuelle,
Cancel et la fermeture sont temporairement désactivés pendant la courte tentative.
Le dossier distant initial facultatif n'est pas ajouté : le serveur continue de
déterminer le répertoire accessible après authentification.
Les onglets, plusieurs sessions et leur routage explicite restent différés au
Sprint 7 ou après.
