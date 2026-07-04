# filament_id plan v2: the `get_filament_id` (P+md5) proposal, validated

> **Status note:** the verdict in §0-§4 and §7-§8 stands. The work items in §5 are
> superseded by `filament_id_plan_v3.md` (OFL-as-catalog, content-addressed triple key,
> succession ledger): W1/W3 are carried into v3.0, W2/W5 carry unchanged, W4 is dropped.

Follow-up to `filament_id_plan.md` (v1, implemented on `feature/filament_id`: deterministic
`OF*` mint + snapshot ledger + 30-vendor migration, tree-wide validator `-f` = 0).

**Proposal under review:** mint system `filament_id`s with the scheme of
`static std::string get_filament_id(std::string vendor_typr_serial)`
(`src/slic3r/GUI/CreatePresetsDialog.cpp:487-552`) — Bambu's user-custom-filament id
allocator — so system ids are "compatible with Bambu AMS sync".

Every claim below was verified in source (`file:line`), against upstream BambuStudio, or in
primary online sources (URLs in §8). Verdict first, evidence after.

---

## 0. Executive summary — the verdict

**Do not re-mint system ids as `P+md5(name)`. Keep the v1 `OF*` mint.** Three facts decide it:

1. **The AMS/device path never validates id *shape*.** `tray_info_idx` is an opaque string
   end-to-end: sent verbatim (`DeviceManager.cpp:1642`), parsed verbatim
   (`DevFilaSystem.cpp:512-514`), and firmware persists arbitrary bytes — an A1 stored and
   echoed a *corrupted* id `Pde2\xea58c` (bambulab/BambuStudio#5436). A P-shaped system id
   is not "more acceptable" to the device than an `OF*` one.
2. **P-shape buys zero recognition.** The printer resolves tray ids only against its built-in
   `GF*` catalog plus the *account's* cloud custom-filament cache (official Bambu wiki); ids
   it cannot resolve round-trip as `?`. Orca can never upload system presets into that cache
   (`Preset.cpp:2071` gates cloud upload on `is_user()`), so an Orca system `P` id shows `?`
   exactly like an `OF` id. The public catalog id space is 100% `GF*` (ha-bambulab
   `filaments.json`: 86/86 ids).
3. **P-shape is the one shape the codebase punishes.** The *only* id-shape dispatch in the
   entire tree is `MachineObject::check_ams_filament_valid` + `update_filament_list`
   (`DeviceManager.cpp:5335/5352/5395/5410`, gate `setting_id.size()==8 && [0]=='P'`), a
   user-custom lifecycle reconciler that **remotely wipes an AMS tray**
   (`command_ams_filament_settings(..., "", "", white, "", 0, 0)` at `:5345`) or rewrites its
   temps (`:5361-5367`) when a P-shaped tray id drops out of the user-root preset list
   (`:5219`, armed at `:5268`). Name-keyed minting makes system id == user-custom id *by
   design* (same name → same id), so "user deletes their now-redundant custom after the
   system preset ships" becomes a remote tray wipe. `GF*` (wrong length) and `OF*` (wrong
   first char) are structurally immune.

The proposal's *real* value lives in two places, and v2 captures both without the re-mint:

