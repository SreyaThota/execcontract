// Collection: obtain typed observations of this process's own execution
// context, or explicit errors. Nothing here modifies process state.
#pragma once

#include "model.hpp"

#include <dirent.h>
#include <sys/types.h>

#include <cstddef>
#include <string_view>

namespace execcontract {

// Enumerate the descriptor directory `dir_path` (production: "/proc/self/fd")
// and return the numbers found, excluding the enumeration stream's own
// descriptor as reported by dirfd(3). At most `cap` descriptors are recorded;
// one more makes the inventory incomplete with code "inventory_capped".
// Types are not filled in; see classify_fds().
//
// `next_entry` is a test seam for the one failure that cannot be produced
// unprivileged on procfs: readdir() failing mid-enumeration. Production code
// always uses the default.
using ReaddirFn = dirent* (*)(DIR*);
FdInventory scan_fd_dir(const char* dir_path, std::size_t cap, ReaddirFn next_entry = ::readdir);

// Fill FdEntry::type / type_error with fstat(2) on each descriptor number.
void classify_fds(FdInventory& inventory);

// Map an st_mode to a coarse descriptor type.
FdType classify_mode(mode_t mode) noexcept;

// Read the "Umask:" field of a /proc/<pid>/status-format file.
Observed<unsigned> read_umask(const char* status_path);
// Pure parser for the content of such a file.
Observed<unsigned> parse_umask_status(std::string_view text);

// getrlimit(2) for `resource`.
Observed<Limits> observe_rlimit(int resource);

// Physical working directory via getcwd(3), grown up to `cap` bytes.
Observed<std::string> observe_cwd(std::size_t cap);

// The full snapshot in the documented order: descriptor inventory first (the
// process has opened nothing of its own yet), then umask, limits, cwd.
Snapshot collect_snapshot();

}  // namespace execcontract
