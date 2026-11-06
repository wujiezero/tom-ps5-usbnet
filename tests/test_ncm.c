/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The configuration fixtures are real: config index 1 of the RTL8159 adapter, on a
 * SuperSpeedPlus and a High-Speed port. NTBs and notifications are constructed bytes. */
#include "ncm.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t captured_ssp[] = {
    0x09,0x02,0x68,0x00,0x02,0x02,0x00,0xa0,0x44,0x09,0x04,0x00,0x00,0x01,0x02,0x0d,
    0x00,0x05,0x05,0x24,0x00,0x10,0x01,0x05,0x24,0x06,0x00,0x01,0x0d,0x24,0x0f,0x03,
    0x1f,0x50,0x31,0x00,0xee,0x05,0x00,0x80,0x00,0x06,0x24,0x1a,0x00,0x01,0x2b,0x07,
    0x05,0x83,0x03,0x10,0x00,0x0b,0x06,0x30,0x00,0x00,0x08,0x00,0x09,0x04,0x01,0x00,
    0x00,0x0a,0x00,0x01,0x00,0x09,0x04,0x01,0x01,0x02,0x0a,0x00,0x01,0x04,0x07,0x05,
    0x81,0x02,0x00,0x04,0x00,0x06,0x30,0x06,0x00,0x00,0x00,0x07,0x05,0x02,0x02,0x00,
    0x04,0x00,0x06,0x30,0x06,0x00,0x00,0x00,
};
static const uint8_t captured_hs[] = {
    0x09,0x02,0x56,0x00,0x02,0x02,0x00,0xa0,0xfa,0x09,0x04,0x00,0x00,0x01,0x02,0x0d,
    0x00,0x05,0x05,0x24,0x00,0x10,0x01,0x05,0x24,0x06,0x00,0x01,0x0d,0x24,0x0f,0x03,
    0x1f,0x50,0x31,0x00,0xee,0x05,0x00,0x80,0x00,0x06,0x24,0x1a,0x00,0x01,0x2b,0x07,
    0x05,0x83,0x03,0x10,0x00,0x0b,0x09,0x04,0x01,0x00,0x00,0x0a,0x00,0x01,0x00,0x09,
    0x04,0x01,0x01,0x02,0x0a,0x00,0x01,0x04,0x07,0x05,0x81,0x02,0x00,0x02,0x00,0x07,
    0x05,0x02,0x02,0x00,0x02,0x00,
};
/* The same adapter's vendor configuration has no NCM function. */
static const uint8_t captured_vendor_hs[] = {
    0x09,0x02,0x27,0x00,0x01,0x01,0x00,0xa0,0xfa,0x09,0x04,0x00,0x00,0x03,0xff,0xff,
    0x00,0x00,0x07,0x05,0x81,0x02,0x00,0x02,0x00,0x07,0x05,0x02,0x02,0x00,0x02,0x00,
    0x07,0x05,0x83,0x03,0x10,0x00,0x0b,
};

static void put16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/* NTH at 0, NDP at 12, two frames of 60 and 64 bytes. */
static size_t build_block(uint8_t *b)
{
    memset(b, 0, 256);
    memcpy(b, "NCMH", 4);
    put16(b + 4, 12);
    put16(b + 6, 7);
    put16(b + 8, 200);
    put16(b + 10, 12);
    memcpy(b + 12, "NCM0", 4);
    put16(b + 16, 24);
    put16(b + 18, 0);
    put16(b + 20, 40);
    put16(b + 22, 60);
    put16(b + 24, 100);
    put16(b + 26, 64);
    return 200;
}

