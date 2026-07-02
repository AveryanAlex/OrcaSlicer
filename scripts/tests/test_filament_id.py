#!/usr/bin/env python3
"""Tests for scripts/assign_filament_ids.py (stdlib unittest, no external deps).

Run from the repo root:  python -m unittest discover -s scripts/tests -v
"""

import contextlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest
import uuid

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

import assign_filament_ids as afi  # noqa: E402

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
REAL_PROFILES = os.path.join(REPO_ROOT, "resources", "profiles")

OFL = "OrcaFilamentLibrary"


def load_json_file(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


# ---------------------------------------------------------------------------
# helpers: synthetic profile trees
# ---------------------------------------------------------------------------

def preset(name, filament_id=None, inherits=None, instantiation=True,
           compatible_printers=None):
    data = {"type": "filament", "name": name}
    if inherits is not None:
        data["inherits"] = inherits
    if filament_id is not None:
        data["filament_id"] = filament_id
    data["instantiation"] = "true" if instantiation else "false"
    if compatible_printers is not None:
        data["compatible_printers"] = compatible_printers
    return data


class SyntheticTree:
    """A throwaway resources/profiles-shaped directory plus ledger paths."""

    def __init__(self):
        self.dir = tempfile.mkdtemp(prefix="filament_id_test_")
        self.profiles = os.path.join(self.dir, "profiles")
        os.makedirs(self.profiles)
        self.snapshot = os.path.join(self.dir, "filament_id_snapshot.json")
        self.retired = os.path.join(self.dir, "retired_filament_ids.json")

    def cleanup(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def preset_path(self, vendor, name):
        return os.path.join(self.profiles, vendor, "filament", name + ".json")

    def add_vendor(self, vendor, presets):
        vendor_dir = os.path.join(self.profiles, vendor, "filament")
        os.makedirs(vendor_dir, exist_ok=True)
        index = {"name": vendor, "version": "01.00.00.00", "filament_list": []}
        for data in presets:
            fname = data["name"] + ".json"
            with open(os.path.join(vendor_dir, fname), "w", encoding="utf-8") as f:
                json.dump(data, f, indent=4, ensure_ascii=False)
            index["filament_list"].append(
                {"name": data["name"], "sub_path": f"filament/{fname}"})
        with open(os.path.join(self.profiles, vendor + ".json"), "w",
                  encoding="utf-8") as f:
            json.dump(index, f, indent=4, ensure_ascii=False)

    def add_to_index(self, vendor, name):
        idx_path = os.path.join(self.profiles, vendor + ".json")
        with open(idx_path, encoding="utf-8") as f:
            index = json.load(f)
        index["filament_list"].append(
            {"name": name, "sub_path": f"filament/{name}.json"})
        with open(idx_path, "w", encoding="utf-8") as f:
            json.dump(index, f, indent=4, ensure_ascii=False)

    def write_preset(self, vendor, data, register=True):
        path = self.preset_path(vendor, data["name"])
        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=4, ensure_ascii=False)
        if register:
            self.add_to_index(vendor, data["name"])

    def remove_preset(self, vendor, name):
        os.remove(self.preset_path(vendor, name))
        idx_path = os.path.join(self.profiles, vendor + ".json")
        with open(idx_path, encoding="utf-8") as f:
            index = json.load(f)
        index["filament_list"] = [
            e for e in index["filament_list"] if e["name"] != name]
        with open(idx_path, "w", encoding="utf-8") as f:
            json.dump(index, f, indent=4, ensure_ascii=False)

    # -- pipeline wrappers ---------------------------------------------------

    def update_snapshot(self, allow_shared_catalog=False):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = afi.update_snapshot(self.profiles, self.snapshot, self.retired,
                                     allow_shared_catalog=allow_shared_catalog)
        return rc, buf.getvalue()

    def check(self):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            errors = afi.check_filament_ids(self.profiles, self.snapshot, self.retired)
        return errors, buf.getvalue()

    def assign(self):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            changed, errors = afi.assign_missing_ids(self.profiles, self.snapshot,
                                                     self.retired)
        return changed, errors, buf.getvalue()


def make_clean_tree():
    """Baseline tree: OFL base+generic, a vendor family, a clean tuned generic."""
    t = SyntheticTree()
    t.add_vendor(OFL, [
        preset("fdm_pla", filament_id="OGFL99", instantiation=False),
        preset("Generic PLA @System", inherits="fdm_pla",
               compatible_printers=[]),
    ])
    t.add_vendor("VendorA", [
        preset("APLA @base", filament_id="AX01", instantiation=False),
        preset("APLA @P1", inherits="APLA @base",
               compatible_printers=["P1 0.4 nozzle"]),
        # Correctly tuned OFL generic: keeps the OFL base name, claims a printer.
        preset("Generic PLA @P1", inherits="Generic PLA @System",
               compatible_printers=["P1 0.4 nozzle"]),
    ])
    rc, _out = t.update_snapshot()
    assert rc == 0
    return t


class SyntheticTreeCase(unittest.TestCase):
    def setUp(self):
        self.t = make_clean_tree()
        self.addCleanup(self.t.cleanup)


# ---------------------------------------------------------------------------
# mint
# ---------------------------------------------------------------------------

class TestMint(unittest.TestCase):
    def test_namespace_literal(self):
        # Frozen: derived from the setting_id namespace; baked into the ledger.
        self.assertEqual(afi.FILAMENT_ID_NAMESPACE,
                         uuid.UUID("c4d3ff49-4c32-5534-a3e3-00894157ab97"))

    def test_known_vector(self):
        # Hardcoded, independently computed vector: freezes prefix, input string
        # layout ("filament_family/<vendor>/<family>") and base62 derivation.
        self.assertEqual(afi.generate_filament_id("Qidi", "HATCHBOX PLA"), "OFiJYDPC")

    def test_determinism_and_format(self):
        for vendor, family in [("Qidi", "HATCHBOX PLA"), ("Creality", "CR PLA"),
                               ("OrcaFilamentLibrary", "Generic ABS"),
                               ("BBL", "拓竹 PLA")]:  # unicode family
            a = afi.generate_filament_id(vendor, family)
            b = afi.generate_filament_id(vendor, family)
            self.assertEqual(a, b)
            self.assertRegex(a, r"^OF[0-9A-Za-z]{6}$")
            self.assertEqual(len(a), 8)

    def test_salt_changes_id(self):
        base = afi.generate_filament_id("Qidi", "HATCHBOX PLA")
        salted = afi.generate_filament_id("Qidi", "HATCHBOX PLA", salt=1)
        self.assertNotEqual(base, salted)
        self.assertRegex(salted, r"^OF[0-9A-Za-z]{6}$")

    def test_mint_salt_iteration(self):
        v, fam = "Qidi", "HATCHBOX PLA"
        taken = {afi.generate_filament_id(v, fam, s) for s in range(2)}
        self.assertEqual(afi.mint_filament_id(v, fam, taken),
                         afi.generate_filament_id(v, fam, salt=2))
        self.assertEqual(afi.mint_filament_id(v, fam, set()),
                         afi.generate_filament_id(v, fam))


class TestBaseName(unittest.TestCase):
    def test_family_derivation(self):
        cases = [
            ("X @base", "X"),
            ("Afinia PLA@HS", "Afinia PLA"),
            ("PolyTerra PLA", "PolyTerra PLA"),
            ("HATCHBOX PLA @Qidi X-Plus 4 0.6 nozzle", "HATCHBOX PLA"),
            ("A @B @C", "A"),                      # first @ wins
            ("Filár PLA 拓竹 @0.4 nozzle", "Filár PLA 拓竹"),
        ]
        for name, family in cases:
            self.assertEqual(afi.base_name(name), family, msg=name)


# ---------------------------------------------------------------------------
# resolver (loader-faithful semantics)
# ---------------------------------------------------------------------------

class TestResolver(unittest.TestCase):
    @staticmethod
    def rec(name, filament_id=None, inherits=None):
        return {"name": name, "filament_id": filament_id, "inherits": inherits}

    def resolve(self, name, vendor_recs, ofl_recs, **kw):
        fmap = {r["name"]: r for r in vendor_recs}
        omap = {r["name"]: r for r in ofl_recs}
        return afi.resolve_filament_id(name, fmap, omap, **kw)

    def test_own_id(self):
        fid, src, entry = self.resolve("A", [self.rec("A", "ID1")], [])
        self.assertEqual((fid, src, entry), ("ID1", "own", None))

    def test_inherited_within_vendor(self):
        fid, src, entry = self.resolve(
            "A", [self.rec("A", inherits="B"), self.rec("B", inherits="C"),
                  self.rec("C", "ID3")], [])
        self.assertEqual((fid, src, entry), ("ID3", "inherited", None))

    def test_ofl_fallback(self):
        # Vendor preset inherits a name that only exists in the OFL map.
        fid, src, entry = self.resolve(
            "A", [self.rec("A", inherits="Generic PLA @System")],
            [self.rec("Generic PLA @System", inherits="fdm_pla"),
             self.rec("fdm_pla", "OGFL99")])
        self.assertEqual(fid, "OGFL99")
        self.assertEqual(entry, "Generic PLA @System")

    def test_ofl_stays_in_ofl(self):
        # Once a chain enters OFL it stays there: a vendor file sharing an
        # OFL-internal hop's name must not shadow it.
        fid, _src, entry = self.resolve(
            "A",
            [self.rec("A", inherits="ofl_entry"), self.rec("fdm_pla", "WRONG")],
            [self.rec("ofl_entry", inherits="fdm_pla"), self.rec("fdm_pla", "RIGHT")])
        self.assertEqual(fid, "RIGHT")
        self.assertEqual(entry, "ofl_entry")

    def test_dead_end_retries_parent_in_ofl(self):
        # The vendor chain dead-ends id-less on a parent that also exists in
        # OFL: the loader re-consults the OFL map for that direct parent.
        fid, _src, entry = self.resolve(
            "A", [self.rec("A", inherits="shared"), self.rec("shared")],
            [self.rec("shared", "OFLID1")])
        self.assertEqual(fid, "OFLID1")
        self.assertEqual(entry, "shared")

    def test_cycle(self):
        fid, src, _e = self.resolve(
            "A", [self.rec("A", inherits="B"), self.rec("B", inherits="A")], [])
        self.assertEqual((fid, src), (None, "cycle"))

    def test_dangling_parent(self):
        fid, src, _e = self.resolve("A", [self.rec("A", inherits="nope")], [])
        self.assertEqual((fid, src), (None, "dangling"))

    def test_missing_id(self):
        fid, src, _e = self.resolve("A", [self.rec("A")], [])
        self.assertEqual((fid, src), (None, "missing"))

    def test_skip_own_resolves_inherited(self):
        fid, _src, _e = self.resolve(
            "A", [self.rec("A", "OWN", inherits="B"), self.rec("B", "PARENT")], [],
            skip_own=True)
        self.assertEqual(fid, "PARENT")


# ---------------------------------------------------------------------------
# reserved namespaces
# ---------------------------------------------------------------------------

class TestReservedSpaces(unittest.TestCase):
    def test_owners(self):
        self.assertEqual(afi.reserved_space_owner("GFL99"), (True, "BBL"))
        self.assertEqual(afi.reserved_space_owner("QD_X4_PLA"), (True, "Qidi"))
        self.assertEqual(afi.reserved_space_owner("P1234abc"), (True, None))
        self.assertEqual(afi.reserved_space_owner("pAbCdEf1"), (True, None))  # case-insensitive
        self.assertEqual(afi.reserved_space_owner("null"), (True, None))
        self.assertEqual(afi.reserved_space_owner("OFiJYDPC"), (False, None))
        self.assertEqual(afi.reserved_space_owner("P1234abcd"), (False, None))  # 8 hex chars: not the user space


# ---------------------------------------------------------------------------
# checks on synthetic trees
# ---------------------------------------------------------------------------

class TestChecks(SyntheticTreeCase):
    def test_clean_tree_is_silent(self):
        errors, out = self.t.check()
        self.assertEqual(errors, 0, out)
        self.assertNotIn("[ERROR]", out)

    def test_check1_unknown_non_of_id(self):
        self.t.write_preset("VendorA", preset("BPLA @base", filament_id="BOGUS_9",
                                              instantiation=False))
        self.t.write_preset("VendorA", preset("BPLA @P1", inherits="BPLA @base",
                                              compatible_printers=["P1"]))
        errors, out = self.t.check()
        self.assertGreater(errors, 0)
        self.assertIn("neither grandfathered in the snapshot", out)
        self.assertIn("BOGUS_9", out)

    def test_check2_new_claim_needs_snapshot_update(self):
        self.t.write_preset("VendorA", preset("ANEW @P2", inherits="APLA @base",
                                              compatible_printers=["P2"]))
        errors, out = self.t.check()
        self.assertGreater(errors, 0)
        self.assertIn('claim "VendorA/ANEW" is not sanctioned', out)
        self.assertIn("--update-snapshot", out)

    def test_check2_vanished_claim_is_stability_error(self):
        self.t.remove_preset("VendorA", "APLA @P1")
        errors, out = self.t.check()
        self.assertGreater(errors, 0)
        self.assertIn("stability", out)
        self.assertIn('"VendorA/APLA"', out)

    def test_check3_of_id_must_match_mint(self):
        self.t.write_preset("VendorA", preset("BNEW @base", filament_id="OFZZZZZZ",
                                              instantiation=False))
        self.t.write_preset("VendorA", preset("BNEW @P1", inherits="BNEW @base",
                                              compatible_printers=["P1"]))
        errors, out = self.t.check()
        self.assertGreater(errors, 0)
        self.assertIn("does not match its mint", out)
        self.assertIn(afi.generate_filament_id("VendorA", "BNEW"), out)

    def test_check3_salted_mint_is_accepted(self):
        salted = afi.generate_filament_id("VendorA", "BNEW", salt=3)
        self.t.write_preset("VendorA", preset("BNEW @base", filament_id=salted,
                                              instantiation=False))
        self.t.write_preset("VendorA", preset("BNEW @P1", inherits="BNEW @base",
                                              compatible_printers=["P1"]))
        _errors, out = self.t.check()  # check 2 still wants a snapshot update
        self.assertNotIn("does not match its mint", out)

    def test_check4_retired_id_reuse(self):
        afi.write_ledger(self.t.retired, {"retired": {"AX01": ["VendorA/APLA"]}})
        errors, out = self.t.check()
        self.assertGreater(errors, 0)
        self.assertIn("retired", out)
        self.assertIn("AX01", out)

    def test_check5_renamed_tuned_generic(self):
        self.t.write_preset("VendorA", preset("Tuned PLA @P1",
                                              inherits="Generic PLA @System",
                                              compatible_printers=["P1"]))
        errors, out = self.t.check()
        self.assertGreater(errors, 0)
        self.assertIn("rename re-exposes the OFL preset", out)
        self.assertIn("Tuned PLA @P1", out)

    def test_check5_empty_compatible_printers(self):
        self.t.write_preset("VendorA", preset("Generic PLA @P2",
                                              inherits="Generic PLA @System",
                                              compatible_printers=[]))
        errors, out = self.t.check()
        self.assertGreater(errors, 0)
        self.assertIn("cannot shadow the OFL preset", out)

    def test_check5_exception_is_grandfathered(self):
        self.t.write_preset("VendorA", preset("Tuned PLA @P1",
                                              inherits="Generic PLA @System",
                                              compatible_printers=["P1"]))
        rc, _out = self.t.update_snapshot()
        self.assertEqual(rc, 0)
        errors, out = self.t.check()
        self.assertEqual(errors, 0, out)

    def test_check6_reserved_namespace_claims(self):
        for fid, marker in [("GFX99", "owned by BBL"),
                            ("QD_X_PLA", "owned by Qidi"),
                            ("P1a2b3c4", "user-custom"),
                            ("null", "user-custom")]:
            with self.subTest(fid=fid):
                name = f"R{fid} @base"
                self.t.write_preset("VendorA", preset(name, filament_id=fid,
                                                      instantiation=False))
                self.t.write_preset("VendorA", preset(f"R{fid} @P1", inherits=name,
                                                      compatible_printers=["P1"]))
                errors, out = self.t.check()
                self.assertGreater(errors, 0)
                self.assertIn("reserved id space", out)
                self.assertIn(marker, out)

    def test_check7a_instantiated_preset_with_own_key(self):
        self.t.write_preset("VendorA", preset("APLA @P1", filament_id="AX01",
                                              inherits="APLA @base",
                                              compatible_printers=["P1 0.4 nozzle"]),
                            register=False)
        errors, out = self.t.check()
        self.assertGreater(errors, 0)
        self.assertIn("declares its own filament_id key", out)

    def test_check7b_declared_vs_inherited_drift(self):
        self.t.write_preset("VendorA", preset("APLA @P1", filament_id="AX02",
                                              inherits="APLA @base",
                                              compatible_printers=["P1 0.4 nozzle"]),
                            register=False)
        errors, out = self.t.check()
        self.assertGreater(errors, 0)
        self.assertIn('declares filament_id "AX02" but its inherits chain resolves "AX01"', out)

    def test_check7c_unresolvable_instantiated_filament(self):
        self.t.write_preset("VendorA", preset("DNEW @P1", compatible_printers=["P1"]))
        errors, out = self.t.check()
        self.assertGreater(errors, 0)
        self.assertIn("resolves no filament_id", out)
        self.assertIn("hard load error", out)

    def test_missing_snapshot_is_an_error(self):
        os.remove(self.t.snapshot)
        errors, out = self.t.check()
        self.assertEqual(errors, 1)
        self.assertIn("snapshot not found", out)


# ---------------------------------------------------------------------------
# --update-snapshot
# ---------------------------------------------------------------------------

class TestUpdateSnapshot(SyntheticTreeCase):
    def test_idempotent_and_deterministic(self):
        with open(self.t.snapshot, "rb") as f:
            first = f.read()
        rc, out = self.t.update_snapshot()
        self.assertEqual(rc, 0)
        self.assertIn("nothing changed", out)
        with open(self.t.snapshot, "rb") as f:
            self.assertEqual(f.read(), first)
        self.assertTrue(first.endswith(b"\n"))
        self.assertNotIn(b"\r", first)
        snap = json.loads(first.decode("utf-8"))
        self.assertEqual(list(snap["ids"]), sorted(snap["ids"]))
        self.assertEqual(snap["ids"]["AX01"], ["VendorA/APLA"])
        self.assertEqual(snap["ids"]["OGFL99"],
                         ["OrcaFilamentLibrary/Generic PLA", "VendorA/Generic PLA"])

    def test_vanished_id_is_retired(self):
        self.t.remove_preset("VendorA", "APLA @base")
        self.t.remove_preset("VendorA", "APLA @P1")
        rc, out = self.t.update_snapshot()
        self.assertEqual(rc, 0)
        self.assertIn("AX01", out)
        retired = load_json_file(self.t.retired)
        self.assertEqual(retired["retired"], {"AX01": ["VendorA/APLA"]})
        snap = load_json_file(self.t.snapshot)
        self.assertNotIn("AX01", snap["ids"])
        # append-only + idempotent: a second run keeps the ledger intact
        rc, out = self.t.update_snapshot()
        self.assertEqual(rc, 0)
        self.assertIn("nothing changed", out)
        retired2 = load_json_file(self.t.retired)
        self.assertEqual(retired, retired2)
        # ... and a retired id may never be minted again
        self.assertNotIn(afi.mint_filament_id("VendorA", "APLA",
                                              set(retired["retired"])), retired["retired"])

    def test_refuses_new_reserved_namespace_claims(self):
        self.t.write_preset("VendorA", preset("CNEW @base", filament_id="GFX99",
                                              instantiation=False))
        self.t.write_preset("VendorA", preset("CNEW @P1", inherits="CNEW @base",
                                              compatible_printers=["P1"]))
        with open(self.t.snapshot, "rb") as f:
            before = f.read()
        rc, out = self.t.update_snapshot()
        self.assertEqual(rc, 1)
        self.assertIn("refusing to sanction", out)
        with open(self.t.snapshot, "rb") as f:
            self.assertEqual(f.read(), before)  # nothing written on refusal
        rc, _out = self.t.update_snapshot(allow_shared_catalog=True)
        self.assertEqual(rc, 0)
        snap = load_json_file(self.t.snapshot)
        self.assertEqual(snap["ids"]["GFX99"], ["VendorA/CNEW"])


# ---------------------------------------------------------------------------
# default run: mint + insert
# ---------------------------------------------------------------------------

class TestAssign(SyntheticTreeCase):
    def test_noop_on_fully_idded_tree(self):
        changed, errors, out = self.t.assign()
        self.assertEqual((changed, errors), (0, 0))
        self.assertIn("nothing to do (0 files changed)", out)

    def test_mints_into_family_root_and_rootless_member(self):
        self.t.write_preset("VendorA", preset("FNEW @base", instantiation=False))
        self.t.write_preset("VendorA", preset("FNEW @P1", inherits="FNEW @base",
                                              compatible_printers=["P1"]))
        self.t.write_preset("VendorA", preset("FNEW @P2", inherits="FNEW @base",
                                              compatible_printers=["P2"]))
        self.t.write_preset("VendorA", preset("GNEW @P1", compatible_printers=["P1"]))
        changed, errors, _out = self.t.assign()
        self.assertEqual(errors, 0)
        self.assertEqual(changed, 2)  # one root + one root-less member
        root = load_json_file(self.t.preset_path("VendorA", "FNEW @base"))
        self.assertEqual(root["filament_id"],
                         afi.generate_filament_id("VendorA", "FNEW"))
        member = load_json_file(self.t.preset_path("VendorA", "GNEW @P1"))
        self.assertEqual(member["filament_id"],
                         afi.generate_filament_id("VendorA", "GNEW"))
        # variants themselves never get the key
        child = load_json_file(self.t.preset_path("VendorA", "FNEW @P1"))
        self.assertNotIn("filament_id", child)
        # idempotent: second run is a no-op
        changed, errors, out = self.t.assign()
        self.assertEqual((changed, errors), (0, 0))
        self.assertIn("nothing to do", out)

    def test_shared_root_between_families_is_refused(self):
        self.t.write_preset("VendorA", preset("shared_base", instantiation=False))
        self.t.write_preset("VendorA", preset("HNEW @P1", inherits="shared_base",
                                              compatible_printers=["P1"]))
        self.t.write_preset("VendorA", preset("INEW @P1", inherits="shared_base",
                                              compatible_printers=["P1"]))
        changed, errors, out = self.t.assign()
        self.assertEqual(changed, 0)
        self.assertGreater(errors, 0)
        self.assertIn("shared with famil", out)


class TestInsertEditing(unittest.TestCase):
    CRLF_TEXT = (
        '{\r\n'
        '\t"type": "filament",\r\n'
        '\t"name": "JNEW @base",\r\n'
        '\t"inherits": "fdm_pla",\r\n'
        '\t"from": "system",\r\n'
        '\t"instantiation": "false",\r\n'
        '\t"filament_type": [\r\n'
        '\t\t"PLA"\r\n'
        '\t]\r\n'
        '}\r\n'
    )

    def test_insert_before_instantiation_preserves_bytes(self):
        text, n = afi.insert_filament_id(self.CRLF_TEXT, "OFabc123")
        self.assertEqual(n, 1)
        json.loads(text)
        inserted = '\t"filament_id": "OFabc123",\r\n'
        self.assertIn(inserted + '\t"instantiation"', text)
        # every original byte is preserved: removing the inserted line restores
        # the input exactly (CRLF stays CRLF, tabs stay tabs)
        self.assertEqual(text.replace(inserted, "", 1), self.CRLF_TEXT)

    def test_insert_after_name_when_no_instantiation_line(self):
        lf_text = '{\n    "type": "filament",\n    "name": "K",\n    "from": "system"\n}\n'
        text, n = afi.insert_filament_id(lf_text, "OFabc123")
        self.assertEqual(n, 1)
        json.loads(text)
        self.assertIn('"name": "K",\n    "filament_id": "OFabc123",\n', text)
        self.assertNotIn("\r", text)

    def test_insert_no_anchor_fails(self):
        _text, n = afi.insert_filament_id('{"type": "filament"}', "OFabc123")
        self.assertEqual(n, 0)

    def test_write_filament_id_keeps_crlf_on_disk(self):
        t = SyntheticTree()
        self.addCleanup(t.cleanup)
        t.add_vendor("VendorA", [])
        path = t.preset_path("VendorA", "JNEW @base")
        with open(path, "wb") as f:
            f.write(self.CRLF_TEXT.encode("utf-8"))
        t.add_to_index("VendorA", "JNEW @base")
        afi.write_filament_id(path, "OFabc123")
        with open(path, "rb") as f:
            raw = f.read()
        self.assertEqual(raw.count(b"\n"), raw.count(b"\r\n"))  # still CRLF-only
        self.assertEqual(
            raw.replace(b'\t"filament_id": "OFabc123",\r\n', b"", 1),
            self.CRLF_TEXT.encode("utf-8"))


# ---------------------------------------------------------------------------
# the real tree
# ---------------------------------------------------------------------------

@unittest.skipUnless(os.path.isdir(REAL_PROFILES), "resources/profiles not present")
class TestRealTree(unittest.TestCase):
    def test_shipped_snapshot_matches_tree(self):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            errors = afi.check_filament_ids(REAL_PROFILES)
        self.assertEqual(errors, 0, buf.getvalue())

    def test_every_instantiated_filament_resolves_an_id(self):
        analysis = afi.analyze_tree(REAL_PROFILES)
        self.assertEqual(analysis["missing_effective"], [])
        self.assertEqual(analysis["read_errors"], [])


# ---------------------------------------------------------------------------
# review-fix regressions
# ---------------------------------------------------------------------------

class TestReviewFixes(SyntheticTreeCase):
    def test_update_snapshot_refuses_retired_id_reuse(self):
        fid = afi.generate_filament_id("VendorA", "LONER")
        self.t.write_preset("VendorA", preset("LONER @base", filament_id=fid,
                                              instantiation=False))
        self.t.write_preset("VendorA", preset("LONER @P1", inherits="LONER @base",
                                              compatible_printers=["P1 0.4 nozzle"]))
        rc, _ = self.t.update_snapshot()
        self.assertEqual(rc, 0)
        self.t.remove_preset("VendorA", "LONER @base")
        self.t.remove_preset("VendorA", "LONER @P1")
        rc, out = self.t.update_snapshot()
        self.assertEqual(rc, 0, out)
        self.assertIn(fid, afi.load_retired(self.t.retired))
        # Resurrect the same id: --update-snapshot must refuse, not sanction.
        self.t.write_preset("VendorA", preset("LONER @base", filament_id=fid,
                                              instantiation=False))
        self.t.write_preset("VendorA", preset("LONER @P1", inherits="LONER @base",
                                              compatible_printers=["P1 0.4 nozzle"]))
        rc, out = self.t.update_snapshot()
        self.assertEqual(rc, 1)
        self.assertIn("retired", out)
        self.assertNotIn(fid, load_json_file(self.t.snapshot)["ids"])

    def test_check3_skips_of_id_inherited_from_other_vendor(self):
        # Post-Phase-1 world: an OFL generic carries its own minted OF id and a
        # vendor tunes it correctly (same base name, non-empty printers). The
        # new claim must trip only the snapshot gate, never mint conformance.
        fid = afi.generate_filament_id(OFL, "Generic PLA Matte")
        self.t.write_preset(OFL, preset("Generic PLA Matte @base", filament_id=fid,
                                        instantiation=False))
        self.t.write_preset(OFL, preset("Generic PLA Matte @System",
                                        inherits="Generic PLA Matte @base",
                                        compatible_printers=[]))
        rc, _ = self.t.update_snapshot()
        self.assertEqual(rc, 0)
        self.t.write_preset("VendorA", preset("Generic PLA Matte @P1",
                                              inherits="Generic PLA Matte @System",
                                              compatible_printers=["P1 0.4 nozzle"]))
        errors, out = self.t.check()
        self.assertNotIn("does not match its mint", out)
        self.assertIn("not sanctioned", out)
        self.assertEqual(errors, 1, out)
        # After sanctioning the claim the tree is fully green again.
        rc, _ = self.t.update_snapshot()
        self.assertEqual(rc, 0)
        errors, out = self.t.check()
        self.assertEqual(errors, 0, out)

    def test_check7c_prints_expected_mint(self):
        self.t.write_preset("VendorA", preset("Orphan PLA @P1",
                                              compatible_printers=["P1 0.4 nozzle"]))
        errors, out = self.t.check()
        self.assertGreater(errors, 0)
        self.assertIn(afi.generate_filament_id("VendorA", "Orphan PLA"), out)


if __name__ == "__main__":
    unittest.main()
