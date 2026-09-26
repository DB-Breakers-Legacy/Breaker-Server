from django.urls import path

from . import views

urlpatterns = [
    # startup
    path("<pfx>/<id>/api/sys/get_env_v3", views.getEnv),
    path("<pfx>/<id>/api/user/auth", views.getUserAuth),
    path("<pfx>/<id>/api/user/get_country", views.getUserCountry),
    path("<pfx>/<id>/api/user/get_tracking_num", views.getTrackingNum),
    # KPI - terms and agreement
    path("<pfx>/<id>/api/sys/agree_kpi", views.agreeKPI),
    path("dbtb-prd/<pfx>/<id>/api/sys/kpi", views.envKPI),
    # lobby
    path("dbtb-prd/<pfx>/<id>/api/close/get_close_info", views.getCloseInfo),
    path("dbtb-prd/<pfx>/<id>/api/user/create_user_info", views.createUserInfo),
    path("dbtb-prd/<pfx>/<id>/api/adjustment_data_manage/read", views.readAdjustmentData),
    path("dbtb-prd/<pfx>/<id>/api/battle/get_stun_server_info", views.getStunServer),
    path("dbtb-prd/<pfx>/<id>/api/patroller/get_status", views.getPatrollerStatus),
    path("dbtb-prd/<pfx>/<id>/api/gamecurrency/get_owned", views.getGameCurrencyOwned),
    # rivals/raider
    path("dbtb-prd/<pfx>/<id>/api/rival/get_list", views.getRivalList),
    path("dbtb-prd/<pfx>/<id>/api/rival/skill_reset", views.rivalSkillReset),
    path("dbtb-prd/<pfx>/<id>/api/rival/training", views.rivalTraining),
    path("dbtb-prd/<pfx>/<id>/api/avatar_create/get_list", views.getAvatarCreateList),
    path("dbtb-prd/<pfx>/<id>/api/character/get", views.getCharacter),
    path("dbtb-prd/<pfx>/<id>/api/character/get_item", views.getCharacterItems),
    path(
        "dbtb-prd/<pfx>/<id>/api/battle/get_diarkis_matching_server_info", views.getMatchingServer
    ),
    path("dbtb-prd/<pfx>/<id>/api/user/get_ban_status", views.getBanStatus),
    path("dbtb-prd/<pfx>/<id>/api/commonpurchase/get_purchase_status", views.getPurchaseStatus),
    path("dbtb-prd/<pfx>/<id>/api/item/item_possession", views.getItemPossession),
    path("dbtb-prd/<pfx>/<id>/api/battle/get_challenge_list", views.getChallengeList),
    path("dbtb-prd/<pfx>/<id>/api/event/get_schedule_list", views.getScheduleList),
    path("dbtb-prd/<pfx>/<id>/api/user/update_manner_point", views.updateMannerPoint),
    path("dbtb-prd/<pfx>/<id>/api/bnid_reward/grant_reward", views.grantReward),
    # messagebox
    path("dbtb-prd/<pfx>/<id>/api/message/get_message_list", views.getMessageList),
    path("dbtb-prd/<pfx>/<id>/api/message/get_message_info", views.getMessageInfo),
    path("dbtb-prd/<pfx>/<id>/api/message/update_message_item_received", views.updateMessageList),
    path("dbtb-prd/<pfx>/<id>/api/regularly_run/get_emergency_message", views.getEmergencyMessage),
    path("dbtb-prd/<pfx>/<id>/api/news/get_3d_model_news", views.get3DModelNews),
    path("dbtb-prd/<pfx>/<id>/api/lootbox/ticket_master_list", views.ticketMasterList),
    path("dbtb-prd/<pfx>/<id>/api/leaderboard/get_leaderboard_model", views.getLeaderboardModel),
    path("dbtb-prd/<pfx>/<id>/api/user/update_avatar", views.updateAvatar),
    # training
    path("dbtb-prd/<pfx>/<id>/api/transball/unlock_spattack", views.unlockSpattack),
    path("dbtb-prd/<pfx>/<id>/api/skill/training", views.skillTraining),
    # shop
    path("dbtb-prd/<pfx>/<id>/api/commonpurchase/tokusho/", views.tokusho),
]