/* Configuration index 2 of the adapter, read from a real one. */
static const uint8_t captured_ecm[] = {
    0x09,0x02,0x62,0x00,0x02,0x03,0x00,0xa0,0x44,0x09,0x04,0x00,0x00,0x01,0x02,0x06,
    0x00,0x05,0x05,0x24,0x00,0x10,0x01,0x05,0x24,0x06,0x00,0x01,0x0d,0x24,0x0f,0x03,
    0x1f,0x50,0x31,0x00,0xee,0x05,0x00,0x80,0x00,0x07,0x05,0x83,0x03,0x10,0x00,0x0b,
    0x06,0x30,0x00,0x00,0x08,0x00,0x09,0x04,0x01,0x00,0x00,0x0a,0x00,0x00,0x00,0x09,
    0x04,0x01,0x01,0x02,0x0a,0x00,0x00,0x04,0x07,0x05,0x81,0x02,0x00,0x04,0x00,0x06,
    0x30,0x06,0x00,0x00,0x00,0x07,0x05,0x02,0x02,0x00,0x04,0x00,0x06,0x30,0x06,0x00,
    0x00,0x00,
};

static void test_function(void)
{
    struct ncm_function f, e;

    assert(ncm_find_function(captured_ssp, sizeof captured_ssp, &f) == NCM_OK);
    assert(f.comm_iface == 0 && f.data_iface == 1 && f.data_alt == 1);
    assert(f.ep_in == 0x81 && f.ep_out == 0x02 && f.ep_notify == 0x83);
    assert(ncm_find_ecm_function(captured_ssp, sizeof captured_ssp, &e) == NCM_ENOTFOUND);
    assert(ncm_find_ecm_function(captured_ecm, sizeof captured_ecm, &e) == NCM_OK);
    assert(e.comm_iface == 0 && e.data_iface == 1 && e.data_alt == 1 && !e.capabilities);
    assert(e.ep_in == 0x81 && e.ep_out == 0x02 && e.ep_notify == 0x83 && e.mac_string == 3);
    assert(e.in_mps == 1024 && e.notify_mps == 16 && e.max_segment == 1518);
    assert(ncm_find_function(captured_ecm, sizeof captured_ecm, &e) == NCM_ENOTFOUND);
    assert(f.in_mps == 1024 && f.out_mps == 1024 && f.notify_mps == 16);
    assert(f.capabilities == 0x2b && f.max_segment == 1518);
    assert(ncm_find_function(captured_hs, sizeof captured_hs, &f) == NCM_OK);
    assert(f.in_mps == 512 && f.data_alt == 1);
    assert(ncm_find_function(captured_vendor_hs, sizeof captured_vendor_hs, &f) == NCM_ENOTFOUND);
    assert(ncm_find_function(captured_ssp, 40, &f) == NCM_ETRUNC);
}

static void test_parameters(void)
{
    uint8_t d[NCM_NTB_PARAMETERS_LEN] = {0x1c, 0, 0x01, 0, 0, 0x40, 0, 0, 4, 0, 0, 0, 4, 0, 0, 0,
        0, 0x40, 0, 0, 4, 0, 0, 0, 4, 0, 0x20, 0};
    struct ncm_parameters p;

    assert(ncm_parse_parameters(d, sizeof d, &p) == NCM_OK);
    assert(p.formats == 1 && p.in_max == 0x4000 && p.out_max == 0x4000);
    assert(p.in_divisor == 4 && p.in_alignment == 4 && p.out_alignment == 4 && p.out_max_datagrams == 32);
    assert(ncm_parse_parameters(d, sizeof d - 1, &p) == NCM_ETRUNC);
}

