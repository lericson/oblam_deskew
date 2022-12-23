/* #region HEADERS ---------------------------------------------------------------------------------------------------*/

#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <condition_variable>
#include <deque>
#include <thread>

#include <Eigen/Dense>
#include <ceres/ceres.h>

/* All needed for pointcloud manipulation -------------*/
#include <pcl/search/impl/kdtree.hpp>
#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/kdtree/impl/kdtree_flann.hpp>
#include <pcl/pcl_base.h>
#include <pcl/impl/pcl_base.hpp>
#include <pcl/filters/filter.h>
#include <pcl/filters/impl/filter.hpp>
#include <pcl/filters/uniform_sampling.h>
#include <pcl/filters/impl/uniform_sampling.hpp>
#include <pcl/filters/impl/voxel_grid.hpp>
#include <pcl/filters/crop_box.h>
#include <pcl/filters/impl/crop_box.hpp>
/* All needed for pointcloud manipulation -------------*/

#include "std_msgs/Header.h"
#include "geometry_msgs/PoseStamped.h"
#include "nav_msgs/Odometry.h"
#include "nav_msgs/Path.h"
#include "sensor_msgs/Image.h"
#include "sensor_msgs/Imu.h"
#include "sensor_msgs/PointCloud2.h"

#include <message_filters/subscriber.h>
#include <message_filters/time_synchronizer.h>
#include <message_filters/sync_policies/approximate_time.h>

// Custom for package
#include "utility.h"

/* #endregion HEADERS -----------------------------------------------------------------------------------------------*/

using namespace std;
using namespace pcl;
using namespace Eigen;
using namespace message_filters;

ros::NodeHandlePtr nh_ptr;

typedef lock_guard<mutex> mylg;
typedef sensor_msgs::Imu ImuMsg;
typedef nav_msgs::Odometry OdomMsg;
typedef sensor_msgs::PointCloud2 CloudMsg;
typedef sensor_msgs::Imu::ConstPtr ImuMsgPtr;
typedef nav_msgs::Odometry::ConstPtr OdomMsgPtr;
typedef sensor_msgs::PointCloud2::ConstPtr CloudMsgPtr;

mutex imu_mtx;
deque<ImuMsgPtr> imu_buf;

mutex oc_mtx;
deque<pair<OdomMsgPtr, CloudMsgPtr>> oc_buf;

std::deque<OdomMsgPtr> odom_buf;
CloudMsgPtr cloud_hold;

// An intrinsic
myTf tf_Bimu_Blidar;

// Publishers
ros::Publisher distortedCloudPub;          // Publishing the distorted pointcloud in world
ros::Publisher imuPropDeskewedCloudPub;    // Publishing the deskewed pointcloud from imu propagation

template<typename T>
double msgTimestamp(T msg) { return msg->header.stamp.toSec(); }

void imuCallback(const ImuMsgPtr &imuMsg)
{
    mylg lock(imu_mtx);
    imu_buf.push_back(imuMsg);
}

void odomCloudCallback(const OdomMsgPtr odomMsg, const CloudMsgPtr cloudMsg)
{
    static int skip = 10; // Skip a few pointclouds
    if (skip > 0) { skip--; return; }
    ROS_ASSERT(msgTimestamp(odomMsg) <= msgTimestamp(cloudMsg));
    //ROS_INFO("Received odom/cloud pair (skip=%d)", skip);
    oc_buf.push_back(make_pair(odomMsg, cloudMsg));    
}

void matchOdomCloud() {
    mylg lock(oc_mtx); 

    // Find odometry message right before cloud, remove as you go
    double t = msgTimestamp(cloud_hold);

    // Prune while odom_buf[1] <= t.
    while ((2 <= odom_buf.size()) && (msgTimestamp(odom_buf[1]) <= t))
        odom_buf.pop_front();

    // We have a pair if the first odom is before t and the next odom is beyond t.
    if ((2 <= odom_buf.size()) && (msgTimestamp(odom_buf[0]) <= t) && (t <= msgTimestamp(odom_buf[1]))) {
        odomCloudCallback(odom_buf.front(), cloud_hold);
        cloud_hold = nullptr;
    }
}

