// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
namespace poima::ui {
enum class Kind { panel,label,button };
struct Element {
    std::string id,parent,name;Kind kind=Kind::panel;std::string text,action;
    bool visible=true,enabled=true;
};
// Immutable authored metadata: at most 256 elements, sorted unique nonzero
// lowercase 32-hex IDs. Parents must be panels; maximum depth is 32 (root=1).
// Names are nonempty UTF-8 <=128 bytes. Text is strict UTF-8 without NUL,
// <=16 KiB per element and <=1 MiB total; panels have no text. Only buttons
// have actions: ASCII [A-Za-z0-9_.-], 1..128 bytes. Names/text are not markup.
using Definition=std::vector<Element>;
Definition parse_definition(const std::string& json);
std::string definition_json(const Definition&);
void validate_definition(const Definition&);
struct Edit { std::string id;std::optional<std::string> text;std::optional<bool> visible,enabled; };
struct Inspection {
    std::string id,text;bool visible=true,enabled=true,effective_visible=true,effective_enabled=true,eligible=false;
    Kind kind=Kind::panel;
};
struct PresentationRow {
    Element element;
    bool effective_visible=true,effective_enabled=true,eligible=false;
};
// Owned, extent-independent projection. No DOM, runtime or GPU pointers.
struct Presentation {
    std::vector<PresentationRow> elements;
    std::string modal;
    std::uint64_t revision=0;
};
// Logical state only: no action execution, input routing or rendering. Button
// eligibility follows ancestor visibility/enabled flags and modal ancestry;
// pixel clipping, focus and hover are presentation state, not authority.
class Model {
    Definition definition_,values_;
    std::uint64_t revision_=0;
    std::string modal_;
    mutable std::shared_ptr<const Presentation> presentation_;
public:
    explicit Model(const Definition&);
    const Definition& definition() const noexcept { return definition_; }
    std::uint64_t revision() const noexcept { return revision_; }
    const std::string& modal() const noexcept { return modal_; }
    std::vector<Inspection> inspect() const;
    std::shared_ptr<const Presentation> presentation() const;
    // Atomic nonempty transaction; no duplicate IDs or empty patches. Accepted
    // edits increment revision once (maximum 2^53-1). Empty definitions reject
    // all edits. nullopt leaves modal unchanged; empty string clears it.
    void edit(std::uint64_t expected,const std::vector<Edit>&,std::optional<std::string> modal=std::nullopt);
    std::string save() const;
    // Strict complete membership/types; restores saved revision and modal.
    // Any validation/allocation failure preserves the previous logical state.
    void load(const std::string&);
};
}
