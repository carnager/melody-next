// SPDX-License-Identifier: GPL-3.0-only

#include "uicommon/metadata_transformation_interchange.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <string>

namespace trackknife::ui {

class MetadataTransformationInterchangeTest final : public QObject {
    Q_OBJECT

  private slots:
    void roundTripsEveryTypedActionExactly();
    void rejectsUnknownMalformedAndOversizedInput();
};

void MetadataTransformationInterchangeTest::roundTripsEveryTypedActionExactly() {
    using namespace metadata;
    const MetadataTransformationChain chain{
        .schema_version = 1U,
        .name = "Portable cleanup — δ",
        .actions =
            {
                MetadataSetValuesAction{.target_field = "GENRE", .values = {"Rock", ""}},
                MetadataAddValuesAction{.target_field = "GENRE", .values = {"Alt", "Rock"}},
                MetadataRemoveFieldAction{
                    .target_field = "ALBUM ARTIST",
                    .match_mode = MetadataFieldMatchMode::exact_native,
                },
                MetadataRemoveFieldIfAction{
                    .target_field = "COMMENT",
                    .dialect = {},
                    .condition = "$eq(%comment%,temporary)",
                    .match_mode = MetadataFieldMatchMode::logical,
                },
                MetadataTransformValuesAction{
                    .target_field = "TITLE",
                    .transform = MetadataValueTransformKind::trim_ascii,
                },
                MetadataTransformValuesAction{
                    .target_field = "ARTIST",
                    .transform = MetadataValueTransformKind::lowercase,
                },
                MetadataTransformValuesAction{
                    .target_field = "ALBUM",
                    .transform = MetadataValueTransformKind::uppercase,
                },
                MetadataTransformValuesAction{
                    .target_field = "COMPOSER",
                    .transform = MetadataValueTransformKind::capitalize_first,
                },
                MetadataFormatValueAction{
                    .target_field = "SUMMARY",
                    .dialect = {},
                    .source = "%artist% — %title%",
                },
                MetadataCopyFieldAction{.target_field = "ALBUMARTIST", .source_field = "ARTIST"},
                MetadataSplitValuesAction{.target_field = "GENRE", .separator = ";"},
                MetadataJoinValuesAction{.target_field = "ARTIST", .separator = " / "},
                MetadataRemoveMatchingValuesAction{.target_field = "GENRE", .match = "Other"},
                MetadataReplaceMatchingValuesAction{
                    .target_field = "MOOD",
                    .match = "Calm",
                    .replacement_values = {"Quiet", ""},
                },
                MetadataNumberSelectedItemsAction{
                    .target_field = "TRACKNUMBER", .start = 7U, .padding = 2U},
                MetadataNumberGroupedItemsAction{.target_field = "TRACKNUMBER",
                                                 .dialect = {},
                                                 .group_expression = "%album%",
                                                 .start = 1U,
                                                 .padding = 2U},
                MetadataKeepFirstCharactersAction{.target_field = "DATE", .character_count = 4U},
                MetadataCaptureValuesAction{
                    .dialect = {},
                    .source_kind = MetadataCaptureSourceKind::filename,
                    .source = {},
                    .pattern = "%artist% - %title%",
                },
                MetadataCaptureValuesAction{
                    .dialect = {},
                    .source_kind = MetadataCaptureSourceKind::full_path,
                    .source = {},
                    .pattern = "%artist%/%album%/%title%",
                },
                MetadataCaptureValuesAction{
                    .dialect = {},
                    .source_kind = MetadataCaptureSourceKind::formatted,
                    .source = "%artist% - %title%",
                    .pattern = "%artist% - %title%",
                },
                MetadataCaptureValuesAction{
                    .dialect = {},
                    .source_kind = MetadataCaptureSourceKind::field,
                    .source = "COMMENT",
                    .pattern = "%artist% - %title%",
                },
                MetadataBlocklistFieldsAction{.fields = {"COMMENT", "ENCODER"}},
                MetadataAllowlistFieldsAction{.fields = {"TITLE", "ARTIST", "ALBUM"}},
            },
    };

    const auto serialized = serializeMetadataTransformationChain(chain);
    QVERIFY2(serialized.has_value(), serialized ? "" : serialized.error().message.c_str());
    QVERIFY(serialized->contains("trackknife-metadata-transformation-chain"));
    // Exports written under the interim application name import unchanged.
    auto legacy = *serialized;
    legacy.replace("trackknife-metadata-transformation-chain",
                   "trackbench-metadata-transformation-chain");
    QVERIFY(deserializeMetadataTransformationChain(legacy).has_value());
    QVERIFY(!serialized->contains("automatic"));
    QVERIFY(!serialized->contains("stable_id"));

    const auto restored = deserializeMetadataTransformationChain(*serialized);
    QVERIFY2(restored.has_value(), restored ? "" : restored.error().message.c_str());
    QCOMPARE(*restored, chain);

    const auto serialized_again = serializeMetadataTransformationChain(*restored);
    QVERIFY(serialized_again.has_value());
    QCOMPARE(*serialized_again, *serialized);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("portable.tbtags.json"));
    const auto saved = saveMetadataTransformationChainFile(path, chain);
    QVERIFY2(saved.has_value(), saved ? "" : saved.error().message.c_str());
    QFile file{path};
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), *serialized);
    const auto loaded = loadMetadataTransformationChainFile(path);
    QVERIFY2(loaded.has_value(), loaded ? "" : loaded.error().message.c_str());
    QCOMPARE(*loaded, chain);
}

