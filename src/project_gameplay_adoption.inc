// SPDX-License-Identifier: Apache-2.0
// Included inside project.cpp's private namespace. This is durable publication,
// not module execution, world editing, or live gameplay activation.
struct AdoptionError : std::runtime_error {
    int code;
    AdoptionError(int value,const std::string& message):std::runtime_error(message),code(value) {}
};
void adoption_require(bool condition,const std::string& message,int code=-32091) {
    if(!condition)throw AdoptionError(code,message);
}
constexpr std::size_t adoption_record_limit=128,adoption_journal_limit=1024*1024;
constexpr std::uint64_t adoption_inventory_limit=total_limit+1024*1024;
std::string adoption_digest(const Json& value) {
    adoption_require(value.is_string() && hexadecimal(value.get<std::string>(),64),"Adoption hashes require 64 lowercase hexadecimal characters.",-32602);
    return value.get<std::string>();
}
void adoption_safe_path(const fs::path& path,bool missing=false) {
    const auto absolute=fs::absolute(path).lexically_normal();auto cursor=absolute.root_path();
    for(const auto& part:absolute.relative_path()) {
        cursor/=part;const auto status=fs::symlink_status(project_io_path(cursor));
        if(status.type()==fs::file_type::not_found) {adoption_require(missing,"Adoption input path is missing.");return;}
        adoption_require(!fs::is_symlink(status),"Adoption paths cannot contain symbolic links.");
#ifdef _WIN32
        const auto attributes=GetFileAttributesW(project_io_path(cursor).c_str());
        adoption_require(attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"Adoption paths cannot contain reparse points.");
#endif
    }
    if(fs::is_regular_file(fs::symlink_status(project_io_path(absolute))))
        adoption_require(fs::hard_link_count(project_io_path(absolute))==1,"Adoption inputs and retained files cannot be hard links.");
}
void adoption_case_name(const fs::path& path) {
    std::size_t entries=0;
    for(const auto& entry:fs::directory_iterator(project_io_path(path.parent_path()))) {
        adoption_require(++entries<=8192,"Adoption parent directory exceeds entry budget.");
        if(folded(text(entry.path().filename()))==folded(text(path.filename())))
            adoption_require(entry.path().filename()==path.filename(),"Case-insensitive adoption path collision.");
    }
}
void adoption_directory(const fs::path& path) {
    adoption_safe_path(path,true);adoption_case_name(path);
    if(fs::symlink_status(project_io_path(path)).type()==fs::file_type::not_found)
        adoption_require(fs::create_directory(project_io_path(path)),"Cannot reserve adoption directory.");
    adoption_require(fs::is_directory(fs::symlink_status(project_io_path(path))),"Adoption directory is not a regular directory.");
}
Json adoption_inventory(const NativeGameplayArtifact& artifact) {
    Json result=Json::array();const auto descriptor_bytes=read(path_of(artifact.descriptor),1024*1024);
    result.push_back({{"path","native-gameplay.json"},{"size",descriptor_bytes.size()},{"sha256",hash(descriptor_bytes)},{"role","descriptor"}});
    for(const auto& file:artifact.files) {
        adoption_require(folded(file.path)!="native-gameplay.json","Artifact payload overlaps the adopted descriptor.");
        adoption_safe_path(contained(path_of(artifact.root),file.path));
        result.push_back({{"path",file.path},{"size",file.size},{"sha256",file.sha256},{"role",file.role}});
    }
    std::sort(result.begin(),result.end(),[](const Json& a,const Json& b){return a.at("path").get<std::string>()<b.at("path").get<std::string>();});
    return result;
}
std::uint64_t adoption_inventory_validate(const Json& inventory) {
    adoption_require(inventory.is_array() && !inventory.empty() && inventory.size()<=257,"Adoption artifact inventory exceeds 257 files.");
    std::set<std::string> names;std::uint64_t total=0;unsigned descriptors=0;std::string previous;
    for(const auto& file:inventory) {
        fields(file,{"path","size","sha256","role"},{"path","size","sha256","role"});
        const auto name=relative_path(file.at("path"));adoption_require(previous.empty() || previous<name,"Adoption inventory must be uniquely sorted.");previous=name;
        adoption_require(names.insert(folded(name)).second,"Case-insensitive adoption inventory collision.");
        const auto size=integer(file.at("size"),file_limit);adoption_require(size<=adoption_inventory_limit-total,"Adoption artifact exceeds 1 GiB payload plus its descriptor.");total+=size;
        adoption_digest(file.at("sha256"));adoption_require(file.at("role").is_string(),"Adoption inventory role must be text.");
        if(name=="native-gameplay.json") {++descriptors;adoption_require(file.at("role")=="descriptor" && size<=1024*1024,"Adoption descriptor inventory differs.");}
        else adoption_require(file.at("role")=="library" || file.at("role")=="dependency" || file.at("role")=="metadata" || file.at("role")=="notice","Unsupported adoption payload role.");
    }
    adoption_require(descriptors==1,"Adoption inventory must contain its descriptor.");return total;
}
void adoption_target(const NativeGameplayArtifact& artifact) {
    adoption_require(artifact.target_os==build_metadata().target_os && artifact.target_arch==build_metadata().target_arch,"Adoption artifact target differs from this native host.");
    const auto error=gameplay_abi::compatibility_error(artifact.requirements,gameplay_abi::available_contract());
    adoption_require(error.empty(),error);
}
void adoption_values(const std::string& schema,const Json& values,bool explicit_values) {
    try {(void)validate_gameplay_values(schema,values.dump());}
    catch(const std::exception& error) {throw AdoptionError(explicit_values ? -32602:-32091,error.what());}
}
std::string adoption_relative(const std::string& request_id) {return "gameplay/adopt-"+request_id+"/native-gameplay.json";}
fs::path adoption_staging(const fs::path& root,const std::string& request_id) {
    return root/".poima-gameplay-adoption-staging"/request_id;
}
void adoption_flush(const fs::path& path) {
    adoption_safe_path(path);regular(path);
#ifdef _WIN32
    const auto file=CreateFileW(project_io_path(path).c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    adoption_require(file!=INVALID_HANDLE_VALUE,"Cannot reopen owned adoption stage for flushing.");
    const bool flushed=FlushFileBuffers(file)!=0;const bool closed=CloseHandle(file)!=0;
#else
    const auto file=::open(path.c_str(),O_RDONLY|O_CLOEXEC);adoption_require(file>=0,"Cannot reopen owned adoption stage for flushing.");
    const bool flushed=fsync(file)==0;const bool closed=::close(file)==0;
#endif
    adoption_require(flushed && closed,"Cannot flush owned adoption stage.");
}
void adoption_publish_new(const fs::path& source,const fs::path& destination) {
    adoption_safe_path(source);adoption_safe_path(destination,true);adoption_case_name(destination);
#ifdef _WIN32
    adoption_require(MoveFileExW(project_io_path(source).c_str(),project_io_path(destination).c_str(),MOVEFILE_WRITE_THROUGH)!=0,"Cannot atomically publish adoption payload without replacement.");
#else
    adoption_require(syscall(SYS_renameat2,AT_FDCWD,source.c_str(),AT_FDCWD,destination.c_str(),RENAME_NOREPLACE)==0,
        std::string("Atomic no-replacement adoption publication failed or is unsupported by this filesystem: ")+std::strerror(errno));
#endif
}
void adoption_replace_file(const fs::path& source,const fs::path& destination) {
#ifdef _WIN32
    adoption_require(MoveFileExW(project_io_path(source).c_str(),project_io_path(destination).c_str(),
        MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0,"Cannot atomically replace adoption metadata.");
#else
    world_detail::replace_file(source,destination);
#endif
}
void adoption_quarantine(const fs::path& root,const std::string& request_id,const fs::path& torn,std::uint64_t maximum) {
    adoption_safe_path(torn);regular(torn);adoption_require(fs::file_size(project_io_path(torn))<=maximum,"Owned torn stage exceeds its declared size budget.");
    const auto stages=adoption_staging(root,request_id);adoption_directory(stages.parent_path());adoption_directory(stages);const auto retained=stages/"retained";adoption_directory(retained);
    std::size_t count=0;std::uint64_t bytes=0;
    for(const auto& entry:fs::directory_iterator(project_io_path(retained))) {
        const auto retained_path=retained/entry.path().filename();adoption_safe_path(retained_path);regular(retained_path);const auto name=text(entry.path().filename());
        adoption_require(name.size()==41 && name.starts_with("torn-") && name.ends_with(".bin") && hexadecimal(name.substr(5,32),32),"Unexpected retained adoption stage.");
        adoption_require(++count<=8,"Retained adoption diagnostics exceed 8 files.");const auto size=fs::file_size(project_io_path(retained_path));adoption_require(size<=total_limit-bytes,"Retained adoption diagnostics exceed 1 GiB.");bytes+=size;
    }
    adoption_require(count<8 && fs::file_size(project_io_path(torn))<=total_limit-bytes,"Retained adoption diagnostics capacity reached.");
    adoption_publish_new(torn,retained/("torn-"+new_id()+".bin"));
}
void adoption_stage_bytes(const fs::path& root,const std::string& request_id,const fs::path& stage,const std::string& bytes,std::uint64_t maximum) {
    adoption_safe_path(stage,true);adoption_case_name(stage);
    if(fs::symlink_status(project_io_path(stage)).type()!=fs::file_type::not_found) {
        if(read(stage,maximum)==bytes) {adoption_flush(stage);return;}
        adoption_quarantine(root,request_id,stage,maximum);
    }
    exclusive_write(stage,bytes);adoption_require(read(stage,maximum)==bytes,"Owned adoption stage did not verify.");
}
Json adoption_read_journal_unchecked(const fs::path& path) {
    adoption_safe_path(path);const auto envelope=parse(read(path,adoption_journal_limit));
    fields(envelope,{"format","version","sha256","payload"},{"format","version","sha256","payload"});
    adoption_require(envelope.at("format")=="poima.project-gameplay-adoption" && integer(envelope.at("version"))==1,"Unsupported adoption journal format.");
    adoption_require(envelope.at("sha256")==hash(envelope.at("payload").dump()),"Adoption journal checksum differs.");
    const auto payload=envelope.at("payload");
    fields(payload,{"request","state","before_manifest","before_project_sha256","after_manifest","after_project_sha256","world_sha256","input_profile_sha256","artifact_inventory","result"},
        {"request","state","before_manifest","before_project_sha256","after_manifest","after_project_sha256","world_sha256","input_profile_sha256","artifact_inventory","result"});
    adoption_require(payload.at("state")=="prepared" || payload.at("state")=="succeeded","Invalid adoption journal state.");
    for(const auto* key:{"before_manifest","after_manifest"})adoption_require(payload.at(key).is_string() && payload.at(key).get_ref<const std::string&>().size()<=65536,"Adoption manifest record exceeds 64 KiB.");
    adoption_require(payload.at("before_project_sha256")==hash(payload.at("before_manifest").get<std::string>()) && payload.at("after_project_sha256")==hash(payload.at("after_manifest").get<std::string>()),"Adoption journal manifest hashes differ.");
    adoption_digest(payload.at("world_sha256"));if(!payload.at("input_profile_sha256").is_null())adoption_digest(payload.at("input_profile_sha256"));
    const auto total=adoption_inventory_validate(payload.at("artifact_inventory"));
    const auto& request=payload.at("request");
    fields(request,{"request_id","expected_project_sha256","expected_artifact_sha256","values","artifact_source","project_manifest"},
        {"request_id","expected_project_sha256","expected_artifact_sha256","artifact_source","project_manifest"});
    const auto request_id=id(request.at("request_id"));adoption_digest(request.at("expected_project_sha256"));adoption_digest(request.at("expected_artifact_sha256"));
    adoption_require(request.at("artifact_source").is_string() && path_of(request.at("artifact_source").get<std::string>()).is_absolute() && request.at("project_manifest").is_string(),"Invalid adoption journal source/manifest identity.");
    const auto before=parse(payload.at("before_manifest").get<std::string>());const auto& result=payload.at("result");
    fields(result,{"request_id","project_id","previous_manifest_sha256","manifest_sha256","world_sha256","descriptor","artifact_descriptor_sha256","library_sha256","target_os","target_arch","values","file_count","payload_bytes"},
        {"request_id","project_id","previous_manifest_sha256","manifest_sha256","world_sha256","descriptor","artifact_descriptor_sha256","library_sha256","target_os","target_arch","values","file_count","payload_bytes"});
    const auto values=request.contains("values") ? request.at("values") : before.contains("gameplay") ? before.at("gameplay").value("values",Json::object()) : Json::object();
    adoption_require(values.is_object() && result.at("values").dump()==values.dump(),"Adoption journal values differ from the exact request/defaults.");
    const auto relative=adoption_relative(request_id);auto expected=before;expected["version"]=2;expected["gameplay"]={{"descriptor",relative},{"values",values}};
    adoption_require(expected.dump(2)+"\n"==payload.at("after_manifest").get<std::string>(),"Adoption journal changes unrelated manifest fields.");
    adoption_require(request.at("expected_project_sha256")==payload.at("before_project_sha256") && result.at("request_id")==request_id && result.at("project_id")==before.at("project_id") &&
        result.at("previous_manifest_sha256")==payload.at("before_project_sha256") && result.at("manifest_sha256")==payload.at("after_project_sha256") && result.at("world_sha256")==payload.at("world_sha256") &&
        result.at("descriptor")==relative && result.at("artifact_descriptor_sha256")==request.at("expected_artifact_sha256") && integer(result.at("file_count"),257)==payload.at("artifact_inventory").size() && integer(result.at("payload_bytes"),adoption_inventory_limit)==total,
        "Adoption journal response identities differ.");
    unsigned libraries=0;
    for(const auto& file:payload.at("artifact_inventory")) {
        if(file.at("role")=="descriptor")adoption_require(file.at("sha256")==request.at("expected_artifact_sha256"),"Adoption descriptor pin differs.");
        if(file.at("role")=="library") {++libraries;adoption_require(file.at("sha256")==result.at("library_sha256"),"Adoption library pin differs.");}
    }
    adoption_require(libraries==1 && (result.at("target_os")=="Windows" || result.at("target_os")=="Linux") && result.at("target_arch")=="x86_64","Adoption journal target/library differs.");return payload;
}
Json adoption_read_journal(const fs::path& path) {
    try {return adoption_read_journal_unchecked(path);}
    catch(const std::exception& error) {throw AdoptionError(-32091,error.what());}
}
void adoption_write_journal(const fs::path& path,const Json& payload) {
    const auto bytes=Json{{"format","poima.project-gameplay-adoption"},{"version",1},{"sha256",hash(payload.dump())},{"payload",payload}}.dump(2)+"\n";
    adoption_require(bytes.size()<=adoption_journal_limit,"Adoption journal exceeds 1 MiB.");
    const auto stage=fs::path(path).concat(".pending");adoption_safe_path(stage,true);adoption_case_name(stage);
    if(fs::symlink_status(project_io_path(stage)).type()!=fs::file_type::not_found) {
        // A flushed first prepare can survive before its final name. Terminal
        // staging can likewise survive before replacing the prepared journal.
        if(read(stage,adoption_journal_limit)!=bytes) {
            adoption_require(fs::is_regular_file(fs::symlink_status(project_io_path(path))),"Invalid first preparation stage lacks canonical recovery authority.");
            auto authority=adoption_read_journal(path);auto comparable=payload;comparable["state"]=authority.at("state");
            adoption_require(authority.at("state")=="prepared" && comparable.dump()==authority.dump(),"Journal staging differs from its canonical prepared authority.");
            adoption_quarantine(path.parent_path().parent_path(),payload.at("request").at("request_id"),stage,adoption_journal_limit);exclusive_write(stage,bytes);
        }
    }else exclusive_write(stage,bytes);
    adoption_flush(stage);adoption_replace_file(stage,path);
}
void adoption_baseline(const Project& project,const Json& journal) {
    adoption_require(hash(project.world_bytes)==journal.at("world_sha256").get<std::string>(),"Entry world changed during prepared adoption.",-32009);
    const auto profile=project.profile.empty() ? Json(nullptr) : Json(hash(project.profile_bytes));
    adoption_require(profile==journal.at("input_profile_sha256"),"Input profile changed during prepared adoption.",-32009);
}
NativeGameplayArtifact adoption_complete(const fs::path& root,const Json& journal) {
    const auto relative=adoption_relative(journal.at("request").at("request_id").get<std::string>());
    const auto descriptor=contained(root,relative);adoption_safe_path(descriptor);
    auto artifact=load_native_gameplay_artifact(text(descriptor));adoption_target(artifact);
    adoption_require(artifact.descriptor_sha256==journal.at("request").at("expected_artifact_sha256").get<std::string>() && adoption_inventory(artifact)==journal.at("artifact_inventory") &&
        artifact.library_sha256==journal.at("result").at("library_sha256").get<std::string>() && artifact.target_os==journal.at("result").at("target_os").get<std::string>() && artifact.target_arch==journal.at("result").at("target_arch").get<std::string>(),"Retained adoption inventory/response differs.");
    (void)validate_gameplay_values(artifact.schema,journal.at("result").at("values").dump());
    return artifact;
}
void adoption_copy(const fs::path& root,const fs::path& source,const Json& journal) {
    const auto request_id=journal.at("request").at("request_id").get<std::string>();const auto destination=root/fs::path(adoption_relative(request_id)).parent_path();
    adoption_directory(root/"gameplay");adoption_directory(destination);
    std::map<std::string,Json> expected;for(const auto& file:journal.at("artifact_inventory"))expected.emplace(file.at("path"),file);
    for(const auto& name:file_tree(destination)) {
        adoption_require(expected.contains(name),"Retained incomplete adoption contains an unlisted file.");
        const auto& file=expected.at(name);const auto path=contained(destination,name);adoption_safe_path(path);const auto bytes=read(path);
        adoption_require(bytes.size()==file.at("size") && hash(bytes)==file.at("sha256").get<std::string>(),"Retained adoption payload was modified; no overwrite.");
    }
    std::optional<NativeGameplayArtifact> original;
    for(const auto& file:journal.at("artifact_inventory")) {
        const auto name=file.at("path").get<std::string>();auto target=destination/fs::path(name);adoption_safe_path(target,true);
        if(fs::symlink_status(project_io_path(target)).type()!=fs::file_type::not_found)continue;
        if(!original) {
            adoption_safe_path(source);original=load_native_gameplay_artifact(text(source));adoption_target(*original);
            adoption_require(original->descriptor_sha256==journal.at("request").at("expected_artifact_sha256").get<std::string>() && adoption_inventory(*original)==journal.at("artifact_inventory"),"Original artifact changed during prepared adoption.");
        }
        const auto incoming=name=="native-gameplay.json" ? source : contained(path_of(original->root),name);
        adoption_safe_path(incoming);const auto bytes=read(incoming,name=="native-gameplay.json" ? 1024*1024:file_limit);
        adoption_require(bytes.size()==file.at("size") && hash(bytes)==file.at("sha256").get<std::string>(),"Artifact source bytes changed during copy.");
        auto parent=destination;for(const auto& part:fs::path(name).parent_path()) {parent/=part;adoption_directory(parent);}
        const auto stages=adoption_staging(root,request_id);adoption_directory(stages.parent_path());adoption_directory(stages);
        const auto stage=stages/("payload-"+hash(name)+".pending");adoption_stage_bytes(root,request_id,stage,bytes,file.at("size").get<std::uint64_t>());
        adoption_publish_new(stage,target);fs::permissions(project_io_path(target),fs::status(project_io_path(incoming)).permissions()&fs::perms::all,fs::perm_options::replace);
        adoption_require(read(target)==bytes,"Copied adoption payload failed verification.");
    }
    if(original) {
        const auto current=load_native_gameplay_artifact(text(source));
        adoption_require(current.descriptor_sha256==original->descriptor_sha256 && adoption_inventory(current)==journal.at("artifact_inventory"),"Artifact source changed during adoption publication.");
    }
    (void)adoption_complete(root,journal);
}
Json adoption_response(const Project& current,const Json& journal,bool replayed) {
    adoption_require(current.spec.at("project_id")==journal.at("result").at("project_id"),"Current project identity differs from the adoption receipt.",-32009);
    const auto copied=adoption_complete(current.root,journal);auto result=journal.at("result");
    result["replayed"]=replayed;result["current_manifest_sha256"]=hash(current.manifest_bytes);
    result["attached"]=current.gameplay && within(path_of(current.gameplay->descriptor),path_of(copied.descriptor)) && within(path_of(copied.descriptor),path_of(current.gameplay->descriptor));return result;
}
Json adoption_apply(const std::string& manifest_name,const std::string& artifact_name,const std::string& request_json) {
    Json request;
    try {
        adoption_require(request_json.size()<=65536,"Adoption request exceeds 64 KiB.",-32602);request=parse(request_json);
        fields(request,{"request_id","expected_project_sha256","expected_artifact_sha256","values"},{"request_id","expected_project_sha256","expected_artifact_sha256"});
        id(request.at("request_id"));adoption_digest(request.at("expected_project_sha256"));adoption_digest(request.at("expected_artifact_sha256"));
        adoption_require(!request.contains("values") || request.at("values").is_object(),"Adoption values must be an object.",-32602);
    }catch(const std::exception& error) {throw AdoptionError(-32602,error.what());}
    const auto manifest=fs::absolute(path_of(manifest_name)).lexically_normal();adoption_safe_path(manifest);regular(manifest);const auto root=fs::canonical(manifest.parent_path());
    const auto source=fs::absolute(path_of(artifact_name)).lexically_normal();request["artifact_source"]=text(source);request["project_manifest"]=text(manifest.filename());
    const auto request_id=request.at("request_id").get<std::string>();const auto records=root/".poima-gameplay-adoptions",journal_file=records/(request_id+".json");
    const auto journal_stage=fs::path(journal_file).concat(".pending");adoption_safe_path(records,true);adoption_safe_path(journal_file,true);adoption_safe_path(journal_stage,true);
    const bool prior=fs::is_regular_file(fs::symlink_status(project_io_path(journal_file))) || fs::is_regular_file(fs::symlink_status(project_io_path(journal_stage)));
    // Fail stale guards and invalid source artifacts before creating any
    // coordination sidecar. The same guard is checked again under the lock.
    if(!prior) {
        adoption_require(hash(read(manifest,65536))==request.at("expected_project_sha256").get<std::string>(),"Project manifest hash conflict.",-32009);
        const auto current=project(text(manifest));adoption_safe_path(source);
        adoption_require(hash(read(source,1024*1024))==request.at("expected_artifact_sha256").get<std::string>(),"Artifact descriptor hash conflict.",-32009);
        const auto artifact=load_native_gameplay_artifact(text(source));adoption_target(artifact);
        adoption_require(artifact.descriptor_sha256==request.at("expected_artifact_sha256").get<std::string>(),"Artifact descriptor hash conflict.",-32009);
        component_bindings_validate(current.content.document,artifact.schema);
        const auto values=request.contains("values") ? request.at("values") : current.spec.contains("gameplay") ? current.spec.at("gameplay").value("values",Json::object()) : Json::object();
        adoption_values(artifact.schema,values,request.contains("values"));
    }
    const auto lock_path=root/".poima-gameplay-adoptions.lock";adoption_safe_path(lock_path,true);adoption_case_name(lock_path);
    std::unique_ptr<world_detail::WriterLock> lock;
    try {lock=std::make_unique<world_detail::WriterLock>(lock_path);}catch(const std::exception& error) {throw AdoptionError(-32009,std::string("Project adoption writer unavailable: ")+error.what());}
    adoption_directory(records);std::size_t entries=0;std::set<std::string> canonical_ids,staged_ids;
    for(const auto& item:fs::directory_iterator(project_io_path(records))) {
        adoption_require(++entries<=2*adoption_record_limit,"Project adoption receipt directory exceeds entry budget.");const auto filename=text(item.path().filename());
        const bool staged=filename.size()==45 && filename.ends_with(".json.pending");
        adoption_require((staged || (filename.size()==37 && filename.ends_with(".json"))) && hexadecimal(filename.substr(0,32),32),"Unexpected adoption journal file.");
        adoption_safe_path(records/item.path().filename());const auto record_id=filename.substr(0,32);
        auto& table=staged ? staged_ids:canonical_ids;adoption_require(table.insert(record_id).second,"Duplicate adoption journal name.");
    }
    std::set<std::string> ids;
    ids.insert(canonical_ids.begin(),canonical_ids.end());ids.insert(staged_ids.begin(),staged_ids.end());
    const auto count=ids.size();adoption_require(count<=adoption_record_limit,"Retained adoption record budget exceeded.");
    const bool existing=ids.contains(request_id);
    Json journal;
    if(existing) {
        journal=adoption_read_journal(canonical_ids.contains(request_id) ? journal_file:journal_stage);
        adoption_require(journal.at("request").at("request_id")==request_id,"Adoption journal filename/request differs.");
        adoption_require(journal.at("request").dump()==request.dump(),"Adoption request ID was reused with different parameters.",-32009);
        if(staged_ids.contains(request_id) && canonical_ids.contains(request_id)) {
            std::optional<Json> staged;
            try {staged=adoption_read_journal(journal_stage);}catch(const std::exception&) {/* Canonical authority can retain/rebuild a torn stage. */}
            if(staged) {
                auto comparable=*staged;comparable["state"]=journal.at("state");
                adoption_require(comparable.dump()==journal.dump(),"Conflicting valid staged adoption journal retained.");
                if(staged->at("state")=="succeeded") {
                    const auto response=adoption_response(project(text(manifest)),*staged,true);
                    adoption_require(adoption_read_journal(journal_stage).dump()==staged->dump(),"Terminal adoption staging changed during verification.");
                    adoption_flush(journal_stage);adoption_replace_file(journal_stage,journal_file);return response;
                }
            }
            if(journal.at("state")=="succeeded") {
                const auto response=adoption_response(project(text(manifest)),journal,true);
                adoption_quarantine(root,request_id,journal_stage,adoption_journal_limit);return response;
            }
        }else if(staged_ids.contains(request_id))adoption_require(journal.at("state")=="prepared","Terminal adoption staging lacks its prepared record.");
        if(journal.at("state")=="succeeded")return adoption_response(project(text(manifest)),journal,true);
    }else {
        adoption_require(count<adoption_record_limit,"Project adoption receipt capacity reached.");const auto current=project(text(manifest));
        adoption_require(hash(current.manifest_bytes)==request.at("expected_project_sha256").get<std::string>(),"Project manifest hash conflict.",-32009);
        adoption_safe_path(source);adoption_require(hash(read(source,1024*1024))==request.at("expected_artifact_sha256").get<std::string>(),"Artifact descriptor hash conflict.",-32009);
        const auto artifact=load_native_gameplay_artifact(text(source));adoption_target(artifact);
        adoption_require(artifact.descriptor_sha256==request.at("expected_artifact_sha256").get<std::string>(),"Artifact descriptor hash conflict.",-32009);
        const auto relative=adoption_relative(request_id);const auto destination=root/fs::path(relative).parent_path();adoption_safe_path(destination,true);
        adoption_require(fs::symlink_status(project_io_path(destination)).type()==fs::file_type::not_found,"Adoption artifact destination already exists.",-32009);
        const auto stages=adoption_staging(root,request_id),manifest_stage=root/(".poima-adopt-"+request_id+".pending");adoption_safe_path(stages,true);adoption_safe_path(manifest_stage,true);
        adoption_require(fs::symlink_status(project_io_path(stages)).type()==fs::file_type::not_found && fs::symlink_status(project_io_path(manifest_stage)).type()==fs::file_type::not_found,"Unowned adoption staging already exists.",-32009);
        component_bindings_validate(current.content.document,artifact.schema);const auto values=request.contains("values") ? request.at("values") : current.spec.contains("gameplay") ? current.spec.at("gameplay").value("values",Json::object()) : Json::object();
        adoption_values(artifact.schema,values,request.contains("values"));auto next=current.spec;next["version"]=2;next["gameplay"]={{"descriptor",relative},{"values",values}};const auto after=next.dump(2)+"\n";adoption_require(after.size()<=65536,"Adopted project manifest exceeds 64 KiB.");
        const auto inventory=adoption_inventory(artifact);const auto bytes=adoption_inventory_validate(inventory);
        Json result={{"request_id",request_id},{"project_id",current.spec.at("project_id")},{"previous_manifest_sha256",hash(current.manifest_bytes)},{"manifest_sha256",hash(after)},{"world_sha256",hash(current.world_bytes)},
            {"descriptor",relative},{"artifact_descriptor_sha256",artifact.descriptor_sha256},{"library_sha256",artifact.library_sha256},{"target_os",artifact.target_os},{"target_arch",artifact.target_arch},{"values",values},{"file_count",inventory.size()},{"payload_bytes",bytes}};
        journal={{"request",request},{"state","prepared"},{"before_manifest",current.manifest_bytes},{"before_project_sha256",hash(current.manifest_bytes)},{"after_manifest",after},{"after_project_sha256",hash(after)},
            {"world_sha256",hash(current.world_bytes)},{"input_profile_sha256",current.profile.empty() ? Json(nullptr) : Json(hash(current.profile_bytes))},{"artifact_inventory",inventory},{"result",result}};
        adoption_require(read(current.manifest,65536)==current.manifest_bytes && read(current.world,16*1024*1024)==current.world_bytes &&
            (current.profile.empty() || read(current.profile,16*1024*1024)==current.profile_bytes),"Project inputs changed before adoption preparation.",-32009);
        adoption_write_journal(journal_file,journal);
    }
    const auto before=journal.at("before_project_sha256").get<std::string>(),after=journal.at("after_project_sha256").get<std::string>(),actual=hash(read(manifest,65536));
    adoption_require(actual==before || actual==after,"Current project differs from the prepared adoption boundary.",-32009);
    auto current=project(text(manifest));adoption_require(hash(current.manifest_bytes)==actual,"Project changed while reading the prepared boundary.",-32009);adoption_baseline(current,journal);
    if(existing && !canonical_ids.contains(request_id))adoption_write_journal(journal_file,journal);
    adoption_copy(root,source,journal);
    const auto prospective=parse(journal.at("after_manifest").get<std::string>());auto validated=project(text(manifest),prospective);adoption_baseline(validated,journal);
    adoption_require(hash(read(manifest,65536))==actual,"Project changed before adoption publication.",-32009);
    if(actual==before) {
        const auto stage=root/(".poima-adopt-"+request_id+".pending");adoption_safe_path(stage,true);adoption_case_name(stage);const auto bytes=journal.at("after_manifest").get<std::string>();
        adoption_require(stage!=current.manifest && stage!=current.world && (current.profile.empty() || stage!=current.profile),"Owned manifest staging overlaps a project input.");
        adoption_stage_bytes(root,request_id,stage,bytes,65536);
        adoption_require(hash(read(manifest,65536))==before,"Project changed before atomic adoption replacement.",-32009);adoption_baseline(project(text(manifest)),journal);
        adoption_replace_file(stage,manifest);
    }
    current=project(text(manifest));adoption_require(hash(current.manifest_bytes)==after,"Adopted project manifest does not match the prepared receipt.");adoption_baseline(current,journal);(void)adoption_complete(root,journal);
    journal["state"]="succeeded";adoption_write_journal(journal_file,journal);return adoption_response(current,journal,existing);
}
