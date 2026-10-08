# Authored HUD and menu

`authored-hud.json` is a native UI definition map, not a complete game. It combines
a centered pause menu and a bottom-left status HUD. Dimensions, responsive
placement, control order, colors and state colors are authored data. No interpreted
gameplay, CSS, scripts or external resources are included.

With the Python client installed and an engine built with
`POIMA_ENABLE_GAME_UI=ON`, import it into a scratch world:

```python
import json
from pathlib import Path
from poima_client import WorldClient

controls = json.loads(Path("examples/ui/authored-hud.json").read_text())
with WorldClient.open("build/game-ui/poima", "build/ui-example/world.json") as client:
    revision = client.inspect()["revision"]
    result = client.transact([
        {"op": "ui.element.set", "id": identity, "element": element}
        for identity, element in controls.items()
    ], revision)
    print(client.call("world.ui.layout", {
        "revision": result["revision"], "width": 1280, "height": 720, "scale": 1
    }))
```

Create `build/ui-example` before running this example. The default headless build
can author and inspect the definitions but does not include native layout.
Buttons declare `resume` and `save` action tokens. A game must implement compiled
control callbacks before those actions perform gameplay. This fixture does not
include a camera, controller, save configuration or gameplay module.

[UI fields and limits](../../docs/GAME_UI.md#authored-layout-and-styling),
[Python client setup](../../docs/PYTHON_CLIENT.md).

Qualification harnesses:

```sh
python3 tests/ui_authoring_contract.py build/game-ui/poima --layout-available 1
python3 tests/ui_authoring_capture.py build/windows-runtime/poima.exe --windows-interop --output build/ui-example-captures --gpu 0 --samples 4
```

The capture harness requires simulation, native layout and Vulkan rendering; it
creates its own world and checks opaque colors, three viewport sizes (16:9 and 4:3), frozen
metadata and same-tick text updates. It does not execute gameplay callbacks or
qualify physical input or the desktop editor.
