#include <chrono>
#include <stdio.h>
#include <ros/package.h>
#include <pcl_conversions/pcl_conversions.h>
#include "imm_ukf_pda.h"


ImmUkfPda::ImmUkfPda()
{
  ros::NodeHandle private_nh_("~");
  private_nh_.param<std::string>("pointcloud_frame", pointcloud_frame_, "velodyne");
  private_nh_.param<std::string>("tracking_frame", tracking_frame_, "world");
  private_nh_.param<int>("life_time_thres", life_time_thres_, 8);
  private_nh_.param<double>("gating_thres", gating_thres_, 9.22);
  private_nh_.param<double>("gate_probability", gate_probability_, 0.99);
  private_nh_.param<double>("detection_probability", detection_probability_, 0.9);
  private_nh_.param<double>("distance_thres", distance_thres_, 99);
  private_nh_.param<double>("static_velocity_thres", static_velocity_thres_, 0.5);
  private_nh_.param<bool>("is_benchmark", is_benchmark_, false);
  private_nh_.param<bool>("use_sukf", use_sukf_, false);
  private_nh_.param<bool>("use_robust_adaptive_filter", use_robust_adaptive_filter_, false);
  private_nh_.param<int>("debug_nth_take", debug_nth_take_, 0);

  init_                       = false;

  // could change below param fot better performance
  use_vectormap_              = false;
  // assign unique ukf_id_ to each tracking targets
  target_id_   = 0;
  frame_count_ = 0;

  if(is_benchmark_)
  {
    std::string path = ros::package::getPath("lidar_imm_ukf_pda_track");
    result_file_path_ = path + "/benchmark/results/result.txt";
    remove(result_file_path_.c_str());

    std::string filepath1 = "/home/kosuke/pre_tf_cluster" +std::to_string(debug_nth_take_) + ".txt";
    std::string filepath2 = "/home/kosuke/post_tf_cluster" +std::to_string(debug_nth_take_) + ".txt";
    std::string filepath3 = "/home/kosuke/tracker_out" +std::to_string(debug_nth_take_) + ".txt";
    remove(filepath1.c_str());
    remove(filepath2.c_str());
    remove(filepath3.c_str());
  }
}

void ImmUkfPda::run()
{
  pub_jskbbox_array_ = node_handle_.advertise<jsk_recognition_msgs::BoundingBoxArray>("/bounding_boxes_tracked", 1);
  pub_object_array_  = node_handle_.advertise<autoware_msgs::DetectedObjectArray>("/detected_objects", 1);
  pub_points_        = node_handle_.advertise<visualization_msgs::Marker>("/points/debug", 1);
  pub_texts_array_   = node_handle_.advertise<visualization_msgs::MarkerArray>("/texts/debug", 1);

  if(use_vectormap_)
  {
    vmap_.subscribe(node_handle_, vector_map::Category::POINT | vector_map::Category::NODE |
                                  vector_map::Category::LANE  | vector_map::Category::DTLANE, 10);
    setPredictionObject();

    pub_adas_direction_array_  = node_handle_.advertise<visualization_msgs::MarkerArray>("/adas_direction", 1);
    pub_adas_prediction_array_ = node_handle_.advertise<visualization_msgs::MarkerArray>("/adas_prediction", 1);
  }

  sub_detected_array_ = node_handle_.subscribe("/detected_objects_range", 1, &ImmUkfPda::callback, this);
}

