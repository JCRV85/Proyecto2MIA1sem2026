#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mia {

struct CommandExecutionResult {
  std::vector<std::string> output;
  std::vector<std::string> artifacts;
};

struct MountedPartitionView {
  std::string id;
  std::string path;
  std::string name;
};

struct SessionView {
  bool active = false;
  std::string user;
  std::string group;
  std::string partition_id;
};

class Engine {
 public:
  explicit Engine(std::filesystem::path workspace_root);

  [[nodiscard]] int api_port() const;
  [[nodiscard]] const std::filesystem::path& workspace_root() const;
  [[nodiscard]] CommandExecutionResult Execute(const std::string& input);
  [[nodiscard]] std::vector<MountedPartitionView> mounted_partitions() const;
  [[nodiscard]] SessionView session() const;

 private:
  struct ParsedCommand {
    std::string name;
    std::unordered_map<std::string, std::string> params;
    std::vector<std::string> flags;
    std::string raw;
    int line = 0;
    bool is_comment = false;
    bool is_blank = false;
  };

  struct MountedPartition {
    std::string id;
    std::filesystem::path disk_path;
    std::string name;
    int partition_index = -1;
    int start = 0;
    int size = 0;
    char disk_letter = 'A';
    int correlative = 1;
  };

  struct Session {
    bool active = false;
    std::string user;
    std::string group;
    std::string partition_id;
    int uid = 0;
    int gid = 0;
  };

  struct PartitionMatch {
    bool is_logical = false;
    int index = -1;
    int ebr_position = -1;
    int start = 0;
    int size = 0;
    char type = 'P';
    std::string name;
  };

#pragma pack(push, 1)
  struct PartitionEntry {
    char part_status;
    char part_type;
    char part_fit;
    std::int32_t part_start;
    std::int32_t part_s;
    char part_name[16];
    std::int32_t part_correlative;
    char part_id[5];
  };

  struct MBR {
    std::int32_t mbr_tamano;
    std::int64_t mbr_fecha_creacion;
    std::int32_t mbr_dsk_signature;
    char dsk_fit;
    PartitionEntry mbr_partitions[4];
  };

  struct EBR {
    char part_mount;
    char part_fit;
    std::int32_t part_start;
    std::int32_t part_s;
    std::int32_t part_next;
    char part_name[16];
  };

  struct SuperBlock {
    std::int32_t s_filesystem_type;
    std::int32_t s_inodes_count;
    std::int32_t s_blocks_count;
    std::int32_t s_free_blocks_count;
    std::int32_t s_free_inodes_count;
    std::int64_t s_mtime;
    std::int64_t s_umtime;
    std::int32_t s_mnt_count;
    std::int32_t s_magic;
    std::int32_t s_inode_s;
    std::int32_t s_block_s;
    std::int32_t s_first_ino;
    std::int32_t s_first_blo;
    std::int32_t s_bm_inode_start;
    std::int32_t s_bm_block_start;
    std::int32_t s_inode_start;
    std::int32_t s_block_start;
  };

  struct Inode {
    std::int32_t i_uid;
    std::int32_t i_gid;
    std::int32_t i_s;
    std::int64_t i_atime;
    std::int64_t i_ctime;
    std::int64_t i_mtime;
    std::int32_t i_block[15];
    char i_type;
    char i_perm[4];
  };

  struct FolderContent {
    char b_name[12];
    std::int32_t b_inodo;
  };

  struct FolderBlock {
    FolderContent b_content[4];
  };

  struct FileBlock {
    char b_content[64];
  };

  struct PointerBlock {
    std::int32_t b_pointers[16];
  };
#pragma pack(pop)

  std::filesystem::path workspace_root_;
  std::string student_suffix_ = "00";
  int api_port_ = 8080;
  std::vector<MountedPartition> mounts_;
  Session current_session_;
  std::unordered_map<std::string, char> disk_letters_;
  std::unordered_map<std::string, int> disk_correlatives_;
  char next_disk_letter_ = 'A';

