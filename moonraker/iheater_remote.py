# Moonraker component for iHeater Remote
#
# The Remote calls the JSON-RPC method server.iheater_remote.report over its
# Moonraker WebSocket. This component checks each report and forwards it to
# the Klippy webhooks endpoint heater_iheater_remote/report, which finds the
# heater_iheater_remote section by remote_token. The rules mirror _validate_report
# in klipper/heater_iheater_remote.py, plus the remote_token format. The two files
# install into different programs and cannot import each other, so the
# small validator is duplicated.
#
# This file may be distributed under the terms of the GNU GPLv3 license.

from __future__ import annotations
import math
import re
from ..common import RequestType, WebRequest

_TOKEN_RE = re.compile(r'^[0-9A-HJKMNP-TV-Z]{8}$')
_LINKS = ('up', 'waiting', 'restarting', 'no-reply')
_KLIPPY_ENDPOINT = 'heater_iheater_remote/report'


def _is_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _check_number(report, key, nullable, lo=None, hi=None):
    value = report[key]
    if value is None and nullable:
        return
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


def validate_report(args):
    # Raises ValueError. Unknown fields are ignored.
    if not isinstance(args, dict):
        raise ValueError("report must be an object")
    token = args.get('remote_token')
    if not isinstance(token, str) or not _TOKEN_RE.match(token):
        raise ValueError("remote_token must be 8 characters from "
                         "0-9 A-Z without I, L, O or U")
    for key in ('link', 'air', 'element', 'power', 'fan', 'state', 'fault',
                'fault_name', 'next_within'):
        if key not in args:
            raise ValueError("missing field '%s'" % (key,))
    link = args['link']
    if not isinstance(link, str) or link not in _LINKS:
        raise ValueError("link must be one of %s" % (", ".join(_LINKS),))
    for key in ('air', 'element'):
        _check_number(args, key, True, -100., 1000.)
    _check_number(args, 'power', True, 0., 1.)
    if not isinstance(args['fan'], bool):
        raise ValueError("fan must be true or false")
    for key in ('state', 'fault_name'):
        if not isinstance(args[key], str):
            raise ValueError("%s must be a string" % (key,))
    fault = args['fault']
    if not isinstance(fault, int) or isinstance(fault, bool):
        raise ValueError("fault must be an integer")
    if not 0 <= fault <= 255:
        raise ValueError("fault must be between 0 and 255")
    _check_number(args, 'next_within', False)
    if not 0. < args['next_within'] <= 60.:
        raise ValueError("next_within must be above 0 and at most 60")


class IheaterRemote:
    def __init__(self, config):
        self.server = config.get_server()
        self.server.register_endpoint(
            "/server/iheater_remote/report", RequestType.POST,
            self._handle_report)

    async def _handle_report(self, web_request):
        args = web_request.get_args()
        try:
            validate_report(args)
        except ValueError as e:
            raise self.server.error(str(e), 400)
        kconn = self.server.lookup_component("klippy_connection")
        if not kconn.is_connected():
            raise self.server.error("Klippy Host not connected", 503)
        # Klippy's own errors pass through unchanged.
        return await kconn.request(WebRequest(_KLIPPY_ENDPOINT, args))


def load_component(config):
    return IheaterRemote(config)
