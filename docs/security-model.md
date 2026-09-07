# Modèle de sécurité

## Principes obligatoires

1. **Vérifier l’identité du serveur.** Une clé déjà connue doit être comparée à `known_hosts`. Une première connexion doit afficher clairement l’empreinte et demander une décision explicite.
2. **Préférer les clés et l’agent SSH.** Le mode par défaut utilise les clés et l’agent, avec repli par mot de passe uniquement si autorisé dans le profil ; un mode mot de passe seul est également disponible. Aucun mot de passe ne doit être persisté, en clair ou autrement.
3. **Isoler les secrets.** Les journaux ne contiendront ni mot de passe, ni clé privée, ni commande incluant une donnée secrète.
4. **Respecter les droits distants.** Toutes les opérations héritent uniquement des permissions du compte SSH connecté. Aucun mécanisme de contournement ou d’élévation automatique n’est prévu.
5. **Traiter les chemins comme des données.** Les noms de fichiers ne doivent jamais devenir du code shell par simple concaténation.
6. **Rester compatible avec un serveur standard.** Aucun démon, agent ou compte privilégié supplémentaire ne sera demandé côté serveur.
7. **Confiner les téléchargements.** Un nom distant est validé comme composant local selon la
   plateforme cliente et ne peut jamais sortir du dossier local explicitement choisi.
8. **Ne pas ouvrir les nœuds spéciaux.** Les téléchargements acceptent uniquement fichiers
   réguliers et dossiers ; liens, FIFO, sockets et périphériques sont refusés avant lecture.

## Application actuelle

- `SshSession` vérifie `known_hosts` avant l’authentification. Une clé inconnue exige une
  confirmation explicite de son empreinte avant enregistrement ; une clé modifiée ou
  d’un autre type que la clé connue bloque la connexion.
- `ServerProfileStore` persiste les paramètres de connexion dans un JSON non chiffré,
  sans mot de passe ni contenu de clé privée. Le chemin optionnel de clé est une
  préférence persistante. Le transport charge cette clé via libssh pour la valider,
  libère l’objet importé et utilise l’authentification automatique libssh. Il ne demande
  ni ne conserve de phrase secrète : une clé protégée doit être chargée dans l’agent,
  avec le champ Private key vide. Un échec de chargement de la clé explicite bloque
  la connexion avant le repli par mot de passe.
- Le mot de passe SSH est demandé séparément après vérification de l’hôte, transféré
  au worker dans `SecurePassword` sans copie partagée et effacé après la tentative
  d’authentification. Les exigences d’absence de secrets dans les journaux restent
  applicables à toutes les méthodes.
- La copie Local ↔ SSH depuis les panneaux utilise les mêmes jobs SFTP que les
  transferts Upload/Download : la validation des chemins locaux et le refus des liens
  et nœuds spéciaux restent en vigueur. Le déplacement Local ↔ SSH est refusé.
- Les commandes de copie et de suppression serveur passent par `RemoteCopyCommand`,
  avec arguments échappés séparément et séparation des options par `--`. Aucun agent
  propriétaire n’est requis ; les opérations shell dépendent des outils du serveur,
  et la suppression récursive protégée ainsi que le fallback de déplacement entre
  filesystems dépendent des mécanismes GNU/Linux décrits dans l’architecture.
- Les suppressions locales et distantes demandent une confirmation explicite et
  restent définitives ; aucune corbeille ni restauration n’est implémentée.

## Menaces couvertes en priorité

- interception de la première connexion ou changement de clé d’hôte ;
- fuite d’identifiants dans les paramètres ou journaux ;
- injection de commande par un nom de fichier malveillant ;
- gel de l’interface lors d’une opération réseau ;
- opération lancée sur le mauvais serveur ou le mauvais chemin ;
- perte de données après une suppression ou un écrasement non confirmé.

## Décisions reportées

- intégration aux trousseaux de secrets Linux, Windows et macOS ;
- politique de reconnexion et d’expiration des sessions ;
- format chiffré des profils persistants ;
- récupération après opérations destructrices ;
- journal d’audit local respectueux des données sensibles.