static void test_block(void)
{
    uint8_t b[256];
    struct ncm_datagram g[4];
    struct ncm_block info;
    size_t n = build_block(b);

    assert(ncm_parse_block(b, n, g, 4, &info) == NCM_OK);
    assert(info.count == 2 && info.sequence == 7 && info.block_len == 200 && info.ndps == 1 && !info.dropped);
    assert(g[0].offset == 40 && g[0].len == 60 && g[1].offset == 100 && g[1].len == 64);
    assert(ncm_parse_block(b, n, g, 1, &info) == NCM_OK && info.count == 1 && info.dropped == 1);
    assert(ncm_parse_block(b, 20, g, 4, &info) == NCM_ETRUNC);
    assert(ncm_parse_block(b, 100, g, 4, &info) == NCM_EBOUNDS);

    n = build_block(b);
    b[0] = 'X';
    assert(ncm_parse_block(b, n, g, 4, &info) == NCM_ESIGNATURE);
    n = build_block(b);
    b[12] = 'X';
    assert(ncm_parse_block(b, n, g, 4, &info) == NCM_ESIGNATURE);
    n = build_block(b);
    put16(b + 10, 13);                          /* unaligned NDP */
    assert(ncm_parse_block(b, n, g, 4, &info) == NCM_EBOUNDS);
    n = build_block(b);
    put16(b + 22, 8);                           /* shorter than an Ethernet header */
    assert(ncm_parse_block(b, n, g, 4, &info) == NCM_EBOUNDS);
    n = build_block(b);
    put16(b + 24, 190);                         /* runs past the block */
    assert(ncm_parse_block(b, n, g, 4, &info) == NCM_EBOUNDS);
    n = build_block(b);
    put16(b + 18, 12);                          /* an NDP chain that loops */
    assert(ncm_parse_block(b, n, g, 4, &info) == NCM_EBOUNDS);
}

static void test_notification(void)
{
    uint8_t up[8] = {0xa1, 0x00, 0x01, 0x00, 0, 0, 0, 0};
    uint8_t speed[16] = {0xa1, 0x2a, 0, 0, 0, 0, 8, 0, 0x00, 0xf9, 0x02, 0x95, 0x00, 0xf9, 0x02, 0x95};
    uint8_t other[8] = {0xa1, 0x7f, 0, 0, 0, 0, 0, 0};
    uint8_t bad[8] = {0x00};
    struct ncm_notification n;

    assert(ncm_parse_notification(up, sizeof up, &n) == NCM_OK);
    assert(n.event == NCM_EVENT_CONNECTION && n.connected == 1);
    assert(ncm_parse_notification(speed, sizeof speed, &n) == NCM_OK);
    assert(n.event == NCM_EVENT_SPEED && n.down_bps == 2500000000u && n.up_bps == 2500000000u);
    assert(ncm_parse_notification(speed, 8, &n) == NCM_ETRUNC);
    assert(ncm_parse_notification(other, sizeof other, &n) == NCM_OK && n.event == NCM_EVENT_NONE);
    assert(ncm_parse_notification(bad, sizeof bad, &n) == NCM_ESIGNATURE);
    /* Captured from a real adapter: speed and link up in one 24-byte completion. */
    {
        static const uint8_t merged[24] = {0xa1, 0x2a, 0, 0, 1, 0, 8, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xa1, 0x00, 1, 0, 1, 0, 0, 0};

        assert(ncm_parse_notification(merged, sizeof merged, &n) == NCM_OK);
        assert(n.event == NCM_EVENT_SPEED && n.length == 16 && n.down_bps == 0xffffffffu);
        assert(ncm_parse_notification(merged + 16, 8, &n) == NCM_OK);
        assert(n.event == NCM_EVENT_CONNECTION && n.connected && n.length == 8);
    }
}

/* Device bytes are untrusted, so these run under ASan with exact-size buffers. */
static void test_random(void)
{
    enum { ROUNDS = 20000 };
    unsigned i, j, accepted = 0;

    srand(0x4e434d);
    for (i = 0; i < ROUNDS; i++) {
        size_t n = (size_t)(rand() % 400);
        uint8_t *p = malloc(n ? n : 1);
        struct ncm_datagram g[16];
        struct ncm_block info;
        struct ncm_function f;
        struct ncm_notification note;
        size_t k;

        assert(p);
        for (j = 0; j < n; j++) p[j] = (uint8_t)(rand() % 4 ? rand() : 0);
        if (n >= 28 && (i & 1)) {
            memcpy(p, "NCMH", 4);
            put16(p + 4, 12);
            put16(p + 8, (unsigned)n);
            put16(p + 10, 12);
            memcpy(p + 12, "NCM0", 4);
            put16(p + 16, 16);
        }
        if (ncm_parse_block(p, n, g, 16, &info) == NCM_OK) {
            accepted++;
            for (k = 0; k < info.count; k++) assert((size_t)g[k].offset + g[k].len <= n);
        }
        (void)ncm_parse_notification(p, n, &note);
        if (n >= 9) {
            p[1] = 2;
            put16(p + 2, (unsigned)n);
        }
        (void)ncm_find_function(p, n, &f);
        (void)ncm_find_ecm_function(p, n, &f);
        free(p);
    }
    assert(accepted > 0);
}

