# filament_id plan v3: OFL as the catalog, content-addressed product ids

Supersedes §5 of `filament_id_plan_v2.md` (whose P+md5 verdict stands unchanged) and
revises the mint rule of `filament_id_plan.md` (v1, implemented on `feature/filament_id`).
Decision driver: v1's mint key scoped families to the *profile bundle* (printer brand), so
one commercial product tuned in N bundles carries N ids — e.g. PolyLite PLA ships in five
bundles (`BBL`, `OrcaFilamentLibrary`, `OrcaArena`, `Qidi`, `Snapmaker`), all with
`filament_vendor: "Polymaker"`, under up to five different ids (`GFL00`, `OFceJcLf`, …).
That contradicts what `filament_id` means: one id per commercial product line.

## 0. The architecture

Three tiers, two frozen islands:

1. **BBL island — untouched.** Bambu profiles keep their `GF*` catalog ids byte-for-byte
   (device/RFID/cloud contract). No convergence between GF and OFL ids is attempted: the
   same product may permanently carry `GFL00` on the BBL side and an `OF*` id everywhere
   else. This is a deliberate trade for simplicity; v2's curated-GF-adoption (old W4) is
   dropped.
2. **`QD_*` island — untouched.** Qidi's box builds `QD_<series>_<vendor>_<type>` from
   device enums and requires exactly-matching presets (`QidiPrinterAgent.cpp:146-152`).
   Qidi presets carrying `QD_*` are exempt from re-minting, forever.
3. **Everything else converges on OrcaFilamentLibrary.** OFL is the product catalog: a
   material family's id is declared once, on its OFL family root. Vendor bundles carry
   only per-printer *specializations* of OFL families — same base name, non-empty
   `compatible_printers`, **no `filament_id` key** — which (a) alias-shadow the OFL preset
   on the printers they claim (`Preset.cpp:3684-3714`: "Generic PLA @Qidi Q2 0.4mm nozzle"
   hides "Generic PLA @System" on that machine) and (b) resolve the OFL family's id
   through the loader's inherits/base-bundle walk (`PresetBundle.cpp:4842-5080`). Products
   OFL does not (yet) carry mint their id in the vendor bundle **with the same rule**;
   because the key is bundle-independent (§1), later hoisting the family into OFL never
   changes its id.

Convergence therefore happens by *single declaration point*, not by copying ids across
bundles — no adoption registry, no cross-bundle id claims to curate.

## 1. The mint rule

```
triple = (filament_vendor, filament_type, family_name)      # from the family ROOT's flattened config
key    = "filament_product/<filament_vendor>/<filament_type>/<family_name>"
id     = "OF" + base62_6( uuid5( FILAMENT_ID_NAMESPACE, key ) )     # namespace unchanged:
                                                                    # c4d3ff49-4c32-5534-a3e3-00894157ab97
```

