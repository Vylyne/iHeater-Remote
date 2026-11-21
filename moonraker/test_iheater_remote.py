import asyncio
import enum
import importlib.util
import os
import sys
import types
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))


class RequestType(enum.Flag):
    GET = enum.auto()
    POST = enum.auto()


class WebRequest:
    def __init__(self, endpoint, args, request_type=RequestType(0),
                 transport=None, ip_addr=None, user=None):
        self.endpoint = endpoint
        self.args = args

    def get_args(self):
        return self.args


class ServerError(Exception):
    def __init__(self, message, status_code=400):
        Exception.__init__(self, message)
        self.status_code = status_code


class FakeConn:
    def __init__(self, connected=True, error=None):
        self.connected = connected
        self.error = error
        self.requests = []

    def is_connected(self):
        return self.connected

    async def request(self, web_request):
        self.requests.append(web_request)
        if self.error is not None:
            raise self.error
        return {'ok': True}


class FakeServer:
    error = ServerError

    def __init__(self, conn):
        self.conn = conn
        self.endpoints = []

    def register_endpoint(self, path, request_types, handler):
        self.endpoints.append((path, request_types, handler))

    def lookup_component(self, name):
        assert name == 'klippy_connection'
        return self.conn


class FakeConfig:
    def __init__(self, server):
        self.server = server

    def get_server(self):
        return self.server


def load_module():
    mr = types.ModuleType('mr')
    mr.__path__ = []
    common = types.ModuleType('mr.common')
    common.RequestType = RequestType
    common.WebRequest = WebRequest
    components = types.ModuleType('mr.components')
    components.__path__ = []
    sys.modules.update({'mr': mr, 'mr.common': common,
                        'mr.components': components})
    spec = importlib.util.spec_from_file_location(
        'mr.components.iheater_remote',
        os.path.join(HERE, 'iheater_remote.py'))
    mod = importlib.util.module_from_spec(spec)
    sys.modules['mr.components.iheater_remote'] = mod
    spec.loader.exec_module(mod)
    return mod


def good():
    return {'remote_token': '7K3P9QXM', 'link': 'up', 'air': 41.5,
            'element': 120.0, 'power': 0.5, 'fan': True, 'state': 'heating',
            'fault': 0, 'fault_name': '', 'next_within': 5}


class IheaterRemoteTest(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def make(self, **kw):
        self.conn = FakeConn(**kw)
        self.server = FakeServer(self.conn)
        return self.mod.load_component(FakeConfig(self.server))

    def call(self, comp, args):
        return asyncio.run(comp._handle_report(WebRequest('x', args)))

    def test_registers_report_endpoint(self):
        comp = self.make()
        self.assertEqual(self.server.endpoints,
                         [('/server/iheater_remote/report',
                           RequestType.POST, comp._handle_report)])

    def test_forwards_report_to_klippy(self):
        comp = self.make()
        args = good()
        self.assertEqual(self.call(comp, args), {'ok': True})
        self.assertEqual(len(self.conn.requests), 1)
        req = self.conn.requests[0]
        self.assertEqual(req.endpoint, 'heater_iheater_remote/report')
        self.assertEqual(req.args, args)

    def test_unknown_field_forwarded(self):
        comp = self.make()
        args = good()
        args['humidity'] = 40
        self.call(comp, args)
        self.assertEqual(self.conn.requests[0].args['humidity'], 40)

    def assert_400(self, args):
        comp = self.make()
        with self.assertRaises(ServerError) as cm:
            self.call(comp, args)
        self.assertEqual(cm.exception.status_code, 400)
        self.assertEqual(self.conn.requests, [])

    def test_bad_token_rejected(self):
        args = good()
        args['remote_token'] = '7K3P9QXI'
        self.assert_400(args)

    def test_bool_is_not_a_number(self):
        for key, value in (('fault', True), ('next_within', True),
                           ('air', False)):
            args = good()
            args[key] = value
            self.assert_400(args)

    def test_ranges_and_missing_refused(self):
        for key, value in (('power', 1.5), ('air', 1000.5),
                           ('element', float('nan')), ('fault', 256),
                           ('next_within', 0), ('next_within', 61),
                           ('link', 'bogus'), ('fan', 1), ('state', 3)):
            args = good()
            args[key] = value
            self.assert_400(args)
        args = good()
        del args['state']
        self.assert_400(args)

    def test_null_readings_accepted(self):
        comp = self.make()
        args = good()
        args.update(air=None, element=None, power=None)
        self.call(comp, args)
        sent = self.conn.requests[0].args
        self.assertIsNone(sent['air'])
        self.assertIsNone(sent['element'])
        self.assertIsNone(sent['power'])

    def test_klippy_not_connected(self):
        comp = self.make(connected=False)
        with self.assertRaises(ServerError) as cm:
            self.call(comp, good())
        self.assertEqual(cm.exception.status_code, 503)
        self.assertEqual(self.conn.requests, [])

    def test_klippy_error_passes_through(self):
        err = ServerError("no heater_iheater_remote has remote_token 7K3P-9QXM", 400)
        comp = self.make(error=err)
        with self.assertRaises(ServerError) as cm:
            self.call(comp, good())
        self.assertIs(cm.exception, err)


if __name__ == '__main__':
    unittest.main()
