/**
 * Author:  Florian Stock, Technische Universität Darmstadt,
 * Embedded Systems & Applications Group 2018
 * Author:  Leonardo Solis, Technische Universität Darmstadt,
 * Embedded Systems & Applications Group 2018
 * Author:  Thilo Gabel, Technische Universität Darmstadt,
 * Embedded Systems & Applications Group 2019
 * License: Apache 2.0 (see attachached File)
 */


#include <cmath>
#include <iostream>
#include <sstream>
#include <fstream>
#include <limits>
#include <cstring>
#include <chrono>
#include <stdexcept>

#include "ndt_mapping.h"
#include "datatypes.h"
#include "kernel/kernel.h"

#include "common/benchmark.h"
#include "common/compute_tools.h"



ndt_mapping::ndt_mapping() :
	read_testcases(0),
	input_file(),
	output_file(),
#if EPHOS_DATAGEN
	datagen_file(),
#endif
	error_so_far(false), max_delta(0.0),
	outlier_ratio_(0.55), resolution_(1.0), trans_eps_(0.01), step_size_(0.1),
	// TODO: check whether this is supposed to be 0 or 30
	iter(30), max_iterations_(30),
	previous_transformation_(), transformation_(), final_transformation_(),
	intermediate_transformations_(),
	gauss_d1_(0.0), gauss_d2_(0.0),
	point_gradient_(),
	point_hessian_(),
	h_ang_a2_(), h_ang_a3_(),
	h_ang_b2_(), h_ang_b3_(),
	h_ang_c2_(), h_ang_c3_(),
	h_ang_d1_(), h_ang_d2_(), h_ang_d3_(),
	h_ang_e1_(), h_ang_e2_(), h_ang_e3_(),
	h_ang_f1_(), h_ang_f2_(), h_ang_f3_(),
	j_ang_a_(), j_ang_b_(), j_ang_c_(), j_ang_d_(), j_ang_e_(), j_ang_f_(), j_ang_g_(), j_ang_h_(),
	transformation_probability_(0.0), transformation_epsilon_(0.1),
	filtered_scan(), maps(), init_guess(), results(), grids(),
	input_cloud(nullptr), target_cloud(nullptr),
	minVoxel(), maxVoxel(), voxelDimension(),
	computeEnv(),
	voxelGridBuffer(),
	pointCloudBuffer(),
	subvoxelBuffer(),
	counterBuffer(),
	gridInfoBuffer(),
#ifdef EPHOS_PINNED_MEMORY
	subvoxelHostBuffer(),
#elif defined(EPHOS_ZERO_COPY)
#else
	subvoxelStorage(nullptr),
#endif
	measureCloudKernel(),
	radiusSearchKernel(),
	initTargetCellsKernel(),
	firstPassKernel(),
	secondPassKernel(),
	maxComputeGridSize(0),
	maxComputeCloudSize(0) {
}


inline int ndt_mapping::linearizeAddr(const int x, const int y, const int z)
{
	return  (x + voxelDimension[0] * (y + voxelDimension[1] * z));
}

inline int ndt_mapping::linearizeCoord(const float x, const float y, const float z)
{
	int idx_x = (x - minVoxel.data[0]) / resolution_;
	int idx_y = (y - minVoxel.data[1]) / resolution_;
	int idx_z = (z - minVoxel.data[2]) / resolution_;
	return linearizeAddr(idx_x, idx_y, idx_z);
}

/**
 * Helper function to calculate the dot product of two vectors.
 */
double dot_product(Vec3 &a, Vec3 &b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/**
 * Helper function to calculate the dot product of two vectors.
 */
double dot_product6(Vec6 &a, Vec6 &b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] + a[4] * b[4] + a[5] * b[5];
}

/**
 * Helper function for offset point generation.
 */
inline double auxilaryFunction_dPsiMT (double g_a, double g_0, double mu = 1.e-4) {
	return (g_a - mu * g_0);
}

/**
 * Helper function for difference offset generation.
 */
inline double auxilaryFunction_PsiMT (double a, double f_a, double f_0, double g_0, double mu = 1.e-4)
{
    return (f_a - f_0 - mu * g_0 * a);
}


/**
 * Solves Ax = b for x.
 * Maybe not as good when handling very ill conditioned systems, but is faster for a 6x6 matrix 
 * and works well enough in practice.
 */
void solve(Vec6& result, Mat66 A, Vec6& b)
{
	double pivot;

	// bring to upper diagonal
	for(int j = 0; j < 6; j++)
	{
		double max = fabs(A.data[j][j]);
		int mi = j;
		for (int i = j + 1; i < 6; i++)
			if (fabs(A.data[i][j]) > max)
			{
				mi = i;
				max = fabs(A.data[i][j]);
			}
		// swap lines mi and j
		if (mi !=j)
			for (int i = 0; i < 6; i++)
			{
				double temp = A.data[mi][i];
				A.data[mi][i] = A.data[j][i];
				A.data[j][i] = temp;
			}
		if (max == 0.0) {
			// singular matrix
			A.data[j][j] = MAX_TRANSLATION_EPS;
		}
		// subtract lines to yield a triagonal matrix
		for (int i = j+1; i < 6; i++)
		{
			pivot=A.data[i][j]/A.data[j][j];
			for(int k = 0; k < 6; k++)
			{
				A.data[i][k]=A.data[i][k]-pivot*A.data[j][k];
			}
			b[i]=b[i]-pivot*b[j];
		}
	}
	// backward substituion
	result[5]=b[5]/A.data[5][5];
	for( int i = 4; i >= 0; i--)
	{
		double sum=0.0;
		for(int j = i+1; j < 6; j++)
		{
			sum=sum+A.data[i][j]*result[j];
		}
		result[i]=(b[i]-sum)/A.data[i][i];
	}
}

void ndt_mapping::init() {
	std::cout << "init\n";
	// open data file streams
	input_file.exceptions ( std::ifstream::failbit | std::ifstream::badbit );
	output_file.exceptions ( std::ifstream::failbit | std::ifstream::badbit );
	try {
		input_file.open("../../../data/ndt_input.dat", std::ios::binary);
	} catch (std::ifstream::failure) {
		std::cerr << "Error opening the testcase file" << std::endl;
		exit(-3);
	}
	try {
		output_file.open("../../../data/ndt_output.dat", std::ios::binary);
	}  catch (std::ifstream::failure e) {
		std::cerr << "Error opening the results file" << std::endl;
		exit(-3);
	}
#ifdef EPHOS_DATAGEN
	try {
		datagen_file.open("../../../data/ndt_output_gen.dat", std::ios::binary);
	} catch (std::ofstream::failure & e) {
		std::cerr << "Error opening the datagen file" << std::endl;
		exit(-3);
	}
#endif // EPHOS_DATAGEN
	// consume the number of testcases from the testcase file
	try {
		testcases = read_number_testcases(input_file);
	} catch (std::ios_base::failure& e) {
		std::cerr << e.what() << std::endl;
		exit(-3);
	}
#ifdef EPHOS_TESTCASE_LIMIT
	if (EPHOS_TESTCASE_LIMIT < testcases) {
		testcases = EPHOS_TESTCASE_LIMIT;
	}
#endif
	// prepare the first iteration
	error_so_far = false;
	max_delta = 0.0;
	input_cloud = nullptr;
	target_cloud = nullptr;
	maps.clear();
	init_guess.clear();
	filtered_scan.clear();
	results.clear();

	// opencl setup
	try {
		std::vector<std::vector<std::string>> requiredExtensions = {
			{"cl_khr_fp64", "cl_amd_fp64"}
		};
		computeEnv = ComputeTools::find_compute_platform(EPHOS_PLATFORM_HINT_S, EPHOS_DEVICE_HINT_S,
			EPHOS_DEVICE_TYPE_S, requiredExtensions);
		std::cout << "OpenCL device: " << computeEnv.device.getInfo<CL_DEVICE_NAME>() << std::endl;
	} catch (std::logic_error& e) {
		std::cerr << e.what() << std::endl;
		exit(EXIT_FAILURE);
	}
	// Kernel code was stringified, rather than read from file
	std::string sourceCode = voxel_grid_ocl_kernel_source;

	//cl::Program::Sources sourcesCL = cl::Program::Sources(1, std::make_pair(sourceCode.c_str(), sourceCode.size()));
	cl::Program::Sources sourcesCL;
	std::vector<cl::Kernel> kernels;
	try {
		std::string sOptions =
#ifdef EPHOS_KERNEL_VOXEL_POINT_STORAGE
			" -DEPHOS_VOXEL_POINT_STORAGE=" STRINGIFY(EPHOS_KERNEL_VOXEL_POINT_STORAGE)
#else
			""
#endif
		;
		std::vector<std::string> kernelNames({
			"measureCloud",
			"initTargetCells",
			"firstPass",
			"secondPass",
			"radiusSearch"
		});
		cl::Program program = ComputeTools::build_program(computeEnv, sourceCode, sOptions,
			kernelNames, kernels);
	} catch (std::logic_error& e) {
		std::cerr << e.what() << std::endl;
		exit(EXIT_FAILURE);
	}
	measureCloudKernel = kernels[0];
	initTargetCellsKernel = kernels[1];
	firstPassKernel = kernels[2];
	secondPassKernel = kernels[3];
	radiusSearchKernel = kernels[4];

	std::cout << "done" << std::endl;
}
void ndt_mapping::quit() {
#ifdef EPHOS_PINNED_MEMORY
	if (subvoxelStorage != nullptr) {
		computeEnv.cmdqueue.enqueueUnmapMemObject(subvoxelHostBuffer, subvoxelStorage);
		subvoxelStorage = nullptr;
	}
#elif defined(EPHOS_ZERO_COPY)
#else
	if (subvoxelStorage != nullptr) {
		delete[] subvoxelStorage;
		subvoxelStorage = nullptr;
	}
#endif
	input_file.close();
	output_file.close();
#ifdef EPHOS_DATAGEN
	datagen_file.close();
#endif
}

