#include "satellite_ros_link.hpp"

#include "satellite_palette.hpp"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>

#include <geometry_msgs/msg/twist.hpp>
#include <nav_msgs/msg/occupancy_grid.hpp>
#include <nav_msgs/msg/odometry.hpp>
#include <nav_msgs/msg/path.hpp>
#include <odrive_can/srv/axis_state.hpp>
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/bool.hpp>
#include <std_msgs/msg/string.hpp>
#include <visualization_msgs/msg/marker_array.hpp>

#include <cmath>

namespace f2c_cpp {

namespace {

double yawFromQuaternion(double qx, double qy, double qz, double qw) {
    const double siny_cosp = 2.0 * (qw * qz + qx * qy);
    const double cosy_cosp = 1.0 - 2.0 * (qy * qy + qz * qz);
    return std::atan2(siny_cosp, cosy_cosp);
}

QString modeName(double mode) {
    if (mode < 0.5) return QStringLiteral("IDLE");
    if (mode < 1.5) return QStringLiteral("TRACKING");
    if (mode < 2.5) return QStringLiteral("PIVOT");
    if (mode < 3.5) return QStringLiteral("COMPLETE");
    return QStringLiteral("BLOCKED");
}

}  // namespace

class RosLink::Impl {
public:
    rclcpp::Node::SharedPtr node;
    rclcpp::Publisher<geometry_msgs::msg::Twist>::SharedPtr cmd_vel_pub;
    rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr autonomy_pub;
    rclcpp::Subscription<nav_msgs::msg::OccupancyGrid>::SharedPtr grid_sub;
    rclcpp::Subscription<nav_msgs::msg::Path>::SharedPtr path_sub;
    rclcpp::Subscription<visualization_msgs::msg::MarkerArray>::SharedPtr swaths_sub;
    rclcpp::Subscription<nav_msgs::msg::Odometry>::SharedPtr odom_sub;
    rclcpp::Subscription<std_msgs::msg::String>::SharedPtr status_sub;
    rclcpp::Subscription<std_msgs::msg::String>::SharedPtr segment_sub;
    rclcpp::Client<odrive_can::srv::AxisState>::SharedPtr left_axis;
    rclcpp::Client<odrive_can::srv::AxisState>::SharedPtr right_axis;
};

RosLink::RosLink(QObject* parent) : QObject(parent), impl_(new Impl) {}

RosLink::~RosLink() { stop(); }

bool RosLink::start(QString* error) {
    if (running_) {
        return true;
    }
    try {
        if (!rclcpp::ok()) {
            rclcpp::init(0, nullptr);
        }
        impl_->node = rclcpp::Node::make_shared("bdr_satellite_ocu");

        impl_->cmd_vel_pub =
            impl_->node->create_publisher<geometry_msgs::msg::Twist>(
                "/cmd_vel", 10);
        impl_->autonomy_pub =
            impl_->node->create_publisher<std_msgs::msg::Bool>(
                "/mpc_autonomy_enable", 10);

        // Matches the manager's map_qos (BEST_EFFORT, VOLATILE, depth 1).
        auto grid_qos = rclcpp::QoS(1).best_effort().durability_volatile();
        impl_->grid_sub =
            impl_->node->create_subscription<nav_msgs::msg::OccupancyGrid>(
                "/coverage/global_occupancy", grid_qos,
                [this](nav_msgs::msg::OccupancyGrid::ConstSharedPtr msg) {
                    const int w = int(msg->info.width);
                    const int h = int(msg->info.height);
                    if (w <= 0 || h <= 0) {
                        return;
                    }
                    QImage img(w, h, QImage::Format_ARGB32);
                    const QRgb occupied =
                        qRgba(0xff, 0x6b, 0x6b, 190);         // danger red
                    const QRgb free_cell = qRgba(0, 179, 90, 42);  // faint accent
                    const QRgb unknown = qRgba(6, 8, 10, 96);
                    for (int row = 0; row < h; ++row) {
                        QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(row));
                        const int8_t* src = msg->data.data() + qint64(row) * w;
                        for (int col = 0; col < w; ++col) {
                            const int8_t v = src[col];
                            line[col] = v < 0 ? unknown
                                        : v >= 50 ? occupied
                                                  : free_cell;
                        }
                    }
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        grid_.image = img;
                        grid_.resolution = msg->info.resolution;
                        grid_.origin_body =
                            QPointF(msg->info.origin.position.x,
                                    msg->info.origin.position.y);
                        grid_.revision++;
                    }
                    emit gridUpdated();
                });

