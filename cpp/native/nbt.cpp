#include "nbt.h"

#include <cstring>

namespace native_mc {

namespace {

struct Reader {
    const std::vector<std::uint8_t>& bytes;
    size_t offset = 0;

    bool CanRead(size_t count) const {
        return offset + count <= bytes.size();
    }

    bool ReadU8(std::uint8_t* value) {
        if (!CanRead(1)) return false;
        *value = bytes[offset++];
        return true;
    }

    bool ReadI8(std::int8_t* value) {
        std::uint8_t raw = 0;
        if (!ReadU8(&raw)) return false;
        *value = static_cast<std::int8_t>(raw);
        return true;
    }

    bool ReadI16(std::int16_t* value) {
        if (!CanRead(2)) return false;
        *value = static_cast<std::int16_t>((bytes[offset] << 8) | bytes[offset + 1]);
        offset += 2;
        return true;
    }

    bool ReadU16(std::uint16_t* value) {
        if (!CanRead(2)) return false;
        *value = static_cast<std::uint16_t>((bytes[offset] << 8) | bytes[offset + 1]);
        offset += 2;
        return true;
    }

    bool ReadI32(std::int32_t* value) {
        if (!CanRead(4)) return false;
        *value = (static_cast<std::int32_t>(bytes[offset]) << 24)
            | (static_cast<std::int32_t>(bytes[offset + 1]) << 16)
            | (static_cast<std::int32_t>(bytes[offset + 2]) << 8)
            | static_cast<std::int32_t>(bytes[offset + 3]);
        offset += 4;
        return true;
    }

    bool ReadI64(std::int64_t* value) {
        if (!CanRead(8)) return false;
        std::uint64_t raw = 0;
        for (int i = 0; i < 8; ++i) {
            raw = (raw << 8) | bytes[offset + i];
        }
        offset += 8;
        *value = static_cast<std::int64_t>(raw);
        return true;
    }

    bool ReadFloat(float* value) {
        std::int32_t raw = 0;
        if (!ReadI32(&raw)) return false;
        std::uint32_t bits = static_cast<std::uint32_t>(raw);
        std::memcpy(value, &bits, sizeof(bits));
        return true;
    }

    bool ReadDouble(double* value) {
        if (!CanRead(8)) return false;
        std::uint64_t bits = 0;
        for (int i = 0; i < 8; ++i) {
            bits = (bits << 8) | bytes[offset + i];
        }
        offset += 8;
        std::memcpy(value, &bits, sizeof(bits));
        return true;
    }

    bool ReadString(std::string* value) {
        std::uint16_t length = 0;
        if (!ReadU16(&length) || !CanRead(length)) return false;
        value->assign(reinterpret_cast<const char*>(bytes.data() + offset), length);
        offset += length;
        return true;
    }
};

struct Writer {
    std::vector<std::uint8_t> bytes;

    void WriteU8(std::uint8_t value) {
        bytes.push_back(value);
    }

    void WriteI8(std::int8_t value) {
        bytes.push_back(static_cast<std::uint8_t>(value));
    }

    void WriteI16(std::int16_t value) {
        bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
        bytes.push_back(static_cast<std::uint8_t>(value & 0xff));
    }

    void WriteU16(std::uint16_t value) {
        bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
        bytes.push_back(static_cast<std::uint8_t>(value & 0xff));
    }

    void WriteI32(std::int32_t value) {
        bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
        bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
        bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
        bytes.push_back(static_cast<std::uint8_t>(value & 0xff));
    }

    void WriteI64(std::int64_t value) {
        std::uint64_t bits = static_cast<std::uint64_t>(value);
        for (int shift = 56; shift >= 0; shift -= 8) {
            bytes.push_back(static_cast<std::uint8_t>((bits >> shift) & 0xff));
        }
    }

    void WriteFloat(float value) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        WriteI32(static_cast<std::int32_t>(bits));
    }

    void WriteDouble(double value) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        for (int shift = 56; shift >= 0; shift -= 8) {
            bytes.push_back(static_cast<std::uint8_t>((bits >> shift) & 0xff));
        }
    }

