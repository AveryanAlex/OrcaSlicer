#include <catch2/catch_all.hpp>

#include "libslic3r/GCodeReader.hpp"

#include "test_helpers.hpp"

#include <cctype>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;

// 0-based tool indices used by extrusions whose role comment contains `role` (needs gcode_comments).
static std::set<int> tools_for_role(const std::string& gcode, const std::string& role)
{
    std::set<int> tools;
    int current_tool = 0;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&](GCodeReader& self, const GCodeReader::GCodeLine& line) {
        const std::string cmd(line.cmd());
        if (cmd.size() >= 2 && cmd[0] == 'T' && std::isdigit((unsigned char)cmd[1]))
            current_tool = std::stoi(cmd.substr(1));
        else if (line.extruding(self) && std::string(line.comment()).find(role) != std::string::npos)
            tools.insert(current_tool);
    });
    return tools;
}

// X where the nozzle sits while each tagged _WAIT_FOR_TEMP_ON_WIPE_TOWER M109 blocks:
// the nearest preceding G1 carrying an X (the park travel emitted just before the wait).
static std::vector<double> wait_park_xs(const std::string& gcode)
{
    std::vector<std::string> lines;
    std::istringstream stream(gcode);
    for (std::string line; std::getline(stream, line);)
        lines.emplace_back(std::move(line));
    std::vector<double> xs;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].rfind("M109", 0) != 0 || lines[i].find("_WAIT_FOR_TEMP_ON_WIPE_TOWER") == std::string::npos)
            continue;
        for (size_t j = i; j-- > 0;) {
            if (lines[j].rfind("G1 ", 0) != 0)
                continue;
            const size_t x_pos = lines[j].find('X');
            if (x_pos == std::string::npos)
                continue;
            xs.push_back(std::stod(lines[j].substr(x_pos + 1)));
            break;
        }
    }
    return xs;
}

// Tool index = filament id - 1; brim and skirt follow the wall filament.
TEST_CASE("Each feature prints with its assigned filament", "[MultiFilament]")
{
    auto [infill_filament, wall_filament] = GENERATE(table<int, int>({ {1, 1}, {1, 2}, {2, 1}, {2, 2} }));
    DYNAMIC_SECTION("infill filament " << infill_filament << ", wall filament " << wall_filament) {
        const std::string gcode = slice({ cube(20) },
            multifilament_config(2, {
                { "sparse_infill_filament_id",  infill_filament },
                { "internal_solid_filament_id", infill_filament },
                { "top_surface_filament_id",    infill_filament },
                { "bottom_surface_filament_id", infill_filament },
                { "outer_wall_filament_id",     wall_filament },
                { "inner_wall_filament_id",     wall_filament },
                { "skirt_loops",                1 },
                { "brim_type",                  "outer_only" },
                { "brim_width",                 5 },
            }));
        const std::set<int> wall_tool{ wall_filament - 1 };
        const std::set<int> infill_tool{ infill_filament - 1 };
        CHECK(tools_for_role(gcode, "perimeter") == wall_tool);
        CHECK(tools_for_role(gcode, "infill")    == infill_tool); // sparse + solid + top/bottom
        CHECK(tools_for_role(gcode, "brim")      == wall_tool);
        CHECK(tools_for_role(gcode, "skirt")     == wall_tool);
    }
}

TEST_CASE("Each feature prints with its assigned filament (three filaments)", "[MultiFilament]")
{
    const std::string gcode = slice({ cube(20) },
        multifilament_config(3, {
            { "sparse_infill_filament_id",  2 },
            { "internal_solid_filament_id", 2 },
            { "top_surface_filament_id",    2 },
            { "bottom_surface_filament_id", 2 },
            { "outer_wall_filament_id",     3 },
            { "inner_wall_filament_id",     3 },
            { "skirt_loops",                0 },
            { "brim_type",                  "no_brim" },
        }));
    CHECK(tools_for_role(gcode, "perimeter") == std::set<int>{ 2 }); // filament 3
    CHECK(tools_for_role(gcode, "infill")    == std::set<int>{ 1 }); // filament 2
}

