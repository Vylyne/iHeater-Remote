"""Unit tests for heater_iheater_remote.py using minimal Klipper fakes.

Run: python -m unittest discover -s klipper -p "test_*.py" -v
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import heater_iheater_remote  # noqa: E402

_MISSING = object()


class ConfigError(Exception):
    pass


class CommandError(Exception):
    pass


class FakeReactor:
    NOW = 0.

    def __init__(self):
        self.timers = []
        self.now = 0.

    def register_timer(self, callback, waketime=None):
        self.timers.append(callback)
        return callback

    def monotonic(self):
        return self.now


class FakePrinter:
    config_error = ConfigError
    command_error = CommandError

    def __init__(self):
        self.objects = {}
        self.handlers = {}
        self.reactor = FakeReactor()

    def lookup_object(self, name, default=_MISSING):
        if name in self.objects:
            return self.objects[name]
        if default is _MISSING:
            raise ConfigError("Unknown config object '%s'" % (name,))
        return default

    def load_object(self, config, name):
        return self.objects[name]

    def add_object(self, name, obj):
        if name in self.objects:
            raise Exception("Printer object '%s' already created" % (name,))
        self.objects[name] = obj

    def register_event_handler(self, event, callback):
        self.handlers.setdefault(event, []).append(callback)

    def fire(self, event):
        for cb in self.handlers.get(event, []):
            cb()

    def get_reactor(self):
        return self.reactor


class FakeConfig:
    error = ConfigError

    def __init__(self, printer, name, options):
        self.printer = printer
        self.name = name
        self.options = options
        # Mirrors Klipper's access_tracking: what configfile.settings exposes
        # (read options, plus defaults that are not None).
        self.settings = {}

    def get_printer(self):
        return self.printer

    def get_name(self):
        return self.name

    def get(self, option, default=_MISSING):
        if option in self.options:
            self.settings[option] = self.options[option]
            return self.options[option]
        if default is _MISSING:
            raise ConfigError("Option '%s' in section '%s' must be specified"
                              % (option, self.name))
        if default is not None:
            self.settings[option] = default
        return default

    def getfloat(self, option, default=_MISSING, minval=None, maxval=None):
        value = float(self.get(option, default))
        if minval is not None and value < minval:
            raise ConfigError("Option '%s' must have minimum of %s"
                              % (option, minval))
        if maxval is not None and value > maxval:
            raise ConfigError("Option '%s' must have maximum of %s"
                              % (option, maxval))
        return value


class FakeHeaters:
    def __init__(self):
        self.heaters = {}
        self.available_heaters = []
        self.registered_sensors = []
        self.sensor_factories = {}

    def add_sensor_factory(self, sensor_type, sensor_factory):
        self.sensor_factories[sensor_type] = sensor_factory

    def register_sensor(self, config, psensor, gcode_id=None):
        self.registered_sensors.append(config.get_name())

    def set_temperature(self, heater, temp, wait=False):
        heater.set_temp(temp)


class FakeWebhooks:
    def __init__(self):
        self.endpoints = {}
        self.registrations = []

    def register_endpoint(self, path, callback):
        if path in self.endpoints:
            raise Exception("Already registered handler for %s" % (path,))
        self.endpoints[path] = callback
        self.registrations.append(path)


class FakeWebRequest:
    error = CommandError

    def __init__(self, args):
        self.args = args
        self.sent = []

    def get_args(self):
        return self.args

    def send(self, data):
        self.sent.append(data)


class FakeGcode:
    def __init__(self):
        self.mux = {}

    def register_mux_command(self, cmd, key, value, func, desc=None):
        self.mux[(cmd, key, value)] = func


class FakeSensor:
    def __init__(self, temp):
        self.temp = temp

    def get_temp(self, eventtime):
        return self.temp, 0.


class HeaterIheaterRemoteTest(unittest.TestCase):
    def setUp(self):
        self.printer = FakePrinter()
        self.heaters = FakeHeaters()
        self.gcode = FakeGcode()
        self.webhooks = FakeWebhooks()
        self.sensor = FakeSensor(21.5)
        self.printer.objects.update({
            'heaters': self.heaters,
            'gcode': self.gcode,
            'webhooks': self.webhooks,
            'temperature_sensor chamber_hexa': self.sensor,
        })

    def make(self, sensor_temp=21.5, **options):
        self.sensor.temp = sensor_temp
        opts = {'sensor': 'temperature_sensor chamber_hexa'}
        opts.update(options)
        self.config = FakeConfig(self.printer, 'heater_iheater_remote chamber', opts)
        hv = heater_iheater_remote.load_config_prefix(self.config)
        # Klipper adds the object returned by load_config_prefix.
        self.printer.objects['heater_iheater_remote chamber'] = hv
        return hv

    def make_connected(self, sensor_temp=21.5, **options):
        hv = self.make(sensor_temp, **options)
        self.printer.fire('klippy:connect')
        return hv

    def test_registers_as_heater(self):
        hv = self.make()
        self.assertIs(self.heaters.heaters['chamber'], hv)
        self.assertIn('heater_iheater_remote chamber', self.heaters.available_heaters)
        self.assertIn('heater_iheater_remote chamber', self.heaters.registered_sensors)
        self.assertIn(('SET_HEATER_TEMPERATURE', 'HEATER', 'chamber'),
                      self.gcode.mux)

    def test_set_heater_temperature_command_sets_target(self):
        hv = self.make_connected()

        class Gcmd:
            def get_float(self, name, default):
                return 55.

        self.gcode.mux[('SET_HEATER_TEMPERATURE', 'HEATER', 'chamber')](Gcmd())
        self.assertEqual(hv.get_status(0.)['target'], 55.)

    def test_set_temp_zero_turns_off(self):
        hv = self.make_connected()
        hv.set_temp(60.)
        hv.set_temp(0.)
        self.assertEqual(hv.get_status(0.)['target'], 0.)
        self.assertEqual(hv.get_status(0.)['power'], 0.0)

    def test_set_temp_low_accepted(self):
        hv = self.make_connected()
        hv.set_temp(40.)
        self.assertEqual(hv.get_status(0.)['target'], 40.)

    def test_set_temp_above_max_target_rejected(self):
        hv = self.make_connected(max_target=70.)
        with self.assertRaises(CommandError):
            hv.set_temp(71.)
        self.assertEqual(hv.get_status(0.)['target'], 0.)

    def test_set_temp_negative_rejected(self):
        hv = self.make_connected()
        with self.assertRaises(CommandError):
            hv.set_temp(-1.)

    def test_max_target_above_90_is_config_error(self):
        with self.assertRaises(ConfigError):
            self.make(max_target=91.)

    def test_max_target_below_45_is_config_error(self):
        with self.assertRaises(ConfigError):
            self.make(max_target=44.)

    def test_missing_sensor_object_on_connect_is_config_error(self):
        del self.printer.objects['temperature_sensor chamber_hexa']
        self.make()
        with self.assertRaises(ConfigError) as ctx:
            self.printer.fire('klippy:connect')
        self.assertIn('chamber_hexa', str(ctx.exception))

    def test_duplicate_heater_name_is_config_error(self):
        self.heaters.heaters['chamber'] = object()
        with self.assertRaises(ConfigError):
            self.make()

    def test_shutdown_zeroes_target(self):
        hv = self.make_connected()
        hv.set_temp(60.)
        self.printer.fire('klippy:shutdown')
        self.assertEqual(hv.get_status(0.)['target'], 0.)

    def test_get_temp_and_check_busy(self):
        hv = self.make_connected(sensor_temp=40.)
        hv.set_temp(50.)
        self.assertEqual(hv.get_temp(0.), (40., 50.))
        self.assertTrue(hv.check_busy(0.))
        self.sensor.temp = 50.
        self.assertFalse(hv.check_busy(0.))

    def test_publishes_max_temp_for_ui_target_limit(self):
        # Mainsail/Fluidd cap the target box at configfile.settings max_temp
        # (missing → 0, so every target would be "too high").
        self.make(max_target=70.)
        self.assertEqual(self.config.settings['max_temp'], 70.)
        self.assertEqual(self.config.settings['min_temp'], 0.)

    def test_explicit_max_temp_is_config_error(self):
        with self.assertRaises(ConfigError) as ctx:
            self.make(max_temp=80.)
        self.assertIn('max_target', str(ctx.exception))

    def test_remote_token_normalized_in_status(self):
        hv = self.make(remote_token='7k3p-9qxm')
        self.assertEqual(hv.get_status(0.)['remote_token'], '7K3P9QXM')

    def test_remote_token_absent_is_empty(self):
        hv = self.make()
        self.assertEqual(hv.get_status(0.)['remote_token'], '')

    def test_remote_token_bad_format_is_config_error(self):
        for bad in ('ABC', '7K3P9QXI', '7K3P9QXMM'):
            with self.subTest(token=bad):
                self.heaters.heaters.clear()
                with self.assertRaises(ConfigError) as ctx:
                    self.make(remote_token=bad)
                self.assertIn('heater_iheater_remote chamber', str(ctx.exception))

    def test_duplicate_remote_token_is_config_error(self):
        self.make(remote_token='7K3P-9QXM')
        config = FakeConfig(self.printer, 'heater_iheater_remote chamber2',
                            {'sensor': 'temperature_sensor chamber_hexa',
                             'remote_token': '7k3p9qxm'})
        with self.assertRaises(ConfigError):
            heater_iheater_remote.load_config_prefix(config)

    def test_mirror_macro_not_read(self):
        self.make(mirror_macro='VIRTUAL_CHAMBER')
        self.assertNotIn('mirror_macro', self.config.settings)

    def test_get_status_reports_sensor_temperature(self):
        hv = self.make_connected(sensor_temp=33.333)
        self.assertEqual(hv.get_status(0.)['temperature'], 33.33)

    # --- Two-way link ---

    def post(self, **over):
        req = FakeWebRequest(report(**over))
        self.webhooks.endpoints['heater_iheater_remote/report'](req)
        return req

    def make_second(self, token='ABCDEFGH'):
        config = FakeConfig(self.printer, 'heater_iheater_remote tent',
                            {'sensor': 'temperature_sensor chamber_hexa',
                             'remote_token': token})
        hv = heater_iheater_remote.load_config_prefix(config)
        self.printer.fire('klippy:connect')
        return hv

    def test_report_endpoint_registered_once(self):
        self.make(remote_token='7K3P9QXM')
        self.make_second()
        self.assertEqual(self.webhooks.registrations,
                         ['heater_iheater_remote/report'])

    def test_report_routes_by_token(self):
        first = self.make_connected(remote_token='ABCDEFGH')
        second = self.make_second('7K3P9QXM')
        self.printer.reactor.now = 100.
        req = self.post()
        self.assertEqual(req.sent, [{'ok': True}])
        self.assertTrue(second.get_status(100.)['linked'])
        self.assertFalse(first.get_status(100.)['linked'])
        self.assertEqual(first.get_status(100.)['iheater_state'], '')

    def test_report_token_hyphen_and_case(self):
        hv = self.make_connected(remote_token='7K3P9QXM')
        self.printer.reactor.now = 100.
        self.post(remote_token='7k3p-9qxm')
        self.assertTrue(hv.get_status(100.)['linked'])

    def test_report_unknown_token_is_error(self):
        self.make_connected(remote_token='7K3P9QXM')
        with self.assertRaises(CommandError) as ctx:
            self.post(remote_token='7K3P9QXX')
        self.assertIn('7K3P-9QXX', str(ctx.exception))

    def test_report_sets_temperature_power_linked(self):
        hv = self.make_connected(remote_token='7K3P9QXM')
        self.printer.reactor.now = 100.
        self.post()
        st = hv.get_status(100.)
        self.assertEqual(st['temperature'], 41.5)
        self.assertEqual(st['power'], 0.42)
        self.assertTrue(st['linked'])
        self.assertEqual(st['iheater_state'], 'heating')
        self.assertTrue(st['fan'])
        self.assertEqual(st['fault'], 0)
        self.assertEqual(st['fault_name'], 'none')

    def test_report_goes_stale_after_2x_next_within_plus_2(self):
        hv = self.make_connected(remote_token='7K3P9QXM')
        self.printer.reactor.now = 100.
        self.post(next_within=5)
        self.assertTrue(hv.get_status(112.0)['linked'])
        st = hv.get_status(112.1)
        self.assertFalse(st['linked'])
        self.assertEqual(st['temperature'], 21.5)

    def test_link_not_up_falls_back_to_sensor(self):
        hv = self.make_connected(remote_token='7K3P9QXM')
        self.printer.reactor.now = 100.
        self.post(link='no-reply', air=None, element=None, power=None)
        st = hv.get_status(100.)
        self.assertFalse(st['linked'])
        self.assertEqual(st['temperature'], 21.5)
        self.assertEqual(st['power'], 0.)
        hv.set_temp(50.)
        self.assertEqual(hv.get_status(100.)['power'], 1.)

    def test_no_sensor_keeps_last_air(self):
        config = FakeConfig(self.printer, 'heater_iheater_remote chamber',
                            {'remote_token': '7K3P9QXM'})
        hv = heater_iheater_remote.load_config_prefix(config)
        self.printer.fire('klippy:connect')
        self.assertEqual(hv.get_status(0.)['temperature'], 0.)
        self.printer.reactor.now = 100.
        self.post()
        self.assertEqual(hv.get_status(100.)['temperature'], 41.5)
        self.assertEqual(hv.get_status(500.)['temperature'], 41.5)
        self.assertFalse(hv.get_status(500.)['linked'])

    def test_malformed_report_raises_command_error_only(self):
        hv = self.make_connected(remote_token='7K3P9QXM')
        self.printer.reactor.now = 100.
        self.post()
        self.printer.reactor.now = 111.
        before = hv.get_status(111.)
        cases = [{'air': 'hot'}, {'air': False}, {'fault': None},
                 {'fault': True}, {'fan': 1}, {'link': 'sideways'},
                 {'next_within': 0}, {'next_within': 61},
                 {'next_within': True}, {'air': float('nan')},
                 {'air': 10 ** 400}, {'state': 5},
                 {'power': 1.5}, {'power': -0.1}, {'fault': 256},
                 {'fault': -1}, {'fault': 2 ** 70}, {'air': 5000},
                 {'air': -100.5}, {'element': 1000.5}]
        for over in cases:
            with self.subTest(over=over):
                args = {'air': 77.7, 'element': 66.6, 'power': 0.9}
                args.update(over)
                with self.assertRaises(CommandError):
                    self.post(**args)
                self.assertEqual(hv.get_status(111.), before)
                self.assertEqual(hv.get_status(111.)['temperature'], 41.5)
                self.assertTrue(hv.get_status(112.)['linked'])
                self.assertFalse(hv.get_status(112.1)['linked'])
                self.assertEqual(hv.get_reading('air'), 41.5)
                self.assertEqual(hv.get_reading('element'), 88.0)
        no_state = report()
        del no_state['state']
        no_token = report()
        del no_token['remote_token']
        for args in (no_state, no_token, report(remote_token=None),
                     report(remote_token=12), report(remote_token='')):
            with self.subTest(args=args):
                with self.assertRaises(CommandError):
                    self.webhooks.endpoints['heater_iheater_remote/report'](
                        FakeWebRequest(args))
                self.assertEqual(hv.get_status(111.), before)

    def test_unknown_report_field_ignored(self):
        hv = self.make_connected(remote_token='7K3P9QXM')
        self.printer.reactor.now = 100.
        req = self.post(humidity=40)
        self.assertEqual(req.sent, [{'ok': True}])
        self.assertTrue(hv.get_status(100.)['linked'])

    def test_set_temp_accepted_while_unlinked(self):
        hv = self.make_connected(remote_token='7K3P9QXM')
        hv.set_temp(60.)
        self.assertEqual(hv.get_status(0.)['target'], 60.)
        self.assertFalse(hv.get_status(0.)['linked'])

    def make_remote_sensor(self, **options):
        opts = {'sensor_type': 'iheater_remote', 'heater': 'chamber'}
        opts.update(options)
        config = FakeConfig(self.printer, 'temperature_sensor chamber_element',
                            opts)
        factory = self.heaters.sensor_factories['iheater_remote']
        sensor = factory(config)
        got = []
        sensor.setup_callback(lambda t, v: got.append((t, v)))
        sensor.setup_minmax(0., 100.)
        return sensor, got

    def test_iheater_remote_sensor_reports_element(self):
        self.make_connected(remote_token='7K3P9QXM')
        sensor, got = self.make_remote_sensor()
        self.printer.fire('klippy:connect')
        self.assertEqual(sensor.get_report_time_delta(), 1.0)
        self.printer.reactor.now = 100.
        self.post()
        self.printer.reactor.timers[-1](101.)
        self.assertEqual(got, [(101., 88.0)])

    def test_iheater_remote_sensor_reading_air(self):
        self.make_connected(remote_token='7K3P9QXM')
        sensor, got = self.make_remote_sensor(reading='air')
        self.printer.fire('klippy:connect')
        self.printer.reactor.now = 100.
        self.post()
        self.printer.reactor.timers[-1](101.)
        self.assertEqual(got, [(101., 41.5)])

    def test_iheater_remote_sensor_bad_heater_is_config_error(self):
        self.make_connected(remote_token='7K3P9QXM')
        self.make_remote_sensor(heater='nope')
        with self.assertRaises(ConfigError):
            self.printer.fire('klippy:connect')

    def test_iheater_remote_sensor_bad_reading_is_config_error(self):
        self.make_connected(remote_token='7K3P9QXM')
        with self.assertRaises(ConfigError):
            self.make_remote_sensor(reading='aux')


def report(**over):
    r = {'remote_token': '7K3P9QXM', 'link': 'up', 'air': 41.5,
         'element': 88.0, 'power': 0.42, 'fan': True, 'state': 'heating',
         'fault': 0, 'fault_name': 'none', 'next_within': 5}
    r.update(over)
    return r


if __name__ == '__main__':
    unittest.main()
