// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/metadata/artwork.hpp"

extern "C" {
#include <libavutil/mem.h>
#include <libavutil/sha.h>
}

#include <attachedpictureframe.h>
#include <fileref.h>
#include <tfilestream.h>
#include <flacfile.h>
#include <flacpicture.h>
#include <id3v2tag.h>
#include <mp4coverart.h>
#include <mp4file.h>
#include <mp4item.h>
#include <mp4tag.h>
#include <mpegfile.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <ranges>
#include <span>
#include <system_error>
#include <utility>

namespace trackknife::metadata {
namespace {

constexpr std::size_t maximum_external_patterns = 256U;
constexpr std::size_t maximum_attribute_bytes = 64U * 1024U;

struct ShaDeleter {
    void operator()(AVSHA* context) const noexcept { av_free(context); }
};

struct InspectedImage {
    std::string mime_type;
    std::optional<std::uint32_t> width;
    std::optional<std::uint32_t> height;
};

[[nodiscard]] core::Error error(const core::ErrorCode code, std::string message,
                                const std::string& raw_path) {
    return core::Error{
        .code = code,
        .message = std::move(message),
        .context = {{.key = "path", .value = core::escape_raw_path(raw_path)}},
    };
}

[[nodiscard]] core::Error cancelled(const std::string& raw_path) {
    return error(core::ErrorCode::cancelled, "artwork inventory was cancelled", raw_path);
}

[[nodiscard]] bool is_native_flac(const std::string& raw_path) {
    std::ifstream input{std::filesystem::path{raw_path}, std::ios::binary};
    std::array<char, 4> marker{};
    return input.read(marker.data(), static_cast<std::streamsize>(marker.size())) &&
           marker == std::array<char, 4>{'f', 'L', 'a', 'C'};
}

[[nodiscard]] bool is_native_mp4(const std::string& raw_path) {
    std::ifstream input{std::filesystem::path{raw_path}, std::ios::binary};
    std::array<char, 8> header{};
    return input.read(header.data(), static_cast<std::streamsize>(header.size())) &&
           header[4] == 'f' && header[5] == 't' && header[6] == 'y' && header[7] == 'p';
}

[[nodiscard]] bool is_native_mpeg(const std::string& raw_path) {
    std::ifstream input{std::filesystem::path{raw_path}, std::ios::binary};
    std::array<char, 3> marker{};
    if (!input.read(marker.data(), static_cast<std::streamsize>(marker.size()))) {
        return false;
    }
    if (marker == std::array<char, 3>{'I', 'D', '3'}) {
        return true;
    }
    const auto first = static_cast<unsigned char>(marker[0]);
    const auto second = static_cast<unsigned char>(marker[1]);
    return first == 0xFFU && (second & 0xE0U) == 0xE0U;
}

[[nodiscard]] core::Result<core::ContentFingerprint>
fingerprint(const std::span<const unsigned char> bytes, const std::string& raw_path) {
    std::unique_ptr<AVSHA, ShaDeleter> context{av_sha_alloc()};
    if (!context || av_sha_init(context.get(), 256) != 0) {
        return std::unexpected(error(core::ErrorCode::backend,
                                     "SHA-256 artwork fingerprint initialization failed",
                                     raw_path));
    }
    av_sha_update(context.get(), bytes.data(), bytes.size());
    core::ContentFingerprint result;
    av_sha_final(context.get(), result.sha256.data());
    return result;
}

[[nodiscard]] std::uint32_t read_be_u32(const std::span<const unsigned char> bytes,
                                        const std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
           (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U) |
           static_cast<std::uint32_t>(bytes[offset + 3U]);
}

[[nodiscard]] std::uint16_t read_be_u16(const std::span<const unsigned char> bytes,
                                        const std::size_t offset) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[offset]) << 8U) |
                                      static_cast<std::uint16_t>(bytes[offset + 1U]));
}

