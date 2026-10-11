// SPDX-License-Identifier: Apache-2.0
// Pure owner-snapshot projection tests; no renderer, SDL window or GPU claims.
#include "poima/player.hpp"
#include "poima/player_preferences.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

using namespace poima;
namespace {
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
class Owner final:public PlayerSession {
public:
    SceneSnapshot source;
    mutable std::string requested_camera;
    std::string identity() const override { return source.presentation_source_id; }
    std::uint64_t tick() const override { return source.revision; }
    bool controller_valid(const std::string&) const override { return true; }
    SceneSnapshot snapshot(const std::string& camera) const override { requested_camera=camera;return source; }
    PlayerAudioState audio_state(const std::string&) const override { return {}; }
    bool advance(const std::vector<RuntimeInput>&,const std::vector<KinematicTarget>&,const std::vector<SoundCommand>&) override {
        throw std::runtime_error("Projection tests do not simulate player frames.");
    }
    void replace(double fov,const std::string& generation) {
        source={};source.world_id="preference-projection";source.revision=generation=="first-owner" ? 11 : 29;
        source.camera_id="retained-camera";source.camera_world=identity_matrix();source.camera_world[12]=source.revision;
        source.vertical_fov=fov;source.near_plane=.08;source.far_plane=750;
        source.presentation_source_id=generation;source.presentation_generation=source.revision;source.view_cut_generation=3;
        SceneObject object;object.entity_id=generation+"-mesh";object.world=identity_matrix();object.albedo={.2f,.3f,.4f};
        source.objects.push_back(std::move(object));
    }
};
void retained(const SceneSnapshot& result,const SceneSnapshot& source) {
    check(result.world_id==source.world_id && result.revision==source.revision && result.camera_id==source.camera_id &&
        result.camera_world==source.camera_world && result.near_plane==source.near_plane && result.far_plane==source.far_plane &&
        result.presentation_source_id==source.presentation_source_id && result.presentation_generation==source.presentation_generation &&
        result.view_cut_generation==source.view_cut_generation && result.authored_vertical_fov==source.authored_vertical_fov &&
        result.ui==source.ui && result.logical_ui==source.logical_ui,
        "Player preference projection altered camera pose, scene identity or ownership metadata.");
    check(result.objects.size()==source.objects.size() && result.objects[0].entity_id==source.objects[0].entity_id &&
        result.objects[0].world==source.objects[0].world && result.objects[0].albedo==source.objects[0].albedo,
        "Player preference projection altered scene objects.");
}
void invalid(const PlayerOptions& options,const Owner& owner) {
    const auto fov=owner.source.vertical_fov;const auto revision=owner.source.revision;
    bool rejected=false;
    try { (void)player_snapshot(options,owner); }catch(const std::invalid_argument&) { rejected=true; }
    check(rejected,"Invalid player preference was admitted.");
    check((owner.source.vertical_fov==fov || (std::isnan(owner.source.vertical_fov) && std::isnan(fov))) &&
          owner.source.revision==revision,"Rejected preference mutated the owner snapshot.");
}
}
int main() {
    try {
        Owner owner;owner.replace(70,"first-owner");
        PlayerOptions options;options.camera="explicit-selector";
        check(!options.vertical_fov && !options.ui_scale,"Default player options unexpectedly override authored presentation.");
        auto inherited=player_snapshot(options,owner);retained(inherited,owner.source);
        check(inherited.vertical_fov==70 && owner.requested_camera==options.camera,"Inherited FOV or owner camera selection changed.");
        options.vertical_fov=97;options.ui_scale=1.5f;
        auto explicit_view=player_snapshot(options,owner);retained(explicit_view,owner.source);
        check(explicit_view.vertical_fov==97 && owner.source.vertical_fov==70,"Player FOV changed the authored/native camera.");
        const auto projection=perspective(explicit_view.vertical_fov,16./9.,explicit_view.near_plane,explicit_view.far_plane);
        const double expected_y=1/std::tan(97*3.14159265358979323846/360);
        check(std::abs(projection[5]-expected_y)<1e-12,"Overridden vertical FOV did not produce its independent analytic projection.");
        explicit_view.objects[0].entity_id="returned-copy-only";
        check(owner.source.objects[0].entity_id=="first-owner-mesh","Returned player scene objects alias mutable owner data.");

        // The same immutable preferences must be projected on a fresh owner
        // query, including a restored runtime with different camera/objects.
        owner.replace(48,"replacement-owner");
        auto replaced=player_snapshot(options,owner);retained(replaced,owner.source);
        check(replaced.vertical_fov==97 && owner.source.vertical_fov==48 && replaced.revision==29 &&
            inherited.revision==11 && inherited.vertical_fov==70,"Replacement reused stale snapshot data or lost explicit FOV.");
        options.vertical_fov.reset();options.ui_scale.reset();
        auto newly_inherited=player_snapshot(options,owner);retained(newly_inherited,owner.source);
        check(newly_inherited.vertical_fov==48,"Unsetting player FOV failed to inherit the replacement camera.");

        for(double value:{5.,150.}) {
            options.vertical_fov=value;
            check(player_snapshot(options,owner).vertical_fov==value,"Inclusive player FOV boundary was rejected.");
        }
        for(double value:{std::nextafter(5.,0.),std::nextafter(150.,200.),0.,-10.,
                          std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
            options.vertical_fov=value;invalid(options,owner);
        }
        options.vertical_fov=85;
        for(float value:{.25f,1.f,8.f}) {
            options.ui_scale=value;auto accepted=player_snapshot(options,owner);retained(accepted,owner.source);
            check(accepted.vertical_fov==85 && *options.ui_scale==value,"Accepted absolute UI scale changed FOV/options.");
        }
        for(float value:{std::nextafter(.25f,0.f),std::nextafter(8.f,9.f),0.f,-1.f,
                         std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
            options.ui_scale=value;invalid(options,owner);
        }
        options.ui_scale.reset();
        check(player_snapshot(options,owner).vertical_fov==85 && owner.source.vertical_fov==48,"Unsetting UI scale altered player/source camera.");

        // Native camera controls express gameplay zoom relative to a frozen
        // authored baseline. Preference intent remains the neutral angle.
        owner.replace(60,"ads-owner");owner.source.authored_vertical_fov=90;
        owner.source.viewmodel=SceneViewmodel{SceneLens{75,.01,10},{}};
        options.vertical_fov=120;
        const auto zoomed=player_snapshot(options,owner);retained(zoomed,owner.source);
        check(std::abs(zoomed.vertical_fov-90)<1e-12 && zoomed.authored_vertical_fov==90 &&
              zoomed.viewmodel->lens==SceneLens{75,.01,10} && owner.source.vertical_fov==60 && *options.vertical_fov==120,
              "ADS failed tangent-half-angle composition or changed baseline/foreground/intent.");
        const auto ads_projection=perspective(zoomed.vertical_fov,16./9.,zoomed.near_plane,zoomed.far_plane);
        check(std::abs(ads_projection[5]-1)<1e-12,"Independent composed ADS projection is wrong.");
        options.vertical_fov.reset();
        check(player_snapshot(options,owner).vertical_fov==60,"Inherited ADS changed the exact native lens.");
        options.vertical_fov=97;owner.source.vertical_fov=90;
        check(player_snapshot(options,owner).vertical_fov==97,"Neutral gameplay lens did not restore the exact preference.");
        owner.source.authored_vertical_fov.reset();owner.source.vertical_fov=60;
        check(player_snapshot(options,owner).vertical_fov==97,"External camera lost its legacy absolute override.");
        owner.source.authored_vertical_fov=150;owner.source.vertical_fov=5;options.vertical_fov=5;
        check(player_snapshot(options,owner).vertical_fov==5,"Composed narrow FOV did not clamp to the inclusive minimum.");
        owner.source.authored_vertical_fov=5;owner.source.vertical_fov=150;options.vertical_fov=150;
        check(player_snapshot(options,owner).vertical_fov==150,"Composed wide FOV did not clamp to the inclusive maximum.");
        for(double bad:{0.,151.,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()}) {
            owner.source.authored_vertical_fov=bad;invalid(options,owner);
            owner.source.authored_vertical_fov=90;owner.source.vertical_fov=bad;invalid(options,owner);
        }
        owner.replace(60,"shared-ads-owner");owner.source.authored_vertical_fov=90;
        options.preferences=std::make_shared<PlayerPreferences>(PlayerPreferences::Values{},R"({"camera.vertical_fov":120})");
        check(std::abs(player_snapshot(options,owner).vertical_fov-90)<1e-12,"Shared preferences lost ADS composition.");
        const auto retained_preference=options.preferences->snapshot();
        options.preferences->publish(options.preferences->prepare(R"({"expected_revision":0,"set":{"camera.vertical_fov":90}})"));
        check(std::abs(player_snapshot(options,owner).vertical_fov-60)<1e-12 && retained_preference->values.vertical_fov==120,
              "Preference update while aiming used stale intent or mutated the retained preference.");
        options.preferences->publish(options.preferences->prepare(R"({"expected_revision":1,"reset":["camera.vertical_fov"]})"));
        check(player_snapshot(options,owner).vertical_fov==60,"Preference reset lost inherited live gameplay lens.");
        options.preferences.reset();options.vertical_fov=120;
        owner.replace(60,"restored-ads-owner");owner.source.authored_vertical_fov=60;
        check(player_snapshot(options,owner).vertical_fov==120 && zoomed.authored_vertical_fov==90,
              "Replacement used an old authored zoom baseline or mutated an earlier snapshot.");
        std::cout<<"Player preferences: inherited/explicit FOV, immutable source projection, fresh replacement, finite inclusive FOV/UI-scale validation and composable ADS passed (5 groups; no GPU qualification).\n";
    }catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
