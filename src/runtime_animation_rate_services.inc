// SPDX-License-Identifier: Apache-2.0
// Included in Runtime::Impl. Speed-only writes stage until Tick publication;
// existing animation getters do not observe this callback's staged changes.
    static int32_t POIMA_CALL set_animation_rate(void* context,const PoimaGameAnimationRateCommandV1* source,
        PoimaGameError* error) {
        return callback(error,[&] {
            require(context&&source,"Animation rate command is absent.");
            require(source->version==1&&source->bytes>=sizeof(PoimaGameAnimationRateCommandV1),
                "Animation rate command requires version1 and at least40 bytes.");
            require(source->reserved==0&&source->layer<=4,"Invalid animation rate layer/reserved encoding.");
            require(std::isfinite(source->speed)&&source->speed>=0&&source->speed<=8,"Animation speed must be within0..8.");
            auto& self=*static_cast<Impl*>(context);self.require_tick();
            require(self.game_animation_rate_write_calls<64&&self.game_animation_commands.size()+self.game_animation_rate_commands.size()<64,
                "Gameplay exceeded64 combined animation writes in one Tick.");
            AnimationRateCommand command;command.entity=gameplay_id(source->entity);command.speed=source->speed;
            if(source->layer)command.layer=source->layer;
            if(const auto* pending=self.pending_entity(command.entity)) {
                require(pending->animation_rig.has_value(),"Reserved animation rate target needs an AnimationRig.");
                if(command.layer)require(std::any_of(pending->animation_rig->layers.begin(),pending->animation_rig->layers.end(),
                    [&](const auto& layer){return layer.slot==*command.layer;}),"Reserved animation rate layer is not configured.");
            }else {
                require(self.owner->animation(command.entity).has_value(),"Animation rate command requires an AnimationRig entity.");
                if(command.layer)require(self.owner->animation_layer(command.entity,*command.layer).has_value(),"Animation rate layer is not configured.");
            }
            require(std::none_of(self.game_animation_rate_commands.begin(),self.game_animation_rate_commands.end(),[&](const auto& old) {
                return old.entity==command.entity&&old.layer==command.layer;
            })&&std::none_of(self.game_animation_commands.begin(),self.game_animation_commands.end(),[&](const auto& old) {
                return old.entity==command.entity&&old.layer==command.layer;
            }),"Duplicate gameplay animation clock in one Tick.");
            self.game_animation_rate_commands.push_back(std::move(command));++self.game_animation_rate_write_calls;
        });
    }
