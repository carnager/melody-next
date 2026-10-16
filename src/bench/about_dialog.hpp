// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QDialog>

namespace trackknife::bench {

// ADR-0287: Help › About Melody -- the mascot, the release and the commit it
// was built from, the licence and where to read more.
class AboutDialog final : public QDialog {
    Q_OBJECT

  public:
    explicit AboutDialog(QWidget* parent = nullptr);
};

} // namespace trackknife::bench
