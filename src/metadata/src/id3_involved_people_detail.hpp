// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <id3v2tag.h>
#include <mpegfile.h>
#include <textidentificationframe.h>
#include <tpropertymap.h>
#include <tstring.h>
#include <tstringlist.h>

#include <vector>

namespace trackknife::metadata::id3_detail {

// TagLib writes the involved-people frames (TIPL, TMCL) again from the
// properties on every save, and a frame it cannot express as one -- a role
// outside its own short list (`orchestration`), a role with no person -- is
// not among them: it was gone after any write, and the reread refused the
// write for it. Such frames are set aside before the properties are applied
// and put back as they were.
struct InvolvedPeopleFrame {
    TagLib::ByteVector id;
    TagLib::String::Type encoding{TagLib::String::UTF8};
    TagLib::StringList fields;
};

[[nodiscard]] inline std::vector<InvolvedPeopleFrame>
unexpressed_involved_people(TagLib::MPEG::File& file) {
    std::vector<InvolvedPeopleFrame> kept;
    if (!file.hasID3v2Tag()) {
        return kept;
    }
    for (const auto* id : {"TIPL", "TMCL"}) {
        for (const auto* frame : file.ID3v2Tag()->frameList(id)) {
            const auto* text = dynamic_cast<const TagLib::ID3v2::TextIdentificationFrame*>(frame);
            if (text != nullptr && !text->asProperties().unsupportedData().isEmpty()) {
                kept.push_back({.id = text->frameID(),
                                .encoding = text->textEncoding(),
                                .fields = text->fieldList()});
            }
        }
    }
    return kept;
}

inline void restore_involved_people(TagLib::MPEG::File& file,
                                    const std::vector<InvolvedPeopleFrame>& kept) {
    for (const auto& frame : kept) {
        auto* restored = new TagLib::ID3v2::TextIdentificationFrame(frame.id, frame.encoding);
        restored->setText(frame.fields);
        file.ID3v2Tag(true)->addFrame(restored); // The tag owns it.
    }
}

} // namespace trackknife::metadata::id3_detail
