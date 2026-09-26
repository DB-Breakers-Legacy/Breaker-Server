"""
Django settings for breaker_server.

All deployment-specific values come from the environment (see .env.example).
Nothing secret or site-specific is committed.
"""

import os
from pathlib import Path

from django.core.management.utils import get_random_secret_key
from dotenv import load_dotenv

# repo root = src/breaker_server/settings.py -> parents[2]
BASE_DIR = Path(__file__).resolve().parents[2]

load_dotenv(BASE_DIR / ".env")


# No Django signing/auth middleware is enabled, so SECRET_KEY protects nothing
# here; unset -> random per-process key. Set DJANGO_SECRET_KEY anyway if you
# later enable signed cookies/sessions.
SECRET_KEY = os.environ.get("DJANGO_SECRET_KEY") or get_random_secret_key()

DEBUG = os.environ.get("DJANGO_DEBUG", "") == "1"

# "testserver" = Django test client
ALLOWED_HOSTS = [
    h.strip()
    for h in os.environ.get("ALLOWED_HOSTS", "127.0.0.1,localhost,testserver").split(",")
    if h.strip()
]

INSTALLED_APPS = [
    "breakerapi",
]

MIDDLEWARE = [
    "django.middleware.common.CommonMiddleware",
    "django.middleware.security.SecurityMiddleware",
    "breaker_server.middleware.HeaderOrderMiddleWare",
]

ROOT_URLCONF = "breaker_server.urls"

WSGI_APPLICATION = "breaker_server.wsgi.application"

DATABASES = {
    "default": {
        "ENGINE": "django.db.backends.sqlite3",
        "NAME": BASE_DIR / "db.sqlite3",
    }
}

TIME_ZONE = "UTC"  # responses use server-local time strings; keep pinned to UTC

DEFAULT_AUTO_FIELD = "django.db.models.BigAutoField"

# CDN impersonation: the real API omits these headers
SECURE_CONTENT_TYPE_NOSNIFF = False
SECURE_REFERRER_POLICY = None
SECURE_CROSS_ORIGIN_OPENER_POLICY = None

# Hosts the API impersonates / listens on (used in sys/get_env_v3)
ENV_HOST = os.environ.get("ENV_HOST", "127.0.0.1")
PRD_HOST = os.environ.get("PRD_HOST", "127.0.0.1")

# Diarkis RUDP matchmaking server handed out by battle/get_diarkis_matching_server_info
UDP_HOST = os.environ.get("UDP_HOST", "127.0.0.1")
UDP_PORT = int(os.environ.get("UDP_PORT", "7100"))

# Session host handed out by battle/get_connection_server_info once a match is
# found (7100 and 7102 both observed on the official servers)
UDP_SESSION_PORT = int(os.environ.get("UDP_SESSION_PORT", "7102"))

# Local STUN server(s) handed out by battle/get_stun_server_info
STUN_HOST = os.environ.get("STUN_HOST", "127.0.0.1")
STUN_PORTS = [int(p) for p in os.environ.get("STUN_PORTS", "3478,3479").split(",") if p.strip()]

# adjustment_data_manage/read serves this blob verbatim. Not committed; point at
# your own export. Empty/missing path -> empty blob.
ADJUSTMENT_DATA_PATH = os.environ.get("ADJUSTMENT_DATA_PATH", "")

# Diarkis session keys are fresh per session, issued by the HTTP handout
# endpoints and shared with the UDP peer via diarkis_peer.KEY_REGISTRY
# (single process — see the rundiarkis management command). Dev override:
# DIARKIS_SID_KEY/DIARKIS_AES_KEY/DIARKIS_IV_KEY/DIARKIS_HASH_KEY env vars pin
# a fixed key set (see .env.example; consumed by diarkis_peer.issue_keyset).