void odomCallback(const OdomMsg &msg){
    //printf("odom %.3f\n", msgTimestamp(&msg));
    odom_buf.push_back(OdomMsgPtr(new OdomMsg(msg)));
    if (cloud_hold)
        matchOdomCloud();
}

void cloudCallback(const CloudMsg &msg){
    if (cloud_hold)
        ROS_WARN("Throwing away a pointcloud");
    cloud_hold = CloudMsgPtr(new CloudMsg(msg));
    if (!odom_buf.empty())
        matchOdomCloud();
}

bool hasData()
{
    if (oc_buf.empty()) {
        ROS_WARN_THROTTLE(1.0, "hasData: Odom/Cloud buffer empty");
        return false;
    }

    if (imu_buf.empty()) {
        ROS_WARN_THROTTLE(1.0, "hasData: IMU buffer empty");
        return false;
    }


    if (msgTimestamp(oc_buf.front().first) < msgTimestamp(imu_buf.front()))
    {
        mylg lock(oc_mtx);
        oc_buf.pop_front();
        ROS_WARN("Deleting stale odom/cloud pair");
        return false;
    }

    if (msgTimestamp(oc_buf.front().second) + 0.125 > msgTimestamp(imu_buf.back())) {
        ROS_WARN_THROTTLE(1.0, "hasData: IMU buffer doesn't propagate far enough to cover entire point cloud");
        return false;
    }

    return true;
}

void ExtractImuData( vector<double> &ts, vector<Vector3d> &gyro, vector<Vector3d> &acce,
                     double tstart, double tend, const deque<ImuMsgPtr> &imuSeq)
{
    int Nend = imuSeq.size() - 2;
    for(int i = 0; i <= Nend; i++)
    {
        if (i == 0)
        {
            Vector3d gyro_tB(imuSeq[i]->angular_velocity.x, imuSeq[i]->angular_velocity.y, imuSeq[i]->angular_velocity.z);
            Vector3d gyro_tE(imuSeq[i+1]->angular_velocity.x, imuSeq[i+1]->angular_velocity.y, imuSeq[i+1]->angular_velocity.z);
            
            Vector3d acce_tB(imuSeq[i]->linear_acceleration.x, imuSeq[i]->linear_acceleration.y, imuSeq[i]->linear_acceleration.z);
            Vector3d acce_tE(imuSeq[i+1]->linear_acceleration.x, imuSeq[i+1]->linear_acceleration.y, imuSeq[i+1]->linear_acceleration.z);

            double tB = imuSeq[i]->header.stamp.toSec(); double tE = imuSeq[i+1]->header.stamp.toSec();
            double s  = (tstart - tB)/(tE - tB);

            Vector3d gyro_ts = (1-s)*gyro_tB + s*gyro_tE;
            Vector3d acce_ts = (1-s)*acce_tB + s*acce_tE;

            ts.push_back(tstart);
            gyro.push_back(gyro_ts);
            acce.push_back(acce_ts);
        }
        else if (i == Nend)
        {
            Vector3d gyro_tB(imuSeq[i]->angular_velocity.x, imuSeq[i]->angular_velocity.y, imuSeq[i]->angular_velocity.z);
            Vector3d gyro_tE(imuSeq[i+1]->angular_velocity.x, imuSeq[i+1]->angular_velocity.y, imuSeq[i+1]->angular_velocity.z);

            Vector3d acce_tB(imuSeq[i]->linear_acceleration.x, imuSeq[i]->linear_acceleration.y, imuSeq[i]->linear_acceleration.z);
            Vector3d acce_tE(imuSeq[i+1]->linear_acceleration.x, imuSeq[i+1]->linear_acceleration.y, imuSeq[i+1]->linear_acceleration.z);

            double tB = imuSeq[i]->header.stamp.toSec(); double tE = imuSeq[i+1]->header.stamp.toSec();
            double s  = (tend - tB)/(tE - tB);

            Vector3d gyro_ts = (1-s)*gyro_tB + s*gyro_tE;
            Vector3d acce_ts = (1-s)*acce_tB + s*acce_tE;

            ts.push_back(tend);
            gyro.push_back(gyro_ts);
            acce.push_back(acce_ts);
        }
        else
        {
            Vector3d gyro_ts(imuSeq[i]->angular_velocity.x, imuSeq[i]->angular_velocity.y, imuSeq[i]->angular_velocity.z);
            Vector3d acce_ts(imuSeq[i]->linear_acceleration.x, imuSeq[i]->linear_acceleration.y, imuSeq[i]->linear_acceleration.z);

            ts.push_back(imuSeq[i]->header.stamp.toSec());
            gyro.push_back(gyro_ts);
            acce.push_back(acce_ts);
        }
    }
}

