// SPDX-License-Identifier: Apache-2.0
#include "world_post_effect_assets.hpp"

#include "asset_catalog.hpp"
#include "asset_store.hpp"
#include "poima/post_effect.hpp"
#include "project_io_path.hpp"
#include "world_json.hpp"

#include <fstream>
#include <system_error>

namespace poima::world_detail {
namespace {
namespace fs = std::filesystem;

std::string read_post_effect_file(const fs::path& path) {
    const auto io=project_detail::project_io_path(path);
    require(fs::is_regular_file(fs::symlink_status(io)) && fs::hard_link_count(io)==1,
        "Post-effect artifacts must be ordinary files.",-32050);
#ifdef _WIN32
    const auto attributes=GetFileAttributesW(io.c_str());
    require(attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_REPARSE_POINT),
        "Post-effect artifacts cannot be reparse points.",-32050);
#endif
    const auto length=fs::file_size(io);
    require(length>0 && length<=max_post_effect_bytes,"Post-effect artifact exceeds its byte budget.",-32050);
    std::string bytes(static_cast<std::size_t>(length),'\0');
    std::ifstream input(io,std::ios::binary);
    require(bool(input.read(bytes.data(),static_cast<std::streamsize>(length))) && input.peek()==std::char_traits<char>::eof(),
        "Post-effect artifact read failed or changed size.",-32050);
    return bytes;
}
void publish_post_effect(const fs::path& directory,const std::string& digest,const std::string& bytes) {
    if(!asset_catalog::plain_directory(directory))fs::create_directory(project_detail::project_io_path(directory));
    require(asset_catalog::plain_directory(directory),"Expected an ordinary post-effect asset directory.",-32050);
    const auto target=project_detail::project_io_path(directory/(digest+".poimapost"));
    if(fs::symlink_status(target).type()!=fs::file_type::not_found) {
        require(read_post_effect_file(asset_package_path(directory,digest,".poimapost"))==bytes,
            "Stored post-effect bytes differ from the admitted source.",-32050);
        return;
    }
    const auto pending=fs::path(target).concat(".pending");
    write_asset_pending_exclusive(pending,bytes);
    try {
        require(asset_catalog::plain_directory(directory),"Expected an ordinary post-effect asset directory.",-32050);
#ifdef _WIN32
        require(MoveFileExW(pending.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH)!=0,
            "Cannot exclusively publish the post effect.",-32050);
#else
        require(::link(pending.c_str(),target.c_str())==0,"Cannot exclusively publish the post effect.",-32050);
        fs::remove(pending);
#endif
    } catch(...) {
        std::error_code ignored;
        fs::remove(pending,ignored);
        throw;
    }
}
} // namespace

LoadedPostEffect load_post_effect_asset(const fs::path& directory, const std::string& digest) {
    try {
        const auto path = asset_package_path(directory, digest, ".poimapost");
        const auto bytes = read_post_effect_file(path);
        return {std::make_shared<const PostEffect>(parse_post_effect(bytes, digest)), bytes.size()};
    } catch (const Error&) {
        throw;
    } catch (const std::exception& error) {
        throw Error(-32050, error.what());
    }
}

ImportedPostEffect import_post_effect_asset(const fs::path& directory, const fs::path& source,
    const std::string& expected_digest) {
    const auto bytes = read_post_effect_file(source);
    require(content_hash(bytes) == expected_digest, "Post-effect source SHA-256 conflict.", -32009);
    auto effect = parse_post_effect(bytes, expected_digest);
    publish_post_effect(directory, effect.sha256, bytes);
    return {std::move(effect.sha256), std::move(effect.id), bytes.size(), effect.words.size()};
}

} // namespace poima::world_detail
