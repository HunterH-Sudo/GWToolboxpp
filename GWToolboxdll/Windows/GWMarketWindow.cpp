#include "stdafx.h"

#include "GWMarketWindow.h"

#include <GWCA/Constants/Constants.h>

#include <GWCA/Managers/SkillbarMgr.h>
#include <GWCA/Managers/GameThreadMgr.h>
#include <GWCA/Managers/UIMgr.h>
#include <GWCA/GameEntities/Attribute.h>

#include <Logger.h>
#include <Utils/GuiUtils.h>
#include <Utils/RateLimiter.h>

#include <easywsclient.hpp>
#include <nlohmann/json.hpp>

#include <Modules/GwDatTextureModule.h>
#include <Modules/Resources.h>
#include <Utils/TextUtils.h>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <queue>
#include <sstream>
#include <thread>
#include <unordered_set>


namespace {
    using easywsclient::WebSocket;
    using json = nlohmann::json;

    const char* market_host = "v2.gwmarket.net";
    const char* market_name = "GWMarket.net";

    // Connection rate limiting
    constexpr uint32_t COST_PER_CONNECTION_MS = 30 * 1000;
    constexpr uint32_t COST_PER_CONNECTION_MAX_MS = 60 * 1000;

    // Enums for type-safe representations
    enum class Currency : uint8_t { Platinum = 0, Ecto = 1, Zkeys = 2, Arms = 3, Count = 4, All = 0xf };

    enum class OrderType : uint8_t { Sell = 0, Buy = 1 };

    enum class OrderSortMode : uint8_t { MostRecent = 0, Currency = 1 };
    Currency order_view_currency = Currency::All;

    // Safe string extraction helper
    std::string parseStringFromJson(const json& j, const char* key, const std::string& default_val) {
        if (!j.is_discarded() && j.contains(key) && j[key].is_string()) {
            return j[key].get<std::string>();
        }
        return default_val;
    };
    int parseIntFromJson(const json& j, const char* key, const int& default_val)
    {
        if (!j.is_discarded() && j.contains(key) && j[key].is_number_integer()) {
            return j[key].get<int>();
        }
        return default_val;
    };
    bool parseBoolFromJson(const json& j, const char* key, const bool& default_val)
    {
        if (!j.is_discarded() && j.contains(key) && j[key].is_boolean()) {
            return j[key].get<bool>();
        }
        return default_val;
    };
    uint64_t parseUint64FromJson(const json& j, const char* key, const uint64_t& default_val)
    {
        if (!j.is_discarded() && j.contains(key) && j[key].is_number_unsigned()) {
            return j[key].get<uint64_t>();
        }
        return default_val;
    };
    float parseFloatFromJson(const json& j, const char* key, const float& default_val)
    {
        if (!j.is_discarded() && j.contains(key)) {
            if (j[key].is_number_float())
                return j[key].get<float>();
            if(j[key].is_number_integer()) 
                return (float)j[key].get<int>();
        }
        return default_val;
    };

    const char* GetPriceTypeString(Currency currency)
    {
        switch (currency) {
            case Currency::Platinum:
                return "Plat";
            case Currency::Ecto:
                return "Ecto";
            case Currency::Zkeys:
                return "Zkeys";
            case Currency::Arms:
                return "Arms";
            default:
                return "Unknown";
        }
    }

    IDirect3DTexture9** GetCurrencyImage(Currency currency)
    {
        switch (currency) {
            case Currency::Platinum:
                return GwDatTextureModule::LoadTextureFromFileId(0x1df32);
            case Currency::Ecto:
                return GwDatTextureModule::LoadTextureFromFileId(0x151fa);
            case Currency::Zkeys:
                return GwDatTextureModule::LoadTextureFromFileId(0x52ecf);
            case Currency::Arms:
                return GwDatTextureModule::LoadTextureFromFileId(0x2ae18);
        }
        return nullptr;
    }

    const char* GetOrderTypeString(OrderType type)
    {
        switch (type) {
            case OrderType::Sell:
                return "SELL";
            case OrderType::Buy:
                return "BUY";
            default:
                return "SELL";
        }
    }

    // String interning to reduce memory footprint
    std::unordered_set<std::string> string_pool;

    const std::string* InternString(const std::string& str)
    {
        auto it = string_pool.insert(str);
        return &(*it.first);
    }


