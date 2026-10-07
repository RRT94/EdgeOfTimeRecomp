// installer/self_install.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

/**
 * @file    installer/self_install.h
 * @brief   Copying the program into the install folder.
 *
 *          After reblue's installer/self_install (BSD 3-Clause, Tom Clay).
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace eot::installer {

std::vector<std::string> MissingProgramFiles();

bool CopyProgramTo(const std::filesystem::path &install, std::string &error);

void WritePortFiles(const std::filesystem::path &game);

void AdoptLegacyUserData(const std::filesystem::path &profile);

}
