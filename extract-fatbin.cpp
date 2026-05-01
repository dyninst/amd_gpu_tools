#include <cassert>
#include <fstream>
#include <iostream>

#include "elfio/elfio.hpp"

// usage:
// extract-fatbin <exec> <output-fatbin>

static ELFIO::section *getSection(const std::string &sectionName, const ELFIO::elfio &file) {
  for (const auto &section: file.sections)  {
    if (section->get_name() == sectionName) {
      return section.get();
    }
  }
  return nullptr;
}

static ELFIO::section *getFatbinSection(const ELFIO::elfio &file) {
  return getSection(".hip_fatbin", file);
}

int main(int argc, char **argv) {
  if (argc != 3) {
    std::cerr << "Usage: " << argv[0] << " <executable file with fatbin> <output fatbin name>" << std::endl;
    return 1;
  }

  std::string execFilePath(argv[1]);
  std::string outputFatbinPath(argv[2]);
  ELFIO::elfio execFile;
  if (!execFile.load(execFilePath)) {
    std::cerr << "can't find or process ELF file " << execFilePath << '\n';
    exit(1);
  }

  ELFIO::section *fatbinSection = getFatbinSection(execFile);
  if (!fatbinSection) {
    std::cerr << ".hip_fatbin section not found in " << execFilePath << "\n";
    exit(1);
  }

  // Write fatbin to a separate file
  std::ofstream fatbinFile(outputFatbinPath, std::ios::out | std::ios::binary);

  fatbinFile.write(fatbinSection->get_data(), fatbinSection->get_size());

  return 0;
}
