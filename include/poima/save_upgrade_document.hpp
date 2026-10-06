// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/save_upgrade_plan.hpp"
#include <cstddef>
#include <string>
#include <vector>
namespace poima::save_upgrades {
struct DocumentComponentMapping {
    std::string id;
    std::size_t authored_instances=0,template_instances=0;
};
struct DocumentMappingResult {
    std::string mapped_document;
    std::vector<DocumentComponentMapping> components;
};
// Pure compatibility check/transformation of already validated FROZEN authored
// documents (freeze_content output). Receipts/retired-ID arrays must be empty;
// no history is discarded here. Only root revision, explicitly planned schemas
// and mapped custom scalar values may differ. Full schema comparison includes
// names/units, which the component fingerprint deliberately excludes.
//
// The owner must validate/freeze both original documents and their asset closure,
// bind exact content/image/schema identities with require_edge, authorize the
// plan, and validate entity references. This helper performs no IO, executable
// loading, built-in component validation or runtime activation. It checks bounded
// document structure, schemas and complete typed custom values independently.
// Output canonicalizes schemas/custom cells; every other authored byte-semantic
// JSON value must equal target (object whitespace/key ordering is irrelevant).
DocumentMappingResult map_authored_document(const std::string& source_document,
    const std::string& target_document,const Plan& plan);
}
