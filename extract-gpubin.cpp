#include <cassert>
#include <fstream>
#include <iostream>
#include <string>

void showHelp(const std::string &toolName) {
  std::cerr << "Usage : " << toolName << " <arch-name> "
            << "<path-to-fatbin>" << std::endl;
  std::cerr << "supported architectures : gfx900, gfx906, gfx908, gfx90a, gfx940" << std::endl;
}

int main(int argc, char *argv[]) {
  if (argc != 3) {
    showHelp(argv[0]);
    exit(1);
  }

  std::string arch(argv[1]);
  std::string fatbinPath(argv[2]);

  std::ifstream fatbin(fatbinPath, std::ios::binary);
  if (!fatbin) {
    std::cerr << "error : can't open " << fatbinPath << std::endl;
    exit(1);
  }

  char buffer[24 + 1];
  fatbin.read(buffer, 24);
  buffer[24] = 0;

  assert(std::string(buffer) == "__CLANG_OFFLOAD_BUNDLE__");

  uint64_t numBundleEntries = 0;
  fatbin.read(reinterpret_cast<char *>(&numBundleEntries), sizeof(numBundleEntries));

  uint64_t elfStart = 0;
  uint64_t elfSize = 0;
  bool found = false;

  // Read metadata for each elf object in this bundle
  while (numBundleEntries) {
    uint64_t bundleEntryCodeObjectOffset; // offset from begining of the fatbin
    fatbin.read(reinterpret_cast<char *>(&bundleEntryCodeObjectOffset),
                sizeof(bundleEntryCodeObjectOffset));

    uint64_t size;
    fatbin.read(reinterpret_cast<char *>(&size), sizeof(size));

    uint64_t idLength;
    fatbin.read(reinterpret_cast<char *>(&idLength), sizeof(idLength));

    char id[idLength];
    fatbin.read(id, idLength);

    std::string idString(id);
    // If idString ends with arch
    if (idString.substr(idLength - arch.length()) == arch) {
      elfStart = bundleEntryCodeObjectOffset;
      elfSize = size;
      found = true;
    }
    numBundleEntries--;
  }

  if (!found) {
    std::cerr << fatbinPath << " doesn't contain a " << arch << " binary\n";
    exit(0);
  }

  // std::cout << arch << ' ' << "ELF at " << elfStart << " of size " << elfSize << '\n';

  fatbin.seekg(elfStart, std::ios::beg);
  char data[elfSize];
  fatbin.read(data, elfSize);

  std::string elfBinPath(fatbinPath + "." + arch);
  std::ofstream elfBin(elfBinPath, std::ios::binary);

  if (!elfBin) {
    std::cerr << "error : can't create " << elfBinPath << std::endl;
    exit(1);
  }

  elfBin.write(data, elfSize);
  elfBin.close();

  fatbin.close();
}
