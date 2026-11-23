<sub>**NOTE**: This repository has undergone a major rewrite since the initial release by mimi. If you're looking for the latest version of that initial release, you can find that in the `legacy` branch.</sub>

# ets2la_controller

ets2la_controller is a DLL plugin for Euro Truck Simulator 2 and American Truck Simulator that lets any number of clients send inputs to the game through shared memory, with minimal latency between a client writing a value and the game applying it.

Every input, its type and what it does, see `docs/CONTROLS.md`.

## Build

Requires CMake 3.15+ and MSVC (Visual Studio 2019+).

```powershell
cmake -B build -A x64
cmake --build build --config Release
```

Outputs `ets2la_controller.dll` (put it in `<game>/bin/win_x64/plugins/`).

Python client: `pip install ./python` (see `python/README.md`).

## Sending inputs

```python
from ets2la_controller import Controller, Button

controller = Controller()
controller.steering = 0.2         # -1 left, +1 right
controller.throttle = 0.5
controller.horn = True            # hold input: stays down until set to False
controller.press(Button.gear_up)  # press input: fires exactly once
controller.commit()               # send the inputs to the game
```

```cpp
#include "ets2la_controller/controller.h"
using namespace ets2la_controller;

Controller controller;
controller.set(Axis::steering, 0.2f);
controller.set(Axis::throttle, 0.5f);
controller.set(Button::horn, true);
controller.press(Button::gear_up);
controller.commit();
```

* Call `commit()` once per control loop iteration. It also acts as heartbeat, without a commit for 3 seconds the plugin releases every input.
* `wait_frame()` blocks until the game starts its next input frame. `wait_frame()` then `commit()`
  gives the lowest possible delay.
* `set_delivery_mode(...)` switches between sending only changes (default) and every input every frame.
