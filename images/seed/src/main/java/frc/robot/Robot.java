// Copyright (c) FIRST and other WPILib contributors.
// Open Source Software; you can modify and/or share it under the terms of
// the WPILib BSD license file in the root directory of this project.

package frc.robot;

import edu.wpi.first.wpilibj.TimedRobot;
import edu.wpi.first.wpilibj.smartdashboard.SmartDashboard;

/** Minimal TimedRobot used to validate the simulation GUI pipeline. */
public class Robot extends TimedRobot {
  private double m_counter;

  @Override
  public void robotInit() {
    SmartDashboard.putString("SpikeStatus", "sim gui pipeline alive");
  }

  @Override
  public void robotPeriodic() {
    m_counter += 0.02;
    SmartDashboard.putNumber("Counter", m_counter);
  }
}
