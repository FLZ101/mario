#include "window.h"

#include <FL/Fl.H>
#include <FL/platform.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

extern "C" {
#include <gui/protocol.h>
#include <gui/surface_upload.h>
}

namespace mario_fltk {
namespace {

constexpr char socket_path[] = "/tmp/mario-wm.sock";
constexpr uint32_t create_request = 1;

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__);

gui_rect unite(gui_rect first, gui_rect second)
{
	if (first.w <= 0 || first.h <= 0)
		return second;
	if (second.w <= 0 || second.h <= 0)
		return first;
	const int left = std::min(first.x, second.x), top = std::min(first.y, second.y);
	return {left, top, std::max(first.x + first.w, second.x + second.w) - left,
		std::max(first.y + first.h, second.y + second.h) - top};
}

int connect_wm()
{
	struct sigaction action{};
	if (sigaction(SIGPIPE, nullptr, &action) == 0 && action.sa_handler == SIG_DFL) {
		action.sa_handler = SIG_IGN;
		sigaction(SIGPIPE, &action, nullptr);
	}
	const int descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
	if (descriptor < 0)
		return -1;
	sockaddr_un address{};
	address.sun_family = AF_UNIX;
	const char *path = std::getenv("MARIO_FLTK_SOCKET");
	if (!path || !*path)
		path = socket_path;
	const auto length = strnlen(path, sizeof(address.sun_path));
	if (length == sizeof(address.sun_path)) {
		close(descriptor);
		return -1;
	}
	std::memcpy(address.sun_path, path, length);
	if (fcntl(descriptor, F_SETFD, FD_CLOEXEC) == 0 &&
	    fcntl(descriptor, F_SETFL, O_NONBLOCK) == 0 &&
	    connect(descriptor, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0)
		return descriptor;
	close(descriptor);
	return -1;
}

struct Message {
	gui_protocol_type type{};
	size_t size = 0;
	std::array<uint8_t, 64> bytes{};

	template <typename Wire>
	bool payload(Wire &result) const
	{
		if (size != sizeof(Wire))
			return false;
		std::memcpy(&result, bytes.data(), size);
		return true;
	}
};

}

class Window_Peer : public std::enable_shared_from_this<Window_Peer> {
public:
	explicit Window_Peer(Window_Driver &driver) : driver_(&driver) {}
	~Window_Peer() { disconnect(); }

	bool start(int width, int height, const char *name)
	{
		if (width <= 0 || height <= 0 || !input_ || (fd_ = connect_wm()) < 0)
			return false;
		width_ = width;
		height_ = height;
		title(name);
		Fl::add_timeout(5, startup_timeout, this);
		return true;
	}

	void disconnect()
	{
		driver_ = nullptr;
		Fl::remove_timeout(startup_timeout, this);
		Fl::remove_timeout(continue_input, this);
		Fl::remove_timeout(retry_upload, this);
		if (fd_ >= 0) {
			Fl::remove_fd(fd_);
			close(fd_);
			fd_ = -1;
		}
	}

	void title(const char *name)
	{
		title_.assign(name ? name : "", name ? strnlen(name, GUI_PROTOCOL_MAX_TITLE_BYTES) : 0);
		title_pending_ = true;
		watch();
	}

	void move(int x, int y)
	{
		move_ = {0, x, y};
		move_pending_ = true;
		watch();
	}

	void resize(int width, int height)
	{
		if (width <= 0 || height <= 0)
			return;
		width_ = width;
		height_ = height;
		resize_pending_ = true;
		watch();
	}

	gui_surface *pixels() const { return pixels_.get(); }
	uint32_t id() const { return id_; }
	gui_rect entire_surface() const { return {0, 0, pixels_->width, pixels_->height}; }

	void changed(gui_rect rectangle)
	{
		dirty_ = unite(dirty_, rectangle);
		watch();
	}

private:
	enum class Startup { hello, capabilities, create, wait, ready };
	Window_Driver *driver_;
	int fd_ = -1;
	int fd_events_ = 0;
	Startup startup_ = Startup::hello;
	std::unique_ptr<gui_protocol_input, decltype(&gui_protocol_input_destroy)>
		input_{gui_protocol_input_create(), gui_protocol_input_destroy};
	std::array<Message, 8> incoming_{};
	size_t incoming_count_ = 0;
	bool bad_input_ = false;
	uint32_t id_ = 0;
	uint64_t configure_ = 0, serial_ = 0, base_ = 0;
	int width_ = 0, height_ = 0;
	std::string title_;
	gui_protocol_wire_window_move move_{};
	bool title_pending_ = false, move_pending_ = false, resize_pending_ = false;
	bool patch_ = false, retry_ = false;
	unsigned failures_ = 0;
	gui_rect dirty_{};
	Pixels pixels_, snapshot_;
	gui_surface_upload upload_{};

