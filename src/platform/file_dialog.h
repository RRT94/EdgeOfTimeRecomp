// platform/file_dialog.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

/**
 * @file    platform/file_dialog.h
 * @brief   Native open-file and open-folder dialogs, through SDL3.
 *
 *          After reblue's platform/file_dialog (BSD 3-Clause, Tom Clay).
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once

#include <filesystem>
#include <optional>
#include <span>

namespace eot::platform {

struct FileFilter {
  const wchar_t *name;
  const wchar_t *pattern;
};

std::optional<std::filesystem::path> ShowOpenFileDialog(const wchar_t *title,
                                                        std::span<const FileFilter> filters = {});

std::optional<std::filesystem::path> ShowOpenFolderDialog(const wchar_t *title);

}
