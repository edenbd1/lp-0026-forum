# Module catalog

Basecamp 0.3.0 installs apps from *repositories*: a `logos-repo.json` that
points at an `index.json` listing packages, versions, SHA-256 and download
URLs, in the format of the official
[`logos-modules-release`](https://github.com/logos-co/logos-modules-release).
A user adds one under **Settings → Package Repositories → Add a repository**;
the catalog then merges it with the official one, which is where the forum's
dependencies (`delivery_module` 0.2.1, `storage_module` 2.1.2) come from.

Basecamp requires the repository to be served over **HTTPS**.

The published catalog is
[`edenbd1/logos-forum-catalog`](https://github.com/edenbd1/logos-forum-catalog); add

```
https://raw.githubusercontent.com/edenbd1/logos-forum-catalog/main/logos-repo.json
```

To publish a release: build, pin the dependency versions into the package
manifest (unpinned, Basecamp installs storage_module 3.0.0-rc1, whose upload
and download calls differ from the 2.1 series the forum is built against), then
generate the two files:

```bash
nix build .#lgx-portable
scripts/pin-deps.py result/logos-logos_forum-module.lgx logos_forum-0.1.1.lgx
scripts/make-catalog.py \
  --base-url  https://github.com/<owner>/<repo>/releases/download/logos_forum-v0.1.1 \
  --index-url https://raw.githubusercontent.com/<owner>/<repo>/main/catalog/index.json \
  --out catalog logos_forum-0.1.1.lgx
```

then attach `logos_forum-0.1.1.lgx` to that release, and add
`https://raw.githubusercontent.com/<owner>/<repo>/main/catalog/logos-repo.json`
in Basecamp.
