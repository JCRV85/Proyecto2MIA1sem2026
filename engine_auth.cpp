#include "mia/engine.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace mia {

void Engine::HandleLogin(const ParsedCommand& command, CommandExecutionResult& result) {
  if (current_session_.active) {
    throw std::runtime_error("Ya existe una sesión activa, usa logout primero");
  }

  const auto user = GetParam(command, "user");
  const auto password = GetParam(command, "pass");
  const auto id = GetParam(command, "id");

  const auto mount_it = std::find_if(mounts_.begin(), mounts_.end(), [&](const MountedPartition& mount) {
    return ToLower(mount.id) == ToLower(id);
  });
  if (mount_it == mounts_.end()) {
    throw std::runtime_error("El id indicado no está montado");
  }

  std::fstream disk(mount_it->disk_path, std::ios::binary | std::ios::in | std::ios::out);
  const auto sb = ReadSuperBlock(disk, mount_it->start);
  if (sb.s_magic != 0xEF53) {
    throw std::runtime_error("La partición no está formateada");
  }

  const auto users_content = ReadUsersFile(disk, sb);
  for (const auto& line : SplitLines(users_content)) {
    const auto parts = SplitCsv(line);
    if (parts.size() != 5 || ToUpper(parts[1]) != "U" || parts[0] == "0") {
      continue;
    }
    if (parts[3] == user) {
      if (parts[4] != password) {
        throw std::runtime_error("Autenticación fallida: contraseña incorrecta");
      }

      current_session_ = {
          true,
          user,
          parts[2],
          mount_it->id,
          std::stoi(parts[0]),
          GroupIdByName(parts[2], users_content),
      };
      result.output.push_back("[OK] Sesión iniciada como " + user + " en " + mount_it->id);
      return;
    }
  }

  throw std::runtime_error("El usuario no existe");
}

void Engine::HandleLogout(CommandExecutionResult& result) {
  if (!current_session_.active) {
    throw std::runtime_error("No existe una sesión activa");
  }
  current_session_ = {};
  result.output.push_back("[OK] Sesión finalizada.");
}

void Engine::HandleMkgrp(const ParsedCommand& command, CommandExecutionResult& result) {
  if (!current_session_.active) {
    throw std::runtime_error("Debes iniciar sesión");
  }
  if (current_session_.user != "root") {
    throw std::runtime_error("Solo root puede crear grupos");
  }

  const auto group = GetParam(command, "name");
  if (group.size() > 10) {
    throw std::runtime_error("El nombre del grupo no puede exceder 10 caracteres");
  }

  const auto mount_it = std::find_if(mounts_.begin(), mounts_.end(), [&](const MountedPartition& mount) {
    return mount.id == current_session_.partition_id;
  });
  std::fstream disk(mount_it->disk_path, std::ios::binary | std::ios::in | std::ios::out);
  auto sb = ReadSuperBlock(disk, mount_it->start);
  auto users = ReadUsersFile(disk, sb);

  int max_id = 1;
  for (const auto& line : SplitLines(users)) {
    const auto parts = SplitCsv(line);
    if (parts.size() != 3 || ToUpper(parts[1]) != "G") {
      continue;
    }
    max_id = std::max(max_id, std::stoi(parts[0]));
    if (parts[0] != "0" && parts[2] == group) {
      throw std::runtime_error("El grupo ya existe");
    }
  }

  users += std::to_string(max_id + 1) + ",G," + group + "\n";
  WriteUsersFile(disk, sb, users);
  WriteSuperBlock(disk, mount_it->start, sb);
  result.output.push_back("[OK] Grupo creado: " + group);
}

void Engine::HandleRmgrp(const ParsedCommand& command, CommandExecutionResult& result) {
  if (!current_session_.active) {
    throw std::runtime_error("Debes iniciar sesión");
  }
  if (current_session_.user != "root") {
    throw std::runtime_error("Solo root puede eliminar grupos");
  }

  const auto group = GetParam(command, "name");
  const auto mount_it = std::find_if(mounts_.begin(), mounts_.end(), [&](const MountedPartition& mount) {
    return mount.id == current_session_.partition_id;
  });
  std::fstream disk(mount_it->disk_path, std::ios::binary | std::ios::in | std::ios::out);
  auto sb = ReadSuperBlock(disk, mount_it->start);
  auto lines = SplitLines(ReadUsersFile(disk, sb));

  bool updated = false;
  for (auto& line : lines) {
    auto parts = SplitCsv(line);
    if (parts.size() == 3 && ToUpper(parts[1]) == "G" && parts[0] != "0" && parts[2] == group) {
      parts[0] = "0";
      line = Join(parts, ",");
      updated = true;
      break;
    }
  }
  if (!updated) {
    throw std::runtime_error("El grupo indicado no existe");
  }

  WriteUsersFile(disk, sb, Join(lines, "\n") + "\n");
  WriteSuperBlock(disk, mount_it->start, sb);
  result.output.push_back("[OK] Grupo eliminado: " + group);
}

