#include "browser.h"
#include "copy_worker.h"
#include "icons.h"
#include "preview.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Multiline_Output.H>
#include <FL/Fl_Image.H>
#include <FL/Fl_Table_Row.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Tree.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <font-icons.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <set>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr const char *columns[] = {"Name", "Type", "Size", "Modified"};

const char *action_label(files::CopyAction action)
{
	using files::CopyAction;
	switch (action) {
	case CopyAction::copy: return "Copy";
	case CopyAction::move: return "Move";
	case CopyAction::rename: return "Rename";
	case CopyAction::mkdir: return "New Folder";
	case CopyAction::trash: return "Trash";
	case CopyAction::restore: return "Restore";
	case CopyAction::recover: return "Recover Copy";
	case CopyAction::remove: return "Permanent Delete";
	}
	return "File operation";
}

std::string describe_selection(const std::vector<std::string> &names)
{
	std::string summary = std::to_string(names.size()) + " selected item(s)";
	const auto shown = std::min<std::size_t>(names.size(), 5);
	for (std::size_t index = 0; index < shown; ++index)
		summary += "\n  " + files::display(names[index]);
	if (names.size() > shown)
		summary += "\n  … and " + std::to_string(names.size() - shown) + " more";
	return summary;
}

std::string modified(const files::Entry &entry)
{
	char buffer[32];
	struct tm timestamp;
	const time_t seconds = entry.info.st_mtim.tv_sec;
	if (entry.error || !gmtime_r(&seconds, &timestamp) ||
	    !strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &timestamp))
		return "--";
	return buffer;
}

class Listing final : public Fl_Table_Row {
public:
	Listing() : Fl_Table_Row(0, 0, 1, 1)
	{
		cols(4);
		col_header(1);
		col_header_height(23);
		col_header_color(fl_rgb_color(222, 224, 228));
		color(FL_WHITE);
		selection_color(fl_rgb_color(188, 217, 251));
		type(SELECT_MULTI);
		when(FL_WHEN_RELEASE);
		end();
	}

	const files::Entry *entry(int row) const
	{
		return row >= 0 && static_cast<std::size_t>(row) < visible.size() ? &(*entries)[visible[row]] : nullptr;
	}

	std::vector<std::string> selection()
	{
		std::vector<std::string> names;
		for (int row = 0; row < rows(); ++row)
			if (row_selected(row))
				names.push_back(entry(row)->name);
		return names;
	}

	void clear_rows()
	{
		rows(0);
		visible.clear();
		select_all_rows(0);
	}

	void update(const std::vector<files::Entry> &source, const char *filter, const files::Preferences &preferences)
	{
		const auto selection_before = selection();
		const std::set<std::string> keep(selection_before.begin(), selection_before.end());
		entries = &source;
		sort = preferences.column;
		descending = preferences.descending;
		visible = files::order(source, filter, preferences.hidden, sort, descending);
		select_all_rows(0);
		rows(visible.size());
		row_height_all(22);
		for (int row = 0; !keep.empty() && row < rows(); ++row)
			if (keep.count(entry(row)->name))
				select_row(row, 1);
		redraw();
	}

	void resize(int xpos, int ypos, int width, int height) override
	{
		Fl_Table_Row::resize(xpos, ypos, width, height);
		constexpr int type_width = 90, size_width = 67, modified_width = 137;
		const int name_width = width - Fl::scrollbar_size() - 4 - type_width - size_width - modified_width;
		col_width(0, std::max(name_width, 126));
		col_width(1, type_width);
		col_width(2, size_width);
		col_width(3, modified_width);
	}

private:
	void draw_cell(TableContext context, int row, int col, int xpos, int ypos, int width, int height) override
	{
		if (context != CONTEXT_COL_HEADER && context != CONTEXT_CELL)
			return;
		fl_push_clip(xpos, ypos, width, height);
		std::string text;
		int text_x = xpos + 5;
		if (context == CONTEXT_COL_HEADER) {
			fl_draw_box(FL_THIN_UP_BOX, xpos, ypos, width, height, col_header_color());
			text = columns[col];
			if (static_cast<int>(sort) == col)
				text += descending ? " -" : " +";
		} else if (const auto *item = entry(row)) {
			fl_color(row_selected(row) ? selection_color() : row % 2 ? fl_rgb_color(246, 248, 251) : FL_WHITE);
			fl_rectf(xpos, ypos, width, height);
			switch (static_cast<files::Column>(col)) {
			case files::Column::name:
				text = item->label;
				if (auto *image = files::icon(files::entry_icon(*item))) {
					image->draw(text_x, ypos + (height - image->h()) / 2);
					text_x += image->w() + 4;
				}
				break;
			case files::Column::type: text = files::kind(*item); break;
			case files::Column::size:
				text = !item->error && S_ISREG(item->info.st_mode) ? std::to_string(item->info.st_size) : "--";
				break;
			case files::Column::modified: text = modified(*item); break;
			}
		}
		fl_font(FL_HELVETICA, 13);
		fl_color(FL_BLACK);
		fl_draw(text.c_str(), text_x, ypos + (height + fl_height()) / 2 - fl_descent());
		fl_pop_clip();
	}

