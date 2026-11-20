# Virtual heater for an externally regulated chamber heater (iHeater Remote)
#
# Registers a Klipper heater with no MCU pin, no PWM and no verify_heater.
# Its temperature comes from an existing sensor object. iHeater Remote reads
# the heater object directly over Moonraker and pairs with it by the
# remote_token option (reported in get_status).
#
# This file may be distributed under the terms of the GNU GPLv3 license.

import re

_TOKEN_RE = re.compile(r'^[0-9A-HJKMNP-TV-Z]{8}$')


class HeaterVirtual:
    def __init__(self, config):
        self.printer = config.get_printer()
        self.full_name = config.get_name()
        self.short_name = self.full_name.split()[-1]
        self.sensor_name = config.get('sensor')
        self.max_target = config.getfloat('max_target', 80.,
                                          minval=45., maxval=90.)
        # There is no shutdown threshold here; the referenced sensor has its
        # own. Reading min_temp/max_temp with defaults publishes them in
        # configfile.settings, which Mainsail and Fluidd use to limit the
        # target box (a missing max_temp makes every target "too high").
        if config.get('max_temp', None) is not None:
            raise config.error(
                "[%s] use max_target, not max_temp" % (self.full_name,))
        config.getfloat('min_temp', 0.)
        config.getfloat('max_temp', self.max_target)
        token = config.get('remote_token', '').upper().replace('-', '')
        if token and not _TOKEN_RE.match(token):
            raise config.error(
                "[%s] remote_token must be 8 characters from "
                "0123456789ABCDEFGHJKMNPQRSTVWXYZ (case-insensitive, "
                "hyphens ignored)" % (self.full_name,))
        self.remote_token = token
        self.sensor = None
        self.target = 0.
        # Register with the heaters module
        self.pheaters = self.printer.load_object(config, 'heaters')
        if self.short_name in self.pheaters.heaters:
            raise config.error("Heater %s already registered"
                               % (self.short_name,))
        if token:
            for other in self.pheaters.heaters.values():
                if (isinstance(other, HeaterVirtual)
                        and other.remote_token == token):
                    raise config.error(
                        "[%s] remote_token %s-%s is already used by "
                        "heater_virtual %s"
                        % (self.full_name, token[:4], token[4:],
                           other.short_name))
        self.pheaters.heaters[self.short_name] = self
        self.pheaters.available_heaters.append(self.full_name)
        self.pheaters.register_sensor(config, self)
        gcode = self.printer.lookup_object('gcode')
        gcode.register_mux_command('SET_HEATER_TEMPERATURE', 'HEATER',
                                   self.short_name,
                                   self.cmd_SET_HEATER_TEMPERATURE,
                                   desc=self.cmd_SET_HEATER_TEMPERATURE_help)
        self.printer.register_event_handler('klippy:connect',
                                            self._handle_connect)
        self.printer.register_event_handler('klippy:shutdown',
                                            self._handle_shutdown)

    def _handle_connect(self):
        self.sensor = self.printer.lookup_object(self.sensor_name, None)
        if self.sensor is None:
            raise self.printer.config_error(
                "[%s] sensor '%s' not found"
                % (self.full_name, self.sensor_name))

    def _handle_shutdown(self):
        self.target = 0.

    def _sensor_temp(self, eventtime):
        if self.sensor is None:
            return 0.
        return self.sensor.get_temp(eventtime)[0]

    # Heater interface used by klippy/extras/heaters.py
    def set_temp(self, degrees):
        if degrees < 0. or degrees > self.max_target:
            raise self.printer.command_error(
                "Requested temperature (%.1f) out of range (0-%.1f)"
                % (degrees, self.max_target))
        self.target = degrees

    def get_temp(self, eventtime):
        return self._sensor_temp(eventtime), self.target

    def check_busy(self, eventtime):
        temp, target = self.get_temp(eventtime)
        return target > 0. and temp < target

    def get_max_power(self):
        return 1.

    def get_status(self, eventtime):
        temp, target = self.get_temp(eventtime)
        return {'temperature': round(temp, 2), 'target': target,
                'power': 1. if target > 0. else 0.,
                'remote_token': self.remote_token}

    cmd_SET_HEATER_TEMPERATURE_help = "Sets a heater temperature"

    def cmd_SET_HEATER_TEMPERATURE(self, gcmd):
        temp = gcmd.get_float('TARGET', 0.)
        self.pheaters.set_temperature(self, temp)


def load_config_prefix(config):
    return HeaterVirtual(config)