- **Triple resolution.** All three values come from the family root preset's *flattened
  effective config* (`filament_vendor` and `filament_type` are inheritable list options —
  take the first element; family_name = the root's base name, `\s?@.*` stripped once).
  For own-key-layout families (no root), each declaring preset's flattened config is used
  and CI requires all declarers of one family to agree on the triple. A root that resolves
  an empty `filament_vendor` or `filament_type` is a CI error (generics use `"Generic"`).
  Values enter the key verbatim (UTF-8, no case folding).
- **Bundle-independent by design.** The key contains no bundle name, so the same product
  yields the same id whether minted in OFL or in a vendor bundle; migrating a family into
  OFL (§0.3) is id-stable. Two bundles independently adding the same triple converge
  automatically — and that is correct, because equal (vendor, type, name) *is* the
  definition of "same product" here. The known look-alike hazards stay separated by the
  type component: the 4 same-name-different-type groups (Flashforge `Generic PLA Silk` as
  SILK, OFL `Generic PETG-CF` as PETG, `Generic PA6-CF`, `Generic PE-CF`) hash apart until
  their type bugs are fixed (§5, W3) and converge automatically after — self-healing that
  pure name-keying (rejected in v2) could not provide.
- **Format and salt unchanged from v1**: 8 chars, `OF` + 6 base62; deterministic `/1`,
  `/2`… salt past any taken or retired id (the O+7 widening was evaluated and rejected:
  62⁶ expects 1.9×10⁻⁵ collisions at today's 1,455 families, and `^O.{7}$` would sweep 35
  legacy ids into the conformance gate vs. one today).
- **Content-addressed, deliberately.** If any triple component changes — a family rename,
  a `filament_vendor` correction, a `filament_type` fix — the id changes with it. This
  *replaces* v1's "ids are immutable once shipped" with "ids are derivable from the
  product identity, and identity changes are migrations" — made safe by the succession
  ledger (§2). `renamed_from` still gates preset-name compatibility as before.
- CLI: `--mint "Polymaker/PLA/PolyLite PLA"` prints without touching the tree; running
  the script plain inserts missing ids; CI errors print the expected id.

## 2. The succession ledger (the amendment that makes this safe)

Shipped ids are referenced outside the tree: AFC/Klipper lane data, Bambu AMS trays
holding OFL-only materials, on-device PA-calibration records, user-root preset copies,
3mf `slice_info`. Retiring an id without a forwarding pointer downgrades all of those to
`Generic <type>` fallbacks. Therefore:

- **Schema.** `scripts/retired_filament_ids.json` entries become objects:
  `{"retired": {"OGFL99": {"claims": [...], "successor": "OFxxxxxx"}}}`. Append-only as
  before; a successor may itself be retired later (chains allowed, cycle-checked, and
  followed to the live end). A retired id may never be minted again (check 4 unchanged).
  Cross-island *hint* entries are permitted for ids Orca cannot retire because another
  island owns them (e.g. `GFL99 → <OFL Generic PLA id>`): consulted only when no live
  preset matches, so BBL installs still resolve `GFL99` natively first.
- **Shipped and consulted at runtime.** The ledger ships in `resources/`; a small helper
  (`resolve_filament_id_succession(id)`) follows the chain and is consulted **only on
  resolution miss**, before the `Generic <type>` name fallback, in:
  `PresetBundle::get_filament_by_filament_id` (covers `DevFilaBlackList`, `SelectMachine`
  warnings, `Plater` tray configs in one place), the AMS sync match predicates
  (`PresetBundle.cpp:3151`, `:3252-3254` miss paths at `:3157-3169`/`:3260-3305`),
  `PresetComboBoxes::add_ams_filaments`, `MachineObject::setting_id_to_type`
  (`DeviceManager.cpp:2545` miss branch), calibration-history name lookup
  (`CaliHistoryDialog.cpp:62`), and the #14423 Moonraker lane matching when it lands.
  With this in place the OFL re-mint is near-residue-free and every future
  content-addressed rename stays safe.
- **Kill the hardcoded generic map.** `MoonrakerPrinterAgent::map_filament_type_to_generic_id`
  (`MoonrakerPrinterAgent.cpp:608-658`) hardcodes 23 OFL ids (`OGFL99`… `OFLSBS99`).
  Replace it with a runtime lookup of the OFL generic preset by name ("Generic PLA
  @System" → its current id), removing the code↔profile lockstep permanently. (This also
  releases `OFLSBS99`, v2's one frozen OF-shaped legacy id, for normal re-minting.)

## 3. Validation rule changes

- **Check 3 (mint conformance)** becomes a pure function of the root's triple: a non-BBL,
  non-`QD_*` id must equal `mint(triple)` ± salt, or be snapshot-grandfathered (the
  grandfather set shrinks to ≈ nothing for non-BBL once migration completes).
- **Check 5 (alias hygiene) generalizes and becomes load-bearing.** For *every*
  OFL-carried family (not just `Generic * @System`): a vendor specialization must keep the
  OFL base name (else the OFL preset un-shadows and creates a live per-printer duplicate —
  v1's P7 pattern) and non-empty `compatible_printers`, and must not declare an id. The
  C++ validator's OFL-aware `-f` (`PresetBundle.cpp:5674-5753`) already enforces the
  runtime consequence; the script check names the rename as the cause.
- **New check 8 (triple integrity):** every id-declaring family resolves a complete,
  family-consistent triple; roots missing `filament_vendor`/`filament_type` error.
- **New check 9 (succession integrity):** every retired entry's successor chain ends at a
  live tree id (or a documented cross-island hint target); no cycles; retired ids absent
  from the tree.
- Snapshot mechanism, reserved spaces (`GF*`→BBL, `QD_*`→Qidi, `P`-hex/`null`→user-custom;
  v2's do-not-reserve-`OF*` note stands), and check 1/2/4/7 are unchanged.

## 4. Migration phases

**v3.0 — tooling + prerequisites (no profile changes).** New mint + triple resolver
(reuse the flatten machinery), succession schema migration + C++ lookup helper wired into
the §2 miss paths, Moonraker map → runtime lookup, checks 3/5/8/9, tests. Carry v2's W1
client hardening (skip tray-wipe/temp-rewrite when a system preset holds the id; relax
the `DeviceManager.cpp:5252` assert; guard the `PresetBundle.cpp:3717` deref) — it ships
first regardless. **W3 type fixes land here**, *before* any re-mint: type is now a key
component, so minting before fixing OFL `Generic PETG-CF`/`Generic PE-CF` would re-id
those families twice.

**v3.1 — OFL re-mint.** Every OFL-declared id re-derives from its triple (mirrors like
`OGFA00`, generics like `OGFL99`, blocks like `OEPLAB00` — all of it); each old shipped id
gains a succession entry pointing at its replacement; the 26 BBL×OFL byte-shared ids
dissolve (BBL keeps its id; the OFL family gets its own — two-island purity). Snapshot
regenerated; full gate battery + fixture overlays (this phase touches preset-visibility
machinery only via ids, but the fixtures are cheap insurance).

**v3.2 — vendor bundles, worksheet-per-vendor (v1 machinery).** Three sub-cases:
  (a) the **391 unshipped v1 `OF*` mints** re-derive under the triple key — no succession
  entries (they never shipped; a documented one-time `--forget-never-shipped <list>` drops
  them from the ledger lineage instead of retiring them, since the ledger's rationale —
  ids live on in user presets and 3mfs — cannot apply to unreleased ids);
  (b) **true generic tunings** riding copied legacy ids (`GFL99`-class) drop their own id
  and re-point to the OFL generic family (the v1 Sovol pattern) — their old ids get
  cross-island hints where BBL owns them, succession entries otherwise;
  (c) remaining **shipped legacy ids** (numeric, name-shaped, pseudo-GF, the 57
  multi-vendor GF residue, the 10 P-hex — everything non-BBL/non-`QD_*`, ~800 ids)
  re-mint with succession entries. Retiring the 10 P-hex system ids also removes the last
  system ids from `check_ams_filament_valid`'s destructive P-gate.

**v3.3 — ongoing consolidation (optional, per-vendor, id-stable).** Hoist vendor-unique
products into OFL where a family is genuinely multi-vendor material; thanks to the
bundle-independent key this never changes ids, so it can proceed opportunistically.

## 5. Gates (every phase)

`python scripts/orca_extra_profile_check.py` exit 0; `assign_filament_ids.py --check`
exit 0; unit tests green (existing 46 + new triple/succession/adoption tests); validator
`-l 2` exit 0, `-f` tree-wide exit 0, `-r` BBL+Qidi exit 0; custom-preset fixture
archives; config-equivalence — flattened effective configs differ only in `filament_id`
(re-mints), `inherits`/`compatible_printers` (re-points, as prescribed per worksheet), and
the W3 `filament_type` corrections. New for v3: a C++ test that a retired id resolves
through the succession chain in the sync miss path, and a Moonraker test that the generic
map lookup matches the shipped OFL presets. Manual AMS smoke test (tray set → old-id
resolve → clear) stays a release-checklist item.

## 6. Accepted costs (explicit)

- **GF ↔ OFL divergence is permanent** for products in both catalogs (PolyLite PLA ≠
  `GFL00` outside BBL). The forward-looking fix is PR #12724-style filament-database
  upload, where Orca ids become first-class device artifacts.
- **Renames/type-fixes re-id families** (by design); the succession ledger absorbs the
  device/user residue, but each one is still a ledger entry and a snapshot diff to review.
- **User roots** keep whatever id they copied at creation; with succession lookup in
  `get_filament_by_filament_id` they now *resolve* instead of dangling — strictly better
  than v1's accepted residue.
- **One-time field transition**: devices holding pre-v3 ids (AFC lanes, AMS trays, cali
  records) resolve via succession on updated clients; *older* Orca versions and
  BambuStudio never resolved OFL-only ids anyway (`?`/generic fallback — unchanged for
  them).

## 7. Open decisions

1. Whether v3.2(b)'s cross-island hints (`GFL99` → OFL generic) are wanted at launch or
   deferred (pure-miss-path feature; zero risk to BBL installs, small review surface).
2. v3.3 pacing: per-vendor PRs opportunistically vs. a dedicated consolidation train.
3. Whether to fold v2's W5 (P+md5 ids for inherited *user* presets, PR #13315) into v3.0's
   C++ work or keep it a separate PR (recommended: separate; W1 is its only prerequisite).

## 8. Evidence

Carried from v2 §8 (all re-verified this session), plus: `filament_vendor` audit — 49
distinct strings tree-wide, `"Polymaker"` byte-consistent across all five PolyLite-PLA
bundles; multi-vendor different-family GF residue = 57 ids; `MoonrakerPrinterAgent.cpp:
608-658` = 23 hardcoded OFL ids; mint examples verified live (`Qidi/PolyLite PLA` →
`OFceJcLf` under the v1 key — the fragmentation this plan removes; salt determinism
`OF8afiMO`). Alias shadowing and the OFL id fallback verified at `Preset.cpp:3684-3714`
and `PresetBundle.cpp:4842-5080` during the v1 audit; per-printer `-f` semantics at
`PresetBundle.cpp:5674-5753`.