  void LoadConfig();

  [[nodiscard]] CommandExecutionResult ExecuteScript(const std::string& input);
  [[nodiscard]] ParsedCommand ParseLine(const std::string& line, int line_number) const;
  [[nodiscard]] std::vector<std::string> Tokenize(const std::string& line) const;
  void ExecuteCommand(const ParsedCommand& command, CommandExecutionResult& result);

  void HandleMkdisk(const ParsedCommand& command, CommandExecutionResult& result);
  void HandleRmdisk(const ParsedCommand& command, CommandExecutionResult& result);
  void HandleFdisk(const ParsedCommand& command, CommandExecutionResult& result);
  void HandleMount(const ParsedCommand& command, CommandExecutionResult& result);
  void HandleMounted(CommandExecutionResult& result) const;
  void HandleMkfs(const ParsedCommand& command, CommandExecutionResult& result);
  void HandleLogin(const ParsedCommand& command, CommandExecutionResult& result);
  void HandleLogout(CommandExecutionResult& result);
  void HandleMkgrp(const ParsedCommand& command, CommandExecutionResult& result);
  void HandleRmgrp(const ParsedCommand& command, CommandExecutionResult& result);
  void HandleMkusr(const ParsedCommand& command, CommandExecutionResult& result);
  void HandleRmusr(const ParsedCommand& command, CommandExecutionResult& result);
  void HandleChgrp(const ParsedCommand& command, CommandExecutionResult& result);
  void HandleMkdir(const ParsedCommand& command, CommandExecutionResult& result);
  void HandleMkfile(const ParsedCommand& command, CommandExecutionResult& result);
  void HandleCat(const ParsedCommand& command, CommandExecutionResult& result);
  void HandleRep(const ParsedCommand& command, CommandExecutionResult& result);
  void HandleExecute(const ParsedCommand& command, CommandExecutionResult& result);

  [[nodiscard]] std::filesystem::path ResolveWorkspacePath(const std::string& raw_path) const;
  [[nodiscard]] std::filesystem::path CanonicalForComparison(const std::filesystem::path& path) const;
  [[nodiscard]] std::string DisplayPath(const std::filesystem::path& path) const;

  [[nodiscard]] MBR ReadMbr(const std::filesystem::path& disk_path) const;
  void WriteMbr(const std::filesystem::path& disk_path, const MBR& mbr) const;
  [[nodiscard]] EBR ReadEbr(std::fstream& disk, int position) const;
  void WriteEbr(std::fstream& disk, int position, const EBR& ebr) const;
  [[nodiscard]] PartitionMatch FindPartitionByName(const std::filesystem::path& disk_path, const std::string& name) const;
  [[nodiscard]] std::optional<PartitionMatch> FindMountedPartition(const std::string& id) const;

  [[nodiscard]] SuperBlock ReadSuperBlock(std::fstream& disk, int partition_start) const;
  void WriteSuperBlock(std::fstream& disk, int partition_start, const SuperBlock& super_block) const;
  [[nodiscard]] Inode ReadInode(std::fstream& disk, const SuperBlock& super_block, int inode_index) const;
  void WriteInode(std::fstream& disk, const SuperBlock& super_block, int inode_index, const Inode& inode) const;
  [[nodiscard]] FolderBlock ReadFolderBlock(std::fstream& disk, const SuperBlock& super_block, int block_index) const;
  void WriteFolderBlock(std::fstream& disk, const SuperBlock& super_block, int block_index, const FolderBlock& block) const;
  [[nodiscard]] FileBlock ReadFileBlock(std::fstream& disk, const SuperBlock& super_block, int block_index) const;
  void WriteFileBlock(std::fstream& disk, const SuperBlock& super_block, int block_index, const FileBlock& block) const;