void PropagateIMU(const OdomMsgPtr &odom,
                  const vector<double> &ts, const vector<Vector3d> &gyro_, const vector<Vector3d> &acce_,
                  vector<Quaternd> &q, vector<Vector3d> &p, vector<Vector3d> &v)
{   
    // Some constant guestimated from SLICT experiment
    static Vector3d grav(9.82, 0, 0); static Vector3d bg(-0.022, -0.033, 0.004); static Vector3d ba(0.0, 0, 0.1);

    // Initial state
    q.push_back(Quaternd(odom->pose.pose.orientation.w,
                         odom->pose.pose.orientation.x, odom->pose.pose.orientation.y, odom->pose.pose.orientation.z));
    p.push_back(Vector3d(odom->pose.pose.position.x, odom->pose.pose.position.y, odom->pose.pose.position.z));
    v.push_back(q.back()*Vector3d(odom->twist.twist.linear.x, odom->twist.twist.linear.y, odom->twist.twist.linear.z));

    // Initial measurement
    double to = ts.front(); Vector3d gyro = gyro_.front(); Vector3d acco = acce_.front();
    Quaternd Qo = q.back(); Vector3d Po = p.back(); Vector3d Vo = v.back();

    // Propagation using euler method
    for(int i = 1; i < ts.size(); i++)
    {
        double tn = ts[i]; Vector3d gyrn = gyro_[i]; Vector3d accn = acce_[i];
        
        /* ASSIGNMENT BLOCK START -----------------------------------------------------------------------------------*/
        
        // Propagate the poses by updating Qn, Vn, Pn below.
        // NOTE: you may need to use the function deltaQ(theta) in file include/utility.h to update quaternion.

        Quaternd Qn = Qo;   // Change Qn to the IMU propagated quaternion
        Vector3d Vn = Vo;   // Change Vn to the IMU propagated velocity
        Vector3d Pn = Po;   // Change Pn to the IMU propagated position

        /* ASSIGNMENT BLOCK END -------------------------------------------------------------------------------------*/

        // Store the data
        q.push_back(Qn); p.push_back(Pn); v.push_back(Vn);
        to = tn; gyro = gyrn; acco = accn; Qo = q.back(); Po = p.back(); Vo = v.back();
    }
}

