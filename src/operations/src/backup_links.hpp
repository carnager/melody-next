// SPDX-License-Identifier: GPL-3.0-only

#pragma once

namespace trackknife::operations::detail {

// ADR-0248: whether link() failing with this errno means the filesystem has
// no hard links -- SMB and CIFS, FAT and exFAT, FUSE without link -- so the
// backup is made as a copy instead.
[[nodiscard]] bool hard_link_refused(int number) noexcept;

// Tests only: link() is not tried, and folder images publish by plain rename,
// as on such a filesystem (use_copied_metadata_backups_for_testing).
[[nodiscard]] bool filesystem_without_links_simulated() noexcept;

} // namespace trackknife::operations::detail
