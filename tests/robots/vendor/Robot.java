package frc.robot;

import com.ctre.phoenix6.controls.DutyCycleOut;
import com.ctre.phoenix6.hardware.CANcoder;
import com.ctre.phoenix6.hardware.Pigeon2;
import com.ctre.phoenix6.hardware.TalonFX;
import com.revrobotics.spark.SparkLowLevel.MotorType;
import com.revrobotics.spark.SparkMax;
import com.studica.frc.AHRS;
import edu.wpi.first.cameraserver.CameraServer;
import edu.wpi.first.cscore.CvSource;
import edu.wpi.first.math.geometry.Pose2d;
import edu.wpi.first.math.geometry.Rotation2d;
import edu.wpi.first.wpilibj.AddressableLED;
import edu.wpi.first.wpilibj.AddressableLEDBuffer;
import edu.wpi.first.wpilibj.AnalogGyro;
import edu.wpi.first.wpilibj.AnalogInput;
import edu.wpi.first.wpilibj.AnalogOutput;
import edu.wpi.first.wpilibj.BuiltInAccelerometer;
import edu.wpi.first.wpilibj.DigitalInput;
import edu.wpi.first.wpilibj.DigitalOutput;
import edu.wpi.first.wpilibj.DutyCycleEncoder;
import edu.wpi.first.wpilibj.Encoder;
import edu.wpi.first.wpilibj.PneumaticsModuleType;
import edu.wpi.first.wpilibj.PowerDistribution;
import edu.wpi.first.wpilibj.Relay;
import edu.wpi.first.wpilibj.Solenoid;
import edu.wpi.first.wpilibj.TimedRobot;
import edu.wpi.first.wpilibj.Timer;
import edu.wpi.first.wpilibj.XboxController;
import edu.wpi.first.wpilibj.motorcontrol.PWMSparkMax;
import edu.wpi.first.wpilibj.smartdashboard.Field2d;
import edu.wpi.first.wpilibj.smartdashboard.Mechanism2d;
import edu.wpi.first.wpilibj.smartdashboard.MechanismLigament2d;
import edu.wpi.first.wpilibj.smartdashboard.SmartDashboard;
import edu.wpi.first.wpilibj.util.Color;
import org.opencv.core.Mat;
import org.opencv.core.Point;
import org.opencv.core.Scalar;
import org.opencv.imgproc.Imgproc;

/**
 * Test robot for the web Sim GUI: one of every device the Sim GUI has a window for, plus the
 * vendor devices that ship simulation support for every desktop platform, and a camera stream.
 */
public class Robot extends TimedRobot {
  // roboRIO devices
  private final PWMSparkMax m_pwm = new PWMSparkMax(0);
  private final DigitalInput m_dio = new DigitalInput(0);
  private final DigitalOutput m_dout = new DigitalOutput(5);
  private final AnalogInput m_ai = new AnalogInput(0);
  private final AnalogOutput m_ao = new AnalogOutput(0);
  private final AnalogGyro m_gyro = new AnalogGyro(1);
  private final Encoder m_enc = new Encoder(1, 2);
  private final DutyCycleEncoder m_absEnc = new DutyCycleEncoder(3);
  private final Relay m_relay = new Relay(0);
  private final BuiltInAccelerometer m_accel = new BuiltInAccelerometer();
  private final Solenoid m_pcmSolenoid = new Solenoid(0, PneumaticsModuleType.CTREPCM, 2);
  private final Solenoid m_phSolenoid = new Solenoid(1, PneumaticsModuleType.REVPH, 3);
  private final PowerDistribution m_pd = new PowerDistribution(1, PowerDistribution.ModuleType.kRev);
  private final AddressableLED m_led = new AddressableLED(9);
  private final AddressableLEDBuffer m_ledBuffer = new AddressableLEDBuffer(12);
  private final XboxController m_xbox = new XboxController(0);