	const std::vector<files::Entry> *entries = nullptr;
	std::vector<std::size_t> visible;
	files::Column sort = files::Column::name;
	bool descending = false;
};

class Pane final : public Fl_Group {
public:
	Pane() : Fl_Group(0, 0, 1, 1), heading(0, 0, 1, 1), filter(0, 0, 1, 1, "Filter:"), listing()
	{
		heading.box(FL_FLAT_BOX);
		heading.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
		heading.labelsize(13);
		filter.labelsize(13);
		filter.when(FL_WHEN_CHANGED);
		end();
	}

	void resize(int xpos, int ypos, int width, int height) override
	{
		Fl_Widget::resize(xpos, ypos, width, height);
		heading.resize(xpos, ypos, width, 23);
		filter.resize(xpos + 47, ypos + 25, width - 49, 24);
		listing.resize(xpos, ypos + 54, width, height - 54);
	}

	int handle(int event) override
	{
		if (event == FL_PUSH)
			do_callback();
		return Fl_Group::handle(event);
	}

	Fl_Box heading;
	Fl_Input filter;
	Listing listing;
	files::Scan scan;
	std::string path, current_name;
	std::vector<std::string> history;
	std::size_t position = 0;
	bool queued = false;
};

class Tab final : public Fl_Group {
public:
	Tab() : Fl_Group(0, 0, 1, 1), panes{new Pane, new Pane}
	{
		end();
	}

	void arrange(int xpos, int ypos, int width, int height, bool split)
	{
		Fl_Widget::resize(xpos, ypos, width, height);
		if (split) {
			panes[0]->resize(xpos, ypos, (width - 8) / 2, height);
			panes[1]->resize(xpos + (width + 8) / 2, ypos, (width - 8) / 2, height);
			panes[1]->show();
		} else {
			panes[0]->resize(xpos, ypos, width, height);
			panes[1]->hide();
			active = 0;
		}
	}

	std::array<Pane *, 2> panes;
	int active = 0;
};

class TabStrip final : public Fl_Tabs {
public:
	TabStrip(int xpos, int ypos, int width, int height) : Fl_Tabs(xpos, ypos, width, height) { end(); }
};

class Browser {
public:
	Browser(const char *initial) : window_(50, 80, 924, 550, "File Manager"),
		menu_(0, 0, 924, 25), back_(8, 29, 60, 26, "Back"), forward_(71, 29, 82, 26, "Forward"),
		up_(156, 29, 47, 26, "Up"), home_button_(206, 29, 64, 26, "Home"),
		location_(274, 29, 529, 26), go_(807, 29, 43, 26, "Go"), split_(854, 29, 62, 26, "Split"),
		sidebar_(8, 64, 144, 459), tabs_(160, 60, 578, 463),
		preview_text_(746, 78, 170, 445, "Preview"), preview_image_(746, 86, 170, 136),
		status_(8, 525, 908, 23), job_status_(8, 525, 656, 23),
		skip_(670, 525, 66, 23, "Skip"), keep_(742, 525, 92, 23, "Keep Both"), cancel_(840, 525, 76, 23, "Cancel")
	{
		window_.end();
		window_.resizable(tabs_);
		window_.size_range(780, 360);
		from_home();
		preferences_.load(home_);
		mario::set_label_icon(back_, files::icon(files::Icon::back));
		mario::set_label_icon(forward_, files::icon(files::Icon::forward));
		mario::set_label_icon(up_, files::icon(files::Icon::up));
		mario::set_label_icon(home_button_, files::icon(files::Icon::home));
		mario::set_label_icon(split_, files::icon(files::Icon::split));
		sidebar_.showroot(0);
		sidebar_.item_labelsize(13);
		sidebar_.selection_color(fl_rgb_color(188, 217, 251));
		sidebar_.callback(sidebar_clicked, this);
		tabs_.callback(invoke<&Browser::changed_tab>, this);
		back_.callback(invoke<&Browser::back>, this);
		forward_.callback(invoke<&Browser::forward>, this);
		up_.callback(invoke<&Browser::up>, this);
		home_button_.callback(invoke<&Browser::home>, this);
		go_.callback(invoke<&Browser::go>, this);
		location_.when(FL_WHEN_ENTER_KEY_ALWAYS);
		location_.callback(invoke<&Browser::go>, this);
		split_.callback(invoke<&Browser::toggle_split>, this);
		status_.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
		job_status_.align(status_.align());
		job_status_.hide();
		skip_.hide();
		keep_.hide();
		cancel_.hide();
		skip_.callback(invoke<&Browser::skip_conflict>, this);
		keep_.callback(invoke<&Browser::keep_conflict>, this);
		cancel_.callback(invoke<&Browser::cancel_copy>, this);
		preview_text_.align(FL_ALIGN_TOP_LEFT);
		preview_text_.textsize(13);
		add_menus();
		if (!new_tab(initial ? absolute(initial) : home_))
			new_tab("/");
		fill_sidebar();
		window_.callback(invoke<&Browser::close>, this);
		window_.show();
		Fl::add_timeout(0.02, tick, this);
	}

