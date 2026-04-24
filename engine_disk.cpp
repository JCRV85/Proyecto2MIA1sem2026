#include "mia/engine.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <random>
#include <stdexcept>

namespace mia {

void Engine::HandleMkdisk(const ParsedCommand& command, CommandExecutionResult& result) {
  const int size = ParsePositiveInt(GetParam(command, "size"), "size");
  const auto path = ResolveWorkspacePath(GetParam(command, "path"));
  const auto unit = ToUpper(command.params.contains("unit") ? command.params.at("unit") : "M");
  const auto fit = ToUpper(command.params.contains("fit") ? command.params.at("fit") : "FF");

  int multiplier = 0;
  if (unit == "K") {
    multiplier = 1024;
  } else if (unit == "M") {
    multiplier = 1024 * 1024;
  } else {
    throw std::runtime_error("unit inválido para mkdisk");
  }

  char fit_value = 'F';
  if (fit == "BF") {
    fit_value = 'B';
  } else if (fit == "WF") {
    fit_value = 'W';
  } else if (fit != "FF") {
    throw std::runtime_error("fit inválido para mkdisk");
  }

  std::filesystem::create_directories(path.parent_path());
  const std::int64_t total_bytes = static_cast<std::int64_t>(size) * multiplier;

  std::ofstream disk(path, std::ios::binary | std::ios::trunc);
  std::array<char, 1024> buffer{};
  for (std::int64_t written = 0; written < total_bytes; written += buffer.size()) {
    disk.write(buffer.data(), static_cast<std::streamsize>(std::min<std::int64_t>(buffer.size(), total_bytes - written)));
  }
  disk.close();

  MBR mbr{};
  mbr.mbr_tamano = static_cast<std::int32_t>(total_bytes);
  mbr.mbr_fecha_creacion = CurrentTimestamp();
  mbr.dsk_fit = fit_value;
  std::mt19937 generator(static_cast<unsigned int>(std::chrono::steady_clock::now().time_since_epoch().count()));
  mbr.mbr_dsk_signature = static_cast<std::int32_t>(generator());
  for (auto& partition : mbr.mbr_partitions) {
    partition.part_status = '0';
    partition.part_type = 'P';
    partition.part_fit = fit_value;
    partition.part_start = -1;
    partition.part_s = 0;
    partition.part_correlative = -1;
    std::memset(partition.part_name, 0, sizeof(partition.part_name));
    std::memset(partition.part_id, 0, sizeof(partition.part_id));
  }
  WriteMbr(path, mbr);

  result.output.push_back("[OK] Disco creado en " + DisplayPath(path));
}

void Engine::HandleRmdisk(const ParsedCommand& command, CommandExecutionResult& result) {
  const auto path = ResolveWorkspacePath(GetParam(command, "path"));
  if (!std::filesystem::exists(path)) {
    throw std::runtime_error("El disco no existe");
  }

  std::filesystem::remove(path);
  mounts_.erase(
      std::remove_if(mounts_.begin(), mounts_.end(), [&](const MountedPartition& mount) {
        return CanonicalForComparison(mount.disk_path) == CanonicalForComparison(path);
      }),
      mounts_.end());
  result.output.push_back("[OK] Disco eliminado: " + DisplayPath(path));
}

void Engine::HandleFdisk(const ParsedCommand& command, CommandExecutionResult& result) {
  const int size = ParsePositiveInt(GetParam(command, "size"), "size");
  const auto disk_path = ResolveWorkspacePath(GetParam(command, "path"));
  const auto name = GetParam(command, "name");
  const auto unit = ToUpper(command.params.contains("unit") ? command.params.at("unit") : "K");
  const auto type = ToUpper(command.params.contains("type") ? command.params.at("type") : "P");
  const auto fit = ToUpper(command.params.contains("fit") ? command.params.at("fit") : "WF");

  if (!std::filesystem::exists(disk_path)) {
    throw std::runtime_error("El disco indicado no existe");
  }
  if (name.size() > 15) {
    throw std::runtime_error("El nombre de la partición no puede exceder 15 caracteres");
  }

  int multiplier = 0;
  if (unit == "B") {
    multiplier = 1;
  } else if (unit == "K") {
    multiplier = 1024;
  } else if (unit == "M") {
    multiplier = 1024 * 1024;
  } else {
    throw std::runtime_error("unit inválido para fdisk");
  }

  char fit_value = 'W';
  if (fit == "BF") {
    fit_value = 'B';
  } else if (fit == "FF") {
    fit_value = 'F';
  } else if (fit != "WF") {
    throw std::runtime_error("fit inválido para fdisk");
  }

  const int size_bytes = size * multiplier;
  auto mbr = ReadMbr(disk_path);
  if (FindPartitionByName(disk_path, name).start != 0) {
    throw std::runtime_error("Ya existe una partición con ese nombre");
  }

  if (type == "L") {
    int extended_index = -1;
    PartitionEntry extended{};
    for (int index = 0; index < 4; ++index) {
      if (mbr.mbr_partitions[index].part_s > 0 &&
          std::toupper(static_cast<unsigned char>(mbr.mbr_partitions[index].part_type)) == 'E') {
        extended_index = index;
        extended = mbr.mbr_partitions[index];
        break;
      }
    }
    if (extended_index == -1) {
      throw std::runtime_error("No existe una partición extendida para crear la lógica");
    }

    std::fstream disk(disk_path, std::ios::binary | std::ios::in | std::ios::out);
    const int extended_end = extended.part_start + extended.part_s;
    auto ebr = ReadEbr(disk, extended.part_start);
    if (ebr.part_s == 0 && ReadName(ebr.part_name, sizeof(ebr.part_name)).empty()) {
      if (extended.part_s < static_cast<int>(sizeof(EBR)) + size_bytes) {
        throw std::runtime_error("No hay espacio suficiente en la partición extendida");
      }
      ebr.part_mount = '0';
      ebr.part_fit = fit_value;
      ebr.part_start = extended.part_start + static_cast<int>(sizeof(EBR));
      ebr.part_s = size_bytes;
      ebr.part_next = -1;
      CopyName(ebr.part_name, sizeof(ebr.part_name), name);
      WriteEbr(disk, extended.part_start, ebr);
      result.output.push_back("[OK] Partición lógica creada: " + name);
      return;
    }

    int current_position = extended.part_start;
    while (ebr.part_next != -1) {
      current_position = ebr.part_next;
      ebr = ReadEbr(disk, current_position);
      if (ReadName(ebr.part_name, sizeof(ebr.part_name)) == name) {
        throw std::runtime_error("Ya existe una partición con ese nombre");
      }
    }

    const int new_ebr_position = ebr.part_start + ebr.part_s;
    const int new_data_start = new_ebr_position + static_cast<int>(sizeof(EBR));
    if (new_data_start + size_bytes > extended_end) {
      throw std::runtime_error("No hay espacio suficiente para la partición lógica");
    }

    ebr.part_next = new_ebr_position;
    WriteEbr(disk, current_position, ebr);

    EBR created{};
    created.part_mount = '0';
    created.part_fit = fit_value;
    created.part_start = new_data_start;
    created.part_s = size_bytes;
    created.part_next = -1;
    CopyName(created.part_name, sizeof(created.part_name), name);
    WriteEbr(disk, new_ebr_position, created);
    result.output.push_back("[OK] Partición lógica creada: " + name);
    return;
  }

  if (type != "P" && type != "E") {
    throw std::runtime_error("type inválido para fdisk");
  }

  int empty_slot = -1;
  bool extended_exists = false;
  std::vector<PartitionEntry> active;
  for (int index = 0; index < 4; ++index) {
    const auto& partition = mbr.mbr_partitions[index];
    if (partition.part_s <= 0) {
      if (empty_slot == -1) {
        empty_slot = index;
      }
      continue;
    }
    if (std::toupper(static_cast<unsigned char>(partition.part_type)) == 'E') {
      extended_exists = true;
    }
    active.push_back(partition);
  }
  if (empty_slot == -1) {
    throw std::runtime_error("No hay entradas libres en el MBR");
  }
  if (type == "E" && extended_exists) {
    throw std::runtime_error("Solo puede existir una partición extendida");
  }

  std::sort(active.begin(), active.end(), [](const PartitionEntry& left, const PartitionEntry& right) {
    return left.part_start < right.part_start;
  });

  struct Gap {
    int start;
    int size;
  };
  std::vector<Gap> gaps;
  int cursor = static_cast<int>(sizeof(MBR));
  for (const auto& partition : active) {
    if (partition.part_start > cursor) {
      gaps.push_back({cursor, partition.part_start - cursor});
    }
    cursor = partition.part_start + partition.part_s;
  }
  if (cursor < mbr.mbr_tamano) {
    gaps.push_back({cursor, mbr.mbr_tamano - cursor});
  }

  std::optional<Gap> chosen;
  for (const auto& gap : gaps) {
    if (gap.size < size_bytes) {
      continue;
    }
    if (fit_value == 'F' && !chosen) {
      chosen = gap;
      break;
    }
    if (fit_value == 'B' && (!chosen || gap.size < chosen->size)) {
      chosen = gap;
    }
    if (fit_value == 'W' && (!chosen || gap.size > chosen->size)) {
      chosen = gap;
    }
  }
  if (!chosen) {
    throw std::runtime_error("No existe espacio suficiente para la partición");
  }

  auto& partition = mbr.mbr_partitions[empty_slot];
  partition.part_status = '0';
  partition.part_type = type[0];
  partition.part_fit = fit_value;
  partition.part_start = chosen->start;
  partition.part_s = size_bytes;
  partition.part_correlative = -1;
  CopyName(partition.part_name, sizeof(partition.part_name), name);
  std::memset(partition.part_id, 0, sizeof(partition.part_id));
  WriteMbr(disk_path, mbr);

  if (type == "E") {
    std::fstream disk(disk_path, std::ios::binary | std::ios::in | std::ios::out);
    EBR ebr{};
    ebr.part_mount = '0';
    ebr.part_fit = fit_value;
    ebr.part_start = partition.part_start + static_cast<int>(sizeof(EBR));
    ebr.part_s = 0;
    ebr.part_next = -1;
    std::memset(ebr.part_name, 0, sizeof(ebr.part_name));
    WriteEbr(disk, partition.part_start, ebr);
  }

  result.output.push_back("[OK] Partición creada: " + name);
}

void Engine::HandleMount(const ParsedCommand& command, CommandExecutionResult& result) {
  const auto disk_path = ResolveWorkspacePath(GetParam(command, "path"));
  const auto name = GetParam(command, "name");
  const auto partition = FindPartitionByName(disk_path, name);
  if (partition.start == 0) {
    throw std::runtime_error("La partición indicada no existe");
  }
  if (partition.is_logical || std::toupper(static_cast<unsigned char>(partition.type)) != 'P') {
    throw std::runtime_error("Solo se montan particiones primarias en esta simulación");
  }

  for (const auto& mounted : mounts_) {
    if (CanonicalForComparison(mounted.disk_path) == CanonicalForComparison(disk_path) && mounted.name == name) {
      result.output.push_back("[OK] Partición ya montada con id " + mounted.id);
      return;
    }
  }

  const auto disk_key = CanonicalForComparison(disk_path).string();
  char letter = 'A';
  if (disk_letters_.contains(disk_key)) {
    letter = disk_letters_.at(disk_key);
  } else {
    letter = next_disk_letter_++;
    disk_letters_[disk_key] = letter;
  }
  const int correlative = ++disk_correlatives_[disk_key];
  const std::string id = student_suffix_ + std::to_string(correlative) + std::string(1, letter);

  auto mbr = ReadMbr(disk_path);
  auto& entry = mbr.mbr_partitions[partition.index];
  entry.part_status = '1';
  entry.part_correlative = correlative;
  CopyName(entry.part_id, sizeof(entry.part_id), id);
  WriteMbr(disk_path, mbr);

  mounts_.push_back({id, disk_path, name, partition.index, partition.start, partition.size, letter, correlative});
  result.output.push_back("[OK] Partición montada con id " + id);
}

void Engine::HandleMounted(CommandExecutionResult& result) const {
  if (mounts_.empty()) {
    result.output.push_back("[INFO] No hay particiones montadas.");
    return;
  }
  result.output.push_back("[INFO] Particiones montadas:");
  for (const auto& mount : mounts_) {
    result.output.push_back(" - " + mount.id + " -> " + DisplayPath(mount.disk_path) + " :: " + mount.name);
  }
}

void Engine::HandleMkfs(const ParsedCommand& command, CommandExecutionResult& result) {
  const auto id = GetParam(command, "id");
  const auto type = ToLower(command.params.contains("type") ? command.params.at("type") : "full");
  if (type != "full") {
    throw std::runtime_error("mkfs solo admite type=full");
  }

  const auto mount_it = std::find_if(mounts_.begin(), mounts_.end(), [&](const MountedPartition& mount) {
    return ToLower(mount.id) == ToLower(id);
  });
  if (mount_it == mounts_.end()) {
    throw std::runtime_error("No existe una partición montada con ese id");
  }

  std::fstream disk(mount_it->disk_path, std::ios::binary | std::ios::in | std::ios::out);
  std::array<char, 1024> zeros{};
  for (int remaining = mount_it->size; remaining > 0; remaining -= zeros.size()) {
    const auto chunk = std::min<int>(static_cast<int>(zeros.size()), remaining);
    disk.seekp(mount_it->start + (mount_it->size - remaining));
    disk.write(zeros.data(), chunk);
  }

  const int denominator = static_cast<int>(sizeof(Inode)) + 3 * static_cast<int>(sizeof(FileBlock)) + 4;
  const int n = (mount_it->size - static_cast<int>(sizeof(SuperBlock))) / denominator;
  if (n < 2) {
    throw std::runtime_error("La partición es demasiado pequeña para formatearse como ext2");
  }

  SuperBlock sb{};
  sb.s_filesystem_type = 2;
  sb.s_inodes_count = n;
  sb.s_blocks_count = n * 3;
  sb.s_free_inodes_count = n - 2;
  sb.s_free_blocks_count = sb.s_blocks_count - 2;
  sb.s_mtime = CurrentTimestamp();
  sb.s_umtime = 0;
  sb.s_mnt_count = 1;
  sb.s_magic = 0xEF53;
  sb.s_inode_s = static_cast<int>(sizeof(Inode));
  sb.s_block_s = static_cast<int>(sizeof(FileBlock));
  sb.s_first_ino = 2;
  sb.s_first_blo = 2;
  sb.s_bm_inode_start = mount_it->start + static_cast<int>(sizeof(SuperBlock));
  sb.s_bm_block_start = sb.s_bm_inode_start + n;
  sb.s_inode_start = sb.s_bm_block_start + sb.s_blocks_count;
  sb.s_block_start = sb.s_inode_start + n * static_cast<int>(sizeof(Inode));
  WriteSuperBlock(disk, mount_it->start, sb);

  for (int index = 0; index < n; ++index) {
    WriteBitmapValue(disk, sb.s_bm_inode_start + index, '0');
  }
  for (int index = 0; index < sb.s_blocks_count; ++index) {
    WriteBitmapValue(disk, sb.s_bm_block_start + index, '0');
  }
  WriteBitmapValue(disk, sb.s_bm_inode_start + 0, '1');
  WriteBitmapValue(disk, sb.s_bm_inode_start + 1, '1');
  WriteBitmapValue(disk, sb.s_bm_block_start + 0, '1');
  WriteBitmapValue(disk, sb.s_bm_block_start + 1, '1');

  FolderBlock root_block{};
  for (auto& entry : root_block.b_content) {
    entry.b_inodo = -1;
    std::memset(entry.b_name, 0, sizeof(entry.b_name));
  }
  CopyName(root_block.b_content[0].b_name, sizeof(root_block.b_content[0].b_name), ".");
  root_block.b_content[0].b_inodo = 0;
  CopyName(root_block.b_content[1].b_name, sizeof(root_block.b_content[1].b_name), "..");
  root_block.b_content[1].b_inodo = 0;
  CopyName(root_block.b_content[2].b_name, sizeof(root_block.b_content[2].b_name), "users.txt");
  root_block.b_content[2].b_inodo = 1;
  WriteFolderBlock(disk, sb, 0, root_block);

  Inode root_inode{};
  root_inode.i_uid = 1;
  root_inode.i_gid = 1;
  root_inode.i_atime = CurrentTimestamp();
  root_inode.i_ctime = root_inode.i_atime;
  root_inode.i_mtime = root_inode.i_atime;
  std::fill(std::begin(root_inode.i_block), std::end(root_inode.i_block), -1);
  root_inode.i_block[0] = 0;
  root_inode.i_type = '0';
  CopyName(root_inode.i_perm, sizeof(root_inode.i_perm), "775");
  WriteInode(disk, sb, 0, root_inode);

  const std::string users_content = "1,G,root\n1,U,root,root,123\n";
  FileBlock users_block{};
  std::memset(users_block.b_content, 0, sizeof(users_block.b_content));
  std::memcpy(users_block.b_content, users_content.data(), std::min(users_content.size(), sizeof(users_block.b_content)));
  WriteFileBlock(disk, sb, 1, users_block);

  Inode users_inode{};
  users_inode.i_uid = 1;
  users_inode.i_gid = 1;
  users_inode.i_s = static_cast<int>(users_content.size());
  users_inode.i_atime = CurrentTimestamp();
  users_inode.i_ctime = users_inode.i_atime;
  users_inode.i_mtime = users_inode.i_atime;
  std::fill(std::begin(users_inode.i_block), std::end(users_inode.i_block), -1);
  users_inode.i_block[0] = 1;
  users_inode.i_type = '1';
  CopyName(users_inode.i_perm, sizeof(users_inode.i_perm), "664");
  WriteInode(disk, sb, 1, users_inode);
  WriteSuperBlock(disk, mount_it->start, sb);

  result.output.push_back("[OK] Partición formateada como EXT2 y users.txt creado.");
}

}  // namespace mia
