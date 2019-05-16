#include "../include/benchmark.h"
#include "datatypes.h"
#include <math.h>
#include <iostream>
#include <fstream>
#include <limits>
#include <cstring>
#include <chrono>
#include <stdlib.h>
#include <omp.h>
#include <float.h>
#include <vector>
#include <algorithm>

#define MAX_EPS 0.001


/**
Author: Florian Stock 2018

Kernel extracted from Autoware suite.
Dependencies on the PCL (PointCloudLib, flann)  are removed.
For their licenses see license folder.


(see Autoware/ros/src/computing/perception/localization/packages/ndt_localizer/nodes/ndt_mapping/ndt_mapping.cpp,
  function points_callback)
  (restricted to the align/getFitnessScore computation, as a openmp seems to be available, which can be compared to our)

  Computed results are compared with the Autoware computed result.

  */

#pragma omp declare reduction(min:PointXYZI:  omp_out.data[0] = omp_out.data[0] < omp_in.data[0] ? omp_out.data[0] : omp_in.data[0],  omp_out.data[1] = omp_out.data[1] < omp_in.data[1] ? omp_out.data[1] : omp_in.data[1], omp_out.data[2] = omp_out.data[2] < omp_in.data[2] ? omp_out.data[2] : omp_in.data[2]) initializer (omp_priv={{FLT_MAX ,FLT_MAX ,FLT_MAX ,0}})


#pragma omp declare reduction(max:PointXYZI:  omp_out.data[0] = omp_out.data[0] > omp_in.data[0] ? omp_out.data[0] : omp_in.data[0],  omp_out.data[1] = omp_out.data[1] > omp_in.data[1] ? omp_out.data[1] : omp_in.data[1], omp_out.data[2] = omp_out.data[2] > omp_in.data[2] ? omp_out.data[2] : omp_in.data[2]) initializer (omp_priv= {{FLT_MIN ,FLT_MIN ,FLT_MIN ,0}})


// maximum allowed deviation from reference
#define MAX_TRANSLATION_EPS 0.001
#define MAX_ROTATION_EPS 0.9

class ndt_mapping : public kernel {
public:
    virtual void init();
    virtual void run(int p = 1);
    virtual bool check_output();
    PointCloud* filtered_scan_ptr = NULL;
    Matrix4f* init_guess = NULL;
    CallbackResult* results = NULL;
    PointCloud* maps = NULL;
protected:
	/**
         * Reads the number of testcases in the data file
         */
        int read_number_testcases(std::ifstream& input_file);
	virtual int read_next_testcases(int count);	
    virtual void check_next_outputs(int count);
    int read_testcases = 0;
    std::ifstream input_file, output_file;
    bool error_so_far;
    double max_delta;

    // some magic numbers for ndt
    double outlier_ratio_ = 0.55;
    float resolution_ = 1.0;
    double trans_eps = 0.01; //Transformation epsilon
    double step_size_ = 0.1; // Step size
    int iter = 30;  // Maximum iterations
    Matrix4f final_transformation_, transformation_, previous_transformation_;
    bool converged_;
    int nr_iterations_;
    Vec3 h_ang_a2_, h_ang_a3_,
        h_ang_b2_, h_ang_b3_,
        h_ang_c2_, h_ang_c3_,
        h_ang_d1_, h_ang_d2_, h_ang_d3_,
        h_ang_e1_, h_ang_e2_, h_ang_e3_,
        h_ang_f1_, h_ang_f2_, h_ang_f3_;
    Vec3 j_ang_a_, j_ang_b_, j_ang_c_, j_ang_d_, j_ang_e_, j_ang_f_, j_ang_g_, j_ang_h_;
    Mat36 point_gradient_;
    Mat186 point_hessian_;
    double gauss_d1_, gauss_d2_;
    double trans_probability_;
    double transformation_epsilon_ = 0.1;
    int max_iterations_;
    PointCloud* input_;
    PointCloud* target_;
    VoxelGrid target_cells_;
    PointXYZI minVoxel, maxVoxel;
    int voxelDimension[3];
    inline int linearizeAddr(const int x, const int y, const int z);
    inline int linearizeCoord(const float x, const float y, const float z);
    double updateDerivatives (Vec6 &score_gradient,
                                           Mat66 &hessian,
                                           Vec3 &x_trans, Mat33 &c_inv,
                                           bool compute_hessian = true);
    void computePointDerivatives (Vec3 &x, bool compute_hessian = true);
    void computeHessian (Mat66 &hessian,
                         PointCloudSource &trans_cloud, Vec6 &);
    void updateHessian (Mat66 &hessian, Vec3 &x_trans, Mat33 &c_inv);
    double computeDerivatives (Vec6 &score_gradient,
                                            Mat66 &hessian,
                                            PointCloudSource &trans_cloud,
                                            Vec6 &p,
                                            bool compute_hessian = true );
    bool updateIntervalMT (double &a_l, double &f_l, double &g_l,
                                        double &a_u, double &f_u, double &g_u,
                                        double a_t, double f_t, double g_t);
    double trialValueSelectionMT (double a_l, double f_l, double g_l,
                                               double a_u, double f_u, double g_u,
                                               double a_t, double f_t, double g_t);
    double computeStepLengthMT (const Vec6 &x, Vec6 &step_dir, double step_init, double step_max,
                                double step_min, double &score, Vec6 &score_gradient, Mat66 &hessian,
                                PointCloudSource &trans_cloud);
    void computeTransformation(PointCloud &output, const Matrix4f &guess);
    void computeAngleDerivatives (Vec6 &p, bool compute_hessian = true);
    void ndt_align (const Matrix4f& guess);
    void initCompute();
    void deinitCompute();
    void buildTransformationMatrix(Matrix4f &matrix, Vec6 transform);
    inline double ndt_getFitnessScore ();
    void eulerAngles(Matrix4f transform, Vec3 &result);
    CallbackResult partial_points_callback(PointCloud &input_cloud, Matrix4f &init_guess, PointCloud& target_cloud);
    int voxelRadiusSearch(VoxelGrid &grid, const PointXYZI& point, double radius,
                          std::vector<Voxel> & indices,
                          std::vector<float> distances);
    inline double findNearest(PointXYZI &p);
    inline double findNearest2(PointXYZI &p);
    inline double findMinInVoxel(int idx, PointXYZI &p);
    inline void linearCoord2Addr(const float x, const float y, const float z, int &xr, int &yr, int &zr);
};