[[nodiscard]] bool is_jpeg_start_of_frame(const unsigned char marker) {
    switch (marker) {
    case 0xC0U:
    case 0xC1U:
    case 0xC2U:
    case 0xC3U:
    case 0xC5U:
    case 0xC6U:
    case 0xC7U:
    case 0xC9U:
    case 0xCAU:
    case 0xCBU:
    case 0xCDU:
    case 0xCEU:
    case 0xCFU:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] std::optional<InspectedImage>
inspect_encoded_image(const std::span<const unsigned char> bytes) {
    constexpr std::array<unsigned char, 8> png_signature{0x89U, 'P',   'N',   'G',
                                                         0x0DU, 0x0AU, 0x1AU, 0x0AU};
    if (bytes.size() >= 24U && std::ranges::equal(png_signature, bytes.first(8U)) &&
        bytes[12U] == 'I' && bytes[13U] == 'H' && bytes[14U] == 'D' && bytes[15U] == 'R') {
        const auto width = read_be_u32(bytes, 16U);
        const auto height = read_be_u32(bytes, 20U);
        if (width == 0U || height == 0U) {
            return std::nullopt;
        }
        return InspectedImage{.mime_type = "image/png", .width = width, .height = height};
    }

    if (bytes.size() < 4U || bytes[0U] != 0xFFU || bytes[1U] != 0xD8U) {
        return std::nullopt;
    }
    std::size_t offset = 2U;
    while (offset + 1U < bytes.size()) {
        while (offset < bytes.size() && bytes[offset] != 0xFFU) {
            ++offset;
        }
        while (offset < bytes.size() && bytes[offset] == 0xFFU) {
            ++offset;
        }
        if (offset >= bytes.size()) {
            break;
        }
        const auto marker = bytes[offset++];
        if (marker == 0x00U || marker == 0xD8U || marker == 0xD9U || marker == 0x01U ||
            (marker >= 0xD0U && marker <= 0xD7U)) {
            continue;
        }
        if (offset + 2U > bytes.size()) {
            break;
        }
        const auto segment_size = static_cast<std::size_t>(read_be_u16(bytes, offset));
        if (segment_size < 2U || segment_size > bytes.size() - offset) {
            break;
        }
        if (is_jpeg_start_of_frame(marker) && segment_size >= 7U) {
            const auto height = static_cast<std::uint32_t>(read_be_u16(bytes, offset + 3U));
            const auto width = static_cast<std::uint32_t>(read_be_u16(bytes, offset + 5U));
            if (width == 0U || height == 0U) {
                return std::nullopt;
            }
            return InspectedImage{.mime_type = "image/jpeg", .width = width, .height = height};
        }
        offset += segment_size;
    }
    return std::nullopt;
}

[[nodiscard]] ArtworkRole artwork_role(const TagLib::FLAC::Picture::Type type) {
    using Picture = TagLib::FLAC::Picture;
    switch (type) {
    case Picture::FrontCover:
        return ArtworkRole::front;
    case Picture::BackCover:
        return ArtworkRole::back;
    case Picture::LeadArtist:
    case Picture::Artist:
    case Picture::Conductor:
    case Picture::Band:
    case Picture::Composer:
    case Picture::Lyricist:
    case Picture::DuringRecording:
    case Picture::DuringPerformance:
    case Picture::BandLogo:
        return ArtworkRole::artist;
    case Picture::Media:
        return ArtworkRole::disc;
    case Picture::FileIcon:
    case Picture::OtherFileIcon:
        return ArtworkRole::icon;
    default:
        return ArtworkRole::other;
    }
}

[[nodiscard]] bool valid_pattern(const ExternalArtworkPattern& pattern) {
    return !pattern.raw_basename.empty() && pattern.raw_basename != "." &&
           pattern.raw_basename != ".." && pattern.raw_basename.find('/') == std::string::npos &&
           pattern.raw_basename.find('\0') == std::string::npos;
}

[[nodiscard]] core::Result<void> validate_policy(const ArtworkInventoryPolicy& policy,
                                                 const std::string& raw_path) {
    if (policy.maximum_items == 0U || policy.maximum_item_bytes == 0U ||
        policy.maximum_total_bytes == 0U ||
        policy.external_patterns.size() > maximum_external_patterns) {
        return std::unexpected(error(core::ErrorCode::invalid_argument,
                                     "artwork inventory policy limits are invalid", raw_path));
    }
    for (std::size_t index = 0U; index < policy.external_patterns.size(); ++index) {
        if (!valid_pattern(policy.external_patterns[index])) {
            return std::unexpected(error(core::ErrorCode::invalid_argument,
                                         "external artwork pattern is not an exact basename",
                                         raw_path));
        }
        if (std::ranges::any_of(
                policy.external_patterns | std::views::take(index), [&](const auto& existing) {
                    return existing.raw_basename == policy.external_patterns[index].raw_basename;
                })) {
            return std::unexpected(error(core::ErrorCode::invalid_argument,
                                         "external artwork pattern basename is duplicated",
                                         raw_path));
        }
    }
    return {};
}

[[nodiscard]] core::Result<void> reserve_item(const ArtworkInventoryPolicy& policy,
                                              const std::uint64_t byte_size,
                                              const std::uint64_t total_bytes,
                                              const std::size_t item_count,
                                              const std::string& raw_path) {
    if (item_count >= policy.maximum_items) {
        return std::unexpected(error(core::ErrorCode::limit_exceeded,
                                     "artwork item count exceeds the inventory limit", raw_path));
    }
    if (byte_size > policy.maximum_item_bytes) {
        return std::unexpected(error(core::ErrorCode::limit_exceeded,
                                     "artwork item exceeds the encoded-size limit", raw_path));
    }
    if (byte_size > policy.maximum_total_bytes ||
        total_bytes > policy.maximum_total_bytes - byte_size) {
        return std::unexpected(error(core::ErrorCode::limit_exceeded,
                                     "artwork bytes exceed the aggregate inventory limit",
                                     raw_path));
    }
    return {};
}

void mark_duplicate(std::vector<ArtworkInventoryItem>& items, ArtworkInventoryItem& item) {
    const auto duplicate = std::ranges::find(items, item.content_fingerprint,
                                             &ArtworkInventoryItem::content_fingerprint);
    if (duplicate != items.end()) {
        item.duplicate_of = static_cast<std::size_t>(std::distance(items.begin(), duplicate));
    }
}

[[nodiscard]] ArtworkInventoryIssue issue(std::string raw_path, core::Error issue_error) {
    return ArtworkInventoryIssue{
        .raw_source_path = std::move(raw_path),
        .error = std::move(issue_error),
    };
}

// `covr` entries carry a format enum instead of a MIME string; unknown
// formats fall back to the signature sniff. The inventory and the byte
// reread must derive MIME identically.
[[nodiscard]] std::string covr_mime_type(const TagLib::MP4::CoverArt::Format format,
                                         const std::span<const unsigned char> bytes) {
    switch (format) {
    case TagLib::MP4::CoverArt::JPEG:
        return "image/jpeg";
    case TagLib::MP4::CoverArt::PNG:
        return "image/png";
    case TagLib::MP4::CoverArt::BMP:
        return "image/bmp";
    case TagLib::MP4::CoverArt::GIF:
        return "image/gif";
    default:
        break;
    }
    const auto inspected = inspect_encoded_image(bytes);
    return inspected ? inspected->mime_type : std::string{};
}

// One embedded picture as its adapter exposes it; empty MIME and absent
// dimensions fall back to the signature sniff in append_embedded_item so
// every adapter derives them identically.
struct EmbeddedPictureView {
    ArtworkRole role{ArtworkRole::other};
    std::string native_type;
    std::string mime_type;
    std::string description;
    std::optional<std::uint32_t> width;
    std::optional<std::uint32_t> height;
    std::span<const unsigned char> bytes;
};

[[nodiscard]] core::Result<void>
append_embedded_item(LocalArtworkInventory& result, const ArtworkInventoryPolicy& policy,
                     std::uint64_t& total_bytes, const std::string& raw_media_path,
                     const core::LocalSourceRevision& media_revision, const std::size_t ordinal,
                     EmbeddedPictureView view) {
    if (view.bytes.empty()) {
        return std::unexpected(error(core::ErrorCode::backend,
                                     "an embedded artwork picture has no encoded bytes",
                                     raw_media_path));
    }
    const auto byte_size = static_cast<std::uint64_t>(view.bytes.size());
    if (auto reserved =
            reserve_item(policy, byte_size, total_bytes, result.items.size(), raw_media_path);
        !reserved) {
        return std::unexpected(std::move(reserved.error()));
    }
    auto item_fingerprint = fingerprint(view.bytes, raw_media_path);
    if (!item_fingerprint) {
        return std::unexpected(std::move(item_fingerprint.error()));
    }
    if (view.native_type.size() + view.mime_type.size() + view.description.size() >
        maximum_attribute_bytes) {
        return std::unexpected(error(core::ErrorCode::limit_exceeded,
                                     "embedded artwork attributes exceed the text limit",
                                     raw_media_path));
    }
    const auto inspected = inspect_encoded_image(view.bytes);
    ArtworkInventoryItem item{
        .role = view.role,
        .native_type = std::move(view.native_type),
        .mime_type = !view.mime_type.empty()
                         ? std::move(view.mime_type)
                         : inspected.transform([](const auto& image) { return image.mime_type; })
                               .value_or(""),
        .description = std::move(view.description),
        .width = view.width ? view.width
                            : inspected.and_then([](const auto& image) { return image.width; }),
        .height = view.height ? view.height
                              : inspected.and_then([](const auto& image) { return image.height; }),
        .byte_size = byte_size,
        .content_fingerprint = *item_fingerprint,
        .provenance = ArtworkProvenance::embedded,
        .raw_source_path = raw_media_path,
        .source_revision = media_revision,
        .source_ordinal = ordinal,
        .duplicate_of = {},
    };
    mark_duplicate(result.items, item);
    result.items.push_back(std::move(item));
    total_bytes += byte_size;
    return {};
}

// APIC frames of one ID3v2 tag, in frame order; a non-APIC entry in the
// APIC list is a malformed tag.
[[nodiscard]] std::vector<TagLib::ID3v2::AttachedPictureFrame*>
apic_frames(TagLib::ID3v2::Tag* tag) {
    std::vector<TagLib::ID3v2::AttachedPictureFrame*> frames;
    if (tag == nullptr) {
        return frames;
    }
    for (auto* frame : tag->frameListMap()["APIC"]) {
        frames.push_back(dynamic_cast<TagLib::ID3v2::AttachedPictureFrame*>(frame));
    }
    return frames;
}

} // namespace

