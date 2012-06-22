#include "pathfollower.h"
#include <cmath>

#include <Eigen/Dense>
#include <visualization_msgs/Marker.h>
#include "exceptions.h"
#include "mapprocessor.h"

using namespace traversable_path;
using namespace Eigen;

PathFollower::PathFollower() :
    motion_control_action_client_("motion_control"),
    path_angle_(NAN),
    lock_goal_(false)
{
    subscribe_map_ = node_handle_.subscribe("traversability_map", 0, &PathFollower::mapCallback, this);
    publish_rviz_marker_ = node_handle_.advertise<visualization_msgs::Marker>("visualization_marker", 100);
    publish_goal_ = node_handle_.advertise<geometry_msgs::PoseStamped>("traversable_path/goal", 1);

    map_processor_ = new MapProcessor();

    // at the beginning there is no goal.
    current_goal_.is_set = false;

    // register reconfigure callback (which will also initialize config_ with the default values)
    reconfig_server_.setCallback(boost::bind(&PathFollower::dynamicReconfigureCallback, this, _1, _2));
}

PathFollower::~PathFollower()
{
    delete map_processor_;
}

void PathFollower::dynamicReconfigureCallback(const follow_pathConfig &config, uint32_t level)
{
    config_ = config;
    ROS_DEBUG("Reconfigure path follower.");
}

void PathFollower::mapCallback(const nav_msgs::OccupancyGridConstPtr &msg)
{
    map_ = msg;

    try {
        map_processor_->setMap(*msg);
        refreshAll();

        // distance of robot to path middle line
        Vector2f to_mid_line = vectorFromPointToLine(path_middle_line_, robot_pose_.position);
        // goal position (1m ahead):
        Vector2f goal_pos = robot_pose_.position + 0.8 * path_middle_line_.direction
                + to_mid_line;


        // check if goalpoint is traversable (and reachable) at all.

        // convert points to pixel coordinates of the map.
        Vector2i robot_on_map = transformToMap(robot_pose_.position);
        Vector2i goal_on_map = transformToMap(goal_pos);


        /////////////// MARKER
        {
            // point
            visualization_msgs::Marker points;
            points.header.frame_id = "/map";
            points.ns = "follow_path/goal";
            points.action = visualization_msgs::Marker::ADD;
            points.pose.orientation.w = 1.0;
            points.id = 1;
            points.type = visualization_msgs::Marker::POINTS;
            // POINTS markers use x and y scale for width/height respectively
            points.scale.x = 0.1;
            points.scale.y = 0.1;
            size_t index = transformToMapIndex(goal_pos);
            if (map_->data[index] != 0) {
                points.color.r = 1.0;
            } else {
                points.color.g = 1.0;
            }
            points.color.a = 1.0;
            geometry_msgs::Point p;
            p.x = goal_pos[0];
            p.y = goal_pos[1];
            points.points.push_back(p);
            publish_rviz_marker_.publish(points);
        }

        /** \todo auch ein bisschen den raum um das ziel checken */
        bool noObstacle = map_processor_->checkTraversabilityOfLine(robot_on_map, goal_on_map);
        if (noObstacle) {
            setGoalPoint(goal_pos, path_angle_);
        }
        else {
            ROS_INFO("OBSTACLE AHEAD!");
            /** \todo Handle this. Don't just stop. */

            // turn
            Vector2f goal_direction = findBestPathDirection();

            if (!goal_direction.isZero()) {
                float goal_angle = atan2(goal_direction[1], goal_direction[0]);
                goal_pos = robot_pose_.position + goal_direction;
                // Note: since findBestPathDirection() returned goal_direction and this method requires at least
                // 1m of free space to return an direction at all, there are no further traversability-checks
                // necessary.

                ROS_INFO("Turning.");
                /** \todo always forcing the goal where will likely lead to problems... Otherwise... does this problem still exists at all, when using a map? */

                setGoalPoint(goal_pos, goal_angle, true);
                // lock this goal until the robot reached it. Otherwise the robot will not turn well.
                lock_goal_ = true;
            } else {
                ROS_INFO("Stop moving.");
                /** \todo testen ob cancelAllGoals das gewünschte tut :) Wenn nicht setze neues Ziel mit v = 0 */
                motion_control_action_client_.cancelAllGoals();
                current_goal_.is_set = false;
            }
        }
    }
    catch (const TransformMapException &e) {
        ROS_WARN_THROTTLE(1, "%s", e.what());
    }
    catch (const Exception &e) {
        ROS_WARN_THROTTLE(1, "Exception in mapCallback(): %s", e.what());
    }
}

