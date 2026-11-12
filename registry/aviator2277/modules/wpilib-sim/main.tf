terraform {
  required_version = ">= 1.0"

  required_providers {
    coder = {
      source  = "coder/coder"
      version = ">= 2.5"
    }
  }
}

variable "agent_id" {
  type        = string
  description = "The ID of a Coder agent."
}

variable "port" {
  type        = number
  description = "Loopback port for the web sim daemon. In a network namespace shared between workspaces this must be unique per workspace — pass a per-workspace random port."
  default     = 13300

  validation {
    condition     = var.port > 1024 && var.port < 65536
    error_message = "port must be between 1025 and 65535."
  }
}

variable "auto_disable" {
  type        = bool
  description = "Disable the simulated robot a few seconds after the last browser tab closes. Turn off if the robot is driven from another NetworkTables dashboard without the simulator tab open."
  default     = true
}

variable "inject_gradle" {
  type        = bool
  description = "Install the Gradle init script that makes every GradleRIO project simulate headless and feed the web GUI, without editing the project. When false, projects must enable the simulation WebSocket server themselves."
  default     = true
}

variable "projects_dir" {
  type        = string
  description = "Where robot projects live; shown in the \"no simulation running\" card."
  default     = "$HOME/projects"
}

variable "order" {
  type        = number
  description = "The order determines the position of the app in the UI presentation."
  default     = null
}

variable "group" {
  type        = string
  description = "The name of a group that this app belongs to."
  default     = null
}

variable "share" {
  type        = string
  description = "Who can open the app. Anyone who can open it can enable the simulated robot."
  default     = "owner"

  validation {
    condition     = contains(["owner", "authenticated", "public"], var.share)
    error_message = "share must be one of: owner, authenticated, public."
  }
}

variable "subdomain" {
  type        = bool
  description = "Serve the app on a subdomain (needs a wildcard access URL). When false it is served under a path; both work without further configuration."
  default     = true
}

locals {
  icon = "https://raw.githubusercontent.com/wpilibsuite/allwpilib/main/wpiutil/src/main/native/resources/wpilib-128.png"
}

# Read by the Gradle init script: "0" restores the native Sim GUI for one
# command (WPILIB_WEBSIM=0 ./gradlew simulateJava).
resource "coder_env" "websim" {
  agent_id = var.agent_id
  name     = "WPILIB_WEBSIM"
  value    = "1"
}

resource "coder_script" "wpilib_sim" {
  agent_id     = var.agent_id
  display_name = "WPILib Simulator"
  icon         = local.icon
  run_on_start = true
  script = templatefile("${path.module}/run.sh", {
    PORT          = var.port
    AUTO_DISABLE  = var.auto_disable ? "true" : "false"
    INJECT_GRADLE = var.inject_gradle ? "true" : "false"
    PROJECTS_DIR  = var.projects_dir
  })
}

resource "coder_app" "wpilib_sim" {
  agent_id     = var.agent_id
  slug         = "wpilib-sim"
  display_name = "WPILib Simulator"
  url          = "http://localhost:${var.port}"
  icon         = local.icon
  subdomain    = var.subdomain
  share        = var.share
  order        = var.order
  group        = var.group

  healthcheck {
    url       = "http://localhost:${var.port}/healthz"
    interval  = 5
    threshold = 5
  }
}

output "port" {
  value       = var.port
  description = "Loopback port the web sim daemon listens on."
}