std::string_view artwork_role_name(const ArtworkRole role) {
    switch (role) {
    case ArtworkRole::front:
        return "front";
    case ArtworkRole::back:
        return "back";
    case ArtworkRole::artist:
        return "artist";
    case ArtworkRole::disc:
        return "disc";
    case ArtworkRole::icon:
        return "icon";
    case ArtworkRole::other:
        return "other";
    }
    return "other";
}

std::string_view artwork_provenance_name(const ArtworkProvenance provenance) {
    switch (provenance) {
    case ArtworkProvenance::embedded:
        return "embedded";
    case ArtworkProvenance::external:
        return "external";
    }
    return "embedded";
}

ArtworkInventoryPolicy default_artwork_inventory_policy() {
    return ArtworkInventoryPolicy{
        .external_patterns =
            {
                {.raw_basename = "cover.jpg", .role = ArtworkRole::front},
                {.raw_basename = "cover.jpeg", .role = ArtworkRole::front},
                {.raw_basename = "cover.png", .role = ArtworkRole::front},
                {.raw_basename = "folder.jpg", .role = ArtworkRole::front},
                {.raw_basename = "folder.jpeg", .role = ArtworkRole::front},
                {.raw_basename = "folder.png", .role = ArtworkRole::front},
                {.raw_basename = "Folder.jpg", .role = ArtworkRole::front},
                {.raw_basename = "Folder.jpeg", .role = ArtworkRole::front},
                {.raw_basename = "Folder.png", .role = ArtworkRole::front},
                {.raw_basename = "front.jpg", .role = ArtworkRole::front},
                {.raw_basename = "front.jpeg", .role = ArtworkRole::front},
                {.raw_basename = "front.png", .role = ArtworkRole::front},
            },
        .maximum_items = 64U,
        .maximum_item_bytes = 16U * 1024U * 1024U,
        .maximum_total_bytes = 64U * 1024U * 1024U,
    };
}

