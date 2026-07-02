#!/usr/bin/env python3
"""
Mint deterministic filament_id values for OrcaSlicer system filament families and
validate the tree against the sanctioned-state ledger.

Policy (companion to assign_vendor_setting_ids.py; see filament_id_plan.md):
  * filament_id is a MATERIAL-FAMILY id: one commercial product line = one id,
    shared by all of that material's per-printer/per-nozzle variants. The key is
    declared only on family ROOT presets (instantiation != "true"); instantiated
    variants inherit it through `inherits` and never write the key themselves.
  * New ids are a pure function of the family's identity:
        filament_id = "OF" + base62_6( uuid5(FILAMENT_ID_NAMESPACE,
                                             "filament_family/<vendor>/<family>") )
    where <vendor> is the vendor bundle name (stem of the vendor index json) and
    <family> is the family name = preset base name (name with /\\s?@.*$/ stripped).
    8 chars total, which satisfies the AMS length limit. Nobody invents ids by
    hand; on the astronomically rare collision with any existing or retired id the
    input is salted ("/1", "/2", ...) until free and the result is frozen in file.
  * Reserved id spaces that are never minted into or altered:
      - GF*                    Bambu AMS/RFID catalog (vendor BBL untouchable)
      - QD_*                   Qidi device protocol
      - P + 7 hex chars (case-insensitive) and the literal "null"
                               user-custom presets (CreatePresetsDialog.cpp)
      - every already-shipped id, grandfathered via scripts/filament_id_snapshot.json
  * scripts/filament_id_snapshot.json is the sanctioned-state ledger: it must
    exactly equal the tree-derived state at all times, so any id/claim change shows
    up as a reviewable diff to that file (the maintainer gate). Ids that fully
    vanish from the tree are appended to scripts/retired_filament_ids.json; a
    retired id may never be used again for anything.

The effective-id resolution below is loader-faithful (PresetBundle.cpp
load_vendor_configs_from_json): own filament_id key, else walk `inherits` within
the vendor map, with OrcaFilamentLibrary base-bundle fallback; once a chain enters
OFL it stays in OFL; a vendor chain that dead-ends id-less retries its direct
parent in the OFL map.

Run from anywhere:  python3 scripts/assign_filament_ids.py
  (default)          mint + insert ids for id-less families; idempotent, never
                     rewrites a valid existing id; a no-op on a fully-idded tree
  --mint "V/Family"  print the id that would be minted; touches nothing
  --update-snapshot  regenerate the snapshot from the tree; retire vanished ids
  --check            run the validation checks (also run by CI through
                     orca_extra_profile_check.py); exit nonzero on errors
"""

import argparse
import json
import os
import re
import sys
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from assign_vendor_setting_ids import ALPHABET, NAMESPACE  # noqa: E402

# Dedicated namespace for filament_id, derived from the setting_id namespace baked
# into both Python and C++ (assign_vendor_setting_ids.NAMESPACE). Never change it.
# FILAMENT_ID_NAMESPACE == UUID("c4d3ff49-4c32-5534-a3e3-00894157ab97")
FILAMENT_ID_NAMESPACE = uuid.uuid5(NAMESPACE, "filament_id")
FILAMENT_ID_LENGTH = 6  # base62 digits after the "OF" prefix -> 8 chars total

SCRIPTS_DIR = os.path.dirname(os.path.abspath(__file__))
PROFILES_DIR = os.path.normpath(os.path.join(SCRIPTS_DIR, "..", "resources", "profiles"))
SNAPSHOT_PATH = os.path.join(SCRIPTS_DIR, "filament_id_snapshot.json")
RETIRED_PATH = os.path.join(SCRIPTS_DIR, "retired_filament_ids.json")

OFL = "OrcaFilamentLibrary"

OF_ID_RE = re.compile(r"^OF[0-9A-Za-z]{6}$")
# User-custom id space minted by CreatePresetsDialog.cpp ("P" + md5(name)[0:7]);
# reserved case-insensitively, together with its "null" sentinel.
USER_CUSTOM_ID_RE = re.compile(r"^P[0-9A-Fa-f]{7}$", re.IGNORECASE)
# Family name = preset base name: strip the first "@..." suffix. The space before
# "@" is optional because names like "Afinia PLA@HS" exist.
BASE_NAME_RE = re.compile(r"\s?@.*$")
# OFL generic presets relevant for alias hygiene (check 5).
OFL_GENERIC_RE = re.compile(r"^Generic .* @System$")
# Salt iterations accepted by the mint-conformance check (check 3).
MAX_CHECK_SALT = 8