void PathFollower::motionControlDoneCallback(const actionlib::SimpleClientGoalState &state,
                                             const motion_control::MotionResultConstPtr &result)
{
    switch (result->status) {
    case motion_control::MotionResult::MOTION_STATUS_SUCCESS:
        ROS_INFO_NAMED("motion_control", "Reached Goal");
        break;
    case motion_control::MotionResult::MOTION_STATUS_COLLISION:
        ROS_INFO_NAMED("motion_control", "Collision!");
        break;
    default:
        ROS_INFO_NAMED("motion_control", "fail code=%d",result->status);
        break;
    }

    // unlock goal.
    lock_goal_ = false;
    current_goal_.is_set = false;
}

void PathFollower::motionControlFeedbackCallback(const motion_control::MotionFeedbackConstPtr &feedback)
{
    ROS_INFO_NAMED("motion_control", "Distance to goal: %f",feedback->dist_goal);
}


void PathFollower::publishGoalMarker(Vector2f position, float theta) const
{
    visualization_msgs::Marker marker;
    marker.header.frame_id = "/map";
    marker.pose.position = vectorToPoint(position);
    marker.pose.orientation = tf::createQuaternionMsgFromYaw(theta);

    // Set the namespace and id for this marker.  This serves to create a unique ID
    // Any marker sent with the same namespace and id will overwrite the old one
    marker.ns = "follow_path";
    marker.id = 0;

    // Set the marker type.
    marker.type = visualization_msgs::Marker::ARROW;

    // Set the marker action.  Options are ADD and DELETE
    marker.action = visualization_msgs::Marker::ADD;

    // Set the scale of the marker
    marker.scale.x = 0.8;
    marker.scale.y = 0.8;
    marker.scale.z = 0.8;

    // Set the color -- be sure to set alpha to something non-zero!
    marker.color.r = 1.0f;
    marker.color.g = 1.0f;
    marker.color.b = 0.0f;
    marker.color.a = 1.0f;

    marker.lifetime = ros::Duration();

    // Publish the marker
    publish_rviz_marker_.publish(marker);
}

void PathFollower::publishLineMarker(Eigen::Vector2f p1, Eigen::Vector2f p2, int id, std_msgs::ColorRGBA color) const
{
    visualization_msgs::Marker line;

    line.header.frame_id = "/map";
    line.ns = "follow_path/lines";
    line.action = visualization_msgs::Marker::ADD;
    line.pose.orientation.w = 1.0;
    line.id = id;
    line.type = visualization_msgs::Marker::LINE_STRIP;

    // LINE_STRIP/LINE_LIST markers use only the x component of scale, for the line width
    line.scale.x = 0.03;
    line.color = color;

    // set the points
    line.points.push_back(vectorToPoint(p1));
    line.points.push_back(vectorToPoint(p2));

    publish_rviz_marker_.publish(line);
}

void PathFollower::publishArrowMarker(Eigen::Vector2f point, float angle, int id, std_msgs::ColorRGBA color) const
{
    visualization_msgs::Marker marker;
    marker.header.frame_id = "/map";
    marker.pose.position = vectorToPoint(point);
    marker.pose.orientation = tf::createQuaternionMsgFromYaw(angle);
    marker.ns = "follow_path/arrow";
    marker.id = id;
    marker.type = visualization_msgs::Marker::ARROW;
    marker.action = visualization_msgs::Marker::ADD;
    marker.scale.x = marker.scale.y = marker.scale.z = 1.0;
    marker.color = color;

    publish_rviz_marker_.publish(marker);
}

