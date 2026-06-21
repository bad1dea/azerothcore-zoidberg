#!/usr/bin/env bash
set -euo pipefail

MODE="yolo"
SCOPE="repo"
FORCE="false"

usage() {
  cat <<'EOF'
Install a permissive Codex dev-server policy.

Usage:
  bash install-codex-dev-server-policy.sh [--mode yolo|safe] [--scope repo|global] [--force]

Examples:
  # Recommended for a throwaway/dev server with minimal prompts:
  bash install-codex-dev-server-policy.sh --mode yolo --scope repo

  # Safer: workspace-write with network enabled:
  bash install-codex-dev-server-policy.sh --mode safe --scope repo

  # Apply to your whole user account:
  bash install-codex-dev-server-policy.sh --mode yolo --scope global

After install:
  - Restart Codex.
  - For repo scope, run Codex from this repository.
  - Use ./codex-dev-yolo.sh for zero-prompt dev-server mode.
  - Use ./codex-dev-safe.sh for workspace-write mode.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --mode)
      MODE="${2:-}"
      shift 2
      ;;
    --scope)
      SCOPE="${2:-}"
      shift 2
      ;;
    --force)
      FORCE="true"
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if [[ "$MODE" != "yolo" && "$MODE" != "safe" ]]; then
  echo "Invalid --mode: $MODE. Use yolo or safe." >&2
  exit 1
fi

if [[ "$SCOPE" != "repo" && "$SCOPE" != "global" ]]; then
  echo "Invalid --scope: $SCOPE. Use repo or global." >&2
  exit 1
fi

timestamp="$(date +%Y%m%d-%H%M%S)"

if [[ "$SCOPE" == "global" ]]; then
  TARGET_DIR="${HOME}/.codex"
  AGENTS_FILE="${TARGET_DIR}/AGENTS.md"
else
  TARGET_DIR="$(pwd)/.codex"
  AGENTS_FILE="$(pwd)/AGENTS.md"
fi

RULES_DIR="${TARGET_DIR}/rules"
CONFIG_FILE="${TARGET_DIR}/config.toml"

mkdir -p "$RULES_DIR"

backup_if_exists() {
  local file="$1"
  if [[ -e "$file" && "$FORCE" != "true" ]]; then
    cp -a "$file" "${file}.bak-${timestamp}"
    echo "Backed up existing $file to ${file}.bak-${timestamp}"
  fi
}

write_file() {
  local file="$1"
  local content="$2"
  backup_if_exists "$file"
  printf "%s\n" "$content" > "$file"
}

append_agents_if_needed() {
  local file="$1"
  local content="$2"
  local marker="AGENTS.md - Dev Server Codex Working Agreement"

  if [[ -e "$file" ]] && grep -q "$marker" "$file"; then
    echo "$file already contains Codex dev-server policy"
    return
  fi

  if [[ -e "$file" && "$FORCE" != "true" ]]; then
    cp -a "$file" "${file}.bak-${timestamp}"
    {
      echo
      echo
      echo "<!-- BEGIN CODEX DEV SERVER POLICY -->"
      printf "%s\n" "$content"
      echo "<!-- END CODEX DEV SERVER POLICY -->"
    } >> "$file"
    echo "Appended policy to existing $file"
  else
    printf "%s\n" "$content" > "$file"
    echo "Created $file"
  fi
}