	~Browser()
	{
		Fl::remove_timeout(tick, this);
		Fl::remove_timeout(show_preview, this);
		if (worker_.fd() >= 0)
			Fl::remove_fd(worker_.fd());
		preview_image_.image(nullptr);
	}

private:
	template<void (Browser::*action)()>
	static void invoke(Fl_Widget *, void *data) { (static_cast<Browser *>(data)->*action)(); }

	void add_menus()
	{
		menu_.add("&File/&New Tab", FL_CTRL + 't', invoke<&Browser::new_tab_here>, this);
		menu_.add("&File/&Close Tab", FL_CTRL + 'w', invoke<&Browser::close_tab>, this);
		menu_.add("&File/&Open Selected", FL_CTRL + FL_Enter, invoke<&Browser::open_selected>, this);
		menu_.add("&File/New &Folder", FL_CTRL + FL_SHIFT + 'n', invoke<&Browser::new_folder>, this);
		menu_.add("&File/&Rename", FL_F + 2, invoke<&Browser::rename_selection>, this);
		menu_.add("&File/&Properties", FL_ALT + FL_Enter, invoke<&Browser::properties>, this);
		menu_.add("&File/&Quit", FL_CTRL + 'q', invoke<&Browser::close>, this);
		menu_.add("&Edit/&Copy", FL_CTRL + 'c', invoke<&Browser::copy_selection>, this);
		menu_.add("&Edit/Cu&t", FL_CTRL + 'x', invoke<&Browser::cut_selection>, this);
		menu_.add("&Edit/&Paste", FL_CTRL + 'v', invoke<&Browser::paste>, this);
		menu_.add("&Edit/Move to &Trash", FL_Delete, invoke<&Browser::trash_selection>, this);
		menu_.add("&Edit/Delete &Permanently", FL_SHIFT + FL_Delete, invoke<&Browser::delete_selection>, this);
		menu_.add("&Edit/Restore or Recover Selected", FL_CTRL + FL_SHIFT + 'r', invoke<&Browser::restore_selection>, this);
		menu_.add("&Edit/&Last Job Summary", 0, invoke<&Browser::copy_summary>, this);
		menu_.add("&View/&Refresh", FL_CTRL + 'r', invoke<&Browser::refresh>, this);
		menu_.add("&View/Toggle &Hidden Files", FL_CTRL + 'h', invoke<&Browser::toggle_hidden>, this);
		menu_.add("&View/Toggle &Split", FL_CTRL + '\\', invoke<&Browser::toggle_split>, this);
		menu_.add("&View/Other &Pane", FL_CTRL + FL_Tab, invoke<&Browser::other_pane>, this);
		menu_.add("&Go/&Location", FL_CTRL + 'l', invoke<&Browser::focus_location>, this);
		menu_.add("&Go/&Back", FL_ALT + FL_Left, invoke<&Browser::back>, this);
		menu_.add("&Go/&Forward", FL_ALT + FL_Right, invoke<&Browser::forward>, this);
		menu_.add("&Go/&Up", FL_ALT + FL_Up, invoke<&Browser::up>, this);
		menu_.add("&Go/&Home", FL_ALT + FL_Home, invoke<&Browser::home>, this);
		menu_.add("&Go/Volume &Trash", FL_CTRL + FL_SHIFT + 't', invoke<&Browser::open_trash>, this);
		menu_.add("&Bookmarks/&Add or Remove Current", FL_CTRL + 'd', invoke<&Browser::bookmark>, this);
	}

	void from_home()
	{
		const auto *home = std::getenv("HOME");
		if (home && *home == '/')
			home_ = home;
		else
			home_ = "/";
	}

	std::string absolute(const std::string &path)
	{
		if (!path.empty() && path.front() == '/')
			return path;
		if (tabs_.value())
			return files::join(tab().panes[tab().active]->path, path);
		std::unique_ptr<char, decltype(&std::free)> cwd(getcwd(nullptr, 0), std::free);
		return cwd ? files::join(cwd.get(), path) : path;
	}

	Tab &tab() { return *static_cast<Tab *>(tabs_.value()); }
	Pane &pane() { return *tab().panes[tab().active]; }

	void message(const std::string &text) { status_.copy_label(text.c_str()); }

	bool resolve(const std::string &input, std::string &resolved)
	{
		int error = files::directory_path(input, false, resolved);
		if (error == ELOOP) {
			if (fl_choice("Follow directory links in this path?\n%s", "Cancel", "Follow", nullptr,
				      files::display(input).c_str()) != 1)
				return false;
			error = files::directory_path(input, true, resolved);
		}
		if (error)
			message("Cannot open " + files::display(input) + ": " + std::strerror(error));
		return !error;
	}

	void queue_scan(Pane &target)
	{
		target.listing.clear_rows();
		target.scan.cancel();
		target.queued = true;
		target.current_name.clear();
	}

