/* Сколько стоит профиль обфускации на транспортном пакете.
 *
 * Вопрос был «а не быстрее ли awg 1.5 / 2.0 / другая версия». Разница между
 * версиями на горячем пути сводится к трём вещам: паддинг S4 у каждого пакета
 * данных, защита заголовка (v3, ChaCha20 на каждый пакет) и всё остальное, что
 * работает только на рукопожатии. Меряем ровно первое и второе. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "transform.h"

#define PKT 1400
#define ITERS 2000000

static awg_config_t mk(int s3, int s4, int hp) {
    awg_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.jc = 4; cfg.jmin = 47; cfg.jmax = 410;
    cfg.s1 = 123; cfg.s2 = 133; cfg.s3 = s3; cfg.s4 = s4;
    cfg.h1 = (hrange_t){382791865, 382791865};
    cfg.h2 = (hrange_t){2090795392, 2090795392};
    cfg.h3 = (hrange_t){1729485619, 1729485619};
    cfg.h4 = (hrange_t){1646921971, 1646921971};
    if (hp) {
        for (int i = 0; i < 32; i++) cfg.hp_key[i] = (uint8_t)(i * 7 + 1);
        cfg.hp_on = 1;
    }
    config_compute(&cfg);
    return cfg;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void run(const char *name, awg_config_t cfg) {
    static uint8_t buf[AWG_PACKET_HEADROOM + AWG_PACKET_BUF_SIZE];
    int dataoff = AWG_PACKET_HEADROOM;
    uint64_t sink = 0;
    double t0 = now_s();
    for (long i = 0; i < ITERS; i++) {
        memset(buf + dataoff, 0, 16);
        uint32_t wt = WG_TRANSPORT_DATA;
        memcpy(buf + dataoff, &wt, 4);
        int out_len = 0, junk = 0;
        uint8_t *out = transform_outbound(buf, dataoff, PKT, &cfg,
                                          (uint64_t)i * 6364136223846793005ULL,
                                          &out_len, &junk);
        sink += out ? (uint64_t)out_len : 0;
    }
    double dt = now_s() - t0;
    double ns = dt * 1e9 / ITERS;
    /* Во что это превращается на пакетах в секунду и на мегабитах при 1400 B. */
    printf("  %-22s %6.1f ns/пакет   %8.0f тыс.пакетов/с   (sink=%llu)\n",
           name, ns, 1e6 / ns, (unsigned long long)sink);
}

int main(void) {
    printf("транспортный пакет %d байт, %d итераций\n\n", PKT, ITERS);
    run("v1.5 (S3=0 S4=0)",        mk(0, 0, 0));
    run("v2 наш (S4=24)",          mk(44, 24, 0));
    run("v2 прежний (S4=148)",     mk(44, 148, 0));
    run("v3 (S4=24 + HP)",         mk(44, 24, 1));
    return 0;
}
