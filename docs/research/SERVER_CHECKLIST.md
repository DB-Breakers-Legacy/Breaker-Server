# Official server calls vs our C# server

From 1362 decoded official calls (Wireshark + TLS key log, May-Oct 2026). Shape = list lengths and value types, recursively; values themselves are not compared.

| Status | Calls |
|---|---|
| dynamic | 36 |
| same shape | 22 |
| missing | 2 |

| Endpoint | Seen | Status | Note |
|---|---|---|---|
| `/battle/waittime_preview` | 187 | same shape |  |
| `/sys/kpi` | 90 | same shape |  |
| `/battle/get_diarkis_matching_server_info` | 77 | dynamic | reply built in code: compare by hand |
| `/gamecurrency/get_owned` | 65 | dynamic | reply built in code: compare by hand |
| `/patroller/get_status` | 43 | same shape |  |
| `/battle/get_battle_member_result_list` | 41 | same shape |  |
| `/event/get_schedule_list` | 41 | same shape |  |
| `/avatar_create/get_list` | 38 | same shape |  |
| `/character/get` | 38 | dynamic | reply built in code: compare by hand |
| `/character/get_item` | 38 | same shape |  |
| `/rival/get_list` | 38 | dynamic | reply built in code: compare by hand |
| `/season/get_selectable_stage` | 38 | dynamic | reply built in code: compare by hand |
| `/adjustment_data_manage/read` | 37 | dynamic | reply built in code: compare by hand |
| `/regularly_run/get_emergency_message` | 33 | same shape |  |
| `/adjustment_data_manage/get_version` | 32 | dynamic | reply built in code: compare by hand |
| `/battle/pre_matching_connection` | 32 | same shape |  |
| `/battle/save_matching_cache` | 32 | same shape |  |
| `/battle/get_challenge_list` | 27 | same shape |  |
| `/bnid_reward/grant_reward` | 27 | same shape |  |
| `/commonpurchase/get_purchase_status` | 27 | same shape |  |
| `/item/item_possession` | 27 | same shape |  |
| `/leaderboard/get_leaderboard_model` | 27 | same shape |  |
| `/message/get_message_list` | 27 | dynamic | reply built in code: compare by hand |
| `/news/get_3d_model_news` | 27 | dynamic | reply built in code: compare by hand |
| `/user/get_ban_status` | 27 | same shape |  |
| `/user/update_manner_point` | 27 | same shape |  |
| `/close/get_close_info` | 25 | same shape |  |
| `/battle/consume_priority_point` | 17 | same shape |  |
| `/battle/get_connection_server_info` | 16 | dynamic | reply built in code: compare by hand |
| `/battle/result` | 14 | dynamic | reply built in code: compare by hand |
| `/player/upload_ghost_player` | 14 | same shape |  |
| `/battle/get_stun_server_info` | 11 | dynamic | reply built in code: compare by hand |
| `/sys/get_env_v3` | 11 | dynamic | reply built in code: compare by hand |
| `/user/auth` | 11 | dynamic | reply built in code: compare by hand |
| `/user/create_user_info` | 11 | dynamic | reply built in code: compare by hand |
| `/user/get_country` | 11 | dynamic | reply built in code: compare by hand |
| `/user/get_tracking_num` | 11 | dynamic | reply built in code: compare by hand |
| `/user/update_avatar` | 11 | same shape |  |
| `/lootbox/ticket_master_list` | 10 | same shape |  |
| `/event_tier/get_event_tier_info` | 5 | missing | no route in our server |
| `/report/report` | 4 | dynamic | reply built in code: compare by hand |
| `/battle/orientation_result` | 3 | dynamic | reply built in code: compare by hand |
| `/battle/orientation_start` | 3 | dynamic | reply built in code: compare by hand |
| `/friend/get_friend_list` | 3 | dynamic | reply built in code: compare by hand |
| `/sys/agree_kpi` | 3 | dynamic | reply built in code: compare by hand |
| `/battle/start` | 2 | dynamic | reply built in code: compare by hand |
| `/block/add_block_user` | 2 | dynamic | reply built in code: compare by hand |
| `/commonpurchase/get_catalog` | 2 | dynamic | reply built in code: compare by hand |
| `/commonpurchase/get_fund_settlement_text` | 2 | dynamic | reply built in code: compare by hand |
| `/commonpurchase/tokusho` | 2 | dynamic | reply built in code: compare by hand |
| `/costume/get_list` | 2 | dynamic | reply built in code: compare by hand |
| `/event_tier/earn_tier_reward` | 2 | missing | no route in our server |
| `/news/get_2d_ui_news_list` | 2 | dynamic | reply built in code: compare by hand |
| `/regularly_run/regularly_maintenance_check` | 2 | dynamic | reply built in code: compare by hand |
| `/transball/get_list` | 2 | dynamic | reply built in code: compare by hand |
| `/friend/send_friend_request` | 1 | dynamic | reply built in code: compare by hand |
| `/rival/training` | 1 | dynamic | reply built in code: compare by hand |
| `/skill/training` | 1 | dynamic | reply built in code: compare by hand |
| `/sys/check_ngname` | 1 | dynamic | reply built in code: compare by hand |
| `/user/update_orientation_step` | 1 | dynamic | reply built in code: compare by hand |
