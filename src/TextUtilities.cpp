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

std::string PathToUtf8(const std::filesystem::path& path) {
    const std::u8string utf8_text = path.u8string();
    return std::string(utf8_text.begin(), utf8_text.end());
}

std::filesystem::path PathFromUtf8(std::string_view utf8_text) {
    return std::filesystem::path(std::u8string(utf8_text.begin(), utf8_text.end()));
}

}  // namespace filesearch::text