	void set_path(Pane &target, const std::string &path, bool remember)
	{
		if (remember && (target.history.empty() || target.history[target.position] != path)) {
			if (!target.history.empty())
				target.history.resize(target.position + 1);
			if (target.history.size() == 64)
				target.history.erase(target.history.begin());
			target.history.push_back(path);
			target.position = target.history.size() - 1;
		}
		target.path = path;
		target.filter.value("");
		queue_scan(target);
	}

	void navigate(const std::string &path)
	{
		std::string resolved;
		if (resolve(path, resolved)) {
			set_path(pane(), resolved, true);
			update_active();
		}
	}

	bool new_tab(const std::string &path)
	{
		if (tabs_.children() >= 12) {
			message("At most 12 tabs can be open");
			return false;
		}
		std::string resolved;
		if (!resolve(path, resolved))
			return false;
		tabs_.begin();
		auto *created = new Tab;
		tabs_.end();
		for (auto *child : created->panes) {
			child->callback(pane_clicked, this);
			child->listing.callback(listing_clicked, this);
			child->filter.callback(filter_changed, this);
			set_path(*child, resolved, true);
		}
		tabs_.value(created);
		layout_tabs();
		changed_tab();
		return true;
	}

	void layout_tabs()
	{
		for (int index = 0; index < tabs_.children(); ++index)
			static_cast<Tab *>(tabs_.child(index))->arrange(tabs_.x() + 2, tabs_.y() + 28,
				tabs_.w() - 4, tabs_.h() - 29, preferences_.split);
		tabs_.redraw();
	}

	void update_active()
	{
		for (auto *target : tab().panes) {
			const auto caption = std::string(target == &pane() ? "> " : "  ") + files::display(target->path);
			target->heading.copy_label(caption.c_str());
			target->heading.color(target == &pane() ? fl_rgb_color(202, 222, 245) : FL_BACKGROUND_COLOR);
		}
		const auto &first = tab().panes[0]->path;
		tab().copy_label(files::display(first == "/" ? first : first.substr(first.rfind('/') + 1)).c_str());
		location_.value(files::display(pane().path).c_str());
		pane().position ? back_.activate() : back_.deactivate();
		pane().position + 1 < pane().history.size() ? forward_.activate() : forward_.deactivate();
		update_status();
		schedule_preview();
		tabs_.redraw();
	}

	void update_status()
	{
		showing_copy_summary_ = false;
		auto &target = pane();
		if (target.queued || target.scan.pending())
			message("Reading directory... " + std::to_string(target.scan.entries.size()));
		else if (target.scan.error)
			message(std::string("Directory read: ") + std::strerror(target.scan.error));
		else
			message(std::to_string(target.listing.rows()) + " shown of " + std::to_string(target.scan.entries.size()) +
				(target.scan.limited ? " (4,096 entry limit)" : "") +
				" | " + std::to_string(target.listing.selection().size()) + " selected");
	}

