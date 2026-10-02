# ets2la_controller

Lock-free writer for the `ets2la_controller.dll` shared memory.

## Installation

```powershell
pip install ./python
```

## Usage

```python
from ets2la_controller import Controller, Button

controller = Controller()
controller.steering = 0.2
controller.press(Button.lights_cycle)
controller.commit()
```

* `controller.set(Axis.x, value)` / `controller.set(Button.x, bool)` and `set_by_name("steering", 0.2)`.
* `press(Button.x)` fires a press input exactly once, hold inputs stay down until set to `False`.
* `wait_frame()` blocks until the game starts its next input frame.
* `latency()` and `stats()` report the delay between `commit()` and the game.
* `connected` reports whether the plugin is loaded. If it is not, calls keep retrying in the background.