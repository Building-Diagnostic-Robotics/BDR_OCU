#include "satellite_ros_link.hpp"

#include "satellite_palette.hpp"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>

#include <geometry_msgs/msg/twist.hpp>
#include <nav_msgs/msg/occupancy_grid.hpp>
#include <nav_msgs/msg/odometry.hpp>
#include <nav_msgs/msg/path.hpp>
#include <odrive_can/msg/controller_status.hpp>
#include <odrive_can/srv/axis_state.hpp>
#include <rcl_interfaces/msg/parameter.hpp>
#include <rcl_interfaces/srv/set_parameters.hpp>
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/bool.hpp>
#include <std_msgs/msg/string.hpp>
#include <std_srvs/srv/set_bool.hpp>
#include <std_srvs/srv/trigger.hpp>
#include <visualization_msgs/msg/marker_array.hpp>

#include <algorithm>
#include <cmath>

namespace f2c_cpp {

namespace {

double yawFromQuaternion(double qx, double qy, double qz, double qw) {
    const double siny_cosp = 2.0 * (qw * qz + qx * qy);
    const double cosy_cosp = 1.0 - 2.0 * (qy * qy + qz * qz);
    return std::atan2(siny_cosp, cosy_cosp);
}

/** JSON null / missing / non-string all read as empty. */
QString jsonString(const QJsonObject& obj, const char* key) {
    const QJsonValue v = obj.value(QLatin1String(key));
    return v.isString() ? v.toString() : QString();
}

double jsonNumber(const QJsonObject& obj, const char* key, double fallback) {
    const QJsonValue v = obj.value(QLatin1String(key));
    return v.isDouble() ? v.toDouble() : fallback;
}

}  // namespace

double CoverageStatus::coverageFraction() const {
    if (swept < 0.0 || remaining < 0.0) {
        return -1.0;
    }
    const double total = swept + remaining + std::max(0.0, deferred);
    return total > 0.0 ? swept / total : -1.0;
}

bool CoverageStatus::fresh(qint64 max_age_ms) const {
    return valid && wall_ms > 0 &&
           QDateTime::currentMSecsSinceEpoch() - wall_ms <= max_age_ms;
}

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
    rclcpp::Subscription<odrive_can::msg::ControllerStatus>::SharedPtr
        left_status_sub;
    rclcpp::Subscription<odrive_can::msg::ControllerStatus>::SharedPtr
        right_status_sub;
    rclcpp::Client<odrive_can::srv::AxisState>::SharedPtr left_axis;
    rclcpp::Client<odrive_can::srv::AxisState>::SharedPtr right_axis;
    rclcpp::Client<rcl_interfaces::srv::SetParameters>::SharedPtr
        coordinator_params;
    rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr dc_finalize_mission;
    rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr coverage_conclude;
    rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr coverage_skip_copy;
    rclcpp::Client<std_srvs::srv::SetBool>::SharedPtr coverage_abort;
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
        // Match the robot's state_qos (RELIABLE, VOLATILE, depth 1). A
        // one-shot default-QoS publish is easy for the coverage manager
        // to miss while it is still constructing; the screen latches.
        auto enable_qos = rclcpp::QoS(rclcpp::KeepLast(1)).reliable();
        impl_->autonomy_pub =
            impl_->node->create_publisher<std_msgs::msg::Bool>(
                "/mpc_autonomy_enable", enable_qos);

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
                    CoverageStatus s;
                    s.state = jsonString(obj, "state").toUpper();
                    s.phase = jsonString(obj, "phase");
                    s.mode = jsonString(obj, "mode");
                    s.stop = jsonString(obj, "stop");
                    s.stale = jsonString(obj, "stale");
                    s.dc = jsonString(obj, "dc");
                    s.copy = jsonString(obj, "copy");
                    s.copy_error = jsonString(obj, "copy_error");
                    s.error = jsonString(obj, "error");
                    s.autonomy = obj.value(QLatin1String("autonomy")).toBool();
                    s.initialized =
                        obj.value(QLatin1String("initialized")).toBool();
                    s.complete = obj.value(QLatin1String("complete")).toBool();
                    s.copy_waived =
                        obj.value(QLatin1String("copy_waived")).toBool();
                    s.waiting_revisit =
                        obj.value(QLatin1String("waiting_revisit")).toBool();
                    s.takeover = obj.value(QLatin1String("takeover")).toBool();
                    s.dc_paused =
                        obj.value(QLatin1String("dc_paused")).toBool();
                    s.revisit = int(jsonNumber(obj, "revisit", 0.0));
                    s.remaining = jsonNumber(obj, "remaining", -1.0);
                    s.swept = jsonNumber(obj, "swept", -1.0);
                    s.deferred = jsonNumber(obj, "deferred", -1.0);
                    const QJsonObject ready =
                        obj.value(QLatin1String("ready")).toObject();
                    for (auto it = ready.begin(); it != ready.end(); ++it) {
                        if (!it.value().toBool()) {
                            s.not_ready.append(it.key());
                        }
                    }
                    s.not_ready.sort();
                    s.wall_ms = QDateTime::currentMSecsSinceEpoch();
                    s.valid = true;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        status_ = s;
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