// The override must survive tool ordering: object 1's walls print on their filament's
// tool, object 0 stays on the first. If dropped, every wall prints on tool 0.
TEST_CASE("Per-object wall filament override is honored", "[MultiFilament]")
{
    const std::string gcode = slice_with_object_overrides(
        { cube(20), cube(20) },
        multifilament_config(2, {
            { "skirt_loops",    0 },
            { "brim_type",      "no_brim" },
            { "print_sequence", "by object" },
        }),
        { {}, { { "outer_wall_filament_id", 2 }, { "inner_wall_filament_id", 2 } } });
    CHECK(tools_for_role(gcode, "perimeter") == std::set<int>{ 0, 1 });
    CHECK(tools_for_role(gcode, "infill")    == std::set<int>{ 0 }); // infill not overridden: stays on F1
}

// With wait_for_temp_on_wipe_tower the blocking M109 moves from right after the Tn command to
// a stop point parked beside the wipe tower (heat-up drool falls next to the tower, not onto
// its top): tagged with _WAIT_FOR_TEMP_ON_WIPE_TOWER, after the toolchange and before the
// repositioning move and the first extrusion of the purge, while the post-toolchange restore
// demotes to a non-blocking M104. Ordering and the off-tower stop are the contract here.
TEST_CASE("Toolchange temperature wait moves to the wipe tower when enabled", "[MultiFilament]")
{
    const bool wait_on_tower = GENERATE(false, true);
    DYNAMIC_SECTION("wait_for_temp_on_wipe_tower " << (wait_on_tower ? 1 : 0)) {
        const std::string gcode = slice_with_object_overrides(
            { cube(20), cube(20) },
            multifilament_config(2, {
                { "nozzle_diameter",                "0.4,0.4" },
                { "printer_extruder_id",            "1,2" },
                { "printer_extruder_variant",       "Direct Drive Standard,Direct Drive Standard" },
                { "extruder_printable_height",      "0,0" },
                { "single_extruder_multi_material", 0 },
                { "enable_prime_tower",             1 },
                { "prime_tower_width",              35 },
                { "wipe_tower_x",                   "50" },
                { "wipe_tower_y",                   "50" },
                { "ooze_prevention",                1 },
                { "standby_temperature_delta",      -40 },
                { "wait_for_temp_on_wipe_tower",    wait_on_tower ? 1 : 0 },
            }),
            // One filament per object -> a toolchange on every layer. Assigned at the object
            // level: the used-filament count that gates the prime tower is derived from
            // object/volume configs on the harness's single apply (region filament ids such
            // as sparse_infill_filament_id are not counted there and the tower would be
            // silently disabled).
            { { { "extruder", 1 } }, { { "extruder", 2 } } });

        // Split into lines and scan the "; CP TOOLCHANGE START".."; CP TOOLCHANGE END" blocks.
        std::vector<std::string> lines;
        std::istringstream gcode_stream(gcode);
        for (std::string line; std::getline(gcode_stream, line);)
            lines.emplace_back(std::move(line));
        const auto is_tool_line   = [](const std::string& l) { return l.size() >= 2 && l[0] == 'T' && std::isdigit((unsigned char)l[1]); };
        const auto is_m109_line   = [](const std::string& l) { return l.rfind("M109", 0) == 0; };
        const auto is_tagged_wait = [](const std::string& l) { return l.find("_WAIT_FOR_TEMP_ON_WIPE_TOWER") != std::string::npos; };
        const auto is_extruding   = [](const std::string& l) {
            if (l.rfind("G1 ", 0) != 0)
                return false;
            const size_t e = l.find(" E");
            return e != std::string::npos && l.find_first_of("XY") != std::string::npos && l[e + 2] != '-';
        };

        int checked_blocks = 0;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].find("; CP TOOLCHANGE START") == std::string::npos)
                continue;
            size_t block_end = i;
            while (block_end < lines.size() && lines[block_end].find("; CP TOOLCHANGE END") == std::string::npos)
                ++block_end;
            size_t tool_line = block_end;
            for (size_t j = i; j < block_end; ++j)
                if (is_tool_line(lines[j])) { tool_line = j; break; }
            if (tool_line == block_end)
                continue; // final unload block, no toolchange
            ++checked_blocks;

            size_t tagged_wait = block_end, untagged_m109 = block_end, first_extrusion = block_end;
            for (size_t j = tool_line + 1; j < block_end; ++j) {
                if (is_m109_line(lines[j]) && tagged_wait == block_end && is_tagged_wait(lines[j]))
                    tagged_wait = j;
                if (is_m109_line(lines[j]) && untagged_m109 == block_end && !is_tagged_wait(lines[j]))
                    untagged_m109 = j;
                if (first_extrusion == block_end && is_extruding(lines[j]))
                    first_extrusion = j;
            }
            INFO("toolchange block at line " << i + 1);
            if (wait_on_tower) {
                // The only blocking wait is the tagged one, parked beside the tower before the purge.
                REQUIRE(tagged_wait < block_end);
                CHECK(untagged_m109 == block_end);
                REQUIRE(first_extrusion < block_end);
                CHECK(tagged_wait < first_extrusion);
                // The travel preceding the wait parks outside the tower footprint. The tower
                // auto-sizes, so derive its extent from the purge extrusions of this block.
                size_t stop_line = block_end;
                for (size_t j = tagged_wait; j-- > tool_line;)
                    if (lines[j].rfind("G1 ", 0) == 0 && lines[j].find('X') != std::string::npos) { stop_line = j; break; }
                REQUIRE(stop_line < block_end);
                const double stop_x = std::stod(lines[stop_line].substr(lines[stop_line].find('X') + 1));
                double purge_min_x = std::numeric_limits<double>::max(), purge_max_x = std::numeric_limits<double>::lowest();
                for (size_t j = tagged_wait; j < block_end; ++j) {
                    const size_t x_pos = lines[j].find('X');
                    if (!is_extruding(lines[j]) || x_pos == std::string::npos)
                        continue;
                    const double x = std::stod(lines[j].substr(x_pos + 1));
                    purge_min_x = std::min(purge_min_x, x);
                    purge_max_x = std::max(purge_max_x, x);
                }
                REQUIRE(purge_min_x <= purge_max_x);
                INFO("stop travel: " << lines[stop_line] << " purge x range: " << purge_min_x << ".." << purge_max_x);
                const bool beside_tower = stop_x < purge_min_x - 0.5 || stop_x > purge_max_x + 0.5;
                CHECK(beside_tower);
            } else {
                // Stock behavior: the blocking wait follows the toolchange command directly.
                REQUIRE(untagged_m109 < block_end);
                CHECK(tagged_wait == block_end);
                if (first_extrusion < block_end)
                    CHECK(untagged_m109 < first_extrusion);
            }
            i = block_end;
        }
        REQUIRE(checked_blocks > 0);
        if (!wait_on_tower)
            CHECK(gcode.find("_WAIT_FOR_TEMP_ON_WIPE_TOWER") == std::string::npos);
    }
}

