#include <cassert>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

struct GpuBinInfo {
  GpuBinInfo(const std::string &id_, uint64_t offset_, uint64_t size_)
      : id(id_), offset(offset_), size(size_) {}

  std::string id;
  uint64_t offset;
  uint64_t size;

  void dump(std::ostream &os) {
    os << "id : " << id << " offset : " << offset << " size : " << size << '\n';
  }
};

static void showHelp(const std::string &toolName) {
  std::cerr << "Usage : " << toolName << " <arch-name> "
            << " <path-to-elf> "
            << "<path-to-fatbin>" << std::endl;
  std::cerr << "supported architectures : gfx900, gfx906, gfx908, gfx90a, gfx940" << std::endl;
  std::cerr << "This tool create a fat binary containing an instrumented GPU binary" << std::endl;
}

static void getgpuBinInfos(const std::string &fatbinPath, std::vector<GpuBinInfo> &infos) {
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

  while (numBundleEntries) {
    uint64_t bundleEntryCodeObjectOffset; // offset from begining of the fatbin
    fatbin.read(reinterpret_cast<char *>(&bundleEntryCodeObjectOffset),
                sizeof(bundleEntryCodeObjectOffset));

    uint64_t size;
    fatbin.read(reinterpret_cast<char *>(&size), sizeof(size));

    uint64_t idLength;
    fatbin.read(reinterpret_cast<char *>(&idLength), sizeof(idLength));

    char id[idLength + 1];
    fatbin.read(id, idLength);
    id[idLength] = 0; // Make id null-terminated

    GpuBinInfo info(id, bundleEntryCodeObjectOffset, size);
    infos.push_back(info);

    numBundleEntries--;
  }

  fatbin.close();
}

void dumpInfos(std::vector<GpuBinInfo> &infos) {
  for (auto &info : infos)
    info.dump(std::cout);
}

static int getIndex(const std::string &arch, const std::vector<GpuBinInfo> &infos) {
  int index = -1;
  for (int i = 0; i < infos.size(); ++i) {
    const GpuBinInfo &info = infos[i];
    size_t idLength = info.id.length();
    if (info.id.substr(idLength - arch.length()) == arch) {
      index = i;
    }
  }
  return index;
}

static uint64_t alignUp(uint64_t value, uint64_t alignment) {
  if (alignment <= 1)
    return value;

  uint64_t diff = value % alignment;
  return diff == 0 ? value : value + (alignment - diff);
}

