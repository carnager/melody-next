// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/metadata/in_place_writer.hpp"

#include "local_reader_detail.hpp"
#include "mp4_item_factory_detail.hpp"
#include "text_writer_detail.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/posix.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/metadata/ratings.hpp"
#include "writer_steps_detail.hpp"

#include <flacfile.h>
#include <mp4file.h>
#include <mpegfile.h>
#include <tiostream.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <map>
#include <string_view>
#include <system_error>
#include <utility>

namespace trackknife::metadata {
namespace {

[[nodiscard]] core::Error in_place_error(const core::ErrorCode code, std::string message,
                                         const std::string& raw_path) {
    return core::Error{
        .code = code,
        .message = std::move(message),
        .context = {{.key = "source", .value = core::escape_raw_path(raw_path)}},
    };
}

// The file as TagLib would see it after its save, without the file being
// written: reads come from the file, writes are kept here. A save that
// would change the file's length -- the tags do not fit -- is noted, and
// nothing of it counts.
class OverlayStream final : public TagLib::IOStream {
  public:
    OverlayStream(std::string raw_path, const int descriptor, const TagLib::offset_t length)
        : raw_path_(std::move(raw_path)), descriptor_(descriptor), length_(length) {}

    [[nodiscard]] TagLib::FileName name() const override { return raw_path_.c_str(); }

    TagLib::ByteVector readBlock(const size_t length) override {
        // Once the save would resize the file, nothing of it counts: reading
        // on would only show TagLib a file that never was.
        if (resized_ || position_ >= length_ || length == 0U) {
            return {};
        }
        const auto wanted = static_cast<std::size_t>(
            std::min<TagLib::offset_t>(static_cast<TagLib::offset_t>(length), length_ - position_));
        std::string bytes(wanted, '\0');
        std::size_t done = 0U;
        while (done < wanted) {
            const auto count = ::pread(descriptor_, bytes.data() + done, wanted - done,
                                       static_cast<off_t>(position_) + static_cast<off_t>(done));
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                failed_ = true;
                break;
            }
            done += static_cast<std::size_t>(count);
        }
        bytes.resize(done);
        overlay(position_, bytes);
        position_ += static_cast<TagLib::offset_t>(bytes.size());
        return TagLib::ByteVector{bytes.data(), static_cast<unsigned int>(bytes.size())};
    }

    void writeBlock(const TagLib::ByteVector& data) override {
        if (position_ + static_cast<TagLib::offset_t>(data.size()) > length_) {
            resized_ = true;
            return;
        }
        keep(position_, std::string{data.data(), data.size()});
        position_ += static_cast<TagLib::offset_t>(data.size());
    }

    void insert(const TagLib::ByteVector& data, const TagLib::offset_t start,
                const size_t replace) override {
        if (data.size() != replace) {
            resized_ = true;
            return;
        }
        if (start + static_cast<TagLib::offset_t>(data.size()) > length_) {
            resized_ = true;
            return;
        }
        keep(start, std::string{data.data(), data.size()});
    }

    void removeBlock(const TagLib::offset_t, const size_t length) override {
        if (length != 0U) {
            resized_ = true;
        }
    }

    [[nodiscard]] bool readOnly() const override { return false; }
    [[nodiscard]] bool isOpen() const override { return true; }

    void seek(const TagLib::offset_t offset, const Position position) override {
        switch (position) {
        case Beginning:
            position_ = offset;
            break;
        case Current:
            position_ += offset;
            break;
        case End:
            position_ = length_ + offset;
            break;
        }
    }

    [[nodiscard]] TagLib::offset_t tell() const override { return position_; }
    TagLib::offset_t length() override { return length_; }

    void truncate(const TagLib::offset_t length) override {
        if (length != length_) {
            resized_ = true;
        }
    }

    // Whether the save would have changed the file's length, or reading it
    // failed: then nothing of it is written in place.
    [[nodiscard]] bool usable() const noexcept { return !resized_ && !failed_; }
    [[nodiscard]] const std::map<TagLib::offset_t, std::string>& writes() const noexcept {
        return writes_;
    }

