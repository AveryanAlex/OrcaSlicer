# filament_id plan v4: dissolve the Qidi `QD_*` island

Supersedes §0.2 of `filament_id_plan_v3.md` (the "`QD_*` island — untouched, forever"
decision) and amends every v3 section that carved out `QD_*`. Everything else in v3 —
the mint rule, the succession ledger, the BBL island, checks 1–9 — stands as
implemented (v3.0–v3.2, all gates green at commit `f7c1b290fd`).

Decision driver: the island contradicts the catalog architecture v3 built. Qidi presets
carry 204 `QD_<series>_<vendor>_<typeidx>` ids over 264 declarations (49 families,
measured 2026-08-21) — one commercial product carries up to *five* ids (one per printer
series: `QIDI PLA Rapido` = `QD_0_1_1` … `QD_4_1_1`), which is exactly the
fragmentation v3 exists to remove. The island was frozen because Qidi's filament box
composes these ids from device enums and requires exactly-matching presets
(`QidiPrinterAgent.cpp:146-152`). But v3.0 shipped the machinery that makes freezing
unnecessary: the succession ledger already translates retired ids on resolution miss.
`QD_*` stops being a *preset id space* and becomes a *device protocol namespace*,
translated once at the agent edge.

## 0. The architecture change

Two tiers, **one** frozen island:

1. **BBL island — untouched, unchanged.** All v3 reasoning holds (device/RFID/cloud
   contract is external and opaque).
2. **Everything else converges on OFL product ids — now including Qidi.** All 204
   `QD_*` ids re-mint from their family triples and gain **retired** ledger entries
   (not cross-island hints: after dissolution there is no island to own them, and
   check 4's "never again" is exactly the guard we want against upstream re-adding
   them). The box keeps working because `QidiPrinterAgent` resolves its composed
   `QD_*` id through `resolve_filament_id_succession()` on miss — the same mechanism
   every other retired id already uses.

Why retire rather than hint (the one real design choice here): hints exist for ids a
*foreign catalog* owns and may legitimately re-ship (`GF*`). Post-dissolution, nothing
may ever re-ship a `QD_*` id in the profile tree — the device composes them at
runtime, the tree translates them. Retired entries make CI enforce that permanently
(`--update-snapshot` refuses resurrections, check 4 refuses occurrences); hints would
permit re-shipping, which is now always a regression. The QD→family mapping is
strictly 1:1 (verified: no `QD_*` id is claimed by more than one family), so the
mode-rule successor is unambiguous for every entry.

## 1. What the dissolution consists of

Three independent work packages, ordered for bisectability:

- **(A) Re-converge the drifted tree** — prerequisite, not Qidi-specific. The
  2026-07 main merge (`f3fa3a34bd`) brought upstream vendor updates in pre-v3 style:
  `--check` currently exits with **243 errors** (123 unsanctioned new ids, 46 new
  instantiated own-key presets, 31 override drifts, 18 reserved-space claims, 5
  hint-keys re-declared — Qidi re-added `GFB99`/`GFG99`/`GFL99`, Snapmaker re-added
  `GFG96`/`GFU99` and even four *retired* ids incl. `OGFL99`, plus Snapmaker U1
  triple errors and a GreenGate3D rename). This is the standing WF-B maintenance
  pass; the dissolution's gates cannot go green on a red base.
- **(B) C++ succession hook in the Qidi agent** — safe to land before any data
  changes (pure miss-path: while `QD_*` presets still exist, the hook never fires).
- **(C) The dissolution proper** — tooling flip + Qidi data fixes + re-mint +
  snapshot/ledger regeneration, one vendor worksheet in the v3.2 mold.

## 2. Runtime translation (work package B)

`QidiPrinterAgent.cpp:183-192` currently: compose `setting_id` → keep it if a visible
base preset declares it → else degrade to `filament_id_by_type(tray_type)` (i.e. every
QIDI-brand box slot silently becomes Generic once the ids re-mint). Insert the
succession walk between those two steps:

```cpp
} else if (!setting_id.empty() && has_visible_base_preset(bundle->filaments, setting_id)) {
    tray.tray_info_idx = setting_id;
} else {
    // Retired QD_* protocol ids forward to their minted successors via the shipped ledger.
    const std::string successor = setting_id.empty() ? std::string()
                                                     : resolve_filament_id_succession(setting_id);
    if (!successor.empty() && has_visible_base_preset(bundle->filaments, successor))
        tray.tray_info_idx = successor;
    else
        tray.tray_info_idx = bundle->filaments.filament_id_by_type(tray.tray_type);
}
```

