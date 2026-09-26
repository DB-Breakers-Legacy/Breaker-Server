import itertools

from django.conf import settings
from django.utils import timezone

from .models import PatrollerUser


def ignore_kwargs(fn):
    """Drop the <pfx>/<id> URL kwargs the client sends."""

    def wrapper(request, **kwargs):
        return fn(request)

    return wrapper


def getServerDate():
    return timezone.now().strftime("%Y/%m/%d %H:%M:%S")


def get_or_create_account(user_id):
    """Fetch the account for user_id, creating it with the starting inventory on
    first login. get_or_create so repeat logins don't IntegrityError."""
    starting_currency = [0, 696969, 0, "", 0, "0"]
    starting_messages = [
        0,
        [
            [
                1,
                "localhosttesting_1",
                "breadbreakr test 1",
                "2024-01-23 17:46:11",
                "2050-02-22 17:46:11",
                1,
                3,
            ],
            [
                2,
                "localhosttesting_2",
                "breadbreakr test 2!!",
                "2026-01-23 17:46:11",
                "2069-02-22 17:46:11",
                1,
                3,
            ],
        ],
    ]
    starting_rivals = [
        0,
        [
            [
                100,
                1,
                0,
                0,
                [[80000, 0], [80010, 0], [80020, 0], [80030, 0]],
                [[180000, 0], [180010, 0], [180020, 0], [180030, 0]],
                [1002001, 1002005, 1003004, 1003007, 1004001, 1004002],
            ],
            [
                200,
                1,
                0,
                0,
                [[80100, 0], [80110, 0], [80120, 0], [80130, 0]],
                [[180100, 0], [180110, 0], [180120, 0], [180130, 0]],
                [2001000, 2001010, 2002000, 2003000, 2004000, 2004005],
            ],
            [
                300,
                1,
                0,
                0,
                [[80200, 0], [80210, 0], [80220, 0], [80230, 0], [80240, 0], [80250, 0]],
                [[180200, 0], [180210, 0], [180220, 0], [180230, 0]],
                [3001001, 3002000, 3002014, 3003000, 3003006, 3004000],
            ],
        ],
    ]
    starting_characters = [
        0,
        [
            [
                1,
                "2026-05-21 18:54:17",
                [[1, "2026-05-21 18:54:17"]],
                [[4, "2026-05-21 18:54:17"]],
                [[3200000, "2026-05-21 18:54:18"]],
                [],
            ],
            [
                4,
                "2026-05-21 18:54:17",
                [[1, "2026-05-21 18:54:17"]],
                [[1, "2026-05-21 18:54:17"]],
                [[3210100, "2026-05-21 18:54:18"]],
                [],
            ],
            [
                5,
                "2026-05-21 18:54:17",
                [[1, "2026-05-21 18:54:17"]],
                [[2, "2026-05-21 18:54:17"]],
                [[3130900, "2026-05-21 18:54:18"]],
                [],
            ],
            [
                8,
                "2026-05-21 18:54:17",
                [[1, "2026-05-21 18:54:17"]],
                [[1, "2026-05-21 18:54:17"]],
                [],
                [[30810, "2026-05-21 18:54:18"]],
            ],
            [
                12,
                "2026-05-21 18:55:37",
                [[1, "2026-05-21 18:55:37"]],
                [[1, "2026-05-21 18:55:37"]],
                [[3130901, "2026-05-21 18:55:37"]],
                [[131210, "2026-05-21 18:55:37"]],
            ],
        ],
        [[1010, "2026-05-21 18:54:18"], [1040, "2026-05-21 18:54:18"]],
        [
            [20100, 0, 0, "2026-05-21 18:54:17"],
            [20110, 0, 0, "2026-05-21 18:54:17"],
            [20200, 0, 0, "2026-05-21 18:54:17"],
            [30810, 0, 0, "2026-05-21 18:54:18"],
            [50030, 0, 0, "2026-05-21 18:54:17"],
            [131210, 0, 0, "2026-05-21 18:55:37"],
        ],
        [
            [10030, "2026-05-21 18:54:18"],
            [10040, "2026-05-21 18:54:18"],
            [10050, "2026-05-21 18:54:18"],
            [110020, "2026-05-21 18:54:18"],
        ],
        [],
    ]
    return PatrollerUser.objects.get_or_create(
        userID=user_id,
        defaults={
            "gameCurrency": starting_currency,
            "messageList": starting_messages,
            "rivalList": starting_rivals,
            "userCharacters": starting_characters,
        },
    )


# Rolling session token: every response issues a new session value that the
# client echoes in the next request.
# ponytail: process-local itertools counter, resets on restart — the real
# server's scheme is unknown and the client only echoes the value. Ceiling:
# derive from the real algorithm if it's ever reverse-engineered.
_session_counter = itertools.count(1855130961786517)


def getSession():
    return hex(next(_session_counter))[2:]


def env_uri():
    return f"https://{settings.ENV_HOST}/LALALALALA/"


def prd_uri():
    return f"https://{settings.PRD_HOST}/dbtb-prd/LALALALALA/"
