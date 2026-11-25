#include "ui.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Table.H>
#include <FL/Fl_Tabs.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <deque>
#include <tuple>
#include <utility>

namespace monitor {
namespace {

constexpr std::size_t history_limit = 120;
constexpr auto left_label = FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP;
const auto highlight = fl_rgb_color(188, 217, 251);
const auto stripe = fl_rgb_color(246, 248, 251);
const auto chart_bg = fl_rgb_color(232, 237, 244);
const auto cpu_color = fl_rgb_color(50, 109, 181);
const auto memory_color = fl_rgb_color(29, 128, 113);
const auto cache_color = fl_rgb_color(135, 79, 160);

void set_label(Fl_Widget &widget, const std::string &text)
{
	if (!widget.label() || text != widget.label()) {
		widget.copy_label(text.c_str());
		widget.redraw();
	}
}

std::string printable(std::string text)
{
	for (auto &ch : text)
		if (static_cast<unsigned char>(ch) < 32 || ch == 127)
			ch = '?';
	return text;
}

std::string fold(std::string text)
{
	for (auto &ch : text)
		if (ch >= 'A' && ch <= 'Z')
			ch += 'a' - 'A';
	return text;
}

const char *status(Status value)
{
	switch (value) {
	case Status::complete: return "";
	case Status::partial: return " (partial)";
	case Status::invalid: return " (invalid)";
	case Status::unavailable: return " (unavailable)";
	}
	return "";
}

std::string percent(std::optional<double> value)
{
	if (!value)
		return "--";
	char buffer[32];
	std::snprintf(buffer, sizeof(buffer), "%.1f%%", *value);
	return buffer;
}

std::string mib(std::optional<uint64_t> kib)
{
	if (!kib)
		return "--";
	char buffer[48];
	std::snprintf(buffer, sizeof(buffer), "%.1f MiB", double(*kib) / 1024);
	return buffer;
}

std::string uptime(std::optional<double> seconds)
{
	if (!seconds)
		return "--";
	const auto minutes = static_cast<uint64_t>(*seconds / 60);
	return std::to_string(minutes / 1440) + "d " + std::to_string(minutes / 60 % 24) + "h " +
		std::to_string(minutes % 60) + "m";
}

void text(const std::string &value, int xpos, int ypos, int width, int height,
	  Fl_Font font = FL_HELVETICA, Fl_Color color = FL_BLACK)
{
	fl_font(font, 13);
	fl_color(color);
	fl_draw(value.c_str(), xpos, ypos, width, height, left_label, nullptr, 0);
}

struct ChartFrame {
	std::vector<std::pair<int, std::optional<double>>> cpu;
	std::optional<double> used, cached;

	std::optional<double> busy(int core) const
	{
		const auto found = std::lower_bound(cpu.begin(), cpu.end(), core,
			[](const auto &row, int core) { return row.first < core; });
		return found != cpu.end() && found->first == core ? found->second : std::optional<double>{};
	}
};

class Overview final : public Fl_Widget {
public:
	Overview(int xpos, int ypos, int width, int height) : Fl_Widget(xpos, ypos, width, height) { }

	int update(const Snapshot &snapshot)
	{
		cpu_title_ = std::string("CPU utilization") + status(snapshot.cpu_status);
		memory_ = snapshot.memory;
		uptime_ = "Uptime: " + uptime(snapshot.uptime_seconds);
		ChartFrame frame;
		for (const auto &cpu : snapshot.cpus)
			frame.cpu.emplace_back(cpu.core, cpu.busy_percent);
		if (memory_.total_kib && *memory_.total_kib) {
			if (memory_.used_kib)
				frame.used = 100.0 * *memory_.used_kib / *memory_.total_kib;
			if (memory_.cached_kib)
				frame.cached = 100.0 * *memory_.cached_kib / *memory_.total_kib;
		}
		if (history_.size() == history_limit)
			history_.pop_front();
		history_.push_back(std::move(frame));
		redraw();
		const auto cores = std::count_if(snapshot.cpus.begin(), snapshot.cpus.end(),
			[](const CPU &cpu) { return cpu.core >= 0; });
		return 196 + 65 * cores;
	}

private:
	void draw() override
	{
		fl_push_clip(x(), y(), w(), h());
		fl_color(FL_WHITE);
		fl_rectf(x(), y(), w(), h());
		text(uptime_, x() + 12, y() + 4, w() - 24, 23, FL_HELVETICA_BOLD);
		text(cpu_title_, x() + 12, y() + 27, 187, 23, FL_HELVETICA_BOLD);
		text("Last 120 samples (newest at right)", x() + 200, y() + 27, w() - 212, 23);
		int top = y() + 51;
		draw_cpu(top, -1);
		top += 65;
		if (!history_.empty())
			for (const auto &[core, rate] : history_.back().cpu)
				if (core >= 0) {
					draw_cpu(top, core);
					top += 65;
				}
		if (fl_not_clipped(x(), top, w(), 72)) {
			text("Memory and cache", x() + 12, top, 184, 18, FL_HELVETICA_BOLD);
			text("Used: " + mib(memory_.used_kib), x() + 12, top + 19, 184, 18, FL_HELVETICA, memory_color);
			text("Cached: " + mib(memory_.cached_kib), x() + 12, top + 36, 184, 18, FL_HELVETICA, cache_color);
			text("Total: " + mib(memory_.total_kib), x() + 12, top + 53, 184, 18);
			graph(top, 69, [](const ChartFrame &frame) { return frame.used; }, memory_color,
			      [](const ChartFrame &frame) { return frame.cached; });
		}
		fl_pop_clip();
	}