`resolve_filament_id_succession` is `Preset.hpp:119` (loads once, cycle-guarded,
empty-safe) — no new includes needed beyond what the file already reaches through
`PresetBundle`. This one hook covers both composition paths: the numeric-series
`build_setting_id` lambda *and* the non-numeric fallback
`map_filament_type_to_setting_id` (`:325-342`), whose four hardcoded returns
(`QD_1_0_1`/`_11`/`_41`/`_50` = Generic PLA/ABS/PETG/TPU 95A) become ledger keys in
package C. Keep that function as-is but extend its comment: the returned ids are
retired ledger keys by design, and `scripts/tests/test_filament_id.py` parses the
initializer (see §4 tests) — the Moonraker treatment (`MoonrakerPrinterAgent.cpp:
619-627`) of replacing the table with name lookups was considered and not taken: the
table already routes through the same ledger as the composed ids, and two translation
mechanisms in one agent is worse than one.

Tests (same commit):

- `tests/libslic3r/test_filament_id_succession.cpp`: add a section asserting a
  `QD_`-shaped key forwards like any other (`{"QD_2_1_11", "OFnew001"}` resolves to
  `"OFnew001"`) — pins that the walk is prefix-agnostic.
- `scripts/tests/test_filament_id.py`: new test parsing the four `QD_` literals out of
  `QidiPrinterAgent.cpp::map_filament_type_to_setting_id` (mirror the parser in
  `scripts/test_moonraker_lane_data.py`) and asserting each is a ledger key whose
  chain terminates at a live tree id. **Add it marked expected-fail/skipped until
  package C lands, then flip it on** — it is the permanent code↔ledger lockstep guard.

## 3. Tooling and validation flip (work package C, first commit)

All in `scripts/assign_filament_ids.py`; every touched line measured 2026-08-21:

- `is_island_declaration` (`:327-329`) → `return vendor == "BBL"`. This single change
  pulls every `QD_*` declarer into the triple bookkeeping (`:385`) and thus into
  checks 3 and 8, into `--remint`'s domain (`:1464`), and out of the hint-key
  tolerances (`:877`, `:1100`).