        // Matches visualization_qos (RELIABLE, TRANSIENT_LOCAL, depth 1).
        auto vis_qos = rclcpp::QoS(1).reliable().transient_local();
        impl_->path_sub =
            impl_->node->create_subscription<nav_msgs::msg::Path>(
                "/coverage/planned_path", vis_qos,
                [this](nav_msgs::msg::Path::ConstSharedPtr msg) {
                    QVector<QPointF> line;
                    line.reserve(int(msg->poses.size()));
                    for (const auto& pose : msg->poses) {
                        line.append(QPointF(pose.pose.position.x,
                                            pose.pose.position.y));
                    }
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        path_.lines = {line};
                        path_.colors = {satpal::info()};
                        path_.revision++;
                    }
                    emit pathUpdated();
                });

        impl_->swaths_sub =
            impl_->node
                ->create_subscription<visualization_msgs::msg::MarkerArray>(
                    "/coverage/planned_swaths", vis_qos,
                    [this](visualization_msgs::msg::MarkerArray::ConstSharedPtr
                               msg) {
                        QVector<QVector<QPointF>> lines;
                        QVector<QColor> colors;
                        for (const auto& marker : msg->markers) {
                            if (marker.action !=
                                    visualization_msgs::msg::Marker::ADD ||
                                marker.points.size() < 2) {
                                continue;
                            }
                            QVector<QPointF> line;
                            line.reserve(int(marker.points.size()));
                            for (const auto& p : marker.points) {
                                line.append(QPointF(p.x, p.y));
                            }
                            lines.append(line);
                            colors.append(QColor::fromRgbF(
                                marker.color.r, marker.color.g,
                                marker.color.b,
                                marker.color.a > 0.0f ? marker.color.a : 1.0f));
                        }
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            swaths_.lines = lines;
                            swaths_.colors = colors;
                            swaths_.revision++;
                        }
                        emit swathsUpdated();
                    });

        impl_->odom_sub =
            impl_->node->create_subscription<nav_msgs::msg::Odometry>(
                "/Odometry_tilt_corrected_diff", rclcpp::SensorDataQoS(),
                [this](nav_msgs::msg::Odometry::ConstSharedPtr msg) {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        odom_.x = msg->pose.pose.position.x;
                        odom_.y = msg->pose.pose.position.y;
                        odom_.yaw = yawFromQuaternion(
                            msg->pose.pose.orientation.x,
                            msg->pose.pose.orientation.y,
                            msg->pose.pose.orientation.z,
                            msg->pose.pose.orientation.w);
                        odom_.wall_ms =
                            QDateTime::currentMSecsSinceEpoch();
                        odom_.valid = true;
                    }
                    emit odomUpdated();
                });

        auto state_qos = rclcpp::QoS(1).reliable().durability_volatile();
        impl_->status_sub =
            impl_->node->create_subscription<std_msgs::msg::String>(
                "/coverage/status", state_qos,
                [this](std_msgs::msg::String::ConstSharedPtr msg) {
                    const QJsonDocument doc = QJsonDocument::fromJson(
                        QByteArray::fromStdString(msg->data));
                    if (!doc.isObject()) {
                        return;
                    }
                    const QJsonObject obj = doc.object();
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        status_.state = obj.value("state").toString();
                        status_.mode = obj.contains("mode_name")
                            ? obj.value("mode_name").toString()
                            : modeName(obj.value("mode").toDouble(0.0));
                        status_.reason = obj.value("reason").toString();
                        status_.coverage =
                            obj.value("coverage").toDouble(-1.0);
                        status_.wall_ms =
                            QDateTime::currentMSecsSinceEpoch();
                        status_.valid = true;
                    }
                    emit statusUpdated();
                });

        impl_->segment_sub =
            impl_->node->create_subscription<std_msgs::msg::String>(
                "/scan_segment_status", state_qos,
                [this](std_msgs::msg::String::ConstSharedPtr msg) {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        segment_status_ =
                            QString::fromStdString(msg->data);
                    }
                    emit segmentStatusUpdated();
                });

        impl_->left_axis =
            impl_->node->create_client<odrive_can::srv::AxisState>(
                "/left/request_axis_state");
        impl_->right_axis =
            impl_->node->create_client<odrive_can::srv::AxisState>(
                "/right/request_axis_state");
    } catch (const std::exception& exc) {
        if (error) {
            *error = QString::fromUtf8(exc.what());
        }
        impl_->node.reset();
        return false;
    }

    running_ = true;
    spin_thread_ = std::thread([this] { spinLoop(); });
    return true;
}

