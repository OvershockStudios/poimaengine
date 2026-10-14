# SPDX-License-Identifier: Apache-2.0
"""Independent Switchback input rules and complete native checkpoint comparisons."""
from dataclasses import dataclass, field, asdict
from pathlib import Path
import copy
import hashlib
import json
import math
import re
import threading
import time
import uuid

SEMANTICS = ("Phase", "Supply", "ClearedMask", "Ammo", "Shots", "Rejected",
             "Health", "Timer", "MenuOpen", "ReloadRemaining")
ACTIONS = ("aim", "crouch", "fire", "reload")
CHECKPOINT_LABELS = ("mid-reload-and-ADS", "same-owner-restored",
                     "cold-reference-restored", "cold-fresh-restored",
                     "cold-reference-continued", "cold-fresh-continued",
                     "menu-checkpoint", "menu-restored", "completed")
EXACT_PAIRS = ((0, 1), (0, 2), (0, 3), (4, 5), (6, 7))


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def identifier(value):
    require(type(value) is str and re.fullmatch(r"[0-9a-f]{32}", value), "Expected native entity/UI identity")
    return value


def number(value):
    require(type(value) in (float, int) and math.isfinite(value), "Expected finite native number")
    return float(value)


def vector(value, length):
    require(type(value) is list and len(value) == length, "Expected exact native vector shape")
    return tuple(number(x) for x in value)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def file_row(path, limit=256 * 1048576):
    path = Path(path)
    require(path.is_file() and not path.is_symlink() and not getattr(path, "is_junction", lambda: False)(), "Regular retained file required")
    require(path.stat().st_size <= limit, "Retained file exceeds declared bound")
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    return {"bytes": path.stat().st_size, "sha256": digest}


def strict_json(data):
    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, "Duplicate JSON member")
            result[key] = value
        return result
    value = json.loads(data, object_pairs_hook=pairs,
                       parse_constant=lambda _: (_ for _ in ()).throw(AssertionError("Nonfinite JSON")))
    def bounded(item, depth=0):
        require(depth <= 64, "JSON nesting exceeds 64")
        if type(item) is float: require(math.isfinite(item), "Nonfinite JSON number")
        if isinstance(item, dict):
            for nested in item.values(): bounded(nested, depth+1)
        elif isinstance(item, list):
            for nested in item: bounded(nested, depth+1)
    bounded(value)
    return value


