#pragma once
// iPhone notifications over Bluetooth LE: Apple Notification Center Service (ANCS).
//
// The board is the peripheral: it advertises a solicitation for ANCS, the iPhone connects and
// pairs from Settings > Bluetooth, and on that same encrypted link the board acts as a GATT
// client of the iPhone's ANCS service. NimBLE-Arduino 1.4 has no client object for a link the
// server owns, so discovery, subscribing and requests use the NimBLE host API directly, and the
// notifications coming back are taken from a GAP event listener.
//
// Everything here runs in the NimBLE host task. Finished notifications are handed to the
// Arduino loop through a queue (ancsTake), together with removals (ancsTakeRemoved).
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <vector>

struct AncsNote {
    uint32_t uid;
    uint8_t category; // 1 incoming call, 2 missed call, 4 social, 6 email, ...
    String app, title, message;
};

namespace ancs {

// 7905F431-B5CE-4E99-A40F-4B1E122D00D0 and its characteristics, little-endian as NimBLE stores them.
static const ble_uuid128_t SVC = BLE_UUID128_INIT(0xd0, 0x00, 0x2d, 0x12, 0x1e, 0x4b, 0x0f, 0xa4, 0x99, 0x4e, 0xce, 0xb5, 0x31, 0xf4, 0x05, 0x79);
static const ble_uuid128_t NOTIF_SRC = BLE_UUID128_INIT(0xbd, 0x1d, 0xa2, 0x99, 0xe6, 0x25, 0x58, 0x8c, 0xd9, 0x42, 0x01, 0x63, 0x0d, 0x12, 0xbf, 0x9f);
static const ble_uuid128_t CTRL_PT = BLE_UUID128_INIT(0xd9, 0xd9, 0xaa, 0xfd, 0xbd, 0x9b, 0x21, 0x98, 0xa8, 0x49, 0xe1, 0x45, 0xf3, 0xd8, 0xd1, 0x69);
static const ble_uuid128_t DATA_SRC = BLE_UUID128_INIT(0xfb, 0x7b, 0x7c, 0xce, 0x6a, 0xb3, 0x44, 0xbe, 0xb5, 0x4b, 0xd6, 0x24, 0xe9, 0xc6, 0xea, 0x22);
static const uint16_t TITLE_MAX = 64, MESSAGE_MAX = 240;

static volatile bool connected = false, encrypted = false, ready = false;
static uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t svcStart, svcEnd, nsVal, nsCccd, cpVal, dsVal, dsCccd;
struct Chr {
    uint16_t def, val;
};
static std::vector<Chr> chrs;
static std::vector<uint32_t> pending; // UIDs waiting for their attributes, one request at a time
static std::vector<uint8_t> dsBuf;    // a Data Source reply, which can span several notifications
static uint8_t pendingCategory[256];  // category by UID low byte, remembered for the reply
static QueueHandle_t notesQ, removedQ;
static ble_gap_event_listener listener;
static String log; // the last steps, newest last, for GET /notifications

inline void note(const char *fmt, ...) {
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Serial.printf("ANCS: %s\n", buf);
    char stamp[12];
    snprintf(stamp, sizeof(stamp), "%lus ", millis() / 1000);
    log += String(stamp) + buf + "\n";
    if (log.length() > 1500) log = log.substring(log.indexOf('\n', log.length() - 1200) + 1);
}

inline void reset() {
    encrypted = ready = false;
    nsVal = nsCccd = cpVal = dsVal = dsCccd = 0;
    chrs.clear();
    pending.clear();
    dsBuf.clear();
}

inline int onWrite(uint16_t, const ble_gatt_error *, ble_gatt_attr *, void *) { return 0; }

inline void requestNext() {
    if (pending.empty() || !cpVal) return;
    uint32_t uid = pending.front();
    uint8_t cmd[] = {0, (uint8_t)uid, (uint8_t)(uid >> 8), (uint8_t)(uid >> 16), (uint8_t)(uid >> 24),
                     0,                                                    // app identifier
                     1, (uint8_t)TITLE_MAX, (uint8_t)(TITLE_MAX >> 8),     // title, max length
                     3, (uint8_t)MESSAGE_MAX, (uint8_t)(MESSAGE_MAX >> 8)}; // message, max length
    dsBuf.clear();
    ble_gattc_write_flat(conn, cpVal, cmd, sizeof(cmd), onWrite, nullptr);
}

// A complete Get Notification Attributes reply: command, UID, then (id, length, bytes) triples.
inline bool parseReply() {
    if (dsBuf.size() < 5 || dsBuf[0] != 0) return false;
    uint32_t uid = dsBuf[1] | dsBuf[2] << 8 | dsBuf[3] << 16 | (uint32_t)dsBuf[4] << 24;
    AncsNote n{uid, pendingCategory[uid & 0xff], "", "", ""};
    size_t p = 5;
    int found = 0;
    while (found < 3) {
        if (p + 3 > dsBuf.size()) return false; // wait for more
        uint8_t id = dsBuf[p];
        uint16_t len = dsBuf[p + 1] | dsBuf[p + 2] << 8;
        if (p + 3 + len > dsBuf.size()) return false;
        String v;
        v.reserve(len);
        for (uint16_t i = 0; i < len; i++) v += (char)dsBuf[p + 3 + i];
        if (id == 0) n.app = v;
        else if (id == 1) n.title = v;
        else if (id == 3) n.message = v;
        p += 3 + len;
        found++;
    }
    AncsNote *out = new AncsNote(n);
    if (xQueueSend(notesQ, &out, 0) != pdTRUE) delete out;
    if (!pending.empty()) pending.erase(pending.begin());
    dsBuf.clear();
    requestNext();
    return true;
}

inline int onGap(ble_gap_event *event, void *) {
    if (event->type == BLE_GAP_EVENT_DISCONNECT) note("disconnect reason 0x%x", event->disconnect.reason);
    if (event->type != BLE_GAP_EVENT_NOTIFY_RX || event->notify_rx.conn_handle != conn) return 0;
    uint8_t buf[256];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(event->notify_rx.om, buf, sizeof(buf), &len) != 0 && len == 0) return 0;
    uint16_t h = event->notify_rx.attr_handle;
    if (h == nsVal && len >= 8) {
        uint8_t ev = buf[0], flags = buf[1], category = buf[2];
        uint32_t uid = buf[4] | buf[5] << 8 | buf[6] << 16 | (uint32_t)buf[7] << 24;
        if (ev == 2) { // removed
            xQueueSend(removedQ, &uid, 0);
        } else if (ev == 0 && !(flags & 0x04) && !(flags & 0x01)) { // added, not pre-existing, not silent
            pendingCategory[uid & 0xff] = category;
            pending.push_back(uid);
            if (pending.size() == 1) requestNext();
        }
    } else if (h == dsVal) {
        dsBuf.insert(dsBuf.end(), buf, buf + len);
        parseReply();
    }
    return 0;
}

