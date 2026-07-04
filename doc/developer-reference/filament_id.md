# Filament IDs (`filament_id`)

`filament_id` identifies a **material family**: one commercial product line = one id, shared by
all of that material's per-printer / per-nozzle variants. Devices use it to match a physical
spool or tray to a filament preset. It is never per-color, per-printer, per-nozzle, or
per-preset (per-preset identity is `setting_id`).

This page is the rule for authoring `filament_id` in system profiles
(`resources/profiles/**`). CI enforces everything below; the short version is:

> [!IMPORTANT]
> **Never write a `filament_id` value by hand.** New families get their id from
> `python scripts/assign_filament_ids.py`; existing families already have one — inherit it.

## Who consumes the id

Every device integration funnels a tray material id (`tray_info_idx`) through the same
matching pipeline (`PresetBundle::sync_ams_list` and friends):

| Ecosystem | Where the id comes from |
| --- | --- |
| Bambu AMS | device side (RFID / user tray setting) — the `GF*` catalog |
| Qidi box | built from device enums (`QD_*`); needs an exactly matching visible preset |
| Creality CFS | runtime brand/type scoring returns the current preset's id |
| Klipper (AFC / Happy Hare) | runtime lookup by filament type |
| Snapmaker | runtime color/vendor/type match |

Tray-to-preset matching is printer-scoped, but **several consumers match globally by id alone,
first hit wins**: tray display names, `filament_is_support`, vitrification warnings, and
multi-nozzle filament grouping in the slicing pipeline. Two *different* materials sharing one
id feed wrong data to those consumers even when the presets live in different vendors — so
cross-material id sharing is never safe. Within one printer, duplicate ids silently break AMS
matching (first match wins, the tray-edit dialog hides the second preset); the profile
validator's `-f` check rejects this.

## Do I need a new id? The one-question test

> **Would a user consider this a different spool product than anything already in the tree?**

Different polymer, different sub-brand (Basic / Matte / Silk / HF), fiber-filled sibling, or a
second selectable diameter → **new family, new id**. The same spool tuned for another printer
or nozzle → **join the existing family** (inherit its `@base`, write no id key). Tuning a
generic material → **join the OrcaFilamentLibrary family** (inherit `Generic X @System`, keep
the `Generic X` base name, write no id key).

| Situation | id |
| --- | --- |
| Per-printer / per-nozzle variant of an existing material | same id (inherit, never write the key) |
| Sub-brand or product line (PLA vs PLA Matte vs PLA Silk vs PLA HF) | new id each |
| Color | never a new id |
| Second diameter selectable on the same printer (1.75 + 2.85) | sibling family, new id |
| "High-speed" tuned for a *different printer model* | same id (it is a printer variant) |
| "High-speed" selectable *alongside* the normal preset on one printer | new id (it is a product line) |

## Structure rules

1. **Only family roots carry the key.** Root presets (any preset *not* marked
   `"instantiation": "true"`, typically `<Family> @base` with `"instantiation": "false"`)
   declare `filament_id`; instantiated variants inherit a root and never
   write the key. A family may have several roots (e.g. per-series bases) — all of them must
   declare the *identical* id.
2. **The family name is the base name**: the preset name with everything from the first
   (optionally space-preceded) `@` stripped. `MyBrand PLA @Orca 3D Fuse1` and `MyBrand PLA@HS`
   both belong to family `MyBrand PLA`.
3. **Within a family, variants' `compatible_printers` are pairwise disjoint** — per printer
   preset, at most one compatible instantiated preset per id. The C++ validator (`-f`)
   enforces this.
4. **Generics belong to OrcaFilamentLibrary.** A vendor tuning a generic material inherits
   `Generic X @System`, keeps the `Generic X` base name (that alias is what hides the library
   preset on your printers), sets a non-empty `compatible_printers`, and writes no id key.
   A vendor-*branded* filament never rides a generic family id.
5. **Ids follow the product identity.** The id is a pure function of the product triple
   `(filament_vendor, filament_type, family name)` — correcting any of them re-mints the id
   **by design**, and `--update-snapshot` records the old id in the shipped succession ledger
   with its successor so device trays, calibration records, and user presets keep resolving
   (`renamed_from` still gates preset-*name* compatibility as before). A shipped id is never
   recycled for a different material: retired ids are blocked forever.