void ImmUkfPda::callback(const autoware_msgs::DetectedObjectArray& input)
{
  if(is_benchmark_)
  {
    if(frame_count_ == 0)
    {
      //skip fisrt frame
      frame_count_ ++;
      return;
    }
  }
  // only transform pose(clusteArray.clusters.bouding_box.pose)
  // std::string filepath1 = "/home/kosuke/pre_tf_cluster" +std::to_string(debug_nth_take_) + ".txt";
  // std::ofstream file1(filepath1, std::ofstream::out | std::ofstream::app);
  // file1<< frame_count_ << " " << input.objects.size() << "\n";
  // for(size_t i = 0; i < input.objects.size(); i++)
  // {
  //   file1<<input.objects[i].pose.position.x << " "
  //        <<input.objects[i].pose.position.y << " "
  //        <<input.objects[i].pose.position.z << " "
  //        <<input.objects[i].pose.orientation.x << " "
  //        <<input.objects[i].pose.orientation.y << " "
  //        <<input.objects[i].pose.orientation.z << " "
  //        <<input.objects[i].pose.orientation.w << "\n";
  // }
  autoware_msgs::DetectedObjectArray transformed_input;
  transformPoseToGlobal(input, transformed_input);
  // std::string filepath2 = "/home/kosuke/post_tf_cluster" +std::to_string(debug_nth_take_) + ".txt";
  // std::ofstream file2(filepath2, std::ofstream::out | std::ofstream::app);
  // file2<< frame_count_ << " " << input.objects.size() << "\n";
  // for(size_t i = 0; i < transformed_input.objects.size(); i++)
  // {
  //   file2<<transformed_input.objects[i].pose.position.x << " "
  //       <<transformed_input.objects[i].pose.position.y << " "
  //       <<transformed_input.objects[i].pose.position.z << " "
  //       <<transformed_input.objects[i].pose.orientation.x << " "
  //       <<transformed_input.objects[i].pose.orientation.y << " "
  //       <<transformed_input.objects[i].pose.orientation.z << " "
  //       <<transformed_input.objects[i].pose.orientation.w << "\n";
  // }
  jsk_recognition_msgs::BoundingBoxArray jskbboxes_output;
  autoware_msgs::DetectedObjectArray detected_objects_output;
  tracker(transformed_input, jskbboxes_output, detected_objects_output);
  // relayJskbbox(input, jskbboxes_output);
  transformPoseToLocal(jskbboxes_output, detected_objects_output);
  pub_jskbbox_array_.publish(jskbboxes_output);
  pub_object_array_.publish(detected_objects_output);

  // std::ofstream file2("/home/kosuke/tracker_out2.txt", std::ofstream::out | std::ofstream::app);
  // file2<< frame_count_ << " " << detected_objects_output.objects.size() << "\n";
  // for(size_t i = 0; i < input.objects.size(); i++)
  // {
  //   file2<<detected_objects_output.objects[i].dimensions.x << " "
  //       <<detected_objects_output.objects[i].dimensions.y << " "
  //       <<detected_objects_output.objects[i].dimensions.z << " "
  //       <<detected_objects_output.objects[i].pose.position.x << " "
  //       <<detected_objects_output.objects[i].pose.position.y << " "
  //       <<detected_objects_output.objects[i].pose.position.z << " "
  //       <<detected_objects_output.objects[i].pose.orientation.x << " "
  //       <<detected_objects_output.objects[i].pose.orientation.y << " "
  //       <<detected_objects_output.objects[i].pose.orientation.z << " "
  //       <<detected_objects_output.objects[i].pose.orientation.w << "\n";
  // }
  if(use_vectormap_)
  {
    visualization_msgs::MarkerArray directionMarkers;
    visualization_msgs::MarkerArray predictionMarkers;
    prediction_.adasMapAssitDirectionAndPrediction(input, tf_listener_, targets_, directionMarkers, predictionMarkers);
    pub_adas_direction_array_.publish(directionMarkers);
    pub_adas_prediction_array_.publish(predictionMarkers);
  }
}

void ImmUkfPda::setPredictionObject()
{
  lanes_ = vmap_.findByFilter([](const vector_map_msgs::Lane &lane){return true;});
  ModelBasePrediction mbp;
  // todo: change to init()
  mbp.setVMap(vmap_);
  mbp.setLanes(lanes_);
  prediction_ = mbp;
}

void ImmUkfPda::relayJskbbox(const autoware_msgs::DetectedObjectArray& input,
                                   jsk_recognition_msgs::BoundingBoxArray& jskbboxes_output)
{
  jskbboxes_output.header = input.header;
  for (size_t i = 0; i < input.objects.size(); i++)
  {
    jsk_recognition_msgs::BoundingBox bb;
    bb.header = input.header;
    bb.pose = input.objects[i].pose;
    bb.dimensions = input.objects[i].dimensions;
    jskbboxes_output.boxes.push_back(bb);
  }
}

void ImmUkfPda::transformPoseToGlobal(const autoware_msgs::DetectedObjectArray& input,
                                            autoware_msgs::DetectedObjectArray& transformed_input)
{
  transformed_input.header = input.header;
  try{
    if(is_benchmark_)
    {
      tf_listener_.waitForTransform(pointcloud_frame_, tracking_frame_, input.header.stamp, ros::Duration(3.0));
    }
    else
    {
      tf_listener_.waitForTransform(pointcloud_frame_, tracking_frame_, ros::Time(0), ros::Duration(1.0));
    }
    // todo: make transform obejct for later use
  }
  catch (tf::TransformException ex){
    std::cout << "cannot transform" << std::endl;
    ROS_ERROR("%s",ex.what());
    ros::Duration(1.0).sleep();
  }
  for (size_t i = 0; i < input.objects.size(); i++)
  {
    geometry_msgs::PoseStamped pose_in, pose_out;

    pose_in.header = input.header;
    pose_in.pose = input.objects[i].pose;

    if(is_benchmark_)
    {
      tf_listener_.transformPose(tracking_frame_, input.header.stamp, pose_in, input.header.frame_id, pose_out);
    }
    else
    {
      tf_listener_.transformPose(tracking_frame_, ros::Time(0), pose_in, input.header.frame_id, pose_out);
    }

    autoware_msgs::DetectedObject dd;
    dd.header = input.header;
    dd = input.objects[i];
    dd.pose = pose_out.pose;

    transformed_input.objects.push_back(dd);
  }
}