    GW::Constants::Attribute AttributeFromString(const std::string& str)
    {
        using namespace GW::Constants;
        // Mesmer
        if (str == "Fast Casting") return Attribute::FastCasting;
        if (str == "Illusion Magic") return Attribute::IllusionMagic;
        if (str == "Domination Magic") return Attribute::DominationMagic;
        if (str == "Inspiration Magic") return Attribute::InspirationMagic;

        // Necromancer
        if (str == "Blood Magic") return Attribute::BloodMagic;
        if (str == "Death Magic") return Attribute::DeathMagic;
        if (str == "Soul Reaping") return Attribute::SoulReaping;
        if (str == "Curses") return Attribute::Curses;

        // Elementalist
        if (str == "Air Magic") return Attribute::AirMagic;
        if (str == "Earth Magic") return Attribute::EarthMagic;
        if (str == "Fire Magic") return Attribute::FireMagic;
        if (str == "Water Magic") return Attribute::WaterMagic;
        if (str == "Energy Storage") return Attribute::EnergyStorage;

        // Monk
        if (str == "Healing Prayers") return Attribute::HealingPrayers;
        if (str == "Smiting Prayers") return Attribute::SmitingPrayers;
        if (str == "Protection Prayers") return Attribute::ProtectionPrayers;
        if (str == "Divine Favor") return Attribute::DivineFavor;

        // Warrior
        if (str == "Strength") return Attribute::Strength;
        if (str == "Axe Mastery") return Attribute::AxeMastery;
        if (str == "Hammer Mastery") return Attribute::HammerMastery;
        if (str == "Swordsmanship") return Attribute::Swordsmanship;
        if (str == "Tactics") return Attribute::Tactics;

        // Ranger
        if (str == "Beast Mastery") return Attribute::BeastMastery;
        if (str == "Expertise") return Attribute::Expertise;
        if (str == "Wilderness Survival") return Attribute::WildernessSurvival;
        if (str == "Marksmanship") return Attribute::Marksmanship;

        // Assassin
        if (str == "Dagger Mastery") return Attribute::DaggerMastery;
        if (str == "Deadly Arts") return Attribute::DeadlyArts;
        if (str == "Shadow Arts") return Attribute::ShadowArts;
        if (str == "Critical Strikes") return Attribute::CriticalStrikes;

        // Ritualist
        if (str == "Communing") return Attribute::Communing;
        if (str == "Restoration Magic") return Attribute::RestorationMagic;
        if (str == "Channeling Magic") return Attribute::ChannelingMagic;
        if (str == "Spawning Power") return Attribute::SpawningPower;

        // Paragon
        if (str == "Spear Mastery") return Attribute::SpearMastery;
        if (str == "Command") return Attribute::Command;
        if (str == "Motivation") return Attribute::Motivation;
        if (str == "Leadership") return Attribute::Leadership;

        // Dervish
        if (str == "Scythe Mastery") return Attribute::ScytheMastery;
        if (str == "Wind Prayers") return Attribute::WindPrayers;
        if (str == "Earth Prayers") return Attribute::EarthPrayers;
        if (str == "Mysticism") return Attribute::Mysticism;

        return Attribute::None;
    }

    // Data structures
    struct Price {
        Currency type;
        int quantity;
        float price;

        static Price FromJson(const json& j)
        {
            Price p;
            p.type = static_cast<Currency>(parseIntFromJson(j, "type", 0));
            p.quantity = parseIntFromJson(j, "quantity", 0);
            p.price = parseFloatFromJson(j, "price", 0.f);
            return p;
        }
    };
    struct WeaponDetails {
        // "weaponDetails":{"attribute":"Tactics","requirement":9,"inscription":true,"oldschool":false,"core":null,"prefix":null,"suffix":null},
        GW::Constants::Attribute attribute = GW::Constants::Attribute::None;
        uint8_t requirement = 0;
        bool inscribable = false;
        static WeaponDetails FromJson(const json& j)
        {
            WeaponDetails p;
            p.attribute = AttributeFromString(parseStringFromJson(j, "attribute", ""));
            p.requirement = parseIntFromJson(j, "requirement", 0) & 0xf;
            p.inscribable = parseBoolFromJson(j, "inscription", false);
            return p;
        }

        std::string toString() const {
            std::string out;
            const auto attrib_data = GW::SkillbarMgr::GetAttributeConstantData(attribute);
            if (attrib_data) {
                out += std::format("Req.{} {}", requirement, Resources::DecodeStringId(attrib_data->name_id, GW::Constants::Language::English)->string().c_str());
            }
            if (inscribable) {
                if (!out.empty()) out += ", ";
                out += "Inscribable";
            }
            return out;
        }

    };

    struct MarketItem {
        std::string name;
        std::string player;
        OrderType orderType;
        int quantity;
        std::vector<Price> prices;
        time_t lastRefresh = 0;
        WeaponDetails weaponDetails;
        std::string description;

        float price_per() const { return prices.empty() ? 0.f : prices[0].price / quantity; }
        Currency currency() const { return prices.empty() ? Currency::All : prices[0].type; }