  // vendor devices
  private final TalonFX m_talon = new TalonFX(1);
  private final CANcoder m_cancoder = new CANcoder(2);
  private final Pigeon2 m_pigeon = new Pigeon2(3);
  private final SparkMax m_spark = new SparkMax(4, MotorType.kBrushless);
  private final AHRS m_navx = new AHRS(AHRS.NavXComType.kMXP_SPI);

  // NetworkTables views
  private final Field2d m_field = new Field2d();
  private final Mechanism2d m_mech = new Mechanism2d(3, 3);
  private MechanismLigament2d m_arm;
  private double m_counter;

  // camera
  private CvSource m_camera;
  private Mat m_frame;

  public Robot() {
    SmartDashboard.putData("Field", m_field);
    m_arm = m_mech.getRoot("base", 1.5, 0.5).append(new MechanismLigament2d("arm", 1.0, 45));
    SmartDashboard.putData("Arm", m_mech);
    m_led.setLength(m_ledBuffer.getLength());
    m_led.start();
    // CameraServer loads the OpenCV natives; a Mat cannot be created before it.
    m_camera = CameraServer.putVideo("testcam", 320, 240);
    m_frame = new Mat(240, 320, org.opencv.core.CvType.CV_8UC3);
  }

  @Override
  public void robotPeriodic() {
    m_counter += 0.02;
    SmartDashboard.putNumber("Counter", m_counter);
    SmartDashboard.putNumber("AnalogIn0", m_ai.getVoltage());
    SmartDashboard.putBoolean("DIO0", m_dio.get());
    SmartDashboard.putNumber("XboxLeftY", m_xbox.getLeftY());
    SmartDashboard.putNumber("EncoderCount", m_enc.get());
    double t = Timer.getFPGATimestamp();
    m_field.setRobotPose(
        new Pose2d(4 + 2 * Math.cos(t), 4 + 2 * Math.sin(t), Rotation2d.fromRadians(t)));
    m_arm.setAngle(45 + 30 * Math.sin(t));
    for (int i = 0; i < m_ledBuffer.getLength(); i++) {
      m_ledBuffer.setLED(i, i % 3 == 0 ? Color.kRed : i % 3 == 1 ? Color.kGreen : Color.kBlue);
    }
    m_led.setData(m_ledBuffer);

    // ten frames a second is plenty to prove the stream
    if (((int) (m_counter * 50)) % 5 == 0) {
      m_frame.setTo(new Scalar(40, 30, 20));
      Imgproc.circle(
          m_frame,
          new Point(160 + 100 * Math.cos(t), 120 + 80 * Math.sin(t)),
          20,
          new Scalar(0, 200, 255),
          -1);
      Imgproc.putText(
          m_frame,
          String.format("t=%.1f", t),
          new Point(10, 25),
          Imgproc.FONT_HERSHEY_SIMPLEX,
          0.7,
          new Scalar(255, 255, 255),
          2);
      m_camera.putFrame(m_frame);
    }
  }

  @Override
  public void teleopPeriodic() {
    double t = Timer.getFPGATimestamp();
    m_pwm.set(Math.sin(t));
    m_ao.setVoltage(2.5 + 2.5 * Math.sin(t));
    m_dout.set(((int) t) % 2 == 0);
    m_relay.set(((int) t) % 2 == 0 ? Relay.Value.kForward : Relay.Value.kReverse);
    m_pcmSolenoid.set(((int) t) % 2 == 0);
    m_phSolenoid.set(((int) t) % 2 == 1);
    m_talon.setControl(new DutyCycleOut(0.5));
    m_spark.set(0.25);
  }

  // The end-to-end tests read these lines from the robot's console output.
  @Override
  public void teleopInit() {
    System.out.println("[websim-test] mode=teleop");
  }

  @Override
  public void autonomousInit() {
    System.out.println("[websim-test] mode=autonomous");
  }

  @Override
  public void disabledInit() {
    System.out.println("[websim-test] mode=disabled");
    m_pwm.set(0);
    m_relay.set(Relay.Value.kOff);
  }
}