/**
 * Applies the transformation matrix to all point cloud elements
 * input: points to be transformed
 * output: transformed points
 * transform: transformation matrix
 */
void transformPointCloud(const PointCloud& input, PointCloud &output, Matrix4f transform)
{
	if (&input != &output)
	{
		output.clear();
		output.resize(input.size());
	}
	for (auto it = 0 ; it < input.size(); ++it)
	{
		PointXYZI transformed;
		for (int row = 0; row < 3; row++)
		{
			transformed.data[row] = transform.data[row][0] * input[it].data[0]
			+ transform.data[row][1] * input[it].data[1]
			+ transform.data[row][2] * input[it].data[2]
			+ transform.data[row][3];
		}
		output[it] = transformed;
	}
}


double ndt_mapping::updateDerivatives (Vec6 &score_gradient,
				       Mat66 &hessian,
				       Vec3 &x_trans, Mat33 &c_inv,
				       bool compute_hessian)
{
	Vec3 cov_dxd_pi;

	// matrix preparation
	double xCx = c_inv.data[0][0] * x_trans[0] * x_trans[0] +
	c_inv.data[1][1] * x_trans[1] * x_trans[1] +
	c_inv.data[2][2] * x_trans[2] * x_trans[2] +
	(c_inv.data[0][1] + c_inv.data[1][0]) * x_trans[0] * x_trans[1] +
	(c_inv.data[0][2] + c_inv.data[2][0]) * x_trans[0] * x_trans[2] +
	(c_inv.data[1][2] + c_inv.data[2][1]) * x_trans[1] * x_trans[2];

	double e_x_cov_x = exp (-gauss_d2_ * (xCx) / 2);
	// Calculate probability of transtormed points existance, Equation 6.9 [Magnusson 2009]
	double score_inc = -gauss_d1_ * e_x_cov_x;
	e_x_cov_x = gauss_d2_ * e_x_cov_x;
	// Error checking for invalid values.
	if (e_x_cov_x > 1 || e_x_cov_x < 0 || e_x_cov_x != e_x_cov_x)
		return (0);
	// Reusable portion of Equation 6.12 and 6.13 [Magnusson 2009]
	e_x_cov_x *= gauss_d1_;
	for (int i = 0; i < 6; i++)
	{
		// Sigma_k^-1 d(T(x,p))/dpi, Reusable portion of Equation 6.12 and 6.13 [Magnusson 2009]
		//cov_dxd_pi = c_inv * point_gradient_.col (i);
		for (int row = 0; row < 3; row++)
		{
			cov_dxd_pi[row] = 0;
			for (int col = 0; col < 3; col++)
			cov_dxd_pi[row] += c_inv.data[row][col] * point_gradient_.data[col][i];
		}
		// Update gradient, Equation 6.12 [Magnusson 2009]
		score_gradient[i] += dot_product(x_trans, cov_dxd_pi) * e_x_cov_x;
		if (compute_hessian)
		{
			for (int j = 0; j < 6; j++)
			{
				Vec3 colVec = { point_gradient_.data[0][j], point_gradient_.data[1][j], point_gradient_.data[2][j] };
				Vec3 colVecHess = {colVec[0] + point_hessian_.data[3*i][j], colVec[1] + point_hessian_.data[3*i+1][j], colVec[2] + point_hessian_.data[3*i+2][j] };
				Vec3 matProd;
				for (int row = 0; row < 3; row++)
				{
					matProd[row] = 0;
					for (int col = 0; col < 3; col++)
					matProd[row] += c_inv.data[row][col] * colVecHess[col];
				}
				// Update hessian, Equation 6.13 [Magnusson 2009]
				hessian.data[i][j] += e_x_cov_x * (-gauss_d2_ * dot_product(x_trans, cov_dxd_pi) *
									dot_product(x_trans, matProd) +
									dot_product( colVec, cov_dxd_pi) );
			}
		}
	}

	return (score_inc);
}

void ndt_mapping::computePointDerivatives (Vec3 &x, bool compute_hessian)
{
	// Calculate first derivative of Transformation Equation 6.17 w.r.t. transform vector p.
	// Derivative w.r.t. ith element of transform vector corresponds to column i, Equation 6.18 and 6.19 [Magnusson 2009]
	point_gradient_.data[1][3] = dot_product(x, j_ang_a_);
	point_gradient_.data[2][3] = dot_product(x, j_ang_b_);
	point_gradient_.data[0][4] = dot_product(x, j_ang_c_);
	point_gradient_.data[1][4] = dot_product(x, j_ang_d_);
	point_gradient_.data[2][4] = dot_product(x, j_ang_e_);
	point_gradient_.data[0][5] = dot_product(x, j_ang_f_);
	point_gradient_.data[1][5] = dot_product(x, j_ang_g_);
	point_gradient_.data[2][5] = dot_product(x, j_ang_h_);

	if (compute_hessian)
	{
		//equation 6.21 [Magnusson 2009]
		Vec3 a, b, c, d, e, f;
		a[0] = 0;
		a[1] = dot_product(x, h_ang_a2_);
		a[2] = dot_product(x, h_ang_a3_);
		b[0] = 0;
		b[1] = dot_product(x, h_ang_b2_);
		b[2] = dot_product(x, h_ang_b3_);
		c[0] = 0;
		c[1] = dot_product(x, h_ang_c2_);
		c[2] = dot_product(x, h_ang_c3_);
		d[0] = dot_product(x, h_ang_d1_);
		d[1] = dot_product(x, h_ang_d2_);
		d[2] = dot_product(x, h_ang_d3_);
		e[0] = dot_product(x, h_ang_e1_);
		e[1] = dot_product(x, h_ang_e2_);
		e[2] = dot_product(x, h_ang_e3_);
		f[0] = dot_product(x, h_ang_f1_);
		f[1] = dot_product(x, h_ang_f2_);
		f[2] = dot_product(x, h_ang_f3_);
		// second derivative of Transformation Equation 6.17 w.r.t. transform vector p.
		// Derivative w.r.t. ith and jth elements of transform vector corresponds to the 3x1 block matrix starting at (3i,j), Equation 6.20 and 6.21 [Magnusson 2009]
		point_hessian_.data[9][3] = a[0];
		point_hessian_.data[10][3] = a[1];
		point_hessian_.data[11][3] = a[2];
		point_hessian_.data[12][3] = b[0];
		point_hessian_.data[13][3] = b[1];
		point_hessian_.data[14][3] = b[2];
		point_hessian_.data[15][3] = c[0];
		point_hessian_.data[16][3] = c[1];
		point_hessian_.data[17][3] = c[2];
		point_hessian_.data[9][4] = b[0];
		point_hessian_.data[10][4] = b[1];
		point_hessian_.data[11][4] = b[2];
		point_hessian_.data[12][4] = d[0];
		point_hessian_.data[13][4] = d[1];
		point_hessian_.data[14][4] = d[2];
		point_hessian_.data[15][4] = e[0];
		point_hessian_.data[16][4] = e[1];
		point_hessian_.data[17][4] = e[2];
		point_hessian_.data[9][5] = c[0];
		point_hessian_.data[10][5] = c[1];
		point_hessian_.data[11][5] = c[2];
		point_hessian_.data[12][5] = e[0];
		point_hessian_.data[13][5] = e[1];
		point_hessian_.data[14][5] = e[2];
		point_hessian_.data[15][5] = f[0];
		point_hessian_.data[16][5] = f[1];
		point_hessian_.data[17][5] = f[2];
	}
}