UPDATE_HINT = 'run "python scripts/assign_filament_ids.py --update-snapshot" and commit the diff for maintainer review'


# Same output helpers/format as orca_extra_profile_check.py (not imported from
# there to avoid a circular import: that script imports check_filament_ids).
def print_error(msg):
    print(f"\033[91m[ERROR]\033[0m {msg}")  # Red

def print_warning(msg):
    print(f"\033[93m[WARNING]\033[0m {msg}")  # Yellow

def print_info(msg):
    print(f"\033[94m[INFO]\033[0m {msg}")  # Blue

def print_success(msg):
    print(f"\033[92m[SUCCESS]\033[0m {msg}")  # Green


def _utf8_console():
    """Make stdout/stderr survive non-ASCII profile names on cp1252 consoles."""
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            try:
                stream.reconfigure(encoding="utf-8", errors="replace")
            except (ValueError, OSError):
                pass


# ---------------------------------------------------------------------------
# Minting
# ---------------------------------------------------------------------------

def base_name(name):
    """Family name of a preset: name with the first "@..." suffix stripped."""
    return BASE_NAME_RE.sub("", name, count=1)


def generate_filament_id(vendor, family, salt=0):
    """Deterministic "OF" + 6-char base62 filament_id for a material family.

    input = "filament_family/<vendor>/<family>" (+ "/<salt>" when salted);
    u = uuid5(FILAMENT_ID_NAMESPACE, input); the id tail is the low
    FILAMENT_ID_LENGTH base62 digits of int(u.bytes, "big"), most-significant
    first — the same derivation as generate_preset_setting_id.
    """
    key = f"filament_family/{vendor}/{family}"
    if salt:
        key = f"{key}/{salt}"
    u = uuid.uuid5(FILAMENT_ID_NAMESPACE, key)
    n = int.from_bytes(u.bytes, "big")
    digits = []
    for _ in range(FILAMENT_ID_LENGTH):
        digits.append(ALPHABET[n % 62])
        n //= 62
    return "OF" + "".join(reversed(digits))


def mint_filament_id(vendor, family, taken):
    """Mint the family's id, salting past any id in `taken` (existing + retired)."""
    for salt in range(10000):
        candidate = generate_filament_id(vendor, family, salt)
        if candidate not in taken:
            return candidate
    raise RuntimeError(f"could not mint a free filament_id for {vendor}/{family}")


# ---------------------------------------------------------------------------
# Tree loading + loader-faithful effective-id resolution
# ---------------------------------------------------------------------------

def load_json(path):
    with open(path, "r", encoding="utf-8-sig") as f:
        return json.load(f)


def list_vendor_names(profiles_dir):
    """Vendor bundles = subdirectories with a matching <name>.json index file.

    (Ignores stray non-bundle entries such as the tracked "user" directory,
    which has no user.json index.)
    """
    profiles_dir = str(profiles_dir)
    return sorted(
        os.path.splitext(f)[0] for f in os.listdir(profiles_dir)
        if f.endswith(".json")
        and os.path.isdir(os.path.join(profiles_dir, os.path.splitext(f)[0]))
    )


def load_vendor_filaments(profiles_dir, vendor):
    """Load a vendor's filament presets from its index's filament_list.

    Returns (presets dict name -> record, list of unreadable-file messages).
    """
    profiles_dir = str(profiles_dir)
    presets = {}
    errors = []
    try:
        idx = load_json(os.path.join(profiles_dir, vendor + ".json"))
    except (OSError, ValueError) as e:
        return presets, [f"unreadable vendor index {vendor}.json: {e}"]
    for entry in idx.get("filament_list", []):
        rel = f"{vendor}/{entry.get('sub_path', '')}"
        path = os.path.join(profiles_dir, vendor, entry.get("sub_path", ""))
        try:
            data = load_json(path)
        except (OSError, ValueError) as e:
            errors.append(f"unreadable filament profile {rel}: {e}")
            continue
        name = data.get("name", entry.get("name"))
        presets[name] = {
            "name": name,
            "file": rel,
            "path": path,
            "filament_id": data.get("filament_id"),
            "inherits": data.get("inherits"),
            "instantiation": str(data.get("instantiation", "")).lower() == "true",
            "compatible_printers": data.get("compatible_printers") or [],
        }
    return presets, errors


