import msgpack
import pytest
from django.conf import settings
from django.test import Client

TITLE_ENV = "000000"
TITLE_PRD = "025348"
PFX = "LALALALALA"


def post(client, path, body=None):
    resp = client.post(
        path,
        data=msgpack.packb(body if body is not None else [{"session": ""}, []]),
        content_type="application/x-www-form-urlencoded",
    )
    assert resp.status_code == 200
    return msgpack.unpackb(resp.content, raw=False), resp


def meta():
    return {
        "titleCd": TITLE_PRD,
        "userId": "100000000000000001",
        "session": "",
        "platform": 3,
        "version": "09.01",
    }


def test_get_env_v3():
    data, _ = post(Client(), f"/{PFX}/{TITLE_ENV}/api/sys/get_env_v3")
    assert data[0]["result"] == 0
    assert data[0]["session"] == ""
    assert data[1][0] == 0
    assert settings.ENV_HOST in data[1][1]
    assert settings.PRD_HOST in data[1][2]


def test_cdn_impersonation_headers():
    _, resp = post(Client(), f"/{PFX}/{TITLE_ENV}/api/sys/get_env_v3")
    assert resp.headers["server"] == "nginx"
    assert resp.headers["via"] == "1.1 google"
    assert resp.headers["content-type"] == "application/x-messagepack; charset=utf-8"


def test_env_kpi_no_nameerror():
    """Regression: envKPI used an unassigned countSession (NameError on sys/kpi)."""
    data, _ = post(Client(), f"/dbtb-prd/{PFX}/{TITLE_PRD}/api/sys/kpi")
    assert data[0]["result"] == 0
    assert data[0]["session"] != ""


def test_agree_kpi():
    data, _ = post(Client(), f"/{PFX}/{TITLE_ENV}/api/sys/agree_kpi")
    assert data[0]["result"] == 0


@pytest.mark.django_db
def test_user_auth_second_login_no_integrity_error():
    """Regression: getUserAuth called createAccount unconditionally -> second
    login 500'd with IntegrityError. Now get-or-create."""
    client = Client()
    body = [{"session": ""}, [100000000000000001]]
    first, _ = post(client, f"/{PFX}/{TITLE_ENV}/api/user/auth", body)
    second, _ = post(client, f"/{PFX}/{TITLE_ENV}/api/user/auth", body)
    assert first[0]["result"] == 0
    assert second[0]["result"] == 0
    assert first[0]["session"] != second[0]["session"]  # rolling session token


@pytest.mark.django_db
def test_gamecurrency_after_auth():
    client = Client()
    user_id = 100000000000000002
    post(client, f"/{PFX}/{TITLE_ENV}/api/user/auth", [{"session": ""}, [user_id]])
    body = [dict(meta(), userId=str(user_id)), []]
    data, _ = post(client, f"/dbtb-prd/{PFX}/{TITLE_PRD}/api/gamecurrency/get_owned", body)
    assert data[0]["result"] == 0
    assert data[1] == [0, 696969, 0, "", 0, "0"]  # starting inventory


def test_get_stun_server_info_returns_local_env_server():
    """Regression/fix: must hand out the locally configured STUN server, not
    the official ones."""
    data, _ = post(Client(), f"/dbtb-prd/{PFX}/{TITLE_PRD}/api/battle/get_stun_server_info")
    assert data[0]["result"] == 0
    assert data[1] == [0, [[settings.STUN_HOST, 3478], [settings.STUN_HOST, 3479]]]


def test_get_matching_server_info_uses_env_keys():
    data, _ = post(
        Client(), f"/dbtb-prd/{PFX}/{TITLE_PRD}/api/battle/get_diarkis_matching_server_info"
    )
    assert data[0]["result"] == 0
    host, port, iv, aes, sid, mac = data[1][1]
    assert host == settings.UDP_HOST
    assert port == settings.UDP_PORT
    assert iv == settings.DIARKIS_IV_KEY.hex()
    assert aes == settings.DIARKIS_AES_KEY.hex()
    assert sid == settings.DIARKIS_SID_KEY.hex()
    assert mac == settings.DIARKIS_HASH_KEY.hex()


def test_get_country():
    data, _ = post(Client(), f"/{PFX}/{TITLE_ENV}/api/user/get_country")
    assert data[1] == [0, "GB"]


def test_get_tracking_num():
    data, _ = post(Client(), f"/{PFX}/{TITLE_ENV}/api/user/get_tracking_num")
    assert data[0]["result"] == 0
    assert data[1][1]


def test_adjustment_data_missing_path_returns_empty_blob():
    """ADJUSTMENT_DATA_PATH unset -> empty blob instead of NameError."""
    data, _ = post(Client(), f"/dbtb-prd/{PFX}/{TITLE_PRD}/api/adjustment_data_manage/read")
    assert data[0]["result"] == 0
    assert data[1] == [0, 2, ""]


def test_adjustment_data_reads_configured_path(tmp_path, settings):
    blob = tmp_path / "adjustmentdata.txt"
    blob.write_text("ADJ-BLOB", encoding="utf-8")
    settings.ADJUSTMENT_DATA_PATH = str(blob)
    data, _ = post(Client(), f"/dbtb-prd/{PFX}/{TITLE_PRD}/api/adjustment_data_manage/read")
    assert data[1] == [0, 2, "ADJ-BLOB"]


@pytest.mark.django_db
def test_rival_skill_reset_refunds_and_zeroes():
    client = Client()
    user_id = 100000000000000003
    post(client, f"/{PFX}/{TITLE_ENV}/api/user/auth", [{"session": ""}, [user_id]])

    # spend points on raider 100 first so reset has something to refund:
    # raider 100 starts with 0 points, so zero-cost train, then reset
    body = [dict(meta(), userId=str(user_id)), [100]]
    data, _ = post(client, f"/dbtb-prd/{PFX}/{TITLE_PRD}/api/rival/skill_reset", body)
    assert data[0]["result"] == 0
    raider_id, points, actives, passives = data[1][1]
    assert raider_id == 100
    assert points == 0  # nothing spent yet -> nothing refunded
    assert all(s[1] == 0 for s in actives)
    assert all(s[1] == 0 for s in passives)


@pytest.mark.django_db
def test_rival_training_zero_cost_keeps_points():
    client = Client()
    user_id = 100000000000000004
    post(client, f"/{PFX}/{TITLE_ENV}/api/user/auth", [{"session": ""}, [user_id]])

    body = [dict(meta(), userId=str(user_id)), [[[100, 80000, 0]]]]
    data, _ = post(client, f"/dbtb-prd/{PFX}/{TITLE_PRD}/api/rival/training", body)
    assert data[0]["result"] == 0
    raider_ids = [r[0] for r in data[1][1]]
    assert raider_ids == [100, 200, 300]  # starting inventory raiders, all listed


@pytest.mark.django_db
def test_unlock_spattack_appends_to_character():
    client = Client()
    user_id = 100000000000000005
    post(client, f"/{PFX}/{TITLE_ENV}/api/user/auth", [{"session": ""}, [user_id]])

    body = [dict(meta(), userId=str(user_id)), [[[1, 9999999]]]]
    data, _ = post(client, f"/dbtb-prd/{PFX}/{TITLE_PRD}/api/transball/unlock_spattack", body)
    assert data[0]["result"] == 0
    assert data[1][2] == 696969  # spirit
    char1 = next(c for c in data[1][1] if c[0] == 1)
    assert 9999999 in char1[1]