void ImmUkfPda::transformPoseToLocal(jsk_recognition_msgs::BoundingBoxArray& jskbboxes_output,
                                     autoware_msgs::DetectedObjectArray& detected_objects_output)
{
  for (size_t i = 0; i < detected_objects_output.objects.size(); i++)
  {
    geometry_msgs::PoseStamped detected_pose_in, detected_pose_out;

    detected_pose_in.header          = jskbboxes_output.header;
    detected_pose_in.header.frame_id = tracking_frame_;
    detected_pose_in.pose            = detected_objects_output.objects[i].pose;

    if(is_benchmark_)
    {
      tf_listener_.transformPose(pointcloud_frame_, jskbboxes_output.header.stamp, detected_pose_in, tracking_frame_, detected_pose_out);
    }
    else
    {
      tf_listener_.transformPose(pointcloud_frame_, ros::Time(0), detected_pose_in, tracking_frame_, detected_pose_out);
    }

    detected_objects_output.objects[i].header.frame_id = pointcloud_frame_;
    detected_objects_output.objects[i].pose            = detected_pose_out.pose;
  }
  detected_objects_output.header.frame_id = pointcloud_frame_;

  for (size_t i = 0; i < jskbboxes_output.boxes.size(); i++)
  {
    geometry_msgs::PoseStamped jsk_pose_in, jsk_pose_out;
    jsk_pose_in.header               = jskbboxes_output.header;
    jsk_pose_in.header.frame_id      = tracking_frame_;
    jsk_pose_in.pose                 = jskbboxes_output.boxes[i].pose;
    if(is_benchmark_)
    {
      tf_listener_.transformPose(pointcloud_frame_, jskbboxes_output.header.stamp, jsk_pose_in, tracking_frame_, jsk_pose_out);
    }
    else
    {
      tf_listener_.transformPose(pointcloud_frame_, ros::Time(0), jsk_pose_in, tracking_frame_, jsk_pose_out);
    }
    jskbboxes_output.boxes[i].header.frame_id = pointcloud_frame_;
    jskbboxes_output.boxes[i].pose            = jsk_pose_out.pose;
  }
  jskbboxes_output.header.frame_id        = pointcloud_frame_;

  if(is_benchmark_)
  {
    dumpResultText(detected_objects_output);
  }
  frame_count_ ++;
}

void ImmUkfPda::measurementValidation(const autoware_msgs::DetectedObjectArray &input, IMM_RAUKF& target, const bool second_init,
                                      const Eigen::VectorXd &max_det_z, const Eigen::MatrixXd &max_det_s,
                                      std::vector<autoware_msgs::DetectedObject>& object_vec,
                                      std::vector<bool>& matching_vec)
{
  // alert: different from original imm-pda filter, here picking most likely measurement
  // if making it allows to have more than one measurement, you will see non semipositive definite covariance

  int count = 0;
  bool second_init_done = false;
  double smallest_nis = std::numeric_limits<double>::max();
  autoware_msgs::DetectedObject smallest_meas_object;
  for (size_t i = 0; i < input.objects.size(); i++)
  {
    double x = input.objects[i].pose.position.x;
    double y = input.objects[i].pose.position.y;

    Eigen::VectorXd meas = Eigen::VectorXd(2);
    meas << x, y;

    Eigen::VectorXd diff = meas - max_det_z;
    double nis = diff.transpose() * max_det_s.inverse() * diff;

    if (nis < gating_thres_)
    {  // x^2 99% range
      // std::cout << "meas and nis " << x << " " << y << " " << nis << std::endl;
      count++;
      if (matching_vec[i] == false)
      {
        target.lifetime_++;
      }

      if (nis < smallest_nis)
      {
        smallest_nis = nis;
        smallest_meas_object = input.objects[i];
        matching_vec[i] = true;
        second_init_done = true;
      }
    }
  }
  if (second_init_done)
  {
    object_vec.push_back(smallest_meas_object);
  }
}

void ImmUkfPda::getNearestEuclidCluster(const IMM_RAUKF& target, const std::vector<autoware_msgs::DetectedObject>& object_vec,
                                        autoware_msgs::DetectedObject& object, double& min_dist)
{
  int min_ind = 0;
  double px = target.x_merge_(0);
  double py = target.x_merge_(1);

  for (size_t i = 0; i < object_vec.size(); i++)
  {
    double meas_x = object_vec[i].pose.position.x;
    double meas_y = object_vec[i].pose.position.y;

    double dist = sqrt((px - meas_x) * (px - meas_x) + (py - meas_y) * (py - meas_y));
    if (dist < min_dist)
    {
      min_dist = dist;
      min_ind = i;
    }
  }

  object = object_vec[min_ind];
}

void ImmUkfPda::associateBB(const std::vector<autoware_msgs::DetectedObject>& object_vec, IMM_RAUKF& target)
{
  // skip if no validated measurement
  if (object_vec.size() == 0)
  {
    return;
  }
  if (target.tracking_num_ == TrackingState::Stable && target.lifetime_ >= life_time_thres_)
  {
    autoware_msgs::DetectedObject nearest_object;
    double min_dist = std::numeric_limits<double>::max();
    getNearestEuclidCluster(target, object_vec, nearest_object, min_dist);
    if (min_dist < distance_thres_)
    {
      target.is_vis_bb_ = true;
      target.jsk_bb_.pose       = nearest_object.pose;
      target.jsk_bb_.dimensions = nearest_object.dimensions;
    }
  }
  else
  {
    autoware_msgs::DetectedObject nearest_object;
    double min_dist = std::numeric_limits<double>::max();
    getNearestEuclidCluster(target, object_vec, nearest_object, min_dist);
    target.jsk_bb_.pose       = nearest_object.pose;
    target.jsk_bb_.dimensions = nearest_object.dimensions;
  }
}

double ImmUkfPda::getJskBBoxYaw(const jsk_recognition_msgs::BoundingBox& jsk_bb)
{
  tf::Quaternion q(jsk_bb.pose.orientation.x, jsk_bb.pose.orientation.y, jsk_bb.pose.orientation.z,
                   jsk_bb.pose.orientation.w);
  double roll, pitch, yaw;
  tf::Matrix3x3(q).getRPY(roll, pitch, yaw);
  return yaw;
}

