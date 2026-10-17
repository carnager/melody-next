// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <id3v1tag.h>
#include <id3v2frame.h>
#include <id3v2tag.h>
#include <mpegfile.h>
#include <tpropertymap.h>
#include <tstring.h>
#include <tstringlist.h>

#include <algorithm>
#include <vector>

namespace trackknife::metadata::id3_detail {

// What one frame reads as among the properties. TagLib declares it protected
// on the frame base and public on each frame type; a pointer to it taken
// through a derived class reaches every frame's own.
// Not final: it is abstract, as the frame base is, and never made.
struct FrameProperties : TagLib::ID3v2::Frame {
    [[nodiscard]] static TagLib::PropertyMap of(const TagLib::ID3v2::Frame& frame) {
        return (frame.*(&FrameProperties::asProperties))();
    }
};

// The planned properties, applied frame by frame. TagLib's setProperties
// keeps a frame only when the map holds exactly what that frame reads as, so
// a name spread over frames of its own -- two comments in two languages,
// lyrics in two -- was never kept: every write merged them into one frame,
// the others lost, and the reread refused the write. Here only the frames of
// the properties that change are touched; a frame that also carries an
// unchanged property -- an involved-people frame of several roles -- is
// written again with what it held. One TagLib cannot read as properties at
// all -- a role outside its list, a role with no person -- reads as none and
// is left as it is. Returns what could not be mapped to a frame, as
// setProperties does.
[[nodiscard]] inline TagLib::PropertyMap set_changed_properties(TagLib::MPEG::File& file,
                                                                const TagLib::PropertyMap& before,
                                                                const TagLib::PropertyMap& after) {
    std::vector<TagLib::String> changed;
    for (const auto& [property, values] : before) {
        if (!after.contains(property) || after[property] != values) {
            changed.push_back(property);
        }
    }
    for (const auto& [property, values] : after) {
        if (!before.contains(property)) {
            changed.push_back(property);
        }
    }
    if (changed.empty()) {
        return {};
    }
    // ID3v1 holds one value of a few names; TagLib updates it as before --
    // only when the file has one: ID3v1Tag() answers an empty tag otherwise,
    // and filled, it was saved as a new one.
    if (file.hasID3v1Tag()) {
        static_cast<void>(file.ID3v1Tag()->setProperties(after));
    }
    auto* tag = file.ID3v2Tag(true);
    const auto touches = [&changed](const TagLib::PropertyMap& read) {
        return std::ranges::any_of(
            changed, [&read](const auto& property) { return read.contains(property); });
    };
    // Frames are removed from the tag as it is walked: a copy, and not a
    // TagLib list -- shared with the tag's, which deletes its frames, the
    // copy left holding the old one would delete them all again.
    const auto& listed = tag->frameList();
    const std::vector<TagLib::ID3v2::Frame*> frames{listed.begin(), listed.end()};
    std::vector<TagLib::String> carried;
    for (auto* frame : frames) {
        const auto read = FrameProperties::of(*frame);
        if (!touches(read)) {
            continue;
        }
        for (const auto& [property, values] : read) {
            if (!std::ranges::contains(changed, property) &&
                !std::ranges::contains(carried, property)) {
                carried.push_back(property);
            }
        }
        tag->removeFrame(frame); // Deletes it.
    }
    // What is written: the changed properties, and of the unchanged ones a
    // removed frame carried, the values no frame left still holds.
    TagLib::PropertyMap written;
    for (const auto& property : changed) {
        if (after.contains(property) && !after[property].isEmpty()) {
            written.insert(property, after[property]);
        }
    }
    for (const auto& property : carried) {
        TagLib::StringList remaining;
        for (const auto* frame : tag->frameList()) {
            const auto read = FrameProperties::of(*frame);
            if (read.contains(property)) {
                remaining.append(read[property]);
            }
        }
        TagLib::StringList missing;
        for (const auto& value : after[property]) {
            if (const auto found = remaining.find(value); found != remaining.end()) {
                remaining.erase(found);
            } else {
                missing.append(value);
            }
        }
        if (!missing.isEmpty()) {
            written.insert(property, missing);
        }
    }
    // Made by TagLib on a tag of their own, as setProperties would make them,
    // and moved over.
    TagLib::ID3v2::Tag made;
    auto unsupported = made.setProperties(written);
    const auto& made_listed = made.frameList();
    const std::vector<TagLib::ID3v2::Frame*> made_frames{made_listed.begin(), made_listed.end()};
    for (auto* frame : made_frames) {
        made.removeFrame(frame, false);
        tag->addFrame(frame); // The tag owns it.
    }
    return unsupported;
}

} // namespace trackknife::metadata::id3_detail
