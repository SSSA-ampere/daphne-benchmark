/**
 * Author:  Florian Stock, Technische Universität Darmstadt,
 * Embedded Systems & Applications Group 2018
 * License: Apache 2.0 (see attachached File)
 */
#include "benchmark.h"
#include "datatypes.h"
#include "kernel.h"
#include <cmath>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <ios>


/**
 * Parses the next point cloud from the input stream.
 */
void  parsePointCloud(std::ifstream& input_file, PointCloud2* pointcloud2) {
	try {
		input_file.read((char*)&(pointcloud2->height), sizeof(int32_t));
		input_file.read((char*)&(pointcloud2->width), sizeof(int32_t));
		input_file.read((char*)&(pointcloud2->point_step), sizeof(uint32_t));
		pointcloud2->data = new float[pointcloud2->height * pointcloud2->width * pointcloud2->point_step];
		input_file.read((char*)pointcloud2->data, pointcloud2->height * pointcloud2->width * pointcloud2->point_step);
    }  catch (std::ifstream::failure) {
		throw std::ios_base::failure("Error reading the next point cloud.");
    }
}
/**
 * Parses the next camera extrinsic matrix.
 */
void  parseCameraExtrinsicMat(std::ifstream& input_file, Mat44* cameraExtrinsicMat) {
	try {
		for (int h = 0; h < 4; h++)
			for (int w = 0; w < 4; w++)
				input_file.read((char*)&(cameraExtrinsicMat->data[h][w]),sizeof(double));
	} catch (std::ifstream::failure) {
		throw std::ios_base::failure("Error reading the next extrinsic matrix.");
	}
}
/**
 * Parses the next camera matrix.
 */
void parseCameraMat(std::ifstream& input_file, Mat33* cameraMat ) {
	try {
	for (int h = 0; h < 3; h++)
		for (int w = 0; w < 3; w++)
			input_file.read((char*)&(cameraMat->data[h][w]), sizeof(double));
	} catch (std::ifstream::failure) {
		throw std::ios_base::failure("Error reading the next camera matrix.");
    }
}
/**
 * Parses the next distance coefficients.
 */
void  parseDistCoeff(std::ifstream& input_file, Vec5* distCoeff) {
	try {
		for (int w = 0; w < 5; w++)
			input_file.read((char*)&(distCoeff->data[w]), sizeof(double));
	} catch (std::ifstream::failure) {
		throw std::ios_base::failure("Error reading the next set of distance coefficients.");
	}
}
/**
 * Parses the next image sizes.
 */
void  parseImageSize(std::ifstream& input_file, ImageSize* imageSize) {
	try {
		input_file.read((char*)&(imageSize->width), sizeof(int32_t));
		input_file.read((char*)&(imageSize->height), sizeof(int32_t));
	} catch (std::ifstream::failure) {
		throw std::ios_base::failure("Error reading the next image size.");
	}
}
/**
 * Parses the next reference image.
 */
void parsePointsImage(std::ifstream& output_file, PointsImage* goldenResult) {
	try {
		// read data of static size
		output_file.read((char*)&(goldenResult->image_width), sizeof(int32_t));
		output_file.read((char*)&(goldenResult->image_height), sizeof(int32_t));
		output_file.read((char*)&(goldenResult->max_y), sizeof(int32_t));
		output_file.read((char*)&(goldenResult->min_y), sizeof(int32_t));
		int pos = 0;
		int elements = goldenResult->image_height * goldenResult->image_width;
		goldenResult->intensity = new float[elements];
		goldenResult->distance = new float[elements];
		goldenResult->min_height = new float[elements];
		goldenResult->max_height = new float[elements];
		// read data of variable size
		for (int h = 0; h < goldenResult->image_height; h++)
			for (int w = 0; w < goldenResult->image_width; w++)
			{
				output_file.read((char*)&(goldenResult->intensity[pos]), sizeof(float));
				output_file.read((char*)&(goldenResult->distance[pos]), sizeof(float));
				output_file.read((char*)&(goldenResult->min_height[pos]), sizeof(float));
				output_file.read((char*)&(goldenResult->max_height[pos]), sizeof(float));
				pos++;
			}
	} catch (std::ios_base::failure) {
		throw std::ios_base::failure("Error reading the next reference image.");
	}
}


