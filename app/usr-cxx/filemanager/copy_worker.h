#ifndef MARIO_FILEMANAGER_COPY_WORKER_H
#define MARIO_FILEMANAGER_COPY_WORKER_H

#include "copy.h"

#include <array>
#include <sys/types.h>

namespace files {

enum class CopyEventType : uint8_t { progress, conflict, result };

struct CopyEvent {
	CopyEventType type;
	uint32_t item;
	uint64_t bytes;
	CopyResult result;
	std::array<char, copy_name_limit + 1> name;
};

class CopyWorker {
public:
	enum class Read { event, pending, closed };

	CopyWorker() = default;
	~CopyWorker();
	CopyWorker(const CopyWorker &) = delete;
	CopyWorker &operator=(const CopyWorker &) = delete;
	int start(const CopyRequest &request);
	void cancel();
	bool decide(CopyDecision decision);
	Read next(CopyEvent &event);
	void close_events();
	bool reap(bool &success);
	int fd() const { return events_; }
	bool active() const { return pid_ > 0; }

private:
	pid_t pid_ = -1;
	int events_ = -1, commands_ = -1;
	std::array<char, sizeof(CopyEvent)> buffer_;
	std::size_t used_ = 0;
};

}

#endif