        static MarketItem FromJson(const json& j)
        {
            MarketItem item;
            if (j.is_discarded()) return item;

            item.name = parseStringFromJson(j, "name", "");
            item.player = parseStringFromJson(j, "player", "");
            item.description = parseStringFromJson(j, "description", "");
            item.orderType = static_cast<OrderType>(parseIntFromJson(j, "orderType", 0));
            item.quantity = parseIntFromJson(j, "quantity", 0);

            if (j.contains("weaponDetails") && j["weaponDetails"].is_object()) {
                item.weaponDetails = WeaponDetails::FromJson(j["weaponDetails"]);
            }

            uint64_t lastRefresh_ms = parseUint64FromJson(j, "lastRefresh", 0ULL);
            item.lastRefresh = lastRefresh_ms ? lastRefresh_ms / 1000 : 0;

            if (j.contains("prices") && j["prices"].is_array()) {
                for (const auto& price_json : j["prices"]) {
                    item.prices.push_back(Price::FromJson(price_json));
                }
            }

            return item;
        }
        bool has_weapon_details() const { return weaponDetails.attribute != GW::Constants::Attribute::None;}
    };

    struct AvailableItem {
        const std::string* name;
        int sellOrders = 0;
        int buyOrders = 0;
    };

    // Settings
    bool auto_refresh = true;
    int refresh_interval = 60;

    // WebSocket
    WebSocket* ws = nullptr;
    bool ws_connecting = false;
    WSAData wsaData = {0};
    RateLimiter ws_rate_limiter;

    // Thread
    std::queue<std::function<void()>> thread_jobs{};
    bool should_stop = false;
    std::thread* worker = nullptr;

    // Data
    std::vector<AvailableItem> available_items;
    std::vector<MarketItem> last_items;
    std::vector<MarketItem> current_item_orders;
    std::string current_viewing_item;
    std::map<std::string, AvailableItem> favorite_items;

    // UI
    char search_buffer[256] = "";
    enum FilterMode { SHOW_ALL, SHOW_SELL_ONLY, SHOW_BUY_ONLY };
    FilterMode filter_mode = SHOW_ALL;
    float refresh_timer = 0.0f;

    // Socket.IO
    bool socket_io_ready = false;
    clock_t last_ping_time = 0;
    int ping_interval = 25000;
    int ping_timeout = 20000;

    OrderSortMode order_sort_mode = OrderSortMode::MostRecent;
    bool available_items_needs_sort = true;
    bool current_orders_needs_sort = true;

    // Forward declarations
    void SendSocketStarted();
    void SendGetAvailableOrders();
    void SendGetLastItemsByFamily(const std::string& family);
    void SendGetItemOrders(const std::string& item_name);
    void SendPing();
    void OnGetAvailableOrders(const json& orders);
    void OnGetLastItems(const json& items);
    void OnGetItemOrders(const json& orders);
    void HandleSocketIOHandshake(const std::string& message);
    void OnNamespaceConnected();
    void OnWebSocketMessage(const std::string& message);
    void DeleteWebSocket(WebSocket* socket);
    void ConnectWebSocket(const bool force);
    void DrawItemList();
    void DrawFavoritesList();
    void DrawItemDetails();
    void Refresh();

    std::string EncodeSocketIOMessage(const std::string& event, const std::string& data = "")
    {
        json msg = json::array();
        msg.push_back(event);
        if (!data.empty()) {
            msg.push_back(data);
        }
        return "42" + msg.dump();
    }

    std::string EncodeSocketIOMessage(const std::string& event, const json& data)
    {
        json msg = json::array();
        msg.push_back(event);
        msg.push_back(data);
        return "42" + msg.dump();
    }

    bool ParseSocketIOMessage(const std::string& message, std::string& event, json& data)
    {
        if (message.length() < 2 || message.substr(0, 2) != "42") return false;

        json parsed = json::parse(message.substr(2), nullptr, false);
        if (!parsed.is_discarded() && parsed.is_array() && parsed.size() >= 1) {
            if (!parsed[0].is_string()) return false;
            event = parsed[0].get<std::string>();
            if (parsed.size() >= 2) {
                data = parsed[1];
                return true;
            }
        }
        return true;
    }

    void OnGetAvailableOrders(const json& orders)
    {
        available_items.clear();
        available_items.reserve(orders.size());
        for (auto& i : favorite_items) {
            i.second = {};
        }
        for (auto it = orders.begin(); it != orders.end(); ++it) {
            AvailableItem item;
            const auto& j = it.value();
            item.name = InternString(it.key());
            item.sellOrders = parseIntFromJson(j, "sellWeek", 0);
            item.buyOrders = parseIntFromJson(j, "buyWeek", 0);
            available_items.push_back(item);
            if (favorite_items.contains(*item.name)) {
                favorite_items[*item.name] = item;
            }
        }
        available_items_needs_sort = true;
        Log::Log("Received %zu available items", available_items.size());
    }

