#!/bin/bash
# Installs the iHeater Remote's Klipper and Moonraker parts.
#
# Idempotent: safe to re-run. It never edits a file without a backup, and
# never restarts Klipper during a print.

INSTALL_PATH="${INSTALL_PATH:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}"; source "${INSTALL_PATH}/scripts/common.sh"

SHIPPED_CFG="${INSTALL_PATH}/klipper/iheater_remote.cfg"
CHANGES_MD="${INSTALL_PATH}/klipper/CONFIG_CHANGES.md"

# link_module SRC DEST: symlink SRC at DEST, asking before replacing anything.
function link_module {
    local src="$1" dest="$2"
    if [ -L "${dest}" ]; then
        # -ef compares the files themselves, so two spellings of one path
        # (as on Git Bash) still match.
        if [ "${dest}" -ef "${src}" ]; then
            skip "${dest} already linked"
            return 0
        fi
        warn "${dest} points to $(readlink "${dest}")"
        if ask "Replace it with a link to ${src}?" y; then
            rm "${dest}"
            ln -s "${src}" "${dest}"
            CHANGED_LINKS=1
            ok "relinked ${dest}"
        else
            warn "left ${dest} alone; the module may be out of date."
        fi
    elif [ -e "${dest}" ]; then
        warn "${dest} is a regular file, not a link"
        if ask "Move it aside and link ${src}?" y; then
            mv "${dest}" "${dest}.bak-${STAMP}"
            note "moved to ${dest}.bak-${STAMP}"
            ln -s "${src}" "${dest}"
            CHANGED_LINKS=1
            ok "linked ${dest}"
        else
            warn "left ${dest} alone; the module may be out of date."
        fi
    else
        ln -s "${src}" "${dest}"
        CHANGED_LINKS=1
        ok "linked ${dest}"
    fi
}

function step_link_modules {
    section "Modules"
    link_module "${INSTALL_PATH}/klipper/heater_iheater_remote.py" \
        "${KLIPPER_PATH}/klippy/extras/heater_iheater_remote.py"
    link_module "${INSTALL_PATH}/moonraker/iheater_remote.py" \
        "${MOONRAKER_PATH}/moonraker/components/iheater_remote.py"
}

# Repeats until the input is blank (or ends) or the token passes the check.
function prompt_token {
    local token
    while :; do
        token="$(ask_value "Pairing token from the Remote's Setup page (blank to set it later)")"
        if [ -z "${token}" ]; then
            return 0
        fi
        if helper check-token -- "${token}"; then
            helper set-key "${ADDON_CFG}" remote_token -- "${token}"
            ok "token set"
            return 0
        fi
        warn "that is not a pairing token (8 characters, 0-9 and A-Z without I, L, O, U)"
    done
}

function prompt_sensor {
    local current sensor
    current="$(helper get-key "${ADDON_CFG}" sensor || true)"
    sensor="$(ask_value "Chamber sensor (blank keeps ${current:-the shipped default})")"
    if [ -n "${sensor}" ]; then
        helper set-key "${ADDON_CFG}" sensor -- "${sensor}"
        ok "sensor set to ${sensor}"
    fi
}

# The manual install the README described before install.sh: the config beside
# printer.cfg, loaded by [include iheater_remote.cfg].
OLD_CFG="${CONFIG_DIR}/iheater_remote.cfg"
OLD_INCLUDE="iheater_remote.cfg"
# Set to 1 when the user keeps that older copy, which Klipper still loads.
OLD_KEPT=0

function step_old_layout {
    if [ ! -e "${OLD_CFG}" ] || [ -e "${ADDON_CFG}" ]; then
        return 0
    fi
    section "Older install"
    warn "found an older install: ${OLD_CFG}"
    if ask "Move it to ${ADDON_REL} and update the include?" y; then
        mkdir -p "$(dirname "${ADDON_CFG}")"
        backup "${OLD_CFG}"
        mv "${OLD_CFG}" "${ADDON_CFG}"
        ok "moved ${OLD_CFG} to ${ADDON_CFG}"
        CHANGED_KLIPPER=1
        if [ -f "${PRINTER_CFG}" ] \
            && [ "$(helper has-include "${PRINTER_CFG}" "${CONFIG_DIR}" "${OLD_INCLUDE}" || true)" = "exact" ]; then
            backup "${PRINTER_CFG}"
            helper remove-include "${PRINTER_CFG}" "${OLD_INCLUDE}"
            if ! helper has-include "${PRINTER_CFG}" "${CONFIG_DIR}" "${ADDON_REL}" >/dev/null; then
                helper add-include "${PRINTER_CFG}" "${ADDON_REL}"
            fi
            ok "printer.cfg now includes ${ADDON_REL} instead of ${OLD_INCLUDE}"
        fi
    else
        OLD_KEPT=1
        warn "left ${OLD_CFG} alone."
    fi
}