	bool can_upload() const
	{
		return pixels_ && dirty_.w > 0 && dirty_.h > 0 &&
		       !retry_ && gui_surface_upload_ready(&upload_);
	}

	void watch()
	{
		if (fd_ < 0)
			return;
		const bool writing = startup_ < Startup::wait || gui_surface_upload_writable(&upload_) ||
			(startup_ == Startup::ready && (title_pending_ || move_pending_ ||
			 resize_pending_ || can_upload()));
		const int events = FL_READ | FL_EXCEPT | (writing ? FL_WRITE : 0);
		if (events != fd_events_) {
			Fl::remove_fd(fd_);
			Fl::add_fd(fd_, events, fd_callback, this);
			fd_events_ = events;
		}
	}

	static void fd_callback(int, void *context)
	{
		auto peer = static_cast<Window_Peer *>(context)->shared_from_this();
		if (!peer->read_messages() || (peer->fd_ >= 0 && !peer->write_messages()))
			peer->fail();
		peer->watch();
	}

	static void startup_timeout(void *context)
	{
		auto peer = static_cast<Window_Peer *>(context)->shared_from_this();
		peer->fail();
	}

	static void continue_input(void *context) { fd_callback(-1, context); }

	static void retry_upload(void *context)
	{
		auto &peer = *static_cast<Window_Peer *>(context);
		peer.retry_ = false;
		peer.watch();
	}

	void fail()
	{
		auto *driver = driver_;
		disconnect();
		if (driver)
			driver->disconnected();
	}

	template <typename Wire>
	bool queue(gui_protocol_type type, const Wire &payload)
	{
		return gui_surface_upload_control(&upload_, type, &payload, sizeof(payload)) == 0;
	}

	template <typename Wire>
	bool queue_title(gui_protocol_type type, const Wire &payload)
	{
		std::array<uint8_t, sizeof(Wire) + GUI_PROTOCOL_MAX_TITLE_BYTES> bytes;
		std::memcpy(bytes.data(), &payload, sizeof(payload));
		std::memcpy(bytes.data() + sizeof(payload), title_.data(), title_.size());
		return gui_surface_upload_control(&upload_, type, bytes.data(), sizeof(payload) + title_.size()) == 0;
	}

	bool prepare_output()
	{
		if (upload_.frame_bytes)
			return true;
		switch (startup_) {
		case Startup::hello:
			startup_ = Startup::capabilities;
			return queue(GUI_PROTOCOL_HELLO,
				     gui_protocol_wire_hello{GUI_PROTOCOL_VERSION_MAJOR, GUI_PROTOCOL_VERSION_MINOR});
		case Startup::capabilities:
			startup_ = Startup::create;
			return queue(GUI_PROTOCOL_CAPABILITIES,
				     gui_protocol_wire_capabilities{GUI_PROTOCOL_CAP_SURFACE_PATCH});
		case Startup::create:
			startup_ = Startup::wait;
			resize_pending_ = title_pending_ = false;
			return queue_title(GUI_PROTOCOL_WINDOW_CREATE,
				gui_protocol_wire_window_create{create_request, uint32_t(width_),
					uint32_t(height_), uint16_t(title_.size()), 0});
		case Startup::wait:
			return true;
		case Startup::ready:
			break;
		}
		if (move_pending_) {
			move_pending_ = false;
			move_.window_id_le = id_;
			return queue(GUI_PROTOCOL_WINDOW_MOVE, move_);
		}
		if (resize_pending_) {
			resize_pending_ = false;
			return queue(GUI_PROTOCOL_WINDOW_RESIZE,
				     gui_protocol_wire_window_resize{id_, uint32_t(width_), uint32_t(height_)});
		}
		if (title_pending_) {
			title_pending_ = false;
			return queue_title(GUI_PROTOCOL_WINDOW_SET_TITLE,
				gui_protocol_wire_window_set_title{id_, uint16_t(title_.size()), 0});
		}
		if (!can_upload())
			return true;
		const auto full = entire_surface();
		const gui_rect area = patch_ && base_ ? dirty_ : full;
		const bool use_patch = area.w != full.w || area.h != full.h;
		snapshot_ = make_pixels(full.w, full.h);
		if (!snapshot_)
			return false;
		gui_blit(snapshot_.get(), area.x, area.y, pixels_.get(), &area);
		dirty_ = {};
		return gui_surface_upload_start(&upload_, id_, configure_, base_, ++serial_,
						snapshot_.get(), area, use_patch) == 0;
	}