void PathFollower::publishArrowMarker(Eigen::Vector2f point, Eigen::Vector2f direction, int id, std_msgs::ColorRGBA color) const
{
    float angle = atan2(direction[1], direction[0]);
    publishArrowMarker(point, angle, id, color);
}

void PathFollower::setGoalPoint(Vector2f position, float theta, bool force)
{
    // don't set new goal, if the current goal is locked.
    if (lock_goal_) {
        return;
    }

    const float MIN_DISTANCE_BETWEEN_GOALS = 0.5;

    // make sure the min. distance doesn't avoid the goal to be set at the first call of this method.
    float distance = INFINITY;
    if (!current_goal_.is_set) {
        distance = (position - current_goal_.goal).norm();
    }

    if (distance > MIN_DISTANCE_BETWEEN_GOALS || force) {
        // send goal to motion_control
        motion_control::MotionGoal goal;
        goal.v     = config_.velocity;
        goal.beta  = 0;
        //goal.pos_tolerance = 0.1;
        goal.mode  = motion_control::MotionGoal::MOTION_TO_GOAL;

        goal.x     = position[0];
        goal.y     = position[1];
        goal.theta = theta;

        // send goal to motion_control
        motion_control_action_client_.sendGoal(goal, boost::bind(&PathFollower::motionControlDoneCallback,this,_1,_2),
                                               boost::function<void () >(), // do not set an active callback
                                               boost::bind(&PathFollower::motionControlFeedbackCallback,this,_1));

        // set as current goal
        current_goal_.is_set = true;
        current_goal_.goal = position;
        // send goal-marker to rviz for debugging
        publishGoalMarker(position, theta);


        ROS_DEBUG_STREAM("Send goal" << (force ? " (forced): " : ": ") << position << "\ntheta: " << theta);
    }
    else {
        ROS_DEBUG_THROTTLE(0.5, "Didn't update goal. New goal is %.2f m distant from the current goal. Minimum distance is %f",
                  distance, MIN_DISTANCE_BETWEEN_GOALS);
    }
}

void PathFollower::refreshAll()
{
    // do not change the order of the following method calls!
    refreshRobotPose();
    refreshPathLine();
    refreshPathDirectionAngle();
}

void PathFollower::refreshRobotPose()
{
    try {
        // position/orientation of the robot
        tf::StampedTransform robot_pose;
        tf_listener_.lookupTransform("/map", "/base_link", ros::Time(0), robot_pose);
        // position
        robot_pose_.position[0] = robot_pose.getOrigin().getX();
        robot_pose_.position[1] = robot_pose.getOrigin().getY();
        // orientation
        btVector3 tmp(1,0,0);
        tmp = tmp.rotate(robot_pose.getRotation().getAxis(), robot_pose.getRotation().getAngle());
        robot_pose_.orientation[0] = tmp.getX();
        robot_pose_.orientation[1] = tmp.getY();
    }
    catch (tf::TransformException e) {
        ROS_WARN_THROTTLE_NAMED(1, "tf", "tf::TransformException in %s (line %d):\n%s", __FILE__, __LINE__, e.what());
        throw Exception("refreshRobotPose failed.");
    }
}

