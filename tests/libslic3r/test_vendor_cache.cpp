#include <catch2/catch_all.hpp>

#include <boost/filesystem.hpp>
#include <boost/crc.hpp>
#include <fstream>
#include <set>

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() / fs::unique_path("orca-cache-test-%%%%-%%%%");
        fs::create_directories(path);
    }
    ~TempDir() { boost::system::error_code ec; fs::remove_all(path, ec); }
};

std::string write_vendor_json(const fs::path& dir, const std::string& vendor_id,
                               const std::string& version = "1.0.0")
{
    const fs::path p = dir / (vendor_id + ".json");
    std::ofstream f(p.string());
    f << R"({"version":")" << version << R"(","name":")" << vendor_id << R"("})";
    return p.string();
}

// One vendor profile with a single process preset beside it, as an install or an
// update lays it down: <dir>/<vendor>.json plus <dir>/<vendor>/process/standard.json.
void write_vendor_tree(const fs::path& dir, const std::string& vendor, const std::string& version)
{
    fs::create_directories(dir / vendor / "process");
    std::ofstream((dir / (vendor + ".json")).string())
        << R"({"version":")" << version << R"(","name":")" << vendor
        << R"(","process_list":[{"name":"0.20mm Standard @)" << vendor << R"(","sub_path":"process/standard.json"}]})";
    std::ofstream((dir / vendor / "process" / "standard.json").string())
        << R"({"type":"process","name":"0.20mm Standard @)" << vendor
        << R"(","from":"system","instantiation":"true","layer_height":"0.2"})";
}

std::string write_versionless_vendor_json(const fs::path& dir, const std::string& vendor_id)
{
    const fs::path p = dir / (vendor_id + ".json");
    std::ofstream f(p.string());
    f << R"({"name":")" << vendor_id << R"("})";
    return p.string();
}

void corrupt_blob_byte(const std::string& path)
{
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(30);
    char b = 0; f.read(&b, 1);
    f.seekp(30);
    b ^= 0xFF;
    f.write(&b, 1);
}

// Patch cache_version (blob[0..3], i.e. file offset 20) and recompute CRC so the
// file passes the CRC check but fails the cache_version check in load_vendor_cache.
void patch_cache_version(const std::string& path, uint32_t wrong_version)
{
    std::ifstream in(path, std::ios::binary);
    std::vector<char> data(std::istreambuf_iterator<char>(in), {});
    in.close();
    if (data.size() < 24) return;
    std::memcpy(&data[20], &wrong_version, 4);
    boost::crc_32_type crc;
    crc.process_bytes(&data[20], data.size() - 20);
    const uint32_t new_crc = crc.checksum();
    std::memcpy(&data[16], &new_crc, 4);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

// Truncates the cache's PAYLOAD (everything after the 20-byte header) by
// `truncate_by` bytes and recomputes data_size/crc32 in the header, exactly
// as write_cache_blob computes them, so read_cache_blob's size and CRC checks
// still pass but cereal runs out of bytes partway through deserializing the
// body — exercising load_vendor_cache's catch block instead of its early
// (pre-body) rejection paths.
void truncate_payload_and_fix_header(const std::string& path, size_t truncate_by)
{
    constexpr size_t header_size = 20; // magic(4) + version(4) + data_size(8) + crc32(4)
    std::ifstream in(path, std::ios::binary);
    std::vector<char> data(std::istreambuf_iterator<char>(in), {});
    in.close();
    REQUIRE(data.size() > header_size + truncate_by);
    const size_t new_payload_size = data.size() - header_size - truncate_by;
    const uint64_t data_size_field = static_cast<uint64_t>(new_payload_size);
    boost::crc_32_type crc;
    crc.process_bytes(&data[header_size], new_payload_size);
    const uint32_t crc_field = crc.checksum();
    std::memcpy(&data[8],  &data_size_field, sizeof(data_size_field)); // data_size offset
    std::memcpy(&data[16], &crc_field,       sizeof(crc_field));       // crc32 offset
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(header_size + new_payload_size));
}

void add_vendor(PresetBundle& bundle, const std::string& vendor_id,
                const std::string& name = "", Semver ver = Semver(1, 0, 0))
{
    VendorProfile vp; vp.id = vendor_id;
    vp.name           = name.empty() ? vendor_id + " Corp" : name;
    vp.config_version = ver;
    bundle.vendors.emplace(vendor_id, vp);
}

Preset& add_system_preset(PresetCollection& coll, const std::string& name,
                            const VendorProfile* vp)
{
    Preset& p = coll.load_preset("", name, DynamicPrintConfig(coll.default_preset().config), false);
    p.is_system = true;
    p.vendor    = vp;
    return p;
}

static bool save_one_vendor(const PresetBundle& bundle, const std::string& path,
                            const std::string& vendor, const std::string& vendor_version,
                            const std::string& lib_version = "1.0.0")
{
    return bundle.save_vendor_cache(path, vendor, vendor_version, lib_version);
}

// resources_dir()/data_dir() are process-wide, so restore them however the test
// leaves — including through a failed REQUIRE — to stay green under --order rand.
struct ScopedDirs {
    std::string prev_data{data_dir()}, prev_rsrc{resources_dir()};
    ScopedDirs(const fs::path& data, const fs::path& rsrc)
    {
        set_data_dir(data.string());
        set_resources_dir(rsrc.string());
    }
    ~ScopedDirs() { set_data_dir(prev_data); set_resources_dir(prev_rsrc); }
};

// Helper: filter a collection by vendor_id.
std::vector<const Preset*> presets_for(const PresetCollection& coll, const std::string& vendor_id)
{
    std::vector<const Preset*> out;
    for (const Preset& p : coll())
        if (p.is_system && p.vendor && p.vendor->id == vendor_id)
            out.push_back(&p);
    return out;
}

} // namespace