    void WriteString(const std::string& value) {
        WriteU16(static_cast<std::uint16_t>(value.size()));
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
};

bool ReadPayload(Reader* reader, TagType type, Tag* outTag, std::string* errorText);
void WritePayload(Writer* writer, const Tag& tag);

bool ReadList(Reader* reader, Tag* outTag, std::string* errorText) {
    std::uint8_t rawListType = 0;
    std::int32_t count = 0;
    if (!reader->ReadU8(&rawListType) || !reader->ReadI32(&count)) {
        if (errorText) *errorText = "Unexpected EOF while reading NBT list header";
        return false;
    }
    if (count < 0) {
        if (errorText) *errorText = "Negative NBT list length";
        return false;
    }
    outTag->type = TagType::List;
    outTag->listType = static_cast<TagType>(rawListType);
    outTag->listValue.clear();
    outTag->listValue.reserve(static_cast<size_t>(count));
    for (std::int32_t i = 0; i < count; ++i) {
        Tag value;
        if (!ReadPayload(reader, outTag->listType, &value, errorText)) {
            return false;
        }
        outTag->listValue.push_back(std::move(value));
    }
    return true;
}

bool ReadCompound(Reader* reader, Tag* outTag, std::string* errorText) {
    outTag->type = TagType::Compound;
    outTag->compoundValue.clear();
    while (true) {
        std::uint8_t rawType = 0;
        if (!reader->ReadU8(&rawType)) {
            if (errorText) *errorText = "Unexpected EOF while reading NBT compound tag type";
            return false;
        }
        TagType type = static_cast<TagType>(rawType);
        if (type == TagType::End) {
            return true;
        }
        std::string name;
        if (!reader->ReadString(&name)) {
            if (errorText) *errorText = "Unexpected EOF while reading NBT compound key";
            return false;
        }
        Tag value;
        if (!ReadPayload(reader, type, &value, errorText)) {
            return false;
        }
        outTag->compoundValue.emplace(std::move(name), std::move(value));
    }
}

bool ReadPayload(Reader* reader, TagType type, Tag* outTag, std::string* errorText) {
    outTag->type = type;
    switch (type) {
    case TagType::Byte:
        return reader->ReadI8(&outTag->byteValue);
    case TagType::Short:
        return reader->ReadI16(&outTag->shortValue);
    case TagType::Int:
        return reader->ReadI32(&outTag->intValue);
    case TagType::Long:
        return reader->ReadI64(&outTag->longValue);
    case TagType::Float:
        return reader->ReadFloat(&outTag->floatValue);
    case TagType::Double:
        return reader->ReadDouble(&outTag->doubleValue);
    case TagType::String:
        return reader->ReadString(&outTag->stringValue);
    case TagType::ByteArray: {
        std::int32_t count = 0;
        if (!reader->ReadI32(&count) || count < 0 || !reader->CanRead(static_cast<size_t>(count))) {
            if (errorText) *errorText = "Invalid NBT byte array";
            return false;
        }
        outTag->byteArrayValue.assign(reader->bytes.begin() + static_cast<std::ptrdiff_t>(reader->offset),
            reader->bytes.begin() + static_cast<std::ptrdiff_t>(reader->offset + static_cast<size_t>(count)));
        reader->offset += static_cast<size_t>(count);
        return true;
    }
    case TagType::List:
        return ReadList(reader, outTag, errorText);
    case TagType::Compound:
        return ReadCompound(reader, outTag, errorText);
    case TagType::IntArray: {
        std::int32_t count = 0;
        if (!reader->ReadI32(&count) || count < 0) {
            if (errorText) *errorText = "Invalid NBT int array length";
            return false;
        }
        outTag->intArrayValue.clear();
        outTag->intArrayValue.reserve(static_cast<size_t>(count));
        for (std::int32_t i = 0; i < count; ++i) {
            std::int32_t value = 0;
            if (!reader->ReadI32(&value)) {
                if (errorText) *errorText = "Unexpected EOF while reading NBT int array";
                return false;
            }
            outTag->intArrayValue.push_back(value);
        }
        return true;
    }
    case TagType::LongArray: {
        std::int32_t count = 0;
        if (!reader->ReadI32(&count) || count < 0) {
            if (errorText) *errorText = "Invalid NBT long array length";
            return false;
        }
        outTag->longArrayValue.clear();
        outTag->longArrayValue.reserve(static_cast<size_t>(count));
        for (std::int32_t i = 0; i < count; ++i) {
            std::int64_t value = 0;
            if (!reader->ReadI64(&value)) {
                if (errorText) *errorText = "Unexpected EOF while reading NBT long array";
                return false;
            }
            outTag->longArrayValue.push_back(value);
        }
        return true;
    }
    default:
        if (errorText) *errorText = "Unsupported NBT tag type";
        return false;
    }
}

void WritePayload(Writer* writer, const Tag& tag) {
    switch (tag.type) {
    case TagType::Byte:
        writer->WriteI8(tag.byteValue);
        break;
    case TagType::Short:
        writer->WriteI16(tag.shortValue);
        break;
    case TagType::Int:
        writer->WriteI32(tag.intValue);
        break;
    case TagType::Long:
        writer->WriteI64(tag.longValue);
        break;
    case TagType::Float:
        writer->WriteFloat(tag.floatValue);
        break;
    case TagType::Double:
        writer->WriteDouble(tag.doubleValue);
        break;
    case TagType::String:
        writer->WriteString(tag.stringValue);
        break;
    case TagType::ByteArray:
        writer->WriteI32(static_cast<std::int32_t>(tag.byteArrayValue.size()));
        writer->bytes.insert(writer->bytes.end(), tag.byteArrayValue.begin(), tag.byteArrayValue.end());
        break;
    case TagType::List:
        writer->WriteU8(static_cast<std::uint8_t>(tag.listType));
        writer->WriteI32(static_cast<std::int32_t>(tag.listValue.size()));
        for (const auto& item : tag.listValue) {
            WritePayload(writer, item);
        }
        break;
    case TagType::Compound:
        for (const auto& [name, value] : tag.compoundValue) {
            writer->WriteU8(static_cast<std::uint8_t>(value.type));
            writer->WriteString(name);
            WritePayload(writer, value);
        }
        writer->WriteU8(static_cast<std::uint8_t>(TagType::End));
        break;
    case TagType::IntArray:
        writer->WriteI32(static_cast<std::int32_t>(tag.intArrayValue.size()));
        for (std::int32_t value : tag.intArrayValue) {
            writer->WriteI32(value);
        }
        break;
    case TagType::LongArray:
        writer->WriteI32(static_cast<std::int32_t>(tag.longArrayValue.size()));
        for (std::int64_t value : tag.longArrayValue) {
            writer->WriteI64(value);
        }
        break;
    case TagType::End:
        break;
    }
}

}  // namespace