core::Result<ArtworkImageFile>
read_artwork_image_file(const std::string& raw_path, const std::uint64_t maximum_bytes,
                        const core::CancellationToken& cancellation) {
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(raw_path));
    }
    if (raw_path.empty() || raw_path.find('\0') != std::string::npos || maximum_bytes == 0U) {
        return std::unexpected(error(core::ErrorCode::invalid_argument,
                                     "artwork image input and limit must be valid", raw_path));
    }
    auto revision_before = core::observe_local_source_revision(raw_path);
    if (!revision_before) {
        return std::unexpected(std::move(revision_before.error()));
    }
    if (revision_before->size == 0U || revision_before->size > maximum_bytes) {
        return std::unexpected(error(core::ErrorCode::limit_exceeded,
                                     revision_before->size == 0U
                                         ? "artwork image input is empty"
                                         : "artwork image input exceeds the encoded-size limit",
                                     raw_path));
    }
    if (revision_before->size >
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return std::unexpected(error(core::ErrorCode::limit_exceeded,
                                     "artwork image input exceeds addressable memory", raw_path));
    }
    std::vector<unsigned char> bytes(static_cast<std::size_t>(revision_before->size));
    std::ifstream input{std::filesystem::path{raw_path}, std::ios::binary};
    if (!input.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size())) ||
        input.peek() != std::ifstream::traits_type::eof()) {
        return std::unexpected(
            error(core::ErrorCode::io, "artwork image input could not be read exactly", raw_path));
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(raw_path));
    }
    auto revision_after = core::observe_local_source_revision(raw_path);
    if (!revision_after || *revision_after != *revision_before) {
        return std::unexpected(error(core::ErrorCode::conflict,
                                     "artwork image input changed while it was being read",
                                     raw_path));
    }
    const auto inspected = inspect_encoded_image(bytes);
    if (!inspected) {
        return std::unexpected(error(core::ErrorCode::unsupported,
                                     "artwork image input is not a supported PNG or JPEG image",
                                     raw_path));
    }
    auto image_fingerprint = fingerprint(bytes, raw_path);
    if (!image_fingerprint) {
        return std::unexpected(std::move(image_fingerprint.error()));
    }
    return ArtworkImageFile{
        .raw_path = raw_path,
        .source_revision = *revision_after,
        .mime_type = inspected->mime_type,
        .width = inspected->width,
        .height = inspected->height,
        .byte_size = revision_after->size,
        .content_fingerprint = *image_fingerprint,
        .embedded_source_ordinal = std::nullopt,
    };
}