namespace Slic3r {
inline bool operator==(const VendorProfile::PrinterVariant& a, const VendorProfile::PrinterVariant& b) { return a.name == b.name; }
inline bool operator==(const VendorProfile::PrinterModel& a, const VendorProfile::PrinterModel& b)
{
    return a.id == b.id && a.name == b.name && a.model_id == b.model_id && a.technology == b.technology
        && a.family == b.family && a.variants == b.variants && a.default_materials == b.default_materials
        && a.not_support_bed_types == b.not_support_bed_types && a.bed_model == b.bed_model
        && a.bed_texture == b.bed_texture && a.image_bed_type == b.image_bed_type
        && a.bottom_texture_end_name == b.bottom_texture_end_name
        && a.use_double_extruder_default_texture == b.use_double_extruder_default_texture
        && a.bottom_texture_rect == b.bottom_texture_rect
        && a.bottom_texture_rect_longer == b.bottom_texture_rect_longer
        && a.middle_texture_rect == b.middle_texture_rect && a.hotend_model == b.hotend_model;
}
} // namespace Slic3r

static bool vendor_deep_equal(const VendorProfile& a, const VendorProfile& b)
{
    return a.name == b.name && a.id == b.id && a.config_version == b.config_version
        && a.config_update_url == b.config_update_url && a.changelog_url == b.changelog_url
        && a.models == b.models && a.default_filaments == b.default_filaments
        && a.default_sla_materials == b.default_sla_materials;
}

static bool preset_deep_equal(const Preset& a, const Preset& b)
{
    return a.type == b.type && a.is_default == b.is_default && a.is_external == b.is_external
        && a.is_system == b.is_system && a.is_visible == b.is_visible && a.is_dirty == b.is_dirty
        && a.is_compatible == b.is_compatible && a.is_project_embedded == b.is_project_embedded
        && a.name == b.name && a.file == b.file && a.loaded == b.loaded
        && a.config.equals(b.config)
        && a.alias == b.alias && a.renamed_from == b.renamed_from
        && a.m_excluded_from == b.m_excluded_from && a.m_from_orca_filament_lib == b.m_from_orca_filament_lib
        && a.bundle_id == b.bundle_id && a.version == b.version && a.ini_str == b.ini_str
        && a.setting_id == b.setting_id && a.filament_id == b.filament_id && a.user_id == b.user_id
        && a.base_id == b.base_id && a.sync_info == b.sync_info && a.description == b.description
        && a.updated_time == b.updated_time && a.key_values == b.key_values;
}

TEST_CASE("a saved cache loads back with names, aliases and filament ids intact", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid   = "Acme";
    const fs::path     cache = tmp.path / "vendor.opc";

    PresetBundle src;
    add_vendor(src, vid);
    const VendorProfile* vp = &src.vendors.at(vid);

    Preset& fp = add_system_preset(src.filaments, vid + " PLA @0.4", vp);
    fp.alias       = "Acme PLA";
    fp.filament_id = "GFL_acme_pla";
    add_system_preset(src.printers, vid + " Printer 0.4", vp);

    REQUIRE(save_one_vendor(src, cache.string(), vid, "1.0.0"));

    PresetBundle out;
    REQUIRE(out.load_vendor_cache(cache.string(), vid, "1.0.0","1.0.0"));
    REQUIRE(out.vendors.count(vid) == 1);

    auto fi = presets_for(out.filaments, vid);
    auto pr = presets_for(out.printers,  vid);
    REQUIRE(fi.size() == 1);
    CHECK(fi[0]->name        == vid + " PLA @0.4");
    CHECK(fi[0]->alias       == "Acme PLA");
    CHECK(fi[0]->filament_id == "GFL_acme_pla");
    REQUIRE(pr.size() == 1);
    CHECK(pr[0]->name == vid + " Printer 0.4");
}

TEST_CASE("loading a missing cache file returns false", "[VendorCache]")
{
    TempDir tmp;
    PresetBundle out;
    REQUIRE(!out.load_vendor_cache((tmp.path / "nonexistent.opc").string(), "Acme", "1.0.0","1.0.0"));
}

TEST_CASE("a cache with a corrupted byte is rejected by the CRC check", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid   = "Acme";
    const fs::path     cache = tmp.path / "vendor.opc";

    PresetBundle src;
    add_vendor(src, vid);
    add_system_preset(src.filaments, vid + " PLA", &src.vendors.at(vid));
    REQUIRE(save_one_vendor(src, cache.string(), vid, "1.0.0"));
    corrupt_blob_byte(cache.string());

    PresetBundle out;
    REQUIRE(!out.load_vendor_cache(cache.string(), vid, "1.0.0","1.0.0"));
}

TEST_CASE("two vendors produce two independent cache files", "[VendorCache]")
{
    TempDir tmp;
    const fs::path cacheA = tmp.path / "vendorA.opc";
    const fs::path cacheB = tmp.path / "vendorB.opc";

    PresetBundle srcA;
    add_vendor(srcA, "VendorA");
    add_system_preset(srcA.filaments, "VendorA PLA", &srcA.vendors.at("VendorA"));
    REQUIRE(save_one_vendor(srcA, cacheA.string(), "VendorA", "1.0.0"));

    PresetBundle srcB;
    add_vendor(srcB, "VendorB");
    add_system_preset(srcB.filaments, "VendorB PLA", &srcB.vendors.at("VendorB"));
    REQUIRE(save_one_vendor(srcB, cacheB.string(), "VendorB", "1.0.0"));

    // Corrupt only vendor B's file; vendor A's must be unaffected.
    corrupt_blob_byte(cacheB.string());

    PresetBundle outA;
    REQUIRE(outA.load_vendor_cache(cacheA.string(), "VendorA", "1.0.0","1.0.0"));
    REQUIRE(outA.vendors.count("VendorA") == 1);
    REQUIRE(presets_for(outA.filaments, "VendorA").size() == 1);

    PresetBundle outB;
    REQUIRE(!outB.load_vendor_cache(cacheB.string(), "VendorB", "1.0.0","1.0.0"));
    REQUIRE(outB.vendors.empty());
}

