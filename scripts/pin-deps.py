#!/usr/bin/env python3
"""Pin the forum's dependency versions in a built .lgx.

The forum is compiled against delivery_module 0.3.0 and storage_module 3.0.0.
Unpinned, Basecamp's resolver would install whatever is newest in the catalog,
and a major version changes the API (2.1 to 3.0 changed uploadInit and
downloadChunks), so Logos Storage history would silently not work.

On logos.test, its default network, it also needs the RLN modules and the
sponsor's client: delivery lists liblogos_rln_module only as an optional
dependency, which Basecamp does not install by itself, and without it every
send waits for a proof that never comes. liblogos_rln_module 0.10.0 brings
liblogos_lez_rln_module 4.2.1, the first that finds the testnet registry's zone
on its own; libp2p_module 1.1.0 and rln_gifter_module 0.2.1 (from the forum's
catalog, built from gifter/rln-gifter-module) carry the request for a gifted
membership to the forum's sponsor. The forum calls these modules without a
generated wrapper, so they are added to the manifest only.

The builder's code generator reads metadata.json's `dependencies` as plain
names, so the ranges cannot live there; they are written into the package
manifest instead, in the form the package manager resolves
({"name", "version"} with an npm-style range).

  scripts/pin-deps.py result/logos-logos_forum-module.lgx out.lgx
"""
import io, json, sys, tarfile

PINS = {"delivery_module": "~0.3.0", "storage_module": "~3.0.0"}
# Runtime-only dependencies: not in metadata.json (no wrapper is generated).
EXTRA = {"liblogos_rln_module": "~0.10.0", "liblogos_lez_rln_module": "~4.2.1",
         "libp2p_module": "~1.1.0", "rln_gifter_module": "~0.2.1"}

src, dst = sys.argv[1], sys.argv[2]
with tarfile.open(src) as t:
    members = [(m, t.extractfile(m).read() if m.isfile() else None) for m in t.getmembers()]
out = []
for m, data in members:
    if m.name.lstrip("./") == "manifest.json":
        man = json.loads(data)
        deps = [d if isinstance(d, str) else d["name"] for d in man.get("dependencies", [])]
        man["dependencies"] = [{"name": n, "version": PINS[n]} if n in PINS else n for n in deps]
        man["dependencies"] += [{"name": n, "version": v} for n, v in EXTRA.items() if n not in deps]
        data = json.dumps(man, indent=2).encode()
        m.size = len(data)
    out.append((m, data))
with tarfile.open(dst, "w:gz") as t:
    for m, data in out:
        t.addfile(m, io.BytesIO(data) if data is not None else None)
print("pinned:", json.dumps(man["dependencies"]))