	bool write_messages()
	{
		if (!prepare_output() || gui_surface_upload_write(&upload_, write_socket, this) < 0)
			return false;
		if (upload_.completed) {
			if (!retry_ && upload_.completed_configure == configure_)
				base_ = upload_.completed_serial;
			upload_.completed = 0;
		}
		if (!upload_.active)
			snapshot_.reset();
		return true;
	}

	static ssize_t write_socket(void *context, const void *data, size_t length)
	{
		return write(static_cast<Window_Peer *>(context)->fd_, data, length);
	}

	bool read_messages()
	{
		Fl::remove_timeout(continue_input, this);
		for (int batch = 0; batch < 4 && fd_ >= 0; ++batch) {
			incoming_count_ = 0;
			const int drained = gui_protocol_input_drain(input_.get(), collect_message, this);
			if (drained < 0 || bad_input_)
				return false;
			for (size_t index = 0; index < incoming_count_ && fd_ >= 0; ++index)
				if (!message(incoming_[index]))
					return false;
			if (fd_ < 0)
				return true;
			if (drained == static_cast<int>(incoming_.size()))
				continue;
			uint8_t bytes[512];
			const auto count = read(fd_, bytes, sizeof(bytes));
			if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
				return true;
			if (count < 0 && errno == EINTR)
				continue;
			if (count <= 0 || gui_protocol_input_append(input_.get(), bytes, count) < 0)
				return false;
		}
		if (fd_ >= 0)
			Fl::add_timeout(0, continue_input, this);
		return true;
	}

	static void collect_message(void *context, const gui_protocol_header *header,
				    const void *payload, size_t size)
	{
		auto &peer = *static_cast<Window_Peer *>(context);
		if (peer.incoming_count_ == peer.incoming_.size() || size > 64) {
			peer.bad_input_ = true;
			return;
		}
		auto &entry = peer.incoming_[peer.incoming_count_++];
		entry.type = header->type;
		entry.size = size;
		std::memcpy(entry.bytes.data(), payload, size);
	}

	bool apply_configure(uint64_t serial, uint32_t width, uint32_t height)
	{
		if (serial <= configure_)
			return true;
		if (!width || !height || width > INT32_MAX || height > INT32_MAX)
			return false;
		gui_surface_upload_cancel(&upload_, false);
		pixels_.reset();
		pixels_ = make_pixels(width, height);
		if (!pixels_)
			return false;
		configure_ = serial;
		base_ = 0;
		failures_ = 0;
		dirty_ = {};
		driver_->configured(id_, width, height);
		return true;
	}

	bool recover(const Message &entry)
	{
		gui_protocol_wire_error error{};
		if (!entry.payload(error))
			return false;
		switch (error.failed_type_le) {
		case GUI_PROTOCOL_SURFACE_CANCEL:
		case GUI_PROTOCOL_WINDOW_MOVE:
		case GUI_PROTOCOL_WINDOW_RESIZE:
			return true;
		case GUI_PROTOCOL_SURFACE_DATA:
			if (gui_surface_upload_data_error(&upload_) < 0)
				return false;
			[[fallthrough]];
		case GUI_PROTOCOL_SURFACE_BEGIN:
		case GUI_PROTOCOL_SURFACE_PATCH_BEGIN:
		case GUI_PROTOCOL_SURFACE_COMMIT:
			if (!pixels_ || ++failures_ > 16)
				return false;
			gui_surface_upload_cancel(&upload_, patch_);
			base_ = 0;
			dirty_ = entire_surface();
			if (!retry_) {
				retry_ = true;
				Fl::add_timeout(.1, retry_upload, this);
			}
			return true;
		default:
			return false;
		}
	}