void DeskewByImuPropagation(const CloudOusterPtr &cloudSkewed, const OdomMsgPtr &odom_W_Bstart,
                            vector<double> &ts, vector<Quaternd> &q_W_Bs, vector<Vector3d> &p_W_Bs)
{
    // Skip if the number of IMU samples is low
    if (ts.size() < 8) {
        ROS_WARN("Short/empty IMU sequence, ignoring");
        return;
    }

    double tstart = odom_W_Bstart->header.stamp.toSec();
    double tend = tstart + cloudSkewed->points.back().t*1e-9;
    ROS_ASSERT(ts[0] <= tstart);
    ROS_ASSERT(tend <= ts[ts.size()-1]);

    mytf tf_W_Bstart(*odom_W_Bstart);
    
    // Skewed cloud in world frame
    CloudOusterPtr cloudSkewedInWorld(new CloudOuster());
    pcl::transformPointCloud(*cloudSkewed, *cloudSkewedInWorld, tf_W_Bstart.cast<float>().tfMat());
    
    // Deskewing the points
    CloudOusterPtr cloudDeskewedInWorld(new CloudOuster());

    // Pre-allocate the elements of clouDeskewed
    int pointsTotal = cloudSkewed->size(); cloudDeskewedInWorld->resize(pointsTotal);

    // Convert points into world frame
    #pragma omp parallel for num_threads(MAX_THREADS)
    for(int i = 0; i < pointsTotal; i++)
    {
        PointOuster &pi = cloudSkewed->points[i];
        PointOuster &po = cloudDeskewedInWorld->points[i];

        po = cloudSkewedInWorld->points[i]; // Initialize the output point with skewed pointcloud for visualization

        // Sample time of the point
        double ti = tstart + pi.t/1.0e9;
        
        /* ASSIGNMENT BLOCK START -----------------------------------------------------------------------------------*/

            // Step 1: Find the j such that ts[j] < ti < ts[j+1], where ti is the point sample time, ts[j] is the IMU sample time
            int j = -1; // Change j

            if (j >= 0)
            {
                // Step 2: Find the linear interpolated pose (q_ti, p_ti)
                // Note: look up https://eigen.tuxfamily.org/dox/classEigen_1_1QuaternionBase.html#ac840bde67d22f2deca330561c65d144e

                // Step 3: Transform the point pi (which is in B_ti frame) to world frame. Assign the value to the variable po
                // Note: Eigen supports the operation Vector3d = Quaternion*Vector.

            }

        /* ASSIGNMENT BLOCK END -------------------------------------------------------------------------------------*/

        po.intensity = pi.intensity; po.t = pi.t; po.reflectivity = pi.reflectivity;
    }

    // Publish the pointcloud
    Util::publishCloud(imuPropDeskewedCloudPub, *cloudDeskewedInWorld, odom_W_Bstart->header.stamp, "world_shifted");
}

void processData()
{
    while(ros::ok())
    {
        // Check if there is data
        if(!hasData())
        {
            ROS_INFO_THROTTLE(1.0, "Waiting for data...");
            this_thread::sleep_for(chrono::milliseconds(50));
            continue;
        }

        OdomMsgPtr odom;
        CloudMsgPtr cloudMsg;
        // Pop the data
        { mylg lock(oc_mtx);
          std::tie(odom, cloudMsg) = oc_buf.front();
          oc_buf.pop_front(); }

        CloudOusterPtr cloud(new CloudOuster());
        pcl::fromROSMsg(*cloudMsg, *cloud);

        // Convert cloud to body frame
        pcl::transformPointCloud(*cloud, *cloud, tf_Bimu_Blidar.cast<float>().tfMat());

        double start_time = odom->header.stamp.toSec();
        double end_time = cloudMsg->header.stamp.toSec() + cloud->points.back().t/1.0e9;

        //for(unsigned int i = 1; i < imu_buf.size(); i++)
        //    ROS_ASSERT(imu_buf[i]->header.stamp.toSec() > imu_buf[i-1]->header.stamp.toSec());


        deque<ImuMsgPtr> imuSeq;
        {
            mylg lock(imu_mtx); 
            while (2 <= imu_buf.size() && msgTimestamp(imu_buf[1]) <= start_time)
                imu_buf.pop_front();

            for (const auto& sample : imu_buf)
            {
                imuSeq.push_back(sample);
                if (end_time < msgTimestamp(sample))
                    break;
            }
        }

        if ((imuSeq.size() < 2) || imuSeq.back()->header.stamp.toSec() < start_time) {
            ROS_WARN_THROTTLE(1.0,
                              ("Pointcloud timestamp outside of IMU buffer "
                               "window. Cloud: %.3f -> %.3f, IMU buffer: %.3f "
                               "-> %.3f, imu_buf.size() = %lu, imuSeq.size() = "
                               "%lu"),
                             start_time,
                             end_time,
                             imu_buf.front()->header.stamp.toSec(),
                             imu_buf.back()->header.stamp.toSec(),
                             imu_buf.size(),
                             imuSeq.size());
            continue;
        }

        // Check ordering consistency
        ROS_ASSERT(imuSeq.size() > 1);
        for(unsigned int i = 1; i < imuSeq.size(); i++)
            ROS_ASSERT(imuSeq[i]->header.stamp.toSec() > imuSeq[i-1]->header.stamp.toSec());

        // Write a report
        static int cloudCount = -1; cloudCount++;
        printf(("Count %3d, %3d. Odom: %.3f. "
                "Cloud: %.3f -> %.3f. "
                "Imu: %lu, %.3f -> %.3f. "
                "Buf: OC: %3lu. Imu: %lu\n"),
                cloudCount, cloudMsg->header.seq, odom->header.stamp.toSec(),
                start_time, end_time,
                imuSeq.size(), imuSeq.front()->header.stamp.toSec(), imuSeq.back()->header.stamp.toSec(),
                oc_buf.size(), imu_buf.size());
        int imuCount = -1; imuCount++;
        // for (auto &imuSample : imuSeq)
        //     printf("IMU %d. Time: %.3f\n", imuCount++, imuSample->header.stamp.toSec());        

        // Transform the pointcloud to world frame
        myTf tf_W_Blidar(*odom);
        CloudOusterPtr distortedCloudInW(new CloudOuster());
        pcl::transformPointCloud(*cloud, *distortedCloudInW, tf_W_Blidar.cast<float>().tfMat());

        // Publish the distorted pointcloud for vizualization
        Util::publishCloud(distortedCloudPub, *distortedCloudInW, ros::Time(start_time), "world");

        // Extract IMU measurements from buffer and interpolate at the ends
        vector<double> ts; vector<Vector3d> gyro, acce;
        ExtractImuData(ts, gyro, acce, start_time, end_time, imuSeq);

        // Propagate the pose estimate using IMU
        vector<Quaternd> q_W_Bs; vector<Vector3d> p_W_Bs, v_W_Bs;
        PropagateIMU(odom, ts, gyro, acce, q_W_Bs, p_W_Bs, v_W_Bs);
        // Report on the propagated pose
        for (int i = 0; i < ts.size(); i++)
        {
            myTf tf_W_Bs(q_W_Bs[i], p_W_Bs[i]);
            printf("IMU prop %2d. Time: %.3f. YPR: %8.3f, %8.3f, %8.3f. XYZ: %.3f, %.3f, %.3f.\n",
                    i, ts[i],
                    tf_W_Bs.yaw(), tf_W_Bs.pitch(), tf_W_Bs.roll(),
                    tf_W_Bs.pos.x(), tf_W_Bs.pos.y(), tf_W_Bs.pos.z());
        }
        
        // Deskew by IMU propagation
        DeskewByImuPropagation(cloud, odom, ts, q_W_Bs, p_W_Bs);
    }
}

