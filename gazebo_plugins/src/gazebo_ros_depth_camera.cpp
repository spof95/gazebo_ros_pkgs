/*
 * Copyright 2013 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/
/*
   Desc: GazeboRosDepthCamera plugin for simulating cameras in Gazebo
   Author: John Hsu
   Date: 24 Sept 2008
 */

#include <algorithm>
#include <assert.h>
#include <boost/thread/thread.hpp>
#include <boost/bind.hpp>

#include <gazebo_plugins/gazebo_ros_depth_camera.h>

#include <gazebo/sensors/Sensor.hh>
#include <sdf/sdf.hh>
#include <gazebo/sensors/SensorTypes.hh>

#ifdef ENABLE_PROFILER
#include <ignition/common/Profiler.hh>
#endif

#include <sensor_msgs/point_cloud2_iterator.h>

#include <tf/tf.h>
#include <tf2_sensor_msgs/tf2_sensor_msgs.h>

namespace gazebo
{
// Register this plugin with the simulator
GZ_REGISTER_SENSOR_PLUGIN(GazeboRosDepthCamera)

////////////////////////////////////////////////////////////////////////////////
// Constructor
GazeboRosDepthCamera::GazeboRosDepthCamera()
{
  this->point_cloud_connect_count_ = 0;
  this->normals_connect_count_ = 0;
  this->depth_image_connect_count_ = 0;
  this->depth_info_connect_count_ = 0;
  this->reflectance_connect_count_ = 0;
  this->last_depth_image_camera_info_update_time_ = common::Time(0);
}

////////////////////////////////////////////////////////////////////////////////
// Destructor
GazeboRosDepthCamera::~GazeboRosDepthCamera()
{
  if (pcd_ != nullptr)
  {
    delete [] pcd_;
  }
}

////////////////////////////////////////////////////////////////////////////////
// Load the controller
void GazeboRosDepthCamera::Load(sensors::SensorPtr _parent, sdf::ElementPtr _sdf)
{
  DepthCameraPlugin::Load(_parent, _sdf);

  // Make sure the ROS node for Gazebo has already been initialized
  if (!ros::isInitialized())
  {
    ROS_FATAL_STREAM_NAMED("depth_camera", "A ROS node for Gazebo has not been initialized, unable to load plugin. "
      << "Load the Gazebo system plugin 'libgazebo_ros_api_plugin.so' in the gazebo_ros package)");
    return;
  }

  // copying from DepthCameraPlugin into GazeboRosCameraUtils
  this->parentSensor_ = this->parentSensor;
  this->width_ = this->width;
  this->height_ = this->height;
  this->depth_ = this->depth;
  this->format_ = this->format;
  this->camera_ = this->depthCamera;

  // using a different default
  if (!_sdf->HasElement("imageTopicName"))
    this->image_topic_name_ = "ir/image_raw";
  if (!_sdf->HasElement("cameraInfoTopicName"))
    this->camera_info_topic_name_ = "ir/camera_info";

  // point cloud stuff
  if (!_sdf->HasElement("pointCloudTopicName"))
    this->point_cloud_topic_name_ = "points";
  else
    this->point_cloud_topic_name_ = _sdf->GetElement("pointCloudTopicName")->Get<std::string>();

  // reflectance stuff
  if (!_sdf->HasElement("reflectanceTopicName"))
    this->reflectance_topic_name_ = "reflectance";
  else
    this->reflectance_topic_name_ = _sdf->GetElement("reflectanceTopicName")->Get<std::string>();

  // normals stuff
  if (!_sdf->HasElement("normalsTopicName"))
    this->normals_topic_name_ = "normals";
  else
    this->normals_topic_name_ = _sdf->GetElement("normalsTopicName")->Get<std::string>();

  // depth image stuff
  if (!_sdf->HasElement("depthImageTopicName"))
    this->depth_image_topic_name_ = "depth/image_raw";
  else
    this->depth_image_topic_name_ = _sdf->GetElement("depthImageTopicName")->Get<std::string>();

  if (!_sdf->HasElement("depthImageCameraInfoTopicName"))
    this->depth_image_camera_info_topic_name_ = "depth/camera_info";
  else
    this->depth_image_camera_info_topic_name_ = _sdf->GetElement("depthImageCameraInfoTopicName")->Get<std::string>();

  if (!_sdf->HasElement("pointCloudCutoff"))
    this->point_cloud_cutoff_ = 0.4;
  else
    this->point_cloud_cutoff_ = _sdf->GetElement("pointCloudCutoff")->Get<double>();

  if (!_sdf->HasElement("reduceNormals"))
    this->reduce_normals_ = 50;
  else
    this->reduce_normals_ = _sdf->GetElement("reduceNormals")->Get<int>();

  // allow optional publication of depth images in 16UC1 instead of 32FC1
  if (!_sdf->HasElement("useDepth16UC1Format"))
    this->use_depth_image_16UC1_format_ = false;
  else
    this->use_depth_image_16UC1_format_ = _sdf->GetElement("useDepth16UC1Format")->Get<bool>();

    if (!_sdf->HasElement("cameraFrameToPointCloudFrame"))
        tf_ = geometry_msgs::Transform();
    else {
        auto trafostring = _sdf->GetElement("cameraFrameToPointCloudFrame")->Get<std::string>();
        std::vector<double> pose;
        std::string token;
        while (token != trafostring) {
            token = trafostring.substr(0, trafostring.find_first_of(' '));
            pose.push_back(std::stod(token));
            trafostring = trafostring.substr(trafostring.find_first_of(' ') + 1);
        }
        tf_.translation.x = pose[0];
        tf_.translation.y = pose[1];
        tf_.translation.z = pose[2];

        ignition::math::Quaterniond q = ignition::math::Quaterniond::EulerToQuaternion(pose[3], pose[4], pose[5]);
        tf_.rotation.x = q.X();
        tf_.rotation.y = q.Y();
        tf_.rotation.z = q.Z();
        tf_.rotation.w = q.W();
    }

    if (!_sdf->HasElement("pointCloudFrameName"))
        pointCloudFrameName_ = frame_name_;
    else
        pointCloudFrameName_ = _sdf->GetElement("pointCloudFrameName")->Get<std::string>();

    if (!_sdf->HasElement("depthImageFrameName"))
        depthImageFrameName_ = frame_name_;
    else
        depthImageFrameName_ = _sdf->GetElement("depthImageFrameName")->Get<std::string>();

  load_connection_ = GazeboRosCameraUtils::OnLoad(boost::bind(&GazeboRosDepthCamera::Advertise, this));
  GazeboRosCameraUtils::Load(_parent, _sdf);
}

void GazeboRosDepthCamera::Advertise()
{
  ros::AdvertiseOptions point_cloud_ao =
    ros::AdvertiseOptions::create<sensor_msgs::PointCloud2 >(
      this->point_cloud_topic_name_,1,
      boost::bind( &GazeboRosDepthCamera::PointCloudConnect,this),
      boost::bind( &GazeboRosDepthCamera::PointCloudDisconnect,this),
      ros::VoidPtr(), &this->camera_queue_);
  this->point_cloud_pub_ = this->rosnode_->advertise(point_cloud_ao);

  ros::AdvertiseOptions depth_image_ao =
    ros::AdvertiseOptions::create< sensor_msgs::Image >(
      this->depth_image_topic_name_,1,
      boost::bind( &GazeboRosDepthCamera::DepthImageConnect,this),
      boost::bind( &GazeboRosDepthCamera::DepthImageDisconnect,this),
      ros::VoidPtr(), &this->camera_queue_);
  this->depth_image_pub_ = this->rosnode_->advertise(depth_image_ao);

  ros::AdvertiseOptions depth_image_camera_info_ao =
    ros::AdvertiseOptions::create<sensor_msgs::CameraInfo>(
        this->depth_image_camera_info_topic_name_,1,
        boost::bind( &GazeboRosDepthCamera::DepthInfoConnect,this),
        boost::bind( &GazeboRosDepthCamera::DepthInfoDisconnect,this),
        ros::VoidPtr(), &this->camera_queue_);
  this->depth_image_camera_info_pub_ = this->rosnode_->advertise(depth_image_camera_info_ao);

#if GAZEBO_MAJOR_VERSION == 9 && GAZEBO_MINOR_VERSION > 12
  ros::AdvertiseOptions reflectance_ao =
    ros::AdvertiseOptions::create<sensor_msgs::Image>(
      reflectance_topic_name_, 1,
      boost::bind( &GazeboRosDepthCamera::ReflectanceConnect,this),
      boost::bind( &GazeboRosDepthCamera::ReflectanceDisconnect,this),
      ros::VoidPtr(), &this->camera_queue_);
  this->reflectance_pub_ = this->rosnode_->advertise(reflectance_ao);

  ros::AdvertiseOptions normals_ao =
    ros::AdvertiseOptions::create<visualization_msgs::MarkerArray >(
      normals_topic_name_, 1,
      boost::bind( &GazeboRosDepthCamera::NormalsConnect,this),
      boost::bind( &GazeboRosDepthCamera::NormalsDisconnect,this),
      ros::VoidPtr(), &this->camera_queue_);
  this->normal_pub_ = this->rosnode_->advertise(normals_ao);
#endif
}

////////////////////////////////////////////////////////////////////////////////
// Increment count
void GazeboRosDepthCamera::PointCloudConnect()
{
  this->point_cloud_connect_count_++;
  (*this->image_connect_count_)++;
  this->parentSensor->SetActive(true);
}

////////////////////////////////////////////////////////////////////////////////
// Decrement count
void GazeboRosDepthCamera::PointCloudDisconnect()
{
  this->point_cloud_connect_count_--;
  (*this->image_connect_count_)--;
  if (this->point_cloud_connect_count_ <= 0)
    this->parentSensor->SetActive(false);
}

////////////////////////////////////////////////////////////////////////////////
// Increment count
void GazeboRosDepthCamera::ReflectanceConnect()
{
  this->reflectance_connect_count_++;
  (*this->image_connect_count_)++;
  this->parentSensor->SetActive(true);
}

////////////////////////////////////////////////////////////////////////////////
// Increment count
void GazeboRosDepthCamera::NormalsConnect()
{
  this->normals_connect_count_++;
  (*this->image_connect_count_)++;
  this->parentSensor->SetActive(true);
}

////////////////////////////////////////////////////////////////////////////////
// Decrement count
void GazeboRosDepthCamera::ReflectanceDisconnect()
{
  this->reflectance_connect_count_--;
  (*this->image_connect_count_)--;
  if (this->reflectance_connect_count_ <= 0)
    this->parentSensor->SetActive(false);
}

////////////////////////////////////////////////////////////////////////////////
// Decrement count
void GazeboRosDepthCamera::NormalsDisconnect()
{
  this->normals_connect_count_--;
  (*this->image_connect_count_)--;
  if (this->reflectance_connect_count_ <= 0)
    this->parentSensor->SetActive(false);
}

////////////////////////////////////////////////////////////////////////////////
// Increment count
void GazeboRosDepthCamera::DepthImageConnect()
{
  this->depth_image_connect_count_++;
  this->parentSensor->SetActive(true);
}

////////////////////////////////////////////////////////////////////////////////
// Decrement count
void GazeboRosDepthCamera::DepthImageDisconnect()
{
  this->depth_image_connect_count_--;
}

////////////////////////////////////////////////////////////////////////////////
// Increment count
void GazeboRosDepthCamera::DepthInfoConnect()
{
  this->depth_info_connect_count_++;
}
////////////////////////////////////////////////////////////////////////////////
// Decrement count
void GazeboRosDepthCamera::DepthInfoDisconnect()
{
  this->depth_info_connect_count_--;
}

////////////////////////////////////////////////////////////////////////////////
// Update the controller
void GazeboRosDepthCamera::OnNewDepthFrame(const float *_image,
    unsigned int _width, unsigned int _height, unsigned int _depth,
    const std::string &_format)
{
#ifdef ENABLE_PROFILER
  IGN_PROFILE("GazeboRosDepthCamera::OnNewDepthFrame");
#endif
  if (!this->initialized_ || this->height_ <=0 || this->width_ <=0)
    return;
#ifdef ENABLE_PROFILER
  IGN_PROFILE_BEGIN("fill ROS message");
#endif
# if GAZEBO_MAJOR_VERSION >= 7
  this->depth_sensor_update_time_ = this->parentSensor->LastMeasurementTime();
# else
  this->depth_sensor_update_time_ = this->parentSensor->GetLastMeasurementTime();
# endif

  if (this->parentSensor->IsActive())
  {
    if (this->point_cloud_connect_count_ <= 0 &&
        this->depth_image_connect_count_ <= 0 &&
        (*this->image_connect_count_) <= 0 &&
        this->normals_connect_count_ <= 0)
    {
      this->parentSensor->SetActive(false);
    }
    else
    {
      if (this->point_cloud_connect_count_ > 0 ||
          this->normals_connect_count_ > 0)
      {
        this->FillPointdCloud(_image);
      }

      if (this->depth_image_connect_count_ > 0)
        this->FillDepthImage(_image);
    }
  }
  else
  {
    if (this->point_cloud_connect_count_ > 0 ||
        this->depth_image_connect_count_ <= 0)
      // do this first so there's chance for sensor to run 1 frame after activate
      this->parentSensor->SetActive(true);
  }
#ifdef ENABLE_PROFILER
  IGN_PROFILE_END();
#endif
}

///////////////////////////////////////////////////////////////////////////////
// Update the controller
void GazeboRosDepthCamera::OnNewRGBPointCloud(const float *_pcd,
    unsigned int _width, unsigned int _height, unsigned int _depth,
    const std::string &_format)
{
#ifdef ENABLE_PROFILER
  IGN_PROFILE("GazeboRosDepthCamera::OnNewRGBPointCloud");
#endif
  if (!this->initialized_ || this->height_ <=0 || this->width_ <=0)
    return;
#ifdef ENABLE_PROFILER
  IGN_PROFILE_BEGIN("fill ROS message");
#endif
# if GAZEBO_MAJOR_VERSION >= 7
  this->depth_sensor_update_time_ = this->parentSensor->LastMeasurementTime();
# else
  this->depth_sensor_update_time_ = this->parentSensor->GetLastMeasurementTime();
# endif

  if (!this->parentSensor->IsActive())
  {
    if (this->point_cloud_connect_count_ > 0)
      // do this first so there's chance for sensor to run 1 frame after activate
      this->parentSensor->SetActive(true);
  }
  else
  {
    if (this->point_cloud_connect_count_ > 0 || this->normals_connect_count_ > 0)
    {
      this->lock_.lock();

      if (pcd_ == nullptr)
        pcd_ = new float[_width * _height * 4];

      memcpy(pcd_, _pcd, sizeof(float)* _width * _height * 4);

      sensor_msgs::PointCloud2 pointCloud;
      
      pointCloud.header.frame_id = this->frame_name_;
      pointCloud.header.stamp.sec = this->depth_sensor_update_time_.sec;
      pointCloud.header.stamp.nsec = this->depth_sensor_update_time_.nsec;
      pointCloud.width = this->width;
      pointCloud.height = this->height;
      pointCloud.row_step = pointCloud.point_step * this->width;

      sensor_msgs::PointCloud2Modifier pcd_modifier(pointCloud);
      pcd_modifier.setPointCloud2FieldsByString(2, "xyz", "rgb");
      pcd_modifier.resize(_width*_height);

      pointCloud.is_dense = true;

      sensor_msgs::PointCloud2Iterator<float> iter_x(pointCloud, "x");
      sensor_msgs::PointCloud2Iterator<float> iter_y(pointCloud, "y");
      sensor_msgs::PointCloud2Iterator<float> iter_z(pointCloud, "z");
      sensor_msgs::PointCloud2Iterator<float> iter_rgb(pointCloud, "rgb");

      for (unsigned int i = 0; i < _width; i++)
      {
        for (unsigned int j = 0; j < _height; j++, ++iter_x, ++iter_y, ++iter_z, ++iter_rgb)
        {
          unsigned int index = (j * _width) + i;
          *iter_x = _pcd[4 * index];
          *iter_y = _pcd[4 * index + 1];
          *iter_z = _pcd[4 * index + 2];
          *iter_rgb = _pcd[4 * index + 3];
        }
      }

        geometry_msgs::TransformStamped tf;
        tf.header.frame_id = pointCloudFrameName_;
        tf.child_frame_id = frame_name_;
        tf.transform = tf_;

        tf2::doTransform(pointCloud, point_cloud_msg_, tf);

      this->point_cloud_pub_.publish(point_cloud_msg_);
      this->lock_.unlock();
    }
  }
#ifdef ENABLE_PROFILER
  IGN_PROFILE_END();
#endif
}

#if GAZEBO_MAJOR_VERSION == 9 && GAZEBO_MINOR_VERSION > 12
////////////////////////////////////////////////////////////////////////////////
// Update the controller
void GazeboRosDepthCamera::OnNewReflectanceFrame(const float *_image,
    unsigned int _width, unsigned int _height, unsigned int _depth,
    const std::string &_format)
{
#ifdef ENABLE_PROFILER
  IGN_PROFILE("GazeboRosDepthCamera::OnNewReflectanceFrame");
#endif
  if (!this->initialized_ || this->height_ <=0 || this->width_ <=0)
    return;

#ifdef ENABLE_PROFILER
    IGN_PROFILE_BEGIN("fill ROS message");
#endif
  /// don't bother if there are no subscribers
  if (this->reflectance_connect_count_ > 0)
  {
    boost::mutex::scoped_lock lock(this->lock_);

    // copy data into image
    this->reflectance_msg_.header.frame_id = this->frame_name_;
    this->reflectance_msg_.header.stamp.sec = this->sensor_update_time_.sec;
    this->reflectance_msg_.header.stamp.nsec = this->sensor_update_time_.nsec;

    // copy from src to image_msg_
    fillImage(this->reflectance_msg_, sensor_msgs::image_encodings::TYPE_32FC1, _height, _width,
        4*_width, reinterpret_cast<const void*>(_image));

    // publish to ros
    this->reflectance_pub_.publish(this->reflectance_msg_);
  }
#ifdef ENABLE_PROFILER
  IGN_PROFILE_END();
#endif
}
#endif

////////////////////////////////////////////////////////////////////////////////
// Update the controller
void GazeboRosDepthCamera::OnNewImageFrame(const unsigned char *_image,
    unsigned int _width, unsigned int _height, unsigned int _depth,
    const std::string &_format)
{
#ifdef ENABLE_PROFILER
  IGN_PROFILE("GazeboRosDepthCamera::OnNewImageFrame");
#endif
  if (!this->initialized_ || this->height_ <=0 || this->width_ <=0)
    return;
#ifdef ENABLE_PROFILER
  IGN_PROFILE_BEGIN("fill ROS message");
#endif
  //ROS_ERROR_NAMED("depth_camera", "camera_ new frame %s %s",this->parentSensor_->GetName().c_str(),this->frame_name_.c_str());
# if GAZEBO_MAJOR_VERSION >= 7
  this->sensor_update_time_ = this->parentSensor->LastMeasurementTime();
# else
  this->sensor_update_time_ = this->parentSensor->GetLastMeasurementTime();
# endif

  if (!this->parentSensor->IsActive())
  {
    if ((*this->image_connect_count_) > 0)
      // do this first so there's chance for sensor to run 1 frame after activate
      this->parentSensor->SetActive(true);
  }
  else
  {
    if ((*this->image_connect_count_) > 0)
    {
      this->PutCameraData(_image);
      // TODO(lucasw) publish camera info with depth image
      // this->PublishCameraInfo(sensor_update_time);
    }
  }
#ifdef ENABLE_PROFILER
  IGN_PROFILE_END();
#endif
}

#if GAZEBO_MAJOR_VERSION == 9 && GAZEBO_MINOR_VERSION > 12
void GazeboRosDepthCamera::OnNewNormalsFrame(const float * _normals,
               unsigned int _width, unsigned int _height,
               unsigned int _depth, const std::string &_format)
{
#ifdef ENABLE_PROFILER
  IGN_PROFILE("GazeboRosDepthCamera::OnNewNormalsFrame");
#endif
  if (!this->initialized_ || this->height_ <=0 || this->width_ <=0)
    return;
#ifdef ENABLE_PROFILER
  IGN_PROFILE_BEGIN("fill ROS message");
#endif
  visualization_msgs::MarkerArray m_array;

  if (!this->parentSensor->IsActive())
  {
    if (this->normals_connect_count_ > 0)
      // do this first so there's chance for sensor to run 1 frame after activate
      this->parentSensor->SetActive(true);
  }
  else
  {
    if (this->normals_connect_count_ > 0)
    {
      boost::mutex::scoped_lock lock(this->lock_);
      if (pcd_ != nullptr)
      {
        for (unsigned int i = 0; i < _width; i++)
        {
          for (unsigned int j = 0; j < _height; j++)
          {
            // plotting some of the normals, otherwise rviz will block it
            unsigned int index = (j * _width) + i;
            if (index % this->reduce_normals_ == 0)
            {
              visualization_msgs::Marker m;
              m.type = visualization_msgs::Marker::ARROW;
              m.header.frame_id = this->frame_name_;
              m.header.stamp.sec = this->depth_sensor_update_time_.sec;
              m.header.stamp.nsec = this->depth_sensor_update_time_.nsec;
              m.action = visualization_msgs::Marker::ADD;

              m.color.r = 1.0;
              m.color.g = 0.0;
              m.color.b = 0.0;
              m.color.a = 1.0;
              m.scale.x = 1;
              m.scale.y = 0.01;
              m.scale.z = 0.01;
              m.lifetime.sec = 1;
              m.lifetime.nsec = 0;

              m.id = index;
              float x = _normals[4 * index];
              float y = _normals[4 * index + 1];
              float z = _normals[4 * index + 2];

              m.pose.position.x = pcd_[4 * index];
              m.pose.position.y = pcd_[4 * index + 1];
              m.pose.position.z = pcd_[4 * index + 2];

              // calculating the angle of the normal with the world
              tf::Vector3 axis_vector(x, y, z);
              tf::Quaternion q = tf::Quaternion::getIdentity();
              if (!axis_vector.isZero())
              {
                tf::Vector3 vector(1.0, 0.0, 0.0);
                tf::Vector3 right_vector = axis_vector.cross(vector);
                right_vector.normalized();
                q.setRotation(right_vector, -1.0*acos(axis_vector.dot(vector)));
                q.normalize();
              }

              m.pose.orientation.x = q.x();
              m.pose.orientation.y = q.y();
              m.pose.orientation.z = q.z();
              m.pose.orientation.w = q.w();

              m_array.markers.push_back(m);
            }
          }
        }
      }
      this->normal_pub_.publish(m_array);
    }
  }
#ifdef ENABLE_PROFILER
  IGN_PROFILE_END();
#endif
}
#endif

////////////////////////////////////////////////////////////////////////////////
// Put camera data to the interface
void GazeboRosDepthCamera::FillPointdCloud(const float *_src)
{
  this->lock_.lock();

  sensor_msgs::PointCloud2 pointCloud;
  pointCloud.header.frame_id = this->frame_name_;
  pointCloud.header.stamp.sec = this->depth_sensor_update_time_.sec;
  pointCloud.header.stamp.nsec = this->depth_sensor_update_time_.nsec;
  pointCloud.width = this->width;
  pointCloud.height = this->height;
  pointCloud.row_step = pointCloud.point_step * this->width;

  ///copy from depth to point cloud message
  FillPointCloudHelper(pointCloud,
                 this->height,
                 this->width,
                 this->skip_,
                 (void*)_src );

  geometry_msgs::TransformStamped tf;
  tf.header.frame_id = pointCloudFrameName_;
  tf.child_frame_id = frame_name_;
  tf.transform = tf_;

  tf2::doTransform(pointCloud, point_cloud_msg_, tf);

  this->point_cloud_pub_.publish(point_cloud_msg_);

  this->lock_.unlock();
}

////////////////////////////////////////////////////////////////////////////////
// Put depth image data to the interface
void GazeboRosDepthCamera::FillDepthImage(const float *_src)
{
  this->lock_.lock();
  // copy data into image
  this->depth_image_msg_.header.frame_id = this->frame_name_;
  this->depth_image_msg_.header.stamp.sec = this->depth_sensor_update_time_.sec;
  this->depth_image_msg_.header.stamp.nsec = this->depth_sensor_update_time_.nsec;

  ///copy from depth to depth image message
  FillDepthImageHelper(this->depth_image_msg_,
                 this->height,
                 this->width,
                 this->skip_,
                 (void*)_src );

  this->depth_image_pub_.publish(this->depth_image_msg_);

  this->lock_.unlock();
}


// Fill depth information
bool GazeboRosDepthCamera::FillPointCloudHelper(
    sensor_msgs::PointCloud2 &point_cloud_msg,
    uint32_t rows_arg, uint32_t cols_arg,
    uint32_t step_arg, void* data_arg)
{
  sensor_msgs::PointCloud2Modifier pcd_modifier(point_cloud_msg);
  pcd_modifier.setPointCloud2FieldsByString(2, "xyz", "rgb");
  pcd_modifier.resize(rows_arg*cols_arg);

  sensor_msgs::PointCloud2Iterator<float> iter_x(point_cloud_msg, "x");
  sensor_msgs::PointCloud2Iterator<float> iter_y(point_cloud_msg, "y");
  sensor_msgs::PointCloud2Iterator<float> iter_z(point_cloud_msg, "z");
  sensor_msgs::PointCloud2Iterator<uint8_t> iter_rgb(point_cloud_msg, "rgb");

  point_cloud_msg.is_dense = true;

  float* toCopyFrom = (float*)data_arg;
  int index = 0;

  double hfov = this->parentSensor->DepthCamera()->HFOV().Radian();
  double fl = ((double)this->width) / (2.0 *tan(hfov/2.0));

  if (pcd_ == nullptr){
    pcd_ = new float[rows_arg * cols_arg * 4];
  }

  // convert depth to point cloud
  for (uint32_t j=0; j<rows_arg; j++)
  {
    double pAngle;
    if (rows_arg>1) pAngle = atan2( (double)j - 0.5*(double)(rows_arg-1), fl);
    else            pAngle = 0.0;

    for (uint32_t i=0; i<cols_arg; i++, ++iter_x, ++iter_y, ++iter_z, ++iter_rgb)
    {
      double yAngle;
      if (cols_arg>1) yAngle = atan2( (double)i - 0.5*(double)(cols_arg-1), fl);
      else            yAngle = 0.0;

      double depth = toCopyFrom[index++];

      // in optical frame
      // hardcoded rotation rpy(-M_PI/2, 0, -M_PI/2) is built-in
      // to urdf, where the *_optical_frame should have above relative
      // rotation from the physical camera *_frame
      unsigned int index = (j * cols_arg) + i;
      *iter_x      = depth * tan(yAngle);
      *iter_y      = depth * tan(pAngle);
      if(depth > this->point_cloud_cutoff_)
      {
        *iter_z    = depth;
        pcd_[4 * index + 2] = *iter_z;
      }
      else //point in the unseeable range
      {
        *iter_x = *iter_y = *iter_z = std::numeric_limits<float>::quiet_NaN ();
        pcd_[4 * index + 2] = 0;
        point_cloud_msg.is_dense = false;
      }

      pcd_[4 * index] = *iter_x;
      pcd_[4 * index + 1] = *iter_y;
      pcd_[4 * index + 3] = 0;

      // put image color data for each point
      uint8_t*  image_src = (uint8_t*)(&(this->image_msg_.data[0]));
      if (this->image_msg_.data.size() == rows_arg*cols_arg*3)
      {
        // color
        iter_rgb[0] = image_src[i*3+j*cols_arg*3+0];
        iter_rgb[1] = image_src[i*3+j*cols_arg*3+1];
        iter_rgb[2] = image_src[i*3+j*cols_arg*3+2];
      }
      else if (this->image_msg_.data.size() == rows_arg*cols_arg)
      {
        // mono (or bayer?  @todo; fix for bayer)
        iter_rgb[0] = image_src[i+j*cols_arg];
        iter_rgb[1] = image_src[i+j*cols_arg];
        iter_rgb[2] = image_src[i+j*cols_arg];
      }
      else
      {
        // no image
        iter_rgb[0] = 0;
        iter_rgb[1] = 0;
        iter_rgb[2] = 0;
      }
    }
  }

  return true;
}

// Fill depth information
bool GazeboRosDepthCamera::FillDepthImageHelper(
    sensor_msgs::Image& image_msg,
    uint32_t rows_arg, uint32_t cols_arg,
    uint32_t step_arg, void* data_arg)
{
  image_msg.height = rows_arg;
  image_msg.width = cols_arg;
  image_msg.is_bigendian = 0;
  // deal with the differences in between 32FC1 & 16UC1
  // http://www.ros.org/reps/rep-0118.html#id4
  union uint16_or_float
  {
    uint16_t* dest_uint16;
    float* dest_float;
  };
  uint16_or_float dest;
  if (!this->use_depth_image_16UC1_format_)
  {
    image_msg.encoding = sensor_msgs::image_encodings::TYPE_32FC1;
    image_msg.step = sizeof(float) * cols_arg;
    image_msg.data.resize(rows_arg * cols_arg * sizeof(float));
    dest.dest_float = (float*)(&(image_msg.data[0]));
  }
  else
  {
    image_msg.encoding = sensor_msgs::image_encodings::TYPE_16UC1;
    image_msg.step = sizeof(uint16_t) * cols_arg;
    image_msg.data.resize(rows_arg * cols_arg * sizeof(uint16_t));
    dest.dest_uint16 = (uint16_t*)(&(image_msg.data[0]));
  }

  float* toCopyFrom = (float*)data_arg;
  int index = 0;

  // convert depth to point cloud
  for (uint32_t j = 0; j < rows_arg; j++)
  {
    for (uint32_t i = 0; i < cols_arg; i++)
    {
      float depth = toCopyFrom[index++];

      if (depth > this->point_cloud_cutoff_)
      {
        if (!this->use_depth_image_16UC1_format_)
          dest.dest_float[i + j * cols_arg] = depth;
        else
          dest.dest_uint16[i + j * cols_arg] = depth * 1000.0;
      }
      else //point in the unseeable range
      {
        if (!this->use_depth_image_16UC1_format_)
          dest.dest_float[i + j * cols_arg] = std::numeric_limits<float>::quiet_NaN();
        else
          dest.dest_uint16[i + j * cols_arg] = 0;
      }
    }
  }
  return true;
}

void GazeboRosDepthCamera::PublishCameraInfo()
{
  ROS_DEBUG_NAMED("depth_camera", "publishing default camera info, then depth camera info");
  GazeboRosCameraUtils::PublishCameraInfo();

  if (this->depth_info_connect_count_ > 0)
  {
# if GAZEBO_MAJOR_VERSION >= 7
    common::Time sensor_update_time = this->parentSensor_->LastMeasurementTime();
# else
    common::Time sensor_update_time = this->parentSensor_->GetLastMeasurementTime();
# endif
    this->sensor_update_time_ = sensor_update_time;
    if (sensor_update_time - this->last_depth_image_camera_info_update_time_ >= this->update_period_)
    {
      this->PublishCameraInfo(this->depth_image_camera_info_pub_);  // , sensor_update_time);
      this->last_depth_image_camera_info_update_time_ = sensor_update_time;
    }
  }
}

//@todo: publish disparity similar to openni_camera_deprecated/src/nodelets/openni_nodelet.cpp.
/*
#include <stereo_msgs/DisparityImage.h>
pub_disparity_ = comm_nh.advertise<stereo_msgs::DisparityImage > ("depth/disparity", 5, subscriberChanged2, subscriberChanged2);

void GazeboRosDepthCamera::PublishDisparityImage(const DepthImage& depth, ros::Time time)
{
  stereo_msgs::DisparityImagePtr disp_msg = boost::make_shared<stereo_msgs::DisparityImage > ();
  disp_msg->header.stamp                  = time;
  disp_msg->header.frame_id               = device_->isDepthRegistered () ? rgb_frame_id_ : depth_frame_id_;
  disp_msg->image.header                  = disp_msg->header;
  disp_msg->image.encoding                = sensor_msgs::image_encodings::TYPE_32FC1;
  disp_msg->image.height                  = depth_height_;
  disp_msg->image.width                   = depth_width_;
  disp_msg->image.step                    = disp_msg->image.width * sizeof (float);
  disp_msg->image.data.resize (disp_msg->image.height * disp_msg->image.step);
  disp_msg->T = depth.getBaseline ();
  disp_msg->f = depth.getFocalLength () * depth_width_ / depth.getWidth ();

  /// @todo Compute these values from DepthGenerator::GetDeviceMaxDepth() and the like
  disp_msg->min_disparity = 0.0;
  disp_msg->max_disparity = disp_msg->T * disp_msg->f / 0.3;
  disp_msg->delta_d = 0.125;

  depth.fillDisparityImage (depth_width_, depth_height_, reinterpret_cast<float*>(&disp_msg->image.data[0]), disp_msg->image.step);

  pub_disparity_.publish (disp_msg);
}
*/


}
