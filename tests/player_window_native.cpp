// SPDX-License-Identifier: Apache-2.0
#include "poima/player.hpp"
#include "poima/player_preferences.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace poima;
namespace {
void check(bool condition,const char* message) {
    if(!condition)throw std::runtime_error(message);
}
void check_player(bool condition,const PlayerWindow& window,const char* message) {
    if(condition)return;
    const auto report=window.report();
    throw std::runtime_error(std::string(message)+"; finished="+(window.finished() ? "true" : "false")+
        "; ready="+(window.ready() ? "true" : "false")+"; paused="+(window.paused() ? "true" : "false")+
        "; stop_reason="+report.stop_reason+"; detail="+report.render.detail+
        "; tick="+std::to_string(report.final_tick)+"; frames="+std::to_string(report.render.frames_presented));
}
template<class F> void rejects(F&& call,const char* message) {
    bool rejected=false;try {call();}catch(const std::exception&) {rejected=true;}
    check(rejected,message);
}
// A routing/lifetime owner, deliberately not a physics or compiled-game oracle.
// The independent shared-service fixture exercises actual Runtime/save owners.
class Owner final:public PlayerSession {
public:
    std::string session="original";
    std::uint64_t current_tick=7,material_revision=0;
    mutable unsigned snapshots=0,lens_samples=0,pose_samples=0,pose_snapshots=0;
    mutable std::vector<std::uint64_t> pose_sampled_ticks;
    mutable std::vector<std::uint64_t> sampled_ticks;
    unsigned advances=0,owner_polls=0;
    SceneLens world_lens;
    std::optional<SceneLens> viewmodel_lens;
    std::optional<double> authored_fov;
    bool animate_lenses=false,pose_provider=false,animate_poses=false;
    std::string pose_source;
    double base_offset=0;
    RuntimeCameraPose world_pose;
    std::optional<RuntimeCameraPose> viewmodel_pose;
    Matrix4 current_base() const {
        // Explicit 90-degree Z base: large committed motion must never blend.
        return {0,1,0,0,-1,0,0,0,0,0,1,0,10+double(current_tick)*.5+base_offset,2,-3,1};
    }
    // One-shot maintenance changes exercise real PlayerWindow routing only;
    // they do not stand in for a Runtime, save writer or compiled callback.
    bool maintenance_tick=false,maintenance_replace=false;
    std::string maintenance_session;
    void owner_poll() noexcept override {
        ++owner_polls;
        if(maintenance_tick) {maintenance_tick=false;++current_tick;}
        if(maintenance_replace) {
            maintenance_replace=false;session.swap(maintenance_session);current_tick=2;
        }
    }
    std::string identity() const override {return session;}
    std::uint64_t tick() const override {return current_tick;}
    bool controller_valid(const std::string& id) const override {return id=="controller";}
    SceneSnapshot scene(const std::string& camera) const {
        check(camera=="camera" || camera=="camera2","Unexpected owner camera");
        SceneSnapshot result;result.world_id="native-window-lifetime";result.camera_id=camera;
        // A valid rigid camera is required even for a scene with no mesh objects.
        // SceneSnapshot's Matrix4 has no default initializer.
        result.camera_world=identity_matrix();
        result.presentation_source_id=pose_source.empty() ? session : pose_source;result.presentation_generation=material_revision;result.material_revision=material_revision;
        result.vertical_fov=world_lens.vertical_fov;result.near_plane=world_lens.near_plane;result.far_plane=world_lens.far_plane;
        result.authored_vertical_fov=authored_fov;
        if(viewmodel_lens)result.viewmodel=SceneViewmodel{*viewmodel_lens,{}};
        return result;
    }
    SceneSnapshot snapshot(const std::string& camera) const override {
        ++snapshots;return scene(camera);
    }
    std::optional<PlayerPoseSample> pose_sample(const std::string& camera) const override {
        check(camera=="camera" || camera=="camera2","Unexpected lightweight pose camera");
        ++pose_samples;pose_sampled_ticks.push_back(current_tick);
        if(!pose_provider)return {};
        return PlayerPoseSample{session,pose_source.empty() ? session : pose_source,camera,current_tick,
            material_revision,world_pose,viewmodel_pose,current_base()};
    }
    SceneSnapshot pose_snapshot(const std::string& camera,const RuntimeCameraPose& world,
        const std::optional<RuntimeCameraPose>& viewmodel) const override {
        check(pose_provider && bool(viewmodel)==bool(viewmodel_lens),"Unexpected pose snapshot provider/channels");
        ++pose_snapshots;auto result=scene(camera);const auto base=current_base();
        result.camera_world=multiply(base,local_matrix(world.position,world.rotation,{1,1,1}));
        if(result.viewmodel) {
            // One synthetic foreground cube gives the real renderer an actual
            // incarnation/matrix to acknowledge, rather than an empty list.
            SceneObject object{};object.entity_id="foreground";object.incarnation=17;
            object.albedo={1,.1f,.1f};
            const auto effect=local_matrix(viewmodel->position,viewmodel->rotation,{1,1,1});
            object.world=multiply(multiply(result.camera_world,effect),local_matrix({0,0,-2},{0,0,0,1},{.3,.3,.3}));
            result.viewmodel->objects.push_back(std::move(object));
        }
        return result;
    }
    std::optional<PlayerLensSample> lens_sample(const std::string& camera) const override {
        check(camera=="camera" || camera=="camera2","Unexpected lightweight lens camera");++lens_samples;sampled_ticks.push_back(current_tick);
        return PlayerLensSample{session,pose_source.empty() ? session : pose_source,camera,current_tick,material_revision,world_lens,viewmodel_lens,authored_fov};
    }
    PlayerAudioState audio_state(const std::string&) const override {
        AudioSnapshot snapshot;snapshot.listener=identity_matrix();return {current_tick,std::move(snapshot),{}};
    }
    bool advance(const std::vector<RuntimeInput>& inputs,const std::vector<KinematicTarget>&,
                 const std::vector<SoundCommand>&) override {
        check(inputs.empty() || (inputs.size()==1 && inputs[0].entity=="controller"),"Unexpected resolved input");
        ++current_tick;++advances;
        if(animate_lenses) {
            world_lens.vertical_fov=70-double(advances%12);
            if(viewmodel_lens)viewmodel_lens->vertical_fov=75-double(advances%12)*.5;
        }
        if(animate_poses) {
            world_pose.position={double(advances)*.01,double(advances)*.02,0};
            const double half_angle=double(advances)*.01;
            world_pose.rotation={0,0,std::sin(half_angle),std::cos(half_angle)};
            if(viewmodel_pose)viewmodel_pose->position={double(advances)*.005,0,0};
        }
        return false;
    }
};
PlayerOptions options() {
    PlayerOptions value;value.camera="camera";value.controller="controller";
    value.ui_scale=1.5f;
    value.gamepad_selection.mode="disabled";value.render.samples=1;
    value.render.width=320;value.render.height=240;value.render.capture_exclusive=true;
    return value;
}
std::vector<char> bytes(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary);
    check(bool(input),"Capture file missing");
    return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
}
std::string utf8(const std::filesystem::path& path) {
    const auto text=path.u8string();return {text.begin(),text.end()};
}
void basic() {
    Owner owner;PlayerWindow window(options(),owner,true);
    check(!window.ready() && !window.finished() && window.paused(),"Deferred native window state changed");
    check(owner.snapshots==0 && owner.advances==0 && owner.owner_polls==0,"Construction touched graphics/simulation snapshot or maintenance");
    rejects([&]{window.capture("never-before-ready.bmp");},"Unready capture was accepted");
    rejects([&]{window.capture_snapshot();},"Unready capture preparation was accepted");
    check(owner.snapshots==0 && owner.advances==0,"Unready capture called owner");
    bool rejected=false;
    std::thread other([&]{try {(void)window.ready();}catch(const std::exception&) {rejected=true;}});
    other.join();check(rejected,"Foreign-thread native window call was accepted");
    rejected=false;
    std::thread foreign_poll([&]{try {(void)window.poll();}catch(const std::exception&) {rejected=true;}});
    foreign_poll.join();
    check(rejected && owner.owner_polls==0,"Foreign poll invoked owner maintenance before rejection");
    window.request_stop();window.request_stop();
    check(!window.poll() && window.finished() && !window.ready(),"Deferred cancellation did not finish");
    check(owner.owner_polls==1,"Paused deferred stop omitted or duplicated owner maintenance");
    const auto report=window.report();
    check(report.stop_reason=="requested_stop" && report.initial_tick==7 && report.final_tick==7,
          "Cancellation changed tick or reason");
    check(!report.materials.observed && !report.materials.submitted && !report.materials.presented,
          "Deferred stop forged a material snapshot or frame receipt");
    check(report.render.frames_presented==0 && owner.snapshots==0 && owner.advances==0,
          "Zero-frame cancellation initialized or advanced");
    check(report.effective_ui_scale==1.5,"Zero-frame report lost configured absolute UI scale");
    check(!window.poll(),"Terminal poll resumed canceled owner");
    check(owner.owner_polls==2,"Terminal native poll omitted owner maintenance");
    rejected=false;
    std::thread foreign_terminal([&]{try {(void)window.poll();}catch(const std::exception&) {rejected=true;}});
    foreign_terminal.join();
    check(rejected && owner.owner_polls==2,"Foreign terminal poll entered owner maintenance");
    rejects([&]{window.pause(false);},"Finished window accepted pause");
    rejects([&]{window.capture("never-after-stop.bmp");},"Finished window accepted capture");
}
void lens_admission_without_window() {
    Owner owner;auto launch=options();launch.replay=true;launch.lens_interpolation=true;
    rejects([&]{PlayerWindow denied(launch,owner,true);},"Opt-in replay was accepted before graphics initialization");
    check(owner.snapshots==0 && owner.lens_samples==0 && owner.advances==0 && owner.owner_polls==0,
        "Rejected replay queried or advanced the owner");
    launch.replay=false;
    PlayerWindow deferred(launch,owner,true);
    check(owner.snapshots==0 && owner.lens_samples==0 && !deferred.report().materials.observed,
        "Deferred opt-in construction took samples or fabricated metadata");
    PlayerCaptureSnapshot forged;
    rejects([&]{deferred.capture("never-forged.bmp",forged);},"Default token bypassed capture readiness");
    deferred.request_stop();check(!deferred.poll(),"Deferred opt-in owner failed to stop");
    check(owner.snapshots==0 && owner.lens_samples==0 && owner.advances==0,
        "Deferred stop created interpolation state without a frame");
}
void pose_admission_without_window() {
    Owner owner;auto launch=options();launch.replay=true;launch.pose_interpolation=true;
    rejects([&]{PlayerWindow denied(launch,owner,true);},"Replay admitted pose interpolation before device initialization");
    check(owner.snapshots==0 && owner.pose_snapshots==0 && owner.pose_samples==0 && owner.owner_polls==0,
        "Rejected pose replay touched the owner or took a scene");
    launch.replay=false;PlayerWindow deferred(launch,owner,true);
    check(owner.snapshots==0 && owner.pose_samples==0 && !deferred.report().materials.observed,
        "Deferred pose option sampled before a frame");
    PlayerCaptureSnapshot forged;
    rejects([&]{deferred.capture("never-pose-before-ready.bmp",forged);},"Unready pose token bypassed admission");
    deferred.request_stop();check(!deferred.poll() && owner.pose_samples==0 && owner.pose_snapshots==0,
        "Deferred pose stop sampled or rendered");
}
Matrix4 independent_product(const Matrix4& a,const Matrix4& b) {
    Matrix4 result{};
    for(unsigned column=0;column<4;++column)for(unsigned row=0;row<4;++row)
        for(unsigned k=0;k<4;++k)result[column*4+row]+=a[k*4+row]*b[column*4+k];
    return result;
}
Matrix4 independent_pose(const RuntimeCameraPose& pose) {
    const auto& q=pose.rotation;const auto x=q[0],y=q[1],z=q[2],w=q[3];
    return {1-2*(y*y+z*z),2*(x*y+z*w),2*(x*z-y*w),0,
        2*(x*y-z*w),1-2*(x*x+z*z),2*(y*z+x*w),0,
        2*(x*z+y*w),2*(y*z-x*w),1-2*(x*x+y*y),0,
        pose.position[0],pose.position[1],pose.position[2],1};
}
void matrix_near(const Matrix4& a,const Matrix4& b,const char* message) {
    for(unsigned i=0;i<16;++i)check(std::isfinite(a[i]) && std::isfinite(b[i]) && std::abs(a[i]-b[i])<1e-11,message);
}
void pose_scene(const SceneSnapshot& snapshot,const PlayerPosePresentation& pose) {
    matrix_near(snapshot.camera_world,independent_product(pose.target_base_world,independent_pose(pose.rendered_world)),
        "Prepared camera did not use exactly current committed base times rendered effect");
    check(snapshot.viewmodel && snapshot.viewmodel->objects.size()==1 && pose.rendered_viewmodel,
        "Synthetic foreground or rendered optional channel missing");
    const Matrix4 object_local{.3,0,0,0,0,.3,0,0,0,0,.3,0,0,0,-2,1};
    const auto camera=independent_product(pose.target_base_world,independent_pose(pose.rendered_world));
    matrix_near(snapshot.viewmodel->objects[0].world,independent_product(independent_product(camera,
        independent_pose(*pose.rendered_viewmodel)),object_local),"Foreground matrix lost the rendered camera/world/viewmodel effect chain");
}
void write_pose_receipt(const std::filesystem::path& path,const PlayerMaterialReceipt& receipt) {
    using Json=nlohmann::json;check(receipt.pose_presentation && receipt.scene.camera_pose,"Real pose presentation receipt missing");
    const auto& p=*receipt.pose_presentation;const auto& scene=*receipt.scene.camera_pose;
    const auto pose=[](const RuntimeCameraPose& v){return Json{{"position",v.position},{"rotation",v.rotation}};};
    Json foreground=Json::array();
    for(const auto& object:scene.viewmodels)foreground.push_back({{"entity_id",object.entity_id},{"incarnation",object.incarnation},{"world",object.world}});
    Json result={{"synthetic_owner",true},{"physics_or_compiled_game_oracle",false},{"session_id",receipt.session_id},
        {"tick",receipt.tick},{"player_generation",receipt.player_generation},{"source_id",receipt.scene.presentation_source_id},
        {"camera_id",receipt.scene.camera_id},{"frame_sequence",receipt.scene.frame_sequence},{"submission_id",receipt.scene.submission_id},
        {"previous_tick",p.previous_tick},{"target_tick",p.target_tick},{"alpha",p.alpha},{"interpolating",p.interpolating},
        {"reset_reason",p.reset_reason},{"previous_world",pose(p.previous_world)},{"target_world",pose(p.target_world)},
        {"rendered_world",pose(p.rendered_world)},{"target_base_world",p.target_base_world},
        {"previous_viewmodel",p.previous_viewmodel ? pose(*p.previous_viewmodel) : Json(nullptr)},
        {"target_viewmodel",p.target_viewmodel ? pose(*p.target_viewmodel) : Json(nullptr)},
        {"rendered_viewmodel",p.rendered_viewmodel ? pose(*p.rendered_viewmodel) : Json(nullptr)},
        {"actual_camera_world",scene.camera_world},{"actual_foreground",std::move(foreground)}};
    check(!std::filesystem::exists(path),"Pose receipt must be new");std::ofstream stream(path,std::ios::binary);
    stream<<result.dump(2)<<'\n';check(bool(stream),"Failed to retain pose receipt");
}
void pose_capture_graphics(int gpu,const std::filesystem::path& output) {
    auto launch=options();launch.render.gpu=gpu;launch.player_generation=71;
    launch.background=true;launch.pose_interpolation=true;launch.lens_interpolation=true;
    Owner owner;owner.pose_provider=true;owner.pose_source="pose-source";
    owner.world_lens={70,.1,100};owner.viewmodel_lens=SceneLens{75,.01,10};owner.authored_fov=70;
    owner.world_pose.position={.1,.2,0};owner.viewmodel_pose=RuntimeCameraPose{};
    std::optional<PlayerCaptureSnapshot> expired;
    {
        PlayerWindow window(launch,owner,true);
        check_player(window.poll() && window.ready(),window,"Pose/lens background window did not initialize");
        const auto initial=window.report().materials.presented;
        check(initial && initial->pose_presentation && initial->lens_presentation &&
            initial->pose_presentation->enabled && !initial->pose_presentation->interpolating &&
            initial->pose_presentation->alpha==1 && initial->pose_presentation->target_world==owner.world_pose &&
            initial->pose_presentation->rendered_world==owner.world_pose && initial->scene.camera_pose &&
            initial->scene.camera_pose->viewmodels.size()==1 && owner.snapshots==0 && owner.advances==0,
            "Paused initial scene did not retain exact poses alongside lens metadata");
        matrix_near(initial->scene.camera_pose->camera_world,independent_product(owner.current_base(),independent_pose(owner.world_pose)),
            "Initial actual camera receipt composed the base/effect incorrectly");
        write_pose_receipt(output/"pose-initial.json",*initial);
        owner.animate_lenses=true;owner.animate_poses=true;window.pause(false);
        const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while(owner.advances==0 && std::chrono::steady_clock::now()<until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            check_player(window.poll() && window.ready(),window,"Background pose owner failed to acquire live Tick");
        }
        check(owner.advances>0 && !window.paused(),"Background pose owner never committed a live Tick");
        const auto advances=owner.advances,scenes=owner.pose_snapshots,samples=owner.pose_samples;
        const auto tick=owner.current_tick;
        std::this_thread::sleep_for(std::chrono::milliseconds(160));
        check_player(window.poll() && window.ready(),window,"Pose eight-Tick catch-up failed");
        check(owner.advances-advances==8 && owner.snapshots==0 && owner.pose_snapshots-scenes<=2 &&
            owner.pose_samples-samples>=8,"Pose catch-up extracted scenes per Tick or lost lightweight samples");
        for(std::uint64_t value=tick+1;value<=owner.current_tick;++value)
            check(std::find(owner.pose_sampled_ticks.begin()+samples,owner.pose_sampled_ticks.end(),value)!=owner.pose_sampled_ticks.end(),
                "Pose history omitted a committed Tick during catch-up");
        const auto pair=window.report().materials.observed->pose_presentation;
        check(pair && pair->previous_tick+1==pair->target_tick && pair->target_tick==owner.current_tick &&
            pair->target_world==owner.world_pose && pair->target_viewmodel==owner.viewmodel_pose &&
            pair->target_base_world==owner.current_base() && pair->alpha>=0 && pair->alpha<1 && pair->interpolating,
            "Live pose receipt did not retain the last consecutive committed pair/current base");
        for(unsigned axis=0;axis<3;++axis)
            check(std::abs(pair->rendered_world.position[axis]-(pair->previous_world.position[axis]+
                (pair->target_world.position[axis]-pair->previous_world.position[axis])*pair->alpha))<1e-12,
                "Live translation was not linear in the reported fixed-clock alpha");
        // The synthetic owner uses a monotone Z arc below pi. Its independent
        // scalar angle oracle also checks the actual window's rotation blend.
        const double angle_before=2*std::atan2(pair->previous_world.rotation[2],pair->previous_world.rotation[3]);
        const double angle_target=2*std::atan2(pair->target_world.rotation[2],pair->target_world.rotation[3]);
        const double angle=angle_before+(angle_target-angle_before)*pair->alpha;
        check(std::abs(pair->rendered_world.rotation[2]-std::sin(angle*.5))<1e-12 &&
            std::abs(pair->rendered_world.rotation[3]-std::cos(angle*.5))<1e-12,
            "Actual pose presentation did not follow the independent constant-speed rotation arc");
        const auto lenses=window.report().materials.observed->lens_presentation;
        check(lenses && lenses->previous_tick==pair->previous_tick && lenses->target_tick==pair->target_tick && lenses->alpha==pair->alpha,
            "Lens and pose interpolation did not coexist on the same committed fixed-clock pair");
        auto stale=window.capture_snapshot();auto frozen=window.capture_snapshot();
        rejects([&]{window.capture(utf8(output/"pose-stale.bmp"),stale);},"New pose preparation retained an old capture lease");
        check(frozen.pose_presentation() && frozen.lens_presentation(),"Frozen capture lost pose or lens channel");
        pose_scene(frozen.snapshot(),*frozen.pose_presentation());
        const auto before_scenes=owner.pose_snapshots,before_advances=owner.advances;const auto before_tick=owner.current_tick;
        const auto capture=window.capture(utf8(output/"pose-live.bmp"),frozen);
        check(capture.success && capture.hardware && capture.capture_written && capture.validation_errors==0 &&
            capture.diagnostics.scene_submitted && capture.diagnostics.scene_presented &&
            *capture.diagnostics.scene_submitted==*capture.diagnostics.scene_presented &&
            same_renderer_snapshot(*capture.diagnostics.scene_presented,renderer_scene_result(frozen.snapshot())) &&
            window.report().materials.presented->pose_presentation==frozen.pose_presentation() &&
            owner.pose_snapshots==before_scenes && owner.advances==before_advances && owner.current_tick==before_tick,
            "Frozen pose capture resampled/advanced authority or acknowledged different camera/foreground transforms");
        const auto bmp=bytes(output/"pose-live.bmp");check(bmp.size()>54 && bmp[0]=='B' && bmp[1]=='M',"Pose capture is not actual BMP bytes");
        write_pose_receipt(output/"pose-live.json",*window.report().materials.presented);
        auto pause_lease=window.capture_snapshot();window.pause(true);
        rejects([&]{window.capture(utf8(output/"pose-paused-stale.bmp"),pause_lease);},"Pause retained an old pose lease");
        window.synchronize();auto paused=window.capture_snapshot();
        check(paused.pose_presentation() && paused.pose_presentation()->alpha==1 &&
            !paused.pose_presentation()->interpolating && paused.pose_presentation()->rendered_world==owner.world_pose &&
            paused.pose_presentation()->rendered_viewmodel==owner.viewmodel_pose && !paused.pose_presentation()->reset_reason.empty(),
            "Pause failed to present exact current pose endpoints");
        pose_scene(paused.snapshot(),*paused.pose_presentation());
        const auto paused_capture=window.capture(utf8(output/"pose-paused.bmp"),paused);
        check(paused_capture.success && paused_capture.hardware && paused_capture.capture_written && paused_capture.validation_errors==0,
            "Exact paused endpoint capture failed");
        write_pose_receipt(output/"pose-paused.json",*window.report().materials.presented);
        const auto cut=[&](const char* name,auto&& change) {
            const auto token=window.capture_snapshot();change();
            const auto denied=output/(std::string("pose-cut-")+name+".bmp");
            rejects([&]{window.capture(utf8(denied),token);},"Changed target retained a pose capture lease");
            check(!std::filesystem::exists(denied),"Rejected pose lease published an artifact");
            window.synchronize();check_player(window.poll() && window.ready(),window,"Changed pose owner failed to redraw");
            const auto fresh=window.capture_snapshot();
            check(fresh.pose_presentation() && fresh.pose_presentation()->alpha==1 &&
                !fresh.pose_presentation()->interpolating && fresh.pose_presentation()->target_tick==owner.current_tick &&
                !fresh.pose_presentation()->reset_reason.empty(),"Pose owner cut retained an old interpolation interval");
            pose_scene(fresh.snapshot(),*fresh.pose_presentation());
        };
        cut("world",[&]{owner.world_pose.position[0]+=.125;});
        cut("viewmodel",[&]{owner.viewmodel_pose->position[1]+=.0625;});
        cut("base",[&]{owner.base_offset+=4;});
        cut("source",[&]{owner.pose_source="pose-source-replaced";});
        cut("material",[&]{++owner.material_revision;});
        cut("restore",[&]{owner.current_tick=2;});
        cut("session",[&]{owner.session="pose-restored-owner";owner.current_tick=1;});
        auto channel=window.capture_snapshot();owner.viewmodel_pose.reset();owner.viewmodel_lens.reset();
        rejects([&]{window.capture(utf8(output/"pose-channel-stale.bmp"),channel);},"Removed optional pose channel retained a lease");
        window.synchronize();check_player(window.poll() && window.ready(),window,"Optional channel removal failed to present");
        const auto absent=window.capture_snapshot();
        check(absent.pose_presentation() && !absent.pose_presentation()->previous_viewmodel &&
            !absent.pose_presentation()->target_viewmodel && !absent.pose_presentation()->rendered_viewmodel &&
            !absent.snapshot().viewmodel && absent.pose_presentation()->alpha==1 && !absent.pose_presentation()->interpolating,
            "Optional foreground removal blended against a stale channel");
        owner.viewmodel_pose=RuntimeCameraPose{};owner.viewmodel_lens=SceneLens{75,.01,10};window.synchronize();
        check_player(window.poll() && window.ready(),window,"Optional foreground reintroduction failed to present");
        auto inactive=window.capture_snapshot();check_player(window.poll() && window.ready(),window,"Paused pose lease poll failed");
        rejects([&]{window.capture(utf8(output/"pose-poll-stale.bmp"),inactive);},"An actual poll retained a pose lease");
        expired=window.capture_snapshot();window.request_stop();check(!window.poll() && window.report().render.success,"Pose window failed natural stop");
        check(owner.pose_snapshots>0 && owner.snapshots==0,"Opt-in frame silently fell back to a committed full snapshot");
    }
    {
        auto next=launch;next.camera="camera2";
        PlayerWindow successor(next,owner,true);
        check_player(successor.poll() && successor.ready(),successor,"New selected-camera window failed initialization");
        const auto token=successor.capture_snapshot();
        check(token.snapshot().camera_id=="camera2" && token.pose_presentation() && token.pose_presentation()->alpha==1 &&
            !token.pose_presentation()->interpolating,"Selected camera/lifetime change retained an old pose pair");
        rejects([&]{successor.capture(utf8(output/"pose-old-lifetime.bmp"),*expired);},"Successor camera/window accepted an expired pose lease");
        check(!std::filesystem::exists(output/"pose-old-lifetime.bmp") && !std::filesystem::exists(output/"pose-stale.bmp") &&
            !std::filesystem::exists(output/"pose-paused-stale.bmp") && !std::filesystem::exists(output/"pose-poll-stale.bmp") && !std::filesystem::exists(output/"pose-channel-stale.bmp"),
            "Rejected pose lease produced an output artifact");
        successor.request_stop();check(!successor.poll() && successor.report().render.success,"Selected-camera successor failed natural stop");
    }
}
void preferences_without_window() {
    Owner owner;auto launch=options();launch.vertical_fov=37;
    PlayerPreferences::Values inherited;inherited.ui_scale=1.25f;inherited.sensitivity_x=.2;inherited.sensitivity_y=.3;
    inherited.samples=launch.render.samples;inherited.frames_in_flight=launch.render.frames_in_flight;
    for(bool wrong_samples:{true,false}) {
        auto mismatch=launch;auto bad=inherited;
        if(wrong_samples)bad.samples=launch.render.samples==1 ? 4 : 1;
        else bad.frames_in_flight=launch.render.frames_in_flight==1 ? 2 : 1;
        mismatch.preferences=std::make_shared<PlayerPreferences>(bad);
        bool rejected=false;
        try {PlayerWindow denied(mismatch,owner,true);}catch(const std::invalid_argument&) {rejected=true;}
        check(rejected && owner.snapshots==0 && owner.advances==0 && owner.current_tick==7,
              "Mismatched frozen preference graphics admitted a window or touched its native owner");
    }
    launch.preferences=std::make_shared<PlayerPreferences>(inherited);
    PlayerWindow window(launch,owner,true);
    const auto initial=window.report();
    check(initial.preferences.observed_revision==0 && !initial.preferences.applied_revision &&
          !initial.preferences.presented_revision && !initial.preferences.sink_gain &&
          !initial.preferences.effective_vertical_fov && initial.effective_ui_scale==1.25,
          "Deferred owner incorrectly claimed hardware application/presentation");
    launch.preferences->publish(launch.preferences->prepare(R"({"expected_revision":0,"set":{"camera.vertical_fov":95,"ui.scale":2,"input.sensitivity_x":0.4,"input.invert_x":true,"audio.master_gain":0.25,"graphics.samples":1}})"));
    window.synchronize();const auto changed=window.report();
    check(changed.preferences.observed_revision==1 && !changed.preferences.applied_revision &&
          !changed.preferences.presented_revision && !changed.preferences.sink_gain &&
          changed.preferences.sensitivity_x==.4 && changed.preferences.sensitivity_y==.3 &&
          changed.preferences.invert_x && changed.preferences.requested_master_gain==.25 &&
          changed.effective_ui_scale==2 && owner.snapshots==0 && owner.advances==0,
          "Pre-initialization preference synchronization touched the native owner or lost intent");
    const auto projected=player_snapshot(launch,owner);
    check(projected.vertical_fov==95 && owner.snapshot("camera").vertical_fov==60,
          "Shared FOV projection used stale legacy override or changed authored camera");
    launch.preferences->publish(launch.preferences->prepare(R"({"expected_revision":1,"reset":["camera.vertical_fov","ui.scale"]})"));
    window.synchronize();
    check(player_snapshot(launch,owner).vertical_fov==60 && window.report().effective_ui_scale==1.25,
          "Reset failed to restore shared inherited camera/scale instead of legacy overrides");
    window.request_stop();check(!window.poll(),"Deferred configured window did not stop");
    check(owner.advances==0 && !window.report().preferences.applied_revision &&
          !window.report().preferences.presented_revision && !window.report().preferences.sink_gain,
          "Zero-frame stop claimed preferences were presented or audible");
}
void lens_capture_graphics(int gpu,const std::filesystem::path& output) {
    auto launch=options();launch.render.gpu=gpu;launch.lens_interpolation=true;launch.player_generation=31;
    Owner owner;owner.world_lens={70,.1,100};owner.viewmodel_lens=SceneLens{75,.01,10};owner.authored_fov=70;
    PlayerPreferences::Values values;values.vertical_fov=90;values.samples=launch.render.samples;
    values.frames_in_flight=launch.render.frames_in_flight;
    launch.preferences=std::make_shared<PlayerPreferences>(values);
    std::optional<PlayerCaptureSnapshot> old_lifetime;
    {
        PlayerWindow window(launch,owner,true);
        check_player(window.poll() && window.ready(),window,"Opt-in paused window initialization failed");
        const auto shown=window.report().materials.presented;
        check(shown && shown->lens_presentation && shown->lens_presentation->enabled &&
            !shown->lens_presentation->interpolating && shown->lens_presentation->alpha==1 &&
            shown->lens_presentation->target_world==owner.world_lens &&
            shown->lens_presentation->rendered_world.vertical_fov==90,
            "Paused opt-in metadata lost configuration, raw target or composed exact endpoint");
        auto obsolete=window.capture_snapshot();auto frozen=window.capture_snapshot();
        const auto untouched=output/"token-obsolete.bmp";
        rejects([&]{window.capture(utf8(untouched),obsolete);},"A newer preparation retained an earlier capture lease");
        check(!std::filesystem::exists(untouched),"Stale token created an output artifact");
        check(frozen.lens_presentation() && frozen.lens_presentation()->enabled &&
            frozen.snapshot().vertical_fov==90 && frozen.snapshot().viewmodel->lens==*owner.viewmodel_lens,
            "Frozen preparation lost its exact scene or optional lens metadata");
        const auto snapshots=owner.snapshots;const auto tick=owner.current_tick;
        const auto capture=window.capture(utf8(output/"token-current.bmp"),frozen);
        check(capture.success && capture.hardware && capture.capture_written && capture.validation_errors==0 &&
            capture.diagnostics.scene_presented && capture.diagnostics.scene_submitted &&
            *capture.diagnostics.scene_presented==*capture.diagnostics.scene_submitted &&
            same_renderer_snapshot(*capture.diagnostics.scene_presented,renderer_scene_result(frozen.snapshot())) &&
            window.report().materials.presented->lens_presentation==frozen.lens_presentation() &&
            owner.snapshots==snapshots && owner.current_tick==tick && owner.advances==0,
            "Frozen capture resampled the scene, advanced authority or acknowledged another projection");

        auto inactive_poll=window.capture_snapshot();check_player(window.poll() && window.ready(),window,"Paused lease-boundary poll failed");
        rejects([&]{window.capture(utf8(output/"token-poll.bmp"),inactive_poll);},"Inactive poll retained a capture lease");
        auto paused=window.capture_snapshot();window.pause(true);
        rejects([&]{window.capture(utf8(output/"token-pause.bmp"),paused);},"Repeated pause retained a capture lease");
        auto preference=window.capture_snapshot();
        launch.preferences->publish(launch.preferences->prepare(R"({"expected_revision":0,"set":{"camera.vertical_fov":100}})"));
        rejects([&]{window.capture(utf8(output/"token-preference.bmp"),preference);},"Unobserved preference update retained a capture lease");
        window.synchronize();
        auto authority=window.capture_snapshot();owner.world_lens.vertical_fov=60;
        rejects([&]{window.capture(utf8(output/"token-authority.bmp"),authority);},"Unobserved same-Tick native lens edit retained a capture lease");
        check(authority.snapshot().vertical_fov==100 && authority.lens_presentation()->target_world.vertical_fov==70,
            "Old immutable capture preparation aliased the changed native lens");
        window.synchronize();
        auto baseline=window.capture_snapshot();owner.authored_fov=75;
        rejects([&]{window.capture(utf8(output/"token-baseline.bmp"),baseline);},"Unobserved authored zoom baseline change retained a capture lease");
        owner.authored_fov=70;window.synchronize();
        auto ownership=window.capture_snapshot();owner.session="new-opt-in-owner";
        rejects([&]{window.capture(utf8(output/"token-owner.bmp"),ownership);},"Changed owner identity retained a capture lease");
        window.synchronize();check_player(window.poll() && window.ready(),window,"Opt-in owner replacement did not present");
        auto foreign=window.capture_snapshot();
        {
            Owner other;other.world_lens=owner.world_lens;other.viewmodel_lens=owner.viewmodel_lens;other.authored_fov=owner.authored_fov;
            other.session=owner.session;
            PlayerWindow different(launch,other,true);
            check_player(different.poll() && different.ready(),different,"Foreign token test window failed to initialize");
            rejects([&]{different.capture(utf8(output/"token-foreign.bmp"),foreign);},"Another window accepted the prepared lease");
            different.request_stop();check(!different.poll() && different.report().render.success,"Foreign window did not stop cleanly");
        }
        // Real interactive polls without physical input. The fake owner proves
        // routing and snapshot cost, not compiled gameplay or physics behavior.
        owner.animate_lenses=true;window.pause(false);
        const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        const auto original_advances=owner.advances;
        while(owner.advances==original_advances && std::chrono::steady_clock::now()<until) {
            // Closing the foreign window can queue focus events. Resume only
            // after those events have made this existing window inactive.
            if(window.paused())window.pause(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            check_player(window.poll() && window.ready(),window,"Interactive lens owner failed to poll");
        }
        check(owner.advances>original_advances && !window.paused(),"Actual interactive window never acquired an active owner Tick");
        const auto before_advances=owner.advances,before_snapshots=owner.snapshots,before_samples=owner.lens_samples;
        const auto before_tick=owner.current_tick;
        std::this_thread::sleep_for(std::chrono::milliseconds(160));
        check_player(window.poll() && window.ready(),window,"Eight-Tick catch-up poll failed");
        check(owner.advances-before_advances==8 && owner.snapshots-before_snapshots<=3 &&
            owner.lens_samples-before_samples>=8,"Catch-up took full scene snapshots per Tick or omitted lightweight committed samples");
        for(std::uint64_t committed=before_tick+1;committed<=owner.current_tick;++committed)
            check(std::find(owner.sampled_ticks.begin()+before_samples,owner.sampled_ticks.end(),committed)!=owner.sampled_ticks.end(),
                "Catch-up omitted a committed lightweight sample");
        const auto pair=window.report().materials.observed->lens_presentation;
        check(pair && pair->enabled && pair->previous_tick+1==pair->target_tick && pair->target_tick==owner.current_tick,
            "Actual catch-up did not retain its final committed lens pair");
        window.pause(true);window.synchronize();old_lifetime=window.capture_snapshot();
        const auto no_output=output/"token-default.bmp";PlayerCaptureSnapshot forged;
        rejects([&]{window.capture(utf8(no_output),forged);},"Default-constructed capture token was accepted");
        check(!std::filesystem::exists(no_output),"Forged token created an artifact");
        for(const auto* name:{"token-poll.bmp","token-pause.bmp","token-preference.bmp","token-authority.bmp","token-baseline.bmp","token-owner.bmp","token-foreign.bmp"})
            check(!std::filesystem::exists(output/name),"Rejected lease published a capture artifact");
        window.request_stop();check(!window.poll() && window.report().render.success,"Opt-in capture owner did not close naturally");
    }
    {
        // Keep the same logical owner alive; only the graphics-window lifetime
        // changes. A retained token must not be accepted if heap addresses recur.
        PlayerWindow successor(launch,owner,true);
        check_player(successor.poll() && successor.ready(),successor,"Successor graphics window failed to initialize");
        auto current=successor.capture_snapshot();
        rejects([&]{successor.capture(utf8(output/"token-old-lifetime.bmp"),*old_lifetime);},"A successor window accepted a destroyed window's lease");
        check(!std::filesystem::exists(output/"token-old-lifetime.bmp"),"Expired window lease created an artifact");
        check(current.snapshot().camera_id=="camera","Successor preparation lost current owner camera");
        successor.request_stop();check(!successor.poll() && successor.report().render.success,"Successor window did not close naturally");
    }
}
void graphics(int gpu,const std::filesystem::path& output,bool check_audio) {
    auto settings=options();settings.render.gpu=gpu;settings.player_generation=12;
    {
        Owner owner;auto configured=settings;
        PlayerPreferences::Values values;values.ui_scale=settings.ui_scale;values.samples=settings.render.samples;
        values.frames_in_flight=settings.render.frames_in_flight;
        configured.preferences=std::make_shared<PlayerPreferences>(values);
        PlayerWindow window(configured,owner,true);
        check_player(window.poll() && window.ready() && window.paused(),window,"Paused graphics initialization failed");
        check(window.report().preferences.applied_revision==0 && window.report().preferences.presented_revision==0,
              "First successful player presentation did not distinguish applied/presented revision");
        check(owner.advances==0 && owner.current_tick==7,"Paused initialization advanced owner");
        const auto first=output/"first.bmp";
        const auto captured=window.capture(utf8(first));
        check(captured.success && captured.hardware && captured.capture_written && captured.validation_errors==0,
              "Actual native hardware capture failed");
        const auto original=bytes(first);
        check(original.size()>54 && original[0]=='B' && original[1]=='M',"Capture is not a real BMP");
        check(owner.advances==0 && owner.current_tick==7,"Capture advanced owner");
        // SDL can deliver initial drawable/display events after the first
        // frame or capture. Establish a stable surface before attributing a
        // later rebuild to a preference update.
        unsigned stable=0;auto settled=window.report();
        for(unsigned attempt=0;attempt<32 && stable<3;++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            check_player(window.poll() && window.ready(),window,"Initial surface failed to settle");
            const auto next=window.report();
            stable=next.swapchain_rebuilds==settled.swapchain_rebuilds &&
                next.render.width==settled.render.width && next.render.height==settled.render.height ? stable+1 : 0;
            settled=next;
        }
        check(stable==3 && owner.current_tick==7 && owner.advances==0,"Initial surface remained unstable or advanced its paused owner");
        check(settled.materials.observed && settled.materials.submitted && settled.materials.presented &&
              settled.materials.presented->scene.material_revision==0 && settled.materials.presented->session_id==owner.session &&
              settled.materials.presented->tick==7 && settled.materials.presented->player_generation==12,
              "Native presentation lost its exact material/session/tick/generation receipt");
        check(!settled.materials.observed->lens_presentation && !settled.materials.submitted->lens_presentation &&
            !settled.materials.presented->lens_presentation && owner.lens_samples==0 &&
            !settled.materials.observed->pose_presentation && !settled.materials.submitted->pose_presentation &&
            !settled.materials.presented->pose_presentation && !settled.materials.presented->scene.camera_pose &&
            owner.pose_samples==0 && owner.pose_snapshots==0,
            "Default-off player emitted interpolation metadata or queried lightweight samples");
        const auto old_presented=*settled.materials.presented;
        owner.material_revision=1;
        check(window.report().materials.observed->scene.material_revision==0,
              "Read-only report polled a new owner snapshot");
        window.synchronize();const auto material_observed=window.report();
        check(material_observed.materials.observed->scene.material_revision==1 &&
              *material_observed.materials.presented==old_presented &&
              material_observed.materials.submitted->scene.material_revision==0,
              "Snapshot observation forged a material frame acknowledgement");
        const auto material_capture=window.capture(utf8(output/"material-epoch.bmp"));
        const auto material_shown=window.report();
        check(material_capture.success && material_capture.diagnostics.scene_submitted &&
              material_capture.diagnostics.scene_presented && material_shown.materials.presented &&
              material_capture.diagnostics.scene_submitted->material_revision==1 &&
              *material_capture.diagnostics.scene_submitted==*material_capture.diagnostics.scene_presented &&
              material_shown.materials.presented->scene==*material_capture.diagnostics.scene_presented &&
              material_shown.materials.presented->tick==7 && owner.current_tick==7 && owner.advances==0,
              "Native capture did not bind its exact latest material snapshot without stepping");
        const auto before_rebuilds=window.report().swapchain_rebuilds;
        configured.preferences->publish(configured.preferences->prepare(R"({"expected_revision":0,"set":{"camera.vertical_fov":90,"ui.scale":2,"input.sensitivity_y":0.25,"input.invert_y":true,"audio.master_gain":0.5,"graphics.samples":4}})"));
        window.synchronize();const auto live=window.report();
        check(live.swapchain_rebuilds==before_rebuilds,"Preference synchronization rebuilt the graphics surface");
        check(!window.ready() && window.paused() && live.preferences.observed_revision==1 &&
              live.preferences.applied_revision==1 && live.preferences.presented_revision==0 &&
              live.preferences.effective_vertical_fov==90 && live.effective_ui_scale==2 &&
              live.preferences.sensitivity_y==.25 && live.preferences.invert_y &&
              live.preferences.requested_master_gain==.5 && !live.preferences.sink_gain &&
              live.preferences.audio_outcome=="disabled" && live.render.samples==settings.render.samples &&
              owner.current_tick==7 && owner.advances==0,
              "Live preferences lost revision/density/FOV authority, forged an audio sink, or rebuilt graphics");
        check_player(window.poll() && window.ready(),window,"Live UI-scale update failed to redraw");
        const auto redrawn=window.report();
        if(redrawn.preferences.presented_revision!=1)
            throw std::runtime_error("Preference redraw failed to present revision1; observed="+
                std::to_string(redrawn.preferences.presented_revision.value_or(0)));
        if(redrawn.swapchain_rebuilds!=before_rebuilds)
            throw std::runtime_error("Preference redraw rebuilt a settled surface: before="+
                std::to_string(before_rebuilds)+" after="+std::to_string(redrawn.swapchain_rebuilds));
        rejects([&]{window.capture(utf8(first));},"Exclusive capture overwrote its first artifact");
        check(bytes(first)==original && owner.current_tick==7,"Failed capture changed artifact or tick");
        check_player(window.poll() && window.ready(),window,"Recoverable capture failure poisoned window");
        owner.session="replacement";owner.current_tick=2;
        window.synchronize();
        check(window.paused() && !window.ready(),"Replacement retained stale presentation/input");
        check(window.report().runtime_replacements==1 && window.report().final_session=="replacement",
              "Replacement identity not observed");
        check(window.report().preferences.applied_revision==1 && window.report().effective_ui_scale==2 &&
              window.report().preferences.effective_vertical_fov==90,
              "Runtime replacement discarded shared presentation preferences");
        check_player(window.poll() && window.ready(),window,"Replacement failed to present");
        const auto restored=window.capture(utf8(output/"replacement.bmp"));
        check(restored.success && owner.current_tick==2 && owner.advances==0,"Replacement capture advanced owner");
        window.request_stop();check(!window.poll(),"Initialized stop did not finish");
        check(window.report().final_tick==2 && window.report().stop_reason=="requested_stop","Final owner boundary stale");
    }
    {
        Owner owner;auto replay=settings;replay.replay=true;
        PlayerSegment segment;segment.ticks=3;segment.input.entity="controller";
        replay.sequence.push_back(segment);PlayerWindow window(replay,owner,true);
        for(unsigned i=0;i<3;++i)check_player(window.poll(),window,"Paused replay terminated early");
        check(window.ready() && owner.advances==0,"Paused replay consumed recorded input");
        window.pause(false);
        for(unsigned i=0;i<3;++i)check_player(window.poll(),window,"Replay terminated before its three ticks");
        check(owner.advances==3 && owner.current_tick==10,"Replay did not commit exactly one tick per poll");
        check(!window.poll() && window.report().stop_reason=="replay_complete","Replay completion missing");
    }
    {
        Owner owner;auto invalid=settings;invalid.render.gpu=4095;
        PlayerWindow failed(invalid,owner,true);
        check(!failed.poll() && failed.finished() && !failed.report().render.success,"Invalid GPU did not terminate cleanly");
        check(owner.advances==0 && owner.current_tick==7,"Initialization failure advanced owner");
    }
    {
        Owner owner;PlayerWindow recovered(settings,owner,true);
        check_player(recovered.poll() && recovered.ready(),recovered,"Previous initialization failure retained graphics lifetime");
        recovered.request_stop();check(!recovered.poll(),"Recovered graphics owner failed to stop");
        check(owner.advances==0,"Recovering graphics initialized simulation");
    }
    if(check_audio) {
        Owner owner;auto launch=settings;launch.audio=true;
        PlayerPreferences::Values inherited;inherited.ui_scale=settings.ui_scale;inherited.samples=settings.render.samples;
        inherited.frames_in_flight=settings.render.frames_in_flight;
        launch.preferences=std::make_shared<PlayerPreferences>(inherited);
        PlayerWindow window(launch,owner,true);
        check_player(window.poll() && window.ready(),window,"Actual SDL stream initialization failed");
        for(const auto gain:{.25,0.,1.}) {
            const auto revision=launch.preferences->snapshot()->revision;
            const auto patch=std::string("{\"expected_revision\":")+std::to_string(revision)+
                ",\"set\":{\"audio.master_gain\":"+std::to_string(gain)+"}}";
            launch.preferences->publish(launch.preferences->prepare(patch));window.synchronize();
            const auto report=window.report();
            check(report.audio.enabled && report.audio.master_gain_applied && report.audio.master_gain==gain &&
                  report.preferences.sink_gain==gain && report.preferences.audio_outcome=="sink_gain_verified" &&
                  report.preferences.applied_revision==revision+1 && owner.current_tick==7 && owner.advances==0,
                  "SDL output gain readback disagreed with live intent or advanced simulation");
        }
        launch.preferences->publish(launch.preferences->prepare(R"({"expected_revision":3,"set":{"audio.master_gain":0.5}})"));
        window.synchronize();owner.session="gain-replacement";owner.current_tick=2;window.synchronize();
        check(window.report().audio.timeline_resets==1 && window.report().audio.master_gain==.5 &&
              window.report().preferences.sink_gain==.5 && window.report().preferences.applied_revision==4,
              "Audio timeline replacement reset output gain or lost preference revision");
        check_player(window.poll() && window.ready(),window,"Gain replacement failed to present");
        window.request_stop();check(!window.poll() && window.report().render.success,"Audio gain owner failed to finish");
        check(owner.advances==0,"SDL gain qualification simulated owner ticks");
        // Empty logical voices qualify the real sink's numeric control only;
        // this is neither an audible listening test nor an offline DSP test.
    }
}
struct CooperativeEvidence {
    unsigned lease_attempts=0,tick_attempts=0,replacement_attempts=0;
};
CooperativeEvidence cooperative_graphics(int gpu,const std::filesystem::path& output) {
    CooperativeEvidence evidence;
    auto launch=options();launch.render.gpu=gpu;launch.player_generation=51;
    {
        Owner owner;PlayerWindow window(launch,owner,true);
        // Real SDL time and the production deadline decide whether to defer.
        // No sleeps, injected focus/events or parallel scheduler establish it.
        auto full=[&] {
            window.pause(true);
            const auto maintenance=owner.owner_polls;
            const auto result=window.poll_cooperative();
            check_player(result.active && result.frame_polled && window.ready() && window.paused(),window,
                "Cooperative paused full poll failed");
            check(owner.owner_polls==maintenance+1 && owner.advances==0 && result.wait_ms<=26,
                "Cooperative full poll duplicated maintenance, advanced or returned excessive pacing");
            return result;
        };
        auto pending_full=[&] {
            for(unsigned attempt=0;attempt<32;++attempt)if(full().wait_ms>0)return;
            check(false,"No pending real idle deadline observed in 32 full polls");
        };
        full();
        bool deferred=false;
        for(unsigned attempt=1;attempt<=32 && !deferred;++attempt) {
            full();
            auto stale=window.capture_snapshot();
            const auto snapshots=owner.snapshots,lenses=owner.lens_samples,maintenance=owner.owner_polls;
            const auto tick=owner.current_tick;const auto before=window.report();
            const auto result=window.poll_cooperative();
            check_player(result.active,window,"Cooperative lease observation terminated");
            check(owner.owner_polls==maintenance+1,"Cooperative poll omitted or duplicated owner maintenance");
            if(result.frame_polled)continue;
            deferred=true;evidence.lease_attempts=attempt;
            const auto after=window.report();
            check(window.ready() && owner.snapshots==snapshots && owner.lens_samples==lenses &&
                  owner.current_tick==tick && owner.advances==0 && after.final_session==before.final_session &&
                  after.final_tick==before.final_tick && after.render.frames_presented==before.render.frames_presented &&
                  after.materials.observed==before.materials.observed && after.materials.submitted==before.materials.submitted &&
                  after.materials.presented==before.materials.presented && result.wait_ms<=26,
                  "Deferred unchanged-owner poll performed scene/Tick/frame work or changed receipts");
            const auto denied=output/"cooperative-stale.bmp";
            rejects([&]{window.capture(utf8(denied),stale);},"Deferred real poll retained a stale capture lease");
            check(!std::filesystem::exists(denied),"Deferred stale lease published an artifact");
        }
        check(deferred,"No genuine deferred lease poll observed in 32 attempts");
        auto frozen=window.capture_snapshot();const auto snapshots=owner.snapshots;
        const auto capture=window.capture(utf8(output/"cooperative-current.bmp"),frozen);
        check(capture.success && capture.hardware && capture.capture_written && capture.validation_errors==0 &&
              capture.diagnostics.scene_presented && capture.diagnostics.scene_submitted &&
              *capture.diagnostics.scene_presented==*capture.diagnostics.scene_submitted &&
              same_renderer_snapshot(*capture.diagnostics.scene_presented,renderer_scene_result(frozen.snapshot())) &&
              owner.snapshots==snapshots && owner.advances==0,
              "Atomic cooperative capture resampled/advanced or presented another scene");
        const auto image=bytes(output/"cooperative-current.bmp");
        check(image.size()>54 && image[0]=='B' && image[1]=='M',"Cooperative capture is not a BMP");

        // Public pause and synchronize must bypass any cached idle deadline.
        pending_full();window.pause(true);
        auto maintenance=owner.owner_polls;auto result=window.poll_cooperative();
        check_player(result.active && result.frame_polled && window.ready(),window,"Pause did not reset cooperative admission");
        check(owner.owner_polls==maintenance+1,"Pause reset poll duplicated maintenance");
        pending_full();window.synchronize();maintenance=owner.owner_polls;result=window.poll_cooperative();
        check_player(result.active && result.frame_polled && window.ready(),window,"Synchronize did not reset cooperative admission");
        check(owner.owner_polls==maintenance+1 && owner.advances==0,"Synchronize reset poll duplicated maintenance or advanced");

        for(const bool replacement:{false,true}) {
            bool observed=false;
            for(unsigned attempt=1;attempt<=32 && !observed;++attempt) {
                full();
                const auto tick=owner.current_tick;const auto before=window.report();
                if(replacement) {
                    owner.maintenance_session="maintenance-"+std::to_string(attempt);
                    owner.maintenance_replace=true;
                }else owner.maintenance_tick=true;
                maintenance=owner.owner_polls;result=window.poll_cooperative();
                check_player(result.active,window,"Cooperative owner-maintenance observation terminated");
                check(owner.owner_polls==maintenance+1 && owner.advances==0 &&
                      !owner.maintenance_tick && !owner.maintenance_replace,
                      "Maintenance change was omitted, duplicated or treated as gameplay");
                const auto after=window.report();
                check(after.final_tick==owner.current_tick && after.final_session==owner.session &&
                      (replacement ? after.runtime_replacements==before.runtime_replacements+1 : owner.current_tick==tick+1),
                      "Maintenance failed to publish the actual owner boundary");
                if(result.frame_polled)continue;
                observed=true;
                (replacement ? evidence.replacement_attempts : evidence.tick_attempts)=attempt;
                check(result.wait_ms==0 && window.paused() && !window.ready() &&
                      after.render.frames_presented==before.render.frames_presented &&
                      after.materials.submitted==before.materials.submitted && after.materials.presented==before.materials.presented,
                      "Deferred maintenance forged presentation or retained its deadline/readiness");
                maintenance=owner.owner_polls;result=window.poll_cooperative();
                check_player(result.active && result.frame_polled && window.ready(),window,
                    "Changed owner was not eligible for the next full poll");
                check(owner.owner_polls==maintenance+1 && owner.advances==0,"Changed-owner redraw duplicated maintenance or advanced");
            }
            check(observed,"No genuine deferred owner change observed in 32 attempts");
        }
        pending_full();
        maintenance=owner.owner_polls;const auto tick=owner.current_tick;
        window.request_stop();result=window.poll_cooperative();
        check(!result.active && result.frame_polled && result.wait_ms==100 && window.finished() &&
              owner.owner_polls==maintenance+1 && owner.current_tick==tick && owner.advances==0 &&
              window.report().stop_reason=="requested_stop" && window.report().render.success,
              "Stop behind a real deadline failed to terminate naturally without gameplay");
        maintenance=owner.owner_polls;result=window.poll_cooperative();
        check(!result.active && !result.frame_polled && result.wait_ms==100 && owner.owner_polls==maintenance+1,
              "Terminal cooperative poll resumed or omitted maintenance");
    }
    {
        Owner owner;auto replay=launch;replay.replay=true;
        PlayerSegment segment;segment.ticks=3;segment.input.entity="controller";replay.sequence.push_back(segment);
        PlayerWindow window(replay,owner,true);
        for(unsigned i=0;i<6;++i) {
            if(i==3)window.pause(false);
            const auto maintenance=owner.owner_polls,advances=owner.advances;
            const auto frames=window.report().render.frames_presented;
            const auto result=window.poll_cooperative();
            check_player(result.active && result.frame_polled && result.wait_ms==0 && window.ready(),window,
                "Drawable cooperative replay deferred or failed to present");
            check(owner.owner_polls==maintenance+1 && owner.advances==advances+(i>=3 ? 1u : 0u) &&
                  window.report().render.frames_presented==frames+1,
                  "Cooperative replay omitted a drawable frame or changed its one-Tick cadence");
        }
        const auto maintenance=owner.owner_polls;const auto result=window.poll_cooperative();
        check(!result.active && result.frame_polled && result.wait_ms==100 && window.finished() &&
              owner.owner_polls==maintenance+1 && owner.advances==3 && owner.current_tick==10 &&
              window.report().stop_reason=="replay_complete" && window.report().render.success,
              "Cooperative replay did not stop naturally at exactly three Ticks");
    }
    return evidence;
}
}
int main(int argc,char** argv) {
    try {
        int gpu=-1;std::filesystem::path output;bool check_audio=false,cooperative_only=false,pose_only=false;
        for(int i=1;i<argc;++i) {
            const std::string_view key=argv[i];check(i+1<argc,"Missing argument value");
            const std::string_view value=argv[++i];
            if(key=="--gpu") {
                check(gpu==-1,"Repeated GPU argument");
                const auto parsed=std::from_chars(value.data(),value.data()+value.size(),gpu);
                check(parsed.ec==std::errc{} && parsed.ptr==value.data()+value.size() && gpu>=0 && gpu<4095,"Invalid GPU argument");
            }else if(key=="--output") {
                check(output.empty(),"Repeated output argument");output=std::filesystem::u8path(std::string(value));
            }else if(key=="--audio") {
                check(value=="0" || value=="1","Audio check expects 0 or 1");check_audio=value=="1";
            }else if(key=="--pose-only") {
                check(value=="0" || value=="1","Pose-only check expects 0 or 1");pose_only=value=="1";
            }else if(key=="--cooperative-only") {
                check(value=="0" || value=="1","Cooperative-only check expects 0 or 1");cooperative_only=value=="1";
            }else check(false,"Unknown argument");
        }
        check((gpu<0)==output.empty(),"GPU check requires both --gpu and new --output");
        check(!check_audio || gpu>=0,"Audio check requires an actual graphics owner");
        check(!cooperative_only || (gpu>=0 && !check_audio),"Cooperative-only requires a GPU and excludes the separate audio group");
        check(!pose_only || (gpu>=0 && !check_audio && !cooperative_only),"Pose-only requires GPU and excludes audio/cooperative modes");
        basic();preferences_without_window();lens_admission_without_window();pose_admission_without_window();
        if(pose_only) {
            check(!std::filesystem::exists(output),"Output directory must be new");
            check(std::filesystem::create_directories(output),"Cannot create owned pose output directory");
            pose_capture_graphics(gpu,output);
            std::cout<<"{\"passed\":true,\"pose_admission_groups\":1,\"pose_window_groups\":1,"
                "\"captures\":2,\"lens_pose_coexistence\":true,\"physics_oracle\":false,\"compiled_game\":false,\"physical_input\":false}\n";
            return 0;
        }
        CooperativeEvidence cooperative;
        if(gpu>=0) {
            check(!std::filesystem::exists(output),"Output directory must be new");
            check(std::filesystem::create_directories(output),"Cannot create owned output directory");
            if(!cooperative_only) {
                graphics(gpu,output,check_audio);
                lens_capture_graphics(gpu,output);
            }
            cooperative=cooperative_graphics(gpu,output);
        }
        std::cout<<"{\"passed\":true,\"basic_groups\":5,\"graphics_groups\":"<<(gpu>=0 && !cooperative_only ? 4 : 0)
                 <<",\"lens_admission_groups\":1,\"lens_capture_token_groups\":"<<(gpu>=0 && !cooperative_only ? 1 : 0)
                 <<",\"lens_catchup_groups\":"<<(gpu>=0 && !cooperative_only ? 1 : 0)
                 <<",\"cooperative_groups\":"<<(gpu>=0 ? 3 : 0)
                 <<",\"cooperative_lease_attempts\":"<<cooperative.lease_attempts
                 <<",\"cooperative_tick_attempts\":"<<cooperative.tick_attempts
                 <<",\"cooperative_replacement_attempts\":"<<cooperative.replacement_attempts
                 <<",\"sdl_gain_groups\":"<<(check_audio ? 1 : 0)
                 <<",\"audible_qualification\":false,\"physical_input\":false,\"physics_oracle\":false}\n";
        return 0;
    }catch(const std::exception& error) {
        std::cerr<<error.what()<<'\n';return 1;
    }
}