## Minting — nobody invents ids

New ids are deterministic, computed exactly like the `setting_id` precedent
(`scripts/assign_vendor_setting_ids.py`):

```text
FILAMENT_ID_NAMESPACE = uuid5(setting-id NAMESPACE, "filament_id")
                      = c4d3ff49-4c32-5534-a3e3-00894157ab97
filament_id = "OF" + base62_6( uuid5(FILAMENT_ID_NAMESPACE,
                  "filament_product/<filament_vendor>/<filament_type>/<family_name>") )
```

`base62_6` is the low 6 base62 digits (alphabet `0-9A-Za-z`) of the UUID taken as a big-endian
integer, most-significant digit first — 8 chars total, within the AMS length limit. The triple
comes from the family root's *flattened* config: `<filament_vendor>` is the filament
**manufacturer** (`"Polymaker"`, or `"Generic"` for generics — never the printer brand),
`<filament_type>` the material type, `<family_name>` the root's base name; the two config
values are inheritable list options and the first element counts. The key contains no bundle
name, so the same product mints the same id in every bundle — hoisting a family into
OrcaFilamentLibrary never changes its id. On the rare collision with any existing or retired
id, the minter salts the input (`…/1`, `…/2`, …) until free and the result is frozen in the
file. Example: `Polymaker/PLA/PolyLite PLA` mints `OF5CgdDq`.

Workflow for a new family:

```bash
# 1. Author the family with NO filament_id key anywhere.
python scripts/assign_filament_ids.py                    # 2. mint + insert ids into the family root(s)
python scripts/assign_filament_ids.py --update-snapshot  # 3. record the new claims in the ledger
python scripts/assign_filament_ids.py --check            # 4. verify — the same checks CI runs
# 5. Commit the profile edits together with scripts/filament_id_snapshot.json.
```

`--mint "filament_vendor/filament_type/family_name"` prints the id a triple would mint without
touching anything. The default run is idempotent and never rewrites a valid existing id.
Maintenance modes (normally only used by id migrations): `--remint VENDOR` re-derives a
vendor's declared ids from their triples (a declaration already equal to a salt iteration
of its own triple is conformant and left alone — deliberate salt splits keeping two
presets of one product apart for per-printer AMS matching survive),
`--drop-redundant-ids VENDOR` deletes declarations
that merely re-declare an inherited OFL id, `--add-hint "OLD=NEW"` records a cross-island
succession hint, and `--retire "OLD=NEW"` records succession for a shipped non-island id
that vanished while another declarer kept it alive (lineage the automatic claim vote can no
longer see).

If you skip the tooling, CI fails and prints the remedy: the expected id for your family, and
the instruction to run `python scripts/assign_filament_ids.py --update-snapshot` and commit
the resulting diff.

## Reserved namespaces — never mint or hand-write into

| Space | Owner | Rule |
| --- | --- | --- |
| `GF*` | Bambu AMS/RFID catalog | BBL vendor only; byte-copies elsewhere only where the snapshot already sanctions them |
| `QD_*` | Qidi device protocol | frozen device contract; Qidi vendor only |
| `P` + 7 hex chars (case-insensitive), `"null"` | user-created custom filaments (`CreatePresetsDialog.cpp`) | never appears in system profiles |
| every already-shipped id | grandfather snapshot | frozen as-is; new claims need maintainer sign-off |
| every retired id | `resources/profiles/retired_filament_ids.json` | never used again, for anything |

## The succession ledger

`resources/profiles/retired_filament_ids.json` ships with the app. Each retired id maps to
`{"claims": [...], "successor": <id|null>}` — successor chains are followed to the live end —
and a `hints` map carries the same forwarding for ids Orca cannot retire because another
island owns them (e.g. a `GF*` id whose material also exists as an OFL family). The client
consults the ledger **only on resolution miss** (AMS tray sync, tray-id type lookup,
calibration history, filament-id preset lookup): a live preset always wins first, so BBL
installs resolve `GF*` natively and behavior is unchanged wherever the raw id still exists.
This is what makes identity-driven re-mints (structure rule 5) safe: the old id keeps
resolving to the family's current preset instead of degrading to a `Generic <type>` fallback.
The file is append-only and maintained exclusively by `--update-snapshot` / `--add-hint` /
`--retire`.