    void OnGetLastItems(const json& items)
    {
        last_items.clear();
        if (items.is_array()) {
            last_items.reserve(items.size());
            for (const auto& item_json : items) {
                last_items.push_back(MarketItem::FromJson(item_json));
            }
        }
        Log::Log("Received %zu recent listings", last_items.size());
    }

    void OnGetItemOrders(const json& orders)
    {
        current_item_orders.clear();
        if (orders.is_array()) {
            current_item_orders.reserve(orders.size());
            for (const auto& order_json : orders) {
                current_item_orders.push_back(MarketItem::FromJson(order_json));
            }
        }
        current_orders_needs_sort = true;
        Log::Log("Received %zu orders", current_item_orders.size());
    }

    void HandleSocketIOHandshake(const std::string& message)
    {
        if (message[0] != '0') return;

        json handshake = json::parse(message.substr(1));
        if (handshake.contains("pingInterval")) {
            ping_interval = handshake["pingInterval"].get<int>();
        }
        if (handshake.contains("pingTimeout")) {
            ping_timeout = handshake["pingTimeout"].get<int>();
        }

        Log::Log("Handshake: ping %dms, timeout %dms", ping_interval, ping_timeout);

        if (ws && ws->getReadyState() == WebSocket::OPEN) {
            ws->send("40");
            Log::Log("[SEND] 40 (connecting to namespace)");
        }

        last_ping_time = clock();
    }

    void OnNamespaceConnected()
    {
        Log::Log("Namespace connected, ready to send events");
        socket_io_ready = true;

        SendSocketStarted();
        Refresh();
    }

    void OnWebSocketMessage(const std::string& message)
    {
        if (message.empty()) return;

        Log::Log("[RECV] %s", message.c_str());

        char type = message[0];

        switch (type) {
            case '0':
                HandleSocketIOHandshake(message);
                break;
            case '1':
                Log::Warning("Server close");
                break;
            case '2':
                if (ws && ws->getReadyState() == WebSocket::OPEN) {
                    ws->send("3");
                    Log::Log("[SEND] 3");
                }
                break;
            case '3':
                Log::Log("Pong received");
                break;
            case '4':
                if (message.length() > 1) {
                    if (message[1] == '0') {
                        Log::Log("Namespace connect confirmed");
                        OnNamespaceConnected();
                    }
                    else if (message[1] == '2') {
                        std::string event;
                        json data;
                        if (!ParseSocketIOMessage(message, event, data)) break;

                        if (event == "GetAvailableOrders")
                            OnGetAvailableOrders(data);
                        else if (event == "GetLastItems")
                            OnGetLastItems(data);
                        else if (event == "GetItemOrders")
                            OnGetItemOrders(data);
                    }
                }
                break;
        }
    }

    void SendSocketStarted()
    {
        if (ws && ws->getReadyState() == WebSocket::OPEN && socket_io_ready) {
            std::string msg = EncodeSocketIOMessage("SocketStarted");
            ws->send(msg);
            Log::Log("[SEND] %s", msg.c_str());
        }
    }

    void SendGetAvailableOrders()
    {
        if (ws && ws->getReadyState() == WebSocket::OPEN && socket_io_ready) {
            std::string msg = EncodeSocketIOMessage("getAvailableOrders");
            ws->send(msg);
            Log::Log("[SEND] %s", msg.c_str());
        }
    }

    void SendGetLastItemsByFamily(const std::string& family)
    {
        if (ws && ws->getReadyState() == WebSocket::OPEN && socket_io_ready) {
            std::string msg = EncodeSocketIOMessage("getLastItemsByFamily", family);
            ws->send(msg);
            Log::Log("[SEND] %s", msg.c_str());
        }
    }

    void SendGetItemOrders(const std::string& item_name)
    {
        if (ws && ws->getReadyState() == WebSocket::OPEN && socket_io_ready) {
            std::string msg = EncodeSocketIOMessage("getItemOrders", item_name);
            ws->send(msg);
            Log::Log("[SEND] %s", msg.c_str());
        }
    }

    void SendPing()
    {
        if (ws && ws->getReadyState() == WebSocket::OPEN) {
            ws->send("2");
            last_ping_time = clock();
            Log::Log("[SEND] 2");
        }
    }

    void DeleteWebSocket(WebSocket* socket)
    {
        if (!socket) return;
        if (socket->getReadyState() == WebSocket::OPEN) socket->close();
        while (socket->getReadyState() != WebSocket::CLOSED)
            socket->poll();
        delete socket;
    }

