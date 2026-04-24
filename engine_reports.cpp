#include "mia/engine.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {

bool CommandExists(const std::string& command) {
#ifdef _WIN32
  const std::string probe = command + " -V >nul 2>nul";
#else
  const std::string probe = command + " -V >/dev/null 2>/dev/null";
#endif
  return std::system(probe.c_str()) == 0;
}

std::string QuoteShell(const std::filesystem::path& path) {
  return "\"" + path.string() + "\"";
}

}  // namespace

namespace mia {

void Engine::HandleRep(const ParsedCommand& command, CommandExecutionResult& result) {
  const auto report_name = ToLower(GetParam(command, "name"));
  const auto id = GetParam(command, "id");
  const auto output_path = ResolveWorkspacePath(GetParam(command, "path"));
  std::filesystem::create_directories(output_path.parent_path());

  const auto mount_it = std::find_if(mounts_.begin(), mounts_.end(), [&](const MountedPartition& mount) {
    return ToLower(mount.id) == ToLower(id);
  });
  if (mount_it == mounts_.end()) {
    throw std::runtime_error("El id indicado no está montado");
  }

  std::fstream disk(mount_it->disk_path, std::ios::binary | std::ios::in | std::ios::out);
  const auto mbr = ReadMbr(mount_it->disk_path);

  const auto write_text = [&](const std::filesystem::path& path, const std::string& content) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << content;
    result.artifacts.push_back(DisplayPath(path));
  };

  const auto render_graph = [&](const std::string& dot_content) {
    const auto dot_path = output_path.extension() == ".dot" ? output_path : std::filesystem::path(output_path.string() + ".dot");
    write_text(dot_path, dot_content);
    if (CommandExists("dot") && output_path.extension() != ".dot") {
      const auto format = output_path.extension().string().substr(1);
      const auto command_line = "dot -T" + format + " " + QuoteShell(dot_path) + " -o " + QuoteShell(output_path);
      if (std::system(command_line.c_str()) == 0) {
        result.artifacts.push_back(DisplayPath(output_path));
      }
    }
  };

  if (report_name == "mbr") {
    render_graph(GenerateMbrDot(mount_it->disk_path, mbr));
  } else if (report_name == "disk") {
    render_graph(GenerateDiskDot(mount_it->disk_path, mbr));
  } else {
    const auto sb = ReadSuperBlock(disk, mount_it->start);
    if (sb.s_magic != 0xEF53) {
      throw std::runtime_error("La partición no está formateada");
    }

    if (report_name == "sb") {
      render_graph(GenerateSbDot(sb));
    } else if (report_name == "inode") {
      render_graph(GenerateInodeDot(disk, sb));
    } else if (report_name == "block") {
      render_graph(GenerateBlockDot(disk, sb));
    } else if (report_name == "tree") {
      render_graph(GenerateTreeDot(disk, sb));
    } else if (report_name == "bm_inode") {
      write_text(output_path, GenerateBitmapReport(disk, sb.s_bm_inode_start, sb.s_inodes_count));
    } else if (report_name == "bm_block" || report_name == "bm_bloc") {
      write_text(output_path, GenerateBitmapReport(disk, sb.s_bm_block_start, sb.s_blocks_count));
    } else if (report_name == "file") {
      if (!command.params.contains("path_file_ls")) {
        throw std::runtime_error("file requiere path_file_ls");
      }
      const int inode_index = ResolvePathToInode(disk, sb, command.params.at("path_file_ls"));
      write_text(output_path, ReadFileContent(disk, sb, inode_index));
    } else if (report_name == "ls") {
      if (!command.params.contains("path_file_ls")) {
        throw std::runtime_error("ls requiere path_file_ls");
      }
      render_graph(GenerateLsDot(disk, sb, command.params.at("path_file_ls")));
    } else {
      throw std::runtime_error("Nombre de reporte no soportado");
    }
  }

  result.output.push_back("[OK] Reporte generado: " + report_name);
}

