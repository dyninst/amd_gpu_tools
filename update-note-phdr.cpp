#include "elfio/elfio.hpp"

#include <cassert>

// usage:
// update-note-phdr <og-bin> <new-bin>

static void showHelp(const char *toolName) {
  std::cout << "usage : \n";
  std::cout << "  ";
  std::cout << toolName << " <og-bin> <newbin>\n\n";
}

static void dumpSection(const ELFIO::section *section, bool printContents = true) {
  assert(section && "section must be non-null");

  std::cout << "section : " << section->get_name() << ", ";
  std::cout << "size : " << section->get_size() << ", ";
  std::cout << "offset : " << section->get_offset() << ", ";
  std::cout << "addr-align : " << section->get_addr_align() << ", ";
  std::cout << "entry-size : " << section->get_entry_size() << '\n';

  if (!printContents)
    return;

  std::cout << "section contents :\n";

  std::cout << std::hex;
  for (int i = 0; i < section->get_size(); ++i) {
    std::cout << (unsigned)section->get_data()[i] << ' ';
  }
  std::cout << std::dec << '\n';
}

ELFIO::section *getSection(const std::string &sectionName, const ELFIO::elfio &file) {
  for (int i = 0; i < file.sections.size(); ++i) {
    if (file.sections[i]->get_name() == sectionName)
      return file.sections[i];
  }
  return nullptr;
}

ELFIO::section *getNoteSection(const ELFIO::elfio &file) { return getSection(".note", file); }

ELFIO::segment *getNoteSegment(const ELFIO::elfio &file) {
  for (int i = 0; i < file.segments.size(); ++i) {
    auto segment = file.segments[i];
    if (segment->get_type() == ELFIO::PT_NOTE)
      return segment;
  }
  return nullptr;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    std::cout << "exactly 2 arguments to " << argv[0] << " expected\n";
    showHelp(argv[0]);
    exit(1);
  }

  const char *gpuBinPath = argv[1];
  const char *newGpuBinPath = argv[2];
  std::cerr << "saving file to " << newGpuBinPath << '\n';

  ELFIO::elfio gpuBin;

  if (!gpuBin.load(gpuBinPath)) {
    std::cout << "can't find or process ELF file " << gpuBinPath << '\n';
    exit(1);
  }

  ELFIO::section *noteSection = getNoteSection(gpuBin);
  if (!noteSection) {
    std::cout << ".note section not found in " << gpuBinPath << "\n";
    exit(1);
  }

  ELFIO::segment *noteSegment = getNoteSegment(gpuBin);
  if (!noteSegment) {
    std::cout << ".note segment not found in " << gpuBinPath << "\n";
    exit(1);
  }

  noteSection->set_address(noteSegment->get_virtual_address());

  noteSegment->add_section(noteSection, noteSection->get_addr_align());
  noteSegment->set_file_size(noteSection->get_size());
  noteSegment->set_memory_size(noteSection->get_size());

  gpuBin.save(newGpuBinPath);
}
