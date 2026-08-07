# Contribuer à RemoteFileManager

## Avant une modification

1. Créer une branche courte et ciblée.
2. Garder l’interface (`app`), le métier (`core`) et le transport (`ssh`) séparés.
3. Ne jamais contourner la validation des clés d’hôte SSH.
4. Ne jamais écrire de mot de passe, clé privée ou donnée sensible dans les journaux.

## Vérifications locales

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Le format C++ est défini par `.clang-format`. Une modification fonctionnelle doit inclure ou adapter un test lorsque c’est raisonnable.

## Conventions

- C++20 ;
- types et classes en `PascalCase` ;
- fonctions et variables en `camelCase` ;
- membres privés préfixés par `m_` ;
- namespace racine `rfm` ;
- en-têtes publics sous `include/remotefilemanager/` ;
- aucun appel SSH bloquant dans le thread de l’interface.