std::string Engine::GenerateMbrDot(const std::filesystem::path& disk_path, const MBR& mbr) const {
  std::ostringstream dot;
  dot << "digraph G {\nnode [shape=plain];\n";
  dot << "mbr [label=<\n<table border='1' cellborder='1' cellspacing='0'>\n";
  dot << "<tr><td colspan='2'><b>MBR</b></td></tr>\n";
  dot << "<tr><td>Tamano</td><td>" << mbr.mbr_tamano << "</td></tr>\n";
  dot << "<tr><td>Fecha</td><td>" << EscapeDot(FormatTimestamp(mbr.mbr_fecha_creacion)) << "</td></tr>\n";
  dot << "<tr><td>Signature</td><td>" << mbr.mbr_dsk_signature << "</td></tr>\n";
  dot << "<tr><td>Fit</td><td>" << mbr.dsk_fit << "</td></tr>\n";
  for (int index = 0; index < 4; ++index) {
    const auto& partition = mbr.mbr_partitions[index];
    if (partition.part_s <= 0) {
      continue;
    }
    dot << "<tr><td>Particion " << index + 1 << "</td><td>"
        << EscapeDot(ReadName(partition.part_name, sizeof(partition.part_name)))
        << " | " << partition.part_type << " | start=" << partition.part_start
        << " | size=" << partition.part_s << "</td></tr>\n";
  }

  std::fstream disk(disk_path, std::ios::binary | std::ios::in | std::ios::out);
  for (const auto& partition : mbr.mbr_partitions) {
    if (partition.part_s <= 0 || std::toupper(static_cast<unsigned char>(partition.part_type)) != 'E') {
      continue;
    }
    int current = partition.part_start;
    while (current != -1) {
      const auto ebr = ReadEbr(disk, current);
      if (ebr.part_s > 0) {
        dot << "<tr><td>EBR</td><td>"
            << EscapeDot(ReadName(ebr.part_name, sizeof(ebr.part_name)))
            << " | start=" << ebr.part_start << " | size=" << ebr.part_s
            << " | next=" << ebr.part_next << "</td></tr>\n";
      }
      current = ebr.part_next;
    }
  }
  dot << "</table>>];\n}\n";
  return dot.str();
}

std::string Engine::GenerateDiskDot(const std::filesystem::path&, const MBR& mbr) const {
  struct Segment {
    std::string label;
    int size;
  };

  std::vector<PartitionEntry> active;
  for (const auto& partition : mbr.mbr_partitions) {
    if (partition.part_s > 0) {
      active.push_back(partition);
    }
  }
  std::sort(active.begin(), active.end(), [](const PartitionEntry& left, const PartitionEntry& right) {
    return left.part_start < right.part_start;
  });

  std::vector<Segment> segments;
  int cursor = static_cast<int>(sizeof(MBR));
  segments.push_back({"MBR", cursor});
  for (const auto& partition : active) {
    if (partition.part_start > cursor) {
      segments.push_back({"Libre", partition.part_start - cursor});
    }
    segments.push_back({ReadName(partition.part_name, sizeof(partition.part_name)), partition.part_s});
    cursor = partition.part_start + partition.part_s;
  }
  if (cursor < mbr.mbr_tamano) {
    segments.push_back({"Libre", mbr.mbr_tamano - cursor});
  }

  std::ostringstream dot;
  dot << "digraph G {\nnode [shape=record];\n";
  dot << "disk [label=\"";
  for (std::size_t index = 0; index < segments.size(); ++index) {
    const auto percentage = (static_cast<double>(segments[index].size) * 100.0) / static_cast<double>(mbr.mbr_tamano);
    dot << EscapeDot(segments[index].label) << " "
        << std::fixed << std::setprecision(2) << percentage << "%";
    if (index + 1 != segments.size()) {
      dot << " | ";
    }
  }
  dot << "\"];\n}\n";
  return dot.str();
}

