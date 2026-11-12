run "defaults" {
  command = plan

  variables {
    agent_id = "test-agent-id"
  }

  assert {
    condition     = resource.coder_app.wpilib_sim.url == "http://localhost:13300"
    error_message = "App URL should use the default port"
  }

  assert {
    condition     = resource.coder_app.wpilib_sim.slug == "wpilib-sim"
    error_message = "The slug is kept from v1 so existing templates and bookmarks still work"
  }

  assert {
    condition     = resource.coder_app.wpilib_sim.display_name == "WPILib Simulator"
    error_message = "The display name is kept from v1"
  }

  assert {
    condition     = one(resource.coder_app.wpilib_sim.healthcheck[*].url) == "http://localhost:13300/healthz"
    error_message = "The healthcheck should probe the daemon's /healthz"
  }

  assert {
    condition     = resource.coder_app.wpilib_sim.share == "owner"
    error_message = "Anyone who can open the app can enable the robot; default to owner only"
  }

  assert {
    condition     = resource.coder_app.wpilib_sim.subdomain == true
    error_message = "Subdomain mode is the default"
  }

  assert {
    condition     = resource.coder_env.websim.name == "WPILIB_WEBSIM" && resource.coder_env.websim.value == "1"
    error_message = "WPILIB_WEBSIM=1 must be exported for the Gradle init script"
  }

  assert {
    condition     = strcontains(resource.coder_script.wpilib_sim.script, "--auto-disable")
    error_message = "Auto-disable is on by default"
  }

  assert {
    condition     = strcontains(resource.coder_script.wpilib_sim.script, "INJECT_GRADLE=\"true\"")
    error_message = "The Gradle init script is installed by default"
  }
}

run "custom_port" {
  command = plan

  variables {
    agent_id = "test-agent-id"
    port     = 23456
  }

  assert {
    condition     = resource.coder_app.wpilib_sim.url == "http://localhost:23456"
    error_message = "App URL should use the configured port"
  }

  assert {
    condition     = one(resource.coder_app.wpilib_sim.healthcheck[*].url) == "http://localhost:23456/healthz"
    error_message = "Healthcheck should use the configured port"
  }

  assert {
    condition     = strcontains(resource.coder_script.wpilib_sim.script, "PORT=\"23456\"")
    error_message = "The daemon should be started on the configured port"
  }

  assert {
    condition     = output.port == 23456
    error_message = "The port output should echo the configured port"
  }
}

run "options_off" {
  command = plan

  variables {
    agent_id      = "test-agent-id"
    auto_disable  = false
    inject_gradle = false
    subdomain     = false
    share         = "authenticated"
    projects_dir  = "/workspace/robots"
  }

  assert {
    condition     = strcontains(resource.coder_script.wpilib_sim.script, "AUTO_DISABLE=\"false\"")
    error_message = "auto_disable = false must reach the script"
  }

  assert {
    condition     = strcontains(resource.coder_script.wpilib_sim.script, "INJECT_GRADLE=\"false\"")
    error_message = "inject_gradle = false must reach the script"
  }

  assert {
    condition     = strcontains(resource.coder_script.wpilib_sim.script, "PROJECTS_DIR=\"/workspace/robots\"")
    error_message = "projects_dir must reach the script"
  }

  assert {
    condition     = resource.coder_app.wpilib_sim.subdomain == false
    error_message = "Path mode must be selectable"
  }

  assert {
    condition     = resource.coder_app.wpilib_sim.share == "authenticated"
    error_message = "share must be passed through"
  }
}

run "rejects_privileged_port" {
  command = plan

  variables {
    agent_id = "test-agent-id"
    port     = 80
  }

  expect_failures = [var.port]
}

run "rejects_unknown_share" {
  command = plan

  variables {
    agent_id = "test-agent-id"
    share    = "everyone"
  }

  expect_failures = [var.share]
}
