#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include "frame_slot_ring.h"

static void check(bool ok, const char *message) {
    if (!ok) { std::fprintf(stderr, "FAILED: %s\n", message); std::exit(1); }
}

int main() {
    sv_slot_ring_t ring{};
    sv_slot_ring_init(&ring);
    uint8_t slot = 0;
    check(!sv_slot_ring_pop(&ring, &slot), "empty ring is nonblocking");
    for (unsigned repeat = 0; repeat < 10000; ++repeat) {
        for (uint8_t id = 0; id < SV_SLOT_COUNT; ++id)
            check(sv_slot_ring_push(&ring, id), "eight usable cells");
        check(!sv_slot_ring_push(&ring, 0), "full ring is nonblocking");
        for (uint8_t id = 0; id < SV_SLOT_COUNT; ++id) {
            check(sv_slot_ring_pop(&ring, &slot) && slot == id, "FIFO across wrap");
        }
        check(!sv_slot_ring_pop(&ring, &slot), "empty after drain");
    }

    // Firmware's exact ownership directions: USB produces free, capture
    // produces ready. Invalid captures also return through ready, never free.
    sv_slot_ring_t free{}, ready{};
    sv_slot_ring_init(&free); sv_slot_ring_init(&ready);
    for (uint8_t id = 0; id < SV_SLOT_COUNT; ++id) check(sv_slot_ring_push(&free, id), "initialize free");
    struct Frame { std::array<uint32_t, 64> data; uint32_t sequence; bool valid; };
    std::array<Frame, SV_SLOT_COUNT> frames{};
    constexpr uint32_t count = 200000;
    std::atomic<uint32_t> valid{0}, invalid{0};
    std::thread capture([&] {
        for (uint32_t seq = 0; seq < count; ++seq) {
            uint8_t id;
            while (!sv_slot_ring_pop(&free, &id)) std::this_thread::yield();
            frames[id].sequence = seq;
            frames[id].valid = seq % 37 != 0;
            for (uint32_t p = 0; p < frames[id].data.size(); ++p) frames[id].data[p] = seq ^ (p * 7919u);
            check(sv_slot_ring_push(&ready, id), "owned slot guarantees ready capacity");
        }
    });
    std::thread usb([&] {
        for (uint32_t seq = 0; seq < count; ++seq) {
            uint8_t id;
            while (!sv_slot_ring_pop(&ready, &id)) std::this_thread::yield();
            check(frames[id].sequence == seq, "chronological metadata");
            check(frames[id].valid == (seq % 37 != 0), "invalid capture reclaim flag");
            for (uint32_t p = 0; p < frames[id].data.size(); ++p)
                check(frames[id].data[p] == (seq ^ (p * 7919u)), "payload publication and no overwrite during USB ownership");
            if (frames[id].valid) ++valid; else ++invalid;
            check(sv_slot_ring_push(&free, id), "USB return never overflows free");
        }
    });
    capture.join(); usb.join();
    check(valid + invalid == count && invalid == (count + 36) / 37, "all captures accounted for");
    for (uint8_t id = 0; id < SV_SLOT_COUNT; ++id) check(sv_slot_ring_pop(&free, &slot), "all slots recovered");
    check(!sv_slot_ring_pop(&free, &slot) && !sv_slot_ring_pop(&ready, &slot), "no duplicate ownership");

    // Simulate disconnect at each transmit/drain position while one slot is
    // still capture-owned. A partial transmit returns only its own slot;
    // the unpublished capture must not be reclaimed by the USB callback.
    for (unsigned already_sent = 0; already_sent < SV_SLOT_COUNT; ++already_sent) {
        sv_slot_ring_init(&free); sv_slot_ring_init(&ready);
        for (uint8_t id = 0; id < SV_SLOT_COUNT; ++id) check(sv_slot_ring_push(&free, id), "disconnect init");
        uint8_t held;
        check(sv_slot_ring_pop(&free, &held), "capture holds a slot during disconnect");
        for (unsigned k = 1; k < SV_SLOT_COUNT; ++k) {
            check(sv_slot_ring_pop(&free, &slot), "allocate queued frame");
            check(sv_slot_ring_push(&ready, slot), "publish queued frame");
        }
        for (unsigned k = 0; k < already_sent; ++k) {
            check(sv_slot_ring_pop(&ready, &slot), "consume before disconnect");
            check(sv_slot_ring_push(&free, slot), "return completed transmit");
        }
        // Pop a partially transmitted frame if any remains, then return it.
        if (sv_slot_ring_pop(&ready, &slot)) check(sv_slot_ring_push(&free, slot), "return interrupted transmit");
        while (sv_slot_ring_pop(&ready, &slot)) check(sv_slot_ring_push(&free, slot), "flush ready at disconnect");
        check(sv_slot_ring_push(&ready, held), "capture publishes after disconnect");
        check(sv_slot_ring_pop(&ready, &slot) && slot == held, "USB reclaims later invalid capture");
        check(sv_slot_ring_push(&free, slot), "restore final slot");
        unsigned seen = 0;
        for (unsigned k = 0; k < SV_SLOT_COUNT; ++k) {
            check(sv_slot_ring_pop(&free, &slot) && slot < SV_SLOT_COUNT, "all slots recovered on disconnect");
            check((seen & (1u << slot)) == 0, "no duplicate reclaim on disconnect");
            seen |= 1u << slot;
        }
        check(seen == 255 && !sv_slot_ring_pop(&free, &slot), "exactly eight distinct slots after disconnect");
    }
    std::puts("PASS: bounded rings, wrap, FIFO, 200000 concurrent handoffs, payload visibility, invalid capture reclamation");
    std::puts("PASS: disconnect/reconnect ownership at all eight drain positions");
}