void ndt_mapping::computeHessian(
	Mat66 &hessian, PointCloud &trans_cloud, Vec6 &)
{
	// TODO: test unused code here
	memset(&(hessian.data[0][0]), 0, sizeof(double) * 6 * 6);
	// move transformed cloud to device
	int pointNo = trans_cloud.size();
	size_t nbytes_cloud = sizeof(PointXYZI)*pointNo;
	computeEnv.cmdqueue.enqueueWriteBuffer(pointCloudBuffer, CL_FALSE, 0, nbytes_cloud,
		trans_cloud.data());
	int nearVoxelNo = 0;
	computeEnv.cmdqueue.enqueueWriteBuffer(counterBuffer, CL_FALSE, 0, sizeof(int), &nearVoxelNo);
	computeEnv.cmdqueue.enqueueWriteBuffer(gridInfoBuffer, CL_FALSE, 0, sizeof(int), &pointNo);
	// call radius search kernel
	size_t local_size = EPHOS_KERNEL_WORK_GROUP_SIZE;
	size_t num_workgroups = pointNo/local_size + 1;
	size_t global_size = local_size*num_workgroups;
	computeEnv.cmdqueue.enqueueNDRangeKernel(
		radiusSearchKernel,
		cl::NDRange(0),
		cl::NDRange(global_size),
		cl::NDRange(local_size));
	// move near voxels to host
	computeEnv.cmdqueue.enqueueReadBuffer(counterBuffer, CL_TRUE, 0, sizeof(int), &nearVoxelNo);
	size_t nbytes_subvoxel = sizeof(PointVoxel)*nearVoxelNo;
	PointVoxel* storage_subvoxel = (PointVoxel*)computeEnv.cmdqueue.enqueueMapBuffer(subvoxelBuffer,
		CL_TRUE, CL_MAP_READ, 0, nbytes_subvoxel);
	// process near voxels
	for (int i = 0; i < nearVoxelNo; i++) {
		int iPoint = storage_subvoxel[i].point;
		PointXYZI& x_pt = input_cloud->at(iPoint);
		Vec3 x = {
			x_pt.data[0],
			x_pt.data[1],
			x_pt.data[2]
		};
		computePointDerivatives(x);
		VoxelMean* mean = &storage_subvoxel[i].mean;
		PointXYZI& x_trans_pt = trans_cloud.at(iPoint);
		Vec3 x_trans = {
			x_trans_pt.data[0] - mean->data[0],
			x_trans_pt.data[1] - mean->data[1],
			x_trans_pt.data[2] - mean->data[2]
		};
		Mat33& c_inv = storage_subvoxel[i].invCovariance;
		updateHessian(hessian, x_trans, c_inv);
	}
	computeEnv.cmdqueue.enqueueUnmapMemObject(subvoxelBuffer, storage_subvoxel);
}

void ndt_mapping::updateHessian (Mat66 &hessian, Vec3 &x_trans, Mat33 &c_inv)
{
	Vec3 cov_dxd_pi;
	// Equation 6.9 [Magnusson 2009]
	double xCx = c_inv.data[0][0] * x_trans[0] * x_trans[0] +
		c_inv.data[1][1] * x_trans[1] * x_trans[1] +
		c_inv.data[2][2] * x_trans[2] * x_trans[2] +
		(c_inv.data[0][1] + c_inv.data[1][0]) * x_trans[0] * x_trans[1] +
		(c_inv.data[0][2] + c_inv.data[2][0]) * x_trans[0] * x_trans[2] +
		(c_inv.data[1][2] + c_inv.data[2][1]) * x_trans[1] * x_trans[2];
	double e_x_cov_x = gauss_d2_ * exp (-gauss_d2_ * (xCx) / 2);

	// Error checking for invalid values.
	if (e_x_cov_x > 1 || e_x_cov_x < 0 || e_x_cov_x != e_x_cov_x)
		return;
	// Equation 6.12 and 6.13 [Magnusson 2009]
	e_x_cov_x *= gauss_d1_;

	for (int i = 0; i < 6; i++)
	{
		// Equation 6.12 and 6.13 [Magnusson 2009]
		for (int row = 0; row < 3; row++)
		{
			cov_dxd_pi[row] = 0;
			for (int col = 0; col < 3; col++)
			cov_dxd_pi[row] += c_inv.data[row][col] * point_gradient_.data[col][i];
		}
		
	for (int j = 0; j < 6; j++)
	{
		// Update hessian, Equation 6.13 [Magnusson 2009]
		Vec3 colVec = { point_gradient_.data[0][j], point_gradient_.data[1][j], point_gradient_.data[2][j] };
		Vec3 colVecHess = {colVec[0] + point_hessian_.data[3*i][j], colVec[1] + point_hessian_.data[3*i+1][j], colVec[2] + point_hessian_.data[3*i+2][j] };
		Vec3 matProd;
		for (int row = 0; row < 3; row++)
		{
			matProd[row] = 0;
			for (int col = 0; col < 3; col++)
				matProd[row] += c_inv.data[row][col] * colVecHess[col];
		}
		hessian.data[i][j] += e_x_cov_x * (-gauss_d2_ * dot_product(x_trans, cov_dxd_pi) *
							dot_product(x_trans, matProd) +
							dot_product( colVec, cov_dxd_pi) );
		}
	}
}

double ndt_mapping::computeDerivatives (
	Vec6 &score_gradient,
	Mat66 &hessian,
	PointCloudSource &trans_cloud,
	Vec6 &p,
	bool compute_hessian)
{
	memset(&(score_gradient[0]), 0, sizeof(double) * 6 );
	memset(&(hessian.data[0][0]), 0, sizeof(double) * 6 * 6);
	double score = 0.0;
	// Precompute Angular Derivatives (eq. 6.19 and 6.21)[Magnusson 2009]
	computeAngleDerivatives (p);
	// move transformed cloud to device
	int pointNo = trans_cloud.size();
#ifdef EPHOS_ZERO_COPY
	PointXYZI* pointCloudStorage = (PointXYZI*)computeEnv.cmdqueue.enqueueMapBuffer(pointCloudBuffer,
		CL_TRUE, CL_MAP_WRITE, 0, sizeof(PointXYZI)*pointNo);
	std::memcpy(pointCloudStorage, trans_cloud.data(), sizeof(PointXYZI)*pointNo);
	computeEnv.cmdqueue.enqueueUnmapMemObject(pointCloudBuffer, pointCloudStorage);
#else
	computeEnv.cmdqueue.enqueueWriteBuffer(pointCloudBuffer, CL_FALSE,
		0, sizeof(PointXYZI)*pointNo, trans_cloud.data());
#endif
	int nearVoxelNo = 0;
	computeEnv.cmdqueue.enqueueWriteBuffer(counterBuffer, CL_FALSE, 0, sizeof(int), &nearVoxelNo);
	computeEnv.cmdqueue.enqueueWriteBuffer(gridInfoBuffer, CL_FALSE, 0, sizeof(int), &pointNo);
	// call radius search kernel
	//radiusSearchKernel.setArg(7, pointNo);
	size_t local_size = EPHOS_KERNEL_WORK_GROUP_SIZE;
	size_t num_workgroups = pointNo/local_size + 1;
	size_t global_size = local_size*num_workgroups;
	try {
	computeEnv.cmdqueue.enqueueNDRangeKernel(
		radiusSearchKernel,
		cl::NDRange(0),
		cl::NDRange(global_size),
		cl::NDRange(local_size));
	// move near voxels to host
	computeEnv.cmdqueue.enqueueReadBuffer(counterBuffer, CL_TRUE, 0, sizeof(int), &nearVoxelNo);
	} catch (cl::Error& e) {
		std::cout << e.what() << std::endl;
		exit(-2);
	}
#ifdef EPHOS_ZERO_COPY
	PointVoxel* subvoxelStorage = (PointVoxel*)computeEnv.cmdqueue.enqueueMapBuffer(subvoxelBuffer,
		CL_TRUE, CL_MAP_READ, 0, sizeof(PointVoxel)*nearVoxelNo);
#else
	computeEnv.cmdqueue.enqueueReadBuffer(subvoxelBuffer, CL_TRUE,
		0, sizeof(PointVoxel)*nearVoxelNo, subvoxelStorage);
#endif
	// process near voxels
	for (int i = 0; i < nearVoxelNo; i++) {
		int iPoint = subvoxelStorage[i].point;
		PointXYZI& x_pt = input_cloud->at(iPoint);
		Vec3 x = {
			x_pt.data[0],
			x_pt.data[1],
			x_pt.data[2]
		};
		computePointDerivatives(x);
		VoxelMean* mean = &subvoxelStorage[i].mean;
		PointXYZI& x_trans_pt = trans_cloud.at(iPoint);
		Vec3 x_trans = {
			x_trans_pt.data[0] - mean->data[0],
			x_trans_pt.data[1] - mean->data[1],
			x_trans_pt.data[2] - mean->data[2]
		};
		Mat33& c_inv = subvoxelStorage[i].invCovariance;
		score += updateDerivatives(score_gradient, hessian, x_trans, c_inv, compute_hessian);
	}
#ifdef EPHOS_ZERO_COPY
	computeEnv.cmdqueue.enqueueUnmapMemObject(subvoxelBuffer, subvoxelStorage);
#endif
	return score;
}