def resolve_filament_id(name, filaments, ofl_filaments, seen=None, in_ofl=False, skip_own=False):
    """Walk the inherits chain for the effective filament_id, loader-faithfully.

    Mirrors PresetBundle.cpp load_vendor_configs_from_json: a hop resolves in the
    vendor's own map first, then falls back to the OFL base-bundle map. OFL's map
    was memoized entirely within OFL, so once a chain enters OFL it stays in OFL
    (a vendor file sharing an OFL preset's name must not shadow OFL-internal
    hops). Additionally, a vendor preset that never resolves an id inside the
    vendor is re-tried against the OFL map keyed by its direct parent name.

    skip_own ignores the first preset's own filament_id key (used to compute the
    id its inherits chain would resolve WITHOUT the declaration — check 7b drift).

    Returns (filament_id or None, source, ofl_entry) where source is one of
    "own"/"inherited"/"missing"/"dangling"/"cycle" and ofl_entry is the name of
    the OFL preset through which a vendor chain entered OFL (None when the id was
    declared vendor-side or resolution started inside OFL).
    """
    if seen is None:
        seen = set()
    if name in seen:
        return None, "cycle", None
    seen.add(name)
    entry = None
    if in_ofl:
        rec = ofl_filaments.get(name)
    else:
        rec = filaments.get(name)
        if rec is None and name in ofl_filaments:
            rec, in_ofl, entry = ofl_filaments[name], True, name
    if rec is None:
        return None, "dangling", None
    if rec.get("filament_id") and not skip_own:
        return rec["filament_id"], "own" if len(seen) == 1 else "inherited", entry
    parent = rec.get("inherits")
    if parent:
        fid, src, sub_entry = resolve_filament_id(parent, filaments, ofl_filaments, seen, in_ofl)
        if fid or in_ofl:
            return fid, src, entry if entry is not None else sub_entry
        # Vendor chain dead-ended id-less: the loader would have consulted the
        # OFL map at each vendor hop's inherits; retry this hop's parent in OFL.
        if parent in ofl_filaments:
            fid, src, _ = resolve_filament_id(parent, filaments, ofl_filaments, set(), True)
            return fid, src, parent
        return fid, src, sub_entry
    return None, "missing", entry


def analyze_tree(profiles_dir):
    """Load every vendor bundle and derive the full filament_id state.

    Returns a dict with the tree-derived snapshot sections plus the working data
    the checks and the assign pass need. All claims are "Vendor/Family" strings
    over INSTANTIATED system filaments, tree-wide including OFL and BBL.
    """
    profiles_dir = str(profiles_dir)
    vendor_names = list_vendor_names(profiles_dir)
    ofl_filaments, ofl_errors = (
        load_vendor_filaments(profiles_dir, OFL) if OFL in vendor_names else ({}, [])
    )

    vendors = {}
    read_errors = list(ofl_errors)
    for vendor in vendor_names:
        if vendor == OFL:
            filaments = ofl_filaments
        else:
            filaments, errs = load_vendor_filaments(profiles_dir, vendor)
            read_errors.extend(errs)
        for rec in filaments.values():
            eff, src, ofl_entry = resolve_filament_id(rec["name"], filaments, ofl_filaments)
            rec["eff_filament_id"] = eff
            rec["id_source"] = src
            rec["ofl_entry"] = ofl_entry
        vendors[vendor] = filaments

    # id -> set of "Vendor/Family" claims over instantiated presets. Every id
    # occurring in the tree is a key; ids only ever DECLARED (e.g. on a root
    # whose children all override them) keep an empty claim list, so that the
    # snapshot exactly equals the tree-derived state and the format/retirement
    # checks can grandfather them.
    ids = {}
    vendor_ids = {}             # vendor -> set of ids occurring there (declared or effective)
    declared_ids = {}           # vendor -> set of ids DECLARED in that vendor's own files
    instantiated_with_id = []   # "Vendor/PresetName" (own filament_id key on an instantiated preset)
    overrides = []              # (vendor, name, declared, inherited, file)
    missing_effective = []      # (vendor, name, file) instantiated presets resolving no id
    alias_candidates = []       # (vendor, rec, ofl_entry) presets id-resolved through an OFL generic

    for vendor, filaments in vendors.items():
        occurring = vendor_ids.setdefault(vendor, set())
        for rec in filaments.values():
            if rec.get("filament_id"):
                occurring.add(rec["filament_id"])
                declared_ids.setdefault(vendor, set()).add(rec["filament_id"])
                ids.setdefault(rec["filament_id"], set())
                if rec.get("inherits"):
                    inherited, _src, _e = resolve_filament_id(
                        rec["name"], filaments, ofl_filaments, skip_own=True)
                    if inherited and inherited != rec["filament_id"]:
                        overrides.append(
                            (vendor, rec["name"], rec["filament_id"], inherited, rec["file"]))
            if not rec["instantiation"]:
                continue
            eff = rec.get("eff_filament_id")
            if not eff:
                missing_effective.append((vendor, rec["name"], rec["file"]))
                continue
            occurring.add(eff)
            ids.setdefault(eff, set()).add(f"{vendor}/{base_name(rec['name'])}")
            if rec.get("filament_id"):
                instantiated_with_id.append(f"{vendor}/{rec['name']}")
            if vendor != OFL and rec["ofl_entry"] and OFL_GENERIC_RE.match(rec["ofl_entry"]):
                alias_candidates.append((vendor, rec, rec["ofl_entry"]))

    # Alias hygiene (check 5): a vendor preset that tunes an OFL generic (no id
    # anywhere in its vendor-side chain) is matched to the OFL preset by ALIAS
    # (base name); renaming it re-exposes the OFL generic and creates a live
    # duplicate, and empty compatible_printers cannot claim any printer.
    alias_violations = []       # (vendor, name, ofl_entry, reason, file)
    for vendor, rec, entry in alias_candidates:
        expected = base_name(entry)
        own_base = base_name(rec["name"])
        if own_base != expected:
            alias_violations.append((
                vendor, rec["name"], entry,
                f'base name "{own_base}" != "{expected}" — the rename re-exposes the '
                f"OFL preset on its printers (alias shadowing is name-based)",
                rec["file"]))
        elif not rec["compatible_printers"]:
            alias_violations.append((
                vendor, rec["name"], entry,
                "empty compatible_printers cannot shadow the OFL preset anywhere",
                rec["file"]))

    return {
        "vendors": vendors,
        "read_errors": read_errors,
        "ids": {fid: sorted(claims) for fid, claims in ids.items()},
        "vendor_ids": vendor_ids,
        "declared_ids": declared_ids,
        "instantiated_with_id": sorted(instantiated_with_id),
        "overrides": overrides,
        "id_overrides": sorted(f"{v}/{n}" for v, n, _d, _i, _f in overrides),
        "missing_effective": sorted(missing_effective),
        "alias_violations": alias_violations,
        "alias_exceptions": sorted(f"{v}/{n}" for v, n, _e, _r, _f in alias_violations),
    }