static const struct ncm_parameters device_parameters = {
    .formats = 1, .in_max = 36860, .in_divisor = 8, .in_remainder = 6, .in_alignment = 4,
    .out_max = 16388, .out_divisor = 4, .out_remainder = 2, .out_alignment = 4, .out_max_datagrams = 1024,
};

/* Whatever is built must pass the receive parser and sit where the device asked. */
static void check_build(const struct ncm_parameters *p, size_t frame_len)
{
    static uint8_t frame[NCM_MAX_FRAME], block[70000];
    struct ncm_datagram datagram[2];
    struct ncm_block info;
    size_t i;
    int total;

    for (i = 0; i < frame_len; i++) frame[i] = (uint8_t)(i * 7 + 1);
    total = ncm_build_block(p, 0x1234, frame, frame_len, block, sizeof block);
    assert(total > 0 && (uint32_t)total <= p->out_max);
    assert(ncm_parse_block(block, (size_t)total, datagram, 2, &info) == NCM_OK);
    assert(info.sequence == 0x1234 && info.count == 1 && info.block_len == total);
    assert(datagram[0].len == frame_len);
    assert(datagram[0].offset % p->out_divisor == p->out_remainder);
    assert(!memcmp(block + datagram[0].offset, frame, frame_len));
    assert(block[10] % (p->out_alignment > 4 ? p->out_alignment : 4) == 0);
}

static void test_build(void)
{
    struct ncm_parameters p = device_parameters;
    uint8_t frame[60] = {0}, block[256];

    check_build(&p, 60);
    check_build(&p, 1514);
    p.out_divisor = 512;
    p.out_remainder = 14;
    p.out_alignment = 16;
    check_build(&p, 60);
    p.out_alignment = 1;
    check_build(&p, 60);

    /* Exactly what the RTL8159 asked for: frame at 30, NTB of 90 bytes. */
    assert(ncm_build_block(&device_parameters, 1, frame, 60, block, sizeof block) == 90);
    assert(block[0] == 'N' && block[3] == 'H' && block[12] == 'N' && block[15] == '0');
    assert(block[8] == 90 && block[10] == 12 && block[12 + 8] == 30 && block[12 + 10] == 60);

    p = device_parameters;
    p.out_max = 89;
    assert(ncm_build_block(&p, 1, frame, 60, block, sizeof block) == NCM_EBOUNDS);
    assert(ncm_build_block(&device_parameters, 1, frame, 60, block, 89) == NCM_EBOUNDS);
    assert(ncm_build_block(&device_parameters, 1, frame, 13, block, sizeof block) == NCM_EINVAL);
    p = device_parameters;
    p.out_divisor = 0;
    assert(ncm_build_block(&p, 1, frame, 60, block, sizeof block) == NCM_EINVAL);
    p = device_parameters;
    p.out_remainder = 4;
    assert(ncm_build_block(&p, 1, frame, 60, block, sizeof block) == NCM_EINVAL);
    p = device_parameters;
    p.out_alignment = 6;
    assert(ncm_build_block(&p, 1, frame, 60, block, sizeof block) == NCM_EINVAL);
}

