#include "ets2la_controller/controller.h"
#include <cstdio>
#include <cmath>


int main() {
    ets2la_controller::Controller controller;

    const uint64_t start = ets2la_controller::qpc_now();
    while (controller.ticks_to_ms(ets2la_controller::qpc_now() - start) < 10000.0) {
        if (!controller.wait_frame()) {
            continue;
        }
        const double t = controller.ticks_to_ms(ets2la_controller::qpc_now() - start) / 1000.0;
        controller.set(ets2la_controller::Axis::steering, float(std::sin(t)));
        controller.commit();
    }

    controller.release_all();
    controller.commit();
    return 0;
}