# Whether the file at PATH is one that should never count as the user's config:
# our own file, a backup of ours, or a SAVE_CONFIG copy of printer.cfg.
function ignored_cfg {
    case "$1" in
        "${ADDON_CFG}") return 0 ;;
        *.bak-*) return 0 ;;
    esac
    [[ "$1" =~ /printer-[0-9]+_[0-9]+\.cfg$ ]]
}

# regex_escape TEXT: escape TEXT for use inside a grep -E pattern.
function regex_escape {
    printf '%s' "$1" | sed 's/[][\.*^$+?(){}|\\/]/\\&/g'
}

# Sets KLIPPER_RESTART_DEFAULT=n when the chosen sensor has no section.
function step_sensor_check {
    local sensor word pattern="" file found=0 files
    sensor="$(helper get-key "${ADDON_CFG}" sensor 2>/dev/null || true)"
    [ -n "${sensor}" ] || return 0
    for word in ${sensor}; do
        [ -z "${pattern}" ] || pattern="${pattern}[[:space:]]+"
        pattern="${pattern}$(regex_escape "${word}")"
    done
    pattern="^\\[[[:space:]]*${pattern}[[:space:]]*\\]"
    files="$(grep -RlE --include='*.cfg' -e "${pattern}" "${CONFIG_DIR}" 2>/dev/null || true)"
    while IFS= read -r file; do
        [ -n "${file}" ] || continue
        if ! ignored_cfg "${file}"; then
            found=1
            break
        fi
    done <<< "${files}"
    if [ "${found}" = 1 ]; then
        ok "found [${sensor}] in your config"
    else
        warn "No [${sensor}] section found in your config. Klipper will stop with a config error until it exists; set sensor: in ${ADDON_REL}."
        KLIPPER_RESTART_DEFAULT=n
    fi
}

function step_config_file {
    section "Config file"
    mkdir -p "$(dirname "${ADDON_CFG}")"
    local shipped_v installed_v line carried changes
    shipped_v="$(helper version "${SHIPPED_CFG}")"

    if [ ! -e "${ADDON_CFG}" ]; then
        cp "${SHIPPED_CFG}" "${ADDON_CFG}"
        CHANGED_KLIPPER=1
        ok "created ${ADDON_CFG}"
        prompt_token
        prompt_sensor
        return 0
    fi

    installed_v="$(helper version "${ADDON_CFG}")"
    if [ "${installed_v}" -ge "${shipped_v}" ]; then
        skip "${ADDON_CFG} is version ${installed_v}"
        return 0
    fi

    warn "${ADDON_CFG} is version ${installed_v}; this release ships version ${shipped_v}."
    note "Changes since your version:"
    changes="$(helper changes "${CHANGES_MD}" "${installed_v}" "${shipped_v}" || true)"
    while IFS= read -r line; do
        note "  ${line}"
    done <<< "${changes}"
    if ask "Replace it with version ${shipped_v}? Your token, sensor and max_target are kept." n; then
        backup "${ADDON_CFG}"
        cp "${SHIPPED_CFG}" "${ADDON_CFG}"
        if carried="$(helper carry "${ADDON_CFG}.bak-${STAMP}" "${ADDON_CFG}")"; then
            while IFS= read -r line; do
                case "${line}" in
                    carried\ *) ok "kept ${line#carried }" ;;
                    renamed\ *)
                        warn "your heater was named '${line#renamed }'; the new file uses 'chamber'."
                        note "The backup holds the old name."
                        ;;
                esac
            done <<< "${carried}"
            CHANGED_KLIPPER=1
            ok "replaced ${ADDON_CFG}"
        else
            cp "${ADDON_CFG}.bak-${STAMP}" "${ADDON_CFG}"
            err "could not carry your values over; the old file was put back."
        fi
    else
        warn "kept your older config. See klipper/CONFIG_CHANGES.md for what changed."
    fi
}