void ndt_mapping::computeAngleDerivatives (Vec6 &p, bool compute_hessian)
{
	// Simplified math for near 0 angles
	double cx, cy, cz, sx, sy, sz;
	if (std::fabs (p[3]) < 10e-5)
	{
		//p(3) = 0;
		cx = 1.0;
		sx = 0.0;
	}
	else
	{
		cx = cos (p[3]);
		sx = sin (p[3]);
	}
	if (std::fabs (p[4]) < 10e-5)
	{
		//p(4) = 0;
		cy = 1.0;
		sy = 0.0;
	}
	else
	{
		cy = cos (p[4]);
		sy = sin (p[4]);
	}

	if (std::fabs (p[5]) < 10e-5)
	{
		//p(5) = 0;
		cz = 1.0;
		sz = 0.0;
	}
	else
	{
		cz = cos (p[5]);
		sz = sin (p[5]);
	}
	// Precomputed angular gradiant components. Letters correspond to Equation 6.19 [Magnusson 2009]
	j_ang_a_[0] = (-sx * sz + cx * sy * cz);
	j_ang_a_[1] = (-sx * cz -  cx * sy * sz);
	j_ang_a_[2] = (-cx * cy);
	j_ang_b_[0] = (cx * sz + sx * sy * cz);
	j_ang_b_[1] = (cx * cz - sx * sy * sz);
	j_ang_b_[2] = (-sx * cy);
	j_ang_c_[0] =  (-sy * cz);
	j_ang_c_[1] = sy * sz;
	j_ang_c_[2] = cy;
	j_ang_d_[0] = sx * cy * cz;
	j_ang_d_[1] = (-sx * cy * sz);
	j_ang_d_[2] = sx * sy;
	j_ang_e_[0] = (-cx * cy * cz);
	j_ang_e_[1] = cx * cy * sz;
	j_ang_e_[2] = (-cx * sy);
	j_ang_f_[0] = (-cy * sz);
	j_ang_f_[1] = (-cy * cz);
	j_ang_f_[2] = 0;
	j_ang_g_[0] = (cx * cz - sx * sy * sz);
	j_ang_g_[1] = (-cx * sz - sx * sy * cz);
	j_ang_g_[2] = 0;
	j_ang_h_[0] = (sx * cz + cx * sy * sz);
	j_ang_h_[1] =(cx * sy * cz - sx * sz);
	j_ang_h_[2] = 0;

	if (compute_hessian)
	{
		// Precomputed angular hessian components. Letters correspond to Equation 6.21 and numbers correspond to row index [Magnusson 2009]
		h_ang_a2_[0] = (-cx * sz - sx * sy * cz);
		h_ang_a2_[1] =  (-cx * cz + sx * sy * sz);
		h_ang_a2_[2] = sx * cy;
		h_ang_a3_[0] =  (-sx * sz + cx * sy * cz);
		h_ang_a3_[1] = (-cx * sy * sz - sx * cz);
		h_ang_a3_[2] = (-cx * cy);
		
		h_ang_b2_[0] = (cx * cy * cz);
		h_ang_b2_[1] = (-cx * cy * sz);
		h_ang_b2_[2] = (cx * sy);
		h_ang_b3_[0] = (sx * cy * cz);
		h_ang_b3_[1] = (-sx * cy * sz);
		h_ang_b3_[2] = (sx * sy);
		
		h_ang_c2_[0] = (-sx * cz - cx * sy * sz);
		h_ang_c2_[1] = (sx * sz - cx * sy * cz);
		h_ang_c2_[2] = 0;
		h_ang_c3_[0] = (cx * cz - sx * sy * sz);
		h_ang_c3_[1] = (-sx * sy * cz - cx * sz);
		h_ang_c3_[2] = 0;
		
		h_ang_d1_[0] = (-cy * cz);
		h_ang_d1_[1] = (cy * sz);
		h_ang_d1_[2] = (sy);
		h_ang_d2_[0] =  (-sx * sy * cz);
		h_ang_d2_[1] = (sx * sy * sz);
		h_ang_d2_[2] = (sx * cy);
		h_ang_d3_[0] = (cx * sy * cz);
		h_ang_d3_[1] = (-cx * sy * sz);
		h_ang_d3_[2] = (-cx * cy);
		
		h_ang_e1_[0] = (sy * sz);
		h_ang_e1_[1] = (sy * cz);
		h_ang_e1_[2] = 0;
		h_ang_e2_[0] =  (-sx * cy * sz);
		h_ang_e2_[1] = (-sx * cy * cz);
		h_ang_e2_[2] = 0;
		h_ang_e3_[0] = (cx * cy * sz);
		h_ang_e3_[1] = (cx * cy * cz);
		h_ang_e3_[2] = 0;
		
		h_ang_f1_[0] = (-cy * cz);
		h_ang_f1_[1] = (cy * sz);
		h_ang_f1_[2] = 0;
		h_ang_f2_[0] = (-cx * sz - sx * sy * cz);
		h_ang_f2_[1] = (-cx * cz + sx * sy * sz);
		h_ang_f2_[2] = 0;
		h_ang_f3_[0] = (-sx * sz + cx * sy * cz);
		h_ang_f3_[1] = (-cx * sy * sz - sx * cz);
		h_ang_f3_[2] = 0;
	}
}

bool
ndt_mapping::updateIntervalMT (
	double &a_l, double &f_l, double &g_l,
	double &a_u, double &f_u, double &g_u,
	double a_t, double f_t, double g_t)
{
	// Case U1 in Update Algorithm and Case a in Modified Update Algorithm [More, Thuente 1994]
	if (f_t > f_l)
	{
		a_u = a_t;
		f_u = f_t;
		g_u = g_t;
		return (false);
	}
	// Case U2 in Update Algorithm and Case b in Modified Update Algorithm [More, Thuente 1994]
	else
	if (g_t * (a_l - a_t) > 0)
	{
		a_l = a_t;
		f_l = f_t;
		g_l = g_t;
		return (false);
	}
	// Case U3 in Update Algorithm and Case c in Modified Update Algorithm [More, Thuente 1994]
	else
	if (g_t * (a_l - a_t) < 0)
	{
		a_u = a_l;
		f_u = f_l;
		g_u = g_l;

		a_l = a_t;
		f_l = f_t;
		g_l = g_t;
		return (false);
	}
	// Interval Converged
	else
		return (true);
}

