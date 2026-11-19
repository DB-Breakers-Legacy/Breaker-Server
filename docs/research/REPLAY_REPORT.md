# Official requests replayed against our server

Each endpoint's first official request (arguments only, under a throw-away test account) sent to our server; reply layout compared with the official reply. Friend/block/report calls skipped.

| Result | Endpoints |
|---|---|
| HTTP 400 | 1 |
| HTTP 404 | 2 |
| differs | 2 |
| same shape | 50 |
| skipped | 1 |

| Endpoint | Result | First difference |
|---|---|---|
| `battle/result` | differs | body[4][7]: official has 1 values, ours 0 |
| `character/get` | differs | body[5]: official has 1 values, ours 0 |
| `event_tier/earn_tier_reward` | HTTP 404 | our server refused the official request |
| `event_tier/get_event_tier_info` | HTTP 404 | our server refused the official request |
| `rival/training` | HTTP 400 | our server refused the official request |
| `user/auth` | skipped | login itself (used above) |
| `adjustment_data_manage/get_version` | same shape |  |
| `adjustment_data_manage/read` | same shape |  |
| `avatar_create/get_list` | same shape |  |
| `battle/consume_priority_point` | same shape |  |
| `battle/get_battle_member_result_list` | same shape |  |
| `battle/get_challenge_list` | same shape |  |
| `battle/get_connection_server_info` | same shape |  |
| `battle/get_diarkis_matching_server_info` | same shape |  |
| `battle/get_stun_server_info` | same shape |  |
| `battle/orientation_result` | same shape |  |
| `battle/orientation_start` | same shape |  |
| `battle/pre_matching_connection` | same shape |  |
| `battle/save_matching_cache` | same shape |  |
| `battle/start` | same shape |  |
| `battle/waittime_preview` | same shape |  |
| `bnid_reward/grant_reward` | same shape |  |
| `character/get_item` | same shape |  |
| `close/get_close_info` | same shape |  |
| `commonpurchase/get_catalog` | same shape |  |
| `commonpurchase/get_fund_settlement_text` | same shape |  |
| `commonpurchase/get_purchase_status` | same shape |  |
| `commonpurchase/tokusho` | same shape |  |
| `costume/get_list` | same shape |  |
| `event/get_schedule_list` | same shape |  |
| `gamecurrency/get_owned` | same shape |  |
| `item/item_possession` | same shape |  |
| `leaderboard/get_leaderboard_model` | same shape |  |
| `lootbox/ticket_master_list` | same shape |  |
| `message/get_message_list` | same shape |  |
| `news/get_2d_ui_news_list` | same shape |  |
| `news/get_3d_model_news` | same shape |  |
| `patroller/get_status` | same shape |  |
| `player/upload_ghost_player` | same shape |  |
| `regularly_run/get_emergency_message` | same shape |  |
| `regularly_run/regularly_maintenance_check` | same shape |  |
| `rival/get_list` | same shape |  |
| `season/get_selectable_stage` | same shape |  |
| `skill/training` | same shape |  |
| `sys/agree_kpi` | same shape |  |
| `sys/check_ngname` | same shape |  |
| `sys/get_env_v3` | same shape |  |
| `sys/kpi` | same shape |  |
| `transball/get_list` | same shape |  |
| `user/create_user_info` | same shape |  |
| `user/get_ban_status` | same shape |  |
| `user/get_country` | same shape |  |
| `user/get_tracking_num` | same shape |  |
| `user/update_avatar` | same shape |  |
| `user/update_manner_point` | same shape |  |
| `user/update_orientation_step` | same shape |  |
