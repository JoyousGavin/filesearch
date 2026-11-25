#include "filesearch/TextUtilities.hpp"

#include <algorithm>
#include <cctype>

namespace filesearch::text {

std::string ToLowerAscii(std::string_view text) {
    std::string lowered_text(text);
    std::transform(lowered_text.begin(), lowered_text.end(), lowered_text.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });  //call std::tolower（经由上面的 lambda，逐字节调用）
    return lowered_text;
}

std::string_view TrimWhitespace(std::string_view text) {
    constexpr std::string_view kWhitespace = " \t\r\n\f\v";
    const std::size_t first = text.find_first_not_of(kWhitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(kWhitespace);
    return text.substr(first, last - first + 1);
}

std::vector<std::string_view> SplitString(std::string_view text, char delimiter) {
    std::vector<std::string_view> pieces;
    std::size_t piece_begin = 0;
    while (true) {
        const std::size_t delimiter_position = text.find(delimiter, piece_begin);
        if (delimiter_position == std::string_view::npos) {
            pieces.push_back(text.substr(piece_begin));
            return pieces;
        }
        pieces.push_back(text.substr(piece_begin, delimiter_position - piece_begin));
        piece_begin = delimiter_position + 1;
    }
}

std::wstring Utf8ToWide(std::string_view utf8_text) {
    return PathFromUtf8(utf8_text).wstring();
}

std::string PathToUtf8(const std::filesystem::path& path) {
    const std::u8string utf8_text = path.u8string();
    return std::string(utf8_text.begin(), utf8_text.end());
}

std::filesystem::path PathFromUtf8(std::string_view utf8_text) {
    return std::filesystem::path(std::u8string(utf8_text.begin(), utf8_text.end()));
}

}  // namespace filesearch::text