core::Result<std::vector<unsigned char>>
read_artwork_image_bytes(const ArtworkImageFile& image, const std::uint64_t maximum_bytes,
                         const core::CancellationToken& cancellation) {
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(image.raw_path));
    }
    if (image.raw_path.empty() || image.raw_path.find('\0') != std::string::npos ||
        maximum_bytes == 0U || image.byte_size == 0U || image.byte_size > maximum_bytes ||
        image.byte_size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return std::unexpected(error(core::ErrorCode::invalid_argument,
                                     "artwork image evidence and limit must be valid",
                                     image.raw_path));
    }
    auto revision_before = core::observe_local_source_revision(image.raw_path);
    if (!revision_before) {
        return std::unexpected(std::move(revision_before.error()));
    }
    if (*revision_before != image.source_revision) {
        return std::unexpected(error(core::ErrorCode::conflict,
                                     "artwork image source changed after inspection",
                                     image.raw_path));
    }

    std::vector<unsigned char> bytes;
    std::string observed_mime;
    std::optional<std::uint32_t> observed_width;
    std::optional<std::uint32_t> observed_height;
    if (image.embedded_source_ordinal) {
        const auto ordinal = *image.embedded_source_ordinal;
        const auto missing_ordinal =
            error(core::ErrorCode::conflict, "embedded artwork donor ordinal no longer exists",
                  image.raw_path);
        // Every embedded adapter rereads through the same derivation the
        // inventory used, so the equality contract below stays exact.
        if (is_native_flac(image.raw_path)) {
            // Read-only: opened for writing, closing it reads as a change to
            // anything watching the folder (ADR-0232).
            TagLib::FileStream stream{image.raw_path.c_str(), true};
            TagLib::FLAC::File file{&stream, false};
            if (!file.isValid()) {
                return std::unexpected(error(core::ErrorCode::backend,
                                             "TagLib rejected the embedded artwork donor",
                                             image.raw_path));
            }
            const auto pictures = file.pictureList();
            if (ordinal >= pictures.size() ||
                pictures[static_cast<unsigned int>(ordinal)] == nullptr) {
                return std::unexpected(missing_ordinal);
            }
            const auto* picture = pictures[static_cast<unsigned int>(ordinal)];
            const auto data = picture->data();
            const auto* begin = reinterpret_cast<const unsigned char*>(data.data());
            bytes.assign(begin, begin + data.size());
            const auto inspected = inspect_encoded_image(bytes);
            observed_mime = picture->mimeType().to8Bit(true);
            if (observed_mime.empty() && inspected) {
                observed_mime = inspected->mime_type;
            }
            observed_width =
                picture->width() > 0
                    ? std::optional{static_cast<std::uint32_t>(picture->width())}
                    : inspected.and_then([](const auto& value) { return value.width; });
            observed_height =
                picture->height() > 0
                    ? std::optional{static_cast<std::uint32_t>(picture->height())}
                    : inspected.and_then([](const auto& value) { return value.height; });
        } else if (is_native_mp4(image.raw_path)) {
            TagLib::FileStream stream{image.raw_path.c_str(), true};
            TagLib::MP4::File file{&stream, false};
            if (!file.isValid()) {
                return std::unexpected(error(core::ErrorCode::backend,
                                             "TagLib rejected the embedded artwork donor",
                                             image.raw_path));
            }
            const auto covers = file.tag() != nullptr && file.tag()->contains("covr")
                                    ? file.tag()->item("covr").toCoverArtList()
                                    : TagLib::MP4::CoverArtList{};
            if (ordinal >= covers.size()) {
                return std::unexpected(missing_ordinal);
            }
            const auto& cover = covers[static_cast<unsigned int>(ordinal)];
            const auto data = cover.data();
            const auto* begin = reinterpret_cast<const unsigned char*>(data.data());
            bytes.assign(begin, begin + data.size());
            const auto inspected = inspect_encoded_image(bytes);
            observed_mime = covr_mime_type(cover.format(), bytes);
            observed_width = inspected.and_then([](const auto& value) { return value.width; });
            observed_height = inspected.and_then([](const auto& value) { return value.height; });
        } else if (is_native_mpeg(image.raw_path)) {
            TagLib::FileStream stream{image.raw_path.c_str(), true};
            TagLib::MPEG::File file{&stream, false};
            if (!file.isValid()) {
                return std::unexpected(error(core::ErrorCode::backend,
                                             "TagLib rejected the embedded artwork donor",
                                             image.raw_path));
            }
            const auto frames = apic_frames(file.ID3v2Tag(false));
            if (ordinal >= frames.size() || frames[ordinal] == nullptr) {
                return std::unexpected(missing_ordinal);
            }
            const auto data = frames[ordinal]->picture();
            const auto* begin = reinterpret_cast<const unsigned char*>(data.data());
            bytes.assign(begin, begin + data.size());
            const auto inspected = inspect_encoded_image(bytes);
            observed_mime = frames[ordinal]->mimeType().to8Bit(true);
            if (observed_mime.empty() && inspected) {
                observed_mime = inspected->mime_type;
            }
            observed_width = inspected.and_then([](const auto& value) { return value.width; });
            observed_height = inspected.and_then([](const auto& value) { return value.height; });
        } else {
            return std::unexpected(error(core::ErrorCode::unsupported,
                                         "this container has no embedded artwork adapter",
                                         image.raw_path));
        }
        if (bytes.empty()) {
            return std::unexpected(error(core::ErrorCode::conflict,
                                         "embedded artwork donor has no encoded bytes",
                                         image.raw_path));
        }
    } else {
        if (revision_before->size != image.byte_size) {
            return std::unexpected(error(core::ErrorCode::conflict,
                                         "standalone artwork size differs from inspection",
                                         image.raw_path));
        }
        bytes.resize(static_cast<std::size_t>(image.byte_size));
        std::ifstream input{std::filesystem::path{image.raw_path}, std::ios::binary};
        if (!input.read(reinterpret_cast<char*>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size())) ||
            input.peek() != std::ifstream::traits_type::eof()) {
            return std::unexpected(error(core::ErrorCode::io,
                                         "standalone artwork could not be read exactly",
                                         image.raw_path));
        }
        const auto inspected = inspect_encoded_image(bytes);
        if (!inspected) {
            return std::unexpected(error(core::ErrorCode::unsupported,
                                         "artwork input is no longer a supported PNG or JPEG",
                                         image.raw_path));
        }
        observed_mime = inspected->mime_type;
        observed_width = inspected->width;
        observed_height = inspected->height;
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(image.raw_path));
    }
    auto revision_after = core::observe_local_source_revision(image.raw_path);
    auto observed_fingerprint = fingerprint(bytes, image.raw_path);
    if (!revision_after || *revision_after != image.source_revision || !observed_fingerprint ||
        bytes.size() != image.byte_size || *observed_fingerprint != image.content_fingerprint ||
        observed_mime != image.mime_type || observed_width != image.width ||
        observed_height != image.height) {
        return std::unexpected(error(core::ErrorCode::conflict,
                                     "artwork image bytes differ from the reviewed evidence",
                                     image.raw_path));
    }
    return bytes;
}