TEST_CASE("vendor profile fields survive a cache round-trip", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid   = "Acme";
    const fs::path     cache = tmp.path / "vendor.opc";

    PresetBundle src;
    VendorProfile vp; vp.id = vid;
    vp.name           = "Acme Corporation";
    vp.config_version = Semver(2, 5, 1);
    VendorProfile::PrinterModel model;
    model.id   = "AcmePro";
    model.name = "Acme Pro";
    VendorProfile::PrinterVariant v0_4; v0_4.name = "0.4";
    model.variants.push_back(v0_4);
    vp.models.push_back(model);
    src.vendors.emplace(vid, vp);
    REQUIRE(save_one_vendor(src, cache.string(), vid, "2.5.1"));

    PresetBundle out;
    REQUIRE(out.load_vendor_cache(cache.string(), vid, "2.5.1","1.0.0"));
    REQUIRE(out.vendors.count(vid) == 1);
    const VendorProfile& gvp = out.vendors.at(vid);
    REQUIRE(vendor_deep_equal(gvp, src.vendors.at(vid)));
    // Spot-check the fields the old test asserted directly, so a
    // vendor_deep_equal regression still points at what actually broke.
    CHECK(gvp.id   == vid);
    CHECK(gvp.name == "Acme Corporation");
    REQUIRE(gvp.models.size() == 1);
    CHECK(gvp.models[0].id   == "AcmePro");
    CHECK(gvp.models[0].name == "Acme Pro");
    REQUIRE(gvp.models[0].variants.size() == 1);
    CHECK(gvp.models[0].variants[0].name  == "0.4");
}

TEST_CASE("config option values survive a cache round-trip", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid   = "Acme";
    const fs::path     cache = tmp.path / "vendor.opc";

    PresetBundle src;
    add_vendor(src, vid);
    Preset& fp = add_system_preset(src.filaments, vid + " PETG @0.4", &src.vendors.at(vid));
    fp.config.set_key_value("filament_type", new ConfigOptionStrings({"PETG"}));
    REQUIRE(save_one_vendor(src, cache.string(), vid, "1.0.0"));

    PresetBundle out;
    REQUIRE(out.load_vendor_cache(cache.string(), vid, "1.0.0","1.0.0"));

    auto fi = presets_for(out.filaments, vid);
    REQUIRE(fi.size() == 1);
    const auto* ft = fi[0]->config.option<ConfigOptionStrings>("filament_type");
    REQUIRE(ft != nullptr);
    REQUIRE(ft->values.size() >= 1);
    CHECK(ft->values[0] == "PETG");
}

TEST_CASE("multiple presets in one collection all round-trip", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid   = "Acme";
    const fs::path     cache = tmp.path / "vendor.opc";

    PresetBundle src;
    add_vendor(src, vid);
    const VendorProfile* vp = &src.vendors.at(vid);

    const std::vector<std::string> fi_names = {vid + " PLA", vid + " PETG", vid + " ABS"};
    const std::vector<std::string> pr_names = {vid + " Printer 0.4", vid + " Printer 0.6"};
    for (const auto& n : fi_names) add_system_preset(src.filaments, n, vp);
    for (const auto& n : pr_names) add_system_preset(src.printers,  n, vp);

    REQUIRE(save_one_vendor(src, cache.string(), vid, "1.0.0"));

    PresetBundle out;
    REQUIRE(out.load_vendor_cache(cache.string(), vid, "1.0.0","1.0.0"));

    auto fi = presets_for(out.filaments, vid);
    auto pr = presets_for(out.printers,  vid);
    REQUIRE(fi.size() == 3);
    REQUIRE(pr.size() == 2);

    std::set<std::string> fi_got, pr_got;
    for (const auto* p : fi) fi_got.insert(p->name);
    for (const auto* p : pr) pr_got.insert(p->name);
    for (const auto& n : fi_names) CHECK(fi_got.count(n) == 1);
    for (const auto& n : pr_names) CHECK(pr_got.count(n) == 1);
}

TEST_CASE("a truncated cache file is rejected", "[VendorCache]")
{
    TempDir        tmp;
    const fs::path cache = tmp.path / "truncated.opc";
    {
        std::ofstream f(cache.string(), std::ios::binary);
        const char data[] = {0x4F, 0x52, 0x43};
        f.write(data, sizeof(data));
    }
    PresetBundle out;
    REQUIRE(!out.load_vendor_cache(cache.string(), "Acme", "1.0.0","1.0.0"));
}

TEST_CASE("a cache with the wrong magic number is rejected", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid   = "Acme";
    const fs::path     cache = tmp.path / "vendor.opc";

    PresetBundle src;
    add_vendor(src, vid);
    add_system_preset(src.filaments, vid + " PLA", &src.vendors.at(vid));
    REQUIRE(save_one_vendor(src, cache.string(), vid, "1.0.0"));

    {
        std::fstream f(cache.string(), std::ios::in | std::ios::out | std::ios::binary);
        const uint32_t bad = 0xDEADBEEFu;
        f.write(reinterpret_cast<const char*>(&bad), sizeof(bad));
    }

    PresetBundle out;
    REQUIRE(!out.load_vendor_cache(cache.string(), vid, "1.0.0","1.0.0"));
}

TEST_CASE("a vendor with no presets saves and loads cleanly", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid   = "Acme";
    const fs::path     cache = tmp.path / "vendor.opc";

    PresetBundle src;
    VendorProfile vp; vp.id = vid;
    vp.name           = "Acme Corporation";
    vp.config_version = Semver(1, 0, 0);
    src.vendors.emplace(vid, vp);

    REQUIRE(save_one_vendor(src, cache.string(), vid, "1.0.0"));

    PresetBundle out;
    REQUIRE(out.load_vendor_cache(cache.string(), vid, "1.0.0","1.0.0"));
    REQUIRE(out.vendors.count(vid) == 1);
    CHECK(out.vendors.at(vid).id   == vid);
    CHECK(out.vendors.at(vid).name == "Acme Corporation");
    CHECK(presets_for(out.filaments, vid).empty());
    CHECK(presets_for(out.printers,  vid).empty());
    CHECK(presets_for(out.prints,    vid).empty());
}

