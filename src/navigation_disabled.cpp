// SPDX-License-Identifier: Apache-2.0
#include "poima/navigation.hpp"
#include <stdexcept>
namespace poima::navigation {
struct Mesh::Impl {};
Mesh::Mesh(std::unique_ptr<Impl> value):impl_(std::move(value)) {}
Mesh::~Mesh()=default;
bool available() noexcept { return false; }
namespace { [[noreturn]] void unavailable() {throw std::runtime_error("Static navigation is not built; configure POIMA_ENABLE_NAVIGATION=ON.");} }
std::string profile_json(const Profile&) {unavailable();}
Profile parse_profile(const std::string&) {unavailable();}
std::shared_ptr<const Mesh> Mesh::bake(const Geometry&,const Profile&,const std::string&,std::size_t) {unavailable();}
std::shared_ptr<const Mesh> Mesh::decode(const std::string&,std::size_t) {unavailable();}
std::string Mesh::encode() const {unavailable();}
std::string Mesh::metadata() const {unavailable();}
std::string Mesh::source_fingerprint() const {unavailable();}
Path Mesh::path(const PathRequest&,std::size_t) const {unavailable();}
}
