# Virtual heater for an externally regulated chamber heater (iHeater Remote)
#
# Registers a Klipper heater with no MCU pin, no PWM and no verify_heater.
# iHeater Remote pairs with it by the remote_token option (reported in
# get_status) and reads its target over Moonraker.
#
# The link is two-way. The Remote sends readings (air and element
# temperature, power, fan, state, fault) through the Moonraker component
# iheater_remote to the webhooks endpoint heater_virtual/report. A report is
# fresh until arrival + 2 * next_within + 2 s; while it is fresh and the
# iHeater link is up, the chamber temperature is the report's air reading
# and "linked" is true. Otherwise the temperature comes from the optional
# sensor: object, or else the last air reading received.
#
# The sensor type iheater_remote exposes the element or air reading to an
# ordinary [temperature_sensor] section.
#
# This file may be distributed under the terms of the GNU GPLv3 license.

import math
import re

_TOKEN_RE = re.compile(r'^[0-9A-HJKMNP-TV-Z]{8}$')
_LINKS = ('up', 'waiting', 'restarting', 'no-reply')
_REPORT_PATH = 'heater_virtual/report'
_ROUTER_NAME = 'heater_virtual_router'
_SENSOR_TYPE = 'iheater_remote'


def _is_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _check_number(report, key, nullable, lo=None, hi=None):
    value = report[key]
    if value is None and nullable:
        return None
    if not _is_number(value):
        raise ValueError("%s must be a number%s"
                         % (key, " or null" if nullable else ""))
    try:
        value = float(value)
    except OverflowError:
        raise ValueError("%s is out of range" % (key,))
    if not math.isfinite(value):
        raise ValueError("%s must be finite" % (key,))
    if lo is not None and not lo <= value <= hi:
        raise ValueError("%s must be between %g and %g" % (key, lo, hi))
    return value


def _validate_report(report):
    # Returns a clean copy; raises ValueError. Nothing is stored here.
    if not isinstance(report, dict):
        raise ValueError("report must be an object")
    for key in ('link', 'air', 'element', 'power', 'fan', 'state', 'fault',
                'fault_name', 'next_within'):
        if key not in report:
            raise ValueError("missing field '%s'" % (key,))
    link = report['link']
    if not isinstance(link, str) or link not in _LINKS:
        raise ValueError("link must be one of %s" % (", ".join(_LINKS),))
    clean = {'link': link}
    for key in ('air', 'element'):
        clean[key] = _check_number(report, key, True, -100., 1000.)
    clean['power'] = _check_number(report, 'power', True, 0., 1.)
    if not isinstance(report['fan'], bool):
        raise ValueError("fan must be true or false")
    clean['fan'] = report['fan']
    for key in ('state', 'fault_name'):
        if not isinstance(report[key], str):
            raise ValueError("%s must be a string" % (key,))
        clean[key] = report[key]
    fault = report['fault']
    if not isinstance(fault, int) or isinstance(fault, bool):
        raise ValueError("fault must be an integer")
    if not 0 <= fault <= 255:
        raise ValueError("fault must be between 0 and 255")
    clean['fault'] = fault
    next_within = _check_number(report, 'next_within', False)
    if not 0. < next_within <= 60.:
        raise ValueError("next_within must be above 0 and at most 60")
    clean['next_within'] = next_within
    return clean


class _ReportRouter:
    # One per printer: webhooks refuses a second handler for a path.
    def __init__(self, printer):
        self.printer = printer
        self.heaters = []
        printer.lookup_object('webhooks').register_endpoint(
            _REPORT_PATH, self._handle_report)

    def add(self, heater):
        self.heaters.append(heater)

    def _handle_report(self, web_request):
        # Only web_request.error may escape: any other exception shuts
        # Klipper down.
        args = web_request.get_args()
        token = args.get('remote_token') if isinstance(args, dict) else None
        if not isinstance(token, str):
            raise web_request.error("remote_token must be a string")
        token = token.upper().replace('-', '')
        heater = None
        if token:
            for candidate in self.heaters:
                if candidate.remote_token == token:
                    heater = candidate
                    break
        if heater is None:
            raise web_request.error(
                "no heater_virtual has remote_token %s-%s"
                % (token[:4], token[4:]))
        now = self.printer.get_reactor().monotonic()
        try:
            heater.apply_report(args, now)
        except ValueError as e:
            raise web_request.error(str(e))
        web_request.send({'ok': True})


