// SPDX-License-Identifier: Apache-2.0
namespace {
// Stored ZIP records, PKWARE APPNOTE sections 4.3 and 4.4.
// The game inventory already bounds this output below ZIP64 limits.
void zip_u16(std::ostream& out,std::uint16_t value) {
    out.put(static_cast<char>(value&0xff));
    out.put(static_cast<char>(value>>8));
}
void zip_u32(std::ostream& out,std::uint32_t value) {
    zip_u16(out,static_cast<std::uint16_t>(value));
    zip_u16(out,static_cast<std::uint16_t>(value>>16));
}
std::uint32_t zip_crc(std::string_view bytes) {
    static constexpr auto table=[] {
        std::array<std::uint32_t,256> values{};
        for(std::uint32_t i=0;i<values.size();++i) {
            auto crc=i;
            for(int bit=0;bit<8;++bit) crc=(crc>>1)^((crc&1) ? 0xedb88320U : 0);
            values[i]=crc;
        }
        return values;
    }();
    std::uint32_t crc=0xffffffffU;
    for(char byte:bytes)crc=(crc>>8)^table[(crc^static_cast<unsigned char>(byte))&0xff];
    return crc^0xffffffffU;
}
std::uint32_t zip_position(std::ostream& out) {
    const auto position=out.tellp();
    require(position>=0 && position<0xffffffffLL,"ZIP write failed or exceeded its size limit.");
    return static_cast<std::uint32_t>(position);
}

struct ZipMember {
    std::string name;
    std::uint32_t offset,size,crc,permissions;

    void header(std::ostream& out) const {
        zip_u16(out,10); // Version needed to extract a stored member.
        zip_u16(out,0); // Portable paths are ASCII. No encryption or trailing descriptor.
        zip_u16(out,0); // Store bytes as supplied.
        zip_u16(out,0);
        zip_u16(out,0x21); // 1980-01-01 at midnight, independent of the source mtime.
        zip_u32(out,crc);
        zip_u32(out,size);
        zip_u32(out,size);
        zip_u16(out,static_cast<std::uint16_t>(name.size()));
        zip_u16(out,0); // No extra fields.
    }
};

std::uint32_t write_game_zip(const VerifiedGame& game,const fs::path& output) {
    const auto root=game.manifest.parent_path();
    std::ofstream out(project_io_path(output),std::ios::binary);
    require(bool(out),"Cannot create a staged game ZIP.");
    std::vector<ZipMember> members;
    const auto add=[&](const std::string& name,std::string_view digest,std::uint64_t size_limit,
                       std::optional<std::uint64_t> size={}) {
        const auto source=contained(root,name);
        const auto bytes=read(source,size_limit);
        require((!size || bytes.size()==*size) && hash(bytes)==digest,"Game file changed while saving ZIP: "+name);
        const bool executable=game.definition.target_os=="Linux" && (name=="launch.sh" ||
            (fs::status(project_io_path(source)).permissions()&fs::perms::owner_exec)!=fs::perms::none);
        ZipMember member{name,zip_position(out),static_cast<std::uint32_t>(bytes.size()),zip_crc(bytes),
            executable ? 0100755U : 0100644U};
        zip_u32(out,0x04034b50U);
        member.header(out);
        out.write(name.data(),static_cast<std::streamsize>(name.size()));
        out.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));
        require(bool(out),"Game ZIP write failed.");
        members.push_back(std::move(member));
    };
    add(game_manifest_filename,game.definition.manifest_sha256,4*1024*1024);
    for(const auto& entry:game.spec.at("files")) {
        const auto size=integer(entry.at("size"),file_limit);
        add(entry.at("path").get<std::string>(),entry.at("sha256").get_ref<const std::string&>(),size,size);
    }
    const auto directory_offset=zip_position(out);
    for(const auto& member:members) {
        zip_u32(out,0x02014b50U);
        zip_u16(out,0x0314); // Unix mode bits in the central directory.
        member.header(out);
        zip_u16(out,0); // Comment length.
        zip_u16(out,0); // Starting disk.
        zip_u16(out,0); // Internal attributes.
        zip_u32(out,member.permissions<<16);
        zip_u32(out,member.offset);
        out.write(member.name.data(),static_cast<std::streamsize>(member.name.size()));
    }
    const auto directory_bytes=zip_position(out)-directory_offset;
    const auto count=static_cast<std::uint16_t>(members.size());
    zip_u32(out,0x06054b50U);
    zip_u16(out,0); // Single disk.
    zip_u16(out,0);
    zip_u16(out,count);
    zip_u16(out,count);
    zip_u32(out,directory_bytes);
    zip_u32(out,directory_offset);
    zip_u16(out,0); // No archive comment.
    const auto archive_bytes=zip_position(out);
    out.close();
    require(bool(out),"Cannot close the staged game ZIP.");
    return archive_bytes;
}
}

Reply share_project_build(const std::string& checkpoint,const std::string& expected_bundle,const std::string& output) {
    return operation("project.share",[&] {
        require(hexadecimal(expected_bundle,64),"Supply the verified build's lowercase SHA-256.");
        require(folded(text(path_of(output).extension()))==".zip","Choose a new .zip output file.");
        const auto destination=destination_path(output);
        const auto source=verified_checkpoint(path_of(checkpoint));
        require(source.game.definition.manifest_sha256==expected_bundle,"The checkpoint belongs to a different game build.");
        require(!within(destination,source.root),"Save the game ZIP outside the verified checkpoint.");
        Stage stage(text(destination));
        const auto staged=stage.path/"game.zip";
        const auto bytes=write_game_zip(source.game,staged);
        require(fs::file_size(project_io_path(staged))==bytes,"The staged game ZIP changed before publication.");
        adoption_flush(staged);
        adoption_publish_new(staged,stage.destination);
        return Json{{"archive",text(stage.destination)},{"bytes",bytes},{"files",source.game.spec.at("files").size()+1},
            {"project_id",source.game.definition.project_id},{"revision",source.game.definition.revision},
            {"bundle_sha256",expected_bundle}};
    });
}
