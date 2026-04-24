#include "mia/engine.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace mia {

int Engine::ResolvePathToInode(std::fstream& disk, const SuperBlock& super_block, const std::string& path) const {
  const auto parts = SplitPath(path);
  int current = 0;
  for (const auto& part : parts) {
    const auto inode = ReadInode(disk, super_block, current);
    if (inode.i_type != '0') {
      throw std::runtime_error("La ruta no corresponde a una carpeta válida: " + path);
    }
    current = FindDirectoryEntry(disk, super_block, inode, part);
    if (current == -1) {
      throw std::runtime_error("No existe la ruta: " + path);
    }
  }
  return current;
}

int Engine::FindDirectoryEntry(std::fstream& disk, const SuperBlock& super_block, const Inode& directory, const std::string& name) const {
  for (int pointer : directory.i_block) {
    if (pointer == -1) {
      continue;
    }
    const auto block = ReadFolderBlock(disk, super_block, pointer);
    for (const auto& entry : block.b_content) {
      if (entry.b_inodo != -1 && ReadName(entry.b_name, sizeof(entry.b_name)) == name) {
        return entry.b_inodo;
      }
    }
  }
  return -1;
}

void Engine::AddDirectoryEntry(std::fstream& disk, SuperBlock& super_block, int parent_inode_index, Inode& parent_inode, const std::string& name, int child_inode_index) const {
  for (int slot = 0; slot < 12; ++slot) {
    if (parent_inode.i_block[slot] == -1) {
      const int new_block_index = AllocateBlock(disk, super_block);
      FolderBlock new_block{};
      for (auto& entry : new_block.b_content) {
        entry.b_inodo = -1;
        std::memset(entry.b_name, 0, sizeof(entry.b_name));
      }
      parent_inode.i_block[slot] = new_block_index;
      WriteFolderBlock(disk, super_block, new_block_index, new_block);
    }

    auto block = ReadFolderBlock(disk, super_block, parent_inode.i_block[slot]);
    for (auto& entry : block.b_content) {
      if (entry.b_inodo == -1) {
        CopyName(entry.b_name, sizeof(entry.b_name), name);
        entry.b_inodo = child_inode_index;
        WriteFolderBlock(disk, super_block, parent_inode.i_block[slot], block);
        parent_inode.i_mtime = CurrentTimestamp();
        WriteInode(disk, super_block, parent_inode_index, parent_inode);
        return;
      }
    }
  }

  throw std::runtime_error("La carpeta ya no tiene espacio en apuntadores directos");
}

int Engine::EnsureDirectory(std::fstream& disk, SuperBlock& super_block, const std::vector<std::string>& parts, bool recursive, int owner_uid, int owner_gid) const {
  int current_inode_index = 0;
  const bool is_root = current_session_.user == "root";

  for (std::size_t index = 0; index < parts.size(); ++index) {
    auto current_inode = ReadInode(disk, super_block, current_inode_index);
    int existing = FindDirectoryEntry(disk, super_block, current_inode, parts[index]);
    if (existing != -1) {
      const auto inode = ReadInode(disk, super_block, existing);
      if (inode.i_type != '0') {
        throw std::runtime_error("El componente " + parts[index] + " ya existe y no es carpeta");
      }
      current_inode_index = existing;
      continue;
    }

    if (index + 1 != parts.size() && !recursive) {
      throw std::runtime_error("Falta una carpeta padre y no se usó el parámetro recursivo");
    }
    if (!HasPermission(current_inode, 2, owner_uid, owner_gid, is_root)) {
      throw std::runtime_error("No tienes permiso de escritura sobre la carpeta padre");
    }

    const int new_inode_index = AllocateInode(disk, super_block);
    const int new_block_index = AllocateBlock(disk, super_block);

    FolderBlock folder_block{};
    for (auto& entry : folder_block.b_content) {
      entry.b_inodo = -1;
      std::memset(entry.b_name, 0, sizeof(entry.b_name));
    }
    CopyName(folder_block.b_content[0].b_name, sizeof(folder_block.b_content[0].b_name), ".");
    folder_block.b_content[0].b_inodo = new_inode_index;
    CopyName(folder_block.b_content[1].b_name, sizeof(folder_block.b_content[1].b_name), "..");
    folder_block.b_content[1].b_inodo = current_inode_index;
    WriteFolderBlock(disk, super_block, new_block_index, folder_block);

    Inode new_inode{};
    new_inode.i_uid = owner_uid;
    new_inode.i_gid = owner_gid;
    new_inode.i_atime = CurrentTimestamp();
    new_inode.i_ctime = new_inode.i_atime;
    new_inode.i_mtime = new_inode.i_atime;
    std::fill(std::begin(new_inode.i_block), std::end(new_inode.i_block), -1);
    new_inode.i_block[0] = new_block_index;
    new_inode.i_type = '0';
    CopyName(new_inode.i_perm, sizeof(new_inode.i_perm), "664");
    WriteInode(disk, super_block, new_inode_index, new_inode);

    AddDirectoryEntry(disk, super_block, current_inode_index, current_inode, parts[index], new_inode_index);
    current_inode_index = new_inode_index;
  }

  return current_inode_index;
}

