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
#include <FL/Fl_Toggle_Button.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <font-icons.h>
#include <mario/netctl.h>
#include <mario/process.h>

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <signal.h>
#include <sys/prctl.h>
#include <tuple>
#include <unordered_map>
#include <unistd.h>
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

struct ControlIcons {
	std::unique_ptr<Fl_RGB_Image> tree;
	std::unique_ptr<Fl_RGB_Image> end_process, force_kill;
};

const ControlIcons &control_icons()
{
	static const auto icons = [] {
		constexpr std::uint32_t ink = 0x35465b;
		mario::IconFont font;
		return ControlIcons{font.render(0xf0e8, ink),
			font.render(0xf28d, ink), font.render(0xf057, 0xa43639)};
	}();
	return icons;
}

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

std::string bytes(double amount)
{
	constexpr std::array units{"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
	std::size_t unit = 0;
	while (amount >= 1024 && unit + 1 < units.size()) {
		amount /= 1024;
		++unit;
	}
	char buffer[48];
	std::snprintf(buffer, sizeof(buffer), "%.*f %s", unit ? 1 : 0, amount, units[unit]);
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

class NetworkPage final : public Fl_Widget {
public:
	NetworkPage(int xpos, int ypos, int width, int height)
		: Fl_Widget(xpos, ypos, width, height, "Network") { }

	void update(const Snapshot &snapshot)
	{
		network_ = snapshot.network;
		redraw();
	}

private:
	void draw() override
	{
		fl_push_clip(x(), y(), w(), h());
		fl_color(FL_WHITE);
		fl_rectf(x(), y(), w(), h());
		const auto row = [this](const std::string &value, int top, Fl_Font font = FL_HELVETICA) {
			text(value, x() + 12, y() + top, w() - 24, 23, font);
		};
		if (!network_) {
			row("Network statistics unavailable", 8);
		} else if (network_->flags & MARIO_NET_PROBE_FAILED) {
			row("Ethernet initialization failed", 8);
		} else if (!(network_->flags & MARIO_NET_PRESENT)) {
			row("No Ethernet interface detected", 8);
		} else {
			const auto &network = *network_;
			const char *link = !(network.flags & MARIO_NET_UP) ? "Disabled" :
				(network.flags & MARIO_NET_CARRIER) ? "Connected" : "No carrier";
			row(std::string("eth0: ") + link, 8, FL_HELVETICA_BOLD);
			char address[INET_ADDRSTRLEN];
			row(network.address && inet_ntop(AF_INET, &network.address, address, sizeof(address)) ?
				std::string("IPv4: ") + address + "/" + std::to_string(network.prefix) : "IPv4: Not configured", 32);
			row("Received: " + bytes(network.rx_bytes) + " total, " +
				(network.rx_per_second ? bytes(*network.rx_per_second) + "/s" : "--"), 62);
			row("Sent: " + bytes(network.tx_bytes) + " total, " +
				(network.tx_per_second ? bytes(*network.tx_per_second) + "/s" : "--"), 86);
		}
		fl_pop_clip();
	}

	std::optional<Network> network_;
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
		cols(7);
		col_header(1);
		col_header_height(25);
		col_header_color(fl_rgb_color(222, 224, 228));
		when(FL_WHEN_NEVER);
		end();
		resize(xpos, ypos, width, height);
	}

	void update(const Snapshot &snapshot, const std::string &filter, bool tree)
	{
		const auto needle = fold(filter);
		filtering_ = !needle.empty();
		std::vector<const Task *> ordered;
		if (selected_ && std::none_of(snapshot.tasks.begin(), snapshot.tasks.end(), [&](const Task &task) {
			return task.identity == *selected_;
		}))
			selected_.reset();
		for (const auto &task : snapshot.tasks)
			if (tree || matches(task, needle))
				ordered.push_back(&task);
		std::sort(ordered.begin(), ordered.end(), [&](const Task *left, const Task *right) {
			if (column_ == 3 && bool(left->machine_percent) != bool(right->machine_percent))
				return bool(left->machine_percent);
			if (descending_)
				std::swap(left, right);
			switch (column_) {
			case 1: if (left->name != right->name) return left->name < right->name; break;
			case 2: if (left->state != right->state) return left->state < right->state; break;
			case 3: if (left->machine_percent != right->machine_percent) return left->machine_percent < right->machine_percent; break;
			case 4: if (ticks(*left) != ticks(*right)) return ticks(*left) < ticks(*right); break;
			case 5: if (left->resident_kib != right->resident_kib) return left->resident_kib < right->resident_kib; break;
			case 6: if (net_total(*left) != net_total(*right)) return net_total(*left) < net_total(*right); break;
			}
			return left->identity.pid < right->identity.pid;
		});
		std::vector<Row> display;
		display.reserve(ordered.size());
		if (tree)
			append_tree(display, ordered, needle);
		else
			for (const auto *task : ordered)
				display.push_back(row(*task));
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
			const int index = callback_row();
			if (index >= 0 && index < rows()) {
				const auto &item = display_[index];
				selected_ = item.id;
				if (click && callback_col() == 1 && item.branch && !filtering_) {
					int xpos, ypos, width, height;
					if (find_cell(CONTEXT_CELL, index, 1, xpos, ypos, width, height) == 0) {
						const int offset = Fl::event_x() - branch_x(item, xpos, width);
						if (offset >= 0 && offset < indent)
							toggle_branch(item);
					}
				}
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

	bool selection_is_displayed() const
	{
		return selected_ && std::any_of(display_.begin(), display_.end(), [&](const auto &row) {
			return row.id == *selected_;
		});
	}

	void resize(int xpos, int ypos, int width, int height) override
	{
		Fl_Table::resize(xpos, ypos, width, height);
		col_width(0, 74);
		col_width(1, std::max(120, width - Fl::scrollbar_size() - 484));
		col_width(2, 56);
		col_width(3, 74);
		col_width(4, 86);
		col_width(5, 86);
		col_width(6, 104);
	}

private:
	static constexpr int indent = 14;

	struct Row {
		Identity id;
		std::array<std::string, 7> cells;
		int depth = -1;
		bool branch = false, expanded = false;
		bool operator==(const Row &other) const
		{
			return id == other.id && cells == other.cells && depth == other.depth &&
				branch == other.branch && expanded == other.expanded;
		}
	};

	static bool matches(const Task &task, const std::string &needle)
	{
		return needle.empty() || fold(printable(task.name)).find(needle) != std::string::npos ||
			std::to_string(task.identity.pid).find(needle) != std::string::npos;
	}

	static Row row(const Task &task)
	{
		return {task.identity, {std::to_string(task.identity.pid), printable(task.name),
			printable(std::string(1, task.state)), percent(task.machine_percent),
			std::to_string(ticks(task)), mib(task.resident_kib),
			bytes(task.rx_bytes) + " / " + bytes(task.tx_bytes)}};
	}

	void append_tree(std::vector<Row> &display, const std::vector<const Task *> &ordered, const std::string &needle)
	{
		const auto root = ordered.size();
		std::unordered_map<int, std::size_t> by_pid;
		by_pid.reserve(root);
		for (std::size_t index = 0; index < root; ++index)
			by_pid.emplace(ordered[index]->identity.pid, index);
		for (auto entry = collapsed_.begin(); entry != collapsed_.end();) {
			const auto task = by_pid.find(entry->first);
			if (task == by_pid.end() || ordered[task->second]->identity.start_ticks != entry->second)
				entry = collapsed_.erase(entry);
			else
				++entry;
		}

		std::vector<std::size_t> parents(root, root), visited(root);
		for (std::size_t index = 0; index < root; ++index) {
			const auto &task = *ordered[index];
			const auto parent = by_pid.find(task.parent);
			if (parent != by_pid.end() && parent->second != index &&
			    ordered[parent->second]->identity.start_ticks <= task.identity.start_ticks)
				parents[index] = parent->second;
		}
		for (std::size_t index = 0; index < root; ++index) {
			auto current = index;
			while (current != root && !visited[current]) {
				visited[current] = index + 1;
				current = parents[current];
			}
			if (current != root && visited[current] == index + 1)
				parents[current] = root;
		}

		std::vector<bool> included(root, needle.empty());
		if (!needle.empty())
			for (std::size_t index = 0; index < root; ++index)
				if (matches(*ordered[index], needle))
					for (auto current = index; current != root && !included[current]; current = parents[current])
						included[current] = true;
		std::vector<std::vector<std::size_t>> children(root + 1);
		for (std::size_t index = 0; index < root; ++index)
			if (included[index])
				children[parents[index]].push_back(index);

		std::vector<std::pair<std::size_t, int>> pending;
		const auto push_children = [&](std::size_t parent, int depth) {
			for (auto child = children[parent].rbegin(); child != children[parent].rend(); ++child)
				pending.emplace_back(*child, depth);
		};
		push_children(root, 0);
		while (!pending.empty()) {
			const auto [index, depth] = pending.back();
			pending.pop_back();
			auto item = row(*ordered[index]);
			item.depth = depth;
			item.branch = !children[index].empty();
			item.expanded = item.branch && (filtering_ || !collapsed_.count(item.id.pid));
			if (item.expanded)
				push_children(index, depth + 1);
			display.push_back(std::move(item));
		}
	}

	void toggle_branch(const Row &item)
	{
		if (item.expanded)
			collapsed_[item.id.pid] = item.id.start_ticks;
		else
			collapsed_.erase(item.id.pid);
	}

	static int branch_x(const Row &item, int xpos, int width)
	{
		return xpos + 6 + std::min(item.depth, std::max(0, (width - 100) / indent)) * indent;
	}

	static uint64_t ticks(const Task &task)
	{
		return task.user > UINT64_MAX - task.system ? UINT64_MAX : task.user + task.system;
	}

	static uint64_t net_total(const Task &task)
	{
		return task.rx_bytes > UINT64_MAX - task.tx_bytes ? UINT64_MAX : task.rx_bytes + task.tx_bytes;
	}

	void draw_cell(TableContext context, int row, int col, int xpos, int ypos, int width, int height) override
	{
		if (context != CONTEXT_COL_HEADER && context != CONTEXT_CELL)
			return;
		static const char *const columns[] = {"PID", "Name", "State", "CPU %", "CPU ticks", "Memory", "Network"};
		fl_push_clip(xpos, ypos, width, height);
		if (context == CONTEXT_COL_HEADER) {
			fl_draw_box(FL_THIN_UP_BOX, xpos, ypos, width, height, col_header_color());
			text(std::string(columns[col]) + (col == column_ ? (descending_ ? " -" : " +") : ""),
			     xpos + 6, ypos, width - 12, height);
		} else if (row >= 0 && static_cast<std::size_t>(row) < display_.size()) {
			const auto &item = display_[row];
			fl_color(selected_ && item.id == *selected_ ? highlight : row % 2 ? stripe : FL_WHITE);
			fl_rectf(xpos, ypos, width, height);
			int label_x = xpos + 6;
			if (col == 1 && item.depth >= 0) {
				const int branch = branch_x(item, xpos, width);
				if (item.branch) {
					const int center = ypos + height / 2;
					fl_color(FL_DARK3);
					if (item.expanded)
						fl_polygon(branch + 2, center - 2, branch + 10, center - 2, branch + 6, center + 3);
					else
						fl_polygon(branch + 3, center - 4, branch + 3, center + 4, branch + 8, center);
				}
				label_x = branch + indent;
			}
			text(item.cells[col], label_x, ypos, xpos + width - label_x - 6, height);
		}
		fl_pop_clip();
	}

	std::vector<Row> display_;
	std::unordered_map<int, uint64_t> collapsed_;
	std::optional<Identity> selected_;
	int column_ = 0;
	bool descending_ = false, filtering_ = false;
};

class Processes final : public Fl_Group {
public:
	Processes(int xpos, int ypos, int width, int height, std::function<void()> on_signal)
		: Fl_Group(xpos, ypos, width, height, "Processes"), filter_(xpos + 60, ypos + 8, 260, 28, "Filter:"),
		tree_(xpos + 330, ypos + 8, 64, 28, "Tree"), task_count_(xpos + 406, ypos + 8, width - 424, 28),
		tasks_(xpos + 10, ypos + 44, width - 20, height - 120),
		detail_name_(xpos + 10, ypos + height - 71, width - 324, 24),
		details_(xpos + 10, ypos + height - 47, width - 20, 34),
		end_(xpos + width - 306, ypos + height - 72, 144, 26, "End Process…"),
		force_(xpos + width - 154, ypos + height - 72, 144, 26, "Force Kill…"),
		on_signal_(std::move(on_signal))
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
		mario::set_label_icon(tree_, control_icons().tree.get());
		tree_.tooltip("Group tasks by parent process");
		tree_.callback([](Fl_Widget *, void *data) { static_cast<Processes *>(data)->refresh(); }, this);
		mario::set_label_icon(end_, control_icons().end_process.get());
		mario::set_label_icon(force_, control_icons().force_kill.get());
		end_.tooltip("Ask the selected process to exit (SIGTERM)");
		force_.tooltip("Force the selected process to exit (SIGKILL)");
		end_.callback([](Fl_Widget *, void *data) { static_cast<Processes *>(data)->signal_selected(SIGTERM); }, this);
		force_.callback([](Fl_Widget *, void *data) { static_cast<Processes *>(data)->signal_selected(SIGKILL); }, this);
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
	bool can_signal(const Task *task) const
	{
		return task && task->identity.pid > 1 && task->identity.pid == task->tgid &&
			task->identity.pid != self_pid_ && task->parent != self_pid_ &&
			task->state != 'Z' && task->identity.start_ticks <= ULONG_MAX && tasks_.selection_is_displayed();
	}

	void signal_selected(int signal)
	{
		const auto *task = tasks_.selected(*snapshot_);
		if (!can_signal(task))
			return;
		const auto identity = task->identity;
		const auto name = printable(task->name);
		const bool force = signal == SIGKILL;
		if (fl_choice("%s %s (PID %d)?\n%s", nullptr, "&Cancel", force ? "&Kill" : "&End Process",
			      force ? "Force kill" : "End", name.c_str(), identity.pid,
			      force ? "Unsaved work may be lost." : "The process can handle or ignore this request.") != 2 ||
		    !window()->shown())
			return;
		if (prctl(MARIO_PR_SIGNAL_PROCESS, static_cast<unsigned long>(identity.pid),
			  static_cast<unsigned long>(identity.start_ticks), static_cast<unsigned long>(signal), 0UL) < 0) {
			const auto error = errno;
			fl_alert("Could not signal %s (PID %d).\n%s", name.c_str(), identity.pid,
				 error == ESRCH ? "The selected process is no longer available." : std::strerror(error));
		}
		on_signal_();
	}

	void refresh()
	{
		tasks_.update(*snapshot_, filter_.value(), tree_.value());
		set_label(task_count_, std::to_string(tasks_.rows()) + " of " + std::to_string(snapshot_->tasks.size()) +
			" tasks" + status(snapshot_->task_status));
		const auto *task = tasks_.selected(*snapshot_);
		if (can_signal(task)) {
			end_.activate();
			force_.activate();
		} else {
			end_.deactivate();
			force_.deactivate();
		}
		if (task) {
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
	Fl_Toggle_Button tree_;
	Fl_Box task_count_;
	TaskTable tasks_;
	Fl_Box detail_name_, details_;
	Fl_Button end_, force_;
	std::function<void()> on_signal_;
	const pid_t self_pid_ = getpid();
	const Snapshot *snapshot_ = nullptr;
};

class Monitor {
public:
	Monitor(Sampler sampler, SampleObserver observer)
		: sampler_(std::move(sampler)), observer_(std::move(observer)),
		window_(92, 78, 840, 580, "System Monitor"),
		tabs_(12, 8, 816, 564), overview_(12, 34, 816, 534, "Overview"),
		processes_(12, 34, 816, 534, [this] { if (window_.shown()) start(); }),
		mounts_(12, 34, 816, 534, "Filesystems"), network_(12, 34, 816, 534)
	{
		tabs_.end();
		tabs_.value(&overview_);
		window_.end();
		window_.resizable(tabs_);
		window_.size_range(660, 380);
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
		self.start();
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
			self.network_.update(self.snapshot_);
			self.processes_.update(self.snapshot_);
			if (self.observer_)
				self.observer_(self.snapshot_);
		}
	}

	void start()
	{
		if (worker_.active())
			return;
		if (worker_.start(sampler_)) {
			Fl::add_fd(worker_.fd(), FL_READ, pipe_ready, this);
		}
	}

	void stop()
	{
		if (worker_.fd() >= 0)
			Fl::remove_fd(worker_.fd());
		Fl::remove_timeout(reap, this);
		worker_.stop();
	}

	void close()
	{
		Fl::remove_timeout(tick, this);
		stop();
		window_.hide();
	}

	Sampler sampler_;
	SampleObserver observer_;
	Snapshot snapshot_;
	SampleWorker worker_;
	Fl_Double_Window window_;
	Fl_Tabs tabs_;
	ScrollPage<Overview> overview_;
	Processes processes_;
	ScrollPage<Mounts> mounts_;
	NetworkPage network_;
};

}

int run(Sampler sampler, SampleObserver observer)
{
	Monitor monitor(std::move(sampler), std::move(observer));
	return Fl::run();
}

}
