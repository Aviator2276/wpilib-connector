package frc.robot;

import com.ctre.phoenix6.controls.DutyCycleOut;
import com.ctre.phoenix6.hardware.CANcoder;
import com.ctre.phoenix6.hardware.Pigeon2;
import com.ctre.phoenix6.hardware.TalonFX;
import com.revrobotics.spark.SparkLowLevel.MotorType;
import com.revrobotics.spark.SparkMax;
import com.studica.frc.AHRS;
import edu.wpi.first.apriltag.AprilTagFieldLayout;
import edu.wpi.first.apriltag.AprilTagFields;
import edu.wpi.first.math.geometry.Pose2d;
import edu.wpi.first.math.geometry.Rotation2d;
import edu.wpi.first.math.geometry.Transform3d;
import edu.wpi.first.wpilibj.AnalogInput;
import edu.wpi.first.wpilibj.DigitalInput;
import edu.wpi.first.wpilibj.Encoder;
import edu.wpi.first.wpilibj.TimedRobot;
import edu.wpi.first.wpilibj.Timer;
import edu.wpi.first.wpilibj.motorcontrol.PWMSparkMax;
import edu.wpi.first.wpilibj.smartdashboard.Field2d;
import edu.wpi.first.wpilibj.smartdashboard.Mechanism2d;
import edu.wpi.first.wpilibj.smartdashboard.MechanismLigament2d;
import edu.wpi.first.wpilibj.smartdashboard.SmartDashboard;
import org.photonvision.PhotonCamera;
import org.photonvision.simulation.PhotonCameraSim;
import org.photonvision.simulation.SimCameraProperties;
import org.photonvision.simulation.VisionSystemSim;

/**
 * Test robot with a PhotonVision simulated camera on top of the vendor devices. PhotonLib ships
 * desktop native code for x86-64 only, so this robot cannot start on an arm64 machine.
 */
public class Robot extends TimedRobot {
  private final PWMSparkMax m_pwm = new PWMSparkMax(0);
  private final DigitalInput m_dio = new DigitalInput(0);
  private final AnalogInput m_ai = new AnalogInput(0);
  private final Encoder m_enc = new Encoder(1, 2);
  private final TalonFX m_talon = new TalonFX(1);
  private final CANcoder m_cancoder = new CANcoder(2);
  private final Pigeon2 m_pigeon = new Pigeon2(3);
  private final SparkMax m_spark = new SparkMax(4, MotorType.kBrushless);
  private final AHRS m_navx = new AHRS(AHRS.NavXComType.kMXP_SPI);
  private final Field2d m_field = new Field2d();
  private final Mechanism2d m_mech = new Mechanism2d(3, 3);
  private MechanismLigament2d m_arm;
  private final PhotonCamera m_cam = new PhotonCamera("frontcam");
  private VisionSystemSim m_vision;
  private double m_counter;

  @Override
  public void robotInit() {
    SmartDashboard.putData("Field", m_field);
    m_arm = m_mech.getRoot("base", 1.5, 0.5).append(new MechanismLigament2d("arm", 1.0, 45));
    SmartDashboard.putData("Arm", m_mech);
    if (isSimulation()) {
      m_vision = new VisionSystemSim("main");
      m_vision.addAprilTags(AprilTagFieldLayout.loadField(AprilTagFields.kDefaultField));
      m_vision.addCamera(new PhotonCameraSim(m_cam, new SimCameraProperties()), new Transform3d());
    }
  }

  @Override
  public void robotPeriodic() {
    m_counter += 0.02;
    SmartDashboard.putNumber("Counter", m_counter);
    double t = Timer.getFPGATimestamp();
    m_field.setRobotPose(new Pose2d(4 + 2 * Math.cos(t), 4 + 2 * Math.sin(t), Rotation2d.fromRadians(t)));
    m_arm.setAngle(45 + 30 * Math.sin(t));
  }

  @Override
  public void teleopPeriodic() {
    m_pwm.set(Math.sin(Timer.getFPGATimestamp()));
    m_talon.setControl(new DutyCycleOut(0.5));
    m_spark.set(0.25);
  }

  @Override
  public void simulationPeriodic() {
    m_vision.update(m_field.getRobotPose());
  }
}
