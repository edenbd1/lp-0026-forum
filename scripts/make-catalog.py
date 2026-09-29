#!/usr/bin/env python3
"""Build a Logos module catalog for the forum, in the format Basecamp's
package manager reads (the same as logos-co/logos-modules-release):

  <out>/logos-repo.json   what a user adds in Basecamp (Package Manager → repositories)
  <out>/index.json        the packages, their versions, hashes and download URLs

  scripts/make-catalog.py --base-url https://…/download/v0.1.0 --index-url https://…/index.json \\
      --out catalog result/*.lgx

Each .lgx is described by its own manifest.json, so what the catalog says is
what the package is; sha256 and size are computed from the file.
"""
import argparse, datetime, hashlib, json, os, sys, tarfile

p = argparse.ArgumentParser()
p.add_argument("--base-url", required=True, help="where the .lgx files are downloaded from")
p.add_argument("--index-url", required=True, help="where index.json is served")
p.add_argument("--out", default="catalog")
p.add_argument("--icon", help="icon PNG shown in Basecamp's package manager (stored as <sha256>.png next to index.json)")
p.add_argument("lgx", nargs="+")
a = p.parse_args()

packages = {}
for path in a.lgx:
    with tarfile.open(path) as t:
        manifest = json.load(t.extractfile("./manifest.json") if "./manifest.json" in t.getnames() else t.extractfile("manifest.json"))
    data = open(path, "rb").read()
    name, version = manifest["name"], manifest["version"]
    file = f"{name}-{version}.lgx"
    entry = {
        "releasedAt": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "publisherRef": f"{name}-v{version}",
        "url": f"{a.base_url.rstrip('/')}/{file}",
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "rootHash": manifest["hashes"]["root"],
        "manifest": manifest,
    }
    entry["urls"] = [entry["url"]]
    if a.icon:
        icon = open(a.icon, "rb").read()
        h = hashlib.sha256(icon).hexdigest()
        entry["icon"] = {"path": f"{h}.png", "sha256": h, "size": len(icon)}
        os.makedirs(a.out, exist_ok=True)
        open(os.path.join(a.out, f"{h}.png"), "wb").write(icon)
    packages.setdefault(name, []).append((entry, file, path))

os.makedirs(a.out, exist_ok=True)
index = {
    "schemaVersion": 1,
    "repositoryName": "logos-forum",
    "generatedAt": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    "packages": [{"name": n, "versions": [e for e, _, _ in vs]} for n, vs in packages.items()],
}
json.dump(index, open(os.path.join(a.out, "index.json"), "w"), indent=2)
json.dump({
    "schemaVersion": 1,
    "name": "logos-forum",
    "displayName": "Logos Forum",
    "description": "A serverless forum over Logos Delivery and Logos Storage. Its dependencies (delivery_module, storage_module) come from the official Logos catalog.",
    "homepage": "https://github.com/edenbd1/lp-0026-forum",
    "indexUrl": a.index_url,
    "trustedSigners": [],
}, open(os.path.join(a.out, "logos-repo.json"), "w"), indent=2)
for n, vs in packages.items():
    for e, file, _ in vs:
        print(f"{n} {e['manifest']['version']}  sha256 {e['sha256'][:16]}…  -> {e['url']}")
