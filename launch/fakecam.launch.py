from launch import LaunchDescription
from launch_ros.actions import LifecycleNode

def generate_launch_description():
    node = LifecycleNode(
        package='cambuffer_recorder_ng',
        executable='cambuffer_recorder_ng',
        name='fakecam_node',
        namespace='',              # <-- this line fixes the error
        output='screen',
        parameters=[{
            'width': 640,
            'height': 480,
            'fps': 30
        }],
    )
    return LaunchDescription([node])