core::Result<LocalArtworkInventory>
read_local_artwork_inventory(const std::string& raw_media_path,
                             const ArtworkInventoryPolicy& policy,
                             const core::CancellationToken& cancellation) {
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(raw_media_path));
    }
    if (auto valid = validate_policy(policy, raw_media_path); !valid) {
        return std::unexpected(std::move(valid.error()));
    }
    auto media_revision = core::observe_local_source_revision(raw_media_path);
    if (!media_revision) {
        return std::unexpected(std::move(media_revision.error()));
    }

    LocalArtworkInventory result{
        .raw_media_path = raw_media_path,
        .media_revision = *media_revision,
        .embedded_adapter_name = {},
        .capabilities = {},
        .items = {},
        .issues = {},
    };
    std::uint64_t total_bytes = 0U;

    bool embedded_read = false;
    if (is_native_flac(raw_media_path)) {
        TagLib::FileStream stream{raw_media_path.c_str(), true};
        TagLib::FLAC::File file{&stream, false};
        if (!file.isValid()) {
            return std::unexpected(error(core::ErrorCode::backend,
                                         "TagLib rejected the FLAC artwork source",
                                         raw_media_path));
        }
        result.capabilities.embedded_readable = true;
        result.embedded_adapter_name = "taglib-flac-picture-v1";
        embedded_read = true;
        const auto pictures = file.pictureList();
        for (std::size_t ordinal = 0U; ordinal < pictures.size(); ++ordinal) {
            if (cancellation.is_cancellation_requested()) {
                return std::unexpected(cancelled(raw_media_path));
            }
            const auto* picture = pictures[static_cast<unsigned int>(ordinal)];
            if (picture == nullptr) {
                return std::unexpected(error(core::ErrorCode::backend,
                                             "FLAC artwork inventory contains a null picture",
                                             raw_media_path));
            }
            const auto data = picture->data();
            if (auto appended = append_embedded_item(
                    result, policy, total_bytes, raw_media_path, *media_revision, ordinal,
                    EmbeddedPictureView{
                        .role = artwork_role(picture->type()),
                        .native_type =
                            TagLib::FLAC::Picture::typeToString(picture->type()).to8Bit(true),
                        .mime_type = picture->mimeType().to8Bit(true),
                        .description = picture->description().to8Bit(true),
                        .width = picture->width() > 0
                                     ? std::optional{static_cast<std::uint32_t>(picture->width())}
                                     : std::nullopt,
                        .height = picture->height() > 0
                                      ? std::optional{static_cast<std::uint32_t>(picture->height())}
                                      : std::nullopt,
                        .bytes = {reinterpret_cast<const unsigned char*>(data.data()),
                                  static_cast<std::size_t>(data.size())},
                    });
                !appended) {
                return std::unexpected(std::move(appended.error()));
            }
        }
    } else if (is_native_mp4(raw_media_path)) {
        TagLib::FileStream stream{raw_media_path.c_str(), true};
        TagLib::MP4::File file{&stream, false};
        if (!file.isValid()) {
            return std::unexpected(error(core::ErrorCode::backend,
                                         "TagLib rejected the MP4 artwork source", raw_media_path));
        }
        result.capabilities.embedded_readable = true;
        result.embedded_adapter_name = "taglib-mp4-covr-v1";
        embedded_read = true;
        const auto covers = file.tag() != nullptr && file.tag()->contains("covr")
                                ? file.tag()->item("covr").toCoverArtList()
                                : TagLib::MP4::CoverArtList{};
        std::size_t ordinal = 0U;
        for (const auto& cover : covers) {
            if (cancellation.is_cancellation_requested()) {
                return std::unexpected(cancelled(raw_media_path));
            }
            const auto data = cover.data();
            const std::span<const unsigned char> bytes{
                reinterpret_cast<const unsigned char*>(data.data()),
                static_cast<std::size_t>(data.size())};
            // covr entries are untyped cover art: no picture type, no
            // description, dimensions from the signature sniff (ADR-0137).
            if (auto appended = append_embedded_item(
                    result, policy, total_bytes, raw_media_path, *media_revision, ordinal,
                    EmbeddedPictureView{
                        .role = ArtworkRole::front,
                        .native_type = {},
                        .mime_type = covr_mime_type(cover.format(), bytes),
                        .description = {},
                        .width = {},
                        .height = {},
                        .bytes = bytes,
                    });
                !appended) {
                return std::unexpected(std::move(appended.error()));
            }
            ++ordinal;
        }
    } else if (is_native_mpeg(raw_media_path)) {
        TagLib::FileStream stream{raw_media_path.c_str(), true};
        TagLib::MPEG::File file{&stream, false};
        if (!file.isValid()) {
            return std::unexpected(error(core::ErrorCode::backend,
                                         "TagLib rejected the MP3 artwork source", raw_media_path));
        }
        result.capabilities.embedded_readable = true;
        result.embedded_adapter_name = "taglib-id3v2-apic-v1";
        embedded_read = true;
        const auto frames = apic_frames(file.ID3v2Tag(false));
        for (std::size_t ordinal = 0U; ordinal < frames.size(); ++ordinal) {
            if (cancellation.is_cancellation_requested()) {
                return std::unexpected(cancelled(raw_media_path));
            }
            const auto* frame = frames[ordinal];
            if (frame == nullptr) {
                return std::unexpected(error(core::ErrorCode::backend,
                                             "ID3v2 APIC inventory contains a malformed frame",
                                             raw_media_path));
            }
            const auto data = frame->picture();
            // APIC picture types share FLAC's numbering, so the role
            // mapping and type vocabulary are the same (ADR-0137).
            const auto type = static_cast<TagLib::FLAC::Picture::Type>(frame->type());
            if (auto appended = append_embedded_item(
                    result, policy, total_bytes, raw_media_path, *media_revision, ordinal,
                    EmbeddedPictureView{
                        .role = artwork_role(type),
                        .native_type = TagLib::FLAC::Picture::typeToString(type).to8Bit(true),
                        .mime_type = frame->mimeType().to8Bit(true),
                        .description = frame->description().to8Bit(true),
                        .width = {},
                        .height = {},
                        .bytes = {reinterpret_cast<const unsigned char*>(data.data()),
                                  static_cast<std::size_t>(data.size())},
                    });
                !appended) {
                return std::unexpected(std::move(appended.error()));
            }
        }
    }
    if (embedded_read) {
        auto revision_after = core::observe_local_source_revision(raw_media_path);
        if (!revision_after || *revision_after != *media_revision) {
            return std::unexpected(error(core::ErrorCode::conflict,
                                         "media source changed while artwork was being read",
                                         raw_media_path));
        }
        result.media_revision = *revision_after;
        for (auto& item : result.items) {
            item.source_revision = *revision_after;
        }
    }

    const auto parent = std::filesystem::path{raw_media_path}.parent_path();
    for (std::size_t ordinal = 0U; ordinal < policy.external_patterns.size(); ++ordinal) {
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(cancelled(raw_media_path));
        }
        const auto candidate =
            parent / std::filesystem::path{policy.external_patterns[ordinal].raw_basename};
        const auto& candidate_path = candidate.native();
        auto revision_before = core::observe_local_source_revision(candidate_path);
        if (!revision_before) {
            if (revision_before.error().code == core::ErrorCode::not_found) {
                continue;
            }
            result.issues.push_back(issue(candidate_path, std::move(revision_before.error())));
            continue;
        }
        if (revision_before->size == 0U || revision_before->size > policy.maximum_item_bytes) {
            result.issues.push_back(
                issue(candidate_path, error(core::ErrorCode::limit_exceeded,
                                            revision_before->size == 0U
                                                ? "external artwork file is empty"
                                                : "external artwork exceeds the encoded-size limit",
                                            candidate_path)));
            continue;
        }
        if (auto reserved = reserve_item(policy, revision_before->size, total_bytes,
                                         result.items.size(), candidate_path);
            !reserved) {
            return std::unexpected(std::move(reserved.error()));
        }
        if (revision_before->size >
            static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            return std::unexpected(error(core::ErrorCode::limit_exceeded,
                                         "external artwork size exceeds addressable memory",
                                         candidate_path));
        }
        std::vector<unsigned char> bytes(static_cast<std::size_t>(revision_before->size));
        std::ifstream input{candidate, std::ios::binary};
        if (!input.read(reinterpret_cast<char*>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size())) ||
            input.peek() != std::ifstream::traits_type::eof()) {
            result.issues.push_back(
                issue(candidate_path,
                      error(core::ErrorCode::io, "external artwork could not be read exactly",
                            candidate_path)));
            continue;
        }
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(cancelled(raw_media_path));
        }
        auto revision_after = core::observe_local_source_revision(candidate_path);
        if (!revision_after || *revision_after != *revision_before) {
            result.issues.push_back(
                issue(candidate_path,
                      error(core::ErrorCode::conflict,
                            "external artwork changed while it was being read", candidate_path)));
            continue;
        }
        const auto inspected = inspect_encoded_image(bytes);
        if (!inspected) {
            result.issues.push_back(
                issue(candidate_path, error(core::ErrorCode::unsupported,
                                            "external artwork is not a supported PNG or JPEG image",
                                            candidate_path)));
            continue;
        }
        auto item_fingerprint = fingerprint(bytes, candidate_path);
        if (!item_fingerprint) {
            return std::unexpected(std::move(item_fingerprint.error()));
        }
        ArtworkInventoryItem item{
            .role = policy.external_patterns[ordinal].role,
            .native_type = {},
            .mime_type = inspected->mime_type,
            .description = {},
            .width = inspected->width,
            .height = inspected->height,
            .byte_size = revision_after->size,
            .content_fingerprint = *item_fingerprint,
            .provenance = ArtworkProvenance::external,
            .raw_source_path = candidate_path,
            .source_revision = *revision_after,
            .source_ordinal = ordinal,
            .duplicate_of = {},
        };
        mark_duplicate(result.items, item);
        result.items.push_back(std::move(item));
        total_bytes += revision_after->size;
    }
    return result;
}