- `reserved_space_owner` (`:596-604`): `QD_*` returns `(True, None)` — reserved,
  ownerless, exactly like the P-hex/user-custom space. Consequences, all wanted:
  check 6 refuses any future vendor claim; the `--update-snapshot` sanction gate
  (`:938-963`) refuses new `QD_*` ids outright; the vanish path (`:994`) routes
  `QD_*` to **retired** (owner `None` ≠ island), not released-with-hint. Update the
  two message sites that render `owner is None` as "reserved for user-custom presets"
  (`:628` docstring, `:958`, and check 6's copy) to name the space generically or
  special-case `QD_*` ("Qidi device protocol; dissolved island — retired, never
  declarable").
- Check 1 (`:667-668`): delete the `vendor == "Qidi" and fid.startswith("QD_")`
  exemption.
- Module docstring (`:27-45`): rewrite the `QD_*` bullet — reserved space stays
  listed, but as "device protocol namespace, translated via the succession ledger;
  dissolved as a catalog island in v4, may never be declared".
- `scripts/tests/test_filament_id.py`: update the three island assertions —
  `reserved_space_owner("QD_X4_PLA")` → `(True, None)` (`:403`),
  `is_island_declaration("Qidi", "QD_X4_PLA")` → `False` (`:413`), and the
  reserved-space message case (`:570`).

No changes to the ledger schema, the C++ checks, `--retire` (post-flip it accepts
`QD_*` olds automatically — they are non-island now), or the validator: check 3's
"non-BBL, non-`QD_*`" phrasing in v3 §3 was always implemented as "non-island", so the
flip *is* the spec change.

## 4. Migration phases

**v4.0 — re-converge the drifted tree (package A).** Per-vendor WF-B worksheets over
the 243 errors: Snapmaker U1 (triple divergence `Generic|Snapmaker|snapmaker`, empty
vendors, four resurrected retired ids — these force re-mints since retirement is
permanent), BBL/addnorth `GF_AN*` overrides (BBL island: grandfather via snapshot,
they are BBL-internal), Qidi/Snapmaker re-declared hint keys (re-mint those declarers,
the check's own prescription), GreenGate3D rename, then `--update-snapshot` (new
upstream `QD_*` ids sanction cleanly — the island is still intact in this phase) and
the full v3 §5 gate battery. Bump `version` in every touched
`resources/profiles/<Vendor>.json`. **Do not start v4.2 until `--check` exits 0.**

**v4.1 — the agent hook (package B).** §2 as written; `libslic3r_tests` +
`scripts/tests/test_filament_id.py` green; behavior-neutral by construction (no `QD_*`
ledger entries exist yet).

**v4.2 — the dissolution (package C).** One Qidi worksheet, v3.2 machinery:

1. Tooling flip commit (§3). `--check` now reports the Qidi island as
   non-conformant — expected, red only between commits of this phase.
2. Data fixes, before any re-mint (the v3 "W3 lands first" lesson — type/vendor are
   key components): add `filament_vendor: ["QIDI"]` at each QIDI-brand family's
   inheritance apex so all declarers resolve it (39 of 49 families currently resolve
   none — check 8a would refuse the mint). Verify the six Generic families
   (`Generic PLA/ABS/PETG/PC/TPU 95A/PLA Silk`) resolve triples identical to their
   OFL counterparts (vendor `Generic`, OFL's `filament_type`) so they *converge onto
   the OFL ids* by triple math — the whole point; any mismatch is a W3-style data fix
   here, not a fork. No `inherits`/`compatible_printers` re-pointing anywhere: the
   series intermediates (`Generic PLA@Q2-Series` etc., all `instantiation: false`)
   simply keep declarations whose values become the OFL ids.
3. `python scripts/assign_filament_ids.py --remint Qidi` — rewrites all 264
   declarations in place to their family mints (same triple ⇒ same id across a
   family's series intermediates and per-nozzle declarers; convergence with OFL ids
   is legal by design, `want_id` already permits same-triple collisions `:1447`).
4. `--update-snapshot` — retires every vanished `QD_*` id with its mode-rule
   successor. **Audit the ledger diff: all ~204 new entries must be `QD_*`→non-null.**
   A null successor (possible only for a declared-only id with zero instantiated
   claims) gets an explicit `--retire "QD_x=OFy"` with the family's minted id.
5. Un-skip the §2 lockstep test. Bump `resources/profiles/Qidi.json` version. Full
   gate battery (§5).

Known residue, accepted: `QIDI PC-ABS-FR` (series 1–2) vs `QIDI PC/ABS-FR`
(series 3–4) are two preset-name families for one product → two ids. Renames were
ruled out in v3 (`renamed_from` rejected for migrations); if upstream ever unifies the
name, content-addressing re-ids and the ledger absorbs it — self-healing, no action
now.

**v4.3 — OFL consolidation (unchanged).** v3.3 stays optional and id-stable; QIDI-brand
families are vendor-unique and stay in the Qidi bundle.

## 5. Gates (per phase, delta from v3 §5)

Unchanged battery: `orca_extra_profile_check.py` exit 0, `assign_filament_ids.py
--check` exit 0, `scripts/tests/test_filament_id.py` all green, `libslic3r_tests`
green, validator `-l 2` tree-wide + `-f` tree-wide + `-v Qidi` exit 0, custom-preset
fixture archives (v4.2 touches no preset visibility, but they are cheap insurance —
run them for v4.0, which touches instantiation-adjacent upstream drift), flatten
config-equivalence. New for v4.2: equivalence diff may contain **only** `filament_id`
value changes (QD→OF) and the added `filament_vendor` keys on QIDI-brand families;
ledger diff audit per §4.4; the map-literal lockstep test. Manual release-checklist
item: Qidi box smoke test — slot holding a QIDI-brand material must surface the brand
preset (not Generic) on an updated client, via the memory-documented local Klipper
test rig.

## 6. Accepted costs (explicit, new relative to v3)

- **Every Qidi box slot resolves through the ledger miss-path forever** (one hash-map
  walk per slot per status poll — negligible, and structurally identical to how
  every retired id already resolves). The QD→OF translation table is maintained by
  the retirement machinery, not by hand.
- **Older Orca clients** (pre-v4 profiles) paired with re-minted profile trees lose
  QIDI-brand slot matching (they look up `QD_*` and fall back generic-by-type — the
  degradation the hook removes for updated clients). Same one-time field-transition
  shape v3 §6 already accepted for AMS/AFC ids.
- **Two ids for PC-ABS-FR** until upstream unifies the family name (§4 residue).

## 7. Evidence

Measured on this tree 2026-08-21 unless cited to v3: 204 distinct `QD_*` ids / 264
declaring presets / 49 families, QD→family strictly 1:1, per-series id sets
(`QD_0..4_1_1` = `QIDI PLA Rapido` etc.); 39 families resolve no `filament_vendor`;
declarations live on `instantiation:false` series intermediates (Q2/Q2C/X5/X4) and
per-nozzle X-Plus-4 presets; composition + miss-fallback at
`QidiPrinterAgent.cpp:146-152, 183-192`, hardcoded fallback table `:325-342`;
succession helpers `Preset.hpp:109-119`; ledger = 575 retired + 194 hints, zero `QD_*`
entries; snapshot holds 160 of the 204 (the 44 newcomers are post-merge drift);
`--check` = 243 errors, categorized in §1(A); island exemption mechanics at
`assign_filament_ids.py:327-329, 385, 596-604, 667, 877, 994, 1464, 1514`; `--remint`
same-triple convergence guard `:1447-1453`; retirement-permanence gate `:965-976`.