void PathFollower::refreshPathLine()
{
    vectorVector2f points_middle;
    findPathMiddlePoints(&points_middle);

    // need at least two points, otherwise the regression will fail.
    if (points_middle.size() < 2) {
        throw Exception("Missing path points");
    }

    // fit a line to the points
    Line new_line;
    fitLinear(points_middle, &new_line);

    // make sure the direction vector of the line points in the direction that is nearer to the robot orientation.
    float angle = acos( new_line.direction.dot(robot_pose_.orientation)
                        / (new_line.direction.norm() * robot_pose_.orientation.norm()) );
    // if angle is greater than 90°, invert direction.
    if (angle > M_PI/2) {
        new_line.direction *= -1;
    }

    // filter line
    if (path_middle_line_.direction.isZero()) {
        path_middle_line_ = new_line;
//        ROS_DEBUG_STREAM("direction unfiltered: " << path_middle_line_.direction);
    } else {
        // soundness of < 0.1 is a good line, > 0.1 is rather bad.
        // Make the weight of the new value higher if soundness is better.
        float filter_factor = 0.8;
        filter_factor += new_line.soundness;
        filter_factor = filter_factor > 0.95 ? 0.95 : filter_factor;

        path_middle_line_.point     = filter_factor * path_middle_line_.point + (1-filter_factor) * new_line.point;
        path_middle_line_.direction = filter_factor * path_middle_line_.direction
                                      + (1-filter_factor) * new_line.direction;
        path_middle_line_.direction.normalize();

        /** \todo is the normal used at all? maybe this can be droped. */
        path_middle_line_.normal[0] = - path_middle_line_.direction[1];
        path_middle_line_.normal[1] = path_middle_line_.direction[0];

//        ROS_DEBUG_STREAM("direction: " << new_line.direction);
//        ROS_DEBUG_STREAM("direction filtered: " << path_middle_line_.direction);
    }

    /////////////// MARKER
    {
        // points
        visualization_msgs::Marker points;
        points.header.frame_id = "/map";
        points.ns = "follow_path/mid_points";
        points.action = visualization_msgs::Marker::ADD;
        points.pose.orientation.w = 1.0;
        points.id = 1;
        points.type = visualization_msgs::Marker::POINTS;
        // POINTS markers use x and y scale for width/height respectively
        points.scale.x = 0.05;
        points.scale.y = 0.05;
        // Points are cyan
        points.color.g = 1.0;
        points.color.b = 1.0;
        points.color.a = 1.0;
        for (vectorVector2f::iterator it = points_middle.begin(); it != points_middle.end(); ++it) {
            geometry_msgs::Point p;
            p.x = (*it)[0];
            p.y = (*it)[1];
            p.z = 0;
            points.points.push_back(p);
        }
        publish_rviz_marker_.publish(points);

        // line
        std_msgs::ColorRGBA color;
        color.r = 1.0; color.g = 0.5; color.a = 1.0; // orange
        Vector2f p1, p2;
        p1 = path_middle_line_.point + 5 * path_middle_line_.direction;
        p2 = path_middle_line_.point - 5 * path_middle_line_.direction;
        publishLineMarker(p1, p2, 13, color);

        // line
        color.r = 1.0; color.g = 0.0; color.a = 1.0; //red
        p1 = new_line.point + 3 * new_line.direction;
        p2 = new_line.point - 3 * new_line.direction;
        publishLineMarker(p1, p2, 14, color);
    }
}

void PathFollower::refreshPathDirectionAngle()
{
    // calculate angle
    path_angle_ = atan2(path_middle_line_.direction[1], path_middle_line_.direction[0]);
}

