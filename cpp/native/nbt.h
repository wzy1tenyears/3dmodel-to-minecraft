#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace native_mc {

enum class TagType : std::uint8_t {
    End = 0,
    Byte = 1,
    Short = 2,
    Int = 3,
    Long = 4,
    Float = 5,
    Double = 6,
    ByteArray = 7,
    String = 8,
    List = 9,
    Compound = 10,
    IntArray = 11,
    LongArray = 12,
};

struct Tag {
    TagType type = TagType::End;
    std::int8_t byteValue = 0;
    std::int16_t shortValue = 0;
    std::int32_t intValue = 0;
    std::int64_t longValue = 0;
    float floatValue = 0.0f;
    double doubleValue = 0.0;
    std::string stringValue;
    std::vector<std::uint8_t> byteArrayValue;
    std::vector<std::int32_t> intArrayValue;
    std::vector<std::int64_t> longArrayValue;
    TagType listType = TagType::End;
    std::vector<Tag> listValue;
    std::map<std::string, Tag> compoundValue;

    static Tag Byte(std::int8_t value);
    static Tag Short(std::int16_t value);
    static Tag Int(std::int32_t value);
    static Tag Long(std::int64_t value);
    static Tag Float(float value);
    static Tag Double(double value);
    static Tag String(std::string value);
    static Tag ByteArray(std::vector<std::uint8_t> value);
    static Tag IntArray(std::vector<std::int32_t> value);
    static Tag LongArray(std::vector<std::int64_t> value);
    static Tag List(TagType listType, std::vector<Tag> value);
    static Tag Compound(std::map<std::string, Tag> value = {});

    Tag* Find(const std::string& key);
    const Tag* Find(const std::string& key) const;
};

struct NamedTag {
    std::string name;
    Tag root;
};

bool ReadNamedTag(const std::vector<std::uint8_t>& bytes, NamedTag* outTag, std::string* errorText);
bool WriteNamedTag(const NamedTag& tag, std::vector<std::uint8_t>* outBytes, std::string* errorText);

}  // namespace native_mc
