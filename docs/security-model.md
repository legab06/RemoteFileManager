# Modèle de sécurité initial

## Principes obligatoires

1. **Vérifier l’identité du serveur.** Une clé déjà connue doit être comparée à `known_hosts`. Une première connexion doit afficher clairement l’empreinte et demander une décision explicite.
2. **Préférer les clés et l’agent SSH.** Le client utilisera en priorité l’agent système et les clés existantes. Un mot de passe ne devra jamais être sauvegardé en clair.
3. **Isoler les secrets.** Les journaux ne contiendront ni mot de passe, ni clé privée, ni commande incluant une donnée secrète.
4. **Respecter les droits distants.** Toutes les opérations héritent uniquement des permissions du compte SSH connecté. Aucun mécanisme de contournement ou d’élévation automatique n’est prévu.
5. **Traiter les chemins comme des données.** Les noms de fichiers ne doivent jamais devenir du code shell par simple concaténation.
6. **Rester compatible avec un serveur standard.** Aucun démon, agent ou compte privilégié supplémentaire ne sera demandé côté serveur.

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
- confirmation et récupération après opérations destructrices ;
- journal d’audit local respectueux des données sensibles.

