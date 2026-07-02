# filament_id: generation rule + system-profile fix plan

Follow-up to PR #14459 (commit `c2e91cb8`, validator `-f` / `check_duplicate_filament_subtypes`).
Goal 1: a filament_id generation rule for all vendors. Goal 2: an error-pattern taxonomy and a
migration plan that removes every ambiguous filament_id, without touching Bambu (BBL) profiles.

All numbers below are reproducible: a loader-faithful audit script re-derives the validator's
output **exactly** (1256/1256 printer-level errors, 356 logical collision groups, 30 vendors).
Every code claim was verified against source with `file:line` references.

---

## 0. Executive summary

- `filament_id` is a **material-family id**: one id per commercial product line, shared by all
  of that material's per-printer/per-nozzle variants. Matching is always `(filament_id +
  printer compatibility)`; the invariant from PR #14459 is *per printer preset, at most one
  compatible instantiated filament preset per id*.
- Every modern device ecosystem funnels through this id — not just Bambu AMS: Qidi box,
  Creality CFS, Klipper AFC/Happy Hare, Snapmaker all emit/consume `tray_info_idx`
  (see §1). Several consumers match **globally, without printer scoping**, so two *different
  materials* sharing one id is unsafe even across vendors.
- **Proposed rule (§3):** deterministic, script-minted ids — `OF` + 6 base62 chars from
  `uuid5(vendor + family)`, declared **only on family-root (`@base`) presets**; variants inherit.
  Nobody ever invents an id by hand; CI prints the expected id when one is missing. Existing
  unambiguous ids are grandfathered and frozen; `GF*` (Bambu), `QD_*` (Qidi device protocol),
  and `P<7-hex>` (user custom presets) are reserved namespaces that must never be minted into.
  This was selected by an adversarial design/judge process over a registry-grammar alternative
  and a structure-only alternative, then stress-tested; amendments from that stress test are
  folded in below.
- **Migration (§5):** fresh-never-reused ids only, family-atomic, names never changed. This is
  provably safe: user presets re-derive `filament_id` from their parent on every load
  (`Preset.cpp:1658-1682`), 3mf resolves presets by name+config (`Preset.cpp:2490-2576`), and
  Klipper/Creality/Snapmaker derive tray ids at runtime. One PR per vendor; CI's `-f` scope
  ratchets per vendor until tree-wide.

---

## 1. How filament_id actually works (verified)

### Consumers and scoping

All device integrations converge on one pipeline: device/agent sets `tray_info_idx` →
`DevAmsTray.setting_id` → `Sidebar::build_filament_ams_list` (`Plater.cpp:3423-3493`) →
`PresetBundle::sync_ams_list` / `get_ams_cobox_infos` (`PresetBundle.cpp:3112-3308`) match it
against filament presets.

| Ecosystem | Where the id comes from | Effect of changing a system id |
|---|---|---|
| BBL AMS | device-side (RFID / user tray setting), `DeviceManager.cpp:3823+` | breaks matching — **frozen by mandate** |
| Qidi box | built from device enums: `"QD_" + series + "_" + vendor + "_" + type_idx`, `QidiPrinterAgent.cpp:146-152`; needs an exactly-matching visible preset | breaks matching — **`QD_*` ids are a frozen device contract** |
| Creality CFS | runtime brand/type scoring returns current preset's id (`CrealityPrintAgent.cpp:46-118`) | invisible |
| Klipper (AFC / Happy Hare) | runtime `filament_id_by_type` (`MoonrakerPrinterAgent.cpp:808,936`) | invisible |
| Snapmaker | runtime color/vendor/type match (`SnapmakerPrinterAgent.cpp:22-64`) | invisible |

Matching is printer-scoped (`is_compatible`) in the AMS sync paths and all printer agents — this
is what makes the per-printer invariant sufficient there. But several consumers match
**globally by id alone**, first match wins:

- `get_filament_by_filament_id("")` — tray display name, `filament_is_support`,
  `temperature_vitrification` warnings (`PresetBundle.cpp:690-733`; callers
  `DevFilaBlackList.cpp:70`, `Plater.cpp:3453`, `SelectMachine.cpp:3560,4623`). The code
  comment at `:695` states the assumption outright: an id maps to ONE material globally.
- `MachineObject::setting_id_to_type` (`DeviceManager.cpp:2538`), calibration-history name
  lookup (`CaliHistoryDialog.cpp:62`), custom-filament cloud grouping (`Preset.cpp:2839`).
- The slicing pipeline itself: multi-nozzle filament grouping merges project filaments whose
  `(filament_id, color)` match (`FilamentGroup.cpp:513-528` via `ToolOrdering.cpp:1164`).

**Consequence:** within one printer, duplicate ids break AMS matching (silent first-wins,
`find_if` at `PresetBundle.cpp:3132/3233`; the AMS tray-edit dialog even *hides* the second
preset, `AMSMaterialsSetting.cpp:894-897`). Across vendors, the same id on *different
materials* feeds wrong name/type/vitrification data to the global consumers and can merge
different materials into one nozzle group. Same id on the *same* material (e.g. `GFL99` =
Generic PLA in 29 vendors) is comparatively benign — those attributes agree.

### Identity machinery

- **Effective id resolution** (`PresetBundle.cpp:4842-5080`): own `filament_id` key → vendor
  `filament_id_maps[inherits]` (file order in the vendor index is load-bearing) →
  OrcaFilamentLibrary base-bundle map. An instantiated system filament that resolves *no* id is
  a hard load error that discards the whole vendor bundle (`:5072`, throw at `:5141-5147`) —
  so "missing id" cannot ship; what looked like 26 id-less Flashforge presets actually resolve
  to OFL's `OGFL99`/`OGFG99` through the base-bundle fallback.
- **Two family-identity systems exist**: `filament_id` (device matching) and `alias` (name
  before `" @"`). OFL shadowing is keyed on **alias**: `update_library_profile_excluded_from`
  (`Preset.cpp:3684-3714`) hides an OFL preset (empty `compatible_printers` = compatible with
  everything, `Preset.cpp:837`) on printers claimed by a same-alias vendor preset. There is
  **no id-based shadowing**. A vendor preset that tunes an OFL generic but renames it
  re-exposes the OFL preset and creates a live duplicate. The rule below aligns the two
  systems: one family = one alias = one id.
- **User-custom id space**: user-created filaments get `"P" + md5(name)[0:7]` (8 chars,
  `CreatePresetsDialog.cpp:533`), or *reuse a system id* when the base name matches an existing
  preset (`:510-528`). `"null"` is used as a sentinel. Root user presets persist their id
  forever; inheriting user presets re-derive it on every load.

## 2. The id landscape today

**Bambu's grammar** (derived from all 1970 BBL instantiated presets; BBL is internally clean —
181 id definitions, 0 duplicates):

- Classic `GF<LETTER><NN>`: letter = family (A Bambu-PLA, B ABS/ASA, C PC, G PETG/PCTG,
  L third-party+generic PLA, N PA/PPA, P PP/PE, R misc, S support, T PET/PPS, U TPU).
  Numbers: 00-49 branded ascending, 50-59 fiber-filled, 60-70 partner block, **95-99 generic
  tier descending** (99 = the family's plain generic).
- Brand partners `GF<BRANDCODE><NNN>`: GFPM Polymaker, GFOT Overture, GFSNL SUNLU, GFNMK
  Numakers. One id per product line; never per color, never per printer/nozzle/diameter.
- Structural rule: the id lives on the material's `@base`; every variant inherits it.
- Hardcoded in C++: `GFS00/GFS01` support check (`DeviceManager.cpp:4739`), per-family PA
  defaults `GFU01/03/04` (`CalibUtils.cpp:54-75`) — `GF*` is Bambu's space, byte-frozen.

**Everything else is ad-hoc, invented by individual contributors and imitated** (full history
in §7): OFL's `O`-prefix mirrors (`OGFA00`; introduced 2025-03-31, commit `8c4a65e3e1`),
Tiertime/Afinia `GFx##_##` per-printer-line suffixes, SeeMeCNC per-nozzle ids, LH `LHF_pla`,
LONGER 11-char pseudo-GF ids, Anycubic ids **with spaces** (`"GFPLA Silk"`), Prusa ids that are
entire preset names (36 chars), and mass copy-paste of `GFL99/GFB99/GFG99` onto everything
(Qidi alone stamped `GFB99` into **317 files** across all materials). The only guidance that
ever existed was "≤ 8 chars" — enforced for BBL only (`orca_extra_profile_check.py:292,320`),
and the (now removed) profile wiki's own examples *taught* id copy-pasting.

