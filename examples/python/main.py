from ets2la_controller import Controller
import math
import time

controller = Controller()

start = time.perf_counter()
while time.perf_counter() - start < 10:
    if not controller.wait_frame():
        continue
    controller.steering = math.sin(time.perf_counter() - start)
    controller.commit()

controller.release_all()
controller.commit()