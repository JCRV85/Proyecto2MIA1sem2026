#include "mia/engine.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace mia {

std::string Engine::Trim(const std::string& value) {
  const auto start = value.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) {
    return "";
  }
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(start, end - start + 1);
}

std::string Engine::ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return value;
}

std::string Engine::ToUpper(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
    return static_cast<char>(std::toupper(character));
  });
  return value;
}

std::vector<std::string> Engine::SplitPath(const std::string& path) {
  std::vector<std::string> parts;
  std::string current;
  for (char character : path) {
    if (character == '/' || character == '\\') {
      if (!current.empty()) {
        parts.push_back(current);
        current.clear();
      }
    } else {
      current.push_back(character);
    }
  }
  if (!current.empty()) {
    parts.push_back(current);
  }
  return parts;
}

std::vector<std::string> Engine::SplitLines(const std::string& text) {
  std::vector<std::string> lines;
  std::stringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lines.push_back(line);
  }
  if (lines.empty() && !text.empty()) {
    lines.push_back(text);
  }
  return lines;
}

std::vector<std::string> Engine::SplitCsv(const std::string& line) {
  std::vector<std::string> parts;
  std::stringstream stream(line);
  std::string current;
  while (std::getline(stream, current, ',')) {
    parts.push_back(Trim(current));
  }
  return parts;
}

std::string Engine::Join(const std::vector<std::string>& lines, const std::string& separator) {
  std::ostringstream stream;
  for (std::size_t index = 0; index < lines.size(); ++index) {
    stream << lines[index];
    if (index + 1 != lines.size()) {
      stream << separator;
    }
  }
  return stream.str();
}

std::int64_t Engine::CurrentTimestamp() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string Engine::FormatTimestamp(std::int64_t timestamp) {
  if (timestamp <= 0) {
    return "-";
  }

  std::time_t time = static_cast<std::time_t>(timestamp);
  std::tm tm_value{};
#ifdef _WIN32
  localtime_s(&tm_value, &time);
#else
  localtime_r(&time, &tm_value);
#endif
  std::ostringstream stream;
  stream << std::put_time(&tm_value, "%Y-%m-%d %H:%M:%S");
  return stream.str();
}

std::string Engine::GetParam(const ParsedCommand& command, const std::string& key) {
  const auto it = command.params.find(ToLower(key));
  if (it == command.params.end() || it->second.empty()) {
    throw std::runtime_error("Falta el parámetro -" + key);
  }
  return it->second;
}

bool Engine::HasFlag(const ParsedCommand& command, const std::string& flag) {
  const auto lowered = ToLower(flag);
  return std::find(command.flags.begin(), command.flags.end(), lowered) != command.flags.end();
}

int Engine::ParsePositiveInt(const std::string& value, const std::string& field_name) {
  const int parsed = std::stoi(value);
  if (parsed <= 0) {
    throw std::runtime_error("El parámetro " + field_name + " debe ser positivo");
  }
  return parsed;
}

std::string Engine::EscapeDot(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (char character : value) {
    switch (character) {
      case '<':
        escaped += "&lt;";
        break;
      case '>':
        escaped += "&gt;";
        break;
      case '&':
        escaped += "&amp;";
        break;
      case '"':
        escaped += "\\\"";
        break;
      case '\n':
        escaped += "\\n";
        break;
      default:
        escaped.push_back(character);
        break;
    }
  }
  return escaped;
}

void Engine::CopyName(char* target, std::size_t size, const std::string& value) {
  std::memset(target, 0, size);
  if (size == 0) {
    return;
  }
  std::memcpy(target, value.c_str(), std::min(size - 1, value.size()));
}

std::string Engine::ReadName(const char* value, std::size_t size) {
  std::size_t length = 0;
  while (length < size && value[length] != '\0') {
    ++length;
  }
  return std::string(value, length);
}

}  // namespace mia