	void changed_tab() { update_active(); }
	void new_tab_here() { new_tab(pane().path); }
	void close_tab()
	{
		if (tabs_.children() <= 1)
			return;
		delete &tab();
		changed_tab();
	}
	void history(int delta)
	{
		auto &target = pane();
		const auto index = static_cast<int>(target.position) + delta;
		std::string resolved;
		if (index >= 0 && static_cast<std::size_t>(index) < target.history.size() && resolve(target.history[index], resolved)) {
			target.position = index;
			set_path(target, resolved, false);
			update_active();
		}
	}
	void back() { history(-1); }
	void forward() { history(1); }
	void up() { navigate(files::parent(pane().path)); }
	void home() { navigate(home_); }
	void go()
	{
		const std::string input(location_.value());
		if (!input.empty() && input != files::display(pane().path))
			navigate(absolute(input));
	}
	void focus_location()
	{
		location_.take_focus();
		location_.insert_position(location_.size(), 0);
	}
	void other_pane()
	{
		if (preferences_.split) {
			tab().active = 1 - tab().active;
			update_active();
			pane().listing.take_focus();
		}
	}
	void save_preferences()
	{
		if (!preferences_.save(home_))
			message("Could not save File Manager preferences");
	}
	void toggle_split()
	{
		preferences_.split = !preferences_.split;
		layout_tabs();
		update_active();
		save_preferences();
	}
	void rebuild(Pane &target)
	{
		if (!target.queued && !target.scan.pending())
			target.listing.update(target.scan.entries, target.filter.value(), preferences_);
	}
	void rebuild_all()
	{
		for (int index = 0; index < tabs_.children(); ++index)
			for (auto *target : static_cast<Tab *>(tabs_.child(index))->panes)
				rebuild(*target);
		update_status();
		schedule_preview();
		save_preferences();
	}
	void toggle_hidden()
	{
		preferences_.hidden = !preferences_.hidden;
		rebuild_all();
	}
	void refresh()
	{
		cache_.clear();
		queue_scan(pane());
		fill_sidebar();
		update_active();
	}
	void set_clipboard(files::CopyAction action)
	{
		if (worker_.active())
			return;
		auto names = pane().listing.selection();
		if (names.empty()) {
			message("Select the files or folders first");
			return;
		}
		clipboard_ = {pane().path, {}, std::move(names), action};
		message(std::to_string(clipboard_.names.size()) + " selected for " + action_label(action));
	}
	void copy_selection() { set_clipboard(files::CopyAction::copy); }
	void cut_selection() { set_clipboard(files::CopyAction::move); }
	void paste()
	{
		if (worker_.active())
			return;
		if (clipboard_.names.empty()) {
			message("Copy or cut a selection before pasting");
			return;
		}
		auto request = clipboard_;
		request.destination = pane().path;
		if (request.action == files::CopyAction::move) {
			if (request.source == request.destination) {
				message("Choose a different folder for the move");
				return;
			}
			if (fl_choice("Move %s\n\nFrom: %s\nTo: %s", "Cancel", "&Move", nullptr,
				      describe_selection(request.names).c_str(), files::display(request.source).c_str(),
				      files::display(request.destination).c_str()) != 1)
				return;
		}
		start_job(std::move(request));
	}
	void new_folder()
	{
		if (worker_.active())
			return;
		if (const auto *name = fl_input("Name of the new folder:", ""))
			start_job({pane().path, {}, {name}, files::CopyAction::mkdir});
	}
	void rename_selection()
	{
		if (worker_.active())
			return;
		auto names = pane().listing.selection();
		if (names.size() != 1) {
			message("Select one item to rename");
			return;
		}
		const auto label = files::display(names.front());
		const auto *name = fl_input("New name for %s:", label == names.front() ? label.c_str() : "", label.c_str());
		if (name && name != names.front())
			start_job({pane().path, name, std::move(names), files::CopyAction::rename});
	}
	void trash_selection()
	{
		if (worker_.active())
			return;
		auto names = pane().listing.selection();
		if (!names.empty())
			start_job({pane().path, {}, std::move(names), files::CopyAction::trash});
	}
	void delete_selection()
	{
		if (worker_.active())
			return;
		auto names = pane().listing.selection();
		if (!names.empty() && fl_choice("Permanently delete %s\n\nFrom: %s\nThis bypasses Trash.",
						nullptr, "&Cancel", "&Delete Permanently", describe_selection(names).c_str(),
						files::display(pane().path).c_str()) == 2)
			start_job({pane().path, {}, std::move(names), files::CopyAction::remove});
	}
	void open_trash()
	{
		std::string path;
		if (const int error = files::trash_directory(pane().path, path)) {
			message(std::string("No accessible Trash on this volume: ") + std::strerror(error));
			return;
		}
		navigate(path);
	}
	void restore_selection()
	{
		if (worker_.active())
			return;
		auto names = pane().listing.selection();
		if (names.size() != 1) {
			message("Select one slot in Volume Trash");
			return;
		}
		files::TrashItem slot;
		if (const int error = files::trash_item(pane().path, names.front(), slot)) {
			message(std::string("Cannot read the Trash slot: ") + std::strerror(error));
			return;
		}
		if (slot.ready) {
			if (fl_choice("Restore to:\n%s", "Cancel", "&Restore", nullptr, files::display(slot.original).c_str()) == 1)
				start_job({pane().path, slot.original, std::move(names), files::CopyAction::restore});
			return;
		}
		if (!slot.has_payload) {
			fl_message("Incomplete Trash slot: no saved item. Use Delete Permanently to discard the slot.");
			return;
		}
		const auto *folder = fl_input("Incomplete Trash slot: %s\nThe copy may be partial. The slot will be kept.\n"
			"Enter an existing folder for the recovered copy:", "",
			files::display(slot.original.empty() ? names.front() : slot.original).c_str());
		std::string path;
		if (folder && *folder && resolve(absolute(folder), path)) {
			const auto output = slot.original.empty() ? names.front() : slot.original.substr(slot.original.rfind('/') + 1);
			if (fl_choice("Recover a copy to:\n%s", "Cancel", "&Recover Copy", nullptr,
				      files::display(files::join(path, output)).c_str()) == 1)
				start_job({pane().path, std::move(path), std::move(names), files::CopyAction::recover});
		}
	}
	void start_job(files::CopyRequest request)
	{
		if (worker_.active())
			return;
		copy_ = std::move(request);
		const int error = worker_.start(copy_);
		if (error) {
			message(std::string("Cannot start ") + action_label(copy_.action) + ": " + std::strerror(error));
			return;
		}
		copy_results_.clear();
		copy_cancelling_ = false;
		status_.hide();
		job_status_.show();
		job_status_.copy_label(action_label(copy_.action));
		cancel_.activate();
		cancel_.show();
		Fl::add_fd(worker_.fd(), copy_ready, this);
	}
	void decide(files::CopyDecision decision)
	{
		skip_.hide();
		keep_.hide();
		if (worker_.decide(decision))
			job_status_.copy_label(action_label(copy_.action));
		else
			cancel_copy();
	}
	void skip_conflict() { decide(files::CopyDecision::skip); }
	void keep_conflict() { decide(files::CopyDecision::keep_both); }
	void cancel_copy()
	{
		copy_cancelling_ = true;
		worker_.cancel();
		skip_.hide();
		keep_.hide();
		cancel_.deactivate();
		job_status_.copy_label("Cancellation requested; finishing safe cleanup...");
	}
	static void copy_ready(int fd, void *data)
	{
		auto &browser = *static_cast<Browser *>(data);
		for (unsigned budget = 0; budget < 32; ++budget) {
			files::CopyEvent event{};
			const auto state = browser.worker_.next(event);
			if (state == files::CopyWorker::Read::pending)
				break;
			if (state == files::CopyWorker::Read::closed) {
				Fl::remove_fd(fd);
				browser.worker_.close_events();
				break;
			}
			if (event.type == files::CopyEventType::result) {
				if (event.item == browser.copy_results_.size() && event.item < browser.copy_.names.size())
					browser.copy_results_.push_back(event.result);
				continue;
			}
			if (browser.copy_cancelling_)
				continue;
			if (event.type == files::CopyEventType::conflict) {
				const auto text = "Already exists: " + files::display(event.name.data());
				browser.job_status_.copy_label(text.c_str());
				browser.skip_.show();
				browser.keep_.show();
			} else {
				const auto text = std::string(action_label(browser.copy_.action)) + ": item " + std::to_string(event.item + 1) + " of " +
					std::to_string(browser.copy_.names.size()) + " | " + std::to_string(event.bytes) + " bytes written";
				browser.job_status_.copy_label(text.c_str());
			}
		}
	}
	void finish_copy(bool success)
	{
		constexpr const char *labels[] = {"completed", "skipped", "failed", "cancelled", "unsupported"};
		std::array<unsigned, std::size(labels)> counts{};
		std::string details;
		unsigned issues = 0, partial = 0, retained = 0;
		std::vector<std::string> pending_cut;
		for (std::size_t index = 0; index < std::min(copy_results_.size(), copy_.names.size()); ++index) {
			const auto result = copy_results_[index];
			const auto state = static_cast<std::size_t>(result.state);
			++counts[state];
			partial += result.partial;
			retained += result.source_remaining > 0;
			if (copy_.action == files::CopyAction::move && result.state != files::CopyState::copied)
				pending_cut.push_back(copy_.names[index]);
			if (result.state != files::CopyState::copied && issues++ < 6) {
				details += "\n" + files::display(copy_.names[index]) + ": " + labels[state];
				if (result.error)
					details += std::string(" — ") + std::strerror(result.error);
				if (result.destination_complete)
					details += " (completed destination kept)";
				if (result.source_remaining)
					details += "\n  Stopped at " + files::display(result.first_remaining.data()) + "; " +
						std::to_string(result.source_remaining) + " recorded source entries not removed";
				if (result.partial)
					details += " (check for partial output or an incomplete Trash slot)";
			}
		}
		if (copy_.action == files::CopyAction::move) {
			pending_cut.insert(pending_cut.end(), copy_.names.begin() + copy_results_.size(), copy_.names.end());
			clipboard_.names = std::move(pending_cut);
		}
		std::string summary = std::string(action_label(copy_.action)) + ": " + std::to_string(counts[0]) +
			(copy_.action == files::CopyAction::copy ? " copied" : " completed");
		for (std::size_t state = 1; state < counts.size(); ++state)
			if (counts[state])
				summary += ", " + std::to_string(counts[state]) + " " + labels[state];
		if (!success || copy_results_.size() != copy_.names.size())
			summary += " | worker stopped unexpectedly; check source and destination";
		else if (retained)
			summary += " | source entries remain";
		else if (partial)
			summary += " | partial output may remain";
		last_copy_ = summary + details;
		if (issues > 6)
			last_copy_ += "\nAnd " + std::to_string(issues - 6) + " more.";
		if (copy_.action != files::CopyAction::mkdir && copy_.action != files::CopyAction::remove)
			last_copy_ += "\n\nCopy-based operations preserve file data and supported symlinks, not permissions, timestamps, or hard links.";
		job_status_.hide();
		skip_.hide();
		keep_.hide();
		cancel_.hide();
		status_.show();
		cache_.clear();
		for (int index = 0; index < tabs_.children(); ++index)
			for (auto *target : static_cast<Tab *>(tabs_.child(index))->panes)
				if (copy_.action != files::CopyAction::copy || target->path == copy_.destination ||
				    (target->path.size() > copy_.destination.size() &&
				    target->path.compare(0, copy_.destination.size(), copy_.destination) == 0 &&
				    (copy_.destination == "/" || target->path[copy_.destination.size()] == '/')))
					queue_scan(*target);
		schedule_preview();
		message(summary);
		showing_copy_summary_ = true;
		if (close_after_copy_)
			window_.hide();
	}
	void copy_summary()
	{
		fl_message("%s", last_copy_.empty() ? "No file job has finished yet." : last_copy_.c_str());
	}

