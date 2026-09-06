# Sous-lot 4B — Profils et authentification SSH

## Parcours utilisateur

Les données persistables d'un serveur (nom, hôte, utilisateur, port, mode d'authentification,
chemin facultatif de clé privée et autorisation de l'authentification par mot de passe) sont éditées par
un formulaire partagé entre New connection, Add, Edit et Properties. Le libellé
utilisateur est « Allow password authentication if key fails ».
Le mode est explicitement « SSH key / agent » ou « Password only »; un chemin de clé vide
ne signifie jamais Password only.

Connect et le double-clic sur un profil enregistré ne rouvrent pas ce formulaire.
Ils lancent directement la connexion avec les réglages enregistrés. En mode SSH key / agent,
après la connexion réseau et la vérification de la clé d'hôte, le worker essaie l'identité explicite du
profil, puis les mécanismes automatiques de libssh, dont l'agent et les clés par défaut.
Si elles ne suffisent pas, le profil doit autoriser le mot de passe et le serveur doit
annoncer la méthode `password` pour qu'un dialogue compact soit affiché. Ce dialogue ne
contient que le nom du serveur, `user@host` et le mot de passe.

En mode Password only, aucune authentification par clé ou agent n'est tentée. Après
known_hosts, le worker vérifie que le serveur annonce `password`; sinon il signale une
erreur claire sans ouvrir de dialogue. Si la méthode est proposée, le même dialogue
password-only est ouvert directement.

Le texte d'introduction précise la cause : échec des clés (`SSH key authentication
failed` lorsqu'une identité explicite est configurée, sinon `SSH key or agent
authentication failed`) ou authentification supplémentaire après un résultat partiel
(`Additional password authentication is required`). Un mot de passe refusé affiche
`Incorrect password. Please try again.` dans le même dialogue.

Un refus de mot de passe laisse le dialogue ouvert pour un nouvel essai. Son annulation
abandonne la session SSH en attente. Une erreur réseau, une erreur libssh ou une méthode
non prise en charge suit le chemin d'erreur normal et n'ouvre jamais ce dialogue.

## Architecture et sécurité

`ServerProfileForm` porte les widgets et la validation communs aux dialogues de création
et d'édition. `MainWindow` orchestre les intentions, sans interpréter les codes libssh.
`SshAuthenticationPolicy` transforme uniquement le résultat passwordless, l'option du
profil et les méthodes annoncées en prochaine étape déterministe. `SshSession` conserve
la session vérifiée pendant la saisie et reprend l'authentification dans son worker.

Le profil conserve uniquement le chemin saisi. Avant `ssh_connect`, le worker développe
explicitement `~` et `~/...` avec le répertoire personnel fourni par Qt, convertit le
résultat en chemin absolu natif et le transmet à libssh avec `SSH_OPTIONS_IDENTITY`.
L'identité est ainsi préfixée à la liste réellement consommée par
`ssh_userauth_publickey_auto`, sans supprimer l'agent ni les identités automatiques.

Le mot de passe n'entre ni dans `ConnectionProfile`, ni dans un signal Qt copiable, ni
dans `ServerProfileStore`. Le dialogue le convertit immédiatement en `SecurePassword`,
efface le champ et transfère la propriété au worker par un événement privé. La mémoire
contrôlée est effacée après chaque tentative. La vérification de `known_hosts` reste
strictement antérieure à toute authentification : une clé inconnue exige toujours une
confirmation explicite et une clé changée bloque la connexion.

Pour préserver le format version 1 et sa compatibilité, le store conserve le nom de clé
historique `allowPasswordFallback`. Le modèle et l'interface emploient désormais le nom
plus exact `allowPasswordAuthentication`; la lecture accepte également ce nouveau nom.
Les champs JSON `authenticationMode` et `privateKeyPath` sont facultatifs et omis
lorsqu'ils prennent leurs valeurs par défaut ou sont vides : un profil
historique garde donc exactement le comportement automatique antérieur. Seul ce chemin
est sérialisé; aucun mot de passe, aucune passphrase et aucun contenu de clé ne l'est.

L'authentification keyboard-interactive et la saisie des passphrases de clés ne font pas
partie de ce sous-lot. Pour éviter tout prompt terminal caché, RFM n'en fournit aucune à
libssh. Une clé explicite chiffrée produit une erreur locale claire; elle doit être
chargée au préalable dans un agent SSH et le champ Private key doit alors rester vide.