Tag Tag::Byte(std::int8_t value) {
    Tag tag;
    tag.type = TagType::Byte;
    tag.byteValue = value;
    return tag;
}

Tag Tag::Short(std::int16_t value) {
    Tag tag;
    tag.type = TagType::Short;
    tag.shortValue = value;
    return tag;
}

Tag Tag::Int(std::int32_t value) {
    Tag tag;
    tag.type = TagType::Int;
    tag.intValue = value;
    return tag;
}

Tag Tag::Long(std::int64_t value) {
    Tag tag;
    tag.type = TagType::Long;
    tag.longValue = value;
    return tag;
}

Tag Tag::Float(float value) {
    Tag tag;
    tag.type = TagType::Float;
    tag.floatValue = value;
    return tag;
}

Tag Tag::Double(double value) {
    Tag tag;
    tag.type = TagType::Double;
    tag.doubleValue = value;
    return tag;
}

Tag Tag::String(std::string value) {
    Tag tag;
    tag.type = TagType::String;
    tag.stringValue = std::move(value);
    return tag;
}

Tag Tag::ByteArray(std::vector<std::uint8_t> value) {
    Tag tag;
    tag.type = TagType::ByteArray;
    tag.byteArrayValue = std::move(value);
    return tag;
}

Tag Tag::IntArray(std::vector<std::int32_t> value) {
    Tag tag;
    tag.type = TagType::IntArray;
    tag.intArrayValue = std::move(value);
    return tag;
}

Tag Tag::LongArray(std::vector<std::int64_t> value) {
    Tag tag;
    tag.type = TagType::LongArray;
    tag.longArrayValue = std::move(value);
    return tag;
}

Tag Tag::List(TagType listTypeValue, std::vector<Tag> value) {
    Tag tag;
    tag.type = TagType::List;
    tag.listType = listTypeValue;
    tag.listValue = std::move(value);
    return tag;
}

Tag Tag::Compound(std::map<std::string, Tag> value) {
    Tag tag;
    tag.type = TagType::Compound;
    tag.compoundValue = std::move(value);
    return tag;
}

Tag* Tag::Find(const std::string& key) {
    auto it = compoundValue.find(key);
    return it == compoundValue.end() ? nullptr : &it->second;
}

const Tag* Tag::Find(const std::string& key) const {
    auto it = compoundValue.find(key);
    return it == compoundValue.end() ? nullptr : &it->second;
}

bool ReadNamedTag(const std::vector<std::uint8_t>& bytes, NamedTag* outTag, std::string* errorText) {
    if (!outTag) {
        if (errorText) *errorText = "Output NBT pointer was null";
        return false;
    }
    Reader reader{bytes};
    std::uint8_t rawType = 0;
    if (!reader.ReadU8(&rawType)) {
        if (errorText) *errorText = "Unexpected EOF while reading root NBT type";
        return false;
    }
    TagType type = static_cast<TagType>(rawType);
    if (type == TagType::End) {
        if (errorText) *errorText = "Invalid root NBT type";
        return false;
    }
    if (!reader.ReadString(&outTag->name)) {
        if (errorText) *errorText = "Unexpected EOF while reading root NBT name";
        return false;
    }
    return ReadPayload(&reader, type, &outTag->root, errorText);
}

bool WriteNamedTag(const NamedTag& tag, std::vector<std::uint8_t>* outBytes, std::string* errorText) {
    if (!outBytes) {
        if (errorText) *errorText = "Output byte vector was null";
        return false;
    }
    Writer writer;
    writer.WriteU8(static_cast<std::uint8_t>(tag.root.type));
    writer.WriteString(tag.name);
    WritePayload(&writer, tag.root);
    *outBytes = std::move(writer.bytes);
    return true;
}

}  // namespace native_mc