TEST_CASE("all preset metadata fields survive a cache round-trip", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid   = "Acme";
    const fs::path     cache = tmp.path / "vendor.opc";

    PresetBundle src;
    add_vendor(src, vid);
    Preset& fp     = add_system_preset(src.filaments, vid + " PLA @0.4", &src.vendors.at(vid));
    fp.setting_id  = "sid-test-001";
    fp.description = "A test filament preset";
    fp.bundle_id   = "bundle-xyz";
    fp.user_id     = "user-abc";
    fp.base_id     = "base-123";
    fp.sync_info   = "update";
    fp.updated_time = 1700000000LL;
    fp.key_values  = {{"color", "red"}, {"diameter", "1.75"}};
    fp.ini_str     = "[filament]\nnozzle_temperature = 230\n";
    REQUIRE(save_one_vendor(src, cache.string(), vid, "1.0.0"));

    PresetBundle out;
    REQUIRE(out.load_vendor_cache(cache.string(), vid, "1.0.0","1.0.0"));

    const Preset* reloaded = out.filaments.find_preset(vid + " PLA @0.4", false);
    REQUIRE(reloaded != nullptr);
    const Preset* original = src.filaments.find_preset(vid + " PLA @0.4", false);
    REQUIRE(original != nullptr);
    REQUIRE(preset_deep_equal(*reloaded, *original));
}

TEST_CASE("a cache with the wrong cache version is rejected", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid   = "Acme";
    const fs::path     cache = tmp.path / "vendor.opc";

    PresetBundle src;
    add_vendor(src, vid);
    add_system_preset(src.filaments, vid + " PLA", &src.vendors.at(vid));
    REQUIRE(save_one_vendor(src, cache.string(), vid, "1.0.0"));
    patch_cache_version(cache.string(), 0xFFFFFFFFu);

    PresetBundle out;
    REQUIRE(!out.load_vendor_cache(cache.string(), vid, "1.0.0","1.0.0"));
}

TEST_CASE("a cache truncated mid-blob is rejected", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid   = "Acme";
    const fs::path     cache = tmp.path / "vendor.opc";

    PresetBundle src;
    add_vendor(src, vid);
    add_system_preset(src.filaments, vid + " PLA", &src.vendors.at(vid));
    REQUIRE(save_one_vendor(src, cache.string(), vid, "1.0.0"));

    {
        std::ifstream in(cache.string(), std::ios::binary);
        std::vector<char> buf(30); // 20-byte header + 10 bytes of blob
        in.read(buf.data(), 30);
        in.close();
        std::ofstream out(cache.string(), std::ios::binary | std::ios::trunc);
        out.write(buf.data(), 30);
    }

    PresetBundle out;
    REQUIRE(!out.load_vendor_cache(cache.string(), vid, "1.0.0","1.0.0"));
}

TEST_CASE("get_vendor_cache_version returns the version field, or nothing", "[VendorCache]")
{
    TempDir tmp;
    const std::string versioned_path   = write_vendor_json(tmp.path, "Acme", "2.5.1");
    const std::string versionless_path = write_versionless_vendor_json(tmp.path, "Beta");

    // Semver::to_string() re-splits patch into two BBS sub-fields (patch/100,
    // patch%100), so derive the expected string from the same Semver the JSON
    // was written with rather than hardcoding its form.
    const auto expected_ver = Semver::parse("2.5.1");
    REQUIRE(expected_ver.has_value());
    CHECK(get_vendor_cache_version(versioned_path) == expected_ver->to_string());

    // A profile without a version has nothing to validate a cache against.
    CHECK(get_vendor_cache_version(versionless_path).empty());
}

TEST_CASE("printer model bed texture fields survive a cache round-trip", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid   = "Acme";
    const fs::path     cache = tmp.path / "vendor.opc";

    PresetBundle src;
    add_vendor(src, vid);
    VendorProfile::PrinterModel model;
    model.id                         = "N1";
    model.name                       = "Neat One";
    model.bottom_texture_rect_longer = "5,5,50,10";
    src.vendors.at(vid).models.push_back(model);
    REQUIRE(save_one_vendor(src, cache.string(), vid, "1.0.0"));

    PresetBundle out;
    REQUIRE(out.load_vendor_cache(cache.string(), vid, "1.0.0","1.0.0"));
    REQUIRE(out.vendors.at(vid).models.size() == 1);
    REQUIRE(vendor_deep_equal(out.vendors.at(vid), src.vendors.at(vid)));
    CHECK(out.vendors.at(vid).models[0].bottom_texture_rect_longer == "5,5,50,10");
}

TEST_CASE("a cache older than the vendor profile on disk is rejected", "[VendorCache]")
{
    TempDir tmp;
    const fs::path cache = tmp.path / "vendor.opc";
    PresetBundle src;
    add_vendor(src, "Acme");
    REQUIRE(save_one_vendor(src, cache.string(), "Acme", "1.0.0"));

    PresetBundle out;
    REQUIRE(!out.load_vendor_cache(cache.string(), "Acme", "1.0.1","1.0.0"));
}

TEST_CASE("a cache newer than the vendor profile on disk is used", "[VendorCache]")
{
    TempDir tmp;
    const fs::path cache = tmp.path / "vendor.opc";
    PresetBundle src;
    add_vendor(src, "Acme");
    add_system_preset(src.filaments, "Acme PLA", &src.vendors.at("Acme"));
    REQUIRE(save_one_vendor(src, cache.string(), "Acme", "1.2.0", "2.0.0"));

    PresetBundle out;
    REQUIRE(out.load_vendor_cache(cache.string(), "Acme", "1.0.0", "1.0.0"));
    CHECK(presets_for(out.filaments, "Acme").size() == 1);
}

TEST_CASE("a cache built against an older filament library is rejected", "[VendorCache]")
{
    TempDir tmp;
    const fs::path cache = tmp.path / "vendor.opc";
    PresetBundle src;
    add_vendor(src, "Acme");
    REQUIRE(save_one_vendor(src, cache.string(), "Acme", "1.0.0", "2.0.0"));

    PresetBundle out;
    REQUIRE(!out.load_vendor_cache(cache.string(), "Acme", "1.0.0", "2.0.1"));
}