double
ndt_mapping::trialValueSelectionMT (
	double a_l, double f_l, double g_l,
	double a_u, double f_u, double g_u,
	double a_t, double f_t, double g_t)
{
	// Case 1 in Trial Value Selection [More, Thuente 1994]
	if (f_t > f_l)
	{
		// Calculate the minimizer of the cubic that interpolates f_l, f_t, g_l and g_t
		// Equation 2.4.52 [Sun, Yuan 2006]
		double z = 3 * (f_t - f_l) / (a_t - a_l) - g_t - g_l;
		double w = sqrt (z * z - g_t * g_l);
		// Equation 2.4.56 [Sun, Yuan 2006]
		double a_c = a_l + (a_t - a_l) * (w - g_l - z) / (g_t - g_l + 2 * w);

		// Calculate the minimizer of the quadratic that interpolates f_l, f_t and g_l
		// Equation 2.4.2 [Sun, Yuan 2006]
		double a_q = a_l - 0.5 * (a_l - a_t) * g_l / (g_l - (f_l - f_t) / (a_l - a_t));

		if (fabs (a_c - a_l) < fabs (a_q - a_l))
		return (a_c);
		else
		return (0.5 * (a_q + a_c));
	}
	// Case 2 in Trial Value Selection [More, Thuente 1994]
	else
	if (g_t * g_l < 0)
	{
		// Calculate the minimizer of the cubic that interpolates f_l, f_t, g_l and g_t
		// Equation 2.4.52 [Sun, Yuan 2006]
		double z = 3 * (f_t - f_l) / (a_t - a_l) - g_t - g_l;
		double w = sqrt (z * z - g_t * g_l);
		// Equation 2.4.56 [Sun, Yuan 2006]
		double a_c = a_l + (a_t - a_l) * (w - g_l - z) / (g_t - g_l + 2 * w);

		// Calculate the minimizer of the quadratic that interpolates f_l, g_l and g_t
		// Equation 2.4.5 [Sun, Yuan 2006]
		double a_s = a_l - (a_l - a_t) / (g_l - g_t) * g_l;

		if (fabs (a_c - a_t) >= fabs (a_s - a_t))
		return (a_c);
		else
		return (a_s);
	}
	// Case 3 in Trial Value Selection [More, Thuente 1994]
	else
	if (fabs (g_t) <= fabs (g_l))
	{
		// Calculate the minimizer of the cubic that interpolates f_l, f_t, g_l and g_t
		// Equation 2.4.52 [Sun, Yuan 2006]
		double z = 3 * (f_t - f_l) / (a_t - a_l) - g_t - g_l;
		double w = sqrt (z * z - g_t * g_l);
		double a_c = a_l + (a_t - a_l) * (w - g_l - z) / (g_t - g_l + 2 * w);

		// Calculate the minimizer of the quadratic that interpolates g_l and g_t
		// Equation 2.4.5 [Sun, Yuan 2006]
		double a_s = a_l - (a_l - a_t) / (g_l - g_t) * g_l;

		double a_t_next;

		if (fabs (a_c - a_t) < fabs (a_s - a_t))
		a_t_next = a_c;
		else
		a_t_next = a_s;

		if (a_t > a_l)
		return (std::min (a_t + 0.66 * (a_u - a_t), a_t_next));
		else
		return (std::max (a_t + 0.66 * (a_u - a_t), a_t_next));
	}
	// Case 4 in Trial Value Selection [More, Thuente 1994]
	else
	{
		// Calculate the minimizer of the cubic that interpolates f_u, f_t, g_u and g_t
		// Equation 2.4.52 [Sun, Yuan 2006]
		double z = 3 * (f_t - f_u) / (a_t - a_u) - g_t - g_u;
		double w = sqrt (z * z - g_t * g_u);
		// Equation 2.4.56 [Sun, Yuan 2006]
		return (a_u + (a_t - a_u) * (w - g_u - z) / (g_t - g_u + 2 * w));
	}
}

void ndt_mapping::buildTransformationMatrix(Matrix4f &matrix, Vec6 transform)
{
	// generating the transformation matrix componentwise with quaternions
	const float q_ha = 0.5f * transform[3];
	const float q_w = cos(q_ha);
	const float q_x = sin(q_ha);
	const float q_y = 0.0;
	const float q_z = 0.0;

	const float q_ha2 = 0.5f * transform[4];
	const float q_w2 = cos(q_ha2);
	const float q_x2 = 0.0;
	const float q_y2 = sin(q_ha2);
	const float q_z2 = 0.0;

	const float q_ha3 = 0.5f * transform[5];
	const float q_w3 = cos(q_ha3);
	const float q_x3 = 0.0;
	const float q_y3 = 0.0;
	const float q_z3 = sin(q_ha3);

	//quaternion 1 * quaternion 2
	const float r_x = q_w * q_w2 - q_x * q_x2 - q_y * q_y2 - q_z * q_z2;
	const float r_y = q_w * q_x2 + q_x * q_w2 + q_y * q_z2 - q_z * q_y2;
	const float r_z = q_w * q_y2 + q_y * q_w2 + q_z * q_x2 - q_x * q_z2;
	const float r_w = q_w * q_z2 + q_z * q_w2 + q_x * q_y2 - q_y * q_x2;

	// q1*q2 * quaternion 3
	const float r2_x = r_w * q_w3 - r_x * q_x3 - r_y * q_y3 - r_z * q_z3;
	const float r2_y = r_w * q_x3 + r_x * q_w3 + r_y * q_z3 - r_z * q_y3;
	const float r2_z = r_w * q_y3 + r_y * q_w3 + r_z * q_x3 - r_x * q_z3;
	const float r2_w = r_w * q_z3 + r_z * q_w3 + r_x * q_y3 - r_y * q_x3;

	//now compute some intermediate values for the rotationmatrix from q1*q2*q3
	const float tx  = 2.0f*r2_x;
	const float ty  = 2.0f*r2_y;
	const float tz  = 2.0f*r2_z;
	const float twx = tx*r2_w;
	const float twy = ty*r2_w;
	const float twz = tz*r2_w;
	const float txx = tx*r2_x;
	const float txy = ty*r2_x;
	const float txz = tz*r2_x;
	const float tyy = ty*r2_y;
	const float tyz = tz*r2_y;
	const float tzz = tz*r2_z;

	matrix.data[3][0] = 0.0;
	matrix.data[3][1] = 0.0;
	matrix.data[3][2] = 0.0;
	matrix.data[3][3] = 1.0;
	matrix.data[0][0] = transform[0];
	matrix.data[0][1] = transform[1];
	matrix.data[0][2] = transform[2];

	matrix.data[0][0] = 1.0f-(tyy+tzz);
	matrix.data[0][1] = txy-twz;
	matrix.data[0][2] = txz+twy;
	matrix.data[1][0] = txy+twz;
	matrix.data[1][1] = 1.0f-(txx+tzz);
	matrix.data[1][2] = tyz-twx;
	matrix.data[2][0] = txz-twy;
	matrix.data[2][1] = tyz+twx;
	matrix.data[2][2] = 1.0f-(txx+tyy);
}

