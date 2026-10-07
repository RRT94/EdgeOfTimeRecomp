// platform/desktop_shortcut.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

/**
 * @file    platform/desktop_shortcut.h
 * @brief   A desktop shortcut to an executable (Windows).
 *
 *          After reblue's platform/desktop_shortcut (BSD 3-Clause, Tom Clay).
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace eot::platform {

bool CreateDesktopShortcut(const std::filesystem::path &target, std::string_view name,
                           std::string &error);

bool RemoveDesktopShortcut(std::string_view name);

}