AGENTS_CONTENT=$(cat <<'EOF_AGENTS'
# AGENTS.md - Dev Server Codex Working Agreement

Context:
- This is a development server / dev environment.
- The project uses C++, Git, Docker, SSH, and SQL/database tooling.
- Prefer moving fast. Do not repeatedly ask before normal dev/build/test/deploy commands.

## Default behavior

You may run commands without asking when they are part of normal development work:
- Inspect files, search code, read logs, and check running services.
- Edit files inside this repository/workspace.
- Build C++ code with CMake, Make, Ninja, GCC, Clang, or project scripts.
- Run test suites, linters, formatters, and debug tooling.
- Use Git locally, including add, commit, stash, branch, switch, fetch, merge, and rebase continue/abort.
- Use Docker and Docker Compose for this dev stack, including build, up, down, restart, logs, exec, run, pull, cp, inspect, and prune for dev cleanup.
- Use SSH to approved dev hosts for read checks, deploys, service restarts, Docker Compose actions, logs, Git pulls, builds, and project-local file updates.
- Run SQL inspection and dev database changes when clearly scoped to local/dev/staging databases.

## Dev server assumptions

This is not production. It is acceptable to:
- Restart services.
- Rebuild containers.
- Bring the dev stack down/up.
- Apply dev migrations.
- Truncate/reload dev data when the task clearly requires it.
- Run deployment scripts for the dev environment.
- Use package managers if needed for project dependencies.
- Create backups automatically before risky database or filesystem changes when practical.

## Still avoid or stop before doing these

Do not expose secrets:
- Do not print `.env`, private keys, tokens, passwords, database credentials, or SSH keys.
- Do not paste secrets into the chat.
- If you need a secret, ask me to provide it through the correct channel.

Do not run obviously catastrophic host commands:
- Do not wipe `/`, `/home`, `/etc`, `/var/lib/docker`, or `~/.ssh`.
- Do not intentionally destroy all Docker volumes unless I explicitly ask.
- Do not change firewall/network/SSH server security globally unless I explicitly ask.
- Do not rotate keys, passwords, tokens, or certificates unless I explicitly ask.

## Preferred workflow

1. Inspect status first.
2. Make the smallest working change.
3. Build/test locally.
4. Use Docker Compose for dev stack changes.
5. For deploys, prefer project scripts or documented commands.
6. After changes, summarize:
   - what changed
   - what commands ran
   - test/build result
   - anything still broken

## Useful command families

Allowed and expected:
- `git status`, `git diff`, `git log`, `git fetch`, `git add`, `git commit`, `git stash`, `git switch`, `git checkout -b`
- `cmake -S . -B build`, `cmake --build build -j`, `ninja`, `make -j`, `ctest --output-on-failure`
- `docker compose ps`, `docker compose logs`, `docker compose build`, `docker compose up -d`, `docker compose restart`, `docker compose down`, `docker compose exec`, `docker compose run --rm`
- `ssh <dev-host> "<project command>"`
- `rsync -avz`, `scp`
- `mysql`, `mariadb`, `psql`, `sqlite3` for dev DB work
- `rg`, `grep`, `find`, `awk`, `sed`, `jq`, `yq`
- project scripts in `scripts/`, `tools/`, `deploy/`, or repo root

When unsure whether a target is production, treat it as production and ask.
EOF_AGENTS
)

CONFIG_SAFE_CONTENT=$(cat <<'EOF_SAFE'
# Codex config - dev server normal mode
# Lower-friction than defaults, but still keeps workspace boundaries.

model = "gpt-5.5"
approval_policy = "on-request"
sandbox_mode = "workspace-write"
web_search = "cached"

[sandbox_workspace_write]
network_access = true

[features]
shell_snapshot = true
EOF_SAFE
)

CONFIG_YOLO_CONTENT=$(cat <<'EOF_YOLO'
# Codex config - dev server YOLO mode
# Use only on a dev server / disposable environment.
# This removes local sandbox restrictions and avoids approval prompts.

model = "gpt-5.5"
approval_policy = "never"
sandbox_mode = "danger-full-access"
web_search = "cached"

[features]
shell_snapshot = true
EOF_YOLO
)

