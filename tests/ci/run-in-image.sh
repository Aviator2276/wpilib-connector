#!/usr/bin/env bash
# Exercise the workspace image the way a Coder workspace would, and run the
# end-to-end suite and the vendor matrix against it.
#
#   tests/ci/run-in-image.sh [image]        default image: wpilib-workspace:dev
#
# In a container started from the image, as the `coder` user:
#   1. run the module's start script (rendered with its default variables),
#      twice, to check it is idempotent;
#   2. create the `vendor` test robot from the unmodified project template and
#      start `./gradlew simulateJava`;
#   3. run tests/e2e/run.mjs with headless Chromium inside the container
#      (the daemon listens on loopback only, as in production);
#   4. run tests/vendor-matrix/run.sh.
#
#   KEEP=1        leave the container running afterwards (name: websim-ci)
#   SKIP_MATRIX=1 skip step 4
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IMAGE="${1:-wpilib-workspace:dev}"
NAME=websim-ci
MODULE="$REPO/registry/aviator2277/modules/wpilib-sim"

docker rm -f "$NAME" > /dev/null 2>&1 || true
[ "${KEEP:-}" = 1 ] || trap 'docker rm -f "$NAME" > /dev/null 2>&1 || true' EXIT
docker run -d --name "$NAME" -v "$REPO:/repo:ro" "$IMAGE" sleep infinity > /dev/null

in_ws() { docker exec "$NAME" bash -lc "$1"; }
as_root() { docker exec -u root "$NAME" bash -c "$1"; }

echo "==> module start script"
# What Terraform's templatefile() does with the module's default variables.
sed -e 's/\$\${/${/g' \
  -e 's/"\${PORT}"/"13300"/' -e 's/"\${AUTO_DISABLE}"/"true"/' \
  -e 's/"\${INJECT_GRADLE}"/"true"/' -e 's|"\${PROJECTS_DIR}"|"$HOME/projects"|' \
  "$MODULE/run.sh" | docker exec -i "$NAME" bash -c 'cat > /tmp/run.sh'
in_ws 'bash /tmp/run.sh && bash /tmp/run.sh'
in_ws '[ "$(pgrep -fc "[w]ebsimd --host")" = 1 ]' || { echo "expected exactly one daemon" >&2; exit 1; }
in_ws 'curl -fsS http://127.0.0.1:13300/healthz > /dev/null'
in_ws 'test -f ~/.gradle/init.d/wpilib-websim.gradle'
in_ws 'grep -q "\"wpilib.skipSelectSimulateExtension\": true" ~/.local/share/code-server/Machine/settings.json'

echo "==> test tooling (Node, Playwright)"
as_root 'export DEBIAN_FRONTEND=noninteractive
  command -v node > /dev/null || { curl -fsSL https://deb.nodesource.com/setup_22.x | bash - > /dev/null 2>&1
    apt-get install -y -qq nodejs > /dev/null; }'
in_ws 'rm -rf /tmp/w && mkdir -p /tmp/w && cp -R /repo/tests /repo/images /tmp/w/ \
  && cd /tmp/w/tests/e2e && rm -rf node_modules && npm install --silent > /dev/null 2>&1'
as_root 'cd /tmp/w/tests/e2e && npx --yes playwright install-deps chromium > /dev/null 2>&1'
in_ws 'cd /tmp/w/tests/e2e && npx playwright install chromium > /dev/null 2>&1'

echo "==> simulate the vendor robot (unmodified template project)"
in_ws 'mkdir -p ~/projects && bash /repo/tests/robots/make-project.sh vendor ~/projects/robot > /dev/null'
docker exec -d "$NAME" bash -lc 'cd ~/projects/robot && ./gradlew simulateJava --console=plain > /tmp/sim.log 2>&1'
for _ in $(seq 1 240); do
  in_ws 'grep -q "Robot program starting" /tmp/sim.log' 2> /dev/null && break
  if in_ws 'grep -q "BUILD FAILED" /tmp/sim.log' 2> /dev/null; then
    in_ws 'tail -n 40 /tmp/sim.log'; exit 1
  fi
  sleep 2
done
in_ws 'grep -q "Robot program starting" /tmp/sim.log' || { in_ws 'tail -n 40 /tmp/sim.log'; exit 1; }
in_ws 'grep -q "Web Sim Bridge Initialized" /tmp/sim.log' || { echo "the HAL bridge did not load" >&2; exit 1; }
# no display, no native GUI
in_ws '! grep -q "Simulator GUI Initializ" /tmp/sim.log'
# The robot logs every NetworkTables connection. With no browser open the
# daemon holds exactly one (camera discovery); detecting the robot must not
# add a connection per second to the student's console.
sleep 15
nt_conns="$(in_ws 'grep -c "Got a NT4 connection" /tmp/sim.log' || true)"
echo "NT connections in the robot log after 15 s idle: $nt_conns"
[ "$nt_conns" -le 2 ] || { echo "the daemon is probing NetworkTables by connecting" >&2; exit 1; }

echo "==> end-to-end suite"
in_ws 'cd /tmp/w/tests/e2e && ROBOT_LOG=/tmp/sim.log EXPECT_BRIDGE=1 node run.mjs'

if [ "${SKIP_MATRIX:-}" != 1 ]; then
  echo "==> vendor matrix"
  in_ws 'pkill -f "[D]java.library.path" || true; sleep 2; bash /tmp/w/tests/vendor-matrix/run.sh'
fi
echo "==> all image checks passed"
