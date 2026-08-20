#ifndef slic3r_ResinProfileMigration_hpp_
#define slic3r_ResinProfileMigration_hpp_

#include "PrintConfig.hpp"

#include <string>
#include <vector>

namespace Slic3r {

constexpr int RESIN_PROFILE_SCHEMA_VERSION = 1;

enum class ResinProfileKind { Print, Material, Printer };

struct ResinMigrationIssue {
    std::string code;
    std::string message;
    bool requires_choice { false };
};

struct ResinMigrationResult {
    int source_version { 0 };
    int target_version { RESIN_PROFILE_SCHEMA_VERSION };
    bool changed { false };
    std::vector<ResinMigrationIssue> issues;

    bool requires_choice() const noexcept;
};

ResinMigrationResult migrate_resin_profile(DynamicPrintConfig &config, ResinProfileKind kind);

bool is_mslicer_output_format(std::string format);
std::string canonical_resin_output_format(std::string format);

// Empty means valid. This is the authoritative cross-profile R4 validation
// boundary used before slicing/export and directly by migration tests.
std::string validate_resin_profile_contract(const DynamicPrintConfig &config);

} // namespace Slic3r

#endif

