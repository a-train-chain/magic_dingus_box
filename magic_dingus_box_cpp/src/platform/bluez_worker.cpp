#include "bluez_worker.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#ifdef HAVE_SYSTEMD
#include <systemd/sd-bus.h>
#endif

namespace platform {

using bt::BtAdapterState;
using bt::BtCommand;
using bt::BtCommandType;
using bt::BtDevice;
using bt::BtResult;
using bt::BtSnapshot;

struct BluezWorker::Session {
    std::atomic<bool> running{true};
    std::mutex mu;
    std::deque<BtCommand> commands;
    std::optional<BtSnapshot> snapshot;
    std::vector<BtResult> results;

    void publish(BtSnapshot s) {
        std::lock_guard<std::mutex> lk(mu);
        snapshot = std::move(s);
    }
    void push_result(BtResult r) {
        std::lock_guard<std::mutex> lk(mu);
        results.push_back(std::move(r));
    }
    std::deque<BtCommand> take_commands() {
        std::lock_guard<std::mutex> lk(mu);
        std::deque<BtCommand> out;
        out.swap(commands);
        return out;
    }
};

namespace {

constexpr const char* kLog = "[bluetooth] ";

[[maybe_unused]] const char* command_name(BtCommandType t) {
    switch (t) {
        case BtCommandType::POWER_ON: return "power on";
        case BtCommandType::START_SCAN: return "start discovery";
        case BtCommandType::STOP_SCAN: return "stop discovery";
        case BtCommandType::REMOVE: return "remove";
        case BtCommandType::PAIR: return "pair";
        case BtCommandType::TRUST: return "trust";
        case BtCommandType::CONNECT: return "connect";
    }
    return "?";
}

#ifdef HAVE_SYSTEMD

constexpr const char* kBluez = "org.bluez";
constexpr const char* kAgentPath = "/org/magicdingus/btagent";
constexpr uint64_t kSyncTimeoutUsec = 5ULL * 1000 * 1000;
constexpr uint64_t kPairTimeoutUsec = 40ULL * 1000 * 1000;
constexpr uint64_t kConnectTimeoutUsec = 20ULL * 1000 * 1000;
constexpr uint64_t kWaitUsec = 100ULL * 1000;
constexpr auto kSnapshotPeriod = std::chrono::milliseconds(500);

std::string error_text(const sd_bus_error& e, int r) {
    if (e.name) {
        std::string s = e.name;
        if (e.message) s += std::string(": ") + e.message;
        return s;
    }
    return std::string("error ") + std::to_string(-r) + " (" + std::strerror(-r) + ")";
}

// ---- pairing agent ----------------------------------------------------------
//
// A raw object handler rather than an sd_bus_vtable: the vtable macros use
// designated initializers, which are C++20 and trip -Wpedantic in this C++17
// build.
int agent_handler(sd_bus_message* m, void*, sd_bus_error*) {
    if (!sd_bus_message_is_method_call(m, "org.bluez.Agent1", nullptr)) return 0;
    const char* member = sd_bus_message_get_member(m);
    const std::string name = member ? member : "";
    int r;
    if (name == "RequestPinCode") {
        r = sd_bus_reply_method_return(m, "s", "0000");    // legacy pads
    } else if (name == "RequestPasskey") {
        r = sd_bus_reply_method_return(m, "u", static_cast<uint32_t>(0));
    } else {
        // Release, DisplayPinCode, DisplayPasskey, RequestConfirmation,
        // RequestAuthorization, AuthorizeService, Cancel: accept.
        r = sd_bus_reply_method_return(m, "");
    }
    std::cout << kLog << "agent: " << name << std::endl;
    return r < 0 ? r : 1;
}

// ---- reading org.bluez objects ---------------------------------------------

// Read the variant at the cursor into `out` if its signature is `sig`,
// otherwise skip it. Returns 1 when read, 0 when skipped, <0 on error.
template <typename T>
int read_variant(sd_bus_message* m, const char* sig, T* out) {
    char type = 0;
    const char* contents = nullptr;
    int r = sd_bus_message_peek_type(m, &type, &contents);
    if (r < 0) return r;
    if (type != SD_BUS_TYPE_VARIANT || !contents || std::strcmp(contents, sig) != 0) {
        r = sd_bus_message_skip(m, "v");
        return r < 0 ? r : 0;   // 0 = skipped
    }
    r = sd_bus_message_read(m, "v", sig, out);
    return r < 0 ? r : 1;       // 1 = read into *out
}

int read_string_variant(sd_bus_message* m, const char* sig, std::string* out) {
    const char* s = nullptr;
    int r = read_variant(m, sig, &s);
    if (r >= 0 && s) *out = s;
    return r;
}

// Iterate an a{sv}; `fn(key, m)` must consume exactly the variant.
template <typename F>
int for_each_property(sd_bus_message* m, F&& fn) {
    int r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "{sv}");
    if (r < 0) return r;
    while ((r = sd_bus_message_enter_container(m, SD_BUS_TYPE_DICT_ENTRY, "sv")) > 0) {
        const char* key = nullptr;
        if ((r = sd_bus_message_read(m, "s", &key)) < 0) return r;
        if ((r = fn(std::string(key ? key : ""), m)) < 0) return r;
        if ((r = sd_bus_message_exit_container(m)) < 0) return r;
    }
    if (r < 0) return r;
    return sd_bus_message_exit_container(m);
}

struct AdapterInfo {
    std::string path;
    bool powered = false;
    bool discovering = false;
    std::string power_state;
};

int read_adapter(sd_bus_message* m, AdapterInfo* a) {
    return for_each_property(m, [&](const std::string& key, sd_bus_message* msg) {
        if (key == "Powered") {
            int v = 0;
            int r = read_variant(msg, "b", &v);
            a->powered = v != 0;
            return r;
        }
        if (key == "Discovering") {
            int v = 0;
            int r = read_variant(msg, "b", &v);
            a->discovering = v != 0;
            return r;
        }
        if (key == "PowerState") return read_string_variant(msg, "s", &a->power_state);
        return sd_bus_message_skip(msg, "v");
    });
}

int read_device(sd_bus_message* m, BtDevice* d, std::string* adapter) {
    return for_each_property(m, [&](const std::string& key, sd_bus_message* msg) {
        if (key == "Address") return read_string_variant(msg, "s", &d->address);
        if (key == "Name") return read_string_variant(msg, "s", &d->name);
        if (key == "Icon") return read_string_variant(msg, "s", &d->icon);
        if (key == "Modalias") return read_string_variant(msg, "s", &d->modalias);
        if (key == "Adapter") return read_string_variant(msg, "o", adapter);
        if (key == "Class") return read_variant(msg, "u", &d->cls);
        if (key == "Appearance") return read_variant(msg, "q", &d->appearance);
        if (key == "RSSI") {
            int r = read_variant(msg, "n", &d->rssi);
            if (r > 0) d->has_rssi = true;
            return r;
        }
        if (key == "Paired" || key == "Trusted" || key == "Connected") {
            int v = 0;
            int r = read_variant(msg, "b", &v);
            if (key == "Paired") d->paired = v != 0;
            else if (key == "Trusted") d->trusted = v != 0;
            else d->connected = v != 0;
            return r;
        }
        return sd_bus_message_skip(msg, "v");
    });
}

// One GetManagedObjects round trip. `adapter_path` is set to the adapter in
// use (the first one, hci0 on a Pi). nullopt on a parse error (the caller
// keeps the previous snapshot).
std::optional<BtSnapshot> read_snapshot(sd_bus* bus, std::string* adapter_path) {
    BtSnapshot snap;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    int r = sd_bus_call_method(bus, kBluez, "/", "org.freedesktop.DBus.ObjectManager",
                               "GetManagedObjects", &err, &reply, "");
    if (r < 0) {
        snap.adapter = BtAdapterState::NO_BLUETOOTH;
        snap.error = error_text(err, r);
        sd_bus_error_free(&err);
        adapter_path->clear();
        return snap;
    }

    std::vector<AdapterInfo> adapters;
    std::vector<std::pair<BtDevice, std::string>> devices;   // device, its adapter
    auto fail = [&](int code) {
        std::cerr << kLog << "GetManagedObjects parse failed: " << std::strerror(-code)
                  << std::endl;
        sd_bus_message_unref(reply);
        return std::optional<BtSnapshot>();
    };

    if ((r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "{oa{sa{sv}}}")) < 0) {
        return fail(r);
    }
    while ((r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_DICT_ENTRY,
                                               "oa{sa{sv}}")) > 0) {
        const char* path = nullptr;
        if ((r = sd_bus_message_read(reply, "o", &path)) < 0) return fail(r);
        const std::string obj = path ? path : "";
        if ((r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "{sa{sv}}")) < 0) {
            return fail(r);
        }
        while ((r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_DICT_ENTRY,
                                                   "sa{sv}")) > 0) {
            const char* iface = nullptr;
            if ((r = sd_bus_message_read(reply, "s", &iface)) < 0) return fail(r);
            const std::string i = iface ? iface : "";
            if (i == "org.bluez.Adapter1") {
                AdapterInfo a;
                a.path = obj;
                if ((r = read_adapter(reply, &a)) < 0) return fail(r);
                adapters.push_back(a);
            } else if (i == "org.bluez.Device1") {
                BtDevice d;
                d.path = obj;
                std::string adapter;
                if ((r = read_device(reply, &d, &adapter)) < 0) return fail(r);
                devices.emplace_back(std::move(d), std::move(adapter));
            } else if ((r = sd_bus_message_skip(reply, "a{sv}")) < 0) {
                return fail(r);
            }
            if ((r = sd_bus_message_exit_container(reply)) < 0) return fail(r);
        }
        if (r < 0) return fail(r);
        if ((r = sd_bus_message_exit_container(reply)) < 0) return fail(r);
        if ((r = sd_bus_message_exit_container(reply)) < 0) return fail(r);
    }
    if (r < 0) return fail(r);
    sd_bus_message_unref(reply);

    if (adapters.empty()) {
        snap.adapter = BtAdapterState::NO_BLUETOOTH;
        snap.error = "no Bluetooth adapter";
        adapter_path->clear();
        return snap;
    }
    std::sort(adapters.begin(), adapters.end(),
              [](const AdapterInfo& a, const AdapterInfo& b) { return a.path < b.path; });
    const AdapterInfo& a = adapters.front();
    *adapter_path = a.path;
    if (a.power_state == "off-blocked") snap.adapter = BtAdapterState::BLOCKED;
    else snap.adapter = a.powered ? BtAdapterState::ON : BtAdapterState::OFF;
    snap.discovering = a.discovering;
    for (auto& [d, adapter] : devices) {
        if (adapter.empty() || adapter == a.path) snap.devices.push_back(std::move(d));
    }
    return snap;
}