  private:
    // The kept writes laid over bytes read from the file.
    void overlay(const TagLib::offset_t offset, std::string& bytes) const {
        const auto end = offset + static_cast<TagLib::offset_t>(bytes.size());
        auto it = writes_.upper_bound(offset);
        if (it != writes_.begin()) {
            --it;
        }
        for (; it != writes_.end() && it->first < end; ++it) {
            const auto write_end = it->first + static_cast<TagLib::offset_t>(it->second.size());
            const auto from = std::max(offset, it->first);
            const auto to = std::min(end, write_end);
            if (from >= to) {
                continue;
            }
            std::memcpy(bytes.data() + (from - offset), it->second.data() + (from - it->first),
                        static_cast<std::size_t>(to - from));
        }
    }

    // Kept as one stretch with every write it touches or adjoins.
    void keep(TagLib::offset_t offset, std::string bytes) {
        auto end = offset + static_cast<TagLib::offset_t>(bytes.size());
        auto it = writes_.upper_bound(offset);
        if (it != writes_.begin()) {
            --it;
        }
        while (it != writes_.end() && it->first <= end) {
            const auto write_end = it->first + static_cast<TagLib::offset_t>(it->second.size());
            if (write_end < offset) {
                ++it;
                continue;
            }
            const auto merged_begin = std::min(offset, it->first);
            const auto merged_end = std::max(end, write_end);
            std::string merged(static_cast<std::size_t>(merged_end - merged_begin), '\0');
            std::memcpy(merged.data() + (it->first - merged_begin), it->second.data(),
                        it->second.size());
            std::memcpy(merged.data() + (offset - merged_begin), bytes.data(), bytes.size());
            offset = merged_begin;
            end = merged_end;
            bytes = std::move(merged);
            it = writes_.erase(it);
        }
        writes_.emplace(offset, std::move(bytes));
    }

    std::string raw_path_;
    int descriptor_;
    TagLib::offset_t length_;
    TagLib::offset_t position_{0};
    std::map<TagLib::offset_t, std::string> writes_;
    bool resized_{false};
    bool failed_{false};
};

[[nodiscard]] bool read_exactly(const int descriptor, const std::uint64_t offset,
                                std::string& bytes) {
    std::size_t done = 0U;
    while (done < bytes.size()) {
        const auto count = ::pread(descriptor, bytes.data() + done, bytes.size() - done,
                                   static_cast<off_t>(offset + done));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return false;
        }
        done += static_cast<std::size_t>(count);
    }
    return true;
}

[[nodiscard]] std::uint32_t big_endian(const std::string_view bytes, const std::size_t at) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at])) << 24U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 1U])) << 16U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 2U])) << 8U) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 3U]));
}

struct Span {
    std::uint64_t begin{0U};
    std::uint64_t end{0U};
};

// FLAC metadata blocks from the front of a file image: each block's type
// and bytes, and where the audio begins.
struct FlacHeader {
    std::vector<std::pair<std::uint8_t, std::string>> blocks;
    std::uint64_t audio_offset{0U};
};

[[nodiscard]] std::optional<FlacHeader>
parse_flac_header(const int descriptor, const OverlayStream* overlay, const std::uint64_t length) {
    // Read through the overlay when one is given, else from the file.
    const auto read = [&](const std::uint64_t offset, const std::size_t size) {
        std::string bytes(size, '\0');
        if (offset + size > length || !read_exactly(descriptor, offset, bytes)) {
            return std::optional<std::string>{};
        }
        if (overlay != nullptr) {
            for (const auto& [at, written] : overlay->writes()) {
                const auto from = std::max<std::uint64_t>(offset, static_cast<std::uint64_t>(at));
                const auto to = std::min<std::uint64_t>(
                    offset + size, static_cast<std::uint64_t>(at) + written.size());
                if (from < to) {
                    std::memcpy(bytes.data() + (from - offset),
                                written.data() + (from - static_cast<std::uint64_t>(at)),
                                static_cast<std::size_t>(to - from));
                }
            }
        }
        return std::optional{std::move(bytes)};
    };
    const auto marker = read(0U, 4U);
    if (!marker || *marker != "fLaC") {
        return std::nullopt;
    }
    FlacHeader header;
    std::uint64_t offset = 4U;
    for (std::size_t count = 0U; count < 4'096U; ++count) {
        const auto block_header = read(offset, 4U);
        if (!block_header) {
            return std::nullopt;
        }
        const auto first = static_cast<unsigned char>((*block_header)[0]);
        const auto size = big_endian(*block_header, 0U) & 0xFFFFFFU;
        auto body = read(offset + 4U, size);
        if (!body) {
            return std::nullopt;
        }
        header.blocks.emplace_back(static_cast<std::uint8_t>(first & 0x7FU), std::move(*body));
        offset += 4U + size;
        if ((first & 0x80U) != 0U) {
            header.audio_offset = offset;
            return header;
        }
    }
    return std::nullopt;
}

