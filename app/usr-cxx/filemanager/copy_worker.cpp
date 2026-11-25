#include "copy_worker.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace files {
namespace {

class Channel final : public CopyObserver {
public:
	Channel(int commands, int events) : commands_(commands), events_(events) { }

	bool cancelled() override
	{
		pollfd commands{commands_, POLLIN, 0};
		if (!stopped_ && poll(&commands, 1, 0) > 0 && (commands.revents & (POLLHUP | POLLERR | POLLNVAL)))
			stopped_ = true;
		return stopped_;
	}
	CopyDecision conflict(std::size_t item, std::string_view name) override
	{
		CopyEvent event{};
		event.type = CopyEventType::conflict;
		event.item = item;
		std::copy(name.begin(), name.end(), event.name.begin());
		if (cancelled() || !emit(event))
			return CopyDecision::cancel;
		while (!cancelled()) {
			pollfd commands{commands_, POLLIN, 0};
			if (poll(&commands, 1, -1) < 0) {
				if (errno == EINTR)
					continue;
				break;
			}
			CopyDecision decision{};
			if (read(commands_, &decision, sizeof(decision)) == sizeof(decision) &&
			    (decision == CopyDecision::skip || decision == CopyDecision::keep_both))
				return decision;
			break;
		}
		stopped_ = true;
		return CopyDecision::cancel;
	}
	void progress(std::size_t item, uint64_t bytes) override
	{
		timespec now{};
		clock_gettime(CLOCK_MONOTONIC, &now);
		const int64_t millis = int64_t(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
		if (millis - last_progress_ < 100)
			return;
		last_progress_ = millis;
		CopyEvent event{};
		event.type = CopyEventType::progress;
		event.item = item;
		event.bytes = bytes;
		emit(event);
	}
	void result(std::size_t item, CopyResult result) override
	{
		CopyEvent event{};
		event.type = CopyEventType::result;
		event.item = item;
		event.result = result;
		emit(event);
	}

private:
	bool emit(const CopyEvent &event)
	{
		if (disconnected_)
			return false;
		const auto *payload = reinterpret_cast<const char *>(&event);
		std::size_t offset = 0;
		while (offset < sizeof(event)) {
			const auto count = write(events_, payload + offset, sizeof(event) - offset);
			if (count < 0 && errno == EINTR)
				continue;
			if (count <= 0) {
				stopped_ = disconnected_ = true;
				return false;
			}
			offset += count;
		}
		return true;
	}

	int commands_, events_;
	bool stopped_ = false, disconnected_ = false;
	int64_t last_progress_ = -100;
};

}

CopyWorker::~CopyWorker()
{
	cancel();
	close_events();
	if (active())
		while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) { }
}

int CopyWorker::start(const CopyRequest &request)
{
	if (active())
		return EBUSY;
	if (request.names.empty() || request.names.size() > copy_selection_limit)
		return EINVAL;
	int commands[2], events[2];
	if (pipe2(commands, O_CLOEXEC) < 0)
		return errno;
	if (pipe2(events, O_CLOEXEC) < 0) {
		const int error = errno;
		close(commands[0]);
		close(commands[1]);
		return error;
	}
	int error = 0;
	if (fcntl(events[0], F_SETFL, O_NONBLOCK) < 0 || fcntl(commands[1], F_SETFL, O_NONBLOCK) < 0)
		error = errno;
	else {
		struct rlimit limit{};
		const auto limit_fd = getrlimit(RLIMIT_NOFILE, &limit) == 0 ? limit.rlim_max : 256;
		pid_ = fork();
		if (!pid_) {
			const char name[16] = "fm-copy-worker";
			prctl(PR_SET_NAME, name, 0UL, 0UL, 0UL);
			for (int fd = 0; fd < static_cast<int>(limit_fd); ++fd)
				if (fd != commands[0] && fd != events[1])
					close(fd);
			std::signal(SIGPIPE, SIG_IGN);
			Channel channel(commands[0], events[1]);
			copy_items(request, channel);
			_exit(0);
		}
		if (pid_ < 0)
			error = errno;
	}
	close(commands[0]);
	close(events[1]);
	if (error) {
		close(commands[1]);
		close(events[0]);
		return error;
	}
	commands_ = commands[1];
	events_ = events[0];
	used_ = 0;
	return 0;
}

void CopyWorker::cancel()
{
	if (commands_ >= 0) {
		close(commands_);
		commands_ = -1;
	}
}

bool CopyWorker::decide(CopyDecision decision)
{
	ssize_t count;
	do {
		count = write(commands_, &decision, sizeof(decision));
	} while (count < 0 && errno == EINTR);
	return count == sizeof(decision);
}

CopyWorker::Read CopyWorker::next(CopyEvent &event)
{
	while (used_ < buffer_.size()) {
		const auto count = read(events_, buffer_.data() + used_, buffer_.size() - used_);
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0 && errno == EAGAIN)
			return Read::pending;
		if (count <= 0)
			return Read::closed;
		used_ += count;
	}
	std::memcpy(&event, buffer_.data(), sizeof(event));
	used_ = 0;
	return Read::event;
}

void CopyWorker::close_events()
{
	if (events_ >= 0) {
		close(events_);
		events_ = -1;
	}
}

bool CopyWorker::reap(bool &success)
{
	if (!active() || events_ >= 0)
		return false;
	int status;
	const auto waited = waitpid(pid_, &status, WNOHANG);
	if (!waited || (waited < 0 && errno == EINTR))
		return false;
	success = waited > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 && !used_;
	pid_ = -1;
	cancel();
	return true;
}

}
