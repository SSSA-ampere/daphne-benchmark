/**
 * Author:  Florian Stock, Technische Universität Darmstadt,
 * Embedded Systems & Applications Group 2018
 * Author:  Thilo Gabel, Technische Universität Darmstadt,
 * Embedded Systems & Applications Group 2019 - 2020
 * License: Apache 2.0 (see attached files)
 */
#ifndef EPHOS_POINTS2IMAGE_H
#define EPHOS_POINTS2IMAGE_H

#include <vector>
#include <cmath>
#include <iostream>
#include <fstream>
#include <string>

#include "common/benchmark.h"
#include "common/points2image_base.h"

// maximum allowed deviation from the reference results
#define MAX_EPS 0.001

class points2image : public points2image_base {

public:
	points2image();
	virtual ~points2image();
public:

	virtual void init() override;

	virtual void quit() override;
protected:
	/**
	 * Transforms the given point cloud and produces the result as a two dimensional image.
	 * cloud: input point cloud
	 * cameraExtrinsicMat: perspective projection matrix
	 * distCoeff: distortion coefficients
	 * cameraMat: internal camera matrix
	 * imageSize: output image dimensions
	 * return: output image
	 */
	virtual PointsImage cloud2Image(
		PointCloud& cloud,
		Mat44& cameraExtrinsicMat,
		Mat33& cameraMat,
		Vec5& distCoeff,
		ImageSize& imageSize) override;
	};

#endif // EPHOS_POINTS2IMAGE_H