// ---- running commands -------------------------------------------------------

struct AsyncCall {
    std::shared_ptr<BluezWorker::Session> session;
    BtCommandType type;
    std::string path;
    sd_bus_slot* slot = nullptr;
};

int on_async_reply(sd_bus_message* m, void* userdata, sd_bus_error*) {
    auto* call = static_cast<AsyncCall*>(userdata);
    const sd_bus_error* e = sd_bus_message_get_error(m);
    BtResult res{call->type, call->path, e == nullptr, ""};
    if (e) res.error = error_text(*e, -EIO);
    std::cout << kLog << command_name(call->type) << " " << call->path << ": "
              << (res.ok ? "ok" : res.error) << std::endl;
    call->session->push_result(std::move(res));
    return 0;
}

class Runner {
public:
    Runner(sd_bus* bus, std::shared_ptr<BluezWorker::Session> session)
        : bus_(bus), session_(std::move(session)) {}

    ~Runner() {
        for (auto& c : calls_) sd_bus_slot_unref(c->slot);
    }

    void execute(const BtCommand& c, const std::string& adapter) {
        const bool needs_adapter = c.type == BtCommandType::POWER_ON ||
                                   c.type == BtCommandType::START_SCAN ||
                                   c.type == BtCommandType::STOP_SCAN ||
                                   c.type == BtCommandType::REMOVE;
        if (needs_adapter && adapter.empty()) {
            done(c, -ENODEV, "no Bluetooth adapter");
            return;
        }
        sd_bus_error err = SD_BUS_ERROR_NULL;
        int r = 0;
        switch (c.type) {
            case BtCommandType::POWER_ON:
                r = sd_bus_set_property(bus_, kBluez, adapter.c_str(), "org.bluez.Adapter1",
                                        "Powered", &err, "b", 1);
                break;
            case BtCommandType::START_SCAN:
                r = sd_bus_call_method(bus_, kBluez, adapter.c_str(), "org.bluez.Adapter1",
                                       "StartDiscovery", &err, nullptr, "");
                if (r >= 0) discovering_ = true;
                break;
            case BtCommandType::STOP_SCAN:
                r = sd_bus_call_method(bus_, kBluez, adapter.c_str(), "org.bluez.Adapter1",
                                       "StopDiscovery", &err, nullptr, "");
                discovering_ = false;
                break;
            case BtCommandType::REMOVE:
                r = sd_bus_call_method(bus_, kBluez, adapter.c_str(), "org.bluez.Adapter1",
                                       "RemoveDevice", &err, nullptr, "o",
                                       c.device_path.c_str());
                break;
            case BtCommandType::TRUST:
                r = sd_bus_set_property(bus_, kBluez, c.device_path.c_str(),
                                        "org.bluez.Device1", "Trusted", &err, "b", 1);
                break;
            case BtCommandType::PAIR:
                start_async(c, "Pair", kPairTimeoutUsec);
                return;
            case BtCommandType::CONNECT:
                start_async(c, "Connect", kConnectTimeoutUsec);
                return;
        }
        done(c, r, r < 0 ? error_text(err, r) : "");
        sd_bus_error_free(&err);
    }

