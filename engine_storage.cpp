#include "mia/engine.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace mia {

Engine::MBR Engine::ReadMbr(const std::filesystem::path& disk_path) const {
  std::ifstream disk(disk_path, std::ios::binary);
  if (!disk) {
    throw std::runtime_error("No se pudo abrir el disco");
  }
  MBR mbr{};
  disk.read(reinterpret_cast<char*>(&mbr), sizeof(MBR));
  return mbr;
}

void Engine::WriteMbr(const std::filesystem::path& disk_path, const MBR& mbr) const {
  std::fstream disk(disk_path, std::ios::binary | std::ios::in | std::ios::out);
  if (!disk) {
    throw std::runtime_error("No se pudo abrir el disco");
  }
  disk.seekp(0);
  disk.write(reinterpret_cast<const char*>(&mbr), sizeof(MBR));
}

Engine::EBR Engine::ReadEbr(std::fstream& disk, int position) const {
  EBR ebr{};
  disk.seekg(position);
  disk.read(reinterpret_cast<char*>(&ebr), sizeof(EBR));
  return ebr;
}

void Engine::WriteEbr(std::fstream& disk, int position, const EBR& ebr) const {
  disk.seekp(position);
  disk.write(reinterpret_cast<const char*>(&ebr), sizeof(EBR));
}

Engine::PartitionMatch Engine::FindPartitionByName(const std::filesystem::path& disk_path, const std::string& name) const {
  const auto mbr = ReadMbr(disk_path);
  for (int index = 0; index < 4; ++index) {
    const auto& partition = mbr.mbr_partitions[index];
    if (partition.part_s > 0 && ReadName(partition.part_name, sizeof(partition.part_name)) == name) {
      return {false, index, -1, partition.part_start, partition.part_s, partition.part_type, name};
    }
  }

  std::fstream disk(disk_path, std::ios::binary | std::ios::in | std::ios::out);
  for (int index = 0; index < 4; ++index) {
    const auto& partition = mbr.mbr_partitions[index];
    if (partition.part_s <= 0 || std::toupper(static_cast<unsigned char>(partition.part_type)) != 'E') {
      continue;
    }
    int current = partition.part_start;
    while (current != -1) {
      const auto ebr = ReadEbr(disk, current);
      if (ebr.part_s > 0 && ReadName(ebr.part_name, sizeof(ebr.part_name)) == name) {
        return {true, index, current, ebr.part_start, ebr.part_s, 'L', name};
      }
      current = ebr.part_next;
    }
  }

  return {};
}

std::optional<Engine::PartitionMatch> Engine::FindMountedPartition(const std::string& id) const {
  const auto mount_it = std::find_if(mounts_.begin(), mounts_.end(), [&](const MountedPartition& mount) {
    return ToLower(mount.id) == ToLower(id);
  });
  if (mount_it == mounts_.end()) {
    return std::nullopt;
  }
  return PartitionMatch{false, mount_it->partition_index, -1, mount_it->start, mount_it->size, 'P', mount_it->name};
}

Engine::SuperBlock Engine::ReadSuperBlock(std::fstream& disk, int partition_start) const {
  SuperBlock super_block{};
  disk.seekg(partition_start);
  disk.read(reinterpret_cast<char*>(&super_block), sizeof(SuperBlock));
  return super_block;
}

void Engine::WriteSuperBlock(std::fstream& disk, int partition_start, const SuperBlock& super_block) const {
  disk.seekp(partition_start);
  disk.write(reinterpret_cast<const char*>(&super_block), sizeof(SuperBlock));
}

Engine::Inode Engine::ReadInode(std::fstream& disk, const SuperBlock& super_block, int inode_index) const {
  Inode inode{};
  disk.seekg(super_block.s_inode_start + inode_index * static_cast<int>(sizeof(Inode)));
  disk.read(reinterpret_cast<char*>(&inode), sizeof(Inode));
  return inode;
}

