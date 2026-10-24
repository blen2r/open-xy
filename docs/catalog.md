# Catalog

Machines published from their own repositories reach the Open-XY builder
through [`catalog/machines.json`](../catalog/machines.json). The catalog pins
one tagged commit of each; nothing is copied into this repository. Each
machine's ID is held for good, so no two machines ever share one. Engine,
effect and page numbers are not the catalog's: each firmware's
[layout](api.md#identity) gives them. Authors follow the
[submission guide](https://github.com/etcetc000/open-xy-machine-template/blob/main/SUBMITTING.md).

## Entries

```json
{"id": "karplus", "kind": "synth", "maintainer": "etcetc000", "status": "published",
 "repository": "https://github.com/etcetc000/open-xy-karplus", "tag": "v1.3.0",
 "commit": "617e321f8037541f283cfe46fd39ff90d5e9db8c", "path": "."}
```

| Field | Contract |
| --- | --- |
| `id`, `kind` | As in the machine's `definition.json` and `machine.json` ([identity rules](api.md#identity)) |
| `maintainer` | The GitHub user who maintains it |
| `status` | `reserved`: the ID is held for a machine being written. `published`: built from its release. `retired`: no longer built; its ID stays held |
| `repository`, `tag`, `commit`, `path` | A published machine's release: `https://github.com/<owner>/<repo>`, the tag, its full commit SHA, and the machine's folder in the repository (`.` at the root). A retired entry may keep them |

IDs are unique across the catalog. Open-XY's own machines are entries like
any other, published from the [core machines](https://github.com/etcetc000/open-xy-core-machines).

The builder's machine details panel links to the catalog's maintainer profile,
repository and source at the pinned release commit. Community machines also
show a short notice about support and possible instability or unexpected audio.

## Commands

| Command | Does |
| --- | --- |
| `python sdk.py catalog check [--base <ref>] [--allow-control-changes]` | Validates the catalog. With `--base`, also against an earlier catalog: every entry stays, with its kind, and its status only moves forward; a published machine whose commit moved is fetched at both commits and its [controls compared](#controls) |
| `python sdk.py catalog fetch [--tags]` | Fetches every published machine at exactly its commit into `local/catalog/`, checks it matches its entry and writes `local/catalog/collection.json`: every published machine, in catalog order. `--tags` also confirms each tag still names its commit |
| `python sdk.py catalog test [--tags]` | Fetches, then runs [`sdk.py test`](first-machine.md#what-you-own) on every published machine |

A fetched folder that changed is refused; delete it to fetch again. Pass the
collection to any build command:

```sh
python sdk.py compile --collection local/catalog/collection.json --out <folder>
```

## Pull requests

A machine submission from a fork changes only `catalog/machines.json`; CI
refuses one that changes anything else. Changes the SDK needs for a machine are
a separate pull request.

The [Catalog workflow](../.github/workflows/catalog.yml) runs on every pull
request that changes `catalog/` or anything published machines build against
(the SDK, tooling, profiles or linker script). It validates the catalog, fetches each
published machine and checks it as its own repository's CI does, and compiles
the firmware module with all of them on the pinned Windows tools.

Merged catalog changes reach the site with the next release
([`sdk.py release`](builder.md#cloudflare-deployment)): beta from `staging`,
then production from `main`.

The builder shows catalog machines as **Community**, unselected at first.
Open-XY's own examples, which maintainers list by ID (`OFFICIAL` in
[`tooling/catalog.py`](../tooling/catalog.py)), are shown as Open-XY machines
instead. A catalog entry has no field to claim this, and who hosts its
repository does not decide it.

A maintainer then reviews a new machine once, in full:

- It uses only the public headers: no assembly, raw addresses or platform code.
- Its audio callbacks never allocate, block, log or do I/O.
- Its name is sensible, and its licence allows redistribution.
- It sounds right in the browser preview from its CI.

An update moves `tag` and `commit` and is reviewed as a diff. The workflow
compares the catalog with the pull request's target branch: a change of `kind`
is refused, and a removed machine becomes `retired`, never deleted.
A new `repository` or `maintainer` is a handover: it fails the check, and a
catalog maintainer approves it in a pull request of its own.

### Controls

Projects save a machine's parameter values by index in its `definition.json`
`parameters`, so an update keeps what each saved value means. Compared with
the previous release, each parameter keeps its index, `id`, `mapping` and, for
an alternate, `control`. A removed, moved or renamed parameter, or a changed
mapping (its kind, range, values or enum labels), fails the check, which names
each by index. Parameters added after the last one and new `label`, `display`
or `default` values are reported and allowed.

A catalog maintainer approves a control change by adding the
`controls-changed` label to the pull request; the workflow runs again and
passes `--allow-control-changes`.
