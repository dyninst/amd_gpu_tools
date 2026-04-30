#include <cassert>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

// Magic string at the beginning of the bundle
static std::string magicStr("__CLANG_OFFLOAD_BUNDLE__");


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

  std::string buffer;
  buffer.resize(magicStr.length());
  fatbin.read(&buffer[0], magicStr.length());

  assert(std::string(buffer) == magicStr);

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

    std::string id;
    id.resize(idLength);
    fatbin.read(&id[0], idLength);

    GpuBinInfo info(id, bundleEntryCodeObjectOffset, size);
    infos.push_back(info);

    numBundleEntries--;
  }

  fatbin.close();
}

// Commenting out to prevent unused function warning as this can be used later if we
// add log levels for debugging
//
// static void dumpInfos(std::vector<GpuBinInfo> &infos) {
//   for (auto &info : infos)
//     info.dump(std::cout);
// }

static int getIndex(const std::string &arch, const std::vector<GpuBinInfo> &infos) {
  int index = -1;
  int infosLength = static_cast<int>(infos.size());
  for (int i = 0; i < infosLength; ++i) {
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

  std::string elfBinContents;
  elfBinContents.resize(elfBinSize);
  elfBin.seekg(0, std::ios::beg);
  elfBin.read(&elfBinContents[0], elfBinSize);
  elfBin.close();

  std::vector<GpuBinInfo> newBinInfos(gpuBinInfos);

  // If the binary we want to "replace" is followed by other binaries, we must
  // update their offsets. Since all offsets are 0x1000 (i.e 4096) aligned we also
  // respect the alignment when updating the offsets.

  constexpr uint64_t alignment = 0x1000;
  newBinInfos[archIndex].size = elfBinSize;

  for (size_t i = archIndex + 1; i < newBinInfos.size(); ++i) {
    GpuBinInfo prevInfo = newBinInfos[i - 1];
    if (prevInfo.offset + prevInfo.size > newBinInfos[i].offset) {
      newBinInfos[i].offset = alignUp(prevInfo.offset + prevInfo.size, alignment);
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
    std::string buffer;
    buffer.resize(gpuBinInfos[i].size);
    fatbin.seekg(gpuBinInfos[i].offset, std::ios::beg);
    fatbin.read(&buffer[0], gpuBinInfos[i].size);

    // Write padding before we start writing the ELF files
    pos = newFatbin.tellp();
    offset = static_cast<uint64_t>(pos - std::streampos(0));

    uint64_t paddingCount = alignUp(offset, alignment) - offset;
    std::vector<char> padding(paddingCount, 0);
    newFatbin.write(padding.data(), paddingCount);

    pos = newFatbin.tellp();
    offset = static_cast<uint64_t>(pos - std::streampos(0));

    // std::cout << offset << ' ' << gpuBinInfos[i].offset << '\n';
    assert(static_cast<uint64_t>(offset) == newBinInfos[i].offset &&
           "Offset while writing ELF in new fatbin must match what we computed");
    newFatbin.write(buffer.c_str(), gpuBinInfos[i].size);
  }

  // The instrumented gpubin
  pos = newFatbin.tellp();
  offset = static_cast<int>(pos - std::streampos(0));

  uint64_t newBinPaddingCount = alignUp(offset, alignment) - offset;
  std::cout << "padding for instrumented bin = " << newBinPaddingCount << " bytes\n";

  std::vector<char> newBinPadding(newBinPaddingCount, ' ');
  newFatbin.write(newBinPadding.data(), newBinPaddingCount);

  pos = newFatbin.tellp();
  offset = static_cast<int>(pos - std::streampos(0));

  std::cout << "writing instrumented bin at offset " << offset << '\n';
  newFatbin.write(elfBinContents.c_str(), elfBinSize);

  // After archIndex
  for (size_t i = archIndex + 1; i < gpuBinInfos.size(); ++i) {
    std::string buffer;
    buffer.resize(gpuBinInfos[i].size);
    fatbin.seekg(gpuBinInfos[i].offset, std::ios::beg);
    fatbin.read(&buffer[0], gpuBinInfos[i].size);

    pos = newFatbin.tellp();
    offset = static_cast<int>(pos - std::streampos(0));

    uint64_t paddingCount = alignUp(offset, alignment) - offset;
    std::vector<char> padding(paddingCount, 0);
    newFatbin.write(padding.data(), paddingCount);

    pos = newFatbin.tellp();
    offset = static_cast<int>(pos - std::streampos(0));

    // std::cout << offset << ' ' << gpuBinInfos[i].offset << ' ' << newBinInfos[i].offset << '\n';
    assert(static_cast<uint64_t>(offset) == newBinInfos[i].offset &&
           "Offset while writing ELF in new fatbin must match what we computed");
    newFatbin.write(buffer.c_str(), gpuBinInfos[i].size);
  }

  fatbin.close();
  newFatbin.close();
}