def read_handoff(path):
    """Read identities and presentation metadata; gameplay rules stay independent."""
    require(file_row(path, 1048576)["bytes"] > 0, "Empty handoff")
    value = strict_json(Path(path).read_bytes())
    require(set(value) == {"format", "roles", "fields", "ui", "b_binding", "presentation"}, "Unexpected normalized handoff members")
    require(value["format"] == "poima.switchback-exercise.v1", "Wrong exercise format")
    roles = value["roles"]
    require(set(roles) == {"player", "camera", "target_a", "target_b", "rig_b", "region_a", "region_b", "pickup_hit_emitter", "completion_emitter"}, "Incomplete game roles")
    for entity in roles.values():
        identifier(entity)
    actor_roles = ("player", "camera", "target_a", "target_b", "rig_b")
    require(len({roles[x] for x in actor_roles}) == len(actor_roles), "Unrelated actor roles alias")
    # A may own its HitRegion on itself. B may put the component on its RigNode.
    require(roles["region_a"] not in {roles[x] for x in ("player", "camera", "target_b", "rig_b")}
            and roles["region_b"] not in {roles[x] for x in actor_roles}, "Target region aliases an unrelated actor")
    fields = value["fields"]
    require(set(fields) == set(SEMANTICS), "Incomplete semantic field map")
    require(all(type(x) is str and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]{0,63}", x) for x in fields.values()), "Invalid scalar field name")
    require(len(set(fields.values())) == len(fields), "Semantic fields alias")
    require(set(value["ui"]) == {"begin", "menu", "save", "load", "resume", "retry"}, "Incomplete control roles")
    for entry in value["ui"].values():
        identifier(entry)
    require(len(set(value["ui"].values())) == 6, "Declared controls alias")
    require(roles["pickup_hit_emitter"] != roles["completion_emitter"], "Distinct cue emitter roles required")
    binding = value["b_binding"]
    require(set(binding) == {"asset", "node", "node_name", "node_path", "rig_node", "skin", "idle_clip", "run_clip"}, "Incomplete imported binding")
    require(type(binding["asset"]) is str and re.fullmatch(r"[0-9a-f]{64}", binding["asset"]), "Invalid model asset selector")
    for key in ("node", "skin", "idle_clip", "run_clip"):
        require(type(binding[key]) is int and 0 <= binding[key] < 2 ** 32 - 1, "Invalid imported index")
    require(binding["idle_clip"] != binding["run_clip"], "Idle/Run must be distinct imported clips")
    require(type(binding["node_name"]) is str and 1 <= len(binding["node_name"]) <= 1024 and type(binding["node_path"]) is list and 1 <= len(binding["node_path"]) <= 1024, "Missing inspected node path")
    require(all(type(x) is int and 0 <= x < 2 ** 32 - 1 for x in binding["node_path"]), "Invalid inspected ancestry indices")
    require(binding["node_path"][-1] == binding["node"], "Node path must terminate at selected node")
    require(len(set(binding["node_path"])) == len(binding["node_path"]), "Imported ancestry cannot contain a cycle")
    require(binding["rig_node"] == roles["region_b"], "B query region must use its declared imported rig node")
    identifier(binding["rig_node"])
    require(binding["rig_node"] not in {roles["player"], roles["camera"], roles["target_a"], roles["target_b"]}, "Rig node aliases an actor")
    presentation = value["presentation"]
    require(type(presentation) is dict and set(presentation) == {"save_status_field", "save_status_complete"}, "Unexpected presentation mapping")
    require(type(presentation["save_status_field"]) is str and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]{0,63}", presentation["save_status_field"]), "Invalid save status field")
    require(type(presentation["save_status_complete"]) is int, "Save success must be a discovered integer presentation value")
    require(presentation["save_status_field"] not in fields.values(), "Save presentation aliases a gameplay semantic")
    return value


def inspect_import_binding(owner):
    """Rejoin declared identities with native authored/imported metadata.

    This does not read source art, infer anatomy or qualify skin-weight topology.
    It rejects contradictory node/name/ancestry/skin/clip and region declarations.
    """
    handoff = owner.handoff
    roles, binding = handoff["roles"], handoff["b_binding"]
    revision = owner.rpc("world.inspect")["revision"]
    def authored(entity):
        receipt = owner.rpc("entity.get", id=entity, revision=revision)
        require(receipt["revision"] == revision and receipt["id"] == entity, "Foreign authored entity receipt")
        return receipt["value"]
    rig = authored(roles["rig_b"])
    require(rig["parent"] == roles["target_b"] and rig["components"]["AnimationRig"]["asset"] == binding["asset"], "Declared B rig does not use its imported asset")
    region = authored(roles["region_b"])
    require(region["components"]["RigNode"] == {"rig": roles["rig_b"], "node": binding["node"]}, "Declared imported node does not match the authored binding")
    hit_region = region["components"]["HitRegion"]
    require(hit_region["owner"] == roles["target_b"] and hit_region["name"] == "target" and hit_region.get("enabled", True) is True, "B query region ownership differs")
    def imported(section, index):
        page = owner.rpc("asset.inspect", asset=binding["asset"], section=section, offset=index, limit=1)
        require(page["asset"] == binding["asset"] and len(page["items"]) == 1 and page["items"][0]["index"] == index, "Declared imported index is absent")
        return page["items"][0]
    ancestry = []
    for offset, index in enumerate(binding["node_path"]):
        row = imported("nodes", index)
        require(row["parent"] == (-1 if offset == 0 else binding["node_path"][offset-1]), "Declared node ancestry differs from the imported asset")
        ancestry.append(row)
    require(ancestry[-1]["name"] == binding["node_name"], "Declared node name differs from the imported node")
    skin = imported("skins", binding["skin"])
    require(skin["joints"] > 0, "Declared imported skin has no joints")
    clips = {}
    for semantic, name in (("idle_clip", "Idle"), ("run_clip", "Run")):
        row = imported("animations", binding[semantic])
        require(row["name"] == name and number(row["duration"]) > 0 and row["channels"] > 0, "Declared original Idle/Run clip differs")
        clips[semantic] = row
    selected = []
    after = None
    for _ in range(16):
        params = {"revision": revision, "component": "SkinnedMesh", "limit": 64}
        if after is not None: params["after"] = after
        page = owner.rpc("entity.query", **params)
        require(page["revision"] == revision, "Skinned entity membership changed")
        for item in page["entities"]:
            value = authored(item["id"])
            mesh = value["components"]["SkinnedMesh"]
            if mesh["rig"] != roles["rig_b"]: continue
            require(mesh["asset"] == binding["asset"] and mesh.get("visible", True) is True, "B skinned geometry asset/visibility differs")
            node = imported("nodes", mesh["node"])
            primitive = imported("primitives", mesh["primitive"])
            require(node["skin"] == binding["skin"] and mesh["primitive"] in node["primitives"], "B skin/primitive mapping differs")
            require(primitive["skinned"] is True and primitive["vertices"] > 0 and primitive["triangles"] > 0, "B imported geometry lacks real skin influences")
            selected.append({"entity": item["id"], "mesh": mesh, "node": node, "primitive": primitive})
        next_after = page["next_after"]
        if next_after is None: break
        identifier(next_after)
        require(after is None or next_after > after, "Skinned query pagination did not advance")
        after = next_after
    else:
        raise AssertionError("Skinned entity membership exceeds the verifier's bounded fixture scope")
    require(selected, "No visible imported skinned geometry uses B's rig")
    return {"revision": revision, "rig": roles["rig_b"], "region": roles["region_b"],
            "ancestry": ancestry, "skin": skin, "clips": clips, "meshes": selected,
            "weight_topology_verified": False, "rendered_animation_verified": False}


def camera_ray(matrix):
    matrix = vector(matrix, 16)
    origin = matrix[12:15]
    raw = tuple(-matrix[i] for i in (8, 9, 10))
    length = math.sqrt(sum(x * x for x in raw))
    require(length > 1e-12, "Degenerate camera direction")
    return list(origin), [x / length for x in raw]


def perspective_curve(start, target, elapsed):
    """Independent fractional-power oracle, not SDK log/exp source reuse."""
    require(5 <= number(start) <= 150 and 5 <= number(target) <= 150, "Invalid FOV endpoint")
    require(type(elapsed) is int and 0 <= elapsed <= 12, "Invalid task transition phase")
    if elapsed == 0 or start == target:
        return float(start)
    if elapsed == 12:
        return float(target)
    # At quarter phase the exponent is exactly 5/32; at midpoint exactly 1/2.
    weight = (elapsed * elapsed * (36 - 2 * elapsed)) / 1728
    a, b = math.tan(math.radians(start) / 2), math.tan(math.radians(target) / 2)
    return math.degrees(2 * math.atan(a ** (1 - weight) * b ** weight))


@dataclass
class Golden:
    Phase: int = 0
    Supply: int = 0
    ClearedMask: int = 0
    Ammo: int = 0
    Shots: int = 0
    Rejected: int = 0
    Health: int = 3
    Timer: int = 0
    MenuOpen: int = 0
    ReloadRemaining: int = 0
    world_start: float = 70.0
    world_target: float = 70.0
    vm_start: float = 75.0
    vm_target: float = 75.0
    ads_elapsed: int = 12
    aim: bool = False
    hazard_inside: bool = False
    accepted_cues: list = field(default_factory=list)

    def lenses(self):
        return (perspective_curve(self.world_start, self.world_target, self.ads_elapsed),
                perspective_curve(self.vm_start, self.vm_target, self.ads_elapsed))

    def control(self, role):
        """Only deterministic controls; loads replace this model from saved copy."""
        if role == "begin":
            require(self.Phase == 0, "Begin outside Ready")
            self.Phase = 1
        elif role == "menu":
            require(self.Phase == 1 and self.MenuOpen == 0, "Menu outside running game")
            self.MenuOpen = 1
        elif role == "resume":
            require(self.Phase == 1 and self.MenuOpen == 1, "Resume outside Menu")
            self.MenuOpen = 0
        elif role == "save":
            require(self.MenuOpen == 1, "Menu save outside Menu")
        else:
            raise AssertionError("Restore controls require a retained full golden checkpoint")

    def tick(self, observation, actions, use=False):
        """All inputs are callback-entry native observations, never after-physics."""
        require(set(actions) == set(ACTIONS), "Incomplete semantic action record")
        require(type(use) is bool, "Use edge must be bool")
        if self.Phase != 1 or self.MenuOpen:
            return
        self.Timer += 1
        aim = actions["aim"]["held"]
        if aim != self.aim:
            self.world_start, self.vm_start = self.lenses()
            self.world_target, self.vm_target = (35.0, 55.0) if aim else (70.0, 75.0)
            self.aim, self.ads_elapsed = aim, 0
        else:
            self.ads_elapsed = min(12, self.ads_elapsed + 1)
        if self.ReloadRemaining:
            self.ReloadRemaining -= 1
            if not self.ReloadRemaining:
                self.Ammo = 4
        if actions["reload"]["pressed"]:
            if self.Supply and self.Ammo < 4 and not self.ReloadRemaining:
                self.ReloadRemaining = 12
            else:
                self.Rejected += 1
        x, y, z = observation["foot"]
        floor = abs(y) <= .05
        inside = floor and x * x + (z + 4) ** 2 <= .4 ** 2
        if inside and not self.hazard_inside:
            self.Health -= 1
        self.hazard_inside = inside
        if self.Health == 0:
            self.Phase = 2
            return
        if use:
            supply = (floor and (x + 3) ** 2 + z * z <= .5 ** 2
                      and -4 <= x <= -2 and -.8 <= z <= .8 and observation["crouched"])
            finish = floor and x * x + (z - 5) ** 2 <= .6 ** 2
            if supply and not self.Supply:
                self.Supply, self.Ammo = 1, 4
                self.accepted_cues.append("pickup")
            elif finish and self.Supply and self.ClearedMask == 3:
                self.Phase = 3
                self.accepted_cues.append("win")
                return
            else:
                self.Rejected += 1
        if actions["fire"]["pressed"]:
            if self.Ammo == 0 or self.ReloadRemaining:
                self.Rejected += 1
            else:
                self.Ammo -= 1
                self.Shots += 1
                bit = observation["hit_bit"]
                require(bit in (0, 1, 2), "Unknown independently observed hit role")
                if self.Supply and bit and not self.ClearedMask & bit:
                    self.ClearedMask |= bit
                    self.accepted_cues.append("hit")
                else:
                    self.Rejected += 1

    def assert_observed(self, values, lenses, fields):
        for semantic in SEMANTICS:
            actual = values[fields[semantic]]
            require(type(actual) is int and actual == getattr(self, semantic), "Golden mismatch: " + semantic)
        world, vm = self.lenses()
        require(abs(number(lenses["world"]["vertical_fov"]) - world) <= 1e-8, "Authoritative world ADS differs")
        require(lenses["viewmodel"] is not None and abs(number(lenses["viewmodel"]["vertical_fov"]) - vm) <= 1e-8, "Authoritative foreground ADS differs")
        for lens, near, far in ((lenses["world"], .1, 80), (lenses["viewmodel"], None, None)):
            # Foreground clipping is checked separately by renderer qualification.
            if near is not None:
                require(abs(number(lens["near"]) - near) <= 1e-8 and number(lens["far"]) == far, "World clips changed")


class ActionFrames:
    def __init__(self):
        self.held = {name: False for name in ACTIONS}

    def next(self, **held):
        require(set(held) <= set(ACTIONS) and all(type(x) is bool for x in held.values()), "Invalid requested named actions")
        new = {name: held.get(name, self.held[name]) for name in ACTIONS}
        records = {name: {"held": new[name], "pressed": new[name] and not self.held[name],
                          "released": self.held[name] and not new[name]} for name in ACTIONS}
        self.held = new
        return records


class Owner:
    """Standalone explicit-Tick owner. Continuous player scope is not provided.

    admission() must verify the verifier's already assigned known Windows Job and
    frozen input/native/SDK closure. Calling it is mandatory before SDK import.
    Explicit native_command selects a literal argv prefix, not shell text.
    command_admission(argv) must validate that complete command against the
    verifier's frozen selection before SDK import or process construction. It may
    raise on rejection; returning False also rejects. The legacy single-binary
    route retains admission() and may additionally select a command validator.
    The verifier owns output-size/deadline/process-tree final gates.
    This library supplies neither a shared host nor an executable campaign.
    """
    def __init__(self, binary, world, handoff, admission, deadline, raw_limit=50000,
                 *, native_command=None, development_profiles=None,
                 command_admission=None):
        require(callable(admission), "Owned process admission callback required")
        admission()
        require(time.monotonic() < deadline and type(raw_limit) is int
                and 0 < raw_limit <= 50000, "Invalid frozen owner budget")
        if native_command is None:
            prefix = [str(binary)]
        else:
            require(type(native_command) in (list, tuple) and native_command,
                    "Explicit native command must be a nonempty argv prefix")
            prefix = list(native_command)
            require(callable(command_admission),
                    "Selected native prefix requires verifier command admission")
        require(all(type(x) is str and x and "\0" not in x for x in prefix),
                "Native prefix requires literal nonempty argv strings")
        world_path = str(world)
        require(world_path and "\0" not in world_path, "Invalid native world path")
        argv = [*prefix, "world", world_path]
        profile_path = None
        if development_profiles is not None:
            profile_path = str(development_profiles)
            require(profile_path and "\0" not in profile_path,
                    "Invalid native development profile path")
            argv.extend(["--development-profiles", profile_path])
        if command_admission is not None:
            require(callable(command_admission), "Verifier command admission must be callable")
            require(command_admission(tuple(argv)) is not False,
                    "Verifier rejected selected native command")
        self.native_command = tuple(prefix)
        self.development_profiles = profile_path
        self.argv = tuple(argv)
        from poima_client import WorldClient, JsonRpcProcess
        self.calls, self.raw, self.raw_bytes = [], [], 0
        self.lock = threading.Lock()
        self.deadline, self.raw_limit = deadline, raw_limit
        self.handoff = handoff
        owner = self

        class Recorded(JsonRpcProcess):
            def _accept_response(self, response):
                encoded = json.dumps(response, allow_nan=False).encode()
                with owner.lock:
                    require(len(owner.raw) < owner.raw_limit and owner.raw_bytes + len(encoded) <= 64 * 1048576, "Raw response budget exceeded")
                    owner.raw.append(copy.deepcopy(response))
                    owner.raw_bytes += len(encoded)
                return super()._accept_response(response)

        self.transport = Recorded(list(self.argv), max_pending=1,
                                  stderr_limit=65536, close_timeout=9)
        self.client = WorldClient(self.transport)
        self.forced = []
        # Observe the SDK's unchanged cleanup policy; never count forced exit as healthy.
        for name in ("terminate", "kill"):
            original = getattr(self.transport._process, name)
            def watched(*args, _name=name, _original=original, **kwargs):
                self.forced.append(_name)
                return _original(*args, **kwargs)
            setattr(self.transport._process, name, watched)
        self.session = None

    def rpc(self, method, **params):
        require(time.monotonic() < self.deadline, "Owned campaign deadline")
        with self.transport._condition:
            rpc_id = self.transport._next_id
        record = {"id": rpc_id, "method": method, "params": copy.deepcopy(params)}
        self.calls.append(record)
        try:
            result = self.client.call(method, params, timeout=min(20, self.deadline - time.monotonic()))
            record["result"] = copy.deepcopy(result)
            return result
        finally:
            with self.lock:
                matching = [x for x in self.raw if x.get("id") == rpc_id]
            require(len(matching) == 1 and matching[0].get("jsonrpc") == "2.0", "Missing/duplicate native raw response; unknown outcome fails")
            require(("result" in matching[0]) != ("error" in matching[0]), "Malformed native envelope")
            if "error" in matching[0]:
                record["error"] = copy.deepcopy(matching[0]["error"])
            else:
                require(record.get("result") == matching[0]["result"], "Typed/native result mismatch")

    def inspect(self):
        require(self.session is not None, "Owned active session required")
        value = self.rpc("runtime.inspect", session_id=self.session)
        require(value["session_id"] == self.session, "Foreign runtime receipt")
        return value

    def start(self, revision):
        self.session = uuid.uuid4().hex
        result = self.rpc("runtime.start", session_id=self.session, revision=revision)
        require(result["session_id"] == self.session and result["tick"] == 0 and result["started"] is True, "Bad native start")

    def observe(self, query_hit=True):
        runtime = self.inspect()
        tick = runtime["tick"]
        roles = self.handoff["roles"]
        def entity(role):
            return self.rpc("runtime.entity", session_id=self.session, tick=tick,
                            structure_revision=runtime["structure_revision"], id=roles[role])
        player, camera, target = entity("player"), entity("camera"), entity("target_b")
        stance = self.rpc("runtime.character_stance", session_id=self.session, tick=tick, entity=roles["player"])["state"]
        lenses = self.rpc("runtime.camera_lens", session_id=self.session, tick=tick, entity=roles["camera"])["state"]
        module = self.rpc("runtime.gameplay.inspect", session_id=self.session, tick=tick, include_schema=True)["module"]
        require(module is not None and stance is not None and stance["configured"] is True, "Missing real game/profile")
        ray_origin, ray_direction = camera_ray(camera["world_matrix"])
        hit = self.rpc("runtime.hit_test", session_id=self.session, tick=tick, origin=ray_origin,
                       direction=ray_direction, distance=20, ignore=[roles["player"]])["hit"] if query_hit else None
        hit_bit = 0
        if hit is not None:
            require(0 <= number(hit["distance"]) <= 20 and 0 <= number(hit["fraction"]) <= 1, "Invalid native hit distance")
            if hit["region"] == "target":
                for role, bit in (("target_a", 1), ("target_b", 2)):
                    if hit["entity"] == roles[role]:
                        require(hit["region_entity"] == roles["region_a" if bit == 1 else "region_b"], "Wrong target region binding")
                        hit_bit = bit
        require(self.inspect()["tick"] == tick, "Read-only observation advanced physics")
        return {"runtime": runtime, "module": module, "player": player, "camera": camera,
                "target_b": target, "stance": stance, "lenses": lenses, "hit": hit,
                "foot": list(vector(player["world_matrix"], 16)[12:15]),
                "crouched": stance["crouched"], "hit_bit": hit_bit}

    def step(self, records, move=(0, 0), look=(0, 0), use=False, jump=False, ticks=1):
        require(set(records) == set(ACTIONS), "Complete named action record required")
        for item in records.values():
            require(set(item) == {"held", "pressed", "released"} and all(type(v) is bool for v in item.values()), "Malformed action record")
        require(type(ticks) is int and 1 <= ticks <= 600, "Invalid step count")
        runtime = self.inspect()
        result = self.rpc("runtime.step", session_id=self.session, request_id=uuid.uuid4().hex,
                          expected_tick=runtime["tick"], expected_structure_revision=runtime["structure_revision"], ticks=ticks,
                          inputs=[{"entity": self.handoff["roles"]["player"], "move": list(move), "look": list(look),
                                   "jump": jump, "use": use,
                                   "actions": [{"name": name, **records[name]} for name in sorted(ACTIONS)]}])
        require(result["runtime_replaced"] is False and result["previous_tick"] == runtime["tick"]
                and result["tick"] == runtime["tick"] + ticks, "Unexpected replacement/step interval")
        return result

    def control(self, role):
        require(role in self.handoff["ui"], "Unknown declared UI role")
        state = self.inspect()
        game = self.rpc("runtime.gameplay.inspect", session_id=self.session, tick=state["tick"])
        result = self.rpc("runtime.ui.activate", session_id=self.session, request_id=uuid.uuid4().hex,
                          expected_tick=state["tick"], expected_ui_revision=state["ui_revision"],
                          expected_control_sequence=state["control_sequence"], expected_gameplay_revision=game["revision"],
                          expected_structure_revision=state["structure_revision"], id=self.handoff["ui"][role])
        require(result["session_id"] == self.session and result["tick"] == state["tick"], "Control receipt has foreign source session/tick")
        if result["runtime_replaced"]:
            require(result["current_session_id"] != self.session, "Replacement reused its source session")
            self.session = identifier(result["current_session_id"])
        else:
            require(result["current_session_id"] == self.session and result["current_tick"] == state["tick"], "Control advanced authoritative Tick")
        return result

    def checkpoint(self, configuration_generation, save_root, label):
        require(label in CHECKPOINT_LABELS, "Unexpected canonical checkpoint")
        # Independent slots do not overwrite the game's switchback-checkpoint.
        slot = "oracle-" + str(CHECKPOINT_LABELS.index(label) + 1)
        state = self.inspect()
        game = self.rpc("runtime.gameplay.inspect", session_id=self.session, tick=state["tick"])
        selected = self.rpc("save.inspect", slot=slot)["selected"]
        expected_generation = 0 if selected is None else selected["generation"]
        receipt = self.rpc("save.write", request_id=uuid.uuid4().hex,
                           configuration_generation=configuration_generation, slot=slot,
                           expected_generation=expected_generation, session_id=self.session,
                           expected_tick=state["tick"], expected_gameplay_revision=game["revision"],
                           expected_structure_revision=state["structure_revision"], expected_material_revision=state["material_revision"],
                           expected_ui_revision=state["ui_revision"], expected_control_sequence=state["control_sequence"])
        inspected = self.rpc("save.inspect", slot=slot)["selected"]
        require(inspected is not None and inspected["verified"] is True
                and all(inspected[k] == receipt[k] for k in ("generation", "bytes", "sha256")), "Selected checkpoint/native write mismatch")
        require(self.inspect()["tick"] == state["tick"], "Checkpoint advanced physics")
        return read_checkpoint(save_root, slot, receipt)

    def close(self):
        self.client.close()
        require(not self.forced and self.transport.returncode == 0 and not self.transport.stderr_tail
                and not self.transport.stderr_truncated and self.transport.closed, "Native owner did not close naturally/cleanly")
        require(len(self.calls) == len(self.raw), "Raw native population differs")


class CheckedSession:
    """One-Tick golden/input driver; no shortcuts that silently advance gameplay."""
    def __init__(self, owner, golden=None):
        self.owner = owner
        self.golden = copy.deepcopy(golden) if golden is not None else Golden()
        self.frames = ActionFrames()
        self.records = []

    def tick(self, move=(0, 0), look=(0, 0), use=False, jump=False, **held):
        # Nonzero look changes the callback ray before HitTest. Do not fire in
        # that Tick; aim on a separate Tick, then query/Fire with zero look.
        actions = self.frames.next(**held)
        require(not actions["fire"]["pressed"] or tuple(look) == (0, 0), "Fire/aim observation boundaries must be separate")
        before = self.owner.observe()
        candidate = copy.deepcopy(self.golden)
        candidate.tick(before, actions, use=use)
        result = self.owner.step(actions, move=move, look=look, use=use, jump=jump)
        after = self.owner.observe(query_hit=False)
        candidate.assert_observed(after["module"]["values"], after["lenses"], self.owner.handoff["fields"])
        self.golden = candidate
        self.records.append({"tick": before["runtime"]["tick"], "input": {"move": list(move), "look": list(look), "use": use, "jump": jump, "actions": actions},
                             "before": before, "result": result, "after": after, "golden": asdict(candidate)})
        return after

    def aim_at(self, point, **held):
        before = self.owner.observe(query_hit=False)
        origin, _ = camera_ray(before["camera"]["world_matrix"])
        direction = [number(point[i]) - origin[i] for i in range(3)]
        length = math.sqrt(sum(x * x for x in direction))
        require(length > .01, "Aim point too close")
        yaw = math.degrees(math.atan2(-direction[0], -direction[2]))
        pitch = math.degrees(math.asin(direction[1] / length))
        require(-85 <= pitch <= 85, "Task target outside supported pitch")
        dy = (yaw - number(before["player"]["yaw"]) + 180) % 360 - 180
        dp = pitch - number(before["player"]["pitch"])
        return self.tick(look=(dy, dp), **held)

    def drive_to(self, x, z, *, max_ticks, tolerance=.08, **held):
        """Measured native movement; the caller supplies safe route waypoints."""
        require(type(max_ticks) is int and 1 <= max_ticks <= 1200 and .02 <= tolerance <= .2, "Frozen navigation bounds required")
        for _ in range(max_ticks):
            before = self.owner.observe(query_hit=False)
            dx, dz = number(x) - before["foot"][0], number(z) - before["foot"][2]
            distance = math.hypot(dx, dz)
            if distance <= tolerance:
                return before
            yaw = math.radians(number(before["player"]["yaw"]))
            # Invert the native Y-rotation: world = right*move.x + forward*move.y.
            right = (math.cos(yaw), -math.sin(yaw))
            forward = (-math.sin(yaw), -math.cos(yaw))
            speed = 2 if before["crouched"] else 4
            magnitude = min(1, distance / (speed / 60))
            move = (magnitude * (dx * right[0] + dz * right[1]) / distance,
                    magnitude * (dx * forward[0] + dz * forward[1]) / distance)
            self.tick(move=move, **held)
        raise AssertionError("Native waypoint deadline; do not teleport or silently change route")


def read_checkpoint(save_root, slot, receipt):
    """Rejoin native save receipt/current manifest and retain the FULL snapshot."""
    require(type(slot) is str and re.fullmatch(r"[a-z0-9][a-z0-9_-]{0,31}", slot), "Portable save slot required")
    directory = Path(save_root) / ("slot-" + slot)
    current_path = directory / "current.json"
    file_row(current_path, 1048576)
    envelope = strict_json(current_path.read_bytes())
    current = envelope["payload"]["current"]
    name = current["file"]
    require(type(name) is str and re.fullmatch(r"p-[0-9a-f]{32}\.bin", name), "Unexpected durable payload name")
    path = directory / name
    row = file_row(path, 64 * 1048576)
    require(row == {"bytes": current["bytes"], "sha256": current["sha256"]}, "Durable manifest payload differs")
    require(current["generation"] == receipt["generation"] and row == {k: receipt[k] for k in ("bytes", "sha256")}, "Native save receipt differs")
    payload = strict_json(path.read_bytes())
    require(payload["format"] == "poima.world-save" and payload["version"] == 1
            and payload["snapshot"]["version"] == 9, "Explicit virtual profile requires complete v9 snapshot")
    return {"receipt": copy.deepcopy(receipt), "manifest": file_row(current_path),
            "payload": row, "snapshot": payload["snapshot"], "document": payload["document"]}


def exact_checkpoint(left, right):
    """Compare all snapshot/document fields, preserving JSON number/bool types.

    Python's ordinary equality accepts True == 1 and 1 == 1.0. Stable JSON
    encoding keeps those distinctions without excluding any session/state field.
    Object member order is immaterial; list order remains significant.
    """
    def canonical(value):
        return json.dumps(value, sort_keys=True, ensure_ascii=False, allow_nan=False, separators=(",", ":"))
    return all(canonical(left[section]) == canonical(right[section]) for section in ("snapshot", "document"))


def assert_nine_checkpoints(rows):
    require(set(rows) == set(CHECKPOINT_LABELS), "Nine canonical native checkpoints required")
    for left, right in EXACT_PAIRS:
        a, b = rows[CHECKPOINT_LABELS[left]], rows[CHECKPOINT_LABELS[right]]
        require(exact_checkpoint(a, b), "Full cold/native checkpoint mismatch; no exclusions allowed")

