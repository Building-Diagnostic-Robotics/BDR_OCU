#ifndef TELEOP_WIDGET_HPP
#define TELEOP_WIDGET_HPP

#include <QWidget>
#include <QDockWidget>
#include <QPushButton>
#include <QLabel>
#include <QSlider>
#include <QCheckBox>
#include <QTimer>
#include <QKeyEvent>
#include <QGridLayout>
#include <QGroupBox>
#include <QFrame>

#include <rclcpp/rclcpp.hpp>
#include <geometry_msgs/msg/twist.hpp>
#include <std_msgs/msg/bool.hpp>
#include <std_srvs/srv/trigger.hpp>
#include <std_srvs/srv/set_bool.hpp>
#include <odrive_can/srv/axis_state.hpp>

namespace f2c_cpp {

/**
 * @brief Floating teleop widget for robot control with keyboard and buttons
 * 
 * Provides:
 * - WASD keyboard control for movement
 * - Motor arm/disarm buttons
 * - MPC autonomous toggle
 * - Map saving, video recording, rosbag toggle
 * - GPR line control
 * - Speed adjustment sliders
 */
class TeleopWidget : public QWidget {
    Q_OBJECT

public:
    explicit TeleopWidget(rclcpp::Node::SharedPtr node, QWidget* parent = nullptr);
    ~TeleopWidget() = default;
    
    // Enable/disable teleop keyboard capture
    void setTeleopEnabled(bool enabled);
    bool isTeleopEnabled() const { return teleop_enabled_; }
    
signals:
    void statusMessage(const QString& message);
    void mpcStateChanged(bool enabled);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onArmMotors();
    void onDisarmMotors();
    void onToggleMpc();
    void onSaveMap();
    void onStartRecording();
    void onStopRecording();
    void onToggleRosbag();
    void onToggleGprScan();
    void onGprLineUp();
    void onGprLineDown();
    void onLinearSpeedChanged(int value);
    void onAngularSpeedChanged(int value);
    void onTeleopToggled(bool checked);
    void publishCmdVel();
    void updateKeyDisplay();

private:
    void setupUI();
    void setupRosInterfaces();
    void updateButtonStates();
    void setKeyIndicator(QLabel* label, bool pressed);
    void setMotionKeyState(Qt::Key key, bool pressed);
    void requestAxisState(const QString& axis_name,
                          const rclcpp::Client<odrive_can::srv::AxisState>::SharedPtr& client,
                          int requested_state,
                          const QString& action_label);
    
    // ROS2 node (shared from main GUI)
    rclcpp::Node::SharedPtr node_;
    
    // Publishers
    rclcpp::Publisher<geometry_msgs::msg::Twist>::SharedPtr cmd_vel_pub_;
    rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr mpc_autonomy_pub_;
    
    // Service clients
    rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr save_map_client_;
    rclcpp::Client<std_srvs::srv::SetBool>::SharedPtr video_record_client_;
    rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr rosbag_toggle_client_;
    rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr gpr_scan_toggle_client_;
    rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr gpr_line_start_client_;
    rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr gpr_line_stop_client_;
    rclcpp::Client<odrive_can::srv::AxisState>::SharedPtr left_axis_client_;
    rclcpp::Client<odrive_can::srv::AxisState>::SharedPtr right_axis_client_;
    rclcpp::Client<odrive_can::srv::AxisState>::SharedPtr gpr_axis_client_;
    
    // Timer for publishing cmd_vel
    QTimer* cmd_vel_timer_;
    
    // UI elements
    QCheckBox* chk_teleop_enabled_;
    QPushButton* btn_arm_;
    QPushButton* btn_disarm_;
    QPushButton* btn_mpc_toggle_;
    QPushButton* btn_save_map_;
    QPushButton* btn_record_start_;
    QPushButton* btn_record_stop_;
    QPushButton* btn_rosbag_toggle_;
    QPushButton* btn_gpr_scan_;
    QPushButton* btn_gpr_up_;
    QPushButton* btn_gpr_down_;
    QSlider* slider_linear_;
    QSlider* slider_angular_;
    QLabel* lbl_linear_speed_;
    QLabel* lbl_angular_speed_;
    QLabel* lbl_status_;
    QLabel* lbl_focus_hint_;
    
    // Key indicators (WASD display)
    QLabel* lbl_key_w_;
    QLabel* lbl_key_a_;
    QLabel* lbl_key_s_;
    QLabel* lbl_key_d_;
    
    // State
    bool teleop_enabled_ = false;
    bool mpc_enabled_ = false;
    bool recording_active_ = false;
    bool rosbag_active_ = false;
    bool gpr_scan_active_ = false;
    
    // Key states for WASD
    bool key_w_ = false;
    bool key_a_ = false;
    bool key_s_ = false;
    bool key_d_ = false;
    
    // Speed settings
    double linear_speed_ = 0.4;   // m/s
    double angular_speed_ = 1.0;  // rad/s
    double max_linear_ = 1.5;
    double max_angular_ = 4.5;
    int publish_ticks_ = 0;
};

/**
 * @brief Dockable container for the TeleopWidget
 */
class TeleopDockWidget : public QDockWidget {
    Q_OBJECT

public:
    explicit TeleopDockWidget(rclcpp::Node::SharedPtr node, QWidget* parent = nullptr);
    
    TeleopWidget* teleopWidget() { return teleop_widget_; }

private:
    TeleopWidget* teleop_widget_;
};

} // namespace f2c_cpp

#endif // TELEOP_WIDGET_HPP
