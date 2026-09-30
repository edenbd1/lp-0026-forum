#!/usr/bin/env python3
"""Pin the forum's dependency versions in a built .lgx.

The forum is compiled against delivery_module 0.2.1 and storage_module 3.0.0.
Unpinned, Basecamp's resolver would install whatever is newest in the catalog,
and a major version changes the API (2.1 to 3.0 changed uploadInit and
downloadChunks), so Logos Storage history would silently not work.

The builder's code generator reads metadata.json's `dependencies` as plain
names, so the ranges cannot live there; they are written into the package
manifest instead, in the form the package manager resolves
({"name", "version"} with an npm-style range).

  scripts/pin-deps.py result/logos-logos_forum-module.lgx out.lgx
"""
import io, json, sys, tarfile

PINS = {"delivery_module": "~0.2.1", "storage_module": "~3.0.0"}

src, dst = sys.argv[1], sys.argv[2]
with tarfile.open(src) as t:
    members = [(m, t.extractfile(m).read() if m.isfile() else None) for m in t.getmembers()]
out = []
for m, data in members:
    if m.name.lstrip("./") == "manifest.json":
        man = json.loads(data)
        deps = [d if isinstance(d, str) else d["name"] for d in man.get("dependencies", [])]
        man["dependencies"] = [{"name": n, "version": PINS[n]} if n in PINS else n for n in deps]
        data = json.dumps(man, indent=2).encode()
        m.size = len(data)
    out.append((m, data))
with tarfile.open(dst, "w:gz") as t:
    for m, data in out:
        t.addfile(m, io.BytesIO(data) if data is not None else None)
print("pinned:", json.dumps(man["dependencies"]))
