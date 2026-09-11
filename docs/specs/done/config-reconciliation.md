# Config Reconciliation — firmware side (`BOOT`/`SYNC`/`UPDATE`, fingerprints, confirmed delivery)

> Status: **Phase 1 complete; Phase 3 underway** (atomicity + boot-time revert + rejection
> reporting on `BOOT` and that boot's `SYNC`, `device` in `SYNC`, full `UPDATE` staging for both
> device-changed and functions-only changes, and dropping flat device-config storage entirely have
> all landed; device-config authority transfer (retiring `nvs/write` as a write mechanism) and
> cause-classified rejection codes remain). See the
> progress checklist below, and [`Configuration.md`](../Configuration.md) for how the current implementation
> actually behaves. This is the firmware counterpart to the server-side spec in the app repo,
> [`cornucopia-app/docs/specs/config-reconciliation.md`](../../../cornucopia-app/docs/specs/config-reconciliation.md).
> **Read that spec first** — it owns the protocol design, the *why* behind every decision, and the
> server/UX half. This document is the firmware implementation plan.
>
> **Prerequisite:** [`configuration-as-schema.md`](configuration-as-schema.md) — the `Configuration`/`Property`
> teardown. This plan **assumes that refactor is already done**: configuration bodies are stored and echoed as
> **verbatim JSON**, and persistence is separated from parsing. Land that first on its own branch, then build
> this on top.
>
> The one-line summary of the firmware's job: **persist each configuration as `(data, fingerprint, requestedAt)`,
> echo the fingerprints back in a `SYNC` manifest, apply configuration live (function-only change) or across a
> reboot (device change), and report what is actually *running* — never compute a fingerprint, only parrot it.**

## What the firmware is responsible for (and what it is not)

The server is the **authority**. It decides what a device should run, holds `requested`/`confirmed` state,
derives "pending", runs the retry/deadline policy, and owns the UX. The firmware's contract is deliberately
dumb and stateless-by-comparison:

- **Store what it is handed, verbatim.** Each `UPDATE` carries, per configuration, a body plus an opaque
  `fingerprint` and a `requestedAt` timestamp. The firmware persists the three together and **never hashes
  anything** — the fingerprint is a token it echoes, so a serialization mismatch can never cause an
  apply-resend-apply livelock (server spec, *"The identity of a revision"*). Storing the body verbatim is what
  makes this safe (see [`configuration-as-schema.md`](configuration-as-schema.md)).
- **Skip what it already has.** If an `UPDATE` names a configuration whose `fingerprint` the device already
  holds, that entry is a no-op. If **nothing** in an `UPDATE` differs, the whole message is ignored — no
  re-apply, no reboot.
- **Report what is running, not what is held.** `SYNC` is a manifest of the fingerprints of the configurations
  the device has **applied and booted with** — proof-of-apply, not proof-of-receipt. It is built from the
  **`confirmed`** configuration set (see *Storage*), and only sent **after a fully successful boot**.
- **Keep running without the network.** The device applies its last-known-good (`confirmed`) configuration at
  boot regardless of connectivity, and reconciles with the authority only when a connection exists. An
  unreachable server never stalls the device.

Everything about *when to retry*, *what "pending" means*, and *what the user sees* lives on the server.

### New-protocol only — no dual stack, no legacy compatibility

This firmware speaks **exactly one protocol: the new one.** There is no in-firmware compatibility with the old
`init`-at-boot / retained-`config`-topic protocol, and we only ever talk to a server that speaks the new
protocol end-to-end. This removes the "awkward hybrid" sequencing constraint the earlier draft worried about:
the `BOOT`/`SYNC` split, the `UPDATE` channel, and device+function reconciliation all land **together**, in one
self-consistent build. Coordinate the cutover so the server is on the new protocol before the first
new-protocol device connects.

## Naming: `settings` → `device configuration`

As part of this work we **rename what the code currently calls (device) `settings` to "device configuration."**
Today `DeviceSettings` ([`DeviceSettings.hpp`](../../components/devices/src/devices/DeviceSettings.hpp)) holds
the `peripherals`/`functions` lists (which determine *which functions and peripherals exist*) plus device-wide
tunables, loaded from the `device-config` key. Under reconciliation this is just **one more configuration
document** — the `device` configuration — that carries a `fingerprint` like any other. The rename aligns the
code with the protocol vocabulary (`device` vs. per-function configurations) and removes the "settings vs.
config" ambiguity. This is tracked as its own checklist item because it touches type names, log strings, and
the boot path broadly.

## Where the firmware is today (grounding)

Concrete anchors this plan builds on — verify these still hold before starting:

- **`init` is published once per boot** in
  [`Device.hpp` `startDevice()`](../../components/devices/src/Device.hpp). It carries device identity, the full
  device configuration document, diagnostics (`reset`/`wakeup`/`bootCount`/`time`), an `InitState`, the full
  per-peripheral/per-function init JSON (including `params`/`config`/`error`), and a crash report.
  → This splits into **`BOOT`** (diagnostics + per-peripheral/function `error` feedback, **no configuration
  bodies**) and **`SYNC`** (fingerprint manifest).

- **Function configuration arrives on a retained topic and is applied live.**
  [`Function.hpp` `makeFunctionFactory`](../../components/functions/src/functions/Function.hpp) subscribes each
  function to `functions/<name>/config`; on a message it persists then calls `configure(...)`. This retained
  subscription is **removed**; `UPDATE` becomes the only config-in path. The live `configure(...)` call is the
  exact hot-reload path `UPDATE` reuses.

- **Config is persisted as a bare body in NVS**, and `NvsConfiguration` couples parse and persist — both
  addressed by the [`configuration-as-schema.md`](configuration-as-schema.md) prerequisite and the new
  per-configuration store below.

- **Device configuration** is loaded at boot from `device-config`; function configuration lives per-function in
  the `function-cfg` namespace. **Network configuration** (`network-config`: MQTT broker, WiFi, instance) is
  **separate and out of scope** — it is provisioned independently, must survive across everything here, and is
  never part of the reconciled set.

- **`FunctionManager`** ([`Function.hpp`](../../components/functions/src/functions/Function.hpp)) creates
  functions from the device configuration's `functions` list and holds their handles. It becomes the
  **`FunctionRegistry`** — the in-memory source of truth for `name → {live function, fingerprint}` (see below).

