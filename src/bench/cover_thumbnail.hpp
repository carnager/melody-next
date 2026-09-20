// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QImage>
#include <QLabel>
namespace trackknife::bench {
class CoverThumbnail final : public QLabel {
    Q_OBJECT
  public:
    explicit CoverThumbnail(QWidget* parent = nullptr);
    void setCover(const QImage& image, bool mixed);
  signals:
    void fileDropped(const QString& path);
    void imagePasted(const QImage& image);

  protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
};
} // namespace trackknife::bench