static uint8_t CCCD_ON[2] = {1, 0};

inline int onNsSubscribed(uint16_t, const ble_gatt_error *error, ble_gatt_attr *, void *) {
    ready = error->status == 0;
    note("notification source subscribe: %d", error->status);
    return 0;
}
inline int onDsSubscribed(uint16_t, const ble_gatt_error *error, ble_gatt_attr *, void *) {
    note("data source subscribe: %d", error->status);
    // Apple asks for the Data Source to be subscribed before the Notification Source.
    if (error->status == 0) ble_gattc_write_flat(conn, nsCccd, CCCD_ON, 2, onNsSubscribed, nullptr);
    return 0;
}

inline uint16_t chrEnd(uint16_t val) { // last handle of the characteristic holding `val`
    for (const Chr &c : chrs) {
        if (c.def > val) return c.def - 1;
    }
    return svcEnd;
}

inline int onDsDsc(uint16_t, const ble_gatt_error *error, uint16_t, const ble_gatt_dsc *dsc, void *) {
    if (error->status == 0) {
        if (ble_uuid_u16(&dsc->uuid.u) == 0x2902) dsCccd = dsc->handle;
    } else if (error->status == BLE_HS_EDONE && dsCccd && nsCccd) {
        ble_gattc_write_flat(conn, dsCccd, CCCD_ON, 2, onDsSubscribed, nullptr);
    }
    return 0;
}
inline int onNsDsc(uint16_t, const ble_gatt_error *error, uint16_t, const ble_gatt_dsc *dsc, void *) {
    if (error->status == 0) {
        if (ble_uuid_u16(&dsc->uuid.u) == 0x2902) nsCccd = dsc->handle;
    } else if (error->status == BLE_HS_EDONE) {
        ble_gattc_disc_all_dscs(conn, dsVal, chrEnd(dsVal), onDsDsc, nullptr);
    }
    return 0;
}