	void draw_cpu(int top, int core)
	{
		if (!fl_not_clipped(x(), top, w(), 60))
			return;
		text(core < 0 ? "All CPUs" : "CPU " + std::to_string(core), x() + 12, top + 6, 184, 20);
		text(percent(history_.empty() ? std::optional<double>{} : history_.back().busy(core)),
		     x() + 12, top + 28, 184, 20, FL_HELVETICA, cpu_color);
		graph(top, 56, [core](const ChartFrame &frame) { return frame.busy(core); }, cpu_color,
		      [](const ChartFrame &) { return std::optional<double>{}; });
	}

	template<class Series, class Overlay>
	void graph(int top, int height, Series series, Fl_Color color, Overlay overlay)
	{
		const int left = x() + 200, width = w() - 212;
		fl_color(chart_bg);
		fl_rectf(left, top, width, height);
		fl_color(fl_rgb_color(205, 216, 231));
		fl_xyline(left, top + height / 2, left + width - 1);
		int index = history_limit - history_.size();
		for (const auto &frame : history_) {
			const int xpos = left + index * width / int(history_limit);
			const int next = left + ++index * width / int(history_limit);
			if (auto value = series(frame)) {
				const int fill = std::max(1, int(std::clamp(*value, 0., 100.) * height / 100));
				fl_color(color);
				fl_rectf(xpos, top + height - fill, next - xpos, fill);
			}
			if (auto value = overlay(frame)) {
				const int offset = std::max(1, int(std::clamp(*value, 0., 100.) * height / 100));
				fl_color(cache_color);
				fl_rectf(xpos, top + height - offset, next - xpos, 1);
			}
		}
	}

	std::deque<ChartFrame> history_;
	Memory memory_;
	std::string cpu_title_ = "CPU utilization (unavailable)", uptime_ = "Uptime: --";
};

class Mounts final : public Fl_Widget {
public:
	Mounts(int xpos, int ypos, int width, int height) : Fl_Widget(xpos, ypos, width, height) { }

	int update(const Snapshot &snapshot)
	{
		const std::string caption = std::string("Mounted filesystems") + status(snapshot.mount_status);
		std::vector<Row> rows;
		for (const auto &mount : snapshot.mounts) {
			Row row{printable(mount.path), printable(mount.filesystem + "  " + mount.source), {}, {}};
			if (const auto &capacity = mount.space.capacity; capacity && capacity->total_bytes) {
				row.usage = mib(capacity->available_bytes / 1024) + " free of " + mib(capacity->total_bytes / 1024);
				row.used = 1 - double(capacity->available_bytes) / capacity->total_bytes;
			} else {
				row.usage = "Capacity unavailable";
			}
			rows.push_back(std::move(row));
		}
		if (caption != caption_ || rows != rows_) {
			caption_ = caption;
			rows_ = std::move(rows);
			redraw();
		}
		return 38 + 54 * rows_.size();
	}

private:
	struct Row {
		std::string path, device, usage;
		std::optional<double> used;
		bool operator==(const Row &other) const
		{
			return std::tie(path, device, usage, used) == std::tie(other.path, other.device, other.usage, other.used);
		}
	};

