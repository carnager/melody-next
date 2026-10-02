// SPDX-License-Identifier: GPL-3.0-only
#include "bench/themed_icon.hpp"

#include <QApplication>
#include <QMetaEnum>
#include <QStyle>

namespace trackknife::bench {

QIcon themedIcon(const QStringView spec) {
    for (const auto name : spec.split(QLatin1Char('|'))) {
        QIcon icon;
        if (name.startsWith(QStringLiteral("sp:"))) {
            const auto meta = QMetaEnum::fromType<QStyle::StandardPixmap>();
            bool known = false;
            const auto value = meta.keyToValue(name.mid(3).toLatin1().constData(), &known);
            if (known) {
                icon =
                    QApplication::style()->standardIcon(static_cast<QStyle::StandardPixmap>(value));
            }
        } else {
            icon = QIcon::fromTheme(name.toString());
        }
        if (!icon.isNull()) {
            return icon;
        }
    }
    return {};
}

} // namespace trackknife::bench
