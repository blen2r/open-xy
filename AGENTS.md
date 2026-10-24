# Repository instructions

## First, which task is this?

- **Making a machine** (an instrument or effect, a "plugin"): it does not go in
  this repository, which holds only the SDK and the catalog. Start a repository
  of your own from the
  [machine template](https://github.com/etcetc000/open-xy-machine-template) and
  follow its README and AGENTS.md. Use this checkout only as the SDK beside it,
  for example `python ../open-xy/sdk.py test --machine machines/<id>`. Do not
  add a machine to this repository, or change `sdk/`, `tooling/` or tests, to
  make a machine work. If the SDK lacks something it needs, that is a separate SDK
  pull request.
- **Submitting a machine to the builder:** a pull request against `staging` that
  changes only `catalog/machines.json`, adding or updating that machine's one
  entry ([submitting](https://github.com/etcetc000/open-xy-machine-template/blob/main/SUBMITTING.md),
  [catalog](docs/catalog.md)). CI refuses a submission from a fork that changes
  anything else.
- **Changing the SDK, builder or docs:** branch from `origin/staging` and open the
  pull request against `staging`. `main` only receives releases from `staging`;
  never base work on it or target it.

This repository holds no machines. Examples to read before writing one: the
template's own, and Open-XY's
[core machines](https://github.com/etcetc000/open-xy-core-machines): synths 2op FM, Macro Osc and Karplus, the Timestretch
sample engine, and the Ring Mod and Ping Pong send effects, each published
through the catalog like any other machine.

## SDK changes

Read README.md and CONTRIBUTING.md. For native changes also read
[the firmware ABI](docs/abi-1.2.0.md) and [building firmware](docs/build.md).

- Engine-specific source, metadata, generators and sound tests belong under that
  engine. Keep `tooling/` and root `tests/` focused on shared contracts.
- Contributor C++ contains no platform conditionals, OS headers or assembly.
  Put platform support in `sdk/`. Audio callbacks must not allocate, block,
  log, perform I/O, register engines or call graphics.
- Preserve exact stock/tool authentication, allowed edits, entry/loader records,
  protected ranges, reserved memory, occupied-ID guards and full reconstruction.
  Do not change expected hashes merely to admit a failed build.
- Keep tools and non-firmware generated outputs in ignored `local/`; firmware,
  stock artwork and user data live outside the checkout (below). Use fresh
  output directories and the allowlisted preview server.
- Run `python sdk.py check` for every change. Machine tests are the machine
  maintainer's responsibility and do not live in this repository.
  Native changes to this repository require the build and
  [release verification](docs/build.md#release-verification), with no skipped
  required fixtures. Checks must remain active under `python -O`.
- Build and verification commands are local. Device operations require a separate
  user instruction. Do not infer measured cycles or recovery from host checks.
- Keep public docs about current interfaces and behavior. Remove obsolete text
  instead of adding another status page or repeating caveats across every guide.

User instructions take precedence over this file.

## Private firmware storage

All firmware comes from the user's device. Firmware, extracted firmware,
firmware-containing packages and stock content must live outside every
checkout, including ignored local/ directories. The one exception is the stock
instrument catalog, profiles/opxy-1.2.0/content/op1_factory.json, kept for
development and checked against the profile's hash; never add other stock
files beside it. The docs use %LOCALAPPDATA%/OPXY-SDK/private/ (Windows) or
~/Library/Application Support/OPXY-SDK/private/ (macOS); pass external
input/output paths to build commands. Never force-add these files or bypass the commit/push guards.
