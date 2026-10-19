#pragma once

#include <string>
#include <string_view>

#if defined(__linux__) && !defined(__ANDROID__)
#include <iconv.h>

namespace Core {
// VMD uses Windows CP932, whereas PMX and engine identifiers use UTF-8.
// Preserve the original bytes if the input cannot be converted.
inline std::string Cp932ToUtf8(std::string_view input) {
    if (input.empty()) return {};
    iconv_t converter = iconv_open("UTF-8", "CP932");
    if (converter == reinterpret_cast<iconv_t>(-1)) return std::string(input);
    std::string output(input.size() * 4, '\0');
    char* source = const_cast<char*>(input.data());
    char* destination = output.data();
    size_t sourceLeft = input.size(), destinationLeft = output.size();
    const auto result = iconv(converter, &source, &sourceLeft, &destination, &destinationLeft);
    iconv_close(converter);
    if (result == static_cast<size_t>(-1) || sourceLeft != 0) return std::string(input);
    output.resize(output.size() - destinationLeft);
    return output;
}
} // namespace Core
#endif