- **MQTT topic model.** [`MqttRoot`](../../components/kernel/src/mqtt/MqttRoot.hpp) roots every device at
  `…/devices/ugly-duckling/<instance>` and offers `publish`/`subscribe`/`clear`/`registerCommand`. `boot`,
  `sync`, `update` sit **directly under the device root**, orthogonal to `commands`/`responses`.
  `UPDATE`/`SYNC` deliberately do **not** reuse the command channel (server spec, *"`UPDATE` is not a
  command"*).

- **Connection lifecycle.** [`MqttDriver`](../../components/kernel/src/mqtt/MqttDriver.hpp) forces a **clean
  session on every (re)connect** — the persistent-session path is disabled (`nextSessionShouldBeClean` stays
  true; see the `TODO` around the `Connected` handler). There is **no application-level "connection
  established" callback** today; the `SYNC` trigger needs one (see *SYNC*).
  [`states->kernelReady`](../../components/kernel/src/KernelStatus.hpp) is set at the end of a successful boot
  and is the "boot succeeded" signal the SYNC task gates on.

- **Fingerprints are never computed on-device.** The device stores and echoes the server's token verbatim.

## Storage: verbatim envelopes in swappable `confirmed`/`requested` namespaces

### The per-configuration envelope

Each configuration (the `device` document and each function) is stored as one **verbatim envelope**:

```jsonc
{
  "data": { /* the configuration body, byte-for-byte as the server sent it */ },
  "fingerprint": "…hex…",                // opaque token from the authority; never recomputed
  "requestedAt": "2026-07-30T12:34:56Z"  // authored-at stamp, echoed back; encoded exactly like a
                                         // plot-controller schedule `start` (ISO 8601 string), not interpreted
}
```

A small **per-configuration store wrapper** (`StoredConfig`) owns reading/writing one such envelope in NVS and
exposing `data` (verbatim) + `fingerprint` + `requestedAt`. This is **separate from parsing** — it never
constructs a `Configuration`; parsing the `data` into a typed snapshot is a distinct step
([`configuration-as-schema.md`](configuration-as-schema.md)). `NvsConfiguration`'s old parse-and-persist
coupling is retired in favor of this split. One `nvs_set_blob` per envelope keeps `data`/`fingerprint`/
`requestedAt` inseparable (per-configuration atomicity).

The **device configuration** is held at runtime by its own `StoredConfig` (loaded at boot from the `confirmed`
slot). That in-memory object is the **source of truth for the `device` manifest entry's fingerprint and
`requestedAt`** — the single-document analogue of `FunctionRegistry` (below), which is the source of truth for
every function's fingerprint. The `SYNC` builder reads both, never re-deriving anything from NVS at publish
time.

Because no legitimate configuration body has top-level `data` or `fingerprint` keys, the envelope shape is
unambiguous. `StoredConfig` uses this to detect a **legacy bare body** (pre-reconciliation firmware wrote the
body directly under the key, no wrapper) and adopts it as a one-time bridge — see *Migration* → "Reading a
legacy bare body."

### Two config-set slots + a state pointer

The reconciled set (the `device` envelope + one envelope per function, keyed by function name) lives in a
**slot namespace**. There are two interchangeable slots — `a` and `b`, say — and a separate small
**`config-state`** namespace that records:

- `confirmed`: which slot (`a`/`b`) holds the last-known-good set. Always present after first provisioning.
- `requested`: `{ slot, state }` for a new-but-unconfirmed set, or absent. `state ∈ { pending, attempted,
  rejected }`.
- `rejection`: an optional stored `google.rpc.Code` to report on the next `BOOT` and that boot's next
  `SYNC` (Phase 3+; see *Rejection*).

Each slot namespace holds a **full** set (device + every function), so a slot is self-contained: a boot loads
exactly one slot and never merges. Network configuration stays in its own stable namespace and is never
swapped.

> **Phase 1 note.** The `requested` slot + state machine below is the **atomicity mechanism** and lands in
> Phase 3. In Phase 1 there is a single `confirmed` slot and no revert: an `UPDATE` writes through and (if
> device config changed) reboots; a boot that then fails is unrecoverable **exactly as today**. The full
> two-slot design is documented here as the end goal so the Phase 1 shape is chosen to grow into it.

## How reconciliation works (end-goal design)

### Receiving an `UPDATE`

1. **Filter by fingerprint.** For each named configuration (`device` and/or functions), drop entries whose
   `fingerprint` already matches the `confirmed` slot. **If nothing differs, ignore the `UPDATE` entirely.**
2. **Stage into `requested`.** Take the free slot (the non-`confirmed` one), copy every unchanged configuration
   from `confirmed`, and overwrite the changed ones with the new envelopes. Mark `requested = { slot, pending }`.
3. **Apply:**
   - **Device configuration changed → reboot.** Do not hot-reload; let the boot sequence apply the whole
     `requested` set (peripherals ride inside the device document — a device change can restructure which
     functions exist).
   - **Only function configurations changed → hot-reload.** Mark `requested` `attempted`, then for each changed
     function call `FunctionRegistry.applyLive(name, envelope)` → `configure(...)` (the envelope is already
     persisted, by step 2, so this only applies it to the running function). On success of all, **commit**
     (`requested` becomes `confirmed`, old `confirmed` dropped, `requested` cleared). On any failure, mark
     `rejected` and **reboot** (boot reverts to `confirmed`).

In **Phase 1**, steps 2–3 collapse: write the changed envelopes straight into the single `confirmed` slot; if
the device document changed, reboot; otherwise hot-reload the changed functions. No `requested`, no revert.

### Boot

Read `config-state`, then:

- **No `requested`** → load `confirmed`. Boot. If boot fails, there is no recourse — fail as today (best-effort
  try to get logs out over MQTT, but be prepared that MQTT may also be unavailable).
- **`requested` == `pending`** → mark `attempted`, load `requested`. On a detected failure → mark `rejected`
  and reboot (next boot reverts). On full success → **commit** (`requested` → `confirmed`).
- **`requested` == `attempted`** (we started applying but crashed before committing or gracefully rejecting) or
  **`rejected`** → record the rejection for the next `BOOT` and that boot's next `SYNC` (see *Rejection*'s
  design amendment below), load `confirmed`, drop `requested`.

So a bad `requested` set costs one extra reboot and always lands back on the last-known-good `confirmed` set.
This is the atomic-revert mechanism; it is **Phase 3**. Phase 1 has only the "no `requested` → load
`confirmed`" branch.

**Commit is a single pointer flip.** "Commit" means: flip `config-state.confirmed` to point at the slot that
was `requested` — **one NVS write, and that write is the commit point.** Everything after it (dropping the old
slot, clearing `requested`) is idempotent cleanup that the next boot can re-derive, so a crash mid-cleanup is
harmless: a boot that finds `confirmed` already pointing at the former `requested` slot simply finishes the
cleanup.

**What counts as boot success vs. failure** differs by which set is loaded:

- **Booting the `confirmed` set is best-effort.** A peripheral or function whose apply errors is *recorded*
  (the `InitState`/`error` feedback that rides on `BOOT`, as today), but boot still completes, `kernelReady` is
  set, and `SYNC` is sent. There is no recourse — the last-known-good set is all we have.
