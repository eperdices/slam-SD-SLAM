/**
 *
 *  Copyright (C) 2017 Eduardo Perdices <eperdices at gsyc dot es>
 *
 *  The following code is a derivative work of the code from the ORB-SLAM2 project,
 *  which is licensed under the GNU Public License, version 3. This code therefore
 *  is also licensed under the terms of the GNU Public License, version 3.
 *  For more information see <https://github.com/raulmur/ORB_SLAM2>.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef SD_SLAM_ORBMATCHER_H
#define SD_SLAM_ORBMATCHER_H

#include <vector>
#include <opencv2/core/core.hpp>
#include <opencv2/features2d/features2d.hpp>
#include <Eigen/Dense>
#include "MapPoint.h"
#include "KeyFrame.h"
#include "Frame.h"

namespace SD_SLAM {

class ORBmatcher {
 public:
  ORBmatcher(float nnratio = 0.6, bool checkOri=true);

  // Computes the Hamming distance between two ORB descriptors
  static int DescriptorDistance(const cv::Mat &a, const cv::Mat &b);

  // Search matches between Frame keypoints and projected MapPoints. Returns number of matches
  // Used to track the local map (Tracking)
  int SearchByProjection(Frame &F, const std::vector<MapPoint*> &vpMapPoints, const float th=3);

  // Project MapPoints tracked in last frame into the current frame and search matches.
  // Used to track from previous frame (Tracking)
  int SearchByProjection(Frame &CurrentFrame, const Frame &LastFrame, const float th, const bool bMono);
  int SearchByProjection(Frame &CurrentFrame, KeyFrame* pKF, const float th, const bool bMono);

  // Compare matched points in both keyframes.
  // Used to search loops (LoopClosing)
  int SearchByPoints(KeyFrame* currentKF, KeyFrame* pKF, std::vector<MapPoint*> &matches);

  // Project MapPoints seen in KeyFrame into the Frame and search matches.
  // Used in relocalisation (Tracking)
  int SearchByProjection(Frame &CurrentFrame, KeyFrame* pKF, const std::set<MapPoint*> &sAlreadyFound, const float th, const int ORBdist);

  // Project MapPoints using a Similarity Transformation and search matches.
  // Used in loop detection (Loop Closing)
   int SearchByProjection(KeyFrame* pKF, const Eigen::Matrix4d &Scw, const std::vector<MapPoint*> &vpPoints, std::vector<MapPoint*> &vpMatched, int th);

  // Matching for the Map Initialization (only used in the monocular case)
  int SearchForInitialization(Frame &F1, Frame &F2, std::vector<cv::Point2f> &vbPrevMatched, std::vector<int> &vnMatches12, int windowSize=10);

  // Matching to triangulate new MapPoints. Check Epipolar Constraint.
  int SearchForTriangulation(KeyFrame *pKF1, KeyFrame* pKF2, const Eigen::Matrix3d &F12,
                 std::vector<std::pair<size_t, size_t> > &vMatchedPairs);

  // Search matches between MapPoints seen in KF1 and KF2 transforming by a Sim3 [s12*R12|t12]
  // In the stereo and RGB-D case, s12=1
  int SearchBySim3(KeyFrame* pKF1, KeyFrame* pKF2, std::vector<MapPoint *> &vpMatches12, const float &s12, const Eigen::Matrix3d &R12, const Eigen::Vector3d &t12, const float th);

  // Project MapPoints into KeyFrame and search for duplicated MapPoints.
  int Fuse(KeyFrame* pKF, const std::vector<MapPoint *> &vpMapPoints, const float th=3.0);

  // Project MapPoints into KeyFrame using a given Sim3 and search for duplicated MapPoints.
  int Fuse(KeyFrame* pKF, const Eigen::Matrix4d &Scw, const std::vector<MapPoint*> &vpPoints, float th, std::vector<MapPoint *> &vpReplacePoint);

 public:
  static const int TH_LOW;
  static const int TH_HIGH;
  static const int HISTO_LENGTH;

 protected:
  bool CheckDistEpipolarLine(const cv::KeyPoint &kp1, const cv::KeyPoint &kp2, const Eigen::Matrix3d &F12, const KeyFrame *pKF);

  float RadiusByViewingCos(const float &viewCos);

  void ComputeThreeMaxima(std::vector<int>* histo, const int L, int &ind1, int &ind2, int &ind3);

  float mfNNratio;
  bool mbCheckOrientation;
};

}  // namespace SD_SLAM

#endif  // SD_SLAM_ORBMATCHER_H