TEST_CASE("a profile with no usable version is never served from cache", "[VendorCache]")
{
    TempDir tmp;
    const fs::path cache = tmp.path / "vendor.opc";
    PresetBundle src;
    add_vendor(src, "Acme");
    REQUIRE(save_one_vendor(src, cache.string(), "Acme", "1.0.0"));

    PresetBundle out;
    // An unversioned vendor profile has no version to compare against.
    REQUIRE(!out.load_vendor_cache(cache.string(), "Acme", "", "1.0.0"));
    // Neither has an unversioned filament library.
    REQUIRE(!out.load_vendor_cache(cache.string(), "Acme", "1.0.0", ""));
    // And a cache carrying no version of its own is unusable either way.
    REQUIRE(save_one_vendor(src, cache.string(), "Acme", "", ""));
    REQUIRE(!out.load_vendor_cache(cache.string(), "Acme", "1.0.0", "1.0.0"));
    REQUIRE(out.vendors.empty());
}

TEST_CASE("a vendor's cache is its whole installation", "[VendorCache]")
{
    TempDir tmp;
    const fs::path rsrc = tmp.path / "resources";
    const fs::path data = tmp.path / "data";
    fs::create_directories(rsrc / "profiles" / "Acme" / "machine");
    write_vendor_json(rsrc / "profiles", "Acme");
    std::ofstream((rsrc / "profiles" / "Acme" / "machine" / "printer.json").string()) << "{}";

    PresetBundle src;
    add_vendor(src, "Acme");
    REQUIRE(save_one_vendor(src, (rsrc / "profiles" / "Acme.opc").string(), "Acme", "1.0.0"));

    ScopedDirs dirs(data, rsrc);
    REQUIRE(install_vendor_bundles_from_resources({"Acme"}));
    // The cache carries the presets, the vendor profile and the version they were
    // built at, so it is installed on its own.
    CHECK(fs::exists(data / "system" / "Acme.opc"));
    CHECK(!fs::exists(data / "system" / "Acme.json"));
    CHECK(!fs::exists(data / "system" / "Acme"));
    CHECK(is_vendor_installed("Acme"));
    CHECK(installed_vendor_version("Acme") == Semver(1, 0, 0));

    // A vendor with no cache is installed as its profile and preset JSONs instead,
    // parsing them being the only way left to load it — and the cache the previous
    // install left behind has to go, or it would shadow the profile just installed.
    fs::remove(rsrc / "profiles" / "Acme.opc");
    REQUIRE(install_vendor_bundles_from_resources({"Acme"}));
    CHECK(!fs::exists(data / "system" / "Acme.opc"));
    CHECK(fs::exists(data / "system" / "Acme" / "machine" / "printer.json"));
    CHECK(installed_vendor_version("Acme") == Semver(1, 0, 0));

    // Installing the cache again takes the profile and its preset JSONs back out.
    REQUIRE(save_one_vendor(src, (rsrc / "profiles" / "Acme.opc").string(), "Acme", "1.0.0"));
    REQUIRE(install_vendor_bundles_from_resources({"Acme"}));
    CHECK(fs::exists(data / "system" / "Acme.opc"));
    CHECK(!fs::exists(data / "system" / "Acme.json"));
    CHECK(!fs::exists(data / "system" / "Acme"));
}

TEST_CASE("a vendor shipped as a cache alone is installed and loaded from it", "[VendorCache]")
{
    TempDir tmp;
    const fs::path rsrc = tmp.path / "resources" / "profiles";
    const fs::path user = tmp.path / "data" / PRESET_SYSTEM_DIR;
    fs::create_directories(rsrc);
    fs::create_directories(user);

    // A packaged build: every vendor is its cache, with no profile of any kind
    // beside it — not even the filament library's, and every other vendor's cache
    // is judged against the library in effect.
    const std::string lib(PresetBundle::ORCA_FILAMENT_LIBRARY);
    PresetBundle shipped_lib;
    add_vendor(shipped_lib, lib, "Shipped Library");
    REQUIRE(save_one_vendor(shipped_lib, (rsrc / (lib + ".opc")).string(), lib, "1.0.0"));
    PresetBundle shipped;
    add_vendor(shipped, "Acme", "Shipped Acme");
    REQUIRE(save_one_vendor(shipped, (rsrc / "Acme.opc").string(), "Acme", "1.0.0"));

    ScopedDirs dirs(tmp.path / "data", tmp.path / "resources");
    // The version the build ships the vendor at comes from the cache, there being
    // no profile to read it from.
    CHECK(resource_vendor_version("Acme") == Semver(1, 0, 0));

    // Nothing installed yet, so the library the shipped cache is judged against is
    // the shipped one — itself a cache.
    PresetBundle before;
    before.load_vendor_configs_from_json(user.string(), "Acme", PresetBundle::LoadSystem,
                                         ForwardCompatibilitySubstitutionRule::EnableSilent);
    CHECK(before.vendors.at("Acme").name == "Shipped Acme");

    REQUIRE(install_vendor_bundles_from_resources({lib, "Acme"}));
    CHECK(fs::exists(user / "Acme.opc"));
    CHECK(!fs::exists(user / "Acme.json"));
    CHECK(installed_vendor_version("Acme") == Semver(1, 0, 0));

    PresetBundle after;
    after.load_vendor_configs_from_json(user.string(), "Acme", PresetBundle::LoadSystem,
                                        ForwardCompatibilitySubstitutionRule::EnableSilent);
    CHECK(after.vendors.at("Acme").name == "Shipped Acme");
}