std::string Engine::GenerateSbDot(const SuperBlock& super_block) const {
  std::ostringstream dot;
  dot << "digraph G {\nnode [shape=plain];\n";
  dot << "sb [label=<\n<table border='1' cellborder='1' cellspacing='0'>\n";
  dot << "<tr><td colspan='2'><b>SuperBloque</b></td></tr>\n";
  dot << "<tr><td>s_filesystem_type</td><td>" << super_block.s_filesystem_type << "</td></tr>\n";
  dot << "<tr><td>s_inodes_count</td><td>" << super_block.s_inodes_count << "</td></tr>\n";
  dot << "<tr><td>s_blocks_count</td><td>" << super_block.s_blocks_count << "</td></tr>\n";
  dot << "<tr><td>s_free_inodes_count</td><td>" << super_block.s_free_inodes_count << "</td></tr>\n";
  dot << "<tr><td>s_free_blocks_count</td><td>" << super_block.s_free_blocks_count << "</td></tr>\n";
  dot << "<tr><td>s_mtime</td><td>" << EscapeDot(FormatTimestamp(super_block.s_mtime)) << "</td></tr>\n";
  dot << "<tr><td>s_magic</td><td>" << super_block.s_magic << "</td></tr>\n";
  dot << "<tr><td>s_inode_start</td><td>" << super_block.s_inode_start << "</td></tr>\n";
  dot << "<tr><td>s_block_start</td><td>" << super_block.s_block_start << "</td></tr>\n";
  dot << "</table>>];\n}\n";
  return dot.str();
}

std::string Engine::GenerateInodeDot(std::fstream& disk, const SuperBlock& super_block) const {
  std::ostringstream dot;
  dot << "digraph G {\nrankdir=LR;\nnode [shape=plain];\n";
  for (int index = 0; index < super_block.s_inodes_count; ++index) {
    if (ReadBitmapValue(disk, super_block.s_bm_inode_start + index) != '1') {
      continue;
    }
    const auto inode = ReadInode(disk, super_block, index);
    dot << "inode" << index << " [label=<\n<table border='1' cellborder='1' cellspacing='0'>\n";
    dot << "<tr><td colspan='2'><b>Inodo " << index << "</b></td></tr>\n";
    dot << "<tr><td>uid</td><td>" << inode.i_uid << "</td></tr>\n";
    dot << "<tr><td>gid</td><td>" << inode.i_gid << "</td></tr>\n";
    dot << "<tr><td>size</td><td>" << inode.i_s << "</td></tr>\n";
    dot << "<tr><td>type</td><td>" << inode.i_type << "</td></tr>\n";
    for (int pointer = 0; pointer < 12; ++pointer) {
      dot << "<tr><td>i_block[" << pointer << "]</td><td>" << inode.i_block[pointer] << "</td></tr>\n";
    }
    dot << "</table>>];\n";
  }
  dot << "}\n";
  return dot.str();
}

std::string Engine::GenerateBlockDot(std::fstream& disk, const SuperBlock& super_block) const {
  std::set<int> directory_blocks;
  std::set<int> file_blocks;
  for (int index = 0; index < super_block.s_inodes_count; ++index) {
    if (ReadBitmapValue(disk, super_block.s_bm_inode_start + index) != '1') {
      continue;
    }
    const auto inode = ReadInode(disk, super_block, index);
    for (int pointer = 0; pointer < 12; ++pointer) {
      if (inode.i_block[pointer] == -1) {
        continue;
      }
      if (inode.i_type == '0') {
        directory_blocks.insert(inode.i_block[pointer]);
      } else {
        file_blocks.insert(inode.i_block[pointer]);
      }
    }
  }

  std::ostringstream dot;
  dot << "digraph G {\nrankdir=LR;\nnode [shape=plain];\n";
  for (int block_index : directory_blocks) {
    const auto block = ReadFolderBlock(disk, super_block, block_index);
    dot << "b" << block_index << " [label=<\n<table border='1' cellborder='1' cellspacing='0'>\n";
    dot << "<tr><td colspan='2'><b>FolderBlock " << block_index << "</b></td></tr>\n";
    for (const auto& entry : block.b_content) {
      dot << "<tr><td>" << EscapeDot(ReadName(entry.b_name, sizeof(entry.b_name))) << "</td><td>" << entry.b_inodo << "</td></tr>\n";
    }
    dot << "</table>>];\n";
  }
  for (int block_index : file_blocks) {
    const auto block = ReadFileBlock(disk, super_block, block_index);
    dot << "b" << block_index << " [label=<\n<table border='1' cellborder='1' cellspacing='0'>\n";
    dot << "<tr><td><b>FileBlock " << block_index << "</b></td></tr>\n";
    dot << "<tr><td>" << EscapeDot(ReadName(block.b_content, sizeof(block.b_content))) << "</td></tr>\n";
    dot << "</table>>];\n";
  }
  dot << "}\n";
  return dot.str();
}

