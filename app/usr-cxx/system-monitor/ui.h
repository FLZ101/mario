#ifndef MARIO_SYSTEM_MONITOR_UI_H
#define MARIO_SYSTEM_MONITOR_UI_H

#include "sampler.h"

namespace monitor {

using SampleObserver = std::function<void(const Snapshot &)>;
int run(Sampler sampler, SampleObserver observer = {});

}

#endif
