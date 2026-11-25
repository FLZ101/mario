#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Widget.H>
#include <FL/fl_draw.H>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" {
#include <gui/protocol.h>
#include <gui/surface_upload.h>
}

namespace {

constexpr char socket_path[] = "/tmp/fltk-probe-wm.sock";
constexpr uint32_t window_id = 11;

bool read_frame_bytes(int descriptor, uint8_t *bytes, size_t count)
{
	while (count) {
		pollfd ready{descriptor, POLLIN, 0};
		if (poll(&ready, 1, 4000) != 1 || !(ready.revents & POLLIN))
			return false;
		const auto received = read(descriptor, bytes, count < 997 ? count : 997);
		if (received <= 0)
			return false;
		count -= received;
		bytes += received;
	}
	return true;
}

template <typename Payload>
bool reply(int descriptor, gui_protocol_type type, const Payload &payload)
{
	std::array<uint8_t, GUI_PROTOCOL_HEADER_SIZE + sizeof(Payload)> frame{};
	if (gui_protocol_encode_header(frame.data(), frame.size(), type, sizeof(payload)) < 0)
		return false;
	std::memcpy(frame.data() + GUI_PROTOCOL_HEADER_SIZE, &payload, sizeof(payload));
	if (write(descriptor, frame.data(), 3) != 3)
		return false;
	usleep(1000);
	return write(descriptor, frame.data() + 3, frame.size() - 3) == ssize_t(frame.size() - 3);
}

class Marker : public Fl_Widget {
public:
	bool changed = false;
	Marker() : Fl_Widget(16, 16, 16, 16) {}
	void draw() override
	{
		fl_color(changed ? FL_RED : FL_BLACK);
		fl_rectf(x(), y(), w(), h());
	}
};

void request_patch(int descriptor, void *context)
{
	char signal;
	if (read(descriptor, &signal, 1) == 1) {
		auto &marker = *static_cast<Marker *>(context);
		marker.changed = true;
		marker.redraw();
	}
	Fl::remove_fd(descriptor);
}

int fake_wm(int server, int progress)
{
	pollfd ready{server, POLLIN, 0};
	if (poll(&ready, 1, 4000) != 1 || !(ready.revents & POLLIN))
		return 1;
	const int descriptor = accept(server, nullptr, nullptr);
	if (descriptor < 0)
		return 1;
	uint64_t committed_serial = 0;
	bool reconfigured = false, rejected = false, recovered = false;
	for (unsigned message = 0; message < 256; ++message) {
		gui_protocol_header header{};
		std::array<uint8_t, GUI_SURFACE_UPLOAD_FRAME_BYTES> bytes;
		if (!read_frame_bytes(descriptor, bytes.data(), GUI_PROTOCOL_HEADER_SIZE) ||
		    gui_protocol_decode_header(bytes.data(), GUI_PROTOCOL_HEADER_SIZE, &header) < 0 ||
		    header.length > bytes.size())
			return 2;
		const auto count = header.length - GUI_PROTOCOL_HEADER_SIZE;
		if (!read_frame_bytes(descriptor, bytes.data(), count))
			return 3;
		bool ok = true;
		switch (header.type) {
		case GUI_PROTOCOL_HELLO:
			ok = reply(descriptor, GUI_PROTOCOL_HELLO_REPLY,
				   gui_protocol_wire_hello{GUI_PROTOCOL_VERSION_MAJOR, GUI_PROTOCOL_VERSION_MINOR});
			break;
		case GUI_PROTOCOL_CAPABILITIES:
			ok = reply(descriptor, GUI_PROTOCOL_CAPABILITIES_REPLY,
				   gui_protocol_wire_capabilities{GUI_PROTOCOL_CAP_SURFACE_PATCH});
			break;
		case GUI_PROTOCOL_WINDOW_CREATE:
			ok = reply(descriptor, GUI_PROTOCOL_WINDOW_CREATE_REPLY,
				   gui_protocol_wire_window_create_reply{1, window_id, 1, 224, 160});
			break;
		case GUI_PROTOCOL_SURFACE_BEGIN: {
			gui_protocol_wire_surface_begin begin{};
			if (count != sizeof(begin))
				return 4;
			std::memcpy(&begin, bytes.data(), sizeof(begin));
			if (begin.configure_serial_le == 2 && (begin.width_le != 256 || begin.height_le != 180))
				return 5;
			if (rejected) {
				if (begin.configure_serial_le != 2 || begin.frame_serial_le <= committed_serial)
					return 6;
				recovered = true;
			}
			break;
		}
		case GUI_PROTOCOL_SURFACE_DATA: {
			gui_protocol_wire_surface_data data{};
			if (count < sizeof(data))
				return 7;
			std::memcpy(&data, bytes.data(), sizeof(data));
			if (!reconfigured) {
				if (data.configure_serial_le != 1 || !reply(descriptor, GUI_PROTOCOL_WINDOW_CONFIGURE,
					gui_protocol_wire_window_configure{window_id, 2, 256, 180}))
					return 8;
				reconfigured = true;
			}
			ok = reply(descriptor, GUI_PROTOCOL_SURFACE_DATA_REPLY, data);
			break;
		}
		case GUI_PROTOCOL_SURFACE_COMMIT: {
			gui_protocol_wire_surface_commit commit{};
			if (count != sizeof(commit))
				return 9;
			std::memcpy(&commit, bytes.data(), sizeof(commit));
			if (commit.configure_serial_le != 2)
				return 10;
			if (recovered)
				return reply(descriptor, GUI_PROTOCOL_WINDOW_CLOSE_REQUEST,
					     gui_protocol_wire_window_id{window_id}) ? 0 : 11;
			committed_serial = commit.frame_serial_le;
			ok = write(progress, "p", 1) == 1;
			break;
		}
		case GUI_PROTOCOL_SURFACE_PATCH_BEGIN: {
			gui_protocol_wire_surface_patch_begin patch{};
			if (rejected || !committed_serial || count != sizeof(patch))
				return 12;
			std::memcpy(&patch, bytes.data(), sizeof(patch));
			if (patch.base_frame_serial_le != committed_serial || patch.width_le != 16)
				return 13;
			rejected = true;
			ok = reply(descriptor, GUI_PROTOCOL_ERROR,
				   gui_protocol_wire_error{-ESTALE, GUI_PROTOCOL_SURFACE_PATCH_BEGIN, 0});
			break;
		}
		case GUI_PROTOCOL_SURFACE_CANCEL:
			break;
		default:
			return 14;
		}
		if (!ok)
			return 15;
	}
	return 16;
}

}