class HeaterVirtual:
    def __init__(self, config):
        self.printer = config.get_printer()
        self.full_name = config.get_name()
        self.short_name = self.full_name.split()[-1]
        self.sensor_name = config.get('sensor', None)
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
        # Latest validated report, its arrival time, and the last non-null
        # readings (kept after the report goes stale).
        self.report = None
        self.report_time = 0.
        self.last_reading = {'air': None, 'element': None}
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
        if _SENSOR_TYPE not in self.pheaters.sensor_factories:
            self.pheaters.add_sensor_factory(_SENSOR_TYPE, IheaterRemoteSensor)
        router = self.printer.lookup_object(_ROUTER_NAME, None)
        if router is None:
            router = _ReportRouter(self.printer)
            self.printer.add_object(_ROUTER_NAME, router)
        router.add(self)
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
        if self.sensor_name is None:
            return
        self.sensor = self.printer.lookup_object(self.sensor_name, None)
        if self.sensor is None:
            raise self.printer.config_error(
                "[%s] sensor '%s' not found"
                % (self.full_name, self.sensor_name))

    def _handle_shutdown(self):
        self.target = 0.

    # Reports from iHeater Remote
    def apply_report(self, report, now):
        # Validate everything before storing anything.
        clean = _validate_report(report)
        self.report = clean
        self.report_time = now
        for kind in ('air', 'element'):
            if clean[kind] is not None:
                self.last_reading[kind] = clean[kind]

    def get_reading(self, kind):
        return self.last_reading[kind]

    def _is_fresh(self, eventtime):
        return (self.report is not None
                and eventtime <= self.report_time
                + 2. * self.report['next_within'] + 2.)

    def _is_linked(self, eventtime):
        return self._is_fresh(eventtime) and self.report['link'] == 'up'

    def _sensor_temp(self, eventtime):
        if self._is_fresh(eventtime) and self.report['air'] is not None:
            return self.report['air']
        if self.sensor is not None:
            return self.sensor.get_temp(eventtime)[0]
        if self.last_reading['air'] is not None:
            return self.last_reading['air']
        return 0.

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
        linked = self._is_linked(eventtime)
        report = self.report or {}
        power = report.get('power')
        if not linked or power is None:
            power = 1. if target > 0. else 0.
        return {'temperature': round(temp, 2), 'target': target,
                'power': power,
                'remote_token': self.remote_token,
                'linked': linked,
                'iheater_state': report.get('state', ''),
                'fault': report.get('fault', 0),
                'fault_name': report.get('fault_name', ''),
                'fan': report.get('fan', False)}

    cmd_SET_HEATER_TEMPERATURE_help = "Sets a heater temperature"

    def cmd_SET_HEATER_TEMPERATURE(self, gcmd):
        temp = gcmd.get_float('TARGET', 0.)
        self.pheaters.set_temperature(self, temp)


class IheaterRemoteSensor:
    # sensor_type: iheater_remote, for a [temperature_sensor] section.
    def __init__(self, config):
        self.printer = config.get_printer()
        self.name = config.get_name()
        self.heater_name = config.get('heater')
        self.reading = config.get('reading', 'element')
        if self.reading not in ('element', 'air'):
            raise config.error("[%s] reading must be 'element' or 'air'"
                               % (self.name,))
        self.heater = None
        self.callback = None
        self.min_temp = self.max_temp = 0.
        self.printer.register_event_handler('klippy:connect',
                                            self._handle_connect)

    def setup_minmax(self, min_temp, max_temp):
        self.min_temp = min_temp
        self.max_temp = max_temp

    def setup_callback(self, cb):
        self.callback = cb

    def get_report_time_delta(self):
        return 1.

    def _handle_connect(self):
        heater = self.printer.lookup_object(
            'heater_virtual ' + self.heater_name, None)
        if not isinstance(heater, HeaterVirtual):
            raise self.printer.config_error(
                "[%s] heater '%s' is not a heater_virtual"
                % (self.name, self.heater_name))
        self.heater = heater
        reactor = self.printer.get_reactor()
        reactor.register_timer(self._sample_temperature, reactor.NOW)

    def _sample_temperature(self, eventtime):
        value = self.heater.get_reading(self.reading)
        if self.callback is not None:
            self.callback(eventtime, value or 0.)
        return eventtime + 1.


def load_config_prefix(config):
    return HeaterVirtual(config)