std::string Engine::ReadFileContent(std::fstream& disk, const SuperBlock& super_block, int inode_index) const {
  const auto inode = ReadInode(disk, super_block, inode_index);
  std::string content;
  content.reserve(inode.i_s);
  int remaining = inode.i_s;
  for (int pointer : inode.i_block) {
    if (pointer == -1 || remaining <= 0) {
      continue;
    }
    const auto block = ReadFileBlock(disk, super_block, pointer);
    const int chunk = std::min<int>(remaining, sizeof(block.b_content));
    content.append(block.b_content, block.b_content + chunk);
    remaining -= chunk;
  }
  return content;
}

void Engine::WriteFileContent(std::fstream& disk, SuperBlock& super_block, int, Inode& inode, const std::string& content) const {
  constexpr std::size_t kBlockSize = 64;
  const int required_blocks = static_cast<int>((content.size() + kBlockSize - 1) / kBlockSize);
  if (required_blocks > 12) {
    throw std::runtime_error("El contenido excede la capacidad soportada por esta entrega (12 bloques directos)");
  }

  std::vector<int> current_blocks;
  for (int pointer : inode.i_block) {
    if (pointer != -1) {
      current_blocks.push_back(pointer);
    }
  }

  for (std::size_t index = required_blocks; index < current_blocks.size(); ++index) {
    FreeBlock(disk, super_block, current_blocks[index]);
  }
  for (int index = 0; index < 12; ++index) {
    if (index >= required_blocks) {
      inode.i_block[index] = -1;
    }
  }
  for (int index = static_cast<int>(current_blocks.size()); index < required_blocks; ++index) {
    inode.i_block[index] = AllocateBlock(disk, super_block);
  }

  for (int index = 0; index < required_blocks; ++index) {
    FileBlock block{};
    std::memset(block.b_content, 0, sizeof(block.b_content));
    const std::size_t start = static_cast<std::size_t>(index) * kBlockSize;
    const std::size_t chunk = std::min<std::size_t>(kBlockSize, content.size() - start);
    if (chunk > 0) {
      std::memcpy(block.b_content, content.data() + start, chunk);
    }
    WriteFileBlock(disk, super_block, inode.i_block[index], block);
  }

  inode.i_s = static_cast<int>(content.size());
  inode.i_mtime = CurrentTimestamp();
  inode.i_atime = inode.i_mtime;
}

bool Engine::HasPermission(const Inode& inode, int read_mask, int uid, int gid, bool is_root) const {
  if (is_root) {
    return true;
  }
  const auto perms = ReadName(inode.i_perm, sizeof(inode.i_perm));
  if (perms.size() < 3) {
    return false;
  }

  int index = 2;
  if (uid == inode.i_uid) {
    index = 0;
  } else if (gid == inode.i_gid) {
    index = 1;
  }
  const int digit = perms[index] - '0';
  return (digit & read_mask) == read_mask;
}

std::string Engine::ReadUsersFile(std::fstream& disk, const SuperBlock& super_block) const {
  return ReadFileContent(disk, super_block, 1);
}

void Engine::WriteUsersFile(std::fstream& disk, SuperBlock& super_block, const std::string& content) const {
  auto inode = ReadInode(disk, super_block, 1);
  WriteFileContent(disk, super_block, 1, inode, content);
  WriteInode(disk, super_block, 1, inode);
}

int Engine::GroupIdByName(const std::string& group_name, const std::string& users_content) const {
  for (const auto& line : SplitLines(users_content)) {
    const auto parts = SplitCsv(line);
    if (parts.size() == 3 && ToUpper(parts[1]) == "G" && parts[0] != "0" && parts[2] == group_name) {
      return std::stoi(parts[0]);
    }
  }
  return 0;
}

void Engine::HandleMkdir(const ParsedCommand& command, CommandExecutionResult& result) {
  if (!current_session_.active) {
    throw std::runtime_error("Debes iniciar sesión");
  }

  const auto parts = SplitPath(GetParam(command, "path"));
  const bool recursive = HasFlag(command, "p");
  if (parts.empty()) {
    result.output.push_back("[OK] La raíz ya existe.");
    return;
  }

  const auto mount_it = std::find_if(mounts_.begin(), mounts_.end(), [&](const MountedPartition& mount) {
    return mount.id == current_session_.partition_id;
  });
  std::fstream disk(mount_it->disk_path, std::ios::binary | std::ios::in | std::ios::out);
  auto sb = ReadSuperBlock(disk, mount_it->start);

  const int created_inode = EnsureDirectory(disk, sb, parts, recursive, current_session_.uid, current_session_.gid);
  (void)created_inode;
  WriteSuperBlock(disk, mount_it->start, sb);
  result.output.push_back("[OK] Carpeta creada: " + GetParam(command, "path"));
}