# ---------------------------------------------------------------------------
# Snapshot / retired-ledger IO
# ---------------------------------------------------------------------------

def snapshot_from_analysis(analysis):
    return {
        "ids": {fid: sorted(claims) for fid, claims in analysis["ids"].items()},
        "instantiated_with_id": analysis["instantiated_with_id"],
        "id_overrides": analysis["id_overrides"],
        "alias_exceptions": analysis["alias_exceptions"],
    }


def load_snapshot(path):
    """Return the snapshot dict, or None when the file does not exist."""
    if not os.path.exists(path):
        return None
    data = load_json(path)
    for key in ("ids", "instantiated_with_id", "id_overrides", "alias_exceptions"):
        data.setdefault(key, {} if key == "ids" else [])
    return data


def load_retired(path):
    """Return the retired-ids map {id: ["Vendor/Family", ...]} ({} if absent)."""
    if not os.path.exists(path):
        return {}
    return load_json(path).get("retired", {})


def write_ledger(path, obj):
    """Deterministic serialization: sorted keys, indent 1, LF, trailing newline."""
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(obj, f, indent=1, ensure_ascii=False, sort_keys=True)
        f.write("\n")


# ---------------------------------------------------------------------------
# Reserved namespaces
# ---------------------------------------------------------------------------

def reserved_space_owner(fid):
    """(is_reserved, owner_vendor or None) for the frozen id spaces."""
    if fid.startswith("GF"):
        return True, "BBL"
    if fid.startswith("QD_"):
        return True, "Qidi"
    if USER_CUSTOM_ID_RE.match(fid) or fid == "null":
        return True, None  # user-custom space: no system vendor may own it
    return False, None


# ---------------------------------------------------------------------------
# Checks (imported and called tree-wide by orca_extra_profile_check.py)
# ---------------------------------------------------------------------------

