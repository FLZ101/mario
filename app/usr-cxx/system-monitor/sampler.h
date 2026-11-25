#ifndef MARIO_SYSTEM_MONITOR_SAMPLER_H
#define MARIO_SYSTEM_MONITOR_SAMPLER_H

#include "telemetry.h"

#include <functional>
#include <sys/types.h>

namespace monitor {

using Sampler = std::function<Snapshot()>;

class SampleWorker {
public:
	~SampleWorker();
	SampleWorker() = default;
	SampleWorker(const SampleWorker &) = delete;
	SampleWorker &operator=(const SampleWorker &) = delete;
	bool start(const Sampler &sampler);
	bool drain();
	bool finish(std::optional<Snapshot> &result);
	void stop();
	int fd() const { return fd_; }
	bool active() const { return pid_ > 0; }

private:
	void close_pipe();
	std::string bytes_;
	pid_t pid_ = -1;
	int fd_ = -1;
	bool pipe_ok_ = false;
};

}

#endif