	void fill_sidebar()
	{
		sidebar_.clear();
		places_.clear();
		const auto add_place = [&](Fl_Tree_Item *group, const char *label, const std::string &path, files::Icon id) {
			if (auto *item = sidebar_.add(group, label)) {
				item->usericon(files::icon(id));
				places_.emplace_back(item, path);
			}
		};
		auto *bookmarks = sidebar_.add("Bookmarks");
		add_place(bookmarks, "Home", home_, files::Icon::home);
		for (const auto &bookmark : preferences_.bookmarks)
			add_place(bookmarks, files::display(bookmark).c_str(), bookmark, files::Icon::bookmark);
		auto *volumes = sidebar_.add("Volumes");
		for (const auto &mount : files::mounts())
			add_place(volumes, files::display(mount).c_str(), mount, files::Icon::drive);
		sidebar_.redraw();
	}
	void bookmark()
	{
		auto &paths = preferences_.bookmarks;
		const auto item = std::find(paths.begin(), paths.end(), pane().path);
		if (item != paths.end())
			paths.erase(item);
		else if (paths.size() < 32)
			paths.push_back(pane().path);
		else {
			message("At most 32 bookmarks can be saved");
			return;
		}
		fill_sidebar();
		save_preferences();
	}
	static void sidebar_clicked(Fl_Widget *, void *data)
	{
		auto &browser = *static_cast<Browser *>(data);
		auto &tree = browser.sidebar_;
		if (tree.callback_reason() != FL_TREE_REASON_SELECTED && tree.callback_reason() != FL_TREE_REASON_RESELECTED)
			return;
		const auto item = std::find_if(browser.places_.begin(), browser.places_.end(), [&](const auto &place) {
			return place.first == tree.callback_item();
		});
		if (item != browser.places_.end())
			browser.navigate(item->second);
	}

