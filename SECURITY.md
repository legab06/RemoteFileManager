# Politique de sécurité

RemoteFileManager manipule des connexions SSH/SFTP ainsi que des fichiers locaux et distants. Les problèmes de sécurité sont donc pris au sérieux.

## Versions prises en charge

RemoteFileManager est actuellement en phase de pré-release.

Les correctifs de sécurité sont appliqués à la version la plus récente du projet. Les anciennes versions de développement ne bénéficient pas nécessairement de correctifs rétroportés.

## Signaler une vulnérabilité

Merci de ne pas signaler publiquement une vulnérabilité de sécurité dans une issue GitHub.

Lorsqu'un signalement privé est disponible sur le dépôt, utilisez :

**Security → Report a vulnerability**

Cela permet de transmettre les détails de la vulnérabilité de manière confidentielle.

Un bon rapport devrait contenir, si possible :

- une description du problème ;
- les conditions nécessaires pour le reproduire ;
- les étapes de reproduction ;
- l'impact potentiel ;
- la version ou le commit concerné ;
- toute proposition de correction pertinente.

Merci de ne pas publier de preuve de concept exploitable avant qu'une correction puisse être préparée.

## Périmètre de sécurité

Les principaux objectifs de sécurité de RemoteFileManager comprennent notamment :

- la vérification de l'identité des serveurs SSH via `known_hosts` ;
- l'absence de stockage persistant des mots de passe ;
- la protection des secrets et identifiants ;
- la validation des chemins locaux et distants ;
- la prévention des injections lors des opérations exécutées côté serveur ;
- le respect strict des permissions du compte SSH utilisé ;
- l'absence d'élévation automatique de privilèges ;
- l'utilisation d'un serveur SSH/SFTP standard sans agent propriétaire.

Le modèle de sécurité détaillé du projet est disponible dans
[docs/security-model.md](docs/security-model.md).

## Divulgation

Une vulnérabilité confirmée sera corrigée avant sa divulgation publique lorsque cela est raisonnablement possible.

Les informations nécessaires pourront ensuite être publiées afin de permettre aux utilisateurs de mettre à jour leur installation.
