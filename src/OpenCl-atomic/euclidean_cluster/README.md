* Preparation
  Extract the test case data files
  * ec_input.dat.gz
  * ec_output.dat.gz
  inside the data folder

* Compilation

  Navigate to src/OpenCl/euclidean_cluster:

  $ make

  Optional arguments are:
  * OPENCL_PLATFORM_ID - to select a specific platform
    - can be a platform index e.g. 1 or a identifiying string e.g. AMD
  * OPENCL_DEVICE_ID - to select a specific device by index or name
    - can be an index or an identifiying string e.g. RX 480
  * OPENCL_DEVICE_TYPE - to limit device selection to a specific device type
    - can be CPU, GPU, ACC or DEFAULT
    - for CPUs only, GPUs only, other accelerator types only or default device selection respectively
  * OPENCL_INCLUDE_PATH - if the OpenCL headers are not available at the default locations
    - folder that contains CL/cl.h
  * OPENCL_LIBRARY_PATH - if the OpenCL library is not available at the default locations
    - folder that contains libOpenCL.so or similar

  For example if we wanted to select our Nvidia RTX 2070 graphics card we could type:

  $ make OPENCL_DEVICE_ID=RTX

  to select the first graphics card which name contains "RTX"
  By default the first available OpenCL capable device is selected

* Running the benchmark

  In the src/OpenCl/euclidean_cluster folder:

  $ ./kernel

  This will inform us about:
  * whether the test data could be found
  * the OpenCL device selected
  * problems during the OpenCL setup phase
  * kernel runtime
  * deviation from the reference data