// from /usr/include/pcl-1.7/pcl/registration/impl/ndt.hpp
double
ndt_mapping::computeStepLengthMT (
	const Vec6 &x, Vec6 &step_dir, double step_init, double step_max,
	double step_min, double &score, Vec6 &score_gradient, Mat66 &hessian,
	PointCloudSource &trans_cloud)
{
	// Set the value of phi(0), Equation 1.3 [More, Thuente 1994]
	double phi_0 = -score;
	// Set the value of phi'(0), Equation 1.3 [More, Thuente 1994]
	double d_phi_0 = -(dot_product6(score_gradient, step_dir));
	Vec6  x_t = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
	if (d_phi_0 >= 0)
	{
		// Not a decent direction
		if (d_phi_0 == 0)
			return 0;
		else
		{
			// Reverse step direction and calculate optimal step.
			d_phi_0 *= -1;
			for (int i = 0; i < 6; i++)
			step_dir[i] *= -1;

		}
	}
	// The Search Algorithm for T(mu) [More, Thuente 1994]
	int max_step_iterations = 10;
	int step_iterations = 0;
	// Sufficient decreace constant, Equation 1.1 [More, Thuete 1994]
	double mu = 1.e-4;
	// Curvature condition constant, Equation 1.2 [More, Thuete 1994]
	double nu = 0.9;
	// Initial endpoints of Interval I,
	double a_l = 0, a_u = 0;
	// Auxiliary function psi is used until I is determined ot be a closed interval, Equation 2.1 [More, Thuente 1994]
	double f_l = auxilaryFunction_PsiMT (a_l, phi_0, phi_0, d_phi_0, mu);
	double g_l = auxilaryFunction_dPsiMT (d_phi_0, d_phi_0, mu);

	double f_u = auxilaryFunction_PsiMT (a_u, phi_0, phi_0, d_phi_0, mu);
	double g_u = auxilaryFunction_dPsiMT (d_phi_0, d_phi_0, mu);
	// Check used to allow More-Thuente step length calculation to be skipped by making step_min == step_max
	bool interval_converged = (step_max - step_min) > 0, open_interval = true;
	double a_t = step_init;
	a_t = std::min (a_t, step_max);
	a_t = std::max (a_t, step_min);
	for (int i = 0; i < 6; i++)
		x_t[i] = x[i] + step_dir[i] * a_t;

	buildTransformationMatrix(final_transformation_, x_t);
	intermediate_transformations_.push_back(final_transformation_);
	// New transformed point cloud
	transformPointCloud(*input_cloud, trans_cloud, final_transformation_);
	// Updates score, gradient and hessian.  Hessian calculation is unessisary but testing showed that most step calculations use the
	// initial step suggestion and recalculation the reusable portions of the hessian would intail more computation time.
	score = computeDerivatives (score_gradient, hessian, trans_cloud, x_t, true);
	// Calculate phi(alpha_t)
	double phi_t = -score;
	// Calculate phi'(alpha_t)
	double d_phi_t = -(dot_product6(score_gradient, step_dir));
	// Calculate psi(alpha_t)
	double psi_t = auxilaryFunction_PsiMT (a_t, phi_t, phi_0, d_phi_0, mu);
	// Calculate psi'(alpha_t)
	double d_psi_t = auxilaryFunction_dPsiMT (d_phi_t, d_phi_0, mu);
	// Iterate until max number of iterations, interval convergance or a value satisfies the sufficient decrease, Equation 1.1, and curvature condition, Equation 1.2 [More, Thuente 1994]
	while (!interval_converged && step_iterations < max_step_iterations && !(psi_t <= 0 /*Sufficient Decrease*/ && d_phi_t <= -nu * d_phi_0 /*Curvature Condition*/))
	{
		// Use auxilary function if interval I is not closed
		if (open_interval)
		{
			a_t = trialValueSelectionMT (
				a_l, f_l, g_l,
				a_u, f_u, g_u,
				a_t, psi_t, d_psi_t);
		}
		else
		{
			a_t = trialValueSelectionMT (
				a_l, f_l, g_l,
				a_u, f_u, g_u,
				a_t, phi_t, d_phi_t);
		}
		a_t = std::min (a_t, step_max);
		a_t = std::max (a_t, step_min);
		for (int row = 0; row < 6; row++)
			x_t[row] = x[row] + step_dir[row] * a_t;

		buildTransformationMatrix(final_transformation_, x_t); 
		intermediate_transformations_.push_back(final_transformation_);
		// New transformed point cloud
		// Done on final cloud to prevent wasted computation
		transformPointCloud (*input_cloud, trans_cloud, final_transformation_);
		// Updates score, gradient. Values stored to prevent wasted computation.
		score = computeDerivatives (score_gradient, hessian, trans_cloud, x_t, false);
		// Calculate phi(alpha_t+)
		phi_t = -score;
		// Calculate phi'(alpha_t+)
		d_phi_t = -(dot_product6(score_gradient, step_dir));
		// Calculate psi(alpha_t+)
		psi_t = auxilaryFunction_PsiMT (a_t, phi_t, phi_0, d_phi_0, mu);
		// Calculate psi'(alpha_t+)
		d_psi_t = auxilaryFunction_dPsiMT (d_phi_t, d_phi_0, mu);
		// Check if I is now a closed interval
		if (open_interval && (psi_t <= 0 && d_psi_t >= 0))
		{
			open_interval = false;
			// Converts f_l and g_l from psi to phi
			f_l = f_l + phi_0 - mu * d_phi_0 * a_l;
			g_l = g_l + mu * d_phi_0;
			// Converts f_u and g_u from psi to phi
			f_u = f_u + phi_0 - mu * d_phi_0 * a_u;
			g_u = g_u + mu * d_phi_0;
		}

		if (open_interval)
		{
			// Update interval end points using Updating Algorithm [More, Thuente 1994]
			interval_converged = updateIntervalMT (
				a_l, f_l, g_l,
				a_u, f_u, g_u,
				a_t, psi_t, d_psi_t);
		}
		else
		{
			// Update interval end points using Modified Updating Algorithm [More, Thuente 1994]
			interval_converged = updateIntervalMT (
				a_l, f_l, g_l,
				a_u, f_u, g_u,
				a_t, phi_t, d_phi_t);
		}
		step_iterations++;
	}
	// gradients are required for step length determination
	// so derivative and transform data is stored for the next iteration.
	if (step_iterations)
		computeHessian (hessian, trans_cloud, x_t);

	return a_t;
}

void ndt_mapping::eulerAngles(Matrix4f trans, Vec3 &result)
{
	Vec3 res;
	const int i = 0;
	const int j = 1;
	const int k = 2;
	res[0] = atan2(trans.data[j][k], trans.data[k][k]);
	double n1 = trans.data[i][i];
	double n2 = trans.data[i][j];
	double c2 = sqrt(n1*n1+n2*n2);
	if(res[0]>0.0) {
		if(res[0] > 0.0) {
			res[0] -= PI;
		}
		else {
			res[0] += PI;
		}
		res[1] = atan2(-trans.data[i][k], -c2);
	}
	else
		res[1] = atan2(-trans.data[i][k], c2);
	double s1 = sin(res[0]);
	double c1 = cos(res[0]);
	res[2] = atan2(s1*trans.data[k][i]-c1*trans.data[j][i], c1*trans.data[j][j] - s1 * trans.data[k][j]);
	result[0] = -res[0];
	result[1] = -res[1];
	result[2] = -res[2];
}

void ndt_mapping::computeTransformation(PointCloud &output, const Matrix4f &guess)
{
	nr_iterations_ = 0;
	converged_ = false;
	double gauss_c1, gauss_c2, gauss_d3;
	// Initializes the guassian fitting parameters (eq. 6.8) [Magnusson 2009]
	gauss_c1 = 10 * (1 - outlier_ratio_);
	gauss_c2 = outlier_ratio_ / pow (resolution_, 3);
	gauss_d3 = -log (gauss_c2);
	gauss_d1_ = -log ( gauss_c1 + gauss_c2 ) - gauss_d3;
	gauss_d2_ = -2 * log ((-log ( gauss_c1 * exp ( -0.5 ) + gauss_c2 ) - gauss_d3) / gauss_d1_);
	// Initialise final transformation to the guessed one
	final_transformation_ = guess;
	// Apply guessed transformation prior to search for neighbours
	transformPointCloud (output, output, guess);
	// Initialize Point Gradient and Hessian
	memset(point_gradient_.data, 0, sizeof(double) * 3 * 6);
	point_gradient_.data[0][0] = 1.0;
	point_gradient_.data[1][1] = 1.0;
	point_gradient_.data[2][2] = 1.0;
	memset(point_hessian_.data, 0, sizeof(double) * 18 * 6);
	// Convert initial guess matrix to 6 element transformation vector
	Vec6 p, delta_p, score_gradient;
	p[0] = final_transformation_.data[0][4];
	p[1] = final_transformation_.data[1][4];
	p[2] = final_transformation_.data[2][4];
	Vec3 ea;
	eulerAngles(final_transformation_, ea);
	p[3] = ea[0];
	p[4] = ea[1];
	p[5] = ea[2];
	Mat66 hessian;
	double score = 0;
	double delta_p_norm;
	// Calculate derivates of initial transform vector, subsequent derivative calculations are done in the step length determination.
	score = computeDerivatives (score_gradient, hessian, output, p);
	while (!converged_)
	{
		// Store previous transformation
		previous_transformation_ = transformation_;
		// Negative for maximization as opposed to minimization
		Vec6 neg_grad = {
			-score_gradient[0], 
			-score_gradient[1], 
			-score_gradient[2],
			-score_gradient[3], 
			-score_gradient[4], 
			-score_gradient[5]
		};
		solve (delta_p, hessian, neg_grad);
		//Calculate step length with guarnteed sufficient decrease [More, Thuente 1994]
		delta_p_norm = sqrt(delta_p[0] * delta_p[0] +
				delta_p[1] * delta_p[1] +
				delta_p[2] * delta_p[2] +
				delta_p[3] * delta_p[3] +
				delta_p[4] * delta_p[4] +
				delta_p[5] * delta_p[5]);
		delta_p_norm = 1;
		if (delta_p_norm == 0 || delta_p_norm != delta_p_norm)
		{
			transformation_probability_ = score / static_cast<double> (input_cloud->size());
			converged_ = delta_p_norm == delta_p_norm;
			return;
		}

		delta_p[0] /= delta_p_norm;
		delta_p[1] /= delta_p_norm;
		delta_p[2] /= delta_p_norm;
		delta_p[3] /= delta_p_norm;
		delta_p[4] /= delta_p_norm;
		delta_p[5] /= delta_p_norm;
		
		delta_p_norm = computeStepLengthMT (p, delta_p, delta_p_norm, step_size_, transformation_epsilon_ / 2, score, score_gradient, hessian, output);
		delta_p[0] *= delta_p_norm;
		delta_p[1] *= delta_p_norm;
		delta_p[2] *= delta_p_norm;
		delta_p[3] *= delta_p_norm;
		delta_p[4] *= delta_p_norm;
		delta_p[5] *= delta_p_norm;

		buildTransformationMatrix(transformation_, delta_p);
		intermediate_transformations_.push_back(transformation_);
		p[0] = p[0] + delta_p[0];
		p[1] = p[1] + delta_p[1];
		p[2] = p[2] + delta_p[2];
		p[3] = p[3] + delta_p[3];
		p[4] = p[4] + delta_p[4];
		p[5] = p[5] + delta_p[5];
		if (nr_iterations_ > max_iterations_ ||
			(nr_iterations_ && (fabs (delta_p_norm) < transformation_epsilon_)))
		{
			converged_ = true;
		}
		nr_iterations_++;
	}

	// Store transformation probability.  The realtive differences within each scan registration are accurate
	// but the normalization constants need to be modified for it to be globally accurate
	transformation_probability_ = score / static_cast<double> (input_cloud->size());
}

