// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QLabel>

class QPaintEvent;

namespace trackknife::ui {

// A one-line label that ends in "…" when its text does not fit, instead of
// being cut off mid-letter. It asks for no width of its own, so a long title
// never pushes the controls beside it aside.
class ElidingLabel final : public QLabel {
  public:
    explicit ElidingLabel(QWidget* parent = nullptr);

    [[nodiscard]] QSize minimumSizeHint() const override;
    [[nodiscard]] QSize sizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;
};

} // namespace trackknife::ui