**The damage, quantified** (audit reproduces validator 1256/1256):

| Ledger | Count |
|---|---|
| Within-vendor logical collision groups (validator `-f`) | **356** across 30 vendors (1256 printer-level errors) |
| OFL×vendor same-id groups (validator blind spot) | 42 — of which **10 are live** (alias mismatch defeats shadowing); 32 already neutralized by alias shadowing |
| OFL-internal: one id, several materials, visible on every printer | **16 ids** (e.g. `OEPLAB00` = 14 distinct Elegoo PLA products; `OGFL06` = eSUN PLA-Marble *and* Fiberon PETG-ESD) |
| Cross-vendor semantic collisions (same id, different materials) | **67 ids** (e.g. `GFU99` also covers a PEBA; Anycubic minted `GFL95` "Matte" ≠ Bambu `GFL95` "High Speed") |

Worst vendors by groups: Qidi 97, Flashforge 69, Elegoo 37, Prusa 31, Cubicon 17,
Anycubic 14, InfiMech 14, Snapmaker 11, Artillery 8, Creality 8, FlyingBear 8.

---

## 3. The rule (proposal)

Selected by a 3-design / 2-judge adversarial process (deterministic-mint won over
registry-grammar and structure-only on ambiguity-prevention, contributor simplicity, and
enforceability), then hardened by three adversarial review passes. This section is written as
the future authoring doc.