void RosLink::spinLoop() {
    rclcpp::executors::SingleThreadedExecutor executor;
    executor.add_node(impl_->node);
    while (running_ && rclcpp::ok()) {
        executor.spin_some(std::chrono::milliseconds(50));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void RosLink::stop() {
    if (!running_) {
        return;
    }
    running_ = false;
    if (spin_thread_.joinable()) {
        spin_thread_.join();
    }
    impl_->node.reset();
}

void RosLink::publishTwist(double linear, double angular) {
    if (!running_ || !impl_->cmd_vel_pub) {
        return;
    }
    geometry_msgs::msg::Twist msg;
    msg.linear.x = linear;
    msg.angular.z = angular;
    impl_->cmd_vel_pub->publish(msg);
}

void RosLink::publishAutonomyEnable(bool enabled) {
    if (!running_ || !impl_->autonomy_pub) {
        return;
    }
    std_msgs::msg::Bool msg;
    msg.data = enabled;
    impl_->autonomy_pub->publish(msg);
}

void RosLink::requestAxisState(int state) {
    if (!running_) {
        emit axisResult(false, QStringLiteral("ROS link not running"));
        return;
    }
    const auto send = [this, state](
                          rclcpp::Client<odrive_can::srv::AxisState>::SharedPtr
                              client,
                          const QString& side) {
        if (!client) {
            return;
        }
        if (!client->service_is_ready()) {
            emit axisResult(
                false, QStringLiteral("%1 axis service unavailable").arg(side));
            return;
        }
        auto request =
            std::make_shared<odrive_can::srv::AxisState::Request>();
        request->axis_requested_state = uint32_t(state);
        client->async_send_request(
            request,
            [this, side](rclcpp::Client<odrive_can::srv::AxisState>::SharedFuture
                             future) {
                try {
                    const auto result = future.get();
                    emit axisResult(
                        true, QStringLiteral("%1 axis state now %2")
                                  .arg(side)
                                  .arg(result->axis_state));
                } catch (const std::exception& exc) {
                    emit axisResult(false,
                                    QStringLiteral("%1 axis request failed: %2")
                                        .arg(side, exc.what()));
                }
            });
    };
    send(impl_->left_axis, QStringLiteral("left"));
    send(impl_->right_axis, QStringLiteral("right"));
}

GridSnapshot RosLink::gridSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return grid_;
}

PolylineSet RosLink::pathSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return path_;
}

PolylineSet RosLink::swathsSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return swaths_;
}

OdomSnapshot RosLink::odomSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return odom_;
}

CoverageStatus RosLink::coverageStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

QString RosLink::lastSegmentStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return segment_status_;
}

}  // namespace f2c_cpp