    void ConnectWebSocket(const bool force = false)
    {
        if (ws || ws_connecting) return;

        if (!force && !ws_rate_limiter.AddTime(COST_PER_CONNECTION_MS, COST_PER_CONNECTION_MAX_MS)) {
            return;
        }
        should_stop = true;
        if (worker && worker->joinable()) worker->join();
        delete worker;
        should_stop = false;
        worker = new std::thread([] {
            while (!should_stop) {
                std::function<void()> job;
                bool found = false;

                if (!thread_jobs.empty()) {
                    job = thread_jobs.front();
                    thread_jobs.pop();
                    found = true;
                }

                if (found) job();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });

        int res;
        if (!wsaData.wVersion && (res = WSAStartup(MAKEWORD(2, 2), &wsaData)) != 0) {
            Log::Error("WSAStartup failed: %d", res);
            return;
        }

        ws_connecting = true;
        thread_jobs.push([] {
            std::string ws_host = std::format("wss://{}/socket.io/?EIO=4&transport=websocket", market_host);
            ws = WebSocket::from_url(ws_host);
            if (ws) {
                Log::Log("Connected");
                socket_io_ready = false;
                last_ping_time = clock();
            }
            else {
                Log::Error("Connection failed");
            }
            ws_connecting = false;
        });
    }

    void DrawItemList()
    {
        ImGui::Text("Available Listings (%zu)", available_items.size());
        ImGui::Separator();
        // Sort only when data changes
        if (available_items_needs_sort) {
            std::sort(available_items.begin(), available_items.end(), [](const AvailableItem& a, const AvailableItem& b) {
                return *a.name < *b.name;
            });

            available_items_needs_sort = false;
        }

        for (const auto& item : available_items) {
            // Apply filter mode
            if (filter_mode == SHOW_SELL_ONLY && item.sellOrders == 0) continue;
            if (filter_mode == SHOW_BUY_ONLY && item.buyOrders == 0) continue;

            // Apply search filter
            if (search_buffer[0] != '\0') {
                std::string search_lower = search_buffer;
                std::string name_lower = *item.name;
                std::transform(search_lower.begin(), search_lower.end(), search_lower.begin(), ::tolower);
                std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
                if (name_lower.find(search_lower) == std::string::npos) continue;
            }

            ImGui::PushID(item.name);

            bool selected = (current_viewing_item == *item.name);
            if (ImGui::Selectable(item.name->c_str(), selected)) {
                current_viewing_item = *item.name;
                SendGetItemOrders(*item.name);
            }

            ImGui::SameLine(300);
            if (item.sellOrders > 0 && item.buyOrders > 0) {
                ImGui::Text("%d  Seller%s, %d Buyer%s", item.sellOrders, item.sellOrders == 1 ? "" : "s", item.buyOrders, item.buyOrders == 1 ? "" : "s");
            }
            else if (item.sellOrders > 0) {
                ImGui::Text("%d  Seller%s", item.sellOrders, item.sellOrders == 1 ? "" : "s");
            }
            else if (item.buyOrders > 0) {
                ImGui::Text("%d  Buyer%s", item.buyOrders, item.buyOrders == 1 ? "" : "s");
            }
            ImGui::PopID();
        }
    }

    void DrawFavoritesList()
    {
        if (favorite_items.empty()) {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "No favorites");
            return;
        }

        for (const auto& favorite : favorite_items) {
            ImGui::PushID(favorite.first.c_str());

            bool selected = current_viewing_item == favorite.first;
            if (ImGui::Selectable(favorite.first.c_str(), selected)) {
                current_viewing_item = favorite.first;
                SendGetItemOrders(favorite.first);
            }

            ImGui::SameLine(300);
            const auto& item = favorite.second;
            if (item.sellOrders > 0 && item.buyOrders > 0) {
                ImGui::Text("%d  Seller%s, %d Buyer%s", item.sellOrders, item.sellOrders == 1 ? "" : "s", item.buyOrders, item.buyOrders == 1 ? "" : "s");
            }
            else if (item.sellOrders > 0) {
                ImGui::Text("%d  Seller%s", item.sellOrders, item.sellOrders == 1 ? "" : "s");
            }
            else if (item.buyOrders > 0) {
                ImGui::Text("%d  Buyer%s", item.buyOrders, item.buyOrders == 1 ? "" : "s");
            }

            ImGui::PopID();
        }
    }
    void ToggleFavourite(const std::string& item_name, bool is_favorite) {
        if (item_name.empty()) return;
        if (!is_favorite) {
            favorite_items.erase(item_name);
        }
        else {
            const auto found = std::ranges::find_if(available_items.begin(), available_items.end(), [item_name](auto& item) {
                return *item.name == item_name;
            });
            favorite_items[item_name] = {};
            if (found != available_items.end()) {
                favorite_items[item_name] = *found;
            }
        }
    }
    void DrawItemDetails()
    {
        if (current_viewing_item.empty()) {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Select an item");
            return;
        }

        ImGui::Text("Item: %s", current_viewing_item.c_str());

        // Sort mode dropdown
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150.0f);
        if (ImGui::BeginCombo("##sort_mode", order_sort_mode == OrderSortMode::MostRecent ? "Most Recent" : "Currency")) {
            if (ImGui::Selectable("Most Recent", order_sort_mode == OrderSortMode::MostRecent)) {
                order_sort_mode = OrderSortMode::MostRecent;
                current_orders_needs_sort = true;
            }
            if (ImGui::Selectable("Currency", order_sort_mode == OrderSortMode::Currency)) {
                order_sort_mode = OrderSortMode::Currency;
                current_orders_needs_sort = true;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150.0f);
        if (ImGui::BeginCombo("##view_currency", order_view_currency == Currency::All ? "All" : GetPriceTypeString(order_view_currency))) {
            if (ImGui::Selectable("All", order_view_currency == Currency::All)) {
                order_view_currency = Currency::All;
            }
            for (uint8_t i = 0; i < (uint8_t)Currency::Count; i++) {
                auto c = (Currency)i;
                if (ImGui::Selectable(GetPriceTypeString(c), order_view_currency == c)) {
                    order_view_currency = c;
                }
            }
            ImGui::EndCombo();
        }

        ImGui::Separator();

        // Add favorite/unfavorite button
        if (!current_viewing_item.empty()) {
            bool is_favorite = favorite_items.contains(current_viewing_item);
            std::string fav_label = std::format("{} {}", ICON_FA_STAR, is_favorite ? "Unfavorite" : "Favorite");
            if (ImGui::Button(fav_label.c_str())) {
                ToggleFavourite(current_viewing_item, !is_favorite);
            }
            ImGui::SameLine();
            std::string url = std::format("https://{}/item/{}", market_host, TextUtils::UrlEncode(current_viewing_item,' '));
            auto btn_label = std::format("{} {}", ICON_FA_GLOBE, market_name);
            if (ImGui::Button(btn_label.c_str())) {
                GW::GameThread::Enqueue([url]() {
                    GW::UI::SendUIMessage(GW::UI::UIMessage::kOpenWikiUrl, (void*)url.c_str());
                });
            }
            ImGui::SameLine();
            btn_label = std::format("{} Guild Wars Wiki", ICON_FA_GLOBE);
            if (ImGui::Button(btn_label.c_str())) {
                GW::GameThread::Enqueue([]() {
                    auto url = GuiUtils::WikiUrl(current_viewing_item);
                    GW::UI::SendUIMessage(GW::UI::UIMessage::kOpenWikiUrl, (void*)url.c_str());
                });
            }
            ImGui::Separator();
        }

        if (current_item_orders.empty()) {
            ImGui::Text("Loading...");
            return;
        }

        // Sort orders only when data changes or sort mode changes
        if (current_orders_needs_sort) {
            if (order_sort_mode == OrderSortMode::Currency) {
                // Sort by price (cheapest first)
                std::sort(current_item_orders.begin(), current_item_orders.end(), [](const MarketItem& a, const MarketItem& b) {
                    if (a.prices.empty() || b.prices.empty()) return false;
                    if (a.currency() != b.currency()) return a.currency() < b.currency();
                    return a.price_per() < b.price_per();
                });
            }
            else {
                // Sort by most recent
                std::sort(current_item_orders.begin(), current_item_orders.end(), [](const MarketItem& a, const MarketItem& b) {
                    return a.lastRefresh > b.lastRefresh;
                });
            }
            current_orders_needs_sort = false;
        }

        const auto font_size = ImGui::CalcTextSize(" ");
        auto DrawOrder = [font_size](const MarketItem& order) {
            // NB: Seems to be an array of prices given by the API, but the website only shows the first one?
            const auto& price = order.prices[0];
            if (order_view_currency != Currency::All && order_view_currency != price.type) return;

            ImGui::PushID(&order);
            const auto top = ImGui::GetCursorPosY();
            ImGui::TextUnformatted(order.player.c_str());
            const auto timetext = TextUtils::RelativeTime(order.lastRefresh);
            ImGui::SameLine();
            ImGui::TextDisabled("%s", timetext.c_str());
            if (order.has_weapon_details()) {
                ImGui::TextUnformatted(order.weaponDetails.toString().c_str());
            }
            if (!order.description.empty()) {
                ImGui::TextUnformatted(order.description.c_str());
            }

            ImGui::Text("Wants to %s %d for ", order.orderType == OrderType::Sell ? "sell" : "buy", order.quantity);

            ImGui::SameLine(0, 0);
            const auto tex = GetCurrencyImage(price.type);
            if (tex) {
                ImGui::ImageCropped((void*)*tex, {font_size.y, font_size.y});
                ImGui::SameLine(0, 0);
            }
            if (price.price == static_cast<int>(price.price)) {
                ImGui::Text("%.0f %s", price.price, GetPriceTypeString(price.type));
            }
            else {
                ImGui::Text("%.2f %s", price.price, GetPriceTypeString(price.type));
            }
            ImGui::SameLine();
            const auto price_per = order.price_per();
            ImGui::TextDisabled(price_per == static_cast<int>(price_per) ? "(%.0f %s each)" : "(%.1f %s each)", price_per, GetPriceTypeString(price.type));
            const auto original_cursor_pos = ImGui::GetCursorPos();

            ImGui::SetCursorPos({ImGui::GetContentRegionAvail().x - 100.f, top + 5.f});
            if (ImGui::Button("Whisper##seller", {100.f, 0.f})) {
                GW::GameThread::Enqueue([player = order.player] {
                    std::wstring name_ws = TextUtils::StringToWString(player);
                    GW::UI::SendUIMessage(GW::UI::UIMessage::kOpenWhisper, (wchar_t*)name_ws.c_str());
                });
            }
            ImGui::SetCursorPos(original_cursor_pos);

            ImGui::Separator();
            ImGui::PopID();
        };


        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "SELL ORDERS:");
        ImGui::Separator();