RULES_CONTENT=$(cat <<'EOF_RULES'
# Codex dev-server command rules
# Place as ~/.codex/rules/default.rules or <repo>/.codex/rules/default.rules.
# Rules are evaluated as command-prefix matches.

# -------------------------
# Broad allow: inspection
# -------------------------

prefix_rule(pattern = ["pwd"], decision = "allow", justification = "Basic inspection is safe")
prefix_rule(pattern = ["ls"], decision = "allow", justification = "Basic inspection is safe")
prefix_rule(pattern = ["tree"], decision = "allow", justification = "Basic inspection is safe")
prefix_rule(pattern = ["find"], decision = "allow", justification = "Searching project files is normal dev work")
prefix_rule(pattern = ["fd"], decision = "allow", justification = "Searching project files is normal dev work")
prefix_rule(pattern = ["rg"], decision = "allow", justification = "Code search is normal dev work")
prefix_rule(pattern = ["grep"], decision = "allow", justification = "Code search is normal dev work")
prefix_rule(pattern = ["awk"], decision = "allow", justification = "Text inspection is normal dev work")
prefix_rule(pattern = ["sed"], decision = "allow", justification = "Text inspection/editing is normal dev work")
prefix_rule(pattern = ["cat"], decision = "allow", justification = "File inspection is allowed except secret-specific forbidden rules")
prefix_rule(pattern = ["less"], decision = "allow", justification = "File inspection is allowed")
prefix_rule(pattern = ["head"], decision = "allow", justification = "File inspection is allowed")
prefix_rule(pattern = ["tail"], decision = "allow", justification = "File/log inspection is allowed")
prefix_rule(pattern = ["wc"], decision = "allow", justification = "File inspection is allowed")
prefix_rule(pattern = ["file"], decision = "allow", justification = "File inspection is allowed")
prefix_rule(pattern = ["stat"], decision = "allow", justification = "File inspection is allowed")
prefix_rule(pattern = ["du"], decision = "allow", justification = "Disk inspection is allowed")
prefix_rule(pattern = ["df"], decision = "allow", justification = "Disk inspection is allowed")
prefix_rule(pattern = ["ps"], decision = "allow", justification = "Process inspection is allowed")
prefix_rule(pattern = ["top"], decision = "allow", justification = "Process inspection is allowed")
prefix_rule(pattern = ["free"], decision = "allow", justification = "System inspection is allowed")
prefix_rule(pattern = ["uptime"], decision = "allow", justification = "System inspection is allowed")
prefix_rule(pattern = ["whoami"], decision = "allow", justification = "System inspection is allowed")
prefix_rule(pattern = ["id"], decision = "allow", justification = "System inspection is allowed")
prefix_rule(pattern = ["which"], decision = "allow", justification = "Tool inspection is allowed")
prefix_rule(pattern = ["command"], decision = "allow", justification = "Tool inspection is allowed")

# -------------------------
# Git
# -------------------------

prefix_rule(pattern = ["git", "status"], decision = "allow", justification = "Git inspection is normal dev work")
prefix_rule(pattern = ["git", "diff"], decision = "allow", justification = "Git inspection is normal dev work")
prefix_rule(pattern = ["git", "log"], decision = "allow", justification = "Git inspection is normal dev work")
prefix_rule(pattern = ["git", "show"], decision = "allow", justification = "Git inspection is normal dev work")
prefix_rule(pattern = ["git", "blame"], decision = "allow", justification = "Git inspection is normal dev work")
prefix_rule(pattern = ["git", "branch"], decision = "allow", justification = "Git branch work is allowed on dev")
prefix_rule(pattern = ["git", "remote"], decision = "allow", justification = "Git inspection is normal dev work")
prefix_rule(pattern = ["git", "rev-parse"], decision = "allow", justification = "Git inspection is normal dev work")
prefix_rule(pattern = ["git", "ls-files"], decision = "allow", justification = "Git inspection is normal dev work")
prefix_rule(pattern = ["git", "grep"], decision = "allow", justification = "Git inspection is normal dev work")
prefix_rule(pattern = ["git", "fetch"], decision = "allow", justification = "Fetching is allowed on dev")
prefix_rule(pattern = ["git", "add"], decision = "allow", justification = "Local Git changes are allowed on dev")
prefix_rule(pattern = ["git", "commit"], decision = "allow", justification = "Local commits are allowed on dev")
prefix_rule(pattern = ["git", "stash"], decision = "allow", justification = "Local stash work is allowed on dev")
prefix_rule(pattern = ["git", "switch"], decision = "allow", justification = "Branch switching is allowed on dev")
prefix_rule(pattern = ["git", "checkout"], decision = "allow", justification = "Checkout is allowed on dev")
prefix_rule(pattern = ["git", "merge"], decision = "allow", justification = "Merging is allowed on dev")
prefix_rule(pattern = ["git", "rebase"], decision = "allow", justification = "Rebasing is allowed on dev")
prefix_rule(pattern = ["git", "pull"], decision = "allow", justification = "Pulling is allowed on dev")
prefix_rule(pattern = ["git", "push"], decision = "allow", justification = "Pushing from a dev server is allowed by this policy")

# -------------------------
# C++ build and test
# -------------------------

prefix_rule(pattern = ["cmake"], decision = "allow", justification = "CMake is expected for this C++ project")
prefix_rule(pattern = ["make"], decision = "allow", justification = "Make builds are expected")
prefix_rule(pattern = ["ninja"], decision = "allow", justification = "Ninja builds are expected")
prefix_rule(pattern = ["ctest"], decision = "allow", justification = "Tests are expected")
prefix_rule(pattern = ["gcc"], decision = "allow", justification = "Compiler use is expected")
prefix_rule(pattern = ["g++"], decision = "allow", justification = "Compiler use is expected")
prefix_rule(pattern = ["clang"], decision = "allow", justification = "Compiler use is expected")
prefix_rule(pattern = ["clang++"], decision = "allow", justification = "Compiler use is expected")
prefix_rule(pattern = ["cc"], decision = "allow", justification = "Compiler use is expected")
prefix_rule(pattern = ["c++"], decision = "allow", justification = "Compiler use is expected")
prefix_rule(pattern = ["clang-format"], decision = "allow", justification = "Formatter use is expected")
prefix_rule(pattern = ["clang-tidy"], decision = "allow", justification = "Linter use is expected")
prefix_rule(pattern = ["cppcheck"], decision = "allow", justification = "Static analysis is expected")
prefix_rule(pattern = ["gdb"], decision = "allow", justification = "Debugging is expected")
prefix_rule(pattern = ["valgrind"], decision = "allow", justification = "Debugging/profiling is expected")
prefix_rule(pattern = ["perf"], decision = "allow", justification = "Profiling is expected")

# -------------------------
# Docker / Compose
# -------------------------

prefix_rule(pattern = ["docker"], decision = "allow", justification = "Docker is expected on this dev server")
prefix_rule(pattern = ["docker", "compose"], decision = "allow", justification = "Docker Compose is expected on this dev server")
prefix_rule(pattern = ["docker-compose"], decision = "allow", justification = "Legacy Docker Compose is expected on this dev server")

# -------------------------
# SSH / deploy / files
# -------------------------

prefix_rule(pattern = ["ssh"], decision = "allow", justification = "SSH to dev hosts is expected")
prefix_rule(pattern = ["scp"], decision = "allow", justification = "SCP deploy/copy is expected")
prefix_rule(pattern = ["rsync"], decision = "allow", justification = "Rsync deploy/copy is expected")
prefix_rule(pattern = ["curl"], decision = "allow", justification = "Network checks/downloads are expected on dev")
prefix_rule(pattern = ["wget"], decision = "allow", justification = "Network checks/downloads are expected on dev")
prefix_rule(pattern = ["nc"], decision = "allow", justification = "Network checks are expected on dev")
prefix_rule(pattern = ["ping"], decision = "allow", justification = "Network checks are expected on dev")
prefix_rule(pattern = ["dig"], decision = "allow", justification = "DNS checks are expected on dev")
prefix_rule(pattern = ["nslookup"], decision = "allow", justification = "DNS checks are expected on dev")

# -------------------------
# SQL / DB
# -------------------------

prefix_rule(pattern = ["mysql"], decision = "allow", justification = "Dev database work is expected")
prefix_rule(pattern = ["mariadb"], decision = "allow", justification = "Dev database work is expected")
prefix_rule(pattern = ["psql"], decision = "allow", justification = "Dev database work is expected")
prefix_rule(pattern = ["sqlite3"], decision = "allow", justification = "Dev database work is expected")
prefix_rule(pattern = ["mysqldump"], decision = "allow", justification = "Dev DB backup/export is expected")
prefix_rule(pattern = ["pg_dump"], decision = "allow", justification = "Dev DB backup/export is expected")
prefix_rule(pattern = ["pg_restore"], decision = "allow", justification = "Dev DB restore is allowed on dev")

# -------------------------
# Package managers / scripts
# -------------------------

prefix_rule(pattern = ["apt"], decision = "allow", justification = "Package installation is allowed on this dev server")
prefix_rule(pattern = ["apt-get"], decision = "allow", justification = "Package installation is allowed on this dev server")
prefix_rule(pattern = ["dnf"], decision = "allow", justification = "Package installation is allowed on this dev server")
prefix_rule(pattern = ["yum"], decision = "allow", justification = "Package installation is allowed on this dev server")
prefix_rule(pattern = ["pacman"], decision = "allow", justification = "Package installation is allowed on this dev server")
prefix_rule(pattern = ["python"], decision = "allow", justification = "Project scripts are allowed")
prefix_rule(pattern = ["python3"], decision = "allow", justification = "Project scripts are allowed")
prefix_rule(pattern = ["bash"], decision = "allow", justification = "Project scripts are allowed")
prefix_rule(pattern = ["sh"], decision = "allow", justification = "Project scripts are allowed")
prefix_rule(pattern = ["chmod"], decision = "allow", justification = "Changing script executable bits is allowed")
prefix_rule(pattern = ["mkdir"], decision = "allow", justification = "Filesystem changes are allowed in dev workspace")
prefix_rule(pattern = ["touch"], decision = "allow", justification = "Filesystem changes are allowed in dev workspace")
prefix_rule(pattern = ["cp"], decision = "allow", justification = "Filesystem changes are allowed in dev workspace")
prefix_rule(pattern = ["mv"], decision = "allow", justification = "Filesystem changes are allowed in dev workspace")
prefix_rule(pattern = ["rm"], decision = "allow", justification = "Deletes are allowed on this dev server except forbidden catastrophic patterns")

# -------------------------
# Hard blocks for secrets / catastrophic host wipes
# -------------------------

prefix_rule(pattern = ["cat", ".env"], decision = "forbidden", justification = "Do not print secrets. Inspect variable names without values instead.")
prefix_rule(pattern = ["cat", ".env.local"], decision = "forbidden", justification = "Do not print secrets. Inspect variable names without values instead.")
prefix_rule(pattern = ["cat", ".env.production"], decision = "forbidden", justification = "Do not print secrets. Inspect variable names without values instead.")
prefix_rule(pattern = ["cat", "id_rsa"], decision = "forbidden", justification = "Do not print private SSH keys.")
prefix_rule(pattern = ["cat", "id_ed25519"], decision = "forbidden", justification = "Do not print private SSH keys.")
prefix_rule(pattern = ["printenv"], decision = "forbidden", justification = "May expose secrets. Print only specific non-secret variables.")
prefix_rule(pattern = ["env"], decision = "forbidden", justification = "May expose secrets. Print only specific non-secret variables.")

prefix_rule(pattern = ["rm", "-rf", "/"], decision = "forbidden", justification = "Never wipe the host filesystem.")
prefix_rule(pattern = ["rm", "-rf", "/*"], decision = "forbidden", justification = "Never wipe the host filesystem.")
prefix_rule(pattern = ["rm", "-rf", "~"], decision = "forbidden", justification = "Never wipe the home directory.")
prefix_rule(pattern = ["rm", "-rf", "~/.ssh"], decision = "forbidden", justification = "Never remove SSH keys/config.")
prefix_rule(pattern = ["rm", "-rf", "/etc"], decision = "forbidden", justification = "Never wipe host system configuration.")
prefix_rule(pattern = ["rm", "-rf", "/var/lib/docker"], decision = "forbidden", justification = "Never wipe Docker storage directly.")
prefix_rule(pattern = ["mkfs"], decision = "forbidden", justification = "Never format disks from Codex.")
prefix_rule(pattern = ["dd"], decision = "forbidden", justification = "Disk overwrite commands are blocked.")
EOF_RULES
)

