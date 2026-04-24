#include "mia/engine.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace mia {

Engine::Engine(std::filesystem::path workspace_root) : workspace_root_(std::move(workspace_root)) {
  workspace_root_ = std::filesystem::weakly_canonical(workspace_root_);
  LoadConfig();
}

int Engine::api_port() const {
  return api_port_;
}

const std::filesystem::path& Engine::workspace_root() const {
  return workspace_root_;
}

CommandExecutionResult Engine::Execute(const std::string& input) {
  return ExecuteScript(input);
}

std::vector<MountedPartitionView> Engine::mounted_partitions() const {
  std::vector<MountedPartitionView> result;
  result.reserve(mounts_.size());
  for (const auto& mount : mounts_) {
    result.push_back({mount.id, DisplayPath(mount.disk_path), mount.name});
  }
  return result;
}

SessionView Engine::session() const {
  return {current_session_.active, current_session_.user, current_session_.group, current_session_.partition_id};
}

void Engine::LoadConfig() {
  const auto config_path = workspace_root_ / "mia.config.json";
  if (!std::filesystem::exists(config_path)) {
    return;
  }

  std::ifstream file(config_path);
  if (!file) {
    throw std::runtime_error("No se pudo leer mia.config.json");
  }

  const auto payload = nlohmann::json::parse(file);
  student_suffix_ = payload.value("studentIdLastTwoDigits", std::string("00"));
  api_port_ = payload.value("apiPort", 8080);

  if (student_suffix_.size() < 2) {
    student_suffix_.insert(student_suffix_.begin(), 2 - student_suffix_.size(), '0');
  }
  if (student_suffix_.size() > 2) {
    student_suffix_ = student_suffix_.substr(student_suffix_.size() - 2);
  }
}

CommandExecutionResult Engine::ExecuteScript(const std::string& input) {
  CommandExecutionResult result;
  const auto lines = SplitLines(input);
  for (std::size_t index = 0; index < lines.size(); ++index) {
    const auto parsed = ParseLine(lines[index], static_cast<int>(index + 1));
    if (parsed.is_blank) {
      continue;
    }
    if (parsed.is_comment) {
      result.output.push_back(parsed.raw);
      continue;
    }

    try {
      ExecuteCommand(parsed, result);
    } catch (const std::exception& exception) {
      std::ostringstream stream;
      stream << "[L" << parsed.line << "] Error: " << exception.what();
      result.output.push_back(stream.str());
    }
  }
  return result;
}

Engine::ParsedCommand Engine::ParseLine(const std::string& line, int line_number) const {
  ParsedCommand parsed;
  parsed.line = line_number;
  parsed.raw = line;

  const auto trimmed = Trim(line);
  if (trimmed.empty()) {
    parsed.is_blank = true;
    return parsed;
  }
  if (trimmed[0] == '#') {
    parsed.is_comment = true;
    parsed.raw = trimmed;
    return parsed;
  }

  const auto tokens = Tokenize(trimmed);
  if (tokens.empty()) {
    parsed.is_blank = true;
    return parsed;
  }

  parsed.name = ToLower(tokens.front());
  for (std::size_t index = 1; index < tokens.size(); ++index) {
    const auto& token = tokens[index];
    if (token.empty() || token[0] != '-') {
      continue;
    }

    const auto equals = token.find('=');
    if (equals == std::string::npos) {
      parsed.flags.push_back(ToLower(token.substr(1)));
      continue;
    }

    parsed.params[ToLower(token.substr(1, equals - 1))] = token.substr(equals + 1);
  }

  return parsed;
}

std::vector<std::string> Engine::Tokenize(const std::string& line) const {
  std::vector<std::string> tokens;
  std::string current;
  bool in_quotes = false;

  for (char character : line) {
    if (character == '"') {
      in_quotes = !in_quotes;
      continue;
    }
    if (!in_quotes && character == '#') {
      break;
    }
    if (!in_quotes && std::isspace(static_cast<unsigned char>(character))) {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
      continue;
    }
    current.push_back(character);
  }

  if (!current.empty()) {
    tokens.push_back(current);
  }
  return tokens;
}

void Engine::ExecuteCommand(const ParsedCommand& command, CommandExecutionResult& result) {
  if (command.name == "mkdisk") {
    HandleMkdisk(command, result);
  } else if (command.name == "rmdisk") {
    HandleRmdisk(command, result);
  } else if (command.name == "fdisk") {
    HandleFdisk(command, result);
  } else if (command.name == "mount") {
    HandleMount(command, result);
  } else if (command.name == "mounted") {
    HandleMounted(result);
  } else if (command.name == "mkfs") {
    HandleMkfs(command, result);
  } else if (command.name == "login") {
    HandleLogin(command, result);
  } else if (command.name == "logout") {
    HandleLogout(result);
  } else if (command.name == "mkgrp") {
    HandleMkgrp(command, result);
  } else if (command.name == "rmgrp") {
    HandleRmgrp(command, result);
  } else if (command.name == "mkusr") {
    HandleMkusr(command, result);
  } else if (command.name == "rmusr") {
    HandleRmusr(command, result);
  } else if (command.name == "chgrp") {
    HandleChgrp(command, result);
  } else if (command.name == "mkdir") {
    HandleMkdir(command, result);
  } else if (command.name == "mkfile") {
    HandleMkfile(command, result);
  } else if (command.name == "cat") {
    HandleCat(command, result);
  } else if (command.name == "rep") {
    HandleRep(command, result);
  } else if (command.name == "execute") {
    HandleExecute(command, result);
  } else if (command.name == "pause") {
    result.output.push_back("[INFO] pause ignorado en la API web.");
  } else {
    throw std::runtime_error("Comando no reconocido: " + command.name);
  }
}

std::filesystem::path Engine::ResolveWorkspacePath(const std::string& raw_path) const {
  if (raw_path.empty()) {
    throw std::runtime_error("Se esperaba una ruta");
  }

  const std::filesystem::path input_path(raw_path);
  if (input_path.is_absolute()) {
    return input_path.lexically_normal();
  }

  return (workspace_root_ / input_path).lexically_normal();
}

std::filesystem::path Engine::CanonicalForComparison(const std::filesystem::path& path) const {
  auto normalized = path.lexically_normal();
  if (std::filesystem::exists(normalized)) {
    return std::filesystem::weakly_canonical(normalized);
  }
  const auto parent = normalized.has_parent_path() ? normalized.parent_path() : workspace_root_;
  const auto canonical_parent = std::filesystem::weakly_canonical(parent);
  return (canonical_parent / normalized.filename()).lexically_normal();
}

std::string Engine::DisplayPath(const std::filesystem::path& path) const {
  const auto normalized = path.lexically_normal();
  if (normalized.empty()) {
    return ".";
  }
  return normalized.generic_string();
}

void Engine::HandleExecute(const ParsedCommand& command, CommandExecutionResult& result) {
  const auto path = ResolveWorkspacePath(GetParam(command, "path"));
  if (!std::filesystem::exists(path)) {
    throw std::runtime_error("El script indicado no existe");
  }

  std::ifstream file(path);
  std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  auto nested = ExecuteScript(content);
  result.output.insert(result.output.end(), nested.output.begin(), nested.output.end());
  result.artifacts.insert(result.artifacts.end(), nested.artifacts.begin(), nested.artifacts.end());
}

}  // namespace mia