        for (const auto& order : current_item_orders) {
            if (!(order.orderType == OrderType::Sell && order.quantity && !order.prices.empty())) continue;
            DrawOrder(order);
        }

        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "BUY ORDERS:");
        ImGui::Separator();

        for (const auto& order : current_item_orders) {
            if (!(order.orderType == OrderType::Buy && order.quantity && !order.prices.empty())) continue;
            DrawOrder(order);
        }
    }

    void Refresh()
    {
        if (!socket_io_ready) return;
        SendGetAvailableOrders();
        // SendGetLastItemsByFamily("all");
    }

    bool collapsed = false;
    bool ShouldConnect() {
        return GWMarketWindow::Instance().visible && !collapsed;
    }

    void Disconnect() {
        if (!ws) return;
        should_stop = true;
        if (worker && worker->joinable()) worker->join();
        delete worker;
        worker = 0;
        DeleteWebSocket(ws);
        ws = nullptr;
        ws_connecting = socket_io_ready = false;
    }

} // namespace

void GWMarketWindow::Initialize()
{
    ToolboxWindow::Initialize();
    ConnectWebSocket();
    Log::Log("Market Browser initialized");
}

void GWMarketWindow::Terminate()
{
    Disconnect();
    ToolboxWindow::Terminate();
}

