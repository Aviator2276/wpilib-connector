  private final org.photonvision.PhotonCamera m_cam = new org.photonvision.PhotonCamera("frontcam");
  private final org.photonvision.simulation.VisionSystemSim m_vision = makeVision();

  private org.photonvision.simulation.VisionSystemSim makeVision() {
    var vision = new org.photonvision.simulation.VisionSystemSim("main");
    vision.addCamera(
        new org.photonvision.simulation.PhotonCameraSim(
            m_cam, new org.photonvision.simulation.SimCameraProperties()),
        new edu.wpi.first.math.geometry.Transform3d());
    return vision;
  }

  @Override
  public void simulationPeriodic() {
    m_vision.update(new edu.wpi.first.math.geometry.Pose2d());
  }