bool PathFollower::findPathMiddlePoints(PathFollower::vectorVector2f *out) const
{
    ROS_ASSERT(out != 0);
    // Work in frame /map

    // Orthogonal vector of robot direction: (x,y) -> (-y,x)
    // Points left of the direction (should be the y-axis)
    Vector2f orthogonal;
    orthogonal[0] = - robot_pose_.orientation[1];
    orthogonal[1] = robot_pose_.orientation[0];


    /* *** get path middle points *** */
    //! Size of the steps when looking for the edge points.
    /** Using map resolution gives the greates possible step size which ensures that we miss no cell. */
    float step_size = map_->info.resolution;

    // go forward
    const float DISTANCE_BEHIND_ROBOT = 1.0; /** \todo experimentiere mit kleineren werten */
    const float DISTANCE_IN_FRONT_OF_ROBOT = 1.0;

    //! Position of the current forward step.
    Vector2f forward_pos = robot_pose_.position - DISTANCE_BEHIND_ROBOT*robot_pose_.orientation;
    for (float forward = - DISTANCE_BEHIND_ROBOT; forward < DISTANCE_IN_FRONT_OF_ROBOT; forward += 3*step_size) {
        /** \todo better exception handling here? */
        forward_pos += robot_pose_.orientation * 3*step_size;

        // break, if obstacle is in front.
        try {
            if (map_->data[transformToMapIndex(forward_pos)] != 0) {
                if (forward < 0) {
                    // there is an obstacle between the points and the robot. Drop all this points.
                    out->clear();
                    continue;
                } else {
                    // Obstacle in front of the robot. Do not go any futher.
                    break;
                }
            }
        } catch (TransformMapException e) {
            ROS_WARN_THROTTLE(1, "Ahead: %s", e.what());
            continue;
        }

        Vector2f left_edge = forward_pos, right_edge = forward_pos;
        // find left edge
        try {
            do {
                left_edge += orthogonal * step_size;
            }
            while( map_->data[transformToMapIndex(left_edge)] == 0 );
        } catch (TransformMapException e) {
            // Do nothing here. Reaching the end of the map will then be handled as if there is an obstacle.
        }

        // find right edge
        try {
            do {
                right_edge += -orthogonal * step_size;
            }
            while( map_->data[transformToMapIndex(right_edge)] == 0 );
        } catch (TransformMapException e) {
            // Do nothing here. Reaching the end of the map will then be handled as if there is an obstacle.
        }

        // middle of this points
        Vector2f middle_point = (left_edge + right_edge) / 2;
        out->push_back(middle_point);
    }

    // drop last point for it might disturb the line
    if (out->size()) {
        out->pop_back();
    }

    return true;
}