TEST_CASE("a vendor cache installed in the data dir shadows the shipped one", "[VendorCache]")
{
    TempDir tmp;
    const fs::path rsrc = tmp.path / "resources" / "profiles";
    const fs::path user = tmp.path / "data" / PRESET_SYSTEM_DIR;
    fs::create_directories(rsrc);
    fs::create_directories(user);

    write_vendor_tree(rsrc, "Acme", "1.0.0");
    write_vendor_json(rsrc, PresetBundle::ORCA_FILAMENT_LIBRARY);
    PresetBundle shipped;
    add_vendor(shipped, "Acme", "Shipped Acme");
    REQUIRE(save_one_vendor(shipped, (rsrc / "Acme.opc").string(), "Acme", "1.0.0"));

    ScopedDirs dirs(tmp.path / "data", tmp.path / "resources");
    // The vendor profiles name the vendor "Acme"; the caches name it after where
    // they came from, so the loaded name says which source answered.
    auto loaded_name = [&user](PresetBundle& bundle) {
        bundle.load_vendor_configs_from_json(user.string(), "Acme", PresetBundle::LoadSystem,
                                             ForwardCompatibilitySubstitutionRule::EnableSilent);
        return bundle.vendors.at("Acme").name;
    };

    // Nothing installed yet: the shipped cache answers.
    PresetBundle from_rsrc;
    CHECK(loaded_name(from_rsrc) == "Shipped Acme");

    // Installing a newer vendor profile makes the shipped cache too old for it, even
    // though that cache still matches the profile sitting beside it in resources.
    write_vendor_tree(user, "Acme", "2.0.0");
    write_vendor_json(user, PresetBundle::ORCA_FILAMENT_LIBRARY);
    PresetBundle stale;
    CHECK(loaded_name(stale) == "Acme");

    // The cache installed alongside it does answer, and wins over the shipped one.
    PresetBundle installed;
    add_vendor(installed, "Acme", "Installed Acme");
    REQUIRE(save_one_vendor(installed, (user / "Acme.opc").string(), "Acme", "2.0.0"));
    PresetBundle from_user;
    CHECK(loaded_name(from_user) == "Installed Acme");
}

TEST_CASE("a cache installed with no profile beside it is used whatever its version", "[VendorCache]")
{
    TempDir tmp;
    const fs::path rsrc = tmp.path / "resources" / "profiles";
    const fs::path user = tmp.path / "data" / PRESET_SYSTEM_DIR;
    fs::create_directories(rsrc);
    fs::create_directories(user);
    write_vendor_json(rsrc, "Acme");
    write_vendor_json(rsrc, PresetBundle::ORCA_FILAMENT_LIBRARY);

    // Installed at an older version than the one now shipped in resources. Nothing
    // sits beside it claiming to be newer, so the cache is what the vendor is.
    PresetBundle src;
    add_vendor(src, "Acme", "Installed Acme");
    REQUIRE(save_one_vendor(src, (user / "Acme.opc").string(), "Acme", "0.9.0"));

    ScopedDirs dirs(tmp.path / "data", tmp.path / "resources");
    CHECK(PresetBundle::peek_vendor_cache_version((user / "Acme.opc").string(), "Acme") == "0.9.0");
    CHECK(PresetBundle::peek_vendor_cache_version((user / "Acme.opc").string(), "Other").empty());
    CHECK(installed_vendor_version("Acme") == Semver(0, 9, 0));

    // Loading the vendor takes the installed cache, not the newer shipped profile.
    PresetBundle out;
    out.load_vendor_configs_from_json(user.string(), "Acme", PresetBundle::LoadSystem,
                                      ForwardCompatibilitySubstitutionRule::EnableSilent);
    CHECK(out.vendors.at("Acme").name == "Installed Acme");
}

TEST_CASE("a vendor whose cache covers it is loaded without parsing any JSON", "[VendorCache]")
{
    TempDir tmp;
    const fs::path rsrc = tmp.path / "resources" / "profiles";
    const fs::path user = tmp.path / "data" / PRESET_SYSTEM_DIR;
    fs::create_directories(rsrc);
    fs::create_directories(user);
    write_vendor_json(rsrc, PresetBundle::ORCA_FILAMENT_LIBRARY);

    PresetBundle src;
    add_vendor(src, "Acme", "Cached Acme");
    const VendorProfile* vp = &src.vendors.at("Acme");
    add_system_preset(src.filaments, "Acme PLA @0.4", vp);
    add_system_preset(src.printers, "Acme Printer 0.4", vp);
    REQUIRE(save_one_vendor(src, (user / "Acme.opc").string(), "Acme", "1.0.0"));

    ScopedDirs dirs(tmp.path / "data", tmp.path / "resources");

    // The cache is the whole installation — no profile, no preset JSONs — and the
    // caller asks for the vendor exactly as it would for a JSON install.
    PresetBundle out;
    auto [substitutions, presets_loaded] = out.load_vendor_configs_from_json(
        user.string(), "Acme", PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::Disable);
    CHECK(substitutions.empty());
    CHECK(presets_loaded == 2);
    CHECK(out.vendors.at("Acme").name == "Cached Acme");

    // Nothing was written back: the presets never came from a parse.
    CHECK(!fs::exists(user / "Acme.json"));
}

TEST_CASE("a vendor whose cache is stale falls back to parsing its JSONs", "[VendorCache]")
{
    TempDir tmp;
    const fs::path rsrc = tmp.path / "resources" / "profiles";
    const fs::path user = tmp.path / "data" / PRESET_SYSTEM_DIR;
    fs::create_directories(rsrc);
    fs::create_directories(user);
    write_vendor_json(rsrc, PresetBundle::ORCA_FILAMENT_LIBRARY);

    // An update installed the vendor at 2.0.0; the cache next to it was built from
    // the profile before that, so it no longer covers what is on disk.
    write_vendor_tree(user, "Acme", "2.0.0");
    PresetBundle src;
    add_vendor(src, "Acme", "Cached Acme");
    REQUIRE(save_one_vendor(src, (user / "Acme.opc").string(), "Acme", "1.0.0"));

    ScopedDirs dirs(tmp.path / "data", tmp.path / "resources");

    PresetBundle out;
    auto [substitutions, presets_loaded] = out.load_vendor_configs_from_json(
        user.string(), "Acme", PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent);
    CHECK(presets_loaded == 1);
    CHECK(out.vendors.at("Acme").config_version == Semver(2, 0, 0));

    // A one-off parse like this one leaves the stale cache alone: only a bundle
    // told its parses are complete writes one.
    CHECK(PresetBundle::peek_vendor_cache_version((user / "Acme.opc").string(), "Acme") == "1.0.0");

    PresetBundle caching;
    caching.set_generate_vendor_caches(true);
    caching.load_vendor_configs_from_json(user.string(), "Acme", PresetBundle::LoadSystem,
                                          ForwardCompatibilitySubstitutionRule::EnableSilent);
    CHECK(PresetBundle::peek_vendor_cache_version((user / "Acme.opc").string(), "Acme")
          == get_vendor_cache_version((user / "Acme.json").string()));
}