        // Real axis feedback, so Complete Mission can wait for the motors to
        // actually reach IDLE instead of assuming the request landed. Also
        // stamps the link monitor's ControllerStatus source.
        const auto status_qos = rclcpp::QoS(rclcpp::KeepLast(50)).reliable();
        impl_->left_status_sub =
            impl_->node->create_subscription<odrive_can::msg::ControllerStatus>(
                "/left/controller_status", status_qos,
                [this](odrive_can::msg::ControllerStatus::ConstSharedPtr msg) {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        motors_.left_axis_state = int(msg->axis_state);
                        motors_.left_wall_ms =
                            QDateTime::currentMSecsSinceEpoch();
                    }
                    emit motorStatusUpdated();
                });
        impl_->right_status_sub =
            impl_->node->create_subscription<odrive_can::msg::ControllerStatus>(
                "/right/controller_status", status_qos,
                [this](odrive_can::msg::ControllerStatus::ConstSharedPtr msg) {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        motors_.right_axis_state = int(msg->axis_state);
                        motors_.right_wall_ms =
                            QDateTime::currentMSecsSinceEpoch();
                    }
                    emit motorStatusUpdated();
                });

        impl_->left_axis =
            impl_->node->create_client<odrive_can::srv::AxisState>(
                "/left/request_axis_state");
        impl_->right_axis =
            impl_->node->create_client<odrive_can::srv::AxisState>(
                "/right/request_axis_state");
        impl_->coordinator_params =
            impl_->node->create_client<rcl_interfaces::srv::SetParameters>(
                "/data_collection_coordinator/set_parameters");
        impl_->dc_finalize_mission =
            impl_->node->create_client<std_srvs::srv::Trigger>(
                "/dc/finalize_mission");
        impl_->coverage_conclude =
            impl_->node->create_client<std_srvs::srv::Trigger>(
                "/coverage/conclude");
        impl_->coverage_skip_copy =
            impl_->node->create_client<std_srvs::srv::Trigger>(
                "/coverage/skip_copy");
        impl_->coverage_abort =
            impl_->node->create_client<std_srvs::srv::SetBool>(
                "/coverage/abort");
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

void RosLink::pushSessionMetadata(const QString& building_name,
                                  const QString& operator_name,
                                  const QString& units_preference,
                                  std::function<void(bool ok)> on_complete) {
    // Marshal every completion onto the GUI thread (this object's thread).
    const auto complete = [this, on_complete](bool ok) {
        if (!on_complete) {
            return;
        }
        QMetaObject::invokeMethod(
            this, [on_complete, ok]() { on_complete(ok); },
            Qt::QueuedConnection);
    };

    if (!running_ || !impl_->coordinator_params) {
        complete(false);
        return;
    }
    // Non-blocking readiness check — the caller retries while the robot
    // stack boots, so a not-yet-available service is a normal miss.
    if (!impl_->coordinator_params->service_is_ready()) {
        complete(false);
        return;
    }

    // Push raw strings — the coordinator owns the slugifier (see
    // sendDataCollectorSessionMetadata in app_shell.cpp for the rationale).
    const auto make_string_param = [](const char* name, const QString& value) {
        rcl_interfaces::msg::Parameter p;
        p.name = name;
        p.value.type = rcl_interfaces::msg::ParameterType::PARAMETER_STRING;
        p.value.string_value = value.toStdString();
        return p;
    };
    auto request =
        std::make_shared<rcl_interfaces::srv::SetParameters::Request>();
    request->parameters.reserve(3);
    request->parameters.push_back(
        make_string_param("building_name", building_name));
    request->parameters.push_back(
        make_string_param("operator_name", operator_name));
    request->parameters.push_back(
        make_string_param("units_preference", units_preference));

    impl_->coordinator_params->async_send_request(
        request,
        [complete](rclcpp::Client<rcl_interfaces::srv::SetParameters>::
                       SharedFuture future) {
            bool ok = false;
            try {
                const auto response = future.get();
                ok = response &&
                     response->results.size() == 3 &&
                     std::all_of(response->results.begin(),
                                 response->results.end(),
                                 [](const auto& r) { return r.successful; });
            } catch (const std::exception&) {
                ok = false;
            }
            complete(ok);
        });
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

MotorStatus RosLink::motorStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return motors_;
}

