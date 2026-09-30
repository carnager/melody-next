// SPDX-License-Identifier: GPL-3.0-only
#include "workspace/language_reference.hpp"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTextDocument>
#include <QUrl>

#include <array>

namespace trackknife::bench {
namespace {

struct Page {
    LanguageReference reference;
    const char* resource;
    const char* document;
    const char* page;
    const char* title;
};

constexpr std::array pages{
    Page{LanguageReference::formatting, ":/reference/title-formatting.md", "title-formatting.md",
         "tkfmt-1.html", "tkfmt-1 reference"},
    Page{LanguageReference::scripts, ":/reference/tagging-scripts.md", "tagging-scripts.md",
         "tagging-scripts.html", "Tagging scripts reference"},
};

// Readable in the browser's own light or dark, whatever Qt wrote inline.
constexpr auto style = R"(
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
  :root { color-scheme: light dark; }
  body { max-width: 52rem; margin: 2rem auto; padding: 0 1rem; line-height: 1.5;
         font-family: system-ui, sans-serif !important; }
  body * { font-family: inherit; }
  pre, code { font-family: ui-monospace, monospace !important; }
  pre { padding: .75rem 1rem !important; margin: .75rem 0 !important; border-radius: 6px;
        overflow-x: auto;
        background: color-mix(in srgb, currentColor 7%, transparent); }
  table { border-collapse: collapse; margin: 1rem 0; }
  th, td { border: 1px solid color-mix(in srgb, currentColor 20%, transparent);
           padding: .35rem .6rem; vertical-align: top; text-align: left; }
  a { color: #3daee9; }
</style>
)";

// The document's Markdown with its links made to work outside the source
// tree: to the other reference, its page; anything else -- ADRs, other
// documents -- just its text.
[[nodiscard]] QString linked(QString markdown) {
    static const QRegularExpression link{QStringLiteral(R"(\[([^\]]+)\]\(([^)\s]+)\))")};
    QString result;
    qsizetype last = 0;
    auto matches = link.globalMatch(markdown);
    while (matches.hasNext()) {
        const auto match = matches.next();
        result += markdown.mid(last, match.capturedStart() - last);
        const auto text = match.captured(1);
        auto target = match.captured(2);
        const auto anchor = target.indexOf(QLatin1Char('#'));
        const auto file = anchor >= 0 ? target.left(anchor) : target;
        const Page* other = nullptr;
        for (const auto& page : pages) {
            if (file.endsWith(QLatin1String(page.document))) {
                other = &page;
            }
        }
        if (file.isEmpty()) {
            result += match.captured(0);
        } else if (other != nullptr) {
            result += QStringLiteral("[%1](%2%3)")
                          .arg(text, QLatin1String(other->page),
                               anchor >= 0 ? target.mid(anchor) : QString{});
        } else {
            result += text;
        }
        last = match.capturedEnd();
    }
    result += markdown.mid(last);
    return result;
}

[[nodiscard]] bool writePage(const Page& page, const QDir& directory, QString* error) {
    QFile source{QLatin1String(page.resource)};
    if (!source.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("The %1 is missing from this build").arg(page.title);
        }
        return false;
    }
    QTextDocument document;
    document.setMarkdown(linked(QString::fromUtf8(source.readAll())));
    auto html = document.toHtml();
    // Qt writes each line of a code block as a block of its own: one again.
    static const QRegularExpression split_code{QStringLiteral("</pre>\\s*<pre[^>]*>")};
    html.replace(split_code, QStringLiteral("\n"));
    html.replace(QStringLiteral("</head>"),
                 QStringLiteral("<title>%1</title>%2</head>")
                     .arg(QLatin1String(page.title), QLatin1String(style)));
    QSaveFile target{directory.filePath(QLatin1String(page.page))};
    if (!target.open(QIODevice::WriteOnly) || target.write(html.toUtf8()) < 0 ||
        !target.commit()) {
        if (error != nullptr) {
            *error = QStringLiteral("Could not write %1: %2")
                         .arg(target.fileName(), target.errorString());
        }
        return false;
    }
    return true;
}

} // namespace

QString writeLanguageReference(const LanguageReference reference, const QString& path,
                               QString* error) {
    const QDir directory{path};
    if (!QDir{}.mkpath(directory.path())) {
        if (error != nullptr) {
            *error = QStringLiteral("Could not create %1").arg(directory.path());
        }
        return {};
    }
    // Both, every time: each links to the other, and a new build's text
    // replaces the last one's.
    QString wanted;
    for (const auto& page : pages) {
        if (!writePage(page, directory, error)) {
            return {};
        }
        if (page.reference == reference) {
            wanted = directory.filePath(QLatin1String(page.page));
        }
    }
    return wanted;
}

bool openLanguageReference(const LanguageReference reference, QString* error) {
    const auto page = writeLanguageReference(
        reference,
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
            QStringLiteral("/reference"),
        error);
    if (page.isEmpty()) {
        return false;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(page))) {
        if (error != nullptr) {
            *error = QStringLiteral("No browser opened %1").arg(page);
        }
        return false;
    }
    return true;
}

} // namespace trackknife::bench
