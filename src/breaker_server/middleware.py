from time import time
from wsgiref.handlers import format_date_time


class HeaderOrderMiddleWare:
    """Mimic the real CDN's header set and ordering (nginx via Google frontend)."""

    def __init__(self, get_response):
        self.get_response = get_response

    def __call__(self, request):
        response = self.get_response(request)
        # drop Django-added headers
        response.headers.pop("vary", None)
        response.headers.pop("content-type", None)
        response.headers.pop("content-length", None)

        response["server"] = "nginx"
        response["date"] = format_date_time(time())
        response["content-type"] = "application/x-messagepack; charset=utf-8"
        response["content-length"] = len(response.content)
        response["Access-Control-Allow-Origin"] = "*"
        response["via"] = "1.1 google"
        response["Alt-Svc"] = 'h3=":443"; ma=2592000,h3-29=":443"; ma=2592000'

        return response
