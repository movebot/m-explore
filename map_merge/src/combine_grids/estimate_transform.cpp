/*********************************************************************
 *
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2015-2016, Jiri Horner.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the author nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 *********************************************************************/

#include <combine_grids/estimate_transform.h>

#include <opencv2/core/utility.hpp>
#include <opencv2/stitching/detail/autocalib.hpp>
#include <opencv2/stitching/detail/blenders.hpp>
#include <opencv2/stitching/detail/camera.hpp>
#include <opencv2/stitching/detail/exposure_compensate.hpp>
#include <opencv2/stitching/detail/matchers.hpp>
#include <opencv2/stitching/detail/motion_estimators.hpp>
#include <opencv2/stitching/detail/seam_finders.hpp>
#include <opencv2/stitching/detail/util.hpp>
#include <opencv2/stitching/detail/warpers.hpp>
#include <opencv2/stitching/warpers.hpp>

#include <opencv2/video/tracking.hpp>

#include <ros/console.h>
#include <nav_msgs/OccupancyGrid.h>

#include <iostream>

namespace combine_grids
{
namespace internal
{
bool opencvEstimateTransform(const std::vector<cv::Mat>& images)
{
  std::vector<cv::detail::ImageFeatures> image_features;
  std::vector<cv::detail::MatchesInfo> pairwise_matches;
  std::vector<cv::detail::CameraParams> transforms;
  cv::Ptr<cv::detail::FeaturesFinder> finder =
      cv::makePtr<cv::detail::OrbFeaturesFinder>();
  cv::Ptr<cv::detail::FeaturesMatcher> matcher =
      cv::makePtr<cv::detail::BestOf2NearestRangeMatcher>();
  cv::Ptr<cv::detail::Estimator> estimator =
      cv::makePtr<cv::detail::HomographyBasedEstimator>();

  if (images.size() < 2) {
    return false;
  }

  /* find features in images */
  ROS_DEBUG("computing features");
  image_features.reserve(images.size());
  for (const cv::Mat& image : images) {
    image_features.emplace_back();
    (*finder)(image, image_features.back());
  }
  finder->collectGarbage();

  /* find corespondent features */
  // matches only some (5) images, scales better than full pairwise matcher
  ROS_DEBUG("pairwise matching features");
  (*matcher)(image_features, pairwise_matches);
  matcher->collectGarbage();

  /* estimate transform */
  ROS_DEBUG("estimating final transform");
  if (!(*estimator)(image_features, pairwise_matches, transforms)) {
    return false;
  }

  for (cv::detail::CameraParams& transform : transforms) {
    ROS_DEBUG("TRANSFORM ppx: %f, ppy %f, aspect: %f, focal %f \n",
              transform.ppx, transform.ppy, transform.aspect, transform.focal);
    ROS_DEBUG("R,K,t:");
    std::cout << transform.R << std::endl;
    std::cout << transform.K() << std::endl;
    std::cout << transform.t << std::endl;
    ROS_DEBUG("trans x: %f, trans y %f", transform.R.at<double>(0, 2),
              transform.R.at<double>(1, 2));
  }

  for (auto& match : pairwise_matches) {
    ROS_DEBUG("H:");
    std::cout << match.H << std::endl;
    if (!match.H.empty())
      ROS_DEBUG("trans x: %f, trans y %f, rot %f\n", match.H.at<double>(0, 2),
                match.H.at<double>(1, 2),
                atan2(match.H.at<double>(0, 1), match.H.at<double>(1, 1)));
    ROS_DEBUG("src_id %d, dst_id %d, confidence %f\n", match.src_img_idx,
              match.dst_img_idx, match.confidence);

    if (match.src_img_idx == 0 && match.dst_img_idx == 1) {
      auto& matches_info = match;
      ROS_DEBUG("processing RIGID.");
      // Construct point-point correspondences for homography estimation
      cv::Mat src_points(1, static_cast<int>(matches_info.matches.size()),
                         CV_32FC2);
      cv::Mat dst_points(1, static_cast<int>(matches_info.matches.size()),
                         CV_32FC2);

      // Construct point-point correspondences for inliers only
      src_points.create(1, matches_info.num_inliers, CV_32FC2);
      dst_points.create(1, matches_info.num_inliers, CV_32FC2);
      int inlier_idx = 0;
      for (size_t i = 0; i < matches_info.matches.size(); ++i) {
        if (!matches_info.inliers_mask[i])
          continue;

        const cv::DMatch& m = matches_info.matches[i];

        cv::Point2f p =
            image_features[matches_info.src_img_idx].keypoints[m.queryIdx].pt;
        p.x -= image_features[matches_info.src_img_idx].img_size.width * 0.5f;
        p.y -= image_features[matches_info.src_img_idx].img_size.height * 0.5f;
        src_points.at<cv::Point2f>(0, inlier_idx) = p;

        p = image_features[matches_info.dst_img_idx].keypoints[m.trainIdx].pt;
        p.x -= image_features[matches_info.dst_img_idx].img_size.width * 0.5f;
        p.y -= image_features[matches_info.dst_img_idx].img_size.height * 0.5f;
        dst_points.at<cv::Point2f>(0, inlier_idx) = p;

        inlier_idx++;
      }

      cv::Mat H = cv::estimateRigidTransform(src_points, dst_points, false);
      ROS_DEBUG("src_id %d, dst_id %d, confidence %f\n", match.src_img_idx,
                match.dst_img_idx, match.confidence);
      std::cout << H << std::endl;
      if(!H.empty())
	      ROS_DEBUG("trans x: %f, trans y %f, rot %f\n", H.at<double>(0, 2),
	                H.at<double>(1, 2),
	                atan2(H.at<double>(0, 1), H.at<double>(1, 1)));
    }
  }

  return true;
}
}  // namespace internal
}  // namespace combine_grids