    // Leave the radio as we found it: stop a discovery this session started.
    void finish(const std::string& adapter) {
        if (discovering_ && !adapter.empty()) {
            sd_bus_call_method(bus_, kBluez, adapter.c_str(), "org.bluez.Adapter1",
                               "StopDiscovery", nullptr, nullptr, "");
        }
    }

private:
    void done(const BtCommand& c, int r, const std::string& error) {
        BtResult res{c.type, c.device_path, r >= 0, r >= 0 ? "" : error};
        std::cout << kLog << command_name(c.type)
                  << (c.device_path.empty() ? "" : " " + c.device_path) << ": "
                  << (res.ok ? "ok" : res.error) << std::endl;
        session_->push_result(std::move(res));
    }

    void start_async(const BtCommand& c, const char* member, uint64_t timeout_usec) {
        sd_bus_message* m = nullptr;
        int r = sd_bus_message_new_method_call(bus_, &m, kBluez, c.device_path.c_str(),
                                               "org.bluez.Device1", member);
        if (r < 0) {
            done(c, r, "could not build the request");
            return;
        }
        auto call = std::make_unique<AsyncCall>();
        call->session = session_;
        call->type = c.type;
        call->path = c.device_path;
        r = sd_bus_call_async(bus_, &call->slot, m, on_async_reply, call.get(), timeout_usec);
        sd_bus_message_unref(m);
        if (r < 0) {
            done(c, r, "could not send the request");
            return;
        }
        std::cout << kLog << command_name(c.type) << " " << c.device_path << "..."
                  << std::endl;
        calls_.push_back(std::move(call));
    }

