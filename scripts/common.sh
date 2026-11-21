#!/bin/bash
# Shared by install.sh and uninstall.sh: paths, output helpers, prompts,
# preflight checks and the restart step. Source it; do not run it.
#
# The caller sets INSTALL_PATH (the repo checkout) before sourcing.

set -eu
export LC_ALL=C

KLIPPER_PATH="${KLIPPER_PATH:-${HOME}/klipper}"
MOONRAKER_PATH="${MOONRAKER_PATH:-${HOME}/moonraker}"
PRINTER_DATA="${PRINTER_DATA:-${HOME}/printer_data}"
MOONRAKER_PORT="${MOONRAKER_PORT:-7125}"
PYTHON_BIN="${PYTHON_BIN:-$(command -v python3 || true)}"

CONFIG_DIR="${PRINTER_DATA}/config"
ADDON_CFG="${CONFIG_DIR}/addons/iheater_remote.cfg"
PRINTER_CFG="${CONFIG_DIR}/printer.cfg"
MOONRAKER_CONF="${CONFIG_DIR}/moonraker.conf"

# What the include line and the helper call "this file", relative to CONFIG_DIR.
ADDON_REL="addons/iheater_remote.cfg"
INCLUDE_LINE="[include ${ADDON_REL}]"

# One stamp per run, so every backup from this run carries the same suffix.
STAMP="$(date +%Y%m%d-%H%M%S)"

# What this run changed. restart_services reads these.
CHANGED_LINKS=0
CHANGED_MOONRAKER=0
CHANGED_KLIPPER=0

# What "Restart Klipper now?" offers when the printer is idle. A script sets
# this to n, after printing the reason, when a restart would leave Klipper
# with a config error. An active print still wins: Klipper is not restarted.
KLIPPER_RESTART_DEFAULT=y

# Whether each service unit exists; set by preflight.
HAVE_KLIPPER_SVC=0
HAVE_MOONRAKER_SVC=0

# --- output ---------------------------------------------------------------
# Same layout as mcu-updater's install.sh: a status word in an eight-column
# gutter, `note` indented to match, colour only on a terminal and never when
# NO_COLOR is set.
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    C_HEAD=$'\033[1m'
    C_DIM=$'\033[2m'
    C_OK=$'\033[32m'
    C_SKIP=$'\033[36m'
    C_WARN=$'\033[33m'
    C_ERR=$'\033[31m'
    C_OFF=$'\033[0m'
else
    C_HEAD=""
    C_DIM=""
    C_OK=""
    C_SKIP=""
    C_WARN=""
    C_ERR=""
    C_OFF=""
fi

# A phase header: one blank line before, none after.
function section {
    printf '\n%s── %-11s ─────────────────────────────────%s\n' \
        "${C_HEAD}" "$1" "${C_OFF}"
}

function ok {
    printf '  %sok%s    %s\n' "${C_OK}" "${C_OFF}" "$*"
}

function skip {
    printf '  %sskip%s  %s\n' "${C_SKIP}" "${C_OFF}" "$*"
}

function warn {
    printf '  %swarn%s  %s\n' "${C_WARN}" "${C_OFF}" "$*"
}

function err {
    printf '  %serror%s %s\n' "${C_ERR}" "${C_OFF}" "$*"
}

# Something in progress whose result gets its own line afterwards.
function step {
    printf '  %s...%s   %s\n' "${C_DIM}" "${C_OFF}" "$*"
}

# Continuation text, indented to the gutter.
function note {
    printf '        %s%s%s\n' "${C_DIM}" "$*" "${C_OFF}"
}

# ask PROMPT DEFAULT: a yes/no prompt. DEFAULT is y or n. An empty answer, or
# end of input (no terminal), takes the default. The prompt goes to stderr so
# it never mixes into captured output.
function ask {
    local prompt="$1" default="$2" answer="" hint="[Y/n]"
    if [ "${default}" = "n" ]; then
        hint="[y/N]"
    fi
    while :; do
        printf '  ?     %s %s: ' "${prompt}" "${hint}" >&2
        if ! read -r answer; then
            # End of input (no terminal): take the default.
            printf '
' >&2
            answer=""
        fi
        case "${answer}" in
            "")
                [ "${default}" = "y" ] && return 0
                return 1
                ;;
            [yY] | [yY][eE][sS]) return 0 ;;
            [nN] | [nN][oO]) return 1 ;;
        esac
        warn "please answer y or n."
    done
}

# ask_value PROMPT: prints the line read, or nothing at end of input.
function ask_value {
    local answer=""
    printf '  ?     %s: ' "$1" >&2
    read -r answer || answer=""
    if [ -z "${answer}" ]; then
        printf '\n' >&2
    fi
    printf '%s\n' "${answer}"
}

# --- helpers --------------------------------------------------------------

# helper ARGS...: run the config helper. Its exit status is passed through.
function helper {
    "${PYTHON_BIN}" "${INSTALL_PATH}/scripts/iheater_config.py" "$@"
}

BACKED_UP=" "

