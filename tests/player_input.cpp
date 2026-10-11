// SPDX-License-Identifier: Apache-2.0
#include "poima/player.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace poima;
void check(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
int main() {
    try {
        PlayerInput input;
        input.button(PlayerAction::forward,true); input.button(PlayerAction::right,true);
        input.button(PlayerAction::jump,true); input.button(PlayerAction::jump,false);
        input.button(PlayerAction::use,true);input.button(PlayerAction::use,false);
        input.look(2.5,-1.25); input.look(.5,.25);
        auto first=input.consume("player");
        check(first.entity=="player" && first.move==std::array<float,2>{1,1} && first.look==std::array<float,2>{3,-1} && first.jump && first.use,"Sub-tick input was lost.");
        auto second=input.consume("player");
        check(second.move==first.move && second.look==std::array<float,2>{0,0} && !second.jump && !second.use,"Edges repeated across ticks.");
        input.button(PlayerAction::jump,true); check(input.consume("player").jump,"Jump press lost.");
        input.button(PlayerAction::jump,true); check(!input.consume("player").jump,"Key repeat retriggered jump.");
        input.button(PlayerAction::use,true);check(input.consume("player").use,"Use press lost.");
        input.button(PlayerAction::use,true);check(!input.consume("player").use,"Key repeat retriggered use.");
        input.button(PlayerAction::use,false);input.button(PlayerAction::use,true);
        input.button(PlayerAction::backward,true); check(input.consume("player").move[1]==0,"Opposed movement did not cancel.");
        input.look(200,0); check(input.consume("player").look[0]==180 && input.consume("player").look[0]==20,"Look overflow was lost.");
        input.look(4,5); input.clear(); auto cleared=input.consume("player");
        check(cleared.move==std::array<float,2>{0,0} && cleared.look==std::array<float,2>{0,0} && !cleared.jump && !cleared.use,"Focus loss retained input.");
        PlayerClock a,b;
        unsigned fast=0,slow=0;
        for(int i=0;i<144;++i) fast+=a.advance(1.0/144,true);
        for(int i=0;i<30;++i) slow+=b.advance(1.0/30,true);
        check(fast==60 && slow==60,"Fixed timestep depends on presentation rate.");
        PlayerClock paused; check(paused.advance(.01,true)==0,"Fractional time advanced.");
        check(paused.advance(100,false)==0 && paused.advance(.01,true)==0,"Pause caught up elapsed wall time.");
        check(paused.advance(10,true)==8 && paused.dropped_seconds()>9.8,"Stall catch-up is unbounded.");
        const auto stall=paused.last_sample();
        check(stall.observed && stall.active && stall.elapsed_seconds==10 &&
              stall.accepted_seconds==8*Runtime::fixed_dt &&
              stall.dropped_seconds==10-8*Runtime::fixed_dt && stall.planned_ticks==8 &&
              stall.committed_ticks==0 && stall.accumulator_seconds>=0,
              "Clock observation changed or concealed bounded stall policy.");
        paused.advance(10,false);const auto suspension=paused.last_sample();
        check(suspension.observed && !suspension.active && suspension.elapsed_seconds==10 &&
              suspension.accepted_seconds==0 && suspension.dropped_seconds==0 &&
              suspension.accumulator_seconds==0 && suspension.planned_ticks==0 &&
              paused.dropped_seconds()==stall.dropped_seconds,
              "Paused observation invented discarded time or reset lifetime drops.");
        PlayerClock boundary;
        // Exact double reproducer: epsilon-assisted rounding formerly returned
        // nine ticks from the second call despite the eight-tick work budget.
        check(boundary.advance(0.016666666665666664,true)==0,"Boundary fraction unexpectedly advanced.");
        check(boundary.advance(8*Runtime::fixed_dt,true)==8,"Fractional carry exceeded the eight-tick cap.");
        check(boundary.dropped_seconds()==0,"Work capping discarded accepted fractional time.");
        check(boundary.last_sample().planned_ticks==8 && boundary.last_sample().dropped_seconds==0 &&
              boundary.last_sample().accumulator_seconds>0,
              "Clock diagnostics lost accepted fractional carry.");
        check(boundary.advance(.0001,true)==1,"Work cap lost the prior fractional carry.");
        PlayerClock bounded;
        check(bounded.advance(0.016666666665666664,true)==0,"Bounded fixture fraction unexpectedly advanced.");
        for(int i=0;i<100;++i)check(bounded.advance(8*Runtime::fixed_dt,true)==8,"Repeated bounded polls exceeded the work cap.");
        bounded.advance(0,false);
        check(bounded.advance(0,true)==0,"Suspension retained capped fractional time.");
        bool rejected=false; try { paused.advance(std::numeric_limits<double>::quiet_NaN(),true); } catch(const std::invalid_argument&) { rejected=true; }
        check(rejected,"NaN frame duration accepted.");
        SceneSnapshot scene;scene.world_id="world";scene.camera_id="camera";scene.presentation_source_id="source-a";
        scene.revision=6;scene.presentation_generation=4;scene.material_revision=4;
        PlayerMaterialReport materials;materials.observe(scene,"session-a",17,9);
        check(materials.observed->scene.camera_lens==SceneLens{60,.1,1000},
              "Snapshot receipt omitted its exact effective world lens.");
        check(materials.observed && !materials.submitted && !materials.presented,"Observation forged a GPU receipt.");
        auto no_submission=*materials.observed;
        check(!materials.submit(std::move(no_submission)),"Observation IDs forged submitted work.");
        for(unsigned mismatch=0;mismatch<3;++mismatch) {
            auto wrong_owner=*materials.observed;wrong_owner.scene.frame_sequence=1;wrong_owner.scene.submission_id=41;
            if(mismatch==0)++wrong_owner.tick;
            if(mismatch==1)++wrong_owner.player_generation;
            if(mismatch==2)wrong_owner.session_id="other-session";
            check(!materials.submit(std::move(wrong_owner)),"Frame receipt lost an owner guard.");
        }
        auto frame=*materials.observed;frame.scene.frame_sequence=1;frame.scene.submission_id=41;
        auto shown=frame;
        check(materials.submit(std::move(frame)) && !materials.presented,"Submission forged presentation.");
        check(materials.present(std::move(shown)),"Successful exact submitted receipt was rejected.");
        const auto retained=*materials.presented;
        scene.material_revision=5;scene.presentation_generation=5;materials.observe(scene,"session-a",17,9);
        auto wrong=retained;wrong.scene.frame_sequence=2;wrong.scene.submission_id=42;
        check(!materials.submit(std::move(wrong)) && *materials.presented==retained,"Old material epoch acknowledged a new snapshot.");
        auto current=*materials.observed;current.scene.frame_sequence=2;current.scene.submission_id=42;
        auto not_submitted=current;not_submitted.scene.submission_id=43;
        check(materials.submit(std::move(current)) && !materials.present(std::move(not_submitted)) &&
              *materials.presented==retained,"Presentation accepted an unrelated queue submission.");
        // Same revision after replacement is insufficient: source/session tags
        // must match, and historical successful presentation remains visible.
        scene.presentation_source_id="source-b";materials.observe(scene,"session-b",2,9);
        auto prior_present=*materials.submitted;
        check(!materials.present(std::move(prior_present)),"An older submitted snapshot was presented as the replacement.");
        auto stale=*materials.submitted;stale.scene.frame_sequence=3;stale.scene.submission_id=44;
        check(!materials.submit(std::move(stale)) && *materials.presented==retained,"Replacement aliased material revision.");
        auto replacement=*materials.observed;replacement.scene.frame_sequence=3;replacement.scene.submission_id=44;
        auto replacement_shown=replacement;
        check(materials.submit(std::move(replacement)) && materials.present(std::move(replacement_shown)) &&
              materials.presented->session_id=="session-b" && materials.presented->tick==2 &&
              materials.presented->player_generation==9,"Exact replacement receipt lost its owner boundary.");
        auto duplicate=*materials.submitted;
        check(!materials.submit(std::move(duplicate)),"Old frame sequence was acknowledged twice.");
        // First-person lens and concrete selected drawable incarnations belong
        // to the frame identity, even when material/source epochs match.
        check(!renderer_scene_result(scene).camera_draw,"Omitted camera draw changed legacy receipt.");
        scene.viewmodel=SceneViewmodel{SceneLens{75,.01,10},{}};
        for(const auto& id:{"weapon","arms"}) {
            SceneObject object;object.entity_id=id;object.world=identity_matrix();object.incarnation=4;
            scene.viewmodel->objects.push_back(std::move(object));
        }
        SceneObject body;body.entity_id="body";body.world=identity_matrix();body.incarnation=8;
        body.camera_visible=false;body.camera_draw=CameraDrawBinding{"camera",CameraDrawMode::Hidden};
        scene.objects.push_back(body);
        const auto selected=renderer_scene_result(scene);
        check(selected.camera_draw && selected.camera_draw->viewmodel_lens==SceneLens{75,.01,10} &&
              selected.camera_draw->viewmodels==std::vector<SceneDrawableIdentity>{{"arms",4},{"weapon",4}} &&
              selected.camera_draw->hidden==std::vector<SceneDrawableIdentity>{{"body",8}},
              "Frame selection metadata lost lens, sorted identity or shadow-only body.");
        materials.observe(scene,"session-b",2,9);
        for(unsigned mismatch=0;mismatch<4;++mismatch) {
            auto wrong_lens=*materials.observed;wrong_lens.scene.frame_sequence=4;wrong_lens.scene.submission_id=45;
            if(mismatch==0)wrong_lens.scene.camera_lens->vertical_fov=61;
            if(mismatch==1)wrong_lens.scene.camera_lens->near_plane=.2;
            if(mismatch==2)wrong_lens.scene.camera_lens->far_plane=999;
            if(mismatch==3)wrong_lens.scene.camera_lens.reset();
            check(!materials.submit(std::move(wrong_lens)),"Frame receipt accepted a mismatched world lens at the same tick.");
        }
        for(unsigned mismatch=0;mismatch<5;++mismatch) {
            auto wrong_view=*materials.observed;wrong_view.scene.frame_sequence=4;wrong_view.scene.submission_id=45;
            auto& draw=*wrong_view.scene.camera_draw;
            if(mismatch==0)draw.viewmodel_lens->vertical_fov=76;
            if(mismatch==1)++draw.viewmodels[0].incarnation;
            if(mismatch==2)draw.viewmodels.pop_back();
            if(mismatch==3)draw.hidden[0].entity_id="other-body";
            if(mismatch==4)wrong_view.scene.camera_draw.reset();
            check(!materials.submit(std::move(wrong_view)),"Frame receipt accepted a mismatched camera draw domain.");
        }
        auto foreground_frame=*materials.observed;foreground_frame.scene.frame_sequence=4;foreground_frame.scene.submission_id=45;
        auto foreground_shown=foreground_frame;
        check(materials.submit(std::move(foreground_frame)),"Exact foreground frame submission failed.");
        auto wrong_presented=foreground_shown;wrong_presented.scene.camera_lens->near_plane=.02;
        check(!materials.present(std::move(wrong_presented)),"Presentation accepted a lens different from the submitted slot.");
        auto wrong_foreground_presented=foreground_shown;wrong_foreground_presented.scene.camera_draw->viewmodel_lens->far_plane=9;
        check(!materials.present(std::move(wrong_foreground_presented)),"Presentation accepted an unrelated foreground lens.");
        check(materials.present(std::move(foreground_shown)),
              "Exact foreground frame receipt failed submission/presentation.");
        scene.viewmodel->objects.clear();scene.objects.clear();
        const auto empty=renderer_scene_result(scene);
        check(empty.camera_draw && empty.camera_draw->viewmodel_lens && empty.camera_draw->viewmodels.empty() &&
              !same_renderer_snapshot(empty,selected),"Configured empty foreground aliased selected or absent domain.");
        // A real player projection overrides the world lens only.
        class LensOwner final:public PlayerSession {
        public:
            SceneSnapshot value;
            std::string identity() const override {return "lens-owner";}
            std::uint64_t tick() const override {return 0;}
            bool controller_valid(const std::string&) const override {return false;}
            SceneSnapshot snapshot(const std::string&) const override {return value;}
            PlayerAudioState audio_state(const std::string&) const override {return {};}
            bool advance(const std::vector<RuntimeInput>&,const std::vector<KinematicTarget>&,
                         const std::vector<SoundCommand>&) override {return false;}
        } lens_owner;
        lens_owner.value=scene;PlayerOptions lens_options;lens_options.camera="camera";lens_options.vertical_fov=110;
        const auto projected=player_snapshot(lens_options,lens_owner);
        check(projected.vertical_fov==110 && projected.viewmodel->lens==SceneLens{75,.01,10} &&
              lens_owner.value.vertical_fov==60,"Player world FOV override changed foreground or authored lens.");
        std::cout<<"Input/clock policy and exact material snapshot/submission/presentation receipts passed.\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
