#include "teleop_widget.hpp"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QApplication>

namespace f2c_cpp {

// =============================================================================
// TeleopWidget Implementation
// =============================================================================

TeleopWidget::TeleopWidget(rclcpp::Node::SharedPtr node, QWidget* parent)
    : QWidget(parent)
    , node_(node)
{
    setupUI();
    setupRosInterfaces();
    
    // Timer for publishing cmd_vel at 10Hz
    cmd_vel_timer_ = new QTimer(this);
    connect(cmd_vel_timer_, &QTimer::timeout, this, &TeleopWidget::publishCmdVel);
    
    // Set focus policy to receive keyboard events
    setFocusPolicy(Qt::StrongFocus);
    
    updateButtonStates();
}

void TeleopWidget::setupUI() {
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(8);
    mainLayout->setContentsMargins(10, 10, 10, 10);
    
    // Title and enable checkbox
    QHBoxLayout* headerLayout = new QHBoxLayout();
    QLabel* title = new QLabel("🎮 Robot Teleop");
    title->setStyleSheet("font-size: 14px; font-weight: bold;");
    headerLayout->addWidget(title);
    headerLayout->addStretch();
    
    chk_teleop_enabled_ = new QCheckBox("Enable");
    chk_teleop_enabled_->setToolTip("Enable keyboard control (WASD)");
    connect(chk_teleop_enabled_, &QCheckBox::toggled, this, &TeleopWidget::onTeleopToggled);
    headerLayout->addWidget(chk_teleop_enabled_);
    mainLayout->addLayout(headerLayout);
    
    // Focus hint
    lbl_focus_hint_ = new QLabel("Click here and check 'Enable' for keyboard control");
    lbl_focus_hint_->setStyleSheet("color: #888; font-size: 10px; font-style: italic;");
    lbl_focus_hint_->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(lbl_focus_hint_);
    
    // WASD key display
    QGroupBox* wasdGroup = new QGroupBox("Movement (WASD)");
    QGridLayout* wasdLayout = new QGridLayout(wasdGroup);
    wasdLayout->setSpacing(4);
    
    auto createKeyLabel = [](const QString& text) {
        QLabel* lbl = new QLabel(text);
        lbl->setFixedSize(36, 36);
        lbl->setAlignment(Qt::AlignCenter);
        lbl->setStyleSheet(
            "QLabel { background: #444; color: #aaa; border: 2px solid #555; "
            "border-radius: 4px; font-weight: bold; font-size: 14px; }"
        );
        return lbl;
    };
    
    lbl_key_w_ = createKeyLabel("W");
    lbl_key_a_ = createKeyLabel("A");
    lbl_key_s_ = createKeyLabel("S");
    lbl_key_d_ = createKeyLabel("D");
    
    wasdLayout->addWidget(lbl_key_w_, 0, 1, Qt::AlignCenter);
    wasdLayout->addWidget(lbl_key_a_, 1, 0, Qt::AlignCenter);
    wasdLayout->addWidget(lbl_key_s_, 1, 1, Qt::AlignCenter);
    wasdLayout->addWidget(lbl_key_d_, 1, 2, Qt::AlignCenter);
    
    // Add labels
    QLabel* lblFwd = new QLabel("↑");
    lblFwd->setAlignment(Qt::AlignCenter);
    wasdLayout->addWidget(lblFwd, 0, 2);
    QLabel* lblBwd = new QLabel("↓");
    lblBwd->setAlignment(Qt::AlignCenter);
    wasdLayout->addWidget(lblBwd, 1, 3);
    
    mainLayout->addWidget(wasdGroup);
    
    // Speed sliders
    QGroupBox* speedGroup = new QGroupBox("Speed");
    QGridLayout* speedLayout = new QGridLayout(speedGroup);
    
    speedLayout->addWidget(new QLabel("Linear:"), 0, 0);
    slider_linear_ = new QSlider(Qt::Horizontal);
    slider_linear_->setRange(10, 150);  // 0.1 to 1.5 m/s
    slider_linear_->setValue(40);
    connect(slider_linear_, &QSlider::valueChanged, this, &TeleopWidget::onLinearSpeedChanged);
    speedLayout->addWidget(slider_linear_, 0, 1);
    lbl_linear_speed_ = new QLabel("0.4 m/s");
    lbl_linear_speed_->setFixedWidth(60);
    speedLayout->addWidget(lbl_linear_speed_, 0, 2);
    
    speedLayout->addWidget(new QLabel("Angular:"), 1, 0);
    slider_angular_ = new QSlider(Qt::Horizontal);
    slider_angular_->setRange(10, 450);  // 0.1 to 4.5 rad/s
    slider_angular_->setValue(100);
    connect(slider_angular_, &QSlider::valueChanged, this, &TeleopWidget::onAngularSpeedChanged);
    speedLayout->addWidget(slider_angular_, 1, 1);
    lbl_angular_speed_ = new QLabel("1.0 rad/s");
    lbl_angular_speed_->setFixedWidth(60);
    speedLayout->addWidget(lbl_angular_speed_, 1, 2);
    
    mainLayout->addWidget(speedGroup);
    
    // Motor controls
    QGroupBox* motorGroup = new QGroupBox("Motors");
    QHBoxLayout* motorLayout = new QHBoxLayout(motorGroup);
    
    btn_arm_ = new QPushButton("🔋 Arm (E)");
    btn_arm_->setToolTip("Arm motors - enable closed loop control");
    connect(btn_arm_, &QPushButton::clicked, this, &TeleopWidget::onArmMotors);
    motorLayout->addWidget(btn_arm_);
    
    btn_disarm_ = new QPushButton("⏹ Disarm (Q)");
    btn_disarm_->setToolTip("Disarm motors - set to idle");
    connect(btn_disarm_, &QPushButton::clicked, this, &TeleopWidget::onDisarmMotors);
    motorLayout->addWidget(btn_disarm_);
    
    mainLayout->addWidget(motorGroup);
    
    // MPC control
    QGroupBox* mpcGroup = new QGroupBox("Autonomous");
    QHBoxLayout* mpcLayout = new QHBoxLayout(mpcGroup);
    
    btn_mpc_toggle_ = new QPushButton("🤖 Enable MPC (X)");
    btn_mpc_toggle_->setCheckable(true);
    btn_mpc_toggle_->setToolTip("Toggle MPC autonomous control\nWhen enabled, WASD is disabled");
    connect(btn_mpc_toggle_, &QPushButton::clicked, this, &TeleopWidget::onToggleMpc);
    mpcLayout->addWidget(btn_mpc_toggle_);
    
    mainLayout->addWidget(mpcGroup);
    
    // Data collection controls
    QGroupBox* dataGroup = new QGroupBox("Data Collection");
    QGridLayout* dataLayout = new QGridLayout(dataGroup);
    
    btn_save_map_ = new QPushButton("💾 Save Map (M)");
    btn_save_map_->setToolTip("Save map checkpoint from Fast-LIO2");
    connect(btn_save_map_, &QPushButton::clicked, this, &TeleopWidget::onSaveMap);
    dataLayout->addWidget(btn_save_map_, 0, 0, 1, 2);
    
    btn_record_start_ = new QPushButton("⏺ Record (R)");
    btn_record_start_->setToolTip("Start video recording");
    connect(btn_record_start_, &QPushButton::clicked, this, &TeleopWidget::onStartRecording);
    dataLayout->addWidget(btn_record_start_, 1, 0);
    
    btn_record_stop_ = new QPushButton("⏹ Stop (T)");
    btn_record_stop_->setToolTip("Stop video recording");
    connect(btn_record_stop_, &QPushButton::clicked, this, &TeleopWidget::onStopRecording);
    dataLayout->addWidget(btn_record_stop_, 1, 1);
    
    btn_rosbag_toggle_ = new QPushButton("📼 Rosbag (B)");
    btn_rosbag_toggle_->setCheckable(true);
    btn_rosbag_toggle_->setToolTip("Toggle rosbag recording");
    connect(btn_rosbag_toggle_, &QPushButton::clicked, this, &TeleopWidget::onToggleRosbag);
    dataLayout->addWidget(btn_rosbag_toggle_, 2, 0, 1, 2);
    
    mainLayout->addWidget(dataGroup);
    
    // GPR controls
    QGroupBox* gprGroup = new QGroupBox("GPR Control");
    QGridLayout* gprLayout = new QGridLayout(gprGroup);
    
    btn_gpr_scan_ = new QPushButton("📡 GPR Scan (G)");
    btn_gpr_scan_->setCheckable(true);
    btn_gpr_scan_->setToolTip("Toggle GPR scan (line + motor + logging)");
    connect(btn_gpr_scan_, &QPushButton::clicked, this, &TeleopWidget::onToggleGprScan);
    gprLayout->addWidget(btn_gpr_scan_, 0, 0, 1, 2);
    
    btn_gpr_up_ = new QPushButton("⬆ Line Up (L)");
    btn_gpr_up_->setToolTip("Raise GPR line (linear actuator)");
    connect(btn_gpr_up_, &QPushButton::clicked, this, &TeleopWidget::onGprLineUp);
    gprLayout->addWidget(btn_gpr_up_, 1, 0);
    
    btn_gpr_down_ = new QPushButton("⬇ Line Down (K)");
    btn_gpr_down_->setToolTip("Lower GPR line (linear actuator)");
    connect(btn_gpr_down_, &QPushButton::clicked, this, &TeleopWidget::onGprLineDown);
    gprLayout->addWidget(btn_gpr_down_, 1, 1);
    
    mainLayout->addWidget(gprGroup);
    
    // Status
    lbl_status_ = new QLabel("Status: Ready");
    lbl_status_->setStyleSheet("color: #666; font-size: 10px;");
    mainLayout->addWidget(lbl_status_);
    
    mainLayout->addStretch();
    
    setMinimumWidth(280);
}

void TeleopWidget::setupRosInterfaces() {
    if (!node_) {
        lbl_status_->setText("Status: No ROS2 node!");
        lbl_status_->setStyleSheet("color: red; font-size: 10px;");
        return;
    }
    
    // Publishers
    cmd_vel_pub_ = node_->create_publisher<geometry_msgs::msg::Twist>("/cmd_vel", 10);
    mpc_autonomy_pub_ = node_->create_publisher<std_msgs::msg::Bool>("/mpc_autonomy_enable", 10);
    
    // Service clients
    save_map_client_ = node_->create_client<std_srvs::srv::Trigger>("/save_raw_map");
    video_record_client_ = node_->create_client<std_srvs::srv::SetBool>("/video_record_set");
    rosbag_toggle_client_ = node_->create_client<std_srvs::srv::Trigger>("/rosbag/toggle");
    gpr_scan_toggle_client_ = node_->create_client<std_srvs::srv::Trigger>("/gpr_scan/toggle");
    gpr_line_start_client_ = node_->create_client<std_srvs::srv::Trigger>("/gpr_line_start");
    gpr_line_stop_client_ = node_->create_client<std_srvs::srv::Trigger>("/gpr_line_stop");
    
    lbl_status_->setText("Status: ROS2 connected");
    lbl_status_->setStyleSheet("color: green; font-size: 10px;");
}

void TeleopWidget::setTeleopEnabled(bool enabled) {
    teleop_enabled_ = enabled;
    chk_teleop_enabled_->setChecked(enabled);
    
    if (enabled) {
        cmd_vel_timer_->start(100);  // 10Hz
        setFocus();
        lbl_focus_hint_->setText("WASD active - this widget has focus");
        lbl_focus_hint_->setStyleSheet("color: #0a0; font-size: 10px; font-weight: bold;");
    } else {
        cmd_vel_timer_->stop();
        // Send zero velocity when disabling
        if (cmd_vel_pub_) {
            geometry_msgs::msg::Twist msg;
            cmd_vel_pub_->publish(msg);
        }
        key_w_ = key_a_ = key_s_ = key_d_ = false;
        updateKeyDisplay();
        lbl_focus_hint_->setText("Click here and check 'Enable' for keyboard control");
        lbl_focus_hint_->setStyleSheet("color: #888; font-size: 10px; font-style: italic;");
    }
}

void TeleopWidget::onTeleopToggled(bool checked) {
    setTeleopEnabled(checked);
}

void TeleopWidget::keyPressEvent(QKeyEvent* event) {
    if (!teleop_enabled_ || event->isAutoRepeat()) {
        QWidget::keyPressEvent(event);
        return;
    }
    
    bool handled = true;
    
    switch (event->key()) {
        case Qt::Key_W: key_w_ = true; break;
        case Qt::Key_A: key_a_ = true; break;
        case Qt::Key_S: key_s_ = true; break;
        case Qt::Key_D: key_d_ = true; break;
        case Qt::Key_E: onArmMotors(); break;
        case Qt::Key_Q: onDisarmMotors(); break;
        case Qt::Key_X: onToggleMpc(); break;
        case Qt::Key_M: onSaveMap(); break;
        case Qt::Key_R: onStartRecording(); break;
        case Qt::Key_T: onStopRecording(); break;
        case Qt::Key_B: onToggleRosbag(); break;
        case Qt::Key_G: onToggleGprScan(); break;
        case Qt::Key_L: onGprLineUp(); break;
        case Qt::Key_K: onGprLineDown(); break;
        default:
            handled = false;
            break;
    }
    
    if (handled) {
        updateKeyDisplay();
        event->accept();
    } else {
        QWidget::keyPressEvent(event);
    }
}

void TeleopWidget::keyReleaseEvent(QKeyEvent* event) {
    if (!teleop_enabled_ || event->isAutoRepeat()) {
        QWidget::keyReleaseEvent(event);
        return;
    }
    
    bool handled = true;
    
    switch (event->key()) {
        case Qt::Key_W: key_w_ = false; break;
        case Qt::Key_A: key_a_ = false; break;
        case Qt::Key_S: key_s_ = false; break;
        case Qt::Key_D: key_d_ = false; break;
        default:
            handled = false;
            break;
    }
    
    if (handled) {
        updateKeyDisplay();
        event->accept();
    } else {
        QWidget::keyReleaseEvent(event);
    }
}

void TeleopWidget::focusInEvent(QFocusEvent* event) {
    if (teleop_enabled_) {
        lbl_focus_hint_->setText("WASD active - this widget has focus");
        lbl_focus_hint_->setStyleSheet("color: #0a0; font-size: 10px; font-weight: bold;");
    }
    QWidget::focusInEvent(event);
}

void TeleopWidget::focusOutEvent(QFocusEvent* event) {
    // Release all keys when focus is lost
    key_w_ = key_a_ = key_s_ = key_d_ = false;
    updateKeyDisplay();
    
    if (teleop_enabled_) {
        lbl_focus_hint_->setText("Click to regain focus for keyboard control");
        lbl_focus_hint_->setStyleSheet("color: #c80; font-size: 10px;");
    }
    QWidget::focusOutEvent(event);
}

void TeleopWidget::setKeyIndicator(QLabel* label, bool pressed) {
    if (pressed) {
        label->setStyleSheet(
            "QLabel { background: #4CAF50; color: white; border: 2px solid #2E7D32; "
            "border-radius: 4px; font-weight: bold; font-size: 14px; }"
        );
    } else {
        label->setStyleSheet(
            "QLabel { background: #444; color: #aaa; border: 2px solid #555; "
            "border-radius: 4px; font-weight: bold; font-size: 14px; }"
        );
    }
}

void TeleopWidget::updateKeyDisplay() {
    setKeyIndicator(lbl_key_w_, key_w_);
    setKeyIndicator(lbl_key_a_, key_a_);
    setKeyIndicator(lbl_key_s_, key_s_);
    setKeyIndicator(lbl_key_d_, key_d_);
}

void TeleopWidget::publishCmdVel() {
    if (!cmd_vel_pub_ || mpc_enabled_) return;
    
    geometry_msgs::msg::Twist msg;
    
    if (key_w_) msg.linear.x = linear_speed_;
    if (key_s_) msg.linear.x = -linear_speed_;
    if (key_a_) msg.angular.z = angular_speed_;
    if (key_d_) msg.angular.z = -angular_speed_;
    
    cmd_vel_pub_->publish(msg);
}

void TeleopWidget::onLinearSpeedChanged(int value) {
    linear_speed_ = value / 100.0;
    lbl_linear_speed_->setText(QString("%1 m/s").arg(linear_speed_, 0, 'f', 1));
}

void TeleopWidget::onAngularSpeedChanged(int value) {
    angular_speed_ = value / 100.0;
    lbl_angular_speed_->setText(QString("%1 rad/s").arg(angular_speed_, 0, 'f', 1));
}

void TeleopWidget::onArmMotors() {
    emit statusMessage("Arming motors...");
    lbl_status_->setText("Status: Arming motors...");
    
    // Note: Full ODrive service call would require the odrive_can package
    // For now, we'll emit a status and the user can use the original teleop for motor control
    // Or we can add a simple ROS2 topic/service for this later
    
    QMessageBox::information(this, "Arm Motors", 
        "Motor arming requires ODrive service.\n"
        "Use 'E' key in the original teleop window or ensure ODrive nodes are running.");
}

void TeleopWidget::onDisarmMotors() {
    emit statusMessage("Disarming motors...");
    lbl_status_->setText("Status: Disarming motors...");
    
    QMessageBox::information(this, "Disarm Motors", 
        "Motor disarming requires ODrive service.\n"
        "Use 'Q' key in the original teleop window or ensure ODrive nodes are running.");
}

void TeleopWidget::onToggleMpc() {
    mpc_enabled_ = !mpc_enabled_;
    btn_mpc_toggle_->setChecked(mpc_enabled_);
    
    if (mpc_autonomy_pub_) {
        std_msgs::msg::Bool msg;
        msg.data = mpc_enabled_;
        mpc_autonomy_pub_->publish(msg);
    }
    
    if (mpc_enabled_) {
        btn_mpc_toggle_->setText("🤖 MPC ACTIVE (X)");
        btn_mpc_toggle_->setStyleSheet("QPushButton { background-color: #4CAF50; color: white; }");
        lbl_status_->setText("Status: MPC autonomous - WASD disabled");
        emit statusMessage("MPC autonomous control ENABLED");
    } else {
        btn_mpc_toggle_->setText("🤖 Enable MPC (X)");
        btn_mpc_toggle_->setStyleSheet("");
        lbl_status_->setText("Status: Manual control");
        emit statusMessage("MPC disabled - manual control");
    }
    
    emit mpcStateChanged(mpc_enabled_);
}

void TeleopWidget::onSaveMap() {
    if (!save_map_client_) return;
    
    lbl_status_->setText("Status: Saving map...");
    emit statusMessage("Saving map checkpoint...");
    
    auto request = std::make_shared<std_srvs::srv::Trigger::Request>();
    
    save_map_client_->async_send_request(request,
        [this](rclcpp::Client<std_srvs::srv::Trigger>::SharedFuture future) {
            try {
                auto result = future.get();
                if (result->success) {
                    QMetaObject::invokeMethod(this, [this, msg = QString::fromStdString(result->message)]() {
                        lbl_status_->setText("Status: Map saved");
                        emit statusMessage("Map saved: " + msg);
                    });
                } else {
                    QMetaObject::invokeMethod(this, [this]() {
                        lbl_status_->setText("Status: Map save failed");
                    });
                }
            } catch (...) {
                QMetaObject::invokeMethod(this, [this]() {
                    lbl_status_->setText("Status: Map save error");
                });
            }
        });
}

void TeleopWidget::onStartRecording() {
    if (!video_record_client_) return;
    
    lbl_status_->setText("Status: Starting recording...");
    
    auto request = std::make_shared<std_srvs::srv::SetBool::Request>();
    request->data = true;
    
    video_record_client_->async_send_request(request,
        [this](rclcpp::Client<std_srvs::srv::SetBool>::SharedFuture future) {
            try {
                auto result = future.get();
                QMetaObject::invokeMethod(this, [this, success = result->success]() {
                    if (success) {
                        recording_active_ = true;
                        lbl_status_->setText("Status: Recording...");
                        btn_record_start_->setStyleSheet("QPushButton { background-color: #f44336; }");
                    } else {
                        lbl_status_->setText("Status: Recording failed");
                    }
                });
            } catch (...) {}
        });
}

void TeleopWidget::onStopRecording() {
    if (!video_record_client_) return;
    
    lbl_status_->setText("Status: Stopping recording...");
    
    auto request = std::make_shared<std_srvs::srv::SetBool::Request>();
    request->data = false;
    
    video_record_client_->async_send_request(request,
        [this](rclcpp::Client<std_srvs::srv::SetBool>::SharedFuture future) {
            try {
                auto result = future.get();
                QMetaObject::invokeMethod(this, [this, success = result->success]() {
                    if (success) {
                        recording_active_ = false;
                        lbl_status_->setText("Status: Recording stopped");
                        btn_record_start_->setStyleSheet("");
                    }
                });
            } catch (...) {}
        });
}

void TeleopWidget::onToggleRosbag() {
    if (!rosbag_toggle_client_) return;
    
    auto request = std::make_shared<std_srvs::srv::Trigger::Request>();
    
    rosbag_toggle_client_->async_send_request(request,
        [this](rclcpp::Client<std_srvs::srv::Trigger>::SharedFuture future) {
            try {
                auto result = future.get();
                QMetaObject::invokeMethod(this, [this, success = result->success, 
                                                  msg = QString::fromStdString(result->message)]() {
                    if (success) {
                        rosbag_active_ = !rosbag_active_;
                        btn_rosbag_toggle_->setChecked(rosbag_active_);
                        lbl_status_->setText("Status: " + msg);
                        if (rosbag_active_) {
                            btn_rosbag_toggle_->setStyleSheet("QPushButton { background-color: #ff9800; }");
                        } else {
                            btn_rosbag_toggle_->setStyleSheet("");
                        }
                    }
                });
            } catch (...) {}
        });
}

void TeleopWidget::onToggleGprScan() {
    if (!gpr_scan_toggle_client_) return;
    
    auto request = std::make_shared<std_srvs::srv::Trigger::Request>();
    
    gpr_scan_toggle_client_->async_send_request(request,
        [this](rclcpp::Client<std_srvs::srv::Trigger>::SharedFuture future) {
            try {
                auto result = future.get();
                QMetaObject::invokeMethod(this, [this, success = result->success,
                                                  msg = QString::fromStdString(result->message)]() {
                    if (success) {
                        gpr_scan_active_ = !gpr_scan_active_;
                        btn_gpr_scan_->setChecked(gpr_scan_active_);
                        lbl_status_->setText("Status: " + msg);
                        if (gpr_scan_active_) {
                            btn_gpr_scan_->setStyleSheet("QPushButton { background-color: #2196F3; }");
                        } else {
                            btn_gpr_scan_->setStyleSheet("");
                        }
                    }
                });
            } catch (...) {}
        });
}

void TeleopWidget::onGprLineUp() {
    if (!gpr_line_start_client_) return;
    
    lbl_status_->setText("Status: GPR line UP...");
    
    auto request = std::make_shared<std_srvs::srv::Trigger::Request>();
    gpr_line_start_client_->async_send_request(request,
        [this](rclcpp::Client<std_srvs::srv::Trigger>::SharedFuture future) {
            try {
                auto result = future.get();
                QMetaObject::invokeMethod(this, [this, success = result->success]() {
                    lbl_status_->setText(success ? "Status: GPR line UP" : "Status: GPR line failed");
                });
            } catch (...) {}
        });
}

void TeleopWidget::onGprLineDown() {
    if (!gpr_line_stop_client_) return;
    
    lbl_status_->setText("Status: GPR line DOWN...");
    
    auto request = std::make_shared<std_srvs::srv::Trigger::Request>();
    gpr_line_stop_client_->async_send_request(request,
        [this](rclcpp::Client<std_srvs::srv::Trigger>::SharedFuture future) {
            try {
                auto result = future.get();
                QMetaObject::invokeMethod(this, [this, success = result->success]() {
                    lbl_status_->setText(success ? "Status: GPR line DOWN" : "Status: GPR line failed");
                });
            } catch (...) {}
        });
}

void TeleopWidget::updateButtonStates() {
    // Can be expanded to check service availability
}

// =============================================================================
// TeleopDockWidget Implementation
// =============================================================================

TeleopDockWidget::TeleopDockWidget(rclcpp::Node::SharedPtr node, QWidget* parent)
    : QDockWidget("Robot Teleop", parent)
{
    teleop_widget_ = new TeleopWidget(node, this);
    setWidget(teleop_widget_);
    
    setFeatures(QDockWidget::DockWidgetMovable | 
                QDockWidget::DockWidgetFloatable | 
                QDockWidget::DockWidgetClosable);
    
    setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
}

} // namespace f2c_cpp