void Engine::WriteInode(std::fstream& disk, const SuperBlock& super_block, int inode_index, const Inode& inode) const {
  disk.seekp(super_block.s_inode_start + inode_index * static_cast<int>(sizeof(Inode)));
  disk.write(reinterpret_cast<const char*>(&inode), sizeof(Inode));
}

Engine::FolderBlock Engine::ReadFolderBlock(std::fstream& disk, const SuperBlock& super_block, int block_index) const {
  FolderBlock block{};
  disk.seekg(super_block.s_block_start + block_index * static_cast<int>(sizeof(FolderBlock)));
  disk.read(reinterpret_cast<char*>(&block), sizeof(FolderBlock));
  return block;
}

void Engine::WriteFolderBlock(std::fstream& disk, const SuperBlock& super_block, int block_index, const FolderBlock& block) const {
  disk.seekp(super_block.s_block_start + block_index * static_cast<int>(sizeof(FolderBlock)));
  disk.write(reinterpret_cast<const char*>(&block), sizeof(FolderBlock));
}

Engine::FileBlock Engine::ReadFileBlock(std::fstream& disk, const SuperBlock& super_block, int block_index) const {
  FileBlock block{};
  disk.seekg(super_block.s_block_start + block_index * static_cast<int>(sizeof(FileBlock)));
  disk.read(reinterpret_cast<char*>(&block), sizeof(FileBlock));
  return block;
}

void Engine::WriteFileBlock(std::fstream& disk, const SuperBlock& super_block, int block_index, const FileBlock& block) const {
  disk.seekp(super_block.s_block_start + block_index * static_cast<int>(sizeof(FileBlock)));
  disk.write(reinterpret_cast<const char*>(&block), sizeof(FileBlock));
}

int Engine::AllocateInode(std::fstream& disk, SuperBlock& super_block) const {
  for (int index = 0; index < super_block.s_inodes_count; ++index) {
    if (ReadBitmapValue(disk, super_block.s_bm_inode_start + index) == '0') {
      WriteBitmapValue(disk, super_block.s_bm_inode_start + index, '1');
      --super_block.s_free_inodes_count;
      super_block.s_first_ino = index + 1;
      while (super_block.s_first_ino < super_block.s_inodes_count &&
             ReadBitmapValue(disk, super_block.s_bm_inode_start + super_block.s_first_ino) == '1') {
        ++super_block.s_first_ino;
      }
      return index;
    }
  }
  throw std::runtime_error("No hay inodos libres");
}

int Engine::AllocateBlock(std::fstream& disk, SuperBlock& super_block) const {
  for (int index = 0; index < super_block.s_blocks_count; ++index) {
    if (ReadBitmapValue(disk, super_block.s_bm_block_start + index) == '0') {
      WriteBitmapValue(disk, super_block.s_bm_block_start + index, '1');
      --super_block.s_free_blocks_count;
      super_block.s_first_blo = index + 1;
      while (super_block.s_first_blo < super_block.s_blocks_count &&
             ReadBitmapValue(disk, super_block.s_bm_block_start + super_block.s_first_blo) == '1') {
        ++super_block.s_first_blo;
      }
      return index;
    }
  }
  throw std::runtime_error("No hay bloques libres");
}

void Engine::FreeBlock(std::fstream& disk, SuperBlock& super_block, int block_index) const {
  if (block_index < 0) {
    return;
  }
  if (ReadBitmapValue(disk, super_block.s_bm_block_start + block_index) == '1') {
    WriteBitmapValue(disk, super_block.s_bm_block_start + block_index, '0');
    ++super_block.s_free_blocks_count;
    super_block.s_first_blo = std::min(super_block.s_first_blo, block_index);
  }
  FileBlock block{};
  std::memset(block.b_content, 0, sizeof(block.b_content));
  WriteFileBlock(disk, super_block, block_index, block);
}

void Engine::WriteBitmapValue(std::fstream& disk, int position, char value) const {
  disk.seekp(position);
  disk.put(value);
}

char Engine::ReadBitmapValue(std::fstream& disk, int position) const {
  disk.seekg(position);
  return static_cast<char>(disk.get());
}

}  // namespace mia
