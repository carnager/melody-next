// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <mp4item.h>
#include <mp4itemfactory.h>
#include <tbytevector.h>
#include <tstring.h>
#include <tstringlist.h>

#include <iterator>

namespace trackknife::metadata::mp4_detail {

// TagLib renders a free-form atom from its key, `----:mean:name`, split on
// every colon -- and drops one whose name has a colon of its own, as
// Picard's AcousticBrainz tags do (`AB:GENRE`, `AB:MOOD`). Saved through
// TagLib, such a file lost them, and the reread refused every write to it.
// This factory renders the mean up to the second colon and the rest as the
// name, and leaves every other atom to TagLib.
class FreeFormFactory final : public TagLib::MP4::ItemFactory {
  public:
    static FreeFormFactory* instance() {
        static FreeFormFactory factory;
        return &factory;
    }

    TagLib::ByteVector renderItem(const TagLib::String& item_name,
                                  const TagLib::MP4::Item& item) const override {
        const auto header = TagLib::StringList::split(item_name, ":");
        if (!item_name.startsWith("----:") || header.size() <= 3U) {
            return ItemFactory::renderItem(item_name, item);
        }
        // `----`, the mean, and the name with its own colons put back.
        TagLib::String name;
        for (auto part = std::next(header.begin(), 2); part != header.end(); ++part) {
            name += (name.isEmpty() ? TagLib::String{} : TagLib::String{":"}) + *part;
        }
        TagLib::ByteVector data;
        data.append(renderAtom("mean", TagLib::ByteVector::fromUInt(0) +
                                           header[1].data(TagLib::String::UTF8)));
        data.append(renderAtom("name", TagLib::ByteVector::fromUInt(0) +
                                           name.data(TagLib::String::UTF8)));
        auto type = item.atomDataType();
        if (type == TagLib::MP4::TypeUndefined) {
            type = item.toStringList().isEmpty() ? TagLib::MP4::TypeImplicit
                                                 : TagLib::MP4::TypeUTF8;
        }
        if (type == TagLib::MP4::TypeUTF8) {
            for (const auto& value : item.toStringList()) {
                data.append(renderAtom("data", TagLib::ByteVector::fromUInt(type) +
                                                   TagLib::ByteVector(4, '\0') +
                                                   value.data(TagLib::String::UTF8)));
            }
        } else {
            for (const auto& value : item.toByteVectorList()) {
                data.append(renderAtom("data", TagLib::ByteVector::fromUInt(type) +
                                                   TagLib::ByteVector(4, '\0') + value));
            }
        }
        return renderAtom("----", data);
    }

  private:
    FreeFormFactory() = default;
};

} // namespace trackknife::metadata::mp4_detail