inline int onChr(uint16_t, const ble_gatt_error *error, const ble_gatt_chr *chr, void *) {
    if (error->status == 0) {
        chrs.push_back({chr->def_handle, chr->val_handle});
        if (ble_uuid_cmp(&chr->uuid.u, &NOTIF_SRC.u) == 0) nsVal = chr->val_handle;
        else if (ble_uuid_cmp(&chr->uuid.u, &CTRL_PT.u) == 0) cpVal = chr->val_handle;
        else if (ble_uuid_cmp(&chr->uuid.u, &DATA_SRC.u) == 0) dsVal = chr->val_handle;
    } else if (error->status == BLE_HS_EDONE) {
        note("chars: ns %u cp %u ds %u (status %d)", nsVal, cpVal, dsVal, error->status);
        if (nsVal && cpVal && dsVal) ble_gattc_disc_all_dscs(conn, nsVal, chrEnd(nsVal), onNsDsc, nullptr);
    }
    return 0;
}

inline int onSvc(uint16_t, const ble_gatt_error *error, const ble_gatt_svc *svc, void *) {
    if (error->status == 0) {
        svcStart = svc->start_handle;
        svcEnd = svc->end_handle;
    } else {
        note("service discovery: status %d, found %d", error->status, svcStart != 0);
        if (error->status == BLE_HS_EDONE && svcStart) ble_gattc_disc_all_chrs(conn, svcStart, svcEnd, onChr, nullptr);
    }
    return 0;
}

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *, ble_gap_conn_desc *desc) override {
        conn = desc->conn_handle;
        connected = true;
        reset();
        svcStart = 0;
        NimBLEDevice::startSecurity(conn); // pairing (first time) or encryption with the stored bond
        note("connected, addr type %d", desc->peer_id_addr.type);
    }
    void onDisconnect(NimBLEServer *, ble_gap_conn_desc *desc) override {
        note("disconnected");
        connected = false;
        conn = BLE_HS_CONN_HANDLE_NONE;
        reset();
    }
    void onAuthenticationComplete(ble_gap_conn_desc *desc) override {
        encrypted = desc->sec_state.encrypted;
        note("security: encrypted %d bonded %d", desc->sec_state.encrypted, desc->sec_state.bonded);
        if (encrypted) ble_gattc_disc_svc_by_uuid(conn, &SVC.u, onSvc, nullptr);
    }
};

inline void begin(const char *name) {
    notesQ = xQueueCreate(8, sizeof(AncsNote *));
    removedQ = xQueueCreate(16, sizeof(uint32_t));
    NimBLEDevice::init(name);
    NimBLEDevice::setSecurityAuth(true, false, true); // bond, no MITM (no screen code), secure connections
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    NimBLEDevice::setMTU(185);
    NimBLEServer *server = NimBLEDevice::createServer();
    server->setCallbacks(new ServerCallbacks());
    ble_gap_event_listener_register(&listener, onGap, nullptr);

    // Flags, a solicitation for ANCS (AD type 0x15) and the name. The solicitation is what makes
    // iOS list the board as an accessory that wants notifications.
    NimBLEAdvertisementData adv;
    adv.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    std::string sol;
    sol += (char)17;
    sol += (char)0x15;
    sol.append((const char *)SVC.value, 16);
    adv.addData(sol);
    NimBLEAdvertisementData scan;
    scan.setName(name);
    NimBLEAdvertising *a = NimBLEDevice::getAdvertising();
    a->setAdvertisementData(adv);
    a->setScanResponseData(scan);
    a->start();
}

inline bool take(AncsNote &out) {
    AncsNote *n;
    if (!notesQ || xQueueReceive(notesQ, &n, 0) != pdTRUE) return false;
    out = *n;
    delete n;
    return true;
}
inline bool takeRemoved(uint32_t &uid) { return removedQ && xQueueReceive(removedQ, &uid, 0) == pdTRUE; }

} // namespace ancs