double ImmUkfPda::getJskBBoxArea(const jsk_recognition_msgs::BoundingBox& jsk_bb)
{
  double area = jsk_bb.dimensions.x * jsk_bb.dimensions.y;
  return area;
}

void ImmUkfPda::updateBB(IMM_RAUKF& target)
{
  // skip to prevent memory leak by accessing empty target.bbox_
  if (!target.is_vis_bb_)
  {
    return;
  }
  double yaw = getJskBBoxYaw(target.jsk_bb_);

  // skip the rest of process if it is first bbox associaiton
  if (target.is_best_jsk_bb_empty_ == false)
  {
    target.best_jsk_bb_ = target.jsk_bb_;
    target.best_yaw_ = yaw;
    target.is_best_jsk_bb_empty_ = true;
    return;
  }

  // restricting yaw movement
  double diff_yaw = yaw - target.best_yaw_;

  // diffYaw is within the threshold, apply the diffYaw chamge
  if(abs(diff_yaw) < bb_yaw_change_thres_)
  {
      target.best_jsk_bb_.pose.orientation = target.jsk_bb_.pose.orientation;
      target.best_yaw_ = yaw;
  }
  else
  {
      target.jsk_bb_.pose.orientation = target.best_jsk_bb_.pose.orientation;
  }

  // // bbox area
  double area = getJskBBoxArea(target.jsk_bb_);
  double best_area = getJskBBoxArea(target.best_jsk_bb_);


  // start updating bbox params
  double delta_area = area - best_area;

  // when the delta area is under 0, keep best area and relocate(slide) it for current cp
  if (delta_area < 0)
  {
    // updateVisBoxArea(target, dtCP);
    target.jsk_bb_.dimensions = target.best_jsk_bb_.dimensions;
    // for  mergeSegmentation, area comparison
    target.bb_area_ = best_area;
  }
  else if (delta_area > 0)
  {
    // target.bestBBox_ = target.BBox_;
    target.best_jsk_bb_.dimensions = target.jsk_bb_.dimensions;
    // for mergeSegmentation, area comparison
    target.bb_area_ = area;
  }
}

void ImmUkfPda::updateLabel(const IMM_RAUKF& target, autoware_msgs::DetectedObject& dd)
{
  int tracking_num = target.tracking_num_;
  // cout << "trackingnum "<< trackingNum << endl;
  if (target.is_static_)
  {
    dd.label = "Static";
  }
  else if (tracking_num > TrackingState::Die && tracking_num < TrackingState::Stable)
  {
    dd.label = "Initialized";
  }
  else if (tracking_num == TrackingState::Stable)
  {
    dd.label = "Stable";
  }
  else if (tracking_num > TrackingState::Stable && tracking_num <= TrackingState::Lost)
  {
    dd.label = "Lost";
  }
  else
  {
    dd.label = "None";
  }
}

void ImmUkfPda::updateJskLabel(const IMM_RAUKF& target, jsk_recognition_msgs::BoundingBox& bb)
{
  int tracking_num = target.tracking_num_;
  if (target.is_static_)
  {
    bb.label = 15;  // white color
  }
  else if (tracking_num == TrackingState::Stable)
  {
    bb.label = 2;  // orange color
  }
}

void ImmUkfPda::initTracker(const autoware_msgs::DetectedObjectArray& input, double timestamp)
{
  for (size_t i = 0; i < input.objects.size(); i++)
  {
    double px = input.objects[i].pose.position.x;
    double py = input.objects[i].pose.position.y;
    Eigen::VectorXd init_meas = Eigen::VectorXd(2);
    init_meas << px, py;

    IMM_RAUKF ukf;
    ukf.initialize(init_meas, timestamp, target_id_);
    targets_.push_back(ukf);
    target_id_ ++;
  }
  timestamp_ = timestamp;
  init_ = true;
  return;
}

void ImmUkfPda::secondInit(IMM_RAUKF& target, const std::vector<autoware_msgs::DetectedObject>& object_vec, double dt)
{
  if (object_vec.size() == 0)
  {
    target.tracking_num_ = TrackingState::Die;
    return;
  }
  // record init measurement for env classification
  target.init_meas_ << target.x_merge_(0), target.x_merge_(1);

  // state update
  double target_x = object_vec[0].pose.position.x;
  double target_y = object_vec[0].pose.position.y;
  double target_diff_x = target_x - target.x_merge_(0);
  double target_diff_y = target_y - target.x_merge_(1);
  double target_yaw = atan2(target_diff_y, target_diff_x);
  double dist = sqrt(target_diff_x * target_diff_x + target_diff_y * target_diff_y);
  double target_v = dist / dt;

  while (target_yaw > M_PI)
    target_yaw -= 2. * M_PI;
  while (target_yaw < -M_PI)
    target_yaw += 2. * M_PI;

  target.x_merge_(0) = target.x_cv_(0) = target.x_ctrv_(0) = target.x_rm_(0) = target_x;
  target.x_merge_(1) = target.x_cv_(1) = target.x_ctrv_(1) = target.x_rm_(1) = target_y;
  target.x_merge_(2) = target.x_cv_(2) = target.x_ctrv_(2) = target.x_rm_(2) = target_v;
  target.x_merge_(3) = target.x_cv_(3) = target.x_ctrv_(3) = target.x_rm_(3) = target_yaw;

  // target.initCovarQs(dt, target_yaw);

  target.tracking_num_++;
  return;
}