    sd_bus* bus_;
    std::shared_ptr<BluezWorker::Session> session_;
    std::vector<std::unique_ptr<AsyncCall>> calls_;
    bool discovering_ = false;
};

// The session loop. Its Runner (and with it every in-flight Pair/Connect
// slot) is gone before the caller closes the bus.
void pump_session(sd_bus* bus, const std::shared_ptr<BluezWorker::Session>& session) {
    Runner runner(bus, session);
    std::string adapter;
    auto last_snapshot = std::chrono::steady_clock::now() - kSnapshotPeriod;
    while (session->running) {
        // Dispatch agent calls and async Pair/Connect replies.
        int r;
        while ((r = sd_bus_process(bus, nullptr)) > 0) {
        }
        if (r < 0) {
            std::cerr << kLog << "lost the system bus: " << std::strerror(-r) << std::endl;
            BtSnapshot s;
            s.adapter = BtAdapterState::NO_BLUETOOTH;
            s.error = "lost the system bus";
            session->publish(s);
            break;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last_snapshot >= kSnapshotPeriod) {
            if (auto snap = read_snapshot(bus, &adapter)) session->publish(std::move(*snap));
            last_snapshot = now;
        }
        for (const auto& c : session->take_commands()) runner.execute(c, adapter);
        sd_bus_wait(bus, kWaitUsec);
    }
    runner.finish(adapter);
}

