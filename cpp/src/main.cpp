/**
 * @file main.cpp
 * @brief Entry point for F2C Coverage Planner GUI
 * 
 * C++ version of the Fields2Cover coverage planner GUI.
 * Equivalent to f2c_gui.py but with native performance.
 */

#include <QApplication>
#include <QStyleFactory>
#include <QFont>
#include <QFontDatabase>
#include <QIcon>
#include <cstdlib>
#include <iostream>
#include <string>
#include <rclcpp/rclcpp.hpp>

#include "app_shell.hpp"

int main(int argc, char* argv[])
{
    // Keep app ROS graph behavior consistent with manual teleop launch:
    // an inherited custom Cyclone profile can break zenoh ros2dds discovery.
    if (const char* cdds_uri = std::getenv("CYCLONEDDS_URI")) {
        const std::string uri(cdds_uri);
        if (uri.find("rf_cyclonedds.xml") != std::string::npos) {
            std::cout << "[BDR] Clearing CYCLONEDDS_URI for app ROS context: "
                      << uri << std::endl;
            unsetenv("CYCLONEDDS_URI");
        }
    }

    rclcpp::init(argc, argv);

    // Create Qt application
    QApplication app(argc, argv);
    
    // Set application metadata
    app.setApplicationName("BDR Coverage Planner");
    app.setApplicationDisplayName("BDR Coverage Planner");
    app.setApplicationVersion("1.0.0");
    app.setOrganizationName("PilotControl");

    // Set application/window icon (embedded via Qt resources)
    app.setWindowIcon(QIcon(":/assets/bdr_logo.png"));
    
    // Use Fusion style for modern look
    app.setStyle(QStyleFactory::create("Fusion"));
    
    // Set default font
    QFont font("Segoe UI", 10);
    QFontDatabase fontDb;
    if (fontDb.families().filter("Segoe UI").isEmpty()) {
        font = QFont("Sans Serif", 10);
    }
    app.setFont(font);
    
    // Print startup info
    std::cout << "=== BDR Coverage Planner (C++) ===" << std::endl;
    std::cout << "Version: 1.0.0" << std::endl;
    std::cout << "Qt Version: " << QT_VERSION_STR << std::endl;
#ifdef HAVE_FIELDS2COVER
    std::cout << "Fields2Cover: Available" << std::endl;
#else
    std::cout << "Fields2Cover: NOT FOUND (coverage features disabled)" << std::endl;
#endif
#ifdef HAVE_CGAL
    std::cout << "CGAL: Available (alphashape enabled)" << std::endl;
#else
    std::cout << "CGAL: NOT FOUND (using convex hull fallback)" << std::endl;
#endif
    std::cout << "==================================" << std::endl;
    
    // Create and show app shell (stage router)
    f2c_cpp::AppShellWindow shell;
    shell.show();
    
    // Run event loop
    int exit_code = app.exec();

    rclcpp::shutdown();
    return exit_code;
}