void Engine::HandleMkfile(const ParsedCommand& command, CommandExecutionResult& result) {
  if (!current_session_.active) {
    throw std::runtime_error("Debes iniciar sesión");
  }

  const auto path = GetParam(command, "path");
  const bool recursive = HasFlag(command, "r");
  std::string content;
  if (command.params.contains("cont")) {
    const auto source_path = ResolveWorkspacePath(command.params.at("cont"));
    if (!std::filesystem::exists(source_path)) {
      throw std::runtime_error("El archivo indicado en cont no existe");
    }
    std::ifstream file(source_path, std::ios::binary);
    content.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  } else {
    int size = 0;
    if (command.params.contains("size")) {
      size = std::stoi(command.params.at("size"));
      if (size < 0) {
        throw std::runtime_error("size no puede ser negativo");
      }
    }
    content.reserve(size);
    for (int index = 0; index < size; ++index) {
      content.push_back(static_cast<char>('0' + (index % 10)));
    }
  }

  const auto parts = SplitPath(path);
  if (parts.empty()) {
    throw std::runtime_error("La ruta del archivo es inválida");
  }
  const std::string file_name = parts.back();
  if (file_name.size() > 11) {
    throw std::runtime_error("El nombre del archivo no puede exceder 11 caracteres");
  }

  std::vector<std::string> parent_parts(parts.begin(), parts.end() - 1);
  const auto mount_it = std::find_if(mounts_.begin(), mounts_.end(), [&](const MountedPartition& mount) {
    return mount.id == current_session_.partition_id;
  });
  std::fstream disk(mount_it->disk_path, std::ios::binary | std::ios::in | std::ios::out);
  auto sb = ReadSuperBlock(disk, mount_it->start);
  const int parent_index = EnsureDirectory(disk, sb, parent_parts, recursive, current_session_.uid, current_session_.gid);
  auto parent_inode = ReadInode(disk, sb, parent_index);
  if (!HasPermission(parent_inode, 2, current_session_.uid, current_session_.gid, current_session_.user == "root")) {
    throw std::runtime_error("No tienes permiso de escritura en la carpeta padre");
  }

  int file_inode_index = FindDirectoryEntry(disk, sb, parent_inode, file_name);
  Inode file_inode{};
  if (file_inode_index == -1) {
    file_inode_index = AllocateInode(disk, sb);
    file_inode.i_uid = current_session_.uid;
    file_inode.i_gid = current_session_.gid;
    file_inode.i_atime = CurrentTimestamp();
    file_inode.i_ctime = file_inode.i_atime;
    file_inode.i_mtime = file_inode.i_atime;
    std::fill(std::begin(file_inode.i_block), std::end(file_inode.i_block), -1);
    file_inode.i_type = '1';
    CopyName(file_inode.i_perm, sizeof(file_inode.i_perm), "664");
    AddDirectoryEntry(disk, sb, parent_index, parent_inode, file_name, file_inode_index);
    WriteInode(disk, sb, parent_index, parent_inode);
  } else {
    file_inode = ReadInode(disk, sb, file_inode_index);
    if (file_inode.i_type != '1') {
      throw std::runtime_error("La ruta ya existe y no es un archivo");
    }
  }

  WriteFileContent(disk, sb, file_inode_index, file_inode, content);
  WriteInode(disk, sb, file_inode_index, file_inode);
  WriteSuperBlock(disk, mount_it->start, sb);
  result.output.push_back("[OK] Archivo creado/actualizado: " + path);
}

void Engine::HandleCat(const ParsedCommand& command, CommandExecutionResult& result) {
  if (!current_session_.active) {
    throw std::runtime_error("Debes iniciar sesión");
  }

  std::vector<std::pair<int, std::string>> files;
  for (const auto& [key, value] : command.params) {
    if (key.rfind("file", 0) == 0) {
      files.push_back({std::stoi(key.substr(4)), value});
    }
  }
  if (files.empty()) {
    throw std::runtime_error("cat requiere al menos un parámetro fileN");
  }
  std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
    return left.first < right.first;
  });

  const auto mount_it = std::find_if(mounts_.begin(), mounts_.end(), [&](const MountedPartition& mount) {
    return mount.id == current_session_.partition_id;
  });
  std::fstream disk(mount_it->disk_path, std::ios::binary | std::ios::in | std::ios::out);
  const auto sb = ReadSuperBlock(disk, mount_it->start);
  std::vector<std::string> chunks;

  for (const auto& [_, file_path] : files) {
    const int inode_index = ResolvePathToInode(disk, sb, file_path);
    const auto inode = ReadInode(disk, sb, inode_index);
    if (inode.i_type != '1') {
      throw std::runtime_error("cat solo puede leer archivos");
    }
    if (!HasPermission(inode, 4, current_session_.uid, current_session_.gid, current_session_.user == "root")) {
      throw std::runtime_error("No tienes permiso de lectura sobre " + file_path);
    }
    chunks.push_back(ReadFileContent(disk, sb, inode_index));
  }

  result.output.push_back(Join(chunks, "\n"));
}

}  // namespace mia