/**
 * Helper function for simple matrix inversion using the determinant
 */
void invertMatrix(Mat33 &m)
{
	Mat33 temp;
	double det = m.data[0][0] * (m.data[2][2] * m.data[1][1] - m.data[2][1] * m.data[1][2]) -
	m.data[1][0] * (m.data[2][2] * m.data[0][1] - m.data[2][1] * m.data[0][2]) +
	m.data[2][0] * (m.data[1][2] * m.data[0][1] - m.data[1][1] * m.data[0][2]);
	double invDet = 1.0 / det;

	// adjungated matrix of minors
	temp.data[0][0] = m.data[2][2] * m.data[1][1] - m.data[2][1] * m.data[1][2];
	temp.data[0][1] = -( m.data[2][2] * m.data[0][1] - m.data[2][1] * m.data[0][2]);
	temp.data[0][2] = m.data[1][2] * m.data[0][1] - m.data[1][1] * m.data[0][2];

	temp.data[1][0] = -( m.data[2][2] * m.data[0][1] - m.data[2][0] * m.data[1][2]);
	temp.data[1][1] = m.data[2][2] * m.data[0][0] - m.data[2][1] * m.data[0][2];
	temp.data[1][2] = -( m.data[1][2] * m.data[0][0] - m.data[1][0] * m.data[0][2]);

	temp.data[2][0] = m.data[2][1] * m.data[1][0] - m.data[2][0] * m.data[1][1];
	temp.data[2][1] = -( m.data[2][1] * m.data[0][0] - m.data[2][0] * m.data[0][1]);
	temp.data[2][2] = m.data[1][1] * m.data[0][0] - m.data[1][0] * m.data[0][1];

	for (int row = 0; row < 3; row++)
	for (int col = 0; col < 3; col++)
		m.data[row][col] = temp.data[row][col] * invDet;

}
inline float fcompunpack(int val) {
	int& ival = val;
	if (val>>31) {
		ival = -val;
		ival |= 0x1<<31;
	}
	return reinterpret_cast<float&>(ival);
}
inline int fcomppack(float val) {
	int& ival = reinterpret_cast<int&>(val);
	if ((ival>>31)) {
		return -(ival & (~(0x1<<31)));
	} else {
		return ival;
	}

}
int ndt_mapping::pack_minmaxf(float val) {
	int& ival = reinterpret_cast<int&>(val);
	return (-(ival & ~(0x1<<31)))*(ival>>31) + (ival*((ival>>31) ^ 0x1));
}
float ndt_mapping::unpack_minmaxf(int val) {
	int ival = (-val | (0x1<<31))*(val>>31) + (val*((val>>31) ^ 0x1));
	return reinterpret_cast<float&>(ival);
}

void ndt_mapping::initCompute()
{
	// create the point cloud buffers
	int pointNo = target_cloud->size();
	prepare_compute_buffers(pointNo, nullptr);
	minVoxel = target_cloud->at(0);
	maxVoxel = target_cloud->at(0);
	// move point cloud to device
#ifdef EPHOS_PINNED_MEMORY
	computeEnv.cmdqueue.enqueueWriteBuffer(pointCloudBuffer, CL_FALSE,
		0, sizeof(PointXYZI)*pointNo, target_cloud->data());
#elif defined(EPHOS_ZERO_COPY)
	PointXYZI* pointCloudStorage = (PointXYZI*)computeEnv.cmdqueue.enqueueMapBuffer(pointCloudBuffer,
		CL_TRUE, CL_MAP_WRITE_INVALIDATE_REGION, 0, sizeof(PointXYZI)*pointNo);
	memcpy(pointCloudStorage, target_cloud->data(), sizeof(PointXYZI)*pointNo);
	computeEnv.cmdqueue.enqueueUnmapMemObject(pointCloudBuffer, pointCloudStorage);
#else
	computeEnv.cmdqueue.enqueueWriteBuffer(pointCloudBuffer, CL_FALSE,
		0, sizeof(PointXYZI)*pointNo, target_cloud->data());
#endif

	// measure the given cloud
	size_t globalRange1 = pointNo;
	if (pointNo%EPHOS_KERNEL_WORK_GROUP_SIZE != 0) {
		size_t workgroupNo = pointNo/EPHOS_KERNEL_WORK_GROUP_SIZE + 1;
		globalRange1 = workgroupNo*EPHOS_KERNEL_WORK_GROUP_SIZE;
	}
#ifdef EPHOS_KERNEL_CLOUD_MEASURE
	int izero = pack_minmaxf(0.0f);
	PackedVoxelGridInfo gridInfo1 = {
		pointNo, // cloud size
		0, // grid size
		{ izero, izero, izero }, // min corner
		{ izero, izero, izero }, // max corner
	};
	computeEnv.cmdqueue.enqueueWriteBuffer(gridInfoBuffer, CL_FALSE,
		0, sizeof(PackedVoxelGridInfo), &gridInfo1);
	measureCloudKernel.setArg(0, gridInfoBuffer);
	measureCloudKernel.setArg(1, pointCloudBuffer);
	measureCloudKernel.setArg(2, cl::Local(sizeof(PointXYZI)));
	measureCloudKernel.setArg(3, cl::Local(sizeof(PointXYZI)));
//

	computeEnv.cmdqueue.enqueueNDRangeKernel(
		measureCloudKernel,
		cl::NDRange(0),
		cl::NDRange(globalRange1),
		cl::NDRange(EPHOS_KERNEL_WORK_GROUP_SIZE));

	// read back grid info and update host data
	computeEnv.cmdqueue.enqueueReadBuffer(gridInfoBuffer, CL_TRUE,
		0, sizeof(PackedVoxelGridInfo), &gridInfo1);
	for (int i = 0; i < 3; i++) {
		minVoxel.data[i] = unpack_minmaxf(gridInfo1.minCorner.data[i]) - transformation_epsilon_;
		maxVoxel.data[i] = unpack_minmaxf(gridInfo1.maxCorner.data[i]) + transformation_epsilon_;
		voxelDimension[i] = (maxVoxel.data[i] - minVoxel.data[i]) / resolution_ + 1;
	}

#else // !EPHOS_KERNEL_CLOUD_MEASURE
	for (int i = 1; i < pointNo; i++)
	{
		for (int elem = 0; elem < 3; elem++)
		{
			if ( target_cloud->at(i).data[elem] > maxVoxel.data[elem] )
				maxVoxel.data[elem] = target_cloud->at(i).data[elem];
			if ( target_cloud->at(i).data[elem] < minVoxel.data[elem] )
				minVoxel.data[elem] = target_cloud->at(i).data[elem];
		}
	}
	for (int i = 0; i < 3; i++) {
		minVoxel.data[i] = minVoxel.data[i] - transformation_epsilon_;
		maxVoxel.data[i] = maxVoxel.data[i] + transformation_epsilon_;
		voxelDimension[i] = (maxVoxel.data[i] - minVoxel.data[i]) / resolution_ + 1;
	}

#endif // !EPHOS_KERNEL_CLOUD_MEASURE
	// now resize the grid while other compute operations may still be active
	int cellNo = voxelDimension[0] * voxelDimension[1] * voxelDimension[2];
	VoxelGridInfo gridInfo2 = {
		pointNo, // cloudSize
		cellNo, // gridSize
		{ minVoxel.data[0], minVoxel.data[1], minVoxel.data[2] }, // min corner
		{ maxVoxel.data[0], maxVoxel.data[1], maxVoxel.data[2] }, // max corner
		{ voxelDimension[0], voxelDimension[1] } // grid dimension
	};
	// move updated grid info to device
	computeEnv.cmdqueue.enqueueWriteBuffer(gridInfoBuffer, CL_FALSE,
		0, sizeof(VoxelGridInfo), &gridInfo2);
	prepare_compute_buffers(0, voxelDimension);

	// call the grid initialization kernel
	initTargetCellsKernel.setArg(0, gridInfoBuffer);
	initTargetCellsKernel.setArg(1, voxelGridBuffer);
	//initTargetCellsKernel.setArg(1, cellNo);
	size_t globalRange2 = cellNo;
	if (cellNo%EPHOS_KERNEL_WORK_GROUP_SIZE != 0) {
		size_t workgroupNo = cellNo/EPHOS_KERNEL_WORK_GROUP_SIZE + 1;
		globalRange2 = workgroupNo*EPHOS_KERNEL_WORK_GROUP_SIZE;
	}
	computeEnv.cmdqueue.enqueueNDRangeKernel(
		initTargetCellsKernel,
		cl::NDRange(0),
		cl::NDRange(globalRange2),
		cl::NDRange(EPHOS_KERNEL_WORK_GROUP_SIZE));
	
	// call the kernel that assigns points to cells
	firstPassKernel.setArg(0, gridInfoBuffer);
	firstPassKernel.setArg(1, pointCloudBuffer);
	firstPassKernel.setArg(2, voxelGridBuffer);

	computeEnv.cmdqueue.enqueueNDRangeKernel(
		firstPassKernel,
		cl::NDRange(0),
		cl::NDRange(globalRange1),
		cl::NDRange(EPHOS_KERNEL_WORK_GROUP_SIZE));

	// call the kernel that postprocesses the voxel grid
	secondPassKernel.setArg(0, gridInfoBuffer);
	secondPassKernel.setArg(1, voxelGridBuffer);
	secondPassKernel.setArg(2, pointCloudBuffer);
	// ranges have been computed for the initialization kernel
	computeEnv.cmdqueue.enqueueNDRangeKernel(
		secondPassKernel,
		cl::NDRange(0),
		cl::NDRange(globalRange2),
		cl::NDRange(EPHOS_KERNEL_WORK_GROUP_SIZE));

	// the result will be used in the radius search kernel
	// prepare radius search kernel calls
	radiusSearchKernel.setArg(0, gridInfoBuffer);
	radiusSearchKernel.setArg(1, pointCloudBuffer);
	radiusSearchKernel.setArg(2, voxelGridBuffer);
	radiusSearchKernel.setArg(3, subvoxelBuffer);
	radiusSearchKernel.setArg(4, counterBuffer);
	radiusSearchKernel.setArg(5, cl::Local(sizeof(int)));
	radiusSearchKernel.setArg(6, cl::Local(sizeof(int)));
}