	void draw() override
	{
		fl_push_clip(x(), y(), w(), h());
		fl_color(FL_WHITE);
		fl_rectf(x(), y(), w(), h());
		text(caption_, x() + 12, y() + 4, w() - 24, 26, FL_HELVETICA_BOLD);
		int top = y() + 34;
		const int half = w() / 2;
		for (std::size_t index = 0; index < rows_.size(); ++index, top += 54) {
			if (!fl_not_clipped(x(), top, w(), 54))
				continue;
			const auto &row = rows_[index];
			fl_color(index % 2 ? FL_WHITE : stripe);
			fl_rectf(x(), top, w(), 54);
			text(row.path, x() + 12, top + 5, half - 24, 21, FL_HELVETICA_BOLD);
			text(row.device, x() + 12, top + 27, half - 24, 19);
			text(row.usage, x() + half, top + 5, half - 12, 21);
			if (row.used) {
				const int width = w() - half - 12;
				fl_color(chart_bg);
				fl_rectf(x() + half, top + 31, width, 12);
				fl_color(memory_color);
				fl_rectf(x() + half, top + 31, int(std::clamp(*row.used, 0., 1.) * width), 12);
			}
		}
		fl_pop_clip();
	}

	std::string caption_ = "Mounted filesystems (unavailable)";
	std::vector<Row> rows_;
};

template<class Canvas> class ScrollView final : public Fl_Scroll {
public:
	ScrollView(int xpos, int ypos, int width, int height)
		: Fl_Scroll(xpos, ypos, width, height), canvas_(xpos, ypos, width - Fl::scrollbar_size(), height)
	{
		type(VERTICAL);
		color(FL_WHITE);
		end();
	}

	void update(const Snapshot &snapshot)
	{
		content_ = canvas_.update(snapshot);
		layout();
	}

	void resize(int xpos, int ypos, int width, int height) override
	{
		Fl_Scroll::resize(xpos, ypos, width, height);
		layout();
	}

private:
	void layout()
	{
		const int width = w() - Fl::scrollbar_size(), height = std::max(h(), content_);
		if (width != canvas_.w() || height != canvas_.h()) {
			canvas_.size(width, height);
			scroll_to(0, std::min(yposition(), height - h()));
			redraw();
		}
	}

	Canvas canvas_;
	int content_ = 0;
};

template<class Canvas> class ScrollPage final : public Fl_Group {
public:
	ScrollPage(int xpos, int ypos, int width, int height, const char *title)
		: Fl_Group(xpos, ypos, width, height, title), view_(xpos + 1, ypos + 1, width - 2, height - 2)
	{
		end();
		resizable(view_);
	}

	void update(const Snapshot &snapshot) { view_.update(snapshot); }

private:
	ScrollView<Canvas> view_;
};

class TaskTable final : public Fl_Table {
public:
	TaskTable(int xpos, int ypos, int width, int height) : Fl_Table(xpos, ypos, width, height)
	{
		cols(5);
		col_header(1);
		col_header_height(25);
		col_header_color(fl_rgb_color(222, 224, 228));
		when(FL_WHEN_NEVER);
		end();
		resize(xpos, ypos, width, height);
	}

	void update(const Snapshot &snapshot, const std::string &filter)
	{
		const auto needle = fold(filter);
		std::vector<const Task *> visible;
		if (selected_ && std::none_of(snapshot.tasks.begin(), snapshot.tasks.end(), [&](const Task &task) {
			return task.identity == *selected_;
		}))
			selected_.reset();
		for (const auto &task : snapshot.tasks)
			if (needle.empty() || fold(printable(task.name)).find(needle) != std::string::npos ||
			    std::to_string(task.identity.pid).find(needle) != std::string::npos)
				visible.push_back(&task);
		std::sort(visible.begin(), visible.end(), [&](const Task *left, const Task *right) {
			if (column_ == 3 && bool(left->machine_percent) != bool(right->machine_percent))
				return bool(left->machine_percent);
			if (descending_)
				std::swap(left, right);
			switch (column_) {
			case 1: if (left->name != right->name) return left->name < right->name; break;
			case 2: if (left->state != right->state) return left->state < right->state; break;
			case 3: if (left->machine_percent != right->machine_percent) return left->machine_percent < right->machine_percent; break;
			case 4: if (ticks(*left) != ticks(*right)) return ticks(*left) < ticks(*right); break;
			}
			return left->identity.pid < right->identity.pid;
		});
		std::vector<Row> display;
		for (const auto *task : visible)
			display.push_back({task->identity, {std::to_string(task->identity.pid), printable(task->name),
				printable(std::string(1, task->state)), percent(task->machine_percent), std::to_string(ticks(*task))}});
		if (display == display_)
			return;
		display_ = std::move(display);
		if (display_.size() != static_cast<std::size_t>(rows())) {
			rows(display_.size());
			row_height_all(23);
		}
		redraw();
	}