Eigen::Vector2f PathFollower::findBestPathDirection() const
{
    /*
     * Description of how this method works:
     *  - Starting from the Robot, go cell by cell forward, until there is an untraversable cell.
     *  - Do this in all directions (increasing the angle alpha stepwise about ANGLE_INCREMENT).
     *  - Calcuate the distance d(alpha) from robot to obstacle.
     *  - Multiply distance with an weight w(alpha) that depends on the angle (to make turns about 180 degree less
     *    likely). This gives us the value v(alpha) = w(alpha)*d(alpha)
     *  - The angle of the direction which will be assumed as best is then determined by argmax_alpha( d(alpha) )
     *
     * Additionally there is an minimum distance for an direction to be used and the maximum distance at which
     * obstacles are searched is limited (see constants below).
     */

    //! Minimum distance at which an direction is assessed as drivable.
    const float MIN_FREE_DISTANCE = 1.0;
    //! Maximum distance to search for obstacles.
    /**
     * If there is no obstacle within this distance, be happy and do not look further.
     * This is necessary since the search is done with the cv::LineIterator which needs a fixed end point of the line.
     * Anyway, since the range of the scanner is limited and search that goes futher would only find the way back to
     * where the robot comes from.
     */
    const float MAX_SEARCHING_DISTANCE = 3.0;
    //! Increment of the angle when searching for the path direction.
    /** Note: Be sure that 2*pi % ANGLE_INCREMENT == 0 */
    const double ANGLE_INCREMENT = M_PI / 9.0; // = 20°


    ////////// MARKER
    visualization_msgs::Marker line_marker;
    line_marker.header.frame_id = "/map";
    line_marker.ns = "follow_path/findBestPathDirection";
    line_marker.action = visualization_msgs::Marker::ADD;
    line_marker.pose.orientation.w = 1.0;
    line_marker.id = 0;
    line_marker.type = visualization_msgs::Marker::LINE_LIST;
    // LINE_STRIP/LINE_LIST markers use only the x component of scale, for the line width
    line_marker.scale.x = 0.02;
    line_marker.color.a = 0.3;
    line_marker.color.r = 1.0;
    line_marker.color.b = 1.0;
    line_marker.lifetime = ros::Duration(1.0);
    ////////// END


    // transform position of the robot to the map.
    const Vector2i robot_pos_on_map = transformToMap(robot_pose_.position);

    /*
     * Rotate vector v about alpha degrees:
     *   v' = Rot_z(alpha)*v = [cos(alpha), -sin(alpha); sin(alpha), cos(alpha)] * v
     *
     * Let v = robot_pose_.orientation to rotate around the robot.
     */

    Vector2f bestDirection(0,0);
    double bestDirectionValue = -INFINITY;

    for (double alpha = -M_PI; alpha < M_PI; alpha += ANGLE_INCREMENT) {
        //ROS_DEBUG("alpha = %.0f", alpha*180/M_PI);

        // rotate the direction vector of the robot to get the direction of this iteration step.
        Matrix2f rotation;
        rotation << cos(alpha), -sin(alpha),
                    sin(alpha), cos(alpha);
        // direction to check in this iteration. Note that since robot_pose_.orientation is normalized, direction will
        // be normalized to.
        Vector2f direction = rotation * robot_pose_.orientation;
        Vector2f end_of_line = robot_pose_.position + MAX_SEARCHING_DISTANCE * direction;

        // Using the direction, get the end point of the line to check.
        /** \todo handle exceptions? */
        Vector2i line_end;
        try {
            line_end = transformToMap(end_of_line);
        } catch (...) {
            // This should never happen, as long as MAX_SEARCHING_DISTANCE, map size and map move rate are well aligned.
            ROS_DEBUG("findBestPathDirection - MEEEP");
            continue;
        }

        //ROS_DEBUG_STREAM("Robot: " << robot_pos_on_map << "\nLine end: " << line_end);

        // check the distance to the first untraversable cell
        cv::LineIterator line_it = map_processor_->getLineIterator(robot_pos_on_map, line_end);
        for (int i = 0; i < line_it.count; ++i, ++line_it) {
            if (*((uchar*) *line_it) == 0) {
                end_of_line = Vector2f(map_->info.origin.position.x, map_->info.origin.position.y) +
                        Vector2f(line_it.pos().x, line_it.pos().y)*map_->info.resolution;

                break;
            }
        }

        ////////// MARKER
        line_marker.points.push_back(vectorToPoint(robot_pose_.position));
        line_marker.points.push_back(vectorToPoint(end_of_line));
        ////////// END

        // distance of the robot to the obstacle
        float distance_to_robot = (robot_pose_.position - end_of_line).norm();
        //ROS_DEBUG("Distant of obstacle to robot: %f m", distance_to_robot);

        if (distance_to_robot > MIN_FREE_DISTANCE) {
            // calculate the "value" of this vector
            float value = helperAngleWeight(alpha) * distance_to_robot;
            if (value > bestDirectionValue) {
                bestDirection = direction;
                bestDirectionValue = value;
            }

            //ROS_DEBUG("Weight: %f", helperAngleWeight(alpha));
            //ROS_DEBUG("Value: %f", value);
        }
    }

    ////////// MARKER
    publish_rviz_marker_.publish(line_marker);

    return bestDirection;
}

float PathFollower::helperAngleWeight(float angle)
{
    angle = fabs(angle);
    ROS_ASSERT_MSG(angle >= 0 && angle <= M_PI+0.1, "Angle %f is out of range", angle*180/M_PI);

    if (angle < M_PI/4.0) { // 0-45°
        return 0.7;
    }
    else if (angle < 5.0/9.0*M_PI) { // 45-100°
        return 1.0;
    }
    else if (angle < 2.0/3.0*M_PI) { // 100-120°
        return 0.7;
    } else {
        /** \todo this weight should depend on MAX_SEARCHING_DISTANCE */
        return 0.3;
    }
}