void ImmUkfPda::updateTrackingNum(const std::vector<autoware_msgs::DetectedObject>& object_vec, IMM_RAUKF& target)
{
  if (object_vec.size() > 0)
  {
    if (target.tracking_num_ < TrackingState::Stable)
    {
      target.tracking_num_++;
    }
    else if (target.tracking_num_ == TrackingState::Stable)
    {
      target.tracking_num_ = TrackingState::Stable;
    }
    else if (target.tracking_num_ >= TrackingState::Stable && target.tracking_num_ < TrackingState::Lost)
    {
      target.tracking_num_ = TrackingState::Stable;
    }
    else if (target.tracking_num_ == TrackingState::Lost)
    {
      // std::cout << target.ukf_id_ <<" target die though found meas in the lost state" << std::endl;
      target.tracking_num_ = TrackingState::Die;
    }
  }
  else
  {
    if (target.tracking_num_ < TrackingState::Stable)
    {
      // std::cout << target.ukf_id_ <<" target lost because of lost before stable" << std::endl;
      target.tracking_num_ = TrackingState::Die;
    }
    else if (target.tracking_num_ >= TrackingState::Stable && target.tracking_num_ < TrackingState::Lost)
    {
      target.tracking_num_++;
    }
    else if (target.tracking_num_ == TrackingState::Lost)
    {
      // std::cout << target.ukf_id_ <<" target lost after lost state" << std::endl;
      target.tracking_num_ = TrackingState::Die;
    }
  }

  return;
}

void ImmUkfPda::probabilisticDataAssociation(const autoware_msgs::DetectedObjectArray& input, const double dt,
                                             const double det_explode_param, std::vector<bool>& matching_vec,
                                             std::vector<autoware_msgs::DetectedObject>& object_vec, IMM_RAUKF& target, bool& is_skip_target)
{
  double det_s = 0;
  Eigen::VectorXd max_det_z;
  Eigen::MatrixXd max_det_s;
  is_skip_target = false;


  if(use_sukf_)
  {
    max_det_z = target.z_pred_ctrv_;
    max_det_s = target.s_ctrv_;
    det_s     = max_det_s.determinant();
  }
  else
  {
    // find maxDetS associated with predZ
    target.findMaxZandS(max_det_z, max_det_s);
    det_s = max_det_s.determinant();
  }

  // prevent ukf not to explode
  if (std::isnan(det_s) || det_s > det_explode_param)
  {
    // std::cout << "max det z" << std::endl << max_det_z << std::endl;
    // std::cout << "max det s" << std::endl << max_det_s << std::endl;
    // std::cout << "x cv " << std::endl << target.x_cv_ << std::endl;
    // std::cout << "x ctrv " << std::endl << target.x_ctrv_ << std::endl;
    // std::cout << "x rm " << std::endl << target.x_rm_ << std::endl;
    // std::cout << target.ukf_id_ <<" target lost because of det explode" << std::endl;
    target.tracking_num_ = TrackingState::Die;
    is_skip_target = true;
    return;
  }

  bool is_second_init;
  if (target.tracking_num_ == TrackingState::Init)
  {
    is_second_init = true;
  }
  else
  {
    is_second_init = false;
  }

  // measurement gating, get measVec, bboxVec, matchingVec through reference
  measurementValidation(input, target, is_second_init, max_det_z, max_det_s, object_vec, matching_vec);

  // bounding box association if target is stable :plus, right angle correction if its needed
  // input: track number, bbox measurements, &target
  associateBB(object_vec, target);

  // second detection for a target: update v and yaw
  if (is_second_init)
  {
    secondInit(target, object_vec, dt);
    is_skip_target = true;
    return;
  }

  // update tracking number
  updateTrackingNum(object_vec, target);

  if (target.tracking_num_ == TrackingState::Die)
  {
    is_skip_target = true;
    return;
  }
}

void ImmUkfPda::makeNewTargets(const double timestamp, const autoware_msgs::DetectedObjectArray& input, const std::vector<bool>& matching_vec)
{
  for (size_t i = 0; i < input.objects.size(); i++)
  {
    if (matching_vec[i] == false)
    {
      double px = input.objects[i].pose.position.x;
      double py = input.objects[i].pose.position.y;
      Eigen::VectorXd init_meas = Eigen::VectorXd(2);
      init_meas << px, py;

      IMM_RAUKF ukf;
      ukf.initialize(init_meas, timestamp, target_id_);
      targets_.push_back(ukf);
      target_id_ ++;
    }
  }
}

void ImmUkfPda::staticClassification()
{
  for (size_t i = 0; i < targets_.size(); i++)
  {
    targets_[i].vel_history_.push_back(targets_[i].x_merge_(2));
    if (targets_[i].tracking_num_ == TrackingState::Stable &&
        targets_[i].lifetime_ > life_time_thres_)
    {
      double sum_vel = 0;
      double avg_vel = 0;
      for (int ind = 1; ind < life_time_thres_; ind++)
      {
        sum_vel += targets_[i].vel_history_.end()[-ind];
      }
      avg_vel = double(sum_vel/life_time_thres_);

      if ((avg_vel< static_velocity_thres_) &&
         (targets_[i].mode_prob_rm_ > targets_[i].mode_prob_cv_ ||
          targets_[i].mode_prob_rm_ > targets_[i].mode_prob_ctrv_))
      {
        targets_[i].is_static_ = true;
      }
    }
  }
}