	void input()
	{
		const bool click = Fl::event() == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE;
		if (click && callback_context() == CONTEXT_COL_HEADER) {
			const int col = callback_col();
			descending_ = column_ == col ? !descending_ : col >= 3;
			column_ = col;
			redraw();
		} else if ((click || Fl::event() == FL_KEYBOARD) && callback_context() == CONTEXT_CELL) {
			const int row = callback_row();
			if (row >= 0 && row < rows()) {
				selected_ = display_[row].id;
				redraw();
			}
		}
	}

	const Task *selected(const Snapshot &snapshot) const
	{
		if (selected_)
			for (const auto &task : snapshot.tasks)
				if (task.identity == *selected_)
					return &task;
		return nullptr;
	}

	void resize(int xpos, int ypos, int width, int height) override
	{
		Fl_Table::resize(xpos, ypos, width, height);
		col_width(0, 74);
		col_width(1, std::max(120, width - Fl::scrollbar_size() - 324));
		col_width(2, 66);
		col_width(3, 84);
		col_width(4, 96);
	}

private:
	struct Row {
		Identity id;
		std::array<std::string, 5> cells;
		bool operator==(const Row &other) const { return id == other.id && cells == other.cells; }
	};

	static uint64_t ticks(const Task &task)
	{
		return task.user > UINT64_MAX - task.system ? UINT64_MAX : task.user + task.system;
	}

	void draw_cell(TableContext context, int row, int col, int xpos, int ypos, int width, int height) override
	{
		if (context != CONTEXT_COL_HEADER && context != CONTEXT_CELL)
			return;
		static const char *const columns[] = {"PID", "Name", "State", "CPU %", "CPU ticks"};
		fl_push_clip(xpos, ypos, width, height);
		if (context == CONTEXT_COL_HEADER) {
			fl_draw_box(FL_THIN_UP_BOX, xpos, ypos, width, height, col_header_color());
			text(std::string(columns[col]) + (col == column_ ? (descending_ ? " -" : " +") : ""),
			     xpos + 6, ypos, width - 12, height);
		} else if (row >= 0 && static_cast<std::size_t>(row) < display_.size()) {
			const auto &item = display_[row];
			fl_color(selected_ && item.id == *selected_ ? highlight : row % 2 ? stripe : FL_WHITE);
			fl_rectf(xpos, ypos, width, height);
			text(item.cells[col], xpos + 6, ypos, width - 12, height);
		}
		fl_pop_clip();
	}

	std::vector<Row> display_;
	std::optional<Identity> selected_;
	int column_ = 0;
	bool descending_ = false;
};

class Processes final : public Fl_Group {
public:
	Processes(int xpos, int ypos, int width, int height)
		: Fl_Group(xpos, ypos, width, height, "Processes"), filter_(xpos + 60, ypos + 8, 260, 28, "Filter:"),
		task_count_(xpos + 338, ypos + 8, width - 356, 28),
		tasks_(xpos + 10, ypos + 44, width - 20, height - 120),
		detail_name_(xpos + 10, ypos + height - 71, width - 20, 24),
		details_(xpos + 10, ypos + height - 47, width - 20, 34)
	{
		end();
		resizable(tasks_);
		task_count_.align(left_label);
		detail_name_.labelfont(FL_HELVETICA_BOLD);
		detail_name_.align(left_label);
		details_.align(left_label);
		filter_.tooltip("Filter by task name or PID");
		filter_.when(FL_WHEN_CHANGED);
		filter_.callback([](Fl_Widget *, void *data) { static_cast<Processes *>(data)->refresh(); }, this);
		tasks_.callback([](Fl_Widget *, void *data) {
			auto &self = *static_cast<Processes *>(data);
			self.tasks_.input();
			self.refresh();
		}, this);
	}

	void update(const Snapshot &snapshot)
	{
		snapshot_ = &snapshot;
		refresh();
	}

private:
	void refresh()
	{
		tasks_.update(*snapshot_, filter_.value());
		set_label(task_count_, std::to_string(tasks_.rows()) + " of " + std::to_string(snapshot_->tasks.size()) +
			" tasks" + status(snapshot_->task_status));
		if (const auto *task = tasks_.selected(*snapshot_)) {
			set_label(detail_name_, "PID " + std::to_string(task->identity.pid) + "   " + printable(task->name));
			set_label(details_, "Parent: " + std::to_string(task->parent) + "   Start: " +
				std::to_string(task->identity.start_ticks) + "   User ticks: " + std::to_string(task->user) +
				"   System ticks: " + std::to_string(task->system));
		} else {
			set_label(detail_name_, "Select a task to see its details");
			set_label(details_, "");
		}
	}

