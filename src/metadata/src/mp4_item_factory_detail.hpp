// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <mp4file.h>
#include <mp4item.h>
#include <mp4itemfactory.h>
#include <mp4tag.h>
#include <tbytevector.h>
#include <tpropertymap.h>
#include <tstring.h>
#include <tstringlist.h>

#include <iterator>
#include <vector>

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
        data.append(
            renderAtom("name", TagLib::ByteVector::fromUInt(0) + name.data(TagLib::String::UTF8)));
        auto type = item.atomDataType();
        if (type == TagLib::MP4::TypeUndefined) {
            type =
                item.toStringList().isEmpty() ? TagLib::MP4::TypeImplicit : TagLib::MP4::TypeUTF8;
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

// The planned properties, applied atom by atom. TagLib's setProperties
// rewrites every property under the atom name it would give it: a free-form
// atom the file spells otherwise -- `----:com.apple.iTunes:label`, or
// `encodedby` where TagLib's own is `\251enc` -- was kept and written again in
// TagLib's spelling, so every write doubled such atoms, and one being removed
// was not. Here only the properties that change are touched: every atom that
// reads as one is removed, and its new values are written under the
// file's own free-form spelling where it had one. Returns what could not be
// mapped to an atom, as setProperties does.
[[nodiscard]] inline TagLib::PropertyMap set_changed_properties(TagLib::MP4::File& file,
                                                                const TagLib::PropertyMap& before,
                                                                const TagLib::PropertyMap& after) {
    TagLib::PropertyMap unsupported;
    auto* tag = file.tag();
    if (tag == nullptr) {
        return after;
    }
    const auto* factory = FreeFormFactory::instance();
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
    for (const auto& property : changed) {
        TagLib::String spelled;
        // A copy: atoms are removed from the tag as it is walked.
        const auto items = tag->itemMap();
        for (const auto& [key, item] : items) {
            // As TagLib's own properties() names an atom: the free-form name
            // as spelled, capitalised by the property map.
            const auto read = factory->itemToProperty(key.data(TagLib::String::Latin1), item);
            if (read.first.upper() != property) {
                continue;
            }
            if (spelled.isEmpty() && key.startsWith("----:")) {
                spelled = key;
            }
            tag->removeItem(key);
        }
        if (!after.contains(property) || after[property].isEmpty()) {
            continue;
        }
        const auto [name, item] = factory->itemFromProperty(property, after[property]);
        if (name.isEmpty()) {
            unsupported.insert(property, after[property]);
            continue;
        }
        const TagLib::String key{name, TagLib::String::Latin1};
        tag->setItem(key.startsWith("----:") && !spelled.isEmpty() ? spelled : key, item);
    }
    return unsupported;
}

} // namespace trackknife::metadata::mp4_detail
