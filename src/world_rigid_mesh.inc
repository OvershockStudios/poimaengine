// SPDX-License-Identifier: Apache-2.0
// Included in World.
    Json extract_rigid_mesh(const Json& params) const {
        fields(params,{"asset","node","joint","primitives"},{"asset","node","joint","primitives"});
        require(params.at("asset").is_string() && valid_asset_id(params.at("asset").get<std::string>()),"Invalid model asset ID.");
        const auto node=revision(params.at("node")),joint=revision(params.at("joint"));
        const auto& raw=params.at("primitives");
        require(raw.is_array() && !raw.empty() && raw.size()<=32,"Select 1..32 distinct source primitives.");
        std::set<std::uint64_t> primitives;
        for(const auto& index:raw)require(primitives.insert(revision(index)).second,"Duplicate source primitive.");
        try {
            const auto source=read_model_asset(asset_directory(),params.at("asset"));
            const auto& model=*source.model;
            require(node<model.nodes.size() && joint<model.nodes.size(),"Source node or joint is outside the cooked model.");
            const auto& binding=model.nodes[node];
            require(binding.skin>=0,"Source mesh node needs a skin binding.");
            const auto& skin=model.skins.at(static_cast<std::size_t>(binding.skin));
            const auto palette_entry=std::find(skin.joints.begin(),skin.joints.end(),joint);
            require(palette_entry!=skin.joints.end(),"Selected joint is absent from the source node's skin palette.");
            const auto palette_index=static_cast<std::size_t>(palette_entry-skin.joints.begin());
            for(const auto index:primitives) {
                require(std::find(binding.primitives.begin(),binding.primitives.end(),index)!=binding.primitives.end(),
                    "Requested primitive is not bound to the source mesh node.");
                const auto& mesh=*model.primitives.at(index);
                require(mesh.influences.size()==mesh.vertices.size() && !mesh.influences.empty(),"Primitive needs weighted geometry.");
                for(std::size_t vertex=0;vertex<mesh.influences.size();++vertex) {
                    const auto& influence=mesh.influences[vertex];
                    bool rigid=false;
                    for(std::size_t slot=0;slot<influence.weights.size();++slot)
                        if(influence.weights[slot]==1 && influence.joints[slot]==palette_index)rigid=true;
                    if(!rigid || std::count_if(influence.weights.begin(),influence.weights.end(),[](float weight){return weight!=0;})!=1)
                        throw Error(-32602,"Selected vertices must have one exact unit weight on the selected joint.",
                            {{"primitive",index},{"vertex",vertex},{"joint",joint},{"reason","non_rigid_skin_weights"}});
                }
            }
            ModelAsset extracted;
            extracted.nodes.resize(1);
            extracted.nodes.front().name=model.nodes[joint].name;
            extracted.roots={0};
            Json mapping=Json::array();
            for(const auto index:primitives) {
                // The source's draw world cancels the inverse mesh world in the
                // skin palette. At a unit-weight vertex the remaining product
                // is joint_world * inverse_bind * source_vertex.
                const auto mesh=deform_mesh(*model.primitives.at(index),skin.inverse_bind);
                for(const auto& map:mesh->textures)
                    if(map.image && std::find(extracted.images.begin(),extracted.images.end(),map.image)==extracted.images.end())
                        extracted.images.push_back(map.image);
                const auto output=static_cast<std::uint32_t>(extracted.primitives.size());
                extracted.nodes.front().primitives.push_back(output);
                extracted.primitives.push_back(mesh);
                mapping.push_back({{"source_primitive",index},{"primitive",output}});
            }
            const auto result=store_cooked_model(asset_directory(),extracted,"poima-rigid-mesh-extract-1");
            return {{"source_asset",source.id},{"source_node",node},{"joint",joint},{"asset",result.id},
                {"bytes",result.bytes},{"frame","joint_local"},{"primitives",std::move(mapping)}};
        }catch(const Error&) {throw;}
        catch(const std::exception& error) {throw Error(-32050,error.what());}
    }