- **The adopt step** (the function's first branch, `:509-511/:526-528`): same base name →
  same id. In-app, this already gives Orca-local convergence today — a user custom named
  like a system family adopts the system id *whatever its shape*. At authoring time, the
  same semantics = the v1 `shared_catalog` mechanism; §5/W4 extends it (curated, optional)
  to let byte-authentic Bambu-catalog families carry authentic `GF*` ids.
- **The hash step belongs to user space.** `P+md5(name)` is Bambu's allocator for *user*
  presets; the community is already asking for it there (OrcaSlicer PR #13315, unique ids
  for inherited user presets). §5/W5 endorses that lane. System profiles stay out of the
  `P` namespace — and v2 adds client hardening (W1) because ten *already-shipped* system
  P-hex ids (Cubicon `P510cf**`×8, Ginger `P510eff9`, Artillery `Pfcf9c4c`) sit inside the
  wipe-gate's shape match today.

Judge panel (3 independent lenses over 4 variants; §7): keep-`OF*` won 23/40 aggregate
(device-ecosystem 6, identity-semantics 8, migration-enforceability 9) vs full Bambu
emulation 11, hash-only name-keyed 12, vendor-scoped P-hash 12. The single dissent
(device-ecosystem, favoring adoption) is honored by W4; its own verdict on the shape was
"adopt the semantics, not the costume".

---

## 1. What the proposed generator actually is (verified)

`get_filament_id(vendor_typr_serial)` (`CreatePresetsDialog.cpp:487-552`), called with
`"<vendor> <type> <serial>"` (`:1128`, `PLA-AERO`→`"PLA Aero"`), and per-preset with the
display base name when cloning presets for a user-created printer
(`Preset.cpp:2785-2803` → `:2798-2800`):

1. **Adopt:** scan a temp bundle of *every* vendor bundle on disk
   (`PresetBundle::load_system_filaments_json`, `PresetBundle.cpp:2342-2391`) plus the
   user's presets, then the live bundle (`Preset.cpp:2831-2842`); if any preset's base name
   (`name.substr(0, first('@') - 1)`) equals the input and its id ≠ `"null"`, **return that
   id** — which can be `GF*`, `QD_*`, `OF*`, or another user's P-hex.
2. **Mint:** `"P" + md5(input)[0:7]`, lowercase hex over the raw UTF-8 bytes (`:533`,
   `:480`).
3. **Collide:** if the id is held by a *different* name, re-hash with a local wall-clock
   salt (`md5(input + get_curr_time())`, `:547`) — non-deterministic by construction.

Upstream parity: the function and its caller input construction are **byte-identical** in
BambuStudio master (`CreatePresetsDialog.cpp` L434-499 / L1061; diff empty, fetched
2026-07-03). So identical dialog inputs in BBS and Orca yield identical ids — the kernel of
the proposal's compatibility claim.

Reproduction limits (why "exactly the approach" cannot be ported to system minting anyway):

- The base-name parser truncates names lacking a space before `@` (`"Afinia PLA@HS"` →
  `"Afinia PL"`); 48 shipped presets hit this, and the *clone* path parses differently
  (`" @"` two-char, `Preset.cpp:2792`), so the app itself is internally inconsistent.
- 597 presets (361 whole families, e.g. all of Anker) contain no `@` and are invisible to
  the adopt scan and its collision map (`:505-508`).
- The collision salt is wall-clock time — unreproducible across machines, so id values
  cannot be CI-verified from names; a ledger would still be the sole source of truth.
- Shipped P-hex ids don't round-trip from names today: Cubicon `P510cfb0` ≠
  `P+md5("Cubicon ABS")` = `Pc624b68` (verified) — so even Bambu-lineage P ids are not
  name-derivable in practice.

## 2. What Bambu AMS sync actually consumes (verified)

- Send: `command_ams_filament_settings` puts the preset's `filament_id` verbatim into
  `tray_info_idx` (`DeviceManager.cpp:1642`); cloud vs LAN differ only in transport
  (`:2502-2536`). No client- or firmware-side rejection path exists; the ack parse stores
  whatever the printer echoes (`:3808-3860`).
- Resolve (slicer side): tray id → first compatible root preset with equal `filament_id` —
  system *and* user roots, no shape filter (`PresetBundle.cpp:3151`, `:3252-3254`); misses
  fall back by `"Generic <type>"` name, then similarity, then keep-previous
  (`:3157-3169`, `:3260-3305`). `setting_id_to_type` resolves against `is_system` presets
  only (`DeviceManager.cpp:2538-2559`) — a system preset resolves *because it is system*,
  never because of its id shape.
- Resolve (device side): built-in `GF*` catalog + the account's cloud custom cache;
  custom-filament sync is **cloud-only** (not LAN), and an id the printer has no data for
  displays `?` until opened once on the printer screen (official wiki, §8). Third-party
  consumers see customs as "unknown" (ha-bambulab #466/#540).
- Since Jan 2025, AMS configuration is gated behind Bambu-signed clients on new firmware
  (Developer Mode / older firmware exempt) — any AMS-sync benefit for Orca is conditional
  on that regardless of id scheme (Bambu authorization blog, §8).
- Orca already ships non-`GF`/non-`P` shapes to Bambu trays in the field: OFL presets are
  compatible with every printer (`PresetBundle.cpp:5681-5684`) and carry `OGF*`/`OFL*` ids
  (e.g. `OFLSBS99`, hardcoded in `MoonrakerPrinterAgent.cpp:654`). The premise "the
  ecosystem has only ever seen `GF*`/P-hex" is already false, with no observed rejection.

**Cross-client reality check.** The name-keyed benefit ("a BBS-created custom named like an
Orca system preset lands on the same id, so Orca's sync matches the system preset") is
mechanically true but narrow: it needs byte-exact names (including the `PLA Aero` mapping
and single spacing), the unsalted hash path, cloud mode, and permissive firmware. The
symmetric case already works in Orca *without* any re-mint: an Orca user creating a custom
named like a system family **adopts the system id** via the in-app adopt branch, so local
custom ↔ system ↔ tray matching is shape-independent today.

## 3. The disqualifying finding, in mechanism form

`update_filament_list` (`DeviceManager.cpp:5208-5272`) snapshots `{filament_id → temps}`
over **user root presets only** (`preset.is_user() && preset.inherits() == ""`, `:5219`);
ids that vanish or change temps between snapshots are armed into `checked_filament`
(`:5268`). `check_ams_filament_valid` (`:5311-5445`) then, for every AMS/virtual tray whose
`setting_id` matches `size()==8 && [0]=='P'`:

- id armed and **not** in the current user-root list → remotely **clears the tray**
  (`:5335-5349`, `:5395-5408`);
- id armed and in the list with unequal temps → **rewrites tray temps from the user
  preset**, ignoring same-id system presets (`:5352-5374`; `PresetBundle.cpp:3719` skips
  non-user presets; unguarded `find()->second` at `:3717`).

Failure scenario under the proposal: system `"PolyLite PLA"` ships with
`P+md5("PolyLite PLA")`. A user's same-named custom (BBS- or Orca-created — same id by
design) is on an AMS tray. The user deletes the now-redundant custom. Next status refresh:
the id is armed, absent from user roots → Orca wipes the tray, even though the system
preset still resolves that id perfectly. Runs continuously (`StatusPanel.cpp:3275-3277`).

Two corollaries independent of the proposal (→ W1):

- The ten shipped P-hex **system** ids (Cubicon/Ginger/Artillery) already pass the shape
  gate; only the "id must transit a user root" arming condition protects them, and the
  in-app adopt branch can create exactly that transit today.
- `assert(it->first.size() == 8 && it->first[0] == 'P')` (`:5252`) is violable today: a
  user custom named `"Bambu PLA Basic"` adopts `GFA00` onto a user root and aborts debug
  builds.

## 4. Identity costs the re-mint would add (quantified, tree dry-run)

- 1148 distinct family base names; **198** appear in 2+ vendors; **159** groups carry ≥2
  distinct ids today and would silently merge under name-keying (Generic PETG alone: 15
  ids today). 112 involve BBL; 82 are `GF↔OGF` mirror pairs.
- **4** cross-vendor groups share a name across materially different `filament_type`
  (Generic PLA Silk, Generic PETG-CF, Generic PA6-CF, Generic PE-CF) — one merged id would
  feed wrong type/name/vitrification to the global unscoped consumers
  (`PresetBundle.cpp:690-733`; `DeviceManager.cpp:2538`; `FilamentGroup.cpp:513-519`) (→ W3).
- 33 deliberately-split families on the branch would re-merge (25 Elegoo-vs-OFL pairs, 7
  FlyingBear-vs-InfiMech "Other *" pairs, the salted Cubicon PC pair) — undoing v1
  decisions that the fixture gate forced.
- Renames become id migrations: name-keyed ids re-key on any marketing rename; the ledger
  can freeze them, but then name-derivability — the scheme's selling point — dies family
  by family. The `OF*` key (`vendor/family`) survives display-name churn.
- Entropy drops 35.7 → 28 bits (still 0 collisions at today's 1148 names; ~0.75 expected
  at 20k). Re-mint surface: 391 unshipped ids across 1327 files, 13 hardcoded tests, the
  reserved-namespace policy inversion, and a retired-ledger rebuild.

## 5. Work items (delta from `feature/filament_id` HEAD)

**W1 — client hardening (C++, small, ships with the migration PR train).**
  a. Skip the tray-reset and temp-rewrite in `check_ams_filament_valid` when **any
     `is_system` filament preset carries the tray's id** — no compatibility filter,
     mirroring the semantics of the global consumers (`get_filament_by_filament_id`,
     `setting_id_to_type`). This protects the ten shipped P-hex system ids and any BBS
     custom colliding with them, while ids resolving only to user presets (or to nothing)
     keep today's cleanup behavior.
  b. Relax the debug assert `DeviceManager.cpp:5252` to a log (user roots legitimately
     carry adopted `GF*`/`OF*` ids).
  c. Guard `PresetBundle.cpp:3717` (`find()` unchecked before `->second`).
  d. Regression-check that AMS sync never clears trays whose id it merely cannot resolve
     (OrcaSlicer#4431 class): the keep-previous fallbacks at `PresetBundle.cpp:3163-3169`,
     `:3296-3305` must cover the UI paths.

**W2 — tooling + doc guardrails.**
  a. Document `^OF[0-9A-Za-z]{6}$` as the system-mint namespace. **Do not add it to
     `reserved_space_owner`** (`scripts/assign_filament_ids.py:394-402`): check 6 exempts
     only snapshot-grandfathered claims and the `--update-snapshot` refusal gate
     (`:596-621`) would then reject every *future* mint. The space is already enforced —
     check 3 (mint conformance, `:488-509`) errors on any OF-shaped id that does not equal
     its own family's mint (covered by `test_check3_of_id_must_match_mint`), and the
     snapshot diff is the human gate. Add one doc paragraph + a test asserting a foreign
     vendor claiming another family's OF id fails check 3.
  b. `doc/developer-reference/filament_id.md`: add a "why not P-hex" section citing the
     `DeviceManager.cpp:5335` gate and the account-cache resolution model — community
     pressure toward `P+md5` exists (PR #13315) and will recur.
  c. Document that user roots may legitimately hold adopted system ids (incl. what that
     means for cloud upload, `Preset.cpp:1874-1876`), matching BBS behavior.

**W3 — data hygiene: the 4 same-name-different-type groups.** Audit each (they confuse
  name-based fallback matching and any future interop even under vendor-scoped ids):
  verify whether the divergent `filament_type` is a data bug (e.g. OFL `Generic PETG-CF`
  resolving `PETG`, OFL `Generic PE-CF` resolving `PE`, Flashforge `Generic PLA Silk`
  mixing `SILK`+`PLA`, Creality-vs-Elegoo `Generic PA6-CF` as `PA-CF`/`PA6-CF`) or
  intentional; fix by type alignment or rename with evidence, gated by
  config-equivalence (only `filament_type` diffs as prescribed) + full validator suite.

**W4 — optional, curated GF adoption (the proposal's adopt step, done safely).** Extend the
  v1 `--allow-shared-catalog` mechanism into an explicit per-family adoption worksheet: a
  family may carry a byte-authentic Bambu catalog id iff (i) it is verifiably the same
  commercial product as the BBL family (vendor evidence + equal `filament_type`), (ii)
  validator `-f` stays 0 tree-wide (alias shadowing covers BBL/OFL overlaps), (iii) the
  fixture gate passes, (iv) the snapshot diff records the adoption. Candidates: the 13
  actionable BBL-overlapping families minted `OF*` in v1 (e.g. Qidi `Bambu ABS`→`GFB00`,
  `PolyLite PLA`→`GFL00`, `Overture PLA`→`GFL04`, `PolyLite ABS`→`GFB60`; `OFLSBS99` is
  shipped and frozen; the two type-hazard OFL families are excluded until W3 lands).
  **Honest benefit statement:** these presets are compatible only with non-Bambu printers,
  so no Bambu device resolves their ids today; the payoff is one-product-one-id coherence
  for the global consumers, project portability, and readiness for W6. Recommended seed:
  the four Qidi brand families; generics deferred.

**W5 — user-preset lane (where the proposal's hash belongs).** Support unique ids for
  inherited user presets (the PR #13315 pain: children share the parent's id, so AMS always
  resolves the generic parent). `P + md5(preset name)` matching
  `CreatePresetsDialog::get_filament_id` is correct *in user space*: adopt-first against
  existing ids, then hash; ids frozen per preset after mint. Constraints: never emitted
  into system profiles (W2a makes that mechanical); W1 must land first so shared-id
  lifecycles can't wipe trays.

**W6 — recorded future option:** SoftFever's "upload OrcaSlicer's filament database
  (especially the filament ID) to the printer" (PR #12724 comment). If pursued, Orca ids
  become first-class device-visible artifacts and the P-mimicry question is permanently
  moot; the `OF*` scheme is the *better* citizen there (collision-free vendor-scoped ids,
  no user-custom masquerade).

Unchanged from v1: mint rule, snapshot/retired ledgers (no OF retirement — nothing is
re-minted), CI wiring, migration commits. `filament_id_plan.md` remains the implemented
baseline; this document records the v2 decision and its follow-on work.

## 6. Gates (per work item)

Same battery as v1, all local: `python scripts/orca_extra_profile_check.py` exit 0;
`assign_filament_ids.py --check` exit 0 (46+ unit tests green; new tests for W2a
reservation and, when implemented, W4 adoption sanctioning); validator `-l 2` exit 0,
`-f` tree-wide exit 0, `-r` BBL+Qidi exit 0; custom-preset fixture overlays (the W1/W4
changes touch exactly the preset-visibility machinery the fixtures exist to protect);
config-equivalence: flattened effective configs differ only in prescribed keys
(`filament_id` for W4, `filament_type` for W3, none for W1/W2). W1 additionally needs a
manual AMS smoke test on a live Bambu printer (tray set/clear round-trip) — a release
checklist item, not CI (the workflows have no hardware runners).

## 7. Scheme comparison (judge panel record)

| Variant | Device lens | Identity lens | Migration lens | Σ |
|---|---|---|---|---|
| V1 full Bambu emulation (adopt incl. `GF*` + P+md5 + salt) | 7 | 2 | 2 | 11 |
| V2 hash-only name-keyed `P+md5(base name)` | 4 | 4 | 4 | 12 |
| V3 vendor-scoped P-hash (`P+md5(vendor/family)`) | 2 | 5 | 5 | 12 |
| **V4 keep `OF*` uuid5 vendor-scoped (chosen)** | 6 | 8 | 9 | **23** |

All three judges answered the pivotal question the same way: the P-hex **shape** confers no
device/cloud benefit (resolution is by value against catalog + account cache; the path is
otherwise shape-agnostic) and is the only shape with a destructive client-side gate. V3 is
strictly dominated (all of the re-mint cost, none of the name-keyed benefit). V1's genuine
half — GF adoption — is captured by W4 at zero re-mint cost.

## 8. Evidence index

Code (this branch): `CreatePresetsDialog.cpp:487-552,1128,2798-2800` (generator + callers);
`DeviceManager.cpp:1642` (verbatim send), `:2538-2559` (system-only type resolve),
`:5208-5272` (user-root snapshot, `:5219`, `:5252` assert, `:5268` arming), `:5311-5445`
(P-shape gate + tray wipe `:5335/:5345`, temp rewrite `:5352-5374`, virtual tray
`:5395-5435`); `DevFilaSystem.cpp:512-524`; `PresetBundle.cpp:690-733,3116-3128,3151,
3252-3254,3709-3767,5674-5753`; `Preset.cpp:1874-1876,2071,2785-2842`;
`AMSMaterialsSetting.cpp:886,894-897`; `WebGuideDialog.cpp:67`;
`PresetComboBoxes.cpp:1952-1966`; `MoonrakerPrinterAgent.cpp:654`;
`scripts/assign_filament_ids.py:69,71-72,118-144,394-402,596-621`.

Online (fetched 2026-07-03): BambuStudio master `CreatePresetsDialog.cpp` L434-499
(byte-identical generator); wiki.bambulab.com `create-filament` (custom filaments AMS-able
from firmware 1.6.6; only dialog-created presets sync) and `custom-filament-issue`
(cloud-only sync; unknown ids show `?`); blog.bambulab.com authorization-control (Jan-2025
AMS-config gating); bambulab/BambuStudio#5436 (firmware persists corrupted id);
OrcaSlicer #4431 (tray clobber to `?`), #3874 (cloud-only confirmation), PR #14459
(the `-f` invariant), PR #14423 + PR #12724 + PR #13315 (maintainer statements / community
P+md5 pressure); greghesp/ha-bambulab `filaments.json` (86/86 `GF*`) + #466/#540.

Method: 6 parallel evidence agents (generator semantics; AMS/device path; exhaustive
shape-dispatch sweep; upstream/online; tree-wide dry-run over 5892 presets / 1455 families;
branch change inventory) → 3-judge panel (device-ecosystem, identity-semantics,
migration-enforceability lenses). Dry-run artifacts: session scratchpad
`phex_dryrun_{summary,details,followup}.json`.

## 9. Open decisions for maintainers

1. **W4 scope**: none / 4 Qidi brand families (recommended) / + generic-tier re-adoption.
2. **W5 timing**: implement in-repo vs review upstream PR #13315 with the W1 hardening as a
   prerequisite either way.
3. **W6**: pursue the filament-database-upload design (makes Orca ids device-visible and
   ends the mimicry debate for good).
