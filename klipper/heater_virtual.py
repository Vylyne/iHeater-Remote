# Virtual heater for an externally regulated chamber heater (iHeater Remote)
#
# Registers a Klipper heater with no MCU pin, no PWM and no verify_heater.
# Its temperature comes from an existing sensor object; its target is
# mirrored into a gcode_macro (default VIRTUAL_CHAMBER) that iHeater Remote
# reads over Moonraker.
#
# This file may be distributed under the terms of the GNU GPLv3 license.


class HeaterVirtual:
    def __init__(self, config):
        self.printer = config.get_printer()
        self.full_name = config.get_name()
        self.short_name = self.full_name.split()[-1]
        self.sensor_name = config.get('sensor')
        self.max_target = config.getfloat('max_target', 80.,
                                          minval=45., maxval=90.)
        self.macro_name = 'gcode_macro ' + config.get('mirror_macro',
                                                      'VIRTUAL_CHAMBER')
        self.sensor = None
        self.macro = None
        self.target = 0.
        # Register with the heaters module
        self.pheaters = self.printer.load_object(config, 'heaters')
        if self.short_name in self.pheaters.heaters:
            raise config.error("Heater %s already registered"
                               % (self.short_name,))
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
        self.printer.register_event_handler('klippy:ready',
                                            self._handle_ready)
        self.printer.register_event_handler('klippy:shutdown',
                                            self._handle_shutdown)

    def _handle_connect(self):
        self.sensor = self.printer.lookup_object(self.sensor_name, None)
        if self.sensor is None:
            raise self.printer.config_error(
                "[%s] sensor '%s' not found"
                % (self.full_name, self.sensor_name))
        self.macro = self.printer.lookup_object(self.macro_name, None)
        if self.macro is None:
            raise self.printer.config_error(
                "[%s] requires [%s] (include iheater_remote.cfg)"
                % (self.full_name, self.macro_name))
        self._mirror(self.printer.get_reactor().monotonic())

    def _handle_ready(self):
        reactor = self.printer.get_reactor()
        reactor.register_timer(self._mirror_timer, reactor.NOW)

    def _handle_shutdown(self):
        self.target = 0.
        self._mirror(self.printer.get_reactor().monotonic())

    def _mirror_timer(self, eventtime):
        self._mirror(eventtime)
        return eventtime + 1.

    def _sensor_temp(self, eventtime):
        if self.sensor is None:
            return 0.
        return self.sensor.get_temp(eventtime)[0]

    def _mirror(self, eventtime):
        if self.macro is None:
            return
        self.macro.variables = dict(
            self.macro.variables, target=self.target,
            temperature=round(self._sensor_temp(eventtime), 2),
            has_sensor=1)

    # Heater interface used by klippy/extras/heaters.py
    def set_temp(self, degrees):
        if degrees < 0. or degrees > self.max_target:
            raise self.printer.command_error(
                "Requested temperature (%.1f) out of range (0-%.1f)"
                % (degrees, self.max_target))
        self.target = degrees
        self._mirror(self.printer.get_reactor().monotonic())

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
                'power': 1. if target > 0. else 0.}

    cmd_SET_HEATER_TEMPERATURE_help = "Sets a heater temperature"

    def cmd_SET_HEATER_TEMPERATURE(self, gcmd):
        temp = gcmd.get_float('TARGET', 0.)
        self.pheaters.set_temperature(self, temp)


def load_config_prefix(config):
    return HeaterVirtual(config)