void MetadataTransformationInterchangeTest::rejectsUnknownMalformedAndOversizedInput() {
    using namespace metadata;
    const MetadataTransformationChain chain{
        .schema_version = 1U,
        .name = "Test",
        .actions = {MetadataNumberSelectedItemsAction{
            .target_field = "TRACKNUMBER", .start = 1U, .padding = 2U}},
    };
    const auto serialized = serializeMetadataTransformationChain(chain);
    QVERIFY(serialized.has_value());

    QVERIFY(!deserializeMetadataTransformationChain({}).has_value());
    QVERIFY(!deserializeMetadataTransformationChain(QByteArray{"\xff", 1}).has_value());
    QVERIFY(!deserializeMetadataTransformationChain(
                 QByteArray(metadata_transformation_interchange_maximum_bytes + 1, 'x'))
                 .has_value());

    const auto changed = [&serialized](const auto& mutate) {
        auto document = QJsonDocument::fromJson(*serialized);
        auto root = document.object();
        mutate(root);
        return QJsonDocument{root}.toJson(QJsonDocument::Compact);
    };

    const auto future_envelope =
        changed([](QJsonObject& root) { root.insert(QStringLiteral("format_version"), 2); });
    const auto future_result = deserializeMetadataTransformationChain(future_envelope);
    QVERIFY(!future_result.has_value());
    QCOMPARE(future_result.error().code, core::ErrorCode::unsupported);

    const auto extra_root_key =
        changed([](QJsonObject& root) { root.insert(QStringLiteral("future"), true); });
    QVERIFY(!deserializeMetadataTransformationChain(extra_root_key).has_value());

    const auto unknown_action = changed([](QJsonObject& root) {
        auto chain_object = root.value(QStringLiteral("chain")).toObject();
        auto actions = chain_object.value(QStringLiteral("actions")).toArray();
        auto action = actions.at(0).toObject();
        action.insert(QStringLiteral("kind"), QStringLiteral("future_action"));
        actions.replace(0, action);
        chain_object.insert(QStringLiteral("actions"), actions);
        root.insert(QStringLiteral("chain"), chain_object);
    });
    const auto unknown_result = deserializeMetadataTransformationChain(unknown_action);
    QVERIFY(!unknown_result.has_value());
    QCOMPARE(unknown_result.error().code, core::ErrorCode::unsupported);

    const auto fractional_number = changed([](QJsonObject& root) {
        auto chain_object = root.value(QStringLiteral("chain")).toObject();
        auto actions = chain_object.value(QStringLiteral("actions")).toArray();
        auto action = actions.at(0).toObject();
        action.insert(QStringLiteral("start"), 1.5);
        actions.replace(0, action);
        chain_object.insert(QStringLiteral("actions"), actions);
        root.insert(QStringLiteral("chain"), chain_object);
    });
    QVERIFY(!deserializeMetadataTransformationChain(fractional_number).has_value());

    const auto semantically_invalid = changed([](QJsonObject& root) {
        auto chain_object = root.value(QStringLiteral("chain")).toObject();
        auto actions = chain_object.value(QStringLiteral("actions")).toArray();
        actions.replace(0, QJsonObject{{QStringLiteral("kind"), QStringLiteral("split_values")},
                                       {QStringLiteral("separator"), QString{}},
                                       {QStringLiteral("target_field"), QStringLiteral("GENRE")}});
        chain_object.insert(QStringLiteral("actions"), actions);
        root.insert(QStringLiteral("chain"), chain_object);
    });
    QVERIFY(!deserializeMetadataTransformationChain(semantically_invalid).has_value());

    auto oversized_export = chain;
    oversized_export.actions.clear();
    for (auto index = 0; index < 9; ++index) {
        oversized_export.actions.push_back(MetadataSetValuesAction{
            .target_field = "COMMENT",
            .values = {std::string(1U * 1'024U * 1'024U, static_cast<char>('a' + index))},
        });
    }
    const auto oversized_export_result = serializeMetadataTransformationChain(oversized_export);
    QVERIFY(!oversized_export_result.has_value());
    QCOMPARE(oversized_export_result.error().code, core::ErrorCode::limit_exceeded);
}

} // namespace trackknife::ui

QTEST_APPLESS_MAIN(trackknife::ui::MetadataTransformationInterchangeTest)

#include "metadata_transformation_interchange_test.moc"