def check_filament_ids(profiles_dir=PROFILES_DIR, snapshot_path=SNAPSHOT_PATH,
                       retired_path=RETIRED_PATH):
    """Validate filament_id state across every vendor. Returns the error count.

    1. Format: every id occurring in the tree (declared or effective) must be in
       the snapshot, or match ^OF[0-9A-Za-z]{6}$, or belong to vendor BBL, or be
       a QD_* id within vendor Qidi.
    2. Snapshot equality, both directions: the tree-derived id->families multimap
       must equal the snapshot exactly (the snapshot diff is the maintainer gate).
    3. Mint conformance: an OF-format id's claim that is not grandfathered must
       equal the mint of (vendor, family) or a salted iteration.
    4. Retired ids may never occur again.
    5. Alias hygiene: a tuned OFL generic must keep the generic's base name and
       claim printers via non-empty compatible_printers.
    6. Reserved namespaces (GF*/QD_*/P-hex/"null") only for their owner vendors,
       except claims grandfathered in the snapshot.
    7. Structure ratchet: (a) no NEW instantiated preset carries its own
       filament_id key; (b) no NEW declared-vs-inherited id drift; (c) every
       instantiated filament resolves an effective id (a hard load error in C++).
    """
    _utf8_console()
    errors = 0
    analysis = analyze_tree(profiles_dir)
    snapshot = load_snapshot(snapshot_path)
    if snapshot is None:
        print_error(f"filament_id snapshot not found at {snapshot_path}; {UPDATE_HINT}")
        return 1
    retired = load_retired(retired_path)

    for msg in analysis["read_errors"]:
        print_error(msg)
        errors += 1

    snap_ids = snapshot["ids"]
    tree_ids = analysis["ids"]

    # -- 1. format ----------------------------------------------------------
    for vendor in sorted(analysis["vendor_ids"]):
        for fid in sorted(analysis["vendor_ids"][vendor]):
            if fid in snap_ids or OF_ID_RE.match(fid):
                continue
            if vendor == "BBL":
                continue
            if vendor == "Qidi" and fid.startswith("QD_"):
                continue
            print_error(
                f'filament_id "{fid}" ({vendor}) is neither grandfathered in the '
                f'snapshot nor a minted "OF" id; new family ids must come from '
                f'"python scripts/assign_filament_ids.py" (see --mint)')
            errors += 1

    # -- 2. snapshot equality (both directions) -----------------------------
    for fid in sorted(tree_ids):
        if fid not in snap_ids:
            print_error(
                f'filament_id "{fid}" is not sanctioned by '
                f"scripts/filament_id_snapshot.json; {UPDATE_HINT}")
            errors += 1
            continue
        for claim in tree_ids[fid]:
            if claim not in snap_ids[fid]:
                print_error(
                    f'filament_id "{fid}" claim "{claim}" is not sanctioned by '
                    f"scripts/filament_id_snapshot.json; {UPDATE_HINT}")
                errors += 1
    for fid in sorted(snap_ids):
        if fid not in tree_ids:
            print_error(
                f'filament_id stability: snapshot id "{fid}" vanished from the tree '
                f"(ids are immutable once shipped); {UPDATE_HINT}")
            errors += 1
            continue
        for claim in snap_ids[fid]:
            if claim not in tree_ids[fid]:
                print_error(
                    f'filament_id stability: snapshot claim "{claim}" of id "{fid}" '
                    f"vanished from the tree (ids are immutable once shipped); {UPDATE_HINT}")
                errors += 1

    # -- 3. mint conformance for OF-format ids ------------------------------
    for fid in sorted(tree_ids):
        if not OF_ID_RE.match(fid):
            continue
        for claim in tree_ids[fid]:
            if claim in snap_ids.get(fid, []):
                continue  # grandfathered
            vendor, family = claim.split("/", 1)
            if fid not in analysis["declared_ids"].get(vendor, set()):
                # The claiming vendor never declares this id — it is inherited
                # from another vendor's root (an OFL generic family): correctly
                # tuned generics ride the OFL family id by design, so only the
                # snapshot gate (check 2) applies to this claim.
                continue
            minted = [generate_filament_id(vendor, family, s) for s in range(MAX_CHECK_SALT + 1)]
            if fid not in minted:
                print_error(
                    f'filament_id "{fid}" of family "{claim}" does not match its mint: '
                    f'expected "{minted[0]}" (or a salted iteration); paste the expected '
                    f"id into the family root (or, for an intentionally shared id, "
                    f"{UPDATE_HINT})")
                errors += 1

    # -- 4. retired ids may never come back ---------------------------------
    for vendor in sorted(analysis["vendor_ids"]):
        for fid in sorted(analysis["vendor_ids"][vendor]):
            if fid in retired:
                print_error(
                    f'filament_id "{fid}" ({vendor}) is retired '
                    f"(scripts/retired_filament_ids.json) and may never be reused")
                errors += 1

    # -- 5. alias hygiene for tuned OFL generics -----------------------------
    exceptions = set(snapshot["alias_exceptions"])
    for vendor, name, entry, reason, file in analysis["alias_violations"]:
        if f"{vendor}/{name}" in exceptions:
            continue
        print_error(
            f'preset "{name}" ({file}) tunes the OFL generic "{entry}" but {reason}; '
            f'keep the OFL base name and non-empty compatible_printers, or give the '
            f"family its own minted id")
        errors += 1

    # -- 6. reserved namespaces ----------------------------------------------
    for fid in sorted(tree_ids):
        is_reserved, owner = reserved_space_owner(fid)
        if not is_reserved:
            continue
        for claim in tree_ids[fid]:
            vendor = claim.split("/", 1)[0]
            if vendor == owner:
                continue
            if claim in snap_ids.get(fid, []):
                continue  # grandfathered
            space = f"owned by {owner}" if owner else "reserved for user-custom presets"
            print_error(
                f'filament_id "{fid}" of "{claim}" is in a reserved id space '
                f"({space}) and must not be claimed by system presets of other vendors")
            errors += 1

    # -- 7. structure ratchet -------------------------------------------------
    grandfathered = set(snapshot["instantiated_with_id"])
    for key in analysis["instantiated_with_id"]:
        if key not in grandfathered:
            print_error(
                f'instantiated preset "{key}" declares its own filament_id key; the key '
                f"belongs on the family root preset only (variants inherit it)")
            errors += 1
    grandfathered = set(snapshot["id_overrides"])
    for vendor, name, declared, inherited, file in analysis["overrides"]:
        if f"{vendor}/{name}" in grandfathered:
            continue
        print_error(
            f'preset "{name}" ({file}) declares filament_id "{declared}" but its '
            f'inherits chain resolves "{inherited}"; a preset must not override its '
            f"family's id")
        errors += 1
    for vendor, name, file in analysis["missing_effective"]:
        expected = generate_filament_id(vendor, base_name(name))
        print_error(
            f'instantiated filament "{name}" ({file}) resolves no filament_id anywhere '
            f"in its inherits chain — this is a hard load error in the C++ loader; "
            f'run "python scripts/assign_filament_ids.py" (expected id for family '
            f'"{vendor}/{base_name(name)}": "{expected}", salted if taken)')
        errors += 1

    return errors


