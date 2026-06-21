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