int main(int argc, char **argv)
{
    ros::init(argc, argv, "oblam_deskew");
    ros::NodeHandle nh("~");
    nh_ptr = boost::make_shared<ros::NodeHandle>(nh);

    printf(KGRN "OBLAM Deskew Started\n" RESET);

    // Initialize a transform
    Matrix4d tfm_Bimu_Blidar;
    tfm_Bimu_Blidar << -1.0, 0,   0,  -0.006253,
                        0,  -1.0, 0,   0.011775,
                        0,   0,   1.0, 0.028535,
                        0,   0,   0,   1.000000;
    tf_Bimu_Blidar = myTf(tfm_Bimu_Blidar);

    // Subscribe to IMU topic
    ros::Subscriber imuSub = nh.subscribe("/os1_cloud_node/imu", 1000, imuCallback);

    // Subscribe to the odometry and pointcloud topics
    ros::Subscriber odomSub = nh.subscribe("/odometry/filtered", 100, odomCallback);
    ros::Subscriber cloudSub = nh.subscribe("/os1_cloud_node/points", 100, cloudCallback);

    // Advertise the pointclouds
    distortedCloudPub = nh.advertise<CloudMsg>("/distorted_cloud", 100);
    imuPropDeskewedCloudPub = nh.advertise<CloudMsg>("/imu_propagated_deskewed_cloud", 100);

    // Create synchronized callback of two topics
    //typedef sync_policies::ApproximateTime<OdomMsg, CloudMsg> MySyncPolicy;
    //Synchronizer<MySyncPolicy> sync(MySyncPolicy(sync_time),
    //                                odomSub, cloudSub);
    //sync.registerCallback(boost::bind(&odomCloudCallback, _1, _2));

    // Process the data
    thread processDataThread(processData); // For processing the image

    ros::spin();

    //ros::MultiThreadedSpinner spinner(0);
    //spinner.spin();

    ROS_ERROR("Reached end!");

    return 0;
}
