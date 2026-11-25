#ifndef MARIO_FILEMANAGER_COPY_H
#define MARIO_FILEMANAGER_COPY_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace files {

constexpr std::size_t copy_name_limit = 255;
constexpr std::size_t copy_selection_limit = 4096;
constexpr std::size_t copy_chunk = 32 * 1024;

enum class CopyAction : uint8_t { copy, move, rename, mkdir, trash, restore, recover, remove };

struct CopyRequest {
	std::string source, destination;
	std::vector<std::string> names;
	CopyAction action = CopyAction::copy;
};

enum class CopyState : uint8_t { copied, skipped, failed, cancelled, unsupported };
enum class CopyDecision : uint8_t { skip, keep_both, cancel };

struct CopyResult {
	CopyState state = CopyState::failed;
	int error = 0;
	bool partial = false;
	bool destination_complete = false;
	std::size_t source_remaining = 0;
	std::array<char, copy_name_limit + 1> first_remaining{};
};

struct TrashItem {
	std::string original;
	bool ready = false, has_payload = false;
};

struct CopyObserver {
	virtual ~CopyObserver() = default;
	virtual bool cancelled() = 0;
	virtual CopyDecision conflict(std::size_t item, std::string_view name) = 0;
	virtual void progress(std::size_t item, uint64_t bytes) = 0;
	virtual void result(std::size_t item, CopyResult result) = 0;
};

void copy_items(const CopyRequest &request, CopyObserver &observer);
int trash_directory(const std::string &path, std::string &directory);
int trash_item(const std::string &directory, const std::string &name, TrashItem &item);

}

#endif
