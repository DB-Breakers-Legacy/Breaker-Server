"""Run the HTTP API and the Diarkis UDP peer in ONE process.

The session-key registry (diarkis_peer.KEY_REGISTRY) is a plain in-process
dict: the HTTP handout endpoints write it, the UDP peer reads it. Running
`runserver` and `RUDPserver.py` separately does NOT share it — this command
exists to keep both in one process.

ponytail: HTTP here is wsgiref (HTTP/1.1) — fine for development. The real
client requires HTTP/2; production serving needs hypercorn (or equivalent)
in the same process as the UDP peer. Ceiling: swap the wsgiref block for an
HTTP/2 server when the live client smoke (Phase 2 T9) lands.
"""

import threading
from wsgiref.simple_server import make_server

from django.conf import settings
from django.core.management.base import BaseCommand
from django.core.servers.basehttp import ThreadedWSGIServer
from django.core.wsgi import get_wsgi_application
from twisted.internet import reactor

from diarkis_peer import DiarkisProtocol


class Command(BaseCommand):
    help = "Run HTTP (WSGI) + Diarkis UDP peer in one process (shared key registry)"

    def handle(self, *args, **options):
        reactor.listenUDP(settings.UDP_PORT, DiarkisProtocol())
        reactor.listenUDP(settings.UDP_SESSION_PORT, DiarkisProtocol(probe=True))
        httpd = make_server(
            "127.0.0.1", 8000, get_wsgi_application(), server_class=ThreadedWSGIServer
        )
        threading.Thread(target=httpd.serve_forever, daemon=True).start()
        self.stdout.write(
            f"HTTP on 127.0.0.1:8000, "
            f"Diarkis UDP on {settings.UDP_PORT} (+ probe {settings.UDP_SESSION_PORT})"
        )
        reactor.run()