// The temperature-wait park picks its side of the tower by testing bed containment with the
// tower position at psWipeTower generation time, while WipeTowerIntegration shifts the cached
// moves by the CURRENT position at export. Moving the tower normally invalidates only
// psSkirtBrim (tower gcode is position-independent), but the park makes it bed-relative, so a
// GUI-style move-and-reslice on the same Print must regenerate the tower — otherwise the stale
// park prints outside the bed. Contract: every tagged wait parks inside the printable area.
TEST_CASE("Wipe tower temperature-wait park is regenerated when the tower moves", "[MultiFilament]")
{
    // Two objects, one filament each: a toolchange (and a tagged wait) on every layer, like
    // the wait test above — but on a single-extruder machine profile: the synthetic
    // dual-extruder keys would drag in the extruder-variant expansion, which is not
    // idempotent on the default machine profile and would pollute the re-apply diff below.
    // Rectangle wall and no brim keep the tower-local footprint inside [0, 35], so the park
    // sits at the generator's 2mm side gap: local -2 or 37.
    DynamicPrintConfig config = multifilament_config(2, {
        { "single_extruder_multi_material", 0 },
        { "enable_prime_tower",             1 },
        { "prime_tower_width",              35 },
        { "wipe_tower_wall_type",           "rectangle" }, // the default rib bulges past the width
        { "prime_tower_brim_width",         0 },           // the default 3 widens the first-layer envelope
        { "printable_area",                 "0x0,200x0,200x200,0x200" },
        { "wipe_tower_x",                   "0" },
        { "wipe_tower_y",                   "50" },
        { "ooze_prevention",                1 },
        { "standby_temperature_delta",      -40 },
        { "wait_for_temp_on_wipe_tower",    1 },
    });
    // init_print force-sets this on its own copy; set it here too so the re-apply below
    // diffs in wipe_tower_x ONLY — the exact GUI increment under test.
    config.set_key_value("gcode_comments", new ConfigOptionBool(true));

    Print print;
    Model model;
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{
        { { "extruder", 1 } }, { { "extruder", 2 } } }; // object-level, see the wait test above
    init_print(std::vector<TriangleMesh>{ cube(20), cube(20) }, print, model, config, &overrides);

    const std::string         at_edge       = gcode(print);
    const std::vector<double> at_edge_parks = wait_park_xs(at_edge);
    REQUIRE(!at_edge_parks.empty()); // the feature under test is active
    for (double x : at_edge_parks) {
        INFO("wait park X " << x << " with the tower at x=0 on a 200mm bed");
        CHECK(x >= -0.05);
        CHECK(x <= 200.05);
    }
    REQUIRE(print.is_step_done(psWipeTower));

    // Move the tower to the right bed edge (164 + 35 = 199 keeps the body printable) and
    // re-apply on the SAME Print, as the GUI does. Base the re-apply on the print's own
    // resolved config so the diff is wipe_tower_x alone — re-applying the caller's config
    // would also diff the apply-time extruder normalization write-backs, and those keys
    // regenerate the tower for the wrong reason. The cached right-side park would export
    // at 164 + 37 = 201, off the bed; regeneration clamps the park against the bed edge.
    // Assemble the moved config exactly the way init_print assembled the first one — the
    // apply-time normalization is only idempotent when both applies start from the same
    // derivation, and any stray diff key would regenerate the tower for the wrong reason.
    config.set_deserialize_strict({ { "wipe_tower_x", "164" } });
    DynamicPrintConfig moved_config = DynamicPrintConfig::full_print_config();
    moved_config.apply(config);
    moved_config.set_key_value("gcode_comments", new ConfigOptionBool(true));
    print.apply(model, moved_config);
    CHECK_FALSE(print.is_step_done(psWipeTower)); // the move must re-generate the tower

    const std::string         moved       = gcode(print);
    const std::vector<double> moved_parks = wait_park_xs(moved);
    REQUIRE(!moved_parks.empty()); // the waits must survive the re-slice
    for (double x : moved_parks) {
        INFO("wait park X " << x << " with the tower at x=164 on a 200mm bed");
        CHECK(x >= -0.05);
        CHECK(x <= 200.05);
    }
}

// max_layer_height can be shorter than the extruder count (normalization sizes it to the
// filament count under single_extruder_multi_material). calc_max_layer_height() in ToolOrdering
// indexed it per-nozzle and read past the end. Shortened directly here to isolate that read;
// the other per-extruder keys stay extruder-length so slicing reaches the code under test.
TEST_CASE("Multi-extruder slice stays in bounds with a short max_layer_height", "[MultiFilament]")
{
    DynamicPrintConfig config = multifilament_config(2);
    config.set_deserialize_strict({
        { "nozzle_diameter",           "0.4,0.4" },
        { "printer_extruder_id",       "1,2" },
        { "printer_extruder_variant",  "Direct Drive Standard,Direct Drive Standard" },
        { "extruder_printable_height", "0,0" },
        { "max_layer_height",          "0.3" }, // deliberately one entry short
    });
    Print print;
    init_and_process_print({ cube(20) }, print, config);
    REQUIRE_FALSE(print.objects().front()->layers().empty());
}
