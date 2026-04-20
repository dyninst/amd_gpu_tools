#include "hip/hip_runtime.h"

#include <chrono>
#include <dlfcn.h>
#include <iostream>
#include <fstream>
#include <cassert>
#include <string>
#include <sstream>
#include <cstring>
#include <vector>
#include <unordered_map>

// Environment variable for the instrumentation variable table path:
const char *instrumentationVariableTableEnv = "DYNINST_AMDGPU_INSTRUMENTATON_VAR_TABLE";

// Environment variable for the instrumented kernel names path:
const char *instrumentedKernelNamesEnv = "DYNINST_AMDGPU_INSTRUMENTED_KERNEL_NAMES";

// This will be used to print the names and values of the instrumentation variables after the kernel launch is done and the instrumentation variables are copied back.
struct InstrumentationVarTableEntry {
  int offset;
  std::string name;
  // TODO : This needs a size field too

  InstrumentationVarTableEntry(std::vector<std::string> words) {
    assert(words.size() == 2);
    offset = std::stoi(words[0]);
    name = words[1];
  }
};

std::unordered_map<std::string, int> &getKernargSizeMap() {
  static std::unordered_map<std::string, int> instance;
  return instance;
}

std::unordered_map<std::string, int> &getFirstHiddenArgIndexMap() {
  static std::unordered_map<std::string, int> instance;
  return instance;
}

std::vector<InstrumentationVarTableEntry> &getInstrumentationVarTableEntries() {
  static std::vector<InstrumentationVarTableEntry> instance;
  return instance;
}

// Read words from a string
void getWords(const std::string &str, std::vector<std::string> &words) {
  std::stringstream ss(str);
  std::string word;
  while (ss >> word) {
    words.push_back(word);
  }
}

// The code here is to retrieve the map :
//  offset -> instrumentation variable name

// The table is sorted by offset
void readInstrumentedVarTable(const std::string &filePath) {
  auto &tableEntries = getInstrumentationVarTableEntries();
  std::ifstream tableFile(filePath);
  std::string line;

  assert(tableFile.is_open());

  std::vector<std::string> words;

  while (std::getline(tableFile, line)) {
    getWords(line, words);
    InstrumentationVarTableEntry tableEntry(words);
    tableEntries.push_back(tableEntry);
    words.clear();
  }

  tableFile.close();
}

// This is used to retrieve the maps:
//   kernelName -> kernargBufferSize
//   kernelName -> firstHiddenArgIndex
//
// We extend the kernel signature to take an additional argument, which is the memory holding
// instrumentation variables. The map will be used to update the kernarg signature with a
// bigger kernarg buffer size, to accomodate for the additional argument.
void readPreloadInfo(const std::string &filePath) {
  auto &kernargSizeMap = getKernargSizeMap();
  auto &firstHiddenArgIndexMap = getFirstHiddenArgIndexMap();

  std::cerr << "readPreloadMaps : reading " << filePath << "\n";
  std::ifstream mapFile(filePath);
  std::string line;

  assert(mapFile.is_open());

  std::vector<std::string> words;


  std::cerr << " size : " << kernargSizeMap.size() << '\n';
  for(auto it : kernargSizeMap) {
    std::cerr << it.first << ' ' << it.second << '\n';
  }

  while (std::getline(mapFile, line)) {
    getWords(line, words);
    std::cerr << words.size() << '\n';
    assert(words.size() == 3); // (<kernel name> <kernarg size> <numArgs>)

    std::string kernelName = words[0];

    int kernargSize = std::stoi(words[1]);
    kernargSizeMap[kernelName] = kernargSize;

    int firstHiddenArgIndex = std::stoi(words[2]);
    firstHiddenArgIndexMap[kernelName] = firstHiddenArgIndex;

    words.clear();
  }
  mapFile.close();
}

typedef void (*registerFunc_t ) (
    void** modules,
    const void*  hostFunction,
    char*        deviceFunction,
    const char*  deviceName,
    unsigned int threadLimit,
    uint3*       tid,
    uint3*       bid,
    dim3*        blockDim,
    dim3*        gridDim,
    int*         wSize);

static registerFunc_t realRegisterFunction;

static std::unordered_map<const void *, std::string> addressToKernelName;

extern "C" void __hipRegisterFunction(
    void** modules,
    const void*  hostFunction,
    char*        deviceFunction,
    const char*  deviceName,
    unsigned int threadLimit,
    uint3*       tid,
    uint3*       bid,
    dim3*        blockDim,
    dim3*        gridDim,
    int*         wSize) {

  if(realRegisterFunction == 0) {
    realRegisterFunction = (registerFunc_t) dlsym(RTLD_NEXT,"__hipRegisterFunction");
    // Map address to kernel name
    addressToKernelName[hostFunction] = std::string(deviceFunction);
  }
  realRegisterFunction(modules,hostFunction,deviceFunction,deviceName,threadLimit,tid,bid,blockDim,gridDim,wSize);
  return;
}

