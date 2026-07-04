#include <catch2/catch_all.hpp>

#include "libslic3r/Preset.hpp"

using namespace Slic3r;

// Pure-function coverage of the succession chain walk (follow_filament_id_succession).
// The ledger-file loader (filament_id_succession_map) is deliberately untested here so
// the test carries no resources-dir dependency.
TEST_CASE("filament_id succession follows forwarding chains", "[Preset][filament_id]") {
    SECTION("empty map returns empty") {
        const std::map<std::string, std::string> empty;
        CHECK(follow_filament_id_succession("OFabc123", empty).empty());
        CHECK(follow_filament_id_succession("", empty).empty());
    }

    const std::map<std::string, std::string> forwards = {
        {"OFold001", "OFnew001"},
        {"OFchainA", "OFchainB"},
        {"OFchainB", "OFchainC"},
        {"OFchainC", "OFlive00"},
    };

    SECTION("absent id returns empty") {
        CHECK(follow_filament_id_succession("GFL99", forwards).empty());
    }

    SECTION("single hop resolves to the successor") {
        CHECK(follow_filament_id_succession("OFold001", forwards) == "OFnew001");
    }

    SECTION("multi-hop chain resolves to the live end") {
        CHECK(follow_filament_id_succession("OFchainA", forwards) == "OFlive00");
        CHECK(follow_filament_id_succession("OFchainB", forwards) == "OFlive00");
        CHECK(follow_filament_id_succession("OFchainC", forwards) == "OFlive00");
    }
}

// Cycles are a ledger-validation error (script check 9) and never ship, but the client
// walk must still terminate on corrupt data: it stops at the last id reached before any
// id would repeat.
TEST_CASE("filament_id succession terminates on malformed cycles", "[Preset][filament_id]") {
    SECTION("two-node cycle returns the last id before revisiting") {
        const std::map<std::string, std::string> cycle = {{"A", "B"}, {"B", "A"}};
        CHECK(follow_filament_id_succession("A", cycle) == "B");
        CHECK(follow_filament_id_succession("B", cycle) == "A");
    }

    SECTION("cycle entered through a tail stops before re-entering it") {
        // T -> A -> B -> C -> A: stops at C, the last id reached before repeating A.
        const std::map<std::string, std::string> cycle = {
            {"T", "A"}, {"A", "B"}, {"B", "C"}, {"C", "A"}};
        CHECK(follow_filament_id_succession("T", cycle) == "C");
        CHECK(follow_filament_id_succession("A", cycle) == "C");
    }

    SECTION("self-loop returns the id itself") {
        const std::map<std::string, std::string> loop = {{"A", "A"}};
        CHECK(follow_filament_id_succession("A", loop) == "A");
    }
}
