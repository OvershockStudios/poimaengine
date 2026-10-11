// SPDX-License-Identifier: Apache-2.0
// Included inside Runtime::Impl. Queries read committed native controls;
// complete replacements publish only after a successful Tick callback.
    static int32_t POIMA_CALL get_two_bone_ik(void* context,const PoimaEntityId* id,
        std::uint32_t slot,PoimaGameTwoBoneIkStateV1* output,PoimaGameError* error) {
        return callback(error,[&] {
            require(context && id && output,"IK state query is absent.");
            require(output->version==1 && output->bytes>=sizeof(PoimaGameTwoBoneIkStateV1) &&
                output->reserved==0 && output->reserved_tail==0,"IK state requires version 1, at least 160 bytes and zero reserved fields.");
            auto& self=*static_cast<Impl*>(context);self.require_tick();
            require(self.game_ik_read_calls<1024,"Gameplay exceeded 1024 IK reads in one Tick.");
            const auto state=self.owner->ik(gameplay_id(*id),slot);
            PoimaGameTwoBoneIkStateV1 candidate{};candidate.version=1;candidate.bytes=sizeof(candidate);
            if(state) {
                candidate.present=1;candidate.entity=*id;candidate.slot=state->slot;
                candidate.root=state->root;candidate.middle=state->middle;candidate.end=state->end;
                candidate.enabled=state->enabled;candidate.space=static_cast<std::uint32_t>(state->space);
                candidate.weight=state->weight;candidate.target_error=state->target_error;
                candidate.clamped=state->clamped;candidate.pole_fallback=state->pole_fallback;
                std::copy(state->target.begin(),state->target.end(),candidate.target);
                std::copy(state->pole.begin(),state->pole.end(),candidate.pole);
                std::copy(state->end_position.begin(),state->end_position.end(),candidate.end_position);
            }
            ++self.game_ik_read_calls;*output=candidate;
        });
    }
    static int32_t POIMA_CALL set_two_bone_ik(void* context,const PoimaGameTwoBoneIkCommandV1* source,
        PoimaGameError* error) {
        return callback(error,[&] {
            require(context && source,"IK command is absent.");
            require(source->version==1 && source->bytes>=sizeof(PoimaGameTwoBoneIkCommandV1) &&
                source->reserved==0,"IK command requires version 1, at least 96 bytes and zero reserved.");
            require(source->slot>=1 && source->slot<=8 && source->enabled<=1 && source->space<=1,
                "Invalid IK slot, space or enabled flag.");
            require(std::isfinite(source->weight) && source->weight>=0 && source->weight<=1,"IK weight must be 0..1.");
            for(double x:source->target)require(std::isfinite(x) && std::abs(x)<=1e6,"IK target must be finite within +/-1000 km.");
            for(double x:source->pole)require(std::isfinite(x) && std::abs(x)<=1e6,"IK pole must be finite within +/-1000 km.");
            auto& self=*static_cast<Impl*>(context);self.require_tick();
            require(self.game_ik_write_calls<64,"Gameplay exceeded 64 IK writes in one Tick.");
            RuntimeIkCommand command;command.entity=gameplay_id(source->entity);command.slot=source->slot;
            command.enabled=source->enabled!=0;command.space=static_cast<IkSpace>(source->space);command.weight=source->weight;
            std::copy_n(source->target,3,command.target.begin());std::copy_n(source->pole,3,command.pole.begin());
            if(const auto* pending=self.pending_entity(command.entity)) {
                require(pending->animation_rig.has_value(),"Reserved IK target needs an AnimationRig.");
                const auto& chains=pending->animation_rig->ik;
                require(std::any_of(chains.begin(),chains.end(),[&](const auto& chain){return chain.slot==command.slot;}),
                    "Reserved IK target slot is not configured.");
            } else require(self.owner->ik(command.entity,command.slot).has_value(),"IK target slot is not configured.");
            require(std::none_of(self.game_ik_commands.begin(),self.game_ik_commands.end(),[&](const auto& old) {
                return old.entity==command.entity && old.slot==command.slot;
            }),"Duplicate gameplay IK slot in one Tick.");
            self.game_ik_commands.push_back(std::move(command));++self.game_ik_write_calls;
        });
    }