- **Booting a `requested` set is strict.** *Any* peripheral/function apply error (or a crash before boot
  completes, i.e. `requested` still `attempted`) is a **detected failure** → mark `rejected`, reboot, revert to
  `confirmed`. Only an error-free boot of a `requested` set commits and lets `SYNC` fire.

### `FunctionRegistry` — the in-memory source of truth

`FunctionManager` becomes `FunctionRegistry`. It owns, per live function, `{ handle, fingerprint }`:

- **Populated at boot** as each function is created from the loaded slot: the registry records the function's
  fingerprint from its envelope once `configure(...)` **succeeds** (proof-of-apply, not proof-of-receipt).
- **`applyLive(name, envelope)`** — the hot-reload entry point `UPDATE` calls: parse, `configure(...)`, and on
  success update the stored fingerprint. Persistence is not this method's job — by the time it's called, the
  envelope is already persisted as part of writing the whole staged slot (see *Receiving an `UPDATE`* above) —
  so this only applies the change to the already-running function. This apply-and-track-fingerprint logic
  originally lived inside the per-function `functions/<name>/config` subscription lambda; it **moved out of the
  factory into the registry**, so there is one apply path shared by boot and `UPDATE`, and one place that knows
  every function's current fingerprint.
- **`manifest()`** — yields `name → fingerprint` for the `SYNC` builder, straight from in-memory state (never
  re-reading NVS, so it reflects what actually applied).

A configuration for a function **not defined in the (potentially updated) device configuration** is a
**protocol violation**, treated with the same seriousness as a malformed body — the server should never send
one. It is a faulty configuration → `INVALID_ARGUMENT` once rejection exists (Phase 3); before then it is
simply unhandled, exactly like any other faulty configuration today. Note the check is against the *post-update*
device configuration: an `UPDATE` that introduces a new function bundles the device-configuration change that
defines it, which reboots and rebuilds the registry, so the function *is* defined by the time its config
applies.

### `SYNC`

- **Trigger: any successful MQTT connection**, delivered through a **single-element overwrite queue** into a
  dedicated SYNC task. Coalescing means a flurry of reconnects (or a post-`UPDATE` request) collapses to one
  pending SYNC — no pile-up if the link is flaky. (Explicit rate-limiting/throttling beyond the overwrite queue
  is deemed unnecessary for now.)
- **Gate: only after a fully successful boot.** The SYNC task awaits `kernelReady` (all functions configured
  successfully) before publishing. If MQTT connects early, SYNC waits. If boot is **unsuccessful** with a
  `requested` set, the device reboots to revert and **never sends SYNC** for that failed attempt.
- **There is no boot-time / `startDevice()` SYNC.** With nothing to sync against absent a connection, the
  connect trigger is the only trigger; the connection-established hook covers first boot and every reconnect
  uniformly.
- **Payload:** `{ configurations: { device: {fingerprint, requestedAt}, <function>: {fingerprint, requestedAt},
  … }, rejection? }` built from the `confirmed` slot / registry. `NoRetain, QoS 2`. `rejection` is present only
  on the first `SYNC` published after a revert (see *Rejection* below) — it rides alongside `BOOT`, not instead
  of it.

The **connection-established hook** is new surface on `MqttDriver`: a registered callback (analogous to how
commands are registered), fired from the `Connected` event. Because that event runs on the MQTT event-loop task
and `publish()` enqueues onto that same task and waits, the callback **must not publish inline** — it only
pushes to the overwrite queue; the separate SYNC task does the publish.

### `BOOT`

- Queued once at boot (diagnostics + per-peripheral/function `error` feedback; **no configuration bodies**),
  delivered when MQTT connects. `NoRetain, QoS 2`.
- If the device reboots due to a faulty `requested` configuration **before** MQTT is established, that `BOOT`
  never reaches the server — accepted; the *next* boot (which reverts to `confirmed` and always publishes
  `BOOT`) is what informs the server, carrying the rejection code (see *Rejection*).
- **Rejection reporting rides on `BOOT`, and that same boot's `SYNC`.** A `rejection` code stored in
  `config-state` is included in this device's `BOOT` payload as `rejection` (a `google.rpc.Code` int) whenever
  one is set — whether recorded by this exact boot's revert or an earlier, still-unreported one — and cleared
  from `config-state` immediately after being read (report-once, at the persistence layer). `BOOT` carries it
  unconditionally because it fires deterministically on every boot, including the revert boot itself, without
  waiting on `kernelReady` or a live MQTT connection the way `SYNC` does. The same code also rides on the
  first `SYNC` this boot publishes, if any — an in-memory copy is handed to the SYNC task and consumed after
  its first publish, so a later `SYNC` in the same boot session doesn't repeat it.

### QoS 2 + clean session — an explicit assumption