void GWMarketWindow::Update(float delta)
{
    ToolboxWindow::Update(delta);

    if (ws) {
        if (!ShouldConnect()) {
            Disconnect();
            ws_rate_limiter = {};
            return;
        }
        ws->poll();
        ws->dispatch([](const std::string& msg) {
            OnWebSocketMessage(msg);
        });

        // Don't send our own pings - just respond to server pings with pongs
        // The server will ping us every 25 seconds

        if (ws->getReadyState() == WebSocket::CLOSED) {
            Log::Warning("Disconnected");
            Disconnect();
            return;
        }
        if (auto_refresh && socket_io_ready) {
            refresh_timer += delta;
            if (refresh_timer >= refresh_interval) {
                refresh_timer = 0.0f;
                Refresh();
            }
        }
    }
    if (!ws && ShouldConnect())
        ConnectWebSocket();
}

void GWMarketWindow::LoadSettings(ToolboxIni* ini)
{
    ToolboxWindow::LoadSettings(ini);
    LOAD_BOOL(auto_refresh);
    refresh_interval = static_cast<int>(ini->GetLongValue(Name(), "refresh_interval", 60));

    // Load favorite items
    favorite_items.clear();
    const char* favorites_str = ini->GetValue(Name(), "favorite_items", "");
    if (favorites_str && strlen(favorites_str) > 0) {
        std::string favorites(favorites_str);
        size_t start = 0;
        size_t pos = 0;
        while ((pos = favorites.find('|', start)) != std::string::npos) {
            std::string item = favorites.substr(start, pos - start);
            if (!item.empty()) {
                ToggleFavourite(item, true);
            }
            start = pos + 1;
        }
        // Add the last item (or only item if no pipes found)
        if (start < favorites.length()) {
            std::string item = favorites.substr(start);
            if (!item.empty()) {
                ToggleFavourite(item, true);
            }
        }
    }
}

