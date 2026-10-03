#pragma once

#include <string>

namespace kairo {

// Makes untrusted diagnostic text safe for a local log. Prompts and tool
// output must still never be passed to this function for logging.
std::string SanitiseDiagnostic(std::string text);

}  // namespace kairo