# ---------------------------------------------------------------------------
# --update-snapshot
# ---------------------------------------------------------------------------

def update_snapshot(profiles_dir=PROFILES_DIR, snapshot_path=SNAPSHOT_PATH,
                    retired_path=RETIRED_PATH, allow_shared_catalog=False):
    """Regenerate the snapshot from the tree; retire ids that fully vanished.

    Refuses to sanction NEW reserved-namespace ids (or new claims on them) for
    non-owner vendors unless --allow-shared-catalog is passed. Idempotent: a
    second run over an unchanged tree changes nothing. Returns 0 on success.
    """
    analysis = analyze_tree(profiles_dir)
    for msg in analysis["read_errors"]:
        print_error(msg)
    new_snap = snapshot_from_analysis(analysis)
    old_snap = load_snapshot(snapshot_path)
    old_ids = old_snap["ids"] if old_snap else {}

    # Gate: new reserved-namespace ids / claims for non-owner vendors.
    refusals = []
    for fid, claims in sorted(new_snap["ids"].items()):
        is_reserved, owner = reserved_space_owner(fid)
        if not is_reserved:
            continue
        for claim in claims:
            if claim in old_ids.get(fid, []):
                continue
            vendor = claim.split("/", 1)[0]
            if vendor == owner:
                continue
            refusals.append((fid, claim, owner))
        if not claims and fid not in old_ids:
            # Declared-only new id: attribute it to its declaring vendor(s).
            for vendor in sorted(analysis["vendor_ids"]):
                if fid in analysis["vendor_ids"][vendor] and vendor != owner:
                    refusals.append((fid, f"{vendor}/(declared only)", owner))
    if refusals and not allow_shared_catalog:
        for fid, claim, owner in refusals:
            space = f"owned by {owner}" if owner else "reserved for user-custom presets"
            print_error(
                f'refusing to sanction new claim "{claim}" on reserved-namespace id '
                f'"{fid}" ({space}); pass --allow-shared-catalog only for '
                f"maintainer-approved shared-catalog families")
        return 1

    # Retirement is permanent: refuse to sanction a tree that resurrects a
    # retired id (check 4 would reject the resulting snapshot forever anyway).
    retired = load_retired(retired_path)
    reused = sorted(set(new_snap["ids"]) & set(retired))
    if reused:
        for fid in reused:
            print_error(
                f'refusing to sanction retired filament_id "{fid}" '
                f"(scripts/retired_filament_ids.json is append-only; retired ids may "
                f"never be reused — mint a fresh id for the family instead)")
        return 1

    # Retire ids that fully vanished from the tree (append-only ledger).
    newly_retired = []
    for fid in sorted(old_ids):
        if fid not in new_snap["ids"]:
            retired[fid] = sorted(set(retired.get(fid, [])) | set(old_ids[fid]))
            newly_retired.append(fid)

    # Diff summary.
    added_ids = sorted(set(new_snap["ids"]) - set(old_ids))
    removed_ids = sorted(set(old_ids) - set(new_snap["ids"]))
    added_claims = sum(
        len(set(claims) - set(old_ids.get(fid, [])))
        for fid, claims in new_snap["ids"].items())
    removed_claims = sum(
        len(set(claims) - set(new_snap["ids"].get(fid, [])))
        for fid, claims in old_ids.items())
    old_snap = old_snap or {"ids": {}, "instantiated_with_id": [], "id_overrides": [],
                            "alias_exceptions": []}
    changed = new_snap != old_snap

    if changed:
        write_ledger(snapshot_path, new_snap)
    if newly_retired or not os.path.exists(retired_path):
        write_ledger(retired_path, {"retired": retired})

    print_info(f"snapshot ids      : {len(new_snap['ids'])} (+{len(added_ids)} / -{len(removed_ids)})")
    print_info(f"claims added      : {added_claims}")
    print_info(f"claims removed    : {removed_claims}")
    print_info(f"ids retired now   : {len(newly_retired)}" +
               (f" ({', '.join(newly_retired)})" if newly_retired else ""))
    for section in ("instantiated_with_id", "id_overrides", "alias_exceptions"):
        before, after = len(old_snap.get(section, [])), len(new_snap[section])
        print_info(f"{section:<18}: {after} ({after - before:+d})")
    if changed:
        print_success(f"snapshot written to {snapshot_path}")
    else:
        print_success("snapshot already up to date; nothing changed")
    return 0