void ImmUkfPda::makeOutput(const autoware_msgs::DetectedObjectArray& input,
                           jsk_recognition_msgs::BoundingBoxArray& jskbboxes_output,
                           autoware_msgs::DetectedObjectArray& detected_objects_output)
{
  // output.header = input.header;
  jskbboxes_output.header = input.header;
  detected_objects_output.header = input.header;
  for (size_t i = 0; i < targets_.size(); i++)
  {
    double tx = targets_[i].x_merge_(0);
    double ty = targets_[i].x_merge_(1);

    double tv = targets_[i].x_merge_(2);
    double tyaw = targets_[i].x_merge_(3);
    while (tyaw > M_PI)
      tyaw -= 2. * M_PI;
    while (tyaw < -M_PI)
      tyaw += 2. * M_PI;

    if (targets_[i].is_vis_bb_ )
    {
      jsk_recognition_msgs::BoundingBox bb;
      bb.header = input.header;
      bb = targets_[i].jsk_bb_;
      updateJskLabel(targets_[i], bb);
      jskbboxes_output.boxes.push_back(bb);
    }
    // RPY to convert: 0, 0, targets_[i].x_merge_(3)
    tf::Quaternion q = tf::createQuaternionFromRPY(0, 0, tyaw);
    autoware_msgs::DetectedObject dd;
    dd.header             = input.header;
    dd.id                 = targets_[i].ukf_id_;
    dd.velocity.linear.x  = tv;
    dd.pose               = targets_[i].jsk_bb_.pose;
    dd.pose.position.x    = tx;
    dd.pose.position.y    = ty;
    dd.pose.orientation.x = q[0];
    dd.pose.orientation.y = q[1];
    dd.pose.orientation.z = q[2];
    dd.pose.orientation.w = q[3];
    dd.dimensions         = targets_[i].jsk_bb_.dimensions;
    dd.pose_reliable      = targets_[i].is_vis_bb_;
    // Store tyaw in velocity.linear.y since nowhere to store estimated_yaw
    updateLabel(targets_[i], dd);
    detected_objects_output.objects.push_back(dd);
  }
}

void ImmUkfPda::removeUnnecessaryTarget()
{
  std::vector<IMM_RAUKF> temp_targets;
  for(size_t i = 0; i < targets_.size(); i++)
  {
    if(targets_[i].tracking_num_ != TrackingState::Die)
    {
      temp_targets.push_back(targets_[i]);
    }
  }
  std::vector<IMM_RAUKF>().swap(targets_);
  targets_ = temp_targets;
}

void ImmUkfPda::dumpResultText(autoware_msgs::DetectedObjectArray& detected_objects)
{
  std::ofstream outputfile(result_file_path_, std::ofstream::out | std::ofstream::app);
  for(size_t i = 0; i < detected_objects.objects.size(); i++)
  {
    tf::Quaternion q(detected_objects.objects[i].pose.orientation.x, detected_objects.objects[i].pose.orientation.y,
                     detected_objects.objects[i].pose.orientation.z, detected_objects.objects[i].pose.orientation.w);
    double roll, pitch, yaw;
    tf::Matrix3x3(q).getRPY(roll, pitch, yaw);
    outputfile << std::to_string(frame_count_)                               <<" "
               << std::to_string(detected_objects.objects[i].id)             <<" "
               << "Unknown"                                                  <<" "
               << "-1"                                                       <<" "
               << "-1"                                                       <<" "
               << "-10"                                                      <<" "
               << "-1 -1 -1 -1"                                              <<" "
               << std::to_string(detected_objects.objects[i].dimensions.x)   <<" "
               << std::to_string(detected_objects.objects[i].dimensions.y)   <<" "
               << "-1"                                                       <<" "
               << std::to_string(detected_objects.objects[i].pose.position.x)<<" "
               << std::to_string(detected_objects.objects[i].pose.position.y)<<" "
               << "-1"                                                       <<" "
               << std::to_string(yaw)                                        <<"\n";
  }
}