	bool message(const Message &entry)
	{
		switch (entry.type) {
		case GUI_PROTOCOL_HELLO_REPLY: {
			gui_protocol_wire_hello reply{};
			return entry.payload(reply) && reply.major_le == GUI_PROTOCOL_VERSION_MAJOR;
		}
		case GUI_PROTOCOL_CAPABILITIES_REPLY: {
			gui_protocol_wire_capabilities reply{};
			if (!entry.payload(reply))
				return false;
			patch_ = (reply.capabilities_le & GUI_PROTOCOL_CAP_SURFACE_PATCH) != 0;
			upload_.batch = patch_;
			return true;
		}
		case GUI_PROTOCOL_WINDOW_CREATE_REPLY: {
			gui_protocol_wire_window_create_reply reply{};
			if (!entry.payload(reply) || reply.request_id_le != create_request ||
			    !reply.window_id_le || !reply.configure_serial_le || startup_ != Startup::wait)
				return false;
			id_ = reply.window_id_le;
			startup_ = Startup::ready;
			Fl::remove_timeout(startup_timeout, this);
			return apply_configure(reply.configure_serial_le, reply.width_le, reply.height_le);
		}
		case GUI_PROTOCOL_WINDOW_CONFIGURE: {
			gui_protocol_wire_window_configure reply{};
			return entry.payload(reply) && id_ && id_ == reply.window_id_le &&
			       apply_configure(reply.configure_serial_le, reply.width_le, reply.height_le);
		}
		case GUI_PROTOCOL_SURFACE_DATA_REPLY:
			return gui_surface_upload_reply(&upload_, entry.bytes.data(), entry.size) == 0;
		case GUI_PROTOCOL_ERROR:
			return recover(entry);
		case GUI_PROTOCOL_WINDOW_CLOSE_REQUEST: {
			gui_protocol_wire_window_id reply{};
			if (!entry.payload(reply) || !id_ || id_ != reply.window_id_le)
				return false;
			driver_->close_requested();
			return true;
		}
		default:
			return true;
		}
	}
};

Window_Driver::Window_Driver(Fl_Window *window) : Fl_Window_Driver(window) {}

Window_Driver::~Window_Driver()
{
	if (peer_)
		peer_->disconnect();
}

void Window_Driver::show()
{
	if (shown())
		return;
	auto peer = std::make_shared<Window_Peer>(*this);
	if (!peer->start(w(), h(), pWindow->label())) {
		Fl::error("FLTK: could not connect a window to mario-wm");
		return;
	}
	peer_ = std::move(peer);
	if (force_position())
		peer_->move(x(), y());
	auto *record = new Fl_X{0, pWindow, nullptr, Fl_X::first};
	Fl_X::first = record;
	flx(record);
	wait_for_expose_value = 1;
	pWindow->set_visible();
	Fl::handle(FL_SHOW, pWindow);
}

void Window_Driver::hide()
{
	auto *record = Fl_X::flx(pWindow);
	if (peer_)
		peer_->disconnect();
	peer_.reset();
	if (Fl_Window::current() == pWindow) {
		static_cast<Graphics_Driver &>(Fl_Graphics_Driver::default_driver()).target(nullptr);
		current(nullptr);
		fl_window = 0;
	}
	if (!hide_common()) {
		Fl_Graphics_Driver::default_driver().XDestroyRegion(record->region);
		delete record;
	}
}

void Window_Driver::label(const char *name, const char *)
{
	if (peer_)
		peer_->title(name);
}

void Window_Driver::make_current()
{
	Fl_Display_Device::display_device()->set_current();
	auto &graphics = static_cast<Graphics_Driver &>(Fl_Graphics_Driver::default_driver());
	graphics.target(peer_ ? peer_->pixels() : nullptr);
	graphics.set_origin(0, 0);
	fl_clip_region(nullptr);
	fl_window = os_id();
}

void Window_Driver::flush()
{
	if (!peer_ || !peer_->pixels())
		return;
	const auto *region = static_cast<const gui_rect *>(Fl_X::flx(pWindow)->region);
	auto damage = peer_->entire_surface();
	if (region) {
		const int left = std::clamp(region->x, 0, damage.w);
		const int top = std::clamp(region->y, 0, damage.h);
		const int right = std::clamp<int64_t>(int64_t(region->x) + region->w, left, damage.w);
		const int bottom = std::clamp<int64_t>(int64_t(region->y) + region->h, top, damage.h);
		damage = {left, top, right - left, bottom - top};
	}
	pWindow->Fl_Window::flush();
	peer_->changed(damage);
}

void Window_Driver::resize(int position_x, int position_y, int width, int height)
{
	if (!peer_) {
		pWindow->Fl_Group::resize(position_x, position_y, width, height);
		return;
	}
	if (position_x != x() || position_y != y()) {
		peer_->move(position_x, position_y);
		x(position_x);
		y(position_y);
	}
	if (width != w() || height != h())
		peer_->resize(width, height);
}

fl_uintptr_t Window_Driver::os_id() { return peer_ ? peer_->id() : 0; }

void Window_Driver::configured(uint32_t window_id, int width, int height)
{
	Fl_X::flx(pWindow)->xid = window_id;
	wait_for_expose_value = 0;
	pWindow->Fl_Group::resize(x(), y(), width, height);
	pWindow->redraw();
}

void Window_Driver::close_requested() { Fl::handle(FL_CLOSE, pWindow); }

void Window_Driver::disconnected()
{
	Fl::error("FLTK: mario-wm disconnected");
	hide();
}

}
