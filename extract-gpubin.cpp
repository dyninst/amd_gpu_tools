#include <cassert>
#include <fstream>
#include <iostream>
#include <string>

static void showHelp(const std::string &toolName) {
  std::cerr << "Usage : " << toolName << " <arch-name> "
            << "<path-to-fatbin>" << " <path-to-output-gpubin>" << std::endl;
  std::cerr << "supported architectures : gfx900, gfx906, gfx908, gfx90a, gfx940" << std::endl;
}

int main(int argc, char *argv[]) {
  if (argc != 4) {
    showHelp(argv[0]);
    exit(1);
  }

  std::string arch(argv[1]);
  std::string fatbinPath(argv[2]);
  std::string gpubinPath(argv[3]);

  std::ifstream fatbin(fatbinPath, std::ios::binary);
  if (!fatbin) {
    std::cerr << "error : can't open " << fatbinPath << std::endl;
    exit(1);
  }

  // This is at the beginning of the clang-offload-bundle file.
  // See https://clang.llvm.org/docs/ClangOffloadBundler.html
  constexpr std::string_view magicString("__CLANG_OFFLOAD_BUNDLE__");
  constexpr uint32_t magicStringLength = magicString.length();

  char buffer[magicStringLength + 1];
  fatbin.read(buffer, magicStringLength);
  buffer[magicStringLength] = 0;

  assert(std::string(buffer) == magicString);

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

    std::string idString;
    idString.resize(idLength);
    fatbin.read(&idString[0], idLength);

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
    exit(1);
  }

  // std::cout << arch << ' ' << "ELF at " << elfStart << " of size " << elfSize << '\n';

  fatbin.seekg(elfStart, std::ios::beg);
  std::string data;
  data.resize(elfSize);
  fatbin.read(&data[0], elfSize);

  std::ofstream elfBin(gpubinPath, std::ios::binary);

  if (!elfBin) {
    std::cerr << "error : can't create " << gpubinPath << std::endl;
    exit(1);
  }

  elfBin.write(&data[0], elfSize);
}
