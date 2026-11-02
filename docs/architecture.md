# Architecture

How the SDK is put together: what a build is made of, how machines and
extensions reach stock firmware, and where new work belongs. It is an overview
that links to the pages owning each part; read it before changing the SDK
itself. Machine authors need only [your first machine](first-machine.md) and
the [machine API](api.md).

## From sources to firmware

A build combines the user's own stock OS with a collection of machines and
extensions. They live outside the SDK, in their authors' repositories; one
repository can hold several machines and extensions, as
[open-xy-core-machines](https://github.com/etcetc000/open-xy-core-machines) holds
the core machines and Capture. It never ships firmware: every
image starts from the `os.ldr` on the user's OP-XY.

```mermaid
flowchart LR
    subgraph sources["Sources"]
        machines["Machines<br/>(authors' repositories)"]
        extensions["Extensions<br/>(authors' repositories)"]
        catalog["Catalog<br/>catalog/*.json"]
    end
    stock["Stock os.ldr 1.2.1<br/>(the user's device)"]
    profile["Firmware profile<br/>profiles/opxy-1.2.1.json"]
    subgraph tools["Tooling"]
        native["sdk.py build<br/>(native)"]
        builder["Open-XY builder<br/>(browser, from a kit)"]
    end
    out["Firmware: os.ldr + content/<br/>(catalog, screens, presets)"]

    catalog --> machines & extensions
    machines & extensions --> native & builder
    profile --> native & builder
    stock --> native & builder
    native & builder --> out
```

- **Native builds** (`sdk.py build`, [building firmware](build.md)) compile the
  collection with the pinned Blackfin [toolchain](toolchain.md) and package it.
- **The browser builder** ([Open-XY](builder.md)) links a precompiled kit of
  catalog machines in the browser, for the selection a user picks.
- Both read the same [firmware profile](../profiles/opxy-1.2.1.json), whose
  digest each pins, so they apply exactly the same edits.

A build links every selected machine and extension into one **module**, placed
in a 2 MiB reservation taken from stock's heap. Everything else in the image
stays byte for byte stock, except the edits the profile reviews
([what a build changes](abi-1.2.1.md#what-a-build-changes)).

## Layers

The SDK keeps the firmware's internals away from machine and extension code.
Four layers, each depending only on the one below:

```mermaid
flowchart TB
    subgraph contributors["Machines and extensions (C++11, no firmware addresses)"]
        m["Machines: synths, sample engines, drum kits, effects, note players"]
        x["Extensions"]
    end
    subgraph public["Public interfaces (sdk/include/opxy/), stable across OS releases"]
        api["Machine API: callbacks per kind, display frames"]
        ev["Extension events, claims and actions"]
    end
    subgraph platform["Platform layer (sdk/platform/opxy-1.2.1/), one per OS release"]
        adapters["Adapters and generated tables"]
        services["Profile services: samples, drums, players, extensions, ..."]
    end
    subgraph sites["Patch sites (profiles/opxy-1.2.1.json), private to one OS release"]
        hooks["Reviewed hooks, table and text edits"]
    end
    stockfw["Stock OS 1.2.1"]

    m --> api
    x --> ev
    api & ev --> adapters
    adapters --> services
    services --> hooks
    hooks --> stockfw
```

1. **Patch sites** are the places where stock code calls into the module:
   hooks, plus a few table and text edits. They belong to one OS release and are
   reviewed byte for byte; the SDK owns every site, each exactly once, and a
   build rejects two edits that overlap.
2. **The platform layer** turns those sites into something usable: adapters
   that speak stock's data structures, the tables a build generates for its
   machines, and the services below. Porting to a new OS release moves this
   layer and the profile ([porting](porting.md)); nothing above changes.
3. **Public interfaces** are what contributors write against: the
   [machine API](api.md) and [display API](display.md), and the
   [extension](extensions.md) events, claims and actions. They outlive an OS
   release.
4. **Machines and extensions** are ordinary C++11, with no firmware addresses,
   assembly, platform conditionals or OS headers.

[Firmware extensions](extensions.md#how-the-sdk-exposes-the-firmware) describes
the same split from the side of events: sites, events, subscribers.

## Profile services

A **profile service** groups the patch sites and SDK code one feature needs.
A build applies a service's edits, and links its code, only when a selected
machine or extension needs it; otherwise those sites stay stock. The profile
names each service and what turns it on: the machine kinds that use it, the
features machines declare (`trackControl`, `auxTrack`, `gesture` for a chord
claim), or `extension` when the build includes an extension.

```mermaid
flowchart LR
    tc["a track control machine"] --> control["control service"] & tick["tick service"]
    ax["a machine on an aux track"] --> aux["aux service"]
    s["a sample engine"] --> samples["samples service"] & octaves["octaves service"]
    d["a drum kit"] --> samples & drums["drums service"] & octaves
    p["a note player"] --> players["players service"] & octaves
    g["a machine with a chord claim"] --> keys["keys service"]
    e["any extension"] --> ext["extensions service"] & tick & keys
    control & tick & keys & aux & samples & drums & players & octaves & ext --> edits["their sites and code<br/>applied and linked"]
    none["none of these"] --> stockonly["those sites stay stock"]
```

[`tooling/native.py`](../tooling/native.py) chooses the services from the
selection (`service_users`) and the units each one links (`UNIT_SERVICES`);
the browser builder does the same from the kit.

A site belongs to exactly one service. When several features need the same site,
it gets a small shared service of its own, used by each of them, and a
dispatcher that calls each user the build links, in a fixed order. The GUI tick
is the first: the `tick` service, used by track control and by extensions. Its
dispatcher, `sdk_gui_tick`, is generated with the build's startup code, which
already stands in for the services a build leaves out; in a browser kit, a user
the selection leaves out resolves to a stand-in that does nothing. The key queue
is the second: the `keys` service, used by extensions and by chord claims
([keys and gestures](#keys-and-gestures)).

A feature that needs new sites
adds a service, or extends the one it belongs to, rather than editing builds
that don't use it.

"Service" also has a narrower meaning in the [machine API](api.md#services):
the optional resources a sample engine declares in `definition.json`. Those are
features of one machine; profile services are groups of firmware edits.

## At runtime

Stock firmware runs in two roles that exchange messages: the **audio side**
renders voices and effects, and the **GUI side** runs the screens, the
controls and the sequencer.

```mermaid
flowchart TB
    subgraph gui["GUI side"]
        loop["Main loop, every GUI tick"]
        tick["GUI tick (tick service):<br/>stock, track control, extensions' gui_tick"]
        ctrl["Controller update and sequencer<br/>extension events"]
        screens["Screens: machines' displays"]
        file["Background file task:<br/>project load and save"]
        loop --> tick --> ctrl
        loop --> screens
    end
    subgraph audio["Audio side"]
        voices["Voices and effects:<br/>machines' audio callbacks"]
    end
    gui -- "parameter values (messages)" --> audio
    audio -- "telemetry (LFO amounts, meters)" --> gui
```

- **Audio callbacks** (a voice's render, an effect's process) run on the audio
  side: bounded, with no allocation, blocking, logging, I/O or graphics.
- **Displays and extension events** run on the GUI side. Extension callbacks run
  on the controller path, in collection order ([events](extensions.md#events)).
- **Projects** load on stock's background file task, not in the GUI's main
  loop, while the loaded project isn't in use yet. Code that reacts to a load
  records what it needs there and acts on the next GUI tick
  ([project companions](abi-1.2.1.md#project-companions)). Treat saving the same
  way: it goes through stock's file layer too.
- **Values reach the audio side as messages.** The GUI side keeps each track's
  stored parameters (what a project saves) and live parameters (what plays,
  after locks and sequencing), and publishes live values to the audio side when
  they change.

## A parameter's values

Stock keeps two values for each track parameter: the **stored** one, which a
project saves and an encoder turn changes, and the **live** one, which plays,
after the sequencer's steps and parameter locks. The SDK's
[track control](api.md#track-control) adds layers above stock's live value,
in a fixed order, so one place decides what plays:

```mermaid
flowchart BT
    stored["Stored value<br/>(saved; encoder turns)"]
    live["Stock's live value<br/>(steps and parameter locks)"]
    hold["Silent capture's hold<br/>(what plays without the capture's edits)"]
    overlay["Machines' overlays<br/>(by track; the later track wins)"]
    plays["What plays and the page shows"]
    stored --> live --> hold --> overlay --> plays
```

- **Overlays** set what plays for one tick, never what is saved. Each machine
  adds every entry it still wants each tick; a parameter none overlays any more
  returns to the layer beneath. Machines read the value beneath their
  overlays, never each other's.
- **Capture** records encoder turns for one owner, a machine or an extension
  (through the `capture` action, which turns the control service on for its
  build). Audibly (`capture` 1), the
  turns play as usual. Silently (`capture` 2), a captured parameter is held at
  what stock would play without the turns, so the turns only move the stored
  value; machines read the held value, and their overlays play over it. When a
  capture ends, every captured parameter returns to its value from before it.
- **Stock's writes** pass through the layers too: a step's parameter lock
  becomes the value beneath an overlay instead of sounding over it, and, while
  capturing silently, plays and is held.

```mermaid
stateDiagram-v2
    [*] --> Idle
    Idle --> Capturing: a machine asks (capture 1 or 2), or an extension (Actions::capture, from a claimed key)
    Capturing --> Capturing: its owner asks again next tick (it may change 1 and 2)
    Capturing --> Idle: the owner stops asking, a machine leaves its track, or a project loads
    note right of Idle
        On ending, every captured parameter
        returns to its value from before it.
    end note
```

## Keys and gestures

Stock queues every key, the keyboard's included, and dispatches each from the
queue. The `keys` service takes that one call: its gesture layer sees each key
before stock does, so a combination can be the SDK's without stock acting on it
first.

```mermaid
flowchart LR
    queue["Stock's key queue"] --> layer{"Gesture layer<br/>(keys service)"}
    layer -- "a chord claim's held key:<br/>waits" --> wait["held back"]
    wait -- "released alone,<br/>or another key" --> next
    layer -- "a keyboard key while it waits,<br/>or while it is taken" --> owner["the claim's owner:<br/>an extension's gesture event,<br/>or a machine's next tick"]
    layer -- "any other key" --> next["extensions' wrapper:<br/>stock, then queued_control"]
```

- **A chord claim** names a held key and the keys it combines with
  (`metronome_chord`: the metronome and the keyboard). Its owner is the
  extension that holds it, else the first running, unmuted machine that does; a
  build refuses an extension and a machine holding the same one. Without an
  owner, keys pass untouched.
- **The held key's own action waits**: stock gets its press when it is released
  alone (the metronome page opens on release) or when another key comes, never
  after a chord. An owner can **take** it, renewing every tick, so its taps are
  the owner's too.
- **Chord keys never reach stock**, their releases included: no note plays and
  no drum pad is selected.

## Machines and extensions

| | Machine | Extension |
| --- | --- | --- |
| What it is | A sound or a source of notes on a track: synth, sample engine, drum kit, send effect, note player | A change to how the OP-XY itself behaves: recording, controls, the sequencer |
| Where it lives | On a track, chosen per pattern like a stock engine | Firmware-wide, in every project |
| What it supplies | Callbacks for its kind, a display and artwork ([API](api.md)) | Event callbacks (`Hooks`) and an `extension.json` ([extensions](extensions.md)) |
| How it acts | Through its kind's callbacks | Through actions it has permission for, inside a claim |
| Saved with a project | Its parameters, as stock saves an engine's | Nothing yet |
| Draws | Its own page | Nothing yet |
| Example | [2op FM](https://github.com/etcetc000/open-xy-core-machines/tree/main/machines/fm2op) | [Capture](https://github.com/etcetc000/open-xy-core-machines/tree/main/extensions/capture) |

Both reach builds through the [catalog](catalog.md) and are chosen in the
builder; a build lists its extensions after its machines, which is also the
order of their callbacks.

## Startup

```mermaid
sequenceDiagram
    participant Stock as Stock firmware
    participant SDK as SDK startup (module)
    participant M as Machines
    Stock->>SDK: synth initializer's exit (hook)
    SDK->>M: register every effect, then on the audio side every synth and sample engine
    Stock->>SDK: screen factory's exit (hook), GUI side
    SDK->>M: construct each machine's screen, check its artwork
    SDK->>M: register a synth once its screen is ready
    Stock->>SDK: catalog insertion (hook)
    SDK-->>Stock: a machine's menu entry, only while its engine and screen are ready
```

The details, including the startup label and the catalog, are in the
[firmware contract](abi-1.2.1.md#startup).

## Changing the SDK

- **A new machine or extension** never changes the SDK; it lives in its author's repository
  ([your first machine](first-machine.md)).
- **A new capability for machines** adds to the public interface, an adapter in
  the platform layer, and, if it needs stock to call new code, sites in the
  profile, grouped as a service that only its users turn on.
- **A new capability for extensions** reuses or widens an event before adding
  one, and adds claims and actions to the registry
  ([changing the SDK for an extension](extensions.md#change-the-sdk-for-an-extension)).
- **A new site** is reviewed against stock byte for byte, and must not overlap
  another; sites are private, so a public interface never names one.

Keep the docs at their owning layer: this page links rather than repeats them.
When a change adds a layer, a service or a runtime path, update the diagrams
here.

## Glossary

| Term | Meaning |
| --- | --- |
| Machine | A synth, sample engine, drum kit, send effect or note player, built from its author's repository |
| Extension | Firmware-wide behavior built from its author's repository, driven by events |
| Module | The selected machines, extensions and SDK code, linked together into the reservation |
| Profile | [`profiles/opxy-1.2.1.json`](../profiles/opxy-1.2.1.json): stock hashes, the reservation, identity ranges and every reviewed edit |
| Site, hook | A place in stock code that calls into the module; a hook replaces a reviewed call or jump |
| Profile service | A group of sites and SDK code applied only when a selected machine or extension needs it |
| Event | A public callback that something happened in stock, delivered to extensions |
| Claim | An exclusive gesture on a key, owned by one extension per build; a chord claim (a held key with others) may be a machine's instead |
| Action | Permission to change stock state, granted inside a claim |
| Kit | A precompiled collection the browser builder links from |
| Role (side) | Where code runs: the audio side or the GUI side |