TEST_CASE("a vendor with nothing installed is parsed from the shipped profiles", "[VendorCache]")
{
    TempDir tmp;
    const fs::path rsrc = tmp.path / "resources" / "profiles";
    const fs::path user = tmp.path / "data" / PRESET_SYSTEM_DIR;
    fs::create_directories(user);
    fs::create_directories(rsrc);
    write_vendor_json(rsrc, PresetBundle::ORCA_FILAMENT_LIBRARY);
    write_vendor_tree(rsrc, "Acme", "1.0.0");

    ScopedDirs dirs(tmp.path / "data", tmp.path / "resources");

    // Neither a cache nor a profile in the directory asked for, so the vendor comes
    // out of resources — which is where a build that ships caches keeps the JSONs a
    // rejected cache has to be re-parsed from.
    PresetBundle out;
    auto [substitutions, presets_loaded] = out.load_vendor_configs_from_json(
        user.string(), "Acme", PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent);
    CHECK(presets_loaded == 1);
    CHECK(out.vendors.at("Acme").config_version == Semver(1, 0, 0));

    // The cache such a parse writes lands in the directory asked for, stamped with
    // the version of the profile it was actually built from.
    PresetBundle caching;
    caching.set_generate_vendor_caches(true);
    caching.load_vendor_configs_from_json(user.string(), "Acme", PresetBundle::LoadSystem,
                                          ForwardCompatibilitySubstitutionRule::EnableSilent);
    CHECK(PresetBundle::peek_vendor_cache_version((user / "Acme.opc").string(), "Acme")
          == get_vendor_cache_version((rsrc / "Acme.json").string()));
}

TEST_CASE("a cache with a mismatched vendor name is rejected", "[VendorCache]")
{
    TempDir tmp;
    const fs::path cache = tmp.path / "vendor.opc";
    PresetBundle src;
    add_vendor(src, "VendorA");
    REQUIRE(save_one_vendor(src, cache.string(), "VendorA", "1.0.0"));

    PresetBundle out;
    REQUIRE(!out.load_vendor_cache(cache.string(), "VendorB", "1.0.0","1.0.0"));
}

TEST_CASE("a cache is rejected against an unparsable version", "[VendorCache]")
{
    TempDir tmp;
    const fs::path cache = tmp.path / "vendor.opc";
    PresetBundle src;
    add_vendor(src, "Acme");
    REQUIRE(save_one_vendor(src, cache.string(), "Acme", "1.0.0"));
    PresetBundle out;
    REQUIRE(!out.load_vendor_cache(cache.string(), "Acme", "not-a-version","1.0.0"));
    REQUIRE(out.vendors.empty());   // rejection happens before the body is touched
}

TEST_CASE("obsolete preset names survive a cache round-trip", "[VendorCache]")
{
    TempDir tmp;
    const fs::path cache = tmp.path / "vendor.opc";
    PresetBundle src;
    add_vendor(src, "Acme");
    src.obsolete_presets.filaments = {"Old filament"};
    src.obsolete_presets.printers  = {"Old printer"};
    REQUIRE(save_one_vendor(src, cache.string(), "Acme", "1.0.0"));
    PresetBundle out;
    REQUIRE(out.load_vendor_cache(cache.string(), "Acme", "1.0.0","1.0.0"));
    REQUIRE(out.obsolete_presets.filaments == std::vector<std::string>{"Old filament"});
    REQUIRE(out.obsolete_presets.printers  == std::vector<std::string>{"Old printer"});
}

TEST_CASE("printer hold aliases survive a cache round-trip", "[VendorCache]")
{
    // m_printer_hold_alias has no public getter: is_alias_exist() looks it up,
    // but only after finding the alias in m_map_alias_to_profile_name, which
    // is populated solely by the (protected) system-preset load path, so it
    // cannot be probed here without that machinery. Round-trip it the same
    // way the general parity test does instead: save, load, re-save, and
    // require the second file to be byte-identical to the first — if load
    // dropped or mangled the hold-alias map, the two saves would differ. Kept
    // deliberately preset-free so this test isolates the hold-alias map from
    // the fuller parity test below.
    TempDir tmp;
    const fs::path     cache1       = tmp.path / "c1.opc";
    const fs::path     cache2       = tmp.path / "c2.opc";
    const std::string  vid          = "Acme";
    const std::string  printer_name = vid + " Printer 0.4";

    PresetBundle src;
    add_vendor(src, vid);
    Preset probe(Preset::TYPE_FILAMENT, "probe");
    probe.config.set_key_value("compatible_printers", new ConfigOptionStrings({printer_name}));
    src.filaments.set_printer_hold_alias("Acme PLA", probe);

    REQUIRE(save_one_vendor(src, cache1.string(), vid, "1.0.0"));
    PresetBundle loaded;
    REQUIRE(loaded.load_vendor_cache(cache1.string(), vid, "1.0.0","1.0.0"));
    REQUIRE(save_one_vendor(loaded, cache2.string(), vid, "1.0.0"));

    auto slurp = [](const fs::path& p) {
        std::ifstream ifs(p.string(), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(ifs), {});
    };
    REQUIRE(slurp(cache1) == slurp(cache2));
}