	static void filter_changed(Fl_Widget *widget, void *data)
	{
		auto &browser = *static_cast<Browser *>(data);
		auto &tab = browser.tab();
		tab.active = widget == &tab.panes[1]->filter ? 1 : 0;
		browser.rebuild(browser.pane());
		browser.update_active();
	}
	static void pane_clicked(Fl_Widget *widget, void *data)
	{
		auto &browser = *static_cast<Browser *>(data);
		auto &tab = browser.tab();
		const int active = widget == tab.panes[1] ? 1 : 0;
		if (tab.active != active) {
			tab.active = active;
			browser.update_active();
		}
	}
	static void listing_clicked(Fl_Widget *widget, void *data)
	{
		auto &browser = *static_cast<Browser *>(data);
		auto &tab = browser.tab();
		const int active = widget == &tab.panes[1]->listing ? 1 : 0;
		const bool switched = active != tab.active;
		tab.active = active;
		auto &target = browser.pane();
		auto &listing = target.listing;
		const auto context = listing.callback_context();
		if (switched)
			browser.update_active();
		if (context == Fl_Table::CONTEXT_COL_HEADER) {
			const auto column = static_cast<files::Column>(listing.callback_col());
			browser.preferences_.descending = column == browser.preferences_.column && !browser.preferences_.descending;
			browser.preferences_.column = column;
			browser.rebuild_all();
		} else if (context == Fl_Table::CONTEXT_CELL) {
			const auto *item = listing.entry(listing.callback_row());
			target.current_name = item ? item->name : "";
			if (item && Fl::event_clicks() && Fl::event_button() == FL_LEFT_MOUSE) {
				Fl::event_clicks(0);
				browser.open_selected();
			} else {
				browser.update_status();
				browser.schedule_preview();
			}
		}
	}

	std::string selected_path()
	{
		auto &target = pane();
		const auto names = target.listing.selection();
		const auto current = std::find(names.begin(), names.end(), target.current_name);
		if (current != names.end())
			return files::join(target.path, *current);
		return names.empty() ? "" : files::join(target.path, names.front());
	}
	void properties()
	{
		const auto path = selected_path();
		if (path.empty())
			return;
		const auto item = files::inspect(files::parent(path), path.substr(path.rfind('/') + 1));
		if (item.error)
			fl_message("%s\n%s", files::display(path).c_str(), std::strerror(item.error));
		else
			fl_message("%s\n%s | %lld bytes\nModified (UTC): %s\nPermissions: %04o", files::display(path).c_str(),
				files::kind(item), static_cast<long long>(item.info.st_size), modified(item).c_str(),
				unsigned(item.info.st_mode & 07777));
	}
	void open_selected()
	{
		const auto path = selected_path();
		if (path.empty())
			return;
		const auto item = files::inspect(files::parent(path), path.substr(path.rfind('/') + 1));
		if (item.error)
			message(std::strerror(item.error));
		else if (S_ISDIR(item.info.st_mode) || (S_ISLNK(item.info.st_mode) && !item.broken))
			navigate(path);
		else if (S_ISREG(item.info.st_mode) && files::image_name(path))
			launch_viewer(path);
		else
			message(item.broken ? "This link is broken" : "No external viewer for this file");
	}
	void launch_viewer(const std::string &path)
	{
		if (viewers_.size() >= 8) {
			message("Close an Image Viewer window first");
			return;
		}
		char program[] = "/usr/bin/imageviewer";
		char *argv[] = {program, const_cast<char *>(path.c_str()), nullptr};
		const auto pid = fork();
		if (!pid) {
			execv(program, argv);
			_exit(127);
		}
		if (pid < 0)
			message(std::string("Could not launch Image Viewer: ") + std::strerror(errno));
		else
			viewers_.push_back(pid);
	}

