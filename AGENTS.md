# Guide des agents — RemoteFileManager

## Exigences permanentes du produit

RemoteFileManager est un gestionnaire graphique natif de fichiers distants, inspiré de Dolphin. Il fonctionne avec un serveur SSH/SFTP standard, sans démon, agent propriétaire ni autre composant à installer côté serveur.

Le produit doit permettre la navigation et, progressivement, la copie, le déplacement, le renommage, la suppression et le transfert de fichiers, avec une progression visible et des erreurs compréhensibles et récupérables. Exécuter côté serveur les opérations entre deux emplacements distants chaque fois que cela est possible, afin d’éviter un transit inutile par le client. L’interface doit à terme prendre en charge les onglets et l’affichage scindé.

Chaque évolution doit préserver un logiciel clair, sécurisé, réactif, léger, fiable et documenté. Linux est la plateforme prioritaire, mais le code et la construction doivent rester portables vers Windows et macOS.

Ces exigences sont des invariants. Les documents `docs/sprint-*.md` décrivent la feuille de route et l’état des livraisons ; ne pas transformer ce fichier en journal de sprint.

## Architecture

- Respecter les couches existantes : UI Qt Widgets dans `src/app`, modèles et orchestration dans `src/core`, transport SSH/SFTP dans `src/ssh`, API publiques dans `include/remotefilemanager`.
- L’UI exprime des intentions et reçoit des résultats métier. Elle ne doit jamais exposer ni manipuler `ssh_session`, `sftp_session`, `sftp_attributes` ou tout autre type libssh.
- Réserver le thread graphique à Qt. Toute connexion et toute opération réseau ou distante potentiellement lente s’exécute dans un worker ; remonter résultats, progression et erreurs par signaux asynchrones.
- Concevoir les opérations longues pour pouvoir recevoir un identifiant stable, publier leur progression et être annulées proprement.
- Utiliser SFTP pour la navigation, les métadonnées, les transferts et les opérations qu’il prend en charge. Pour les opérations serveur, isoler la construction des commandes et traiter les chemins comme des données : aucune concaténation naïve dans un shell.
- Préserver C++20, Qt 6, CMake et libssh ainsi que les cibles séparées `rfm_core`, `rfm_ui` et `RemoteFileManager`.

## Sécurité et fiabilité

- Vérifier strictement chaque clé d’hôte avec `known_hosts`. Une première clé exige une confirmation explicite de son empreinte ; une clé connue qui change bloque la connexion sans contournement automatique.
- Préférer l’agent et les clés SSH. Ne jamais persister, journaliser ou inclure dans une erreur un mot de passe, une clé privée ou une autre donnée sensible ; réduire la durée de vie des secrets en mémoire et les effacer après usage.
- Ne jamais introduire d’élévation automatique de privilèges. Les opérations restent limitées aux droits du compte SSH connecté.
- Associer les erreurs au serveur, au chemin et à l’opération concernés sans révéler de secret. Les opérations destructrices ou avec écrasement doivent obtenir une confirmation explicite et offrir une récupération lorsque cela devient possible.
- Employer uniquement des API et en-têtes publics et portables. Éviter les hypothèses propres à Linux hors des adaptateurs prévus à cet effet.

## Développement

- Suivre `.clang-format` et les conventions existantes : classes et types en `PascalCase`, fonctions et variables en `camelCase`, membres privés préfixés par `m_`, namespace racine `rfm`.
- Garder les changements ciblés. Mettre à jour la documentation lorsque le comportement, l’architecture, la sécurité ou les prérequis changent.
- Ajouter ou adapter des tests pour chaque comportement fonctionnel et chaque régression raisonnablement testable. Tester au minimum la logique métier, les transitions UI importantes et les cas d’erreur ; ne pas dépendre d’un serveur externe dans la suite unitaire.
- Compiler sans avertissement, y compris avec `RFM_WARNINGS_AS_ERRORS=ON`. Avant livraison, exécuter :

```bash
cmake --preset debug -DRFM_WARNINGS_AS_ERRORS=ON
cmake --build --preset debug
ctest --preset debug
```

## Workflow Git

- Partir d’une base à jour et créer une branche courte, descriptive et limitée à une évolution.
- Examiner l’état du dépôt avant toute modification. Préserver les changements de l’utilisateur et ne pas mêler de modifications sans rapport.
- Ne pas réécrire l’historique, supprimer des données, pousser ou créer un commit sans demande explicite.
- Avant remise, vérifier `git diff --check`, relire le diff complet et signaler toute validation impossible, notamment un test d’intégration nécessitant un serveur SSH réel.

## Définition de terminé

Une modification est terminée lorsque le comportement demandé fonctionne, respecte les frontières d’architecture et les invariants de sécurité, garde l’interface réactive, gère clairement les erreurs, possède les tests pertinents, compile sans avertissement sur la configuration stricte, passe toute la suite de tests et inclut la documentation nécessaire. Le diff doit rester ciblé, sans secret, artefact de compilation ni changement étranger à la tâche.

## File editing rules

When modifying source files:

1. ALWAYS use the Edit files tool as the primary method.
2. NEVER use sed, awk, perl, python, cat, echo, head/tail, cp or shell
   redirections to rewrite source-code files.
3. Reading/searching with tools or terminal is allowed.
4. Terminal commands may be used for build, tests, git diff and inspection,
   but NEVER for editing source code.
5. If an Edit files operation fails:
   - reread the exact target region;
   - retry Edit files with a smaller and more precise change.
6. If Edit files still fails after two attempts:
   STOP and report the failure.
   Do NOT fall back to shell-based editing.
7. Never claim a modification succeeded without verifying the resulting diff.
8. After editing, run:
   git diff -- <modified files>
   and inspect the result before continuing.