# backup FILE: copy to FILE.bak-STAMP, once per file per run.
function backup {
    local file="$1"
    case "${BACKED_UP}" in
        *" ${file} "*) return 0 ;;
    esac
    cp -p "${file}" "${file}.bak-${STAMP}"
    BACKED_UP="${BACKED_UP}${file} "
    note "backup: ${file}.bak-${STAMP}"
}

# link_target LINK: print where an existing symlink finally points.
function link_target {
    readlink -f "$1"
}

# --- preflight ------------------------------------------------------------

function service_exists {
    systemctl list-unit-files --no-legend "$1.service" 2>/dev/null \
        | grep -q "$1.service"
}

function preflight {
    section "Checks"
    if [ "$(id -u)" -eq 0 ]; then
        err "run this as your normal user, not root. sudo is used only to restart services."
        exit 1
    fi

    if [ -z "${PYTHON_BIN}" ]; then
        err "python3 not found on PATH."
        exit 1
    fi
    if ! "${PYTHON_BIN}" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 7) else 1)'; then
        err "python3 3.7 or later is required, found $("${PYTHON_BIN}" -V 2>&1)"
        exit 1
    fi
    ok "$("${PYTHON_BIN}" -V 2>&1) at ${PYTHON_BIN}"

    if [ ! -d "${KLIPPER_PATH}/klippy/extras" ]; then
        err "${KLIPPER_PATH}/klippy/extras not found. Set KLIPPER_PATH."
        exit 1
    fi
    ok "Klipper at ${KLIPPER_PATH}"

    if [ ! -d "${MOONRAKER_PATH}/moonraker/components" ]; then
        err "${MOONRAKER_PATH}/moonraker/components not found. Set MOONRAKER_PATH."
        exit 1
    fi
    ok "Moonraker at ${MOONRAKER_PATH}"

    if [ ! -d "${CONFIG_DIR}" ]; then
        err "${CONFIG_DIR} not found. Set PRINTER_DATA."
        exit 1
    fi
    ok "config in ${CONFIG_DIR}"

    if service_exists klipper; then
        HAVE_KLIPPER_SVC=1
    else
        warn "klipper.service not found; Klipper will not be restarted."
    fi
    if service_exists moonraker; then
        HAVE_MOONRAKER_SVC=1
    else
        warn "moonraker.service not found; Moonraker will not be restarted."
    fi
}

# --- restart --------------------------------------------------------------

# restart_services: Moonraker when a link or moonraker.conf changed, Klipper
# when a link, printer.cfg or the config file changed. The print state is read
# before anything restarts, and Klipper is never restarted during a print.
function restart_services {
    local need_moonraker=0 need_klipper=0 state=""
    if [ "${CHANGED_LINKS}" = 1 ] || [ "${CHANGED_MOONRAKER}" = 1 ]; then
        need_moonraker=1
    fi
    if [ "${CHANGED_LINKS}" = 1 ] || [ "${CHANGED_KLIPPER}" = 1 ]; then
        need_klipper=1
    fi

    section "Restart"
    if [ "${need_moonraker}" = 0 ] && [ "${need_klipper}" = 0 ]; then
        skip "nothing changed, no restart needed"
        return 0
    fi

    if [ "${need_klipper}" = 1 ] && [ "${HAVE_KLIPPER_SVC}" = 1 ]; then
        state="$(curl -s -m 5 \
            "http://localhost:${MOONRAKER_PORT}/printer/objects/query?print_stats" \
            2>/dev/null | helper print-state 2>/dev/null)" || state=""
    fi

    if [ "${need_moonraker}" = 1 ]; then
        if [ "${HAVE_MOONRAKER_SVC}" = 0 ]; then
            skip "Moonraker restart (no moonraker.service)"
        else
            step "restarting Moonraker"
            if sudo systemctl restart moonraker; then
                ok "Moonraker restarted"
            else
                warn "could not restart Moonraker; run: sudo systemctl restart moonraker"
            fi
        fi
    fi

    if [ "${need_klipper}" = 1 ]; then
        if [ "${HAVE_KLIPPER_SVC}" = 0 ]; then
            skip "Klipper restart (no klipper.service)"
            return 0
        fi
        local go=0
        case "${state}" in
            printing | paused)
                warn "the printer is ${state}; Klipper was not restarted."
                note "Run FIRMWARE_RESTART after the print."
                ;;
            "")
                warn "could not read the print state."
                if ask "Restart Klipper now?" n; then go=1; fi
                ;;
            *)
                if ask "Restart Klipper now?" "${KLIPPER_RESTART_DEFAULT}"; then go=1; fi
                ;;
        esac
        if [ "${go}" = 0 ]; then
            case "${state}" in
                printing | paused) ;;
                *) note "Restart Klipper (or run FIRMWARE_RESTART) before using the new config." ;;
            esac
        else
            step "restarting Klipper"
            if sudo systemctl restart klipper; then
                ok "Klipper restarted"
            else
                warn "could not restart Klipper; run: sudo systemctl restart klipper"
            fi
        fi
    fi
}