### 3.1 The one-question test

> **Would a user consider this a different spool product than anything already in the tree?**
> Different polymer, different sub-brand (Basic / Matte / Silk / HF), fiber-filled sibling, or
> a second selectable diameter → **new family, new id**. The same spool tuned for another
> printer or nozzle → **join the existing family** (inherit its `@base`, no id key). Tuning a
> generic material → **join the OFL family** (inherit the `Generic X @System` preset, keep the
> `Generic X` base name, add no id).

Same id / new id at a glance:

| Situation | id |
|---|---|
| Per-printer / per-nozzle variant of an existing material | same id (inherit, never write the key) |
| Sub-brand or product line (PLA vs PLA Matte vs PLA Silk vs PLA HF) | new id each |
| Color | never a new id |
| Second diameter selectable on the same printer (1.75 + 2.85) | sibling family, new id |
| "High-speed" tuned for a *different printer model* | same id (it's a printer variant) |
| "High-speed" selectable *alongside* the normal preset on one printer | new id (it's a product line) |

### 3.2 Structure

1. **One family = one root.** Each material family has root preset(s) (`instantiation:false`,
   typically `<Family> @base`) and only roots carry the `filament_id` key. Instantiated
   variants inherit a root and never write `filament_id`. (A family MAY have several roots —
   e.g. Qidi's per-series bases — but they must all declare the *identical* id.)
2. **Family identity is declared, not name-derived.** Default: the family name is the
   instantiated presets' base name (name with `/\s?@.*$/` stripped — note *optional* space,
   because `Afinia PLA@HS`-style names exist). When vendor naming makes that ambiguous, the
   root declares an explicit `"filament_family"` key that overrides derivation; tooling errors
   loudly when a root's derived family differs from its children's.
3. **Within a family, variants' `compatible_printers` are pairwise disjoint** — that *is* the
   PR #14459 invariant, enforced by the validator.
4. **Generics belong to OFL.** A vendor tuning `Generic PLA` inherits
   `Generic PLA @System`, keeps the `Generic PLA` base name/alias (so alias shadowing excludes
   the OFL preset on those printers, `Preset.cpp:3684`), sets non-empty `compatible_printers`,
   and writes no id. A vendor-*branded* filament never rides a generic family id.
5. **Ids are immutable once shipped.** Renaming a family does not change its id (use
   `renamed_from`). No id is ever recycled for a different material — stale ids live on in
   user root presets and old 3mfs, and a recycled id would silently match the wrong material.

### 3.3 Minting — nobody invents ids

New family ids are computed, exactly like the `setting_id` precedent
(`scripts/assign_vendor_setting_ids.py` / `Slic3r::generate_preset_setting_id`):

```
filament_id = "OF" + base62_6( uuid5( NAMESPACE, "filament_family/<vendor>/<family>" ) )
```

8 chars total (satisfies the AMS length limit), same base62 derivation and a dedicated
namespace constant. On the astronomically rare collision with an existing id, the minter salts
the input (`.../1`, `/2`, …) until free; the result is simply frozen in the file.

- **Script path:** author commits the family with *no id anywhere*;
  `python scripts/assign_filament_ids.py` inserts the minted id into the root(s). Idempotent;
  never rewrites a valid existing id. A `--mint "<Vendor>/<Family>"` one-shot prints the id
  without touching the tree.
- **No-script path:** CI fails with the exact line to paste:
  `family "MyBrand PLA" (vendor X) needs filament_id "OFq3xT9k" in "MyBrand PLA @base.json"`.

**Reserved namespaces — never mint or hand-write into:**

| Space | Owner | Status |
|---|---|---|
| `GF*` | Bambu AMS/RFID catalog | byte-copies of authentic Bambu ids only, and only where a checked-in `shared_catalog` list sanctions the family (BBL bundle; OFL mirrors; byte-matching families in other vendors, e.g. Snapmaker's Fiberon) |
| `QD_*` | Qidi device protocol | frozen; Qidi-only; exempt from family-shape checks |
| `P[0-9A-Fa-f]{7}`, `"null"` | user-created custom filaments (`CreatePresetsDialog.cpp:533`) | never emitted for system presets (reserve case-insensitively) |
| everything already shipped | grandfather snapshot | frozen as-is (§5) |

Trade-off accepted: minted ids are opaque (`OFq3xT9k` carries no "PLA" mnemonic — the family
name in the same file provides that). The judges preferred this over a Bambu-style extended
grammar because it removes the "who allocates the next number" ceremony, cannot race between
concurrent PRs, and needs no registry maintenance. If mnemonic ids are strongly preferred, the
runner-up design (`<NSCODE><FAMLETTER><SEQ>` + registry file) is documented in the workflow
records; everything else in this plan is unchanged under either format.

### 3.4 What CI enforces (all vendors, ratcheted)

Extend `scripts/orca_extra_profile_check.py` (it imports the same mint function; setting_id
precedent) and the C++ validator:

1. **Format**: an id is valid iff `OF[0-9A-Za-z]{6}` **or** in the grandfather snapshot **or**
   vendor==BBL **or** `QD_*` in Qidi. No whitespace/ASCII/length checks needed outside the
   grandfather set — new ids are minted, and the grandfather set is closed.
2. **Uniqueness ratchet**: id→families multimap computed tree-wide; no id may acquire a family
   claim not recorded in the snapshot (snapshot legalizes today's benign `GFL99`-style sharing;
   new multi-claims are errors; `shared_catalog` entries are the sanctioned exception).
3. **Structure ratchet**: `filament_id` key only on roots; every instantiated filament must
   resolve an effective id via the simulated loader walk; all members of one family resolve the
   same id; **a preset may not declare an id different from its inherited effective id** (the
   `Generic SBS` drift bug class). Pre-existing violations are snapshot-frozen; new ones error.
4. **Mint conformance**: an id new relative to the snapshot must equal the mint (or a salt
   iteration); the error message prints the expected value.
5. **Stability**: an `(id, family)` pair on main may not change or vanish, *following
   `renamed_from` chains* (so an honest rename passes), unless listed in a maintainer-gated
   migrations file. Retired ids go to an append-only `retired_ids` ledger; a retired id may
   never be defined again for any family.
6. **Alias hygiene for tuned generics**: a preset inheriting an OFL `Generic * @System` (with
   no own id anywhere in its vendor chain) must keep the OFL base name and have non-empty
   `compatible_printers` — error message names the rename as the cause.
7. **C++ validator**: extend `check_duplicate_filament_subtypes` (`PresetBundle.cpp:5654`) to
   include OFL presets in every vendor's per-printer check, *minus* alias-excluded ones —
   `m_excluded_from` is already populated in the validator context (`update_system_maps` at
   `PresetBundle.cpp:2302`), so the 32 shadowed pairs won't false-positive and the 10 live ones
   will be caught. Run `-f` per vendor in CI, widening as vendors are cleaned (§5).
8. **Runtime backstop** (one-line change): log a warning when the AMS-sync `find_if`
   (`PresetBundle.cpp:3132/3233`) finds 2+ compatible presets for one id — the only layer that
   can see side-loaded/forked bundles.

---

## 4. Error-pattern taxonomy (goal 2) — with counts and fixes

356 groups were classified by 37 agents reading the actual profiles, spot-checked
independently (1 substantive disagreement in 14 samples). Counts below fold the spot-check
corrections in. **Fix rule for all patterns: replacement id values are always freshly minted
`OF*`; the "which preset keeps the id" decision uses the precedence _Bambu-catalog material >
OFL generic family > family that historically introduced the id_.**

| # | Pattern | Groups | Fix |
|---|---|---|---|
| P1 | `copy_paste_id` — different materials share an id verbatim (task's error 1). Qidi's `GFB99`×317-files epidemic; Anycubic's three id "eras"; Peopoly `GFSL99` on ABS | ~167 | impostor families get minted ids on their roots; owner keeps the id |
| P2 | `wrong_inherits` / id-less product lines — a variant inherits another family's root (PR #14459's Panchroma case) or a product line never got its own root (Prusa HF; Flashforge's ~140-preset `FFG01` umbrella) | ~87 | create per-family roots with minted ids; re-point `inherits`; **never** delete the id-less shadow/base files — convert them (they carry real config: Flashforge `fdm_filament_pla` differs materially from OFL's) |
| P3 | `generic_family_overlap` — vendor-*branded* preset rides a generic family id via inheritance (task's error 2, generalized). Includes the 10 live OFL duplicates | ~68 | branded presets get minted family ids; true generic tunings instead adopt the OFL family *with matching alias* (rule 3.2.4); Sovol is the elegant case — just **delete** its wrong own-id lines and let OFL ids flow through inheritance |
| P4 | `overclaim_compat` — same material, broader variant claims a printer that a dedicated variant covers (the BBL H2DP pattern fixed in #14459) | ~24 | trim `compatible_printers` of the broader preset (Dremel, Cubicon `@base`s that are also instantiated, Wanhao France Bowden/Direct) |
| P5 | template-carried id — id declared on a shared settings template (`fdm_filament_*`, `fdm_filament_common`) so every family inheriting it collapses (Prusa, Ginger Additive, Snapmaker TPU base) | inside P1/P2 counts | move ids off templates onto family roots |
| P6 | OFL-internal collisions — 16 ids spanning several materials, visible on every printer (Elegoo blocks, Elas `OGFA00`×3, `OGFL06` polymer mismatch, `Generic PETG HF/PETG-CF @System` missing own ids) | 16 ids | fix inside OFL first (it's the base bundle every vendor resolves against) |
| P7 | alias-mismatch re-exposure — vendor tunes a generic under a different name, OFL preset resurfaces (Snapmaker `PolyTerra J1 PLA` vs OFL `PolyTerra PLA`) | 10 live | rename-to-alias where it's genuinely the same family, else mint |
| P8 | per-variant ids — no ambiguity, but family semantics broken: every variant has its own id (Prusa name-ids, SeeMeCNC nozzle suffixes, Afinia/Tiertime `_##`, iQ) so device matching can't identify the material across nozzles | ~210 presets | grandfather (they're unambiguous); converge opportunistically; document as anti-pattern |
| P9 | format violations — spaces (`"GFPLA Silk"`), >8 chars (LONGER, SeeMeCNC, LH), GF-shaped inventions (Anycubic `GFL93-97`, CoLiDo `GFA99`) | in the above | fixed as a byproduct of re-minting; snapshot freezes the unambiguous rest |
| P10 | cross-vendor semantic collisions — 67 ids meaning different materials in different vendors (dangerous via the global unscoped consumers, §1) | 67 ids | mostly eliminated by P1-P3 re-minting; the remaining same-material generic sharing is legalized by the snapshot |
| P11 | deliberate coexistence & data hygiene — Snapmaker "Benchy" demo presets (gated by `compatible_prints`, which the id check can't see) and a self-collision from duplicate `compatible_printers` entries | 4 | give demo presets own minted ids; dedupe list entries; add a lint for duplicate array entries |

New patterns beyond the two in the task (goal 2.3): P5-P11.

---

## 5. Migration plan

### Safety foundation (verified, §1/§7)

Safe: fresh never-used ids, family-consistent; trims of `compatible_printers`; inherits
re-pointing; OFL id changes (children re-derive). Unsafe: touching `BBL`/`QD_*`; recycling or
swapping ids; splitting a family's id; **deleting or renaming preset names** (user presets
whose `inherits` no longer resolves are dropped at load, `Preset.cpp:1687-1691`) — if a name
must go, `renamed_from` coverage is mandatory.

### Phases

0. **Land the rule + tooling first** (no profile changes): `scripts/assign_filament_ids.py`
   (mint + insert + `--mint`), the extended `orca_extra_profile_check.py` checks in
   snapshot-ratchet mode, the C++ validator OFL cross-check, the runtime warning, the rule doc.
   Generate `filament_id_snapshot.json` (id→families multimap over main) and empty
   `retired_ids.json` — both checked in.
1. **OFL first** (it's the base bundle every vendor resolves against): fix the 16 internal
   collisions (Elas/eSUN/DREMC copy-pastes get mints; `Generic PETG HF/PETG-CF/PP-CF/PP-GF/
   PE-CF/PLA Matte @System` get their own family roots+ids instead of collapsing into their
   parent generic), keeping every current effective id that is unambiguous.
2. **Per-vendor PRs, worst-first**: Qidi → Flashforge → Elegoo → Prusa → Cubicon → Anycubic →
   InfiMech → Snapmaker → the long tail (22 vendors, mostly 1-8 one-line fixes). Each PR flips
   that vendor into CI's `-f` scope (`check_profiles.yml` currently `-v BBL -f`; append
   vendors as they reach zero; when all are in, drop `-v` and run tree-wide).
3. **Delete the ratchet allowlists** once tree-wide zero holds; checks become hard rules for
   everything born after the snapshot.

### Migration-script contract (from the adversarial pass — important)

- Consumes from the classification only: group membership, fix category, keep-id precedence,
  inherits-repoint targets. **All replacement id values are recomputed via the mint** — id
  literals in analysis notes (e.g. `GFA00_02`, `GFS98`, `GFG96`) are legacy-culture artifacts
  and are ignored with a warning; assert no emitted id matches `^(GF|QD_|P[0-9A-Fa-f]{7}$)`.
- **Family-atomic**: re-idding any preset re-ids every same-family sibling in the same commit,
  even siblings outside the collision group (FlyingBear `GFB99 @S1` vs `@Ghost7`); Prusa is
  family-atomic per material (its HF/CF families span frozen `_N`-suffix ids — freeze what's
  unambiguous, mint once per family for the colliding members).
- **Diff bound**: a vendor migration PR may only touch collision-group files + same-family
  siblings of re-idded presets; every id that is per-printer-unambiguous today stays
  byte-identical.
- **Config-equivalence gate**: dump every instantiated preset's flattened effective config on
  main and on the PR head; the diff must be empty except `filament_id`/`inherits`/
  `compatible_printers` edits the plan prescribes (this is what makes the Flashforge
  shadow-file conversions safe: each id-less `fdm_filament_*` shadow becomes a named vendor
  family root carrying its config byte-for-byte, children re-pointed, then the shadow name
  retired).
- **No deletions**: redundant presets (Flashforge's byte-identical `Generic X`/`Flashforge X`
  twins) are re-minted, not dropped; consolidation with `renamed_from` is a separate,
  human-reviewed cleanup.
- Expected user impact: none for inheriting user presets, 3mfs, Klipper/Creality/Snapmaker
  sync. Residue: user *root* presets that copied an old system id keep it forever
  (AMS auto-match falls back to generic-by-type — low severity, unavoidable from the repo).

### Vendor-specific notes (from classification)

- **Qidi (97)**: three profile generations. `QD_*` generics are correct and frozen; the fix is
  the brand families (Bambu/HATCHBOX/Overture/PolyLite/Tinmorry/QIDI-brand) that all carry
  `GFB99/GFG99/GFL99`. Multi-root families are the norm (`...@Q2-Series` / `@Q2C-Series` /
  `@X-Max 4-Series` bases) — same mint lands in every series root of one family.
- **Flashforge (69)**: two umbrellas (`FFG01` ~140 presets; `GFB99/GFG99/GFL99` G3U-era) +
  OFL-riding branded presets + shadow-file conversion (above).
- **Elegoo (37)**: single mistake — every commercial variant inherits the material-class
  `@base` (`E<MAT>B00`); mint one id per product line (Silk/Matte/PRO/Rapid/…), roots exist.
- **Prusa (31)**: HF product lines need their own roots; ids move off `fdm_filament_*`
  templates; frozen name-shaped and `_N` ids stay.
- **Sovol (6)**: delete the wrong own-id lines; correct ids flow from OFL by inheritance.
- **Cubicon (17)**: `@base` presets are themselves instantiated + over-claiming; 9 file edits.

---

## 6. Concrete work items (PR-sized)

1. `scripts/assign_filament_ids.py` + mint function + tests (incl. the 14-group regression set
   from the adversarial pass). *(new)*
2. `orca_extra_profile_check.py`: checks §3.4-1..6 in ratchet mode + snapshot/ledger files;
   drop the BBL/OFL-only gate at `:292` and the OFL skip at `:601`. *(extend)*
3. C++ validator: OFL-aware `check_duplicate_filament_subtypes`; runtime ambiguity warning at
   the two `find_if` sites. *(small)*
4. Rule documentation: `doc/developer-reference/filament_id.md` (recreate the path; wiki
   cross-link) + profile-PR template checkbox ("new materials: no filament_id key anywhere; CI
   prints the minted id"). *(new)*
5. OFL migration PR (phase 1).
6. Per-vendor migration PRs (phase 2), each widening CI `-f` scope.

The audit tooling from this analysis (loader-faithful resolver; reproduces the validator
1256/1256) is in this session's scratchpad (`audit_filament_ids.py`) and is the natural seed
for items 1-2.

## 7. Evidence & methodology

- Validator ground truth: `OrcaSlicer_profile_validator -f` tree-wide → 1256 errors; audit
  script reproduces exactly (356 logical groups after dedup by (vendor, id, preset-set)).
- Code analysis: 5 parallel agents over runtime consumers, persistence/migration surface,
  loader semantics, Bambu grammar, and convention history — all claims carry `file:line`.
- Classification: 37 agents (one per vendor chunk) reading actual profile JSONs; 14-sample
  independent re-derivation found 1 substantive error (a Flashforge group mislabeled
  `missing_id`; corrected — the loader resolves those ids from OFL).
- Design: 3 independent designs → 2 judges (both chose the deterministic mint; scores 58/50/56
  and 60/54/56) → 3 adversarial attackers (35 scenarios; every `breaks` finding is folded into
  §3.2-3.4/§5 as an amendment: declared families, multi-root support, ratchet-not-absolute
  checks, no-deletion rule, shadow-file conversion, classification-id-literal quarantine,
  diameter siblings, case-insensitive P-hex reservation, fork guidance).
- Key history: `OGF*` born 2025-03-31 (`8c4a65e3e1`, no PR); `_##` suffix born PR #9739;
  8-char check born PR #9574; wiki (with the id-copy-paste example) removed 2025-11-24
  (`f0d79b99eb`).

## 8. Open decisions for maintainers

1. **Id format**: opaque deterministic `OF*` mint (recommended, judges 2/2) vs mnemonic
   registry grammar (runner-up). Everything else in the plan is format-agnostic.
2. **Multi-vendor brands** (Snapmaker ships Fiberon with authentic `GF*` ids): sanction via
   `shared_catalog` (recommended, low churn) vs hoisting those families into OFL.
3. **Benchy-style demo presets**: mint ids per demo preset (recommended) vs teaching the
   validator `compatible_prints` gating.
4. **Alias alignment**: fix the 10 live alias-mismatch OFL duplicates by rename-to-alias
   (better long-term, needs `renamed_from`) vs minting vendor ids (safer, more ids).
5. **Where the rule doc lives**: in-repo `doc/` (recommended — CI messages need a stable link)
   vs wiki-only.
