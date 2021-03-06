// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// gDLS*: Generalized Pose-and-Scale Estimation Given Scale and Gravity Priors
//
// Victor Fragoso, Joseph DeGol, Gang Hua.
// Proc. of the IEEE/CVF Conf. on Computer Vision and Pattern Recognition 2020.
//
// Please contact the author of this library if you have any questions.
// Author: Victor Fragoso (victor.fragoso@microsoft.com)

#include "gdls_star/gdls_star_robust_estimator.h"

#include <algorithm>
#include <limits>
#include <numeric>
#include <random>
#include <utility>
#include <vector>
#include <glog/logging.h>
#include "gdls_star/camera_feature_correspondence_2d_3d.h"
#include "gdls_star/util.h"

namespace msft {

// Minimal sample size.
constexpr int kMinimalSampleSize = 4;

GdlsStarRobustEstimator::GdlsStarRobustEstimator(
    const GdlsStarRobustEstimator::RansacParameters& ransac_params) :
    params_(ransac_params), prng_(ransac_params.seed) {
  // Make sure parameters are valid.
  CHECK_GT(params_.failure_probability, 0.0);
  CHECK_LT(params_.failure_probability, 1.0);
  CHECK_GT(params_.reprojection_error_thresh, 0.0);
  CHECK_GE(params_.min_iterations, 0);
  CHECK_GT(params_.max_iterations, params_.min_iterations);
}

int GdlsStarRobustEstimator::RandInt(const int min_value, const int max_value) {
  std::uniform_int_distribution<int> index_dist(min_value, max_value);
  return index_dist(prng_);
}

std::vector<CameraFeatureCorrespondence2D3D>
GdlsStarRobustEstimator::Sample(
    const std::vector<CameraFeatureCorrespondence2D3D>& correspondences) {
  std::vector<CameraFeatureCorrespondence2D3D> sample;
  sample.reserve(kMinimalSampleSize);
  const int num_correspondences = correspondences.size();
  for (int i = 0; i < kMinimalSampleSize; ++i) {
    // Randomly sample from the correspondence indices.
    std::swap(correspondence_indices_[i],
              correspondence_indices_[RandInt(i, num_correspondences - 1)]);
    // Copy correspondence.
    sample.push_back(correspondences[correspondence_indices_[i]]);
  }
  return sample;
}

double GdlsStarRobustEstimator::UpdateBestSolution(
    const std::vector<CameraFeatureCorrespondence2D3D>& correspondences,
    const GdlsStar::Solution& estimated_solns,
    GdlsStar::Solution* best_solution,
    std::vector<int>* best_inliers) {
  const int num_estimated_solns = estimated_solns.rotations.size();
  // Check reprojection errors for every point.
  const double sq_reprojection_error_thresh =
      params_.reprojection_error_thresh * params_.reprojection_error_thresh;

  std::vector<int> inliers;
  inliers.reserve(correspondences.size());
  Eigen::Vector2d pixel;
  double best_inlier_ratio =
      best_inliers->size() / static_cast<double>(correspondences.size()) +
      std::numeric_limits<double>::epsilon();
  for (int i = 0; i < num_estimated_solns; ++i) {
    const Eigen::Quaterniond& rotation = estimated_solns.rotations[i];
    const Eigen::Vector3d& translation = estimated_solns.translations[i];
    const double& scale = estimated_solns.scales[i];
    inliers.clear();
    VLOG(3) << "Rotation matrix: \n" << rotation.toRotationMatrix();
    VLOG(3) << "Translation: " << translation.transpose();
    VLOG(3) << "Scale: " << scale;
    for (int j = 0; j < correspondences.size(); ++j) {
      // Compute point coordinates wrt generalized coordinate frame:
      //   scale * cam_position + depth * ray = rotation * point + translation
      //   cam_position + d' * ray = (rotation * point + translation) / scale,
      // where d' is = depth / scale.
      const Eigen::Vector3d point_in_gen_camera =
          (rotation * correspondences[j].point + translation) / scale;
      // Project point in camera.
      const PinholeCamera& camera = correspondences[j].camera;
      if (camera.ProjectPoint(point_in_gen_camera, &pixel) < 0) {
        continue;
      }
      // Reprojection error.
      const double sq_reprojection_error =
          (pixel - correspondences[j].observation).squaredNorm();
      // Is it an inlier?
      if (sq_reprojection_error < sq_reprojection_error_thresh) {
        inliers.push_back(j);
      }
    }
    // Do we have more inliers than the best solution? If so, then update best
    // solution and inliers.
    if (best_inliers->size() < inliers.size()) {
      *best_inliers = inliers;
      best_solution->rotations[0] = rotation;
      best_solution->translations[0] = translation;
      best_solution->scales[0] = scale;
      best_inlier_ratio =
          best_inliers->size() / static_cast<double>(correspondences.size());
      VLOG(3) << "Update num. inliers: " << best_inliers->size();
      VLOG(3) << "Update inlier ratio: " << best_inlier_ratio;
    }
  }

  return best_inlier_ratio;
}

int GdlsStarRobustEstimator::ComputeMaxIterations(
    const double inlier_ratio,
    const double log_failure_prob) {
  CHECK_GT(inlier_ratio, 0.0);
  if (inlier_ratio == 1.0) {
    return params_.min_iterations;
  }

  // Log. probability of producing a bad hypothesis.
  const double log_prob =
      std::log(1.0 - pow(inlier_ratio, kMinimalSampleSize)) -
      std::numeric_limits<double>::epsilon();

  // Compute the number of iterations to achieve a certain confidence.
  const int num_iterations = static_cast<int>(log_failure_prob / log_prob);

  return std::clamp(num_iterations,
                    params_.min_iterations,
                    params_.max_iterations);
}

GdlsStar::Solution GdlsStarRobustEstimator::Estimate(
    const GdlsStar::Priors& priors,
    const std::vector<CameraFeatureCorrespondence2D3D>& correspondences,
    RansacSummary* ransac_summary) {
  RansacSummary& summary = *CHECK_NOTNULL(ransac_summary);
  summary.inliers.clear();

  // Check that we have enough correspondences to produce a single hypothesis.
  CHECK(correspondences.size() >= kMinimalSampleSize)
      << "Not enough correspondences.";

  // Initialize correspondence indices.
  correspondence_indices_.clear();
  correspondence_indices_.resize(correspondences.size());
  std::iota(correspondence_indices_.begin(), correspondence_indices_.end(), 0);

  // The hypothesis-and-test loop.
  const double log_failure_prob = std::log(params_.failure_probability);
  int max_iterations = params_.max_iterations;
  std::vector<CameraFeatureCorrespondence2D3D> sample;
  GdlsStar::Solution hypotheses;
  // Initialize best solution to identity solution.
  GdlsStar::Solution best_solution;
  best_solution.rotations.push_back(Eigen::Quaterniond::Identity());
  best_solution.translations.push_back(Eigen::Vector3d::Zero());
  best_solution.scales.push_back(1.0);
  GdlsStar::Input input;
  double inlier_ratio = 0.0;
  for (summary.num_iterations = 0;
       summary.num_iterations < max_iterations;
       ++summary.num_iterations) {
    // Compute a minimal sample to produce hypotheses.
    sample = Sample(correspondences);

    // Compute hypotheses.
    input = ComputeInputDatum(sample);
    input.priors = priors;

    if (!estimator_.EstimateSimilarityTransformation(input, &hypotheses)) {
      VLOG(3) << "Failed to estimate hypotheses. Skipping sample ...";
      continue;
    }

    summary.num_hypotheses += hypotheses.rotations.size();
    VLOG(3) << "Num. candidate solutions: " << hypotheses.rotations.size();

    // Update best solution.
    inlier_ratio = UpdateBestSolution(correspondences,
                                      hypotheses,
                                      &best_solution,
                                      &summary.inliers);

    // Update max. iterations.
    max_iterations = ComputeMaxIterations(inlier_ratio, log_failure_prob);
  }

  // Compute confidence.
  summary.confidence =
      1.0 - std::pow(1.0 - std::pow(inlier_ratio, kMinimalSampleSize),
                     summary.num_iterations);
  VLOG(3) << "Best inlier ratio: " << inlier_ratio;
  VLOG(3) << "Confidence: " << summary.confidence;
  return best_solution;
}

}  // namespace msft
