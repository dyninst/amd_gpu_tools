# AMDGPU Tools for Dyninst

The Dyninst mutator can currently only rewrite the GPU ELF binary, but doesn't rewrite the metadata in the GPU binary.

These tools are used alongside the Dyninst mutator for all the additional tasks. This includes the following:
1. Extracting and embedding the fat binary in the host executable
2. Extracting and embedding the GPU ELF binary in the fat binary
3. Rewriting metadata in the instrumented GPU binary
4. Using a preload library to pass additional argument for kernel launch

These tools are tested and developed on ROCm 6.0.0 and GFX908.

## Building
	```
	cmake /path/to/amd_gpu_tools -DROCM_PATH=/path/to/rocm/install
	```

## Running

  ```
  instr-driver <dyninst-mutator> <host-executable>
  ```

The host executable contains the host code and the fat binary which contains device code.

Ensure that the build directory for these tools is appended to `PATH`
