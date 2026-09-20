// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "trackknife/metadata/artwork_write_plan.hpp"
#include <functional>
class QWidget;
namespace trackknife::bench {
void reviewFolderImages(QWidget* parent, const std::vector<metadata::FolderImageWritePlan>& images,
                        std::function<void()> apply);
}