# Sets MACROS_FOUND to the number of M141/M191 macros outside our own file.
MACROS_FOUND=0
function step_macro_check {
    section "Macros"
    local hits line file num text
    if [ "${OLD_KEPT}" = 1 ]; then
        warn "${OLD_CFG} is an older copy of this config that Klipper still loads."
        note "Its macros are not counted below. Two copies would clash, so move or delete one."
    fi
    hits="$(grep -RniE --include='*.cfg' '^\[gcode_macro (M141|M191)\]' \
        "${CONFIG_DIR}" 2>/dev/null || true)"
    while IFS= read -r line; do
        [ -n "${line}" ] || continue
        # path:line:text, where the path may itself hold a colon.
        if [[ "${line}" =~ ^(.*):([0-9]+):(.*)$ ]]; then
            file="${BASH_REMATCH[1]}"
            num="${BASH_REMATCH[2]}"
            text="${BASH_REMATCH[3]}"
            if ignored_cfg "${file}"; then
                continue
            fi
            if [ "${OLD_KEPT}" = 1 ] && [ "${file}" = "${OLD_CFG}" ]; then
                continue
            fi
            warn "${file}:${num} defines ${text}"
        else
            warn "${line}"
        fi
        MACROS_FOUND=$((MACROS_FOUND + 1))
    done <<< "${hits}"
    if [ "${MACROS_FOUND}" = 0 ]; then
        ok "no other M141 or M191 macros"
    else
        note "iheater_remote.cfg defines both. Klipper merges duplicate sections, so whichever loads last silently replaces the other."
    fi
}

function step_printer_cfg {
    section "printer.cfg"
    if [ ! -f "${PRINTER_CFG}" ]; then
        warn "${PRINTER_CFG} not found; add ${INCLUDE_LINE} yourself."
        return 0
    fi
    local how default=y
    if how="$(helper has-include "${PRINTER_CFG}" "${CONFIG_DIR}" "${ADDON_REL}")"; then
        skip "printer.cfg already includes the config (${how})"
        return 0
    fi
    if [ "${MACROS_FOUND}" != 0 ]; then
        default=n
        note "Delete or rename your macros above, then re-run install.sh."
    fi
    if [ "${OLD_KEPT}" = 1 ]; then
        default=n
        note "Including ${ADDON_REL} as well would load the config twice."
    fi
    if ask "Add ${INCLUDE_LINE} to printer.cfg?" "${default}"; then
        backup "${PRINTER_CFG}"
        helper add-include "${PRINTER_CFG}" "${ADDON_REL}"
        CHANGED_KLIPPER=1
        ok "added ${INCLUDE_LINE}"
    else
        warn "printer.cfg not changed; Klipper will not load the config until you include it."
    fi
}

function step_moonraker_conf {
    section "moonraker.conf"
    if [ ! -f "${MOONRAKER_CONF}" ]; then
        warn "${MOONRAKER_CONF} not found; add the sections from scripts/ yourself."
        return 0
    fi
    local origin="" add_component=0 add_update=0 have_update=0
    if ! helper has-section "${MOONRAKER_CONF}" "iheater_remote"; then
        add_component=1
    fi
    if helper has-section "${MOONRAKER_CONF}" "update_manager iheater-remote"; then
        have_update=1
    else
        origin="$(git -C "${INSTALL_PATH}" remote get-url origin 2>/dev/null || true)"
        if [ -n "${origin}" ]; then
            add_update=1
        else
            warn "no git origin for ${INSTALL_PATH}; skipping the update manager block."
            note "Update with git pull by hand."
        fi
    fi

    if [ "${add_component}" = 1 ] || [ "${add_update}" = 1 ]; then
        backup "${MOONRAKER_CONF}"
        CHANGED_MOONRAKER=1
    fi
    if [ "${add_component}" = 1 ]; then
        helper append-block "${MOONRAKER_CONF}" \
            "${INSTALL_PATH}/scripts/moonraker-component.conf"
        ok "added [iheater_remote]"
    else
        skip "[iheater_remote] already present"
    fi
    if [ "${add_update}" = 1 ]; then
        helper append-block "${MOONRAKER_CONF}" \
            "${INSTALL_PATH}/scripts/moonraker-update-manager.conf" \
            "INSTALL_PATH=${INSTALL_PATH}" "ORIGIN=${origin}"
        ok "added [update_manager iheater-remote]"
    elif [ "${have_update}" = 1 ]; then
        skip "[update_manager iheater-remote] already present"
    fi
}

function step_next {
    section "Next steps"
    note "Config: ${ADDON_CFG}"
    if [ -z "$(helper get-key "${ADDON_CFG}" remote_token 2>/dev/null || true)" ]; then
        warn "no pairing token yet. Copy it from the Remote's Setup page into remote_token,"
        note "then restart Klipper."
    else
        ok "pairing token set"
    fi
    note "Check the link in the Klipper console: printer[\"heater_iheater_remote chamber\"].linked"
    note "and on the Remote's link diagnostics."
    note "After an update, read klipper/CONFIG_CHANGES.md and re-run ./install.sh if it lists changes."
}

section "iHeater Remote"
note "Installing from ${INSTALL_PATH}"
preflight
step_link_modules
step_old_layout
step_config_file
step_sensor_check
step_macro_check
step_printer_cfg
step_moonraker_conf
restart_services
step_next