> **Superseded in part by issue #634.** `boot`, `sync`, `telemetry` and `responses/*` are now published at
> **QoS 1**: the broker downgrades to the subscriber's QoS on the next hop, so QoS 2's guarantee never
> reached the server anyway, and the duplicate-delivery hazard described below is now handled by
> server-side dedup on the device's own timestamp instead. `log` stays at QoS 2 for now — not for the
> reason given below, but because its blocking publish is what currently orders log records; issue #635
> replaces that with an explicit sequence. The `update` **subscription** is now QoS 1 too — subscription QoS
> is only a ceiling, and UPDATE is idempotent under a duplicate delivery — while `commands` stays at QoS 2
> until responses carry a correlation id (cornucopia-app#508). None of that touches the clean-session
> reasoning in this section, which is about offline queueing rather than QoS.

`update` is subscribed and `sync` published at **QoS 2**. The MQTT driver forces a **clean session on every
reconnect**, so an `UPDATE` published while the device is offline is **not broker-queued** and is lost.
**This is by design and fine:** delivery of `UPDATE` is never relied upon. The device's `SYNC` on the next
successful connection re-advertises its `confirmed` fingerprints, and the server re-pushes whatever differs.
Reconciliation converges through SYNC-driven re-push, not through MQTT delivery guarantees.

Every other outbound topic (`boot`, `telemetry`, `log`, `responses/*`) is QoS 2 too, for a different reason
than reconciliation convergence: esp-mqtt's outbox resends an unacked PUBLISH verbatim (same packet id, DUP
set) if the ack doesn't arrive within its retransmit timeout while the connection stays up, and QoS 2's
packet-id-keyed handshake is what stops the broker from fanning that resend out to subscribers a second time.
At QoS 1 (`telemetry`'s setting until issue #579) the broker has no such obligation, so a resend under a flaky
link surfaces as a genuine duplicate delivery — harmless for continuously-sampled readings, but not for
delta-since-last-report values (e.g. flow-meter volume) whose on-device counter is reset immediately after
being read.

### Rejection

Rejection is a single `google.rpc.Code` on `BOOT` (and that boot's `SYNC`) (`INVALID_ARGUMENT`=3, `FAILED_PRECONDITION`=9,
`RESOURCE_EXHAUSTED`=8, `UNIMPLEMENTED`=12, `INTERNAL`=13; `OK`=0 is never sent — a matching fingerprint *is*
success). No message travels on the wire beyond that int; human-readable detail goes to the `log` channel and
an operator correlates by device + time.

> **Design amendment (landed with atomicity):** the original draft of this section specified reporting on
> `SYNC` only. Once the two-slot machine and boot-time revert made a rejection code exist to report at all,
> `BOOT` was added as a second, unconditional channel — see *`BOOT`* above for why — since `SYNC` isn't
> guaranteed to fire at all for a revert boot (no MQTT connection, or a crash before `kernelReady`). `SYNC`
> keeps its rejection field: the first `SYNC` published after a revert still carries the same code, alongside
> `BOOT`, not instead of it.

**Persistence + report-once is done; cause classification is not.** `config-state.rejection` is stored across
the revert reboot and reported exactly once, on the next `BOOT` and that boot's next `SYNC`, then cleared —
that part landed alongside the two-slot machine and boot-time revert (below). What's still open: every
rejection is currently reported as `INTERNAL`, regardless of cause. The classification this section originally
specified — parse/validation → `INVALID_ARGUMENT`, unknown function type → `UNIMPLEMENTED`, NVS-full →
`RESOURCE_EXHAUSTED`, else `INTERNAL` — needs a typed error path through peripheral/function creation that
doesn't exist yet, and remains a Phase 3 checklist item.

## Migration

There is **no in-place migration of the configuration model, and no bridge from any older on-device
storage shape.** Migrating a real device to this firmware — whether it's upgrading from Phase 1's flat
`device-config`/`function-cfg` storage, from firmware that predates config reconciliation entirely
(a bare, unwrapped body under those same keys), or is a factory-fresh device with nothing written at
all — means the device boots **as if freshly minted**: no confirmed device or function configuration,
defaults only, no functions. It reports this via an empty (or minimal) `SYNC`, which prompts the
server to re-push the full configuration set via `UPDATE`, exactly like a brand-new device's first
reconciliation.

**This is deliberate, not a temporarily-accepted shortcut.** Fingerprinting is exclusively the server's
job (see "Fingerprints are never computed on-device" above), so the firmware has no legitimate way to
synthesize a fingerprint for configuration it already holds locally under some older shape — the only
way to get a real fingerprint is to receive one from the server. Given that standing invariant, there
is no version of "migrate the old bytes forward" that doesn't involve minting a fingerprint the
firmware isn't allowed to mint, so reconciling from empty is not a fallback of last resort, it's the
only correct answer.

### A missing/absent `confirmed` slot is the one bootstrap path

A missing `config-state` namespace, or one with `confirmed` absent, covers every case that isn't an
already-slotted device with a confirmed set: an actual first boot with nothing ever written; a device
upgrading from Phase 1 firmware (flat `device-config`/`function-cfg` keys, no `config-state` namespace
at all); and a device running firmware from before config reconciliation existed (a bare body with no
envelope wrapper, again no `config-state`). All three collapse to the same handling: boot with
defaults and no functions, send an empty/minimal `SYNC`, let the server re-push the full device +
function configuration set. **The device never reads old-format storage to bootstrap itself** — not
the flat Phase 1 keys, not a bare pre-reconciliation body. Whatever is sitting under those old keys is
simply never looked at again.

This means such a device runs with **no device/function configuration** (defaults only) for the window
between boot and the server's re-push + reboot. **We accept this as a cost of keeping the design
simple:** a firmware update already takes the device offline for a while (HTTP update requires
connectivity and reboots), so a short additional reconfiguration round-trip is not a meaningful
regression.

### Generated NVS (`gen_config_nvs.py`) no longer seeds a device configuration

`scripts/gen_config_nvs.py` (and its `test/e2e-tests` copy, a symlink to the same file) used to write
`config/device-config.json` into the flat `device-config` key as a bare body. Since firmware no longer
reads that key under any circumstance (see above), the script no longer writes it either — a
generated/erased partition now has no confirmed slot and reconciles from the server via the same
empty-slot bootstrap as any other missing configuration.
`config-templates/*device-config*.json` remain on disk as schema/fixture examples; they just aren't
NVS-seeded any more.

---

## Progress checklist

These phases are the firmware's own; they do **not** have to line up with the server spec's phase numbers.
Because there is no legacy dual stack, the firmware's new-protocol surface (`BOOT`/`SYNC`/`UPDATE`,
drop-retained, device+function reconciliation) lands as **one milestone** in Phase 1; atomicity + rejection +
device-configuration *authority transfer* land in Phase 3.

### Phase 0 — prerequisite

- [x] **`Configuration`/`Property` teardown** per [`configuration-as-schema.md`](configuration-as-schema.md):
      verbatim-JSON storage, persistence split from parsing, `store()` removed once `BOOT` stops echoing config
      bodies. **Do this first, on its own branch.** Landed in [#597](https://github.com/cornucopia-machines/ugly-duckling-firmware/pull/597).

### Phase 1 — the new protocol, happy path (single `confirmed` slot, no atomicity)

- [x] **Rename `settings` → device `configuration`.** `DeviceSettings` and the `device-config` load path,
      including type names and log strings. The `device` document becomes a reconciled configuration with a
      fingerprint like any other. `DeviceSettings` → `DeviceConfiguration` (type, file, and every derived local
      variable/parameter) across the boot path and per-device factories. The `init` message's wire-format
      `"settings"` JSON key is intentionally left alone here — it disappears entirely once the "Split `init`
      into `BOOT` + `SYNC`" item below removes configuration bodies from `init`.
- [x] **Per-configuration envelope + store.** Introduce the verbatim `{data, fingerprint, requestedAt}`
      envelope and a `StoredConfig` wrapper (read/write one envelope, expose `data`/`fingerprint`/`requestedAt`),
      separate from parsing. `requestedAt` is an ISO 8601 string (encoded like a schedule `start`).
      `ConfigEnvelope` (verbatim envelope + its `ArduinoJson::Converter`) and `StoredConfig` (NVS-backed wrapper,
      built on `NvsStore::get<ConfigEnvelope>`/`set<ConfigEnvelope>`) landed in
      `components/kernel/src/{ConfigEnvelope,StoredConfig}.hpp`. Not yet wired into `DeviceConfiguration`
      loading or function configuration at the time this item landed — function configuration got it via the
      `FunctionRegistry` item next, and `DeviceConfiguration` loading got it as part of the `…/update`
      subscription + handler item below (it needed a real device fingerprint to filter against).
- [x] **`FunctionRegistry`.** Evolve `FunctionManager` into the in-memory `name → {handle, fingerprint}` source
      of truth. Move the hot-reload logic out of the per-function `config` subscription into
      `reconfigure(name, envelope)`; record fingerprints on successful `configure(...)` at boot and on reload;
      expose `manifest()`.
      `FunctionManager` renamed to `FunctionRegistry` in `components/functions/src/functions/Function.hpp`;
      boot-time function config loading switched from bare-body `NvsConfiguration` to envelope-based
      `StoredConfig`, so the registry has a real fingerprint to record at creation. `reconfigure(name, envelope)`
      and `manifest()` land as designed, but aren't yet called by anything live — the `…/update` handler (next
      item) is what calls `reconfigure()` for real. The apply-and-track-fingerprint bookkeeping itself was
      split into `FunctionConfigTracker` (no NVS dependency, unit-tested with a fake `configureFn`), matching
      `ConfigEnvelope`/`StoredConfig`'s codec/IO split, since `FunctionRegistry` itself can't be built natively.