// What a write in place must leave alone, in the file as it is: the audio,
// and in MP4 every top-level box but the metadata's.
[[nodiscard]] std::optional<std::vector<Span>>
protected_spans(const std::string& adapter, const int descriptor, const std::uint64_t length) {
    if (adapter == "taglib-flac-v1") {
        const auto header = parse_flac_header(descriptor, nullptr, length);
        if (!header) {
            return std::nullopt;
        }
        return std::vector{Span{.begin = header->audio_offset, .end = length}};
    }
    if (adapter == "taglib-mpeg-v1") {
        std::string id3(10U, '\0');
        if (!read_exactly(descriptor, 0U, id3) || !id3.starts_with("ID3")) {
            return std::nullopt;
        }
        const auto byte = [&id3](const std::size_t at) {
            return static_cast<std::uint64_t>(static_cast<unsigned char>(id3[at]) & 0x7FU);
        };
        auto tag = 10U + ((byte(6) << 21U) | (byte(7) << 14U) | (byte(8) << 7U) | byte(9));
        if ((static_cast<unsigned char>(id3[5]) & 0x10U) != 0U) {
            tag += 10U;
        }
        // The frames: after the leading tag, before the trailing ones --
        // ID3v1's last 128 bytes, and an APEv2 tag before them.
        auto audio_end = length;
        if (length >= tag + 128U) {
            std::string id3v1(3U, '\0');
            if (read_exactly(descriptor, length - 128U, id3v1) && id3v1 == "TAG") {
                audio_end -= 128U;
            }
        }
        if (audio_end >= tag + 32U) {
            std::string footer(32U, '\0');
            if (read_exactly(descriptor, audio_end - 32U, footer) &&
                footer.starts_with("APETAGEX")) {
                const auto little = [&footer](const std::size_t at) {
                    return static_cast<std::uint64_t>(static_cast<unsigned char>(footer[at])) |
                           (static_cast<std::uint64_t>(static_cast<unsigned char>(footer[at + 1U]))
                            << 8U) |
                           (static_cast<std::uint64_t>(static_cast<unsigned char>(footer[at + 2U]))
                            << 16U) |
                           (static_cast<std::uint64_t>(static_cast<unsigned char>(footer[at + 3U]))
                            << 24U);
                };
                // The size counts the items and the footer; a header adds 32.
                auto ape = little(12U);
                if ((static_cast<unsigned char>(footer[23]) & 0x80U) != 0U) {
                    ape += 32U;
                }
                if (ape > audio_end - tag) {
                    return std::nullopt;
                }
                audio_end -= ape;
            }
        }
        return std::vector{Span{.begin = tag, .end = audio_end}};
    }
    if (adapter == "taglib-mp4-v1") {
        std::vector<Span> spans;
        std::uint64_t offset = 0U;
        while (offset < length) {
            std::string box(16U, '\0');
            box.resize(static_cast<std::size_t>(std::min<std::uint64_t>(16U, length - offset)));
            if (box.size() < 8U || !read_exactly(descriptor, offset, box)) {
                return std::nullopt;
            }
            std::uint64_t size = big_endian(box, 0U);
            const auto type = box.substr(4U, 4U);
            if (size == 1U) {
                if (box.size() < 16U) {
                    return std::nullopt;
                }
                size =
                    (static_cast<std::uint64_t>(big_endian(box, 8U)) << 32U) | big_endian(box, 12U);
            } else if (size == 0U) {
                size = length - offset;
            }
            if (size < 8U || offset + size > length) {
                return std::nullopt;
            }
            if (type != "moov" && type != "free" && type != "skip") {
                spans.push_back(Span{.begin = offset, .end = offset + size});
            }
            offset += size;
        }
        return spans;
    }
    return std::nullopt;
}