std::string artwork_fingerprint_hex(const core::ContentFingerprint& fingerprint) {
    constexpr std::string_view digits{"0123456789abcdef"};
    std::string result;
    result.reserve(fingerprint.sha256.size() * 2U);
    for (const auto byte : fingerprint.sha256) {
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 0x0FU]);
    }
    return result;
}

core::Result<core::ContentFingerprint>
fingerprint_embedded_artwork_inventory(const std::span<const ArtworkInventoryItem> items) {
    std::vector<unsigned char> bytes;
    const auto append_u64 = [&bytes](const std::uint64_t value) {
        for (unsigned shift = 64U; shift != 0U; shift -= 8U) {
            bytes.push_back(static_cast<unsigned char>((value >> (shift - 8U)) & 0xFFU));
        }
    };
    const auto append_text = [&bytes, &append_u64](const std::string_view text) {
        append_u64(text.size());
        bytes.insert(bytes.end(), text.begin(), text.end());
    };
    constexpr std::string_view domain{"trackknife-embedded-artwork-inventory-v1"};
    bytes.insert(bytes.end(), domain.begin(), domain.end());
    append_u64(items.size());
    for (std::size_t index = 0U; index < items.size(); ++index) {
        const auto& item = items[index];
        if (item.provenance != ArtworkProvenance::embedded || item.source_ordinal != index) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::invalid_argument,
                .message = "embedded artwork inventory is not ordered by contiguous ordinals",
                .context = {},
            });
        }
        append_u64(index);
        bytes.push_back(static_cast<unsigned char>(item.role));
        append_text(item.native_type);
        append_text(item.mime_type);
        append_text(item.description);
        bytes.push_back(item.width ? 1U : 0U);
        if (item.width) {
            append_u64(*item.width);
        }
        bytes.push_back(item.height ? 1U : 0U);
        if (item.height) {
            append_u64(*item.height);
        }
        append_u64(item.byte_size);
        bytes.insert(bytes.end(), item.content_fingerprint.sha256.begin(),
                     item.content_fingerprint.sha256.end());
    }
    return fingerprint(bytes, {});
}

std::optional<InspectedEncodedImage>
inspect_encoded_image_bytes(const std::span<const unsigned char> bytes) {
    const auto inspected = inspect_encoded_image(bytes);
    if (!inspected || !inspected->width || !inspected->height) {
        return std::nullopt;
    }
    return InspectedEncodedImage{
        .mime_type = inspected->mime_type,
        .width = *inspected->width,
        .height = *inspected->height,
    };
}

} // namespace trackknife::metadata