void GWMarketWindow::SaveSettings(ToolboxIni* ini)
{
    ToolboxWindow::SaveSettings(ini);
    SAVE_BOOL(auto_refresh);
    ini->SetLongValue(Name(), "refresh_interval", refresh_interval);

    // Save favorite items as pipe-separated string
    std::string favorites_str;
    for (const auto& item : favorite_items) {
        if (!favorites_str.empty()) {
            favorites_str += "|";
        }
        favorites_str += item.first;
    }
    ini->SetValue(Name(), "favorite_items", favorites_str.c_str());
}

void GWMarketWindow::DrawSettingsInternal()
{
    ImGui::Checkbox("Auto-refresh", &auto_refresh);
    if (auto_refresh) {
        ImGui::SliderInt("Interval (sec)", &refresh_interval, 30, 300);
    }

    ImGui::Separator();

    if (socket_io_ready) {
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Connected");
    }
    else if (ws_connecting) {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Connecting...");
    }
    else {
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Disconnected");
    }

    if (socket_io_ready && ImGui::Button("Refresh")) {
        Refresh();
    }
}

void GWMarketWindow::Draw(IDirect3DDevice9*)
{
    if (!visible) return;

    ImGui::SetNextWindowSize(ImVec2(900, 700), ImGuiCond_FirstUseEver);
    collapsed = !ImGui::Begin(Name(), GetVisiblePtr(), GetWinFlags());
    if (!collapsed) {
        if (socket_io_ready) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Connected");
        }
        else if (ws_connecting) {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Connecting...");
        }
        else {
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Disconnected");
        }

        ImGui::SameLine();
        if (ImGui::Button("Refresh")) {
            Refresh();
        }

        ImGui::Separator();

        ImGui::Text("Show:");
        ImGui::SameLine();
        if (ImGui::RadioButton("All", filter_mode == SHOW_ALL)) filter_mode = SHOW_ALL;
        ImGui::SameLine();
        if (ImGui::RadioButton("WTS", filter_mode == SHOW_SELL_ONLY)) filter_mode = SHOW_SELL_ONLY;
        ImGui::SameLine();
        if (ImGui::RadioButton("WTB", filter_mode == SHOW_BUY_ONLY)) filter_mode = SHOW_BUY_ONLY;

        ImGui::Separator();
        ImGui::InputText("Search", search_buffer, sizeof(search_buffer));
        ImGui::Separator();

        // Calculate available width and height for the two-column layout
        const float available_width = ImGui::GetContentRegionAvail().x;
        const float available_height = ImGui::GetContentRegionAvail().y - 32.f;
        const float left_column_width = available_width * 0.5f - ImGui::GetStyle().ItemSpacing.x * 0.5f;
        const float right_column_width = available_width * 0.5f - ImGui::GetStyle().ItemSpacing.x * 0.5f;

        // Left column split: 65% for item list, 35% for favorites
        const float item_list_height = available_height * 0.7f - ImGui::GetStyle().ItemSpacing.y * 0.5f;
        const float favorites_height = available_height * 0.3f - ImGui::GetStyle().ItemSpacing.y * 0.5f;

        // Left column - Item List (top 65%)
        ImGui::BeginChild("ItemList", ImVec2(left_column_width, item_list_height), true);
        DrawItemList();
        ImGui::EndChild();

        const auto favourites_cursor_pos = ImGui::GetCursorPos();

        // Right column - Item Details (full height)
        ImGui::SameLine();
        ImGui::BeginChild("ItemDetails", ImVec2(right_column_width, available_height), true);
        DrawItemDetails();
        ImGui::EndChild();

        ImGui::SetCursorPos(favourites_cursor_pos);

        // Left column - Favorites List (bottom 35%)
        ImGui::BeginChild("FavoritesList", ImVec2(left_column_width, favorites_height), true);
        ImGui::Text("Favorites");
        ImGui::Separator();
        DrawFavoritesList();
        ImGui::EndChild();

        

            /* Link to website footer */
        static char buf[128];
        if (!buf[0]) {
            const auto url = std::format("https://{}", market_host);
            snprintf(buf, 128, "Powered by %s", url.c_str());
        }

        if (ImGui::Button(buf, ImVec2(ImGui::GetContentRegionAvail().x, 20.0f))) {
            GW::GameThread::Enqueue([]() {
                const auto url = std::format("https://{}", market_host);
                GW::UI::SendUIMessage(GW::UI::UIMessage::kOpenWikiUrl, (void*)url.c_str());
            });
        }
    }
    ImGui::End();
}