void ImmUkfPda::pubPoints(const autoware_msgs::DetectedObjectArray& input)
{
  visualization_msgs::MarkerArray texts_markers;
  visualization_msgs::Marker target_points, meas_points;
  target_points.header.frame_id = meas_points.header.frame_id =  "/world";
  target_points.header.stamp = meas_points.header.stamp = input.header.stamp;
  target_points.ns = meas_points.ns = "target_points";
  target_points.action = meas_points.action = visualization_msgs::Marker::ADD;
  target_points.pose.orientation.w =  meas_points.pose.orientation.w =1.0;

  target_points.id = 0;
  meas_points.id = 1;

  target_points.type = meas_points.type = visualization_msgs::Marker::POINTS;

  // POINTS markers use x and y scale for width/height respectively
  target_points.scale.x = 0.4;
  target_points.scale.y = 0.4;
  meas_points.scale.x = 0.3;
  meas_points.scale.y = 0.3;

  // Points are green
  target_points.color.r = 1.0f;
  target_points.color.a =1.0;
  meas_points.color.g = 1.0f;
  meas_points.color.a = 1.0;

  // targets text
  for(size_t i = 0; i < targets_.size(); i++)
  {
    geometry_msgs::Point p;
    p.x = targets_[i].x_merge_(0);
    p.y = targets_[i].x_merge_(1);
    p.z = 1.0;

    target_points.points.push_back(p);

    visualization_msgs::Marker id;
    id.header.frame_id =  "/world";
    id.header.stamp = input.header.stamp;
    id.ns ="target_points";
    id.action = visualization_msgs::Marker::ADD;
    id.type = visualization_msgs::Marker::TEXT_VIEW_FACING;
    id.id = targets_[i].ukf_id_ * 100;
    id.lifetime = ros::Duration(0.1);

    id.color.g = 1.0f;
    id.color.a = 1.0;

    // Set the pose of the marker.  This is a full 6DOF pose relative to the frame/time specified in the header
    id.pose.position.x = targets_[i].x_merge_(0);
    id.pose.position.y = targets_[i].x_merge_(1);
    id.pose.position.z = 2.5;

    id.scale.z = 0.5;

    double tv = targets_[i].x_merge_(2);
    // not to visualize '-0.0'
    if(abs(tv) < 0.1)
    {
      tv = 0.0;
    }
    std::string s_velocity = std::to_string(tv*3.6);
    std::string modified_sv = s_velocity.substr(0, s_velocity.find(".")+3);
    // std::string text = "<" + std::to_string(targets_[i].ukf_id_) + ">"
    //                   +" " + std::to_string(targets_[i].tracking_num_) + " "
    //                   + modified_sv + " km/h";

    // std::string text = "<" + std::to_string(targets_[i].ukf_id_) + ">" + " "
    //                   + modified_sv + " km/h";

    // std::string text = "<" + std::to_string(targets_[i].ukf_id_) + ">" + " "
    //                   + std::to_string(targets_[i].x_merge_(2)) + " m/s "
    //                   + "(" + std::to_string(targets_[i].x_merge_(0))+", " + std::to_string(targets_[i].x_merge_(1)) + ")";
    std::string text = "<" + std::to_string(targets_[i].ukf_id_) + ">" + " "
    + std::to_string(targets_[i].mode_prob_cv_) + " "
    + std::to_string(targets_[i].mode_prob_ctrv_) + " "
    + std::to_string(targets_[i].mode_prob_rm_);
    // id.text = std::to_string(input.objects[i].id);
    id.text = text;
    texts_markers.markers.push_back(id);
  }

  // meas text
  // for(size_t i = 0; i < input.objects.size(); i++)
  // {
  //   visualization_msgs::Marker id;
  //   id.header.frame_id =  "/world";
  //   id.header.stamp = input.header.stamp;
  //   id.ns ="target_points";
  //   id.action = visualization_msgs::Marker::ADD;
  //   id.type = visualization_msgs::Marker::TEXT_VIEW_FACING;
  //   id.id = i;
  //   id.lifetime = ros::Duration(0.1);
  //
  //   id.color.g = 1.0f;
  //   id.color.a = 1.0;
  //
  //   // Set the pose of the marker.  This is a full 6DOF pose relative to the frame/time specified in the header
  //   id.pose.position.x = input.objects[i].pose.position.x;
  //   id.pose.position.y = input.objects[i].pose.position.y;
  //   id.pose.position.z = 1.5;
  //
  //   id.scale.z = 0.5;
  //
  //   std::string s_px = std::to_string(input.objects[i].pose.position.x);
  //   std::string s_py = std::to_string(input.objects[i].pose.position.y);
  //
  //   std::string text = "(" + s_px+", " + s_py + ")";
  //   // id.text = std::to_string(input.objects[i].id);
  //   id.text = text;
  //   texts_markers.markers.push_back(id);
  // }


  for (size_t i = 0; i < input.objects.size(); i++)
  {
    geometry_msgs::Point p;
    p.x = input.objects[i].pose.position.x;
    p.y = input.objects[i].pose.position.y;
    p.z = 1.0;

    meas_points.points.push_back(p);
  }
  pub_points_.publish(target_points);
  pub_points_.publish(meas_points);
  pub_texts_array_.publish(texts_markers);
}