[[nodiscard]] std::string_view label_of(const std::string& adapter) {
    if (adapter == "taglib-flac-v1") {
        return "FLAC";
    }
    if (adapter == "taglib-mpeg-v1") {
        return "MP3";
    }
    return "MP4";
}

// The write, as the format's writer makes it on a prepared copy, made on the
// overlay. Answers whether it went through.
[[nodiscard]] core::Result<void> write_on(const MetadataWritePlanSource& source_plan,
                                          OverlayStream& stream,
                                          const core::CancellationToken& cancellation) {
    const auto& adapter = source_plan.adapter_name;
    const auto& path = source_plan.raw_path;
    if (adapter == "taglib-flac-v1") {
        TagLib::FLAC::File file{&stream, false};
        if (!file.isValid()) {
            return std::unexpected(
                in_place_error(core::ErrorCode::backend, "TagLib rejected the FLAC file", path));
        }
        return text_writer_detail::apply_text_changes_to_properties("FLAC", source_plan, file, path,
                                                                    cancellation);
    }
    if (adapter == "taglib-mpeg-v1") {
        {
            TagLib::MPEG::File file{&stream, false};
            if (!file.isValid()) {
                return std::unexpected(
                    in_place_error(core::ErrorCode::backend, "TagLib rejected the MP3 file", path));
            }
            auto applied = text_writer_detail::apply_text_changes_to_properties(
                "MP3", source_plan, file, path, cancellation, false);
            if (!applied) {
                return applied;
            }
        }
        TagLib::MPEG::File file{&stream, false};
        return mp3_detail::apply_rating_frames_to(source_plan, file, path);
    }
    {
        TagLib::MP4::File file{&stream, false, TagLib::AudioProperties::Average,
                               mp4_detail::FreeFormFactory::instance()};
        if (!file.isValid()) {
            return std::unexpected(
                in_place_error(core::ErrorCode::backend, "TagLib rejected the MP4 file", path));
        }
        auto applied = text_writer_detail::apply_text_changes_to_properties(
            "MP4", source_plan, file, path, cancellation, false);
        if (!applied) {
            return applied;
        }
    }
    TagLib::MP4::File file{&stream, false, TagLib::AudioProperties::Average,
                           mp4_detail::FreeFormFactory::instance()};
    return mp4_detail::spell_rating_atom_in(source_plan, file, path);
}

} // namespace