  [[nodiscard]] int AllocateInode(std::fstream& disk, SuperBlock& super_block) const;
  [[nodiscard]] int AllocateBlock(std::fstream& disk, SuperBlock& super_block) const;
  void FreeBlock(std::fstream& disk, SuperBlock& super_block, int block_index) const;
  void WriteBitmapValue(std::fstream& disk, int position, char value) const;
  [[nodiscard]] char ReadBitmapValue(std::fstream& disk, int position) const;

  [[nodiscard]] int ResolvePathToInode(std::fstream& disk, const SuperBlock& super_block, const std::string& path) const;
  [[nodiscard]] int FindDirectoryEntry(std::fstream& disk, const SuperBlock& super_block, const Inode& directory, const std::string& name) const;
  void AddDirectoryEntry(std::fstream& disk, SuperBlock& super_block, int parent_inode_index, Inode& parent_inode, const std::string& name, int child_inode_index) const;
  [[nodiscard]] int EnsureDirectory(std::fstream& disk, SuperBlock& super_block, const std::vector<std::string>& parts, bool recursive, int owner_uid, int owner_gid) const;
  [[nodiscard]] std::string ReadFileContent(std::fstream& disk, const SuperBlock& super_block, int inode_index) const;
  void WriteFileContent(std::fstream& disk, SuperBlock& super_block, int inode_index, Inode& inode, const std::string& content) const;

  [[nodiscard]] bool HasPermission(const Inode& inode, int read_mask, int uid, int gid, bool is_root) const;
  [[nodiscard]] int GroupIdByName(const std::string& group_name, const std::string& users_content) const;
  [[nodiscard]] std::string ReadUsersFile(std::fstream& disk, const SuperBlock& super_block) const;
  void WriteUsersFile(std::fstream& disk, SuperBlock& super_block, const std::string& content) const;

  [[nodiscard]] std::string GenerateMbrDot(const std::filesystem::path& disk_path, const MBR& mbr) const;
  [[nodiscard]] std::string GenerateDiskDot(const std::filesystem::path& disk_path, const MBR& mbr) const;
  [[nodiscard]] std::string GenerateSbDot(const SuperBlock& super_block) const;
  [[nodiscard]] std::string GenerateInodeDot(std::fstream& disk, const SuperBlock& super_block) const;
  [[nodiscard]] std::string GenerateBlockDot(std::fstream& disk, const SuperBlock& super_block) const;
  [[nodiscard]] std::string GenerateTreeDot(std::fstream& disk, const SuperBlock& super_block) const;
  [[nodiscard]] std::string GenerateLsDot(std::fstream& disk, const SuperBlock& super_block, const std::string& path) const;
  [[nodiscard]] std::string GenerateBitmapReport(std::fstream& disk, int start, int count) const;

  [[nodiscard]] static std::string Trim(const std::string& value);
  [[nodiscard]] static std::string ToLower(std::string value);
  [[nodiscard]] static std::string ToUpper(std::string value);
  [[nodiscard]] static std::vector<std::string> SplitPath(const std::string& path);
  [[nodiscard]] static std::vector<std::string> SplitLines(const std::string& text);
  [[nodiscard]] static std::vector<std::string> SplitCsv(const std::string& line);
  [[nodiscard]] static std::string Join(const std::vector<std::string>& lines, const std::string& separator);
  [[nodiscard]] static std::int64_t CurrentTimestamp();
  [[nodiscard]] static std::string FormatTimestamp(std::int64_t timestamp);
  [[nodiscard]] static std::string GetParam(const ParsedCommand& command, const std::string& key);
  [[nodiscard]] static bool HasFlag(const ParsedCommand& command, const std::string& flag);
  [[nodiscard]] static int ParsePositiveInt(const std::string& value, const std::string& field_name);
  [[nodiscard]] static std::string EscapeDot(const std::string& value);
  static void CopyName(char* target, std::size_t size, const std::string& value);
  [[nodiscard]] static std::string ReadName(const char* value, std::size_t size);
};

}  // namespace mia
