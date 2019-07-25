#include <sycl/sycl_tools.h>

#include <SYCL/sycl.hpp>

cl::sycl::device SyclTools::findComputeDevice(const std::string& deviceType) {
	cl::sycl::device result;
	if (deviceType.compare("CPU")) {
		result = cl::sycl::host_selector().select_device();
	} else if (deviceType.compare("GPU")) {
		result = cl::sycl::gpu_selector().select_device();
	} else if (deviceType.compare("HOST")) {
		result = cl::sycl::host_selector().select_device();
	} else {
		result = cl::sycl::host_selector().select_device();
	}
	return result;
}