# ---------------------------------------------------------------------------
# Default run: mint + insert ids for id-less families
# ---------------------------------------------------------------------------

def insert_filament_id(text, new_id):
    """Insert a `"filament_id"` line into a preset that lacks one.

    Placed just before `instantiation` (or, failing that, after the `name` line)
    so it matches the canonical key order, reusing that anchor line's indentation
    and line ending. Same byte-preserving approach as assign_vendor_setting_ids.
    """
    m = re.search(r'^([ \t]*)"instantiation"[ \t]*:.*?(\r?\n)', text, re.MULTILINE)
    if m:
        line = f'{m.group(1)}"filament_id": {json.dumps(new_id, ensure_ascii=False)},{m.group(2)}'
        return text[:m.start()] + line + text[m.start():], 1
    m = re.search(r'^([ \t]*)"name"[ \t]*:.*?(\r?\n)', text, re.MULTILINE)
    if m:
        line = f'{m.group(1)}"filament_id": {json.dumps(new_id, ensure_ascii=False)},{m.group(2)}'
        return text[:m.end()] + line + text[m.end():], 1
    return text, 0


def write_filament_id(path, new_id):
    """Insert new_id into the profile at path, byte-preserving everything else.

    Binary IO keeps the file's original line endings (LF or CRLF) and exact
    formatting apart from the inserted line; the result is re-parsed to
    guarantee it is still valid JSON.
    """
    with open(path, "rb") as f:
        raw = f.read()
    bom = raw.startswith(b"\xef\xbb\xbf")
    text = raw.decode("utf-8-sig")
    text, n = insert_filament_id(text, new_id)
    if n == 0:
        raise RuntimeError(f"Could not insert filament_id into {path}")
    json.loads(text)  # fail loudly if the edit broke the JSON
    with open(path, "wb") as f:
        f.write((b"\xef\xbb\xbf" if bom else b"") + text.encode("utf-8"))


