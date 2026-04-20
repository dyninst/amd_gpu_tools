#include <cassert>
#include <fstream>
#include <iostream>

#include "elfio/elfio.hpp"

static ELFIO::section *getSection(const std::string &sectionName, const ELFIO::elfio &file) {
  for (int i = 0; i < file.sections.size(); ++i) {
    if (file.sections[i]->get_name() == sectionName)
      return file.sections[i];
  }
  return nullptr;
}

static ELFIO::section *getFatbinSection(const ELFIO::elfio &file) {
  return getSection(".hip_fatbin", file);
}

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "Usage: " << argv[0] << " <executable file with fatbin>" << std::endl;
    return 1;
  }

  ELFIO::elfio execFile;
  if (!execFile.load(argv[1])) {
    std::cout << "can't find or process ELF file " << argv[1] << '\n';
    exit(1);
  }

  ELFIO::section *fatbinSection = getFatbinSection(execFile);
  if (!fatbinSection) {
    std::cout << ".hip_fatbin section not found in " << argv[1] << "\n";
    exit(1);
  }

  // Write fatbin to a separate file
  std::ofstream fatbinFile(std::string(argv[1]) + ".fatbin", std::ios::out | std::ios::binary);

  fatbinFile.write(fatbinSection->get_data(), fatbinSection->get_size());
  fatbinFile.close();

  return 0;
}