void ndt_mapping::prepare_compute_buffers(int cloudSize, int* gridSize) {
	// TODO: think about ways to increase sizes artificially as these increase with later test cases anyway
	// create buffers of satisfactory size
	if (maxComputeCloudSize == 0 && maxComputeGridSize == 0) {
		// create constant size buffers only once
		counterBuffer = cl::Buffer(computeEnv.context, CL_MEM_READ_WRITE,
			sizeof(int));
		gridInfoBuffer = cl::Buffer(computeEnv.context, CL_MEM_READ_WRITE,
			sizeof(VoxelGridInfo));
	}
	if (cloudSize > maxComputeCloudSize) {
		// resize buffers that depend on cloud size
#ifdef EPHOS_ZERO_COPY
		cl_mem_flags flags = CL_MEM_ALLOC_HOST_PTR;
#else
		cl_mem_flags flags = 0;
#endif
		pointCloudBuffer = cl::Buffer(computeEnv.context, flags | CL_MEM_READ_ONLY | CL_MEM_HOST_WRITE_ONLY,
			sizeof(PointXYZI)*cloudSize);
		subvoxelBuffer = cl::Buffer(computeEnv.context, flags | CL_MEM_WRITE_ONLY | CL_MEM_HOST_READ_ONLY,
			sizeof(PointVoxel)*cloudSize);
		maxComputeCloudSize = cloudSize;
#ifdef EPHOS_PINNED_MEMORY
		if (subvoxelStorage != nullptr) {
			computeEnv.cmdqueue.enqueueUnmapMemObject(subvoxelHostBuffer, subvoxelStorage);
		}
		// TODO: since we have more operations here pinned memory might be considerably slower with current test data layout
		subvoxelHostBuffer = cl::Buffer(computeEnv.context,
			CL_MEM_WRITE_ONLY | CL_MEM_HOST_READ_ONLY | CL_MEM_ALLOC_HOST_PTR, sizeof(PointVoxel)*cloudSize);
		subvoxelStorage = (PointVoxel*)computeEnv.cmdqueue.enqueueMapBuffer(subvoxelHostBuffer,
			CL_TRUE, CL_MAP_READ, 0, sizeof(PointVoxel)*cloudSize);
#elif defined(EPHOS_ZERO_COPY)
#else
		if (subvoxelStorage != nullptr) {
			delete[] subvoxelStorage;
		}
		subvoxelStorage = new PointVoxel[cloudSize];
#endif
	}
	if (gridSize != nullptr) {
		int voxelNo = gridSize[0]*gridSize[1]*gridSize[2];
		if (voxelNo > maxComputeGridSize) {
			// resize buffers that depend on grid size
			voxelGridBuffer = cl::Buffer(computeEnv.context, CL_MEM_READ_WRITE | CL_MEM_HOST_NO_ACCESS,
				sizeof(Voxel)*voxelNo);
			maxComputeGridSize = voxelNo;
		}
	}
}
void ndt_mapping::ndt_align(const Matrix4f& guess)
{

	initCompute();
	// Copy the point data to output
	PointCloud output(*input_cloud);
	// Perform the actual transformation computation
	converged_ = false;
	final_transformation_ = transformation_ = previous_transformation_ = {
		{{ 1.0, 0.0, 0.0, 0.0 },
		 { 0.0, 1.0, 0.0, 0.0 },
		 { 0.0, 0.0, 1.0, 0.0 },
		 { 0.0, 0.0, 0.0, 1.0 }}
	};
	// Right before we estimate the transformation, we set all the point.data[3] values to 1
	// to aid the rigid transformation
	for (size_t i = 0; i < input_cloud->size (); ++i)
		output[i].data[3] = 1.0;
	computeTransformation (output, guess);
}


CallbackResult ndt_mapping::partial_points_callback(
	PointCloud& input_cloud,
	Matrix4f& init_guess,
	PointCloud& target_cloud)
{
	CallbackResult result;
	this->input_cloud = &input_cloud;
	this->target_cloud = &target_cloud;
	intermediate_transformations_.clear();
	ndt_align(init_guess);
	result.final_transformation = final_transformation_;
	result.intermediate_transformations = intermediate_transformations_;
	result.converged = converged_;
	return result;
}

void ndt_mapping::run(int p) {
	// prepare to read the first data set
	pause_func();
	while (read_testcases < testcases)
	{
		// read the next data set while paused
		int count = read_next_testcases(p);
		// resume kernel runtime measurement
		unpause_func();
		for (int i = 0; i < count; i++)
		{
			// actual kernel invocation
			results[i] = partial_points_callback(filtered_scan_ptr[i], init_guess[i], maps[i]);
		}
		// pause and compare results to reference
		pause_func();
		check_next_outputs(count);
	}
}

void ndt_mapping::check_next_outputs(int count)
{
	CallbackResult reference;
	for (int i = 0; i < count; i++)
	{
		try {
			parseResult(output_file, &reference);
		} catch (std::ios_base::failure& e) {
			std::cerr << e.what() << std::endl;
			exit(-3);
		}
		if (results[i].converged != reference.converged)
		{
			error_so_far = true;
		}
		// transform a point for error checking
		PointXYZI origin = {
			{ 1.0f, 1.0f, 1.0f, 1.0f }
		};
		PointXYZI refPoint = {
			{ 0.0f, 0.0f, 0.0f, 0.0f }
		};
		PointXYZI resPoint = {
			{ 0.0f, 0.0f, 0.0f, 0.0f }
		};
		for (int h = 0; h < 4; h++) {
			for (int w = 0; w < 4; w++) {
				refPoint.data[h] += reference.final_transformation.data[h][w]*origin.data[w];
				resPoint.data[h] += results[i].final_transformation.data[h][w]*origin.data[w];
			}
		}
		// assuming points are in carthesian coordinates
		// compare the transformation results
		for (int h = 0; h < 3; h++) {
			float delta = std::fabs(refPoint.data[h] - resPoint.data[h]);
			if (delta > max_delta) {
				max_delta = delta;
			}
			if (delta > MAX_TRANSLATION_EPS) {
				error_so_far = true;
			}
		}
		// compare the matrices
// 		for (int h = 0; h < 4; h++)
// 			for (int w = 0; w < 4; w++)
// 			{
// 				float delta = std::fabs(reference.final_transformation.data[h][w] - results[i].final_transformation.data[h][w]);
// 				if (delta > max_delta)
// 					max_delta = delta;
// 				// apply different thresholds for the rotation and translation components
// 				if (w < 3) {
// 					if (delta > MAX_ROTATION_EPS) {
// 						error_so_far = true;
// 					}
// 				} else {
// 					if (delta > MAX_TRANSLATION_EPS) {
// 						error_so_far = true;
// 					}
// 				}
// 			}
	}
}