def assign_missing_ids(profiles_dir=PROFILES_DIR, snapshot_path=SNAPSHOT_PATH,
                       retired_path=RETIRED_PATH):
    """Mint + insert ids for id-less families. Never rewrites a valid existing id.

    A family = (vendor, base name) group over instantiated filaments with no
    effective id. The minted id is inserted into the family's root(s): the
    presets its members inherit that carry no id, or the member itself when it
    has no vendor-side parent. Returns (files_changed, errors).
    """
    analysis = analyze_tree(profiles_dir)
    errors = 0
    for msg in analysis["read_errors"]:
        print_error(msg)
        errors += 1

    # Group id-less instantiated presets into families.
    families = {}  # (vendor, family) -> [rec]
    for vendor, name, _file in analysis["missing_effective"]:
        rec = analysis["vendors"][vendor][name]
        if rec["id_source"] in ("cycle", "dangling"):
            print_error(f'cannot mint for "{vendor}/{name}": broken inherits chain '
                        f'({rec["id_source"]})')
            errors += 1
            continue
        families.setdefault((vendor, base_name(name)), []).append(rec)

    if not families:
        print_success("every instantiated filament already resolves a filament_id; "
                      "nothing to do (0 files changed)")
        return 0, errors

    # Ids already spoken for: whole tree (declared or effective) + ledgers.
    snapshot = load_snapshot(snapshot_path) or {"ids": {}}
    taken = set(snapshot["ids"]) | set(load_retired(retired_path))
    for occurring in analysis["vendor_ids"].values():
        taken |= occurring

    # Family root(s): the direct vendor-side parents of the members (id-less by
    # construction), or the member itself when it has none.
    roots = {}  # (vendor, family) -> {preset name: rec}
    root_claims = {}  # (vendor, root name) -> set of families wanting to write it
    for key, members in sorted(families.items()):
        vendor = key[0]
        vendor_map = analysis["vendors"][vendor]
        family_roots = {}
        for rec in members:
            parent = rec.get("inherits")
            root = vendor_map.get(parent) if parent else None
            if root is None or root.get("filament_id"):
                root = rec  # root-less member carries the id itself
            family_roots[root["name"]] = root
            root_claims.setdefault((vendor, root["name"]), set()).add(key)
        roots[key] = family_roots

    files_changed = 0
    families_minted = 0
    for key, family_roots in sorted(roots.items()):
        vendor, family = key
        shared = [n for n in family_roots
                  if len(root_claims[(vendor, n)]) > 1]
        if shared:
            others = sorted({f"{v}/{f}" for n in shared
                             for (v, f) in root_claims[(vendor, n)] if (v, f) != key})
            print_error(
                f'cannot mint for family "{vendor}/{family}": root(s) '
                f"{sorted(shared)} are shared with famil(ies) {others}; split the "
                f"roots so each family has its own")
            errors += 1
            continue
        new_id = mint_filament_id(vendor, family, taken)
        taken.add(new_id)
        families_minted += 1
        for name in sorted(family_roots):
            root = family_roots[name]
            write_filament_id(root["path"], new_id)
            files_changed += 1
            print_info(f'family "{vendor}/{family}": filament_id "{new_id}" -> {root["file"]}')

    print_info(f"families minted  : {families_minted}")
    print_info(f"files changed    : {files_changed}")
    if files_changed:
        print_warning(f"now {UPDATE_HINT}")
    return files_changed, errors


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv=None):
    _utf8_console()
    parser = argparse.ArgumentParser(
        description="Mint deterministic filament_id values for id-less filament "
                    "families and validate the tree against the sanctioned snapshot.")
    parser.add_argument("--mint", metavar='"Vendor/Family"',
                        help="print the id that would be minted for a family; "
                             "touches nothing")
    parser.add_argument("--update-snapshot", action="store_true",
                        help="regenerate scripts/filament_id_snapshot.json from the "
                             "tree; ids that fully vanished are retired")
    parser.add_argument("--check", action="store_true",
                        help="run the filament_id checks; exit nonzero on errors")
    parser.add_argument("--allow-shared-catalog", action="store_true",
                        help="with --update-snapshot: allow sanctioning new claims "
                             "on reserved-namespace ids for non-owner vendors")
    parser.add_argument("--profiles", default=PROFILES_DIR,
                        help="profiles directory (default: resources/profiles)")
    args = parser.parse_args(argv)

    if args.mint:
        if "/" not in args.mint:
            parser.error('--mint expects "Vendor/Family"')
        vendor, family = args.mint.split("/", 1)
        snapshot = load_snapshot(SNAPSHOT_PATH) or {"ids": {}}
        taken = set(snapshot["ids"]) | set(load_retired(RETIRED_PATH))
        print(mint_filament_id(vendor, family, taken))
        return 0

    if args.update_snapshot:
        return update_snapshot(args.profiles, SNAPSHOT_PATH, RETIRED_PATH,
                               allow_shared_catalog=args.allow_shared_catalog)

    if args.check:
        errors = check_filament_ids(args.profiles, SNAPSHOT_PATH, RETIRED_PATH)
        if errors:
            print_error(f"filament_id check: {errors} error(s)")
            return 1
        print_success("filament_id check: no errors")
        return 0

    _changed, errors = assign_missing_ids(args.profiles, SNAPSHOT_PATH, RETIRED_PATH)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
