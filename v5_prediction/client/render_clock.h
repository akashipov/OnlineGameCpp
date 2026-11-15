#pragma once
// Часы интерполяции: какой тик сервера рисовать для соперника ПРЯМО СЕЙЧАС.
//
// Наивный способ: renderTick = (тик последнего пакета) + (время с его прихода) - задержка.
// Он ломается при неровной сети: пакет опоздал на 80 мс → renderTick прыгнул назад,
// следующий пришёл вовремя → прыгнул вперёд. Соперник дёргается на каждом пакете.
//
// Здесь иначе:
//  1. renderTick идёт РОВНО вместе с реальным временем (+60 тиков в секунду),
//     а пакеты его только мягко подправляют (ускорение/замедление максимум на 10%).
//  2. Задержка подбирается сама: меряем, насколько неровно приходят пакеты (jitter),
//     и держим запас ≈ 3 × jitter. Сеть ровная → задержка маленькая (~50 мс),
//     Wi-Fi скачет → задержка растёт, но картинка остаётся плавной.

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "../common/jitter.h"

struct RenderClock {
    double renderTick = 0;   // какой момент сервера рисуем (дробный тик)
    double delayTicks = 6;   // насколько отстаём от самого свежего (для HUD)
    JitterMeter meter;       // насколько неровно приходят снимки

    // Вызывать для КАЖДОГО полученного снимка
    void onSnapshot(uint32_t tick, double now, int tickRate) {
        bool first = !meter.ready();
        meter.add((double)tick - now * tickRate);
        if (first) renderTick = tick - delayTicks;
    }

    // Вызывать каждый кадр
    void advance(double now, double frameDt, int tickRate) {
        if (!meter.ready()) return;
        delayTicks = std::clamp(2.0 + 3.0 * meter.jitterTicks, 3.0, 30.0);
        double target = now * tickRate + meter.offset - delayTicks;
        double err = target - renderTick;

        if (std::fabs(err) > 30) {
            renderTick = target;   // совсем разошлись (долгий обрыв) — прыгаем
        } else {
            // отстаём — чуть быстрее, убежали вперёд — чуть медленнее. Глазу не заметно.
            double speed = 1.0 + std::clamp(err * 0.05, -0.1, 0.1);
            renderTick += frameDt * tickRate * speed;
        }
    }
};