static void test_layouts(void)
{
    static const struct ncm_layout at_end = {.ndp_at_end = 1}, padded = {.min_block = 2049}, linux_shape = {.ndp_reserve = 172};
    struct ncm_datagram datagram[2];
    struct ncm_block info;
    uint8_t frame[60], block[4096];
    struct ncm_parameters p = device_parameters;
    struct ncm_layout too_long = {.min_block = 16389};
    int total;

    memset(frame, 0x5a, sizeof frame);
    /* Linux keeps room for 40 datagrams after the NDP header and still says 16. */
    total = ncm_build_block_layout(&p, 7, frame, 42, &linux_shape, block, sizeof block);
    assert(total == 186 + 42 && block[10] == 12 && block[16] == 16);
    assert(ncm_parse_block(block, (size_t)total, datagram, 2, &info) == NCM_OK && info.count == 1);
    assert(datagram[0].offset == 186 && datagram[0].len == 42);
    total = ncm_build_block_layout(&p, 7, frame, 60, &at_end, block, sizeof block);
    assert(total == 92);
    assert(ncm_parse_block(block, 92, datagram, 2, &info) == NCM_OK && info.count == 1);
    assert(datagram[0].offset == 14 && block[10] == 76);
    assert(!memcmp(block + 14, frame, 60));

    total = ncm_build_block_layout(&p, 7, frame, 60, &padded, block, sizeof block);
    assert(total == 2049);
    assert(ncm_parse_block(block, 2049, datagram, 2, &info) == NCM_OK && info.block_len == 2049);
    assert(datagram[0].offset == 30 && !block[2048]);

    assert(ncm_build_block_layout(&p, 7, frame, 60, &too_long, block, sizeof block) == NCM_EBOUNDS);
    assert(ncm_build_block_layout(&p, 7, frame, 60, NULL, block, sizeof block) == NCM_EINVAL);
    p.out_alignment = 16;
    total = ncm_build_block_layout(&p, 7, frame, 60, &at_end, block, sizeof block);
    assert(total > 0 && block[10] % 16 == 0);
    assert(ncm_parse_block(block, (size_t)total, datagram, 2, &info) == NCM_OK);
}

static void test_mac_string(void)
{
    static const char digits[] = "02aaBBccDDee";
    uint8_t d[26], mac[6], copy[26];
    struct ncm_function f;
    size_t i;

    d[0] = 26;
    d[1] = 3;
    for (i = 0; i < 12; i++) {
        d[2 + 2 * i] = (uint8_t)digits[i];
        d[3 + 2 * i] = 0;
    }
    assert(ncm_parse_mac_string(d, sizeof d, mac) == NCM_OK);
    assert(mac[0] == 0x02 && mac[1] == 0xaa && mac[2] == 0xbb && mac[5] == 0xee);
    assert(ncm_parse_mac_string(d, 25, mac) == NCM_ETRUNC);
    memcpy(copy, d, sizeof d);
    copy[1] = 2;
    assert(ncm_parse_mac_string(copy, sizeof d, mac) == NCM_ESIGNATURE);
    memcpy(copy, d, sizeof d);
    copy[4] = 'g';
    assert(ncm_parse_mac_string(copy, sizeof d, mac) == NCM_ESIGNATURE);
    memcpy(copy, d, sizeof d);
    copy[5] = 1;
    assert(ncm_parse_mac_string(copy, sizeof d, mac) == NCM_ESIGNATURE);
    memcpy(copy, d, sizeof d);
    copy[4] = '3';                      /* first octet 0x03: a group address */
    assert(ncm_parse_mac_string(copy, sizeof d, mac) == NCM_EBOUNDS);
    memcpy(copy, d, sizeof d);
    for (i = 0; i < 12; i++) copy[2 + 2 * i] = '0';
    assert(ncm_parse_mac_string(copy, sizeof d, mac) == NCM_EBOUNDS);

    assert(ncm_find_function(captured_ssp, sizeof captured_ssp, &f) == NCM_OK);
    assert(f.mac_string == 3);
}

int main(void)
{
    test_function();
    test_parameters();
    test_block();
    test_notification();
    test_random();
    test_build();
    test_layouts();
    test_mac_string();
    puts("ncm: captured descriptors, NTB16 both ways, MAC string, notifications, 20000 bounded random inputs OK");
    return 0;
}
