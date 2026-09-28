# Module catalog

Basecamp 0.3.0 installs apps from *repositories*: a `logos-repo.json` that
points at an `index.json` listing packages, versions, SHA-256 and download
URLs — the format of the official
[`logos-modules-release`](https://github.com/logos-co/logos-modules-release).
A user adds one under **Settings → Package Repositories → Add a repository**;
the catalog then merges it with the official one, which is where the forum's
dependencies (`delivery_module` 0.2.1, `storage_module` 2.1.2) come from.

Basecamp requires the repository to be served over **HTTPS**.

Generate the two files from the built package:

```bash
nix build .#lgx-portable
scripts/make-catalog.py \
  --base-url  https://github.com/<owner>/<repo>/releases/download/logos_forum-v0.1.0 \
  --index-url https://raw.githubusercontent.com/<owner>/<repo>/main/catalog/index.json \
  --out catalog result/*.lgx
```

then attach `logos_forum-0.1.0.lgx` to that release, and add
`https://raw.githubusercontent.com/<owner>/<repo>/main/catalog/logos-repo.json`
in Basecamp.