/**
 * Reads the next point cloud.
 */
void  parseFilteredScan(std::ifstream& input_file, PointCloud* pointcloud) {
        int32_t size;
        try {
                input_file.read((char*)&(size), sizeof(int32_t));
                pointcloud->clear();
                for (int i = 0; i < size; i++)
                {
                        PointXYZI p;
                        input_file.read((char*)&p.data[0], sizeof(float));
                        input_file.read((char*)&p.data[1], sizeof(float));
                        input_file.read((char*)&p.data[2], sizeof(float));
                        input_file.read((char*)&p.data[3], sizeof(float));
                        pointcloud->push_back(p);
                }
        }  catch (std::ifstream::failure) {
                throw std::ios_base::failure("Error reading filtered scan");
        }
}

/**
 * Reads the next initilization matrix.
 */
void  parseInitGuess(std::ifstream& input_file, Matrix4f* initGuess) {
        try {
        for (int h = 0; h < 4; h++)
                for (int w = 0; w < 4; w++)
                        input_file.read((char*)&(initGuess->data[h][w]),sizeof(float));
        }  catch (std::ifstream::failure) {
                throw std::ios_base::failure("Error reading initial guess");
        }
}

/**
 * Reads the next reference matrix.
 */
void parseResult(std::ifstream& output_file, CallbackResult* goldenResult) {
        try {
                for (int h = 0; h < 4; h++)
                        for (int w = 0; w < 4; w++)
                        {
                                output_file.read((char*)&(goldenResult->final_transformation.data[h][w]), sizeof(float));
                        }
                output_file.read((char*)&(goldenResult->fitness_score), sizeof(double));
                output_file.read((char*)&(goldenResult->converged), sizeof(bool));
        }  catch (std::ifstream::failure) {
                throw std::ios_base::failure("Error reading reference result");
        }
}

int ndt_mapping::read_next_testcases(int count)
{
        int i;
        // free memory used in the previous test case and allocate new one
        delete [] maps;
        maps = new PointCloud[count];
        delete [] filtered_scan_ptr;
        filtered_scan_ptr = new PointCloud[count];
        delete [] init_guess;
        init_guess = new Matrix4f[count];
        delete [] results;
        results = new CallbackResult[count];
        // parse the test cases
        for (i = 0; (i < count) && (read_testcases < testcases); i++,read_testcases++)
        {
                try {
                        parseInitGuess(input_file, init_guess + i);
                        parseFilteredScan(input_file, filtered_scan_ptr + i);
                        parseFilteredScan(input_file, maps + i);
                } catch (std::ios_base::failure& e) {
                        std::cerr << e.what() << std::endl;
                        exit(-3);
                }
        }
        return i;
}

