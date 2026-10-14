#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace klip {

// Build a safe, stable folder/prefix name for a previously unknown fullscreen app.
// We keep profile names ASCII, lowercase, bounded, and valid as a Windows path segment.
inline std::string MakeApplicationProfileSlug(std::string_view executable_name) {
  const auto separator = executable_name.find_last_of("/\\\\");
  if (separator != std::string_view::npos) executable_name.remove_prefix(separator + 1);
  const auto extension = executable_name.find_last_of('.');
  if (extension != std::string_view::npos && extension > 0)
    executable_name = executable_name.substr(0, extension);

  std::string slug;
  slug.reserve(executable_name.size());
  bool previous_separator = false;
  for (const unsigned char ch : executable_name) {
    if (std::isalnum(ch) && ch < 0x80) {
      slug.push_back(static_cast<char>(std::tolower(ch)));
      previous_separator = false;
    } else if (!slug.empty() && !previous_separator) {
      slug.push_back('-');
      previous_separator = true;
    }
    if (slug.size() >= 64) break;
  }
  while (!slug.empty() && slug.back() == '-') slug.pop_back();
  if (slug.empty()) slug = "application";

  // Windows reserves these names even when they are used as directory names.
  if (slug == "con" || slug == "prn" || slug == "aux" || slug == "nul" ||
      slug == "com1" || slug == "com2" || slug == "com3" || slug == "com4" ||
      slug == "com5" || slug == "com6" || slug == "com7" || slug == "com8" ||
      slug == "com9" || slug == "lpt1" || slug == "lpt2" || slug == "lpt3" ||
      slug == "lpt4" || slug == "lpt5" || slug == "lpt6" || slug == "lpt7" ||
      slug == "lpt8" || slug == "lpt9")
    slug.insert(0, "app-");
  return slug;
}

}  // namespace klip
