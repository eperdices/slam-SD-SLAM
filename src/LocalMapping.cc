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

#include "LocalMapping.h"
#include <unistd.h>
#include "LoopClosing.h"
#include "ORBmatcher.h"
#include "Optimizer.h"
#include "Converter.h"
#include "extra/log.h"

using std::vector;
using std::list;
using std::map;
using std::mutex;
using std::unique_lock;
using std::endl;

namespace SD_SLAM {

LocalMapping::LocalMapping(Map *pMap, const float bMonocular):
  mbMonocular(bMonocular), mbResetRequested(false), mbFinishRequested(false), mbFinished(true), mpMap(pMap),
  mbAbortBA(false), mbStopped(false), mbStopRequested(false), mbNotStop(false), mbAcceptKeyFrames(true) {

  mpLoopCloser = nullptr;
  mpTracker = nullptr;
}

void LocalMapping::SetLoopCloser(LoopClosing* pLoopCloser) {
  mpLoopCloser = pLoopCloser;
}

void LocalMapping::SetTracker(Tracking *pTracker) {
  mpTracker=pTracker;
}

void LocalMapping::Run() {
  mbFinished = false;

  while (1) {
    // Tracking will see that Local Mapping is busy
    SetAcceptKeyFrames(false);

    // Check if there are keyframes in the queue
    if (CheckNewKeyFrames()) {
      // Insertion in Map
      ProcessNewKeyFrame();

      // Check recent MapPoints
      MapPointCulling();

      // Triangulate new MapPoints
      CreateNewMapPoints();

      if (!CheckNewKeyFrames()) {
        // Find more matches in neighbor keyframes and fuse point duplications
        SearchInNeighbors();
      }

      mbAbortBA = false;

      if (!CheckNewKeyFrames() && !stopRequested()) {
        // Local BA
        if (mpMap->KeyFramesInMap()>2)
          Optimizer::LocalBundleAdjustment(mpCurrentKeyFrame, &mbAbortBA, mpMap);

        // Check redundant local Keyframes
        KeyFrameCulling();
      }

      if (mpLoopCloser)
        mpLoopCloser->InsertKeyFrame(mpCurrentKeyFrame);
    } else if (Stop()) {
      // Safe area to stop
      while (isStopped() && !CheckFinish()) {
        usleep(3000);
      }
      if (CheckFinish())
        break;
    }

    ResetIfRequested();

    // Tracking will see that Local Mapping is busy
    SetAcceptKeyFrames(true);

    if (CheckFinish())
      break;

    usleep(3000);
  }

  SetFinish();
}

void LocalMapping::InsertKeyFrame(KeyFrame *pKF) {
  unique_lock<mutex> lock(mMutexNewKFs);
  mlNewKeyFrames.push_back(pKF);
  mbAbortBA=true;
}


bool LocalMapping::CheckNewKeyFrames() {
  unique_lock<mutex> lock(mMutexNewKFs);
  return(!mlNewKeyFrames.empty());
}

void LocalMapping::ProcessNewKeyFrame() {
  {
    unique_lock<mutex> lock(mMutexNewKFs);
    mpCurrentKeyFrame = mlNewKeyFrames.front();
    mlNewKeyFrames.pop_front();
  }

  // Associate MapPoints to the new keyframe and update normal and descriptor
  const vector<MapPoint*> vpMapPointMatches = mpCurrentKeyFrame->GetMapPointMatches();

  for (size_t i = 0; i < vpMapPointMatches.size(); i++) {
    MapPoint* pMP = vpMapPointMatches[i];
    if (pMP) {
      if (!pMP->isBad()) {
        if (!pMP->IsInKeyFrame(mpCurrentKeyFrame)) {
          pMP->AddObservation(mpCurrentKeyFrame, i);
          pMP->UpdateNormalAndDepth();
          pMP->ComputeDistinctiveDescriptors();
        } else { // this can only happen for new stereo points inserted by the Tracking
          mlpRecentAddedMapPoints.push_back(pMP);
        }
      }
    }
  }

  // Update links in the Covisibility Graph
  mpCurrentKeyFrame->UpdateConnections();

  // Insert Keyframe in Map
  mpMap->AddKeyFrame(mpCurrentKeyFrame);
}

void LocalMapping::MapPointCulling() {
  // Check Recent Added MapPoints
  list<MapPoint*>::iterator lit = mlpRecentAddedMapPoints.begin();
  const unsigned long int nCurrentKFid = mpCurrentKeyFrame->mnId;

  int nThObs;
  if (mbMonocular)
    nThObs = 2;
  else
    nThObs = 3;
  const int cnThObs = nThObs;

  while (lit != mlpRecentAddedMapPoints.end()) {
    MapPoint* pMP = *lit;
    if (pMP->isBad()) {
      lit = mlpRecentAddedMapPoints.erase(lit);
    } else if (pMP->GetFoundRatio() < 0.25f ) {
      pMP->SetBadFlag();
      lit = mlpRecentAddedMapPoints.erase(lit);
    } else if (((int)nCurrentKFid-(int)pMP->mnFirstKFid)>=2 && pMP->Observations()<=cnThObs) {
      pMP->SetBadFlag();
      lit = mlpRecentAddedMapPoints.erase(lit);
    } else if (((int)nCurrentKFid-(int)pMP->mnFirstKFid)>=3)
      lit = mlpRecentAddedMapPoints.erase(lit);
    else
      lit++;
  }
}

void LocalMapping::CreateNewMapPoints() {
  // Retrieve neighbor keyframes in covisibility graph
  int nn = 10;
  if (mbMonocular)
    nn=20;
  const vector<KeyFrame*> vpNeighKFs = mpCurrentKeyFrame->GetBestCovisibilityKeyFrames(nn);

  ORBmatcher matcher(0.6, false);

  Eigen::Matrix3d Rcw1 = mpCurrentKeyFrame->GetRotation();
  Eigen::Matrix3d Rwc1 = Rcw1.transpose();
  Eigen::Vector3d tcw1 = mpCurrentKeyFrame->GetTranslation();
  Eigen::Matrix<double, 3, 4> Tcw1;
  Tcw1.block<3, 3>(0, 0) = Rcw1;
  Tcw1.block<3, 1>(0, 3) = tcw1;
  Eigen::Vector3d Ow1 = mpCurrentKeyFrame->GetCameraCenter();

  const float &fx1 = mpCurrentKeyFrame->fx;
  const float &fy1 = mpCurrentKeyFrame->fy;
  const float &cx1 = mpCurrentKeyFrame->cx;
  const float &cy1 = mpCurrentKeyFrame->cy;
  const float &invfx1 = mpCurrentKeyFrame->invfx;
  const float &invfy1 = mpCurrentKeyFrame->invfy;

  const float ratioFactor = 1.5f*mpCurrentKeyFrame->mfScaleFactor;

  int nnew = 0;

  // Search matches with epipolar restriction and triangulate
  for (size_t i = 0; i < vpNeighKFs.size(); i++) {
    if (i > 0 && CheckNewKeyFrames())
      return;

    KeyFrame* pKF2 = vpNeighKFs[i];

    // Check first that baseline is not too short
    Eigen::Vector3d Ow2 = pKF2->GetCameraCenter();
    Eigen::Vector3d vBaseline = Ow2-Ow1;
    const float baseline = vBaseline.norm();

    if (!mbMonocular) {
      if (baseline<pKF2->mb)
      continue;
    } else {
      const float medianDepthKF2 = pKF2->ComputeSceneMedianDepth(2);
      const float ratioBaselineDepth = baseline/medianDepthKF2;

      if (ratioBaselineDepth < 0.01)
        continue;
    }

    // Compute Fundamental Matrix
    Eigen::Matrix3d F12 = ComputeF12(mpCurrentKeyFrame, pKF2);

    // Search matches that fullfil epipolar constraint
    vector<std::pair<size_t, size_t> > vMatchedIndices;
    matcher.SearchForTriangulation(mpCurrentKeyFrame, pKF2, F12, vMatchedIndices);

    Eigen::Matrix3d Rcw2 = pKF2->GetRotation();
    Eigen::Matrix3d Rwc2 = Rcw2.transpose();
    Eigen::Vector3d tcw2 = pKF2->GetTranslation();
    Eigen::Matrix<double, 3, 4> Tcw2;
    Tcw2.block<3, 3>(0, 0) = Rcw2;
    Tcw2.block<3, 1>(0, 3) = tcw2;

    const float &fx2 = pKF2->fx;
    const float &fy2 = pKF2->fy;
    const float &cx2 = pKF2->cx;
    const float &cy2 = pKF2->cy;
    const float &invfx2 = pKF2->invfx;
    const float &invfy2 = pKF2->invfy;

    // Triangulate each match
    const int nmatches = vMatchedIndices.size();
    for (int ikp = 0; ikp<nmatches; ikp++) {
      const int &idx1 = vMatchedIndices[ikp].first;
      const int &idx2 = vMatchedIndices[ikp].second;

      const cv::KeyPoint &kp1 = mpCurrentKeyFrame->mvKeysUn[idx1];
      const float kp1_ur = mpCurrentKeyFrame->mvuRight[idx1];
      bool bStereo1 = kp1_ur >= 0;

      const cv::KeyPoint &kp2 = pKF2->mvKeysUn[idx2];
      const float kp2_ur = pKF2->mvuRight[idx2];
      bool bStereo2 = kp2_ur >= 0;

      // Check parallax between rays
      Eigen::Vector3d xn1((kp1.pt.x-cx1)*invfx1, (kp1.pt.y-cy1)*invfy1, 1.0);
      Eigen::Vector3d xn2((kp2.pt.x-cx2)*invfx2, (kp2.pt.y-cy2)*invfy2, 1.0);

      Eigen::Vector3d ray1 = Rwc1*xn1;
      Eigen::Vector3d ray2 = Rwc2*xn2;
      const float cosParallaxRays = ray1.dot(ray2)/(ray1.norm()*ray2.norm());

      float cosParallaxStereo = cosParallaxRays+1;
      float cosParallaxStereo1 = cosParallaxStereo;
      float cosParallaxStereo2 = cosParallaxStereo;

      if (bStereo1)
        cosParallaxStereo1 = cos(2*atan2(mpCurrentKeyFrame->mb/2, mpCurrentKeyFrame->mvDepth[idx1]));
      else if (bStereo2)
        cosParallaxStereo2 = cos(2*atan2(pKF2->mb/2,pKF2->mvDepth[idx2]));

      cosParallaxStereo = std::min(cosParallaxStereo1, cosParallaxStereo2);

      Eigen::Vector3d x3D;
      if (cosParallaxRays<cosParallaxStereo && cosParallaxRays > 0 && (bStereo1 || bStereo2 || cosParallaxRays < 0.9998)) {
        // Linear Triangulation Method
        Eigen::Matrix4d A;
        A.row(0) = xn1(0)*Tcw1.row(2)-Tcw1.row(0);
        A.row(1) = xn1(1)*Tcw1.row(2)-Tcw1.row(1);
        A.row(2) = xn2(0)*Tcw2.row(2)-Tcw2.row(0);
        A.row(3) = xn2(1)*Tcw2.row(2)-Tcw2.row(1);

        cv::Mat A_cv(4, 4, CV_32F);
        cv::Mat w, u, vt;
        A_cv = Converter::toCvMat(A);
        cv::SVD::compute(A_cv, w, u, vt, cv::SVD::MODIFY_A| cv::SVD::FULL_UV);

        cv::Mat x3D_cv = vt.row(3).t();

        if (x3D_cv.at<float>(3) == 0)
          continue;

        // Euclidean coordinates
        x3D_cv = x3D_cv.rowRange(0, 3)/x3D_cv.at<float>(3);
        x3D = Converter::toVector3d(x3D_cv);

      } else if (bStereo1 && cosParallaxStereo1<cosParallaxStereo2) {
        x3D = mpCurrentKeyFrame->UnprojectStereo(idx1);
      } else if (bStereo2 && cosParallaxStereo2<cosParallaxStereo1) {
        x3D = pKF2->UnprojectStereo(idx2);
      } else
        continue; //No stereo and very low parallax

      Eigen::Vector3d x3Dt = x3D.transpose();

      //Check triangulation in front of cameras
      float z1 = Rcw1.row(2).dot(x3Dt)+tcw1(2);
      if (z1 <= 0)
        continue;

      float z2 = Rcw2.row(2).dot(x3Dt)+tcw2(2);
      if (z2 <= 0)
        continue;

      //Check reprojection error in first keyframe
      const float &sigmaSquare1 = mpCurrentKeyFrame->mvLevelSigma2[kp1.octave];
      const float x1 = Rcw1.row(0).dot(x3Dt)+tcw1(0);
      const float y1 = Rcw1.row(1).dot(x3Dt)+tcw1(1);
      const float invz1 = 1.0/z1;

      if (!bStereo1) {
        float u1 = fx1*x1*invz1+cx1;
        float v1 = fy1*y1*invz1+cy1;
        float errX1 = u1 - kp1.pt.x;
        float errY1 = v1 - kp1.pt.y;
        if ((errX1*errX1+errY1*errY1)>5.991*sigmaSquare1)
          continue;
      } else {
        float u1 = fx1*x1*invz1+cx1;
        float u1_r = u1 - mpCurrentKeyFrame->mbf*invz1;
        float v1 = fy1*y1*invz1+cy1;
        float errX1 = u1 - kp1.pt.x;
        float errY1 = v1 - kp1.pt.y;
        float errX1_r = u1_r - kp1_ur;
        if ((errX1*errX1+errY1*errY1+errX1_r*errX1_r)>7.8*sigmaSquare1)
          continue;
      }

      //Check reprojection error in second keyframe
      const float sigmaSquare2 = pKF2->mvLevelSigma2[kp2.octave];
      const float x2 = Rcw2.row(0).dot(x3Dt)+tcw2(0);
      const float y2 = Rcw2.row(1).dot(x3Dt)+tcw2(1);
      const float invz2 = 1.0/z2;
      if (!bStereo2) {
        float u2 = fx2*x2*invz2+cx2;
        float v2 = fy2*y2*invz2+cy2;
        float errX2 = u2 - kp2.pt.x;
        float errY2 = v2 - kp2.pt.y;
        if ((errX2*errX2+errY2*errY2)>5.991*sigmaSquare2)
          continue;
      } else {
        float u2 = fx2*x2*invz2+cx2;
        float u2_r = u2 - mpCurrentKeyFrame->mbf*invz2;
        float v2 = fy2*y2*invz2+cy2;
        float errX2 = u2 - kp2.pt.x;
        float errY2 = v2 - kp2.pt.y;
        float errX2_r = u2_r - kp2_ur;
        if ((errX2*errX2+errY2*errY2+errX2_r*errX2_r)>7.8*sigmaSquare2)
          continue;
      }

      //Check scale consistency
      Eigen::Vector3d normal1 = x3D-Ow1;
      float dist1 = normal1.norm();

      Eigen::Vector3d normal2 = x3D-Ow2;
      float dist2 = normal2.norm();

      if (dist1 == 0 || dist2 == 0)
        continue;

      const float ratioDist = dist2/dist1;
      const float ratioOctave = mpCurrentKeyFrame->mvScaleFactors[kp1.octave]/pKF2->mvScaleFactors[kp2.octave];

      /*if (fabs(ratioDist-ratioOctave)>ratioFactor)
        continue;*/
      if (ratioDist*ratioFactor<ratioOctave || ratioDist>ratioOctave*ratioFactor)
        continue;

      // Triangulation is succesfull
      MapPoint* pMP = new MapPoint(x3D, mpCurrentKeyFrame, mpMap);

      pMP->AddObservation(mpCurrentKeyFrame, idx1);
      pMP->AddObservation(pKF2, idx2);

      mpCurrentKeyFrame->AddMapPoint(pMP, idx1);
      pKF2->AddMapPoint(pMP, idx2);

      pMP->ComputeDistinctiveDescriptors();

      pMP->UpdateNormalAndDepth();

      mpMap->AddMapPoint(pMP);
      mlpRecentAddedMapPoints.push_back(pMP);

      nnew++;
    }
  }
}

void LocalMapping::SearchInNeighbors() {
  // Retrieve neighbor keyframes
  int nn = 10;
  if (mbMonocular)
    nn=20;
  const vector<KeyFrame*> vpNeighKFs = mpCurrentKeyFrame->GetBestCovisibilityKeyFrames(nn);
  vector<KeyFrame*> vpTargetKFs;
  for (vector<KeyFrame*>::const_iterator vit=vpNeighKFs.begin(), vend=vpNeighKFs.end(); vit!=vend; vit++) {
    KeyFrame* pKFi = *vit;
    if (pKFi->isBad() || pKFi->mnFuseTargetForKF == mpCurrentKeyFrame->mnId)
      continue;
    vpTargetKFs.push_back(pKFi);
    pKFi->mnFuseTargetForKF = mpCurrentKeyFrame->mnId;

    // Extend to some second neighbors
    const vector<KeyFrame*> vpSecondNeighKFs = pKFi->GetBestCovisibilityKeyFrames(5);
    for (vector<KeyFrame*>::const_iterator vit2=vpSecondNeighKFs.begin(), vend2=vpSecondNeighKFs.end(); vit2!=vend2; vit2++) {
      KeyFrame* pKFi2 = *vit2;
      if (pKFi2->isBad() || pKFi2->mnFuseTargetForKF == mpCurrentKeyFrame->mnId || pKFi2->mnId == mpCurrentKeyFrame->mnId)
        continue;
      vpTargetKFs.push_back(pKFi2);
    }
  }


  // Search matches by projection from current KF in target KFs
  ORBmatcher matcher;
  vector<MapPoint*> vpMapPointMatches = mpCurrentKeyFrame->GetMapPointMatches();
  for (vector<KeyFrame*>::iterator vit=vpTargetKFs.begin(), vend=vpTargetKFs.end(); vit!=vend; vit++) {
    KeyFrame* pKFi = *vit;

    matcher.Fuse(pKFi, vpMapPointMatches);
  }

  // Search matches by projection from target KFs in current KF
  vector<MapPoint*> vpFuseCandidates;
  vpFuseCandidates.reserve(vpTargetKFs.size()*vpMapPointMatches.size());

  for (vector<KeyFrame*>::iterator vitKF=vpTargetKFs.begin(), vendKF=vpTargetKFs.end(); vitKF!=vendKF; vitKF++) {
    KeyFrame* pKFi = *vitKF;

    vector<MapPoint*> vpMapPointsKFi = pKFi->GetMapPointMatches();

    for (vector<MapPoint*>::iterator vitMP=vpMapPointsKFi.begin(), vendMP=vpMapPointsKFi.end(); vitMP!=vendMP; vitMP++) {
      MapPoint* pMP = *vitMP;
      if (!pMP)
        continue;
      if (pMP->isBad() || pMP->mnFuseCandidateForKF == mpCurrentKeyFrame->mnId)
        continue;
      pMP->mnFuseCandidateForKF = mpCurrentKeyFrame->mnId;
      vpFuseCandidates.push_back(pMP);
    }
  }

  matcher.Fuse(mpCurrentKeyFrame, vpFuseCandidates);

  // Update points
  vpMapPointMatches = mpCurrentKeyFrame->GetMapPointMatches();
  for (size_t i = 0, iend=vpMapPointMatches.size(); i < iend; i++) {
    MapPoint* pMP=vpMapPointMatches[i];
    if (pMP) {
      if (!pMP->isBad()) {
        pMP->ComputeDistinctiveDescriptors();
        pMP->UpdateNormalAndDepth();
      }
    }
  }

  // Update connections in covisibility graph
  mpCurrentKeyFrame->UpdateConnections();
}

Eigen::Matrix3d LocalMapping::ComputeF12(KeyFrame *&pKF1, KeyFrame *&pKF2) {
  Eigen::Matrix3d R1w = pKF1->GetRotation();
  Eigen::Vector3d t1w = pKF1->GetTranslation();
  Eigen::Matrix3d R2w = pKF2->GetRotation();
  Eigen::Vector3d t2w = pKF2->GetTranslation();

  Eigen::Matrix3d R12 = R1w*R2w.transpose();
  Eigen::Vector3d t12 = -R1w*R2w.transpose()*t2w+t1w;

  Eigen::Matrix3d t12x = SkewSymmetricMatrix(t12);

  Eigen::Matrix3d K1 = pKF1->mK;
  Eigen::Matrix3d K2 = pKF2->mK;

  Eigen::Matrix3d res = K1.transpose().inverse()*t12x*R12*K2.inverse();

  return res;
}

void LocalMapping::RequestStop() {
  unique_lock<mutex> lock(mMutexStop);
  mbStopRequested = true;
  unique_lock<mutex> lock2(mMutexNewKFs);
  mbAbortBA = true;
}

bool LocalMapping::Stop() {
  unique_lock<mutex> lock(mMutexStop);
  if (mbStopRequested && !mbNotStop) {
    mbStopped = true;
    LOGD("Local Mapping STOP");
    return true;
  }

  return false;
}

bool LocalMapping::isStopped() {
  unique_lock<mutex> lock(mMutexStop);
  return mbStopped;
}

bool LocalMapping::stopRequested() {
  unique_lock<mutex> lock(mMutexStop);
  return mbStopRequested;
}

void LocalMapping::Release() {
  unique_lock<mutex> lock(mMutexStop);
  unique_lock<mutex> lock2(mMutexFinish);
  if (mbFinished)
    return;
  mbStopped = false;
  mbStopRequested = false;
  for (list<KeyFrame*>::iterator lit = mlNewKeyFrames.begin(), lend = mlNewKeyFrames.end(); lit!=lend; lit++)
    delete *lit;
  mlNewKeyFrames.clear();

  LOGD("Local Mapping RELEASE");
}

bool LocalMapping::AcceptKeyFrames() {
  unique_lock<mutex> lock(mMutexAccept);
  return mbAcceptKeyFrames;
}

void LocalMapping::SetAcceptKeyFrames(bool flag) {
  unique_lock<mutex> lock(mMutexAccept);
  mbAcceptKeyFrames=flag;
}

bool LocalMapping::SetNotStop(bool flag) {
  unique_lock<mutex> lock(mMutexStop);

  if (flag && mbStopped)
    return false;

  mbNotStop = flag;

  return true;
}

void LocalMapping::InterruptBA() {
  mbAbortBA = true;
}

void LocalMapping::KeyFrameCulling() {
  // Check redundant keyframes (only local keyframes)
  // A keyframe is considered redundant if the 90% of the MapPoints it sees, are seen
  // in at least other 3 keyframes (in the same or finer scale)
  // We only consider close stereo points
  vector<KeyFrame*> vpLocalKeyFrames = mpCurrentKeyFrame->GetVectorCovisibleKeyFrames();

  for (vector<KeyFrame*>::iterator vit=vpLocalKeyFrames.begin(), vend=vpLocalKeyFrames.end(); vit!=vend; vit++) {
    KeyFrame* pKF = *vit;
    if (pKF->mnId == 0)
      continue;
    const vector<MapPoint*> vpMapPoints = pKF->GetMapPointMatches();

    int nObs = 3;
    const int thObs=nObs;
    int nRedundantObservations = 0;
    int nMPs = 0;
    for (size_t i = 0, iend=vpMapPoints.size(); i < iend; i++) {
      MapPoint* pMP = vpMapPoints[i];
      if (pMP) {
        if (!pMP->isBad()) {
          if (!mbMonocular) {
            if (pKF->mvDepth[i]>pKF->mThDepth || pKF->mvDepth[i] < 0)
              continue;
          }

          nMPs++;
          if (pMP->Observations()>thObs) {
            const int &scaleLevel = pKF->mvKeysUn[i].octave;
            const map<KeyFrame*, size_t> observations = pMP->GetObservations();
            int nObs = 0;
            for (map<KeyFrame*, size_t>::const_iterator mit=observations.begin(), mend=observations.end(); mit != mend; mit++) {
              KeyFrame* pKFi = mit->first;
              if (pKFi == pKF)
                continue;
              const int &scaleLeveli = pKFi->mvKeysUn[mit->second].octave;

              if (scaleLeveli <= scaleLevel+1) {
                nObs++;
                if (nObs>=thObs)
                  break;
              }
            }
            if (nObs>=thObs) {
              nRedundantObservations++;
            }
          }
        }
      }
    }

    if (nRedundantObservations > 0.9*nMPs)
      pKF->SetBadFlag();
  }
}

Eigen::Matrix3d LocalMapping::SkewSymmetricMatrix(const Eigen::Vector3d &v) {
  Eigen::Matrix3d m;
  m << 0, -v(2), v(1), v(2), 0, -v(0), -v(1), v(0), 0;
  return m;
}

void LocalMapping::RequestReset() {
  {
    unique_lock<mutex> lock(mMutexReset);
    mbResetRequested = true;
  }

  while (1) {
    {
      unique_lock<mutex> lock2(mMutexReset);
      if (!mbResetRequested)
        break;
    }
    usleep(3000);
  }
}

void LocalMapping::ResetIfRequested() {
  unique_lock<mutex> lock(mMutexReset);
  if (mbResetRequested) {
    mlNewKeyFrames.clear();
    mlpRecentAddedMapPoints.clear();
    mbResetRequested=false;
  }
}

void LocalMapping::RequestFinish() {
  unique_lock<mutex> lock(mMutexFinish);
  mbFinishRequested = true;
}

bool LocalMapping::CheckFinish() {
  unique_lock<mutex> lock(mMutexFinish);
  return mbFinishRequested;
}

void LocalMapping::SetFinish() {
  unique_lock<mutex> lock(mMutexFinish);
  mbFinished = true;
  unique_lock<mutex> lock2(mMutexStop);
  mbStopped = true;
}

bool LocalMapping::isFinished() {
  unique_lock<mutex> lock(mMutexFinish);
  return mbFinished;
}

}  // namespace SD_SLAM