## How CI enforces this

Profile CI (`check_profiles.yml` → `scripts/orca_extra_profile_check.py`) runs
`check_filament_ids()` tree-wide. Its ground truth is
**`scripts/filament_id_snapshot.json` — the sanctioned state**: the id state derived from the
tree must equal the snapshot exactly, in both directions. Any change to the id landscape
therefore surfaces as a diff to that file, and **that snapshot diff is what maintainers review
and gate in a PR**. Never edit the snapshot by hand — `--update-snapshot` regenerates it
deterministically (running it twice changes nothing).

The checks, in brief:

- **Format** — every id is either in the snapshot, `OF` + 6 base62 chars, BBL's, or Qidi `QD_*`.
- **Snapshot equality** — tree claims == snapshot claims **and** tree triples == snapshot
  triples, both directions: any `filament_vendor`/`filament_type`/family-name change surfaces
  as a snapshot diff.
- **Mint conformance** — a non-grandfathered `OF*` id must equal the mint (or a salt
  iteration) of its declarer's product triple; the error prints the expected id.
- **Retired reuse** — any tree id present in `resources/profiles/retired_filament_ids.json` is an error.
  Ids that fully vanish from the tree are appended there by `--update-snapshot`; the file is
  **append-only**.
- **Alias hygiene** — any vendor preset riding an OFL family id must keep the library
  preset's base name, a non-empty `compatible_printers`, and no own id key (structure rule 4);
  the error names the rename as the cause.
- **Triple integrity** — every declarer outside the BBL/`QD_*` islands must resolve a
  non-empty `filament_vendor` and `filament_type` (generics use `"Generic"`), and all
  declarers of one family within a bundle must agree on the triple.
- **Succession integrity** — retired successor chains terminate at a live id (or null) with
  no cycles; `hints` keys are live, island-owned ids.
- **Reserved namespaces** — `GF*` outside BBL, `QD_*` outside Qidi, `P<7-hex>` or `null`
  anywhere, unless that exact claim is grandfathered in the snapshot.
- **Structure** — no `filament_id` key on instantiated presets; no declared-vs-inherited id
  drift; every instantiated system filament must resolve an effective id through its
  `inherits` chain (an id-less one is a hard load error in C++ that discards the whole vendor
  bundle).

Sharing a **reserved-catalog** id with a new family or vendor (e.g. shipping a Bambu-cataloged
product under another vendor with its authentic `GF*` id) is refused by `--update-snapshot`
unless you pass `--allow-shared-catalog` — and it still lands in the snapshot diff for
maintainer review. Any other new sharing of an existing id is caught by the mint-conformance
check instead.

## FAQ

- **A new color of an existing product?** Never a new id — colors are not families.
- **A second diameter (1.75 mm and 2.85 mm) of the same product?** A sibling family with its
  own id: two diameters are separately selectable spool products.
- **A high-speed tune of an existing material for another printer model?** Same family:
  inherit the family's root, write no id key.
- **A tuned generic ("our profile for Generic PLA")?** Inherit `Generic PLA @System`, keep the
  `Generic PLA` base name, set `compatible_printers`, write no id key.
- **I need to rename a family (or fix its `filament_vendor`/`filament_type`).** Add
  `renamed_from` for the name, run `--remint <Vendor>` then `--update-snapshot`: the id
  re-derives from the corrected identity and the old id lands in the succession ledger
  pointing at the new one. Commit the profile, snapshot, and ledger diffs together.
- **CI says my family needs an id.** Run `python scripts/assign_filament_ids.py`, then
  `--update-snapshot`, and commit both diffs. Do not type an id by hand.

For general profile authoring, see the profile development guide on the
[OrcaSlicer wiki](https://www.orcaslicer.com/wiki).
