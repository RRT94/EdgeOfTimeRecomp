// installer/dlc_publish.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

/**
 * @file    installer/dlc_publish.h
 * @brief   The DLC packages an install keeps, published into a profile's
 *          content tree where the SDK finds them.
 *
 *          After reblue's vfs/dlc_publish (BSD 3-Clause, Tom Clay). The
 *          SDK's content manager knows content as folders under the user
 *          data root, <profile>/<xuid>/<title>/<type>/<name>/, with the
 *          package's XCONTENT_AGGREGATE_DATA and licence mask beside them
 *          in <profile>/<xuid>/<title>/Headers/<type>/<name>.header; that
 *          is what it lists for the game and mounts when the game opens
 *          the content. Marketplace content (DLC) lives under xuid 0. A
 *          package file is not that: copied whole into the game's Content
 *          tree it was listed by the disc-side enumerator and could not be
 *          opened. So the install keeps the packages in <root>/dlc and
 *          every boot publishes each into the profile that boots.
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once

#include <filesystem>

namespace eot::installer {

inline constexpr const char *kDlcFolderName = "dlc";

void PublishDlc(const std::filesystem::path &dlc_dir, const std::filesystem::path &game,
                const std::filesystem::path &profile);

}