TEST_CASE("inheritance maps survive a cache round-trip", "[VendorCache]")
{
    // m_config_maps/m_filament_id_maps are public data members of PresetBundle
    // (no accessor needed); they matter only for the Orca filament library's
    // own cache, since they are the inheritance base other vendors resolve
    // against, but save_vendor_cache/load_vendor_cache archive them
    // unconditionally regardless of which vendor is being cached.
    TempDir tmp;
    const fs::path     cache      = tmp.path / "lib.opc";
    const std::string  lib_vendor = "OrcaFilamentLibrary";

    PresetBundle src;
    DynamicPrintConfig cfg;
    cfg.set_key_value("filament_type", new ConfigOptionStrings({"PLA"}));
    src.m_config_maps["Generic PLA"]      = cfg;
    src.m_filament_id_maps["Generic PLA"] = "GFL00";

    REQUIRE(save_one_vendor(src, cache.string(), lib_vendor, "1.0.0", "1.0.0"));

    PresetBundle out;
    REQUIRE(out.load_vendor_cache(cache.string(), lib_vendor, "1.0.0", "1.0.0"));
    REQUIRE(out.m_filament_id_maps == src.m_filament_id_maps);
    REQUIRE(out.m_config_maps.count("Generic PLA") == 1);
    CHECK(out.m_config_maps.at("Generic PLA").equals(src.m_config_maps.at("Generic PLA")));
}

TEST_CASE("a loaded cache re-serializes to byte-identical output", "[VendorCache]")
{
    // The strongest cheap parity check: if load dropped or mangled anything
    // that save writes (including the inheritance maps and hold aliases),
    // the second file differs.
    TempDir tmp;
    const fs::path cache1 = tmp.path / "c1.opc", cache2 = tmp.path / "c2.opc";
    PresetBundle src;
    add_vendor(src, "Acme");
    const VendorProfile* vp = &src.vendors.at("Acme");
    Preset& f = add_system_preset(src.filaments, "Acme Filament", vp);
    f.alias = "AF";
    add_system_preset(src.printers, "Acme Printer 0.4 nozzle", vp);
    src.obsolete_presets.prints = {"Old print"};
    src.m_config_maps["Generic PLA"]      = DynamicPrintConfig();
    src.m_filament_id_maps["Generic PLA"] = "GFL00";
    Preset probe(Preset::TYPE_FILAMENT, "probe");
    probe.config.set_key_value("compatible_printers", new ConfigOptionStrings({"Acme Printer 0.4 nozzle"}));
    src.filaments.set_printer_hold_alias("AF", probe);

    REQUIRE(src.save_vendor_cache(cache1.string(), "Acme", "1.0.0", "1.0.0"));
    PresetBundle loaded;
    REQUIRE(loaded.load_vendor_cache(cache1.string(), "Acme", "1.0.0", "1.0.0"));
    REQUIRE(loaded.save_vendor_cache(cache2.string(), "Acme", "1.0.0", "1.0.0"));
    auto slurp = [](const fs::path& p) {
        std::ifstream ifs(p.string(), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(ifs), {});
    };
    REQUIRE(slurp(cache1) == slurp(cache2));
}

TEST_CASE("a cache that fails mid-body deserialization is rejected and leaves the bundle clean", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid           = "Acme";
    const fs::path    valid_cache   = tmp.path / "valid.opc";
    const fs::path    corrupt_cache = tmp.path / "corrupt.opc";

    PresetBundle src;
    add_vendor(src, vid);
    add_system_preset(src.filaments, vid + " PLA @0.4", &src.vendors.at(vid));
    Preset probe(Preset::TYPE_FILAMENT, "probe");
    probe.config.set_key_value("compatible_printers", new ConfigOptionStrings({vid + " Printer 0.4"}));
    src.filaments.set_printer_hold_alias(vid + " PLA alias", probe);

    REQUIRE(save_one_vendor(src, valid_cache.string(), vid, "1.0.0"));
    // Truncate the tail (obsolete-preset vectors + m_errors, per save_vendor_cache's
    // field order) so the header's size/CRC still validate but cereal runs out of
    // bytes partway through the body. Grow the cut if a given size ever stops
    // throwing (e.g. after an unrelated field-order change to the cache format).
    size_t truncate_by = 40;
    bool   throws      = false;
    for (; truncate_by <= 200; truncate_by += 8) {
        fs::copy_file(valid_cache, corrupt_cache, fs::copy_option::overwrite_if_exists);
        truncate_payload_and_fix_header(corrupt_cache.string(), truncate_by);
        PresetBundle probe_bundle;
        if (!probe_bundle.load_vendor_cache(corrupt_cache.string(), vid, "1.0.0","1.0.0")) {
            throws = true;
            break;
        }
    }
    REQUIRE(throws);

    PresetBundle out;
    // Pre-populate the target bundle with prior-cycle state: Fix 1(b) must
    // clear this in the catch block, not just leave it from a previous cycle.
    Preset stale_probe(Preset::TYPE_FILAMENT, "stale-probe");
    stale_probe.config.set_key_value("compatible_printers", new ConfigOptionStrings({"Stale Printer"}));
    out.filaments.set_printer_hold_alias("Stale alias", stale_probe);

    REQUIRE(!out.load_vendor_cache(corrupt_cache.string(), vid, "1.0.0","1.0.0"));
    CHECK(out.vendors.empty());

    // No public getter exists for m_printer_hold_alias, so probe it the same
    // way the round-trip tests do: serialize `out` and a never-touched fresh
    // bundle under the same vendor/keys and require the bytes to match. Since
    // save_collection archives the hold-alias map, any leftover "Stale alias"
    // entry would make the two files differ.
    const fs::path out_after = tmp.path / "out_after.opc";
    const fs::path clean_ref = tmp.path / "clean_ref.opc";
    PresetBundle   clean;
    REQUIRE(out.save_vendor_cache(out_after.string(), vid, "1.0.0","1.0.0"));
    REQUIRE(clean.save_vendor_cache(clean_ref.string(), vid, "1.0.0","1.0.0"));
    auto slurp2 = [](const fs::path& p) {
        std::ifstream ifs(p.string(), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(ifs), {});
    };
    CHECK(slurp2(out_after) == slurp2(clean_ref));

    // The recovery must leave a bundle a caller can still load a good cache into.
    REQUIRE(out.load_vendor_cache(valid_cache.string(), vid, "1.0.0","1.0.0"));
    CHECK(out.vendors.count(vid) == 1);
    CHECK(presets_for(out.filaments, vid).size() == 1);
}