int points2image::read_next_testcases(int count)
{
	// free the memory that has been allocated in the previous iteration
	// and allocate new for the currently required data sizes
	if (pointcloud2)
		for (int m = 0; m < count; ++m)
			delete [] pointcloud2[m].data;
	delete [] pointcloud2;
	pointcloud2 = new PointCloud2[count];
	delete [] cameraExtrinsicMat;
	cameraExtrinsicMat = new Mat44[count];
	delete [] cameraMat;
	cameraMat = new Mat33[count];
	delete [] distCoeff;
	distCoeff = new Vec5[count];
	delete [] imageSize;
	imageSize = new ImageSize[count];
	if (results)
	for (int m = 0; m < count; ++m)
	{
		delete [] results[m].intensity;
		delete [] results[m].distance;
		delete [] results[m].min_height;
		delete [] results[m].max_height;
	}
	delete [] results;
	results = new PointsImage[count];

	// iteratively read the data for the test cases
	int i;
	for (i = 0; (i < count) && (read_testcases < testcases); i++,read_testcases++)
	{
		try {
			parsePointCloud(input_file, pointcloud2 + i);
			parseCameraExtrinsicMat(input_file, cameraExtrinsicMat + i);
			parseCameraMat(input_file, cameraMat + i);
			parseDistCoeff(input_file, distCoeff + i);
			parseImageSize(input_file, imageSize + i);
		} catch (std::ios_base::failure& e) {
			std::cerr << e.what() << std::endl;
			exit(-3);
		}
	}

	return i;
}

int points2image::read_number_testcases(std::ifstream& input_file)
{
	// reads the number of testcases in the data stream
	int32_t number;
	try {
		input_file.read((char*)&(number), sizeof(int32_t));
	} catch (std::ifstream::failure) {
		throw std::ios_base::failure("Error reading the number of testcases.");
	}

	return number;
}

void points2image::check_next_outputs(int count)
{
	PointsImage reference;
	// parse the next reference image
	// and compare it to the data generated by the algorithm
	for (int i = 0; i < count; i++)
	{
		std::ostringstream sError;
		int caseErrorNo = 0;
		try {
			parsePointsImage(output_file, &reference);
		} catch (std::ios_base::failure& e) {
			std::cerr << e.what() << std::endl;
			exit(-3);
		}
		// detect image size deviation
		if ((results[i].image_height != reference.image_height)
			|| (results[i].image_width != reference.image_width))
		{
			error_so_far = true;
			caseErrorNo += 1;
			sError << " deviating image size: [" << results[i].image_width << " ";
			sError << results[i].image_height << "] should be [";
			sError << reference.image_width << " " << reference.image_height << "]" << std::endl;
		}
		// detect image extend deviation
		if ((results[i].min_y != reference.min_y)
			|| (results[i].max_y != reference.max_y))
		{
			error_so_far = true;
			caseErrorNo += 1;
			sError << " deviating vertical intervall: [" << results[i].min_y << " ";
			sError << results[i].max_y << "] should be [";
			sError << reference.min_y << " " << reference.max_y << "]" << std::endl;
		}
		// compare all pixels
		int pos = 0;
		for (int h = 0; h < reference.image_height; h++)
			for (int w = 0; w < reference.image_width; w++)
			{
				// test for intensity
				float delta = std::fabs(reference.intensity[pos] - results[i].intensity[pos]);
				if (delta > max_delta) {
					max_delta = delta;
				}
				if (delta > MAX_EPS) {
					sError << " at [" << w << " " << h << "]: Intensity " << results[i].intensity[pos];
					sError << " should be " << reference.intensity[pos] << std::endl;
					caseErrorNo += 1;
				}
				// test for distance
				delta = std::fabs(reference.distance[pos] - results[i].distance[pos]);
				if (delta > max_delta) {
					max_delta = delta;
				}
				if (delta > MAX_EPS) {
					sError << " at [" << w << " " << h << "]: Distance " << results[i].distance[pos];
					sError << " should be " << reference.distance[pos] << std::endl;
					caseErrorNo += 1;
				}
				// test for min height
				delta = std::fabs(reference.min_height[pos] - results[i].min_height[pos]);
				if (delta > max_delta) {
					max_delta = delta;
				}
				if (delta > MAX_EPS) {
					sError << " at [" << w << " " << h << "]: Min height " << results[i].min_height[pos];
					sError << " should be " << reference.min_height[pos] << std::endl;
					caseErrorNo += 1;
				}
				// test for max height
				delta = std::fabs(reference.max_height[pos] - results[i].max_height[pos]);
				if (delta > max_delta) {
					max_delta = delta;
				}
				if (delta > MAX_EPS) {
					sError << " at [" << w << " " << h << "]: Max height " << results[i].max_height[pos];
					sError << " should be " << reference.max_height[pos] << std::endl;
					caseErrorNo += 1;
				}
				pos += 1;
			}
		if (caseErrorNo > 0) {
			std::cerr << "Errors for test case " << read_testcases - count + i;
			std::cerr << " (" << caseErrorNo << "):" << std::endl;
			std::cerr << sError.str() << std::endl;
		}
		// free the memory allocated by the reference image read above
		delete [] reference.intensity;
		delete [] reference.distance;
		delete [] reference.min_height;
		delete [] reference.max_height;
	}
}

bool points2image::check_output() {
	std::cout << "checking output \n";
	// complement to init()
	input_file.close();
	output_file.close();
	std::cout << "max delta: " << max_delta << "\n";
	if ((max_delta > MAX_EPS) || error_so_far) {
		return false;
	} else {
		return true;
	}
}
// set the external kernel instance used in main()
points2image a = points2image();
kernel& myKernel = a;