std::string Engine::GenerateTreeDot(std::fstream& disk, const SuperBlock& super_block) const {
  std::ostringstream dot;
  dot << "digraph G {\nrankdir=LR;\nnode [shape=box];\n";
  for (int index = 0; index < super_block.s_inodes_count; ++index) {
    if (ReadBitmapValue(disk, super_block.s_bm_inode_start + index) != '1') {
      continue;
    }
    const auto inode = ReadInode(disk, super_block, index);
    dot << "inode" << index << " [label=\"Inodo " << index << "\\n" << (inode.i_type == '0' ? "Dir" : "File") << "\"];\n";
    for (int pointer = 0; pointer < 12; ++pointer) {
      if (inode.i_block[pointer] == -1) {
        continue;
      }
      dot << "block" << inode.i_block[pointer] << " [label=\"Bloque " << inode.i_block[pointer] << "\"];\n";
      dot << "inode" << index << " -> block" << inode.i_block[pointer] << ";\n";
      if (inode.i_type == '0') {
        const auto block = ReadFolderBlock(disk, super_block, inode.i_block[pointer]);
        for (const auto& entry : block.b_content) {
          const auto name = ReadName(entry.b_name, sizeof(entry.b_name));
          if (entry.b_inodo != -1 && name != "." && name != "..") {
            dot << "block" << inode.i_block[pointer] << " -> inode" << entry.b_inodo
                << " [label=\"" << EscapeDot(name) << "\"];\n";
          }
        }
      }
    }
  }
  dot << "}\n";
  return dot.str();
}

std::string Engine::GenerateLsDot(std::fstream& disk, const SuperBlock& super_block, const std::string& path) const {
  const int inode_index = ResolvePathToInode(disk, super_block, path);
  const auto inode = ReadInode(disk, super_block, inode_index);

  std::ostringstream dot;
  dot << "digraph G {\nnode [shape=plain];\n";
  dot << "ls [label=<\n<table border='1' cellborder='1' cellspacing='0'>\n";
  dot << "<tr><td colspan='6'><b>LS " << EscapeDot(path) << "</b></td></tr>\n";
  dot << "<tr><td>Nombre</td><td>Tipo</td><td>UID</td><td>GID</td><td>Perm</td><td>Modificado</td></tr>\n";

  const auto write_row = [&](const std::string& name, const Inode& entry_inode) {
    dot << "<tr><td>" << EscapeDot(name) << "</td><td>" << (entry_inode.i_type == '0' ? "dir" : "file")
        << "</td><td>" << entry_inode.i_uid << "</td><td>" << entry_inode.i_gid << "</td><td>"
        << EscapeDot(ReadName(entry_inode.i_perm, sizeof(entry_inode.i_perm))) << "</td><td>"
        << EscapeDot(FormatTimestamp(entry_inode.i_mtime)) << "</td></tr>\n";
  };

  if (inode.i_type == '1') {
    write_row(path, inode);
  } else {
    for (int pointer : inode.i_block) {
      if (pointer == -1) {
        continue;
      }
      const auto block = ReadFolderBlock(disk, super_block, pointer);
      for (const auto& entry : block.b_content) {
        const auto name = ReadName(entry.b_name, sizeof(entry.b_name));
        if (entry.b_inodo == -1 || name == "." || name == "..") {
          continue;
        }
        write_row(name, ReadInode(disk, super_block, entry.b_inodo));
      }
    }
  }

  dot << "</table>>];\n}\n";
  return dot.str();
}

std::string Engine::GenerateBitmapReport(std::fstream& disk, int start, int count) const {
  std::ostringstream stream;
  for (int index = 0; index < count; ++index) {
    stream << ReadBitmapValue(disk, start + index);
    if ((index + 1) % 20 == 0) {
      stream << '\n';
    } else if (index + 1 != count) {
      stream << ' ';
    }
  }
  if (count % 20 != 0) {
    stream << '\n';
  }
  return stream.str();
}

}  // namespace mia
