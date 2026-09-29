// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/workspace.hpp"

namespace trackknife::bench {

Workspace::Workspace(QObject* parent) : QObject(parent) {}

Workspace::~Workspace() = default;

} // namespace trackknife::bench