append_agents_if_needed "$AGENTS_FILE" "$AGENTS_CONTENT"

if [[ "$MODE" == "yolo" ]]; then
  write_file "$CONFIG_FILE" "$CONFIG_YOLO_CONTENT"
else
  write_file "$CONFIG_FILE" "$CONFIG_SAFE_CONTENT"
fi

write_file "$RULES_DIR/default.rules" "$RULES_CONTENT"

# Repo convenience wrappers.
if [[ "$SCOPE" == "repo" ]]; then
  cat > ./codex-dev-yolo.sh <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
exec codex --sandbox danger-full-access --ask-for-approval never "$@"
EOF
  chmod +x ./codex-dev-yolo.sh

  cat > ./codex-dev-safe.sh <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
exec codex --sandbox workspace-write --ask-for-approval on-request "$@"
EOF
  chmod +x ./codex-dev-safe.sh
fi

cat <<EOF

Installed Codex dev-server policy.

Scope: $SCOPE
Mode:  $MODE

Files:
- $AGENTS_FILE
- $CONFIG_FILE
- $RULES_DIR/default.rules

Next:
1. Restart Codex.
2. Run Codex from the repo root.
3. Verify instructions:
   codex --ask-for-approval never "Summarize the current instructions."

EOF

if [[ "$SCOPE" == "repo" ]]; then
  cat <<'EOF'
Convenience launchers:
  ./codex-dev-yolo.sh
  ./codex-dev-safe.sh

EOF
fi