Vector2i PathFollower::transformToMap(Vector2f point) const
{
    Vector2i result;
    result[0] = (int) ((point[0] - map_->info.origin.position.x) / map_->info.resolution);
    result[1] = (int) ((point[1] - map_->info.origin.position.y) / map_->info.resolution);

    // cast map width/height from uint to int here, to avoid warnings. There will be no overflow problems since the
    // values will much less than 2*10^9.
    if (result[0] < 0 || result[0] >= (int)map_->info.width || result[1] < 0 || result[1] >= (int)map_->info.height) {
        throw TransformMapException();
    }

    return result;
}

size_t PathFollower::transformToMapIndex(Vector2f point) const
{
    Vector2i pixel = transformToMap(point);

    return pixel[1] * map_->info.width + pixel[0];
}

void PathFollower::fitLinear(const PathFollower::vectorVector2f &points, PathFollower::Line *result)
{
    /*
     * This is a modified version of the function fitHyperplane() of eigen3 (eigen2support/LeastSquares.h),
     * which seems to be deprecated and thus is not used here directly.
     */
    // need at least two points.
    ROS_ASSERT(points.size() > 1);
    ROS_ASSERT(result != 0);

    // compute the mean of the data
    Vector2f mean = Vector2f::Zero(2);
    for (size_t i = 0; i < points.size(); ++i) {
        mean += points[i];
    }
    mean /= points.size();

    // compute the covariance matrix
    Matrix2f covMat = Matrix2f::Zero(2,2);
    for (size_t i = 0; i < points.size(); ++i) {
        Vector2f diff = (points[i] - mean).conjugate();
        covMat += diff * diff.adjoint();
    }

    // now we just have to pick the eigen vector with largest eigen value
    SelfAdjointEigenSolver<Matrix2f> eig(covMat);
    result->normal    = eig.eigenvectors().col(0); // eigen vector with smallest eigen value (= normal)
    result->direction = eig.eigenvectors().col(1); // eigen vector with largest eigen value (= direction of the line)

    result->point = mean;
    result->soundness = eig.eigenvalues().coeff(0)/eig.eigenvalues().coeff(1);
}

Vector2f PathFollower::vectorFromPointToLine(const PathFollower::Line &line, const Vector2f &point) const
{
    float k = (point.dot(line.direction) - line.point.dot(line.direction)) / line.direction.dot(line.direction);
    Vector2f point_to_line = -(point - line.point - k*line.direction);

    /////////////// MARKER
    /*
    // arrow
    visualization_msgs::Marker direction_marker;
    direction_marker.header.frame_id = "/map";
    direction_marker.pose.orientation = tf::createQuaternionMsgFromYaw( atan2(point_to_line[1], point_to_line[0]) );
    direction_marker.pose.position.x = robot_pose_.position[0];
    direction_marker.pose.position.y = robot_pose_.position[1];
    direction_marker.pose.position.z = 0;
    direction_marker.ns = "follow_path/direction";
    direction_marker.id = 19;
    direction_marker.type = visualization_msgs::Marker::ARROW;
    direction_marker.action = visualization_msgs::Marker::ADD;
    direction_marker.scale.x = direction_marker.scale.y = direction_marker.scale.z = point_to_line.norm();
    direction_marker.color.b = 1.0;
    direction_marker.color.a = 1.0;
    publish_rviz_marker_.publish(direction_marker);
    /////////////////
    */

    return point_to_line;
}

geometry_msgs::Point PathFollower::vectorToPoint(Vector2f v)
{
    geometry_msgs::Point p;
    p.x = v[0];
    p.y = v[1];
    p.z = 0;

    return p;
}


//--------------------------------------------------------------------------

int main(int argc, char **argv)
{
    ros::init(argc, argv, "follow_path");

    PathFollower follower;

    // main loop
    ros::spin();
    return 0;
}
