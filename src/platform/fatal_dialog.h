// platform/fatal_dialog.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

/**
 * @file    platform/fatal_dialog.h
 * @brief   Blocking modal dialogs usable before presentation setup.
 *
 *          After the reblue platform/fatal_dialog (BSD 3-Clause, Tom Clay).
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once

#include <string_view>

namespace rex::ui {
class Window;
}

namespace eot::platform {

void ShowFatalError(std::string_view title, std::string_view body);

void ShowWarning(std::string_view title, std::string_view body);

void ShowInfo(std::string_view title, std::string_view body);

bool ShowConfirm(std::string_view title, std::string_view body);

bool ShowQuestion(std::string_view title, std::string_view body);

bool ShowFatalErrorWithAction(std::string_view title, std::string_view body, std::string_view action,
                              rex::ui::Window *parent);

}