	Fl_Input filter_;
	Fl_Box task_count_;
	TaskTable tasks_;
	Fl_Box detail_name_, details_;
	const Snapshot *snapshot_ = nullptr;
};

class Monitor {
public:
	Monitor(Sampler sampler, SampleObserver observer)
		: sampler_(std::move(sampler)), observer_(std::move(observer)),
		window_(92, 78, 840, 580, "System Monitor"), health_(12, 8, 590, 28),
		pause_(618, 8, 100, 28, "Pause"), refresh_(728, 8, 100, 28, "Refresh"),
		tabs_(12, 46, 816, 522), overview_(12, 72, 816, 496, "Overview"),
		processes_(12, 72, 816, 496), mounts_(12, 72, 816, 496, "Filesystems")
	{
		tabs_.end();
		tabs_.value(&overview_);
		window_.end();
		window_.resizable(tabs_);
		window_.size_range(660, 380);
		health_.align(left_label);
		pause_.callback([](Fl_Widget *, void *data) { static_cast<Monitor *>(data)->toggle_pause(); }, this);
		refresh_.callback([](Fl_Widget *, void *data) { static_cast<Monitor *>(data)->start(); }, this);
		window_.callback([](Fl_Widget *, void *data) { static_cast<Monitor *>(data)->close(); }, this);
		processes_.update(snapshot_);
		window_.show();
		start();
		Fl::add_timeout(1, tick, this);
	}

	~Monitor() { close(); }

private:
	static void tick(void *data)
	{
		auto &self = *static_cast<Monitor *>(data);
		if (!self.paused_)
			self.start();
		self.update_health();
		Fl::repeat_timeout(1, tick, data);
	}

	static void pipe_ready(int fd, void *data)
	{
		auto &self = *static_cast<Monitor *>(data);
		if (self.worker_.drain()) {
			Fl::remove_fd(fd);
			reap(data);
		}
	}

	static void reap(void *data)
	{
		auto &self = *static_cast<Monitor *>(data);
		std::optional<Snapshot> result;
		if (!self.worker_.finish(result)) {
			Fl::add_timeout(0.02, reap, data);
			return;
		}
		if (result) {
			derive_rates(*result, self.snapshot_);
			self.snapshot_ = std::move(*result);
			self.overview_.update(self.snapshot_);
			self.mounts_.update(self.snapshot_);
			self.processes_.update(self.snapshot_);
			if (self.observer_)
				self.observer_(self.snapshot_);
		}
		self.failed_ = !result;
		self.refresh_.activate();
		self.update_health();
	}

	void start()
	{
		if (worker_.active())
			return;
		if (worker_.start(sampler_)) {
			Fl::add_fd(worker_.fd(), FL_READ, pipe_ready, this);
			refresh_.deactivate();
		} else {
			failed_ = true;
		}
		update_health();
	}

	void stop()
	{
		if (worker_.fd() >= 0)
			Fl::remove_fd(worker_.fd());
		Fl::remove_timeout(reap, this);
		worker_.stop();
		refresh_.activate();
	}

	void close()
	{
		Fl::remove_timeout(tick, this);
		stop();
		window_.hide();
	}

	void toggle_pause()
	{
		paused_ = !paused_;
		set_label(pause_, paused_ ? "Resume" : "Pause");
		if (paused_)
			stop();
		else
			start();
		update_health();
	}

	void update_health()
	{
		const auto now = ProcSource{}.monotonic_ns();
		const auto stamp = snapshot_.monotonic_ns;
		std::string state = paused_ ? "Paused" : "Live";
		if (worker_.active())
			state += " | Sampling";
		if (failed_)
			state += " | Refresh failed";
		if (!stamp || !now || *stamp > *now)
			state += " | No timestamped sample";
		else if (*now - *stamp > stale_ns)
			state += " | Stale data";
		set_label(health_, state);
	}

	Sampler sampler_;
	SampleObserver observer_;
	Snapshot snapshot_;
	SampleWorker worker_;
	Fl_Double_Window window_;
	Fl_Box health_;
	Fl_Button pause_, refresh_;
	Fl_Tabs tabs_;
	ScrollPage<Overview> overview_;
	Processes processes_;
	ScrollPage<Mounts> mounts_;
	bool paused_ = false, failed_ = false;
};

}

int run(Sampler sampler, SampleObserver observer)
{
	Monitor monitor(std::move(sampler), std::move(observer));
	return Fl::run();
}

}
