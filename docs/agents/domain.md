# Domain Docs

How the engineering skills should consume this repo's domain documentation when exploring the codebase.

## Before exploring, read these

- **`docs/llm/`**: the primary domain reference for `libtsk`. Read the file matching the area you're about to work in (the index is in `CLAUDE.md` under "Reference docs"). Each ends with a "Known defects and traps" section; read it.
  - `base-and-img.md`: error handling, `TSK_TCHAR`, core types, disk images, `tsk/util/`.
  - `vs-and-pool.md`: partition tables, APFS and LVM pools, offsets and units between layers.
  - `fs.md`: file systems, files, directories, attributes, walks, encryption.
  - `auto-hashdb-cpp.md`: `TskAuto`, hash databases, C++ wrapper classes, `samples/`.
  - `build-and-test.md`: repo map, build options, `HAVE_*` macros, tests, conventions, portability.
- **`GLOSSARY.md`** at the repo root, if it exists: canonical terms.
- **`docs/adr/`**, if it exists: read ADRs that touch the area you're about to work in.

If `GLOSSARY.md` or `docs/adr/` don't exist, **proceed silently**. Don't flag their absence; don't suggest creating them upfront. The `/domain-modeling` skill creates them lazily when terms or decisions actually get resolved.

## Layout

Single-context repo:

```
/
├── CLAUDE.md
├── GLOSSARY.md        ← created lazily
├── docs/
│   ├── llm/           ← per-area domain reference (existing)
│   └── adr/           ← created lazily
└── tsk/
```

Keep `docs/llm/` as the place for area reference material; use `GLOSSARY.md` for term definitions and `docs/adr/` for decisions. Don't duplicate content between them: link instead.

## Use the domain vocabulary

When your output names a domain concept (in an issue title, a refactor proposal, a hypothesis, a test name), use the term as `docs/llm/` and `GLOSSARY.md` use it (for example `TSK_FS_INFO`, `TSK_VS_INFO`, pool, attribute, run). Don't drift to synonyms.

If the concept you need isn't documented yet, that's a signal: either you're inventing language the project doesn't use (reconsider) or there's a real gap (note it for `/domain-modeling`).

## Flag conflicts

If your output contradicts an existing ADR or a "Known defects and traps" entry in `docs/llm/`, surface it explicitly rather than silently overriding:

> _Contradicts ADR-0007 (…), but worth reopening because…_
