#include "elfio/elfio.hpp"

#include <cassert>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <unordered_map>

// This tool creates a clone of the original executable, adds the new fatbin
// to the clone, and later patches the clone so that the Linux kernel loader can
// see the program headers.
//
// usage:
// update-exec <og-exec> <fatbin> <new-exec>

// These maps are for correcting the section links in the clone.
std::unordered_map<ELFIO::section *, ELFIO::section *> ogToNewSectionMap;
std::unordered_map<ELFIO::section *, ELFIO::section *> newToOgSectionMap;

static void showHelp(const char *toolName) {
  std::cout << "usage : \n";
  std::cout << "  ";
  std::cout << toolName << " <og-exec> <fatbin> <new-exec> \n\n";
  std::cout << toolName << " will emit <new-exec> containing the <fatbin>\n";
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

// === SECTION-GETTING HELPERS BEGIN ===
//
ELFIO::section *getSection(const std::string &sectionName, const ELFIO::elfio &file) {
  for (int i = 0; i < file.sections.size(); ++i) {
    if (file.sections[i]->get_name() == sectionName)
      return file.sections[i];
  }
  return nullptr;
}

ELFIO::section *getFatbinSection(const ELFIO::elfio &file) {
  return getSection(".hip_fatbin", file);
}

ELFIO::section *getFatbinWrapperSection(const ELFIO::elfio &file) {
  return getSection(".hipFatBinSegment", file);
}
//
// === SECTION-GETTING HELPERS END ===

static size_t getFileSize(const std::string &filePath) {
  std::ifstream file(filePath);
  assert(file.is_open());

  file.seekg(0, std::ifstream::end);

  std::streampos pos = file.tellg();
  std::streamoff offset = pos - std::streampos(0);
  uint64_t size = static_cast<size_t>(offset);

  file.close();
  return size;
}

ELFIO::segment *getPtLoad1(const ELFIO::elfio &file) {
  for (int i = 0; i < file.segments.size(); ++i) {
    auto segment = file.segments[i];
    if (segment->get_type() == ELFIO::PT_LOAD)
      return segment;
  }
  return nullptr;
}

ELFIO::segment *getPhdrSegment(const ELFIO::elfio &file) {
  size_t entryPoint = file.get_entry();
  for (int i = 0; i < file.segments.size(); ++i) {
    auto segment = file.segments[i];
    if (segment->get_type() == ELFIO::PT_PHDR)
      return segment;
  }
  return nullptr;
}

ELFIO::segment *getLastSegment(const ELFIO::elfio &execFile) {
  const size_t numSegments = execFile.segments.size();
  assert(numSegments != 0);

  ELFIO::segment *lastSegment = execFile.segments[0];
  for (size_t i = 0; i < numSegments; ++i) {
    ELFIO::segment *currSegment = execFile.segments[i];
    size_t currSegmentBegin = currSegment->get_virtual_address();
    size_t currSegmentSize = currSegment->get_memory_size();
    size_t currSegmentEnd = currSegmentBegin + currSegmentSize;

    size_t lastSegmentBegin = lastSegment->get_virtual_address();
    size_t lastSegmentSize = lastSegment->get_memory_size();
    size_t lastSegmentEnd = lastSegmentBegin + lastSegmentSize;

    if (currSegmentEnd > lastSegmentEnd) {
      lastSegment = currSegment;
    }
  }

  return lastSegment;
}

void cloneHeader(const ELFIO::elfio &ogExec, ELFIO::elfio &newExec) {
  newExec.create(ogExec.get_class(), ogExec.get_encoding());
  newExec.set_os_abi(ogExec.get_os_abi());
  newExec.set_abi_version(ogExec.get_abi_version());
  newExec.set_type(ogExec.get_type());
  newExec.set_machine(ogExec.get_machine());
  newExec.set_entry(ogExec.get_entry());
}

bool shouldClone(const ELFIO::section *section) {
  switch (section->get_type()) {
  case ELFIO::SHT_NULL:
    return false;

  case ELFIO::SHT_STRTAB:
    // Don't clone section header string table, ELFIO will create a new one
    if (section->get_name() == ".shstrtab")
      return false;
    return true;

  default:
    return true;
  }
}

void cloneSections(const ELFIO::elfio &ogExec, ELFIO::elfio &newExec) {
  auto ogSections = ogExec.sections;
  for (size_t i = 0; i < ogSections.size(); ++i) {
    ELFIO::section *ogSection = ogSections[i];

    if (!shouldClone(ogSection))
      continue;

    std::cout << "cloning\n";
    dumpSection(ogSection, false);
    std::cout << '\n';

    const std::string &name = ogSection->get_name();
    ELFIO::section *newSection = newExec.sections.add(name);
    newSection->set_type(ogSection->get_type());
    newSection->set_flags(ogSection->get_flags());
    newSection->set_info(ogSection->get_info());

    // NOTE: This can be incorrect link, and will be corrected later, after all
    // sections are cloned.
    newSection->set_link(ogSection->get_link());

    newSection->set_addr_align(ogSection->get_addr_align());
    newSection->set_entry_size(ogSection->get_entry_size());
    newSection->set_address(ogSection->get_address());
    newSection->set_size(ogSection->get_size());

    if (const char *contents = ogSection->get_data())
      newSection->set_data(contents, ogSection->get_size());

    ogToNewSectionMap[ogSection] = newSection;
    newToOgSectionMap[newSection] = ogSection;
  }
}

void correctSectionLinks(const ELFIO::elfio &ogExec, ELFIO::elfio &newExec) {
  auto ogSections = ogExec.sections;
  auto newSections = newExec.sections;

  // If ogSection's sh_link holds index in ogExec's section header table, we
  // must update newSection's sh_link hold corresponding index in newExec's
  // section header table.
  for (size_t i = 0; i < newSections.size(); ++i) {
    auto *currentNewSection = newSections[i];

    auto iter1 = newToOgSectionMap.find(currentNewSection);
    if (iter1 == newToOgSectionMap.end())
      continue;

    auto *ogSection = iter1->second;
    auto ogLinkSectionIdx = ogSection->get_link();
    auto *ogLinkSection = ogSections[ogLinkSectionIdx];

    auto iter2 = ogToNewSectionMap.find(ogLinkSection);
    if (iter2 == ogToNewSectionMap.end())
      continue;

    auto *newLinkSection = iter2->second;
    auto newLinkSectionIdx = newLinkSection->get_index();
    currentNewSection->set_link(newLinkSectionIdx);
  }
}

void cloneSegments(const ELFIO::elfio &ogExec, ELFIO::elfio &newExec) {
  auto ogSegments = ogExec.segments;
  for (size_t i = 0; i < ogSegments.size(); ++i) {
    ELFIO::segment *ogSegment = ogSegments[i];
    ELFIO::segment *newSegment = newExec.segments.add();
    newSegment->set_type(ogSegment->get_type());
    newSegment->set_flags(ogSegment->get_flags());
    newSegment->set_align(ogSegment->get_align());
    newSegment->set_virtual_address(ogSegment->get_virtual_address());
    newSegment->set_physical_address(ogSegment->get_physical_address());

    newSegment->set_file_size(ogSegment->get_file_size());
    newSegment->set_memory_size(ogSegment->get_memory_size());
  }

  auto newSegments = newExec.segments;
  auto newSections = newExec.sections;

  // Now map new sections into new segments
  for (size_t i = 0; i < newSections.size(); ++i) {
    auto currSection = newSections[i];
    auto currSectionBegin = currSection->get_address();
    auto currSectionSize = currSection->get_size();
    auto currSectionEnd = currSectionBegin + currSectionSize;

    for (size_t j = 0; j < newSegments.size(); ++j) {
      auto newSegmentBegin = newSegments[j]->get_virtual_address();
      auto newSegmentSize = newSegments[j]->get_memory_size();
      auto newSegmentEnd = newSegmentBegin + newSegmentSize;
      bool c1 = currSectionBegin >= newSegmentBegin && currSectionBegin < newSegmentEnd;
      bool c2 = currSectionEnd <= newSegmentEnd;
      if (c1 && c2) {
        newSegments[j]->add_section(currSection, 1);
      }
    }
  }
}

void cloneExec(const ELFIO::elfio &ogExec, ELFIO::elfio &newExec) {
  cloneHeader(ogExec, newExec);
  cloneSections(ogExec, newExec);
  correctSectionLinks(ogExec, newExec);
  cloneSegments(ogExec, newExec);
}

void updateFatbinAddr(ELFIO::elfio &execFile, uint64_t newAddr) {
  ELFIO::section *fatbinWrapperSection = getFatbinWrapperSection(execFile);

  // address is at offset 8.
  uint64_t *addrPtr = (uint64_t *)(fatbinWrapperSection->get_data() + 8);
  *addrPtr = newAddr;
}

// Create a .new_fatbin section, map it to a new PT_LOAD segment, update the
// fatbin wrapper.
void addNewFatbin(ELFIO::elfio &newExec, const char *newFatbinContent, size_t newFatbinSize) {

  ELFIO::section *fatbinSection = getFatbinSection(newExec);
  assert(fatbinSection);

  // Calculate next virtual address for loading the new fatbin.
  ELFIO::segment *lastSegment = getLastSegment(newExec);
  size_t nextAddr = lastSegment->get_virtual_address() + lastSegment->get_memory_size();

  size_t alignment = fatbinSection->get_addr_align();
  assert(alignment != 0);

  while (nextAddr % alignment != 0) {
    ++nextAddr;
  }

  ELFIO::section *newFatbinSection = newExec.sections.add(".new_fatbin");
  newFatbinSection->set_type(fatbinSection->get_type());
  newFatbinSection->set_flags(fatbinSection->get_flags());
  newFatbinSection->set_info(fatbinSection->get_info());
  newFatbinSection->set_addr_align(fatbinSection->get_addr_align());
  newFatbinSection->set_entry_size(fatbinSection->get_entry_size());
  newFatbinSection->set_size(newFatbinSize);
  newFatbinSection->set_data(newFatbinContent, newFatbinSize);
  newFatbinSection->set_address(nextAddr);

  ELFIO::segment *newSegment = newExec.segments.add();
  newSegment->set_type(ELFIO::PT_LOAD);
  newSegment->set_flags(ELFIO::PF_R);
  newSegment->set_align(fatbinSection->get_addr_align());
  newSegment->set_virtual_address(nextAddr);
  newSegment->set_physical_address(nextAddr);

  newSegment->add_section(newFatbinSection, 1);
  updateFatbinAddr(newExec, nextAddr);
}

// This is for patching the clone at last. For some reason, editing raw segments
// doesn't work with ELFIO. The macros in elf.h conflict with ELFIO's constants,
// hence keeping the include here.
#include <elf.h>

void patchExec(const char *rwExecPath) {
  ELFIO::elfio newExecFile;
  FILE *rawNewElf = fopen(rwExecPath, "rb+");

  if (!rawNewElf || !newExecFile.load(rwExecPath)) {
    std::cout << "can't find or process new ELF file " << rwExecPath << '\n';
    exit(1);
  }

  ELFIO::segment *ptLoad1 = getPtLoad1(newExecFile);
  uint64_t ptLoad1Offset = ptLoad1->get_offset();
  ELFIO::segment *phdrSeg = getPhdrSegment(newExecFile);

  char *ptLoad1Data = (char *)ptLoad1->get_data();
  char *pHdrs = (char *)phdrSeg->get_data();

  size_t numZeroes = 0;
  for (size_t i = 0; i < ptLoad1->get_memory_size() && ptLoad1Data[i] == 0; ++i) {
    ++numZeroes;
  }

  if (numZeroes < phdrSeg->get_memory_size()) {
    std::cout << "can't patch final executable, please explicitly use ld to run it\n";
    exit(1);
  }

  // Step 1. Copy program header table to beginning of PT_LOAD1.
  std::cout << "Copying program header table to beginning of PT_LOAD1...\n";
  if (fseek(rawNewElf, ptLoad1Offset, SEEK_SET)) {
    std::cout << "error going to " << ptLoad1Offset << '\n';
    exit(1);
  }
  std::cout << fwrite(pHdrs, sizeof(char), phdrSeg->get_file_size(), rawNewElf)
            << " bytes written to PT_LOAD1\n";
  std::cout << '\n';

  // Step 2. Update PT_LOAD1's program header (the one present in PT_LOAD1).
  // Update p_vaddr to hold the address of PT_LOAD1
  assert(fseek(rawNewElf, ptLoad1Offset, SEEK_SET) == 0);
  Elf64_Phdr progHeader;
  std::cout << "Updating PT_LOAD1's program header in PT_LOAD1...\n";
  std::cout << fread(&progHeader, sizeof(Elf64_Phdr), 1, rawNewElf)
            << " Elf64_Phdrs read from beginning of PT_LOAD1\n";

  progHeader.p_vaddr = ptLoad1->get_virtual_address();
  progHeader.p_paddr = ptLoad1->get_physical_address();
  assert(fseek(rawNewElf, ptLoad1Offset, SEEK_SET) == 0);
  std::cout << fwrite(&progHeader, sizeof(Elf64_Phdr), 1, rawNewElf)
            << " Elf64_Phdrs written to beginning of PT_LOAD1\n";
  std::cout << '\n';

  // Step 3. Update ELF header on disk.
  // The offset of program header table should be offset of PT_LOAD1.
  std::cout << "Updating ELF header's e_phoff to PT_LOAD1's offset...\n";
  Elf64_Ehdr elfHeader;
  assert(fseek(rawNewElf, 0, SEEK_SET) == 0);
  std::cout << (fread(&elfHeader, sizeof(Elf64_Ehdr), 1, rawNewElf))
            << " Elf64_Ehdrs read from beginning of " << rwExecPath << '\n';

  std::cout << "old e_phoff : " << elfHeader.e_phoff << '\n';
  elfHeader.e_phoff = ptLoad1->get_offset();
  std::cout << "new e_phoff : " << elfHeader.e_phoff << '\n';

  assert(fseek(rawNewElf, 0, SEEK_SET) == 0);
  std::cout << fwrite(&elfHeader, sizeof(Elf64_Ehdr), 1, rawNewElf)
            << " Elf64_Ehdrs written to beginning of " << rwExecPath << '\n';

  fclose(rawNewElf);
}

int main(int argc, char **argv) {
  if (argc != 4) {
    std::cout << "exactly 3 arguments to " << argv[0] << " expected\n";
    showHelp(argv[0]);
    exit(1);
  }

  const char *execFilePath = argv[1];
  const char *newFatbinPath = argv[2];
  const char *rwExecPath = argv[3];

  ELFIO::elfio execFile;
  ELFIO::elfio newExecFile;
  std::ifstream newFatbin;

  if (!execFile.load(execFilePath)) {
    std::cout << "can't find or process ELF file " << execFilePath << '\n';
    exit(1);
  }

  ELFIO::section *fatbinSection = getFatbinSection(execFile);
  if (!fatbinSection) {
    std::cout << ".hip_fatbin section not found in " << execFilePath << "\n";
    exit(1);
  }

  ELFIO::section *fatbinWrapperSection = getFatbinWrapperSection(execFile);
  if (!fatbinWrapperSection) {
    std::cout << ".hipFatBinSegment section not found in " << execFilePath << "\n";
    exit(1);
  }

  cloneExec(execFile, newExecFile);

  size_t newFatbinSize = getFileSize(newFatbinPath);
  char *newFatbinContent = new char[newFatbinSize];

  newFatbin.open(newFatbinPath, std::ios::binary);
  newFatbin.read(newFatbinContent, newFatbinSize);
  addNewFatbin(newExecFile, newFatbinContent, newFatbinSize);

  delete[] newFatbinContent;
  newFatbin.close();

  std::cout << newExecFile.validate() << '\n';
  newExecFile.save(rwExecPath);

  // To ensure that the linux kernel loader picks up the program headers.
  patchExec(rwExecPath);
}