- [x] **`…/update` subscription + handler.** Subscribe the device root to `update` (`NoRetain, QoS 2`). Parse
      `configurations`; filter by held fingerprints (ignore the whole message if nothing differs). Persist
      changed envelopes to the (single) `confirmed` slot. **Device changed → reboot; functions-only changed →
      hot-reload via `FunctionRegistry.reconfigure`.** A config for a function not defined by the (post-update)
      device configuration is a faulty configuration (unhandled in Phase 1, exactly like a malformed body).
      The fingerprint-skip filter landed as a pure, NVS/MQTT-free function, `filterUpdate()` in
      `components/kernel/src/UpdateFilter.hpp`, unit-tested directly (native `unit-tests`) — mirroring the
      `FunctionConfigTracker` split. The handler itself lives in `registerUpdateHandler()` in
      `components/devices/src/Device.hpp`: it builds a `name → fingerprint` map from `FunctionRegistry.manifest()`
      plus the device configuration's own fingerprint, runs it through `filterUpdate()`, and branches on
      `deviceChanged`. Device configuration is now itself loaded at boot through a `StoredConfig` (the
      `"device-config"` key holds a verbatim envelope, not a bare body, closing the "not yet wired into
      `DeviceConfiguration` loading" gap noted in the `StoredConfig` item above) — this is what gives the
      handler a real confirmed device fingerprint to filter against, even though reporting it on `SYNC` is
      still Phase 3. On a device change, every changed entry (device *and* any bundled function envelopes) is
      persisted verbatim via `StoredConfig`/`FunctionRegistry.persist()` — deliberately skipping
      `FunctionRegistry.reconfigure()`'s live-apply step, since a device change reboots and boot re-derives
      everything from NVS — then `esp_restart()`. On a functions-only change, each entry goes through
      `FunctionRegistry.reconfigure()`; a throw (bad body, or a function name the current device configuration
      doesn't define) is caught and logged per-entry rather than crashing the MQTT dispatch task, matching how
      the old retained-topic subscription handled it before this rewrite.
- [x] **`StoredConfig` reads a legacy bare body.** A blob with no `data`/`fingerprint`/`requestedAt` wrapper
      (pre-reconciliation firmware, or `gen_config_nvs.py`'s still-unupdated `device-config` seeding — see
      *Migration*) is adopted verbatim as `data` with an **empty fingerprint** and immediately re-persisted as a
      proper envelope, so the bridge fires at most once per device. `StoredConfig`'s constructor now reads the
      raw JSON first and branches on `is<ConfigEnvelope>()` (`components/kernel/src/config/StoredConfig.hpp`) instead
      of always trusting `NvsStore::get<ConfigEnvelope>()`, which previously parsed a bare body into a
      *valid-looking but silently empty* envelope with no error and no log distinguishing it from a legitimately
      empty one. Tested natively (`ConfigEnvelopeTest.cpp`, shape detection) and against real NVS
      (`StoredConfigTest.cpp`, adopt-then-normalize round trip).
      **Removed** once flat device-config storage was dropped entirely (see the functions-only
      atomicity item under Phase 3 below) — `StoredConfig` no longer branches on shape at all, since
      every reader of a slotted namespace is this firmware's own `StoredConfig.store()`, always
      envelope-shaped. `ConfigEnvelope::checkJson`/`is<ConfigEnvelope>()` and the corresponding tests
      were removed along with it.
- [x] **Split `init` into `BOOT` + `SYNC`.** `boot` keeps all diagnostics + per-peripheral/function `error`
      feedback and **drops all configuration bodies** (`NoRetain, QoS 1` at the time; bumped to `QoS 2` by
      issue #579, see *QoS 2 + clean session* above). `sync` is the fingerprint manifest
      from the `confirmed` slot / registry (`NoRetain, QoS 2`).
      `startDevice()` in `components/devices/src/Device.hpp` now publishes `boot` instead of `init`, and the
      publish body no longer echoes the device configuration (`json["settings"]` dropped). The per-peripheral/
      function init JSON also stopped echoing configuration bodies: `SettingsBasedManager::createFromSettings`
      (`components/kernel/src/Manager.hpp`) no longer sets `initJson["params"]`, and `makeFunctionFactory`
      (`components/functions/src/functions/Function.hpp`) no longer echoes the function's `config` body — both
      still report `name`/`type`/`factory`/`error` as before. `sync` is built by a new `publishSync()` in
      `Device.hpp` from `FunctionRegistry::manifest()`. To carry `requestedAt` alongside each fingerprint (the
      payload shape in *SYNC* below), `FunctionConfigTracker` (`components/functions/src/functions/
      FunctionConfigTracker.hpp`) now tracks `requestedAt` per entry and `manifest()` returns
      `name -> FunctionManifestEntry {fingerprint, requestedAt}` instead of a bare fingerprint string;
      `FunctionRegistry::reconfigure()`/`createFunction()` and `registerUpdateHandler`'s held-fingerprint lookup
      (`Device.hpp`) were updated for the new shape. **The `device` entry is deliberately not in `sync` yet** —
      reporting it is Phase 3 (see the `…/update` handler note above); `sync` in Phase 1 only reports functions.
      `FunctionConfigTrackerTest.cpp` updated for the `requestedAt`-carrying signatures.
- [x] **Connection-established hook + SYNC task.** Add a registered on-connect callback to `MqttDriver` (fires
      from the `Connected` event; must not publish inline). It pushes to a **single-element overwrite queue**; a
      dedicated SYNC task takes from it, **awaits `kernelReady`**, then publishes `SYNC`. Also publish `SYNC`
      immediately after a successful hot-reload `UPDATE` (via the same queue). No `SYNC` from `startDevice()`.
      `MqttDriver::onConnected(callback)` (`components/kernel/src/mqtt/MqttDriver.hpp`) queues a
      `ConnectedListenerRegistration` event through the existing `eventQueue` (same pattern as `subscribe()`,
      so registration is race-free against the event-loop task); the `Connected` visitor invokes every
      registered listener after processing resubscriptions, on every (re)connect. In `Device.hpp`,
      `syncTriggerQueue` is a `CopyQueue<bool>` of capacity 1, and the connected-listener callback only calls
      `syncTriggerQueue->overwrite(true)` — never publishes inline. `initSyncTask()` runs a dedicated
      `Task::loop` that takes from the queue, awaits `states->kernelReady`, then calls `publishSync()`.
      `registerUpdateHandler`'s functions-only (hot-reload) branch also overwrites `syncTriggerQueue` after
      applying its changed entries, so a successful `UPDATE` re-advertises fingerprints without waiting for a
      reconnect; the device-changed branch reboots instead, so `sync` follows from the next boot's
      connection-established trigger, not from this handler.
- [x] **Drop the retained `config` subscription.** Remove `functions/<name>/config` in `Function.hpp`; `UPDATE`
      is the only config-in path.
      Removed together with the `FunctionRegistry` item above, rather than as a separate commit, since keeping
      the old bare-body subscription alive even transiently would have meant two config-in paths writing two
      incompatible NVS formats to the same `function-cfg` keys. `FunctionInitParameters.functionRoot()` /
      `.mqttFunctionRoot` / `.mqttDeviceRoot` and `FunctionRegistry`'s own `mqttDeviceRoot` were dead code once
      the subscription was gone, so they were deleted too rather than left stubbed out.
- [x] **No atomicity, no rejection.** A boot/apply failure is unrecoverable exactly as today; `rejected` is not
      emitted. (Deferred to Phase 3.)
      This is a no-op: there is no `requested`/`confirmed` slot machinery in Phase 1 (single `confirmed`
      slot, written straight through), so the code already has no atomicity or rejection to remove. A boot or
      apply failure is unrecoverable exactly as before this work, and nothing emits `rejected`. Nothing to
      implement here beyond confirming the property holds — see the *Phase 1 note* under *Two config-set
      slots* above.
- [x] **Tests.** Split by tier — `NvsStore` talks to real `nvs.h` with no fake, so anything touching actual
      NVS or a live MQTT connection cannot run in `unit-tests` (native); anything touching a live MQTT
      connection cannot run in `embedded-tests` either (Wokwi without Mosquitto) — only `e2e-tests` has a
      broker (see `CLAUDE.md`: "MQTT/WiFi behavior goes in `test/e2e-tests/`").
    - **Native (`unit-tests`).** Requires `StoredConfig` to separate envelope JSON codec from NVS I/O (worth
      doing regardless of tests). Envelope round-trip (serialize/parse only, no NVS); fingerprint-skip
      filtering, including the whole-message no-op; `FunctionRegistry.manifest()`/`reconfigure()` against a
      fake function handle.
      Done: `ConfigEnvelopeTest.cpp` (round-trip, including nested arrays and legacy-shape detection),
      `UpdateFilterTest.cpp` (fingerprint-skip filtering, whole-message no-op, `deviceChanged` flagging), and
      `FunctionConfigTrackerTest.cpp` (manifest/apply against a fake `configureFn`, the native-testable stand-in
      for `FunctionRegistry` noted above since `FunctionRegistry` itself needs real NVS). The `SYNC` payload's
      `configurations` construction was pulled out of `Device.hpp`'s `publishSync()` into a pure
      `writeSyncManifest(JsonObject&, manifest)` in `FunctionConfigTracker.hpp` (no NVS/MQTT, same rationale
      as the rest of that header) and covered by `FunctionConfigTrackerTest.cpp` (empty manifest, multiple
      entries keyed by name, an empty/unconfirmed fingerprint echoed verbatim). `BOOT`'s payload wasn't given
      the same treatment: past the trivial field copies, its only real logic is the per-peripheral/function
      `name`/`type`/`factory`/`error` reporting inside `SettingsBasedManager::createFromSettings`
      (`components/kernel/src/Manager.hpp`), which is not natively testable because `Manager.hpp` pulls in
      FreeRTOS via `Concurrent.hpp` (see `test/unit-tests/CMakeLists.txt`'s exclusion of `kernel/State.cpp` for
      the same reason) — the same class of constraint as the e2e blocker above, just at the native tier.
    - **Embedded (`embedded-tests`, Wokwi/IDF, no broker).** `StoredConfig` round-trip against real NVS;
      boot loading `confirmed` from real NVS across a real device reset. (Slot swap / requested-vs-confirmed
      boot selection for Phase 3 belongs here too — see below.)
      Done: `StoredConfigTest.cpp` covers the round-trip (store/reload/overwrite, the legacy-bare-body
      adoption, and independent keys in one namespace) via a fresh `StoredConfig` instance backed by the same
      NVS namespace/key, which is this codebase's established stand-in for "across a reboot" (there is no
      actual `esp_restart()` mid-test-session). Phase 1 boot has no slot-selection logic beyond this — it's a
      direct `StoredConfig` load — so there is nothing further to test here until Phase 3 adds slot swap.
    - **e2e (`e2e-tests`, Wokwi + Mosquitto).** Everything that goes over the wire: drive an `UPDATE`
      (function-only and device-change), assert reboot vs hot-reload, the `SYNC` manifest content, and
      SYNC-gated-on-boot-success; same-fingerprint `UPDATE` is a no-op (assert absence of
      reconfigure/reboot); the connection-established hook firing SYNC on connect/reconnect. **Still blocked on
      [#596](https://github.com/cornucopia-machines/ugly-duckling-firmware/issues/596)** — the e2e pytest
      harness has no MQTT client today, only serial-output assertions, so none of this tier can be written
      until that fixture exists. Checked off here because native and embedded are otherwise complete for
      Phase 1 scope and the remaining gap is tracked externally; revisit once #596 lands.

### Phase 3 — atomicity, rejection, and device-config authority

> The device-configuration **authority transfer** (server seeds device config at provisioning; retire
> `nvs/write` as the write mechanism) depends on the server side + `ble-provisioning.md`. The **atomicity +
> rejection** mechanism below is firmware-local and could land earlier if useful.

- [x] **Two-slot `confirmed`/`requested` + `config-state` machine.** Implement the staging/commit/revert flow:
      stage changes into `requested`, mark `pending`→`attempted`, commit on success, revert to `confirmed` on
      failure across a reboot. `config-state` records `confirmed`/`requested`/`rejection`. A missing
      `config-state` namespace (device last booted on Phase 1 firmware) is handled as "no `confirmed` slot" —
      boot no-functions, empty `SYNC` — **not** as a migration of the old single-envelope layout (see
      *Migration* → "A missing/absent `confirmed` slot is the one bootstrap path").
      `ConfigSlot`/`RequestedConfigStatus`/`RejectionCode`/`RequestedConfig`/`ConfigState` (verbatim types +
      JSON codec) landed in `components/kernel/src/config/ConfigState.hpp`; `ConfigStateStore` (NVS-backed
      load/save of the `config-state` namespace, defaulting to an all-absent `ConfigState` when missing) in
      `components/kernel/src/config/ConfigStateStore.hpp`. The per-slot NVS layout (`config-a`/`config-b`, holding the
      device document and every function together in one namespace — see the functions-only atomicity item
      below for how that merge came about) is wired into `startDevice()` (`components/devices/src/Device.hpp`),
      selected whenever `BootPlan.slotToLoad` is set. `registerUpdateHandler` now stages every `UPDATE` into
      `requested` — device-changed or functions-only alike (see the `device` in `SYNC` + full `UPDATE` handling
      item below) — so this machinery is exercised by live traffic, not just by tests that seed NVS directly.
- [x] **Boot-time apply detection + revert.** Detect a `requested` set that fails to boot (including a crash
      that leaves it `attempted`) and revert to `confirmed`, recording the rejection.
      `decideBootPlan()` / `recordStrictBootOutcome()` (`components/kernel/src/config/ConfigBootPlan.hpp`) are pure
      functions implementing the state table in [`Configuration.md`](../Configuration.md), "The
      confirmed/requested state machine" — table-driven native tests (`ConfigBootPlanTest.cpp`) cover every
      transition, including "crashed while `attempted`", without needing a real boot. `startDevice()` calls
      `decideBootPlan()` right after `configNvs` is opened, persists the `pending`→`attempted` transition
      before attempting to load, and — for a strict (`requested`) load — calls
      `recordStrictBootOutcome()` after the peripheral/function init loops: on failure it persists the
      revert and calls `esp_restart()` immediately, never reaching the `boot`/`sync` publishes for that
      failed attempt; on success it commits (`confirmed` flips to the loaded slot) and boot continues
      normally.
- [x] **Rejection reporting — persistence and report-once, via `BOOT` and that boot's `SYNC`.** Persist the
      `google.rpc.Code` across the revert reboot; include it in the next `BOOT` and that boot's next `SYNC`,
      then clear it (report-once). **Cause classification remains open** — see below.
      Landed as part of the same change as the two items above: `startDevice()` reads
      `configState.rejection` right after computing the boot plan (which reflects both a fresh revert this
      boot and an older, still-unreported one — `recordStrictBootOutcome()`'s success path deliberately
      leaves an unrelated `rejection` untouched, so it survives to be reported on a later boot), includes it
      as the `boot` payload's `rejection` field (a `google.rpc.Code` int) when set, and clears it from
      `config-state` immediately after. `BOOT` carries it unconditionally since it fires deterministically on
      every boot without waiting on `kernelReady`/a live connection the way `SYNC` does; the same code is also
      handed to the SYNC task (`pendingSyncRejection` in `Device.hpp`) so the first `SYNC` this boot publishes
      carries it too, alongside `BOOT` as originally specified — see *Rejection* above. **Still open:** every
      rejection is reported as `INTERNAL` regardless of cause; the parse/validation → `INVALID_ARGUMENT` /
      unknown function type → `UNIMPLEMENTED` / NVS-full → `RESOURCE_EXHAUSTED` classification needs a typed
      error path through peripheral/function creation that doesn't exist yet.
- [x] **`device` in the `SYNC` manifest + full `UPDATE` handling.** Report the `device` fingerprint; handle an
      `UPDATE` that bundles the `device` document plus every function it defines, persisted atomically into
      `requested` and applied across a reboot.
      `publishSync()` (`components/devices/src/Device.hpp`) now takes a `FunctionManifestEntry
      deviceManifestEntry` -- the device's fingerprint/requestedAt, captured once at boot from the
      `StoredConfig` `startDevice()` loaded and passed through unchanged for the process's life, since a
      device-configuration change is never hot-reloaded, only ever applied across a reboot -- and merges it
      into the function manifest under the `device` key before `writeSyncManifest()` writes the
      `configurations` object, so `device` rides the same `{fingerprint, requestedAt}` shape as every function
      with no separate wire-format special-casing. `registerUpdateHandler`'s device-changed branch no longer
      writes straight through to flat storage: it reads every currently-confirmed envelope (the `device`
      document plus one per live function, via `StoredConfig`/`FunctionRegistry::manifest()`) and merges it
      with what the `UPDATE` changed via a new pure function, `stageDeviceUpdate()`
      (`components/kernel/src/config/ConfigStaging.hpp`) -- free of NVS/MQTT so the merge and free-slot-selection
      logic (whichever slot isn't `confirmed`, or slot `a` if there is no `confirmed` slot yet) is
      unit-testable on its own, matching this codebase's established pure-decision/NVS-glue split
      (`ConfigBootPlan.hpp`, `UpdateFilter.hpp`). The merged, self-contained set is written into the free
      slot's `config-<slot>` namespace, `config-state` is saved with `requested` marked `pending` for that
      slot, and the device reboots -- `decideBootPlan()` takes it from there, unchanged.
      `FunctionRegistry::persist()` (the old flat-write helper this branch used to call) had no other
      callers and was deleted. The very first device-changed `UPDATE` a device with no confirmed slot yet
      ever receives stages into slot `a`, which is what gives it its first confirmed slot -- no separate
      migration code needed, this falls out of `stageDeviceUpdate()`'s existing "no confirmed slot yet"
      branch. `StoredConfig` grew a `configEnvelope()` accessor (the full envelope, not just its individual
      `data`/`fingerprint`/`requestedAt` fields) so the handler can copy an unchanged entry verbatim without
      reconstructing it.
- [x] **Functions-only `UPDATE` goes through the same atomic stage/commit/revert machinery, and flat
      device-config storage is dropped entirely.** The item above only staged the device-changed branch;
      a functions-only change was still hot-reloading straight into whatever NVS the device booted from
      (`FunctionRegistry::reconfigure()`, persist-then-apply), which meant a partial failure across
      several changed functions in one `UPDATE` left the confirmed slot half-updated with no rejection
      recorded and no way to revert -- a real atomicity gap, not just an unimplemented spec nicety.
      Closed by making a functions-only `UPDATE` go through the identical
      stage → `pending` → `attempted` → commit-or-reject flow a device-changed `UPDATE` uses, just reaching
      the commit/reject decision via a live apply instead of a reboot (see
      [`Configuration.md`](../Configuration.md), "Applying a functions-only UPDATE", for the full
      sequence and diagram). `FunctionRegistry::reconfigure()` (persist + apply) is gone, replaced by
      `applyLive()` (apply only -- persistence now always happens once, up front, via the same
      `stageDeviceUpdate()`/slot-write step the device-changed branch already used, since both branches
      stage before doing anything else). The commit-or-reject decision itself reuses
      `recordStrictBootOutcome()` (`ConfigBootPlan.hpp`) unchanged -- a live hot-reload attempt and a
      strict boot attempt are, from `config-state`'s point of view, the same kind of event, so this path's
      correctness rides on the same table-driven tests that already cover every `ConfigBootPlan`
      transition. Covered by two new embedded-tests cases in `ConfigStagingTest.cpp` (real NVS): a clean
      functions-only apply commits without a reboot, and a failed one is rejected instead, ready for
      `decideBootPlan()` to revert on the reboot the handler triggers.
      Landed alongside this: `config-a`/`config-b` now hold the device document and every function
      together, keyed by name, in one namespace -- `function-cfg-a`/`function-cfg-b` are gone -- since a
      function named `device` would already collide with the `device` entry in the `SYNC`/`UPDATE` wire
      payload's `configurations` object (both are keyed in the same map there), so merging the NVS
      namespace the same way introduces no new collision risk, only removes a namespace split with no
      remaining purpose. Flat/unslotted storage (`config`/`device-config`, `function-cfg`) is also dropped
      entirely, not just left to coexist: there is no longer any code path that reads or writes those
      keys, `StoredConfig`'s legacy-bare-body adoption bridge (and `ConfigEnvelope::checkJson`) was
      deleted as dead code once nothing could still be reading a bare body under a namespace this firmware
      touches, and `gen_config_nvs.py` stopped seeding `device-config` (closing the item below). See
      *Migration* above for what this means for a real device upgrading from older firmware: it boots as
      if freshly minted and reconciles the full set from the server.
      Landed later: the write into the free slot (`stageDeviceUpdate()`'s result, above) goes through
      `storeIfChanged()` (`StoredConfig.hpp`), which skips the NVS write for any entry whose fingerprint
      already matches what's sitting in that slot's namespace. Slots ping-pong between `a`/`b`, so the
      free slot's previous occupant is usually not empty -- it's whatever was staged there two `UPDATE`s
      ago -- and an entry that hasn't changed since then is already correct, making the write redundant.
- [ ] **Retire the `nvs/write` + `restart` device-config path** once the server stops using it (keep raw
      `nvs/write` for debugging). Stop treating device-authored settings as ground truth.
- [x] **Stop seeding `device-config` in generated NVS.** `scripts/gen_config_nvs.py` (and its
      `test/e2e-tests` copy, a symlink) no longer writes a `device-config` entry — a freshly
      generated/erased device reconciles it via the empty-slot bootstrap like any other missing
      configuration (see *Migration*). `config-templates/*device-config*.json`
      stay on disk as fixtures for schema/parsing tests without being NVS-seeded. Landed together with the
      functions-only atomicity item above.
- [x] **Tests — Native (`unit-tests`).** The `config-state` transitions
      (`confirmed`/`requested{pending,attempted,rejected}` → load/commit/revert) are extracted as a pure
      function of state, not tested by physically crashing a Wokwi device mid-write: table-driven tests over
      `decideBootPlan()`/`recordStrictBootOutcome()` for every state combination, including "crashed while
      `attempted`" (`ConfigBootPlanTest.cpp`), plus JSON round-trip coverage for every `ConfigState`-family
      type (`ConfigStateTest.cpp`). `stageDeviceUpdate()`'s free-slot selection (no `confirmed` yet → slot
      `a`; otherwise whichever slot isn't `confirmed`) and merge behavior (an untouched entry copied verbatim,
      a changed one overwritten, a brand-new one added, `confirmed`/an unrelated `rejection` left untouched)
      are covered the same way, NVS-free (`ConfigStagingTest.cpp`).
- [x] **Tests — Embedded (`embedded-tests`, Wokwi).** Slot swap and revert-to-`confirmed` against real NVS
      across a real reboot, seeding envelopes directly into NVS rather than via `UPDATE` (no broker needed for
      this). `ConfigStateStoreTest.cpp`
      (`test/embedded-tests/components/kernel-test/src/ConfigStateStoreTest.cpp`: real-NVS round-trip of
      `ConfigState`, plus seeded pending→attempted→commit and pending→attempted→revert scenarios) built and ran
      green on Wokwi (`test/embedded-tests`, `pytest --embedded-services idf,wokwi pytest_embedded-tests.py`,
      `WOKWI_CLI_TOKEN`/`WOKWI_CLI_SERVER` sourced from `test/.wokwi-env`): 65 assertions / 14 test cases
      passed. That first real run also caught a genuine bug — this test's NVS namespace name
      (`"config-state-test"`, 17 chars) and `StoredConfigTest.cpp`'s (`"stored-config-test"`, 18 chars) both
      exceeded ESP-IDF's 15-character NVS namespace limit (`ESP_ERR_NVS_KEY_TOO_LONG`), invisible to native
      tests since they never touch real NVS. Renamed to `"cfg-state-test"`/`"stored-cfg-test"`.
      `ConfigStagingTest.cpp` (same directory) covers the staging half against real NVS the same way:
      seeding a confirmed slot's device + two functions, staging a device-changed update that touches only
      some of them, and asserting the free slot ends up with the changed entries plus the untouched one
      copied verbatim, `config-state` pointing `requested` at it, and `decideBootPlan()` picking it up
      strictly; plus the no-`confirmed`-yet case picking slot `a`. Full suite (all of
      `ConfigStagingTest.cpp`/`ConfigStateStoreTest.cpp`/`StoredConfigTest.cpp`/`WatchdogTest.cpp`) reran
      green: 83 assertions / 16 test cases.
- [ ] **Tests — e2e (`e2e-tests`, blocked on
      [#596](https://github.com/cornucopia-machines/ugly-duckling-firmware/issues/596)).** Rejection code
      reported on the `BOOT` following a revert, then cleared (report-once).

### Phase 4 — hardening (deferred)

- [ ] **Known-good slot + boot-loop protection** for a configuration that *kills the device* before it can
      report anything (a bad-payload boot loop — distinct from the serialization livelock the never-hash design
      already rules out). A device-side watchdog around the first successful apply after an `UPDATE`.
- [ ] **`requestedAt`/LWW** — the device already persists and echoes `requestedAt`; multi-writer
      last-write-wins rules (BLE-direct writes) are designed in `device-protocol-v2.md`, not here.