int main(int argc, char *argv[]) {
  if (argc != 4) {
    showHelp(argv[0]);
    exit(1);
  }

  std::string arch(argv[1]);
  std::string elfBinPath(argv[2]);
  std::string fatbinPath(argv[3]);

  std::vector<GpuBinInfo> gpuBinInfos;
  getgpuBinInfos(fatbinPath, gpuBinInfos);

  int archIndex = getIndex(arch, gpuBinInfos);
  if (archIndex == -1) {
    std::cerr << fatbinPath << " doesn't contain a " << arch << " binary" << std::endl;
    exit(1);
  }

  std::ifstream elfBin(elfBinPath, std::ios::binary);
  if (!elfBin) {
    std::cerr << "error : can't open " << elfBinPath << std::endl;
    exit(1);
  }

  // Determine size of the elf binary and read it
  elfBin.seekg(0, std::ios::end);
  std::streampos pos = elfBin.tellg();
  std::streamoff offset = pos - std::streampos(0);
  uint64_t elfBinSize = static_cast<uint64_t>(offset);

  std::cout << "elfBinSize = " << elfBinSize << '\n';

  char elfBinContents[elfBinSize];
  elfBin.seekg(0, std::ios::beg);
  elfBin.read(elfBinContents, elfBinSize);
  elfBin.close();

  std::vector<GpuBinInfo> newBinInfos(gpuBinInfos);

  // If the binary we want to "replace" is followed by other binaries, we must
  // update their offsets. Since all offsets are 0x1000 (i.e 4096) aligned we also
  // respect the alignment when updating the offsets.
  newBinInfos[archIndex].size = elfBinSize;

  for (int i = archIndex + 1; i < newBinInfos.size(); ++i) {
    GpuBinInfo prevInfo = newBinInfos[i - 1];
    if (prevInfo.offset + prevInfo.size > newBinInfos[i].offset) {
      newBinInfos[i].offset = alignUp(prevInfo.offset + prevInfo.size, 0x1000);
    }
  }

  // dumpInfos(gpuBinInfos);
  // dumpInfos(newBinInfos);

  // Now we create a new fatbin
  std::ofstream newFatbin(fatbinPath + ".updated", std::ios::binary);
  if (!newFatbin) {
    std::cerr << "error : can't open new fatbin" << std::endl;
    exit(1);
  }

  // "write" doesn't write null-terminated strings
  // Magic string
  std::string magicStr = "__CLANG_OFFLOAD_BUNDLE__";
  newFatbin.write(magicStr.c_str(), magicStr.size());

  assert(gpuBinInfos.size() == newBinInfos.size());

  // Number of bundle entries
  uint64_t numBundleEntries = newBinInfos.size();
  newFatbin.write(reinterpret_cast<char *>(&numBundleEntries), sizeof(numBundleEntries));

  // Write the following for each entry:
  // offset
  // size
  // id length
  // id
  for (auto &info : newBinInfos) {
    newFatbin.write(reinterpret_cast<char *>(&info.offset), sizeof(info.offset));
    newFatbin.write(reinterpret_cast<char *>(&info.size), sizeof(info.size));

    uint64_t length = info.id.size();
    newFatbin.write(reinterpret_cast<char *>(&length), sizeof(length));
    newFatbin.write(info.id.c_str(), length);
  }

  // Now we write the GPU objects
  // 1. For each object upto archIndex, write padding, and copy contents from fatbin to newFatbin.
  //    Also assert that the offsets match what we computed.
  //
  // 2. Now write padding, and the updated gpubin provided as argument.
  //
  // 3. For each object after archIndex, write padding, and copy contents from fatbin to newFatbin.
  //    Also assert that the offsets match what we computed.

  std::ifstream fatbin(fatbinPath, std::ios::binary);
  assert(fatbin);

  // Writing upto archIndex
  for (int i = 0; i < archIndex; ++i) {
    char buffer[gpuBinInfos[i].size];
    fatbin.seekg(gpuBinInfos[i].offset, std::ios::beg);
    fatbin.read(buffer, gpuBinInfos[i].size);

    // Write padding before we start writing the ELF files
    pos = newFatbin.tellp();
    offset = static_cast<int>(pos - std::streampos(0));

    uint64_t paddingCount = alignUp(offset, 0x1000) - offset;
    std::vector<char> padding(paddingCount, 0);
    newFatbin.write(padding.data(), paddingCount);

    pos = newFatbin.tellp();
    offset = static_cast<int>(pos - std::streampos(0));

    // std::cout << offset << ' ' << gpuBinInfos[i].offset << '\n';
    assert(offset == newBinInfos[i].offset &&
           "Offset while writing ELF in new fatbin must match what we computed");
    newFatbin.write(buffer, gpuBinInfos[i].size);
  }

  // The instrumented gpubin
  pos = newFatbin.tellp();
  offset = static_cast<int>(pos - std::streampos(0));

  uint64_t newBinPaddingCount = alignUp(offset, 0x1000) - offset;
  std::cout << "padding for instrumented bin = " << newBinPaddingCount << " bytes\n";

  std::vector<char> newBinPadding(newBinPaddingCount, ' ');
  newFatbin.write(newBinPadding.data(), newBinPaddingCount);

  pos = newFatbin.tellp();
  offset = static_cast<int>(pos - std::streampos(0));

  std::cout << "writing instrumented bin at offset " << offset << '\n';
  newFatbin.write(elfBinContents, elfBinSize);

  // After archIndex
  for (int i = archIndex + 1; i < gpuBinInfos.size(); ++i) {
    char buffer[gpuBinInfos[i].size];
    fatbin.seekg(gpuBinInfos[i].offset, std::ios::beg);
    fatbin.read(buffer, gpuBinInfos[i].size);

    pos = newFatbin.tellp();
    offset = static_cast<int>(pos - std::streampos(0));

    uint64_t paddingCount = alignUp(offset, 0x1000) - offset;
    std::vector<char> padding(paddingCount, 0);
    newFatbin.write(padding.data(), paddingCount);

    pos = newFatbin.tellp();
    offset = static_cast<int>(pos - std::streampos(0));

    // std::cout << offset << ' ' << gpuBinInfos[i].offset << ' ' << newBinInfos[i].offset << '\n';
    assert(offset == newBinInfos[i].offset &&
           "Offset while writing ELF in new fatbin must match what we computed");
    newFatbin.write(buffer, gpuBinInfos[i].size);
  }

  fatbin.close();
  newFatbin.close();
}
