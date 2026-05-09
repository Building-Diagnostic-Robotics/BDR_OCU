cmake_minimum_required(VERSION 3.16)
project(odrive_can)

find_package(ament_cmake REQUIRED)
find_package(rosidl_default_generators REQUIRED)

rosidl_generate_interfaces(${PROJECT_NAME}
  msg/ControlMessage.msg
  msg/ControllerStatus.msg
  msg/ODriveStatus.msg
  srv/AxisState.srv
)

ament_export_dependencies(rosidl_default_runtime)
ament_package()