int ndt_mapping::read_number_testcases(std::ifstream& input_file)
{
        int32_t number;
        try {
                input_file.read((char*)&(number), sizeof(int32_t));
        } catch (std::ifstream::failure) {
                throw std::ios_base::failure("Error reading number of test cases");
        }
        return number;
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

#pragma omp declare target
int linearizeAddrOFF(const int x, const int y, const int z,int voxelDim[3])
{
    return  (x + voxelDim[0] * (y + voxelDim[1] * z));
}

int linearizeCoordOFF(const float x, const float y, const float z,float res,PointXYZI minVox,int dim[3])
{
    int idx_x = (x - minVox.data[0]) / res;
    int idx_y = (y - minVox.data[1]) / res;
    int idx_z = (z - minVox.data[2]) / res;


    return linearizeAddrOFF(idx_x, idx_y, idx_z,dim);
}
#pragma omp end declare target

// also performs clipping
inline void ndt_mapping::linearCoord2Addr(const float x, const float y, const float z, int &xr, int &yr, int &zr)
{
    xr = (x - minVoxel.data[0]) / resolution_;
    yr = (y - minVoxel.data[1]) / resolution_;
    zr = (z - minVoxel.data[2]) / resolution_;

    if (xr < 0)
	xr = 0;
    if (xr >= voxelDimension[0])
	xr = voxelDimension[0]-1;
    if (yr < 0)
	yr = 0;
    if (yr >= voxelDimension[1])
	yr = voxelDimension[1]-1;
    if (zr < 0)
	zr = 0;
    if (zr >= voxelDimension[2])
	zr = voxelDimension[2]-1;

}

int ndt_mapping::voxelRadiusSearch(VoxelGrid &grid, const PointXYZI& point, double radius,
				   std::vector<Voxel> & indices,
				   std::vector<float> distances)
{

    int result = 0;
    indices.clear();
    distances.clear();
    //start_timer(5);
    // checking the voxel and the voxel besides
    for (float x = point.data[0] - radius; x <= point.data[0]+radius; x+= resolution_)
	for (float y = point.data[1] - radius; y <= point.data[1]+radius; y+= resolution_)
	    for (float z = point.data[2] - radius; z <= point.data[2]+radius; z+= resolution_)
		{
		    if ((x < minVoxel.data[0]) ||
			(x > maxVoxel.data[0]) ||
			(y < minVoxel.data[1]) ||
			(y > maxVoxel.data[1]) ||
			(z < minVoxel.data[2]) ||
			(z > maxVoxel.data[2]))
			continue;
		    int idx =  linearizeCoord(x, y, z);
		    Vec3 &c =  grid[idx].mean;
		    float dx = c[0] - point.data[0];
		    float dy = c[1] - point.data[1];
		    float dz = c[2] - point.data[2];
		    float dist = sqrt(dx * dx + dy * dy + dz * dz);
		    if (dist < radius)
			{
			    result++;
			    indices.push_back(grid[idx]);
			    distances.push_back(dist);
			}
		}
    //end_timer(5);
    return result;
}

/**
   Solve routine to replace the used SVD.solve from EIgen.
   Solves Ax = b, and returns (newly allocated) x.
   Maybe not as good when handling very ill conditioned systems, but for a 6x6 faster and good enough
*/
void solve(Vec6& result, Mat66 A, Vec6& b)
{
    double pivot;
    //start_timer(6);
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
		//	std::cout << "Singular matrix\n";
		A.data[j][j] = MAX_EPS;
	    }
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
    //end_timer(6);
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
        // consume the number of testcases from the testcase file
        try {
                testcases = read_number_testcases(input_file);
        } catch (std::ios_base::failure& e) {
                std::cerr << e.what() << std::endl;
                exit(-3);
        }
        // prepare the first iteration
        error_so_far = false;
        max_delta = 0.0;
        maps = nullptr;
        init_guess = nullptr;
        filtered_scan_ptr = nullptr;
        results = nullptr;
        std::cout << "done\n" << std::endl;
}



/*void ndt_mapping::init() {

    std::cout << "init\n";
    //input_file.read((char*)&testcases, sizeof(uint32_t));
    testcases = 115;

    input_file.exceptions ( std::ifstream::failbit | std::ifstream::badbit );
    output_file.exceptions ( std::ifstream::failbit | std::ifstream::badbit );

    try {
	input_file.open("data/input.dat", std::ios::binary);
	output_file.open("data/output.dat", std::ios::binary);
    }  catch (std::ifstream::failure e) {
	std::cerr << "Error opening file\n";
	exit(-3);
    }
    error_so_far = false;
    max_delta = 0.0;

    maps = NULL;
    init_guess = NULL;
    filtered_scan_ptr = NULL;
    results = NULL;

    std::cout << "done\n" << std::endl;
}*/


// own implementation: applies transformation matrix transform on all points in input point cloud and writes
// the result in output point cloud.
void transformPointCloud(const PointCloud& input, PointCloud &output, Matrix4f transform) //TODO
{
	//start_timer(2);
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
    //end_timer(2);
}



/**
    Helperfunction which allocates and sets everything to a given value.
*/
float* assign(uint32_t count, float value) {
    float* result;

    result = new float[count];
    for (int i = 0; i < count; i++) {
	result[i] = value;
    }
    return result;
}

// fs: Helperfunction
// dot product of two vectors
double dot_product(Vec3 &a, Vec3 &b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}


double dot_product6(Vec6 &a, Vec6 &b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] + a[4] * b[4] + a[5] * b[5];
}


// from pcl  ndt.h
inline double
auxilaryFunction_dPsiMT (double g_a, double g_0, double mu = 1.e-4)
{
    return (g_a - mu * g_0);
}