int run_upload_test()
{
	const int server = socket(AF_UNIX, SOCK_STREAM, 0);
	int progress[2];
	sockaddr_un address{};
	address.sun_family = AF_UNIX;
	std::memcpy(address.sun_path, socket_path, sizeof(socket_path));
	if (server < 0 || pipe(progress) < 0 ||
	    bind(server, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 ||
	    listen(server, 1) < 0)
		return 1;
	const pid_t child = fork();
	if (child < 0)
		return 2;
	if (!child) {
		close(progress[0]);
		const int result = fake_wm(server, progress[1]);
		if (result)
			std::fprintf(stderr, "fltk-upload: peer failed (%d)\n", result);
		_exit(result);
	}
	close(server);
	close(progress[1]);
	if (setenv("MARIO_FLTK_SOCKET", socket_path, 1) < 0)
		return 3;
	Fl_Window window(224, 160, "Test upload");
	Marker marker;
	window.end();
	Fl::add_fd(progress[0], request_patch, &marker);
	window.show();
	Fl::run();
	Fl::remove_fd(progress[0]);
	close(progress[0]);
	unlink(socket_path);
	int status = 0;
	if (waitpid(child, &status, 0) != child || status || window.w() != 256 || !marker.changed)
		return 3;
	std::puts("fltk-upload: interrupted upload, fragmented replies and stale patch recovered");
	return 0;
}