typedef uint32_t (*launch_t)(const void *hostFunction, dim3 gridDim,
                             dim3 blockDim, void **args, size_t sharedMemBytes,
                             hipStream_t stream);
launch_t realLaunch = 0;

extern "C" hipError_t hipLaunchKernel(const void *hostFunction, dim3 gridDim,
                                      dim3 blockDim, void **args,
                                      size_t sharedMemBytes,
                                      hipStream_t stream) {

  if (realLaunch == 0) {
    realLaunch = (launch_t)dlsym(RTLD_NEXT, "hipLaunchKernel");
  }
  assert(realLaunch != 0);

  auto &kernargSizeMap = getKernargSizeMap();
  auto &instrumentationVarTableEntries = getInstrumentationVarTableEntries();

  // Step 0. Get kernel name
  auto iter = addressToKernelName.find(hostFunction);
  if (iter == addressToKernelName.end()) {
    std::cerr << "ERROR : kernel being launched wasn't registered by hipRegisterFunction\n"
              << "Doing regular launch...";

    realLaunch(hostFunction, gridDim, blockDim, args, sharedMemBytes, stream);

    // host code might unnecessarily get triggered if this isn't hipSuccess
    return hipSuccess;
  }

  std::string kernelName = iter->second;

  // Step 1. Check whether this is an instrumented kernel, i.e it should be in kernargSizeMapPath.
  // If not instrumented, just launch it.
  auto it = kernargSizeMap.find(kernelName);
  int kernargSize = it->second;
  if (it == kernargSizeMap.end()) {
    // Do regular launch
    std::cerr << kernelName << " is not instrumented. Doing regular launch\n";
    realLaunch(hostFunction, gridDim, blockDim, args, sharedMemBytes, stream);
    return hipSuccess;
  }

  // Step 2. Get size of instrumentation memory
  // TODO: Use size
  assert(!instrumentationVarTableEntries.empty());
  InstrumentationVarTableEntry lastEntry = *(instrumentationVarTableEntries.end() - 1);
  size_t allocSize = lastEntry.offset + 4;
  unsigned *instrumentationDataHost = (unsigned *)calloc(1, allocSize);

  std::cerr << '\n';

  unsigned *instrumentationDataDevice;

  hipError_t hip_ret = hipMalloc((void **)&instrumentationDataDevice, allocSize);
  assert(hip_ret == hipSuccess);

  hip_ret = hipMemset(instrumentationDataDevice, 0, allocSize);

  assert(hip_ret == hipSuccess);

  int newKernargSize = kernargSize + sizeof(void *);
  void **newArgs = (void **)malloc(newKernargSize);

  memcpy(newArgs, args, kernargSize);

  int newArgIndex = getFirstHiddenArgIndexMap()[kernelName];
  newArgs[newArgIndex] = (void *)(&instrumentationDataDevice);

  std::cerr << "Launching instrumented kernel : " << kernelName << '\n';

  auto start = std::chrono::high_resolution_clock::now();

  realLaunch(hostFunction, gridDim, blockDim, newArgs, sharedMemBytes, stream);
  assert(hipStreamSynchronize(stream) == hipSuccess);

  auto end = std::chrono::high_resolution_clock::now();

  std::chrono::duration<double, std::milli> elapsed = end - start;
  std::cout << "Runtime : " << elapsed.count() << " ms\n";

  std::cerr << "Kernel execution complete. Copying instrumentation variables to host...\n";

  hip_ret = hipMemcpy(instrumentationDataHost, instrumentationDataDevice, /* size = */ allocSize,
            hipMemcpyDeviceToHost);
  assert(hip_ret == hipSuccess);

  std::cerr << "Done.\n";
  std::cerr << "Instrumentation variable values: \n";
  for (auto entry : instrumentationVarTableEntries) {
    std::cerr << entry.name << " = " << instrumentationDataHost[entry.offset / 4] << '\n';
  }
  std::cerr << '\n';
  return hipSuccess;
}

__attribute__((constructor)) void setup(void) {
  realLaunch = 0;

  const char *kernargSizeMapPath = getenv(instrumentedKernelNamesEnv);
  if (!kernargSizeMapPath) {
    std::cerr << "LD_PRELOAD setup: " << instrumentedKernelNamesEnv << " not defined\n";
    exit(1);
  }

  readPreloadInfo(kernargSizeMapPath);

  const char *tableFilePath = getenv(instrumentationVariableTableEnv);
  if (!tableFilePath) {
    std::cerr << "LD_PRELOAD setup: " << instrumentationVariableTableEnv << " not defined\n";
    exit(1);
  }
  readInstrumentedVarTable(tableFilePath);
}