// from pcl  ndt.h
inline double
auxilaryFunction_PsiMT (double a, double f_a, double f_0, double g_0, double mu = 1.e-4)
{
    return (f_a - f_0 - mu * g_0 * a);
}


// from /usr/include/pcl-1.7/pcl/registration/impl/ndt.hpp
double ndt_mapping::updateDerivatives (Vec6 &score_gradient,
				       Mat66 &hessian,
				       Vec3 &x_trans, Mat33 &c_inv,
				       bool compute_hessian)
{
	//start_timer(3);
    Vec3 cov_dxd_pi;

    // e^(-d_2/2 * (x_k - mu_k)^T Sigma_k^-1 (x_k - mu_k)) Equation 6.9 [Magnusson 2009]
    //double e_x_cov_x = exp (-gauss_d2_ * dot_product(x_trans, (c_inv * x_trans)) / 2);
    // fs: remove explicit matrix multiplication
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
	//end_timer(3);
    return (score_inc);
}

// from /usr/include/pcl-1.7/pcl/registration/impl/ndt.hpp
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
	    // Vectors from Equation 6.21 [Magnusson 2009]
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

	    // Calculate second derivative of Transformation Equation 6.17 w.r.t. transform vector p.
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


void
ndt_mapping::computeHessian (Mat66 &hessian,
			     PointCloud &trans_cloud, Vec6 &)
{
  // Original Point and Transformed Point
  PointXYZI  x_pt, x_trans_pt;
  // Original Point and Transformed Point (for math)
  Vec3 x, x_trans;
  // Occupied Voxel
  Voxel cell;
  // Inverse Covariance of Occupied Voxel
  Mat33 c_inv;

  memset(&(hessian.data[0][0]), 0, sizeof(double) * 6 * 6);

  // Precompute Angular Derivatives unessisary because only used after regular derivative calculation
  //start_timer(7);
// Update hessian for each point, line 17 in Algorithm 2 [Magnusson 2009]
  for (size_t idx = 0; idx < input_->size (); idx++)
  {
    x_trans_pt = trans_cloud[idx];

    // Find nieghbors (Radius search has been experimentally faster than direct neighbor checking.
    std::vector<Voxel> neighborhood;
    std::vector<float> distances;
    voxelRadiusSearch (target_cells_, x_trans_pt, resolution_, neighborhood, distances);

    for (auto neighborhood_it = neighborhood.begin (); neighborhood_it != neighborhood.end (); neighborhood_it++)
    {
      cell = *neighborhood_it;

      {
	x_pt = (*input_)[idx];
        x[0] = x_pt.data[0];
	x[1] = x_pt.data[1];
	x[2] = x_pt.data[2];

        x_trans[0] = x_trans_pt.data[0];
	x_trans[1] = x_trans_pt.data[1];
	x_trans[2] = x_trans_pt.data[2];

        // Denorm point, x_k' in Equations 6.12 and 6.13 [Magnusson 2009]
        x_trans[0] -= cell.mean[0];
	x_trans[1] -= cell.mean[1];
	x_trans[2] -= cell.mean[2];
        // Uses precomputed covariance for speed.
        c_inv = cell.invCovariance;

        // Compute derivative of transform function w.r.t. transform vector, J_E and H_E in Equations 6.18 and 6.20 [Magnusson 2009]
        computePointDerivatives (x);
        // Update hessian, lines 21 in Algorithm 2, according to Equations 6.10, 6.12 and 6.13, respectively [Magnusson 2009]
        updateHessian (hessian, x_trans, c_inv);
      }
    }
  }
  //end_timer(7);
}