	void schedule_preview()
	{
		Fl::remove_timeout(show_preview, this);
		preview_image_.image(nullptr);
		image_.reset();
		preview_image_.hide();
		preview_text_.value("Select a file");
		if (!selected_path().empty())
			Fl::add_timeout(0.08, show_preview, this);
	}
	static void show_preview(void *data)
	{
		auto &browser = *static_cast<Browser *>(data);
		const auto selected = browser.selected_path();
		const auto &directory = browser.pane().path;
		if (directory.substr(directory.rfind('/') + 1) == ".mario-trash") {
			files::TrashItem slot;
			if (!files::trash_item(directory, selected.substr(selected.rfind('/') + 1), slot)) {
				std::string text = slot.ready ? "Ready to restore" : slot.has_payload ?
					"Incomplete: recover a copy" : "Incomplete: no saved item";
				if (!slot.original.empty())
					text += "\n\nOriginal path:\n" + files::display(slot.original);
				browser.preview_text_.value(text.c_str());
				return;
			}
		}
		browser.preview_ = browser.cache_.read(selected);
		browser.preview_text_.value(browser.preview_.text.c_str());
		if (!browser.preview_.rgb.empty()) {
			browser.image_ = std::make_unique<Fl_RGB_Image>(browser.preview_.rgb.data(),
				browser.preview_.width, browser.preview_.height, 3);
			browser.preview_image_.image(browser.image_.get());
			browser.preview_image_.show();
			browser.preview_image_.redraw();
		}
	}
	static void tick(void *data)
	{
		auto &browser = *static_cast<Browser *>(data);
		if (browser.tabs_.w() != browser.last_width_ || browser.tabs_.h() != browser.last_height_) {
			browser.last_width_ = browser.tabs_.w();
			browser.last_height_ = browser.tabs_.h();
			browser.layout_tabs();
		}
		bool success;
		if (browser.worker_.reap(success)) {
			browser.finish_copy(success);
			if (!browser.window_.shown())
				return;
		}
		for (auto child = browser.viewers_.begin(); child != browser.viewers_.end();) {
			int status;
			const auto result = waitpid(*child, &status, WNOHANG);
			if (result > 0 || (result < 0 && errno == ECHILD)) {
				if (result > 0 && (!WIFEXITED(status) || WEXITSTATUS(status)))
					browser.message(WIFEXITED(status) && WEXITSTATUS(status) == 127 ?
						"Could not execute /usr/bin/imageviewer" : "Image Viewer exited with an error");
				child = browser.viewers_.erase(child);
			} else
				++child;
		}
		bool scanning = false;
		for (auto *target : browser.tab().panes) {
			if (!target->visible())
				continue;
			if (target->queued) {
				target->queued = false;
				target->scan.start(target->path);
				if (!target->scan.pending() && target == &browser.pane() && !browser.showing_copy_summary_)
					browser.update_status();
			}
			if (target->scan.pending()) {
				target->scan.step();
				scanning |= target->scan.pending();
				if (!target->scan.pending())
					browser.rebuild(*target);
				if (target == &browser.pane() && !browser.showing_copy_summary_)
					browser.update_status();
			}
		}
		Fl::repeat_timeout(scanning ? 0.02 : 0.10, tick, data);
	}
	void close()
	{
		if (!worker_.active()) {
			window_.hide();
			return;
		}
		if (close_after_copy_)
			return;
		const int choice = fl_choice("A file job is still in progress.", "Keep Open", "Wait and Close", "&Cancel and Close");
		if (!choice)
			return;
		if (!worker_.active()) {
			window_.hide();
			return;
		}
		close_after_copy_ = true;
		if (choice == 2)
			cancel_copy();
	}

	Fl_Double_Window window_;
	Fl_Menu_Bar menu_;
	Fl_Button back_, forward_, up_, home_button_;
	Fl_Input location_;
	Fl_Button go_, split_;
	Fl_Tree sidebar_;
	TabStrip tabs_;
	Fl_Multiline_Output preview_text_;
	Fl_Box preview_image_, status_, job_status_;
	Fl_Button skip_, keep_, cancel_;
	std::string home_;
	files::Preferences preferences_;
	files::PreviewCache cache_;
	files::Preview preview_;
	std::unique_ptr<Fl_RGB_Image> image_;
	std::vector<std::pair<Fl_Tree_Item *, std::string>> places_;
	std::vector<pid_t> viewers_;
	files::CopyRequest clipboard_, copy_;
	files::CopyWorker worker_;
	std::vector<files::CopyResult> copy_results_;
	std::string last_copy_;
	bool copy_cancelling_ = false, close_after_copy_ = false, showing_copy_summary_ = false;
	int last_width_ = 0, last_height_ = 0;
};

}

int main(int argc, char **argv)
{
	if (argc > 2) {
		std::fprintf(stderr, "usage: filemanager [directory]\n");
		return 2;
	}
	std::signal(SIGPIPE, SIG_IGN);
	Browser browser(argc == 2 ? argv[1] : nullptr);
	return Fl::run();
}