core::Result<std::optional<InPlaceMetadataWrite>>
prepare_in_place_metadata_write(const MetadataWritePlanSource& source_plan,
                                const core::CancellationToken& cancellation) {
    const auto& adapter = source_plan.adapter_name;
    const auto& path = source_plan.raw_path;
    if ((adapter != "taglib-flac-v1" && adapter != "taglib-mpeg-v1" &&
         adapter != "taglib-mp4-v1") ||
        source_plan.changes.empty() || source_plan.artwork || !source_plan.observed_revision) {
        return std::optional<InPlaceMetadataWrite>{};
    }
    auto before = read_local_metadata(path, cancellation);
    if (!before) {
        return std::unexpected(std::move(before.error()));
    }
    if (before->source_revision != *source_plan.observed_revision ||
        before->adapter_name != adapter) {
        return std::unexpected(in_place_error(
            core::ErrorCode::conflict, "the source changed after the write was planned", path));
    }
    auto originals = text_writer_detail::verify_plan_originals(label_of(adapter), before->document,
                                                               source_plan, path);
    if (!originals) {
        return std::unexpected(std::move(originals.error()));
    }

    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return std::unexpected(
            in_place_error(core::ErrorCode::io,
                           std::string{"opening the file failed: "} +
                               std::error_code{errno, std::generic_category()}.message(),
                           path));
    }
    struct Closer {
        int descriptor;
        ~Closer() { ::close(descriptor); }
    } closer{descriptor};
    const auto length = before->source_revision.size;
    const auto spans = protected_spans(adapter, descriptor, length);
    if (!spans) {
        return std::optional<InPlaceMetadataWrite>{};
    }

    OverlayStream stream{path, descriptor, static_cast<TagLib::offset_t>(length)};
    auto written = write_on(source_plan, stream, cancellation);
    if (!stream.usable()) {
        // Outgrew the room it had: the prepared copy takes it.
        return std::optional<InPlaceMetadataWrite>{};
    }
    if (!written) {
        return std::unexpected(std::move(written.error()));
    }

    // Nothing that must stay may be touched.
    for (const auto& [offset, bytes] : stream.writes()) {
        const auto begin = static_cast<std::uint64_t>(offset);
        const auto end = begin + bytes.size();
        for (const auto& span : *spans) {
            if (begin < span.end && span.begin < end) {
                return std::optional<InPlaceMetadataWrite>{};
            }
        }
    }
    // FLAC: every block but the comment and the padding as it was, in order,
    // and the audio where it was.
    if (adapter == "taglib-flac-v1") {
        auto old_header = parse_flac_header(descriptor, nullptr, length);
        auto new_header = parse_flac_header(descriptor, &stream, length);
        if (!old_header || !new_header || old_header->audio_offset != new_header->audio_offset) {
            return std::optional<InPlaceMetadataWrite>{};
        }
        const auto kept = [](const FlacHeader& header) {
            std::vector<std::pair<std::uint8_t, std::string>> blocks;
            for (const auto& block : header.blocks) {
                if (block.first != 1U && block.first != 4U) {
                    blocks.push_back(block);
                }
            }
            return blocks;
        };
        if (kept(*old_header) != kept(*new_header)) {
            return std::unexpected(
                in_place_error(core::ErrorCode::conflict,
                               "an in-place FLAC write changed a preserved metadata block", path));
        }
    }

    // The tags the ranges leave, read back and held to the plan as a
    // prepared copy's are.
    stream.seek(0, TagLib::IOStream::Beginning);
    auto after = read_metadata_from_stream(path, stream, cancellation);
    if (!after) {
        return std::unexpected(std::move(after.error()));
    }
    auto before_document = before->document;
    auto after_document = after->document;
    if (adapter == "taglib-mpeg-v1") {
        if (const auto planned = mp3_detail::planned_rating_of(source_plan)) {
            const auto popularimeter = [](const NativeObjectIdentity& object) {
                return object.identity.starts_with("POPM");
            };
            std::erase_if(before_document.unsupported_native_objects, popularimeter);
            std::erase_if(after_document.unsupported_native_objects, popularimeter);
            const auto expected = *planned && **planned > 0U
                                      ? std::optional{popularimeter_byte(**planned)}
                                      : std::nullopt;
            if (expected && after->popularimeter != expected) {
                return std::unexpected(
                    in_place_error(core::ErrorCode::conflict,
                                   "an in-place MP3 rating frame differs from the plan", path));
            }
        }
    }
    auto verified =
        text_writer_detail::verify_text_result(label_of(adapter), before_document, after_document,
                                               source_plan, path, adapter == "taglib-flac-v1");
    if (!verified) {
        return std::unexpected(std::move(verified.error()));
    }

    InPlaceMetadataWrite write{
        .ranges = {},
        .document = std::move(after->document),
        .popularimeter = after->popularimeter,
        .length = length,
    };
    for (const auto& [offset, bytes] : stream.writes()) {
        std::string old_bytes(bytes.size(), '\0');
        if (!read_exactly(descriptor, static_cast<std::uint64_t>(offset), old_bytes)) {
            return std::unexpected(
                in_place_error(core::ErrorCode::io, "reading the bytes replaced failed", path));
        }
        if (old_bytes == bytes) {
            continue;
        }
        write.ranges.push_back(InPlaceRange{.offset = static_cast<std::uint64_t>(offset),
                                            .before = std::move(old_bytes),
                                            .after = bytes});
    }
    return std::optional{std::move(write)};
}

} // namespace trackknife::metadata