void ndt_mapping::updateHessian (Mat66 &hessian, Vec3 &x_trans, Mat33 &c_inv)
{
    //start_timer(8);
  Vec3 cov_dxd_pi;
  // e^(-d_2/2 * (x_k - mu_k)^T Sigma_k^-1 (x_k - mu_k)) Equation 6.9 [Magnusson 2009]
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

  // Reusable portion of Equation 6.12 and 6.13 [Magnusson 2009]
  e_x_cov_x *= gauss_d1_;

  for (int i = 0; i < 6; i++)
  {
      // Sigma_k^-1 d(T(x,p))/dpi, Reusable portion of Equation 6.12 and 6.13 [Magnusson 2009]
      //     cov_dxd_pi = c_inv * point_gradient_.col (i);
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
  //end_timer(8);
}

// from /usr/include/pcl-1.7/pcl/registration/impl/ndt.hpp
double ndt_mapping::computeDerivatives (Vec6 &score_gradient,
					Mat66 &hessian,
					PointCloudSource &trans_cloud,
					Vec6 &p,
					bool compute_hessian)
{
    // Original Point and Transformed Point
    PointXYZI x_pt, x_trans_pt;
    // Original Point and Transformed Point (for math)
    Vec3 x, x_trans;
    // Occupied Voxel
    Voxel cell;
    // Inverse Covariance of Occupied Voxel
    Mat33 c_inv;

    memset(&(score_gradient[0]), 0, sizeof(double) * 6 );
    memset(&(hessian.data[0][0]), 0, sizeof(double) * 6 * 6);
    double score = 0.0;

    // Precompute Angular Derivatives (eq. 6.19 and 6.21)[Magnusson 2009]
    computeAngleDerivatives (p);
    //start_timer(9);
    // Update gradient and hessian for each point, line 17 in Algorithm 2 [Magnusson 2009]
    for (size_t idx = 0; idx < input_->size (); idx++)
	{
	    x_trans_pt = trans_cloud[idx];

	    // Find nieghbors (Radius search has been experimentally faster than direct neighbor checking.
	    std::vector<Voxel> neighborhood;
	    std::vector<float> distances;
	    voxelRadiusSearch (target_cells_, x_trans_pt, resolution_, neighborhood, distances);

	    for (auto neighborhood_it = neighborhood.begin (); neighborhood_it != neighborhood.end (); neighborhood_it++)
		{
		    cell = *neighborhood_it;
		    x_pt = (*input_)[idx];
		    x[0] = x_pt.data[0];
		    x[1] = x_pt.data[1];
		    x[2] = x_pt.data[2];

		    x_trans[0] = x_trans_pt.data[0];
		    x_trans[1] = x_trans_pt.data[1];
		    x_trans[2] = x_trans_pt.data[2];

		    // Denorm point, x_k' in Equations 6.12 and 6.13 [Magnusson 2009]
		    x_trans[0] -= cell.mean[0];
		    x_trans[1] -= cell.mean[1];
		    x_trans[2] -= cell.mean[2];
		    // Uses precomputed covariance for speed.
		    c_inv = cell.invCovariance;

		    // Compute derivative of transform function w.r.t. transform vector, J_E and H_E in Equations 6.18 and 6.20 [Magnusson 2009]
		    computePointDerivatives (x);
		    // Update score, gradient and hessian, lines 19-21 in Algorithm 2, according to Equations 6.10, 6.12 and 6.13, respectively [Magnusson 2009]
		    score += updateDerivatives (score_gradient, hessian, x_trans, c_inv, compute_hessian);

		}
	}
    //end_timer(9);
    return (score);
}

// from /usr/include/pcl-1.7/pcl/registration/impl/ndt.hpp
void ndt_mapping::computeAngleDerivatives (Vec6 &p, bool compute_hessian)
{
    // Simplified math for near 0 angles
    double cx, cy, cz, sx, sy, sz;
    if (fabs (p[3]) < 10e-5)
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
    if (fabs (p[4]) < 10e-5)
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

    if (fabs (p[5]) < 10e-5)
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
ndt_mapping::updateIntervalMT (double &a_l, double &f_l, double &g_l,
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
ndt_mapping::trialValueSelectionMT (double a_l, double f_l, double g_l,
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
    // fs: generating the transformation matrix componentwise (with quaternions)
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
ndt_mapping::computeStepLengthMT (const Vec6 &x, Vec6 &step_dir, double step_init, double step_max,
				  double step_min, double &score, Vec6 &score_gradient, Mat66 &hessian,
				  PointCloudSource &trans_cloud)
{
    //start_timer(10);
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
	//(Eigen::Translation<float, 3>(static_cast<float> (x_t (0)), static_cast<float> (x_t (1)), static_cast<float> (x_t (2))) *
	//		     Eigen::AngleAxis<float> (static_cast<float> (x_t (3)), Eigen::Vector3f::UnitX ()) *
	//		     Eigen::AngleAxis<float> (static_cast<float> (x_t (4)), Eigen::Vector3f::UnitY ()) *
	//		     Eigen::AngleAxis<float> (static_cast<float> (x_t (5)), Eigen::Vector3f::UnitZ ())).matrix ();

    // New transformed point cloud
    transformPointCloud (*input_, trans_cloud, final_transformation_);

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
		    a_t = trialValueSelectionMT (a_l, f_l, g_l,
						 a_u, f_u, g_u,
						 a_t, psi_t, d_psi_t);
		}
	    else
		{
		    a_t = trialValueSelectionMT (a_l, f_l, g_l,
						 a_u, f_u, g_u,
						 a_t, phi_t, d_phi_t);
		}

	    a_t = std::min (a_t, step_max);
	    a_t = std::max (a_t, step_min);


	    for (int row = 0; row < 6; row++)
		x_t[row] = x[row] + step_dir[row] * a_t;

	    buildTransformationMatrix(final_transformation_, x_t);
	    /*	    final_transformation_ = (Eigen::Translation<float, 3> (static_cast<float> (x_t (0)), static_cast<float> (x_t (1)), static_cast<float> (x_t (2))) *
				     Eigen::AngleAxis<float> (static_cast<float> (x_t (3)), Eigen::Vector3f::UnitX ()) *
				     Eigen::AngleAxis<float> (static_cast<float> (x_t (4)), Eigen::Vector3f::UnitY ()) *
				     Eigen::AngleAxis<float> (static_cast<float> (x_t (5)), Eigen::Vector3f::UnitZ ())).matrix ();
	    */

	    // New transformed point cloud
	    // Done on final cloud to prevent wasted computation
	    transformPointCloud (*input_, trans_cloud, final_transformation_);

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
		    interval_converged = updateIntervalMT (a_l, f_l, g_l,
							   a_u, f_u, g_u,
							   a_t, psi_t, d_psi_t);
		}
	    else
		{
		    // Update interval end points using Modified Updating Algorithm [More, Thuente 1994]
		    interval_converged = updateIntervalMT (a_l, f_l, g_l,
							   a_u, f_u, g_u,
							   a_t, phi_t, d_phi_t);
		}

	    step_iterations++;
	}

    // If inner loop was run then hessian needs to be calculated.
    // Hessian is unnessisary for step length determination but gradients are required
    // so derivative and transform data is stored for the next iteration.
    if (step_iterations)
	computeHessian (hessian, trans_cloud, x_t);
    //end_timer(10);
    return (a_t);
}

//computes the eulerangles from an rotation matrix (modified code from eigen lib)
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

// from /usr/include/pcl-1.7/pcl/registration/impl/ndt.hpp
void ndt_mapping::computeTransformation(PointCloud &output, const Matrix4f &guess)
{
	//start_timer(1);
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
    //start_timer(4);
    while (!converged_)
	{
	    // Store previous transformation
	    previous_transformation_ = transformation_;

	    // Solve for decent direction using newton method, line 23 in Algorithm 2 [Magnusson 2009]
	    //Eigen::JacobiSVD<Eigen::Matrix<double, 6, 6> > sv (hessian, Eigen::ComputeFullU | Eigen::ComputeFullV);
	    // Negative for maximization as opposed to minimization
	    Vec6 neg_grad = {-score_gradient[0], -score_gradient[1], -score_gradient[2],
			     -score_gradient[3], -score_gradient[4], -score_gradient[5]};
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
		    trans_probability_ = score / static_cast<double> (input_->size ());
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
	    /*	    transformation_ = (Eigen::Translation<float, 3> (static_cast<float> (delta_p (0)), static_cast<float> (delta_p (1)), static_cast<float> (delta_p (2))) *
			       Eigen::AngleAxis<float> (static_cast<float> (delta_p (3)), Eigen::Vector3f::UnitX ()) *
			       Eigen::AngleAxis<float> (static_cast<float> (delta_p (4)), Eigen::Vector3f::UnitY ()) *
			       Eigen::AngleAxis<float> (static_cast<float> (delta_p (5)), Eigen::Vector3f::UnitZ ())).matrix ();
	    */

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
	    //std::cout << "iterations: " << nr_iterations_ << "\n";
	}
	//end_timer(4);
	//end_timer(1);
    // Store transformation probability.  The realtive differences within each scan registration are accurate
    // but the normalization constants need to be modified for it to be globally accurate
    trans_probability_ = score / static_cast<double> (input_->size ());
}

// invert matrix: its just 3x3 so we use the determinant
#pragma omp declare target
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
#pragma omp end declare target

/*
int compareCloudindex(const void *p1, const void *p2)
{
    CloudIndex *c1 = (CloudIndex *) p1;
    CloudIndex *c2 = (CloudIndex *) p2;

    return (c1->sequenceIndex - c2->sequenceIndex);

}
*/

void ndt_mapping::initCompute() // kann als ganzes geoffloaded werden
{
    // fs: performs now the init of the voxel grid structure

    // find min/max
    minVoxel = (*target_)[0];
    maxVoxel = (*target_)[0];
//	start_timer(0);
  #pragma omp parallel for collapse(2) reduction(min:minVoxel) reduction(max:maxVoxel)
    for (int i = 1; i < target_->size(); i++) // reuction min bestenfalls
	{
	    for (int elem = 0; elem < 3; elem++)
		{
		    if ( (*target_)[i].data[elem] > maxVoxel.data[elem] )
			maxVoxel.data[elem] = (*target_)[i].data[elem];
		    if ( (*target_)[i].data[elem] < minVoxel.data[elem] )
			minVoxel.data[elem] = (*target_)[i].data[elem];
		}
	}

    voxelDimension[0] = (maxVoxel.data[0] - minVoxel.data[0]) / resolution_ + 1;
    voxelDimension[1] = (maxVoxel.data[1] - minVoxel.data[1]) / resolution_ + 1;
    voxelDimension[2] = (maxVoxel.data[2] - minVoxel.data[2]) / resolution_ + 1;

    // init the voxel array
    target_cells_.clear();
    target_cells_.resize(voxelDimension[0] * voxelDimension[1] * voxelDimension[2]);

    int sizeOfDatacell = voxelDimension[0] * voxelDimension[1] * voxelDimension[2]; // erreicht 3millionen
    Voxel* dataCell = target_cells_.data();
    int targetSize = target_->size();
    PointXYZI *targetData = target_->data();

    PointXYZI minVoxelTransferable = minVoxel;
    int voxelDim[3];
    for (int i = 0; i < 3; i++) {
      voxelDim[i] = voxelDimension[i];
    }
    float resolution = resolution_;

  //  std::cout << "<"<< targetData[0].data[0]<< ", " <<targetData[0].data[1]<< ", " <<targetData[0].data[2]<< ", " <<targetData[0].data[3]<< ">" << '\n';
  //  bool a =false;
//    bool b =false;

  //Mat33 invCovarianceDeepCopy[sizeOfDatacell];
	std::vector<Mat33> invCovarianceDeepCopyStorage(sizeOfDatacell);
	Mat33* invCovarianceDeepCopy = invCovarianceDeepCopyStorage.data();
// https://e-reports-ext.llnl.gov/pdf/884915.pdf



#pragma omp target map(to:sizeOfDatacell,targetSize,targetData[:targetSize],resolution,minVoxelTransferable.data[:4],voxelDim[:3]) map(from:dataCell[:sizeOfDatacell],invCovarianceDeepCopy[:sizeOfDatacell])// map(tofrom:a)
{
//  a = (targetData[0].data[0] + targetData[0].data[1] + targetData[0].data[2] + targetData[0].data[3] == 0);
//  b = (minVoxelTransferable.data[0] + minVoxelTransferable.data[1] + minVoxelTransferable.data[2] + minVoxelTransferable.data[3] == 0);


  #pragma omp teams distribute parallel for
    for (int i = 0; i < sizeOfDatacell; i++)
	{
	    dataCell[i].numberPoints = 0;
 	    dataCell[i].mean[0] = 0;
	    dataCell[i].mean[1] = 0;
	    dataCell[i].mean[2] = 0;

      //#pragma omp simd
      invCovarianceDeepCopy[i] = {.data ={
                                  {0,0,1},
                                  {0,1,0},
                                  {1,0,0}
      }};
	    //memset(dataCell[i].invCovariance.data, 0, sizeof(double) * 3 * 3); //nicht sicher wegen dem memset eventuell ersetzen durch vectorisierte forloop
	    //invCovarianceDeepCopy[i].data[2][0] = 1.0;
	    //invCovarianceDeepCopy[i].data[1][1] = 1.0;
	    //invCovarianceDeepCopy[i].data[0][2] = 1.0;
	}
//    start_timer(11);
    // first pass to put everything in the right voxel leaf
    //#pragma omp parallel for reduction(gridAdd:dataCell[:n]) //verbraucht zuviel speicher => Segfault
    for (int i = 0; i <targetSize; i++) // Reduction sum
	  {
	    int voxelIndex = linearizeCoordOFF( targetData[i].data[0], targetData[i].data[1], targetData[i].data[2],resolution,minVoxelTransferable,voxelDim);

	    dataCell[voxelIndex].mean[0] += targetData[i].data[0];
	    dataCell[voxelIndex].mean[1] += targetData[i].data[1];
	    dataCell[voxelIndex].mean[2] += targetData[i].data[2];
	    dataCell[voxelIndex].numberPoints++;

	    // sum up x * xT for single pass covariance calculation
	    for (int row = 0; row < 3; row ++)
    		for (int col = 0; col < 3; col ++)
    		   invCovarianceDeepCopy[voxelIndex].data[row][col] += targetData[i].data[row] * targetData[i].data[col];
    }


//    end_timer(11);
//    start_timer(12);
    // second pass to finalize average and sum of all leafs
    #pragma omp teams distribute parallel for
   for (int i = 0; i < sizeOfDatacell; i++)
	{
	    // if not enough points it canot accuratly approximated using a normal distribution
	    Vec3 pointSum = {dataCell[i].mean[0], dataCell[i].mean[1], dataCell[i].mean[2]};

	    dataCell[i].mean[0] /= dataCell[i].numberPoints;
	    dataCell[i].mean[1] /= dataCell[i].numberPoints;
	    dataCell[i].mean[2] /= dataCell[i].numberPoints;
	    //	    invCovariance = (invCovariance - 2*(pointSum * mean.transpose)) / numberPoints + mean * mean.transpose;
	    //  invCovariance *= (numberPoints - 1.0) / leaf.nr_points;
	    for (int row = 0; row < 3; row++)
		for (int col = 0; col < 3; col++)
		    {
			invCovarianceDeepCopy[i].data[row][col] = (invCovarianceDeepCopy[i].data[row][col] -
									 2 * (pointSum[row] * dataCell[i].mean[col])) / sizeOfDatacell +
			    dataCell[i].mean[row]*dataCell[i].mean[col];

			invCovarianceDeepCopy[i].data[row][col] *= (sizeOfDatacell -1.0) / dataCell[i].numberPoints;
		    }


	    // Avoids matrices near singularities (eq 6.11)[Magnusson 2009]
	    // if (eigen_val < min_covar_eig_value)
	    // invCovariance = evecs * eigen_val * leaf.evecs.inverse;

	    // so far computed covariance

	    invertMatrix(invCovarianceDeepCopy[i]);

	    /*if (not invertable)
		numberPoints = -1;
	    */
	}

//   end_timer(12);
//   end_timer(0);
  }

  for (int i = 0; i < sizeOfDatacell; i++) {
    dataCell[i].invCovariance = invCovarianceDeepCopy[i];
  }
//  std::cout << dataCell[0].invCovariance.data[0][0] <<","<< dataCell[0].invCovariance.data[1][1]<<","<< dataCell[0].invCovariance.data[2][2] << '\n';
}

void ndt_mapping::deinitCompute()
{

}

// removed "PointCloudSource output" parameter as it was not used in the Autoware code
void ndt_mapping::ndt_align (const Matrix4f& guess)
{
    PointCloud output;

    initCompute ();

    // Resize the output dataset
    output.resize (input_->size ());

    // Copy the point data to output
    for (size_t i = 0; i < input_->size (); ++i)
	output[i] = (*input_)[i];

    // Perform the actual transformation computation
    converged_ = false;
    final_transformation_ = transformation_ = previous_transformation_ = Matrix4f_Identity;

    // Right before we estimate the transformation, we set all the point.data[3] values to 1 to aid the rigid
    // transformation
    for (size_t i = 0; i < input_->size (); ++i)
	output[i].data[3] = 1.0;

    computeTransformation (output, guess);

    deinitCompute ();
}

// returns squared euclidean distance of two points a,b
double distance_sqr(const PointXYZI& a, const PointXYZI& b)
{
    double dx = a.data[0]-b.data[0];
    double dy = a.data[1]-b.data[1];
    double dz = a.data[2]-b.data[2];

    return dx*dx + dy*dy + dz*dz;
}

// find nearest via finding its voxel and the neighbouring, and then check all points in it
inline double ndt_mapping::findNearest(PointXYZI &p)
{
    double min = std::numeric_limits<double>::max();
    double dist = 0.0;
    for (size_t j = 0; j < target_->size(); j++)
	{
	    dist = distance_sqr((*target_)[j], p);
	    if (dist < min)
		min = dist;
	}
    return min;
}

// fs: functions called, actually output_cloud is not used as the result, later in the original code in Autoware
// final_transformation_ is get and and converged_ is checked
CallbackResult ndt_mapping::partial_points_callback(PointCloud &input_cloud, Matrix4f &init_guess, PointCloud& target_cloud)
{
    CallbackResult result;

    input_ = &input_cloud;
    target_ = &target_cloud;

    ndt_align(init_guess);
    result.final_transformation = final_transformation_;
    result.converged = converged_;

    return result;
}

void ndt_mapping::run(int p) {
    //std::chrono::high_resolution_clock::time_point start,end;
    //std::chrono::duration<double> elapsed;
    //std::chrono::high_resolution_clock timer;
    pause_func();
	//std::string a[] = {"initCompute"," Compute Transformation","TransformPointcloud","Updatederivates","while in TransformPointCloud","VoxelRadiusSearch","solve","computeHessian","HessianUpdate","computeDerivatives","computeStepLengthMT","initCompute first pass","initCompute second pass"};
	//init_timer(a,13);
    //std::cout << "Define number of threads" << '\n';
    //int corenum  ;
    //std::cin >> corenum;
    //omp_set_num_threads(corenum);
	//std::cout << "Threads: " << omp_get_num_threads() << std::endl;

    while (read_testcases < testcases)
	{
	    int count = read_next_testcases(p);
	    unpause_func();
	    for (int i = 0; i < count; i++)
		{
		    // actual kernel invocation
		    results[i] = partial_points_callback(filtered_scan_ptr[i], init_guess[i], maps[i]);
		}
	    pause_func();
	    check_next_outputs(count);
	}
    //print_timer;
}

void ndt_mapping::check_next_outputs(int count)
{
    CallbackResult reference;
    for (int i = 0; i < count; i++)
	{
	    parseResult(output_file, &reference);
	    if (results[i].converged != reference.converged)
		{
		    error_so_far = true;
		}
	    for (int h = 0; h < 4; h++)
		for (int w = 0; w < 4; w++)
		    {
			if (fabs(reference.final_transformation.data[h][w] - results[i].final_transformation.data[h][w]) > max_delta)
			    max_delta = fabs(reference.final_transformation.data[h][w] - results[i].final_transformation.data[h][w]);
		    }
	}
}

bool ndt_mapping::check_output() {
    std::cout << "checking output \n";

    input_file.close();
    output_file.close();

    std::cout << "max delta: " << max_delta << "\n";
    if ((max_delta > MAX_EPS) || error_so_far)
	return false;
    return true;
}

ndt_mapping a = ndt_mapping();
kernel& myKernel = a;