void run_session(std::shared_ptr<BluezWorker::Session> session) {
    sd_bus* bus = nullptr;
    int r = sd_bus_open_system(&bus);
    if (r < 0) {
        BtSnapshot s;
        s.adapter = BtAdapterState::NO_BLUETOOTH;
        s.error = std::string("cannot reach the system bus: ") + std::strerror(-r);
        std::cerr << kLog << s.error << std::endl;
        session->publish(s);
        // Fail whatever is asked until the session is ended.
        while (session->running) {
            for (const auto& c : session->take_commands()) {
                session->push_result({c.type, c.device_path, false, s.error});
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return;
    }
    sd_bus_set_method_call_timeout(bus, kSyncTimeoutUsec);

    // Pairing agent, default for as long as the screen is up.
    sd_bus_slot* agent_slot = nullptr;
    bool agent_registered = false;
    r = sd_bus_add_object(bus, &agent_slot, kAgentPath, agent_handler, nullptr);
    if (r >= 0) {
        sd_bus_error err = SD_BUS_ERROR_NULL;
        r = sd_bus_call_method(bus, kBluez, "/org/bluez", "org.bluez.AgentManager1",
                               "RegisterAgent", &err, nullptr, "os", kAgentPath,
                               "NoInputNoOutput");
        if (r >= 0) {
            agent_registered = true;
            sd_bus_error_free(&err);
            r = sd_bus_call_method(bus, kBluez, "/org/bluez", "org.bluez.AgentManager1",
                                   "RequestDefaultAgent", &err, nullptr, "o", kAgentPath);
        }
        if (r < 0) {
            // Not fatal: "Just Works" pads pair without an agent.
            std::cerr << kLog << "agent: " << error_text(err, r) << std::endl;
        }
        sd_bus_error_free(&err);
    }

    std::cout << kLog << "pairing session started" << std::endl;
    pump_session(bus, session);
    if (agent_registered) {
        sd_bus_call_method(bus, kBluez, "/org/bluez", "org.bluez.AgentManager1",
                           "UnregisterAgent", nullptr, nullptr, "o", kAgentPath);
    }
    sd_bus_slot_unref(agent_slot);
    std::cout << kLog << "pairing session ended" << std::endl;
    sd_bus_flush_close_unref(bus);
}

#else  // !HAVE_SYSTEMD

void run_session(std::shared_ptr<BluezWorker::Session> session) {
    BtSnapshot s;
    s.adapter = BtAdapterState::NO_BLUETOOTH;
    s.error = "built without libsystemd (sd-bus)";
    session->publish(s);
    while (session->running) {
        for (const auto& c : session->take_commands()) {
            session->push_result({c.type, c.device_path, false, s.error});
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

#endif

}  // namespace

BluezWorker::~BluezWorker() { stop(); }

void BluezWorker::start() {
    if (session_) return;
    session_ = std::make_shared<Session>();
    std::thread(run_session, session_).detach();
}

void BluezWorker::stop() {
    if (!session_) return;
    session_->running = false;   // the thread ends itself; never joined
    session_.reset();
}

void BluezWorker::send(const BtCommand& command) {
    if (!session_) return;
    std::lock_guard<std::mutex> lk(session_->mu);
    session_->commands.push_back(command);
}

std::optional<BtSnapshot> BluezWorker::take_snapshot() {
    if (!session_) return std::nullopt;
    std::lock_guard<std::mutex> lk(session_->mu);
    std::optional<BtSnapshot> out;
    out.swap(session_->snapshot);
    return out;
}

std::vector<BtResult> BluezWorker::take_results() {
    if (!session_) return {};
    std::lock_guard<std::mutex> lk(session_->mu);
    std::vector<BtResult> out;
    out.swap(session_->results);
    return out;
}

}  // namespace platform
