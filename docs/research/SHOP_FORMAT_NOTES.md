# Shop API reply formats (read from the game exe, 2026-11-13)

Not in any official capture (no purchases were captured). Read from the client's reply parsers instead.
Endpoints the game calls (strings in the exe): shop/get_category_list, shop/get_info_category,
shop/get_info_special_category, shop/payment.

Helper readers (msgpack-c object types: 2 = positive int, 5 = string):
- sub_140820690 = unsigned int (type 2, <= 0xFFFFFFFF)
- sub_1407CFF90 = string (type 5)
- sub_140820420 = array size, sub_1408203B0(obj, i, out) = element i of an array

## shop/get_category_list  (ServerApiResponseGetShopCategoryList, body reader sub_1407F6300)
Reply: [status, categories, uint]
- status: one of the game's known codes (0 = OK)
- categories: array of 5-element arrays (0x88-byte records):
  [uint (+0), string (+0x08), uint (+0x30), string (+0x38), string (+0x60)]   (meanings TODO)
- [2]: uint (stored at +0xB8)

## shop/get_info_category  (ServerApiResponseGetShopCategoryDetail, body reader sub_1407F5910)
Reply: [status, uint (+0x98), string (+0xA0), string (+0xC8), productsA (+0xF0, sub_1407C87B0), listB (+0x110, sub_1407CF340)]
- productsA: array of 13-element arrays (0x110-byte records, reader sub_1407C84E0):
  [str +0, str +0x28, uint +0x50, str +0x58, str +0x80, uint +0xA8, uint +0xAC, str +0xB0, str +0xD8,
   uint +0x100, uint +0x104, uint +0x108, uint +0x10C]          (meanings TODO: which is price / currency)
- listB: not read yet (sub_1407CF340).
- Products become ShopProductBase objects (size 0xB8, CosmosImageDownloader = each has a downloaded image);
  bundles are ShopBundleProduct (ctor sub_1406E67C0) holding ShopItem entries {u64/double +8, u32 +0x10, +0x14,
  +0x18}. Next step: find the caller of sub_1406E67C0 / sub_1406E6A10 to map record fields -> price/currency.

## Notes
- Server has no shop handlers yet (all four calls answer 404). costume/get_list, rival/get_list, avatar_create
  replies carry no prices, so prices the player sees in TP medals come from the client (game files) or from a
  screen we have not identified yet.

## Category list meanings (ModCosmosShopManager sub_1406E5B20, 2026-11-13)
- [0] category id (must be non-zero; entries with 0 are skipped)
- [1] category name (string)
- [2] product count: if non-zero and different from the cached lineup size -> category refreshed
- [3] last-updated time (CosmosTime string, e.g. "2026-11-13 00:00:00"): if newer than the cached one -> refreshed
- [4] string (+0x60): not used by the refresh check (TODO)
- Refreshed categories are queued for shop/get_info_category (id + time stored, sub_1406E2E70 / sub_1406E32B0).
- Request shop/get_category_list: args [] (empty).

## shop/get_info_category request (ModCosmosShopManager sub_1406E56B0)
- Arguments built from: language code string (table 0x141210D50 by the game's language id), category id, offset.
- Paged: offset += 500 after each reply until offset >= the category's product count -> 500 items per page.
- Reply handling: sub_1406E1620 finds the category by reply[1] (id) and builds products from **listB** (reply[5]),
  sub_1406E2030. productsA (reply[4], 13-field records) is NOT used by this path (TODO: who uses it).

## listB = lineup entries (0xE8 bytes, reader sub_1407CF050), 14 fields:
  [0] uint type: 1 = single product (ShopSingleProduct, ctor sub_1406E6A10), 2 = bundle (ShopBundleProduct)
  [1] uint id (bundle id; for single products the id comes from item[0])
  [2] string name
  [3] uint -> product +0xB0 (both kinds)                 <- price or currency type (TODO, test in game)
  [4] uint -> ShopItem second value (single) / bundle arg <- the other of price / currency (TODO)
  [5] uint -> product +0xA8 = (value != 0), +0xAC = value  (purchase limit? TODO)
  [6] uint, [7] uint, [8] uint                              (TODO)
  [9] string -> product string (+0x58 pair)  [10] string -> product string (+0x80)  [11] string (TODO)
  [12] uint (TODO)
  [13] items: array of 4-field arrays (0x38 bytes, reader sub_1407CED80):
       [uint kind/product id (+0), string item id (+8, parsed to a number by sub_1404FDB90), uint (+0x30), uint (+0x34)]
       single product: ShopItem(itemId number, item[2], 1), lineup[4], item[3]; product id = item[0]

## Confirmed in game + exe, 2026-11-13 (test round 1 screenshots, borer, live read of the running game)
- Shop tabs are chosen by item type, not by category id: RivalID items appear under "Raider Characters",
  CostumeItemID under "Visual Items" (tabs: Recommended, Visual Items, Raider Characters, Anime Character Skins, TP).
- PRICE = last number of a lineup item entry (item[3] -> ShopItem +0x18). ShopProductBase vfunc_4 (0x1406E6F10)
  returns it when the item is not owned yet (0 when owned); bundles add up their items (0x1406E6EC0).
  Round 1 used 1 there for every item -> every tile showed "1".
- CURRENCY = lineup [4] (-> ShopItem +0x14 -> tile +0x48). After a purchase MenuShop sub_140A189E0 takes the price off:
  1 = TP tokens (PatrollerData +0x34 + +0x38, the paid "charged" + "free" parts), 2 = Zeni (PatrollerData +0x10, u64),
  3 = Possible_ TP medals (PatrollerData +0x1C). Codes 14/15/100 (round 1) are not currencies -> TP icon shown.
- PatrollerData (SysGameData +0x28 -> AccountData +0x70) holds patroller/get_status part 1 [a, b, c, d, e] at
  +0x8, +0xC, +0x10 (u64), +0x18, +0x1C. Live read: [1, 0, 0, 80000, 0] -> the shop showed Zeni 0, so Zeni = part 1 [2].
  Official save: [531, 13290, 7665579, 330898, 0] -> Zeni 7,665,579; [3] = Possible_ Super Warrior Spirit.
- Tile record (sub_1409FBE30): +0x28 price, +0x2C lineup[3], +0x30/+0x34/+0x38 purchase limit, +0x3C product type,
  +0x40 category value, +0x44 product id, +0x48 currency, +0x70 product +0xB4 (tile size 0-3).
- shop/payment request args = [product type (tile +0x3C), product id (tile +0x44), tile +0x40, ""] (MenuShop
  sub_140A082C0 -> sub_1406E4E80): no price or currency, the server must look them up.
- commonpurchase/get_catalog empty -> "GAME SERVER ERROR: Unable to acquire TP Token catalog information." popup on
  opening the shop; after OK the shop works. (Catalog entries need "is_tptoken":1 in their json, sub_1406DB090.)
- transball/get_all_chara (ServerApiRequestGetTransballReleaseDateList) is asked when the shop opens:
  reply [status, [[number, "date"], ...]]; [0, []] works.
