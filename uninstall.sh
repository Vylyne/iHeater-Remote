#!/bin/bash
# Removes what install.sh added: the moonraker.conf sections, the include, the
# two module links and, only if you say so, the config file.
#
# Idempotent: safe to re-run. It never edits a file without a backup, never
# deletes the config file without asking, and never restarts Klipper during a
# print.

INSTALL_PATH="${INSTALL_PATH:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}"; source "${INSTALL_PATH}/scripts/common.sh"

# inside_install PATH: succeeds when PATH lies under INSTALL_PATH. It walks up
# the parents and compares each with -ef, which compares the files themselves,
# so two spellings of one path (C:/x and /c/x on Git Bash) still match.
function inside_install {
    local dir="$1" parent
    while :; do
        if [ -e "${dir}" ] && [ "${dir}" -ef "${INSTALL_PATH}" ]; then
            return 0
        fi
        parent="$(dirname "${dir}")"
        if [ "${parent}" = "${dir}" ] || [ "${parent}" = "." ]; then
            return 1
        fi
        dir="${parent}"
    done
}

# unlink_module DEST: remove DEST only when it is a link into this checkout.
function unlink_module {
    local dest="$1" target
    if [ -L "${dest}" ]; then
        # readlink -f fails when a parent of the target is missing. That must
        # not abort the run: use the raw target and treat it as not ours.
        if target="$(link_target "${dest}" 2>/dev/null)" && [ -n "${target}" ] \
            && inside_install "${target}"; then
            rm "${dest}"
            CHANGED_LINKS=1
            ok "removed ${dest}"
        else
            [ -n "${target}" ] || target="$(readlink "${dest}" || true)"
            warn "${dest} points to ${target}, which isn't in ${INSTALL_PATH}; left it"
            note "Remove it yourself if you want it gone."
        fi
    elif [ -e "${dest}" ]; then
        skip "${dest} is a regular file, not a link"
        note "Left alone; remove it yourself if you want it gone."
    else
        skip "${dest} not present"
    fi
}

function step_moonraker_conf {
    section "moonraker.conf"
    if [ ! -f "${MOONRAKER_CONF}" ]; then
        skip "${MOONRAKER_CONF} not found"
        return 0
    fi
    local name
    for name in "iheater_remote" "update_manager iheater-remote"; do
        if helper has-section "${MOONRAKER_CONF}" "${name}"; then
            backup "${MOONRAKER_CONF}"
            helper remove-section "${MOONRAKER_CONF}" "${name}"
            CHANGED_MOONRAKER=1
            ok "removed [${name}]"
        else
            skip "[${name}] not present"
        fi
    done
}

function step_printer_cfg {
    section "printer.cfg"
    if [ ! -f "${PRINTER_CFG}" ]; then
        skip "${PRINTER_CFG} not found"
        return 0
    fi
    local how
    if ! how="$(helper has-include "${PRINTER_CFG}" "${CONFIG_DIR}" "${ADDON_REL}")"; then
        skip "printer.cfg has no include of the config"
        return 0
    fi
    if [ "${how}" != "exact" ]; then
        warn "printer.cfg loads it through another include line; left it"
        return 0
    fi
    if ask "Remove the include from printer.cfg?" y; then
        backup "${PRINTER_CFG}"
        helper remove-include "${PRINTER_CFG}" "${ADDON_REL}"
        CHANGED_KLIPPER=1
        ok "removed ${INCLUDE_LINE}"
    else
        warn "printer.cfg not changed."
    fi
}

function step_config_file {
    section "Config file"
    if [ ! -e "${ADDON_CFG}" ]; then
        skip "${ADDON_CFG} not present"
        return 0
    fi
    if ask "Delete addons/iheater_remote.cfg? A backup is kept." n; then
        backup "${ADDON_CFG}"
        rm "${ADDON_CFG}"
        CHANGED_KLIPPER=1
        ok "deleted ${ADDON_CFG}"
    else
        skip "kept ${ADDON_CFG}"
    fi
}

# Warns, and stops Klipper restarting by default, when printer.cfg still loads
# a config file that needs the module this run unlinks.
function step_still_loaded {
    [ -f "${PRINTER_CFG}" ] || return 0
    local name=""
    if [ -e "${ADDON_CFG}" ] \
        && helper has-include "${PRINTER_CFG}" "${CONFIG_DIR}" "${ADDON_REL}" >/dev/null; then
        name="${ADDON_CFG}"
    elif [ -e "${CONFIG_DIR}/iheater_remote.cfg" ] \
        && helper has-include "${PRINTER_CFG}" "${CONFIG_DIR}" "iheater_remote.cfg" >/dev/null; then
        name="${CONFIG_DIR}/iheater_remote.cfg"
    fi
    if [ -n "${name}" ]; then
        warn "printer.cfg still loads ${name}."
        note "Klipper will stop with a config error until you remove that include line"
        note "from printer.cfg (or delete the file). Edit printer.cfg, then restart Klipper."
        KLIPPER_RESTART_DEFAULT=n
    fi
}

function step_links {
    section "Modules"
    unlink_module "${KLIPPER_PATH}/klippy/extras/heater_iheater_remote.py"
    unlink_module "${MOONRAKER_PATH}/moonraker/components/iheater_remote.py"
}

section "iHeater Remote"
note "Removing what ${INSTALL_PATH} installed"
preflight
step_moonraker_conf
step_printer_cfg
step_config_file
step_still_loaded
step_links
restart_services