void ImmUkfPda::tracker(const autoware_msgs::DetectedObjectArray& input,
                        jsk_recognition_msgs::BoundingBoxArray& jskbboxes_output,
                        autoware_msgs::DetectedObjectArray& detected_objects_output)
{
  double timestamp = input.header.stamp.toSec();

  const double det_explode_param = 100;
  const double cov_explode_param = 1000;

  std::cout << "<<global callback count>> " << frame_count_ << std::endl;
  if (!init_)
  {
    initTracker(input, timestamp);
    makeOutput(input, jskbboxes_output, detected_objects_output);
    // std::string filepath = "/home/kosuke/tracker_out" +std::to_string(debug_nth_take_) + ".txt";
    // std::ofstream file2(filepath, std::ofstream::out | std::ofstream::app);
    // file2<< frame_count_ << " " << detected_objects_output.objects.size() <<" "<<timestamp<< "\n";
    // for(size_t i = 0; i < targets_.size(); i++)
    // {
    //   file2<<targets_[i].ukf_id_ << " "
    //        <<targets_[i].x_ctrv_(0) << " "
    //        <<targets_[i].x_ctrv_(1) << " "
    //        <<targets_[i].x_ctrv_(2) << " "
    //        <<targets_[i].x_ctrv_(3) << " "
    //        <<targets_[i].x_ctrv_(4) << " "
    //        <<targets_[i].p_ctrv_(0,0) << " "
    //        <<targets_[i].p_ctrv_(1,1) << " "
    //        <<targets_[i].p_ctrv_(2,2) << " "
    //        <<targets_[i].p_ctrv_(3,3) << " "
    //        <<targets_[i].p_ctrv_(4,4) << "\n";
    //        // <<targets_[i].p_merge_(0,0) << " "
    //        // <<targets_[i].p_merge_(1,1) << " "
    //        // <<targets_[i].p_merge_(2,2) << " "
    //        // <<targets_[i].p_merge_(3,3) << " "
    //        // <<targets_[i].p_merge_(4,4) << "\n";
    // }
    return;
  }

  double dt = (timestamp - timestamp_);
  // std::cout << "dt " << dt << std::endl;
  timestamp_ = timestamp;
  // // used for making new target with no data association
  std::vector<bool> matching_vec(input.objects.size(), false);  // make 0 vector

  // start UKF process
  for (size_t i = 0; i < targets_.size(); i++)
  {
    // std::cout << "----ukf id ------" << targets_[i].ukf_id_ << std::endl;
    // std::cout << "target lifiteme " << targets_[i].lifetime_ << std::endl;
    // std::cout << "target tracking num " << targets_[i].tracking_num_ << std::endl;
    // reset is_vis_bb_ to false
    targets_[i].is_vis_bb_ = false;
    targets_[i].is_static_ = false;

    // todo: modify here. This skips irregular measurement and nan
    if (targets_[i].tracking_num_ == TrackingState::Die)
    {
      continue;
    }
    // prevent ukf not to explode
    if (targets_[i].p_merge_.determinant() > det_explode_param || targets_[i].p_merge_(4, 4) > cov_explode_param)
    {
      // std::cout << targets_[i].ukf_id_<<" target lost because of explosion " << std::endl;
      // std::cout << "det " << targets_[i].p_merge_.determinant()<< "cov yaw rate "<< targets_[i].p_merge_(4, 4) << std::endl;
      targets_[i].tracking_num_ = TrackingState::Die;
      continue;
    }

    if(use_sukf_)
    {
      // sukf prediction step
      targets_[i].predictionSUKF(dt);
      // data association
      bool is_skip_target;
      std::vector<autoware_msgs::DetectedObject> object_vec;
      probabilisticDataAssociation(input, dt, det_explode_param, matching_vec, object_vec, targets_[i], is_skip_target);
      if (is_skip_target)
      {
        targets_[i].debug_object_num_meas_ = 0;
        continue;
      }
      targets_[i].debug_object_num_meas_ = object_vec.size();
      // sukf update step
      targets_[i].updateSUKF(object_vec);
    }
    else //imm ukf pda filter
    {
      // immukf prediction step
      targets_[i].predictionIMMUKF(dt);
      // data association
      bool is_skip_target;
      std::vector<autoware_msgs::DetectedObject> object_vec;
      probabilisticDataAssociation(input, dt, det_explode_param, matching_vec, object_vec, targets_[i], is_skip_target);
      if (is_skip_target)
      {
        continue;
      }
      // immukf update step
      targets_[i].updateIMMUKF(detection_probability_, gate_probability_ , gating_thres_, object_vec);
    }

    if(use_robust_adaptive_filter_)
    {
      targets_[i].robustAdaptiveFilter(use_sukf_);
    }

    // std::cout << "x " << std::endl<<targets_[i].x_ctrv_ << std::endl;
    // std::cout << "p " << std::endl<<targets_[i].p_ctrv_ << std::endl;
    // std::cout << "k " << std::endl<<targets_[i].k_cv_ << std::endl;
    // std::cout << "mode prob " << targets_[i].mode_prob_cv_ << " " << targets_[i].mode_prob_ctrv_ << " " << targets_[i].mode_prob_rm_ << std::endl;

  }
  // end UKF process

  // std::string filepath = "/home/kosuke/tracker_out" +std::to_string(debug_nth_take_) + ".txt";
  // std::ofstream file2(filepath, std::ofstream::out | std::ofstream::app);
  // file2<< frame_count_ << " " << detected_objects_output.objects.size() <<" "<<dt <<"\n";
  // for(size_t i = 0; i < targets_.size(); i++)
  // {
  //   file2<<targets_[i].ukf_id_ << " "
  //        <<targets_[i].ctrv_meas_(0)<<" "
  //        <<targets_[i].ctrv_meas_(1)<<" "
  //        <<targets_[i].debug_object_num_meas_ << " "
  //        <<targets_[i].x_ctrv_(0) << " "
  //        <<targets_[i].x_ctrv_(1) << " "
  //        <<targets_[i].x_ctrv_(2) << " "
  //        <<targets_[i].x_ctrv_(3) << " "
  //        <<targets_[i].x_ctrv_(4) << " "
  //        <<targets_[i].p_ctrv_(0,0) << " "
  //        <<targets_[i].p_ctrv_(1,1) << " "
  //        <<targets_[i].p_ctrv_(2,2) << " "
  //        <<targets_[i].p_ctrv_(3,3) << " "
  //        <<targets_[i].p_ctrv_(4,4) << "\n";
  // }

  //debug, green is for measurement points, red is for estimated points
  pubPoints(input);

  // making new ukf target for no data association clusters
  makeNewTargets(timestamp, input, matching_vec);

  // static dynamic classification
  staticClassification();

  // making output for visualization
  makeOutput(input, jskbboxes_output, detected_objects_output);

  removeUnnecessaryTarget();
}
