"""websimd: the workspace-side daemon of the WPILib Web Sim.

It serves the browser app and relays three things between the browser and the
robot program running in the same workspace: NetworkTables (NT4 WebSocket),
HAL state (halsim_ws or the native bridge) and camera streams (MJPEG).
"""

__version__ = "2.0.0"