void Engine::HandleMkusr(const ParsedCommand& command, CommandExecutionResult& result) {
  if (!current_session_.active) {
    throw std::runtime_error("Debes iniciar sesión");
  }
  if (current_session_.user != "root") {
    throw std::runtime_error("Solo root puede crear usuarios");
  }

  const auto user = GetParam(command, "user");
  const auto pass = GetParam(command, "pass");
  const auto group = GetParam(command, "grp");
  if (user.size() > 10 || pass.size() > 10 || group.size() > 10) {
    throw std::runtime_error("user, pass y grp admiten máximo 10 caracteres");
  }

  const auto mount_it = std::find_if(mounts_.begin(), mounts_.end(), [&](const MountedPartition& mount) {
    return mount.id == current_session_.partition_id;
  });
  std::fstream disk(mount_it->disk_path, std::ios::binary | std::ios::in | std::ios::out);
  auto sb = ReadSuperBlock(disk, mount_it->start);
  auto lines = SplitLines(ReadUsersFile(disk, sb));
  bool group_exists = false;
  int max_id = 1;

  for (const auto& line : lines) {
    const auto parts = SplitCsv(line);
    if (parts.empty()) {
      continue;
    }
    max_id = std::max(max_id, std::stoi(parts[0]));
    if (parts.size() == 3 && ToUpper(parts[1]) == "G" && parts[0] != "0" && parts[2] == group) {
      group_exists = true;
    }
    if (parts.size() == 5 && ToUpper(parts[1]) == "U" && parts[0] != "0" && parts[3] == user) {
      throw std::runtime_error("El usuario ya existe");
    }
  }

  if (!group_exists) {
    throw std::runtime_error("El grupo indicado no existe");
  }

  lines.push_back(std::to_string(max_id + 1) + ",U," + group + "," + user + "," + pass);
  WriteUsersFile(disk, sb, Join(lines, "\n") + "\n");
  WriteSuperBlock(disk, mount_it->start, sb);
  result.output.push_back("[OK] Usuario creado: " + user);
}

void Engine::HandleRmusr(const ParsedCommand& command, CommandExecutionResult& result) {
  if (!current_session_.active) {
    throw std::runtime_error("Debes iniciar sesión");
  }
  if (current_session_.user != "root") {
    throw std::runtime_error("Solo root puede eliminar usuarios");
  }

  const auto user = GetParam(command, "user");
  const auto mount_it = std::find_if(mounts_.begin(), mounts_.end(), [&](const MountedPartition& mount) {
    return mount.id == current_session_.partition_id;
  });
  std::fstream disk(mount_it->disk_path, std::ios::binary | std::ios::in | std::ios::out);
  auto sb = ReadSuperBlock(disk, mount_it->start);
  auto lines = SplitLines(ReadUsersFile(disk, sb));

  bool updated = false;
  for (auto& line : lines) {
    auto parts = SplitCsv(line);
    if (parts.size() == 5 && ToUpper(parts[1]) == "U" && parts[0] != "0" && parts[3] == user) {
      parts[0] = "0";
      line = Join(parts, ",");
      updated = true;
      break;
    }
  }
  if (!updated) {
    throw std::runtime_error("El usuario indicado no existe");
  }

  WriteUsersFile(disk, sb, Join(lines, "\n") + "\n");
  WriteSuperBlock(disk, mount_it->start, sb);
  result.output.push_back("[OK] Usuario eliminado: " + user);
}

void Engine::HandleChgrp(const ParsedCommand& command, CommandExecutionResult& result) {
  if (!current_session_.active) {
    throw std::runtime_error("Debes iniciar sesión");
  }
  if (current_session_.user != "root") {
    throw std::runtime_error("Solo root puede cambiar grupos");
  }

  const auto user = GetParam(command, "user");
  const auto group = GetParam(command, "grp");
  const auto mount_it = std::find_if(mounts_.begin(), mounts_.end(), [&](const MountedPartition& mount) {
    return mount.id == current_session_.partition_id;
  });
  std::fstream disk(mount_it->disk_path, std::ios::binary | std::ios::in | std::ios::out);
  auto sb = ReadSuperBlock(disk, mount_it->start);
  auto lines = SplitLines(ReadUsersFile(disk, sb));

  bool group_exists = false;
  bool updated = false;
  for (const auto& line : lines) {
    const auto parts = SplitCsv(line);
    if (parts.size() == 3 && ToUpper(parts[1]) == "G" && parts[0] != "0" && parts[2] == group) {
      group_exists = true;
      break;
    }
  }
  if (!group_exists) {
    throw std::runtime_error("El grupo indicado no existe");
  }

  for (auto& line : lines) {
    auto parts = SplitCsv(line);
    if (parts.size() == 5 && ToUpper(parts[1]) == "U" && parts[0] != "0" && parts[3] == user) {
      parts[2] = group;
      line = Join(parts, ",");
      updated = true;
      break;
    }
  }
  if (!updated) {
    throw std::runtime_error("El usuario indicado no existe");
  }

  WriteUsersFile(disk, sb, Join(lines, "\n") + "\n");
  WriteSuperBlock(disk, mount_it->start, sb);
  if (current_session_.user == user) {
    current_session_.group = group;
    current_session_.gid = GroupIdByName(group, Join(lines, "\n"));
  }
  result.output.push_back("[OK] Usuario " + user + " movido al grupo " + group);
}

}  // namespace mia