bool RosLink::motorsIdle() const {
    const MotorStatus motors = motorStatus();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // Stale status is not proof of IDLE — a dead CAN bus would otherwise read
    // as "disarmed" while the axes are still in closed loop.
    if (motors.left_wall_ms <= 0 || motors.right_wall_ms <= 0) {
        return false;
    }
    if (now - motors.left_wall_ms > kControllerStatusStaleMs ||
        now - motors.right_wall_ms > kControllerStatusStaleMs) {
        return false;
    }
    return motors.left_axis_state == kAxisIdle &&
           motors.right_axis_state == kAxisIdle;
}

namespace {

/**
 * Fire a std_srvs Trigger and marshal the outcome to `owner`'s thread.
 * Discovery may not have settled if the operator acts immediately after
 * launch: a brief wait, then give up rather than block the caller's flow.
 */
void callTrigger(QObject* owner,
                 rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr client,
                 const char* service_name, bool running,
                 RosLink::TriggerCallback on_done) {
    const auto complete = [owner, on_done](bool ok, const QString& detail) {
        if (!on_done) {
            return;
        }
        QMetaObject::invokeMethod(
            owner, [on_done, ok, detail]() { on_done(ok, detail); },
            Qt::QueuedConnection);
    };
    if (!running || !client) {
        complete(false, QStringLiteral("ROS link not running"));
        return;
    }
    if (!client->wait_for_service(std::chrono::milliseconds(250))) {
        complete(false, QStringLiteral("%1 unavailable")
                            .arg(QLatin1String(service_name)));
        return;
    }
    auto request = std::make_shared<std_srvs::srv::Trigger::Request>();
    (void)client->async_send_request(
        request,
        [complete](
            rclcpp::Client<std_srvs::srv::Trigger>::SharedFuture future) {
            try {
                auto result = future.get();
                complete(result && result->success,
                         result ? QString::fromStdString(result->message)
                                : QStringLiteral("null response"));
            } catch (const std::exception& exc) {
                complete(false, QString::fromUtf8(exc.what()));
            }
        });
}

}  // namespace

void RosLink::finalizeMission(
    std::function<void(bool ok, QString detail)> on_done) {
    callTrigger(this, impl_->dc_finalize_mission, "/dc/finalize_mission",
                running_, std::move(on_done));
}

void RosLink::concludeCoverage(TriggerCallback on_done) {
    callTrigger(this, impl_->coverage_conclude, "/coverage/conclude",
                running_, std::move(on_done));
}

void RosLink::skipCopy(TriggerCallback on_done) {
    callTrigger(this, impl_->coverage_skip_copy, "/coverage/skip_copy",
                running_, std::move(on_done));
}

void RosLink::abortCoverage(bool save, TriggerCallback on_done) {
    const auto complete = [this, on_done](bool ok, const QString& detail) {
        if (!on_done) {
            return;
        }
        QMetaObject::invokeMethod(
            this, [on_done, ok, detail]() { on_done(ok, detail); },
            Qt::QueuedConnection);
    };
    if (!running_ || !impl_->coverage_abort) {
        complete(false, QStringLiteral("ROS link not running"));
        return;
    }
    if (!impl_->coverage_abort->wait_for_service(
            std::chrono::milliseconds(250))) {
        complete(false, QStringLiteral("/coverage/abort unavailable"));
        return;
    }
    auto request = std::make_shared<std_srvs::srv::SetBool::Request>();
    request->data = save;
    (void)impl_->coverage_abort->async_send_request(
        request,
        [complete](
            rclcpp::Client<std_srvs::srv::SetBool>::SharedFuture future) {
            try {
                auto result = future.get();
                complete(result && result->success,
                         result ? QString::fromStdString(result->message)
                                : QStringLiteral("null response"));
            } catch (const std::exception& exc) {
                complete(false, QString::fromUtf8(exc.what()));
            }
        });
}

bool RosLink::directorServicesReady() const {
    return running_ && impl_->coverage_conclude &&
           impl_->coverage_conclude->service_is_ready();
}

bool RosLink::motorsArmed() const {
    const MotorStatus motors = motorStatus();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (motors.left_wall_ms <= 0 || motors.right_wall_ms <= 0) {
        return false;
    }
    if (now - motors.left_wall_ms > kControllerStatusStaleMs ||
        now - motors.right_wall_ms > kControllerStatusStaleMs) {
        return false;
    }
    return motors.left_axis_state == kAxisClosedLoop &&
           motors.right_axis_state == kAxisClosedLoop;
}

}  // namespace f2c_cpp
