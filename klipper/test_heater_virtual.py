"""Unit tests for heater_virtual.py using minimal Klipper fakes.

Run: python -m unittest discover -s klipper -p "test_*.py" -v
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import heater_virtual  # noqa: E402

_MISSING = object()


class ConfigError(Exception):
    pass


class CommandError(Exception):
    pass


class FakeReactor:
    NOW = 0.

    def __init__(self):
        self.timers = []

    def register_timer(self, callback, waketime=None):
        self.timers.append(callback)
        return callback

    def monotonic(self):
        return 0.


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

    def get_printer(self):
        return self.printer

    def get_name(self):
        return self.name

    def get(self, option, default=_MISSING):
        if option in self.options:
            return self.options[option]
        if default is _MISSING:
            raise ConfigError("Option '%s' in section '%s' must be specified"
                              % (option, self.name))
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

    def register_sensor(self, config, psensor, gcode_id=None):
        self.registered_sensors.append(config.get_name())

    def set_temperature(self, heater, temp, wait=False):
        heater.set_temp(temp)


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


class FakeMacro:
    def __init__(self):
        self.variables = {'target': 0, 'temperature': -1, 'has_sensor': 0}


class HeaterVirtualTest(unittest.TestCase):
    def setUp(self):
        self.printer = FakePrinter()
        self.heaters = FakeHeaters()
        self.gcode = FakeGcode()
        self.sensor = FakeSensor(21.5)
        self.macro = FakeMacro()
        self.printer.objects.update({
            'heaters': self.heaters,
            'gcode': self.gcode,
            'temperature_sensor chamber_hexa': self.sensor,
            'gcode_macro VIRTUAL_CHAMBER': self.macro,
        })

    def make(self, sensor_temp=21.5, **options):
        self.sensor.temp = sensor_temp
        opts = {'sensor': 'temperature_sensor chamber_hexa'}
        opts.update(options)
        config = FakeConfig(self.printer, 'heater_virtual chamber', opts)
        return heater_virtual.load_config_prefix(config)

    def make_connected(self, sensor_temp=21.5, **options):
        hv = self.make(sensor_temp, **options)
        self.printer.fire('klippy:connect')
        return hv

    def make_ready(self, sensor_temp=21.5, **options):
        hv = self.make_connected(sensor_temp, **options)
        self.printer.fire('klippy:ready')
        return hv

    def test_registers_as_heater(self):
        hv = self.make()
        self.assertIs(self.heaters.heaters['chamber'], hv)
        self.assertIn('heater_virtual chamber', self.heaters.available_heaters)
        self.assertIn('heater_virtual chamber', self.heaters.registered_sensors)
        self.assertIn(('SET_HEATER_TEMPERATURE', 'HEATER', 'chamber'),
                      self.gcode.mux)

    def test_set_heater_temperature_command_sets_target(self):
        hv = self.make_connected()

        class Gcmd:
            def get_float(self, name, default):
                return 55.

        self.gcode.mux[('SET_HEATER_TEMPERATURE', 'HEATER', 'chamber')](Gcmd())
        self.assertEqual(hv.get_status(0.)['target'], 55.)

    def test_set_temp_mirrors_to_macro(self):
        hv = self.make_connected(sensor_temp=30.0)
        hv.set_temp(60.)
        self.assertEqual(self.macro.variables['target'], 60.)
        self.assertEqual(self.macro.variables['temperature'], 30.0)
        self.assertEqual(self.macro.variables['has_sensor'], 1)
        self.assertEqual(hv.get_status(0.)['power'], 1.0)

    def test_set_temp_zero_turns_off(self):
        hv = self.make_connected()
        hv.set_temp(60.)
        hv.set_temp(0.)
        self.assertEqual(self.macro.variables['target'], 0.)
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

    def test_missing_sensor_option_is_config_error(self):
        config = FakeConfig(self.printer, 'heater_virtual chamber', {})
        with self.assertRaises(ConfigError):
            heater_virtual.load_config_prefix(config)

    def test_missing_macro_on_connect_is_config_error(self):
        del self.printer.objects['gcode_macro VIRTUAL_CHAMBER']
        self.make()
        with self.assertRaises(ConfigError) as ctx:
            self.printer.fire('klippy:connect')
        self.assertIn('VIRTUAL_CHAMBER', str(ctx.exception))

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

    def test_shutdown_zeroes_and_mirrors(self):
        hv = self.make_connected()
        hv.set_temp(60.)
        self.printer.fire('klippy:shutdown')
        self.assertEqual(self.macro.variables['target'], 0.)
        self.assertEqual(hv.get_status(0.)['target'], 0.)

    def test_timer_mirrors_temperature(self):
        self.make_ready(sensor_temp=25.0)
        self.sensor.temp = 26.5
        nxt = self.printer.reactor.timers[0](100.)
        self.assertEqual(self.macro.variables['temperature'], 26.5)
        self.assertEqual(nxt, 101.)

    def test_get_temp_and_check_busy(self):
        hv = self.make_connected(sensor_temp=40.)
        hv.set_temp(50.)
        self.assertEqual(hv.get_temp(0.), (40., 50.))
        self.assertTrue(hv.check_busy(0.))
        self.sensor.temp = 50.
        self.assertFalse(hv.check_busy(0.))

    def test_get_status_reports_sensor_temperature(self):
        hv = self.make_connected(sensor_temp=33.333)
        self.assertEqual(hv.get_status(0.)['temperature'], 33.33)


if __name__ == '__main__':
    unittest.main()
